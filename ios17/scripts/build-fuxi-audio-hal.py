#!/usr/bin/env python3
"""fuxi (iOS 17.7 21H16, arm64e audio server): build the audio server plug-in hal/ShengAudioPlugIn.c as ShengAudio.driver, signed ad hoc
(UniXNU-sheng scripts/build-sheng-audio-hal.py, https://unixnu.com/zh/porting/power section 3, for this tree's layout and target).

  build-fuxi-audio-hal.py [OUTDIR]

Sources: device/hal (ShengAudioPlugIn.c, Info.plist) and device/package (ShengAudioAbi.h). Writes OUTDIR/ShengAudio.driver and
OUTDIR/trust.json (the executable's CDHash for the owner trust cache). The iOS SDK has no audio server plug-in headers; the macOS SDK's
CoreAudio headers declare the same interface and are used for the build only."""
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DEVICE = Path('/home/xiaomi/sadnut-fuxi-local-20261103/device')
HAL, PACKAGE = DEVICE / 'hal', DEVICE / 'package'
out = Path(sys.argv[1] if len(sys.argv) > 1 else '/home/xiaomi/sadnut-fuxi-local-20261103/UniXNU/out-ios17/fuxi/tree/build/sheng-audio-hal').resolve()
target = 'arm64e-apple-ios17.0'
# This Linux host's xcrun shim has no SDK paths (it echoes the SDK name): the iOS 17 SDK of this tree (build-fuxi-statusbar.py) and the
# three CoreAudio headers of the macOS 11.3 SDK (phracker/MacOSX-SDKs, downloaded 2026-11-13 with the owner's approval), build-only.
ios = '/home/xiaomi/sdk/iPhoneOS17-21H16.sdk'
CA_HEADERS = Path('/home/xiaomi/coreaudio-headers/CoreAudio')
bundle = out / 'ShengAudio.driver'
shutil.rmtree(bundle, ignore_errors=True)
bundle.mkdir(parents=True)
with tempfile.TemporaryDirectory() as tmp:
    headers = Path(tmp) / 'CoreAudio'
    headers.mkdir()
    for name in ('AudioServerPlugIn.h', 'AudioHardwareBase.h', 'AudioHardware.h'):
        src = CA_HEADERS / name
        if not src.is_file():
            raise SystemExit(f'{src} missing')
        (headers / name).symlink_to(src)
    # The 21H16 SDK of this tree lacks CoreAudioTypes.framework, which AudioServerPlugIn.h includes; Xcode 15.4's iOS 17.5 SDK has it.
    (Path(tmp) / 'CoreAudioTypes').symlink_to('/home/xiaomi/xcode-15.4-sdk/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/'
                                             'Developer/SDKs/iPhoneOS17.5.sdk/System/Library/Frameworks/CoreAudioTypes.framework/Headers')
    executable = bundle / 'ShengAudio'
    subprocess.run(['xcrun', 'clang', '-target', target, '-isysroot', ios, '-I', tmp, '-I', str(PACKAGE), '-O2', '-g0',
                    # the Linux clang is newer than Xcode's (as build-adreno-metal-plugin.py notes)
                    '-Wall', '-Wextra', '-Werror', '-Wno-unknown-warning-option', '-Wno-unused-but-set-global',
                    '-DkAudioObjectPropertyElementMain=kAudioObjectPropertyElementMaster',   # 11.3 headers predate the rename (value 0)
                    '-bundle', '-fvisibility=hidden', '-framework', 'CoreFoundation', '-framework', 'IOKit',
                    str(HAL / 'ShengAudioPlugIn.c'), '-o', str(executable)], check=True)
shutil.copy(HAL / 'Info.plist', bundle / 'Info.plist')
# fuxi: AID8006's VAD graph_configurations.plist without its speaker_* entries; the plug-in's fopen hook hands it to VirtualAudio in place
# of the OS volume's copy. Apple's file is not kept in the repository: it is read from the owner's own 21H16 OS volume
# (System/Library/Audio/Tunings/AID8006/VAD/graph_configurations.plist, e.g. extracted with 7z from the IPSW's OS dmg) and edited here.
import os, plistlib
AID_GRAPH = Path(os.environ.get('FUXI_AID_GRAPH_CONFIG',
                                '/home/xiaomi/ios17-bins-21H16/aid/Library/Audio/Tunings/AID8006/VAD/graph_configurations.plist'))
if not AID_GRAPH.is_file():
    raise SystemExit(f'{AID_GRAPH} missing: extract AID8006/VAD/graph_configurations.plist from the 21H16 OS volume '
                     '(or set FUXI_AID_GRAPH_CONFIG)')
graph = plistlib.loads(AID_GRAPH.read_bytes())
for name in [k for k in graph['Configurations'] if k.startswith('speaker_')]:
    del graph['Configurations'][name]
(bundle / 'graph_configurations.plist').write_bytes(plistlib.dumps(graph, fmt=plistlib.FMT_XML))
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.unixnu.sheng.audio', str(bundle)], check=True)
detail = subprocess.run(['codesign', '--display', '--verbose=4', str(bundle)], capture_output=True, text=True)
text = detail.stderr + detail.stdout
m = re.search(r'^CDHash=([0-9a-f]{40})$', text, re.M)
if not m:
    raise SystemExit('no CDHash in codesign output:\n' + text)
(out / 'trust.json').write_text(json.dumps({'cdhash': m.group(1)}, indent=2) + '\n')
print(f'{bundle} CDHash {m.group(1)}')
