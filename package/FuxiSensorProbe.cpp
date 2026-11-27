// SPDX-License-Identifier: BSD-2-Clause-Patent
// FuxiSensorProbe (S6): Proper FastRPC INIT_ATTACH_SNS (sc=0x08010000, in=1, out=0)
// with fastrpc_remote_buf + fastrpc_phy_page metadata in low DRAM (0xd0100000, SID=5).
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/kauth.h>
#include <sys/proc.h>

extern "C" int vn_rdwr(enum uio_rw rw, struct vnode *vp, caddr_t base, int len, off_t offset, enum uio_seg segflg, int ioflg,
    kauth_cred_t cred, int *aresid, proc_t p);
extern "C" {
#include "ShengAdsp.h"
#include "QcomQrtr.h"
#include "SmmuBypass.h"
#include "QcomRpmh.h"
QcomQrtr *sheng_qrtr_get(void);
void sheng_qrtr_pd_query(const char *path);
void sheng_qrtr_lookup(uint32_t service);
void sheng_qrtr_dump_remote(void);
typedef void (*ShengQrtrDiag)(uint32_t our_port, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length);
int sheng_qrtr_diag_publish(ShengQrtrDiag cntl, ShengQrtrDiag data);
int sheng_qrtr_send(uint32_t from_port, uint32_t node, uint32_t port, const void *data, uint32_t length);
void fuxi_sensor_probe(void);

volatile uint32_t fuxi_sensor_seq = 0;
volatile int32_t fuxi_sensor_accel[3] = {0, -1000, 0};
volatile int32_t fuxi_sensor_gyro[3] = {0, 0, 0};
volatile int32_t fuxi_sensor_mag[3] = {0, 0, 0};
volatile uint32_t fuxi_sensor_als = 150;
volatile uint32_t fuxi_sensor_prox = 0;
}

namespace {

constexpr uint32_t kSnsClientSvc = 0x190u;
constexpr uint32_t kFastrpcHandleInit = 1u;
constexpr uint32_t kFastrpcScInitAttachSns = (8u << 24) | (1u << 16);  // method 8, in 1, out 0
constexpr uint32_t kFastrpcScInitAttach = (0u << 24) | (1u << 16);     // method 0, in 1, out 0
constexpr uint64_t kSmmuPhys = 0x15000000ULL;
constexpr uint64_t kSmmuSize = 0x00100000ULL;
constexpr uint64_t kSensorLowPa = 0xd0100000ULL;
constexpr uint64_t kSensorSid = 5ULL;  // compute_cb12 (shared-cb=8): 0x1005 & 0xf == 5
static const uint16_t kSnsStreams[] = {0x1005u, 0x1085u, 0x1065u};

ShengAdspChannel *gFastrpcCh = nullptr;
volatile int32_t gFastrpcUp = 0;
volatile uint32_t gFastrpcRxCount = 0;

struct __attribute__((packed)) FastrpcMsg64 {
    uint32_t pid;
    uint32_t tid;
    uint64_t ctx;
    uint32_t handle;
    uint32_t sc;
    uint64_t addr;
    uint64_t size;
};

struct __attribute__((packed)) FastrpcRspV2 {
    uint64_t ctx;
    int32_t retval;
    uint32_t flags;
    uint32_t version;
};

struct __attribute__((packed)) FastrpcRemoteBuf {
    uint64_t pv;
    uint64_t len;
};

struct __attribute__((packed)) FastrpcPhyPage {
    uint64_t addr;
    uint64_t size;
};

static uint32_t smmuRd(void *ctx, unsigned o) {
    return *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(ctx) + o);
}
static void smmuWr(void *ctx, unsigned o, uint32_t v) {
    *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(ctx) + o) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

bool setupSensorSmmu() {
    IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(kSmmuPhys, kSmmuSize, kIODirectionInOut);
    IOMemoryMap *map = md ? md->map(kIOMapInhibitCache) : nullptr;
    if (!map) return false;
    void *va = reinterpret_cast<void *>(map->getVirtualAddress());
    struct smmu_io io = {va, smmuRd, smmuWr};
    uint32_t mask = 0;
    for (unsigned i = 0; i < sizeof(kSnsStreams) / sizeof(kSnsStreams[0]); ++i) {
        struct smmu_plan plan = smmu_plan_bypass(&io, kSnsStreams[i], kSensorLowPa, 4096);
        const char *e = plan.error ? plan.error : smmu_configure_bypass(&io, &plan, kSnsStreams[i]);
        if (!e) mask |= (1u << i);
    }
    IOLog("fuxi sensor smmu: bypass mask=%#x\n", mask);
    return (mask & 1u) != 0;
}

void fastrpcState(void *, int up) {
    gFastrpcUp = up ? 1 : 0;
    IOLog("fuxi sensor fastrpc: state=%d\n", up);
}

void fastrpcReceive(void *, const uint8_t *data, uint32_t length) {
    uint32_t cnt = gFastrpcRxCount + 1;
    gFastrpcRxCount = cnt;
    if (cnt > 25) return;

    char hex[128] = {};
    unsigned at = 0;
    uint32_t show = length < 40 ? length : 40;
    for (uint32_t i = 0; i < show && at + 3 < sizeof(hex); ++i) {
        hex[at++] = "0123456789abcdef"[data[i] >> 4];
        hex[at++] = "0123456789abcdef"[data[i] & 0xf];
        if (i + 1 < show) hex[at++] = ' ';
    }
    hex[at] = 0;

    if (length >= sizeof(FastrpcRspV2)) {
        FastrpcRspV2 r = {};
        memcpy(&r, data, sizeof(r));
        IOLog("fuxi sensor fastrpc: rx #%u len=%u ctx=%#llx ret=%#x flags=%#x ver=%u [%s]\n",
              cnt, length, (unsigned long long)r.ctx, (unsigned)r.retval, r.flags, r.version, hex);
    } else {
        IOLog("fuxi sensor fastrpc: rx #%u len=%u [%s]\n", cnt, length, hex);
    }
}


// ---- S10c-3 (2026-11-24): read-only apps_std file bridge for sensors_pd ----
// sensors_pd reads its registry config through reverse FastRPC (apps_std). The files come from the stock fuxi images (vendor
// etc/sensors, persist sensors/, and the soc0 sysfs values sns_reg_config names) and the GPU addon writes them below
// /private/var/ngpu/sensors/{vendor,persist,soc0}. sensors/index.txt lists every file and directory (one path per line,
// directories end in '/') for opendir/readdir. Writes (the registry output) are accepted and dropped.
constexpr uint32_t kBridgeFiles = 24, kBridgeMax = 65536;
struct BridgeFile {bool used, write; uint32_t size, pos; uint8_t *data; int ov;};
// S10c-9 (2026-11-24): the registry is regenerated through temp.json + frename. Writes land in a RAM overlay (lost at reboot,
// like the stock registry would be rebuilt), renames move overlay entries, removes leave a tombstone, and reads, stat and readdir
// see the overlay before the addon files. Before this the renamed files never existed and the final registry scan found only the
// old ones (S10c-8M: sensor_pd UP, 0x190 never published).
constexpr uint32_t kOverlays = 512;
struct Overlay {bool used, dead; uint32_t size, cap; uint8_t *data; char path[160];};
Overlay *gOv;
BridgeFile *gFiles;      // heap: the kernel __DATA has no room to spare (U50)
struct BridgeDir {bool used; char path[160]; uint32_t cursor;};
BridgeDir *gDirs;
char *gRecent; uint32_t gRecentAt;   // 32 x 96: the last request summaries
// fuxi 2026-11-27: the first requests (the config reads) come before the USB log attaches, so failures and opens for reading
// are kept here (64 x 96) and printed when the PD goes quiet.
char *gEarly; uint32_t gEarlyAt;
bool inRegistry(const char *s) {   // the kernel has no strstr
    for (; s && *s; ++s) if (!strncmp(s, "/registry/", 10)) return true;
    return false;
}
uint8_t *gIndex; uint32_t gIndexLen;
const char *const kRoot = "/private/var/ngpu/sensors";

