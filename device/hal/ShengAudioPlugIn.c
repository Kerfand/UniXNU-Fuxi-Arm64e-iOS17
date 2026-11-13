// SPDX-License-Identifier: BSD-2-Clause-Patent
// The sheng built-in audio device for the iOS audio server: one device with a stereo output stream (the six speakers) and a mono input
// stream (the microphones). Samples move through rings shared with the kernel service ShengAudio (package/ShengAudioAbi.h); the service
// feeds the audio DSP. Objects: the plug-in (1), the device (2), the output stream (3), the input stream (4), the output volume (5).
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <math.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <os/log.h>
#include "ShengAudioAbi.h"

enum {kPlugIn = kAudioObjectPlugInObject, kDevice = 2, kOutStream = 3, kInStream = 4, kVolume = 5};
// fuxi (Codec impersonation): the HAL answers a device's 'mute' and 'volm' itself, from the control objects of that scope and element; with
// none for output element 4 (the built-in speaker of VirtualAudio's Codec device) it answers 'who?' and Device_Codec_Aspen throws while it
// creates the device. A mute and a volume control for output element 4: the mute is the output mute, the volume shares the main one.
enum {kMute4 = 6, kVolume4 = 7, kLastObject = kVolume4};
static int isControl(AudioObjectID o) {return o == kVolume || o == kMute4 || o == kVolume4;}
#define kPlugInBundleID "org.unixnu.sheng.audio"
// fuxi (iPhone15,2 identity): the UID of the built-in AOP audio device of iPhone 14 Pro, through which VirtualAudio's D73 routing database
// builds its built-in ports (speaker, receiver, mics: "AOP Audio-1" at VirtualAudio 0x20b90, 0x443b0, 0x56694 PortUtilities.cpp,
// 0x2074b0, 0x2aa4ac). That database has no Thunderbolt port, so the iPad's Thunderbolt device gets no route on this identity.
// VirtualAudio's device factory (0x1e8d38) picks the device class by UID: Codec, Baseband Voice, USB Audio Output, USB IDAM, Speaker,
// AppleSongbirdDSP, PuffinInput, PuffinOutput, Actuator, AOP Audio-1. The built-in speaker/receiver/mic ports come from Device_Codec_Aspen
// ("creating a speaker port", VirtualAudio 0x208f84), the class of the "Codec" device: this device answers as that one.
#define kDeviceUID "Codec"
#define kModelUID "ShengAudioModel"
static const Float64 kRate = SHENG_AUDIO_RATE;
// The time stamp period. It must exceed the safety offset plus an IO cycle, or the audio server never runs IO (i543: 8192, no writes).
static const UInt32 kZeroPeriod = SHENG_AUDIO_RING_FRAMES;
// The time stamps start at the frame IO started on, as in Apple's sample driver, not on a grid from boot: the audio server took the first
// stamp, up to a period old, for the start, and wrote that much late (least write-ahead -734 frames with a 16384 period, i539; about
// -9700 with 65536, i540 to i542). Sample times stay those of the shared timeline. Playback is smooth this way (i544); i545 went back to
// the grid with a 32768 period and SimpleIIR, and both directions stuttered.
static uint64_t gZeroOrigin, gZeroSeed;

static AudioServerPlugInHostRef gHost;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint gRefs = 1;
static io_connect_t gConnect;
static ShengAudioControl *gControl;
static int16_t *gOut, *gIn;
static UInt32 gIoUsers;
static Float32 gVolume = 1.0f;   // the volume control's scalar (slider position)
// fuxi (AOP impersonation): the device's output mute, per element. VirtualAudio's Device_Codec_Aspen creates the built-in speaker port
// only when the "AOP Audio-1" device has kFuxiDevicePropertyMute on the output scope, element 4 (VirtualAudio 0x208f28, address at
// 0x456ca0); the real AOP device carries one mute per built-in output. Every output element answers, muted state shared.
static UInt32 gOutMute;
// The output stream's channels (fuxi, Codec impersonation): VirtualAudio addresses each built-in output by element (the speaker's mute is
// element 4), and the HAL answers 'who?' for an element past the scope's channel count before the plug-in sees the request, so the
// device offers 8 output channels. The ring stays stereo: even channels mix into the left, odd ones into the right.
enum {kOutCh = 8};
// kAudioDevicePropertyMute ('mute'): no longer declared by the SDK's CoreAudio headers.
enum {kFuxiDevicePropertyMute = 'mute'};
static Float64 gHostTicksPerFrame;
static void setHostClock(void);

// ---- Kernel service ------------------------------------------------------------------------------------------------------------
static int mapType(uint32_t type, void **out) {
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;
    if (IOConnectMapMemory64(gConnect, type, mach_task_self(), &address, &size, kIOMapAnywhere) != kIOReturnSuccess) return -1;
    *out = (void *)(uintptr_t)address;
    return 0;
}
static int connectService(void) {
    if (gControl) return 0;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(SHENG_AUDIO_CLASS));
    if (!service) return -1;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &gConnect);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) return -1;
    void *control = NULL, *out = NULL, *in = NULL;
    if (mapType(ShengAudioMemoryControl, &control) || mapType(ShengAudioMemoryOutput, &out) || mapType(ShengAudioMemoryInput, &in)) {
        IOServiceClose(gConnect); gConnect = 0; return -1;
    }
    ShengAudioControl *c = control;
    if (c->magic != SHENG_AUDIO_MAGIC || c->version != SHENG_AUDIO_VERSION) {IOServiceClose(gConnect); gConnect = 0; return -1;}
    gOut = out; gIn = in; gControl = c;
    gHostTicksPerFrame = (Float64)c->ticks_per_frame_q32 / 4294967296.0;
    setHostClock();
    return 0;
}
static kern_return_t call(uint32_t selector) {return gConnect ? IOConnectCallScalarMethod(gConnect, selector, NULL, 0, NULL, NULL) : KERN_FAILURE;}

// CoreAudio's host clock frequency is a constant of 24 MHz (Apple's timer) that its IO clock and IO thread read directly; this board's timer
// runs at 19.2 MHz, so the audio server's IO cycles came 25 % late (i546), and each recording fell silent from about 1.0 to 1.8 s until its
// clock caught up (i548 to i551; gone with the constant set, i552). AudioGetHostClockFrequency loads it with adrp and ldr d0: the address
// is taken from those two instructions and the service sets the constant (on the shared page it cannot be written from here).
static void setHostClock(void) {
    ShengAudioDiag *diag = (ShengAudioDiag *)((uint8_t *)gControl + SHENG_AUDIO_DIAG_OFFSET);
    // dlsym signs the pointer for an arm64e caller: the code is read through the stripped address and called through the signed one
    // (an arm64 build sees the signature bits only in an arm64e server, and neither reads nor calls through them).
    void *const signed_symbol = dlsym(RTLD_DEFAULT, "AudioGetHostClockFrequency");
#if __has_feature(ptrauth_calls)
    const uintptr_t symbol = (uintptr_t)ptrauth_strip(signed_symbol, ptrauth_key_function_pointer);
    Float64 (*frequency)(void) = (Float64 (*)(void))signed_symbol;
#else
    const uintptr_t symbol = (uintptr_t)signed_symbol & (((uintptr_t)1 << 47) - 1);
    Float64 (*frequency)(void) = (Float64 (*)(void))symbol;
#endif
    if (!symbol) {diag->host_clock_result = -1; return;}
    const uint32_t *code = (const uint32_t *)symbol, adrp = code[0], ldr = code[1];
    if ((adrp & 0x9f000000u) != 0x90000000u || (ldr & 0xffc00000u) != 0xfd400000u || ((ldr >> 5) & 31) != (adrp & 31)) {
        diag->host_clock_result = -1;
        return;
    }
    int64_t page = (int64_t)(((adrp >> 5) & 0x7ffffu) << 2 | ((adrp >> 29) & 3u));
    if (page & (1 << 20)) page -= 1 << 21;
    const uint64_t address = ((uintptr_t)code & ~(uintptr_t)0xfff) + (uint64_t)(page << 12) + ((ldr >> 10) & 0xfffu) * 8;
    const kern_return_t r = IOConnectCallScalarMethod(gConnect, ShengAudioSetHostClock, &address, 1, NULL, NULL);
    diag->host_clock_result = r;
    diag->host_clock = (uint32_t)frequency();
}

