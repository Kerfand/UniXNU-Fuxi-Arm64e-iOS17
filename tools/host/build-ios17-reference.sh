#!/usr/bin/env bash
# RELEASE comparison kernel of the same patched xnu-10063 arm64e source, for the fileset prelink.
set -euo pipefail
BASE=/home/xiaomi/sadnut-fuxi-local-20261103
T=$BASE/UniXNU/out-ios17/fuxi/tree
TC=/home/xiaomi/unixnu-linux
exec > >(tee -a "$BASE/ios17-reference.log") 2>&1
export UNIXNU_XNU_ARCH=arm64e UNIXNU_IOS_VERSION=17.0 UNIXNU_XNU_DARWIN=23.6.0 UNIXNU_XNU_VERSION=10063.141.1
export LC_ALL=C CLT_PREFIX=$TC/CommandLineTools/usr XCTOOLCHAIN_PREFIX=$TC/XcodeDefault.xctoolchain/usr
export XNU_TOOLCHAIN_BIN="$CLT_PREFIX/bin $XCTOOLCHAIN_PREFIX/bin" UNIXNU_SDKROOT=$T/.cache/fakeroot UNIXNU_SDK_VERSION=17.0
export UNIXNU_PLATFORM_PATH=$T/.cache/fakeroot/Platforms/iPhoneOS.platform CROSS_CLANG=$TC/llvm/bin/clang
export DARWIN_TRIPLE=arm64e-apple-ios17.0
export CROSS_EXTRA_FLAGS='-fptrauth-function-pointer-type-discrimination -mllvm -disable-atexit-based-global-dtor-lowering'
export CCC_OVERRIDE_OPTIONS='# +-Wno-missing-designated-field-initializers +-Wno-unused-but-set-global +-Wno-unsafe-buffer-usage +-Wno-shadow-header'
export PATH="$CLT_PREFIX/bin:$TC/bin:$BASE/bin:$PATH" JOBS=${JOBS:-8}
# --lto needs an ld64 with LTO support (the toolchain's is built --disable-lto-support). build-lto-toolchain.sh
# builds one against libLTO from the clang's own llvm-project commit; xcrun -find searches XNU_TOOLCHAIN_BIN in
# order, so a directory holding only that ld goes first and every other tool stays the toolchain's.
case " $* " in
  *" --lto "*)
    LTO_LD_DIR=/home/xiaomi/lto-toolchain/ld-only
    mkdir -p "$LTO_LD_DIR" && ln -sf /home/xiaomi/lto-toolchain/bin/ld "$LTO_LD_DIR/ld"
    "$LTO_LD_DIR/ld" -v 2>&1 | grep -q 'LTO support using' || { echo "no LTO-capable ld: run build-lto-toolchain.sh"; exit 1; }
    export XNU_TOOLCHAIN_BIN="$LTO_LD_DIR $XNU_TOOLCHAIN_BIN"
    # The kernel link runs through the clang driver, which finds ld on PATH (not through xcrun): put the same
    # ld-only directory first there too.
    export PATH="$LTO_LD_DIR:$PATH"
    echo "LTO link with $(xcrun -find ld) / clang finds $("$CROSS_CLANG" -print-prog-name=ld): $("$LTO_LD_DIR/ld" -v 2>&1 | grep 'LTO support')" ;;
esac
date -Is
cd "$T"
# Pass --lto for the documented comparison input (unixnu.com/drivers/xnu-prelink: "Keep LTO enabled").
python3 "$BASE/device-ios17/scripts/build-fileset-release-reference.py" --tree "$T" "$@"
date -Is
