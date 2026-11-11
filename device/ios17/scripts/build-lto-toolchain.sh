#!/usr/bin/env bash
# LTO-capable ld64 for the RELEASE-LTO fileset comparison kernel (unixnu.com/drivers/xnu-prelink: "Keep LTO
# enabled for this comparison"). The installed toolchain's ld64 is built --disable-lto-support and its LLVM 24
# build tree was removed, so this builds, beside it and without touching it:
#   1. libLTO from the same llvm-project commit as the toolchain clang (bitcode must match), AArch64 only;
#   2. cctools-port at the toolchain's ref with LTO support, linked against that libLTO.
# Source and build trees are kept, so a rerun is incremental. JOBS defaults to 8; links run one at a time
# (7 GB of RAM).
set -euo pipefail
LLVM_COMMIT=58c46bae21181fb4240cf964461bea738d96d50e
CCTOOLS_PORT_REF=1030.6.3-ld64-956.6
TC=/home/xiaomi/unixnu-linux
ROOT=/home/xiaomi/lto-toolchain
JOBS=${JOBS:-8}
HOSTCC=/usr/lib/llvm-21/bin/clang
HOSTCXX=/usr/lib/llvm-21/bin/clang++
mkdir -p "$ROOT"
step() { echo; echo "=== $(date -Is) $*"; }
checkout() {   # dir url ref: one commit, no history
  if [ ! -e "$1/.unixnu-checked-out" ]; then
    mkdir -p "$1"; git init -q "$1"
    # GitHub connections from this host time out now and then; retry before giving up.
    for try in 1 2 3 4 5; do
      git -C "$1" fetch --progress --depth 1 "$2" "$3" && break
      [ "$try" = 5 ] && { echo "fetch of $2 failed 5 times"; exit 1; }
      echo "fetch attempt $try failed; retrying in 20 s"; sleep 20
    done
    git -C "$1" checkout -q --force FETCH_HEAD
    touch "$1/.unixnu-checked-out"
  fi
  echo "$1 at $(git -C "$1" rev-parse HEAD)"
}

step "llvm-project $LLVM_COMMIT"
checkout "$ROOT/llvm-project" https://github.com/llvm/llvm-project "$LLVM_COMMIT"

step "configure libLTO"
LINKER=()
command -v ld.lld >/dev/null && LINKER=(-DLLVM_USE_LINKER=lld)
if [ ! -f "$ROOT/llvm-build/build.ninja" ]; then
  cmake -G Ninja -S "$ROOT/llvm-project/llvm" -B "$ROOT/llvm-build" \
    -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_TARGETS_TO_BUILD=AArch64 -DLLVM_ENABLE_PROJECTS= \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_PARALLEL_LINK_JOBS=1 "${LINKER[@]}" \
    -DCMAKE_C_COMPILER="$HOSTCC" -DCMAKE_CXX_COMPILER="$HOSTCXX"
fi

step "build libLTO and llvm-config (-j$JOBS)"
ninja -C "$ROOT/llvm-build" -j"$JOBS" LTO llvm-config
ls -la "$ROOT"/llvm-build/lib/libLTO.so*
"$ROOT/llvm-build/bin/llvm-config" --version

step "cctools-port $CCTOOLS_PORT_REF"
checkout "$ROOT/cctools-port" https://github.com/tpoechtrager/cctools-port.git "$CCTOOLS_PORT_REF"

step "configure cctools with LTO support"
cd "$ROOT/cctools-port/cctools"
if [ ! -f Makefile ]; then
  CC="$HOSTCC" CXX="$HOSTCXX" ./configure --prefix="$ROOT" --target=arm64-apple-darwin --program-prefix= \
    --enable-lto-support --with-llvm-config="$ROOT/llvm-build/bin/llvm-config" \
    --with-libdispatch="$TC/CommandLineTools/usr/libdispatch" \
    --with-libblocksruntime="$TC/CommandLineTools/usr/libdispatch"
fi
grep -E '^#define (LTO_SUPPORT|LTO_LIBRARY|LLVM)' config.h 2>/dev/null || grep -rn 'LTO' config.log | tail -5

step "build cctools (-j$JOBS)"
make -j"$JOBS" PROGRAM_PREFIX=
make install PROGRAM_PREFIX=

step "check ld"
"$ROOT/bin/ld" -v 2>&1 | head -5
echo "LTO_TOOLCHAIN_DONE"
