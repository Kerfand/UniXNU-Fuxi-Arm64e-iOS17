"""Regenerate the three log/screen-only probes: rootauth-probe, init-exec-probe, init-hang-probe (+ screen marks)."""
import subprocess, tempfile
from pathlib import Path
work = Path('/home/xiaomi/sadnut-fuxi-local-20261103/UniXNU/out-ios17/fuxi/tree/.cache/xnu-work')
P = Path('/home/xiaomi/sadnut-fuxi-local-20261103/UniXNU/patches/xnu-10063-arm64e')
NAMES = ('init-hang-probe.patch', 'init-exec-probe.patch', 'rootauth-probe.patch')
for name in NAMES:
    p = P / name
    if p.exists() and subprocess.run(['git', '-C', str(work), 'apply', '--reverse', '--check', str(p)], capture_output=True).returncode == 0:
        subprocess.run(['git', '-C', str(work), 'apply', '--reverse', str(p)], check=True)

def make(rel, transform, patch):
    src = (work / rel).read_text(); new = transform(src)
    with tempfile.TemporaryDirectory() as d:
        a = Path(d, 'a', rel); b = Path(d, 'b', rel); a.parent.mkdir(parents=True); b.parent.mkdir(parents=True)
        a.write_text(src); b.write_text(new)
        out = subprocess.run(['diff', '-u', 'a/' + rel, 'b/' + rel], cwd=d, capture_output=True, text=True).stdout
    lines = out.splitlines(True); lines[0] = f'--- a/{rel}\n'; lines[1] = f'+++ b/{rel}\n'
    patch.write_text(f'diff --git a/{rel} b/{rel}\n' + ''.join(lines))
    r = subprocess.run(['git', '-C', str(work), 'apply', '--check', str(patch)], capture_output=True, text=True)
    print(patch.name, 'check rc', r.returncode, r.stderr.strip())

def rootauth(src):
    old = ('\t\t\tif (autherr && !owner_authorized) {\n'
           '\t\t\t\tpanic("rootvp not authenticated after mounting");\n')
    assert src.count(old) == 1
    return src.replace(old,
        '\t\t\t/*\n'
        '\t\t\t * Probe: log the FSIOC_KERNEL_ROOTAUTH result of the sealed root, which the panic below does not\n'
        '\t\t\t * report, and mark it on screen (osfmk/arm64/pcb.c). Logging only: the check itself is unchanged.\n'
        '\t\t\t */\n'
        '\t\t\tprintf("unixnu rootauth: FSIOC_KERNEL_ROOTAUTH=%d owner=%d mnt_flag=0x%llx\\n", autherr,\n'
        '\t\t\t    owner_authorized, (unsigned long long)vfs_flags(vnode_mount(rootvnode)));\n'
        '\t\t\t{\n'
        '\t\t\t\textern void unixnu_screen_mark(unsigned int, unsigned int);\n'
        '\t\t\t\tunixnu_screen_mark(1, 0x00ff0000);\n'
        '\t\t\t}\n' + old)

def exec_(src):
    old = '\treturn execve(p, &init_exec_args, retval);\n}\n'
    assert src.count(old) == 1
    return src.replace(old,
        '\t/*\n'
        '\t * Probe (log and screen only): which init program the kernel tried and what execve returned. With\n'
        '\t * boot-arg unixnu-hang-probe=N the init task\'s threads are reported every N seconds (osfmk/arm64/pcb.c).\n'
        '\t */\n'
        '\t{\n'
        '\t\textern void unixnu_init_hang_probe_arm(task_t);\n'
        '\t\textern void unixnu_screen_mark(unsigned int, unsigned int);\n'
        '\t\tunixnu_screen_mark(2, 0x00ffff00);\n'
        '\t\tunixnu_init_hang_probe_arm(proc_task(p));\n'
        '\t\terror = execve(p, &init_exec_args, retval);\n'
        '\t\tprintf("unixnu init: exec %s returned %d\\n", path, error);\n'
        '\t\tunixnu_screen_mark(3, error ? 0x00ff00ff : 0x0000ff00);\n'
        '\t\textern void unixnu_init_hang_probe_disarm(int);\n'
        '\t\tunixnu_init_hang_probe_disarm(error);\n'
        '\t}\n'
        '\treturn error;\n}\n')

