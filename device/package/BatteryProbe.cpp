// SPDX-License-Identifier: BSD-2-Clause-Patent
// Starts the battery power source (ShengBattery.cpp) whenever the audio DSP runs (sheng-adsp=2: the values come from the DSP's battery
// manager); sheng-battery=0 leaves it off, sheng-battery=1 asks for it without the DSP argument. It registers that file's classes with the
// kernel's module (its static initializer, which the build takes out of __mod_init_func), then creates the service, which is published
// once the first refresh is complete.
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_battery_static_init(void) __asm__("__GLOBAL__sub_I_ShengBattery.cpp");
extern "C" void sheng_battery_start(void);
// See ShengBattery.cpp for the meaning of each entry.
extern "C" {volatile int32_t native_battery_stats[16];}

// fuxi: the [battery] and [charger] rows of sheng's boot log, as "fuxi battery:" kernel log rows whenever a value changes (every 2 s
// for 4 minutes, then every 30 s; one row every 5 minutes regardless).
extern "C" {extern volatile int32_t native_battery_raw[10]; extern volatile int32_t native_charger_stats[16];
            extern volatile int32_t native_charger_auth_stats[32];}
extern "C" {extern volatile uint64_t armcpu_sgi_unexpected __attribute__((weak_import));
            extern volatile uint32_t armcpu_sgi_unexpected_id __attribute__((weak_import)), armcpu_sgi_unexpected_core __attribute__((weak_import)),
                                     armcpu_sgi_unexpected_mask __attribute__((weak_import));}
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <string.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/kauth.h>
#include <sys/proc.h>
extern "C" int vn_rdwr(enum uio_rw rw, struct vnode *vp, caddr_t base, int len, off_t offset, enum uio_seg segflg, int ioflg,
    kauth_cred_t cred, int *aresid, proc_t p);
