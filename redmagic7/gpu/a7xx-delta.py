#!/usr/bin/env python3
"""How many of the registers hakios-metal's a6xx.hpp names differ on A7XX (Adreno 730), per Mesa's a6xx.xml.

usage: a7xx-delta.py A6XX_HPP A6XX_XML [--list]
For each REG_A6XX_<NAME> constant the header defines: is <NAME> in the XML for A7XX at the same offset,
at a different offset, or absent on A7XX? Variant attributes follow Mesa's rules: "A6XX" = a6xx only,
"A7XX-" = a7xx and later, "A6XX-" = both, none = both.
"""
import re
import sys
import xml.etree.ElementTree as ET


def strip_ns(tag):
    return tag.split('}')[-1]


def applies(variants, gen):
    if not variants:
        return True
    for part in variants.replace(' ', '').split(','):
        m = re.fullmatch(r'A(\d)XX(-?)(A(\d)XX)?', part)
        if not m:
            continue
        lo = int(m.group(1))
        hi = int(m.group(4)) if m.group(4) else (99 if m.group(2) else lo)
        if lo <= gen <= hi:
            return True
    return False


def collect(root):
    """name -> list of (offset, variants) for reg32/reg64/array entries, domain A6XX."""
    regs = {}

    def walk(node, base, inherited):
        for child in node:
            tag = strip_ns(child.tag)
            var = child.get('variants') or inherited
            if tag in ('reg32', 'reg64'):
                off = int(child.get('offset'), 0) + base
                regs.setdefault(child.get('name'), []).append((off, var))
            elif tag == 'array':
                off = int(child.get('offset'), 0) + base
                name = child.get('name')
                if name:
                    regs.setdefault(name, []).append((off, var))
                walk(child, off, var)
            elif tag in ('domain', 'database', 'bitset', 'enum'):
                if tag == 'domain' and child.get('name') not in ('A6XX', None):
                    continue
                walk(child, base, inherited)
    walk(root, 0, None)
    return regs


hpp = open(sys.argv[1]).read()
names = sorted(set(re.findall(r'REG_A6XX_([A-Z0-9_]+)', hpp)))
root = ET.parse(sys.argv[2]).getroot()
regs = collect(root)
same, moved, gone, unknown = [], [], [], []
for n in names:
    defs = regs.get(n)
    if not defs:
        # array element accessors like RB_MRT_BUF_INFO -> try the prefix before the last field
        unknown.append(n)
        continue
    a6 = {o for o, v in defs if applies(v, 6)}
    a7 = {o for o, v in defs if applies(v, 7)}
    if not a7:
        gone.append(n)
    elif a6 == a7:
        same.append(n)
    else:
        moved.append((n, sorted(a6), sorted(a7)))
print(f'{len(names)} register names in the header: {len(same)} same on A7XX, {len(moved)} at another offset, '
      f'{len(gone)} not on A7XX, {len(unknown)} not found by name (array members, sub-fields)')
if '--list' in sys.argv:
    for n, a, b in moved:
        print('MOVED', n, [hex(x) for x in a], '->', [hex(x) for x in b])
    for n in gone:
        print('GONE ', n)
