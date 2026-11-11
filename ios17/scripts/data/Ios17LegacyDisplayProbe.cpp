// SPDX-License-Identifier: BSD-2-Clause-Patent
// iOS 17.7 21H16 (fuxi, arm64e) companion of drivers/GopFramebuffer/NativeLegacyDisplay.cpp: the boot-time lifecycle
// (register the class, attach the display to a GOP framebuffer nub under /arm-io, start it) and the hooks the display
// calls. A trimmed form of the iPadOS package/LegacyDisplayProbe.cpp: no GPU hand-off, no GOP presenter, no
// boot-progress scanout arbitration, no user-client interposition. Observation hooks only count and log.
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSData.h>
#include <kern/task.h>
#include <sys/proc.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void native_legacy_display_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
static IOService *pendingLegacyDisplay, *activeLegacyDisplay, *activeNub;
static IORegistryEntry *unitScaleChosen;
static bool unitScaleAdded, unitRotationAdded;

extern "C" {
volatile uint32_t native_legacy_stage;
volatile int native_legacy_meta_status = -1;
volatile uint32_t native_legacy_init_result, native_legacy_start_result, native_legacy_probe_match, native_legacy_dt_attached;
volatile uint64_t native_legacy_object, native_legacy_nub;
volatile int native_legacy_surface_status = -1;
volatile uint32_t native_legacy_surface_found;
volatile uint32_t native_legacy_swap_accepted_count, native_legacy_swap_rejected_count, native_legacy_rej_reason;
volatile uint32_t native_legacy_transfer_calls, native_legacy_retirement_calls, native_legacy_client_calls, native_legacy_frames;
volatile int native_legacy_transfer_status = -1, native_legacy_client_status = -1, native_legacy_pm_status = -1;
volatile uint32_t native_legacy_pm_stage, native_legacy_vsync_events[6];
volatile uint32_t native_legacy_gpu_fallbacks;
volatile uint32_t native_ufs_dpu_state;
}

static void stage(unsigned value, const char *what)
{
	native_legacy_stage = value;
	printf("ios17 legacy display: stage %u %s\n", value, what);
}

static void finishUnitScalePolicy(bool keep)
{
	if (unitScaleChosen && !keep) {
		if (unitScaleAdded) unitScaleChosen->removeProperty("display-scale");
		if (unitRotationAdded) unitScaleChosen->removeProperty("display-rotation");
	}
	OSSafeReleaseNULL(unitScaleChosen);
	unitScaleAdded = unitRotationAdded = false;
}

static bool publishOrMatchWord(IORegistryEntry *entry, const char *key, uint32_t value, bool &added)
{
	OSObject *previous = entry->getProperty(key);
	if (previous) {
		OSData *data = OSDynamicCast(OSData, previous);
		uint32_t word = 0;
		if (!data || data->getLength() != sizeof(word)) return false;
		memcpy(&word, data->getBytesNoCopy(), sizeof(word));
		return word == value;
	}
	OSData *data = OSData::withBytes(&value, sizeof(value));
	bool result = data && entry->setProperty(key, data);
	OSSafeReleaseNULL(data);
	added = result;
	return result;
}