// The USB debug log's state (QcomXnuUsbDebug.h qcom_usb_debug_status, same layout): with no host draining it, the buffer fills and
// its overflow count grows. A host is taken as attached while the overflow count stands still and the buffer is under half full.
struct FuxiUsbStatus {
    uint32_t phase, error; int32_t core_result;
    uint32_t qcom_result, restore_result, dma_mode, dma_lease_active, vbus_owned, vbus_saved;
    uint32_t phy_owned, phy_saved, phy_applied, phy_restoring, stop_failed, dma_address_width, hwparams0;
    uint32_t last_trace, trace_a, trace_b, traces, poll_calls, last_poll_events, io_failed;
    uint64_t dma_pa, counter_frequency, cache_line, dropped;
    uint32_t domain_result, domain_stage, domain_bank, domain_slot, domain_asid;
    uint64_t tables_pa, dma_iova, domain_last_address, domain_last_expected, domain_last_readback;
    uint32_t diag_enabled, diag_checkpoint;
    uint64_t buffer_capacity, buffer_pending, buffer_high_water, buffer_accepted;
    uint64_t buffer_overflow, buffer_contended, buffer_replayed, transport_pending, rx_overflow, control_replies;
    uint32_t rx_pending, rx_generation, control_enabled, control_hook_owned, control_reboot_state, control_rejected;
};
extern "C" void qcom_usb_debug_get_status(FuxiUsbStatus *status);
static int qcom_usb_debug_host_ready(void) {
    static uint64_t lastOverflow;
    FuxiUsbStatus st = {};
    qcom_usb_debug_get_status(&st);
    const bool growing = st.buffer_overflow != lastOverflow;
    lastOverflow = st.buffer_overflow;
    // 2026-11-20: a USB log that is not active (the cable was pulled and the link never came back) counts as no host, so the rows
    // are kept and saved to the Data volume for the next boot.
    if (st.phase != 3 /* ACTIVE */ || !st.buffer_capacity) return 0;
    return !growing && st.buffer_pending < st.buffer_capacity / 2;
}
// fuxi 2026-11-19: while a charger holds the USB-C port no host reads the log. One compact row every 5 s (battery, charger, charge
// authentication) goes into a RAM ring (600 rows, 50 minutes); when a computer has the port again the rows are printed as
// "fuxi charge replay:", oldest first, after 3 s for the capture to open. The same boot only: nothing is stored.
static constexpr unsigned kReplayRows = 600, kReplayRow = 512;   // 2026-11-20: room for the 18 diagnostics
static char *gReplay;
static unsigned gReplayHead, gReplayCount;
static uint64_t gReplayStartMs;
static uint64_t replayNowMs() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;}
static void replayKeep(const int32_t *s, const int32_t *c) {
    if (!gReplay && !(gReplay = static_cast<char *>(IOMallocZeroData(kReplayRows * kReplayRow)))) return;
    const volatile int32_t *a = native_charger_auth_stats;
    char *row = gReplay + (size_t)((gReplayHead + gReplayCount) % kReplayRows) * kReplayRow;
    // diag: ShengChargeAuth.h order (svid, pdo2, adapter id, apdo max, power max, fast mode, quick type, pps ptf, pump1 ok, pump2 ok,
    // pump1 mA, pump2 mA, pump V, pump battery V, gauge fast, vdm version, vdm V, vdm temp); -1 = not read.
    snprintf(row, kReplayRow, "t+%llus pct %d mV %d mA %d temp %d | chg %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d | auth %d %d %d %d %d %d %d %d %d %d %d %d | diag %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
             (unsigned long long)((replayNowMs() - gReplayStartMs) / 1000), s[1], s[2], s[3], s[4],
             c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15],
             a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11],
             a[14], a[15], a[16], a[17], a[18], a[19], a[20], a[21], a[22], a[23], a[24], a[25], a[26], a[27], a[28], a[29], a[30], a[31]);
    if (gReplayCount < kReplayRows) gReplayCount++; else gReplayHead = (gReplayHead + 1) % kReplayRows;
}
// fuxi 2026-11-20: unplugging the cable loses the USB log until the next boot, so the ring is also written to the Data volume
// (every 6 rows, 30 s) and printed by the next boot ("fuxi charge replay (file):"), then emptied. Text, oldest row first.
static const char kChargeLogPath[] = "/private/var/db/fuxi-charge-log";
static unsigned gReplaySinceSave;
static void replaySave() {
    if (!gReplay || !gReplayCount) return;
    const size_t cap = (size_t)kReplayRows * (kReplayRow + 1);
    char *text = static_cast<char *>(IOMallocData(cap));
    if (!text) return;
    size_t len = 0;
    for (unsigned i = 0; i < gReplayCount; ++i) {
        const char *row = gReplay + (size_t)((gReplayHead + i) % kReplayRows) * kReplayRow;
        const size_t n = strnlen(row, kReplayRow - 1);
        memcpy(text + len, row, n); len += n; text[len++] = '\n';
    }
    vfs_context_t ctx = vfs_context_create(nullptr);
    vnode_t vp = nullptr;
    int error = ctx ? vnode_open(kChargeLogPath, FWRITE | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600, 0, &vp, ctx) : ENOMEM;
    if (!error) {
        int resid = 0;
        error = vn_rdwr(UIO_WRITE, vp, text, (int)len, 0, UIO_SYSSPACE, IO_NOAUTH | IO_SYNC, vfs_context_ucred(ctx), &resid,
                        vfs_context_proc(ctx));
        vnode_close(vp, FWRITE, ctx);
    }
    if (ctx) vfs_context_rele(ctx);
    IOFreeData(text, cap);
    static unsigned reported;
    if (error && reported++ < 3) IOLog("fuxi charge replay: file write error %d\n", error);
}
// The previous boot's rows, printed after the Data volume is mounted, then the file is emptied.
static void replayFileThread(void *, wait_result_t) {
    IOSleep(90000);
    const size_t cap = (size_t)kReplayRows * (kReplayRow + 1);
    char *text = static_cast<char *>(IOMallocData(cap + 1));
    if (!text) return;
    vfs_context_t ctx = vfs_context_create(nullptr);
    vnode_t vp = nullptr;
    int resid = 0;
    int error = ctx ? vnode_open(kChargeLogPath, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx) : ENOMEM;
    size_t len = 0;
    if (!error) {
        error = vn_rdwr(UIO_READ, vp, text, (int)cap, 0, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (!error) len = cap - (size_t)resid;
        vnode_close(vp, FREAD, ctx);
    }
    if (len) {
        text[len] = 0;
        IOLog("fuxi charge replay (file): previous boot, %lu bytes\n", (unsigned long)len);
        unsigned lines = 0;
        for (char *line = text; line && *line;) {
            char *next = strchr(line, '\n');
            if (next) *next++ = 0;
            IOLog("fuxi charge replay (file): %s\n", line);
            if ((++lines & 15) == 0) IOSleep(50);
            line = next;
        }
        IOLog("fuxi charge replay (file): end\n");
        if (!vnode_open(kChargeLogPath, FWRITE | O_TRUNC | O_NOFOLLOW, 0600, 0, &vp, ctx)) vnode_close(vp, FWRITE, ctx);
    } else {
        IOLog("fuxi charge replay (file): none (error %d)\n", error);
    }
    if (ctx) vfs_context_rele(ctx);
    IOFreeData(text, cap + 1);
}
static void replayFlush() {
    if (!gReplay || !gReplayCount) return;
    IOSleep(3000);
    IOLog("fuxi charge replay: %u rows kept while no host was attached\n", gReplayCount);
    for (unsigned i = 0; i < gReplayCount; ++i) {
        IOLog("fuxi charge replay: %s\n", gReplay + (size_t)((gReplayHead + i) % kReplayRows) * kReplayRow);
        if ((i & 15) == 15) IOSleep(50);   // let the USB log drain
    }
    IOLog("fuxi charge replay: end\n");
    gReplayHead = gReplayCount = 0;
}
static void fuxiBatteryLog(void *, wait_result_t) {
    int32_t last[16] = {}, lastC[16] = {};
    int hostWas = 1;
    for (unsigned tick = 0;; ++tick) {
        const int host = qcom_usb_debug_host_ready();
        if (!host) {
            if (hostWas) gReplayStartMs = replayNowMs();
            hostWas = 0;
            IOSleep(5000);
            int32_t s[16], c[16];
            for (unsigned i = 0; i < 16; ++i) {s[i] = native_battery_stats[i]; c[i] = native_charger_stats[i];}
            replayKeep(s, c);
            if (++gReplaySinceSave >= 6) {gReplaySinceSave = 0; replaySave();}
            continue;
        }
        if (!hostWas) {hostWas = 1; replayFlush();}
        IOSleep(tick < 120 ? 2000 : 30000);
        int32_t s[16], c[16];
        for (unsigned i = 0; i < 16; ++i) {s[i] = native_battery_stats[i]; c[i] = native_charger_stats[i];}
        if (!memcmp(s, last, sizeof s) && !memcmp(c, lastC, sizeof c) && tick % (tick < 120 ? 15 : 2)) continue;
        memcpy(last, s, sizeof s); memcpy(lastC, c, sizeof c);
        IOLog("fuxi battery: stage %d pct %d mV %d mA %d temp %d flags %#x | %d %d %d %d %d %d %d %d %d %d\n", s[0], s[1], s[2], s[3], s[4], s[5],
              s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15]);
        IOLog("fuxi battery raw: %d %d %d %d %d %d %d %d %d %d\n", native_battery_raw[0], native_battery_raw[1], native_battery_raw[2],
              native_battery_raw[3], native_battery_raw[4], native_battery_raw[5], native_battery_raw[6], native_battery_raw[7],
              native_battery_raw[8], native_battery_raw[9]);
        if (&armcpu_sgi_unexpected)
            IOLog("fuxi sgi: unexpected %llu last id %u core %u ids %#x\n", (unsigned long long)armcpu_sgi_unexpected, armcpu_sgi_unexpected_id,
                  armcpu_sgi_unexpected_core, armcpu_sgi_unexpected_mask);
        IOLog("fuxi charger: %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9],
              c[10], c[11], c[12], c[13], c[14], c[15]);
        {const volatile int32_t *a = native_charger_auth_stats;   // fuxi 2026-11-19: ShengChargeAuth.c (gauges, then the Xiaomi adapter)
         IOLog("fuxi charge auth: %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9],
               a[10], a[11], a[12], a[13], a[14], a[15]);}
    }
}
extern "C" void sheng_battery_probe(void) {
    unsigned enabled = 0, adsp = 0;
    if (!PE_parse_boot_argn("sheng-battery", &enabled, sizeof enabled)) enabled = PE_parse_boot_argn("sheng-adsp", &adsp, sizeof adsp) && adsp == 2;
    if (enabled != 1 || native_battery_stats[0]) return;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {native_battery_stats[0] = -10; return;}
    native_battery_static_init();
    if (OSMetaClass::postModLoad(handle)) {native_battery_stats[0] = -11; return;}
    sheng_battery_start();
    thread_t thread;
    if (kernel_thread_start(fuxiBatteryLog, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    if (kernel_thread_start(replayFileThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
