// SPDX-License-Identifier: BSD-2-Clause-Patent
// fuxi (Xiaomi 13, SM8550) 2026-11-18: the modem DSP (MPSS), phase 1 of https://unixnu.com/zh/porting/baseband. Boot argument
// fuxi-modem=1 surveys what the boot stages left (SMEM partition with the modem, its SMP2P state, the firmware in the modem partition)
// and writes nothing; fuxi-modem=2 also starts the modem and brings up its GLINK edge, logging the channels the modem opens (IPCRTR
// for the IPC router). The QMI services, rmtfs and the AppleBaseband bridge (UniXNU/QcomBaseband) come in later steps.
//
// Devicetree facts (sm8550.dtsi remoteproc@4080000, qcom,sm8550-mpss-pas; Linux qcom_q6v5_pas sm8550_mpss_resource): PAS id 4 for
// modem.mdt in mpss 0x8a800000 + 0x10800000, PAS id 0x26 for modem_dtb.mdt in q6-mpss-dtb 0x9b000000 + 0x80000, the DSM region
// 0xd4d00000 + 0x3300000 assigned to the modem (VMID MSS_MSA) before start; proxy power domains cx and mss (RPMh ARC cx.lvl, mss.lvl),
// AOSS load_state "modem"; SMP2P items 435 (in) and 428 (out), remote pid 1, "master-kernel" / "slave-kernel" (fatal 0, ready 1,
// handover 2); IPCC client MPSS 2 with signal 0 GLINK and 2 SMP2P; GLINK items 478/479/480 in the apps-modem partition.
//
// The doorbell line (IPCC SPI 229) and the AOSS mailbox belong to AdspProbe.cpp's thread: its interrupt handler passes MPSS doorbells to
// fuxi_modem_ipcc() and it sends this probe's AOSS message (fuxi_adsp_qmp_request). Nothing here spins on a register.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <sys/errno.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "QcomGlink.h"
#include "QcomMdt.h"
#include "QcomRpmh.h"
#include "QcomScm.h"
#include "QcomSmem.h"
#include "QcomSmp2p.h"
int sheng_fw_open(const char *path, void **handle, uint64_t *size);
int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void sheng_fw_close(void *handle);
int fuxi_adsp_qmp_request(const char *message, uint32_t timeout_ms);   // AdspProbe.cpp: 0 sent and answered
extern volatile int32_t native_adsp_stats[16];
}

// [0] stage (negative: the step that failed), [1] last error, [2] doorbells, [3] slave-kernel bits (-1 no item), [4] items (bit 0 SMP2P
// in 435, 1 out 428, 2 GLINK 478, 3 479, 4 480, 8 the apps-modem partition), [5] modem.mdt KiB, [6] dtb KiB, [7] ms to ready,
// [8] GLINK in, [9] out, [10] version agreed | broken << 1, [11] channel opens, [12] last SCM x0, [13] x1, [14] votes cx | mss << 8,
// [15] DSM assign result
extern "C" {volatile int32_t native_modem_stats[16]; char native_modem_text[128];}

