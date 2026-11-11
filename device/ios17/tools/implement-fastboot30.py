from pathlib import Path
import difflib
base=Path('/home/xiaomi/sadnut-fuxi-local-20261103')
scripts=base/'device-ios17/scripts'
mod=base/'UniXNU/out-ios17/fuxi/tree/.cache/fuxi-qud-module'
p=Path('kernel/QcomXnuUsbDebug.c')
orig=(mod/p).read_text()
def sub(s,a,b):
 assert s.count(a)==1,a
 return s.replace(a,b)
code=r"""
#ifdef FUXI_PANIC_FASTBOOT
/* Prepare mappings in normal context. No allocation or thread services after panic. */
#include "BootResetCore.h"
static struct br_arbiter reset_arb;
static volatile uint32_t *reset_obs, *reset_chn;
static void fuxi_prepare_fastboot(void)
{
    int seconds = 0;
    struct br_found f;
    if (!PE_parse_boot_argn("fuxi-panic-fastboot", &seconds, sizeof(seconds)) || seconds != 30)
        return;
    reset_arb.core = (void *)ml_io_map(0xc400000, 0x3000);
    reset_arb.cnfg = (void *)ml_io_map(0xc42d000, 0x4000);
    if (!reset_arb.core || !reset_arb.cnfg || br_find(&reset_arb, &f) || f.owner != 0)
        return;
    unsigned off = BR_OBS_APID_STRIDE * (unsigned)f.apid;
    vm_offset_t obs = ml_io_map(0xc440000 + (off & ~4095u), 4096);
    if (!obs) return;
    reset_obs = (void *)(obs + (off & 4095u));
    reset_chn = (void *)ml_io_map(0xc500000 + BR_CHNL_APID_STRIDE * (unsigned)f.apid, 4096);
}
static void fuxi_panic_fastboot(void)
{
    uint8_t before, after;
    uint64_t hz = native.status.counter_frequency;
    if (!reset_obs || !reset_chn || !hz || hz > COUNTER_MAX_FREQUENCY) return;
    uint64_t start = counter();
    while (counter() - start < 30 * hz) __asm__ volatile("yield" ::: "memory");
    /* Both pointers already select this APID, EE0, so channel offset is zero. */
    if (br_set_bootloader(reset_obs, reset_chn, &reset_arb, 0, &before, &after)) return;
    __asm__ volatile("movz w0, #9\nmovk w0, #0x8400, lsl #16\n"
                     "mov x1, xzr\nmov x2, xzr\nmov x3, xzr\ndsb sy\nsmc #0"
                     ::: "x0","x1","x2","x3","x4","x5","x6","x7","x8","x9",
                         "x10","x11","x12","x13","x14","x15","x16","x17","memory","cc");
}
#endif
"""
s=sub(orig,'static int qcom_usb_control_halt_restart(unsigned int type)\n{',code+'\nstatic int qcom_usb_control_halt_restart(unsigned int type)\n{')
s=sub(s,'    native.status.phase = QCOM_USB_DEBUG_ACTIVE;', '#ifdef FUXI_PANIC_FASTBOOT\n    fuxi_prepare_fastboot();\n#endif\n    native.status.phase = QCOM_USB_DEBUG_ACTIVE;')
s=sub(s,'    char announcement[192];','#ifdef FUXI_PANIC_FASTBOOT\n    int finished = 0;\n#endif\n    char announcement[192];')
s=sub(s,'if (marker_queued && dwc3_queued(native.core) == 0 &&\n            soc_usb_buffer_pending(&usb_log_buffer) == 0) break;', 'if (marker_queued && dwc3_queued(native.core) == 0 &&\n            soc_usb_buffer_pending(&usb_log_buffer) == 0) {\n#ifdef FUXI_PANIC_FASTBOOT\n            finished = 1;\n#endif\n            break;\n        }')
s=sub(s,'    if (usb_diag_enabled) diag_reset(10);','#ifdef FUXI_PANIC_FASTBOOT\n    if (finished) fuxi_panic_fastboot();\n#endif\n    if (usb_diag_enabled) diag_reset(10);')
patch=''.join(difflib.unified_diff(orig.splitlines(True),s.splitlines(True),'a/'+str(p),'b/'+str(p)))
builder=scripts/'build-fuxi-usb-debug.py'
add="""# Only the iOS 17 fuxi archive opts into the post-drain hook.
shutil.copy2(root / 'kits/sheng/BootResetCore.h', module / 'kernel/BootResetCore.h')
board_header = module / 'soc/FuxiSm8550.h'
if 'FUXI_PANIC_FASTBOOT' not in board_header.read_text():
    with board_header.open('a') as stream:
        stream.write('\\n#define FUXI_PANIC_FASTBOOT 1\\n')
if 'fuxi_panic_fastboot(void)' not in (module / 'kernel/QcomXnuUsbDebug.c').read_text():
    subprocess.run(['patch','-p1','--batch'],cwd=module,input=PATCH_BODY,text=True,check=True)
""".replace('PATCH_BODY',repr(patch))
builder.write_text(sub(builder.read_text(),"patcher = module / 'scripts/xnu_patches.py'",add+"\npatcher = module / 'scripts/xnu_patches.py'"))
pack=(base/'pack-ios17-nohint.py').read_text().replace('use_contiguous_hint=0\'', 'use_contiguous_hint=0 fuxi-panic-fastboot=30\'').replace('nohint-20261105','fastboot30-20261105').replace('pack-work-ios17-nohint','pack-work-ios17-fastboot30')
(base/'pack-ios17-fastboot30.py').write_text(pack)