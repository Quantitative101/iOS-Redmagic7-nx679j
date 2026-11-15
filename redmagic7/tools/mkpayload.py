#!/usr/bin/env python3
"""Pack an xnu kernel Mach-O + ADT into the payload the sweet shim boots.

  mkpayload.py KERNEL ADT OUT [--cmdline "..."] [--ramdisk IMAGE]

The kernel is linked as a PIE with LC_DYLD_CHAINED_FIXUPS. Nobody else
(no kmutil, no iBoot) resolves those for us, and xnu only rebases static
kernelcaches itself, so the chains are walked here and every pointer is
rewritten to its unslid target. The shim then only copies flat segments.

Payload layout (little endian, all offsets from payload start):
  0x000  char  magic[8] = "SWEETXNU"
  0x008  u32   version = 1, nsegs
  0x010  u64   virt_base      kernel VA that maps to phys_base
  0x018  u64   entry_va       LC_UNIXTHREAD pc
  0x020  u64   adt_off, adt_size
  0x030  u64   kernel_end_va  end of the highest segment (page aligned)
  0x038  u64   phys_base      kernel-managed RAM (same values as the ADT's dram-base/size)
  0x040  u64   mem_size
  0x048  char  cmdline[608]
  0x2a8  seg[nsegs] { u64 vmaddr, vmsize, file_off(in payload), file_size }

--ramdisk appends IMAGE as one more segment right after the kernel and moves
kernel_end past it, so the shim needs no changes: the image lands below the
ADT and boot_args, inside topOfKernelData (never handed to the VM), and the
ADT gets /chosen/memory-map "RAMDisk" = <phys, size> for IOFindBSDRoot to
turn into md0 (boot with rd=md0).
"""
import argparse, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402
import boards  # noqa: E402

LC_SEGMENT_64, LC_UNIXTHREAD, LC_DYLD_CHAINED_FIXUPS = 0x19, 0x5, 0x80000034
PAGE = 0x4000
BOOT_LINE_LENGTH = 608
CMDLINE = 0x48
SEG_TABLE = CMDLINE + BOOT_LINE_LENGTH	# 0x2a8


