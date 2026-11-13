"""iOS 17.7 21H16 checkpoint 2a: boot.img (arm64e xnu-10063 + original fileset kexts + read-only NativeUfs).

For `fastboot boot` only: nothing is flashed and userdata is not written. NativeUfs runs one read-only
probe chosen by UFS_PROBE (default 10) and ends in its report panic:
  10  UFS bring-up, LUN 0 GPT -> userdata -> first block and root manifest
  12  publish the SHNGROOT windows (write-protected) through the 21H16 IOStorageFamily, list IOMedia
   0  root mount: native-fuxi-ufs-root=1 publishes the windows, /chosen/root-matching selects the 21H16 OS
      system volume (disk3s1 in the 2026-11-06 17:33 media report), /chosen gets the 21H16 SystemVolume root
      hash as system-volume-auth-blob, and the payload trust cache holds the 21H16 StaticTrustCache and
      Cryptex1,SystemTrustCache so AMFI can admit /sbin/launchd. The windows stay write-protected, so the root
      is mounted read-only.
The UFS log window (sheng-log) and Data persistence (sheng-data-persist) stay off, so no write path is
armed. In the probe modes the ADT asks for a root volume UUID that does not exist, so nothing is mounted.
"""
from pathlib import Path
import hashlib, json, re, shutil, struct, subprocess, time, tomllib, os

base = Path('/home/xiaomi/sadnut-fuxi-local-20261103')
kernel_dir = base / 'UniXNU/out-ios17/fuxi/kernel'
UFS_PROBE = int(os.environ.get('UFS_PROBE', '10'))
assert UFS_PROBE in (0, 10, 12), 'root mount (0), read-only root check (10) or media report (12)'
ROOT_MODE = UFS_PROBE == 0
out = Path('/mnt/f/UniXNU-FUXI-work/outputs/' + ('sadnut-fuxi-ios17-hidpath-20261109' if ROOT_MODE else
                                                 'sadnut-fuxi-ios17-ufs-probe%d-20261106' % UFS_PROBE))
# 21H16 IPSW members extracted from their IM4P containers (type trst, trcs, isys) next to the dmg cache.
IPSW_CACHE = Path('/mnt/f/UniXNU-FUXI-work/ios17-cache/ipsw-21H16')
OS_VOLUME = '4A4918F2-B5D8-45EF-B9A7-D3639D762155'   # disk3s1, the OS system volume of the flashed 21H16 image
previous = Path('/mnt/f/UniXNU-FUXI-work/outputs/fuxi17-usb-local-20261102')
# Scratch Data seed (root mode): a packed DBRDISK1 APFS container of 1 GiB with one Data volume FEE1FE9D-... (the
# defconfig's CONFIG_XNU_OWNER_DATA_VOLUME_UUID). At most 2 GiB, so NativeRamBlock keeps it in memory only: nothing is
# persisted and no UFS write path is armed. DATA_SEED=none packs the image without it.
# The iPadOS seed with its migration build string set to 21H16 (scratchpad seed21h16.py): otherwise data migration runs on every
# boot, fails, and SpringBoard never leaves the boot logo.
DATA_SEED = os.environ.get('DATA_SEED', '/mnt/f/UniXNU-FUXI-work/ios17-cache/scratch-data-21H16.img')
DATA_SEED = Path(DATA_SEED) if ROOT_MODE and DATA_SEED != 'none' else None
# GPU addon (make-gpu-addon.py --publish of the ios17e Metal plugin): the loader hands it to the kernel as
# /chosen/memory-map/GpuAddon, the kernel writes it below /private/var/ngpu on the Data volume and mounts it over
# /System/Library/Extensions (sheng-gpu-addon=1, patches/xnu/zzy-native-gpu-addon-mount.patch). GPU_ADDON=none packs without.
_TREE = Path('/home/xiaomi/sadnut-fuxi-local-20261103/UniXNU/out-ios17/fuxi/tree')
# The ios17e addon plus the iPhone default wallpaper 7565 overlay (both layers, PNG from the HEIC images):
# device-ios17/scripts/add-fuxi-wallpaper-to-addon.py ... background floating; mounted by ios17-wallpaper-iphone.patch.
GPU_ADDON = os.environ.get('GPU_ADDON', '/mnt/f/UniXNU-FUXI-work/ios17-cache/gpuaddon-ios17e-wallpaper7565-chained-sb.bin')
GPU_ADDON = Path(GPU_ADDON) if ROOT_MODE and DATA_SEED and GPU_ADDON != 'none' and Path(GPU_ADDON).is_file() else None
GPU_TRUST = _TREE / 'build/adreno-metal-plugin17e/trust.json'
if DATA_SEED:
    _seed = DATA_SEED.read_bytes()
    assert _seed[:8] == b'DBRDISK1'
    import uuid as _uuid
    _apsb = _seed.find(b'APSB') - 32
    assert _apsb >= 0 and str(_uuid.UUID(bytes=_seed[_apsb + 0xF0:_apsb + 0x100])).upper() == 'FEE1FE9D-1C1D-42F9-9A12-0732FA326C30'
    _nxsb = _seed.find(b'NXSB') - 32
    assert _nxsb >= 0 and struct.unpack_from('<I', _seed, _nxsb + 36)[0] * struct.unpack_from('<Q', _seed, _nxsb + 40)[0] <= 0x80000000
work = base / 'pack-work-ios17-hid-digitizer'
out.mkdir(parents=True, exist_ok=True)
work.mkdir(exist_ok=True)
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
TITLE = 'FUXI IOS17 TOUCH DIGITIZER' if ROOT_MODE else 'FUXI IOS17 UFS PROBE %d 20261106' % UFS_PROBE
NO_ROOT = 'D73A0000-0000-4000-8000-000000000000'   # no such volume: stop at the root-device wait


def parse(data):
    nodes = {}
    def step(off, parent=None):
        np_, nc = struct.unpack_from('<II', data, off); off += 8; props = {}
        for _ in range(np_):
            key, n = struct.unpack_from('<32sI', data, off); off += 36
            props[key.split(b'\0')[0].decode()] = data[off:off + n]; off += (n + 3) & ~3
        path = '/' if parent is None else parent.rstrip('/') + '/' + props['name'].rstrip(b'\0').decode()
        nodes[path] = props
        for _ in range(nc):
            off = step(off, path)
        return off
    assert step(0) == len(data)
    return nodes


kernel = kernel_dir / 'kernel.macho'
report = json.loads((kernel_dir / 'kernel.json').read_text())
assert sha(kernel) == report['candidate_sha256']
k = kernel.read_bytes()
assert struct.unpack_from('<4I', k) [:4] == (0xfeedfacf, 0x100000c, struct.unpack_from('<I', k, 8)[0], 12), 'not a fileset'
assert struct.unpack_from('<I', k, 8)[0] & 0xff == 2, 'not arm64e'
assert len(k) <= 128 * 1024 * 1024, 'DarwinBoot rejects a kernel over 128 MiB'

adt = work / 'kernel.macho.adt'
auth_blob = IPSW_CACHE / 'SystemVolume.payload'
if ROOT_MODE:
    assert len(auth_blob.read_bytes()) == 208 and struct.unpack_from('<4I', auth_blob.read_bytes()) == (2, 0, 1, 32)
