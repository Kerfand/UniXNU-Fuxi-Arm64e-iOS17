#!/usr/bin/env python3
"""Patched iOS 17 21H16 bluetoothd for fuxi, delivered through the boot GPU addon.

  build-fuxi-bluetoothd.py BLUETOOTHD PLIST ADDON_IN ADDON_OUT

BLUETOOTHD / PLIST: the OS volume's /usr/sbin/bluetoothd and /System/Library/LaunchDaemons/com.apple.bluetoothd.plist (local copies;
Apple files are never committed).
1. Apply PATCHES (file offset, expected bytes, new bytes) - each one is checked against the original bytes first.
2. Sign ad hoc with 4 KiB pages, the original identifier and the original entitlements; trust.json gets its CDHash
   (pack-ios17-hidpath.py adds it to the owner trust module with BTD_PATCHED=1).
3. ADDON_OUT = ADDON_IN + btd/bluetoothd (lands at /private/var/ngpu/btd/bluetoothd) + daemons/com.apple.bluetoothd.plist whose
   ProgramArguments point there (the addon's daemons/ is unioned over /System/Library/LaunchDaemons).
"""
import importlib.util, json, os, plistlib, re, subprocess, sys
from pathlib import Path

BASE = Path('/home/xiaomi/sadnut-fuxi-local-20261103')
OUT = BASE / 'UniXNU/out-ios17/fuxi/tree/build/fuxi-bluetoothd'
TARGET = '/private/var/ngpu/btd/bluetoothd'
env = dict(os.environ)
env['PATH'] = ':'.join(['/home/xiaomi/.local/bin', str(BASE / 'bin'), '/home/xiaomi/unixnu-linux/CommandLineTools/usr/bin', env['PATH']])

# (file offset, original bytes, patched bytes); empty = an unmodified copy (verifies the delivery chain).
# fuxi 2026-11-15 trace (btdtrace.py): os_log sites of the power-on path call stat(format) instead; the kernel syscall probe prints it.
PATCHES = [
    # fuxi 2026-11-22: type 0 controller table +0x60 is NULL; 0x100171184 (braaz x0) crashes with SIGBUS/PAC after HCI reset.
    # Replace braaz x0 (d61f081f) with ret (d65f03c0) so the cleanup path returns cleanly.
    (0x171184, bytes.fromhex('1f081fd6'), bytes.fromhex('c0035fd6')),
    # fuxi 2026-11-21 U52: with type 0 (U51) no vendor command is sent any more, but the controller answers the legacy LE scan
    # commands (0x200b) with 0x0c Command Disallowed, and the scan-stop callback (21H16 0x10005e1e0, "Scan stopped improperly
    # with result %u -- restart") reports a nonzero result as fatal and traps. The result is taken as 0 (scan stopped):
    # mov x20, x1 -> mov x20, xzr ; cbnz w1, fatal -> nop. Extended scan (the official capability patches) is the real fix.
    (0x5e21c, bytes.fromhex('f40301aa'), bytes.fromhex('f4031faa')),
    (0x5e220, bytes.fromhex('81210035'), bytes.fromhex('1f2003d5')),
    # fuxi 2026-11-21 U51, official routing (UniXNU QcaBluetooth native-ios27.md "Every product reaches the original constructor
    # with controller type zero"): the 21H16 factory is 0x10034900c - 75 branches new(0x588) + 0x1002fa850 (a b to the model
    # constructor 0x1002f9c90, which calls the base constructor 0x1002fa7b8) with w1 = the product's controller type. The base
    # constructor keeps that type in x20 and stores it at model+0x2a8. Both constructors now keep zero instead:
    # mov x20, x1 -> mov x20, xzr.
    (0x2f9cac, bytes.fromhex('f40301aa'), bytes.fromhex('f4031faa')),
    (0x2fa7cc, bytes.fromhex('f40301aa'), bytes.fromhex('f4031faa')),
    # fuxi 2026-11-21 U46: the HCI command timeout handler (21H16 block 0x1004a6230, scheduled by 0x1004a3f08 for each
    # command) logs the stalled command and ends in brk #1 (U45: SIGTRAP at 0x1004a6754 after the Broadcom vendor command
    # of the chip path got no reply the daemon accepts from the Qualcomm controller). It returns at once instead:
    # sub sp, sp, #0x160 -> retab (the pacibsp before it pairs with the retab).
    (0x4a6234, bytes.fromhex('ff8305d1'), bytes.fromhex('ff0f5fd6')),
    # fuxi 2026-11-21 U45: the error reporter (21H16 0x10056fd30, args code, name, message, fatal) ends in exit(1) on
    # both branches (fatal 0x10011725c, non-fatal 0x100117314 logs and exits too; U44 still exited with 0xe12). On the
    # Qualcomm controller the Broadcom vendor commands of the chip path fail (BD_VSC_OLYMPIC_SET_DIAG_DEBUG_EVENT_BEHAVIOR),
    # so the reporter returns at once: sub sp, sp, #0xd0 -> retab (the pacibsp before it pairs with the retab).
    (0x56fd34, bytes.fromhex('ff4303d1'), bytes.fromhex('ff0f5fd6')),
    # fuxi 2026-11-21 U43 experiment: the HCI init sequence completion (21H16 0x100195f34) sets state 3 on any step
    # error, bluetoothd then logs "HCI initialization failed" (0x6c) and exits 1, and launchd's restart loop ended in a
    # kernel panic (U42). The controller completed all 450 commands with status 0, so the failing step is a Broadcom
    # path check; the cinc goes, the state is always 2 (initialized).
    (0x195f3c, bytes.fromhex('0805881a'), bytes.fromhex('1f2003d5')),
    # fuxi 2026-11-21: the virtual machine check (21H16 0x1003abcc4, cached kern.hv_vmm_present, read at the
    # "skipping chip initialization / chipset power cycle / using default chip initialization" sites). The board's
    # ADT carries vmm-present, so bluetoothd never booted the controller. Return 0 before the dispatch_once:
    # mov w0, #0; b 0x1003abce8 (the function's own ldp/retab epilogue).
    (0x3abcd0, bytes.fromhex('683000f008c945f9'), bytes.fromhex('0000805205000014')),
    (0x477b80, bytes.fromhex('45008052'), bytes.fromhex('e00303aa')),
    (0x477b84, bytes.fromhex('df3a0c94'), bytes.fromhex('b3400c94')),
    (0x477d14, bytes.fromhex('45008052'), bytes.fromhex('e00303aa')),
    (0x477d18, bytes.fromhex('7a3a0c94'), bytes.fromhex('4e400c94')),
    (0x477e1c, bytes.fromhex('45008052'), bytes.fromhex('e00303aa')),
    (0x477e20, bytes.fromhex('383a0c94'), bytes.fromhex('0c400c94')),
    (0x477e88, bytes.fromhex('45008052'), bytes.fromhex('e00303aa')),
    (0x477e8c, bytes.fromhex('1d3a0c94'), bytes.fromhex('f13f0c94')),
    (0x477f10, bytes.fromhex('45008052'), bytes.fromhex('e00303aa')),
    (0x477f14, bytes.fromhex('fb390c94'), bytes.fromhex('cf3f0c94')),
    (0x47826c, bytes.fromhex('45008052'), bytes.fromhex('e00303aa')),
    (0x478270, bytes.fromhex('24390c94'), bytes.fromhex('f83e0c94')),
    (0x49e008, bytes.fromhex('45008052'), bytes.fromhex('e00303aa')),
    (0x49e00c, bytes.fromhex('bda10b94'), bytes.fromhex('91a70b94')),
    (0x49e0e8, bytes.fromhex('45008052'), bytes.fromhex('e00303aa')),
    (0x49e0ec, bytes.fromhex('85a10b94'), bytes.fromhex('59a70b94')),
]