// ---- Helpers ---------------------------------------------------------------------------------------------------------------------
static AudioStreamBasicDescription format(UInt32 channels) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = kRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * channels; f.mFramesPerPacket = 1; f.mBytesPerFrame = 4 * channels;
    f.mChannelsPerFrame = channels; f.mBitsPerChannel = 32;
    return f;
}
#define REPLY(type, value) do { if (inDataSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; *(type *)outData = (value); *outDataSize = sizeof(type); return 0; } while (0)
#define REPLY_STR(text) REPLY(CFStringRef, CFSTR(text))
static UInt32 latencyFrames(void) {return gControl ? gControl->lead_frames : 2 * SHENG_AUDIO_PERIOD_FRAMES;}

// ---- IUnknown --------------------------------------------------------------------------------------------------------------------
static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out);
static ULONG AddRef(void *self) {(void)self; return atomic_fetch_add(&gRefs, 1) + 1;}
static ULONG Release(void *self) {(void)self; UInt32 r = atomic_load(&gRefs); if (r) r = atomic_fetch_sub(&gRefs, 1) - 1; return r;}

// Connects at once, so the service sees the audio server load the plug-in; a failure here is retried at the first StartIO.
static OSStatus Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host) {
    (void)d;
    gHost = host;
    pthread_mutex_lock(&gLock);
    const int connected = connectService();
    pthread_mutex_unlock(&gLock);
    os_log_error(OS_LOG_DEFAULT, "ShengAudio: loaded, service %{public}s", connected ? "not connected" : "connected");
    if (gControl) {
        ShengAudioDiag *diag = (ShengAudioDiag *)((uint8_t *)gControl + SHENG_AUDIO_DIAG_OFFSET);
        diag->loads += 1;
        diag->volume_milli = (UInt32)(gVolume * 1000.0f);
    }
    return 0;
}
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *c, AudioObjectID *o) {(void)d; (void)desc; (void)c; (void)o; return kAudioHardwareUnsupportedOperationError;}
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o) {(void)d; (void)o; return kAudioHardwareUnsupportedOperationError;}
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {(void)d; (void)o; (void)c; return 0;}
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {(void)d; (void)o; (void)c; return 0;}
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) {(void)d; (void)o; (void)a; (void)i; return 0;}
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) {(void)d; (void)o; (void)a; (void)i; return 0;}

// ---- Properties ------------------------------------------------------------------------------------------------------------------
static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a);
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *q, UInt32 *size);
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 inDataSize, UInt32 *outDataSize, void *outData);

static void trace(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 kind, OSStatus r) {
    if (!gControl) return;
    ShengAudioTrace *tr = (ShengAudioTrace *)((uint8_t *)gControl + SHENG_AUDIO_TRACE_OFFSET);
    const UInt32 info = (UInt32)o << 24 | (a->mScope >> 24) << 16 | kind | (r ? 2u : 0u);
    const UInt32 n = tr->count < SHENG_AUDIO_TRACE_SLOTS ? tr->count : SHENG_AUDIO_TRACE_SLOTS;
    for (UInt32 i = 0; i < n; ++i) if (tr->entry[i][0] == a->mSelector && tr->entry[i][1] == info) return;
    if (n < SHENG_AUDIO_TRACE_SLOTS) {tr->entry[n][0] = a->mSelector; tr->entry[n][1] = info; tr->count = n + 1;}
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    (void)d; (void)pid;
    *out = ((o == kVolume || o == kVolume4) && (a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue)) ||
           (o == kMute4 && a->mSelector == kAudioBooleanControlPropertyValue) ||
           ((o == kOutStream || o == kInStream) && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat ||
                                                     a->mSelector == kAudioStreamPropertyIsActive)) ||
           (o == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate) ||
           (o == kDevice && a->mSelector == kFuxiDevicePropertyMute && a->mScope == kAudioObjectPropertyScopeOutput);
    return HasProperty(d, o, pid, a) ? 0 : kAudioHardwareUnknownPropertyError;
}

// The output volume spans kMinDb..0 dB on a squared curve over the scalar (loud in the middle, fine steps near the bottom); scalar 0
// mutes. The audio server maps its user volume onto this range: a -96 dB floor made a third of full volume -56 dB (i532), a linear
// -40 dB range still -26 dB (i533, quiet); now it is -16 dB.
static const Float32 kMinDb = -36.0f;
static Float32 toDb(Float32 scalar) {const Float32 r = 1.0f - (scalar < 0 ? 0 : scalar > 1 ? 1 : scalar); return kMinDb * r * r;}
static Float32 toScalar(Float32 db) {return db <= kMinDb ? 0.0f : db >= 0 ? 1.0f : 1.0f - sqrtf(db / kMinDb);}
static Float32 gain(void) {return gOutMute || gVolume <= 0.0f ? 0.0f : powf(10.0f, toDb(gVolume) / 20.0f);}