# The red "rdar" status bar (2026-11-12): +[_UIStatusBarVisualProvider_Phone visualProviderSubclassForScreen:visualProviderInfo:]
# (UIKitCore) and its SystemStatusUI twin pick DynamicSplit only when the screen has an exclusion area (the island) below the top
# edge, else Split (notch) or LegacyPhone, whose size tables have no 360x800 pt screen; nothing matches and
# _UIStatusBarGetVisualProviderClassForScreen falls back to the Fallback provider ("No status bar visual provider for screen ...
# rdar://45025538"), a red strip. Each fixup turns the first test (tbz w0, #0: no exclusion area) into b +0x14, the DynamicSplit
# branch; DynamicSplit picks DynamicSplitMedium for a screen up to 393 pt wide (the iPhone 14 Pro's own provider). Applied through
# the kernel's signed-text fixups (bsd/kern/ubc_subr.c, /chosen/text-fixups) to the 21H16 dyld cache. STATUSBAR_FIXUPS=0 packs
# without them.
# Tried first and dropped: returning at once from the Fallback providers' -setupInContainerView: (no bar at all) sent SpringBoard
# into a stack overflow at every launch, and the phone stayed at the Apple logo.
STATUSBAR_FIXUPS = [
    # 2026-11-12, second form. DynamicSplitMedium is the 393 pt island layout: on this 360 pt screen without an exclusion area its
    # time and battery sat off the island and Control Center's grid did not fill the width. Split1080 is Apple's layout for the
    # 1080-pixel phones (360 pt wide), which pong/mondrian select (userland/ui_compat.c PongStatusBarVisualProvider1080, a Split1080
    # subclass whose +referenceScreenScale answers the screen's nativeScale). Here, for UIKit (03) and SystemStatusUI (25):
    # Phone takes the Split branch, Split answers Split1080 (its size table has no 360x800 pt entry), Split1080's
    # +referenceScreenScale answers 3.0 (its own 2.88 is the 375 pt mini scaled to 1080 pixels).
    'dyld_shared_cache_arm64e.03,498de58,00010036e00314aa,13000014e00314aa',   # _UIStatusBarVisualProvider_Phone +0x38: b Split branch
    'dyld_shared_cache_arm64e.25,5e7fc74,00010036e00314aa,1a000014e00314aa',   # STUIStatusBarVisualProvider_Phone +0x38: b Split branch
    'dyld_shared_cache_arm64e.03,499247c,7f2303d5ff8304d1eb2b0c6d,a0432fd000003c91c0035fd6',   # _UIStatusBarVisualProvider_Split: return Split1080
    'dyld_shared_cache_arm64e.25,5e85b50,7f2303d5ff8304d1eb2b0c6d,c0520ad000e01191c0035fd6',   # STUIStatusBarVisualProvider_Split: return Split1080
    'dyld_shared_cache_arm64e.03,499d7f0,882600d0009942fd,0010611ec0035fd6',   # _UIStatusBarVisualProvider_Split1080 +referenceScreenScale: 3.0
    'dyld_shared_cache_arm64e.25,5e8e324,a80000f000dd45fd,0010611ec0035fd6',   # STUIStatusBarVisualProvider_Split1080 +referenceScreenScale: 3.0
] if ROOT_MODE and os.environ.get('STATUSBAR_FIXUPS', '1') != '0' else []
# The ios17 overlay's copy of make-fuxi-adt.py (adds --vmm-present; device/ stays the iPadOS one).
subprocess.run(['python3', str(base / 'device-ios17/scripts/make-fuxi-adt.py'), '--output', str(adt),
                '--dram-size', '0x80000000', '--owned-memory-size', '0x130000000',
                '--root-match-volume-uuid', OS_VOLUME if ROOT_MODE else NO_ROOT,
                '--unique-chip-id', 'ce854c9ae043320b', '--chip-id', '207',
                '--cpu-regs', '0,0x100,0x200,0x300,0x400,0x500,0x600,0x700',
                '--phone-identity', '--phone-model', 'iPhone15,2', '--trust-cache',
                *(['--packed-data-ramdisk'] if DATA_SEED else []),
                *(['--gpu-addon'] if GPU_ADDON else []),
                *[a for f in STATUSBAR_FIXUPS for a in ('--text-fixup', f)],
                # vmm-present: the original AMFI developer-mode init waits 30 s for ApplePMGRNub and panics
                # "AMFI: No PMGR?" unless kern.hv_vmm_present is set (fuxi has no original PMGR).
                *(['--system-volume-auth-blob', str(auth_blob), '--vmm-present'] if ROOT_MODE else [])], check=True)
