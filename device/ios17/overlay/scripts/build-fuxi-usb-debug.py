#!/usr/bin/env python3
"""Build and qualify the selected fuxi USB archive using actual XNU records."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

root = Path(__file__).resolve().parents[1]
module = root / '.cache/fuxi-qud-module'
# A restored link-only reference already has compile records, so the framework
# may skip its bootstrap recipe. Stage/apply USB before this archive as well.
subprocess.run([sys.executable, root / 'scripts/prepare-fuxi-usb-debug.py',
                '--module', root / 'drivers/QcomXnuUsbDebug', '--xnu', root / 'refer/xnu',
                '--output', module], check=True)
# iOS 17 fileset: the kernel __DATA has to fit ahead of the retained kext data of the original
# collection (0x171070 bytes on 21H16), so the 4 MiB log ring and the 16 MiB screenshot buffer shrink.
# Checkpoint 2 also links NativeUfs (~174 KiB of __DATA/__bss), so the ring drops to 32 KiB; the USB
# log is drained live, the ring only covers the gap before the host attaches.
# 2026-11-08: the AppleKeyStore stand-in put the __DATA end 0x170 bytes past its last page, and the page-aligned QUD DMA buffer
# swallows savings made before it; the screenshot buffer (after it, and too small for this panel anyway) gives 4 KiB back. The ring
# stays a multiple of the 16 KiB write budget its model test writes in.
log_header = module / 'kernel/QcomUsbBuffer.h'
log_text = log_header.read_text()
for stale in ('(4u * 1024u * 1024u)', '(192u * 1024u)', '(30u * 1024u)'):
    log_text = log_text.replace('#define SOC_USB_LOG_BUFFER_BYTES ' + stale, '#define SOC_USB_LOG_BUFFER_BYTES (4u * 1024u * 1024u)')
log_header.write_text(log_text)
shot_header = module / 'include/QcomUsbScreenshot.h'
shot_header.write_text(shot_header.read_text().replace('#define QUD_SCREENSHOT_MAX_BYTES (16u * 1024u)\n',
                                                        '#define QUD_SCREENSHOT_MAX_BYTES (16u * 1024u * 1024u)\n'))
for relative, old, new in (('kernel/QcomUsbBuffer.h', '#define SOC_USB_LOG_BUFFER_BYTES (4u * 1024u * 1024u)',
                            '#define SOC_USB_LOG_BUFFER_BYTES (32u * 1024u)'),
                           ('include/QcomUsbScreenshot.h', '#define QUD_SCREENSHOT_MAX_BYTES (16u * 1024u * 1024u)',
                            '#define QUD_SCREENSHOT_MAX_BYTES (12u * 1024u)'),
                           ('tests/test_usb_buffer.c', 'ring.used == 4u * 1024u * 1024u',
                            'ring.used == SOC_USB_LOG_BUFFER_BYTES')):
    header = module / relative
    text = header.read_text()
    if old in text:
        header.write_text(text.replace(old, new, 1))
    elif new not in text:
        raise SystemExit(f'{relative}: buffer size anchor changed')
# Only the iOS 17 fuxi archive opts into the post-drain hook.
shutil.copy2(root / 'kits/sheng/BootResetCore.h', module / 'kernel/BootResetCore.h')
board_header = module / 'soc/FuxiSm8550.h'
if 'FUXI_PANIC_FASTBOOT' not in board_header.read_text():
    with board_header.open('a') as stream:
        stream.write('\n#define FUXI_PANIC_FASTBOOT 1\n')
if 'fuxi_panic_fastboot(void)' not in (module / 'kernel/QcomXnuUsbDebug.c').read_text():
    subprocess.run(['patch','-p1','--batch'],cwd=module,input='--- a/kernel/QcomXnuUsbDebug.c\n+++ b/kernel/QcomXnuUsbDebug.c\n@@ -102,6 +102,44 @@\n #endif\n     for (;;) __asm__ volatile("wfe" ::: "memory");\n }\n+\n+#ifdef FUXI_PANIC_FASTBOOT\n+/* Prepare mappings in normal context. No allocation or thread services after panic. */\n+#include "BootResetCore.h"\n+static struct br_arbiter reset_arb;\n+static volatile uint32_t *reset_obs, *reset_chn;\n+static void fuxi_prepare_fastboot(void)\n+{\n+    int seconds = 0;\n+    struct br_found f;\n+    if (!PE_parse_boot_argn("fuxi-panic-fastboot", &seconds, sizeof(seconds)) || seconds != 30)\n+        return;\n+    reset_arb.core = (void *)ml_io_map(0xc400000, 0x3000);\n+    reset_arb.cnfg = (void *)ml_io_map(0xc42d000, 0x4000);\n+    if (!reset_arb.core || !reset_arb.cnfg || br_find(&reset_arb, &f) || f.owner != 0)\n+        return;\n+    unsigned off = BR_OBS_APID_STRIDE * (unsigned)f.apid;\n+    vm_offset_t obs = ml_io_map(0xc440000 + (off & ~4095u), 4096);\n+    if (!obs) return;\n+    reset_obs = (void *)(obs + (off & 4095u));\n+    reset_chn = (void *)ml_io_map(0xc500000 + BR_CHNL_APID_STRIDE * (unsigned)f.apid, 4096);\n+}\n+static void fuxi_panic_fastboot(void)\n+{\n+    uint8_t before, after;\n+    uint64_t hz = native.status.counter_frequency;\n+    if (!reset_obs || !reset_chn || !hz || hz > COUNTER_MAX_FREQUENCY) return;\n+    uint64_t start = counter();\n+    while (counter() - start < 30 * hz) __asm__ volatile("yield" ::: "memory");\n+    /* Both pointers already select this APID, EE0, so channel offset is zero. */\n+    if (br_set_bootloader(reset_obs, reset_chn, &reset_arb, 0, &before, &after)) return;\n+    __asm__ volatile("movz w0, #9\\nmovk w0, #0x8400, lsl #16\\n"\n+                     "mov x1, xzr\\nmov x2, xzr\\nmov x3, xzr\\ndsb sy\\nsmc #0"\n+                     ::: "x0","x1","x2","x3","x4","x5","x6","x7","x8","x9",\n+                         "x10","x11","x12","x13","x14","x15","x16","x17","memory","cc");\n+}\n+#endif\n+\n static int qcom_usb_control_halt_restart(unsigned int type)\n {\n     if (type == CONTROL_PE_RESTART_CPU &&\n@@ -808,6 +846,9 @@\n     usb_diag_polling = 0;\n     if (usb_diag_mode >= 201u && usb_diag_mode <= 203u) diag_reset(12);\n     diag_checkpoint(QCOM_USB_DEBUG_DIAG_ENUM_DONE);\n+#ifdef FUXI_PANIC_FASTBOOT\n+    fuxi_prepare_fastboot();\n+#endif\n     native.status.phase = QCOM_USB_DEBUG_ACTIVE;\n     ret = 1;\n out:\n@@ -1270,6 +1311,9 @@\n     static const char end[] = "\\nQCOM PANIC END\\n";\n     uint64_t flags, now;\n     unsigned int iterations = 200000u, marker_queued = 0, announced = 0;\n+#ifdef FUXI_PANIC_FASTBOOT\n+    int finished = 0;\n+#endif\n     char announcement[192];\n     size_t announcement_bytes;\n     if (usb_diag_enabled && (native.status.phase == QCOM_USB_DEBUG_PREPARING ||\n@@ -1307,7 +1351,12 @@\n         }\n         if (!poll_owned()) break;\n         if (marker_queued && dwc3_queued(native.core) == 0 &&\n-            soc_usb_buffer_pending(&usb_log_buffer) == 0) break;\n+            soc_usb_buffer_pending(&usb_log_buffer) == 0) {\n+#ifdef FUXI_PANIC_FASTBOOT\n+            finished = 1;\n+#endif\n+            break;\n+        }\n         delay_us(0, 10);\n     }\n     native.deadline_active = 0;\n@@ -1318,6 +1367,9 @@\n out:\n     buffer_status_owned();\n     release(flags);\n+#ifdef FUXI_PANIC_FASTBOOT\n+    if (finished) fuxi_panic_fastboot();\n+#endif\n     if (usb_diag_enabled) diag_reset(10);\n }\n int qcom_usb_debug_stop(void)\n',text=True,check=True)

# Reset layer for the upstream control verbs (ljhhkx529/QcomXnuUsbDebug: REBOOT and REBOOT-FASTBOOT call
# qcom_reset_request(mode) directly instead of going through the halt hook). fuxi provides it here on top of
# the BootResetCore mappings: the SPMI windows are now prepared whenever the USB console comes up, while the
# automatic 30 s panic -> fastboot path still needs fuxi-panic-fastboot=30.
debug_source = module / 'kernel/QcomXnuUsbDebug.c'
debug_text = debug_source.read_text()
if 'qcom_reset_request(int mode)\n{' not in debug_text:
    for old, new in (
            ('static volatile uint32_t *reset_obs, *reset_chn;\n',
             'static volatile uint32_t *reset_obs, *reset_chn;\nstatic int fuxi_panic_fastboot_armed;\n'),
            ('    if (!PE_parse_boot_argn("fuxi-panic-fastboot", &seconds, sizeof(seconds)) || seconds != 30)\n        return;\n',
             '    fuxi_panic_fastboot_armed = PE_parse_boot_argn("fuxi-panic-fastboot", &seconds, sizeof(seconds)) &&\n'
             '                                seconds == 30;\n'),
            ('    if (!reset_obs || !reset_chn || !hz || hz > COUNTER_MAX_FREQUENCY) return;\n',
             '    if (!fuxi_panic_fastboot_armed || !reset_obs || !reset_chn || !hz || hz > COUNTER_MAX_FREQUENCY) return;\n'),
            ('#endif\n\nstatic int qcom_usb_control_halt_restart',
             '/* Control-channel reset layer: mode 1 (REBOOT-FASTBOOT) sets the PON bootloader reason first, mode 0\n'
             ' * (REBOOT) resets plainly. Called by the USB worker in thread context after the ACK drained. Returns\n'
             ' * only when the request could not be carried out. */\n'
             'int qcom_reset_request(int mode);\n'
             'int qcom_reset_request(int mode)\n'
             '{\n'
             '    uint8_t before, after;\n'
             '    if (mode == 1 && (!reset_obs || !reset_chn ||\n'
             '                      br_set_bootloader(reset_obs, reset_chn, &reset_arb, 0, &before, &after)))\n'
             '        return 0;\n'
             '    __asm__ volatile("movz w0, #9\\nmovk w0, #0x8400, lsl #16\\n"\n'
             '                     "mov x1, xzr\\nmov x2, xzr\\nmov x3, xzr\\ndsb sy\\nsmc #0"\n'
             '                     ::: "x0","x1","x2","x3","x4","x5","x6","x7","x8","x9",\n'
             '                         "x10","x11","x12","x13","x14","x15","x16","x17","memory","cc");\n'
             '    return 0;\n'
             '}\n'
             '#endif\n\nstatic int qcom_usb_control_halt_restart')):
        if debug_text.count(old) != 1:
            raise SystemExit('QcomXnuUsbDebug.c: reset layer anchor changed: ' + old.splitlines()[0])
        debug_text = debug_text.replace(old, new, 1)
    debug_source.write_text(debug_text)

# 2026-11-12: panic -> fastboot whatever the CDC state. The drain above only reached fuxi_panic_fastboot() once the host had read
# every byte (finished): with no host reading, a stalled IN endpoint (deadline expired) or the console no longer ACTIVE at panic,
# the phone stayed in the panic spin. Now every exit of the drain takes the 30 s -> PON bootloader -> PSCI reset path; it still
# needs fuxi-panic-fastboot=30 and the reset windows that were mapped when the console came up (no mapping, no reset).
debug_text = debug_source.read_text()
if 'fuxi_panic_fastboot_any_cdc_state' not in debug_text:
    for old, new in (
            ('    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE || !acquire(&flags)) return;\n'
             '    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE) goto out;\n'
             '    {\n'
             '        char *p = append_text(announcement, "\\nQCOM USB PANIC pending_at_start=");\n',
             '    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE || !acquire(&flags)) {\n'
             '#ifdef FUXI_PANIC_FASTBOOT\n'
             '        fuxi_panic_fastboot();   /* fuxi_panic_fastboot_any_cdc_state: console not ACTIVE or guard busy */\n'
             '#endif\n'
             '        return;\n'
             '    }\n'
             '    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE) goto out;\n'
             '    {\n'
             '        char *p = append_text(announcement, "\\nQCOM USB PANIC pending_at_start=");\n'),
            ('    if (finished) fuxi_panic_fastboot();\n',
             '    /* Drained or not (no host reading, IN endpoint stalled, deadline expired): fastboot either way. */\n'
             '    (void)finished;\n'
             '    fuxi_panic_fastboot();\n')):
        if debug_text.count(old) != 1:
            raise SystemExit('QcomXnuUsbDebug.c: panic fastboot anchor changed: ' + old.splitlines()[0])
        debug_text = debug_text.replace(old, new, 1)
    debug_source.write_text(debug_text)

# 2026-11-12: USB serial screenshot and phone control. The INPUT verb (touch and buttons) is the one verb that takes arguments, every
# other verb keeps its exact grammar; its action runs after the reply is queued and the sequence committed (like REBOOT), so a replayed
# frame does nothing. Screenshot storage can be supplied at run time: the static buffer stays the default (the module's tests use it),
# the display driver hands over a buffer the size of its downscaled scanout (the fileset's __DATA cannot hold one).
def qud_edit(relative, pairs):
    path = module / relative
    text = path.read_text()
    if 'fuxi-usb-control-20261112' in text:
        return
    for old, new in pairs:
        if text.count(old) != 1:
            raise SystemExit(f'{relative}: usb control anchor changed: ' + old.splitlines()[0])
        text = text.replace(old, new, 1)
    path.write_text(text)
qud_edit('kernel/QcomUsbControl.h', (
    ('    SXNU_CONTROL_VERB_REBOOT_FASTBOOT\n};\n',
     '    SXNU_CONTROL_VERB_REBOOT_FASTBOOT,\n'
     '    /* fuxi-usb-control-20261112: synthetic input, <2hex op> <4hex a> <4hex b> after the verb. */\n'
     '    SXNU_CONTROL_VERB_INPUT\n};\n'),
    ('    enum sxnu_control_verb verb;\n};\n',
     '    enum sxnu_control_verb verb;\n    uint32_t arg[3];   /* INPUT only */\n};\n'),
    ('        text[7] == \'T\') return SXNU_CONTROL_VERB_SHOTNEXT;\n    return SXNU_CONTROL_VERB_INVALID;\n',
     '        text[7] == \'T\') return SXNU_CONTROL_VERB_SHOTNEXT;\n'
     '    if (bytes == 5u && text[0] == \'I\' && text[1] == \'N\' && text[2] == \'P\' &&\n'
     '        text[3] == \'U\' && text[4] == \'T\') return SXNU_CONTROL_VERB_INPUT;\n'
     '    return SXNU_CONTROL_VERB_INVALID;\n'),
    ('    case SXNU_CONTROL_VERB_REBOOT_FASTBOOT: return "REBOOT-FASTBOOT";\n',
     '    case SXNU_CONTROL_VERB_REBOOT_FASTBOOT: return "REBOOT-FASTBOOT";\n'
     '    case SXNU_CONTROL_VERB_INPUT: return "INPUT";\n'),
    ('    if (verb == SXNU_CONTROL_VERB_INVALID) return 0;\n    cursor++;\n',
     '    if (verb == SXNU_CONTROL_VERB_INVALID) return 0;\n    cursor++;\n'
     '    uint32_t args[3] = {0u, 0u, 0u};   /* a refused frame leaves *frame untouched */\n'
     '    if (verb == SXNU_CONTROL_VERB_INPUT) {\n'
     '        static const size_t widths[3] = {2u, 4u, 4u};\n'
     '        for (unsigned int a = 0; a < 3u; a++) {\n'
     '            uint64_t value;\n'
     '            if (cursor > bytes || bytes - cursor < widths[a] + 1u) return 0;\n'
     '            if (!sxnu_control_parse_hex(p + cursor, widths[a], &value) ||\n'
     '                p[cursor + widths[a]] != (uint8_t)\' \') return 0;\n'
     '            args[a] = (uint32_t)value;\n'
     '            cursor += widths[a] + 1u;\n'
     '        }\n'
     '    }\n'),
    ('    frame->verb = verb;\n    return 1;\n}\n',
     '    frame->verb = verb;\n'
     '    frame->arg[0] = args[0]; frame->arg[1] = args[1]; frame->arg[2] = args[2];\n'
     '    return 1;\n}\n'),
    ('        frame->verb > SXNU_CONTROL_VERB_REBOOT_FASTBOOT) return SXNU_CONTROL_VERB_INVALID;\n',
     '        frame->verb > SXNU_CONTROL_VERB_INPUT) return SXNU_CONTROL_VERB_INVALID;\n'),
    ('        accepted > SXNU_CONTROL_VERB_REBOOT_FASTBOOT || frame->verb != accepted) return 0;\n',
     '        accepted > SXNU_CONTROL_VERB_INPUT || frame->verb != accepted) return 0;\n')))
qud_edit('kernel/QcomUsbScreenshot.c', (
    ('static uint8_t pixels[QUD_SCREENSHOT_MAX_BYTES];\n',
     '/* fuxi-usb-control-20261112: the static buffer is the default; a provider may hand over a larger one. */\n'
     'static uint8_t pixels_static[QUD_SCREENSHOT_MAX_BYTES];\n'
     'static uint8_t *pixels = pixels_static;\n'
     'static size_t pixels_capacity = sizeof(pixels_static);\n'),
    ('           (uint64_t)w * h * 3u <= QUD_SCREENSHOT_MAX_BYTES;\n}\n',
     '           (uint64_t)w * h * 3u <= pixels_capacity;\n}\n'
     'int qcom_usb_debug_set_screenshot_storage(void *storage, size_t bytes)\n'
     '{\n'
     '    if (!storage || bytes < sizeof(pixels_static) || bytes > UINT32_MAX || ready) return 0;\n'
     '    pixels = storage; pixels_capacity = bytes;\n'
     '    return 1;\n'
     '}\n'),
    ('provider(provider_cookie, pixels, sizeof(pixels), &w, &h)', 'provider(provider_cookie, pixels, pixels_capacity, &w, &h)')))
qud_edit('include/QcomUsbScreenshot.h', (
    ('int qcom_usb_debug_register_framebuffer(const struct qcom_usb_framebuffer *framebuffer);\n',
     'int qcom_usb_debug_register_framebuffer(const struct qcom_usb_framebuffer *framebuffer);\n'
     '/* fuxi-usb-control-20261112: caller storage for the snapshot (at least the static default, kept until reset).\n'
     ' * Call once, before the first SCREENSHOT. Returns 1 when taken. */\n'
     'int qcom_usb_debug_set_screenshot_storage(void *storage, size_t bytes);\n'),))
qud_edit('kernel/QcomXnuUsbDebug.c', (
    # Placed inside the span tests/check.py extracts (control_reply_reboot .. control_service) so the handler model compiles too.
    ('static void control_handle_frame(const struct sxnu_control_frame *frame,\n',
     '/* fuxi-usb-control-20261112: synthetic input behind the INPUT verb. op 0 asks whether input is available (no side effect);\n'
     ' * the board (FuxiButtons) provides the real one. Returns 1 when carried out. */\n'
     'int fuxi_control_input(unsigned int op, unsigned int a, unsigned int b);\n'
     '__attribute__((weak)) int fuxi_control_input(unsigned int op, unsigned int a, unsigned int b)\n'
     '{\n    (void)op; (void)a; (void)b;\n    return 0;\n}\n'
     'static void control_handle_frame(const struct sxnu_control_frame *frame,\n'),
    ('    int reboot_accepted = 0;\n    size_t shot_bytes = 0;\n',
     '    int reboot_accepted = 0;\n    int input_accepted = 0;\n    size_t shot_bytes = 0;\n'),
    ('    default:\n        return;\n    }\n    if (!reply_bytes) {\n',
     '    case SXNU_CONTROL_VERB_INPUT:\n'
     '        if (frame->arg[0] < 1u || frame->arg[0] > 3u)\n'
     '            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "badop");\n'
     '        else if (!fuxi_control_input(0u, 0u, 0u))\n'
     '            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unavailable");\n'
     '        else {\n'
     '            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "OK", "queued");\n'
     '            input_accepted = 1;\n'
     '        }\n'
     '        break;\n'
     '    default:\n        return;\n    }\n    if (!reply_bytes) {\n'),
    ('    if (shot_bytes) (void)qcom_usb_shot_commit(shot_bytes);\n',
     '    if (shot_bytes) (void)qcom_usb_shot_commit(shot_bytes);\n'
     '    if (input_accepted) (void)fuxi_control_input(frame->arg[0], frame->arg[1], frame->arg[2]);\n')))
# The handler model builds its frames positionally (-Wextra -Werror): give the new arg field its zero initializer there.
qud_edit('tests/test_usb_screenshot_control.c', (
    ('    const struct sxnu_control_frame frame = {7, seq, 0, verb};\n',
     '    const struct sxnu_control_frame frame = {7, seq, 0, verb, {0u, 0u, 0u}};   /* fuxi-usb-control-20261112 */\n'),))

patcher = module / 'scripts/xnu_patches.py'
state = json.loads(subprocess.check_output([sys.executable, patcher, 'status',
                   '--xnu', root / '.cache/xnu-work', '--profile', 'darwin23-fuxi'], text=True))
if state['state_before'] == 'ready':
    subprocess.run([sys.executable, patcher, 'apply', '--xnu', root / '.cache/xnu-work',
                    '--profile', 'darwin23-fuxi'], check=True)
elif state['state_before'] != 'applied':
    raise SystemExit('USB source integration is not ready: '+str(state))
out = root / '.cache/fuxi-qud-build'
records = root / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
llvm = Path('/opt/homebrew/opt/llvm/bin')
if not llvm.is_dir():
    # Linux host: the framework's toolchain provides llvm-config. Its clang is the host
    # compiler the kit's tests must use, and /usr/bin has to come before the cross tools
    # so the host link gets the ELF linker rather than cctools' Mach-O ld.
    llvm = Path(subprocess.check_output(['llvm-config', '--bindir'], text=True).strip())
    os.environ['PATH'] = (str(llvm) + os.pathsep + '/usr/bin' + os.pathsep
                          + os.environ.get('PATH', ''))
    # check.py builds its host tests with -std=c11, which hides posix_memalign behind
    # glibc's feature-test macros; Apple's headers always declare it.
    staged_check = module / 'tests/check.py'
    if staged_check.is_file():
        source = staged_check.read_text()
        if '-D_POSIX_C_SOURCE' not in source and "'-std=c11'," in source:
            staged_check.write_text(source.replace("'-std=c11',",
                                                 "'-std=c11', '-D_POSIX_C_SOURCE=200809L',", 1))
def run(arguments, host=False):
    command = [str(v) for v in arguments]
    env = os.environ.copy()
    if host:
        env.pop('CCC_OVERRIDE_OPTIONS', None)
    result = subprocess.run(command, cwd=root, env=env, capture_output=True, text=True)
    print(result.stdout, end='')
    if result.returncode:
        print(result.stderr, end='')
        for base in (out, root / '.cache/fuxi-qud-xnu-check'):
            if base.is_dir():
                for log in base.glob('*.log'):
                    content = log.read_text(errors='replace')
                    if 'error:' in content:
                        print(log.name, content[-6500:])
        raise SystemExit(result.returncode)

# The upstream checks require a new output directory on every run.
for previous in (root / '.cache/fuxi-qud-model-check', out, root / '.cache/fuxi-qud-xnu-check'):
    if previous.exists():
        previous.rename(previous.with_name(previous.name + '.previous-' + str(time.time_ns())))

run([sys.executable, module / 'tests/check.py', '--target-clang', llvm / 'clang',
     '--output', root / '.cache/fuxi-qud-model-check'], host=True)
matches = list(records.glob('**/pe_kprintf.o.json'))
if len(matches) != 1:
    raise SystemExit('missing or ambiguous actual PE compile record')
run([sys.executable, module / 'scripts/build.py', '--compile-record', matches[0],
     '--soc', 'soc/FuxiSm8550.h', '--ar', llvm / 'llvm-ar', '--nm', llvm / 'llvm-nm', '--output', out])
run([sys.executable, module / 'tests/check_xnu.py', '--xnu', root / '.cache/xnu-work',
     '--records', records, '--archive', out / 'libQcomXnuUsbDebug.a', '--nm', llvm / 'llvm-nm',
     '--output', root / '.cache/fuxi-qud-xnu-check'])
for name in ('check-report.json', 'xnu-check-report.json'):
    directory = root / ('.cache/fuxi-qud-model-check' if name == 'check-report.json' else '.cache/fuxi-qud-xnu-check')
    print((directory / name).read_text())
print((out / 'build-report.json').read_text())
print('PASS: fuxi QcomXnuUsbDebug archive, models and six-unit enabled/disabled XNU integration')
