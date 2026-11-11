#!/usr/bin/env python3
"""Absent-subsystem stubs for the iOS 17 checkpoint-1 kernel (arm64e).

BootLog.c weakly imports the sheng/fuxi dashboard subsystems (display, Adreno, touch, UFS log window,
boot progress). On arm64 an unresolved weak import links as NULL; on arm64e the linker cannot sign a
pointer to an undefined symbol, so every one of them needs a definition. package/BootLogStubs.c and
package/LegacyDisplayStub.c are the existing empty definitions; this adds the UFS log window and the
touch probe, whose drivers this checkpoint leaves out. The UFS functions report failure, so BootLog
keeps its RAM ring only.
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from kernel_target import TRIPLE, check_object

out = ROOT / 'build/ios17-stubs'
out.mkdir(parents=True, exist_ok=True)
extra = out / 'Ios17AbsentStubs.c'
# With CONFIG_XNU_NATIVE_UFS the real NativeUfs archive defines the UFS log window functions.
ufs_stubs = '' if os.environ.get('XNU_NATIVE_UFS') == '1' else '''
int native_ufs_log_open(uint64_t *first, uint64_t *count, const char **why) { if (why) *why = "no UFS (ios17 checkpoint)"; (void)first; (void)count; return -1; }
int native_ufs_log_write(uint64_t lba, uint32_t blocks, const void *data) { (void)lba; (void)blocks; (void)data; return -1; }
uint64_t native_ufs_export_blocks(void) { return 0; }
int native_ufs_export_write(uint64_t lba, uint32_t blocks, const void *data) { (void)lba; (void)blocks; (void)data; return -1; }
int native_ufs_log_sync(void) { return -1; }
unsigned native_ufs_store_gpt_check(void) { return 0; }
'''
# NativeUfs weakly calls the boot-progress observer from its verified read path; without CONFIG_XNU_BOOT_PROGRESS
# nothing defines it, and arm64e cannot link an undefined weak import, so provide the no-op observer.
progress_stub = '' if os.environ.get('XNU_BOOT_PROGRESS') == '1' else 'void boot_progress_poll(void) {}\n'
# With CONFIG_XNU_NATIVE_RAM_BLOCK the real NativeRamBlock object defines the Data persistence diagnostics.
persist_stubs = '' if os.environ.get('XNU_NATIVE_RAM_BLOCK') == '1' else '''volatile uint32_t native_data_persist_state, native_data_persist_header, native_data_persist_gpt, native_data_persist_gpt_after;
volatile uint64_t native_data_persist_pages, native_data_persist_loaded, native_data_persist_writes, native_data_persist_syncs,
    native_data_persist_errors, native_data_persist_first_lba, native_data_persist_present;
'''
# With CONFIG_XNU_NATIVE_LEGACY_DISPLAY the display archive (build-ios17-legacy-display.py) defines the two
# lifecycle probes, so package/LegacyDisplayStub.c is left out and only its GOP hand-off no-ops are kept here.
legacy_display = os.environ.get('XNU_NATIVE_LEGACY_DISPLAY') == '1'
handoff_stubs = '''int native_gop_handoff_call(unsigned op, void *task, const void *in, unsigned long in_size, void *out, unsigned long out_size)
{ (void)op; (void)task; (void)in; (void)in_size; (void)out; (void)out_size; return -536870203; /* kIOReturnNotReady */ }
void *native_gop_handoff_memory(void) { return 0; }
''' if legacy_display else ''
# Original kexts get IOInterruptEventSource::interruptEventSource(owner, action, provider, index) through this wrapper
# (fixture: original 0xfffffff0083a8964 -> _unixnu_interrupt_event_source_soft). Software sources (provider NULL:
# IOSurfaceRoot, IOHIDFamily, IOMobileGraphicsFamily) are created; hardware sources (ApplePMGR and the other original
# platform drivers asking for an Apple interrupt that the Qualcomm board does not have) get NULL, as the unmapped
# stub gave them. The plain mapping let ApplePMGR register interrupts and the boot died before the USB console.
soft_events = '''extern void *unixnu_original_interrupt_event_source(void *owner, void *action, void *provider, int index)
    __asm__("__ZN22IOInterruptEventSource20interruptEventSourceEP8OSObjectPFvS1_PS_iEP9IOServicei");
extern int printf(const char *, ...);
void *unixnu_interrupt_event_source_soft(void *owner, void *action, void *provider, int index)
{
	if (provider) {
		printf("ios17 interrupt event source refused: owner %p provider %p index %d caller %p\\n", owner, provider, index,
		    __builtin_extract_return_addr(__builtin_return_address(0)));
		return 0;
	}
	return unixnu_original_interrupt_event_source(owner, action, provider, index);
}
'''
extra.write_text('''#include <stdint.h>
''' + ufs_stubs + progress_stub + persist_stubs + handoff_stubs + soft_events + '''volatile uint32_t native_process_wait_port[10];
volatile uint32_t native_hid_input_events[8] __attribute__((weak));
volatile int32_t fuxi_buttons_stats[10] __attribute__((weak));
__attribute__((weak)) void native_touch_probe(void) {}
''')
# The device package (the tree's package/ is the framework's); the build driver exports UNIXNU_DEVICE.
package = Path(os.environ['UNIXNU_DEVICE']) / 'package' if os.environ.get('UNIXNU_DEVICE') else ROOT / 'kits/sheng'
boot_log_stubs = package / 'BootLogStubs.c'
if os.environ.get('XNU_NATIVE_ADRENO') == '1':
    # GpuPowerProbe (in the display archive) defines the GPU counters BootLogStubs carries for builds without a GPU.
    text = boot_log_stubs.read_text()
    for decl in ('char native_gpu_coherence_row[120];', 'volatile uint64_t native_gpu_idle_sleeps;',
                 'volatile uint64_t native_gpu_idle_spins;', 'volatile uint64_t native_gpu_idle_timeouts;',
                 'uint32_t native_gpu_perf_vote[2];',
                 # The touch probe (FuxiTouchProbe.cpp, in the display archive) defines the boot-log touch counters.
                 'volatile int32_t native_touch_strokes[16][7];', 'volatile uint32_t native_touch_stroke_count;',
                 'volatile int32_t native_touch_extent[4];', 'volatile uint32_t native_touch_edge_stats[4];'):
        if text.count(decl) != 1:
            raise SystemExit('BootLogStubs.c changed: ' + decl)
        text = text.replace(decl, decl[:-1] + ' __attribute__((weak));')
    boot_log_stubs = out / 'BootLogStubs.c'
    boot_log_stubs.write_text(text)
sources = [boot_log_stubs] + ([] if legacy_display else [package / 'LegacyDisplayStub.c']) + [extra]
objects = []
for source in sources:
    obj = out / (source.stem + '.o')
    subprocess.run(['xcrun', 'clang', '-target', TRIPLE, '-mkernel', '-ffreestanding', '-O2', '-Wno-everything',
                    '-c', str(source), '-o', str(obj)], check=True)
    check_object(obj.read_bytes(), source.name)
    objects.append(obj)
archive = out / 'libIos17Stubs.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'libtool', '-static', '-o', str(archive), *map(str, objects)], check=True)
print(archive)
