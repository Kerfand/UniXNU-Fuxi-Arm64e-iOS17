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
#include <stdio.h>
#include <IOKit/IOKitLib.h>

// The kernel IOKit probe (unixnu-iokit-probe=1) logs every service lookup with the caller's name; a lookup of a class that does not
// exist is the library's only diagnostic channel on a device without a readable unified log.
static void fsbBeacon(const char *what) {
    char name[200];   // the scr: row ends with the screen type: 96 bytes cut it off
    snprintf(name, sizeof name, "FSB:%s", what);
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(name));
    if (s) IOObjectRelease(s);
}

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

static int gChoiceReported[kKinds];
static Class fsbProviderClass(int k, id self, SEL cmd) {
    Class chosen = ((Class (*)(id, SEL))gOrigProviderClass[k])(self, cmd);
    if (gChoiceReported[k] < 3) {
        char what[80];
        snprintf(what, sizeof what, "choice%d:%s", k, chosen ? class_getName(chosen) : "nil");
        ++gChoiceReported[k]; fsbBeacon(what);
    }
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
    fsbBeacon(k == kUI ? "rebuild0" : "rebuild1");
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
    fsbBeacon(k == kUI ? "installed0" : "installed1");
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


// fuxi layout probe (2026-11-13, Control Center cut on the right, keyboard taps dead): the geometry UIKit, Control Center and the keyboard
// actually use, as beacons (the kernel IOKit probe always logs "FSB" lookups). Read-only.
#include <dlfcn.h>
typedef struct { double top, left, bottom, right; } FsbInsets;
static void fsbRect(char *out, size_t size, const char *tag, FsbRect r) { snprintf(out, size, "%s=%.1f,%.1f,%.1fx%.1f", tag, r.x, r.y, r.w, r.h); }
static int gProbeRounds;
static void fsbLayoutProbe(void) {
    if (gProbeRounds >= 4) return;
    Class screenClass = objc_getClass("UIScreen");
    if (!screenClass) return;
    id screen = ((id (*)(id, SEL))objc_msgSend)((id)screenClass, sel_registerName("mainScreen"));
    if (!screen) return;
    ++gProbeRounds;
    char what[200], a[64], b[64], c[64];
    FsbRect bounds = ((FsbRect (*)(id, SEL))objc_msgSend)(screen, sel_registerName("bounds"));
    FsbRect ref = ((FsbRect (*)(id, SEL))objc_msgSend)(screen, sel_registerName("_referenceBounds"));
    FsbRect native = ((FsbRect (*)(id, SEL))objc_msgSend)(screen, sel_registerName("nativeBounds"));
    double scale = ((double (*)(id, SEL))objc_msgSend)(screen, sel_registerName("scale"));
    double nscale = ((double (*)(id, SEL))objc_msgSend)(screen, sel_registerName("nativeScale"));
    long type = ((long (*)(id, SEL))objc_msgSend)(screen, sel_registerName("_screenType"));
    fsbRect(a, sizeof a, "b", bounds); fsbRect(b, sizeof b, "ref", ref); fsbRect(c, sizeof c, "nat", native);
    snprintf(what, sizeof what, "scr:%s:%s:%s:%s:s%.2f:ns%.3f:t%ld", getprogname(), a, b, c, scale, nscale, type);
    fsbBeacon(what);
    double (*edge)(void) = (double (*)(void))dlsym(RTLD_DEFAULT, "CCUIItemEdgeSize");
    double (*gutter)(void) = (double (*)(void))dlsym(RTLD_DEFAULT, "CCUILayoutGutter");
    FsbRect (*ccref)(void) = (FsbRect (*)(void))dlsym(RTLD_DEFAULT, "CCUIReferenceScreenBounds");
    FsbInsets (*insets)(long) = (FsbInsets (*)(long))dlsym(RTLD_DEFAULT, "CCUILayoutEdgeInsetsForInterfaceOrientation");
    if (edge || gutter || ccref || insets) {
        FsbRect r = ccref ? ccref() : (FsbRect){0, 0, 0, 0};
        FsbInsets in = insets ? insets(1) : (FsbInsets){0, 0, 0, 0};
        snprintf(what, sizeof what, "cc:%s:edge%.1f:gut%.1f:ref%.1fx%.1f:ins%.1f,%.1f,%.1f,%.1f", getprogname(), edge ? edge() : -1.0,
                 gutter ? gutter() : -1.0, r.w, r.h, in.top, in.left, in.bottom, in.right);
        fsbBeacon(what);
    }
    Class kb = objc_getClass("UIKeyboardImpl");
    if (kb) {
        typedef struct { double w, h; } FsbSize;
        FsbSize s = ((FsbSize (*)(id, SEL, long))objc_msgSend)((id)kb, sel_registerName("defaultSizeForInterfaceOrientation:"), 1);
        snprintf(what, sizeof what, "kb:%s:%.1fx%.1f", getprogname(), s.w, s.h);
        fsbBeacon(what);
    }
}
// fuxi audio route probe (2026-11-13): the audio server loads the ShengAudio plug-in and answers every property, but never starts output
// for a ringtone preview. What this process's audio session routes to (AVAudioSession currentRoute) and what the system offers as
// routes (AVSystemController PickableRoutes) tell whether iOS made a port of the plug-in's device at all. Two rounds, beacons "FSB:au:".
#include <dlfcn.h>
static id fsbStr(const char *s) {
    return ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), s);
}
static const char *fsbUtf8(id o) {
    if (!o) return "-";
    id d = ((id (*)(id, SEL))objc_msgSend)(o, sel_registerName("description"));
    const char *s = d ? ((const char *(*)(id, SEL))objc_msgSend)(d, sel_registerName("UTF8String")) : NULL;
    return s ? s : "?";
}
static long fsbCount(id a) {return a ? ((long (*)(id, SEL))objc_msgSend)(a, sel_registerName("count")) : -1;}
static id fsbAt(id a, long i) {return ((id (*)(id, SEL, long))objc_msgSend)(a, sel_registerName("objectAtIndex:"), i);}
static id fsbGet(id o, const char *sel) {return o ? ((id (*)(id, SEL))objc_msgSend)(o, sel_registerName(sel)) : NULL;}
static int gAudioRounds;
static void fsbAudioProbe(void) {
    if (gAudioRounds >= 2) return;
    ++gAudioRounds;
    char what[200];
    const char *prog = getprogname();
    dlopen("/System/Library/Frameworks/AVFAudio.framework/AVFAudio", RTLD_LAZY);
    Class sc = objc_getClass("AVAudioSession");
    if (sc) {
        id ses = fsbGet((id)sc, "sharedInstance");
        id route = fsbGet(ses, "currentRoute");
        id outs = fsbGet(route, "outputs"), ins = fsbGet(route, "inputs");
        snprintf(what, sizeof what, "au:%s:r%d:cat=%s:outs%ld:ins%ld", prog, gAudioRounds, fsbUtf8(fsbGet(ses, "category")), fsbCount(outs), fsbCount(ins));
        fsbBeacon(what);
        for (long i = 0; i < fsbCount(outs) && i < 4; ++i) {
            id port = fsbAt(outs, i);
            snprintf(what, sizeof what, "au:%s:out%ld:%s:%s:%s", prog, i, fsbUtf8(fsbGet(port, "portType")), fsbUtf8(fsbGet(port, "portName")),
                     fsbUtf8(fsbGet(port, "UID")));
            fsbBeacon(what);
        }
        id avail = fsbGet(ses, "availableInputs");
        for (long i = 0; i < fsbCount(avail) && i < 4; ++i) {
            id port = fsbAt(avail, i);
            snprintf(what, sizeof what, "au:%s:in%ld:%s:%s", prog, i, fsbUtf8(fsbGet(port, "portType")), fsbUtf8(fsbGet(port, "portName")));
            fsbBeacon(what);
        }
    } else {
        snprintf(what, sizeof what, "au:%s:noAVAudioSession", prog);
        fsbBeacon(what);
    }
    dlopen("/System/Library/PrivateFrameworks/Celestial.framework/Celestial", RTLD_LAZY);
    Class ac = objc_getClass("AVSystemController");
    id ctl = ac ? fsbGet((id)ac, "sharedAVSystemController") : NULL;
    if (ctl) {
        id routes = ((id (*)(id, SEL, id))objc_msgSend)(ctl, sel_registerName("attributeForKey:"), fsbStr("AVSystemController_PickableRoutesAttribute"));
        snprintf(what, sizeof what, "au:%s:pick%ld", prog, fsbCount(routes));
        fsbBeacon(what);
        for (long i = 0; i < fsbCount(routes) && i < 6; ++i) {
            id r = fsbAt(routes, i);
            id name = ((id (*)(id, SEL, id))objc_msgSend)(r, sel_registerName("objectForKey:"), fsbStr("RouteName"));
            id type = ((id (*)(id, SEL, id))objc_msgSend)(r, sel_registerName("objectForKey:"), fsbStr("AVAudioRouteName"));
            id uid = ((id (*)(id, SEL, id))objc_msgSend)(r, sel_registerName("objectForKey:"), fsbStr("RouteUID"));
            snprintf(what, sizeof what, "au:%s:pick%ld:%s:%s:%s", prog, i, fsbUtf8(name), fsbUtf8(type), fsbUtf8(uid));
            fsbBeacon(what);
        }
    }
}
static int gTick;
static void fsbTick(void *context) {
    (void)context;
    for (int k = 0; k < kKinds; ++k) (void)fsbInstall(k);   // SystemStatusUI may load after the plugin
    fsbScanWindows();
    if (gTick == 3 || gTick == 15) fsbLayoutProbe();
    if (gTick == 10 || gTick == 28) fsbAudioProbe();
    if (++gTick < (kScanRounds > kInstallTries ? kScanRounds : kInstallTries))
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, fsbTick);
}

__attribute__((constructor)) static void fsbStart(void) {
    const char *name = getprogname();
    if (name && !strcmp(name, "backboardd")) return;
    fsbBeacon("loaded");
    for (int k = 0; k < kKinds; ++k) (void)fsbInstall(k);
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, fsbTick);
}