nodes = parse(adt.read_bytes())
if ROOT_MODE:
    assert OS_VOLUME.encode() in nodes['/chosen']['root-matching'], nodes['/chosen'].get('root-matching')
    assert nodes['/chosen']['system-volume-auth-blob'] == auth_blob.read_bytes()
    assert nodes['/defaults']['vmm-present'] == struct.pack('<I', 1)
assert nodes['/']['model'] == b'iPhone15,2\0', nodes['/']['model']
assert nodes['/chosen'].get('text-fixups', b'') == b''.join(f.encode() + b'\0' for f in STATUSBAR_FIXUPS)
assert len([n for n in nodes if n.startswith('/cpus/')]) == 8

args = ('serial=3 debug=0x14c kt_fixed_zones=120 qcom-usb-debug=1 armcpu-spi-ipi=1 sheng-smp=1 '
        '-no_slto_panic ml-timeout-lock-panic-scale=40 unixnu-unmapped=1 use_contiguous_hint=0 fuxi-panic-fastboot=30 fuxi-wifi=5 sheng-adsp=2 sheng-audio=1 offline-legacy-logical-width=1125 offline-legacy-logical-height=2436 log-gpu=1 sheng-cpufreq=3 cpumask=0xf9 sheng-fb-gpu=1 '
        # unixnu-hang-probe (log only, xnu-10063-arm64e/init-hang-probe.patch): report the init task's threads every
        # 10 s while the exec of /sbin/launchd does not return.
        # qcom-usb-diag=1: the QUD driver's diagnostic mode gives every console write a 1 ms flush window, so the
        # last line reaching the host is the last one the kernel printed before a hard hang.
        # unixnu-hang-panic=1: a deliberate diagnostic panic after the second hang round, for every core's PC/LR/FP.
        # qcom-usb-ctl=1: the QUD worker accepts SXNU1 control frames (HELLO/STATUS/REBOOT/REBOOT-FASTBOOT) from the
        # host, e.g. Capture-FuxiUsbControl.ps1 key F; REBOOT-FASTBOOT goes through fuxi's qcom_reset_request(1).
        + ('-apfs_shared_datavolume ' if DATA_SEED else '')
        # Display (build-ios17-legacy-display.py): load IOMobileGraphicsFamily, register NativeLegacyDisplay, attach it
        # as the /arm-io display-subsystem on the GOP framebuffer (phase 4), CPU-copy swaps, UI scale 3, 60 Hz vsync tick.
        + ('offline-mobile-graphics-load=1 offline-legacy-register=1 offline-legacy-phase=4 offline-legacy-gop=1 '
           'offline-legacy-pm=1 offline-legacy-validate=1 offline-legacy-transfer=1 offline-legacy-cpu-present=1 '
           'offline-legacy-unit-scale=3 sheng-vsync=120 '
           # offline-legacy-logical-width/height=1125x2436 (set with the fuxi args above): 375x812 pt at 3x is a screen type UIKit knows
           # (iPhone X class); 360x800 and 375x833.3 match none, so the keyboard and Control Center fell back to generic layouts.
           '' if ROOT_MODE else '')
        # sheng-diag=1: the plugin keeps its command-buffer stage ring and reports it through its telemetry rows (GPU hang diagnosis).
        # AppleKeyStore stand-in (no SEP): SpringBoard leaves the boot logo only once the class answers its state queries.
        # sheng-backlight=1 is what lets BacklightProbe run at all; without it the driver returns before
        # publishing backlight-control, CoreBrightness finds no backlight, and the panel stays dark.
        # fuxi-backlight=dsi selects DSI brightness path (not I2C hardware); sheng-backlight-control=1 publishes backlight-control
        # with the AppleARMBacklight impersonation (README-亮度修复: the DSI 0x51 path verified on t32/t36), so CoreBrightness drives it.
        + 'offline-key-events=1 sheng-aks=1 sheng-backlight=1 fuxi-backlight=dsi sheng-backlight-control=1 '
        # Thread report of SpringBoard every 40 s (init-hang-probe.patch unixnu-pp): where it waits after the boot logo.
        # Thread probe for the user-driven hangs (Settings, sleep/wake): three processes every 20 s for 40 rounds.
        + ''
        # fuxi FTM5 touch (FuxiTouchProbe stage 4, polled) feeding the HID digitizer (NativeHidInput on the 21H16 IOHIDDevice).
        + 'fuxi-touch=4 unixnu-iokit-probe=1 fuxi-touch-poll=1 fuxi-touch-hid=1 fuxi-touch-digitizer-only=1 '
        # dk=0: skip DriverKit personalities (the AppleUserHIDDrivers user server never launches); the kernel
        # IOHIDEventDriver (IOHIDEventDriverSafeBoot personality) claims the digitizer instead.
        + 'dk=0 '
        # Unattended synthetic taps (FuxiTouchProbe fuxi-touch-autotap): 60 s after the HID start settled, 3 taps 15 s apart.
        # fuxi-touch-autotap=60 off: the user tests by hand.
        # Buttons (kits/sheng/FuxiButtons.cpp): power / volume through NativeHidInput report 2.
        + 'fuxi-buttons=1 '
        # /private/var/Keychains for securityd (BootWatchdogPolicy.c): the Data seed lacks it, so no keychain query can succeed.
        # sheng-keychains=1 dropped: sheng-data-skel creates /private/var/Keychains (64:0 0755) as well.
        # The 21H16 /private/var template (DataSkel21H16.h): directories daemons expect, with their owners.
        + 'sheng-data-skel=1 '
        # launchd job policy (ios17-job-policy.patch): mobileactivationd dies in its keychain migration and SpringBoard waits
        # on it synchronously; disabled, the activation-state lookup fails at once.
        # mobileactivationd runs natively now (sheng-keychains=1 gives securityd its Keychains directory).
        + ('sheng-gpu-addon=1 ' if GPU_ADDON else '')
        + ('native-fuxi-ufs-root=1 sheng-gpu-power=9 sheng-gpu-metal=1 sheng-gpu-dcvs=1 sheng-gpu-pause-us=100 qcom-usb-ctl=1' if ROOT_MODE else 'native-ufs-probe=%d' % UFS_PROBE))   # unixnu-unmapped: log unmapped calls, return 0
