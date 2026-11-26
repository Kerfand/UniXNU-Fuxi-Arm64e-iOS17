#!/usr/bin/env python3
"""Add the WCN7850 firmware to a GPU addon (fuxi iOS 17, FuxiWifiFwPart.cpp).

  add-wifi-fw-to-addon.py WIFI_FIRMWARE_DIR ADDON_IN ADDON_OUT

The addon (scripts/make-gpu-addon.py, "NGPUADD2") is boot memory the loader hands over as /chosen/memory-map/GpuAddon and NativeRamBlock
never frees (native_ram_block_gpu_addon). The Wi-Fi start reads its firmware from there (wifi/amss.bin, wifi/m3.bin, wifi/board-2.bin), so
it no longer depends on the partition named oops, whose WFW1 header the boot loader overwrote with a crash record after a forced reboot
(2026-11-12: "fuxi wifi fw: amss.bin FAILED (rc -7)"). The kernel's addon install also writes the files below /private/var/ngpu/wifi on the
RAM Data volume, as it does with every addon file. Every file is checked against manifest.json (SHA-256 and size) first; entries already
named wifi/* in ADDON_IN are replaced.

Layout, little endian: "NGPUADD2", u32 count, u32 0, u64 total bytes; per file u32 path bytes (with its NUL), u32 mode, u64 size,
u64 compressed size, the path padded to 8 bytes, the zlib stream padded to 8 bytes."""
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

ORDER = ['amss.bin', 'm3.bin', 'board-2.bin']
LIMIT = 64 << 20   # native_ram_block_gpu_addon refuses a larger addon


def pad(data):
    return data + bytes(-len(data) % 8)


def entries(addon):
    assert addon[:8] == b'NGPUADD2', 'not an NGPUADD2 addon'
    count, = struct.unpack_from('<I', addon, 8)
    total, = struct.unpack_from('<Q', addon, 16)
    assert total == len(addon), 'addon size field does not match the file'
    at, out = 24, []
    for _ in range(count):
        name_bytes, mode, size, packed = struct.unpack_from('<IIQQ', addon, at)
        start = at
        at += 24
        name = addon[at:at + name_bytes - 1].decode()
        at += (name_bytes + 7) & ~7
        at += (packed + 7) & ~7
        out.append((name, addon[start:at]))
    assert at == len(addon), 'trailing bytes after the last entry'
    return out


def entry(name, mode, data):
    raw = name.encode() + b'\0'
    packed = zlib.compress(data, 9)
    return struct.pack('<IIQQ', len(raw), mode, len(data), len(packed)) + pad(raw) + pad(packed)


def main():
    fw_dir, addon_in, addon_out = (Path(a) for a in sys.argv[1:4])
    manifest = json.loads((fw_dir / 'manifest.json').read_text())['files']
    kept = [blob for name, blob in entries(addon_in.read_bytes()) if not name.startswith('wifi/')]
    added = []
    for name in ORDER:
        data = (fw_dir / name).read_bytes()
        want = manifest[name]
        if hashlib.sha256(data).hexdigest() != want['sha256'] or len(data) != want['bytes']:
            raise SystemExit(f'{name}: does not match manifest.json')
        added.append(entry('wifi/' + name, 0o644, data))
    body = b''.join(kept + added)
    total = 24 + len(body)
    if total > LIMIT:
        raise SystemExit(f'addon would be {total} bytes, over the {LIMIT} the kernel accepts')
    out = b'NGPUADD2' + struct.pack('<IIQ', len(kept) + len(added), 0, total) + body
    entries(out)   # parses again as the kernel does
    addon_out.write_bytes(out)
    print(f'{addon_out}: {len(kept)} + {len(added)} files, {total} bytes, sha256 {hashlib.sha256(out).hexdigest()[:16]}')


if __name__ == '__main__':
    main()
