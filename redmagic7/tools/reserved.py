#!/usr/bin/env python3
"""List /reserved-memory regions of a dtc-decompiled device tree within an address range.
usage: reserved.py live.dts [lo_hex hi_hex]"""
import re
import sys

text = open(sys.argv[1]).read()
lo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 1 << 64
start = text.index('reserved-memory {')
depth, i = 0, start
while True:
    if text[i] == '{':
        depth += 1
    elif text[i] == '}':
        depth -= 1
        if depth == 0:
            break
    i += 1
block = text[start:i]
rows = []
for m in re.finditer(r'\n\t\t([\w@,.-]+) \{(.*?)\n\t\t\};', block, re.S):
    name, body = m.group(1), m.group(2)
    r = re.search(r'reg = <0x00 (0x[0-9a-f]+) 0x00 (0x[0-9a-f]+)>', body)
    if not r:
        continue
    a, s = int(r.group(1), 16), int(r.group(2), 16)
    if a + s > lo and a < hi:
        rows.append((a, s, name, 'no-map' if 'no-map' in body else ''))
for a, s, name, nm in sorted(rows):
    print(f'0x{a:09x} - 0x{a + s:09x} {s >> 20:5d} MB  {name} {nm}')
