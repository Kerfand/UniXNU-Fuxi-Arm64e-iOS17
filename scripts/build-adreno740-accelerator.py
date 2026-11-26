#!/usr/bin/env python3
"""iOS 17 21H16 (arm64e fileset): build the staged AGXAccelerator class (drivers/Adreno740/NativeAdrenoAccelerator.cpp)
against the original IOAcceleratorES of the 21H16 IOGPUFamily (scripts/data/ioaccelerator-base-abi-21H16.json), as a
"late" object: its static initializer is removed from __mod_init_func and exported so the board probe runs it after
IOGPUFamily has loaded. The original constructor/destructor are reached through owned BTI/B adapters whose
<name>.original targets XnuPrelink retargets after placing native text; the metaclass is an absolute data address.
The objects are appended to build/adreno740/libAdreno740.a, which the kernel links."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from kernel_target import ARCH, TRIPLE

OUT = ROOT / 'build/adreno740-accelerator'
OUT.mkdir(parents=True, exist_ok=True)
KC = ROOT.parents[1] / 'ipsw/21H16/kernelcache.decompressed'
plan_path = ROOT / 'scripts/data/ioaccelerator-base-abi-21H16.json'
plan = json.loads(plan_path.read_text())


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=180, **kw)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


if ARCH != 'arm64e':
    sys.exit('This builder is for the arm64e 21H16 fileset')
if plan['kernelcache_sha256'] != sha(KC):
    sys.exit('IOAcceleratorES plan is for a different kernelcache')
if plan['operational_overrides'] or plan['instance_bytes'] != 136 or plan['virtual_entries'] != 168:
    sys.exit('The audited original base is not the expected 136-byte, 168-entry class without operational overrides')

record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
source = ROOT / 'drivers/Adreno740/NativeAdrenoAccelerator.cpp'
obj = OUT / 'NativeAdrenoAccelerator.o'
cmd, i, args = [], 0, record['arguments']
while i < len(args):
    v = args[i]
    if v == record['file']:
        cmd.append(str(source))
    elif v == '-o':
        cmd += ['-o', str(obj)]
        i += 1
    elif v == '-Xclang' and args[i + 1].startswith('-W'):
        i += 1
    elif not v.startswith('-W') and v not in ('-MD', '-MP'):
        cmd.append(v)
    i += 1
    if v in ('-MF', '-MJ'):
        i += 1
cmd += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator',
        '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-Wno-overloaded-virtual',
        '-Wno-unknown-warning-option', '-I', str(ROOT / '.cache/xnu-work/osfmk')]
subprocess.run(cmd, cwd=record['directory'], check=True)

undefined = {l.split()[-1] for l in run(['llvm-nm', '-u', obj], text=True).splitlines() if 'IOAcceleratorES' in l}
bindings = {'__ZN15IOAcceleratorESC2EPK11OSMetaClass': int(plan['protected_compatible_constructor_aliases'][0], 16),
            '__ZN15IOAcceleratorESD2Ev': int(plan['nondeleting_destructor'], 16),
            '__ZN15IOAcceleratorES10gMetaClassE': int(plan['metaclass'], 16)}
extra = undefined - set(bindings)
if extra or not undefined & set(bindings):
    sys.exit('Unexpected original-base imports: %s' % sorted(extra))
text, addresses = '.text\n.p2align 2\n', ''
for n, a in sorted(bindings.items()):
    if n not in undefined:
        continue
    if n.endswith('gMetaClassE'):
        addresses += f'.globl {n}\n.set {n}, {a:#x}\n'
    else:
        addresses += f'.globl {n}.original\n.set {n}.original, {a:#x}\n'
        text += f'.globl {n}\n{n}:\n bti c\n b {n}.original\n'
(OUT / 'original-bindings.S').write_text(text)
(OUT / 'original-addresses.S').write_text(addresses)
clang = record['arguments'][0]
for stem in ('original-bindings', 'original-addresses'):
    run([clang, '-target', TRIPLE, '-c', OUT / f'{stem}.S', '-o', OUT / f'{stem}.o'])
binding_objects = [OUT / 'original-bindings.o', OUT / 'original-addresses.o']

late = OUT / 'NativeAdrenoAccelerator.late.o'
run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
     '--globalize-symbol=__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp', obj, late])
archive = OUT / 'libAdreno740Accelerator.late.a'
archive.unlink(missing_ok=True)
run(['llvm-ar', 'rcs', archive, late, *binding_objects])
native = ROOT / 'build/adreno740/libAdreno740.a'
if not native.exists():
    sys.exit('Build libAdreno740.a first (build-arm-driver.py Adreno740, build-adreno740-kits.py)')
run(['llvm-ar', 'rcs', native, late, *binding_objects])
(OUT / 'build-report.json').write_text(json.dumps(dict(
    source_sha256={str(source.relative_to(ROOT)): sha(source),
                   'OriginalAcceleratorABI.h': sha(source.resolve().parent.parent / 'AdrenoMetal/OriginalAcceleratorABI.h')},
    archive_sha256=sha(archive), original_abi_sha256=sha(plan_path), bindings={n: hex(a) for n, a in bindings.items()},
    original_instructions_modified=False, runtime_qualified=False), indent=2) + '\n')
print('PASS: staged AGXAccelerator builds against the 21H16 IOAcceleratorES imports:', sorted(undefined))
