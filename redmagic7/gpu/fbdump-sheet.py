#!/usr/bin/env python3
"""RedMagic 7: every FBDUMP of a ring on one PNG, a row per dump, surface 1 then surface 2 (fbdump-png.py's parsing).
Usage: fbdump-sheet.py ring.txt out.png [first last]"""
import re, struct, sys, zlib
text = open(sys.argv[1], 'rb').read().decode('latin-1')
sizes, rows = {}, {}
for m in re.finditer(r'FBDUMP (\d+) (\d+) size (\d+)x(\d+)', text):
    sizes[(int(m[1]), int(m[2]))] = (int(m[3]), int(m[4]))
for m in re.finditer(r'FBDUMP (\d+) (\d+) (\d+) ([01]) ([0-9a-f]*)', text):
    rows.setdefault((int(m[1]), int(m[2])), {})[(int(m[3]), int(m[4]))] = m[5]
dumps = sorted({k[0] for k in sizes})
if len(sys.argv) > 4:
    dumps = [d for d in dumps if int(sys.argv[3]) <= d <= int(sys.argv[4])]
W, H, gap = 103, 224, 6
cols = len(dumps)
out_w, out_h = cols * (W + gap), 2 * (H + gap)
img = [[40, 40, 40] * out_w for _ in range(out_h)]
for c, d in enumerate(dumps):
    for r, sid in enumerate((1, 2)):
        got = rows.get((d, sid))
        if not got: continue
        for y in range(H):
            hx = got.get((y, 0), '') + got.get((y, 1), '')
            for x in range(W):
                v = int(hx[4*x:4*x+4], 16) if len(hx) >= 4*x+4 else 0xf81f
                px = [(v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3]
                X, Y = c * (W + gap) + x, r * (H + gap) + y
                img[Y][3*X:3*X+3] = px
raw = b''.join(b'\x00' + bytes(row) for row in img)
def chunk(t, dd): return struct.pack('>I', len(dd)) + t + dd + struct.pack('>I', zlib.crc32(t + dd) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', out_w, out_h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
print(out_w, out_h, dumps)
