// WCN7850 hw2.0 firmware, SHA-256 equal to UniXNU-sheng firmware/wcn7850/manifest.json (see wifi-firmware/manifest.json), embedded as zlib
// streams (build-native-legacy-display.py writes the .zlib files) and inflated on first use by fuxi_wifi_firmware_get (FuxiWifiNw.cpp):
// DarwinBoot refuses a kernel.macho over 128 MiB, which the plain files (8.8 MB) pushed it past. In __DATA, after the kernel's code: in
// __DATA,__const (before __TEXT_EXEC) they would move late objects out of the +/-128 MB branch range of the prelinked kexts.
.section __DATA,__data
.p2align 4
.globl _fuxi_wifi_amss_z_start
_fuxi_wifi_amss_z_start:
.incbin "/home/xiaomi/sadnut-fuxi-local-20261103/wifi-firmware/amss.bin.zlib"
.globl _fuxi_wifi_amss_z_end
_fuxi_wifi_amss_z_end:
.p2align 4
.globl _fuxi_wifi_m3_z_start
_fuxi_wifi_m3_z_start:
.incbin "/home/xiaomi/sadnut-fuxi-local-20261103/wifi-firmware/m3.bin.zlib"
.globl _fuxi_wifi_m3_z_end
_fuxi_wifi_m3_z_end:
.p2align 4
.globl _fuxi_wifi_board2_z_start
_fuxi_wifi_board2_z_start:
.incbin "/home/xiaomi/sadnut-fuxi-local-20261103/wifi-firmware/board-2.bin.zlib"
.globl _fuxi_wifi_board2_z_end
_fuxi_wifi_board2_z_end:
