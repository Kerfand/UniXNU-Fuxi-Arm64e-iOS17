// SPDX-License-Identifier: BSD-2-Clause-Patent
// fuxi (2026-11-15): Bluetooth firmware (qca/hmtbtfw20.tlv, qca/hmtnv20.*) from the boot GPU addon's bluetooth/<name> entries,
// the container FuxiWifiFwPart.cpp reads wifi/<name> from ("NGPUADD2", per file u32 path bytes, u32 mode, u64 size, u64 packed
// size, path and zlib stream each padded to 8). Same interface as UniXNU-sheng's BluetoothFirmware.c.
#include <IOKit/IOLib.h>
#include <libkern/zlib.h>
#include <sys/errno.h>
#include "BluetoothFirmware.h"
extern "C" int native_ram_block_gpu_addon(const uint8_t **, uint64_t *) __attribute__((weak_import));
namespace {
voidpf zAlloc(voidpf, uInt items, uInt size) {
    size_t bytes = size_t(items) * size + 16;
    uint8_t *p = static_cast<uint8_t *>(IOMallocData(bytes));
    if (!p) return Z_NULL;
    memcpy(p, &bytes, sizeof bytes);
    return p + 16;
}
void zFree(voidpf, voidpf address) {
    uint8_t *p = static_cast<uint8_t *>(address) - 16;
    size_t bytes;
    memcpy(&bytes, p, sizeof bytes);
    IOFreeData(p, bytes);
}
}
extern "C" int sheng_bt_firmware_read(const char *name, uint8_t **data, size_t *size) {
    if (!data || !size || !name) return EINVAL;
    *data = nullptr; *size = 0;
    if (strncmp(name, "qca/", 4)) return ENOENT;
    const uint8_t *addon = nullptr; uint64_t bytes = 0;
    if (!native_ram_block_gpu_addon || !native_ram_block_gpu_addon(&addon, &bytes)) return EAGAIN;
    if (bytes < 24 || memcmp(addon, "NGPUADD2", 8)) return ENOENT;
    uint32_t count; uint64_t total;
    memcpy(&count, addon + 8, 4); memcpy(&total, addon + 16, 8);
    if (total != bytes) return ENOENT;
    char want[64];
    snprintf(want, sizeof want, "bluetooth/%s", name + 4);
    uint64_t at = 24;
    for (uint32_t i = 0; i < count; ++i) {
        if (bytes - at < 24) return ENOENT;
        uint32_t nameBytes; uint64_t length, packed;
        memcpy(&nameBytes, addon + at, 4); memcpy(&length, addon + at + 8, 8); memcpy(&packed, addon + at + 16, 8);
        at += 24;
        const uint64_t namePadded = (nameBytes + 7) & ~7ull, packedPadded = (packed + 7) & ~7ull;
        if (namePadded > bytes - at || packedPadded > bytes - at - namePadded) return ENOENT;
        const char *entry = reinterpret_cast<const char *>(addon + at);
        at += namePadded;
        if (nameBytes == strlen(want) + 1 && !memcmp(entry, want, nameBytes)) {
            if (!length || length > 4u * 1024 * 1024) return EINVAL;
            uint8_t *buf = static_cast<uint8_t *>(IOMallocData(length));
            if (!buf) return ENOMEM;
            z_stream z; memset(&z, 0, sizeof z);
            z.zalloc = zAlloc; z.zfree = zFree;
            z.next_in = const_cast<Bytef *>(addon + at); z.avail_in = uInt(packed);
            z.next_out = buf; z.avail_out = uInt(length);
            int r = inflateInit(&z);
            if (r == Z_OK) {r = inflate(&z, Z_FINISH); inflateEnd(&z);}
            if (r != Z_STREAM_END || z.total_out != length) {IOFreeData(buf, length); return EIO;}
            IOLog("[bluetooth] firmware %s from the boot addon (%llu bytes)\n", name, (unsigned long long)length);
            *data = buf; *size = size_t(length);
            return 0;
        }
        at += packedPadded;
    }
    IOLog("[bluetooth] firmware %s not in the boot addon\n", name);
    return ENOENT;
}
extern "C" void sheng_bt_firmware_release(uint8_t *data, size_t size) {if (data) IOFreeData(data, size);}