namespace {
enum Stage {
    StMapped = 1, StSurveyed = 2, StFirmware = 3, StSmp2p = 4, StLoadState = 5, StRails = 6, StDsm = 7, StDtb = 8, StLoaded = 9,
    StStarted = 10, StReady = 11, StGlink = 12, StRouter = 13,
};
constexpr uint64_t kSmem = 0x81d00000, kSmemSize = 0x200000, kMutex = 0x1f40000 + 3 * 0x1000, kIpcc = 0x408000;
constexpr uint64_t kRscDrv = 0x17a20000, kRscTcs = 0xd00, kCmdDb = 0x81c60000, kCmdDbSize = 0x20000;
constexpr uint64_t kMpssBase = 0x8a800000, kMpssSize = 0x10800000, kDtbBase = 0x9b000000, kDtbSize = 0x80000;
constexpr uint64_t kDsmBase = 0xd4d00000, kDsmSize = 0x3300000;
constexpr uint32_t kPasMpss = 4, kPasDtb = 0x26, kRemotePid = 1, kItemIn = 435, kItemOut = 428, kItemCrash = 421;
constexpr uint32_t kItemGlinkDesc = 478, kItemGlinkTx = 479, kItemGlinkRx = 480, kGlinkFifo = 0x4000;
constexpr uint32_t kClientMpss = 2;
constexpr uint32_t kFatal = 1u << 0, kReady = 1u << 1, kHandover = 1u << 2;
constexpr uint32_t kEvSmp2p = 1, kEvGlink = 2;
constexpr uint32_t kVmHlos = 3, kVmMssMsa = 15, kPermRw = 6;

struct Window {IOMemoryMap *map; volatile uint8_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size, IOOptionBits cache = kIOMapInhibitCache) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void unmap(Window &w) {if (w.map) w.map->release(); w = {};}
uint32_t rd(const Window &w, uint64_t at) {return *reinterpret_cast<volatile uint32_t *>(w.va + at);}
void wr(const Window &w, uint64_t at, uint32_t value) {*reinterpret_cast<volatile uint32_t *>(w.va + at) = value; __asm__ volatile("dsb sy" ::: "memory");}
void fail(int step, int error) {native_modem_stats[1] = error; native_modem_stats[0] = -step;}
void stage(int value) {native_modem_stats[0] = value;}
void note(const char *prefix, const char *text) {
    size_t at = 0;
    for (; prefix[at] && at < sizeof native_modem_text - 1; ++at) native_modem_text[at] = prefix[at];
    for (size_t i = 0; text[i] && at < sizeof native_modem_text - 1; ++i) native_modem_text[at++] = text[i];
    native_modem_text[at] = 0;
}

Window gSmemW, gMutex, gIpcc, gRsc;
QcomSmem gSmem;
QcomSmemPartition gPart;
bool gPartOk;
// On the heap (allocated by the thread before use): the GLINK state is several KiB and the kernel's __DATA has no room for it.
QcomSmp2p *gP2pHeap;
QcomGlink *gGlinkHeap;
#define gP2p (*gP2pHeap)
#define gGlink (*gGlinkHeap)
bool gGlinkStarted;
volatile uint32_t gPending;

void wake(uint32_t bits) {
    __atomic_fetch_or(&gPending, bits, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gPending);
}
uint32_t waitEvents(uint32_t ms) {
    uint32_t bits = __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL);
    if (bits) return bits;
    assert_wait_timeout((event_t)&gPending, THREAD_UNINT, ms, kMillisecondScale);
    bits = __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL);
    if (bits) {clear_wait(current_thread(), THREAD_AWAKENED); return bits;}
    thread_block(THREAD_CONTINUE_NULL);
    return __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL);
}
void ring(uint32_t signal) {wr(gIpcc, QCOM_IPCC_SEND, qcom_ipcc_id(kClientMpss, signal));}
void kickGlink(void *) {ring(QCOM_IPCC_SIGNAL_GLINK_QMP);}
void kickSmp2p() {ring(QCOM_IPCC_SIGNAL_SMP2P);}

bool lockSmem() {
    for (unsigned i = 0; i < 1000; ++i) {wr(gMutex, 0, 1); if (rd(gMutex, 0) == 1) return true; IODelay(10);}
    return false;
}
// The TCSR mutex is shared with AdspProbe's allocations; each holder keeps it only for one allocation, so the value written is ours:
// the hardware mutex takes the id of the processor that writes it (apps 1), and both probes run on the apps processor.
int allocItem(uint16_t id, uint32_t size) {
    if (!lockSmem()) return -16;
    const int rc = qcom_smem_alloc(&gSmem, &gPart, id, size);
    wr(gMutex, 0, 0);
    return rc == QcomSmemExists ? 0 : rc;
}
volatile uint8_t *item(uint16_t id, uint32_t *size) {
    uint32_t at = 0, bytes = 0;
    if (!gPartOk || qcom_smem_find(&gSmem, &gPart, id, &at, &bytes)) return nullptr;
    if (size) *size = bytes;
    return gSmemW.va + at;
}
void readCrashReason() {
    QcomSmemPartition global;
    uint32_t at = 0, size = 0;
    if (qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &global) ||
        qcom_smem_find(&gSmem, &global, kItemCrash, &at, &size) || !size)
        return;
    char text[100];
    size_t n = 0;
    for (; n < sizeof text - 1 && n < size; ++n) {const char c = (char)gSmemW.va[at + n]; if (!c) break; text[n] = c >= 32 && c < 127 ? c : '.';}
    text[n] = 0;
    if (n) note("crash: ", text);
}