def run(*cmd):
    r = subprocess.run([str(c) for c in cmd], env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'{cmd[0]} failed ({r.returncode}):\n{r.stdout}\n{r.stderr}')
    return r.stdout + r.stderr


def entitlements_of(path):
    xml = run('codesign', '-d', '--entitlements', ':-', path)
    return xml[xml.index('<?xml'):xml.rindex('</plist>') + len('</plist>')] + '\n'


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    src, plist, addon_in, addon_out = map(Path, sys.argv[1:])
    OUT.mkdir(parents=True, exist_ok=True)
    ents = OUT / 'entitlements.plist'
    ents.write_text(entitlements_of(src))
    want = plistlib.loads(ents.read_bytes())
    data = bytearray(src.read_bytes())
    for off, old, new in PATCHES:
        assert len(old) == len(new) and data[off:off + len(old)] == old, hex(off)
        data[off:off + len(new)] = new
    exe = OUT / 'bluetoothd'
    exe.write_bytes(bytes(data))
    run('codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'com.apple.bluetoothd', '--entitlements', ents, exe)
    detail = run('codesign', '--display', '--verbose=4', exe)
    assert re.search(r'^Page size=4096$', detail, re.M)
    h = re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1]
    assert plistlib.loads(entitlements_of(exe).encode()) == want, 'entitlements lost'
    (OUT / 'trust.json').write_text(json.dumps({'cdhash': h, 'patches': len(PATCHES)}, indent=2) + '\n')
    job = plistlib.loads(plist.read_bytes())
    assert job['Label'] == 'com.apple.bluetoothd'
    job['ProgramArguments'] = [TARGET] + job['ProgramArguments'][1:]
    spec = importlib.util.spec_from_file_location('addfx', BASE / 'device-ios17/scripts/add-fuxi-wallpaper-to-addon.py')
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    mine = {'btd/bluetoothd': (0o755, exe.read_bytes()),
            'daemons/com.apple.bluetoothd.plist': (0o644, plistlib.dumps(job, fmt=plistlib.FMT_XML))}
    out = [(n, mode, d) for n, mode, d in m.read_addon(addon_in.read_bytes()) if n not in mine]
    out += [(n, mode, d) for n, (mode, d) in mine.items()]
    blob = m.write_addon(out)
    assert m.read_addon(blob) == out
    addon_out.write_bytes(blob)
    print(f'bluetoothd cdhash {h}, {len(PATCHES)} patches; {addon_out}: {len(out)} files, {len(blob)} bytes')


if __name__ == '__main__':
    main()
