/*
 * DLisaAudio.driver: lisa's speakers for iOS 26's audio server (audiomxd), an AudioServerPlugIn in
 * /Library/Audio/Plug-Ins/HAL (CoreAudio scans that directory; the stock root has VirtualAudio.plugin there).
 *
 * Ported from HakiOS's userland/src/audio_plugin.c (redwood, iOS 16, mediaserverd), MIT License, Copyright (c) 2025-2026
 * the HakiOS authors (the full notice is in research/hackios-xnu-kernel/LICENSE). What changed for lisa and 26:
 *   - the devices published are chosen by boot-args (dlisa-audio-logic.h): by default HakiOS's set, "Speaker" (stereo
 *     out to /dev/hakios-audio, a silent stereo in), "Hawking" (one silent input: the companion the virtual audio
 *     device wants beside the default route), "Actuator" (one output: the haptics' waveforms, counted and dropped:
 *     lisa's /dev/hakios-haptic takes effect numbers, not samples) and "Codec" (one input, silence: no microphone
 *     driver on lisa yet); "Receiver" (the earpiece's mono output, dropped) on request;
 *   - the log goes to the console (/dev/console), with the process it runs in, and says every property asked of an
 *     object that it does not have (the first 96), every start and stop, and once a second while playing how much
 *     was written and how loud;
 *   - kernel side: dlisa.sound.c at sound=4 is the sink of /dev/hakios-audio (DC blocker, the volume given by
 *     HAKIOS_AUDIO_SET_VOLUME, at least sound_atten dB under full scale, the amplifiers on only while there is sound).
 *
 * The interface is the one macOS documents for user-space audio drivers (AudioServerPlugIn.h); the iOS SDK has no
 * header for it, so what is needed of it is declared below.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreAudioTypes/CoreAudioTypes.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "dlisa-audio-logic.h"

/* ---- the kernel's interface (bsd/sys/hakios.audio.h) ---- */
#define HAKIOS_AUDIO_DEVICE     "/dev/hakios-audio"
#define HAKIOS_AUDIO_RATE       48000
#define HAKIOS_AUDIO_CHANNELS   2
#define HAKIOS_AUDIO_FRAME      4
struct hakios_audio_volume {
	uint32_t below;                 /* eighths of a dB */
	uint32_t muted;
};
#define HAKIOS_AUDIO_SET_VOLUME _IOW('r', 1, struct hakios_audio_volume)

/* ---- the AudioServerPlugIn interface (macOS: CoreAudio/AudioServerPlugIn.h, AudioHardwareBase.h) ---- */

typedef UInt32 AudioObjectID;
typedef struct {
	UInt32 mSelector, mScope, mElement;
} AudioObjectPropertyAddress;
typedef struct {
	AudioStreamBasicDescription mFormat;
	AudioValueRange mSampleRateRange;
} AudioStreamRangedDescription;
typedef struct {
	UInt32 mClientID;
	pid_t mProcessID;
	Boolean mIsNativeEndian;
	CFStringRef mBundleID;
} AudioServerPlugInClientInfo;
typedef struct {
	UInt64 mIOCycleCounter;
	UInt32 mNominalIOBufferFrameSize;
	AudioTimeStamp mCurrentTime, mInputTime, mOutputTime;
	Float64 mMasterHostTicksPerFrame, mDeviceHostTicksPerFrame;
} AudioServerPlugInIOCycleInfo;

typedef struct AudioServerPlugInHostInterface AudioServerPlugInHostInterface;
typedef const AudioServerPlugInHostInterface *AudioServerPlugInHostRef;
struct AudioServerPlugInHostInterface {
	OSStatus (*PropertiesChanged)(AudioServerPlugInHostRef, AudioObjectID, UInt32, const AudioObjectPropertyAddress *);
	OSStatus (*CopyFromStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef *);
	OSStatus (*WriteToStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef);
	OSStatus (*DeleteFromStorage)(AudioServerPlugInHostRef, CFStringRef);
	OSStatus (*RequestDeviceConfigurationChange)(AudioServerPlugInHostRef, AudioObjectID, UInt64, void *);
};

