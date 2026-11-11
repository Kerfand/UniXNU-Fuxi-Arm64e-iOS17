"""autotap.py (2026-11-11): unattended synthetic taps through the real touch path (published contact set -> sink ->
NativeHidInput::touchSend -> IOHIDEventDriver -> backboardd). Boot-arg fuxi-touch-autotap=<s after the HID start
settled> (0/absent = off), fuxi-touch-autotap-count (default 3, max 8), interval 15 s, hold 80 ms. Positions are in
the mapped space (0..30480 long, 0..20320 short): centre, then quarter points."""
from pathlib import Path
f = Path('/home/xiaomi/sadnut-fuxi-local-20261103/device/package/FuxiTouchProbe.cpp')
t = f.read_text()
helper_anchor = "bool snapshot(Session &s) {\n"
helper = (
"// Synthetic contact in mapped coordinates, published exactly like a drained frame (fuxi-touch-autotap).\n"
"static volatile uint32_t gAutotapSent;\n"
"static void publishSynthetic(unsigned count, int x, int y) {\n"
"    native_touch_seq = native_touch_seq + 1;\n"
"    __asm__ volatile(\"dmb ish\" ::: \"memory\");\n"
"    native_touch_count = count;\n"
"    if (count) {native_touch_xs[0] = x; native_touch_ys[0] = y; native_touch_ids[0] = 9;}\n"
"    __asm__ volatile(\"dmb ish\" ::: \"memory\");\n"
"    native_touch_seq = native_touch_seq + 1;\n"
"    if (void (*sink)(void *) = __atomic_load_n(&gTouchSink, __ATOMIC_ACQUIRE)) sink(gTouchSinkContext);\n"
"}\n"
"static uint64_t nowMs(void) {uint64_t a = mach_absolute_time(), ns = 0; absolutetime_to_nanoseconds(a, &ns); return ns / 1000000;}\n\n")
if 'publishSynthetic' not in t:
    assert t.count(helper_anchor) == 1
    t = t.replace(helper_anchor, helper + helper_anchor)
loop_old = ("            if (!settled && fuxi_hid_settled()) {settled = true; leavesBefore = s->core.leaves;}\n")
loop_new = (loop_old +
"            if (autotapSec && settled && gAutotapSent < autotapCount) {\n"
"                static const int kTapX[3] = {15240, 7620, 22860}, kTapY[3] = {10160, 5080, 5080};\n"
"                const uint64_t now = nowMs();\n"
"                if (!autotapAt) autotapAt = now + (uint64_t)autotapSec * 1000;\n"
"                if (!autotapDown && now >= autotapAt) {\n"
"                    const unsigned k = gAutotapSent % 3;\n"
"                    publishSynthetic(1, kTapX[k], kTapY[k]);\n"
"                    autotapDown = true; autotapAt = now + 80;\n"
"                } else if (autotapDown && now >= autotapAt) {\n"
"                    publishSynthetic(0, 0, 0);\n"
"                    autotapDown = false; gAutotapSent = gAutotapSent + 1; autotapAt = now + 15000;\n"
"                    printf(\"FTS autotap %u rep%u up%u/%x fail%u\\n\", gAutotapSent, native_touch_hid_reports,\n"
"                           native_hid_input_events_weak(6), (unsigned)native_hid_input_result_weak(6), native_hid_input_events_weak(7));\n"
"                }\n"
"            }\n")
if 'FTS autotap' not in t:
    assert t.count(loop_old) == 1, 'loop anchor'
    t = t.replace(loop_old, loop_new)
decl_old = "        unsigned pollOnly = 0;\n"
decl_new = ("        unsigned autotapSec = 0, autotapCount = 3;\n"
            "        PE_parse_boot_argn(\"fuxi-touch-autotap\", &autotapSec, sizeof autotapSec);\n"
            "        PE_parse_boot_argn(\"fuxi-touch-autotap-count\", &autotapCount, sizeof autotapCount);\n"
            "        if (autotapCount > 8) autotapCount = 8;\n"
            "        uint64_t autotapAt = 0;\n"
            "        bool autotapDown = false;\n" + decl_old)
if 'fuxi-touch-autotap"' not in t:
    assert t.count(decl_old) == 1, 'decl anchor'
    t = t.replace(decl_old, decl_new)
# HidInputProbe counters (weak, as the probe can be built without it).
acc_anchor = "static bool gHidRegistryPrinted;\n"
acc = ("extern \"C\" volatile uint32_t native_hid_input_events[8] __attribute__((weak_import));\n"
       "extern \"C\" volatile int native_hid_input_results[8] __attribute__((weak_import));\n"
       "static unsigned native_hid_input_events_weak(unsigned i) {return &native_hid_input_events ? native_hid_input_events[i] : 0;}\n"
       "static int native_hid_input_result_weak(unsigned i) {return &native_hid_input_results ? native_hid_input_results[i] : -1;}\n")
if 'native_hid_input_events_weak' not in t:
    assert t.count(acc_anchor) == 1
    t = t.replace(acc_anchor, acc + acc_anchor)
f.write_text(t)
print('ok')
p = Path('/home/xiaomi/sadnut-fuxi-local-20261103/pack-ios17-hidpath.py')
s = p.read_text()
if 'fuxi-touch-autotap' not in s:
    o = "        + 'dk=0 '\n"
    assert s.count(o) == 1
    s = s.replace(o, o + "        # Unattended synthetic taps (FuxiTouchProbe fuxi-touch-autotap): 60 s after the HID start settled, 3 taps 15 s apart.\n"
                         "        + 'fuxi-touch-autotap=60 '\n")
    p.write_text(s)
print('pack ok')
