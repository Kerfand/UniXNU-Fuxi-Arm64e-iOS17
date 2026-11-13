// SPDX-License-Identifier: BSD-2-Clause-Patent
// iOS 17 build: runs CpuFreqProbe.cpp's poll and writes its row to the console. The iPadOS build polls from its log rounds
// (LegacyDisplayProbe.cpp); the iOS 17 probe has none, so without this the frequency domains were never voted and stayed at the boot
// loader's level for the whole session (2026-11-12, UI at 10-23 frames per second under touch). The row reports each domain's
// enable bit, current kHz/1000 (c), the level voted (v) and the state before the vote (pre): the before/after evidence of one boot.
// Every 2 s for the first 90 s (the secondary clusters come up late and are voted the first time they read enabled), then every 60 s,
// and only when the row changed.
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>

extern "C" {
extern char native_cpufreq_row[120];
extern volatile uint32_t native_cpufreq_stage;
void sheng_cpufreq_probe(void);
void sheng_cpufreq_poll(void);
// GpuPowerProbe.cpp's GPU governor (sheng-gpu-dcvs): [0] running, [1] level now, [2] changes, [3] failed votes, [4] busy % over the
// last 10 s, [5..12] % of those 10 s at levels 1..8 (220 295 348 401 475 550 615 680 MHz), [13] touch boosts, [14] wake boosts.
extern volatile uint32_t native_gpu_dcvs[15] __attribute__((weak_import));
// GpuPowerProbe.cpp ringIdle: fence waits that slept until the GPU interrupt (or sheng-gpu-pause-us), that spun 20 us (no interrupt line,
// interrupts off, preemption disabled), and sleeps that timed out. backboardd's CPU time sat in kern_SwapEnd (2026-11-12 samples):
// spinning waits would count there.
extern volatile uint64_t native_gpu_idle_sleeps __attribute__((weak_import));
extern volatile uint64_t native_gpu_idle_spins __attribute__((weak_import));
extern volatile uint64_t native_gpu_idle_timeouts __attribute__((weak_import));
// NativeLegacyDisplay.cpp's swap event ring (always on, 65536 entries): [0] microseconds, [1] event << 56 | slot << 48 | validated << 40 |
// fences << 24 | request tag. Events: 1 validated, 2 fence wait, 3 fence passed, 4 flipped, 5 latched, 6 retired, 7 offered to the GPU
// copy, 8 blank.
extern volatile uint64_t (*native_legacy_ring)[2] __attribute__((weak_import));
extern volatile uint32_t native_legacy_ring_cap __attribute__((weak_import));
extern volatile uint32_t native_legacy_ring_count __attribute__((weak_import));
// CPU time per process (2026-11-12 probe: frames 40-50 ms apart with the GPU 5-23 % busy, its copy queue under 1.4 ms and the CPU
// domains at their top; which tasks take the CPU?). Kernel-internal calls; this archive is linked into the kernel.
typedef struct proc *proc_t;
typedef struct task *task_t;
int proc_iterate(unsigned int flags, int (*callout)(proc_t, void *), void *arg, int (*filterfn)(proc_t, void *), void *filterarg);
int proc_pid(proc_t);
void proc_name(int pid, char *buf, int size);
task_t proc_task(proc_t);
kern_return_t task_info(task_t, uint32_t flavor, int *info, uint32_t *count);
}