typedef struct AudioServerPlugInDriverInterface AudioServerPlugInDriverInterface;
typedef AudioServerPlugInDriverInterface **AudioServerPlugInDriverRef;
struct AudioServerPlugInDriverInterface {
	void *_reserved;
	HRESULT (*QueryInterface)(void *, REFIID, LPVOID *);
	ULONG (*AddRef)(void *);
	ULONG (*Release)(void *);
	OSStatus (*Initialize)(AudioServerPlugInDriverRef, AudioServerPlugInHostRef);
	OSStatus (*CreateDevice)(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
	OSStatus (*DestroyDevice)(AudioServerPlugInDriverRef, AudioObjectID);
	OSStatus (*AddDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
	OSStatus (*RemoveDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
	OSStatus (*PerformDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
	OSStatus (*AbortDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
	Boolean (*HasProperty)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
	OSStatus (*IsPropertySettable)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
	OSStatus (*GetPropertyDataSize)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32 *);
	OSStatus (*GetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32, UInt32 *, void *);
	OSStatus (*SetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32, const void *);
	OSStatus (*StartIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
	OSStatus (*StopIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
	OSStatus (*GetZeroTimeStamp)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
	OSStatus (*WillDoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
	OSStatus (*BeginIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *);
	OSStatus (*DoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *, void *, void *);
	OSStatus (*EndIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *);
};

#define kAudioServerPlugInTypeUUID CFUUIDGetConstantUUIDWithBytes(NULL, 0x44, 0x3A, 0xBA, 0xB8, 0xE7, 0xB3, 0x49, 0x1A, \
	    0xB9, 0x85, 0xBE, 0xB9, 0x18, 0x70, 0x30, 0xDB)
#define kAudioServerPlugInDriverInterfaceUUID CFUUIDGetConstantUUIDWithBytes(NULL, 0xEE, 0xA5, 0x77, 0x3D, 0xCC, 0x43, \
	    0x49, 0xF1, 0x8E, 0x00, 0x8F, 0x96, 0xE7, 0xD2, 0x3B, 0x17)

enum {
	kAudioHardwareUnknownPropertyError = 'who?', kAudioHardwareBadPropertySizeError = '!siz',
	kAudioHardwareIllegalOperationError = 'nope', kAudioHardwareBadObjectError = '!obj',
	kAudioHardwareUnsupportedOperationError = 'unop', kAudioDeviceUnsupportedFormatError = '!dat',

	kAudioObjectPropertyScopeGlobal = 'glob', kAudioObjectPropertyScopeInput = 'inpt', kAudioObjectPropertyScopeOutput = 'outp',

	kAudioObjectClassID = 'aobj', kAudioPlugInClassID = 'aplg', kAudioDeviceClassID = 'adev', kAudioStreamClassID = 'astr',

	kAudioObjectPropertyBaseClass = 'bcls', kAudioObjectPropertyClass = 'clas', kAudioObjectPropertyOwner = 'stdv',
	kAudioObjectPropertyName = 'lnam', kAudioObjectPropertyManufacturer = 'lmak', kAudioObjectPropertyOwnedObjects = 'ownd',

	kAudioPlugInPropertyBoxList = 'box#', kAudioPlugInPropertyTranslateUIDToBox = 'uidb',
	kAudioPlugInPropertyDeviceList = 'dev#', kAudioPlugInPropertyTranslateUIDToDevice = 'uidd',
	kAudioPlugInPropertyResourceBundle = 'rsrc',

	kAudioDevicePropertyDeviceUID = 'uid ', kAudioDevicePropertyModelUID = 'muid', kAudioDevicePropertyTransportType = 'tran',
	kAudioDevicePropertyRelatedDevices = 'akin', kAudioDevicePropertyClockDomain = 'clkd',
	kAudioDevicePropertyDeviceIsAlive = 'livn', kAudioDevicePropertyDeviceIsRunning = 'goin',
	kAudioDevicePropertyDeviceCanBeDefaultDevice = 'dflt', kAudioDevicePropertyDeviceCanBeDefaultSystemDevice = 'sflt',
	kAudioDevicePropertyLatency = 'ltnc', kAudioDevicePropertyStreams = 'stm#', kAudioObjectPropertyControlList = 'ctrl',
	kAudioDevicePropertySafetyOffset = 'saft', kAudioDevicePropertyNominalSampleRate = 'nsrt',
	kAudioDevicePropertyAvailableNominalSampleRates = 'nsr#', kAudioDevicePropertyIsHidden = 'hidn',
	kAudioDevicePropertyPreferredChannelsForStereo = 'dch2', kAudioDevicePropertyZeroTimeStampPeriod = 'ring',
	kAudioSelectorControlClassID = 'slct', kAudioDataSourceControlClassID = 'dsrc',
	kAudioSelectorControlPropertyCurrentItem = 'scci', kAudioSelectorControlPropertyAvailableItems = 'scai',
	kAudioSelectorControlPropertyItemName = 'scin',

	kAudioStreamPropertyIsActive = 'sact', kAudioStreamPropertyDirection = 'sdir', kAudioStreamPropertyTerminalType = 'term',
	kAudioStreamPropertyStartingChannel = 'schn', kAudioStreamPropertyLatency = 'ltnc',
	kAudioStreamPropertyVirtualFormat = 'sfmt', kAudioStreamPropertyAvailableVirtualFormats = 'sfma',
	kAudioStreamPropertyPhysicalFormat = 'pft ', kAudioStreamPropertyAvailablePhysicalFormats = 'pfta',

	kAudioStreamTerminalTypeSpeaker = 'spkr', kAudioStreamTerminalTypeMicrophone = 'micr',
	kAudioStreamTerminalTypeReceiverSpeaker = 'rspk',

	kAudioServerPlugInIOOperationReadInput = 'read', kAudioServerPlugInIOOperationWriteMix = 'rite',

	kAudioLevelControlClassID = 'levl', kAudioVolumeControlClassID = 'vlme',
	kAudioControlPropertyScope = 'cscp', kAudioControlPropertyElement = 'celm',
	kAudioLevelControlPropertyScalarValue = 'lcsv', kAudioLevelControlPropertyDecibelValue = 'lcdv',
	kAudioLevelControlPropertyDecibelRange = 'lcdr', kAudioLevelControlPropertyConvertScalarToDecibels = 'lcsd',
	kAudioLevelControlPropertyConvertDecibelsToScalar = 'lcds',
};

/* ---- the devices ---------------------------------------------------------------------------------- */

/* Object numbers are ours to choose; a device's streams follow it (output, then input), all five always exist here but
 * only the chosen ones are in the plug-in's device list. */
enum {
	OBJECT_PLUGIN = 1,
	OBJECT_SPEAKER, OBJECT_SPEAKER_OUTPUT, OBJECT_SPEAKER_INPUT,
	OBJECT_RECEIVER, OBJECT_RECEIVER_OUTPUT,
	OBJECT_HAWKING, OBJECT_HAWKING_INPUT,
	OBJECT_ACTUATOR, OBJECT_ACTUATOR_OUTPUT,
	OBJECT_CODEC, OBJECT_CODEC_INPUT,
	OBJECT_SPEAKER_VOLUME,          /* the speaker output's volume control */
	OBJECT_CODEC_SOURCE,            /* the codec input's data source control */
	OBJECT_LAST = OBJECT_CODEC_SOURCE
};

struct device {
	int role;                       /* DLISA_* */
	AudioObjectID object;
	AudioObjectID output, input;    /* its streams; 0 = none */
	UInt32 output_channels, input_channels;
	UInt32 terminal;                /* of its output */
	CFStringRef uid;                /* set at load from the choice */
	unsigned running;               /* clients that started I/O */
	struct dlisa_audio_clock clock;
};

static struct device devices[] = {
	{ DLISA_SPEAKER, OBJECT_SPEAKER, OBJECT_SPEAKER_OUTPUT, OBJECT_SPEAKER_INPUT, HAKIOS_AUDIO_CHANNELS, 2,
	  kAudioStreamTerminalTypeSpeaker, NULL, 0, { 0, 0 } },
	{ DLISA_RECEIVER, OBJECT_RECEIVER, OBJECT_RECEIVER_OUTPUT, 0, 1, 0, kAudioStreamTerminalTypeReceiverSpeaker, NULL, 0,
	  { 0, 0 } },
	{ DLISA_HAWKING, OBJECT_HAWKING, 0, OBJECT_HAWKING_INPUT, 0, 1, 0, NULL, 0, { 0, 0 } },
	{ DLISA_ACTUATOR, OBJECT_ACTUATOR, OBJECT_ACTUATOR_OUTPUT, 0, 1, 0, kAudioStreamTerminalTypeSpeaker, NULL, 0, { 0, 0 } },
	{ DLISA_CODEC, OBJECT_CODEC, 0, OBJECT_CODEC_INPUT, 0, 1, 0, NULL, 0, { 0, 0 } },
};
#define DEVICES                 (sizeof(devices) / sizeof(devices[0]))

#define TRANSPORT_BUILT_IN      'bltn'
/* VirtualAudio keeps a codec's data sources it knows ('imic' is an iPhone's bottom microphone) */
#define MIC_SOURCE              'imic'
#define MANUFACTURER            CFSTR("darwin-lisa")
/* how often a device says where its clock is, in frames: also the largest I/O buffer the HAL will use */
#define TIMESTAMP_PERIOD        16384
#define UNKNOWN_TOLD            96

static struct dlisa_audio_choice choice;
static AudioServerPlugInHostRef host;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int sink = -1;                   /* /dev/hakios-audio */
static int console = -1;
static Float64 ticks_per_frame;
static Boolean inactive[OBJECT_LAST + 1];       /* streams the HAL has switched off */
static Float32 volume = 1.0f;           /* the speaker's volume control, 0 to 1 */

/* what the speaker's output has had since its I/O started, and in the second being counted */
static struct {
	UInt64 frames, refused, sound_frames;
	UInt64 second_start, second_frames;
	int second_peak, peak;
	unsigned seconds_said;
} played;
static UInt64 actuator_frames, receiver_frames;
static int actuator_peak, receiver_peak;

static void
say(const char *format, ...)
{
	char line[400];
	va_list ap;
	int n, prefix;

	prefix = snprintf(line, sizeof(line), "dlisa-audio[%s %d]: ", getprogname(), getpid());
	va_start(ap, format);
	n = vsnprintf(line + prefix, sizeof(line) - (size_t)prefix, format, ap);
	va_end(ap);
	n += prefix;
	n = n > (int)sizeof(line) - 1 ? (int)sizeof(line) - 1 : n;
	if (console < 0) {
		console = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
	}
	(void)write(console >= 0 ? console : 2, line, (size_t)n);
}

static bool
chosen(const struct device *d)
{
	return choice.on && (choice.roles & (1u << d->role)) != 0;
}

/* The device with this object number, or the one this stream belongs to; NULL = neither. */
static struct device *
device_of(AudioObjectID object)
{
	for (unsigned i = 0; i < DEVICES; i++) {
		if (object == devices[i].object || (object != 0 && (object == devices[i].output || object == devices[i].input))) {
			return &devices[i];
		}
	}
	return NULL;
}

static const char *
uid_text(const struct device *d)
{
	return d != NULL ? choice.uid[d->role] : "-";
}

/* The volume, to the kernel. With the lock held. */
static void
volume_to_kernel(void)
{
	struct dlisa_audio_kernel_volume k = dlisa_audio_kernel_volume(volume);
	struct hakios_audio_volume v = { k.below, k.muted };

	if (sink < 0 || ioctl(sink, HAKIOS_AUDIO_SET_VOLUME, &v) < 0) {
		say("the volume %.3f could not be given to the kernel (%s)\n", volume, sink < 0 ? "no sink" : strerror(errno));
		return;
	}
	say("the volume is %.3f: %s%.1f dB under the loudest the kernel allows\n", volume, v.muted ? "silence, " : "",
	    v.below / 8.0);
}

/* The one format of a stream: 16-bit samples at HAKIOS_AUDIO_RATE, so many channels. */
static AudioStreamBasicDescription
format_of(AudioObjectID stream)
{
	const struct device *d = device_of(stream);
	UInt32 channels = stream == d->output ? d->output_channels : d->input_channels;

	return (AudioStreamBasicDescription) {
		       .mSampleRate = HAKIOS_AUDIO_RATE, .mFormatID = kAudioFormatLinearPCM,
		       .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
		       .mBytesPerPacket = 2 * channels, .mFramesPerPacket = 1, .mBytesPerFrame = 2 * channels,
		       .mChannelsPerFrame = channels, .mBitsPerChannel = 16,
	};
}

static HRESULT
query_interface(void *driver, REFIID uuid, LPVOID *interface)
{
	CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
	Boolean ours = CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID);

	CFRelease(requested);
	if (interface == NULL || !ours) {
		return E_NOINTERFACE;
	}
	*interface = driver;            /* one driver, never freed: no counting */
	return S_OK;
}

static ULONG
add_ref(__unused void *driver)
{
	return 1;
}

static ULONG
release(__unused void *driver)
{
	return 1;
}

static void
read_choice(void)
{
	char line[1024] = "";
	size_t size = sizeof(line) - 1;

	if (sysctlbyname("kern.bootargs", line, &size, NULL, 0) == 0) {
		line[size < sizeof(line) ? size : sizeof(line) - 1] = 0;
	}
	dlisa_audio_choose(&choice, line);
	for (unsigned i = 0; i < DEVICES; i++) {
		devices[i].uid = CFStringCreateWithCString(NULL, choice.uid[devices[i].role], kCFStringEncodingUTF8);
	}
}

static OSStatus
initialize(__unused AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef the_host)
{
	mach_timebase_info_data_t timebase;
	char list[200] = "";

	host = the_host;
	mach_timebase_info(&timebase);
	ticks_per_frame = 1e9 * timebase.denom / timebase.numer / HAKIOS_AUDIO_RATE;
	for (unsigned i = 0; i < DEVICES; i++) {
		if (chosen(&devices[i])) {
			size_t at = strlen(list);

			snprintf(list + at, sizeof(list) - at, "%s%s", at ? ", " : "", choice.uid[devices[i].role]);
		}
	}
	sink = choice.on ? open(HAKIOS_AUDIO_DEVICE, O_WRONLY) : -1;
	say("loaded by the HAL: %s; devices: %s; %s: %s\n", choice.on ? "on" : "dlisa_audio=0, nothing published",
	    list[0] ? list : "none", HAKIOS_AUDIO_DEVICE, !choice.on ? "not opened" : sink < 0 ? strerror(errno) : "open");
	return 0;
}

static OSStatus
create_device(__unused AudioServerPlugInDriverRef driver, __unused CFDictionaryRef description,
    __unused const AudioServerPlugInClientInfo *client, __unused AudioObjectID *object)
{
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
destroy_device(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object)
{
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
device_client(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, const AudioServerPlugInClientInfo *client)
{
	say("a client of %s: pid %d\n", uid_text(device_of(object)), client != NULL ? client->mProcessID : -1);
	return 0;
}

static OSStatus
configuration_change(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt64 action,
    __unused void *info)
{
	return 0;
}

/* A property's value: `size` bytes of it, or a list with nothing in it, or (neither) no such property. */
struct value {
	UInt32 size;
	bool empty;
	bool string;                    /* a CFString: whoever is given it gets a reference of their own */
	union {
		UInt32 words[8];
		Float64 number;
		Float32 decimal;
		CFStringRef string;
		AudioValueRange range;
		AudioStreamBasicDescription format;
		AudioStreamRangedDescription ranged;
	} u;
};

#define WORD(x)         (v.u.words[0] = (x), v.size = sizeof(UInt32))
#define LIST(x)         (v.u.words[v.size / sizeof(UInt32)] = (x), v.size += sizeof(UInt32))
#define STRING(x)       (v.u.string = (x), v.string = true, v.size = sizeof(CFStringRef))
#define FLOAT(x)        (v.u.decimal = (x), v.size = sizeof(Float32))

static struct value
volume_property(UInt32 selector)
{
	struct value v;

	memset(&v, 0, sizeof(v));
	switch (selector) {
	case kAudioObjectPropertyBaseClass: WORD(kAudioLevelControlClassID); break;
	case kAudioObjectPropertyClass: WORD(kAudioVolumeControlClassID); break;
	case kAudioObjectPropertyOwner: WORD(OBJECT_SPEAKER); break;
	case kAudioObjectPropertyOwnedObjects: v.empty = true; break;
	case kAudioControlPropertyScope: WORD(kAudioObjectPropertyScopeOutput); break;
	case kAudioControlPropertyElement: WORD(0); break;
	case kAudioLevelControlPropertyScalarValue: FLOAT(volume); break;
	case kAudioLevelControlPropertyDecibelValue: FLOAT(dlisa_audio_scalar_to_db(volume)); break;
	case kAudioLevelControlPropertyDecibelRange:
		v.u.range.mMinimum = -DLISA_AUDIO_VOLUME_RANGE_DB;
		v.u.range.mMaximum = 0;
		v.size = sizeof(AudioValueRange);
		break;
	case kAudioLevelControlPropertyConvertScalarToDecibels:
	case kAudioLevelControlPropertyConvertDecibelsToScalar:
		FLOAT(0);               /* (converts what it is given: get_property_data()) */
		break;
	}
	return v;
}

static struct value
source_property(UInt32 selector, UInt32 qualifier_size, const void *qualifier)
{
	struct value v;

	memset(&v, 0, sizeof(v));
	switch (selector) {
	case kAudioObjectPropertyBaseClass: WORD(kAudioSelectorControlClassID); break;
	case kAudioObjectPropertyClass: WORD(kAudioDataSourceControlClassID); break;
	case kAudioObjectPropertyOwner: WORD(OBJECT_CODEC); break;
	case kAudioObjectPropertyOwnedObjects: v.empty = true; break;
	case kAudioControlPropertyScope: WORD(kAudioObjectPropertyScopeInput); break;
	case kAudioControlPropertyElement: WORD(0); break;
	case kAudioSelectorControlPropertyCurrentItem:
	case kAudioSelectorControlPropertyAvailableItems: WORD(MIC_SOURCE); break;
	case kAudioSelectorControlPropertyItemName:
		STRING(qualifier_size == sizeof(UInt32) && qualifier != NULL && *(const UInt32 *)qualifier == MIC_SOURCE ?
		    CFSTR("Bottom") : CFSTR(""));
		break;
	}
	return v;
}

static struct value
plugin_property(UInt32 selector, UInt32 qualifier_size, const void *qualifier)
{
	struct value v;

	memset(&v, 0, sizeof(v));
	switch (selector) {
	case kAudioObjectPropertyBaseClass: WORD(kAudioObjectClassID); break;
	case kAudioObjectPropertyClass: WORD(kAudioPlugInClassID); break;
	case kAudioObjectPropertyOwner: WORD(0); break;
	case kAudioObjectPropertyManufacturer: STRING(MANUFACTURER); break;
	case kAudioObjectPropertyOwnedObjects:
	case kAudioPlugInPropertyDeviceList:
		for (unsigned i = 0; i < DEVICES; i++) {
			if (chosen(&devices[i])) {
				LIST(devices[i].object);
			}
		}
		v.empty = v.size == 0;
		break;
	case kAudioPlugInPropertyBoxList: v.empty = true; break;
	case kAudioPlugInPropertyTranslateUIDToBox: WORD(0); break;
	case kAudioPlugInPropertyTranslateUIDToDevice:
		WORD(0);
		for (unsigned i = 0; i < DEVICES; i++) {
			if (chosen(&devices[i]) && qualifier_size == sizeof(CFStringRef) && qualifier != NULL &&
			    *(const CFStringRef *)qualifier != NULL && CFEqual(*(const CFStringRef *)qualifier, devices[i].uid)) {
				WORD(devices[i].object);
			}
		}
		break;
	case kAudioPlugInPropertyResourceBundle: STRING(CFSTR("")); break;
	}
	return v;
}

static struct value
device_property(const struct device *d, UInt32 selector, UInt32 scope)
{
	struct value v;

	memset(&v, 0, sizeof(v));
	switch (selector) {
	case kAudioObjectPropertyBaseClass: WORD(kAudioObjectClassID); break;
	case kAudioObjectPropertyClass: WORD(kAudioDeviceClassID); break;
	case kAudioObjectPropertyOwner: WORD(OBJECT_PLUGIN); break;
	case kAudioObjectPropertyManufacturer: STRING(MANUFACTURER); break;
	case kAudioObjectPropertyName:
	case kAudioDevicePropertyDeviceUID:
	case kAudioDevicePropertyModelUID: STRING(d->uid); break;
	case kAudioDevicePropertyTransportType: WORD(TRANSPORT_BUILT_IN); break;
	case kAudioDevicePropertyRelatedDevices: WORD(d->object); break;
	case kAudioDevicePropertyClockDomain:
	case kAudioDevicePropertyLatency:
	case kAudioDevicePropertySafetyOffset:
	case kAudioDevicePropertyIsHidden: WORD(0); break;
	case kAudioDevicePropertyDeviceIsAlive: WORD(1); break;
	case kAudioDevicePropertyDeviceCanBeDefaultDevice:
	case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: WORD(d->role == DLISA_SPEAKER); break;
	case kAudioDevicePropertyDeviceIsRunning: WORD(d->running != 0); break;
	case kAudioObjectPropertyOwnedObjects:
	case kAudioDevicePropertyStreams:
		/* the streams of one direction, or of both (and the controls it owns) */
		if (scope != kAudioObjectPropertyScopeInput && d->output != 0) {
			LIST(d->output);
		}
		if (scope != kAudioObjectPropertyScopeOutput && d->input != 0) {
			LIST(d->input);
		}
		if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_SPEAKER &&
		    scope != kAudioObjectPropertyScopeInput) {
			LIST(OBJECT_SPEAKER_VOLUME);
		}
		if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_CODEC && scope != kAudioObjectPropertyScopeOutput) {
			LIST(OBJECT_CODEC_SOURCE);
		}
		v.empty = v.size == 0;
		break;
	case kAudioObjectPropertyControlList:
		if (d->object == OBJECT_SPEAKER) {
			LIST(OBJECT_SPEAKER_VOLUME);
		}
		if (d->object == OBJECT_CODEC) {
			LIST(OBJECT_CODEC_SOURCE);
		}
		v.empty = v.size == 0;
		break;
	case kAudioDevicePropertyNominalSampleRate:
		v.u.number = HAKIOS_AUDIO_RATE;
		v.size = sizeof(Float64);
		break;
	case kAudioDevicePropertyAvailableNominalSampleRates:
		v.u.range.mMinimum = v.u.range.mMaximum = HAKIOS_AUDIO_RATE;
		v.size = sizeof(AudioValueRange);
		break;
	case kAudioDevicePropertyPreferredChannelsForStereo:
		v.u.words[0] = 1;
		v.u.words[1] = d->output_channels >= 2 || d->input_channels >= 2 ? 2 : 1;
		v.size = 2 * sizeof(UInt32);
		break;
	case kAudioDevicePropertyZeroTimeStampPeriod: WORD(TIMESTAMP_PERIOD); break;
	}
	return v;
}

static struct value
stream_property(const struct device *d, AudioObjectID object, UInt32 selector)
{
	struct value v;

	memset(&v, 0, sizeof(v));
	switch (selector) {
	case kAudioObjectPropertyBaseClass: WORD(kAudioObjectClassID); break;
	case kAudioObjectPropertyClass: WORD(kAudioStreamClassID); break;
	case kAudioObjectPropertyOwner: WORD(d->object); break;
	case kAudioObjectPropertyOwnedObjects: v.empty = true; break;
	case kAudioStreamPropertyIsActive: WORD(!inactive[object]); break;
	case kAudioStreamPropertyDirection: WORD(object == d->input); break;
	case kAudioStreamPropertyTerminalType:
		WORD(object == d->input ? kAudioStreamTerminalTypeMicrophone : d->terminal);
		break;
	case kAudioStreamPropertyStartingChannel: WORD(1); break;
	case kAudioStreamPropertyLatency: WORD(0); break;
	case kAudioStreamPropertyVirtualFormat:
	case kAudioStreamPropertyPhysicalFormat:
		v.u.format = format_of(object);
		v.size = sizeof(AudioStreamBasicDescription);
		break;
	case kAudioStreamPropertyAvailableVirtualFormats:
	case kAudioStreamPropertyAvailablePhysicalFormats:
		v.u.ranged.mFormat = format_of(object);
		v.u.ranged.mSampleRateRange.mMinimum = v.u.ranged.mSampleRateRange.mMaximum = HAKIOS_AUDIO_RATE;
		v.size = sizeof(AudioStreamRangedDescription);
		break;
	}
	return v;
}

static struct value
property(AudioObjectID object, const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier)
{
	const struct device *d = device_of(object);
	struct value none;

	if (object == OBJECT_SPEAKER_VOLUME) {
		return volume_property(address->mSelector);
	}
	if (object == OBJECT_CODEC_SOURCE) {
		return source_property(address->mSelector, qualifier_size, qualifier);
	}
	if (object == OBJECT_PLUGIN) {
		return plugin_property(address->mSelector, qualifier_size, qualifier);
	}
	memset(&none, 0, sizeof(none));
	if (d == NULL) {
		return none;
	}
	return object == d->object ? device_property(d, address->mSelector, address->mScope) :
	       stream_property(d, object, address->mSelector);
}

#undef WORD
#undef LIST
#undef STRING
#undef FLOAT

/*
 * What the HAL (or the virtual audio device through it) asks of an object that it does not have, the first times for
 * each object, selector and scope: what a device it will not take may lack.
 */
static UInt32 unknown_told[UNKNOWN_TOLD][3];
static unsigned unknown_count;

static void
unknown(AudioObjectID object, const AudioObjectPropertyAddress *address)
{
	UInt32 s = address->mSelector, c = address->mScope;

	for (unsigned i = 0; i < unknown_count; i++) {
		if (unknown_told[i][0] == object && unknown_told[i][1] == s && unknown_told[i][2] == c) {
			return;
		}
	}
	if (unknown_count >= UNKNOWN_TOLD) {
		return;
	}
	unknown_told[unknown_count][0] = object;
	unknown_told[unknown_count][1] = s;
	unknown_told[unknown_count][2] = c;
	unknown_count++;
	say("asked of object %u (%s) what it does not have: '%c%c%c%c' (%#x), scope '%c%c%c%c', element %u\n", (unsigned)object,
	    object == OBJECT_PLUGIN ? "the plug-in" : uid_text(device_of(object)), (int)(s >> 24 & 0xff), (int)(s >> 16 & 0xff),
	    (int)(s >> 8 & 0xff), (int)(s & 0xff), (unsigned)s, (int)(c >> 24 & 0xff), (int)(c >> 16 & 0xff),
	    (int)(c >> 8 & 0xff), (int)(c & 0xff), (unsigned)address->mElement);
}

static Boolean
has_property(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused pid_t client,
    const AudioObjectPropertyAddress *address)
{
	struct value v;

	if (address == NULL) {
		return false;
	}
	v = property(object, address, 0, NULL);
	if (v.size == 0 && !v.empty) {
		unknown(object, address);
	}
	return v.size != 0 || v.empty;
}

static OSStatus
is_property_settable(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, Boolean *settable)
{
	const struct device *d = device_of(object);

	if (address == NULL || settable == NULL || !has_property(driver, object, client, address)) {
		return kAudioHardwareUnknownPropertyError;
	}
	/* the rate and the formats can be "set", to the ones there are; the volume can be set */
	if (object == OBJECT_SPEAKER_VOLUME) {
		*settable = address->mSelector == kAudioLevelControlPropertyScalarValue ||
		    address->mSelector == kAudioLevelControlPropertyDecibelValue;
	} else if (object == OBJECT_CODEC_SOURCE) {
		*settable = address->mSelector == kAudioSelectorControlPropertyCurrentItem;
	} else if (object == OBJECT_PLUGIN) {
		*settable = false;
	} else if (object == d->object) {
		*settable = address->mSelector == kAudioDevicePropertyNominalSampleRate;
	} else {
		*settable = address->mSelector == kAudioStreamPropertyIsActive ||
		    address->mSelector == kAudioStreamPropertyVirtualFormat || address->mSelector == kAudioStreamPropertyPhysicalFormat;
	}
	return 0;
}

static OSStatus
get_property_data_size(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 *size)
{
	if (address == NULL || size == NULL || !has_property(driver, object, client, address)) {
		return object < OBJECT_PLUGIN || object > OBJECT_LAST ? kAudioHardwareBadObjectError : kAudioHardwareUnknownPropertyError;
	}
	*size = property(object, address, qualifier_size, qualifier).size;
	return 0;
}

static bool
is_list(UInt32 selector)
{
	return selector == kAudioObjectPropertyOwnedObjects || selector == kAudioDevicePropertyStreams ||
	       selector == kAudioPlugInPropertyDeviceList || selector == kAudioObjectPropertyControlList;
}

static OSStatus
get_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 room, UInt32 *size,
    void *data)
{
	struct value v;

	if (address == NULL || size == NULL || data == NULL || !has_property(driver, object, client, address)) {
		return object < OBJECT_PLUGIN || object > OBJECT_LAST ? kAudioHardwareBadObjectError : kAudioHardwareUnknownPropertyError;
	}
	if (object == OBJECT_SPEAKER_VOLUME && (address->mSelector == kAudioLevelControlPropertyConvertScalarToDecibels ||
	    address->mSelector == kAudioLevelControlPropertyConvertDecibelsToScalar)) {
		/* what is in the buffer, converted where it is */
		if (room < sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(Float32 *)data = address->mSelector == kAudioLevelControlPropertyConvertScalarToDecibels ?
		    dlisa_audio_scalar_to_db(*(Float32 *)data) : dlisa_audio_db_to_scalar(*(Float32 *)data);
		*size = sizeof(Float32);
		return 0;
	}
	pthread_mutex_lock(&lock);
	v = property(object, address, qualifier_size, qualifier);
	pthread_mutex_unlock(&lock);
	if (v.size > room) {
		/* a list may be asked for in part; anything else must fit */
		if (!is_list(address->mSelector)) {
			return kAudioHardwareBadPropertySizeError;
		}
		v.size = room - room % sizeof(UInt32);
	}
	memcpy(data, &v.u, v.size);
	if (v.string) {
		CFRetain(v.u.string);
	}
	*size = v.size;
	return 0;
}

static OSStatus
set_volume(const AudioObjectPropertyAddress *address, UInt32 size, const void *data)
{
	AudioObjectPropertyAddress changed[2] = {
		{ kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0 },
		{ kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, 0 },
	};
	Float32 wanted;

	if (size != sizeof(Float32)) {
		return kAudioHardwareBadPropertySizeError;
	}
	wanted = *(const Float32 *)data;
	wanted = address->mSelector == kAudioLevelControlPropertyDecibelValue ? dlisa_audio_db_to_scalar(wanted) :
	    wanted < 0 ? 0 : wanted > 1 ? 1 : wanted;
	pthread_mutex_lock(&lock);
	volume = wanted;
	volume_to_kernel();
	pthread_mutex_unlock(&lock);
	if (host != NULL) {
		host->PropertiesChanged(host, OBJECT_SPEAKER_VOLUME, 2, changed);
	}
	return 0;
}

static OSStatus
set_format(AudioObjectID object, UInt32 size, const void *data)
{
	const AudioStreamBasicDescription *wanted = data;
	AudioStreamBasicDescription format;

	if (size != sizeof(AudioStreamBasicDescription)) {
		return kAudioHardwareBadPropertySizeError;
	}
	format = format_of(object);
	return wanted->mSampleRate == format.mSampleRate && wanted->mFormatID == format.mFormatID &&
	       wanted->mChannelsPerFrame == format.mChannelsPerFrame && wanted->mBitsPerChannel == format.mBitsPerChannel &&
	       (wanted->mFormatFlags & kAudioFormatFlagIsSignedInteger) ? 0 : kAudioDeviceUnsupportedFormatError;
}

static OSStatus
set_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, __unused UInt32 qualifier_size, __unused const void *qualifier, UInt32 size,
    const void *data)
{
	Boolean settable = false;

	if (address == NULL || data == NULL || is_property_settable(driver, object, client, address, &settable) != 0) {
		return kAudioHardwareUnknownPropertyError;
	}
	if (!settable) {
		return kAudioHardwareIllegalOperationError;
	}
	if (object == OBJECT_SPEAKER_VOLUME) {
		return set_volume(address, size, data);
	}
	if (object == OBJECT_CODEC_SOURCE) {
		return size != sizeof(UInt32) ? kAudioHardwareBadPropertySizeError :
		       *(const UInt32 *)data == MIC_SOURCE ? 0 : kAudioHardwareIllegalOperationError;
	}
	if (address->mSelector == kAudioStreamPropertyIsActive) {
		if (size != sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		pthread_mutex_lock(&lock);
		inactive[object] = *(const UInt32 *)data == 0;
		pthread_mutex_unlock(&lock);
		return 0;
	}
	if (address->mSelector == kAudioDevicePropertyNominalSampleRate) {
		return size == sizeof(Float64) && *(const Float64 *)data == HAKIOS_AUDIO_RATE ? 0 : kAudioDeviceUnsupportedFormatError;
	}
	return set_format(object, size, data);
}

static OSStatus
start_io(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client)
{
	struct device *d = device_of(object);

	if (d == NULL || object != d->object) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (d->running++ == 0) {
		d->clock.anchor = mach_absolute_time();
		d->clock.periods = 0;
		say("%s: I/O starts\n", uid_text(d));
		if (d->role == DLISA_SPEAKER) {
			memset(&played, 0, sizeof(played));
			played.second_start = d->clock.anchor;
		}
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

static OSStatus
stop_io(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client)
{
	struct device *d = device_of(object);

	if (d == NULL || object != d->object) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (d->running != 0 && --d->running == 0) {
		if (d->role == DLISA_SPEAKER) {
			say("Speaker: I/O stops after %llu.%02llu s, %llu.%02llu s of it sound, peak %d of 32768, %llu frames not "
			    "taken by %s\n", played.frames / HAKIOS_AUDIO_RATE, played.frames % HAKIOS_AUDIO_RATE * 100 / HAKIOS_AUDIO_RATE,
			    played.sound_frames / HAKIOS_AUDIO_RATE, played.sound_frames % HAKIOS_AUDIO_RATE * 100 / HAKIOS_AUDIO_RATE,
			    played.peak, played.refused, HAKIOS_AUDIO_DEVICE);
		} else {
			say("%s: I/O stops (actuator %llu frames peak %d, receiver %llu frames peak %d: dropped)\n", uid_text(d),
			    actuator_frames, actuator_peak, receiver_frames, receiver_peak);
		}
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

static OSStatus
get_zero_time_stamp(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client,
    Float64 *sample_time, UInt64 *host_time, UInt64 *seed)
{
	struct device *d = device_of(object);
	double sample;
	uint64_t at;

	if (d == NULL) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	dlisa_audio_zero_time(&d->clock, mach_absolute_time(), ticks_per_frame, TIMESTAMP_PERIOD, &sample, &at);
	pthread_mutex_unlock(&lock);
	*sample_time = sample;
	*host_time = at;
	*seed = 1;
	return 0;
}

static OSStatus
will_do_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt32 client,
    UInt32 operation, Boolean *will_do, Boolean *in_place)
{
	*will_do = operation == kAudioServerPlugInIOOperationReadInput || operation == kAudioServerPlugInIOOperationWriteMix;
	*in_place = true;
	return 0;
}

static OSStatus
begin_end_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt32 client,
    __unused UInt32 operation, __unused UInt32 frames, __unused const AudioServerPlugInIOCycleInfo *cycle)
{
	return 0;
}

static int
peak_of(const int16_t *samples, size_t count)
{
	int peak = 0;

	for (size_t i = 0; i < count; i++) {
		int v = samples[i] < 0 ? -samples[i] : samples[i];

		peak = v > peak ? v : peak;
	}
	return peak;
}

/* The speaker's output: to the kernel, which takes it all or leaves it (this thread must not wait); counted. */
static void
speaker_write(const int16_t *samples, UInt32 frames)
{
	int peak = peak_of(samples, (size_t)frames * HAKIOS_AUDIO_CHANNELS);
	UInt64 now = mach_absolute_time();

	if (sink < 0 || write(sink, samples, (size_t)frames * HAKIOS_AUDIO_FRAME) < 0) {
		played.refused += frames;
	}
	played.frames += frames;
	played.sound_frames += peak > 0 ? frames : 0;
	played.peak = peak > played.peak ? peak : played.peak;
	played.second_frames += frames;
	played.second_peak = peak > played.second_peak ? peak : played.second_peak;
	if ((double)(now - played.second_start) >= ticks_per_frame * HAKIOS_AUDIO_RATE) {
		if (played.seconds_said < 120 && played.second_peak > 0) {
			played.seconds_said++;
			say("Speaker: %llu frames in the last second to %s, peak %d of 32768 (volume %.3f)\n", played.second_frames,
			    HAKIOS_AUDIO_DEVICE, played.second_peak, volume);
		}
		played.second_start = now;
		played.second_frames = 0;
		played.second_peak = 0;
	}
}

static OSStatus
do_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, AudioObjectID stream,
    __unused UInt32 client, UInt32 operation, UInt32 frames, __unused const AudioServerPlugInIOCycleInfo *cycle, void *buffer,
    __unused void *secondary)
{
	const struct device *d = device_of(stream);
	int peak;

	if (d == NULL) {
		return kAudioHardwareBadObjectError;
	}
	if (operation == kAudioServerPlugInIOOperationReadInput) {
		/* every input here is silence: no microphone driver, no amplifier telemetry */
		memset(buffer, 0, (size_t)frames * 2 * d->input_channels);
		return 0;
	}
	if (operation != kAudioServerPlugInIOOperationWriteMix || stream != d->output) {
		return 0;
	}
	if (d->role == DLISA_SPEAKER) {
		speaker_write(buffer, frames);
		return 0;
	}
	peak = peak_of(buffer, (size_t)frames * d->output_channels);
	if (d->role == DLISA_ACTUATOR) {
		actuator_frames += frames;
		actuator_peak = peak > actuator_peak ? peak : actuator_peak;
	} else {
		receiver_frames += frames;
		receiver_peak = peak > receiver_peak ? peak : receiver_peak;
	}
	return 0;
}

static AudioServerPlugInDriverInterface interface = {
	NULL, query_interface, add_ref, release, initialize, create_device, destroy_device, device_client, device_client,
	configuration_change, configuration_change, has_property, is_property_settable, get_property_data_size,
	get_property_data, set_property_data, start_io, stop_io, get_zero_time_stamp, will_do_io_operation,
	begin_end_io_operation, do_io_operation, begin_end_io_operation,
};
static AudioServerPlugInDriverInterface *driver = &interface;

/* The factory named in the bundle's Info.plist. */
void *
DLisaAudio_Create(__unused CFAllocatorRef allocator, CFUUIDRef type)
{
	if (!CFEqual(type, kAudioServerPlugInTypeUUID)) {
		return NULL;
	}
	read_choice();
	say("factory called: the HAL asks for an AudioServerPlugIn\n");
	return &driver;
}