void survey() {
    int32_t bits = 0;
    if (!qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_APPS, kRemotePid, &gPart)) {
        gPartOk = true;
        bits |= 1 << 8;
        const uint16_t ids[5] = {kItemIn, kItemOut, kItemGlinkDesc, kItemGlinkTx, kItemGlinkRx};
        for (unsigned i = 0; i < 5; ++i) if (item(ids[i], nullptr)) bits |= 1 << i;
    }
    native_modem_stats[4] = bits;
    native_modem_stats[3] = -1;
    if (volatile uint8_t *in = item(kItemIn, nullptr)) {
        QcomSmp2p view = {};
        view.in = reinterpret_cast<volatile uint32_t *>(in);
        view.local_pid = 0; view.remote_pid = kRemotePid;
        const int index = qcom_smp2p_check_in(view.in, 0, kRemotePid) ? -1 : qcom_smp2p_find_in(&view, "slave-kernel");
        if (index >= 0) native_modem_stats[3] = (int32_t)qcom_smp2p_in_value(&view, index);
    }
    readCrashReason();
}

// ---- secure world calls: any service, up to 7 arguments (the 4th onwards through a buffer in x5, the SMCCC convention of qcom_scm) ----
struct ScmResult {int64_t x0; uint64_t x1;};
IOBufferMemoryDescriptor *gScmArgs;
ScmResult scmCall(uint32_t svc, uint32_t command, uint32_t arginfo, const uint64_t *args, unsigned count) {
    ScmResult result = {-1, 0};
    uint64_t a[4] = {count > 0 ? args[0] : 0, count > 1 ? args[1] : 0, count > 2 ? args[2] : 0, count > 3 ? args[3] : 0};
    if (count > 4) {
        if (!gScmArgs) {
            gScmArgs = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 4096, 0xfffffffffffff000ull);
            if (!gScmArgs || gScmArgs->prepare() != kIOReturnSuccess) return result;
        }
        uint64_t *extra = static_cast<uint64_t *>(gScmArgs->getBytesNoCopy());
        for (unsigned i = 3; i < count; ++i) extra[i - 3] = args[i];
        __asm__ volatile("dsb sy" ::: "memory");
        for (uintptr_t p = reinterpret_cast<uintptr_t>(extra) & ~63ul; p < reinterpret_cast<uintptr_t>(extra) + 64; p += 64) __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
        __asm__ volatile("dsb sy" ::: "memory");
        a[3] = gScmArgs->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    }
    for (unsigned busy = 0; busy < 20; ++busy) {
        uint64_t function = qcom_scm_fn(svc, command), session = 0;
        unsigned resumes = 0;
        for (;;) {
            register uint64_t x0 __asm__("x0") = function, x1 __asm__("x1") = arginfo, x2 __asm__("x2") = a[0], x3 __asm__("x3") = a[1],
                              x4 __asm__("x4") = a[2], x5 __asm__("x5") = a[3], x6 __asm__("x6") = session, x7 __asm__("x7") = 0;
            __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                             :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
            result = {(int64_t)x0, x1};
            if ((int64_t)x0 != QCOM_SCM_INTERRUPTED || ++resumes > 100000) break;
            function = QCOM_SCM_INTERRUPTED;
            session = x6;
        }
        if (result.x0 != QCOM_SCM_V2_EBUSY) break;
        IOSleep(30);
    }
    native_modem_stats[12] = (int32_t)result.x0;
    native_modem_stats[13] = (int32_t)result.x1;
    return result;
}
ScmResult scm(uint32_t command, uint32_t arginfo, uint64_t a, uint64_t b, uint64_t c) {
    const uint64_t args[3] = {a, b, c};
    return scmCall(QCOM_SCM_SVC_PIL, command, arginfo, args, (arginfo & 0xf) < 3 ? (arginfo & 0xf) : 3);
}
bool scmOk(const ScmResult &r) {return r.x0 == 0 && r.x1 == 0;}
void cleanToPoc(const void *start, size_t length) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(start) & ~(line - 1); a < reinterpret_cast<uintptr_t>(start) + length; a += line)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// qcom_scm_assign_mem: the DSM region from the apps VM (HLOS) to the modem's (MSS_MSA, read/write). SVC_MP 0x0c, MP_ASSIGN 0x16, seven
// arguments: memory list, its size, source VMs, their size, destination permissions, their size, 0.
int assignDsm() {
    IOBufferMemoryDescriptor *buf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 4096, 0xfffffffffffff000ull);
    if (!buf || buf->prepare() != kIOReturnSuccess) {if (buf) buf->release(); return -12;}
    uint8_t *va = static_cast<uint8_t *>(buf->getBytesNoCopy());
    bzero(va, 4096);
    const uint64_t pa = buf->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    uint64_t *mem = reinterpret_cast<uint64_t *>(va);            // {addr, size} at 0
    mem[0] = kDsmBase; mem[1] = kDsmSize;
    uint32_t *src = reinterpret_cast<uint32_t *>(va + 64);        // {HLOS} at 64
    src[0] = kVmHlos;
    uint8_t *dst = va + 128;                                      // {vmid, perm, ctx(64), ctx_size, unused} at 128
    const uint32_t vm = kVmMssMsa, perm = kPermRw;
    memcpy(dst, &vm, 4); memcpy(dst + 4, &perm, 4);
    cleanToPoc(va, 256);
    const uint64_t args[7] = {pa, 16, pa + 64, 4, pa + 128, 24, 0};
    const uint32_t info = 7 | QCOM_SCM_RO << 4 | QCOM_SCM_VAL << 6 | QCOM_SCM_RO << 8 | QCOM_SCM_VAL << 10 | QCOM_SCM_RO << 12 |
                          QCOM_SCM_VAL << 14 | QCOM_SCM_VAL << 16;
    const ScmResult r = scmCall(0x0c, 0x16, info, args, 7);
    buf->complete(); buf->release();
    native_modem_stats[15] = (int32_t)r.x0;
    return r.x0 == 0 ? 0 : -1;
}

