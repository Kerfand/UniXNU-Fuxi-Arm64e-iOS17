#!/usr/bin/env python3
"""Build the WCN7850 firmware container for the partition named "oops" (fuxi iOS 17, FuxiWifiFwPart.cpp).
Usage: make-wifi-fw.py <wifi-firmware dir> <output image> [partition bytes]

Layout, as the UniXNU reference reads it (https://unixnu.com/zh/porting/network, UniXNU-sheng package/WifiProbe.cpp): block 0 (4096 bytes)
holds 'WFW1', u32 count, then count entries of name[24] (NUL padded), u32 offset (bytes from the partition start, 4096-aligned) and
u32 size; each file follows uncompressed at its offset. Every file is checked against manifest.json (SHA-256 and size) first.
With the partition size given the image is padded to it and refused if it does not fit (fastboot flash writes the whole image)."""
import hashlib, json, struct, sys
from pathlib import Path

fw_dir, out = Path(sys.argv[1]), Path(sys.argv[2])
part_bytes = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
manifest = json.loads((fw_dir / 'manifest.json').read_text())['files']
order = ['amss.bin', 'm3.bin', 'board-2.bin']
assert set(order) == set(manifest), sorted(manifest)
files = []
for name in order:
    data = (fw_dir / name).read_bytes()
    want = manifest[name]
    if hashlib.sha256(data).hexdigest() != want['sha256'] or len(data) != want['bytes']:
        raise SystemExit(f'{name}: does not match manifest.json')
    files.append((name, data))
header = bytearray(4096)
header[0:8] = b'WFW1' + struct.pack('<I', len(files))
offset, body = 4096, bytearray()
for i, (name, data) in enumerate(files):
    raw = name.encode()
    assert len(raw) < 24
    struct.pack_into('<24sII', header, 8 + 32 * i, raw, offset, len(data))
    padded = data + bytes(-len(data) % 4096)
    body += padded
    offset += len(padded)
image = bytes(header) + bytes(body)
if part_bytes:
    if len(image) > part_bytes:
        raise SystemExit(f'image {len(image)} bytes does not fit the {part_bytes}-byte partition')
    image += bytes(part_bytes - len(image))
out.write_bytes(image)
print(f'{out}: {len(image)} bytes, sha256 {hashlib.sha256(image).hexdigest()}')
for i, (name, data) in enumerate(files):
    n, off, size = struct.unpack_from('<24sII', header, 8 + 32 * i)
    print(f'  {name:12s} offset {off:#x} size {size}')
