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
