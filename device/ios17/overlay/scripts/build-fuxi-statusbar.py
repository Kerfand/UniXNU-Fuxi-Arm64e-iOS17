#!/usr/bin/env python3
"""Build FuxiStatusBar.dylib (device-ios17/statusbar/FuxiStatusBar.c) and load it from the NativeAdrenoMetal plugin.

  build-fuxi-statusbar.py ADDON_IN ADDON_OUT [SPRINGBOARD_PLIST]

SPRINGBOARD_PLIST (optional): the OS volume's System/Library/LaunchDaemons/com.apple.SpringBoard.plist from your own IPSW. A copy
with DYLD_INSERT_LIBRARIES naming the library is packed as daemons/com.apple.SpringBoard.plist (the LaunchDaemons overlay):
SpringBoard draws the home and lock screen status bars but never loads the Metal plugin.

1. Compile the library for iOS 17 arm64e with chained fixups and without function-pointer type discrimination (this
   toolchain's default; libobjc's class_addMethod and libdispatch authenticate IMPs and callbacks with a zero discriminator,
   so a type-discriminated pointer fails in WidgetRenderer and mediaserverd), (a dyld-info arm64e image makes dyld build its chain starts on the stack,
   which overflows 512 KiB secondary-thread stacks), sign it ad hoc with 4 KiB pages.
2. Put it into build/adreno-metal-plugin17e/NativeAdrenoMetal.bundle and give the plugin executable an LC_LOAD_WEAK_DYLIB
   @loader_path/FuxiStatusBar.dylib (once), then re-sign the bundle as build-adreno-metal-plugin.py does.
3. trust.json gets the new plugin CDHash and statusbar_cdhash (pack-ios17-hidpath.py adds it to the owner trust module).
4. ADDON_OUT = ADDON_IN with the bundle's files replaced and FuxiStatusBar.dylib added; every other entry is kept.
"""
import importlib.util, json, os, re, struct, subprocess, sys
from pathlib import Path

BASE = Path('/home/xiaomi/sadnut-fuxi-local-20261103')
TREE = BASE / 'UniXNU/out-ios17/fuxi/tree'
SRC = BASE / 'device-ios17/statusbar/FuxiStatusBar.c'
OUT = TREE / 'build/adreno-metal-plugin17e'
BUNDLE = OUT / 'NativeAdrenoMetal.bundle'
SDK = '/home/xiaomi/sdk/iPhoneOS17-21H16.sdk'
LOAD_PATH = b'@loader_path/FuxiStatusBar.dylib'
env = dict(os.environ)
env['PATH'] = ':'.join(['/home/xiaomi/.local/bin', str(BASE / 'bin'), '/home/xiaomi/unixnu-linux/CommandLineTools/usr/bin', env['PATH']])


def run(*cmd, capture=False):
    r = subprocess.run([str(c) for c in cmd], env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'{cmd[0]} failed ({r.returncode}):\n{r.stdout}\n{r.stderr}')
    return r.stdout + r.stderr


def cdhash(path):
    detail = run('codesign', '--display', '--verbose=4', path)
    assert re.search(r'^Page size=4096$', detail, re.M), path
    return re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1]


def load_commands(data):
    ncmds, size = struct.unpack_from('<II', data, 16)
    off, cmds = 32, []
    for _ in range(ncmds):
        c, s = struct.unpack_from('<II', data, off)
        cmds.append((c, off, s)); off += s
    return ncmds, size, cmds


