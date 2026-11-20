# UniXNU-Fuxi-Arm64e-iOS17

[简体中文](README项目.md) · **English**

UniXNU port sources for running **iOS 17.7 (21H16, iPhone15,2 kernelcache, arm64e)** on the Xiaomi 13 (fuxi, SM8550).

> This repository contains sources and scripts only: **no Apple binaries** (kernelcache, IPSW, dyld shared cache, files extracted from the system volume, wallpaper images, etc.), **no build products** (kernel, boot.img, payload, GPU addon), and **no keys or firmware**. All of these must be generated from your own inputs as described below.

| Item | Value |
| --- | --- |
| Device | Xiaomi 13 (`fuxi`, SM8550) |
| Target OS | iOS 17.7, build 21H16 |
| Kernel | xnu-10063, arm64e |
| Machine identity | iPhone15,2 kernelcache; ADT identifies as iPhone 14 Pro (D73AP) |
| Current progress | Reaches the Home Screen; touch, buttons, audio, storage, serial, battery, migration and wallpaper work |

## State (2026-11-19)

| Feature | State | Notes |
| --- | --- | --- |
| Touch | ✅ | FTM5 works |
| Buttons | ✅ | Power and volume keys |
| Audio | ✅ | Playback and volume control |
| Storage | ✅ | User data survives reboot |
| Serial | ✅ | Reboot, screenshot, key injection |
| Battery | ✅ | Level and charging state |
| Migration | ✅ | System apps completed |
| Wallpaper | ✅ | Lock screen and Home Screen wallpaper |
| Charging | ⚠️ | Fast charging does not work |
| Bluetooth | ⚠️ | Power-up and initialisation done |
| Baseband | ⚠️ | Working through the first modem step |
| Wi-Fi | ⚠️ | Scan and join work, but there is no internet access |
| GPU | ⚠️ | Works, crashes in some scenarios |
| Camera | ❌ | Does not work |
| Flashlight | ❌ | Does not work |

✅ works · ⚠️ partial · ❌ does not work

## Repository layout

| Directory | Contents |
| --- | --- |
| `device/` | Current sources of the fuxi device repository (based on SadNutWall/unixnu-fuxi, branch `ios17-arm64e-checkpoint1`): FTM5 touch, buttons, backlight, Wi-Fi, GPU probes, UFS, etc. |
| `ios17/` | iOS 17 / arm64e overlay: board configuration, kernel baseline scripts, 21H16 build scripts, status bar library. Its symlinks point back into `device/` |
| `unixnu/patches/xnu-10063-arm64e/` | xnu-10063 (arm64e) kernel patches, including the iOS 17 boot policy, iPhone wallpaper mounting and probes |
| `unixnu/diffs/` | Uncommitted changes of the UniXNU framework (SadNutWall/UniXNU, `fuxi/ios17-arm64e-checkpoint1`) and its submodules (`*.diff`, base commits in `*.HEAD.txt`) |
| `unixnu/files/` | New files from those repositories that are not tracked upstream |
| `unixnu/commits/` | Local-only commits that were never pushed upstream (`git format-patch`): the arm64e xnu-10063 kernel body in UniXNU, 21H16 fileset support in XnuPrelink (with the function map), and checkpoint 1 of the device repository |
| `host/` | Host-side scripts: `build-ios17-kernel.sh` (8-thread incremental build), `pack-ios17-hidpath.py` (packs boot.img), `fuxi-auto-cycle.ps1` (unattended loop: serial back to fastboot → temporary boot → log capture) |
| `tools/` | Debug and generation tools: Data skeleton table generation (`mkskel.py`), HEIC→PNG wallpaper conversion (`heic2png.ps1`, `heicalpha.py`, `mkwallpaper.ps1`), shared-cache symbolication, thread probe analysis, crash analysis, etc. |
| `SKIPPED.txt` | Files excluded during cleanup, with reasons |

## Build overview

