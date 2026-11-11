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
extra.write_text('''#include <stdint.h>
int native_ufs_log_open(uint64_t *first, uint64_t *count, const char **why) { if (why) *why = "no UFS (ios17 checkpoint)"; (void)first; (void)count; return -1; }
int native_ufs_log_write(uint64_t lba, uint32_t blocks, const void *data) { (void)lba; (void)blocks; (void)data; return -1; }
uint64_t native_ufs_export_blocks(void) { return 0; }
int native_ufs_export_write(uint64_t lba, uint32_t blocks, const void *data) { (void)lba; (void)blocks; (void)data; return -1; }
int native_ufs_log_sync(void) { return -1; }
unsigned native_ufs_store_gpt_check(void) { return 0; }
volatile uint32_t native_data_persist_state, native_data_persist_header, native_data_persist_gpt, native_data_persist_gpt_after;
volatile uint64_t native_data_persist_pages, native_data_persist_loaded, native_data_persist_writes, native_data_persist_syncs,
    native_data_persist_errors, native_data_persist_first_lba, native_data_persist_present;
volatile uint32_t native_process_wait_port[10];
volatile int32_t fuxi_buttons_stats[10];
void native_touch_probe(void) {}
''')
# The device package (the tree's package/ is the framework's); the build driver exports UNIXNU_DEVICE.
package = Path(os.environ['UNIXNU_DEVICE']) / 'package' if os.environ.get('UNIXNU_DEVICE') else ROOT / 'kits/sheng'
sources = [package / 'BootLogStubs.c', package / 'LegacyDisplayStub.c', extra]
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
