#!/usr/bin/env python3
"""RedMagic 7: turn the kernel's FBDUMP lines (boot-arg mondrian_fbdump=1, MondrianFramebuffer.cpp v93) back into PNGs.

Each dump is the two display surfaces (IDs 1 and 2) downscaled by 8, as RGB565, one row per two lines (halves of 52
pixels). Usage: fbdump-png.py ring.txt OUTDIR -- writes OUTDIR/dump<seq>-s<id>.png, scaled up 3x so they can be looked at.
"""
import re
import struct
import sys
import zlib


def png(path, w, h, rows):
    raw = b''.join(b'\x00' + bytes(r) for r in rows)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    text = open(sys.argv[1], 'rb').read().decode('latin-1')
    out = sys.argv[2]
    sizes, rows = {}, {}
    for m in re.finditer(r'FBDUMP (\d+) (\d+) size (\d+)x(\d+)', text):
        sizes[(int(m[1]), int(m[2]))] = (int(m[3]), int(m[4]))
    for m in re.finditer(r'FBDUMP (\d+) (\d+) (\d+) ([01]) ([0-9a-f]*)', text):
        rows.setdefault((int(m[1]), int(m[2])), {})[(int(m[3]), int(m[4]))] = m[5]
    scale = 3
    for key, (w, h) in sorted(sizes.items()):
        got = rows.get(key, {})
        out_rows = []
        for y in range(h):
            hexs = got.get((y, 0), '') + got.get((y, 1), '')
            px = []
            for x in range(w):
                v = int(hexs[4 * x:4 * x + 4], 16) if len(hexs) >= 4 * x + 4 else 0xf81f   # missing: magenta
                r, g, b = (v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3
                px += [r, g, b] * scale
            for _ in range(scale):
                out_rows.append(px)
        name = '%s/dump%02d-s%d.png' % (out, key[0], key[1])
        png(name, w * scale, h * scale, out_rows)
        print(name, '%d of %d rows' % (len({y for (y, _) in got}), h))


main()