// /vendor/etc/sensors/x -> <root>/vendor/x and so on; false for anything the bridge does not serve
bool bridgeMap(const char *in, char *out, size_t outLen) {
    static const struct {const char *prefix; const char *dir;} kMap[] = {
        {"/vendor/etc/sensors", "vendor"}, {"/odm/etc/sensors", "odm"}, {"/mnt/vendor/persist/sensors", "persist"}, {"/persist/sensors", "persist"},
        {"/sys/devices/soc0", "soc0"}};
    for (const auto &m : kMap) {
        size_t n = strlen(m.prefix);
        if (!strncmp(in, m.prefix, n) && (in[n] == 0 || in[n] == '/')) {
            snprintf(out, outLen, "%s/%s%s", kRoot, m.dir, in + n);
            size_t l = strlen(out);
            while (l > 1 && out[l - 1] == '/') out[--l] = 0;
            return true;
        }
    }
    return false;
}
// whole file into a kalloc'd buffer; 0 or an errno
int bridgeRead(const char *path, uint8_t **data, uint32_t *size) {
    vfs_context_t ctx = vfs_context_create(nullptr);
    if (!ctx) return ENOMEM;
    vnode_t vp = nullptr;
    int error = vnode_open(path, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx);
    if (!error) {
        if (vnode_isdir(vp)) error = EISDIR;
        uint8_t *buf = error ? nullptr : static_cast<uint8_t *>(IOMalloc(kBridgeMax));
        if (!error && !buf) error = ENOMEM;
        if (!error) {
            int resid = 0;
            error = vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(buf), (int)kBridgeMax, 0, UIO_SYSSPACE, IO_NOAUTH,
                            vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
            if (!error) {*data = buf; *size = kBridgeMax - (uint32_t)resid;} else IOFree(buf, kBridgeMax);
        }
        vnode_close(vp, FREAD, ctx);
    }
    vfs_context_rele(ctx);
    return error;
}
// 1 file, 2 directory, 0 missing
int bridgeKind(const char *path) {
    vfs_context_t ctx = vfs_context_create(nullptr);
    if (!ctx) return 0;
    vnode_t vp = nullptr;
    int kind = 0;
    if (!vnode_lookup(path, 0, &vp, ctx)) {kind = vnode_isdir(vp) ? 2 : 1; vnode_put(vp);}
    vfs_context_rele(ctx);
    return kind;
}

int ovFind(const char *host) {
    for (uint32_t i = 0; i < kOverlays; ++i) if (gOv[i].used && !strcmp(gOv[i].path, host)) return (int)i;
    return -1;
}
int ovCreate(const char *host) {        // an empty, live entry for host (reusing any old one)
    int i = ovFind(host);
    if (i < 0) {for (uint32_t k = 0; k < kOverlays; ++k) if (!gOv[k].used) {i = (int)k; break;}}
    if (i < 0) return -1;
    Overlay &o = gOv[i];
    if (!o.used) {o.data = nullptr; o.cap = 0;}
    o.used = true; o.dead = false; o.size = 0;
    strlcpy(o.path, host, sizeof o.path);
    return i;
}
bool ovAppend(int i, const uint8_t *src, uint32_t n) {
    Overlay &o = gOv[i];
    if (o.size + n > kBridgeMax) return false;
    if (o.size + n > o.cap) {
        uint32_t cap = o.cap ? o.cap : 1024; while (cap < o.size + n) cap *= 2; if (cap > kBridgeMax) cap = kBridgeMax;
        uint8_t *d = static_cast<uint8_t *>(IOMalloc(cap));
        if (!d) return false;
        if (o.data) {memcpy(d, o.data, o.size); IOFree(o.data, o.cap);}
        o.data = d; o.cap = cap;
    }
    memcpy(o.data + o.size, src, n); o.size += n;
    return true;
}
// 1 file, 2 directory, 0 missing: the overlay first, then the addon files
int bridgeKindOv(const char *host, uint32_t *size) {
    int i = ovFind(host);
    if (i >= 0) {if (gOv[i].dead) return 0; if (size) *size = gOv[i].size; return 1;}
    int kind = bridgeKind(host);
    if (kind == 1 && size) {uint8_t *d = nullptr; uint32_t n = 0; if (!bridgeRead(host, &d, &n)) {*size = n; IOFree(d, kBridgeMax);}}
    return kind;
}
void bridgeLoadIndex() {
    if (gIndex) return;
    char p[200]; snprintf(p, sizeof p, "%s/index.txt", kRoot);
    if (bridgeRead(p, &gIndex, &gIndexLen)) {gIndex = nullptr; gIndexLen = 0;}
}
// next index entry that is a direct child of dir (root-relative "vendor", "persist/registry/registry"); name written to out
bool bridgeNextChildIndex(const char *rel, uint32_t *cursor, char *out, size_t outLen);
bool bridgeNextChild(const char *rel, uint32_t *cursor, char *out, size_t outLen) {
    char host[200];
    while (bridgeNextChildIndex(rel, cursor, out, outLen)) {
        snprintf(host, sizeof host, "%s/%s/%s", kRoot, rel, out);
        int i = ovFind(host);
        if (i < 0 || !gOv[i].dead) return true;               // a removed file stays hidden
    }
    // past the index: the overlay's own files in this directory
    for (uint32_t k = *cursor >= gIndexLen + 1 ? *cursor - gIndexLen - 1 : 0; k < kOverlays; ++k) {
        *cursor = gIndexLen + 1 + k + 1;
        const Overlay &o = gOv[k];
        if (!o.used || o.dead) continue;
        const char *rest = o.path + strlen(kRoot) + 1;       // root-relative
        size_t rl = strlen(rel);
        if (strncmp(rest, rel, rl) || rest[rl] != '/' || strchr(rest + rl + 1, '/')) continue;
        if (bridgeKind(o.path) == 1) continue;              // the index listed it already
        strlcpy(out, rest + rl + 1, outLen);
        return true;
    }
    return false;
}
bool bridgeNextChildIndex(const char *rel, uint32_t *cursor, char *out, size_t outLen) {
    bridgeLoadIndex();
    size_t rl = strlen(rel);
    if (*cursor > gIndexLen) return false;
    while (gIndex && *cursor < gIndexLen) {
        uint32_t s = *cursor, e = s;
        while (e < gIndexLen && gIndex[e] != '\n') e++;
        *cursor = e + 1;
        uint32_t len = e - s;
        const char *line = reinterpret_cast<const char *>(gIndex + s);
        if (len <= rl + 1 || strncmp(line, rel, rl) || line[rl] != '/') continue;
        const char *name = line + rl + 1; uint32_t nl = len - (uint32_t)rl - 1;
        if (nl && name[nl - 1] == '/') nl--;
        if (!nl || memchr(name, '/', nl)) continue;           // deeper entry
        if (nl >= outLen) nl = (uint32_t)outLen - 1;
        memcpy(out, name, nl); out[nl] = 0;
        return true;
    }
    *cursor = gIndexLen + 1;
    return false;
}


