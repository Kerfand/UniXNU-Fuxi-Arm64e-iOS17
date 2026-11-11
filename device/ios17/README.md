# fuxi on iPhone15,2 iOS 17.7 (21H16) — arm64e kernel bring-up

**Status (2026-11-06): checkpoint 1 reached.** A kernel-only boot.img (`fastboot boot`, nothing flashed) boots
the arm64e xnu-10063 kernel prelinked with the original 21H16 fileset kexts through IOKit kext start (corecrypto,
AppleImage4, AMFI, Sandbox, apfs) and `bsd_init`, and ends at the root-device wait for the placeholder UUID
`D73A0000-0000-4000-8000-000000000000`. See PROGRESS.md (written just before the last fixes) for the history.

This directory is a snapshot of the local bring-up files; the build runs from the working directory
`/home/xiaomi/sadnut-fuxi-local-20261103` (paths in the scripts are absolute to it).

- `overlay/` — the real files of the device overlay `device-ios17/`: board.toml (21H16 ipsw, minimal prelink
  roots, early security init), `configs/fuxi_ios17_kernel_defconfig`, and the overlay scripts
  (`fuxi-kernel-baseline.sh` applies `UniXNU/patches/xnu-10063-arm64e` and unwinds
  `early-security-init-sign.patch` before the shared patch loop). Everything else in `device-ios17/` is a
  symlink into `device/`; `overlay/SYMLINKS.txt` lists them.
- `scripts/` — `build-ios17-iig-chain.sh` (kernel + prelink, incremental, then `pack-ios17-iig-pac.py`), the
  kernel / RELEASE-LTO reference builds, `build-lto-toolchain.sh` (libLTO from the clang's own llvm commit and an
  LTO-capable cctools-port ld64 in /home/xiaomi/lto-toolchain), DarwinBoot v2 into the firmware volume, and the
  pack scripts.
- `tools/` — one-off generators and probes used while debugging (IIG handler patch, fastboot-after-panic, sandbox).

Related source commits: UniXNU branch `fuxi/ios17-arm64e-checkpoint1` (patches/xnu-10063-arm64e, arm64e build
scripts) and XnuPrelink branch `fuxi-ios17-21h16` (21H16 fileset support and the reviewed identity fixture).
No products (boot.img, kernelcache, out-ios17) are part of these commits.

Next (checkpoint 2): storage (UFS), a real root volume, display, and the IOCPUInterruptController warning.
