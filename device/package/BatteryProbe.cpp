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
extern "C" {extern volatile int32_t native_battery_raw[10]; extern volatile int32_t native_charger_stats[16];}
extern "C" {extern volatile uint64_t armcpu_sgi_unexpected __attribute__((weak_import));
            extern volatile uint32_t armcpu_sgi_unexpected_id __attribute__((weak_import)), armcpu_sgi_unexpected_core __attribute__((weak_import)),
                                     armcpu_sgi_unexpected_mask __attribute__((weak_import));}
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <string.h>
static void fuxiBatteryLog(void *, wait_result_t) {
    int32_t last[16] = {}, lastC[16] = {};
    for (unsigned tick = 0;; ++tick) {
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
}
