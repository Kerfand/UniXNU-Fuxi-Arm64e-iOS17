#!/usr/bin/env python3
"""Bind NativeAES to the original IOAESAccelerator of the iOS 17.7 21H16 kernelcache (arm64e fileset).

scripts/data/native-aes-original-bindings.S records the 22 original addresses (see its header for the evidence).
On arm64e an N_ABS function symbol cannot be used directly: ld64 leaves authenticated pointers to it out of the
chained fixups (inherited vtable slots would never be signed) and the prelinker moves native text without a
split-segment record for BL/B to it. So, as for the storage bindings in build-arm-driver.py, every function gets a
native BTI/B adapter <name> with its address in <name>.original (XnuPrelink relocate_original_stubs retargets the B),
and only data (gMetaClass) stays a plain absolute symbol; the fileset is not slid.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from kernel_target import TRIPLE, check_object

out = ROOT / 'build/native-aes'
recorded = ROOT / 'scripts/data/native-aes-original-bindings.S'
bindings = {}
for line in recorded.read_text().splitlines():
    match = re.match(r'\.set\s+(\S+),\s*(0x[0-9a-fA-F]+)', line)
    if match:
        bindings[match.group(1)] = match.group(2)
if len(bindings) != 22:
    raise SystemExit(f'Expected the 22 recorded IOAESAccelerator bindings, found {len(bindings)}')
absolute = ['// Generated from native-aes-original-bindings.S: fixed addresses.']
adapters = ['// Generated from native-aes-original-bindings.S: BTI/B adapters.',
            '.section __TEXT,__text,regular,pure_instructions']
arm64e = TRIPLE.startswith('arm64e-')
for name, value in sorted(bindings.items()):
    if not arm64e or name.endswith('gMetaClassE'):
        absolute += [f'.globl {name}', f'.set {name}, {value}']
    else:
        absolute += [f'.globl "{name}.original"', f'.set "{name}.original", {value}']
        adapters += [f'.globl {name}', '.p2align 2', f'{name}:', '  bti c', f'  b "{name}.original"']
objects = [out / 'NativeAES.o']
for stem, lines in (('original-bindings', absolute), ('original-adapters', adapters)):
    if stem == 'original-adapters' and not arm64e:
        continue
    source = out / (stem + '.S')
    source.write_text('\n'.join(lines) + '\n')
    subprocess.run(['xcrun', 'clang', '-target', TRIPLE, '-Werror', '-Wno-incompatible-sysroot', '-c', str(source),
                    '-o', str(out / (stem + '.o'))], check=True)
    check_object((out / (stem + '.o')).read_bytes(), stem)
    objects.append(out / (stem + '.o'))
archive = out / 'libNativeAES.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
print(f'PASS: NativeAES bound to {len(bindings)} recorded 21H16 IOAESAccelerator addresses'
      + (' through BTI/B adapters' if arm64e else ''))
