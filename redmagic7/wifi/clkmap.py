#!/usr/bin/env python3
"""Map GCC register offsets to clock names in a Linux gcc-*.c file, or names to offsets.

usage: clkmap.py FILE off 0x6f014 0x9000c ...   |   clkmap.py FILE name gcc_pcie_0_aux_clk ...
"""
import re
import sys

text = open(sys.argv[1]).read()
blocks = re.findall(r'static struct (\w+) (\w+) = \{(.*?)\n\};', text, re.S)
mode = sys.argv[2]
for key in sys.argv[3:]:
    hits = []
    for kind, name, body in blocks:
        regs = re.findall(r'\.(halt_reg|enable_reg|cmd_rcgr|gdscr|reg|hwcg_reg) = (0x[0-9a-fA-F]+)', body)
        if mode == 'off':
            for field, value in regs:
                if int(value, 16) == int(key, 16):
                    hits.append(f'{name} ({kind}.{field})')
        elif name == key:
            hits.append(', '.join(f'{f}={v}' for f, v in regs))
    print(key, '->', '; '.join(hits) or '?')