def add_weak_dylib(exe):
    data = bytearray(exe.read_bytes())
    ncmds, size, cmds = load_commands(data)
    for c, off, s in cmds:
        if c in (0xc, 0x80000018) and data[off + 24:off + s].split(b'\0')[0] == LOAD_PATH:
            return False
    first = min(struct.unpack_from('<I', data, off + 72 + 80 * k + 48)[0]
                for c, off, s in cmds if c == 0x19
                for k in range(struct.unpack_from('<I', data, off + 64)[0])
                if struct.unpack_from('<I', data, off + 72 + 80 * k + 48)[0])
    name = LOAD_PATH + b'\0'
    cmdsize = (24 + len(name) + 7) // 8 * 8
    end = 32 + size
    if end + cmdsize > first:
        sys.exit('no room for the load command')
    assert all(b == 0 for b in data[end:end + cmdsize]), 'header padding is not empty'
    cmd = struct.pack('<IIIIII', 0x80000018, cmdsize, 24, 2, 0x10000, 0x10000) + name
    data[end:end + cmdsize] = cmd + bytes(cmdsize - len(cmd))
    struct.pack_into('<II', data, 16, ncmds + 1, size + cmdsize)
    exe.write_bytes(bytes(data))
    return True


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    addon_in, addon_out = Path(sys.argv[1]), Path(sys.argv[2])
    sb_plist = Path(sys.argv[3]) if len(sys.argv) == 4 else None
    lib = BUNDLE / 'FuxiStatusBar.dylib'
    run('xcrun', 'clang', '-target', 'arm64e-apple-ios17.0', '-isysroot', SDK, '-O2', '-Wall', '-Wextra', '-Werror',
        '-fno-ptrauth-function-pointer-type-discrimination', '-dynamiclib', '-install_name', LOAD_PATH.decode(), '-Wl,-fixup_chains', SRC, '-lobjc', '-framework', 'IOKit', '-o', lib)
    data = lib.read_bytes()
    _, _, cmds = load_commands(data)
    assert any(c == 0x80000034 for c, _, _ in cmds), 'FuxiStatusBar.dylib must use chained fixups'
    run('codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.blu-sharky.FuxiStatusBar', lib)
    sb_hash = cdhash(lib)
    added = add_weak_dylib(BUNDLE / 'NativeAdrenoMetal')
    run('codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.blu-sharky.NativeAdrenoMetal', BUNDLE)
    run('codesign', '--verify', '--strict', BUNDLE)
    plugin_hash = cdhash(BUNDLE)
    trust_path = OUT / 'trust.json'
    trust = json.loads(trust_path.read_text())
    trust.update(cdhash=plugin_hash, statusbar_cdhash=sb_hash)
    trust_path.write_text(json.dumps(trust, indent=2) + '\n')
    spec = importlib.util.spec_from_file_location('addfx', BASE / 'device-ios17/scripts/add-fuxi-wallpaper-to-addon.py')
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    prefix = 'addons/NativeAdrenoMetal.bundle/'
    files = [(n, mode, d) for n, mode, d in m.read_addon(addon_in.read_bytes()) if not (n.startswith(prefix) and n != prefix + 'Info.plist' and not (BUNDLE / n[len(prefix):]).exists())]
    have = {n for n, _, _ in files}
    out = []
    for n, mode, d in files:
        if n.startswith(prefix):
            d = (BUNDLE / n[len(prefix):]).read_bytes()
        out.append((n, mode, d))
    for p in sorted(BUNDLE.rglob('*')):
        n = prefix + p.relative_to(BUNDLE).as_posix()
        if p.is_file() and n not in have:
            out.append((n, 0o755 if p.suffix == '.dylib' else 0o644, p.read_bytes()))
    names_bundle = sorted(prefix + p.relative_to(BUNDLE).as_posix() for p in BUNDLE.rglob('*') if p.is_file())
    assert sorted(n for n, _, _ in out if n.startswith(prefix)) == names_bundle
    if sb_plist:
        import plistlib
        job = plistlib.loads(sb_plist.read_bytes())
        assert job.get('Label') == 'com.apple.SpringBoard', 'not the SpringBoard job'
        env_vars = dict(job.get('EnvironmentVariables', {}))
        env_vars['DYLD_INSERT_LIBRARIES'] = '/System/Library/Extensions/NativeAdrenoMetal.bundle/FuxiStatusBar.dylib'
        job['EnvironmentVariables'] = env_vars
        out = [e for e in out if e[0] != 'daemons/com.apple.SpringBoard.plist']
        out.append(('daemons/com.apple.SpringBoard.plist', 0o644, plistlib.dumps(job, fmt=plistlib.FMT_XML)))
    blob = m.write_addon(out)
    assert m.read_addon(blob) == out
    addon_out.write_bytes(blob)
    print(f'FuxiStatusBar.dylib cdhash {sb_hash}; plugin cdhash {plugin_hash}; load command {"added" if added else "already present"}')
    print(f'{addon_out}: {len(out)} files, {len(blob)} bytes')


if __name__ == '__main__':
    main()
