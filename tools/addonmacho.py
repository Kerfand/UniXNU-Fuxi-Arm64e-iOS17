"""List the Mach-O files in the GPU addon with their load-command fixup format and segment sizes (read-only)."""
import struct, zlib, sys
blob = open(sys.argv[1], 'rb').read()
count, _z, total = struct.unpack_from('<IIQ', blob, 8)
at = 24
for _ in range(count):
    nb, mode, size, packed = struct.unpack_from('<IIQQ', blob, at); at += 24
    name = blob[at:at + nb - 1].decode(); at += (nb + 7) // 8 * 8
    data = zlib.decompress(blob[at:at + packed]); at += (packed + 7) // 8 * 8
    magic = data[:4]
    if magic != b'\xcf\xfa\xed\xfe':
        print(f'{size:>10} {name}'); continue
    cpu, sub, ftype, ncmds = struct.unpack_from('<iiII', data, 4)
    off = 32; cmds = []; segs = []
    for _ in range(ncmds):
        c, s = struct.unpack_from('<II', data, off)
        cmds.append(hex(c))
        if c == 0x19:
            sn = data[off + 8:off + 24].rstrip(b'\0').decode()
            vm, vs, fo, fs = struct.unpack_from('<QQQQ', data, off + 24)
            segs.append(f'{sn}:{vs:#x}')
        off += s
    fix = 'chained' if '0x80000034' in cmds else ('dyld_info' if '0x80000022' in cmds or '0x22' in cmds else 'none')
    print(f'{size:>10} {name} MachO type {ftype} sub {sub & 0xffffff:#x} fixups {fix} segs {segs}')