PROBE = r'''
/*
 * Probes (log and screen only).
 *
 * unixnu_screen_mark(slot, color) paints a 96x96 block in row y=160 of the boot framebuffer (PE_state.video), so a
 * photo shows how far the kernel got even when the USB console stops. The first mark also starts a 1 s heartbeat
 * in slot 0 (white/blue), which keeps changing only while timer callouts still run.
 *
 * boot-arg unixnu-hang-probe=N: N seconds after the kernel starts exec of the init program, and every N seconds
 * for 5 rounds, print each thread of the init task: state, wait event, continuation and the return addresses of
 * the frame-pointer chain of its saved kernel context (blocked threads only; the walk stays inside the thread's
 * kernel stack). Each round also marks slot 4+round cyan. Reads only; nothing is changed.
 */
#include <kern/thread_call.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <kern/processor.h>
#include <ptrauth.h>

#define UNIXNU_TEXT_Y 300
#define UNIXNU_TEXT_SCALE 2
#define UNIXNU_LINE_PX ((7 + 2) * UNIXNU_TEXT_SCALE)
#define UNIXNU_TEXT_LINES 24
#define UNIXNU_MAP_ROWS ((UNIXNU_TEXT_Y - 160) + UNIXNU_TEXT_LINES * UNIXNU_LINE_PX)
static volatile uint32_t *unixnu_fb;
static unsigned long unixnu_fb_row, unixnu_fb_width, unixnu_fb_height;
static thread_call_t unixnu_beat_call;
static uint32_t unixnu_beat;

static void
unixnu_fill(unsigned int slot, uint32_t color)
{
	if (!unixnu_fb) {
		return;
	}
	unsigned long x0 = 40 + slot * 110;
	if (x0 + 96 > unixnu_fb_width) {
		return;
	}
	for (unsigned long y = 0; y < 96; y++) {
		volatile uint32_t *row = (volatile uint32_t *)((volatile uint8_t *)unixnu_fb + y * unixnu_fb_row);
		for (unsigned long x = x0; x < x0 + 96; x++) {
			row[x] = color;
		}
	}
}

static void
unixnu_beat_schedule(void)
{
	uint64_t deadline;
	clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(unixnu_beat_call, deadline);
}

static void
unixnu_beat_tick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	unixnu_fill(0, (unixnu_beat++ & 1) ? 0x000000ff : 0x00ffffff);
	unixnu_beat_schedule();
}

void unixnu_screen_mark(unsigned int slot, unsigned int color);
void
unixnu_screen_mark(unsigned int slot, unsigned int color)
{
	if (!unixnu_fb && PE_state.video.v_baseAddr && PE_state.video.v_rowBytes && PE_state.video.v_depth == 32) {
		unixnu_fb_row = PE_state.video.v_rowBytes;
		unixnu_fb_width = PE_state.video.v_width;
		vm_offset_t base = (vm_offset_t)(PE_state.video.v_baseAddr & ~PAGE_MASK);
		/* Map only rows 160 .. 160 + UNIXNU_MAP_ROWS: the whole framebuffer (about 10 MiB) does not fit the
		 * 8 MiB io_submap, and kmem_alloc failing there panics. */
		unixnu_fb_height = MIN(PE_state.video.v_height, 160 + UNIXNU_MAP_ROWS);
		vm_size_t size = round_page((PE_state.video.v_baseAddr & PAGE_MASK) + unixnu_fb_height * unixnu_fb_row);
		vm_offset_t va = ml_io_map(base, size);
		if (va) {
			unixnu_fb = (volatile uint32_t *)(va + (PE_state.video.v_baseAddr & PAGE_MASK) + 160 * unixnu_fb_row);
			unixnu_beat_call = thread_call_allocate_with_options(unixnu_beat_tick, NULL, THREAD_CALL_PRIORITY_KERNEL, 0);
			unixnu_beat_schedule();
		}
		printf("unixnu screen: framebuffer %#lx row %lu width %lu mapped %p\n", PE_state.video.v_baseAddr,
		    unixnu_fb_row, unixnu_fb_width, (void *)unixnu_fb);
	}
	unixnu_fill(slot, color);
}

/* 5x7 glyphs for the on-screen hang report: 0-9 a-f, then ' ' ':' '#' 'r' 's' 't' 'w' 'c' 'l' 'x' '-'. */
static const char unixnu_glyph_chars[] = "0123456789abcdef :#rstwclx-";
static const uint8_t unixnu_glyphs[][7] = {
	{0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}, {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}, {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e},
	{0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}, {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e},
	{0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}, {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
	{0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}, {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c},
	{0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e},
	{0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}, {0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c},
	{0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}, {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00},
	{0x0a, 0x0a, 0x1f, 0x0a, 0x1f, 0x0a, 0x0a}, {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10},
	{0x00, 0x00, 0x0e, 0x10, 0x0e, 0x01, 0x1e}, {0x08, 0x08, 0x1c, 0x08, 0x08, 0x09, 0x06},
	{0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0a}, {0x00, 0x00, 0x0e, 0x10, 0x10, 0x11, 0x0e},
	{0x0c, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}, {0x00, 0x00, 0x11, 0x0a, 0x04, 0x0a, 0x11},
	{0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00},
};
static unsigned int unixnu_text_line;

static void
unixnu_text(const char *s)
{
	if (!unixnu_fb) {
		return;
	}
	/* unixnu_fb points at row 160; text starts UNIXNU_TEXT_Y - 160 rows below it (the mapping covers it). */
	unsigned long top = (UNIXNU_TEXT_Y - 160) + unixnu_text_line * UNIXNU_LINE_PX;
	unsigned long x = 40;
	if (160 + top + UNIXNU_LINE_PX > unixnu_fb_height) {
		return;
	}
	for (; *s && x + 6 * UNIXNU_TEXT_SCALE < unixnu_fb_width; s++, x += 6 * UNIXNU_TEXT_SCALE) {
		const char *at = strchr(unixnu_glyph_chars, *s);
		const uint8_t *g = unixnu_glyphs[at ? at - unixnu_glyph_chars : 16];
		for (int gy = 0; gy < 7 * UNIXNU_TEXT_SCALE; gy++) {
			volatile uint32_t *row = (volatile uint32_t *)((volatile uint8_t *)unixnu_fb + (top + gy) * unixnu_fb_row);
			for (int gx = 0; gx < 6 * UNIXNU_TEXT_SCALE; gx++) {
				int bit = gx / UNIXNU_TEXT_SCALE;
				row[x + gx] = (bit < 5 && (g[gy / UNIXNU_TEXT_SCALE] >> (4 - bit)) & 1) ? 0x00ffffff : 0x00000000;
			}
		}
	}
	unixnu_text_line++;
}

/*
 * Sample of the CPU that runs a TH_RUN thread: a cross-call reads the interrupted context (cpu_int_state) on
 * that CPU and walks its kernel frame chain there. No answer within 100 ms means interrupts are masked or the
 * core is stuck in a bus access.
 */
static volatile uint32_t unixnu_sample_done;
static uint64_t unixnu_sample_pc, unixnu_sample_lr, unixnu_sample_fp, unixnu_sample_cpsr;
static uint64_t unixnu_sample_frames[20];
static uint32_t unixnu_sample_count;

static void
unixnu_sample_cpu(__unused void *arg)
{
	arm_saved_state_t *state = (arm_saved_state_t *)getCpuDatap()->cpu_int_state;
	unixnu_sample_count = 0;
	if (state) {
		unixnu_sample_pc = (uint64_t)ptrauth_strip((void *)get_saved_state_pc(state), ptrauth_key_return_address);
		unixnu_sample_lr = (uint64_t)ptrauth_strip((void *)get_saved_state_lr(state), ptrauth_key_return_address);
		unixnu_sample_fp = get_saved_state_fp(state);
		unixnu_sample_cpsr = get_saved_state_cpsr(state);
		thread_t self = current_thread();
		if ((unixnu_sample_cpsr & 0xc) && self && self->kernel_stack) {      /* interrupted at EL1 */
			vm_offset_t low = self->kernel_stack, high = self->kernel_stack + kernel_stack_size;
			uint64_t fp = unixnu_sample_fp;
			while (unixnu_sample_count < 20 && fp >= low && fp + 16 <= high && !(fp & 7)) {
				uint64_t *frame = (uint64_t *)fp;
				unixnu_sample_frames[unixnu_sample_count++] =
				    (uint64_t)ptrauth_strip((void *)frame[1], ptrauth_key_return_address);
				if (frame[0] <= fp) {
					break;
				}
				fp = frame[0];
			}
		}
	} else {
		unixnu_sample_pc = unixnu_sample_lr = unixnu_sample_fp = unixnu_sample_cpsr = 0;
	}
	__atomic_store_n(&unixnu_sample_done, 1, __ATOMIC_RELEASE);
}

static void
unixnu_sample_running(int cpu)
{
	char line[96];
	if (cpu < 0) {
		unixnu_text("  x-");
		return;
	}
	__atomic_store_n(&unixnu_sample_done, 0, __ATOMIC_RELEASE);
	kern_return_t kr = cpu_xcall(cpu, unixnu_sample_cpu, NULL);
	for (int i = 0; i < 1000 && !__atomic_load_n(&unixnu_sample_done, __ATOMIC_ACQUIRE); i++) {
		delay(100);
	}
	uint32_t done = __atomic_load_n(&unixnu_sample_done, __ATOMIC_ACQUIRE);
	printf("unixnu hang probe:   cpu %d xcall %d answered %u pc %#llx lr %#llx fp %#llx cpsr %#llx\n", cpu, kr, done,
	    unixnu_sample_pc, unixnu_sample_lr, unixnu_sample_fp, unixnu_sample_cpsr);
	snprintf(line, sizeof(line), "  x%x s%x w%x", cpu, kr, done);
	unixnu_text(line);
	if (!done) {
		return;
	}
	snprintf(line, sizeof(line), "  l%llx l%llx", unixnu_sample_pc, unixnu_sample_lr);
	unixnu_text(line);
	snprintf(line, sizeof(line), "  c%llx r%llx", unixnu_sample_cpsr, unixnu_sample_fp);
	unixnu_text(line);
	for (uint32_t i = 0; i < unixnu_sample_count; i += 4) {
		int used = snprintf(line, sizeof(line), "  #%x", i);
		for (uint32_t k = i; k < unixnu_sample_count && k < i + 4 && used < (int)sizeof(line) - 18; k++) {
			used += snprintf(line + used, sizeof(line) - used, " %llx", unixnu_sample_frames[k]);
		}
		unixnu_text(line);
	}
}

/*
 * exec-stage-probe.patch marks: a 40x40 block per stage in row 230 (x = 40 + stage * 48), drawn synchronously at
 * the entry of each exec-path function; the colour advances on every call, so the last block that changed is
 * where the exec stopped. Before the framebuffer is mapped (first unixnu_screen_mark) this only counts.
 */
static uint32_t unixnu_stage_count[24];
void unixnu_stage(unsigned int stage);
void
unixnu_stage(unsigned int stage)
{
	static const uint32_t palette[] = {0x00ff4040, 0x0040ff40, 0x004040ff, 0x00ffff40, 0x00ff40ff, 0x0040ffff, 0x00ffffff, 0x00ff8000};
	if (stage >= 24) {
		return;
	}
	uint32_t count = ++unixnu_stage_count[stage];
	if (!unixnu_fb) {
		return;
	}
	unsigned long x0 = 40 + stage * 44;
	if (x0 + 40 > unixnu_fb_width) {
		return;
	}
	uint32_t color = palette[count % (sizeof(palette) / sizeof(palette[0]))];
	for (unsigned long y = 100; y < 130; y++) {         /* rows 260..290, below the slot row 160..256 */
		volatile uint32_t *row = (volatile uint32_t *)((volatile uint8_t *)unixnu_fb + y * unixnu_fb_row);
		for (unsigned long x = x0; x < x0 + 40; x++) {
			row[x] = color;
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * unmapped-screen-probe.patch: each unixnu_fileset_unmapped stub hit draws "-<hit> l<site> x<x0> x<x1>" on text
 * lines 20..23 (the last four hits, rotating), synchronously, so a stub that loops shows up with a dead console.
 */
void unixnu_unmapped_screen(uint32_t hit, void *site, uint64_t arg0, uint64_t arg1);
/* One line per distinct call site (text lines 10..23, first 14 sites): "-<last hit> l<site> #<count> x<x16> x<x0>". */
#define UNIXNU_UNMAPPED_SITES 14
static void *unixnu_unmapped_site[UNIXNU_UNMAPPED_SITES];
static uint32_t unixnu_unmapped_count[UNIXNU_UNMAPPED_SITES];
void
unixnu_unmapped_screen(uint32_t hit, void *site, uint64_t arg0, uint64_t arg1)
{
	char line[96];
	unsigned int saved = unixnu_text_line, slot;
	for (slot = 0; slot < UNIXNU_UNMAPPED_SITES; slot++) {
		if (unixnu_unmapped_site[slot] == site || unixnu_unmapped_site[slot] == NULL) {
			break;
		}
	}
	if (slot == UNIXNU_UNMAPPED_SITES) {
		return;
	}
	unixnu_unmapped_site[slot] = site;
	unixnu_unmapped_count[slot]++;
	/* The fileset report logs only the first 64 hits; past that, still log each new call site once. */
	if (unixnu_unmapped_count[slot] == 1 && hit >= 64) {
		printf("fileset: unmapped call #%u from %p (x16 %#llx x0 %#llx); new site after the logged 64\n", hit, site,
		    arg0, arg1);
	}
	unixnu_text_line = 10 + slot;
	snprintf(line, sizeof(line), "-%x l%lx #%x x%llx x%llx", hit, (unsigned long)site, unixnu_unmapped_count[slot],
	    arg0, arg1);
	unixnu_text(line);
	unixnu_text_line = saved;
}

/*
 * External PC sampling of every core through the CoreSight debug APB, without IPIs (a core stuck with interrupts
 * masked still answers). SM8550: DSU-110 "older" debug APB address map (Arm 101381, ROM tables -> Debug system
 * address map), DebugBlock at 0x12000000 -- the vendor DT lists coresight-cti-cpu<n> at 0x12000000 + 0x10000 * (n + 1),
 * which is that map's Core n CTI. Core n Debug = 0x13010000 + n * 0x100000, Core n PMU = 0x13020000 + n * 0x100000.
 * PMU page (Cortex-A715 TRM 101590, External PMU registers): PMPCSSR 0x600 (64-bit snapshot PC), PMSSSR 0x610,
 * PMSSCR 0x6F0 (SS bit 0 = capture now), PMLAR 0xFB0, PMDEVARCH 0xFBC. Debug page (Arm ARM external debug):
 * EDPCSRlo 0x0A0 / EDPCSRhi 0x0AC, EDPRSR 0x314, EDLAR 0xFB0. Line per core: "c<n> l<snap1> l<snap2> l<edpcsr>
 * x<pmdevarch> x<edprsr>". Mapped at arm time, read only from the hang report.
 */
#define UNIXNU_PCS_CORES 8
static volatile uint8_t *unixnu_pcs_dbg[UNIXNU_PCS_CORES], *unixnu_pcs_pmu[UNIXNU_PCS_CORES];

static void
unixnu_pcs_map(void)
{
	for (int n = 0; n < UNIXNU_PCS_CORES; n++) {
		unixnu_pcs_dbg[n] = (volatile uint8_t *)ml_io_map(0x13010000UL + 0x100000UL * n, PAGE_SIZE);
		unixnu_pcs_pmu[n] = (volatile uint8_t *)ml_io_map(0x13020000UL + 0x100000UL * n, PAGE_SIZE);
	}
}

static uint64_t
unixnu_pcs_snapshot(volatile uint8_t *pmu)
{
	*(volatile uint32_t *)(pmu + 0xfb0) = 0xc5acce55;   /* PMLAR: unlock software access */
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(pmu + 0x6f0) = 1;            /* PMSSCR.SS: capture now */
	__asm__ volatile ("dsb sy" ::: "memory");
	delay(5);
	uint32_t lo = *(volatile uint32_t *)(pmu + 0x600);
	uint32_t hi = *(volatile uint32_t *)(pmu + 0x604);
	return ((uint64_t)hi << 32) | lo;
}

static void
unixnu_pcs_report(void)
{
	char line[96];
	for (int n = 0; n < UNIXNU_PCS_CORES; n++) {
		volatile uint8_t *dbg = unixnu_pcs_dbg[n], *pmu = unixnu_pcs_pmu[n];
		if (!dbg || !pmu) {
			snprintf(line, sizeof(line), "c%x -", n);
			unixnu_text(line);
			continue;
		}
		uint32_t devarch = *(volatile uint32_t *)(pmu + 0xfbc);
		uint32_t edprsr = *(volatile uint32_t *)(dbg + 0x314);
		uint64_t s1 = unixnu_pcs_snapshot(pmu);
		delay(1000);
		uint64_t s2 = unixnu_pcs_snapshot(pmu);
		uint32_t elo = *(volatile uint32_t *)(dbg + 0x0a0);   /* EDPCSRlo read samples, then EDPCSRhi */
		uint32_t ehi = *(volatile uint32_t *)(dbg + 0x0ac);
		uint64_t ed = ((uint64_t)ehi << 32) | elo;
		printf("unixnu hang probe: core %d pmpcssr %#llx %#llx edpcsr %#llx pmdevarch %#x edprsr %#x\n", n, s1, s2, ed,
		    devarch, edprsr);
		snprintf(line, sizeof(line), "c%x l%llx l%llx l%llx x%x x%x", n, s1, s2, ed, devarch, edprsr);
		unixnu_text(line);
	}
}

/*
 * Panic identity (called first thing in panic_trap_to_debugger): text lines 16..18 / 20..22 (alternating panics):
 * "--<n> c<cpu> l<caller> l<format> x<arg0> x<arg1>" and, when ctx (or sleh's saved-state argument) is an exception
 * state, "   l<pc> l<lr> x<far> x<esr>" and "   b<ret1> <ret2> <ret3> <ret4>" from its frame-pointer chain.
 * The format string is read offline from the image.
 */
static uint32_t unixnu_panic_seen;
void unixnu_panic_screen(const char *fmt, va_list *args, void *ctx, unsigned long caller);
void
unixnu_panic_screen(const char *fmt, va_list *args, void *ctx, unsigned long caller)
{
	char line[96];
	uint64_t a[4] = {0, 0, 0, 0};
	unsigned int saved = unixnu_text_line;
	uint32_t n = __atomic_fetch_add(&unixnu_panic_seen, 1, __ATOMIC_RELAXED);
	if (args) {
		va_list ap;
		va_copy(ap, *args);
		a[0] = va_arg(ap, uint64_t);
		a[1] = va_arg(ap, uint64_t);
		a[2] = va_arg(ap, uint64_t);
		a[3] = va_arg(ap, uint64_t);
		va_end(ap);
	}
	/* Three lines per panic: the first panic on 16..18, the second on 20..22; later (nested) panics are not drawn,
	 * so the root panic is never overwritten. Each line fits the 1080 px row. */
	if (n > 1) {
		return;
	}
	unixnu_text_line = 16 + 4 * n;
	snprintf(line, sizeof(line), "--%x c%x l%lx l%lx x%llx x%llx", n, cpu_number(),
	    (unsigned long)ptrauth_strip((void *)caller, ptrauth_key_return_address), (unsigned long)fmt, a[0], a[1]);
	unixnu_text(line);
	/* sleh's "%s at pc %p, lr %p (saved state: %p...": no ctx, the exception state is the fourth argument. */
	if (!ctx && fmt && fmt[0] == '%' && fmt[1] == 's' && fmt[2] == ' ' && fmt[3] == 'a' && fmt[4] == 't' &&
	    a[3] >= VM_MIN_KERNEL_ADDRESS && !(a[3] & 7)) {
		ctx = (void *)a[3];
	}
	if (ctx && (uintptr_t)ctx >= VM_MIN_KERNEL_ADDRESS) {
		arm_saved_state_t *state = (arm_saved_state_t *)ctx;
		snprintf(line, sizeof(line), "   l%llx l%llx x%llx x%x",
		    (uint64_t)ptrauth_strip((void *)get_saved_state_pc(state), ptrauth_key_return_address),
		    (uint64_t)ptrauth_strip((void *)get_saved_state_lr(state), ptrauth_key_return_address),
		    (uint64_t)get_saved_state_far(state), (uint32_t)get_saved_state_esr(state));
		unixnu_text(line);
		/* Frame-pointer chain from the faulting context: up to four callers above lr. */
		uint64_t fp = get_saved_state_fp(state), ret[4] = {0, 0, 0, 0};
		for (int i = 0; i < 4; i++) {
			if (fp < VM_MIN_KERNEL_ADDRESS || (fp & 15) || fp > VM_MAX_KERNEL_ADDRESS - 16) {
				break;
			}
			const uint64_t *frame = (const uint64_t *)fp;
			ret[i] = (uint64_t)ptrauth_strip((void *)frame[1], ptrauth_key_return_address);
			if (frame[0] <= fp) {
				break;
			}
			fp = frame[0];
		}
		snprintf(line, sizeof(line), "   b%llx %llx %llx %llx", ret[0], ret[1], ret[2], ret[3]);
		unixnu_text(line);
	}
	unixnu_text_line = saved;
}

/* exit-probe.patch helper: the user register state of a thread (pc/lr stripped of PAC). */
int unixnu_thread_user_state(thread_t thread, uint64_t *out);
int
unixnu_thread_user_state(thread_t thread, uint64_t *out)
{
	arm_saved_state_t *state = thread ? get_user_regs(thread) : NULL;
	if (!state) {
		return 0;
	}
	out[0] = (uint64_t)ptrauth_strip((void *)get_saved_state_pc(state), ptrauth_key_return_address);
	out[1] = (uint64_t)ptrauth_strip((void *)get_saved_state_lr(state), ptrauth_key_return_address);
	out[2] = get_saved_state_sp(state);
	out[3] = get_saved_state_far(state);
	out[4] = get_saved_state_esr(state);
	out[5] = get_saved_state_cpsr(state);
	out[6] = get_saved_state_reg(state, 0);
	out[7] = get_saved_state_reg(state, 16);
	out[8] = get_saved_state_reg(state, 17);
	return 1;
}

/* crash-probe.patch helper: user general registers x0..x28 of a thread. */
int unixnu_thread_user_regs(thread_t thread, uint64_t *out);
int
unixnu_thread_user_regs(thread_t thread, uint64_t *out)
{
	arm_saved_state_t *state = thread ? get_user_regs(thread) : NULL;
	if (!state) {
		return 0;
	}
	for (int i = 0; i < 29; i++) {
		out[i] = get_saved_state_reg(state, i);
	}
	return 1;
}

/*
 * crash-probe.patch helper: the crash-reporter annotations of the current (crashing) process. Every image that uses
 * CrashReporterClient has a __crash_info section {version, message, signature_string, backtrace, message2, ...}; the
 * assertion/abort paths (Swift fatalError, os_crash, BaseBoard/FrontBoard asserts, NSAssert) store their text there.
 * Walks dyld_all_image_infos->infoArray (24-byte {load address, path, mod date} entries), parses each image's load
 * commands in user memory and prints non-empty message/message2 strings (at most 8 per process). Reads only.
 */
void unixnu_print_crash_info(uint64_t infos);
void
unixnu_print_crash_info(uint64_t infos)
{
	uint32_t hdr[2] = {0, 0};
	uint64_t array = 0;
	if (!infos || copyin((user_addr_t)infos, hdr, sizeof(hdr)) || copyin((user_addr_t)(infos + 8), &array, sizeof(array)) || !array) {
		return;
	}
	uint32_t count = hdr[1] > 4096 ? 4096 : hdr[1];
	int printed = 0;
	for (uint32_t i = 0; i < count && printed < 8; i++) {
		uint64_t ent[3];
		if (copyin((user_addr_t)(array + i * 24), ent, sizeof(ent))) {
			break;
		}
		uint64_t mh = ent[0];
		uint32_t h[8];
		if (!mh || copyin((user_addr_t)mh, h, sizeof(h)) || h[0] != 0xfeedfacf || h[5] > 0x20000) {
			continue;
		}
		uint64_t at = mh + 32, end = at + h[5], text_vm = 0, ci_addr = 0, ci_size = 0;
		bool have_text = false;
		for (uint32_t c = 0; c < h[4] && at + 8 <= end; c++) {
			uint32_t lc[2];
			if (copyin((user_addr_t)at, lc, sizeof(lc)) || lc[1] < 8) {
				break;
			}
			if (lc[0] == 0x19) {
				char segname[16];
				uint64_t vm = 0;
				uint32_t nsects = 0;
				if (copyin((user_addr_t)(at + 8), segname, 16) || copyin((user_addr_t)(at + 24), &vm, 8) ||
				    copyin((user_addr_t)(at + 64), &nsects, 4)) {
					break;
				}
				if (!strncmp(segname, "__TEXT", 16)) {
					text_vm = vm;
					have_text = true;
				}
				for (uint32_t s = 0; s < nsects && s < 64; s++) {
					char sect[16];
					uint64_t sa[2];
					uint64_t so = at + 72 + (uint64_t)s * 80;
					if (copyin((user_addr_t)so, sect, 16) || copyin((user_addr_t)(so + 32), sa, sizeof(sa))) {
						break;
					}
					if (!strncmp(sect, "__crash_info", 16)) {
						ci_addr = sa[0];
						ci_size = sa[1];
					}
				}
			}
			at += lc[1];
		}
		if (!ci_addr || !have_text) {
			continue;
		}
		uint64_t ann[5] = {0, 0, 0, 0, 0};   /* version, message, signature_string, backtrace, message2 */
		size_t n = ci_size < sizeof(ann) ? (size_t)ci_size : sizeof(ann);
		if (copyin((user_addr_t)(ci_addr + (mh - text_vm)), ann, n)) {
			continue;
		}
		for (int k = 0; k < 3; k++) {
			uint64_t p = k == 0 ? ann[1] : k == 1 ? ann[4] : ann[2];
			p = (uint64_t)ptrauth_strip((void *)p, ptrauth_key_asda);
			if (!p || p < 0x100000000ULL) {
				continue;
			}
			/* Up to 1 KiB, in 160-character pieces (the console line limit cut NSError descriptions mid-way). */
			char text[1024];
			size_t done = 0;
			memset(text, 0, sizeof(text));
			int error = copyinstr((user_addr_t)p, text, sizeof(text) - 1, &done);
			if ((error == 0 || error == 63 /* ENAMETOOLONG: truncated, still printable */) && text[0]) {
				size_t length = strlen(text);
				for (size_t at = 0; at < length; at += 160) {
					printf("unixnu crash:   crash_info[%u] %s%s: %.160s\n", i, k == 0 ? "message" : k == 1 ? "message2" : "signature",
					    at ? "+" : "", text + at);
				}
				printed++;
			}
		}
	}
}

/* reboot-probe.patch: text line 22 "--r x<howto> x<pid>" when reboot_kernel is entered. */
void unixnu_reboot_screen(int howto, int pid);
void
unixnu_reboot_screen(int howto, int pid)
{
	char line[96];
	unsigned int saved = unixnu_text_line;
	unixnu_text_line = 22;
	snprintf(line, sizeof(line), "--r x%x x%x", (unsigned int)howto, (unsigned int)pid);
	unixnu_text(line);
	unixnu_text_line = saved;
}

static task_t unixnu_hang_task;
static uint32_t unixnu_hang_seconds, unixnu_hang_rounds;
static thread_call_t unixnu_hang_call;

static void
unixnu_hang_schedule(void)
{
	uint64_t deadline;
	clock_interval_to_deadline(unixnu_hang_seconds, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(unixnu_hang_call, deadline);
}

/* Set once the init exec returned 0: the probe (and its diagnostic panic) only covers a stuck exec. */
static volatile uint32_t unixnu_hang_disarmed;

static void
unixnu_hang_report(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	thread_t thread;
	char line[96];
	if (__atomic_load_n(&unixnu_hang_disarmed, __ATOMIC_ACQUIRE)) {
		return;
	}
	printf("unixnu hang probe: round %u slide %#lx\n", unixnu_hang_rounds, (unsigned long)vm_kernel_slide);
	unixnu_fill(4 + unixnu_hang_rounds, 0x0000ffff);
	/* Each round redraws the on-screen report from the top of the text area. */
	unixnu_text_line = 0;
	snprintf(line, sizeof(line), "r%x s%lx", unixnu_hang_rounds, (unsigned long)vm_kernel_slide);
	unixnu_text(line);
	task_lock(unixnu_hang_task);
	queue_iterate(&unixnu_hang_task->threads, thread, thread_t, task_threads) {
		void *continuation = ptrauth_strip((void *)thread->continuation, ptrauth_key_function_pointer);
		printf("unixnu hang probe: thread %p state %#x wait_event %#llx continuation %p\n", thread, thread->state,
		    (unsigned long long)thread->wait_event, continuation);
		snprintf(line, sizeof(line), "t%lx s%x w%llx c%lx", (unsigned long)thread, thread->state,
		    (unsigned long long)thread->wait_event, (unsigned long)continuation);
		unixnu_text(line);
		if (!thread->kernel_stack || (thread->state & TH_RUN)) {
			unixnu_text(thread->kernel_stack ? "  r" : "  -");   /* r: running/runnable, -: no kernel stack */
			/* The 21:51 photo showed the cross-call sample never returns (the target core takes no IPI), so it
			 * is only taken with boot-arg unixnu-hang-xcall=1. */
			uint32_t xcall = 0;
			if (thread->kernel_stack && PE_parse_boot_argn("unixnu-hang-xcall", &xcall, sizeof(xcall)) && xcall == 1) {
				unixnu_sample_running(thread->last_processor ? thread->last_processor->cpu_id : -1);
			}
			continue;
		}
		struct arm_kernel_saved_state *ss = &thread_get_kernel_state(thread)->machine.ss;
		vm_offset_t low = thread->kernel_stack, high = thread->kernel_stack + kernel_stack_size;
		void *saved_lr = ptrauth_strip((void *)ss->lr, ptrauth_key_return_address);
		printf("unixnu hang probe:   saved lr %p\n", saved_lr);
		uint64_t frames[24];
		int count = 0;
		uint64_t fp = ss->fp;
		for (; count < 24 && fp >= low && fp + 16 <= high && !(fp & 7); count++) {
			uint64_t *frame = (uint64_t *)fp;
			frames[count] = (uint64_t)ptrauth_strip((void *)frame[1], ptrauth_key_return_address);
			printf("unixnu hang probe:   #%d %#llx\n", count, (unsigned long long)frames[count]);
			if (frame[0] <= fp) {
				count++;
				break;
			}
			fp = frame[0];
		}
		snprintf(line, sizeof(line), "  l %lx", (unsigned long)saved_lr);
		unixnu_text(line);
		for (int i = 0; i < count; i += 4) {
			int used = snprintf(line, sizeof(line), "  #%x", i);
			for (int k = i; k < count && k < i + 4 && used < (int)sizeof(line) - 18; k++) {
				used += snprintf(line + used, sizeof(line) - used, " %llx", (unsigned long long)frames[k]);
			}
			unixnu_text(line);
		}
	}
	/*
	 * boot-arg unixnu-hang-panic=1: after the second round, panic on purpose. The arm64 panic report gathers
	 * every core's PC/LR/FP through the debugger cross-call, which is the only way to see where a thread that
	 * stays on a CPU (TH_RUN, no saved context) is executing; the panic path still drains the USB console.
	 */
	uint32_t hang_panic = 0;
	thread_t first = queue_empty(&unixnu_hang_task->threads) ? THREAD_NULL :
	    (thread_t)queue_first(&unixnu_hang_task->threads);
	int cpu = (first && first->last_processor) ? first->last_processor->cpu_id : -1;
	task_unlock(unixnu_hang_task);
	/* Every core's PC through the external debug APB (no IPI), drawn before the diagnostic panic below, whose
	 * cross-call never completes when a core is stuck with interrupts masked. */
	unixnu_pcs_report();
	if (unixnu_hang_rounds == 1 && PE_parse_boot_argn("unixnu-hang-panic", &hang_panic, sizeof(hang_panic)) &&
	    hang_panic == 1) {
		panic("unixnu hang probe: init thread %p state %#x still in exec after %u s, last cpu %d",
		    first, first ? first->state : 0, 2 * unixnu_hang_seconds, cpu);
	}
	if (++unixnu_hang_rounds < 5) {
		unixnu_hang_schedule();
	}
}

void unixnu_init_hang_probe_disarm(int error);
void
unixnu_init_hang_probe_disarm(int error)
{
	if (error || !unixnu_hang_call) {
		return;
	}
	__atomic_store_n(&unixnu_hang_disarmed, 1, __ATOMIC_RELEASE);
	thread_call_cancel(unixnu_hang_call);
	printf("unixnu hang probe: init exec returned 0, disarmed\n");
}

/*
 * boot-arg unixnu-pp=<process name>: 40 s after init's exec and then every 40 s (6 rounds), every thread of that process
 * is reported: state, wait event, the kernel frames of a blocked thread (which trap/syscall it waits in), and its user
 * pc/lr plus up to 14 user frame return addresses read from its own map, with the dyld shared cache slide, so the
 * userland wait (an XPC reply, a semaphore, a run loop) can be symbolized offline. Reads only; log lines "unixnu pp:".
 */
extern kern_return_t vm_map_read_user(vm_map_t map, vm_map_address_t src_addr, void *dst_p, vm_size_t size);
extern mach_vm_address_t task_get_all_image_info_addr(task_t);
extern char *proc_name_address(void *p);
extern queue_head_t tasks;
extern lck_mtx_t tasks_threads_lock;
static char unixnu_pp_name[64];
static uint32_t unixnu_pp_max = 6, unixnu_pp_interval = 40;
static uint32_t unixnu_pp_rounds;
static thread_call_t unixnu_pp_call;

static void
unixnu_pp_schedule(void)
{
	uint64_t deadline;
	clock_interval_to_deadline(unixnu_pp_interval, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(unixnu_pp_call, deadline);
}

static void
unixnu_pp_one(const char *unixnu_pp_target)
{
	task_t task, found = TASK_NULL;
	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, task, task_t, tasks) {
		void *proc = get_bsdtask_info(task);
		const char *name = proc ? proc_name_address(proc) : NULL;
		if (name && !strcmp(name, unixnu_pp_target)) {
			task_reference(task);
			found = task;
			break;
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);
	if (!found) {
		printf("unixnu pp: round %u: no process %s\n", unixnu_pp_rounds, unixnu_pp_target);
	} else {
		vm_map_t map = get_task_map(found);
		mach_vm_address_t infos = task_get_all_image_info_addr(found);
		uint64_t slide = 0;
		if (infos) {
			(void)vm_map_read_user(map, infos + 0x98, &slide, sizeof(slide));
		}
		printf("unixnu pp: round %u %s pid %d cache slide %#llx\n", unixnu_pp_rounds, unixnu_pp_target, task_pid(found), slide);
		thread_t thread;
		unsigned index = 0;
		task_lock(found);
		queue_iterate(&found->threads, thread, thread_t, task_threads) {
			char line[200];
			int used = snprintf(line, sizeof(line), "unixnu pp: t%u state %#x wait %#llx", index++, thread->state,
			    (unsigned long long)thread->wait_event);
			if (thread->kernel_stack && !(thread->state & TH_RUN)) {
				struct arm_kernel_saved_state *ss = &thread_get_kernel_state(thread)->machine.ss;
				vm_offset_t low = thread->kernel_stack, high = thread->kernel_stack + kernel_stack_size;
				uint64_t fp = ss->fp;
				used += snprintf(line + used, sizeof(line) - used, " k");
				for (int count = 0; count < 8 && fp >= low && fp + 16 <= high && !(fp & 7) && used < (int)sizeof(line) - 20; count++) {
					uint64_t *frame = (uint64_t *)fp;
					used += snprintf(line + used, sizeof(line) - used, " %lx",
					    (unsigned long)ptrauth_strip((void *)frame[1], ptrauth_key_return_address));
					if (frame[0] <= fp) {
						break;
					}
					fp = frame[0];
				}
			}
			printf("%s\n", line);
			arm_saved_state_t *us = get_user_regs(thread);
			if (us) {
				uint64_t pc = get_saved_state_pc(us), lr = get_saved_state_lr(us), ufp = get_saved_state_fp(us);
				used = snprintf(line, sizeof(line), "unixnu pp:   u pc %llx lr %llx", (unsigned long long)pc,
				    (unsigned long long)((uint64_t)ptrauth_strip((void *)lr, ptrauth_key_return_address)));
				for (int count = 0; count < 14 && ufp && !(ufp & 7) && used < (int)sizeof(line) - 14; count++) {
					uint64_t frame[2];
					if (vm_map_read_user(map, ufp, frame, sizeof(frame)) != KERN_SUCCESS) {
						break;
					}
					used += snprintf(line + used, sizeof(line) - used, " %llx",
					    (unsigned long long)((uint64_t)ptrauth_strip((void *)frame[1], ptrauth_key_return_address) & 0xfffffffffULL));
					if (frame[0] <= ufp) {
						break;
					}
					ufp = frame[0];
				}
				printf("%s\n", line);
			}
		}
		task_unlock(found);
		task_deallocate(found);
	}
}

static void
unixnu_pp_report(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	char names[sizeof(unixnu_pp_name)];
	strlcpy(names, unixnu_pp_name, sizeof(names));
	for (char *cursor = names; cursor && *cursor;) {
		char *comma = strchr(cursor, ',');
		if (comma) {
			*comma = 0;
		}
		if (*cursor) {
			unixnu_pp_one(cursor);
		}
		cursor = comma ? comma + 1 : NULL;
	}
	if (++unixnu_pp_rounds < unixnu_pp_max) {
		unixnu_pp_schedule();
	}
}

static void
unixnu_pp_arm(void)
{
	if (unixnu_pp_call || !PE_parse_boot_argn("unixnu-pp", unixnu_pp_name, sizeof(unixnu_pp_name)) || !unixnu_pp_name[0]) {
		return;
	}
	unixnu_pp_name[sizeof(unixnu_pp_name) - 1] = 0;
	(void)PE_parse_boot_argn("unixnu-pp-rounds", &unixnu_pp_max, sizeof(unixnu_pp_max));
	(void)PE_parse_boot_argn("unixnu-pp-interval", &unixnu_pp_interval, sizeof(unixnu_pp_interval));
	if (unixnu_pp_max < 1 || unixnu_pp_max > 60) {
		unixnu_pp_max = 6;
	}
	if (unixnu_pp_interval < 5) {
		unixnu_pp_interval = 40;
	}
	unixnu_pp_call = thread_call_allocate_with_options(unixnu_pp_report, NULL, THREAD_CALL_PRIORITY_KERNEL, 0);
	printf("unixnu pp: armed for %s\n", unixnu_pp_name);
	unixnu_pp_schedule();
}

void unixnu_init_hang_probe_arm(task_t task);
void
unixnu_init_hang_probe_arm(task_t task)
{
	unixnu_pp_arm();
	uint32_t seconds = 0;
	if (!PE_parse_boot_argn("unixnu-hang-probe", &seconds, sizeof(seconds)) || !seconds || unixnu_hang_call || !task) {
		return;
	}
	task_reference(task);
	unixnu_hang_task = task;
	unixnu_hang_seconds = seconds;
	unixnu_pcs_map();
	unixnu_hang_call = thread_call_allocate_with_options(unixnu_hang_report, NULL, THREAD_CALL_PRIORITY_KERNEL, 0);
	printf("unixnu hang probe: armed, %u s\n", seconds);
	unixnu_hang_schedule();
}
'''
make('bsd/kern/bsd_init.c', rootauth, P / 'rootauth-probe.patch')
make('bsd/kern/kern_exec.c', exec_, P / 'init-exec-probe.patch')
make('osfmk/arm64/pcb.c', lambda s: s + PROBE, P / 'init-hang-probe.patch')
for name in reversed(NAMES):
    subprocess.run(['git', '-C', str(work), 'apply', str(P / name)], check=True)
print('all three applied')