if ROOT_MODE and os.environ.get('NO_QUD') == '1':
    # Control experiment: without the QUD USB console (qcom-usb-debug/qcom-usb-diag) and without the diagnostic
    # panic; only the on-screen marks report whether the exec of /sbin/launchd returns.
    args = ' '.join(a for a in args.split() if not a.startswith(('qcom-usb-debug=', 'qcom-usb-diag=', 'qcom-usb-ctl=',
                                                                  'unixnu-hang-panic=')))
    out = out.with_name(out.name.replace('ufs-root', 'ufs-root-noqud'))
    out.mkdir(parents=True, exist_ok=True)
    TITLE = TITLE.replace('ROOT', 'ROOT NOQUD')
print('boot-args bytes', len(args.encode()))
assert len(args.encode()) <= 1000
assert 'sheng-log' not in args and 'sheng-data-persist' not in args, 'checkpoint 2a must not arm a UFS write path'
template = (previous / 'pack-work/unixnu.toml').read_text()
first = template.index('[[entry]]')
reset = template.index('[[entry]]\nid = "reset"') if '[[entry]]\nid = "reset"' in template else None
head = template[:first]
head = re.sub(r'timeout = \d+', 'timeout = 8', head)
head = re.sub(r'default = "[^"]*"', 'default = "xnu"', head)
entry = ('[[entry]]\nid = "xnu"\ntitle = "%s"\ntype = "xnu"\npath = "Payloads/kernel.macho"\n' % TITLE
         # The Data seed goes to the ADT's PackedRAMDisk placeholder (DarwinBoot.c:662); without one, no initrd.
         + ('initrd = "Payloads/scratch-data.img"\n' if DATA_SEED else '')
         + 'args = "%s"\n' % args)
