#!/usr/bin/env python3
"""BlueTool for fuxi iOS 17 (21H16): the UniXNU QcaBluetooth XPC bootstrap adapter (userland/BlueTool.c), delivered through the boot
GPU addon like the patched bluetoothd (https://unixnu.com/porting/bluetooth, native-ios27.md "Runtime interfaces").

  build-fuxi-bluetool.py BLUETOOL_C ADDON_IN ADDON_OUT

bluetoothd asks com.apple.BlueTool to run the "boot"/"init" script before it opens the transport; the adapter answers by reading
kern.qbt_power_on (qbt_transport.c) and reports kern.qbt_firmware. Apple's BlueTool (Broadcom scripts) is replaced, not patched.
1. Compile arm64e for iOS 17.0 against the 21H16 SDK, sign ad hoc with 4 KiB pages, identifier org.unixnu.bluetooth.BlueTool and
   com.apple.private.security.no-container; trust.json gets its CDHash (pack-ios17-hidpath.py adds it with BLUETOOL=1).
2. ADDON_OUT = ADDON_IN + btd/BlueTool (/private/var/ngpu/btd/BlueTool) + daemons/com.apple.BlueTool.plist (root, Interactive,
   transactions on, pressured exit off: the job keeps the Mach service com.apple.BlueTool).
"""
import importlib.util, json, os, plistlib, re, subprocess, sys
from pathlib import Path

BASE = Path('/home/xiaomi/sadnut-fuxi-local-20261103')
OUT = BASE / 'UniXNU/out-ios17/fuxi/tree/build/fuxi-bluetool'
SDK = Path('/home/xiaomi/sdk/iPhoneOS17-21H16.sdk')   # the SDK build-fuxi-audio-hal.py builds against (headers and stubs)
TARGET = '/private/var/ngpu/btd/BlueTool'
CLANG = '/home/xiaomi/unixnu-linux/llvm/bin/clang'
env = dict(os.environ)
env['PATH'] = ':'.join(['/home/xiaomi/.local/bin', '/home/xiaomi/unixnu-linux/CommandLineTools/usr/bin', env['PATH']])


def run(*cmd):
    r = subprocess.run([str(c) for c in cmd], env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'{cmd[0]} failed ({r.returncode}):\n{r.stdout}\n{r.stderr}')
    return r.stdout + r.stderr


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    src, addon_in, addon_out = map(Path, sys.argv[1:])
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / 'BlueTool'
    run('xcrun', 'clang', '-target', 'arm64e-apple-ios17.0', '-isysroot', SDK, '-O2', '-Wall', '-Wextra', '-fblocks',
        '-fstack-protector-strong', src, '-o', exe)
    ents = OUT / 'BlueTool.entitlements.plist'
    ents.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True}))
    run('codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.unixnu.bluetooth.BlueTool',
        '--entitlements', ents, exe)
    detail = run('codesign', '--display', '--verbose=4', exe)
    assert re.search(r'^Page size=4096$', detail, re.M), detail
    h = re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1]
    (OUT / 'trust.json').write_text(json.dumps({'cdhash': h}, indent=2) + '\n')
    job = {'Label': 'com.apple.BlueTool', 'ProgramArguments': [TARGET, '-R'], 'MachServices': {'com.apple.BlueTool': True},
           'UserName': 'root', 'POSIXSpawnType': 'Interactive', 'EnableTransactions': True, 'EnablePressuredExit': False,
           'ThrottleInterval': 3}
    spec = importlib.util.spec_from_file_location('addfx', BASE / 'device-ios17/scripts/add-fuxi-wallpaper-to-addon.py')
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    mine = {'btd/BlueTool': (0o755, exe.read_bytes()),
            'daemons/com.apple.BlueTool.plist': (0o644, plistlib.dumps(job, fmt=plistlib.FMT_XML))}
    out = [(n, mode, d) for n, mode, d in m.read_addon(addon_in.read_bytes()) if n not in mine]
    out += [(n, mode, d) for n, (mode, d) in mine.items()]
    blob = m.write_addon(out)
    assert m.read_addon(blob) == out
    addon_out.write_bytes(blob)
    print(f'BlueTool cdhash {h}; {addon_out}: {len(out)} files, {len(blob)} bytes')


if __name__ == '__main__':
    main()