static OSStatus setProperty(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 size, const void *data) {
    if (o == kMute4 && a->mSelector == kAudioBooleanControlPropertyValue) {
        if (size < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        gOutMute = *(const UInt32 *)data ? 1 : 0;
        gHost->PropertiesChanged(gHost, kMute4, 1, a);
        return 0;
    }
    if ((o == kVolume || o == kVolume4) && size >= sizeof(Float32)) {
        Float32 v = *(const Float32 *)data;
        if (a->mSelector == kAudioLevelControlPropertyDecibelValue) v = toScalar(v);
        else if (a->mSelector != kAudioLevelControlPropertyScalarValue) return kAudioHardwareUnknownPropertyError;
        gVolume = v < 0 ? 0 : v > 1 ? 1 : v;
        if (gControl) {
            ShengAudioDiag *diag = (ShengAudioDiag *)((uint8_t *)gControl + SHENG_AUDIO_DIAG_OFFSET);
            diag->volume_milli = (UInt32)(gVolume * 1000.0f);
            diag->volume_sets += 1;
        }
        AudioObjectPropertyAddress changed[2] = {{kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
                                                 {kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}};
        gHost->PropertiesChanged(gHost, kVolume, 2, changed);
        gHost->PropertiesChanged(gHost, kVolume4, 2, changed);
        return 0;
    }
    if ((o == kOutStream || o == kInStream) && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        const AudioStreamBasicDescription *f = data;
        return size >= sizeof *f && f->mSampleRate == kRate && f->mChannelsPerFrame == (o == kOutStream ? (UInt32)kOutCh : 1u) ? 0 : kAudioDeviceUnsupportedFormatError;
    }
    if (o == kDevice && a->mSelector == kFuxiDevicePropertyMute && a->mScope == kAudioObjectPropertyScopeOutput) {
        if (size < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        gOutMute = *(const UInt32 *)data ? 1 : 0;
        gHost->PropertiesChanged(gHost, kDevice, 1, a);
        return 0;
    }
    if (o == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate)
        return size >= sizeof(Float64) && *(const Float64 *)data == kRate ? 0 : kAudioDeviceUnsupportedFormatError;
    // The audio server turns streams it does not use off and on (i530); both streams stay running, so this only acknowledges it.
    if ((o == kOutStream || o == kInStream) && a->mSelector == kAudioStreamPropertyIsActive) return size >= sizeof(UInt32) ? 0 : kAudioHardwareBadPropertySizeError;
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 size, const void *data) {
    (void)d; (void)pid; (void)qs; (void)q;
    const OSStatus r = setProperty(o, a, size, data);
    trace(o, a, 1, r);
    return r;
}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a) {
    UInt32 size = 0;
    return GetPropertyDataSize(d, o, pid, a, 0, NULL, &size) == 0;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *q, UInt32 *size) {
    UInt8 scratch[1024];
    UInt32 written = 0;
    OSStatus r = GetPropertyData(d, o, pid, a, qs, q, sizeof scratch, &written, scratch);
    if (r == 0) *size = written;
    return r;
}

static UInt32 objects(AudioObjectID o, AudioObjectPropertyScope scope, AudioObjectID *list) {
    UInt32 n = 0;
    if (o == kPlugIn) list[n++] = kDevice;
    else if (o == kDevice) {
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kOutStream;
        if (scope != kAudioObjectPropertyScopeOutput) list[n++] = kInStream;
        if (scope != kAudioObjectPropertyScopeInput) {list[n++] = kVolume; list[n++] = kMute4; list[n++] = kVolume4;}
    }
    return n;
}

static OSStatus propertyData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 inDataSize,
                             UInt32 *outDataSize, void *outData) {
    const AudioObjectPropertySelector s = a->mSelector;
    // Lists every object answers, empty here: custom properties ('cust'), and on the plug-in its boxes and clock devices. The audio
    // server's device manager reads them for each new device and drops a device whose answer is an unknown-property error (i525).
    if (o <= kLastObject && (s == kAudioObjectPropertyCustomPropertyInfoList || ((s == kAudioPlugInPropertyBoxList ||
        s == kAudioPlugInPropertyClockDeviceList) && o == kPlugIn) || ((s == 'taps') && (o == kOutStream || o == kInStream)))) {
        *outDataSize = 0;
        return 0;
    }
    switch (o) {
    case kPlugIn:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            if (inDataSize < sizeof(AudioObjectID)) {*outDataSize = 0; return 0;}
            ((AudioObjectID *)outData)[0] = kDevice; *outDataSize = sizeof(AudioObjectID); return 0;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CFStringRef uid = qs >= sizeof(CFStringRef) ? *(const CFStringRef *)q : NULL;
            REPLY(AudioObjectID, uid && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo ? kDevice : kAudioObjectUnknown);
        }
        case kAudioPlugInPropertyResourceBundle: REPLY_STR("");
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kDevice:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kPlugIn);
        case kAudioObjectPropertyName: REPLY_STR("Speaker");
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioDevicePropertyDeviceUID: REPLY_STR(kDeviceUID);
        case kAudioDevicePropertyModelUID: REPLY_STR(kModelUID);
        case kAudioObjectPropertyModelName: REPLY_STR("Speaker");
        case kAudioObjectPropertySerialNumber: REPLY_STR("0001");
        case kAudioObjectPropertyFirmwareVersion: REPLY_STR("1.0");
        case kAudioObjectPropertyIdentify: REPLY(UInt32, 0);
        // Private device properties the audio server reads for each new device (i526: 'accs' and 'auto' were its last unanswered ones).
        case 'accs': REPLY(UInt32, 0);
        case 'auto': REPLY(UInt32, 0);
        // 'dpel' (fuxi, AOP impersonation): the private device property VirtualAudio asks the "AOP Audio-1" device next; CoreAudio's
        // AMCP::HAL::get_hal_property_value_type_code gives it type 3, a UInt32 (as 'tran', 'ltnc', 'dflt'). Enabled.
        case 'dpel': REPLY(UInt32, 1);
        // 'cfsz' (fuxi): a UInt32 (CoreAudio type code 3) VirtualAudio asks the Codec device while it builds the speaker's DSP chain
        // (DSPChainUtilities, VirtualAudio 0x35804c), which then dereferenced a null graph and crashed the audio server. One period.
        case 'cfsz': REPLY(UInt32, SHENG_AUDIO_PERIOD_FRAMES);
        case kFuxiDevicePropertyMute:
            if (a->mScope != kAudioObjectPropertyScopeOutput) return kAudioHardwareUnknownPropertyError;
            REPLY(UInt32, gOutMute);
        case kAudioObjectPropertyElementName: REPLY_STR("");
        case kAudioDevicePropertyConfigurationApplication: REPLY_STR("com.apple.Preferences");
        // Thunderbolt: the audio server's routing layer makes a device of this transport a driver device with one output and one input
        // port of its own. Built-in devices are only used through Apple's port descriptions, and a USB device must carry a USB location
        // ('lcID') that the HAL does not take from a plug-in (i529).
        case kAudioDevicePropertyTransportType: REPLY(UInt32, kAudioDeviceTransportTypeBuiltIn);
        case kAudioDevicePropertyRelatedDevices: REPLY(AudioObjectID, kDevice);
        case kAudioDevicePropertyClockDomain: REPLY(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: REPLY(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: REPLY(UInt32, gIoUsers > 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: REPLY(UInt32, 1);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: REPLY(UInt32, 1);
        case kAudioDevicePropertyLatency: REPLY(UInt32, latencyFrames());
        // The service takes a period lead_frames before it plays (its own queue and the DSP's); the service measures and publishes it, so
        // the audio server writes each frame at least that early.
        case kAudioDevicePropertySafetyOffset: REPLY(UInt32, latencyFrames() + 2 * SHENG_AUDIO_PERIOD_FRAMES);
        case kAudioDevicePropertyNominalSampleRate: REPLY(Float64, kRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: REPLY(AudioValueRange, ((AudioValueRange){kRate, kRate}));
        case kAudioDevicePropertyIsHidden: REPLY(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: REPLY(UInt32, kZeroPeriod);
        case kAudioDevicePropertyClockIsStable: REPLY(UInt32, 1);
        // Raw: the zero time stamps are exact (one host clock), so smoothing only delays the audio server's view of them.
        case kAudioDevicePropertyClockAlgorithm: REPLY(UInt32, kAudioDeviceClockAlgorithmRaw);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)outData)[0] = 1; ((UInt32 *)outData)[1] = a->mScope == kAudioObjectPropertyScopeInput ? 1 : 2;
            *outDataSize = 2 * sizeof(UInt32); return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            const UInt32 channels = a->mScope == kAudioObjectPropertyScopeInput ? 1 : kOutCh;
            const UInt32 bytes = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + channels * sizeof(AudioChannelDescription));
            if (inDataSize < bytes) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *l = outData;
            memset(l, 0, bytes);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = channels;
            l->mChannelDescriptions[0].mChannelLabel = channels == 1 ? kAudioChannelLabel_Mono : kAudioChannelLabel_Left;
            if (channels > 1) l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            for (UInt32 c = 2; c < channels; ++c) l->mChannelDescriptions[c].mChannelLabel = (AudioChannelLabel)(kAudioChannelLabel_Discrete_0 + c);
            *outDataSize = bytes; return 0;
        }
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList: {
            AudioObjectID list[8];
            UInt32 n = objects(kDevice, a->mScope, list), out = 0;
            for (UInt32 i = 0; i < n; ++i) {
                if (s == kAudioDevicePropertyStreams && isControl(list[i])) continue;
                if (s == kAudioObjectPropertyControlList && !isControl(list[i])) continue;
                if ((out + 1) * sizeof(AudioObjectID) > inDataSize) break;
                ((AudioObjectID *)outData)[out++] = list[i];
            }
            *outDataSize = out * (UInt32)sizeof(AudioObjectID); return 0;
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kOutStream:
    case kInStream: {
        const UInt32 channels = o == kOutStream ? kOutCh : 1;
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyName: if (o == kOutStream) REPLY_STR("Speaker"); REPLY_STR("Microphone");
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioStreamPropertyIsActive: REPLY(UInt32, 1);
        case kAudioStreamPropertyDirection: REPLY(UInt32, o == kInStream);
        case kAudioStreamPropertyTerminalType: REPLY(UInt32, o == kOutStream ? kAudioStreamTerminalTypeSpeaker : kAudioStreamTerminalTypeMicrophone);
        case kAudioStreamPropertyStartingChannel: REPLY(UInt32, 1);
        case kAudioStreamPropertyLatency: REPLY(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: REPLY(AudioStreamBasicDescription, format(channels));
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription r = {format(channels), {kRate, kRate}};
            REPLY(AudioStreamRangedDescription, r);
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    }
    case kMute4:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioBooleanControlClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioMuteControlClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioControlPropertyScope: REPLY(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: REPLY(AudioObjectPropertyElement, 4);
        case kAudioBooleanControlPropertyValue: REPLY(UInt32, gOutMute);
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kVolume:
    case kVolume4:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioLevelControlClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioVolumeControlClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioControlPropertyScope: REPLY(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: REPLY(AudioObjectPropertyElement, o == kVolume4 ? 4 : kAudioObjectPropertyElementMain);
        case kAudioLevelControlPropertyScalarValue: REPLY(Float32, gVolume);
        case kAudioLevelControlPropertyDecibelValue: REPLY(Float32, toDb(gVolume));
        case kAudioLevelControlPropertyDecibelRange: REPLY(AudioValueRange, ((AudioValueRange){kMinDb, 0.0}));
        case kAudioLevelControlPropertyConvertScalarToDecibels:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = toDb(*(Float32 *)outData); *outDataSize = sizeof(Float32); return 0;
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = toScalar(*(Float32 *)outData); *outDataSize = sizeof(Float32); return 0;
        default: return kAudioHardwareUnknownPropertyError;
        }
    default:
        return kAudioHardwareBadObjectError;
    }
}

// Every property the audio server asks for and the plug-in does not answer is logged once (object, selector, scope), so a client that
// drops the device after an unknown-property error names what it wanted.
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    (void)d;
    const OSStatus r = propertyData(o, a, qs, q, inDataSize, outDataSize, outData);
    trace(o, a, 0, r);
    if (r == kAudioHardwareUnknownPropertyError) {
        static UInt64 seen[128];
        static atomic_uint count;
        const UInt64 key = (UInt64)o << 56 ^ (UInt64)a->mSelector << 24 ^ a->mScope;
        unsigned n = atomic_load(&count);
        for (unsigned i = 0; i < n && i < 128; ++i) if (seen[i] == key) return r;
        n = atomic_fetch_add(&count, 1);
        if (n < 128) seen[n] = key;
        const UInt32 v = a->mSelector, sc = a->mScope;
        if (gControl && n < SHENG_AUDIO_DIAG_SLOTS) {
            ShengAudioDiag *diag = (ShengAudioDiag *)((uint8_t *)gControl + SHENG_AUDIO_DIAG_OFFSET);
            diag->selector[n] = v;
            diag->where[n] = (UInt32)o << 24 | (sc >> 24);
            diag->count = n + 1;
        }
        os_log_error(OS_LOG_DEFAULT, "ShengAudio: unknown property object %u selector '%c%c%c%c' scope '%c%c%c%c' element %u pid %d", o,
               (char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v, (char)(sc >> 24), (char)(sc >> 16), (char)(sc >> 8), (char)sc,
               a->mElement, pid);
    }
    return r;
}

// ---- The audio server's view of the device ---------------------------------------------------------------------------------------
// The routing layer of the audio server (VirtualAudio.plugin, in the same process) drops the device after a 'who?' error that never
// reaches this plug-in (i528). Its HAL calls and C++ throws are rebound here through its symbol pointers: every failing call (object,
// selector, scope, status) and every throw (the thrower's offset in the image, the exception's first word) is logged once and added to the
// trace with kind 4 (call) or 8 (throw, the offset in place of the selector).
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <sys/mman.h>
static OSStatus (*gRealGet)(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *, void *);
static OSStatus (*gRealGetSize)(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
static Boolean (*gRealHas)(AudioObjectID, const AudioObjectPropertyAddress *);
static void (*gRealThrow)(void *, void *, void (*)(void *));
static uintptr_t gVaBase;

static void noteCall(AudioObjectID o, const AudioObjectPropertyAddress *a, OSStatus r, const char *what) {
    const AudioObjectPropertyAddress t = {a->mSelector, a->mScope, 0};
    if (!gControl) return;
    ShengAudioTrace *tr = (ShengAudioTrace *)((uint8_t *)gControl + SHENG_AUDIO_TRACE_OFFSET);
    const UInt32 before = tr->count;
    trace(o & 0xff, &t, 4, r ? r : 1);
    if (tr->count == before) return;
    const UInt32 v = a->mSelector, sc = a->mScope;
    os_log_error(OS_LOG_DEFAULT, "ShengAudio: va %{public}s object %u '%c%c%c%c' scope '%c%c%c%c' element %u status %d", what, o,
                 (char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v, (char)(sc >> 24), (char)(sc >> 16), (char)(sc >> 8), (char)sc,
                 a->mElement, (int)r);
}
static OSStatus hookGet(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *size, void *data) {
    const OSStatus r = gRealGet(o, a, qs, q, size, data);
    if (r && o) noteCall(o, a, r, "get");
    return r;
}
static OSStatus hookGetSize(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *size) {
    const OSStatus r = gRealGetSize(o, a, qs, q, size);
    if (r && o) noteCall(o, a, r, "size");
    return r;
}
static Boolean hookHas(AudioObjectID o, const AudioObjectPropertyAddress *a) {
    const Boolean r = gRealHas(o, a);
    if (!r && o) noteCall(o, a, 0, "has");
    return r;
}
static void hookThrow(void *thrown, void *type, void (*destroy)(void *)) {
#if __has_feature(ptrauth_calls)
    const uintptr_t at = (uintptr_t)ptrauth_strip(__builtin_return_address(0), ptrauth_key_return_address) - gVaBase;
#else
    const uintptr_t at = (uintptr_t)__builtin_return_address(0) - gVaBase;
#endif
    const UInt32 first = thrown ? *(const UInt32 *)thrown : 0;
    const char *name = type ? ((const char *const *)type)[1] : "";
    if (gControl) {
        const AudioObjectPropertyAddress t = {(UInt32)at, kAudioObjectPropertyScopeGlobal, 0};
        trace(0, &t, 8, (OSStatus)(first | 1));
    }
    os_log_error(OS_LOG_DEFAULT, "ShengAudio: va throw at 0x%lx type %{public}s first 0x%08x", (unsigned long)at, name, first);
    __attribute__((musttail)) return gRealThrow(thrown, type, destroy);
}

// Rebinds the named imports of one image: its non-lazy and lazy symbol pointer sections, through the indirect symbol table.
static void rebind(const struct mach_header_64 *h, intptr_t slide) {
    const struct load_command *lc = (const void *)(h + 1);
    const struct segment_command_64 *linkedit = NULL;
    const struct symtab_command *symtab = NULL;
    const struct dysymtab_command *dysymtab = NULL;
    for (uint32_t i = 0; i < h->ncmds; ++i, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, SEG_LINKEDIT)) linkedit = (const void *)lc;
        else if (lc->cmd == LC_SYMTAB) symtab = (const void *)lc;
        else if (lc->cmd == LC_DYSYMTAB) dysymtab = (const void *)lc;
    }
    if (!linkedit || !symtab || !dysymtab) return;
    const uintptr_t base = (uintptr_t)slide + linkedit->vmaddr - linkedit->fileoff;
    const struct nlist_64 *syms = (const void *)(base + symtab->symoff);
    const char *strs = (const char *)(base + symtab->stroff);
    const uint32_t *indirect = (const void *)(base + dysymtab->indirectsymoff);
    static const char *const names[] = {"_AudioObjectGetPropertyData", "_AudioObjectGetPropertyDataSize", "_AudioObjectHasProperty", "___cxa_throw"};
    void *const hooks[] = {(void *)hookGet, (void *)hookGetSize, (void *)hookHas, (void *)hookThrow};
    void **const reals[] = {(void **)&gRealGet, (void **)&gRealGetSize, (void **)&gRealHas, (void **)&gRealThrow};
    lc = (const void *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; ++i, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *seg = (const void *)lc;
        const struct section_64 *sect = (const void *)(seg + 1);
        for (uint32_t s = 0; s < seg->nsects; ++s, ++sect) {
            const uint32_t type = sect->flags & SECTION_TYPE;
            if (type != S_NON_LAZY_SYMBOL_POINTERS && type != S_LAZY_SYMBOL_POINTERS) continue;
            void **slots = (void **)((uintptr_t)slide + sect->addr);
            for (uint64_t k = 0; k < sect->size / sizeof(void *); ++k) {
                const uint32_t index = indirect[sect->reserved1 + k];
                if (index & (INDIRECT_SYMBOL_ABS | INDIRECT_SYMBOL_LOCAL)) continue;
                const char *name = strs + syms[index].n_un.n_strx;
                for (unsigned n = 0; n < 4; ++n) {
                    if (strcmp(name, names[n]) || slots[k] == hooks[n]) continue;
                    const uintptr_t page = (uintptr_t)&slots[k] & ~(uintptr_t)(getpagesize() - 1);
                    if (mprotect((void *)page, (size_t)getpagesize(), PROT_READ | PROT_WRITE) &&
                        vm_protect(mach_task_self(), page, (vm_size_t)getpagesize(), false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY)) {
                        os_log_error(OS_LOG_DEFAULT, "ShengAudio: va rebind %{public}s failed", name);
                        continue;
                    }
                    if (!*reals[n]) *reals[n] = slots[k];
                    slots[k] = hooks[n];
                    os_log_error(OS_LOG_DEFAULT, "ShengAudio: va rebind %{public}s", name);
                }
            }
        }
    }
}
__attribute__((unused)) static void imageAdded(const struct mach_header *h, intptr_t slide) {
    Dl_info info;
    if (h->magic != MH_MAGIC_64 || !dladdr(h, &info) || !info.dli_fname || !strstr(info.dli_fname, "VirtualAudio.plugin/")) return;
    gVaBase = (uintptr_t)h;
    rebind((const struct mach_header_64 *)h, slide);
}
// arm64e (iPadOS 27's server): the symbol pointers are authenticated with their slot address, so a plain rebind faults; the trace is off.
#if !__has_feature(ptrauth_calls)
__attribute__((constructor)) static void watchAudioServer(void) {_dyld_register_func_for_add_image(imageAdded);}
#else
// fuxi (iOS 17.7 21H16 as iPhone15,2, 2026-11-13): VirtualAudio gates its USB-C / Thunderbolt route support on two MobileGestalt answers,
// in a dozen accessors of the form MGGetBoolAnswer("YiUtBQygkHRhLcdO3LFB4A") && MGGetBoolAnswer("DeviceSupportsUSBTypeC") (VirtualAudio
// at 0x112778, 0x12746c, 0x18db6c, ...). iPhone15,2 has a Lightning port, so both are false: the plug-in's Thunderbolt device gets no route
// and every audio session reports zero outputs (FSB:au probe: outs0 in Preferences' RingtonePreview session), so the server never starts
// IO. sheng's iPad (USB-C) answers true. Only VirtualAudio's import of MGGetBoolAnswer is rebound, in this process (the audio server):
// its stub is adrp/add x17 = slot, ldr x16, braa x16, x17 (__auth_stubs), so the hook is signed with key IA and the slot address. The
// real function comes from dlsym (a signed function pointer), so the old slot value is never authenticated. Every distinct key it asks
// is put into the trace (the key's last four characters; kind 4 | 0x100 when the real answer is true | 0x200 when forced).
#include <ptrauth.h>
static Boolean (*gRealMG)(CFStringRef);
static Boolean hookMG(CFStringRef q) {
    const Boolean real = gRealMG ? gRealMG(q) : 0;
    char key[80] = "";
    if (q) CFStringGetCString(q, key, sizeof key, kCFStringEncodingUTF8);
    const int force = !strcmp(key, "DeviceSupportsUSBTypeC") || !strcmp(key, "YiUtBQygkHRhLcdO3LFB4A");
    const size_t n = strlen(key);
    if (gControl && n >= 4) {
        const AudioObjectPropertyAddress t = {(UInt32)(uint8_t)key[n - 4] << 24 | (UInt32)(uint8_t)key[n - 3] << 16 |
                                                  (UInt32)(uint8_t)key[n - 2] << 8 | (uint8_t)key[n - 1],
                                              kAudioObjectPropertyScopeGlobal, 0};
        trace(0, &t, 4u | (real ? 0x100u : 0u) | (force ? 0x200u : 0u), 0);
    }
    return force ? 1 : real;
}
// The product VirtualAudio picks its compiled-in routing database by (MGGetProductType, mapped at VirtualAudio 0x6e3dc): the value goes
// into the trace as its own selector, kind 4 | 0x400.
static uint64_t (*gRealPT)(void);
static uint64_t hookPT(void) {
    const uint64_t v = gRealPT ? gRealPT() : 0;
    if (gControl) {
        const AudioObjectPropertyAddress t = {(UInt32)v, kAudioObjectPropertyScopeGlobal, 0};
        trace(0, &t, 4u | 0x400u, 0);
    }
    return v;
}
// VirtualAudio's fopen: a path it cannot open (its file reader throws "Could not construct", VirtualAudio 0x17296c, and the plug-in
// then answers "not initialized yet") goes into the control page's paths area, the end of the path kept.
static void notePath(const char *tag, const char *path) {
    if (!gControl) return;
    static atomic_uint used;
    const unsigned slot = atomic_fetch_add(&used, 1);
    if (slot >= SHENG_AUDIO_PATHS_SLOTS) return;
    char *const out = (char *)gControl + SHENG_AUDIO_PATHS_OFFSET + slot * SHENG_AUDIO_PATH_BYTES;
    const size_t room = SHENG_AUDIO_PATH_BYTES - 3, n = strlen(path), keep = n < room ? n : room;
    out[0] = tag[0]; out[1] = tag[1];
    memcpy(out + 2, path + n - keep, keep);
    out[2 + keep] = 0;
}
static FILE *(*gRealFopen)(const char *, const char *);
static FILE *hookFopen(const char *path, const char *mode) {
    // fuxi: AID8006's VAD graph configurations come from this bundle's copy, which has no speaker_* entry: their closed-loop HAL speaker
    // protection ('clhs') needs the speaker's factory Thiele-Small data, missing here ("FDR doesn't have Speaker Thiele-Small data"), and
    // VirtualAudio then dereferenced a null DSP graph (0x358478) and crashed the audio server in a loop. Without them it takes its own
    // fallback, "Speaker Protection DSP missing for speaker route. Output will be manually attenuated by -20dB".
    if (path && gRealFopen && strstr(path, "/VAD/graph_configurations.plist")) {
        Dl_info info;
        if (dladdr((const void *)hookFopen, &info) && info.dli_fname) {
            char own[1024];
            const char *slash = strrchr(info.dli_fname, '/');
            const size_t dir = slash ? (size_t)(slash - info.dli_fname) : 0;
            if (dir && dir + 32 < sizeof own) {
                memcpy(own, info.dli_fname, dir);
                strcpy(own + dir, "/graph_configurations.plist");
                FILE *const g = gRealFopen(own, mode);
                if (g) return g;
                notePath("G ", own);
            }
        }
    }
    FILE *const f = gRealFopen ? gRealFopen(path, mode) : NULL;
    if (!f && path) notePath("F ", path);
    return f;
}
// The failing path of VirtualAudio's device tree reads (EDTUtilsImpl.h: "IORegistryEntryFromPath %s returned MACH_PORT_NULL"), into the
// same paths area as fopen's, with an "R " prefix.
static io_registry_entry_t (*gRealIorPath)(mach_port_t, const char *);
static io_registry_entry_t hookIorPath(mach_port_t port, const char *path) {
    const io_registry_entry_t e = gRealIorPath ? gRealIorPath(port, path) : 0;
    if (!e && path) notePath("R ", path);
    return e;
}
// VirtualAudio's own error log lines (os_log type error or fault): this device has no readable unified log, so each line is decoded
// here (format and its encoded arguments: strings dereferenced, scalars printed) and sent as an IOKit beacon "ShengVA:<text>", which
// the kernel's IOKit probe always logs (the "Sheng" keyword). At most 80 lines, each format once per distinct first argument.
static void (*gRealOsLog)(void *, void *, uint8_t, const char *, uint8_t *, uint32_t);
static void vaBeacon(const char *text) {
    char name[200];
    snprintf(name, sizeof name, "ShengVA:%s", text);
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(name));
    if (s) IOObjectRelease(s);
}
static void hookOsLog(void *dso, void *log, uint8_t type, const char *format, uint8_t *buf, uint32_t size) {
    static atomic_uint sent;
    // Errors always; other levels only when the format names the built-in device path (AOP impersonation, 2026-11-13).
    const int wanted = format && (type >= 0x10 || strstr(format, "AOP") || strstr(format, "Codec") || strstr(format, "Speaker") ||
                                  strstr(format, "ort") || strstr(format, "ransducer") || strstr(format, "PhysicalDevice") ||
                                  strstr(format, "UID") || strstr(format, "Built") || strstr(format, "opology"));
    if (wanted && buf && size >= 2 && atomic_load(&sent) < 160) {
        char out[400];
        size_t o = 0;
        uint32_t at = 2;
        const unsigned count = buf[1];
        const char *f = format;
        for (unsigned arg = 0; *f && o < sizeof out - 2;) {
            if (*f != '%') {out[o++] = *f++; continue;}
            ++f;
            if (*f == '%') {out[o++] = *f++; continue;}
            while (*f && !strchr("sdiuxXpcfg@", *f)) ++f;   // flags, width, length, {public}
            if (!*f) break;
            const char conv = *f++;
            if (arg >= count || at + 2 > size) {o += (size_t)snprintf(out + o, sizeof out - o, "?"); continue;}
            const uint8_t desc = buf[at], len = buf[at + 1];
            const uint8_t *data = buf + at + 2;
            at += 2u + len; ++arg;
            if (o >= sizeof out - 2) break;
            if ((desc >> 4) == 2 && len == 8) {          // string: a pointer
                const char *s = *(const char *const *)(const void *)data;
                o += (size_t)snprintf(out + o, sizeof out - o, "%.80s", s ? s : "(null)");
            } else if (len == 4) {
                int32_t v; memcpy(&v, data, 4);
                o += (size_t)snprintf(out + o, sizeof out - o, conv == 'x' || conv == 'X' ? "%x" : "%d", v);
            } else if (len == 8) {
                int64_t v; memcpy(&v, data, 8);
                o += (size_t)snprintf(out + o, sizeof out - o, conv == 'x' || conv == 'X' || conv == 'p' ? "%llx" : "%lld", (long long)v);
            } else {
                o += (size_t)snprintf(out + o, sizeof out - o, "<%u>", len);
            }
            (void)conv;
        }
        if (o > sizeof out - 1) o = sizeof out - 1;
        out[o] = 0;
        // drop the "%25s:%-5d " file:line prefix's padding
        const char *text = out;
        while (*text == ' ') ++text;
        static uint64_t seen[192];
        uint64_t key = 1469598103934665603ULL;
        for (const char *c = text; *c && c < text + 120; ++c) key = (key ^ (uint8_t)*c) * 1099511628211ULL;
        unsigned n = atomic_load(&sent), dup = 0;
        for (unsigned i = 0; i < n && i < 192; ++i) if (seen[i] == key) {dup = 1; break;}
        if (!dup) {
            n = atomic_fetch_add(&sent, 1);
            if (n < 192) seen[n] = key;
            vaBeacon(text);
        }
    }
    if (gRealOsLog) gRealOsLog(dso, log, type, format, buf, size);
}
static void rebindMG(const struct mach_header_64 *h, intptr_t slide) {
    const struct load_command *lc = (const void *)(h + 1);
    const struct segment_command_64 *linkedit = NULL;
    const struct symtab_command *symtab = NULL;
    const struct dysymtab_command *dysymtab = NULL;
    for (uint32_t i = 0; i < h->ncmds; ++i, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, SEG_LINKEDIT)) linkedit = (const void *)lc;
        else if (lc->cmd == LC_SYMTAB) symtab = (const void *)lc;
        else if (lc->cmd == LC_DYSYMTAB) dysymtab = (const void *)lc;
    }
    if (!linkedit || !symtab || !dysymtab) return;
    if (!gRealMG) gRealMG = (Boolean (*)(CFStringRef))dlsym(RTLD_DEFAULT, "MGGetBoolAnswer");
    // The diagnostic hooks of the non-arm64e build as well (failing HAL calls and C++ throws of VirtualAudio, kinds 4 and 8 of the trace),
    // with the real functions from dlsym instead of the old slot values.
    if (!gRealGet) gRealGet = dlsym(RTLD_DEFAULT, "AudioObjectGetPropertyData");
    if (!gRealGetSize) gRealGetSize = dlsym(RTLD_DEFAULT, "AudioObjectGetPropertyDataSize");
    if (!gRealHas) gRealHas = dlsym(RTLD_DEFAULT, "AudioObjectHasProperty");
    if (!gRealThrow) gRealThrow = dlsym(RTLD_DEFAULT, "__cxa_throw");
    if (!gRealPT) gRealPT = (uint64_t (*)(void))dlsym(RTLD_DEFAULT, "MGGetProductType");
    if (!gRealFopen) gRealFopen = (FILE *(*)(const char *, const char *))dlsym(RTLD_DEFAULT, "fopen");
    if (!gRealIorPath) gRealIorPath = (io_registry_entry_t (*)(mach_port_t, const char *))dlsym(RTLD_DEFAULT, "IORegistryEntryFromPath");
    if (!gRealOsLog) gRealOsLog = (void (*)(void *, void *, uint8_t, const char *, uint8_t *, uint32_t))dlsym(RTLD_DEFAULT, "_os_log_impl");
    static const char *const names[] = {"_MGGetBoolAnswer", "_AudioObjectGetPropertyData", "_AudioObjectGetPropertyDataSize",
                                        "_AudioObjectHasProperty", "___cxa_throw", "_MGGetProductType", "_fopen", "_IORegistryEntryFromPath",
                                        "__os_log_impl"};
    void *const fns[] = {(void *)hookMG, (void *)hookGet, (void *)hookGetSize, (void *)hookHas, (void *)hookThrow, (void *)hookPT,
                         (void *)hookFopen, (void *)hookIorPath, (void *)hookOsLog};
    void *const have[] = {(void *)gRealMG, (void *)gRealGet, (void *)gRealGetSize, (void *)gRealHas, (void *)gRealThrow, (void *)gRealPT,
                          (void *)gRealFopen, (void *)gRealIorPath, (void *)gRealOsLog};
    const uintptr_t base = (uintptr_t)slide + linkedit->vmaddr - linkedit->fileoff;
    const struct nlist_64 *syms = (const void *)(base + symtab->symoff);
    const char *strs = (const char *)(base + symtab->stroff);
    const uint32_t *indirect = (const void *)(base + dysymtab->indirectsymoff);
    lc = (const void *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; ++i, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *seg = (const void *)lc;
        const struct section_64 *sect = (const void *)(seg + 1);
        for (uint32_t s = 0; s < seg->nsects; ++s, ++sect) {
            if ((sect->flags & SECTION_TYPE) != S_NON_LAZY_SYMBOL_POINTERS || strcmp(sect->sectname, "__auth_got")) continue;
            void **slots = (void **)((uintptr_t)slide + sect->addr);
            for (uint64_t k = 0; k < sect->size / sizeof(void *); ++k) {
                const uint32_t index = indirect[sect->reserved1 + k];
                if (index & (INDIRECT_SYMBOL_ABS | INDIRECT_SYMBOL_LOCAL)) continue;
                const char *name = strs + syms[index].n_un.n_strx;
                unsigned n = 0;
                while (n < 9 && strcmp(name, names[n])) ++n;
                if (n == 9 || !have[n]) continue;
                void *const hook = ptrauth_sign_unauthenticated(ptrauth_strip(fns[n], ptrauth_key_function_pointer), ptrauth_key_asia,
                                                                &slots[k]);
                if (slots[k] == hook) continue;
                const uintptr_t page = (uintptr_t)&slots[k] & ~(uintptr_t)(getpagesize() - 1);
                if (mprotect((void *)page, (size_t)getpagesize(), PROT_READ | PROT_WRITE) &&
                    vm_protect(mach_task_self(), page, (vm_size_t)getpagesize(), false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY)) {
                    os_log_error(OS_LOG_DEFAULT, "ShengAudio: va rebind %{public}s failed", name);
                    continue;
                }
                slots[k] = hook;
                (void)mprotect((void *)page, (size_t)getpagesize(), PROT_READ);
                os_log_error(OS_LOG_DEFAULT, "ShengAudio: va %{public}s rebound", name);
            }
        }
    }
}
static void imageAddedMG(const struct mach_header *h, intptr_t slide) {
    Dl_info info;
    if (h->magic != MH_MAGIC_64 || !dladdr(h, &info) || !info.dli_fname || !strstr(info.dli_fname, "VirtualAudio.plugin/")) return;
    gVaBase = (uintptr_t)h;
    rebindMG((const struct mach_header_64 *)h, slide);
}
__attribute__((constructor)) static void watchAudioServerMG(void) {_dyld_register_func_for_add_image(imageAddedMG);}
#endif

