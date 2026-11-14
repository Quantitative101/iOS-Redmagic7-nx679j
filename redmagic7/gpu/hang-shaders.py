#!/usr/bin/env python3
"""Pull the shader words the kernel's HANG handler printed (v57: "MondrianGPU: HANG vs|fs +OFFSET: w0 .. w7") out of a
decoded log into raw little-endian files, for Mesa's ir3-disasm.

usage: hang-shaders.py LOG OUTDIR      ->  OUTDIR/hang-vs.bin, OUTDIR/hang-fs.bin
"""
import re
import struct
import sys

log, out = sys.argv[1], sys.argv[2]
words = {'vs': {}, 'fs': {}}
for line in open(log, errors='replace'):
    m = re.search(r'HANG (vs|fs) \+([0-9a-f]+): ((?:[0-9a-f]{8} ?){8})', line)
    if m:
        base = int(m.group(2), 16) // 4
        for i, w in enumerate(m.group(3).split()):
            words[m.group(1)][base + i] = int(w, 16)
for stage, ws in words.items():
    if not ws:
        print(f'{stage}: nothing in the log')
        continue
    n = max(ws) + 1
    data = b''.join(struct.pack('<I', ws.get(i, 0)) for i in range(n))
    path = f'{out}/hang-{stage}.bin'
    open(path, 'wb').write(data)
    print(f'{stage}: {n} dwords -> {path}')
