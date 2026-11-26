// SPDX-License-Identifier: BSD-2-Clause-Patent
// fuxi: the remote processors' firmware from the device's own firmware partition, read in place - UniXNU-sheng package/AdspFirmware.c
// (docs/zh/porting/power.md section 3) without its VFS half, as C++ for the probe's compile command (2026-11-12). "modem:NAME" is the
// split image NAME.b00, NAME.b01, ... in /image of the FAT volume modem_a (else modem_b), through NativeUfs (read only).
#include <IOKit/IOLib.h>
#include <sys/errno.h>
#include <string.h>
extern "C" {
#include "FatVolume.h"
#include "QcomMdt.h"
int sheng_fw_open(const char *path, void **handle, uint64_t *size);
int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void sheng_fw_close(void *handle);
int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks) __attribute__((weak_import));
int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst) __attribute__((weak_import));
}

namespace {
struct Partition {
    uint32_t lun;
    uint64_t first;
    uint64_t cached;  // block in block (UINT64_MAX: none)
    uint8_t block[4096];
};
Partition *gPart;
FatVolume *gVolume;

// Bytes of the partition: whole blocks straight into to, the ends of a range through the one-block cache.
int partitionRead(void *context, uint64_t offset, void *to, size_t length) {
    Partition *p = static_cast<Partition *>(context);
    uint8_t *out = static_cast<uint8_t *>(to);
    while (length) {
        const uint64_t lba = offset / 4096;
        const size_t within = size_t(offset % 4096);
        if (!within && length >= 4096) {
            const uint32_t n = length / 4096 > 256 ? 256 : uint32_t(length / 4096);
            if (!native_ufs_part_read(p->lun, p->first + lba, n, out)) return EIO;
            out += size_t(n) * 4096; offset += uint64_t(n) * 4096; length -= size_t(n) * 4096;
            continue;
        }
        if (p->cached != lba) {
            p->cached = UINT64_MAX;
            if (!native_ufs_part_read(p->lun, p->first + lba, 1, p->block)) return EIO;
            p->cached = lba;
        }
        const size_t take = 4096 - within < length ? 4096 - within : length;
        memcpy(out, p->block + within, take);
        out += take; offset += take; length -= take;
    }
    return 0;
}

int mountPartition() {
    static const char *const names[] = {"modem_a", "modem_b"};
    if (gVolume) return 0;
    if (!native_ufs_part_find || !native_ufs_part_read) return ENXIO;
    Partition *p = static_cast<Partition *>(IOMallocZero(sizeof(Partition)));
    FatVolume *v = static_cast<FatVolume *>(IOMallocZero(sizeof(FatVolume)));
    if (!p || !v) {if (p) IOFree(p, sizeof *p); if (v) IOFree(v, sizeof *v); return ENOMEM;}
    int rc = ENOENT;
    for (unsigned i = 0; i < 2 && rc; ++i) {
        uint64_t blocks = 0;
        // ENXIO while the UFS driver is not ready yet: the caller tries again later
        if (!native_ufs_part_find(names[i], &p->lun, &p->first, &blocks)) {rc = ENXIO; continue;}
        p->cached = UINT64_MAX;
        rc = fat_mount(v, blocks * 4096, partitionRead, p) ? EFTYPE : 0;
        IOLog("fuxi adsp fw: %s lun %u first %llu blocks %llu fat %d\n", names[i], p->lun, p->first, blocks, rc);
    }
    if (rc) {IOFree(p, sizeof *p); IOFree(v, sizeof *v); return rc;}
    gPart = p;
    gVolume = v;
    return 0;
}

struct ModemImage {
    QcomMdtSplit split;
    FatFile piece[QCOM_MDT_MAX_SEGMENTS];
};

int pieceRead(void *context, unsigned piece, uint64_t offset, void *to, size_t length) {
    ModemImage *m = static_cast<ModemImage *>(context);
    return fat_read(gVolume, &m->piece[piece], offset, to, length);
}

int openModem(const char *name, ModemImage **image, uint64_t *size) {
    char path[48];
    int rc = mountPartition();
    if (rc) return rc;
    ModemImage *m = static_cast<ModemImage *>(IOMallocZero(sizeof(ModemImage)));
    if (!m) return ENOMEM;
    snprintf(path, sizeof path, "image/%s.b00", name);
    FatFile headers;
    rc = fat_open(gVolume, path, &headers);
    if (rc == FatNotFound) rc = ENOENT;
    else if (rc) rc = EIO;
    else if (headers.size < 52 || headers.size > 64 * 1024) rc = EFTYPE;
    uint8_t *bytes = rc ? nullptr : static_cast<uint8_t *>(IOMallocData(headers.size));
    if (!rc && !bytes) rc = ENOMEM;
    if (!rc && fat_read(gVolume, &headers, 0, bytes, headers.size)) rc = EIO;
    if (!rc && qcom_mdt_split(&m->split, bytes, headers.size)) rc = EFTYPE;
    if (bytes) IOFreeData(bytes, headers.size);
    for (unsigned i = 0; !rc && i < m->split.count; ++i) {
        if (!m->split.piece[i].size) continue;
        snprintf(path, sizeof path, "image/%s.b%02u", name, i);
        if (fat_open(gVolume, path, &m->piece[i])) rc = ENOENT;
        else if (m->piece[i].size < m->split.piece[i].size) rc = EFTYPE;
    }
    IOLog("fuxi adsp fw: %s: %d pieces, %llu bytes, rc %d\n", name, rc ? -1 : int(m->split.count), rc ? 0ull : m->split.size, rc);
    if (rc) {IOFree(m, sizeof *m); return rc;}
    *image = m;
    *size = m->split.size;
    return 0;
}
}  // namespace

extern "C" int sheng_fw_open(const char *path, void **handle, uint64_t *size) {
    if (strncmp(path, "modem:", 6)) return ENOENT;
    ModemImage *m = nullptr;
    const int rc = openModem(path + 6, &m, size);
    if (!rc) *handle = m;
    return rc;
}

extern "C" int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length) {
    ModemImage *m = static_cast<ModemImage *>(handle);
    return qcom_mdt_split_read(&m->split, pieceRead, m, offset, to, length) ? EIO : 0;
}

extern "C" void sheng_fw_close(void *handle) {
    if (handle) IOFree(handle, sizeof(ModemImage));
}
