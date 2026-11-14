#!/usr/bin/env python3
"""Pull single partition images out of an Android full-OTA update.zip on a web server, without downloading it.

update.zip stores payload.bin uncompressed; this reads the zip directory and the payload manifest with HTTP range
requests, then fetches and decodes only the operations of the partitions asked for (REPLACE / REPLACE_XZ / REPLACE_BZ /
ZERO / DISCARD, which is all a full OTA uses).

usage: ota_fetch.py URL OUTDIR [partition ...]     (no partitions: list them with sizes)
"""
import bz2
import hashlib
import io
import lzma
import os
import struct
import sys
import urllib.request
import zipfile


class HTTPFile(io.RawIOBase):
    """A read-only, seekable file over HTTP range requests (with a small read-ahead cache)."""

    def __init__(self, url):
        self.url, self.pos = url, 0
        req = urllib.request.Request(url, method='HEAD')
        with urllib.request.urlopen(req) as r:
            self.size = int(r.headers['Content-Length'])
        self.cache_at, self.cache = -1, b''
        self.fetched = 0

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else (self.pos + off if whence == 1 else self.size + off)
        return self.pos

    def _get(self, start, n):
        req = urllib.request.Request(self.url, headers={'Range': f'bytes={start}-{start + n - 1}'})
        with urllib.request.urlopen(req) as r:
            data = r.read()
        self.fetched += len(data)
        return data

    def read(self, n=-1):
        if n is None or n < 0:
            n = self.size - self.pos
        n = max(0, min(n, self.size - self.pos))
        if n == 0:
            return b''
        if self.cache_at <= self.pos and self.pos + n <= self.cache_at + len(self.cache):
            out = self.cache[self.pos - self.cache_at:self.pos - self.cache_at + n]
        elif n < 65536:
            self.cache_at, self.cache = self.pos, self._get(self.pos, min(65536, self.size - self.pos))
            out = self.cache[:n]
        else:
            out = self._get(self.pos, n)
        self.pos += len(out)
        return out

    def readinto(self, b):
        data = self.read(len(b))
        b[:len(data)] = data
        return len(data)


def varint(buf, i):
    v = s = 0
    while True:
        c = buf[i]
        i += 1
        v |= (c & 0x7f) << s
        s += 7
        if not c & 0x80:
            return v, i


def fields(buf):
    """(field number, wire type, value) of a protobuf message; length-delimited values are bytes."""
    i = 0
    while i < len(buf):
        key, i = varint(buf, i)
        f, t = key >> 3, key & 7
        if t == 0:
            v, i = varint(buf, i)
        elif t == 1:
            v, i = struct.unpack_from('<Q', buf, i)[0], i + 8
        elif t == 2:
            n, i = varint(buf, i)
            v, i = bytes(buf[i:i + n]), i + n
        elif t == 5:
            v, i = struct.unpack_from('<I', buf, i)[0], i + 4
        else:
            raise ValueError(f'wire type {t}')
        yield f, t, v


def parse_manifest(buf):
    block_size, parts = 4096, []
    for f, _, v in fields(buf):
        if f == 3:
            block_size = v
        elif f == 13:
            p = {'name': '', 'ops': [], 'size': 0, 'hash': b''}
            for pf, _, pv in fields(v):
                if pf == 1:
                    p['name'] = pv.decode()
                elif pf == 7:
                    for qf, _, qv in fields(pv):
                        if qf == 1:
                            p['size'] = qv
                        elif qf == 2:
                            p['hash'] = qv
                elif pf == 8:
                    op = {'type': 0, 'off': 0, 'len': 0, 'dst': [], 'sha': b''}
                    for of, _, ov in fields(pv):
                        if of == 1:
                            op['type'] = ov
                        elif of == 2:
                            op['off'] = ov
                        elif of == 3:
                            op['len'] = ov
                        elif of == 6:
                            e = dict((ef, ev) for ef, _, ev in fields(ov))
                            op['dst'].append((e.get(1, 0), e.get(2, 0)))
                        elif of == 8:
                            op['sha'] = ov
                    p['ops'].append(op)
            parts.append(p)
    return block_size, parts


def main():
    url, out = sys.argv[1], sys.argv[2]
    want = sys.argv[3:]
    http = HTTPFile(url)
    zf = zipfile.ZipFile(http)
    info = zf.getinfo('payload.bin')
    if info.compress_type != zipfile.ZIP_STORED:
        sys.exit('payload.bin is compressed in this zip')
    # data start of the member: local header (30 bytes + name + extra) at header_offset
    http.seek(info.header_offset)
    lh = http.read(30)
    base = info.header_offset + 30 + struct.unpack_from('<H', lh, 26)[0] + struct.unpack_from('<H', lh, 28)[0]
    http.seek(base)
    head = http.read(24)
    magic, version, msize = head[:4], struct.unpack('>Q', head[4:12])[0], struct.unpack('>Q', head[12:20])[0]
    if magic != b'CrAU' or version != 2:
        sys.exit(f'payload header {magic} v{version}')
    sigsize = struct.unpack('>I', head[20:24])[0]
    http.seek(base + 24)
    block_size, parts = parse_manifest(http.read(msize))
    data_base = base + 24 + msize + sigsize
    if not want:
        for p in parts:
            print(f"{p['name']:24s} {p['size'] / 1048576:9.1f} MB  {len(p['ops'])} ops")
        return
    os.makedirs(out, exist_ok=True)
    for name in want:
        p = next((p for p in parts if p['name'] == name), None)
        if p is None:
            print(f'{name}: not in this payload')
            continue
        path = os.path.join(out, name + '.img')
        with open(path, 'wb') as f:
            f.truncate(p['size'])
            for op in p['ops']:
                if op['type'] in (6, 7):        # ZERO, DISCARD: the file is already zeros
                    continue
                http.seek(data_base + op['off'])
                blob = http.read(op['len'])
                if op['sha'] and hashlib.sha256(blob).digest() != op['sha']:
                    sys.exit(f'{name}: operation data hash mismatch')
                if op['type'] == 8:
                    blob = lzma.decompress(blob)
                elif op['type'] == 1:
                    blob = bz2.decompress(blob)
                elif op['type'] != 0:
                    sys.exit(f'{name}: operation type {op["type"]} is not a full-OTA one')
                at = 0
                for start, count in op['dst']:
                    n = count * block_size
                    f.seek(start * block_size)
                    f.write(blob[at:at + n])
                    at += n
        h = hashlib.sha256()
        with open(path, 'rb') as f:
            for chunk in iter(lambda: f.read(1 << 20), b''):
                h.update(chunk)
        ok = (h.digest() == p['hash']) if p['hash'] else None
        print(f"{name}: {p['size']} bytes -> {path} (sha256 {'OK' if ok else ('MISMATCH' if ok is False else 'n/a')})")
    print(f'fetched {http.fetched / 1048576:.1f} MB in all')


if __name__ == '__main__':
    main()