extern "C" {
// No boot-progress console shares the scanout on this build: the display owns it.
bool native_legacy_prepare_scanout(void) { return true; }
// GPU hand-off and direct DPU scanout are not part of the iOS 17 bring-up: every frame takes the CPU copy.
// Weak: Adreno740 / GpuPowerProbe provide the real ones when the GPU is linked.
__attribute__((weak)) bool native_legacy_gpu_offer(void *, IOMemoryDescriptor *, uint64_t, uint32_t, uint32_t, uint32_t) { return false; }
void native_legacy_gpu_fallback_observed(int) { native_legacy_gpu_fallbacks = native_legacy_gpu_fallbacks + 1; }
__attribute__((weak)) bool native_adreno_fences(uint32_t *submitted, uint32_t *retired) { (void)submitted; (void)retired; return false; }
uint32_t native_ufs_dpu_map(unsigned, const uint64_t *, unsigned) { return 0; }

__attribute__((noreturn, noinline)) void native_legacy_backend_guard(unsigned slot, void *object, void *caller)
{
	panic("NativeLegacyDisplay: unimplemented leaf %u object %p caller %p stage %u", slot, object, caller, native_legacy_stage);
}
void native_legacy_pm_observed(unsigned stage_, IOReturn status, unsigned long ordinal)
{
	native_legacy_pm_stage = stage_; native_legacy_pm_status = status;
	printf("ios17 legacy display: pm stage %u status %#x ordinal %lu\n", stage_, status, ordinal);
}
void native_legacy_surface_rejected(uint64_t w, uint64_t h, uint32_t format, uint32_t planes, uint64_t stride, uint64_t allocation, uint64_t bytes)
{
	printf("ios17 legacy display: surface rejected %llux%llu fmt %#x planes %u stride %llu alloc %llu bytes %llu\n",
	    w, h, format, planes, stride, allocation, bytes);
}
void native_legacy_frame_observed(unsigned kind, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
	uint32_t n = native_legacy_frames + 1;
	native_legacy_frames = n;
	if (n <= 3 || (n & (n - 1)) == 0) printf("ios17 legacy display: frame %u kind %u\n", n, kind);
}
void native_legacy_frame_colors_observed(const uint32_t *, const uint64_t *, uint64_t) {}
void native_legacy_swap_stage(unsigned) {}
void native_legacy_vsync_observed(unsigned event, unsigned hz)
{
	if (event < 6) native_legacy_vsync_events[event] = native_legacy_vsync_events[event] + 1;
	if (event == 4 || event == 5) printf("ios17 legacy display: vsync event %u hz %u\n", event, hz);
}
void native_legacy_swap_validation_observed(bool accepted, unsigned reason, const void *, const void *)
{
	if (accepted) {
		uint32_t n = native_legacy_swap_accepted_count + 1;
		native_legacy_swap_accepted_count = n;
		if (n == 1) printf("ios17 legacy display: first swap accepted\n");
	} else {
		uint32_t n = native_legacy_swap_rejected_count + 1;
		native_legacy_swap_rejected_count = n;
		native_legacy_rej_reason = reason;
		if (n <= 8) printf("ios17 legacy display: swap rejected reason %u (%u)\n", reason, n);
	}
}
void native_legacy_copy_timing_observed(uint64_t, uint64_t) {}
// Scanout sampler: what is actually in the GOP buffer after a transfer. Bounded strided reads only, no allocation.
static volatile uint32_t *gScanout;
static uint32_t gScanoutW, gScanoutH, gScanoutStride, gScanoutSamples;
static void sample_scanout(const char *when)
{
	if (!gScanout || !gScanoutW || !gScanoutH) {printf("ios17 legacy display: scanout %s unavailable\n", when); return;}
	const uint32_t stepX = gScanoutW / 32 ? gScanoutW / 32 : 1, stepY = gScanoutH / 32 ? gScanoutH / 32 : 1;
	uint32_t nonblack = 0, maxChannel = 0, total = 0, dark = 0, blue = 0;
	uint32_t firstPixel = 0, midPixel = 0;
	for (uint32_t y = 0; y < gScanoutH; y += stepY) {
		for (uint32_t x = 0; x < gScanoutW; x += stepX) {
			const uint32_t px = gScanout[(uint64_t)y * (gScanoutStride / 4) + x];
			if (!total) firstPixel = px;
			if (total == 512) midPixel = px;
			const uint32_t b = px & 0xff, g = (px >> 8) & 0xff, r = (px >> 16) & 0xff;
			const uint32_t mx = b > g ? (b > r ? b : r) : (g > r ? g : r);
			if (px & 0xffffff) nonblack++;
			if (mx > maxChannel) maxChannel = mx;
			if (mx < 8) dark++;
			if (b > r + 8 && b > g + 8) blue++;
			total++;
		}
	}
	gScanoutSamples++;
	printf("ios17 legacy display: scanout %s n%u nonblack %u/%u dark %u blue %u max %u first %08x mid %08x\n",
	       when, gScanoutSamples, nonblack, total, dark, blue, maxChannel, firstPixel, midPixel);
}
__attribute__((noinline)) void native_legacy_transfer_observed(IOReturn status, const void *, uint64_t bytes)
{
	native_legacy_transfer_calls = native_legacy_transfer_calls + 1; native_legacy_transfer_status = status;
	if (status || native_legacy_transfer_calls == 1)
		printf("ios17 legacy display: transfer status %#x bytes %llu\n", status, bytes);
	// Contended scanout writes can tear, so only the first few frames are sampled to keep the log small.
	if (native_legacy_transfer_calls <= 4 || native_legacy_transfer_calls == 60) sample_scanout("after-transfer");
}
__attribute__((noinline)) void native_legacy_retirement_observed(IOService *) { native_legacy_retirement_calls = native_legacy_retirement_calls + 1; }
void native_legacy_registration_observed(IOService *display)
{
	printf("ios17 legacy display: registered %p\n", display);
}
void native_legacy_client_enter(task_t task, uint32_t type)
{
	native_legacy_client_calls = native_legacy_client_calls + 1;
	int pid = task ? proc_pid((proc_t)get_bsdtask_info(task)) : 0;
	printf("ios17 legacy display: newUserClient type %u pid %d\n", type, pid);
}
void native_legacy_client_observed(IOReturn status, IOUserClient *client)
{
	native_legacy_client_status = status;
	printf("ios17 legacy display: user client %s status %#x\n", client ? client->getMetaClass()->getClassName() : "(none)", status);
}

// The display has mapped the GOP framebuffer. With offline-legacy-unit-scale=<1..3> (phase 4) publish the UI scale
// (scanout pixels per logical point) and rotation in /chosen, where the original display code reads them.
__attribute__((noinline)) bool native_legacy_gop_mapping_observed(IOService *object, IOMemoryMap *mapping, uint32_t width, uint32_t height)
{
	if ((uintptr_t)object != native_legacy_object) panic("NativeLegacyDisplay: wrong mapping owner");
	printf("ios17 legacy display: GOP mapping va %#llx pa %#llx bytes %#llx %ux%u\n", (uint64_t)mapping->getVirtualAddress(),
	    (uint64_t)mapping->getPhysicalAddress(), (uint64_t)mapping->getLength(), width, height);
	gScanout = (volatile uint32_t *)(uintptr_t)mapping->getVirtualAddress();
	gScanoutW = width; gScanoutH = height; gScanoutStride = (uint32_t)(mapping->getLength() / (height ? height : 1));
	sample_scanout("at-mapping");
	unsigned enabled = 0; PE_parse_boot_argn("offline-legacy-unit-scale", &enabled, sizeof(enabled));
	if (!enabled) return true;
	unsigned phase = 0; PE_parse_boot_argn("offline-legacy-phase", &phase, sizeof(phase));
	PE_Video video = {};
	if (enabled < 1 || enabled > 3 || phase != 4 || unitScaleChosen || PE_current_console(&video) ||
	    video.v_width != width || video.v_height != height || video.v_rotate ||
	    mapping->getPhysicalAddress() != video.v_baseAddr) return false;
	unitScaleChosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
	if (!unitScaleChosen || !publishOrMatchWord(unitScaleChosen, "display-scale", enabled, unitScaleAdded) ||
	    !publishOrMatchWord(unitScaleChosen, "display-rotation", video.v_rotate, unitRotationAdded)) {
		finishUnitScalePolicy(false); return false;
	}
	printf("ios17 legacy display: /chosen display-scale %u rotation %u\n", enabled, video.v_rotate);
	return true;
}

// Early (bsd_init, after IOMobileGraphicsFamily loaded, boot-arg offline-legacy-register=1): register the display's
// metaclass and create the instance.
void native_legacy_display_probe(void);
void native_legacy_display_probe(void)
{
	if (native_legacy_stage) return;
	stage(1, "register");
	void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
	if (!handle) { stage(90, "preModLoad failed"); return; }
	native_legacy_display_static_init();
	native_legacy_meta_status = OSMetaClass::postModLoad(handle);
	if (native_legacy_meta_status) { printf("ios17 legacy display: postModLoad %d\n", native_legacy_meta_status); stage(91, "metaclass failed"); return; }
	PE_Video video = {};
	if (PE_current_console(&video) || !video.v_baseAddr || !video.v_width || !video.v_height || video.v_depth != 32 ||
	    video.v_rotate || video.v_width > 16384 || video.v_height > 16384 || video.v_rowBytes < video.v_width * 4) {
		stage(92, "no usable GOP console"); return;
	}
	IOService *display = native_legacy_display_allocate();
	native_legacy_object = (uintptr_t)display;
	if (!display) { stage(93, "allocate failed"); return; }
	printf("ios17 legacy display: object %p size %u parents %d\n", display, (unsigned)display->getMetaClass()->getClassSize(),
	    display->metaCast("IOMobileFramebufferLegacy") && display->metaCast("IOMobileFramebuffer") && display->metaCast("IOService"));
	stage(2, "init");
	native_legacy_init_result = display->init();
	if (!native_legacy_init_result) { stage(94, "init failed"); display->release(); return; }
	pendingLegacyDisplay = display;
	stage(3, "pending until IOSurface");
}

// Late (after IOKitBSDInit): wait for IOSurface, publish the GOP framebuffer as an IOPlatformDevice under /arm-io
// (display-subsystem), then probe/attach/start the display on it.
void native_legacy_display_late_probe(void);
extern "C" void sheng_gpu_power_probe(void);
// The AppleKeyStore stand-in (kits/sheng/KeyStoreProbe.cpp, drivers/AppleKeyStoreState): SpringBoard waits for the class.
extern "C" void native_keystore_probe(void);
// The fuxi FTM5 touch controller and its HID digitizer (kits/sheng/FuxiTouchProbe.cpp, fuxi-touch=4).
extern "C" void native_touch_probe(void);
// The fuxi buttons (kits/sheng/FuxiButtons.cpp, boot-arg fuxi-buttons=1): PMK8550 PON/RESIN and PM8550 GPIO6 over the SPMI
// observer, reported through NativeHidInput report 2.
extern "C" void fuxi_buttons_probe(void);
// The panel brightness service: BacklightProbe.cpp starts AppleARMBacklight.cpp (boot-arg sheng-backlight=1),
// which publishes backlight-control for CoreBrightness. The 21H16 late probe never called it, so the panel
// stayed dark no matter which backlight boot arguments were set.
extern "C" void sheng_backlight_probe(void);
void native_legacy_display_late_probe(void)
{
	// Board GPU bring-up (kits/sheng/GpuPowerProbe.cpp); does nothing unless boot-arg sheng-gpu-power is set.
	native_keystore_probe();
	sheng_gpu_power_probe();
	native_touch_probe();
	fuxi_buttons_probe();
	// Panel brightness runs before the display-object check: it does not need the GOP nub, and returning early
	// on a missing IOSurface must not also leave the screen unlit.
	sheng_backlight_probe();
	IOService *display = pendingLegacyDisplay;
	if (!display) return;
	pendingLegacyDisplay = nullptr;
	native_legacy_surface_status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
	OSDictionary *matching = IOService::nameMatching("IOCoreSurfaceRoot");
	IOService *surface = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
	native_legacy_surface_found = surface != nullptr;
	OSSafeReleaseNULL(surface); OSSafeReleaseNULL(matching);
	printf("ios17 legacy display: IOSurface load %d root %u\n", native_legacy_surface_status, native_legacy_surface_found);
	PE_Video video = {};
	if (native_legacy_surface_status || !native_legacy_surface_found || PE_current_console(&video) || !video.v_baseAddr ||
	    video.v_depth != 32 || video.v_rotate || video.v_rowBytes < video.v_width * 4) {
		stage(95, "no IOSurface or GOP"); display->release(); return;
	}
	stage(4, "GOP nub");
	IOPlatformDevice *nub = new IOPlatformDevice;
	native_legacy_nub = (uintptr_t)nub;
	bool nubAttached = false, nubStarted = false, attached = false, started = false, dtAttached = false;
	IORegistryEntry *displayTreeParent = nullptr;
	// The nub's provider must be the platform expert: IOPlatformDevice::compareName (device-tree name lookups, e.g.
	// io_device_tree_entry_exists_with_name) calls ((IOPlatformExpert *)getProvider())->compareNubName(). Under
	// IOResources (the iPadOS probe's parent) that virtual call fails pointer authentication on arm64e
	// (2026-11-07 20:10 screen: FPAC in IOPlatformDevice::compareName).
	IOService *parent = IOService::getPlatform();
	IODeviceMemory *memory = IODeviceMemory::withRange(video.v_baseAddr, video.v_rowBytes * video.v_height);
	OSArray *ranges = OSArray::withCapacity(1);
	if (nub && memory && ranges && parent && nub->init()) {
		ranges->setObject(memory); nub->setDeviceMemory(ranges); nub->setName("NativeGOPFramebuffer");
		nubAttached = nub->attach(parent);
		if (nubAttached) nubStarted = nub->start(parent);
		if (nubStarted) {
			unsigned phase = 0; PE_parse_boot_argn("offline-legacy-phase", &phase, sizeof(phase));
			if (phase == 4) {
				displayTreeParent = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
				const char type[] = "display-subsystem";
				OSData *data = OSData::withBytes(type, sizeof(type));
				if (displayTreeParent && data && nub->setProperty("device_type", data)) {
					nub->setName("gop-display", gIODTPlane);
					dtAttached = nub->attachToParent(displayTreeParent, gIODTPlane);
				}
				OSSafeReleaseNULL(data);
				native_legacy_dt_attached = dtAttached;
			}
			SInt32 score = 0;
			native_legacy_probe_match = (phase != 4 || dtAttached) && display->probe(nub, &score) == display;
			if (native_legacy_probe_match) attached = display->attach(nub);
			if (attached) { stage(5, "start"); started = display->start(nub); native_legacy_start_result = started; }
			if (started && phase == 4) {
				activeLegacyDisplay = display; activeNub = nub;
				finishUnitScalePolicy(true);
				OSSafeReleaseNULL(displayTreeParent);
				stage(10, "display started");
				return;
			}
		}
	}
	printf("ios17 legacy display: failed nub %d/%d dt %d probe %u attach %d start %d\n", nubAttached, nubStarted, dtAttached,
	    native_legacy_probe_match, attached, started);
	stage(6, "unwind");
	finishUnitScalePolicy(false);
	if (started) display->stop(nub);
	if (attached) display->detach(nub);
	if (dtAttached) nub->detachFromParent(displayTreeParent, gIODTPlane);
	if (nubStarted) nub->stop(parent);
	if (nubAttached) nub->detach(parent);
	OSSafeReleaseNULL(ranges); OSSafeReleaseNULL(memory); OSSafeReleaseNULL(nub); OSSafeReleaseNULL(displayTreeParent);
	display->release();
	stage(8, "released");
}
}