namespace {
// The GPU row (2026-11-12 probe: the UI ran at 10-23 frames per second with the CPU domains at their top, and nothing logged where
// the GPU clock was). Printed when it changed, at most every 10 s (its window).
void gpuRow(char *last, size_t size) {
    if (!&native_gpu_dcvs) return;
    const volatile uint32_t *g = native_gpu_dcvs;
    char row[160];
    snprintf(row, sizeof row, "run%u level%u changes%u fail%u busy%u%% at %u/%u/%u/%u/%u/%u/%u/%u touch%u wake%u", g[0], g[1], g[2], g[3],
             g[4], g[5], g[6], g[7], g[8], g[9], g[10], g[11], g[12], g[13], g[14]);
    if (strncmp(last, row, size)) {
        strlcpy(last, row, size);
        IOLog("fuxi gpu dcvs: %s\n", row);
    }
    if (&native_gpu_idle_sleeps && &native_gpu_idle_spins && &native_gpu_idle_timeouts) {
        static uint64_t s0, p0, t0;
        const uint64_t s = native_gpu_idle_sleeps, p = native_gpu_idle_spins, t = native_gpu_idle_timeouts;
        if (s != s0 || p != p0)
            IOLog("fuxi gpu wait: sleeps %llu spins %llu (%llu us spun) timeouts %llu in the window\n", (unsigned long long)(s - s0),
                  (unsigned long long)(p - p0), (unsigned long long)(p - p0) * 20, (unsigned long long)(t - t0));
        s0 = s; p0 = p; t0 = t;
    }
}

// Frame pacing since the last call, the measure of the sheng progress notes (frame intervals, not submit counts): frames validated,
// intervals between consecutive validated swaps in buckets (<=9, <=17, <=34, <=50, <=100, >100 ms; gaps over 500 ms are idle and not
// counted), their median, and how long a swap waited between validation and its GPU copy (queue behind the previous copy). Silent when
// no frame came.
void frameRow() {
    if (!&native_legacy_ring || !native_legacy_ring || !native_legacy_ring_cap) return;
    static uint32_t seen;
    static uint64_t lastValidated;
    const uint32_t cap = native_legacy_ring_cap, end = native_legacy_ring_count;
    uint32_t from = seen;
    if (end - from > cap) from = end - cap;
    seen = end;
    unsigned frames = 0, bucket[6] = {}, waits = 0;
    uint64_t waitSum = 0, waitMax = 0;
    static uint16_t gaps[4096];
    unsigned ngaps = 0;
    // validation time per request tag, for the queue wait (a small table, newest wins)
    static uint64_t tagAt[64]; static uint32_t tagOf[64];
    for (uint32_t i = from; i != end; ++i) {
        const volatile uint64_t *e = native_legacy_ring[i & (cap - 1)];
        const uint64_t us = e[0], word = e[1];
        const unsigned ev = unsigned(word >> 56);
        const uint32_t tag = uint32_t(word & 0xffffff);
        if (ev == 1) {
            ++frames;
            if (lastValidated && us > lastValidated) {
                const uint64_t gap = us - lastValidated;
                if (gap <= 500000) {
                    const uint64_t ms = gap / 1000;
                    bucket[ms <= 9 ? 0 : ms <= 17 ? 1 : ms <= 34 ? 2 : ms <= 50 ? 3 : ms <= 100 ? 4 : 5]++;
                    if (ngaps < 4096) gaps[ngaps++] = uint16_t(gap / 10 > 65535 ? 65535 : gap / 10);   // 10 us units
                }
            }
            lastValidated = us;
            tagAt[tag & 63] = us; tagOf[tag & 63] = tag;
        } else if (ev == 7 && tagOf[tag & 63] == tag && tagAt[tag & 63] && us >= tagAt[tag & 63]) {
            const uint64_t w = us - tagAt[tag & 63];
            waitSum += w; if (w > waitMax) waitMax = w; ++waits;
            tagAt[tag & 63] = 0;
        }
    }
    if (!frames) return;
    unsigned median = 0;
    if (ngaps) {   // partial selection: count below each 1 ms step
        for (unsigned ms = 1; ms <= 650; ++ms) {
            unsigned below = 0;
            for (unsigned k = 0; k < ngaps; ++k) if (gaps[k] < ms * 100) ++below;
            if (below * 2 >= ngaps) {median = ms; break;}
        }
    }
    IOLog("fuxi frames: %u frames, gaps <=9:%u <=17:%u <=34:%u <=50:%u <=100:%u >100:%u ms, median %u ms, copy wait avg %llu max %llu us\n",
          frames, bucket[0], bucket[1], bucket[2], bucket[3], bucket[4], bucket[5], median,
          waits ? (unsigned long long)(waitSum / waits) : 0ull, (unsigned long long)waitMax);
}

// TASK_POWER_INFO (flavor 21): total_user, total_system (mach time, terminated threads included), task_interrupt_wakeups, ...
struct PowerInfo {uint64_t totalUser, totalSystem, interruptWakeups, platformIdleWakeups, timerWakeupsBin1, timerWakeupsBin2;};
struct CpuSlot {int pid; uint64_t last; uint64_t delta;};
CpuSlot gCpu[640];
int cpuCallout(proc_t p, void *) {
    const int pid = proc_pid(p);
    task_t t = proc_task(p);
    if (!t) return 0;   // PROC_RETURNED
    PowerInfo info = {};
    uint32_t count = sizeof info / sizeof(int);
    if (task_info(t, 21, reinterpret_cast<int *>(&info), &count) != KERN_SUCCESS) return 0;
    const uint64_t now = info.totalUser + info.totalSystem;
    CpuSlot *free = nullptr;
    for (CpuSlot &s : gCpu) {
        if (s.pid == pid && s.last) {s.delta = now >= s.last ? now - s.last : 0; s.last = now; return 0;}
        if (!free && !s.last) free = &s;
    }
    if (free) {free->pid = pid; free->last = now; free->delta = 0;}
    return 0;
}
// The heaviest tasks of the last window, in percent of one CPU (the window is the time since the previous call).
void cpuRow() {
    static uint64_t lastT;
    const uint64_t t = mach_absolute_time();
    for (CpuSlot &s : gCpu) s.delta = 0;
    proc_iterate(1 /* PROC_ALLPROCLIST */, cpuCallout, nullptr, nullptr, nullptr);
    if (!lastT) {lastT = t; return;}
    const uint64_t window = t - lastT;
    lastT = t;
    char row[200]; size_t n = 0;
    n += snprintf(row + n, sizeof row - n, "fuxi cpu top:");
    uint64_t total = 0;
    for (CpuSlot &s : gCpu) total += s.delta;
    for (unsigned k = 0; k < 7; ++k) {
        CpuSlot *best = nullptr;
        for (CpuSlot &s : gCpu) if (s.delta && (!best || s.delta > best->delta)) best = &s;
        if (!best) break;
        char name[20] = {};
        proc_name(best->pid, name, sizeof name);
        if (n < sizeof row) n += snprintf(row + n, sizeof row - n, " %s[%d] %llu%%", name, best->pid,
                                          (unsigned long long)(best->delta * 100 / window));
        best->delta = 0;
    }
    if (n < sizeof row) snprintf(row + n, sizeof row - n, " all %llu%%", (unsigned long long)(total * 100 / window));
    IOLog("%s\n", row);
}

// fuxi probe (2026-11-13, black screen after the power button): what the panel is shown. The DPU's VIG 0 / VIG 1 source address
// (+0x14) and stride (+0x24) in the MDP block (0xae01000, as NativeLegacyDisplay flips them), and 16 x 16 samples of the memory VIG 0
// scans (identity-mapped, as the boot memory 0xb8000000 is): how many are not black and a checksum, so a frozen or black picture shows
// as a constant row.
extern "C" {extern volatile uint32_t native_legacy_gpu_fallbacks; extern volatile int32_t native_legacy_gpu_last_status;
            extern volatile uint64_t native_legacy_early_retires, native_legacy_early_failures, native_gop_gpu_offers;}
void fbRow() {
    static IOMemoryMap *mdpMap, *fbMap;
    static uint64_t fbPa, fbBytes;
    if (!mdpMap) {
        IODeviceMemory *m = IODeviceMemory::withRange(0xae01000, 0x8f000);
        mdpMap = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        if (m) m->release();
        if (!mdpMap) return;
    }
    const volatile uint32_t *mdp = reinterpret_cast<const volatile uint32_t *>(mdpMap->getVirtualAddress());
    const uint32_t a0 = mdp[(0x4000 + 0x14) / 4], s0 = mdp[(0x4000 + 0x24) / 4] & 0xffff;
    const uint32_t a1 = mdp[(0x6000 + 0x14) / 4], s1 = mdp[(0x6000 + 0x24) / 4] & 0xffff;
    const uint32_t width = 1080, height = 2400;
    unsigned lit = 0; uint32_t sum = 0;
    const uint64_t bytes = (uint64_t)s0 * height;
    if (a0 && s0 >= width * 4 && (fbPa != a0 || fbBytes != bytes)) {
        if (fbMap) {fbMap->release(); fbMap = nullptr;}
        IODeviceMemory *m = IODeviceMemory::withRange(a0, bytes);
        fbMap = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        if (m) m->release();
        fbPa = fbMap ? a0 : 0; fbBytes = fbMap ? bytes : 0;
    }
    if (fbMap) {
        const volatile uint8_t *fb = reinterpret_cast<const volatile uint8_t *>(fbMap->getVirtualAddress());
        for (unsigned y = 0; y < 16; ++y)
            for (unsigned x = 0; x < 16; ++x) {
                const uint64_t row = (uint64_t)(y * 2 + 1) * height / 32, col = (uint64_t)(x * 2 + 1) * width / 32;
                const uint32_t px = *reinterpret_cast<const volatile uint32_t *>(fb + row * s0 + col * 4);
                if (px & 0x00ffffffu) ++lit;
                sum = (sum << 5 | sum >> 27) ^ px;
            }
    }
    IOLog("fuxi fb: vig0 %#x/%u vig1 %#x/%u lit %u/256 sum %08x gpu offers %llu retires %llu early-fail %llu fallbacks %u last %#x\n", a0, s0, a1, s1,
          lit, sum, (unsigned long long)native_gop_gpu_offers, (unsigned long long)native_legacy_early_retires,
          (unsigned long long)native_legacy_early_failures, native_legacy_gpu_fallbacks, (unsigned)native_legacy_gpu_last_status);
}

void logThread(void *, wait_result_t) {
    char last[120] = {}, lastGpu[160] = {};
    for (unsigned round = 0;; ++round) {
        sheng_cpufreq_poll();
        if (strncmp(last, native_cpufreq_row, sizeof last)) {
            strlcpy(last, native_cpufreq_row, sizeof last);
            IOLog("fuxi cpufreq: %s\n", last);
        }
        if (round >= 45 || round % 5 == 4) {gpuRow(lastGpu, sizeof lastGpu); frameRow(); cpuRow(); fbRow();}
        IOSleep(round < 45 ? 2000 : 10000);
    }
}
}

extern "C" void fuxi_cpufreq_start(void) {
    static bool started;
    unsigned mode = 0;
    if (started || !PE_parse_boot_argn("sheng-cpufreq", &mode, sizeof mode) || !mode) return;
    started = true;
    sheng_cpufreq_probe();
    if (!native_cpufreq_stage) {IOLog("fuxi cpufreq: probe did not start\n"); return;}
    thread_t t = nullptr;
    if (kernel_thread_start(logThread, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
}
