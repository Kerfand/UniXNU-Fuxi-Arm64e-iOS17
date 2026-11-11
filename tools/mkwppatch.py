"""mkwppatch.py: patches/xnu-10063-arm64e/ios17-wallpaper-iphone.patch - the GPU addon wallpaper mount for the fuxi iOS 17 identity
(iPhone15,2, 393w-852h@3x): iOS 17's default wallpaper 7565 (Collections~iphone.plist lists the iOS 17 collection first), background
layer over its sealed animation and the floating layer over its own, both from PNG copies of the HEIC images (no hardware HEVC
decoder; Windows WIC conversion, alpha kept). Also wires the patch into fuxi-kernel-baseline.sh's first unwind loop (it edits inside
zzy-native-gpu-addon-mount.patch's hunk), in the overlay and in the tree."""
import subprocess, tempfile
from pathlib import Path
R = Path('/home/xiaomi/sadnut-fuxi-local-20261103')
work = R / 'UniXNU/out-ios17/fuxi/tree/.cache/xnu-work'
rel = 'bsd/kern/kern_exec.c'
patch = R / 'UniXNU/patches/xnu-10063-arm64e/ios17-wallpaper-iphone.patch'
src = (work / rel).read_text()
start = src.index('\t/* The default iPadOS 17 wallpaper\'s layered animation with PNG images in place of its HEIC ones')
end = src.index('&native_gpu_addon3_stage, &native_gpu_addon3_status);\n', start) + len('&native_gpu_addon3_stage, &native_gpu_addon3_status);\n')
D = '"/Library/Wallpaper/Collections/iOS_17~iphone.wallpaperCollection/Wallpapers/7565.iOS_17-393w-852h@3x~iphone.wallpaper/"'
new_block = (
"\t/* fuxi iOS 17 (ios17-wallpaper-iphone.patch): the identity is iPhone15,2 (393w-852h@3x), whose default wallpaper is iOS 17's 7565\n"
"\t * (Collections~iphone.plist lists the iOS 17 collection first). Its layered animation has a background and a floating layer, each\n"
"\t * with HEIC images that ImageIO cannot decode without a hardware HEVC decoder; each overlay carries the animation's documents with\n"
"\t * PNG copies of those images (alpha kept) and mounts over its own sealed animation. The sealed assets stay visible below. */\n"
"\tstatic const char wallpaper_source[] = \"/private/var/ngpu/wallpaper\",\n"
"\t    wallpaper_point[] = " + D + "\n"
"\t    \"7565.iOS_17_Background-393w-852h@3x~iphone.ca\";\n"
"\tnative_gpu_addon_mount(p, scratch, 0x800, wallpaper_source, sizeof(wallpaper_source), wallpaper_point, sizeof(wallpaper_point),\n"
"\t    " + D + " \"7565.iOS_17_Background-393w-852h@3x~iphone.ca/assets/Background Light.HEIC\",\n"
"\t    " + D + " \"7565.iOS_17_Background-393w-852h@3x~iphone.ca/Background-Light.png\",\n"
"\t    &native_gpu_addon3_stage, &native_gpu_addon3_status);\n"
"\tstatic volatile int floating_stage = 0, floating_status = -1;\n"
"\tstatic const char floating_source[] = \"/private/var/ngpu/wallpaper-floating\",\n"
"\t    floating_point[] = " + D + "\n"
"\t    \"7565.iOS_17_Floating-393w-852h@3x~iphone.ca\";\n"
"\tnative_gpu_addon_mount(p, scratch, 0xa00, floating_source, sizeof(floating_source), floating_point, sizeof(floating_point),\n"
"\t    " + D + " \"7565.iOS_17_Floating-393w-852h@3x~iphone.ca/assets/Blue Light.HEIC\",\n"
"\t    " + D + " \"7565.iOS_17_Floating-393w-852h@3x~iphone.ca/Blue-Light.png\",\n"
"\t    &floating_stage, &floating_status);\n"
"\tprintf(\"ios17 wallpaper 7565: background stage %d status %d, floating stage %d status %d\\n\", native_gpu_addon3_stage,\n"
"\t    native_gpu_addon3_status, floating_stage, floating_status);\n")
new = src[:start] + new_block + src[end:]
with tempfile.TemporaryDirectory() as d:
    a, b = Path(d) / 'a' / rel, Path(d) / 'b' / rel
    a.parent.mkdir(parents=True); b.parent.mkdir(parents=True)
    a.write_text(src); b.write_text(new)
    out = subprocess.run(['diff', '-u', 'a/' + rel, 'b/' + rel], cwd=d, capture_output=True, text=True).stdout
lines = out.splitlines(True); lines[0] = f'--- a/{rel}\n'; lines[1] = f'+++ b/{rel}\n'
patch.write_text(f'diff --git a/{rel} b/{rel}\n' + ''.join(lines))
r = subprocess.run(['git', '-C', str(work), 'apply', '--check', str(patch)], capture_output=True, text=True)
print('check', r.returncode, r.stderr[:300])
if r.returncode == 0:
    subprocess.run(['git', '-C', str(work), 'apply', str(patch)], check=True)
    print('applied to the work tree')
for script in (R / 'device-ios17/scripts/fuxi-kernel-baseline.sh', R / 'UniXNU/out-ios17/fuxi/tree/scripts/fuxi-kernel-baseline.sh'):
    if not script.exists():
        print('no', script); continue
    t = script.read_text()
    old = 'for LATE_PATCH in "$ROOT/patches/xnu-10063-arm64e/ios17-job-policy.patch" '
    if 'ios17-wallpaper-iphone.patch' in t:
        print('already wired', script); continue
    assert t.count(old) == 1, script
    t = t.replace('# ios17-job-policy.patch edits inside the zzz-boot-progress hunk of kern_exec.c: unwound before it.\n',
                  '# ios17-job-policy.patch edits inside the zzz-boot-progress hunk of kern_exec.c: unwound before it.\n'
                  '# ios17-wallpaper-iphone.patch edits inside the zzy-native-gpu-addon-mount hunk: unwound first as well.\n')
    t = t.replace(old, 'for LATE_PATCH in "$ROOT/patches/xnu-10063-arm64e/ios17-wallpaper-iphone.patch" '
                       '"$ROOT/patches/xnu-10063-arm64e/ios17-job-policy.patch" ')
    script.write_text(t)
    print('wired', script)
