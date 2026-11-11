#!/usr/bin/env bash
# Kernel + prelink (incremental, reuses the LTO reference) and pack with the user's pack-ios17-iig-pac.py
# (boot-args use_contiguous_hint=0 fuxi-panic-fastboot=30, output sadnut-fuxi-ios17-arm64e-iig-pac-20261105).
cd /home/xiaomi/sadnut-fuxi-local-20261103
{
  echo "=== $(date -Is) kernel build (LTO reference reused)"
  bash ./build-ios17-kernel.sh; rc=$?; echo "BUILD_RC=$rc"
  [ "$rc" = 0 ] && { python3 pack-ios17-iig-pac.py | tail -n 25; echo "PACK_RC=$?"; }
  echo "=== $(date -Is) CHAIN_DONE"
} > /tmp/ios17-build.log 2>&1
