// SPDX-License-Identifier: BSD-2-Clause-Patent
// The WCN7850 firmware from the partition named "oops" (iOS 17 arm64e build, 2026-11-12), the way the UniXNU reference reads it
// (https://unixnu.com/zh/porting/network; UniXNU-sheng package/WifiProbe.cpp pFwGet at 516a780): block 0 of the partition is a WFW1
// container header - 'WFW1', u32 count, then count entries of name[24], u32 offset (bytes from the partition start, 4096-aligned),
// u32 size - and each file follows uncompressed. tools: device-ios17/scripts/make-wifi-fw.py builds the image the user flashes.
// The iPadOS build embeds the files in the kernel instead (FuxiWifiFwStore.cpp); the iOS 17 fileset kernel has no __DATA room for them
// (the native __DATA must fit 0x171070 bytes ahead of the retained kexts' data).
// Same interface as FuxiWifiFwStore.cpp: each file is read once, on its first request, into memory kept for the boot; its size must be
// the one in wifi-firmware/manifest.json.
// First source since 2026-11-12: the GPU addon of the boot payload (device-ios17/scripts/add-wifi-fw-to-addon.py adds wifi/amss.bin,
// wifi/m3.bin, wifi/board-2.bin, each zlib-compressed). It is boot memory NativeRamBlock never frees (native_ram_block_gpu_addon), so the
// firmware travels with the boot image. The oops partition stays the fallback: after a forced reboot the boot loader had replaced its
// WFW1 header with a crash record ("amss.bin FAILED (rc -7)"); its first bytes are now logged when the header is wrong.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/zlib.h>
#include <string.h>

extern "C" int native_ufs_part_find(const char *, uint32_t *, uint64_t *, uint64_t *);   // NativeUfs.cpp, read-only
extern "C" int native_ufs_part_read(uint32_t, uint64_t, uint32_t, void *);               // NativeUfs.cpp, read-only
extern "C" int native_ram_block_gpu_addon(const uint8_t **, uint64_t *) __attribute__((weak_import));   // NativeRamBlock.cpp

namespace {
struct PartFw {const char *name; size_t bytes; uint8_t *data;};
PartFw gPartFw[] = {{"amss.bin", 6090816, nullptr}, {"m3.bin", 299660, nullptr}, {"board-2.bin", 2431968, nullptr}};
uint32_t gLun; uint64_t gFirst, gBlocks;
bool gFound;
int gLastError;

// zlib's state from the kernel heap, its size in front (as kern_exec.c's addon install does).
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

// wifi/<name> from the GPU addon ("NGPUADD2", u32 count, u32 0, u64 total; per file u32 path bytes, u32 mode, u64 size, u64 packed size,
// path and zlib stream each padded to 8). 0 with *out set, 1 when the addon or the entry is absent, negative for a damaged entry.
int fromAddon(const PartFw &f, uint8_t **out) {
    const uint8_t *addon = nullptr;
    uint64_t bytes = 0;
    if (!native_ram_block_gpu_addon || !native_ram_block_gpu_addon(&addon, &bytes)) return 1;
    if (bytes < 24 || memcmp(addon, "NGPUADD2", 8)) return 1;
    uint32_t count; uint64_t total;
    memcpy(&count, addon + 8, 4); memcpy(&total, addon + 16, 8);
    if (total != bytes) return 1;
    char want[40];
    snprintf(want, sizeof want, "wifi/%s", f.name);
    uint64_t at = 24;
    for (uint32_t i = 0; i < count; ++i) {
        if (bytes - at < 24) return 1;
        uint32_t nameBytes; uint64_t size, packed;
        memcpy(&nameBytes, addon + at, 4); memcpy(&size, addon + at + 8, 8); memcpy(&packed, addon + at + 16, 8);
        at += 24;
        const uint64_t namePadded = (nameBytes + 7) & ~7ull, packedPadded = (packed + 7) & ~7ull;
        if (namePadded > bytes - at || packedPadded > bytes - at - namePadded) return 1;
        const char *name = reinterpret_cast<const char *>(addon + at);
        at += namePadded;
        if (nameBytes == strlen(want) + 1 && !memcmp(name, want, nameBytes)) {
            if (size != f.bytes) return -8;
            uint8_t *buf = static_cast<uint8_t *>(IOMallocData(size));
            if (!buf) return -12;
            z_stream z;
            memset(&z, 0, sizeof z);
            z.zalloc = zAlloc; z.zfree = zFree;
            z.next_in = const_cast<Bytef *>(addon + at); z.avail_in = uInt(packed);
            z.next_out = buf; z.avail_out = uInt(size);
            int r = inflateInit(&z);
            if (r == Z_OK) {r = inflate(&z, Z_FINISH); inflateEnd(&z);}
            if (r != Z_STREAM_END || z.total_out != size) {IOFreeData(buf, size); return -9;}
            *out = buf;
            return 0;
        }
        at += packedPadded;
    }
    return 1;
}

// The storage driver comes up a while into the boot (the WiFi worker asks for amss.bin at stage 5): wait for the partition up to 120 s.
bool findPartition() {
    if (gFound) return true;
    for (unsigned i = 0; i < 240; ++i) {
        if (native_ufs_part_find("oops", &gLun, &gFirst, &gBlocks)) {
            gFound = gBlocks != 0;
            IOLog("fuxi wifi fw: partition oops lun %u first %llu blocks %llu\n", gLun, gFirst, gBlocks);
            return gFound;
        }
        IOSleep(500);
    }
    IOLog("fuxi wifi fw: no partition named oops\n");
    return false;
}
}

