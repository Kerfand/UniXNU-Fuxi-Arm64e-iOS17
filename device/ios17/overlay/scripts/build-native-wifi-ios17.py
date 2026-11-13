#!/usr/bin/env python3
"""iOS 17 arm64e (iPhone15,2 21H16) build of the WiFi module (Ath12kWifi f6bf561, the legacy runtime): kernel objects for the board build.
Usage: build-native-wifi-ios17.py <module dir> <output dir> <compiler>. Prints one line "OBJ <path>" per object.

Derived from scripts/build-native-wifi.py (iPadOS 17 arm64). The differences are what the arm64e kernel requires:
- the kernel's own compiler and code generation: -target arm64e-apple-ios17.0 -mkernel with type-discriminated function pointers
  (-fptrauth-function-pointer-type-discrimination) and -disable-atexit-based-global-dtor-lowering, as every XNU 10063 C unit is built
  (build/xnu/ios-obj/.../startup.o.json). The module and the board code (FuxiWifiNw21.cpp, FuxiWifiNet.cpp) call each other through
  tables of function pointers (lx_platform, nw_board, nw_wifi_events): both sides have to sign and authenticate them the same way.
- an arm64 object does not link into the arm64e kernel at all.
- llvm-nm / llvm-objcopy from PATH (the Linux toolchain), not Homebrew."""
import subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

module = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
clang = sys.argv[3]
out.mkdir(parents=True, exist_ok=True)
subprocess.run(['bash', str(module / 'tools/prepare.sh')], check=True, stdout=subprocess.DEVNULL)
resource = subprocess.run([clang, '-print-resource-dir'], check=True, capture_output=True, text=True).stdout.strip() + '/include'
flags = ['-target', 'arm64e-apple-ios17.0', '-mkernel', '-fptrauth-function-pointer-type-discrimination',
         '-mllvm', '-disable-atexit-based-global-dtor-lowering',
         '-O2', '-g0', '-ffreestanding', '-fno-builtin', '-fno-common', '-fno-strict-aliasing', '-fwrapv',
         '-fno-stack-protector', '-Wno-incompatible-pointer-types', '-Wno-error', '-fno-delete-null-pointer-checks', '-mgeneral-regs-only', '-momit-leaf-frame-pointer', '-std=gnu11', '-w',
         '-DCONFIG_ATH12K_DEBUG', '-nostdinc', '-isystem', str(module / 'shim/libc'), '-isystem', resource, '-include', 'lx_pci.h',
         '-I', str(module / 'shim/include'), '-I', str(module / 'include'), '-I', str(module / 'out/pinned'), '-I', str(module / 'src')]
trees = {'ath12k': ['-I', str(module / 'out/ath12k-src'), '-I', str(module / 'out/ath12k-src/wifi7')],
         'mhi': ['-I', str(module / 'out/mhi-src')]}

jobs = []
for line in (module / 'tools/sources.txt').read_text().splitlines():
    line = line.strip()
    if not line or line.startswith('#'):
        continue
    tree, _, name = line.partition(':') if ':' in line else ('', '', line)
    if tree == 'ath12k':
        src = module / 'out/ath12k-src' / name
    elif tree == 'mhi':
        src = module / 'out/mhi-src' / name
    else:
        src = module / name
    obj = out / 'raw' / (('w7_' if '/wifi7/' in str(src) else tree + '_' if tree else '') + src.stem + '.o')
    extra = trees.get(tree, trees['ath12k'] if name.startswith('src/') else [])
    jobs.append((src, obj, extra))
(out / 'raw').mkdir(exist_ok=True)


def build(job):
    src, obj, extra = job
    # Incremental on the raw (unrenamed) objects only: an object newer than its source and this script is reused. The renamed copies
    # below are made from every raw object on every run, so a partly recompiled set can never mix renamed and unrenamed names.
    if obj.exists() and obj.stat().st_mtime > max(src.stat().st_mtime, Path(__file__).stat().st_mtime):
        return obj
    r = subprocess.run([clang, '-c', *flags, *extra, str(src), '-o', str(obj) + '.tmp'], capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr)
        raise SystemExit(f'compile failed: {src}')
    Path(str(obj) + '.tmp').replace(obj)
    return obj



def move_rodata(path):
    """The module's constant tables (__TEXT,__const, ~0x9700) from __TEXT to __DATA_CONST (both read-only once the kernel is up): the native kernel
    __TEXT has to fit the 21H16 collection's largest __PRELINK_TEXT gap (0x139499 with the current prelink roots) and the module's
    __cstring/__const (~0x1b000) took it to 0x140000. Its strings (~0x11d00) stay: __TEXT then ends at ~0x1372000
    (page 0x138000); the constants fit __DATA_CONST's room (0xf8b10 with __LASTDATA_CONST). Both margins are small. Only the section's segment name in the object's section header changes (an object
    file has one unnamed segment; the linker places each section by this name)."""
    import struct
    data = bytearray(path.read_bytes())
    if struct.unpack_from('<I', data, 0)[0] != 0xfeedfacf:
        raise SystemExit(f'{path}: not a 64-bit Mach-O object')
    ncmds = struct.unpack_from('<I', data, 16)[0]
    at = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, at)
        if cmd == 0x19:   # LC_SEGMENT_64
            nsects = struct.unpack_from('<I', data, at + 64)[0]
            for i in range(nsects):
                s = at + 72 + 80 * i
                sect = bytes(data[s:s + 16]).rstrip(b'\0')
                seg = bytes(data[s + 16:s + 32]).rstrip(b'\0')
                if seg == b'__TEXT' and sect == b'__const':
                    data[s + 16:s + 32] = b'__DATA_CONST'.ljust(16, b'\0')
                elif seg == b'__TEXT' and sect == b'__cstring':
                    # The strings (~0x11d00) go with the code: kept in __TEXT they took it to 0x13c000 (page padding included),
                    # past the 0x139499 gap. __TEXT_EXEC is read-only too and has the room (native code goes where the original
                    # kernel code was, ahead of the retained kexts).
                    data[s + 16:s + 32] = b'__TEXT_EXEC'.ljust(16, b'\0')
        at += size
    path.write_bytes(data)

with ThreadPoolExecutor(8) as pool:
    list(pool.map(build, jobs))

# The module's global names (sock_release, kernel_sendmsg, ...) are common in a kernel: give every one the prefix nwk_ except the entry
# points the board uses. The mapping comes from all raw objects together.
keep = {'_nw_wifi_start', '_nw_wifi_step', '_nw_wifi_stop', '_nw_wifi_stats', '_nw_wifi_set_events', '_nw_wifi_scan', '_nw_wifi_bss_count',
        '_nw_wifi_bss_get', '_nw_wifi_connect', '_nw_wifi_disconnect', '_nw_wifi_tx', '_nw_wifi_status_get', '_nw_wifi_where',
        '_nw_wifi_tx_reasons'}
raws = [o for _, o, _ in jobs]
names = set()
for o in raws:
    r = subprocess.run(['llvm-nm', '-g', '--defined-only', str(o)], check=True, capture_output=True, text=True)
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] not in keep:
            names.add(parts[2])
mapping = out / 'rename.txt'
mapping.write_text(''.join(f'{n} _nwk{n}\n' for n in sorted(names)))
finals = []
for o in raws:
    final = out / o.name
    subprocess.run(['llvm-objcopy', f'--redefine-syms={mapping}', str(o), str(final)], check=True)
    move_rodata(final)
    finals.append(final)
for o in finals:
    print(f'OBJ {o}')
