// SPDX-License-Identifier: BSD-2-Clause-Patent
// Starts the panel brightness service (AppleARMBacklight.cpp) with boot argument sheng-backlight=1: registers that file's classes with the
// kernel's module (its static initializer, which the build takes out of __mod_init_func), then lets it set up the engines and publish.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_backlight_static_init(void) __asm__("__GLOBAL__sub_I_AppleARMBacklight.cpp");
extern "C" void sheng_backlight_start(void);
// See AppleARMBacklight.cpp for the meaning of each entry.
extern "C" {volatile int32_t native_backlight_stats[16];}

extern "C" void sheng_backlight_probe(void) {
    unsigned enabled = 0;
    const bool parsed = PE_parse_boot_argn("sheng-backlight", &enabled, sizeof enabled);
    printf("BLP: enter parsed %d enabled %u stats0 %d\n",(int)parsed,enabled,(int)native_backlight_stats[0]);
    if (!parsed || enabled != 1 || native_backlight_stats[0]) {printf("BLP: early return before load\n");return;}
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    printf("BLP: preModLoad -> %p\n",handle);
    if (!handle) {native_backlight_stats[9] = -12; native_backlight_stats[0] = -5; printf("BLP: preModLoad failed\n");return;}
    native_backlight_static_init();
    const OSReturn status = OSMetaClass::postModLoad(handle);
    printf("BLP: postModLoad -> %d\n",(int)status);
    if (status) {native_backlight_stats[9] = status; native_backlight_stats[0] = -5; printf("BLP: postModLoad failed\n");return;}
    sheng_backlight_start();
    printf("BLP: start returned stats0 %d stats9 %d\n",(int)native_backlight_stats[0],(int)native_backlight_stats[9]);
}
