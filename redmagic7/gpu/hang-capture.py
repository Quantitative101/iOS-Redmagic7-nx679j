#!/usr/bin/env python3
"""Rebuild a stuck submission from a v81 hang capture and check it.

The kernel's hang dump (MondrianGPU.cpp, v81) prints the whole stuck IB1, every IB2 it calls and the memory each
CP_LOAD_STATE6 (SS6_INDIRECT) and indexed draw reads, as "HANG stream ADDR N dwords" and "HANG mem ADDR: 8 words".
This puts the words back at their GPU addresses and walks the IB1 the way the CP would: register state, IB2 calls,
draws. For every draw it lists the addresses the draw makes the hardware read and flags any that is 0 or outside
every captured range; with --draw N it prints that draw's full state.

usage: hang-capture.py LOG [--draw N] [--around DWORD]
"""
import re
import sys

mem = {}            # dword address -> value
streams = []        # (address, dwords)
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'HANG mem 0x([0-9a-f]+): ((?:[0-9a-f]{8} ?){8})', line)
    if m:
        base = int(m.group(1), 16)
        for i, w in enumerate(m.group(2).split()):
            mem[base + 4 * i] = int(w, 16)
        continue
    m = re.search(r'HANG stream 0x([0-9a-f]+) (\d+) dwords', line)
    if m:
        streams.append((int(m.group(1), 16), int(m.group(2))))

if not streams:
    sys.exit('no "HANG stream" lines: not a v81 capture')

want_draw = int(sys.argv[sys.argv.index('--draw') + 1]) if '--draw' in sys.argv else None


def word(a):
    return mem.get(a)


def captured(a):
    return a in mem


regs = {}
draws = []
problems = []


def walk(at, n, depth, label):
    i = 0
    while i < n:
        h = word(at + 4 * i)
        if h is None:
            problems.append(f'{label}+{i}: stream word not captured')
            return
        kind = h >> 28
        if kind == 4:
            cnt, reg = h & 0x7f, (h >> 8) & 0x7ffff
            for k in range(cnt):
                v = word(at + 4 * (i + 1 + k))
                if v is not None:
                    regs[reg + k] = v
            i += 1 + cnt
            continue
        if kind != 7:
            return
        cnt, op = h & 0x3fff, (h >> 16) & 0x7f
        b = [word(at + 4 * (i + 1 + k)) or 0 for k in range(min(cnt, 8))]
        where = f'{label}+{i}'
        if op == 0x3f and cnt == 3:
            a = b[0] | b[1] << 32
            if a == 0:
                problems.append(f'{where}: CP_INDIRECT_BUFFER to 0')
            elif depth == 0:
                walk(a, b[2], 1, f'ib2@{a:#x}')
        elif op in (0x30, 0x32, 0x34, 0x36) and cnt >= 3 and ((b[0] >> 16) & 3) == 2:
            a = (b[1] & ~3) | b[2] << 32
            if a == 0:
                problems.append(f'{where}: CP_LOAD_STATE6 from 0 (word0 {b[0]:08x})')
            elif not captured(a):
                problems.append(f'{where}: CP_LOAD_STATE6 from {a:#x}, not captured')
        elif op in (0x3c, 0x14) and cnt >= 3:      # CP_WAIT_REG_MEM, CP_WAIT_MEM_GTE: the CP reads memory
            a = b[1] | b[2] << 32
            if a == 0 and (op == 0x14 or (b[0] & 0x10)):
                problems.append(f'{where}: CP wait on memory at 0')
        elif op == 0x3d and cnt >= 2:              # CP_MEM_WRITE
            if (b[0] | b[1] << 32) == 0:
                problems.append(f'{where}: CP_MEM_WRITE to 0')
        elif op == 0x2c:                           # CP_BLIT: the 2D engine, its source and destination in registers
            ranges = [(0x8c00, 0x8c40), (0xb2c0, 0xb300), (0xb4c0, 0xb500), (0xacc0, 0xad00), (0x88d0, 0x88f0)]
            st = ' '.join(f'{r:#06x}={regs[r]:08x}' for lo, hi in ranges for r in range(lo, hi) if r in regs)
            problems.append(f'{where}: CP_BLIT {b[0]:#x}; 2D/blit registers: {st}')
        elif op in (0x40, 0x42, 0x44, 0x2d, 0x58, 0x5a):   # memory <-> register, CP_MEM_TO_MEM and kin
            problems.append(f'{where}: opcode {op:#x} [{cnt}] {" ".join(f"{x:08x}" for x in b)} (reads or writes memory: check)')
        elif op == 0x46 and cnt >= 4:              # CP_EVENT_WRITE with an address
            a = b[1] | b[2] << 32
            if a == 0:
                problems.append(f'{where}: CP_EVENT_WRITE {b[0]:#x} with address 0')
        elif op in (0x38, 0x2a):                   # a draw
            draw(where, op, b, cnt)
        i += 1 + cnt


def r64(r):
    return regs.get(r, 0) | regs.get(r + 1, 0) << 32


def draw(where, op, b, cnt):
    n = len(draws)
    reads = []
    if op == 0x38 and cnt == 7 and ((b[0] >> 6) & 3) == 0:
        reads.append(('index buffer', b[4] | b[5] << 32))
    fetches = regs.get(0xa000, 0) & 0x3f
    for s in range(fetches):
        reads.append((f'vfd fetch {s} (size {regs.get(0xa012 + 4 * s, 0)})', r64(0xa010 + 4 * s)))
    reads.append(('VS program', r64(0xa81c)))
    reads.append(('FS program', r64(0xa983)))
    for name, r in (('SP_FS_TEX_CONST', 0xa9e4), ('SP_FS_TEX_SAMP', 0xa9e0), ('SP_VS_TEX_CONST', 0xa81a - 0)):
        if r in regs:
            reads.append((name, r64(r)))
    draws.append((where, op, list(b), reads, dict(regs)))
    for name, a in reads:
        if a == 0 and not name.startswith('vfd fetch') or (name.startswith('vfd fetch') and a == 0 and 'size 0' not in name):
            problems.append(f'{where} draw {n}: {name} at 0')


walk(streams[0][0], streams[0][1], 0, 'ib1')
print(f'{len(streams)} streams, {len(mem) * 4} bytes captured, {len(draws)} draws')
for p in problems[:200]:
    print('PROBLEM', p)
near = int(sys.argv[sys.argv.index('--near') + 1]) if '--near' in sys.argv else None
if near is not None:
    for k, d in enumerate(draws):
        if d[0].startswith('ib1+') and near - 400 <= int(d[0][4:]) <= near + 10:
            print('NEAR', k, d[0], ' '.join(f'{x:08x}' for x in d[2]))
if want_draw is not None and want_draw < len(draws):
    where, op, b, reads, st = draws[want_draw]
    print(f'draw {want_draw} at {where}: op {op:#x} {" ".join(f"{x:08x}" for x in b)}')
    for name, a in reads:
        print(f'  reads {name}: {a:#x}{"" if captured(a) or a == 0 else " (not captured)"}')
