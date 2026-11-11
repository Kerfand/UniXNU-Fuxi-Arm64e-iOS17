#!/usr/bin/env bash
# Put the chained-fixup Turnip into the existing NativeAdrenoMetal.bundle and re-sign, exactly as build-adreno-metal-plugin.py does
# (strip -x, 4 KiB ad-hoc signature with identifier org.blu-sharky.turnip, then the bundle as org.blu-sharky.NativeAdrenoMetal);
# the plugin executable and the probe daemon are unchanged. trust.json gets the new plugin and Turnip CDHashes.
set -e
T=/home/xiaomi/sadnut-fuxi-local-20261103/UniXNU/out-ios17/fuxi/tree
export PATH=/home/xiaomi/.local/bin:$T/.cache/mesa-tools/bin:/home/xiaomi/sadnut-fuxi-local-20261103/bin:/home/xiaomi/unixnu-linux/CommandLineTools/usr/bin:$PATH
B=$T/build
OUT=$B/adreno-metal-plugin17e
BAK=$B/adreno-metal-plugin17e.before-chained-20261111
L=$B/mesa-tu-ios17e/src/freedreno/vulkan
echo "== plugin dir vs backup"; diff -rq "$OUT" "$BAK" && echo SAME || true
rm -rf "$OUT/NativeAdrenoMetal.bundle"
cp -a "$BAK/NativeAdrenoMetal.bundle" "$OUT/NativeAdrenoMetal.bundle"
cp "$BAK/trust.json" "$OUT/trust.json"
bundle=$OUT/NativeAdrenoMetal.bundle
cp "$L/libvulkan_freedreno.chained.dylib" "$bundle/libvulkan_freedreno.dylib"
strip -x "$bundle/libvulkan_freedreno.dylib"
codesign --force --sign - --pagesize 4096 --identifier org.blu-sharky.turnip "$bundle/libvulkan_freedreno.dylib"
turnip=$(codesign --display --verbose=4 "$bundle/libvulkan_freedreno.dylib" 2>&1 | sed -n 's/^CDHash=\([0-9a-f]\{40\}\)$/\1/p')
codesign --force --sign - --pagesize 4096 --identifier org.blu-sharky.NativeAdrenoMetal "$bundle"
codesign --verify --strict "$bundle"
plugin=$(codesign --display --verbose=4 "$bundle" 2>&1 | sed -n 's/^CDHash=\([0-9a-f]\{40\}\)$/\1/p')
echo "turnip $turnip plugin $plugin"
python3 - "$OUT/trust.json" "$plugin" "$turnip" <<'EOF'
import json, sys
p, plugin, turnip = sys.argv[1:]
assert len(plugin) == 40 and len(turnip) == 40
d = json.load(open(p)); old = dict(d)
d['cdhash'] = plugin; d['turnip_cdhash'] = turnip
open(p, 'w').write(json.dumps(d, indent=2) + '\n')
print('trust old', old['cdhash'], old['turnip_cdhash'], '-> new', plugin, turnip, 'daemon', d['daemon_cdhash'])
EOF
python3 - "$bundle/libvulkan_freedreno.dylib" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
n = struct.unpack_from('<I', d, 16)[0]; o = 32; cmds = []
for _ in range(n):
    c, s = struct.unpack_from('<II', d, o); cmds.append(c); o += s
print('bundle turnip:', 'chained' if 0x80000034 in cmds else 'NOT chained')
EOF
ls -l "$bundle" "$bundle/_CodeSignature"