// Returns 0; -2 for a file the container does not hold, -12 out of memory, other negative values for a missing or damaged container.
extern "C" int fuxi_wifi_firmware_get(const char *name, const uint8_t **data, size_t *size) {
    static IOLock *lock;
    if (!__atomic_load_n(&lock, __ATOMIC_ACQUIRE)) {
        IOLock *created = IOLockAlloc(), *expected = nullptr;
        if (!created) return -12;
        if (!__atomic_compare_exchange_n(&lock, &expected, created, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) IOLockFree(created);
    }
    const char *base = name;
    for (const char *c = name; *c; ++c) if (*c == '/') base = c + 1;
    PartFw *f = nullptr;
    for (PartFw &e : gPartFw) if (!strcmp(e.name, base)) f = &e;
    if (!f) return -2;
    IOLockLock(lock);
    int rc = 0;
    if (!f->data) {
        uint8_t *fromBoot = nullptr;
        const int a = fromAddon(*f, &fromBoot);
        if (!a) {
            f->data = fromBoot;
            IOLog("fuxi wifi fw: %s read from the boot addon (%zu bytes)\n", f->name, f->bytes);
        } else if (a < 0) {
            IOLog("fuxi wifi fw: %s in the boot addon is damaged (%d); trying the oops partition\n", f->name, a);
        }
    }
    if (!f->data) {
        uint8_t *header = nullptr;
        uint32_t offset = 0, bytes = 0;
        if (!findPartition()) rc = -6;
        else if (!(header = static_cast<uint8_t *>(IOMallocData(4096)))) rc = -12;
        else if (!native_ufs_part_read(gLun, gFirst, 1, header)) rc = -5;
        else if (memcmp(header, "WFW1", 4)) {
            rc = -7;
            IOLog("fuxi wifi fw: oops block 0 starts %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x, not WFW1\n",
                  header[0], header[1], header[2], header[3], header[4], header[5], header[6], header[7],
                  header[8], header[9], header[10], header[11], header[12], header[13], header[14], header[15]);
        }
        else {
            uint32_t count = 0;
            memcpy(&count, header + 4, 4);
            for (uint32_t i = 0; i < count && i < 127; ++i) {
                const uint8_t *e = header + 8 + 32 * i;
                if (!strncmp(reinterpret_cast<const char *>(e), f->name, 24)) {memcpy(&offset, e + 24, 4); memcpy(&bytes, e + 28, 4);}
            }
            if (!bytes) rc = -2;
            else if (bytes != f->bytes || (offset & 4095) || offset / 4096 + (bytes + 4095) / 4096 > gBlocks) rc = -8;
        }
        if (header) IOFreeData(header, 4096);
        if (!rc) {
            const size_t rounded = (bytes + 4095) & ~size_t(4095);
            uint8_t *buf = static_cast<uint8_t *>(IOMallocData(rounded));
            if (!buf) rc = -12;
            else if (!native_ufs_part_read(gLun, gFirst + offset / 4096, uint32_t(rounded / 4096), buf)) {IOFreeData(buf, rounded); rc = -5;}
            else f->data = buf;
        }
        IOLog("fuxi wifi fw: %s %s (rc %d, %u bytes at %u)\n", f->name, rc ? "FAILED" : "read", rc, bytes, offset);
        gLastError = rc;
    }
    IOLockUnlock(lock);
    if (rc) return rc;
    *data = f->data; *size = f->bytes;
    return 0;
}