1. Prepare UniXNU (SadNutWall/UniXNU). First `git am` `unixnu/commits/UniXNU` (main repository), then `git am` `unixnu/commits/<submodule>` in each submodule (`kits_XnuPrelink`, `drivers_Adreno740`, `drivers_AdrenoMetal`, `drivers_ArmCpu`, `drivers_AppleKeyStoreState`, `drivers_GopFramebuffer`; base commits in each `HEAD.txt`, and for AdrenoMetal import the official 51b7003 sources first). Then apply `unixnu/diffs/*.diff` (base commits in `*.HEAD.txt`) and copy in `unixnu/files/` and `unixnu/patches/`. `unixnu/commits/device` is the commit history of `device/`; `device/` is already the final source.
2. Place `device/` next to UniXNU, with `ios17/` as `device-ios17/`.
3. Prepare your own 21H16 IPSW inputs (kernelcache, system volume dmg, trust cache) and SDK, then generate `device/package/DataSkel21H16.h` (`tools/mkskel.py`) and the wallpaper overlay files (`tools/mkwallpaper.ps1`).
4. Build the kernel with `host/build-ios17-kernel.sh`, pack boot.img with `host/pack-ios17-hidpath.py` (`UFS_PROBE=0`), and boot it temporarily with `fastboot boot` (no userdata flashing).

Absolute paths in the scripts are the author's machine paths; adjust them for your own environment. The daily packaging environment and its pitfalls are in [2026-11-17](#2026-11-17).

## Progress log

### 2026-11-17

- **Speaker output**: both CS35L41 PLLs lock, DSP periods go 4395 out / 4395 back, no errors.
  - Root cause: VirtualAudio's speaker route check (`0x24f2d4`, called by the playback / broadcast / record route handlers) only requires HAL speaker protection when `0x8f698` is true; `0x8f698` = the machine is in the list **and** `os_feature_enabled("VirtualAudio", "siri_ducking_for_speaker")`.
  - Answering `'spac'` from the plugin alone does not work (HAL does not forward that selector to the plugin). The plugin now additionally rewrites VirtualAudio's import of `__os_feature_enabled_impl` on arm64e and returns false **only** for this one switch; the other 102 call sites are untouched. Log beacon: `ShengVA:fuxi: feature siri_ducking_for_speaker forced off`.
  - There is no speaker-protection DSP, so keep the volume low to medium.
- **Daily packaging**: `UFS_PROBE=0 PERSIST=2 DATA_SEED=<scratch-data-48g.img> GPU_ADDON=<addon with the new plugin> BTD_PATCHED=1 BTD=1`.
  - `PERSIST=2` must be paired with the 48G seed; the default 1G seed corrupts the existing 48G store at the end of userdata (APFS checksum errors, panic when mounting Data).
  - `PERSIST` drops `unixnu-iokit-probe=1` to make room for boot arguments, so the plugin's `ShengVA:` / `ShengHAL:` beacons are invisible; do not enable `PERSIST` while diagnosing.
  - The default `GPU_ADDON` of `pack-ios17-hidpath.py` is still the old 11-11 package; an addon with the patched bluetoothd also needs `BTD_PATCHED=1`, otherwise bluetoothd is killed repeatedly because of its signature.
- **Userland builds** (HAL plugin, GPU plugin, Turnip): `CROSS_EXTRA_FLAGS` without kernel flags, `CROSS_CLANG` pointing at UniXNU's clang, `-B` pointing at CommandLineTools to use ld64, and a real ad hoc `codesign` (the toolchain's `codesign` is a stub that yields no CDHash).
- **Keyboard lag (unfixed)**:
  - Writing managed preferences to disable dictation (`Dictation Enabled=false` in `com.apple.assistant.support`, `BootWatchdogPolicy.c`) and disabling assistantd / siriknowledged changed nothing; dictation is not the cause.
  - Hot sampling: Preferences sits at 100% while the keyboard is up, with about 66% of userland in `AutoreleasePoolPage::releaseUntil`; search fields and plain text fields show the same distribution, so it is a loop that runs whenever the keyboard is present. The sampler needs to record full call stacks next.
- **GPU**: one GPU page fault occurred while the keyboard was up; after recovery the plugin restarted backboardd by design (`scanout copy failed 30 times`). Trying the upstream default `TU_DEBUG=gmem,nocb` wedged the whole device during the first migration, so it was reverted to empty.
- **Bluetooth / battery / RTC**: QcaBluetooth tty, patched bluetoothd and boot constraints, battery service, RTC time (see `unixnu/commits/` and `device/package/`).