// ---- IO ----------------------------------------------------------------------------------------------------------------------------
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d; (void)client;
    if (o != kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    OSStatus r = 0;
    if (gIoUsers == 0) {
        if (connectService()) r = kAudioHardwareNotRunningError;
        else if (!gControl) r = kAudioHardwareNotRunningError;
        else {
            setHostClock();   // again: the shared page may have been read back from disk since
            if (call(ShengAudioStartOutput) != KERN_SUCCESS) r = kAudioHardwareNotRunningError;
            else {
                call(ShengAudioStartInput);
                const uint64_t host = mach_absolute_time(), anchor = gControl->out_anchor_host;
                gZeroOrigin = host > anchor ? (uint64_t)((Float64)(host - anchor) / gHostTicksPerFrame) : 0;
                gZeroSeed++;
            }
        }
    }
    if (!r) gIoUsers++;
    pthread_mutex_unlock(&gLock);
    return r;
}
static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d; (void)client;
    if (o != kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    if (gIoUsers && --gIoUsers == 0) {call(ShengAudioStopOutput); call(ShengAudioStopInput);}
    pthread_mutex_unlock(&gLock);
    return 0;
}
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, Float64 *sampleTime, UInt64 *hostTime, UInt64 *seed) {
    (void)d; (void)o; (void)client;
    if (!gControl) return kAudioHardwareNotRunningError;
    uint32_t seq;
    uint64_t anchor;
    do {seq = gControl->out_seq; anchor = gControl->out_anchor_host;} while ((seq & 1) || seq != gControl->out_seq);
    const uint64_t now = mach_absolute_time();
    const Float64 period = gHostTicksPerFrame * kZeroPeriod;
    const uint64_t origin = anchor + (uint64_t)((Float64)gZeroOrigin * gHostTicksPerFrame);
    const uint64_t wraps = now > origin ? (uint64_t)((Float64)(now - origin) / period) : 0;
    *sampleTime = (Float64)(gZeroOrigin + wraps * kZeroPeriod);
    *hostTime = origin + (uint64_t)((Float64)wraps * period);
    *seed = 1 + seq / 2 + gZeroSeed;
    return 0;
}
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, Boolean *will, Boolean *inPlace) {
    (void)d; (void)o; (void)client;
    *will = op == kAudioServerPlugInIOOperationWriteMix || op == kAudioServerPlugInIOOperationReadInput;
    *inPlace = true;
    return 0;
}
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info) {
    (void)d; (void)o; (void)client; (void)op; (void)frames; (void)info; return 0;
}
// Records how the audio server schedules its IO thread (i545: 98 gaps of more than 160 ms between IO cycles).
static void noteThread(ShengAudioDiag *diag) {
    const thread_act_t self = mach_thread_self();
    thread_extended_info_data_t info;
    mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
    if (thread_info(self, THREAD_EXTENDED_INFO, (thread_info_t)&info, &count) == KERN_SUCCESS) {
        diag->io_pri = info.pth_curpri;
        diag->io_base = info.pth_priority;
        diag->io_policy = info.pth_policy;
        if (!diag->io_pri_min || info.pth_curpri < diag->io_pri_min) diag->io_pri_min = info.pth_curpri;
    }
    thread_time_constraint_policy_data_t tc;
    count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t def = false;
    if (thread_policy_get(self, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tc, &count, &def) == KERN_SUCCESS) {
        diag->io_tc = def ? 0 : 1;
        diag->io_period = tc.period;
        diag->io_computation = tc.computation;
        diag->io_constraint = tc.constraint;
    } else diag->io_tc = -1;
    mach_port_deallocate(mach_task_self(), self);
}
static inline int16_t toS16(Float32 v) {v *= 32767.0f; return (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);}
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, AudioObjectID stream, UInt32 client, UInt32 op, UInt32 frames,
                              const AudioServerPlugInIOCycleInfo *info, void *main, void *secondary) {
    (void)d; (void)o; (void)client; (void)secondary;
    if (!gControl) return 0;
    // The play position now, on the timeline GetZeroTimeStamp gives the audio server.
    const uint64_t host = mach_absolute_time(), anchor = gControl->out_anchor_host;
    const int64_t nowFrames = host > anchor ? (int64_t)((Float64)(host - anchor) / gHostTicksPerFrame) : 0;
    ShengAudioDiag *diag = (ShengAudioDiag *)((uint8_t *)gControl + SHENG_AUDIO_DIAG_OFFSET);
    if (op == kAudioServerPlugInIOOperationWriteMix && stream == kOutStream) {
        const int32_t ahead = (int32_t)((int64_t)info->mOutputTime.mSampleTime - nowFrames);
        // Where the audio server places now, against the timeline.
        const int32_t skew = (int32_t)((int64_t)info->mCurrentTime.mSampleTime - nowFrames);
        if (!diag->writes || skew < diag->skew_min) diag->skew_min = skew;
        if (!diag->writes || skew > diag->skew_max) diag->skew_max = skew;
        if (!diag->writes || ahead < diag->ahead_min) diag->ahead_min = ahead;
        if (!diag->writes || ahead > diag->ahead_max) diag->ahead_max = ahead;
        diag->io_frames = frames;
        if (diag->writes % 256 == 0) noteThread(diag);
        diag->writes += 1;
        const Float32 *src = main;
        uint64_t t = (uint64_t)info->mOutputTime.mSampleTime;
        const Float32 g = gain();
        Float32 peak = 0;
        for (UInt32 i = 0; i < kOutCh * frames; ++i) {const Float32 v = fabsf(src[i]); if (v > peak) peak = v;}
        const UInt32 p = (UInt32)(fminf(peak, 1.0f) * 32767.0f);
        if (p > diag->mix_peak) diag->mix_peak = p;
        // The audio server runs more than one IO context on the device (i539: 65291 writes of 1024 frames in a 600 s run, 2.3 times the
        // frames that played), each with its own mix. Each is added to what the ring holds, so a context writing silence does not replace
        // another one's sound; the service clears every period it takes.
        if (diag->last_host) {
            const Float64 ms = (Float64)(host - diag->last_host) / gHostTicksPerFrame / (kRate / 1000.0);
            diag->interval[ms < 15 ? 0 : ms < 25 ? 1 : ms < 40 ? 2 : ms < 80 ? 3 : ms < 160 ? 4 : 5] += 1;
            if (ms < 160 && t > diag->last_end) {diag->gaps += 1; diag->gap_frames += (uint32_t)(t - diag->last_end);}
        }
        diag->last_host = host;
        diag->last_end = t + frames;
        diag->frames_total += frames;
        const uint64_t written = gControl->out_written;
        if (t < written) {
            diag->overlaps += 1;
            if (written - t > diag->overlap_max) diag->overlap_max = (uint32_t)(written - t);
        }
        for (UInt32 i = 0; i < frames; ++i, ++t) {
            int16_t *dst = gOut + 2 * (t % SHENG_AUDIO_RING_FRAMES);
            const Float32 *f = src + (size_t)kOutCh * i;
            Float32 l = 0, r = 0;
            for (UInt32 c = 0; c < kOutCh; c += 2) {l += f[c]; r += f[c + 1];}
            dst[0] = toS16(dst[0] / 32767.0f + l * g);
            dst[1] = toS16(dst[1] / 32767.0f + r * g);
        }
        if (t > gControl->out_written) gControl->out_written = t;
        gControl->out_written_host = host;
    } else if (op == kAudioServerPlugInIOOperationReadInput && stream == kInStream) {
        // Frames the service has not captured yet read as silence, not as what the ring held one turn earlier; such reads are counted.
        const int32_t behind = (int32_t)(nowFrames - (int64_t)info->mInputTime.mSampleTime);
        if (!diag->behind_max || behind < diag->behind_min) diag->behind_min = behind;
        if (behind > diag->behind_max) diag->behind_max = behind;
        Float32 *dst = main;
        uint64_t t = (uint64_t)info->mInputTime.mSampleTime;
        const uint64_t written = gControl->in_written;
        if (t + frames > written) diag->in_misses += 1;
        for (UInt32 i = 0; i < frames; ++i, ++t) dst[i] = t < written ? gIn[t % SHENG_AUDIO_RING_FRAMES] / 32768.0f : 0.0f;
    }
    return 0;
}
static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info) {
    (void)d; (void)o; (void)client; (void)op; (void)frames; (void)info; return 0;
}

