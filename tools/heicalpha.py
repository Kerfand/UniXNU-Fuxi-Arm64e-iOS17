"""heicalpha.py SRC.HEIC OUT.HEIC: copy of SRC whose primary item ('pitm') is the alpha auxiliary image, so a decoder that only
reads the primary item (Windows WIC's HEIF decoder) returns the alpha plane as a greyscale image. Prints the alpha item id or 'none'."""
import struct, sys
src, out = sys.argv[1], sys.argv[2]
b = bytearray(open(src, 'rb').read())


def boxes(start, end):
    at = start
    while at + 8 <= end:
        size, typ = struct.unpack_from('>I4s', b, at)
        hdr = 8
        if size == 1:
            size = struct.unpack_from('>Q', b, at + 8)[0]; hdr = 16
        elif size == 0:
            size = end - at
        yield typ.decode('latin1'), at, at + hdr, at + size
        at += size


meta = next(((s, d, e) for t, s, d, e in boxes(0, len(b)) if t == 'meta'), None)
if not meta:
    sys.exit('no meta')
ms, md, me = meta
md += 4  # full box
sub = {t: (s, d, e) for t, s, d, e in boxes(md, me)}
# pitm
ps, pd, pe = sub['pitm']
pver = b[pd]
primary = struct.unpack_from('>H' if pver == 0 else '>I', b, pd + 4)[0]
# ipco property list and ipma associations
iprp = sub['iprp']
ip = {t: (s, d, e) for t, s, d, e in boxes(iprp[1], iprp[2])}
props = [(t, d, e) for t, s, d, e in boxes(ip['ipco'][1], ip['ipco'][2])]
alpha_props = set()
for i, (t, d, e) in enumerate(props, 1):
    if t == 'auxC':
        urn = bytes(b[d + 4:e]).split(b'\0')[0]
        if b'alpha' in urn or urn.endswith(b'auxid:1'):
            alpha_props.add(i)
ms_, md_, me_ = ip['ipma']
ver, flags = b[md_], int.from_bytes(b[md_ + 1:md_ + 4], 'big')
at = md_ + 4
count = struct.unpack_from('>I', b, at)[0]; at += 4
alpha_item = None
for _ in range(count):
    if ver < 1:
        item = struct.unpack_from('>H', b, at)[0]; at += 2
    else:
        item = struct.unpack_from('>I', b, at)[0]; at += 4
    n = b[at]; at += 1
    for _ in range(n):
        if flags & 1:
            v = struct.unpack_from('>H', b, at)[0] & 0x7fff; at += 2
        else:
            v = b[at] & 0x7f; at += 1
        if v in alpha_props and alpha_item is None:
            alpha_item = item
if alpha_item is None:
    print('none'); sys.exit(0)
# the alpha item may be a tile of a grid: prefer the item that references the primary through 'auxl' (iref)
if 'iref' in sub:
    rs, rd, re_ = sub['iref']
    rver = b[rd]
    for t, s, d, e in boxes(rd + 4, re_):
        if t != 'auxl': continue
        w = 2 if rver == 0 else 4
        frm = int.from_bytes(b[d:d + w], 'big')
        n = struct.unpack_from('>H', b, d + w)[0]
        refs = [int.from_bytes(b[d + w + 2 + k * w:d + w + 2 + (k + 1) * w], 'big') for k in range(n)]
        if primary in refs:
            alpha_item = frm
if pver == 0:
    struct.pack_into('>H', b, pd + 4, alpha_item)
else:
    struct.pack_into('>I', b, pd + 4, alpha_item)
open(out, 'wb').write(b)
print(alpha_item)
