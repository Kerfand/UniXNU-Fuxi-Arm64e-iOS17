#!/usr/bin/env bash
# Checkpoint 1 of the iOS 17 move: arm64e xnu-10063 kernel + fileset prelink against iPhone15,2 21H16.
# Isolated from the iPadOS build: own out root (UniXNU/out-ios17) and device overlay (device-ios17).
set -euo pipefail
BASE=/home/xiaomi/sadnut-fuxi-local-20261103
cd "$BASE/UniXNU"
exec 9>"$BASE/ios17-build.lock"
flock -n 9 || exit 75
rm -f -- "$BASE/ios17-build.exit" "$BASE/ios17-build.finished"
exec > >(tee -a "$BASE/ios17-build.log") 2>&1
export UNIXNU_TOOLCHAIN=/home/xiaomi/unixnu-linux
export PATH="$BASE/bin:$PATH"
export PYTHONUNBUFFERED=1
# 8 parallel jobs (user preference); the xnu make in fuxi-kernel-baseline.sh reads JOBS. Builds are incremental:
# nothing here removes OBJROOT.
export JOBS="${JOBS:-8}"
export UNIXNU_OUT="$BASE/UniXNU/out-ios17"
export UNIXNU_XNU_ARCH=arm64e
export UNIXNU_FILESET_KERNEL_FIRST=1
# The fileset prelink reads the native chained fixups (ld64 -kernel), not __thread_starts.
export XNU_KERNEL_CHAINED_FIXUPS=1
export UNIXNU_KERNELCACHE="$UNIXNU_OUT/ipsw/21H16/kernelcache.decompressed"
trap 'result=$?; printf "%s\n" "$result" > "$BASE/ios17-build.exit"; date -Is > "$BASE/ios17-build.finished"' EXIT
date -Is > "$BASE/ios17-build.started"
mkdir -p "$UNIXNU_OUT/ipsw/21H16"
[ -e "$UNIXNU_OUT/feeds" ] || ln -s "$BASE/UniXNU/out/feeds" "$UNIXNU_OUT/feeds"
[ -f "$UNIXNU_OUT/ipsw/21H16/kernelcache.decompressed" ] || cp /home/xiaomi/ios17-21H16/kernelcache.decompressed "$UNIXNU_OUT/ipsw/21H16/"
./unixnu defconfig fuxi_ios17_kernel_defconfig -D ../device-ios17
./unixnu build kernel -D ../device-ios17 -j "$JOBS"