static AudioServerPlugInDriverInterface gInterface = {
    NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformDeviceConfigurationChange, AbortDeviceConfigurationChange, HasProperty, IsPropertySettable, GetPropertyDataSize,
    GetPropertyData, SetPropertyData, StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;

static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out) {
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        AddRef(self);
        *out = &gInterfacePtr;
        r = S_OK;
    }
    CFRelease(requested);
    return r;
}


// fuxi probe (2026-11-13): the kernel IOKit probe (unixnu-iokit-probe=1) logs every matching lookup with the caller's name; a lookup of a
// class that does not exist is this plug-in's only channel to the boot log. "ShengHAL:loaded" from the image constructor (the audio
// server dlopened the bundle), "ShengHAL:create" from the factory (the plug-in manager instantiated it).
#include <stdio.h>
static void fuxiHalBeacon(const char *what) {
    char name[64];
    snprintf(name, sizeof name, "ShengHAL:%s:%s", what, getprogname());
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(name));
    if (s) IOObjectRelease(s);
}
__attribute__((constructor)) static void fuxiHalLoaded(void) {fuxiHalBeacon("loaded");}

__attribute__((visibility("default"))) void *ShengAudio_Create(CFAllocatorRef allocator, CFUUIDRef type) {
    fuxiHalBeacon("create");
    (void)allocator;
    if (!CFEqual(type, kAudioServerPlugInTypeUUID)) return NULL;
    if (!gHostTicksPerFrame) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        gHostTicksPerFrame = 1e9 / kRate * tb.denom / tb.numer;
    }
    return &gInterfacePtr;
}
