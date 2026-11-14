#!/usr/bin/env python3
"""Decode a PM4 stream dumped by the kernel's HANG lines into packets, naming A7XX registers and CP opcodes from
Mesa's a6xx.xml / adreno_pm4.xml.

Two dump forms are read:
  v49: "MondrianGPU: HANG ib1 +OFFSET: w0 .. w7"   (OFFSET in bytes from the IB start)
  v68: "MondrianGPU: HANG raw DWORD: w0 .. w7"      (the 640 dwords before a metal_drawseq marker; DWORD is the index
       in the IB). The window starts mid-packet, so the walk is synced on the packet starts the "drawseq before N"
       lines name.

--zeros lists what in the window points at address 0: a 64-bit register pair of an ADDR/BASE register written 0
(with a non-zero size register nearby noted), a CP_LOAD_STATE6 from address 0, a draw whose index base is 0.

usage: pm4-decode.py LOG MESA_REGISTERS_DIR [--from DWORD] [--to DWORD] [--zeros]
"""
import re
import sys
import xml.etree.ElementTree as ET


def strip_ns(tag):
    return tag.split('}')[-1]


def variant_ok(v):
    if not v:
        return True
    return 'A7XX' in v or v.endswith('-') and v.startswith('A6XX')


def load_regs(path):
    names, wide = {}, set()
    root = ET.parse(path).getroot()

    def walk(node, base, prefix):
        for c in node:
            t = strip_ns(c.tag)
            if t in ('reg32', 'reg64'):
                if not variant_ok(c.get('variants')):
                    continue
                off = base + int(c.get('offset'), 0)
                names.setdefault(off, prefix + c.get('name'))
                if t == 'reg64':
                    names.setdefault(off + 1, prefix + c.get('name') + '_HI')
                    wide.add(off)
            elif t == 'array':
                if not variant_ok(c.get('variants')):
                    continue
                off = base + int(c.get('offset'), 0)
                stride = int(c.get('stride', '1'), 0)
                length = int(c.get('length', '1'), 0)
                for i in range(length):
                    for r in c:
                        if strip_ns(r.tag) in ('reg32', 'reg64') and variant_ok(r.get('variants')):
                            o = off + i * stride + int(r.get('offset'), 0)
                            names.setdefault(o, f"{prefix}{c.get('name')}[{i}].{r.get('name')}")
                            if strip_ns(r.tag) == 'reg64':
                                names.setdefault(o + 1, f"{prefix}{c.get('name')}[{i}].{r.get('name')}_HI")
                                wide.add(o)
            elif t == 'domain' and c.get('name') == 'A6XX':
                walk(c, 0, '')

    walk(root, 0, '')
    return names, wide


def load_ops(path):
    ops = {}
    root = ET.parse(path).getroot()
    for e in root.iter():
        if strip_ns(e.tag) == 'enum' and e.get('name') == 'adreno_pm4_type3_packets':
            for v in e:
                if strip_ns(v.tag) == 'value' and variant_ok(v.get('variants')):
                    ops.setdefault(int(v.get('value'), 0), v.get('name'))
    return ops


def size_of(words, i):
    h = words.get(i, 0)
    if h >> 28 == 4:
        return 1 + (h & 0x7f)
    if h >> 28 == 7:
        return 1 + (h & 0x3fff)
    return None


def sync(words, anchors):
    """the first dword of the window from which a packet walk lands on an anchor (a known packet start)"""
    if not anchors:
        return min(words)
    lo, target = min(words), min(a for a in anchors if a >= min(words)) if any(a >= min(words) for a in anchors) else None
    if target is None:
        return lo
    for s in range(lo, min(lo + 200, target + 1)):
        i = s
        while i < target:
            n = size_of(words, i)
            if n is None:
                break
            i += n
        if i == target:
            return s
    return target