// ---- RPMh proxy votes for cx and mss, through active TCS 2 (TCS 0 is the WiFi bring-up's, 1 AdspProbe's). The done interrupt belongs
// to AdspProbe's handler, so this waits a fixed 20 ms instead; a vote that took effect late still counts (the WiFi rails show so). ----
QcomRpmhResource gCx, gMss;
QcomRscLayout gLayout;
int vote(uint32_t cx, uint32_t mss) {
    const uint32_t addresses[2] = {gCx.address, gMss.address}, values[2] = {cx, mss};
    QcomRpmhWrite writes[16];
    const unsigned n = qcom_rpmh_active_request(&gLayout, 2, addresses, values, 2, writes, 16);
    if (!n) return -22;
    for (unsigned i = 0; i < n; ++i) wr(gRsc, kRscTcs + writes[i].offset, writes[i].value);
    IOSleep(20);
    const unsigned m = qcom_rpmh_finish(&gLayout, 2, writes, 16);
    for (unsigned i = 0; i < m; ++i) wr(gRsc, kRscTcs + writes[i].offset, writes[i].value);
    native_modem_stats[14] = (int32_t)(cx | mss << 8);
    return 0;
}
int rails() {
    Window db = {};
    uint8_t *copy = static_cast<uint8_t *>(IOMallocData(kCmdDbSize));
    int rc = copy && mapWindow(db, kCmdDb, kCmdDbSize) ? 0 : -12;
    if (!rc) {for (uint64_t i = 0; i < kCmdDbSize; i += 4) {const uint32_t w = rd(db, i); memcpy(copy + i, &w, 4);} unmap(db);}
    uint32_t cx = 0, mss = 0;
    if (!rc && (qcom_cmd_db_find(copy, kCmdDbSize, "cx.lvl", &gCx) || qcom_cmd_db_find(copy, kCmdDbSize, "mss.lvl", &gMss) ||
                qcom_rpmh_level_index(&gCx, 0xffffffffu, &cx) || qcom_rpmh_level_index(&gMss, 0xffffffffu, &mss)))
        rc = -2;
    if (copy) IOFreeData(copy, kCmdDbSize);
    return rc ? rc : vote(cx, mss);
}

