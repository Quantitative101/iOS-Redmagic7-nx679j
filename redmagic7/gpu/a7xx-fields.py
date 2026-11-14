#!/usr/bin/env python3
"""Print each named register's A6XX and A7XX definitions (offset and bitfields) from Mesa's a6xx.xml, side by side,
to see whether a register that moved on A7XX also changed its layout.

usage: a7xx-fields.py A6XX_XML NAME [NAME ...]
"""
import sys
import xml.etree.ElementTree as ET


def strip_ns(tag):
    return tag.split('}')[-1]


root = ET.parse(sys.argv[1]).getroot()
bitsets = {}
regs = {}


def fields_of(node):
    out = []
    for f in node:
        if strip_ns(f.tag) != 'bitfield':
            continue
        if f.get('pos') is not None:
            span = f.get('pos')
        else:
            span = f'{f.get("low")}-{f.get("high")}'
        out.append(f'{f.get("name")}[{span}]' + (f'({f.get("variants")})' if f.get('variants') else ''))
    return out


def walk(node):
    for c in node:
        t = strip_ns(c.tag)
        if t == 'bitset':
            bitsets[c.get('name')] = c
        if t in ('reg32', 'reg64'):
            regs.setdefault(c.get('name'), []).append(c)
        walk(c)


walk(root)
for name in sys.argv[2:]:
    for r in regs.get(name, []):
        fs = fields_of(r)
        ty = r.get('type')
        if ty in bitsets:
            fs = fields_of(bitsets[ty]) + fs
        print(f'{name} @{r.get("offset")} variants={r.get("variants")} type={ty}: {" ".join(fs)}')
