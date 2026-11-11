// SPDX-License-Identifier: BSD-2-Clause-Patent
// fuxi status bar layout (iOS 17.7 21H16, arm64e): the 1080x2400 panel at 3x is 360x800 pt, a size no iPhone has, so UIKit
// (_UIStatusBar) and SystemStatusUI (STUIStatusBar) choose their Fallback visual provider, which draws only a red "rdar" strip.
// Following the fuxi iPadOS fix (状态栏修复-20261107.md), this library gives both status bars a subclass of the 1080-wide
// DynamicSplitMedium provider with the class method it lacks (+referenceScreenScale = 3.0) and the panel's island rectangle
// (-sensorAreaRect). Unlike that fix it writes no framework static variable: the status bar's own -_visualProviderClass is wrapped
// through the Objective-C runtime and answers the subclass where it would answer Fallback, so no address is version-specific.
// Status bars built before the library loaded are rebuilt from -layoutSubviews and from a 2 s scan of all windows (the lock screen's
// bar no longer lays out). The system still draws the time, battery and every other item itself. Skipped in backboardd.
// Loaded as a weak dylib of the NativeAdrenoMetal plugin (device-ios17/scripts/build-fuxi-statusbar.py).
#include <objc/runtime.h>
#include <objc/message.h>
#include <dispatch/dispatch.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double x, y, w, h; } FsbRect;   // CGRect's layout (two CGPoint/CGSize double pairs)

enum { kUI = 0, kST = 1, kKinds = 2, kMaxRebuilds = 12, kScanRounds = 31, kInstallTries = 30 };
static const char *const kBar[kKinds] = {"_UIStatusBar", "STUIStatusBar"};
static const char *const kFallback[kKinds] = {"_UIStatusBarVisualProvider_Fallback", "STUIStatusBarVisualProvider_Fallback"};
static const char *const kBase[kKinds] = {"_UIStatusBarVisualProvider_DynamicSplitMedium", "STUIStatusBarVisualProvider_DynamicSplitMedium"};
static const char *const kSub[kKinds] = {"FuxiUIStatusBarVisualProvider", "FuxiSTUIStatusBarVisualProvider"};
static Class gBar[kKinds], gFallback[kKinds], gSub[kKinds];
static IMP gOrigProviderClass[kKinds], gOrigLayout[kKinds];
static int gRebuilds[kKinds];

static double fsbReferenceScreenScale(id self, SEL cmd) { (void)self; (void)cmd; return 3.0; }
// The island on this panel, in points, as measured on the fuxi screenshots of the iPadOS fix.
static FsbRect fsbSensorAreaRect(id self, SEL cmd) { (void)self; (void)cmd; return (FsbRect){118, 12, 125, 36}; }

static Class fsbProviderClass(int k, id self, SEL cmd) {
    Class chosen = ((Class (*)(id, SEL))gOrigProviderClass[k])(self, cmd);
    return (chosen == gFallback[k] && gSub[k]) ? gSub[k] : chosen;
}
static Class fsbProviderClassUI(id self, SEL cmd) { return fsbProviderClass(kUI, self, cmd); }
static Class fsbProviderClassST(id self, SEL cmd) { return fsbProviderClass(kST, self, cmd); }

// A bar still holding a Fallback provider (built before the wrap) is rebuilt; at most kMaxRebuilds per kind, in case the choice still
// comes back as Fallback.
static void fsbRebuild(int k, id bar) {
    if (!gSub[k] || gRebuilds[k] >= kMaxRebuilds) return;
    id provider = ((id (*)(id, SEL))objc_msgSend)(bar, sel_registerName("visualProvider"));
    if (!provider || object_getClass(provider) != gFallback[k]) return;
    ++gRebuilds[k];
    ((void (*)(id, SEL))objc_msgSend)(bar, sel_registerName("resetVisualProvider"));
    Ivar ivar = class_getInstanceVariable(gBar[k], "_visualProvider");
    if (ivar) object_setIvar(bar, ivar, nil);
    ((void (*)(id, SEL, Class))objc_msgSend)(bar, sel_registerName("_setVisualProviderClass:"), Nil);
    ((void (*)(id, SEL))objc_msgSend)(bar, sel_registerName("_prepareVisualProviderIfNeeded"));
    ((void (*)(id, SEL))objc_msgSend)(bar, sel_registerName("setNeedsLayout"));
}
static void fsbLayout(int k, id self, SEL cmd) {
    ((void (*)(id, SEL))gOrigLayout[k])(self, cmd);
    fsbRebuild(k, self);
}
static void fsbLayoutUI(id self, SEL cmd) { fsbLayout(kUI, self, cmd); }
static void fsbLayoutST(id self, SEL cmd) { fsbLayout(kST, self, cmd); }