def main():
    log, regdir = sys.argv[1], sys.argv[2]
    lo = int(sys.argv[sys.argv.index('--from') + 1]) if '--from' in sys.argv else 0
    hi = int(sys.argv[sys.argv.index('--to') + 1]) if '--to' in sys.argv else 1 << 30
    zeros = '--zeros' in sys.argv
    regs, wide = load_regs(regdir + '/a6xx.xml')
    ops = load_ops(regdir + '/adreno_pm4.xml')
    words, anchors, raw = {}, set(), False
    for line in open(log, errors='replace'):
        m = re.search(r'HANG ib1 \+([0-9a-f]+): ((?:[0-9a-f]{8} ?){8})', line)
        if m:
            base = int(m.group(1), 16) // 4
            for i, w in enumerate(m.group(2).split()):
                words[base + i] = int(w, 16)
            continue
        m = re.search(r'HANG raw (\d+): ((?:[0-9a-f]{8} ?){8})', line)
        if m:
            raw = True
            base = int(m.group(1))
            for i, w in enumerate(m.group(2).split()):
                words[base + i] = int(w, 16)
            continue
        m = re.search(r'HANG drawseq before (\d+): ([0-9a-f]{8}(?: [0-9a-f]{8}){4})', line)
        if m:
            at = int(m.group(1))
            anchors.add(at)
            for i, w in enumerate(m.group(2).split()):
                words.setdefault(at + i, int(w, 16))
    if not words:
        print('no HANG ib1 / HANG raw lines')
        return
    n = max(words) + 1
    i = sync(words, anchors) if raw else 0
    regval = {}
    found = []
    while i < n:
        h = words.get(i, 0)
        kind = h >> 28
        if kind == 4:
            cnt = h & 0x7f
            reg = (h >> 8) & 0x7ffff
            for k in range(cnt):
                regval[reg + k] = (words.get(i + 1 + k, 0), i)
            if lo <= i <= hi and not zeros:
                for k in range(cnt):
                    r = reg + k
                    print(f'{i:5d}  {h:08x}  W {r:#06x} {regs.get(r, "?"):40s} = {words.get(i + 1 + k, 0):#010x}'
                          if k == 0 else f'{"":5s}  {"":8s}  W {r:#06x} {regs.get(r, "?"):40s} = {words.get(i + 1 + k, 0):#010x}')
            i += 1 + cnt
        elif kind == 7:
            cnt = h & 0x3fff
            op = (h >> 16) & 0x7f
            name = ops.get(op, hex(op))
            body = [words.get(i + 1 + k, 0) for k in range(cnt)]
            if lo <= i <= hi and not zeros:
                shown = ' '.join(f'{w:08x}' for w in body[:12])
                print(f'{i:5d}  {h:08x}  {name:28s} [{cnt}] {shown}{" ..." if cnt > 12 else ""}')
            if zeros:
                if name.startswith('CP_LOAD_STATE6') and cnt >= 3:
                    src = (body[0] >> 16) & 3     # SS6_DIRECT 0, SS6_BINDLESS 1, SS6_INDIRECT 2, SS6_UBO 3
                    if src in (2, 3) and body[1] & ~3 == 0 and body[2] == 0:
                        found.append(f'{i:5d}  {name} from address 0 (word0 {body[0]:08x})')
                if name == 'CP_DRAW_INDX_OFFSET' and cnt >= 7 and (i + 5 not in words or i + 6 not in words):
                    found.append(f'{i:5d}  {name}: index base not in the dump')
                elif name == 'CP_DRAW_INDX_OFFSET' and cnt >= 7:
                    if body[4] == 0 and body[5] == 0:
                        found.append(f'{i:5d}  {name}: index base 0 ({cnt} words: {" ".join(f"{w:08x}" for w in body)})')
                    else:
                        found.append(f'{i:5d}  {name}: index base {body[5]:x}{body[4]:08x}, max {body[6]} (for reference)')
                if name == 'CP_DRAW_INDX_OFFSET' and cnt == 3:
                    found.append(f'{i:5d}  {name}: auto-indexed {body[2]} vertices, {body[1]} instances (for reference)')
            i += 1 + cnt
        else:
            if lo <= i <= hi and not zeros:
                print(f'{i:5d}  {h:08x}  ?? (not a type4/type7 header)')
            i += 1
    if zeros:
        for r in sorted(wide):
            if r in regval and r + 1 in regval:
                name = regs.get(r, '?')
                v = regval[r][0] | regval[r + 1][0] << 32
                if v == 0 and any(s in name for s in ('ADDR', 'BASE', 'IOVA', 'PTR')):
                    found.append(f'{regval[r][1]:5d}  {name} ({r:#06x}) = 0')
        for r, (v, at) in sorted(regval.items()):
            name = regs.get(r, '?')
            if 'VFD_FETCH' in name and name.endswith('.SIZE') and v != 0:
                b = regs_base = r - 2
                bv = regval.get(b, (None, 0))[0], regval.get(b + 1, (None, 0))[0]
                if bv == (0, 0):
                    found.append(f'{at:5d}  {name} = {v:#x} with its BASE 0')
        print('\n'.join(sorted(found, key=lambda s: int(s.split()[0]))) if found else 'nothing in the window points at 0')


main()