// ---- S11-diag (2026-11-25): the ADSP's DIAG stream, to see where sensors_pd stops before publishing 0x190 ----
// Protocol after andersson/diag (router/diag_cntl.c): the DSP sends FEATURE_MASK (8) on DIAG_CNTL; we answer with our feature
// mask (APPS_HDLC_ENCODE: the DSP then frames data as 7e 01 len16 payload 7e, no escaping), then MSG/LOG/EVENT masks all
// enabled, DIAG_MODE real time and streaming buffering. Data on DIAG: 0x79 extended messages carry their format string and file
// (readable), 0x99/0x92 QShrink messages only a hash (SSID, line and the arguments are still printed), 0x10 log packets carry a
// log code and payload, 0x60 events. Everything is rate limited for the serial console.
// S11-diag-2: over QRTR (service 4097); the DSP's control client is remembered from its first packet.
uint32_t gDiagOurPort, gDiagNode, gDiagPort; bool gDiagPeer;
struct DiagStats {uint32_t cntl, frames, f3, qsr, logs, events, other, printed, ssidSeen; uint16_t ssid[48]; uint32_t ssidCount[48];};
DiagStats *gDiag;
void diagCntlSend(const void *p, uint32_t n) {if (gDiagPeer) sheng_qrtr_send(gDiagOurPort, gDiagNode, gDiagPort, p, n);}
void diagSendMasks() {
    // MSG masks, one per SSID range, all levels on
    static const uint16_t kFirst[25] = {0, 500, 1000, 2000, 3000, 4000, 4500, 4600, 5000, 5500, 6000, 6500, 7000, 7100, 7200, 8000,
                                        8500, 9000, 9500, 10200, 10251, 10300, 10350, 10400, 0xC000};
    static const uint16_t kLast[25] = {120, 506, 1007, 2008, 3014, 4010, 4573, 4615, 5033, 5516, 6081, 6521, 7003, 7111, 7201, 8000,
                                       8529, 9008, 9510, 10210, 10255, 10300, 10377, 10416, 0xC063};
    for (int i = 0; i < 25; ++i) {
        uint8_t p[8 + 3 + 4 + 4 + 4] = {};
        uint32_t cmd = 11, len = sizeof p - 8, items = 1, mask = 0xffffffffu;
        memcpy(p, &cmd, 4); memcpy(p + 4, &len, 4);
        p[8] = 1; p[9] = 2; p[10] = 0;                       // stream 1, ALL_ENABLED, msg mode 0
        memcpy(p + 11, &kFirst[i], 2); memcpy(p + 13, &kLast[i], 2);
        memcpy(p + 15, &items, 4); memcpy(p + 19, &mask, 4);
        diagCntlSend(p, sizeof p);
    }
    {   // LOG mask: all enabled
        uint8_t p[8 + 3 + 4 + 4] = {};
        uint32_t cmd = 9, len = sizeof p - 8;
        memcpy(p, &cmd, 4); memcpy(p + 4, &len, 4);
        p[8] = 1; p[9] = 2; p[10] = 0;
        diagCntlSend(p, sizeof p);
    }
    {   // EVENT mask: all enabled
        uint8_t p[8 + 3 + 4] = {};
        uint32_t cmd = 10, len = sizeof p - 8;
        memcpy(p, &cmd, 4); memcpy(p + 4, &len, 4);
        p[8] = 1; p[9] = 2; p[10] = 1;
        diagCntlSend(p, sizeof p);
    }
    {   // DIAG_MODE v1: real time
        uint32_t p[2 + 9] = {3, 36, 1, 1, 1, 0, 0, 0, 0, 0, 0};
        diagCntlSend(p, sizeof p);
    }
    {   // BUFFERING_TX_MODE v1: streaming
        uint8_t p[8 + 6] = {};
        uint32_t cmd = 17, len = 6, version = 1;
        memcpy(p, &cmd, 4); memcpy(p + 4, &len, 4); memcpy(p + 8, &version, 4);
        p[12] = 0; p[13] = 0;
        diagCntlSend(p, sizeof p);
    }
}
void diagHex(const char *tag, uint32_t a, uint32_t b, const uint8_t *d, uint32_t n) {
    char hex[3 * 40 + 1]; uint32_t k = 0;
    for (uint32_t i = 0; i < n && i < 40; ++i) {hex[k++] = "0123456789abcdef"[d[i] >> 4]; hex[k++] = "0123456789abcdef"[d[i] & 15]; hex[k++] = ' ';}
    hex[k] = 0;
    IOLog("fuxi diag: %s %u:%u len %u: %s\n", tag, a, b, n, hex);
}
uint32_t gDiagRaw;
void diagCntlReceive(uint32_t ourPort, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length) {
    if (!gDiagPeer) {gDiagOurPort = ourPort; gDiagNode = node; gDiagPort = port; gDiagPeer = true;
        IOLog("fuxi diag: control peer %u:%u on our port %u\n", node, port, ourPort);}
    for (uint32_t at = 0; at + 8 <= length;) {
        uint32_t cmd, len;
        memcpy(&cmd, data + at, 4); memcpy(&len, data + at + 4, 4);
        if (at + 8 + len > length) break;
        if (gDiag) gDiag->cntl++;
        if (cmd == 8 && len >= 5) {
            // payload: mask_len (u32) then mask_len bytes of mask (the ADSP sends 3: len 7)
            uint32_t mlen = 0, mask = 0; memcpy(&mlen, data + at + 8, 4);
            uint32_t take = mlen < 4 ? mlen : 4; if (take > len - 4) take = len - 4;
            memcpy(&mask, data + at + 12, take);
            IOLog("fuxi diag: DSP feature mask %#x\n", mask);
            // FEATURE_MASK_SUPPORT | MASTER_SETS_COMMON_MASK | APPS_HDLC_ENCODE, as far as the DSP offers them
            const uint32_t ours = mask & ((1u << 0) | (1u << 1) | (1u << 6));
            uint8_t p[8 + 8 + 2] = {};
            uint32_t c = 8, l = sizeof p - 8, ml = 4;
            memcpy(p, &c, 4); memcpy(p + 4, &l, 4); memcpy(p + 8, &ml, 4); memcpy(p + 12, &ours, 4);
            diagCntlSend(p, sizeof p);
            diagSendMasks();
            IOLog("fuxi diag: answered features %#x and sent the masks\n", ours);
        } else if (gDiag && gDiag->cntl < 40) {
            diagHex("cntl", cmd, len, data + at + 8, len);
        }
        at += 8 + len;
    }
}
void diagCountSsid(uint16_t ssid) {
    DiagStats &d = *gDiag;
    for (uint32_t i = 0; i < d.ssidSeen; ++i) if (d.ssid[i] == ssid) {d.ssidCount[i]++; return;}
    if (d.ssidSeen < 48) {d.ssid[d.ssidSeen] = ssid; d.ssidCount[d.ssidSeen++] = 1;}
}
void diagPacket(const uint8_t *p, uint32_t n) {
    DiagStats &d = *gDiag;
    if (!n) return;
    const uint8_t cmd = p[0];
    const bool print = d.printed < 700;
    if (cmd == 0x79 && n >= 20) {                          // extended message: hdr(4) ts(8) line(2) ssid(2) mask(4) args fmt file
        d.f3++;
        const uint8_t nargs = p[2];
        uint16_t line, ssid; memcpy(&line, p + 12, 2); memcpy(&ssid, p + 14, 2);
        diagCountSsid(ssid);
        uint32_t at = 20 + 4u * nargs;
        if (print && at < n) {
            char fmt[160]; uint32_t k = 0;
            while (at < n && p[at] && k + 1 < sizeof fmt) fmt[k++] = (char)p[at++];
            fmt[k] = 0; at++;
            char file[48]; uint32_t f = 0;
            while (at < n && p[at] && f + 1 < sizeof file) file[f++] = (char)p[at++];
            file[f] = 0;
            uint32_t a[4] = {}; for (uint32_t i = 0; i < nargs && i < 4; ++i) memcpy(&a[i], p + 20 + 4 * i, 4);
            IOLog("fuxi diag: [%u] %s:%u \"%s\" %x %x %x %x\n", ssid, file, line, fmt, a[0], a[1], a[2], a[3]);
            d.printed++;
        }
    } else if ((cmd == 0x99 || cmd == 0x92) && n >= 24) {  // QShrink: hdr(4) ts(8) line(2) ssid(2) mask(4) hash(4) args
        d.qsr++;
        const uint8_t nargs = p[2];
        uint16_t line, ssid; uint32_t hash; memcpy(&line, p + 12, 2); memcpy(&ssid, p + 14, 2); memcpy(&hash, p + 20, 4);
        diagCountSsid(ssid);
        if (print) {
            uint32_t a[4] = {}; for (uint32_t i = 0; i < nargs && i < 4 && 24 + 4 * i + 4 <= n; ++i) memcpy(&a[i], p + 24 + 4 * i, 4);
            IOLog("fuxi diag: q[%u] line %u hash %08x args %u: %x %x %x %x\n", ssid, line, hash, nargs, a[0], a[1], a[2], a[3]);
            d.printed++;
        }
    } else if (cmd == 0x10 && n >= 16) {                   // log packet: cmd more len len code ts payload
        d.logs++;
        uint16_t code; memcpy(&code, p + 6, 2);
        if (print) {
            char hex[100]; uint32_t k = 0;
            for (uint32_t i = 16; i < n && i < 48 && k + 3 < sizeof hex; ++i) {hex[k++] = "0123456789abcdef"[p[i] >> 4]; hex[k++] = "0123456789abcdef"[p[i] & 15]; hex[k++] = ' ';}
            hex[k] = 0;
            IOLog("fuxi diag: log %#06x len %u: %s\n", code, n, hex);
            d.printed++;
        }
    } else if (cmd == 0x60) {
        d.events++;
    } else {
        d.other++;
        if (print && d.other < 20) {IOLog("fuxi diag: packet cmd %#x len %u\n", cmd, n); d.printed++;}
    }
}
void diagDataReceive(uint32_t ourPort, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length) {
    if (!gDiag) return;
    if (gDiagRaw++ < 60) {IOLog("fuxi diag: data rx on our port %u from %u:%u\n", ourPort, node, port); diagHex("raw", node, port, data, length);}
    // non-HDLC frames: 7e 01 len16 payload 7e (several per message)
    for (uint32_t at = 0; at + 4 <= length;) {
        if (data[at] != 0x7e || data[at + 1] != 0x01) {at++; continue;}
        uint16_t len; memcpy(&len, data + at + 2, 2);
        if (at + 4 + len + 1 > length) break;
        gDiag->frames++;
        diagPacket(data + at + 4, len);
        at += 4 + len + 1;
    }
}
void diagStart() {
    gDiag = static_cast<DiagStats *>(IOMalloc(sizeof(DiagStats)));
    if (!gDiag) return;
    memset(gDiag, 0, sizeof(DiagStats));
    sheng_qrtr_diag_publish(diagCntlReceive, diagDataReceive);
}
// fuxi 2026-11-27: the masks went out when the root PD's diag client spoke (before sensorspd existed). The sensor PD's own
// diag client registers later, so the feature answer and the masks go out again at the points the sensor PD is known alive.
void diagResend(const char *why) {
    if (!gDiagPeer) {IOLog("fuxi diag: no control peer yet (%s)\n", why); return;}
    uint8_t p[8 + 8 + 2] = {};
    uint32_t c = 8, l = sizeof p - 8, ml = 4, ours = (1u << 0) | (1u << 1) | (1u << 6);
    memcpy(p, &c, 4); memcpy(p + 4, &l, 4); memcpy(p + 8, &ml, 4); memcpy(p + 12, &ours, 4);
    diagCntlSend(p, sizeof p);
    diagSendMasks();
    // The DSP publishes its own DIAG servers (4097) and streams only to a client it has heard from; the poke in
    // sheng_qrtr_dump_remote() used to run at the 30 s summary, after the USB log had already dropped.
    sheng_qrtr_dump_remote();
    IOLog("fuxi diag: features %#x and masks sent again to %u:%u (%s); cntl %u frames %u so far\n", ours, gDiagNode, gDiagPort, why,
          gDiag ? gDiag->cntl : 0u, gDiag ? gDiag->frames : 0u);
}
void diagSummary() {
    if (!gDiag) return;
    sheng_qrtr_dump_remote();
    DiagStats &d = *gDiag;
    IOLog("fuxi diag: summary cntl %u frames %u f3 %u qsr %u logs %u events %u other %u printed %u\n", d.cntl, d.frames, d.f3, d.qsr,
          d.logs, d.events, d.other, d.printed);
    // fuxi 2026-11-27: the sensor PD finishes its registry and goes quiet without publishing SNS (0x190), and 40 control frames
    // brought no F3 message: either the feature mask never came, or it came before the masks could be heard. While nothing
    // arrives, the peer, the feature answer and the masks are sent again at every summary (all features the DSP may use).
    if (gDiagPeer && !d.frames && !d.f3) {
        uint8_t p[8 + 8 + 2] = {};
        uint32_t c = 8, l = sizeof p - 8, ml = 4, ours = (1u << 0) | (1u << 1) | (1u << 6);
        memcpy(p, &c, 4); memcpy(p + 4, &l, 4); memcpy(p + 8, &ml, 4); memcpy(p + 12, &ours, 4);
        diagCntlSend(p, sizeof p);
        diagSendMasks();
        IOLog("fuxi diag: no frames yet from %u:%u, features %#x and masks sent again\n", gDiagNode, gDiagPort, ours);
    } else if (!gDiagPeer) {
        IOLog("fuxi diag: no control peer yet\n");
    }
    char line[300]; uint32_t k = 0;
    for (uint32_t i = 0; i < d.ssidSeen && k + 16 < sizeof line; ++i) k += snprintf(line + k, sizeof line - k, " %u:%u", d.ssid[i], d.ssidCount[i]);
    line[k] = 0;
    IOLog("fuxi diag: ssids%s\n", line);
}

