"""t32-bl-daily.img with its UEFI firmware volume's DarwinBoot replaced by DarwinBoot v2 (build-darwinboot-fv.sh).

Everything else of the template stays byte-identical: BootShim, the rest of the FD, the FAT payload, the DTB
and the Android ramdisk/trailer. The FD keeps its 3 MiB size, so BootShim's layout is unchanged.
"""
import gzip, hashlib, struct, zlib
from pathlib import Path

base = Path('/home/xiaomi/sadnut-fuxi-local-20261103')
template = Path('/mnt/c/Users/Xiaomi/Downloads/t32-bl-daily.img')
fd_new = (base / 'pack-work-ios17-kernel/t32-darwinboot-v2.fd').read_bytes()
fd_old = (base / 'pack-work-ios17-kernel/t32.fd').read_bytes()
out = base / 'pack-work-ios17-kernel/t32-bl-daily-darwinboot-v2.img'
aligned = lambda n: (n + 4095) & ~4095
old = template.read_bytes()
assert old[:8] == b'ANDROID!' and struct.unpack_from('<I', old, 40)[0] == 3
size, android_ramdisk = struct.unpack_from('<II', old, 8)
d = zlib.decompressobj(31)
raw = d.decompress(old[4096:4096 + size]) + d.flush()
tail = d.unused_data
assert d.eof and tail[:4] == bytes.fromhex('d00dfeed')
FD_AT, FD_SIZE = 0xc0, 0x300000
assert raw[FD_AT:FD_AT + FD_SIZE] == fd_old and len(fd_new) == FD_SIZE and raw[FD_AT + 0x28:FD_AT + 0x2c] == b'_FVH'
raw2 = raw[:FD_AT] + fd_new + raw[FD_AT + FD_SIZE:]
kernel = gzip.compress(raw2, compresslevel=9, mtime=0) + tail
old_end = 4096 + aligned(size)
assert len(old) == old_end + aligned(android_ramdisk)
header = bytearray(old[:4096]); struct.pack_into('<I', header, 8, len(kernel))
result = bytes(header) + kernel + bytes(aligned(len(kernel)) - len(kernel)) + old[old_end:]
check = zlib.decompressobj(31); back = check.decompress(result[4096:4096 + len(kernel)]) + check.flush()
assert back == raw2 and check.unused_data == tail
out.write_bytes(result)
print(out, len(result), hashlib.sha256(result).hexdigest())
