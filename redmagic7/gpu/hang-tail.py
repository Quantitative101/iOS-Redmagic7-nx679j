#!/usr/bin/env python3
"""RedMagic 7: print the packets of one captured stream (a v81 hang capture's "HANG stream"/"HANG mem" lines) from
dword FROM on: type-4 register writes with their values, type-7 packets with opcode and payload. For the end of the IB2
a hung CP had fetched to the end of (rem 0).

usage: hang-tail.py LOG STREAM_ADDR_HEX [FROM]   (FROM negative: that many dwords before the end)
"""
import re
import sys

OPS = {0x10: 'NOP', 0x3f: 'INDIRECT_BUFFER', 0x46: 'EVENT_WRITE', 0x26: 'WAIT_FOR_IDLE', 0x3c: 'WAIT_REG_MEM',
       0x3d: 'MEM_WRITE', 0x30: 'LOAD_STATE6_GEOM', 0x34: 'LOAD_STATE6_FRAG', 0x36: 'LOAD_STATE6', 0x38: 'DRAW_INDX_OFFSET',
       0x2c: 'BLIT', 0x73: 'MEMCPY', 0x13: 'WAIT_MEM_WRITES', 0x52: 'WAIT_FOR_ME', 0x63: 'SET_MODE', 0x43: 'SET_DRAW_STATE',
       0x65: 'SET_MARKER', 0x3e: 'REG_TO_MEM', 0x34 + 0: 'LOAD_STATE6_FRAG', 0x22: 'WAIT_MEM_GTE', 0x2e: 'EXEC_CS',
       0x2a: 'DRAW_INDIRECT_MULTI', 0x5f: 'SET_PROTECTED_MODE', 0x17: 'THREAD_CONTROL', 0x6d: 'SET_VISIBILITY_OVERRIDE',
       0x5d: 'SKIP_IB2_ENABLE_GLOBAL', 0x31: 'REG_RMW', 0x33: 'COND_REG_EXEC'}

mem = {}
size = None
want = int(sys.argv[2], 16)
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'HANG mem 0x([0-9a-f]+): ((?:[0-9a-f]{8} ?){8})', line)
    if m:
        base = int(m.group(1), 16)
        for i, w in enumerate(m.group(2).split()):
            mem[base + 4 * i] = int(w, 16)
        continue
    m = re.search(r'HANG stream 0x([0-9a-f]+) (\d+) dwords', line)
    if m and int(m.group(1), 16) == want and size is None:
        size = int(m.group(2))
if size is None:
    sys.exit('stream not captured')
frm = int(sys.argv[3]) if len(sys.argv) > 3 else 0
if frm < 0:
    frm = size + frm
i = 0
while i < size:
    w = mem.get(want + 4 * i)
    if w is None:
        print(f'{i}: (not captured)')
        i += 1
        continue
    kind = w >> 28
    if kind == 4:
        reg, n = (w >> 8) & 0x7ffff, w & 0x7f
    elif kind == 7:
        op, n = (w >> 16) & 0x7f, w & 0x3fff
    else:
        if i >= frm:
            print(f'{i}: ?? {w:08x}')
        i += 1
        continue
    if i >= frm:
        body = ' '.join(f'{mem.get(want + 4 * (i + 1 + k), 0):08x}' for k in range(min(n, 12)))
        if kind == 4:
            print(f'{i}: REG {reg:#06x} x{n}: {body}')
        else:
            print(f'{i}: {OPS.get(op, hex(op))} x{n}: {body}')
    i += 1 + n
