// SPDX-License-Identifier: BSD-2-Clause-Patent
// fuxi board entry of the QcomDwc3 driver (UniXNU-sheng's package/UsbProbe.cpp, extended 2026-11-25): the controller's classes
// register here, after the hardware answered, the way a module load would register them.
//
// Two things were added for this port, both for bring-up:
//   usb-skip-bringup=1  starts the load/register/publish half alone. The first boot with the stack wired panicked, and the two halves
//                       touch very different ground: bringUp() programs clocks, PHYs and an SMMU context, while the rest only registers
//                       classes and publishes an IOService. Splitting them says which half is at fault without guessing.
//   [FuxiUSB] rows      each step prints one line. usb=1 takes the USB CDC console away, so these reach the panel only through the
//                       message-buffer export that FuxiLog reads; a panic therefore shows how far the worker got.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/OSKextLib.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
extern kmod_info_t g_kernel_kmod_info;
extern "C" bool qcom_dwc3_bring_up(void);
extern "C" void qcom_dwc3_publish(void);
extern "C" void qcom_dwc3_diag(bool);
extern "C" void qcom_dwc3_static_init(void) __asm__("__GLOBAL__sub_I_QcomDwc3.cpp");
extern "C" volatile uint32_t native_usb_stats[54];
extern "C" void qcom_dwc3_host_load(int);
static void worker(void *, wait_result_t) {
    IOSleep(3000);
    unsigned skip = 0, mode = 0;
    PE_parse_boot_argn("usb-skip-bringup", &skip, sizeof skip);
    PE_parse_boot_argn("usb", &mode, sizeof mode);
    printf("[FuxiUSB] stage 1 worker start, usb=%u skip-bringup=%u\n", mode, skip);
    // [0] is the stage/steps flag the driver keeps: negative values name the step that failed.
    if (skip == 1) {
        native_usb_stats[0] = 0;
        printf("[FuxiUSB] stage 2 bringUp skipped\n");
    } else if (!qcom_dwc3_bring_up()) {
        printf("[FuxiUSB] stage 2 bringUp failed, stats0=%d stats1=%#x\n", (int)native_usb_stats[0], native_usb_stats[1]);
        thread_terminate(current_thread());
        return;
    } else {
        printf("[FuxiUSB] stage 2 bringUp ok, stats0=%d core %#x\n", (int)native_usb_stats[0], native_usb_stats[1]);
    }
    // The mirror split: the hardware half alone. With this set the worker stops after bringUp, so a panic here is the controller
    // programming and not a class registration.
    unsigned only = 0;
    PE_parse_boot_argn("usb-only-bringup", &only, sizeof only);
    if (only == 1) {
        printf("[FuxiUSB] stage 2b stopping after bringUp (usb-only-bringup=1)\n");
        thread_terminate(current_thread());
        return;
    }
    // AppleUSBXDCI has no personality of its own, so nothing has loaded it: its constructors and metaclasses, and those of
    // IOUSBDeviceFamily below it, run on this load.
    const OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.driver.AppleUSBXDCI");
    native_usb_stats[21] = (uint32_t)loaded;
    printf("[FuxiUSB] stage 3 AppleUSBXDCI load=%#x\n", (unsigned)loaded);
    if (loaded != kOSReturnSuccess && loaded != kOSKextReturnStartStopError) {
        native_usb_stats[0] = (uint32_t)-9;
        thread_terminate(current_thread());
        return;
    }
    // The same for host mode: Apple's xHCI driver and the USB host family below it.
    qcom_dwc3_host_load((int)OSKextLoadKextWithIdentifier("com.apple.driver.usb.AppleUSBXHCI"));
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    printf("[FuxiUSB] stage 4 preModLoad=%p\n", handle);
    if (!handle) {
        native_usb_stats[0] = (uint32_t)-10;
    } else {
        qcom_dwc3_static_init();
        const int post = OSMetaClass::postModLoad(handle);
        printf("[FuxiUSB] stage 5 postModLoad=%d\n", post);
        if (post) {
            native_usb_stats[0] = (uint32_t)-11;
        } else {
            qcom_dwc3_publish();
            printf("[FuxiUSB] stage 6 publish done\n");
            if (mode == 2 || mode == 3) qcom_dwc3_diag(mode == 3);
        }
    }
    thread_terminate(current_thread());
}
extern "C" void sheng_usb_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("usb", &enabled, sizeof enabled) || !enabled) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}