// ---- firmware images (same flow as AdspProbe) ----
struct Image {void *file; uint64_t size; QcomMdtImage mdt; IOBufferMemoryDescriptor *meta;};
// On the heap: two header tables in __DATA pushed the kernel's data segment into a retained prelink segment (2026-11-18).
Image *gImages;
#define gDtbImage (gImages[0])
#define gMpssImage (gImages[1])
int fileRead(void *context, uint64_t offset, void *to, size_t length) {return sheng_fw_read(context, offset, to, length);}
void release(Image &image) {
    if (image.file) sheng_fw_close(image.file);
    if (image.meta) image.meta->release();
    image.file = nullptr; image.meta = nullptr;
}
int openImage(Image &image, const char *path) {
    int rc = sheng_fw_open(path, &image.file, &image.size);
    if (rc) {image.file = nullptr; return rc;}
    rc = qcom_mdt_parse(&image.mdt, image.size, fileRead, image.file);
    return rc ? 1000 - rc : 0;
}
int authenticateHeader(Image &image, uint32_t pas, uint64_t base) {
    const uint64_t bytes = qcom_mdt_metadata_size(&image.mdt);
    image.meta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, bytes, 0xfffffffffffff000ull);
    if (!image.meta || image.meta->prepare() != kIOReturnSuccess) return -12;
    uint8_t *va = static_cast<uint8_t *>(image.meta->getBytesNoCopy());
    if (qcom_mdt_metadata(&image.mdt, fileRead, image.file, va, bytes)) return -5;
    cleanToPoc(va, bytes);
    const uint64_t phys = image.meta->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    if (!scmOk(scm(QCOM_SCM_PAS_INIT_IMAGE, qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0), pas, phys, 0))) return -1;
    if (image.mdt.relocatable && !scmOk(scm(QCOM_SCM_PAS_MEM_SETUP, qcom_scm_args(3, 0, 0, 0), pas, base, qcom_mdt_span(&image.mdt))))
        return -2;
    return 0;
}
int loadSegments(Image &image, uint64_t base, uint64_t size) {
    constexpr size_t kChunk = 256 * 1024;
    uint8_t *bounce = static_cast<uint8_t *>(IOMallocData(kChunk));
    if (!bounce) return -12;
    int rc = 0;
    // The 264 MiB carveout is mapped one segment at a time, not whole.
    for (unsigned i = 0; !rc && i < image.mdt.count; ++i) {
        const QcomMdtSegment &s = image.mdt.segment[i];
        uint64_t at = 0;
        if (!s.load || !s.memory_size) continue;
        if (qcom_mdt_place(&image.mdt, i, base, size, &at)) {rc = -7; break;}
        Window region = {};
        if (!mapWindow(region, base + at, (s.memory_size + 0xfff) & ~0xfffull, kIOMapWriteCombineCache)) {rc = -12; break;}
        uint8_t *to = const_cast<uint8_t *>(region.va);
        for (uint64_t done = 0; !rc && done < s.file_size;) {
            const size_t n = s.file_size - done < kChunk ? (size_t)(s.file_size - done) : kChunk;
            if (sheng_fw_read(image.file, s.file_offset + done, bounce, n)) {rc = -5; break;}
            memcpy(to + done, bounce, n);
            done += n;
        }
        if (!rc && s.memory_size > s.file_size) bzero(to + s.file_size, (size_t)(s.memory_size - s.file_size));
        __asm__ volatile("dsb sy" ::: "memory");
        unmap(region);
    }
    IOFreeData(bounce, kChunk);
    return rc;
}

