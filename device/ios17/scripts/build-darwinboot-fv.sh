#!/usr/bin/env bash
# Replace the firmware's DarwinBoot (arm64 MH_EXECUTE only) in the t32 UEFI with UniXNU drivers/DarwinBoot
# 3c75734 (arm64e + MH_FILESET), unchanged and with its original FILE_GUID. Firmware-volume images are
# trusted by the secure-boot image verification, which refuses a loose driver from the payload.
set -euo pipefail
BASE=/home/xiaomi/sadnut-fuxi-local-20261103
W=$BASE/darwinboot-v2-fv
P=$BASE/pack-work-ios17-kernel
EDK2=/home/xiaomi/up/UniXNU/out/feeds/mu_basecore-89034cbb
rm -rf "$W"; mkdir -p "$W"
cp -r "$BASE/UniXNU/drivers/DarwinBoot" "$W/DarwinBoot"; rm -rf "$W/DarwinBoot/.git"
sed -i '/Test\/Validation.inf/d' "$W/DarwinBoot/DarwinBootPkg/DarwinBoot.dsc"
sed -i "s#OUTPUT_DIRECTORY = build/DarwinBoot#OUTPUT_DIRECTORY = $W/Build#" "$W/DarwinBoot/DarwinBootPkg/DarwinBoot.dsc"
export WORKSPACE=$W PACKAGES_PATH=$W/DarwinBoot:$BASE/UniXNU/package:$EDK2 EDK_TOOLS_PATH=$EDK2/BaseTools
export CONF_PATH=$W/Conf CLANG_BIN=/usr/bin/ PYTHON_COMMAND=python3
mkdir -p "$CONF_PATH"; cp /home/xiaomi/up/UniXNU/out/fuxi/edk2-conf/*.txt "$CONF_PATH/"
export PATH=$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH
export PYTHONPATH=$EDK_TOOLS_PATH/Source/Python
cd "$W"
build -p DarwinBootPkg/DarwinBoot.dsc -a AARCH64 -b RELEASE -t CLANGPDB -n 6 > "$W/build.log" 2>&1 || { tail -30 "$W/build.log"; exit 1; }
EFI=$(find "$W/Build" -path '*AARCH64*' -name DarwinBoot.efi | head -1)
echo "efi $EFI $(stat -c %s "$EFI")"
GenSec -s EFI_SECTION_PE32 -o "$W/pe32.sec" "$EFI"
GenSec -s EFI_SECTION_USER_INTERFACE -n DarwinBoot -o "$W/ui.sec"
GenFfs -t EFI_FV_FILETYPE_DRIVER -g 1B64AF87-8415-4B57-9B16-680D70913B9F -o "$W/DarwinBoot.ffs" -i "$W/pe32.sec" -i "$W/ui.sec"
ls -la "$W/DarwinBoot.ffs" "$P/old-db/DarwinBoot.ffs"
cd "$P"
python3 "$EDK2/BaseTools/Source/Python/FMMT/FMMT.py" -r t32.fd FV1 DarwinBoot "$W/DarwinBoot.ffs" t32-darwinboot-v2.fd 2>&1 | tail -5
ls -la t32-darwinboot-v2.fd
python3 "$EDK2/BaseTools/Source/Python/FMMT/FMMT.py" -v t32-darwinboot-v2.fd 2>&1 | grep -E 'DarwinBoot|Free Volume|Total Volume' | head
rm -rf new-db && mkdir new-db
python3 "$EDK2/BaseTools/Source/Python/FMMT/FMMT.py" -e t32-darwinboot-v2.fd DarwinBoot new-db/DarwinBoot.ffs > /dev/null 2>&1
cmp new-db/DarwinBoot.ffs "$W/DarwinBoot.ffs" && echo "REPLACED FFS VERIFIED"
