#!/usr/bin/env bash
# Relink libvulkan_freedreno.dylib (iOS 17 arm64e) with chained fixups into a separate file; the original stays untouched.
set -e
T=/home/xiaomi/sadnut-fuxi-local-20261103/UniXNU/out-ios17/fuxi/tree
export PATH=$T/.cache/mesa-tools/bin:$T/.cache/mesa-venv/bin:/home/xiaomi/sadnut-fuxi-local-20261103/bin:/home/xiaomi/unixnu-linux/CommandLineTools/usr/bin:$PATH
cd $T/build/mesa-tu-ios17e
which ld xcrun 2>/dev/null || true
cmd=$(ninja -t commands src/freedreno/vulkan/libvulkan_freedreno.dylib | tail -1)
out=src/freedreno/vulkan/libvulkan_freedreno.chained.dylib
cmd=${cmd/-o src\/freedreno\/vulkan\/libvulkan_freedreno.dylib/-o $out}
eval "$cmd -Wl,-fixup_chains" 2>&1 | tail -5
echo "link rc=${PIPESTATUS[0]}"
ls -l src/freedreno/vulkan/libvulkan_freedreno.dylib $out
python3 - "$out" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
n = struct.unpack_from('<I', d, 16)[0]; o = 32; cmds = []
for _ in range(n):
    c, s = struct.unpack_from('<II', d, o); cmds.append(c); o += s
print('chained' if 0x80000034 in cmds else 'NOT chained', 'dyld_info' if (0x80000022 in cmds or 0x22 in cmds) else '')
EOF