// S11e (2026-11-25): stock fuxi dtbo fragment@29 forces pm_humu_l9 (RPMh "ldob9") always on at 3.2 V; nothing of ours votes it.
// The sensors (on the DSP's SSC buses) are the likely user. Voted like AudioProbe's micRail: active TCS 2 of the apps RSC.
bool sensorRails() {
    constexpr size_t kDbSize = 0x20000;
    uint8_t *db = static_cast<uint8_t *>(IOMallocData(kDbSize));
    if (!db) return false;
    bool ok = false;
    IOMemoryDescriptor *dmd = IOMemoryDescriptor::withPhysicalAddress(0x81c60000ULL, kDbSize, kIODirectionIn);
    IOMemoryMap *dmap = dmd ? dmd->map(kIOMapInhibitCache) : nullptr;
    IOMemoryDescriptor *rmd = IOMemoryDescriptor::withPhysicalAddress(0x17a20000ULL, 0x10000, kIODirectionInOut);
    IOMemoryMap *rmap = rmd ? rmd->map(kIOMapInhibitCache) : nullptr;
    if (dmap && rmap) {
        volatile uint32_t *dv = reinterpret_cast<volatile uint32_t *>(dmap->getVirtualAddress());
        for (size_t i = 0; i < kDbSize / 4; ++i) {const uint32_t w = dv[i]; memcpy(db + 4 * i, &w, 4);}
        volatile uint8_t *rv = reinterpret_cast<volatile uint8_t *>(rmap->getVirtualAddress());
        auto wr = [&](uint32_t off, uint32_t v) {*reinterpret_cast<volatile uint32_t *>(rv + off) = v;};
        const QcomRscLayout layout = qcom_rsc_layout(*reinterpret_cast<volatile uint32_t *>(rv));
        // TCS 2 is shared with the touch, audio and modem bring-ups: wait for it to be idle (status non-zero), as FuxiTouchProbe does.
        const uint32_t tcsBase = 0xd00 + layout.tcs_stride * 2;
        auto idle = [&]() {
            for (unsigned i = 0; i < 2000; ++i) {if (*reinterpret_cast<volatile uint32_t *>(rv + tcsBase + layout.status)) return true; IODelay(10);}
            return false;
        };
        auto send = [&](uint32_t address, uint32_t value) {
            if (!idle()) {IOLog("fuxi sensor: TCS 2 busy, ldob9 command %#x skipped\n", address); return;}
            QcomRpmhWrite w[16];
            const unsigned n = qcom_rpmh_active_request(&layout, 2, &address, &value, 1, w, 16);
            for (unsigned i = 0; i < n; ++i) wr(0xd00 + w[i].offset, w[i].value);
            IOSleep(2);
            const unsigned m = qcom_rpmh_finish(&layout, 2, w, 16);
            for (unsigned i = 0; i < m; ++i) wr(0xd00 + w[i].offset, w[i].value);
        };
        QcomRpmhResource res;
        if (!qcom_cmd_db_find(db, kDbSize, "ldob9", &res)) {
            send(res.address + 8, 7);       // high power mode
            send(res.address, 3200);        // mV
            send(res.address + 4, 1);       // enable
            ok = true;
            IOLog("fuxi sensor: voted ldob9 (pm_humu_l9) 3.2 V at %#x\n", res.address);
        } else IOLog("fuxi sensor: ldob9 not in cmd-db\n");
    }
    if (rmap) rmap->release(); if (rmd) rmd->release();
    if (dmap) dmap->release(); if (dmd) dmd->release();
    IOFreeData(db, kDbSize);
    return ok;
}