// ---- events ----
uint32_t gSlave;
void serviceSmp2p() {
    if (!gP2p.in) {
        if (volatile uint8_t *in = item(kItemIn, nullptr)) gP2p.in = reinterpret_cast<volatile uint32_t *>(in);
        else return;
    }
    uint32_t value = 0, changed = 0;
    if (qcom_smp2p_service(&gP2p, "slave-kernel", &value, &changed)) kickSmp2p();
    gSlave = value;
    native_modem_stats[3] = (int32_t)value;
    if (changed & value & kFatal) readCrashReason();
}
void serviceGlink() {
    if (!gGlinkStarted) return;
    if (!gGlink.rx.data) {
        uint32_t size = 0;
        if (volatile uint8_t *rx = item(kItemGlinkRx, &size)) qcom_glink_set_rx(&gGlink, rx, size);
    }
    qcom_glink_poll(&gGlink);
    native_modem_stats[8] = (int32_t)gGlink.rx_messages;
    native_modem_stats[9] = (int32_t)gGlink.tx_messages;
    native_modem_stats[10] = (int32_t)((uint32_t)gGlink.version_agreed | (uint32_t)gGlink.broken << 1);
    native_modem_stats[11] = (int32_t)gGlink.unknown_opens;
    if (gGlink.unknown_opens && strncmp(native_modem_text, "crash: ", 7)) note("open: ", gGlink.unknown_names);
    if (gGlink.version_agreed && native_modem_stats[0] >= StReady && native_modem_stats[0] < StGlink) stage(StGlink);
}
void service(uint32_t bits) {
    if (bits & (kEvSmp2p | kEvGlink)) serviceSmp2p();
    if (bits & kEvGlink) serviceGlink();
}
template <typename F> bool waitUntil(uint32_t ms, F done) {
    uint64_t deadline;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    while (!done()) {
        const uint64_t now = mach_absolute_time();
        if (now >= deadline) return done();
        uint64_t left = 0;
        absolutetime_to_nanoseconds(deadline - now, &left);
        service(waitEvents((uint32_t)(left / 1000000) + 1));
    }
    return true;
}

int openFirmware(char *path, size_t size) {
    static const uint32_t pauses[] = {1, 2, 4, 8, 16, 30, 30, 30};
    for (unsigned i = 0;; ++i) {
        snprintf(path, size, "modem:modem_dtb");
        int rc = openImage(gDtbImage, path);
        if (!rc) {
            snprintf(path, size, "modem:modem");
            if (!(rc = openImage(gMpssImage, path))) return 0;
            release(gDtbImage);
        }
        if ((rc != ENOENT && rc != ENXIO && rc != EIO) || i == sizeof pauses / sizeof pauses[0]) return rc;
        IOSleep(pauses[i] * 1000);
    }
}
void shutdownModem(bool dtb, bool mpss) {
    if (mpss) scm(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), kPasMpss, 0, 0);
    if (dtb) scm(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), kPasDtb, 0, 0);
}

