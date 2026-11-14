#!/usr/bin/env python3
"""Wrap an lz4 frame in the shim's LZ4P header (shim/lz4.c): 'LZ4P', u32 version 1, u64 raw size, u64 lz4 size.

usage: lz4p.py RAW LZ4 OUT
"""
import os
import struct
import sys

raw, comp, out = sys.argv[1:4]
size = os.path.getsize(raw)
if size == 0:
    sys.exit('error: the payload is empty')
data = open(comp, 'rb').read()
with open(out, 'wb') as f:
    f.write(b'LZ4P' + struct.pack('<IQQ', 1, size, len(data)))
    f.write(data)
print(f'{out}: {size} -> {len(data)} bytes')