def parse_macho(buf):
    magic, cputype, cpusub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<8I', buf, 0)
    assert magic == 0xfeedfacf, 'not a 64-bit Mach-O'
    segs, entry, fixups = [], None, None
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', buf, off)
        if cmd == LC_SEGMENT_64:
            name = buf[off + 8:off + 24].split(b'\0')[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', buf, off + 24)
            segs.append(dict(name=name, vmaddr=vmaddr, vmsize=vmsize, fileoff=fileoff, filesize=filesize))
        elif cmd == LC_UNIXTHREAD:
            flavor, count = struct.unpack_from('<II', buf, off + 8)
            # arm_thread_state64: x0..x28, fp, lr, sp, pc, cpsr
            entry, = struct.unpack_from('<Q', buf, off + 16 + 32 * 8)
        elif cmd == LC_DYLD_CHAINED_FIXUPS:
            fixups = struct.unpack_from('<II', buf, off + 8)	# dataoff, datasize
        off += size
    return dict(cputype=cputype, cpusub=cpusub & 0xff, segs=segs, entry=entry, fixups=fixups)


def apply_chained_fixups(buf, m):
    """Rewrite every chained rebase in buf (the whole file) to its plain target VA."""
    if not m['fixups']:
        return 0
    data_off, _ = m['fixups']
    _, starts_off, _, _, imports_count, _, _ = struct.unpack_from('<7I', buf, data_off)
    assert imports_count == 0, 'kernel has binds; only rebases are supported'
    image_base = m['segs'][0]['vmaddr']		# __TEXT, holds the mach header
    seg_count, = struct.unpack_from('<I', buf, data_off + starts_off)
    nfix = 0
    for i in range(seg_count):
        seg_info, = struct.unpack_from('<I', buf, data_off + starts_off + 4 + 4 * i)
        if not seg_info:
            continue
        base = data_off + starts_off + seg_info
        size, page_size, fmt, seg_offset, max_valid, page_count = struct.unpack_from('<IHHQIH', buf, base)
        starts = struct.unpack_from('<%dH' % page_count, buf, base + 22)
        seg = m['segs'][i]
        for p, start in enumerate(starts):
            if start == 0xffff:
                continue
            assert not start & 0x8000, 'DYLD_CHAINED_PTR_START_MULTI not supported'
            loc = seg['fileoff'] + p * page_size + start
            while True:
                raw, = struct.unpack_from('<Q', buf, loc)
                target, nxt, stride = decode(raw, fmt, image_base)
                struct.pack_into('<Q', buf, loc, target)
                nfix += 1
                if not nxt:
                    break
                loc += nxt * stride
    return nfix


def decode(raw, fmt, image_base):
    """Return (unslid target VA, next, stride) for one chained pointer."""
    if fmt in (2, 6):		# DYLD_CHAINED_PTR_64 / _64_OFFSET
        assert not raw >> 63, 'bind in kernel'
        target = raw & ((1 << 36) - 1)
        high8 = (raw >> 36) & 0xff
        nxt = (raw >> 51) & 0xfff
        if fmt == 6:
            target += image_base
        return target | (high8 << 56), nxt, 4
    if fmt == 8:		# DYLD_CHAINED_PTR_64_KERNEL_CACHE
        target = raw & ((1 << 30) - 1)
        nxt = (raw >> 51) & 0xfff
        return image_base + target, nxt, 4
    if fmt in (1, 7, 9, 12):	# arm64e variants (stock VMAPPLE kernel only; PAC is not re-signed)
        auth = raw >> 63
        nxt = (raw >> 51) & 0x7ff
        stride = 4 if fmt in (7, 12) else 8
        if auth:
            target = raw & 0xffffffff
        else:
            target = raw & ((1 << 43) - 1)
            target |= ((raw >> 43) & 0xff) << 56
        if fmt in (7, 9, 12) or auth:
            target += image_base
        return target, nxt, stride
    raise ValueError(f'unsupported chained pointer format {fmt}')


def add_ramdisk_to_adt(adt_blob, phys, size):
    """Return the ADT with /chosen/memory-map RAMDisk = <phys, size> (two uintptr_t)."""
    root, _ = adt.parse(adt_blob)
    chosen = next(c for c in root.children if c.name == 'chosen')
    mmap = next(c for c in chosen.children if c.name == 'memory-map')
    assert mmap.prop('RAMDisk') is None, 'ADT already has a RAMDisk'
    mmap.props.append(('RAMDisk', 0, struct.pack('<2Q', phys, size)))
    return adt.serialize(root)


def add_rtc_offset_to_adt(adt_blob, offset):
    """Return the ADT with /chosen mondrian-rtc-offset = <u64>: seconds from the PMIC RTC's count to Unix time."""
    root, _ = adt.parse(adt_blob)
    chosen = next(c for c in root.children if c.name == 'chosen')
    assert chosen.prop('mondrian-rtc-offset') is None, 'ADT already has an RTC offset'
    chosen.props.append(('mondrian-rtc-offset', 0, struct.pack('<Q', offset)))
    return adt.serialize(root)


def add_timezone_to_adt(adt_blob, zone):
    """Return the ADT with /chosen mondrian-timezone = "<Area/City>\0": the zone the kernel links localtime to."""
    root, _ = adt.parse(adt_blob)
    chosen = next(c for c in root.children if c.name == 'chosen')
    assert chosen.prop('mondrian-timezone') is None, 'ADT already has a timezone'
    chosen.props.append(('mondrian-timezone', 0, zone.encode() + b'\0'))
    return adt.serialize(root)


def add_chosen_u32_to_adt(adt_blob, name, value):
    """Return the ADT with /chosen NAME = <u32 VALUE>: a setting the kernel reads when the boot line has no room."""
    root, _ = adt.parse(adt_blob)
    chosen = next(c for c in root.children if c.name == 'chosen')
    assert chosen.prop(name) is None, f'ADT already has {name}'
    chosen.props.append((name, 0, struct.pack('<I', value)))
    return adt.serialize(root)


def add_chosen_str_to_adt(adt_blob, name, value):
    """Return the ADT with /chosen NAME = "VALUE" (NUL-terminated): e.g. the Wi-Fi radio's address."""
    root, _ = adt.parse(adt_blob)
    chosen = next(c for c in root.children if c.name == 'chosen')
    assert chosen.prop(name) is None, f'ADT already has {name}'
    chosen.props.append((name, 0, value.encode() + b'\0'))
    return adt.serialize(root)


def add_wlan_node_to_adt(adt_blob):
    """Return the ADT with /arm-io/wlan: launchd starts com.apple.wifid only when the device tree has a node named
    "wlan" (its _LimitLoadToDeviceTree), as the Note 8 port's mkadt does; without wifid Settings has no Wi-Fi."""
    root, _ = adt.parse(adt_blob)
    arm_io = next(c for c in root.children if c.name == 'arm-io')
    if any(c.name == 'wlan' for c in arm_io.children):
        return adt_blob
    node = adt.Node()
    node.props = [('name', 0, b'wlan\0'), ('device_type', 0, b'wlan\0'), ('compatible', 0, b'redmagic7,qca6490\0')]
    arm_io.children.append(node)
    return adt.serialize(root)


def single_cluster_adt(adt_blob):
    """Return the ADT with every /cpus/* cluster-id 0: XNU then puts all 8 cores in one processor set (on this SMP
    kernel each cluster-id is its own pset, and a thread stays in its cluster while that has an idle core)."""
    root, _ = adt.parse(adt_blob)
    cpus = next(c for c in root.children if c.name == 'cpus')
    n = 0
    for cpu in cpus.children:
        for i, (name, flags, value) in enumerate(cpu.props):
            if name == 'cluster-id':
                cpu.props[i] = (name, flags, struct.pack('<I', 0))
                n += 1
    print(f'single cluster: cluster-id 0 on {n} cpus')
    return adt.serialize(root)


def adt_dram(adt_blob):
    """Kernel-managed RAM <base, size> from the ADT's /chosen dram-base/dram-size."""
    root, _ = adt.parse(adt_blob)
    chosen = next(c for c in root.children if c.name == 'chosen')
    return (struct.unpack('<Q', chosen.prop('dram-base'))[0],
            struct.unpack('<Q', chosen.prop('dram-size'))[0])


def build(kernel_path, adt_path, cmdline, ramdisk_path=None, board=None, rtc_offset=None, timezone=None, chosen_u32=(),
          chosen_str=(), wlan=False, single_cluster=False):
    buf = bytearray(open(kernel_path, 'rb').read())
    m = parse_macho(buf)
    nfix = apply_chained_fixups(buf, m)
    segs = [s for s in m['segs'] if s['vmsize']]
    text = m['segs'][0]
    assert text['name'] == '__TEXT' and text['fileoff'] == 0
    # virt_base: one L2 block (ARM_TT_L2_SIZE, which depends on the page
    # size) below the mach header, so start.s's block mapping that starts at
    # the header's block begins exactly at phys_base.
    l2 = board.tt_l2_size if board else 0x2000000
    virt_base = text['vmaddr'] & ~(l2 - 1)
    kernel_end = max(s['vmaddr'] + s['vmsize'] for s in segs)
    kernel_end = (kernel_end + PAGE - 1) & ~(PAGE - 1)

    adt_blob = open(adt_path, 'rb').read()
    if rtc_offset:
        adt_blob = add_rtc_offset_to_adt(adt_blob, rtc_offset)
    if timezone:
        adt_blob = add_timezone_to_adt(adt_blob, timezone)
    for spec in chosen_u32:
        name, value = spec.split('=', 1)
        adt_blob = add_chosen_u32_to_adt(adt_blob, name, int(value, 0))
    for spec in chosen_str:
        name, value = spec.split('=', 1)
        adt_blob = add_chosen_str_to_adt(adt_blob, name, value)
    if wlan:
        adt_blob = add_wlan_node_to_adt(adt_blob)
    if single_cluster:
        adt_blob = single_cluster_adt(adt_blob)
    # The ADT decides where the kernel's RAM is (mkadt.py --highmem).
    PHYS_BASE, MEM_SIZE = adt_dram(adt_blob)
    pieces = [(s['vmaddr'], s['vmsize'], buf[s['fileoff']:s['fileoff'] + s['filesize']]) for s in segs]
    ramdisk = None
    if ramdisk_path:
        image = open(ramdisk_path, 'rb').read()
        rd_size = (len(image) + PAGE - 1) & ~(PAGE - 1)
        rd_va = kernel_end
        kernel_end = rd_va + rd_size
        rd_pa = rd_va - virt_base + PHYS_BASE
        pieces.append((rd_va, rd_size, image))
        adt_blob = add_ramdisk_to_adt(adt_blob, rd_pa, rd_size)
        ramdisk = (hex(rd_pa), hex(rd_size))

    hdr_size = SEG_TABLE + 32 * len(pieces)
    out = bytearray(hdr_size)
    seg_table = []
    for vmaddr, vmsize, data in pieces:
        off = len(out)
        out += data
        out += b'\0' * (-len(out) & 0xf)
        seg_table.append((vmaddr, vmsize, off, len(data)))
    adt_off = len(out)
    out += adt_blob
    out += b'\0' * (-len(out) & 0xf)

    struct.pack_into('<8sII', out, 0, board.payload_magic if board else b'SWEETXNU',
                     1, len(pieces))
    struct.pack_into('<7Q', out, 0x10, virt_base, m['entry'], adt_off, len(adt_blob), kernel_end,
                     PHYS_BASE, MEM_SIZE)
    cl = cmdline.encode()[:BOOT_LINE_LENGTH - 1]
    out[CMDLINE:CMDLINE + len(cl)] = cl
    for i, t in enumerate(seg_table):
        struct.pack_into('<4Q', out, SEG_TABLE + 32 * i, *t)
    info = dict(cputype=hex(m['cputype']), cpusub=m['cpusub'], fixups=nfix, virt_base=hex(virt_base),
                entry=hex(m['entry']), kernel_end=hex(kernel_end), ramdisk=ramdisk, size=len(out),
                segs=[(s['name'], hex(s['vmaddr']), hex(s['vmsize'])) for s in segs])
    return bytes(out), info


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('kernel'); ap.add_argument('adt'); ap.add_argument('out')
    ap.add_argument('--board', default='sweet', help='sweet | mondrian')
    ap.add_argument('--cmdline', help='kernel boot arguments (default: the board\'s)')
    ap.add_argument('--ramdisk', help='root image for md0 (boot with rd=md0)')
    ap.add_argument('--rtc-offset', type=int, help='Unix time minus the PMIC RTC count (redmagic7: tools/rtc-offset.sh)')
    ap.add_argument('--timezone', help='IANA zone (Area/City) the kernel links /private/var/db/timezone/localtime to')
    ap.add_argument('--chosen-u32', action='append', default=[], metavar='NAME=VALUE',
                    help='a /chosen u32 property (e.g. mondrian-rmwifi=2): settings past the full boot line')
    ap.add_argument('--chosen-str', action='append', default=[], metavar='NAME=VALUE',
                    help='a /chosen string property (e.g. mondrian-wifi-mac=aa:bb:cc:dd:ee:ff)')
    ap.add_argument('--wlan', action='store_true', help='add /arm-io/wlan so launchd starts wifid')
    ap.add_argument('--single-cluster', action='store_true', help='every cpu cluster-id 0: one processor set')
    ap.add_argument('--facts', help='shim dtb dump to take the memory map from')
    ap.add_argument('--allow-provisional', action='store_true',
                    help='build even if the board still has guessed values')
    a = ap.parse_args()
    board = boards.get_board(a.board)
    if a.facts:
        board = boards.apply_facts(board, boards.parse_facts(open(a.facts).read()))
    if not a.allow_provisional:
        boards.require_measured(board, 'mkpayload.py')
    cmdline = a.cmdline if a.cmdline is not None else board.cmdline
    blob, info = build(a.kernel, a.adt, cmdline, a.ramdisk, board, a.rtc_offset, a.timezone, a.chosen_u32, a.chosen_str, a.wlan,
                       a.single_cluster)
    if a.chosen_u32:
        info['chosen_u32'] = a.chosen_u32
    if a.timezone:
        info['timezone'] = a.timezone
    if a.rtc_offset:
        info['rtc_offset'] = a.rtc_offset
    open(a.out, 'wb').write(blob)
    for k, v in info.items():
        if k != 'segs':
            print(f'{k}: {v}')
    for s in info['segs']:
        print('  ', *s)