void sensorWorker(void *, wait_result_t) {
    IOSleep(15000);   // after the touch has powered its rails through the same TCS
    sensorRails();
    static const ShengAdspClient kFastrpcClient = {fastrpcState, fastrpcReceive};
    for (int i = 0; i < 40 && !gFastrpcCh; ++i) {
        gFastrpcCh = sheng_adsp_channel("fastrpcglink-apps-dsp", 2048, 4, 0, &kFastrpcClient, nullptr);
        if (!gFastrpcCh) IOSleep(100);
    }
    if (!gFastrpcCh) return;
    for (int i = 0; i < 60 && !gFastrpcUp; ++i) IOSleep(100);
    if (!gFastrpcUp) return;

    diagStart();
    IOSleep(1000);
    if (!setupSensorSmmu()) return;

    IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(kSensorLowPa, 32768, kIODirectionInOut);
    IOMemoryMap *map = md ? md->map(kIOMapWriteCombineCache) : nullptr;
    if (!map) return;
    uint8_t *va = reinterpret_cast<uint8_t *>(map->getVirtualAddress());
    memset(va, 0, 32768);

    // Upstream fastrpc_get_args layout: remote_arg[n] | invoke_buf[n] | phy_page[n] | payload
    uint64_t dspBase = kSensorLowPa | (kSensorSid << 32);
    uint64_t *rpra = reinterpret_cast<uint64_t *>(va);
    rpra[0] = dspBase + 128ULL;  // pv (page offset 128)
    rpra[1] = 4ULL;              // len
    uint32_t *list = reinterpret_cast<uint32_t *>(va + 16);
    list[0] = 1u;                // num
    list[1] = 0u;                // pgidx
    uint64_t *pages = reinterpret_cast<uint64_t *>(va + 24);
    pages[0] = dspBase;          // page-aligned addr
    pages[1] = 4096ULL;
    uint32_t client_id = 1001u;
    memcpy(va + 128, &client_id, 4);
    __asm__ volatile("dsb sy" ::: "memory");

    // 0. S11f (2026-11-25): stock sscrpcd runs "sscrpcd sensorspd" -> libssc_default_listener -> remote_handle_open(
    //    "createstaticpd:sensorspd"), which the kernel sends as INIT_CREATE_STATIC (method 8, in 3): {pgid, namelen, pageslen},
    //    the name, the pages (none here). We only ever attached; send the create first, then the attach as before.
    FastrpcMsg64 msg = {};
    msg.pid = client_id;
    msg.tid = client_id;
    {
        uint8_t *cs = va + 8192;                 // third 4 KiB page, unused until the next2 buffers
        const uint64_t csBase = dspBase + 8192ULL;
        memset(cs, 0, 4096);
        static const char kName[] = "sensorspd";
        uint64_t *ra = reinterpret_cast<uint64_t *>(cs);
        ra[0] = csBase + 128ULL; ra[1] = 12ULL;              // in[0]: {pgid, namelen, pageslen}
        ra[2] = csBase + 160ULL; ra[3] = sizeof kName;       // in[1]: name
        ra[4] = csBase + 192ULL; ra[5] = 16ULL;              // in[2]: pages (empty)
        uint32_t *lb = reinterpret_cast<uint32_t *>(cs + 48);
        lb[0] = 1u; lb[1] = 0u; lb[2] = 1u; lb[3] = 0u; lb[4] = 1u; lb[5] = 0u;
        uint64_t *pp = reinterpret_cast<uint64_t *>(cs + 72);
        pp[0] = csBase; pp[1] = 4096ULL;
        const uint32_t cin[3] = {client_id, (uint32_t)sizeof kName, 0u};
        memcpy(cs + 128, cin, sizeof cin);
        memcpy(cs + 160, kName, sizeof kName);
        __asm__ volatile("dsb sy" ::: "memory");
        msg.ctx = 0x2a2ULL;                       // own context id, pd nibble 2
        msg.handle = kFastrpcHandleInit;
        msg.sc = (8u << 24) | (3u << 16);         // INIT_CREATE_STATIC, in 3, out 0
        msg.addr = csBase;
        msg.size = 4096ULL;
        int crc = sheng_adsp_send(gFastrpcCh, reinterpret_cast<const uint8_t *>(&msg), sizeof(msg));
        IOLog("fuxi sensor fastrpc: sent INIT_CREATE_STATIC(sensorspd) ctx=0x2a2 rc=%d\n", crc);
        IOSleep(500);
    }

    // 1. Attach to sensors_pd (verified ret=0 in S7)
    msg.ctx = 0x12ULL;  // low nibble 2 = SENSORS_PD
    msg.handle = kFastrpcHandleInit;
    msg.sc = kFastrpcScInitAttach;
    msg.addr = dspBase;
    msg.size = 4096ULL;
    int rc = sheng_adsp_send(gFastrpcCh, reinterpret_cast<const uint8_t *>(&msg), sizeof(msg));
    IOLog("fuxi sensor fastrpc: sent ATTACH_SENSORS_PD ctx=0x12 rc=%d\n", rc);
    diagResend("after ATTACH_SENSORS_PD");
    IOSleep(300);

    // 2. Register adsp_default_listener on handle=3 (adsp_listener_init2: method 3, in 0, out 0)
    msg.ctx = 0x32ULL;
    msg.handle = 3u;
    msg.sc = (3u << 24);  // adsp_listener_init2
    msg.addr = 0;
    msg.size = 0;
    rc = sheng_adsp_send(gFastrpcCh, reinterpret_cast<const uint8_t *>(&msg), sizeof(msg));
    IOLog("fuxi sensor fastrpc: sent LISTENER_INIT2 h=3 sc=%#x rc=%d\n", msg.sc, rc);
    IOSleep(300);

    // 3. Call adsp_listener_next2 (method 4, in 2, out 2 -> sc=0x04020200)
    // QAIC ABI:
    //   in[0]  (16B): { prevCtx(u32), prevResult(i32), prevbufsLen(i32), bufsLen(i32) }
    //   in[1]  (0B):  prevbufs
    //   out[0] (16B): { ctx(u32), handle(u32), sc(u32), bufsLenReq(i32) }
    //   out[1] (2048B): bufs
    // Layout in 4 KiB page: remote_arg[4] (64B) | invoke_buf[4] (32B) | phy_page[1] (16B) | buffers
    memset(va, 0, 4096);
    rpra[0] = dspBase + 128ULL; rpra[1] = 16ULL;    // in[0]: 16B scalar struct
    rpra[2] = 0ULL;             rpra[3] = 0ULL;     // in[1]: prevbufs (empty)
    rpra[4] = dspBase + 192ULL; rpra[5] = 16ULL;    // out[0]: 16B scalar struct
    rpra[6] = dspBase + 0x2000ULL; rpra[7] = 8192ULL;  // out[1]: bufs (8 KiB)
    uint32_t *l4 = reinterpret_cast<uint32_t *>(va + 64);
    l4[0] = 1u; l4[1] = 0u;  // in[0]
    l4[2] = 0u; l4[3] = 0u;  // in[1] empty
    l4[4] = 1u; l4[5] = 0u;  // out[0]
    l4[6] = 1u; l4[7] = 0u;  // out[1]
    uint64_t *p1 = reinterpret_cast<uint64_t *>(va + 96);
    p1[0] = dspBase;
    p1[1] = 32768ULL;
    uint32_t in0[4] = {0u, 0u, 0u, 8192u};
    memcpy(va + 128, in0, sizeof(in0));
    __asm__ volatile("dsb sy" ::: "memory");

    msg.ctx = 0x52ULL;
    msg.handle = 3u;
    msg.sc = (4u << 24) | (2u << 16) | (2u << 8);  // 0x04020200 (next2 in=2, out=2)
    msg.addr = dspBase;
    msg.size = 32768ULL;
    rc = sheng_adsp_send(gFastrpcCh, reinterpret_cast<const uint8_t *>(&msg), sizeof(msg));
    IOLog("fuxi sensor fastrpc: sent LISTENER_NEXT2 h=3 sc=%#x rc=%d\n", msg.sc, rc);
    IOSleep(200);

    // 4. Open "adsp_default_listener" using second 4 KiB page (dspBase + 4096)
    //    remotectl_open: handle=0, method=0, in=2, out=2 -> sc=0x00020200
    //    in[0] (8B): { nameLen, dlerrLen }, in[1]: name, out[0] (8B): { handle, dlerrLenReq }, out[1]: dlerr
    uint8_t *va2 = va + 4096;
    uint64_t dspBase2 = dspBase + 4096ULL;
    memset(va2, 0, 4096);
    uint64_t *rpra2 = reinterpret_cast<uint64_t *>(va2);
    const char kDefUri[] = "adsp_default_listener";
    uint32_t uriLen = static_cast<uint32_t>(sizeof(kDefUri));  // includes NUL
    uint32_t dlerrLen = 256u;
    rpra2[0] = dspBase2 + 128ULL; rpra2[1] = 8ULL;             // in[0]: { nameLen, dlerrLen }
    rpra2[2] = dspBase2 + 160ULL; rpra2[3] = uriLen;           // in[1]: name
    rpra2[4] = dspBase2 + 256ULL; rpra2[5] = 8ULL;             // out[0]: { handle, dlerrLenReq }
    rpra2[6] = dspBase2 + 320ULL; rpra2[7] = dlerrLen;         // out[1]: dlerr
    uint32_t *l4b = reinterpret_cast<uint32_t *>(va2 + 64);
    l4b[0] = 1u; l4b[1] = 0u;
    l4b[2] = 1u; l4b[3] = 0u;
    l4b[4] = 1u; l4b[5] = 0u;
    l4b[6] = 1u; l4b[7] = 0u;
    uint64_t *p2 = reinterpret_cast<uint64_t *>(va2 + 96);
    p2[0] = dspBase2;
    p2[1] = 4096ULL;
    uint32_t openIn0[2] = {uriLen, dlerrLen};
    memcpy(va2 + 128, openIn0, sizeof(openIn0));
    memcpy(va2 + 160, kDefUri, uriLen);
    __asm__ volatile("dsb sy" ::: "memory");

    msg.tid = client_id + 1u;  // separate DSP worker thread (tid=1001 is blocked in LISTENER_NEXT2)
    msg.ctx = 0x72ULL;
    msg.handle = 0u;
    msg.sc = (0u << 24) | (2u << 16) | (2u << 8);  // 0x00020200 (remotectl_open)
    msg.addr = dspBase2;
    msg.size = 4096ULL;
    rc = sheng_adsp_send(gFastrpcCh, reinterpret_cast<const uint8_t *>(&msg), sizeof(msg));
    IOLog("fuxi sensor fastrpc: sent OPEN(adsp_default_listener) ctx=0x72 rc=%d\n", rc);
    IOSleep(400);
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t openOut0[2] = {};
    memcpy(openOut0, va2 + 256, sizeof(openOut0));
    uint32_t defHandle = openOut0[0];
    va2[320 + 63] = '\0';
    IOLog("fuxi sensor fastrpc: OPEN(adsp_default_listener) handle=%#x dlerrLen=%u dlerr=\"%s\"\n",
          defHandle, openOut0[1], reinterpret_cast<const char *>(va2 + 320));

    if (defHandle != 0) {
        // 5. Call adsp_default_listener_register() (method 0, in 0, out 0 -> sc=0)
        msg.ctx = 0x92ULL;
        msg.handle = defHandle;
        msg.sc = 0u;
        msg.addr = 0;
        msg.size = 0;
        rc = sheng_adsp_send(gFastrpcCh, reinterpret_cast<const uint8_t *>(&msg), sizeof(msg));
        IOLog("fuxi sensor fastrpc: sent DEFAULT_LISTENER_REGISTER h=%#x ctx=0x92 rc=%d\n", defHandle, rc);
    }

    // Reverse-RPC loop on LISTENER_NEXT2 (ctx=0x52+0x100*k): decode, serve apps_std, reply through prevbufs.
    gFiles = static_cast<BridgeFile *>(IOMalloc(sizeof(BridgeFile) * kBridgeFiles));
    gDirs = static_cast<BridgeDir *>(IOMalloc(sizeof(BridgeDir) * 8));
    gRecent = static_cast<char *>(IOMalloc(32 * 96));
    gEarly = static_cast<char *>(IOMallocZero(64 * 96));
    gOv = static_cast<Overlay *>(IOMalloc(sizeof(Overlay) * kOverlays));
    if (!gFiles || !gDirs || !gRecent || !gOv) return;
    memset(gOv, 0, sizeof(Overlay) * kOverlays);
    memset(gRecent, 0, 32 * 96);
    memset(gFiles, 0, sizeof(BridgeFile) * kBridgeFiles); memset(gDirs, 0, sizeof(BridgeDir) * 8);
    uint32_t lastRx = gFastrpcRxCount;
    uint32_t nextHandle = 0x101u;
    uint8_t *req = va + 0x2000;
    uint8_t *pbuf = va + 0x4000;
    for (int iter = 0; ; ++iter) {
        if (iter == 20 || iter == 200 || iter == 2000) diagResend(iter == 20 ? "request 20" : iter == 200 ? "request 200" : "request 2000");
        bool gotNext = false;
        for (int w = 0; w < 1500; ++w) {     // up to 30 s: sensors_pd also waits on its own timers
            if (gFastrpcRxCount > lastRx) {gotNext = true; lastRx = gFastrpcRxCount; break;}
            IOSleep(20);
        }
        if (!gotNext) {
            QcomQrtr *qq = sheng_qrtr_get();
            IOLog("fuxi sensor rev: quiet 30 s after %d requests, sns190=%d; the last ones:\n", iter, qq ? qcom_qrtr_find_remote(qq, kSnsClientSvc) : -2);
            for (uint32_t k = 0; k < 32; ++k) {const char *r = gRecent + ((gRecentAt + k) % 32) * 96; if (r[0]) IOLog("fuxi sensor   %s\n", r);}
            static bool early;
            if (gEarly && !early) {early = true; IOLog("fuxi sensor early: %u kept (failures and reads)\n", gEarlyAt);
                for (uint32_t k = 0; k < gEarlyAt && k < 64; ++k) IOLog("fuxi sensor early %s\n", gEarly + k * 96);}
            memset(gRecent, 0, 32 * 96);
            diagSummary();
            static int looked;
            if (looked++ < 3) sheng_qrtr_lookup(kSnsClientSvc);   // 0x190 may exist without having been broadcast to us
            sheng_qrtr_pd_query("msm/adsp/sensor_pd");
            sheng_qrtr_pd_query("msm/adsp/root_pd");
            --iter; continue;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        uint32_t o0[4] = {};
        memcpy(o0, va + 192, sizeof(o0));
        const uint32_t rctx = o0[0], rh = o0[1], rsc = o0[2], rreq = o0[3] < 8192u ? o0[3] : 8192u;
        const uint32_t nIn = (rsc >> 16) & 0xffu, nOut = (rsc >> 8) & 0xffu, method = (rsc >> 24) & 0x1fu;
        const uint8_t *inPtr[8] = {}; uint32_t inLen[8] = {}, outLen[8] = {};
        uint32_t at = 0; bool parsed = nIn <= 8 && nOut <= 8;
        for (uint32_t b = 0; parsed && b < nIn; ++b) {
            if (at + 4 > rreq) {parsed = false; break;}
            memcpy(&inLen[b], req + at, 4); at += 4;
            if (inLen[b]) {at = (at + 7u) & ~7u; if (at + inLen[b] > rreq) {parsed = false; break;} inPtr[b] = req + at; at += inLen[b];}
        }
        for (uint32_t b = 0; parsed && b < nOut; ++b) {
            if (at + 4 > rreq) {parsed = false; break;}
            memcpy(&outLen[b], req + at, 4); at += 4;
        }
        // reply buffers: out[b] lands at pbuf + outOff[b]
        uint32_t outOff[8] = {}, w0 = 0;
        for (uint32_t b = 0; parsed && b < nOut; ++b) {
            w0 += 4; if (outLen[b]) {w0 = (w0 + 7u) & ~7u; outOff[b] = w0; w0 += outLen[b];}
        }
        if (w0 > 16384u) parsed = false;
        memset(pbuf, 0, w0 < 16384u ? w0 + 8u : 16384u);
        uint32_t prim[8] = {};                                     // in[0]: the scalar inputs
        if (parsed && nIn >= 1 && inPtr[0]) memcpy(prim, inPtr[0], inLen[0] < sizeof prim ? inLen[0] : sizeof prim);
        auto str = [&](uint32_t b) -> const char * {
            return (b < nIn && inPtr[b] && inLen[b] && inPtr[b][inLen[b] - 1] == 0) ? reinterpret_cast<const char *>(inPtr[b]) : "";
        };
        auto put = [&](uint32_t b, const void *src, uint32_t n) {
            if (b < nOut && n <= outLen[b]) memcpy(pbuf + outOff[b], src, n);
        };
        uint32_t prevRes = 0x8000040eu;   // AEE_EBADPARM
        uint32_t mid = method;
        if (parsed && method == 31u) mid = prim[0];
        char host[200];
        if (!parsed) {
            IOLog("fuxi sensor rev[%d]: undecodable request sc=%#x len=%u\n", iter, rsc, rreq);
        } else if (rh == 0u && method == 0u) {                       // remotectl_open
            if (!strcmp(str(1), "apps_std")) {
                uint32_t o[2] = {nextHandle++, 0u}; put(0, o, 8); prevRes = 0u;
            } else prevRes = 2u;
            IOLog("fuxi sensor rev[%d]: OPEN(%s) -> %u\n", iter, str(1), prevRes);
        } else if (rh == 0u && method == 1u) {                       // remotectl_close
            prevRes = 0u;
        } else if (rh >= 0x101u && rh < nextHandle) {
            const char *path = "";
            prevRes = 0u;
            switch (mid) {
            case 0: case 19: {                                         // fopen(name, mode) / fopen_with_env(env, delim, name, mode)
                const char *name = mid == 0 ? str(1) : str(3), *mode = mid == 0 ? str(2) : str(4);
                path = name;
                uint32_t slot = 0; while (slot < kBridgeFiles && gFiles[slot].used) slot++;
                bool write = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
                if (slot == kBridgeFiles) {prevRes = 24u; break;}       // EMFILE
                if (!bridgeMap(name, host, sizeof host)) {prevRes = 2u; break;}
                if (write) {
                    const bool append = strchr(mode, 'a') != nullptr;
                    int prior = ovFind(host);
                    int ov;
                    if (append && prior < 0 && bridgeKind(host) == 1) {      // append to an addon file: start from its bytes
                        uint8_t *d = nullptr; uint32_t n = 0;
                        ov = ovCreate(host);
                        if (ov >= 0 && !bridgeRead(host, &d, &n)) {ovAppend(ov, d, n); IOFree(d, kBridgeMax);}
                    } else if (append && prior >= 0 && !gOv[prior].dead) {
                        ov = prior;
                    } else {
                        ov = ovCreate(host);
                    }
                    if (ov < 0) {prevRes = 28u; break;}                       // ENOSPC
                    gFiles[slot] = {true, true, 0, 0, nullptr, ov};
                } else {
                    int ov = ovFind(host);
                    uint8_t *d = nullptr; uint32_t n = 0;
                    if (ov >= 0) {
                        if (gOv[ov].dead) {prevRes = 2u; break;}
                        d = static_cast<uint8_t *>(IOMalloc(kBridgeMax));
                        if (!d) {prevRes = 12u; break;}
                        n = gOv[ov].size; if (n) memcpy(d, gOv[ov].data, n);
                    } else {
                        int err = bridgeRead(host, &d, &n);
                        if (err) {prevRes = (uint32_t)err; break;}
                    }
                    gFiles[slot] = {true, false, n, 0, d, -1};
                }
                uint32_t h = slot + 1; put(0, &h, 4);
                break;
            }
            case 3: {                                                  // fclose
                uint32_t i = prim[0] - 1;
                if (i < kBridgeFiles && gFiles[i].used) {if (gFiles[i].data) IOFree(gFiles[i].data, kBridgeMax); gFiles[i] = {};}
                break;
            }
            case 2: case 23: case 11: case 14: break;                  // fflush, fsync, rewind (below), clearerr
            case 4: {                                                  // fread(h, buf[len]) -> {read, eof}
                uint32_t i = prim[0] - 1, want = prim[1];
                if (i >= kBridgeFiles || !gFiles[i].used) {prevRes = 9u; break;}
                BridgeFile &bf = gFiles[i];
                uint32_t n = bf.size > bf.pos ? bf.size - bf.pos : 0; if (n > want) n = want; if (n > outLen[1]) n = outLen[1];
                if (n) memcpy(pbuf + outOff[1], bf.data + bf.pos, n);
                bf.pos += n;
                uint32_t o[2] = {n, bf.pos >= bf.size ? 1u : 0u}; put(0, o, 8);
                break;
            }
            case 20: {                                                 // fgets(h, buf[len]) -> {eof}
                uint32_t i = prim[0] - 1, want = prim[1];
                if (i >= kBridgeFiles || !gFiles[i].used) {prevRes = 9u; break;}
                BridgeFile &bf = gFiles[i];
                uint32_t n = 0; if (want > outLen[1]) want = outLen[1];
                while (n + 1 < want && bf.pos < bf.size) {uint8_t c = bf.data[bf.pos++]; pbuf[outOff[1] + n++] = c; if (c == '\n') break;}
                if (want) pbuf[outOff[1] + n] = 0;
                uint32_t eof = (n == 0 && bf.pos >= bf.size) ? 1u : 0u; put(0, &eof, 4);
                break;
            }
            case 5: {                                                  // fwrite(h, buf) -> {written, eof}
                uint32_t i = prim[0] - 1;
                if (i >= kBridgeFiles || !gFiles[i].used || !gFiles[i].write) {prevRes = 9u; break;}
                if (gFiles[i].ov < 0 || !ovAppend(gFiles[i].ov, inPtr[1], inLen[1])) {prevRes = 28u; break;}
                uint32_t o[2] = {inLen[1], 0u}; put(0, o, 8);
                break;
            }
            case 8: case 12: case 13: {                                // ftell / feof / ferror
                uint32_t i = prim[0] - 1;
                if (i >= kBridgeFiles || !gFiles[i].used) {prevRes = 9u; break;}
                uint32_t v = mid == 8 ? gFiles[i].pos : mid == 12 ? (gFiles[i].pos >= gFiles[i].size ? 1u : 0u) : 0u;
                put(0, &v, 4);
                break;
            }
            case 9: {                                                  // fseek(h, offset, whence)
                uint32_t i = prim[0] - 1;
                if (i >= kBridgeFiles || !gFiles[i].used) {prevRes = 9u; break;}
                int64_t base = prim[2] == 0 ? 0 : prim[2] == 1 ? gFiles[i].pos : gFiles[i].size;
                int64_t p = base + (int32_t)prim[1];
                if (p < 0) {prevRes = 22u; break;}
                gFiles[i].pos = (uint32_t)p;
                break;
            }
            case 10: {                                                 // flen -> u64
                uint32_t i = prim[0] - 1;
                if (i >= kBridgeFiles || !gFiles[i].used) {prevRes = 9u; break;}
                uint64_t v = gFiles[i].size; put(0, &v, 8);
                break;
            }
            case 15: path = str(1); IOLog("fuxi sensor dsp: %s\n", path); break;   // print_string
            case 16: prevRes = 2u; break;                              // getenv
            // S10c-7: the registry is regenerated through temp.json + frename; writes are dropped, so the write-side
            // file operations (rename, remove, mkdir, rmdir, truncate) simply succeed (6465 requests stalled on frename).
            case 33: {                                                 // frename(old, new)
                path = str(1);
                char to[200];
                if (!bridgeMap(str(1), host, sizeof host) || !bridgeMap(str(2), to, sizeof to)) {prevRes = 2u; break;}
                int o = ovFind(host);
                if (o < 0 || gOv[o].dead) {
                    if (o >= 0 || bridgeKind(host) != 1) {prevRes = 2u; break;}
                    // an addon file renamed: copy its bytes into the overlay under the new name, tombstone the old one
                    uint8_t *d = nullptr; uint32_t n = 0;
                    if (bridgeRead(host, &d, &n)) {prevRes = 5u; break;}
                    int nn = ovCreate(to);
                    if (nn >= 0) ovAppend(nn, d, n);
                    IOFree(d, kBridgeMax);
                    int t = ovCreate(host); if (t >= 0) gOv[t].dead = true;
                    break;
                }
                int prior = ovFind(to);
                if (prior >= 0 && prior != o) {if (gOv[prior].data) IOFree(gOv[prior].data, gOv[prior].cap); gOv[prior] = {};}
                strlcpy(gOv[o].path, to, sizeof gOv[o].path);
                int t = ovCreate(host); if (t >= 0) gOv[t].dead = true;   // the old name is gone
                break;
            }
            case 24: {                                                 // fremove(path)
                path = str(1);
                if (!bridgeMap(path, host, sizeof host)) {prevRes = 2u; break;}
                if (!bridgeKindOv(host, nullptr)) {prevRes = 2u; break;}
                int t = ovFind(host);
                if (t < 0) t = ovCreate(host);
                if (t >= 0) {gOv[t].dead = true; gOv[t].size = 0;}
                break;
            }
            case 29: case 30: case 32: path = str(1); break;          // mkdir, rmdir, ftrunc: nothing to keep
            case 22: {                                                 // fileExists -> bool
                path = str(1);
                uint8_t ex = bridgeMap(path, host, sizeof host) && bridgeKindOv(host, nullptr) ? 1 : 0; put(0, &ex, 1);
                break;
            }
            case 31: {                                                 // stat(path) -> apps_std_STAT
                path = str(1);
                uint32_t fsize = 0;
                int kind = bridgeMap(path, host, sizeof host) ? bridgeKindOv(host, &fsize) : 0;
                if (!kind) {prevRes = 2u; break;}
                uint8_t st[96] = {};
                uint32_t mode = kind == 2 ? 040755u : 0100644u, nlink = 1;
                uint64_t size = fsize;
                memcpy(st + 24, &mode, 4); memcpy(st + 28, &nlink, 4); memcpy(st + 40, &size, 8);
                put(0, st, 96);
                break;
            }
            case 26: {                                                 // opendir(path) -> DIR{u64}
                path = str(1);
                if (!bridgeMap(path, host, sizeof host) || bridgeKind(host) != 2) {prevRes = 2u; break;}
                uint32_t d = 0; while (d < 8 && gDirs[d].used) d++;
                if (d == 8) {prevRes = 24u; break;}
                gDirs[d].used = true; gDirs[d].cursor = 0;
                strlcpy(gDirs[d].path, host + strlen(kRoot) + 1, sizeof gDirs[d].path);
                uint64_t h = 0x100u + d; put(0, &h, 8);
                break;
            }
            case 27: {                                                 // closedir
                uint32_t d = prim[0] - 0x100u; if (d < 8) gDirs[d] = {};
                break;
            }
            case 28: {                                                 // readdir(DIR) -> {ino, name[255]}, eof
                uint32_t d = prim[0] - 0x100u;
                if (d >= 8 || !gDirs[d].used) {prevRes = 9u; break;}
                uint8_t ent[264] = {};
                int32_t ino = 1; memcpy(ent, &ino, 4);
                if (!bridgeNextChild(gDirs[d].path, &gDirs[d].cursor, reinterpret_cast<char *>(ent + 4), 255)) {
                    uint32_t eof = 1; memcpy(ent + 260, &eof, 4);
                }
                put(0, ent, 264);
                break;
            }
            default:
                prevRes = 2u;
                break;
            }
            static const char *const kNames[37] = {"fopen","freopen","fflush","fclose","fread","fwrite","fgetpos","fsetpos",
                "ftell","fseek","flen","rewind","feof","ferror","clearerr","print_string","getenv","setenv","unsetenv",
                "fopen_with_env","fgets","get_search_paths_with_env","fileExists","fsync","fremove","fdopen_decrypt",
                "opendir","closedir","readdir","mkdir","rmdir","stat","ftrunc","frename","fopen_fd","fclose_fd","fopen_with_env_fd"};
            static uint32_t logged;
            const char *mode = mid == 0 ? str(2) : mid == 19 ? str(4) : "";
            const bool writeSide = mid == 5 || mid == 24 || mid == 29 || mid == 30 || mid == 32 || mid == 33 || strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
            char *slot = gRecent + (gRecentAt++ % 32) * 96;
            snprintf(slot, 96, "#%d %s(%s,%s) h%u -> %u", iter, mid < 37u ? kNames[mid] : "?", path, mode, prim[0], prevRes);
            // C4: the 64 slots filled with registry reads (all fine); now only failures and the reads outside the registry
            const bool readOpen = (mid == 0 || mid == 19 || mid == 22 || mid == 26) && !writeSide && !inRegistry(path);
            if (gEarly && gEarlyAt < 64 && (prevRes || readOpen)) memcpy(gEarly + (gEarlyAt++) * 96, slot, 96);
            if ((logged++ < 400 && mid != 4 && mid != 20) || prevRes || writeSide)
                IOLog("fuxi sensor apps_std[%d]: %s(%s,%s) len=%u -> %u\n", iter, mid < 37u ? kNames[mid] : "?", path, mode, mid == 5 ? inLen[1] : 0u, prevRes);
        } else {
            IOLog("fuxi sensor rev[%d]: unhandled h=%#x sc=%#x\n", iter, rh, rsc);
        }
        uint32_t prevLen = 0;
        if (parsed) {
            uint32_t w = 0;
            for (uint32_t b = 0; b < nOut; ++b) {
                memcpy(pbuf + w, &outLen[b], 4); w += 4;
                if (outLen[b]) {w = (w + 7u) & ~7u; w += outLen[b];}
            }
            prevLen = w;
        }

        // next adsp_listener_next2 with prevCtx=rctx, prevResult, prevbufs
        memset(va, 0, 256);
        rpra[0] = dspBase + 128ULL; rpra[1] = 16ULL;
        rpra[2] = prevLen ? (dspBase + 0x4000ULL) : 0ULL; rpra[3] = prevLen;
        rpra[4] = dspBase + 192ULL; rpra[5] = 16ULL;
        rpra[6] = dspBase + 0x2000ULL; rpra[7] = 8192ULL;
        l4[0] = 1u; l4[1] = 0u;
        l4[2] = prevLen ? 1u : 0u; l4[3] = 0u;
        l4[4] = 1u; l4[5] = 0u;
        l4[6] = 1u; l4[7] = 0u;
        p1[0] = dspBase;
        p1[1] = 32768ULL;
        uint32_t nextIn0[4] = {rctx, prevRes, prevLen, 8192u};
        memcpy(va + 128, nextIn0, sizeof(nextIn0));
        __asm__ volatile("dsb sy" ::: "memory");
        msg.tid = client_id;
        msg.ctx = 0x52ULL + (static_cast<uint64_t>((iter + 1) & 0xfff) << 8);
        msg.handle = 3u;
        msg.sc = (4u << 24) | (2u << 16) | (2u << 8);
        msg.addr = dspBase;
        msg.size = 32768ULL;
        sheng_adsp_send(gFastrpcCh, reinterpret_cast<const uint8_t *>(&msg), sizeof(msg));
        if (iter == 40) {    // after the registry load, check whether the sensor client service is up
            QcomQrtr *qq = sheng_qrtr_get();
            if (qq) IOLog("fuxi sensor rev: sns190=%d after %d requests\n", qcom_qrtr_find_remote(qq, kSnsClientSvc), iter);
        }
    }

    QcomQrtr *q = sheng_qrtr_get();
    for (int i = 0; i < 10; ++i) {
        IOSleep(1000);
        if (q) {
            int idx = qcom_qrtr_find_remote(q, kSnsClientSvc);
            if (idx >= 0) {
                IOLog("fuxi sensor fastrpc: SNS_CLIENT_SVC 0x190 UP at %u:%u!\n",
                      q->remote[idx].node, q->remote[idx].port);
                break;
            }
        }
    }
    IOLog("fuxi sensor s6 summary: up=%d rx=%u sns190=%d\n",
          (int)gFastrpcUp, gFastrpcRxCount, q ? qcom_qrtr_find_remote(q, kSnsClientSvc) : -2);
}

}  // namespace

extern "C" void fuxi_sensor_probe(void) {
    static int started = 0;
    if (started) return;
    started = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(sensorWorker, nullptr, &thread) == KERN_SUCCESS) {
        thread_deallocate(thread);
    }
}
