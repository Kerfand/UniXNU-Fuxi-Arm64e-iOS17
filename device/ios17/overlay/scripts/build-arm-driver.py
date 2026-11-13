#!/usr/bin/env python3
"""Compile the generic platform expert against the actual source-kernel ABI."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import struct
import sys
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('component', choices=['ArmVirtPlatform', 'ArmDTPlatform', 'ArmCpu', 'NativeAES', 'NativeVirtioBlock', 'NativeRamBlock', 'NativeMobileDisplay', 'NativeVirtioConsole', 'NativeLegacyDisplay', 'NativeGOPPresenter', 'NativeOfflineGPU', 'NativeOfflineAccelerator', 'NativeIOGPUProviderProbe', 'NativeUfs', 'NativeHidInput', 'NativeKeyStoreEvents', 'NativeAdreno', 'Adreno740'])
parser.add_argument('--negative-aes-kat', action='store_true', help='Isolated own-source wrong-answer refusal control; never changes original inputs')
parser.add_argument('--verified-root', action='store_true', help='Authenticate all actual disk reads against an independently signed immutable policy')
parser.add_argument('--verified-root-bundle', action='store_true', help='Select exactly one capacity-matching policy per device, then independently authenticate it')
parser.add_argument('--negative-verified-block', action='store_true', help='Negative own-buffer control: corrupt only the completed DMA snapshot')
args = parser.parse_args()
if args.verified_root_bundle and not args.verified_root:
    parser.error('A policy bundle requires verified block mode')
if args.verified_root_bundle and args.negative_verified_block:
    parser.error('Select either bundle policy qualification or the isolated DMA snapshot fault')
if args.negative_verified_block and args.component != 'NativeVirtioBlock':
    parser.error('The isolated DMA snapshot fault requires NativeVirtioBlock')
if args.verified_root and args.component not in ('NativeVirtioBlock', 'NativeUfs'):
    parser.error('Verified block mode requires NativeVirtioBlock or NativeUfs')
if args.negative_verified_block and not args.verified_root:
    parser.error('The isolated input fault requires verified block mode')
component = args.component
recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
record = json.loads(recipe.read_text().strip().rstrip(','))
source = ROOT / 'drivers' / ('GopFramebuffer' if component in ('NativeLegacyDisplay','NativeGOPPresenter') else 'NativeMetal' if component in ('NativeOfflineGPU','NativeOfflineAccelerator','NativeIOGPUProviderProbe') else component) / {'NativeAdreno':'NativeAdrenoService.cpp','Adreno740':'NativeAdrenoService.cpp'}.get(component, component + '.cpp')
out = ROOT / 'build' / {'ArmVirtPlatform': 'armvirt-platform', 'ArmDTPlatform': 'armdt-platform', 'ArmCpu': 'armcpu', 'NativeAES': 'native-aes',
                        'NativeVirtioBlock': 'native-virtio-block', 'NativeRamBlock': 'native-ram-block',
                        'NativeMobileDisplay': 'native-mobile-display', 'NativeVirtioConsole': 'native-virtio-console', 'Adreno740': 'adreno740',
                        'NativeLegacyDisplay': 'native-legacy-display', 'NativeGOPPresenter': 'native-gop-presenter', 'NativeOfflineGPU':'native-offline-gpu','NativeOfflineAccelerator':'native-offline-accelerator','NativeIOGPUProviderProbe':'native-original-provider', 'NativeUfs':'native-ufs', 'NativeHidInput':'native-hid-input', 'NativeKeyStoreEvents':'native-keystore-events', 'NativeAdreno':'native-adreno'}[component]
if args.verified_root and component == 'NativeVirtioBlock':
    out = ROOT / ('build/native-virtio-block-verified-fault' if args.negative_verified_block else 'build/native-virtio-block-verified')
    if args.verified_root_bundle:
        out = ROOT / 'build/native-virtio-block-verified-bundle'
out.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'scripts'))
from kernel_target import TRIPLE as KERNEL_TRIPLE
if component == 'NativeAES' and KERNEL_TRIPLE.startswith('arm64e-') and not args.negative_aes_kat:
    # 21H16 ABI: getAlgorithmName is first declared by IOCryptoAccelerator (its vtable slot 168 carries the PAC
    # discriminator of _ZN19IOCryptoAccelerator16getAlgorithmNameEv, 0x6497), not by IOAESAccelerator as the
    # shared OriginalAESABI.h says. arm64 has no PAC so the iPadOS build never sees it; on arm64e the override and
    # the calls would sign with the wrong discriminator. Compile a copy with the declaration moved.
    header = (source.with_name('OriginalAESABI.h')).read_text()
    parent = '  OSDeclareDefaultStructors(IOCryptoAccelerator)\n'
    own = '  virtual const char *getAlgorithmName();\n'
    if header.count(parent) != 1 or header.count(own) != 1:
        raise SystemExit('OriginalAESABI.h: getAlgorithmName/IOCryptoAccelerator anchors changed')
    header = header.replace(own, '  const char *getAlgorithmName() override;\n')
    header = header.replace(parent, parent + 'public:\n  virtual const char *getAlgorithmName();\n')
    (out / 'OriginalAESABI.h').write_text(header)
    (out / 'NativeAES.cpp').write_text(source.read_text())
    source = out / 'NativeAES.cpp'
if args.negative_aes_kat:
    if component != 'NativeAES':
        parser.error('The own-source KAT control requires NativeAES')
    out = ROOT / 'build/native-aes-kat-negative'
    out.mkdir(parents=True, exist_ok=True)
    text = source.read_text()
    marker = '0x76,0x49,0xab,0xac'
    if text.count(marker) != 1:
        raise SystemExit('The exact own-source NIST answer was not uniquely identified')
    (out / 'OriginalAESABI.h').write_bytes(source.with_name('OriginalAESABI.h').read_bytes())
    source = out / 'NativeAES.cpp'
    source.write_text(text.replace(marker, '0x77,0x49,0xab,0xac'))
destination = out / (component + '.o')
if component == 'NativeUfs' and KERNEL_TRIPLE.startswith('arm64e-'):
    # iOS 17: the 128 KiB root-layout scratch comes from the heap at first use (the native __DATA is full); compile a copy
    # of the driver sources with that one change. The shared sources are unchanged.
    ufs_copy = out / 'ufs-src'
    ufs_copy.mkdir(exist_ok=True)
    for f in source.parent.iterdir():
        if f.is_file() and f.suffix in ('.h', '.cpp'):
            (ufs_copy / f.name).write_bytes(f.read_bytes())
    layout = ufs_copy / 'RootLayout.h'
    text = layout.read_text()
    old = 'static uint8_t gScratch[kDataMax];\n'
    if text.count(old) != 1:
        raise SystemExit('NativeUfs RootLayout.h: gScratch anchor changed')
    layout.write_text(text.replace(old,
        '// ios17 (build-arm-driver.py): the scratch comes from the heap at first use; the native __DATA has no room for it.\n'
        'static uint8_t *gScratchHeap;\n'
        'static inline uint8_t *gScratchBuffer (void) {\n'
        '  if (!gScratchHeap) {\n'
        '    gScratchHeap = static_cast<uint8_t *> (IOMallocAligned (kDataMax, 4096));\n'
        '    if (!gScratchHeap) panic ("NativeUfs: no memory for the %u-byte root scratch", (unsigned) kDataMax);\n'
        '  }\n'
        '  return gScratchHeap;\n'
        '}\n'
        '#define gScratch (gScratchBuffer ())\n'))
    source = ufs_copy / source.name
arguments = record['arguments']
command = []
index = 0
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(source))
    elif value == '-o':
        command.extend(['-o', str(destination)])
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command.extend(['-Wall', '-Wextra', '-Werror', '-Wno-unknown-warning-option',
                '-Wno-missing-designated-field-initializers',
                '-Wno-unused-parameter',
                '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier', '-I', str(ROOT / '.cache/xnu-work/osfmk')])
if component in ('NativeVirtioBlock', 'NativeRamBlock', 'NativeUfs'):
    command += ['-I', str(ROOT / 'build/storage-abi/include')]
    if args.verified_root:
        command += ['-I', str(ROOT / 'kits/VerifiedRoot/include'), '-I', str(ROOT / 'drivers/NativeVirtioBlock'), '-DNATIVE_VERIFIED_BLOCK=1']
    if args.negative_verified_block:
        command += ['-DNATIVE_VERIFIED_BLOCK_INJECT_FAULT=1']
    if args.verified_root_bundle:
        command += ['-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1']
if component == 'NativeUfs' and KERNEL_TRIPLE.startswith('arm64e-'):
    # iOS 17 bring-up: on-screen read marks of exec-stage-probe (unixnu_stage lives in the patched kernel).
    command += ['-DUNIXNU_STAGE_PROBE=1']
if component == 'NativeUfs':
    # The UFS transport publishes the SHNGROOT windows and, when NativeVirtioBlock
    # is linked with its verified personality, provides native_ufs_block_authenticates.
    command += ['-mgeneral-regs-only']
if component=='NativeOfflineGPU':
    command += ['-I',str(ROOT/'drivers/NativeVirtioConsole')]
if component in ('NativeLegacyDisplay','NativeHidInput'):
    command += ['-I', str(out)]
if component in ('NativeAdreno', 'Adreno740'):
    command += ['-I', str(ROOT / 'kits/ArmSmmu4K'), '-I', str(ROOT / 'kits/AdrenoPM4'),
                '-I', str(ROOT / 'kits/sheng'), '-I', str(ROOT / 'kits/AdrenoFirmware'),
                '-I', str(ROOT / 'build/fuxi-gmu')]
(out / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(out / 'compile.log').write_text(result.stdout)
if result.returncode:
    raise SystemExit(result.stdout[-3500:])
data = destination.read_bytes()
# The kernel slice (arm64 or arm64e) and deployment follow UNIXNU_XNU_ARCH / UNIXNU_IOS_VERSION.
sys.path.insert(0, str(ROOT / 'scripts'))
from kernel_target import check_object, TRIPLE
check_object(data, 'platform object')
archive = out / ('lib' + component + '.a')
archive.unlink(missing_ok=True)
archive_objects = [destination]
if component in ('NativeUfs', 'NativeVirtioBlock', 'NativeRamBlock'):
    # fuxi has no repository-carried live storage capture. Link the pinned
    # original IOStorageFamily imports from the unchanged iOS 17.7 21H16
    # kernelcache instead of producing them from audit-storage-interface.py.
    # Exactly one archive carries the absolute-symbol object: NativeVirtioBlock
    # when it is built, otherwise NativeUfs (the iOS 17 storage bring-up links
    # NativeUfs alone, without the Virtio/verified-root chain).
    bindings_source = ROOT / 'scripts/data/native-storage-original-bindings.S'
    text = bindings_source.read_text()
    bindings = {}
    for line in text.splitlines():
        match = re.match(r'\.set\s+(\S+),\s*(0x[0-9a-fA-F]+)', line)
        if match:
            bindings[match.group(1)] = match.group(2)
    if not bindings:
        raise SystemExit('Pinned original storage bindings are empty')
    (out / 'bindings.json').write_text(json.dumps(dict(
        compiled_object_sha256=hashlib.sha256(destination.read_bytes()).hexdigest(),
        bindings=bindings, original_code_modified=False,
        live_capture=False), indent=2) + '\n')
    carrier = 'NativeVirtioBlock' if os.environ.get('XNU_NATIVE_VIRTIO_BLOCK', '0') == '1' else 'NativeUfs'
    if component == carrier:
        bindings_object = out / 'storage-original-bindings.o'
        sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
        if TRIPLE.startswith('arm64e-'):
            # arm64e fileset: an N_ABS function symbol breaks twice. ld64 leaves authenticated
            # pointers to it out of the chained fixups (vtable slots are never signed), and the
            # prelinker moves native __TEXT_EXEC without a split-segment record for BL/B to it
            # (the call lands at target + text displacement). So each function becomes a native
            # BTI/B adapter <name> with its fixed address in <name>.original; XnuPrelink
            # relocate_original_stubs retargets the B after placement. Data (gMetaClass via
            # the GOT, the vtable) keep plain absolute values: the fileset is not slid.
            # The assembler cannot encode B to an absolute symbol defined in the same file, so
            # the absolute values and the adapters (B to an undefined <name>.original) are two
            # objects; ld then resolves each B to the N_ABS symbol.
            absolute = ['// Generated from native-storage-original-bindings.S for arm64e: fixed addresses.']
            adapters = ['// Generated from native-storage-original-bindings.S for arm64e: BTI/B adapters.',
                        '.section __TEXT,__text,regular,pure_instructions']
            for name, value in sorted(bindings.items()):
                if name.startswith('__ZTV') or name.endswith('gMetaClassE'):
                    absolute += [f'.globl {name}', f'.set {name}, {value}']
                else:
                    absolute += [f'.globl "{name}.original"', f'.set "{name}.original", {value}']
                    adapters += [f'.globl {name}', '.p2align 2', f'{name}:', '  bti c', f'  b "{name}.original"']
            bindings_source = out / 'storage-original-bindings-arm64e.S'
            bindings_source.write_text('\n'.join(absolute) + '\n')
            adapters_source = out / 'storage-original-adapters-arm64e.S'
            adapters_source.write_text('\n'.join(adapters) + '\n')
            adapters_object = out / 'storage-original-adapters.o'
            subprocess.run(['xcrun', 'clang', '-target', TRIPLE, '-isysroot', sdk, '-Werror',
                            '-Wno-incompatible-sysroot', '-c', adapters_source, '-o', adapters_object], check=True)
            archive_objects.append(adapters_object)
        subprocess.run(['xcrun', 'clang', '-target', TRIPLE, '-isysroot', sdk,
                        '-Werror', '-Wno-incompatible-sysroot', '-c', bindings_source, '-o', bindings_object], check=True)
        archive_objects.append(bindings_object)
subprocess.run(['xcrun', 'ar', 'rcs', archive, *archive_objects], check=True)
print(f'{component}: -Werror build and actual ARM64/iOS 17 object audit passed')