static bool fsbInstall(int k) {
    if (gOrigProviderClass[k]) return true;
    Class bar = objc_getClass(kBar[k]), fallback = objc_getClass(kFallback[k]), base = objc_getClass(kBase[k]);
    if (!bar || !fallback || !base) return false;
    Class sub = objc_getClass(kSub[k]);
    if (!sub) {
        sub = objc_allocateClassPair(base, kSub[k], 0);
        if (!sub) return false;
        class_addMethod(object_getClass((id)sub), sel_registerName("referenceScreenScale"), (IMP)fsbReferenceScreenScale, "d16@0:8");
        class_addMethod(sub, sel_registerName("sensorAreaRect"), (IMP)fsbSensorAreaRect, "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8");
        objc_registerClassPair(sub);
    }
    Method chooser = class_getInstanceMethod(bar, sel_registerName("_visualProviderClass"));
    Method layout = class_getInstanceMethod(bar, sel_registerName("layoutSubviews"));
    if (!chooser || !layout) return false;
    gBar[k] = bar; gFallback[k] = fallback; gSub[k] = sub;
    gOrigLayout[k] = method_setImplementation(layout, k == kUI ? (IMP)fsbLayoutUI : (IMP)fsbLayoutST);
    gOrigProviderClass[k] = method_setImplementation(chooser, k == kUI ? (IMP)fsbProviderClassUI : (IMP)fsbProviderClassST);
    return true;
}

static void fsbWalk(id view, unsigned depth) {
    if (!view || depth > 14) return;
    for (int k = 0; k < kKinds; ++k)
        if (gBar[k] && ((bool (*)(id, SEL, Class))objc_msgSend)(view, sel_registerName("isKindOfClass:"), gBar[k])) fsbRebuild(k, view);
    id subviews = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("subviews"));
    unsigned long count = subviews ? ((unsigned long (*)(id, SEL))objc_msgSend)(subviews, sel_registerName("count")) : 0;
    for (unsigned long i = 0; i < count; ++i)
        fsbWalk(((id (*)(id, SEL, unsigned long))objc_msgSend)(subviews, sel_registerName("objectAtIndex:"), i), depth + 1);
}
static void fsbScanWindows(void) {
    Class window = objc_getClass("UIWindow");
    if (!window) return;
    id windows = nil;
    SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
    if (class_getClassMethod(window, all)) windows = ((id (*)(id, SEL, bool, bool))objc_msgSend)((id)window, all, true, false);
    unsigned long count = windows ? ((unsigned long (*)(id, SEL))objc_msgSend)(windows, sel_registerName("count")) : 0;
    for (unsigned long i = 0; i < count; ++i)
        fsbWalk(((id (*)(id, SEL, unsigned long))objc_msgSend)(windows, sel_registerName("objectAtIndex:"), i), 0);
}

static int gTick;
static void fsbTick(void *context) {
    (void)context;
    for (int k = 0; k < kKinds; ++k) (void)fsbInstall(k);   // SystemStatusUI may load after the plugin
    fsbScanWindows();
    if (++gTick < (kScanRounds > kInstallTries ? kScanRounds : kInstallTries))
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, fsbTick);
}

__attribute__((constructor)) static void fsbStart(void) {
    const char *name = getprogname();
    if (name && !strcmp(name, "backboardd")) return;
    for (int k = 0; k < kKinds; ++k) (void)fsbInstall(k);
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, fsbTick);
}
