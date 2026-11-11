#!/usr/bin/env bash
# LTO comparison kernel (documented matching input), then the arm64e kernel + prelink with the unmapped-call probe.
cd /home/xiaomi/sadnut-fuxi-local-20261103
{
  echo "=== $(date -Is) LTO reference build"
  # RELEASE-LTO comparison kernel, linked with the LTO-capable ld64 from build-lto-toolchain.sh. Its cache key
  # hashes the whole xnu source, so every xnu patch would force a full LTO rebuild; a patch that does not change
  # what the matcher compares (e.g. one call site in OSKext::start) does not need one. Rebuild only on request.
  if [ "${REBUILD_REFERENCE:-0}" = 1 ]; then
    bash ./build-ios17-reference.sh --lto; rc=$?; echo "REF_RC=$rc"
    [ "$rc" = 0 ] || { echo "=== reference failed, kernel not built"; echo "=== $(date -Is) CHAIN_DONE"; exit "$rc"; }
  else
    echo "=== reusing LTO reference $(sha256sum UniXNU/out-ios17/fuxi/tree/build/xnu/fileset-reference/mach.release.bcm2837.unstripped.noctf | cut -c1-16)"
  fi
  echo "=== $(date -Is) kernel build with probe (LTO reference)"
  bash ./build-ios17-kernel.sh; rc=$?; echo "BUILD_RC=$rc"
  [ "$rc" = 0 ] && { python3 pack-ios17-kernel-test.py | tail -n 25; echo "PACK_RC=$?"; }
  echo "=== $(date -Is) CHAIN_DONE"
} > /tmp/ios17-build.log 2>&1