# The firmware's built-in DarwinBoot accepts only arm64 MH_EXECUTE kernels ("returned: Unsupported" on the
# fileset), and secure boot refuses a loose driver from the payload. The template's firmware volume carries
# DarwinBoot v2 instead (UniXNU drivers/DarwinBoot 3c75734: arm64e + MH_FILESET; build-darwinboot-fv.sh,
# make-t32-darwinboot-v2-template.py).
TEMPLATE = base / 'pack-work-ios17-kernel/t32-bl-daily-darwinboot-v2.img'
assert TEMPLATE.is_file()
drivers = ''
config = head + drivers + entry + (template[reset:] if reset is not None else '')
parsed = tomllib.loads(config)
assert parsed['boot']['default'] == 'xnu' and parsed['entry'][0]['args'] == args
assert 'driver' not in parsed and ('initrd' in parsed['entry'][0]) == bool(DATA_SEED)
(work / 'unixnu.toml').write_text(config)

payload = work / 'payload.img'
shutil.copy2(previous / 'pack-work/payload.img', payload)
env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
# Leftovers from the iPadOS payload: a GPU addon sidecar without a GpuAddon placeholder in this ADT is
# rejected (DarwinBoot.c:694), and the scratch ramdisk is no longer referenced.
for leftover in ['kernel.macho.gpuaddon', 'scratch-data.img']:
    subprocess.run(['mdel', '-i', str(payload), '::/EFI/UNIXNU/Payloads/' + leftover], env=env)
listing = subprocess.run(['mdir', '-b', '-i', str(payload), '::/EFI/UNIXNU/Payloads'], env=env,
                         capture_output=True, text=True, check=True).stdout
assert 'gpuaddon' not in listing and 'scratch-data' not in listing, listing
if DATA_SEED:
    copies_seed = [(DATA_SEED, '::/EFI/UNIXNU/Payloads/scratch-data.img')]
    if GPU_ADDON:
        copies_seed.append((GPU_ADDON, '::/EFI/UNIXNU/Payloads/kernel.macho.gpuaddon'))
else:
    copies_seed = []
copies = [(kernel, '::/EFI/UNIXNU/Payloads/kernel.macho'), (adt, '::/EFI/UNIXNU/Payloads/kernel.macho.adt'),
          (work / 'unixnu.toml', '::/EFI/UNIXNU/unixnu.toml')] + copies_seed
trust = None
if ROOT_MODE:
    # The inherited payload carries the iPadOS 21H461 trust caches (serialized_trust_caches_v1 with three v2
    # modules: a 3-entry owner cache, the 21H461 StaticTrustCache and its Cryptex1 cache). Replace them with
    # the 21H16 StaticTrustCache (OS volume, /sbin/launchd) and Cryptex1,SystemTrustCache in the same layout.
    modules = [(IPSW_CACHE / name).read_bytes() for name in ('StaticTrustCache.payload', 'Cryptex1_SystemTrustCache.payload')]
    if GPU_ADDON:
        # Owner trust module (v2, as the iPadOS payload's first module): the plugin, Turnip and probe daemon CDHashes,
        # hash type 2 (SHA-256 truncated), sorted. The original 21H16 modules follow unchanged.
        ids = json.loads(GPU_TRUST.read_text())
        # statusbar_cdhash: FuxiStatusBar.dylib, the plugin's weak dylib (device-ios17/scripts/build-fuxi-statusbar.py).
        hashes = sorted(bytes.fromhex(ids[k]) for k in ('cdhash', 'turnip_cdhash', 'daemon_cdhash', 'statusbar_cdhash') if k in ids)
        # The audio server plug-in (device-ios17/scripts/build-fuxi-audio-hal.py; mounted by ios17-zaudio-hal-mount.patch from the
        # addon's audio/ entries), https://unixnu.com/zh/porting/power section 3: its CDHash joins the owner module.
        audio_trust = _TREE / 'build/sheng-audio-hal/trust.json'
        if audio_trust.is_file():
            hashes = sorted(hashes + [bytes.fromhex(json.loads(audio_trust.read_text())['cdhash'])])
        owner = struct.pack('<I16sI', 2, hashlib.sha256(b'unixnu-fuxi-ios17-gpu-owner').digest()[:16], len(hashes))
        owner += b''.join(h + bytes([2, 0, 0, 0]) for h in hashes)
        modules.insert(0, owner)
    for module in modules:
        version, count = struct.unpack_from('<I', module)[0], struct.unpack_from('<I', module, 20)[0]
        assert version == 2 and len(module) == 24 + 24 * count, (version, count, len(module))
    offset, offsets = 4 + 4 * len(modules), []
    for module in modules:
        offsets.append(offset); offset += len(module)
    trust = work / 'kernel.macho.trustcache'
    trust.write_bytes(struct.pack('<%dI' % (1 + len(modules)), len(modules), *offsets) + b''.join(modules))
    copies.append((trust, '::/EFI/UNIXNU/Payloads/kernel.macho.trustcache'))
