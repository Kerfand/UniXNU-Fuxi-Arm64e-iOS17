#!/usr/bin/env python3
"""VerifiedRoot inputs for the iOS 17 (arm64e fileset) owner root: header-only policies.

The iPadOS build links each policy's full block hash table into the kernel (about 73 MiB). The iOS 17 fileset kernel
cannot carry that (DarwinBoot's 128 MiB limit and the original kernel's holes), so the kernel only gets each
policy's signed header and signature (544 bytes) plus the trusted key. NativeUfs then reads the table of each window
from the version 2 userdata data object "<window>.hashes" and vr_open authenticates the whole envelope (the signed
header carries the table digest), so every served block is still verified against the owner-signed table.

Inputs: the policies made from the flashed 21H16 windows (scratch mkpolicy17.py) under VERIFIED_ROOT_DIR, default
/mnt/f/UniXNU-FUXI-work/ios17-cache/verified-root-21H16/{os,systemos}. The private key there never enters a repository.
Output: build/verified-root-os-xnu-bundle/libVerifiedRootProbe.a (CONFIG_XNU_VERIFIED_ROOT_ARCHIVE).
"""
import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from kernel_target import TRIPLE, check_object

POLICIES = Path(os.environ.get('VERIFIED_ROOT_DIR', '/mnt/f/UniXNU-FUXI-work/ios17-cache/verified-root-21H16'))
IMAGE = Path(os.environ.get('VERIFIED_ROOT_IMAGE', '/mnt/f/UniXNU-FUXI-work/outputs/ios17-userdata-21H16/userdata-root.img'))
out = ROOT / 'build/verified-root-os-xnu-bundle'
out.mkdir(parents=True, exist_ok=True)

records, key = [], None
for name in ('os', 'systemos'):
    policy = POLICIES / name
    header, signature = (policy / 'header.bin').read_bytes(), (policy / 'signature.bin').read_bytes()
    if len(header) != 160 or len(signature) != 384:
        raise SystemExit(f'{policy}: expected a 160-byte header and a 384-byte signature')
    manifest = (policy / 'manifest.bin').read_bytes()
    if manifest[:544] != header + signature:
        raise SystemExit(f'{policy}: header/signature differ from manifest.bin')
    image_bytes, block_count = struct.unpack_from('<Q', header, 24)[0], (len(manifest) - 544) // 32
    if hashlib.sha256(manifest[544:]).digest() not in header:
        raise SystemExit(f'{policy}: the signed header does not carry the table digest')
    head = out / f'{name}-header-only.bin'
    head.write_bytes(header + signature)
    this_key = (policy / 'trusted-key.bin').read_bytes()
    if key is not None and this_key != key:
        raise SystemExit('Both policies must be signed under the same trusted key')
    key = this_key
    (out / 'trusted-key.bin').write_bytes(key)
    records.append(dict(name=name, header_only=str(head), image_bytes=image_bytes, block_count=block_count,
                        table_bytes=len(manifest) - 544, table_sha256=hashlib.sha256(manifest[544:]).hexdigest(),
                        header_sha256=hashlib.sha256(header + signature).hexdigest()))

# Sample blocks of the OS window for the crypto self-test (only run with boot-arg offline-owner-root-check). With a
# header-only manifest that test stops at its first vr_open; the samples keep its symbols defined.
report = json.loads(IMAGE.with_name('userdata-root.json').read_text())
window = next(w for w in report['windows'] if w['name'] == 'OS')
count = records[0]['block_count']
indices = [0, 1, count // 2, count - 1]
lengths = [4096, 4096, 4096, window['size'] - (count - 1) * 4096]
samples = out / 'original-samples.bin'
with IMAGE.open('rb') as stream:
    data = b''
    for index, length in zip(indices, lengths):
        stream.seek(window['offset'] + index * 4096)
        data += stream.read(length).ljust(4096, b'\0')
samples.write_bytes(data)

os_head, sys_head = records[0]['header_only'], records[1]['header_only']
assembly = out / 'inputs.S'
assembly.write_text(f'''// Generated header-only VerifiedRoot inputs (iOS 17 21H16). Hash tables live in userdata objects.
.section __DATA_CONST,__const
.balign 8
.globl _verified_root_manifest, _verified_root_manifest_bytes
_verified_root_manifest:
.incbin "{os_head}"
_verified_root_manifest_bytes:
.quad . - _verified_root_manifest
.globl _verified_root_key
_verified_root_key:
.incbin "{out / 'trusted-key.bin'}"
.balign 8
.globl _verified_root_sample_indices
_verified_root_sample_indices:
.quad {','.join(map(str, indices))}
.globl _verified_root_sample_lengths
_verified_root_sample_lengths:
.quad {','.join(map(str, lengths))}
.globl _verified_root_samples
_verified_root_samples:
.incbin "{samples}"
.balign 8
.globl _verified_root_systemos_manifest
_verified_root_systemos_manifest:
.incbin "{sys_head}"
.balign 8
.globl _verified_root_policies, _verified_root_policy_count
_verified_root_policies:
.quad _verified_root_manifest, 544
.quad _verified_root_systemos_manifest, 544
_verified_root_policy_count:
.quad 2
''')

# The bsd_init compile record names this include directory (build-fuxi-verified-root-policy.py makes it too).
(ROOT / 'build/xnu/ios-obj/EXPORT_HDRS/libsa').mkdir(parents=True, exist_ok=True)
recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json'
record = json.loads(recipe.read_text().strip().rstrip(','))
objects = []
for source in (ROOT / 'kits/VerifiedRoot/src/verified_root.c', ROOT / 'kits/VerifiedRoot/adapters/xnu_crypto.c',
               ROOT / 'scripts/qemu-entry/verified-root-probe.c'):
    destination = out / (source.stem + '.o')
    arguments, command, index = record['arguments'], [], 0
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command += ['-o', str(destination)]
            index += 1
        elif value in ('-MF', '-MJ'):
            index += 1
        elif value not in ('-MD', '-MP'):
            command.append(value)
        index += 1
    command += ['-I' + str(ROOT / 'kits/VerifiedRoot/include'), '-Werror']
    result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
    (out / (source.stem + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(result.stderr[-3500:])
    objects.append(destination)
subprocess.run(['xcrun', 'clang', '-target', TRIPLE, '-c', str(assembly), '-o', str(out / 'inputs.o')], check=True)
objects.append(out / 'inputs.o')
for obj in objects:
    check_object(obj.read_bytes(), obj.name)
archive = out / 'libVerifiedRootProbe.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
(out / 'inputs.json').write_text(json.dumps(dict(policies=records, trusted_key_sha256=hashlib.sha256(key).hexdigest(),
    sample_indices=indices, sample_lengths=lengths, header_only=True,
    archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest()), indent=2) + '\n')
print(f'{archive}: header-only VerifiedRoot inputs for {len(records)} policies')
