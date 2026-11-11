#!/usr/bin/env bash
# 1) restore the plugin output directory's other files from the backup (the failed rebuild emptied it);
# 2) replace the NativeAdrenoMetal.bundle files inside the wallpaper addon with the re-signed bundle (everything else kept).
set -e
B=/home/xiaomi/sadnut-fuxi-local-20261103/UniXNU/out-ios17/fuxi/tree/build
OUT=$B/adreno-metal-plugin17e
BAK=$B/adreno-metal-plugin17e.before-chained-20261111
for f in "$BAK"/*; do
  n=$(basename "$f")
  [ "$n" = NativeAdrenoMetal.bundle ] && continue
  [ "$n" = trust.json ] && continue
  [ -e "$OUT/$n" ] || cp -a "$f" "$OUT/$n"
done
echo "== restored"; diff -rq "$OUT" "$BAK" || true
python3 - <<'EOF'
import sys, importlib.util
from pathlib import Path
spec = importlib.util.spec_from_file_location('addfx', '/home/xiaomi/sadnut-fuxi-local-20261103/device-ios17/scripts/add-fuxi-wallpaper-to-addon.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
src = Path('/mnt/f/UniXNU-FUXI-work/ios17-cache/gpuaddon-ios17e-wallpaper7565.bin')
dst = Path('/mnt/f/UniXNU-FUXI-work/ios17-cache/gpuaddon-ios17e-wallpaper7565-chained.bin')
bundle = Path('/home/xiaomi/sadnut-fuxi-local-20261103/UniXNU/out-ios17/fuxi/tree/build/adreno-metal-plugin17e/NativeAdrenoMetal.bundle')
files = m.read_addon(src.read_bytes())
new = []; swapped = 0
for name, mode, data in files:
    prefix = 'addons/NativeAdrenoMetal.bundle/'
    if name.startswith(prefix):
        f = bundle / name[len(prefix):]
        assert f.is_file(), name
        if f.read_bytes() != data:
            swapped += 1
        data = f.read_bytes()
    new.append((name, mode, data))
names_bundle = sorted(p.relative_to(bundle).as_posix() for p in bundle.rglob('*') if p.is_file())
names_addon = sorted(n[len('addons/NativeAdrenoMetal.bundle/'):] for n, _, _ in files if n.startswith('addons/NativeAdrenoMetal.bundle/'))
assert names_bundle == names_addon, (names_bundle, names_addon)
blob = m.write_addon(new)
assert m.read_addon(blob) == new
dst.write_bytes(blob)
print('swapped', swapped, 'files;', dst, len(blob))
EOF
