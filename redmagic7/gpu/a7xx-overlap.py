#!/usr/bin/env python3
"""Registers a6xx.hpp names whose offset, on A7XX, is (also) some OTHER register of Mesa's a6xx.xml.

usage: a7xx-overlap.py A6XX_XML NAMES_FILE
NAMES_FILE: one register name per line (without REG_A6XX_). For each, the offsets it has on A7XX (a7xx-delta.py's
rules: no variants or A6XX-/A7XX- ranges apply) and every other A7XX register (arrays expanded) covering one of them.
Found on the phone (v18-v20): SP_CS_IBO at 0xa9f2 is SP_CS_BINDLESS_BASE[5] on A7XX, and a6xx.xml gives no variant
for it, so a6xx->a7xx deltas by name alone miss it.
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


def collect(root, gen):
    """offset -> list of names for registers that apply to gen (arrays expanded)."""
    at = {}

    def add(off, name):
        at.setdefault(off, []).append(name)

    def walk(node, base, inherited):
        for child in node:
            tag = strip_ns(child.tag)
            var = child.get('variants') or inherited
            if tag in ('reg32', 'reg64'):
                if not applies(var, gen):
                    continue
                off = int(child.get('offset'), 0) + base
                add(off, child.get('name'))
                if tag == 'reg64':
                    add(off + 1, child.get('name') + '+1')
            elif tag == 'array':
                if not applies(var, gen):
                    continue
                off = int(child.get('offset'), 0) + base
                stride = int(child.get('stride', '1'), 0)
                length = int(child.get('length', '1'), 0)
                name = child.get('name') or 'array'
                regs = [c for c in child if strip_ns(c.tag) in ('reg32', 'reg64') and applies(c.get('variants') or var, gen)]
                for i in range(length):
                    if regs:
                        for r in regs:
                            o = off + i * stride + int(r.get('offset'), 0)
                            add(o, f'{name}[{i}].{r.get("name")}')
                            if strip_ns(r.tag) == 'reg64':
                                add(o + 1, f'{name}[{i}].{r.get("name")}+1')
                    else:
                        for s in range(stride):
                            add(off + i * stride + s, f'{name}[{i}]+{s}')
            elif tag in ('domain', 'database'):
                if tag == 'domain' and child.get('name') not in ('A6XX', None):
                    continue
                walk(child, base, inherited)
    walk(root, 0, None)
    return at


root = ET.parse(sys.argv[1]).getroot()
names = [l.strip() for l in open(sys.argv[2]) if l.strip()]
a7 = collect(root, 7)
byname = {}
for off, ns in a7.items():
    for n in ns:
        byname.setdefault(n, set()).add(off)
for name in names:
    offs = sorted(byname.get(name, ()))
    for off in offs:
        others = [n for n in a7[off] if n != name and not n.startswith(name + '+')]
        if others:
            print(f'{name} @0x{off:04x} also: {", ".join(others)}')