### 2026-11-15

- **Bluetooth**: Bluetooth power-up fixed ✓ (not yet connected to userland)

### 2026-11-14

- **Battery**: charge-level probe fixed ✓ battery state display fixed ✓
- **Migration**: boot migration fixed ✓ system apps completed
- **Wallpaper**: wallpaper rendering fixed ✓ lock screen / Home Screen wallpaper ✓
- **Storage**: data retention fixed ✓ Data seed grown to 48G
- **Time**: RTC time calibration added

### 2026-11-13

- **Display**: logical resolution enabled at 1125×2436 (375×812 pt, a screen type UIKit recognises), scaled by the GPU (AdrenoMetal `scanout_scale` AIR kernel, `unixnu/diffs/drivers_AdrenoMetal.diff`) to the 1080×2400 panel; Control Center layout is normal again. AdrenoMetal must be built on `official-51b7003-import` (5d56587).
- **Keyboard**: typing works. mobileassetd (a plain arm64 process) crashes with FPAC inside dyld because the kernel does not disable userland PAC; kbd waits on it synchronously, which hangs any app that opens the keyboard. `native-user-jop-disable.patch` clears SCTLR_EL1 EnIA/IB/DA/DB for `disable_user_jop` threads on the way back to EL0 and restores them on entry to EL1. Typing is still laggy (UIKeyboardInputModeController keeps recomputing dictation languages, to be fixed).
- **Audio**: ADSP, the CS35L41 amps and the HAL plugin `device/hal/ShengAudioPlugIn.c` all work.
  - ADT now carries `/product/product-id` and `/product/audio/acoustic-id = 8006` from the real D73AP device tree, so MobileGestalt reports an iPhone 14 Pro and VirtualAudio loads the AID8006 tuning and routing database.
  - The plugin appears as a VirtualAudio `Codec` device (8-channel output, mute/volume controls on output element 4) and a `Built-In Speaker` port has been created.
  - On arm64e the plugin rewrites VirtualAudio's imports (MobileGestalt, fopen, IORegistry, `__cxa_throw`, `_os_log_impl`); error logs appear in the kernel log with the `ShengVA:` beacon.
  - The `graph_configurations.plist` for AID8006 is generated by the build script from your own system volume with the `speaker_*` entries removed (not committed), so the closed-loop speaker protection does not crash for lack of factory calibration data.
  - **Still silent**: the playback route requires HAL-level speaker protection (`spac`), which was not satisfied yet (solved on 11-17).
- **Kernel probes**: the 1 kHz hot sampler gained LR and inclusive full-stack accounting (`zz-hot-sampler.patch`); thread and IOKit probes stay resident with their keywords.

### 2026-11-12

- **Wi-Fi**: scan and join fixed (2.4G) ✓ no internet access ❌
- **GPU**: GPU efficiency improved, UI smoother
- **Status bar**: removed the red bar at the top of the screen

### 2026-11-11

The iOS 17 Home Screen is reachable:

- **GPU / display**: Adreno 740 executes commands, IOSurface and the swapchain work, and b3a8 (XR10) frames are converted on the CPU and presented.
- **Touch**: FTM5 works; on 21H16 the kernel `IOHIDEventDriver` only matches an IOHIDDevice carrying `HIDDefaultBehavior` (with `dk=0` skipping DriverKit); touch no longer hangs once the `OSArray::withObjects` mapping was added.
- **Buttons**: `FuxiButtons` (PMK8550 PON/RESIN + PM8550 GPIO6) feeds NativeHidInput report 2; power and volume keys work.
- **GPU plugin**: Turnip (`libvulkan_freedreno.dylib`) is now linked with chained fixups, fixing the dyld stack overflow when mediaserverd / iconservicesagent loads Metal on a secondary thread (the root cause of slow Settings).
- **Wallpaper**: the HEIC layers of default wallpaper 7565 must be converted to PNG (Windows WIC, alpha preserved) and are mounted by `ios17-wallpaper-iphone.patch`; mounting succeeds, but PosterBoard's render extension does not appear yet, so **the wallpaper was still not displayed** (fixed on 11-14).

## Known issues

- The `target-type` of iPhone15,2 in ADT is still `D72`, while the real device is `D73`.