for source, dest in copies:
    subprocess.run(['mcopy', '-D', 'o', '-i', str(payload), str(source), dest], check=True, env=env)
readback = work / ('readback-' + str(time.time_ns())); readback.mkdir()
for name in ['kernel.macho', 'kernel.macho.adt']:
    subprocess.run(['mcopy', '-D', 'o', '-i', str(payload), '::/EFI/UNIXNU/Payloads/' + name, str(readback / name)], check=True, env=env)
subprocess.run(['mcopy', '-D', 'o', '-i', str(payload), '::/EFI/UNIXNU/unixnu.toml', str(readback / 'unixnu.toml')], check=True, env=env)
assert sha(readback / 'kernel.macho') == report['candidate_sha256']
assert sha(readback / 'kernel.macho.adt') == sha(adt)
assert (readback / 'unixnu.toml').read_text() == config
if trust:
    subprocess.run(['mcopy', '-D', 'o', '-i', str(payload), '::/EFI/UNIXNU/Payloads/kernel.macho.trustcache',
                    str(readback / 'kernel.macho.trustcache')], check=True, env=env)
    assert sha(readback / 'kernel.macho.trustcache') == sha(trust)

(out / 'payload').mkdir(exist_ok=True)
shutil.copy2(payload, out / 'payload/payload.img')
shutil.copy2(adt, out / 'payload/kernel.macho.adt')
shutil.copy2(kernel_dir / 'kernel.json', out / 'kernel.json')
packer = work / 'uefi-fuxi/scripts/bootimage.py'; packer.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(previous / 'pack-work/uefi-fuxi/scripts/bootimage.py', packer)
subprocess.run(['python3', str(packer), '--template', str(TEMPLATE),
                '--ramdisk', str(out / 'payload/payload.img'), '-o', str(out / 'boot.img')], check=True)
boot = json.loads((out / 'bootimage.json').read_text())
assert boot['header_version'] == 3 and boot['payload_sha256'] == sha(payload)
missing = report['missing']; stubbed = report['stubbed']
summary = dict(boot_img=str(out / 'boot.img'), boot_img_sha256=sha(out / 'boot.img'), kernel_sha256=report['candidate_sha256'],
               kernel_bytes=len(k), arch='arm64e', xnu='10063.141.1', kernelcache='iPhone15,2 21H16',
               adt_sha256=sha(adt), identity='iPhone15,2', template=str(TEMPLATE), template_sha256=sha(TEMPLATE), root_matching=OS_VOLUME if ROOT_MODE else NO_ROOT,
               trust_cache_sha256=sha(trust) if trust else None, kernel_args=args, menu_title=TITLE,
               closure=report['closure'], early_security_init=report['early_security_init'],
               stubbed_targets=sum(len(v) for v in stubbed.values()), stubbed_sites=sum(sum(v.values()) for v in stubbed.values()),
               missing_targets=sum(len(v) for v in missing.values()), missing_sites=sum(sum(v.values()) for v in missing.values()),
               ufs_probe=UFS_PROBE, flash='fastboot boot only; userdata read, never written', hardware_tested=False)
(out / 'pack.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps(summary, indent=2))
print('PACK_COMPLETE: ' + str(out / 'boot.img'), flush=True)