bool start(unsigned mode) {
    char path[64];
    if (!gImages) gImages = static_cast<Image *>(IOMallocZeroData(2 * sizeof(Image)));
    if (!gImages) {fail(StFirmware, -12); return false;}
    int rc = openFirmware(path, sizeof path);
    if (rc) {note("fw: ", path); fail(StFirmware, rc); return false;}
    native_modem_stats[5] = (int32_t)(gMpssImage.size >> 10);
    native_modem_stats[6] = (int32_t)(gDtbImage.size >> 10);
    stage(StFirmware);
    if (mode < 2) {release(gDtbImage); release(gMpssImage); return false;}
    if (native_modem_stats[3] > 0) {fail(StFirmware, -114); release(gDtbImage); release(gMpssImage); return false;}  // running already
    if (!gPartOk) {fail(StSmp2p, -2); release(gDtbImage); release(gMpssImage); return false;}

    if ((rc = allocItem(kItemOut, QCOM_SMP2P_ITEM_BYTES))) {fail(StSmp2p, rc); release(gDtbImage); release(gMpssImage); return false;}
    qcom_smp2p_init_out(&gP2p, reinterpret_cast<volatile uint32_t *>(item(kItemOut, nullptr)), 0, kRemotePid);
    qcom_smp2p_add_entry(&gP2p, "master-kernel");
    wr(gIpcc, QCOM_IPCC_RECV_ENABLE, qcom_ipcc_id(kClientMpss, QCOM_IPCC_SIGNAL_SMP2P));
    wr(gIpcc, QCOM_IPCC_RECV_ENABLE, qcom_ipcc_id(kClientMpss, QCOM_IPCC_SIGNAL_GLINK_QMP));
    kickSmp2p();
    stage(StSmp2p);

    // The AOSS message goes through AdspProbe's thread, which owns the mailbox; it needs the DSP's bring-up to have opened it.
    if ((rc = fuxi_adsp_qmp_request("{class: image, res: load_state, name: modem, val: on}", 3000))) {
        fail(StLoadState, rc); release(gDtbImage); release(gMpssImage); return false;
    }
    stage(StLoadState);
    if ((rc = rails())) {fail(StRails, rc); release(gDtbImage); release(gMpssImage); return false;}
    stage(StRails);
    if ((rc = assignDsm())) {fail(StDsm, rc); vote(0, 0); release(gDtbImage); release(gMpssImage); return false;}
    stage(StDsm);

    if ((rc = authenticateHeader(gDtbImage, kPasDtb, kDtbBase)) || (rc = loadSegments(gDtbImage, kDtbBase, kDtbSize)) ||
        (rc = scmOk(scm(QCOM_SCM_PAS_AUTH_AND_RESET, qcom_scm_args(1, 0, 0, 0), kPasDtb, 0, 0)) ? 0 : -3)) {
        fail(StDtb, rc); vote(0, 0); release(gDtbImage); release(gMpssImage); return false;
    }
    stage(StDtb);
    if ((rc = authenticateHeader(gMpssImage, kPasMpss, kMpssBase)) || (rc = loadSegments(gMpssImage, kMpssBase, kMpssSize))) {
        fail(StLoaded, rc); shutdownModem(true, false); vote(0, 0); release(gDtbImage); release(gMpssImage); return false;
    }
    stage(StLoaded);
    const uint64_t began = mach_absolute_time();
    if (!scmOk(scm(QCOM_SCM_PAS_AUTH_AND_RESET, qcom_scm_args(1, 0, 0, 0), kPasMpss, 0, 0))) {
        fail(StStarted, -3); shutdownModem(true, false); vote(0, 0); release(gDtbImage); release(gMpssImage); return false;
    }
    stage(StStarted);
    const bool ready = waitUntil(15000, [] {return (gSlave & (kReady | kFatal)) != 0;});
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    native_modem_stats[7] = (int32_t)(ns / 1000000);
    release(gDtbImage);
    release(gMpssImage);
    if (!ready || (gSlave & kFatal)) {readCrashReason(); fail(StReady, ready ? -5 : -60); return false;}
    stage(StReady);
    return true;
}

bool startGlink() {
    int rc = allocItem(kItemGlinkDesc, 32);
    if (!rc) rc = allocItem(kItemGlinkTx, kGlinkFifo);
    uint32_t size = 0;
    volatile uint8_t *desc = item(kItemGlinkDesc, nullptr), *txf = item(kItemGlinkTx, &size);
    if (rc || !desc || !txf) {fail(StGlink, rc ? rc : -2); return false;}
    volatile uint32_t *d = reinterpret_cast<volatile uint32_t *>(desc);
    const QcomGlinkFifo tx = {&d[1], &d[0], txf, size}, rx = {&d[3], &d[2], nullptr, 0};
    qcom_glink_init(&gGlink, tx, rx, kickGlink, nullptr);
    gGlinkStarted = true;
    serviceGlink();
    return true;
}

void modemThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "fuxi-modem");
    unsigned mode = 0;
    PE_parse_boot_argn("fuxi-modem", &mode, sizeof mode);
    if (!mapWindow(gSmemW, kSmem, kSmemSize) || !mapWindow(gMutex, kMutex, 0x1000) || !mapWindow(gIpcc, kIpcc, 0x1000) ||
        !mapWindow(gRsc, kRscDrv, 0x10000)) {
        fail(StMapped, -12);
        return;
    }
    gSmem = {gSmemW.va, (uint32_t)kSmemSize};
    gLayout = qcom_rsc_layout(rd(gRsc, 0));
    gP2pHeap = static_cast<QcomSmp2p *>(IOMallocZeroData(sizeof(QcomSmp2p)));
    gGlinkHeap = static_cast<QcomGlink *>(IOMallocZeroData(sizeof(QcomGlink)));
    if (!gP2pHeap || !gGlinkHeap) {fail(StMapped, -12); return;}
    stage(StMapped);
    survey();
    stage(StSurveyed);
    if (mode >= 2) {
        // The doorbells and the AOSS mailbox come up with the audio DSP: wait (up to 3 minutes) for AdspProbe to reach its GLINK stage.
        for (unsigned i = 0; i < 360 && native_adsp_stats[0] >= 0 && native_adsp_stats[0] < 12; ++i) IOSleep(500);
        if (native_adsp_stats[0] < 12) {fail(StSmp2p, -19); return;}
    }
    if (!start(mode) || !startGlink()) return;
    for (;;) service(waitEvents(1000) | kEvGlink);
}

void modemLogThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "fuxi-modem-log");
    int32_t last[16] = {};
    char lastText[128] = "";
    for (unsigned tick = 0;; ++tick) {
        IOSleep(tick < 720 ? 250 : 5000);
        int32_t s[16];
        for (unsigned i = 0; i < 16; ++i) s[i] = native_modem_stats[i];
        bool changed = strncmp(lastText, native_modem_text, sizeof lastText);
        for (unsigned i = 0; i < 16; ++i) if (i != 2 && i != 8 && i != 9 && s[i] != last[i]) changed = true;
        if (!changed && (tick % 240) != 0) continue;
        memcpy(last, s, sizeof last);
        strlcpy(lastText, native_modem_text, sizeof lastText);
        IOLog("fuxi modem: stage %d err %d irq %d slave %#x items %#x fw %d/%dKiB ready %dms glink in %d out %d ver %d opens %d scm %d/%d votes %#x dsm %d \"%s\"\n",
              s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], native_modem_text);
    }
}
}  // namespace

// Called from AdspProbe.cpp's IPCC handler for MPSS doorbells (interrupt context: record and wake only).
extern "C" void fuxi_modem_ipcc(uint32_t id) {
    native_modem_stats[2] = native_modem_stats[2] + 1;
    if (id == qcom_ipcc_id(kClientMpss, QCOM_IPCC_SIGNAL_SMP2P)) wake(kEvSmp2p);
    else wake(kEvGlink);
}

extern "C" void fuxi_modem_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("fuxi-modem", &mode, sizeof mode) || !mode || native_modem_stats[0]) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(modemThread, nullptr, &thread) != KERN_SUCCESS) {fail(StMapped, -7); return;}
    thread_deallocate(thread);
    if (kernel_thread_start(modemLogThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
