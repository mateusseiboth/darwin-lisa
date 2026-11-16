/*
 * lisa's sensors as built-in HID services inside backboardd (included by dlisa-motion.c, backboardd only).
 *
 * backboardd is the HID event server on iOS. What it and the frameworks it loads look for, read from the 23F84
 * backboardd and dyld cache (kernel/drivers/adsp/README.md, "iOS side"):
 *   - accelerometer: a built-in service (0xff00, 0x03) [BKOrientationManager's BKIOHIDServiceMatcher], its
 *     kIOHIDEventTypeAccelerometer (13) events go to BKAccelerometerInterface (UIAccelerometer/BKSAccelerometer
 *     clients, "Acc (type %d): %f %f %f" in backboardd's log);
 *   - gyroscope (0xff00, 0x09) [BKHIDStudyLogEventProcessor], events type 20; compass (0xff00, 0x0a), type 21;
 *   - ambient light: CoreBrightness takes a service that conforms to (0xff00, 0x04) [CBABModuleiOS, AABC] or
 *     (0x20, 0x41) [CBALSNode]; events type 12, level 0xc0000 (whole lux) and illuminance 0xc000b (lux, double;
 *     +[CBALSEvent replaceLuxWithFilteredLux:] writes the two);
 *   - proximity: (0xff00, 0x08) [AABC]; events type 14, detection mask 0xe0000, near = 0x440
 *     (-[BKHIDProximityEventProcessor processEvent:sender:dispatcher:]).
 * The services are virtual (IOHIDVirtualServiceClientCreateWithCallbacks, as libdlisa's touch screen), fed from
 * sysctl kern.hakios.sensors every 20 ms; a sensor's event goes out when the kernel's count for it moves (proximity:
 * when near/far changes).
 *
 * Boot-arg sensors_hid=0 leaves the services out (the rotation hook above still runs).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>

#include "sensor-events.c"

typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

/* IOHIDVirtualServiceClientCallbacksV2 (as userland/inject/dlisa-inject.c) */
struct hidServiceCallbacks {
	uint64_t version;
	void (*notification)(void *, void *, IOHIDServiceClientRef, uint32_t, CFDictionaryRef);
	Boolean (*setProperty)(void *, void *, IOHIDServiceClientRef, CFStringRef, CFTypeRef);
	CFTypeRef (*copyProperty)(void *, void *, IOHIDServiceClientRef, CFStringRef);
	IOHIDEventRef (*copyEvent)(void *, void *, IOHIDServiceClientRef, uint32_t, IOHIDEventRef, uint32_t);
	int (*setOutputEvent)(void *, void *, IOHIDServiceClientRef, IOHIDEventRef);
	CFTypeRef (*copyMatchingEvent)(void *, void *, IOHIDServiceClientRef, CFDictionaryRef);
};

static struct {
	IOHIDEventSystemClientRef (*clientCreateWithType)(CFAllocatorRef, int, CFDictionaryRef);
	void (*clientSchedule)(IOHIDEventSystemClientRef, dispatch_queue_t);
	IOHIDServiceClientRef (*virtualCreate)(IOHIDEventSystemClientRef, CFDictionaryRef, const struct hidServiceCallbacks *,
	    void *, void *);
	Boolean (*virtualDispatch)(IOHIDServiceClientRef, IOHIDEventRef);
	Boolean (*virtualRemove)(IOHIDServiceClientRef);
	/* the typed constructors: their argument registers checked in the 23F84 IOKit */
	IOHIDEventRef (*accelerometer)(CFAllocatorRef, uint64_t, double, double, double, uint32_t);
	IOHIDEventRef (*gyro)(CFAllocatorRef, uint64_t, double, double, double, uint32_t);
	IOHIDEventRef (*compass)(CFAllocatorRef, uint64_t, double, double, double, uint32_t);
	IOHIDEventRef (*light)(CFAllocatorRef, uint64_t, uint32_t, uint32_t);
	IOHIDEventRef (*proximity)(CFAllocatorRef, uint64_t, uint32_t, uint32_t);
	void (*setDouble)(IOHIDEventRef, uint32_t, double);
	void (*setInteger)(IOHIDEventRef, uint32_t, long);
} iohid;

static bool
findIOHID(void)
{
	static const struct {
		const char *name;
		void **slot;
	} wanted[] = {
		{ "IOHIDEventSystemClientCreateWithType", (void **)&iohid.clientCreateWithType },
		{ "IOHIDEventSystemClientScheduleWithDispatchQueue", (void **)&iohid.clientSchedule },
		{ "IOHIDVirtualServiceClientCreateWithCallbacks", (void **)&iohid.virtualCreate },
		{ "IOHIDVirtualServiceClientDispatchEvent", (void **)&iohid.virtualDispatch },
		{ "IOHIDVirtualServiceClientRemove", (void **)&iohid.virtualRemove },
		{ "IOHIDEventCreateAccelerometerEvent", (void **)&iohid.accelerometer },
		{ "IOHIDEventCreateGyroEvent", (void **)&iohid.gyro },
		{ "IOHIDEventCreateCompassEvent", (void **)&iohid.compass },
		{ "IOHIDEventCreateAmbientLightSensorEvent", (void **)&iohid.light },
		{ "IOHIDEventCreateProximtyEvent", (void **)&iohid.proximity },        /* (Apple's spelling) */
		{ "IOHIDEventSetDoubleValue", (void **)&iohid.setDouble },
		{ "IOHIDEventSetIntegerValue", (void **)&iohid.setInteger },
	};
	void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);

	for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
		*wanted[i].slot = dlsym(iokit != NULL ? iokit : RTLD_DEFAULT, wanted[i].name);
		if (*wanted[i].slot == NULL) {
			say("dlisa-motion: hid: %s not found: no sensor services\n", wanted[i].name);
			return false;
		}
	}
	return true;
}

#define HID_CLIENT_SIMPLE               4
#define PAGE_APPLE_VENDOR               0xff00
#define PAGE_SENSOR                     0x20
#define FIELD_LIGHT_ILLUMINANCE         0xc000b
#define FIELD_IS_BUILT_IN               0x4

/* ---- one strategy per sensor: its service's usages and the event made from the kernel's values ---------------- */

typedef IOHIDEventRef (*makeEvent_t)(const struct dlisa_sensors *, uint64_t time);

static IOHIDEventRef
makeAccelerometerEvent(const struct dlisa_sensors *s, uint64_t time)
{
	struct sensorVector v = appleAcceleration(asFloat(s->accel[0]), asFloat(s->accel[1]), asFloat(s->accel[2]));

	return iohid.accelerometer(NULL, time, v.x, v.y, v.z, 0);
}

static IOHIDEventRef
makeGyroEvent(const struct dlisa_sensors *s, uint64_t time)
{
	struct sensorVector v = appleRotation(asFloat(s->gyro[0]), asFloat(s->gyro[1]), asFloat(s->gyro[2]));

	return iohid.gyro(NULL, time, v.x, v.y, v.z, 0);
}

static IOHIDEventRef
makeCompassEvent(const struct dlisa_sensors *s, uint64_t time)
{
	struct sensorVector v = appleField(asFloat(s->mag[0]), asFloat(s->mag[1]), asFloat(s->mag[2]));

	return iohid.compass(NULL, time, v.x, v.y, v.z, 0);
}

static IOHIDEventRef
makeLightEvent(const struct dlisa_sensors *s, uint64_t time)
{
	float lux = asFloat(s->light);
	IOHIDEventRef event = iohid.light(NULL, time, (uint32_t)luxLevel(lux), 0);

	if (event != NULL) {
		iohid.setDouble(event, FIELD_LIGHT_ILLUMINANCE, luxIlluminance(lux));
	}
	return event;
}

static int proximitySent = PROXIMITY_UNKNOWN;

static IOHIDEventRef
makeProximityEvent(const struct dlisa_sensors *s, uint64_t time)
{
	if (!proximityChanged(s->proximity_near, &proximitySent)) {
		return NULL;
	}
	return iohid.proximity(NULL, time, proximityMask(proximitySent), 0);
}

static const struct sensorService {
	int sensor;
	const char *product;
	int page, usage;
	int alsoPage, alsoUsage;        /* a second usage pair the service conforms to (0: none) */
	makeEvent_t make;
} sensorServices[] = {
	{ SENSOR_ACCEL, "darwin-lisa accelerometer", PAGE_APPLE_VENDOR, 0x03, 0, 0, makeAccelerometerEvent },
	{ SENSOR_GYRO, "darwin-lisa gyroscope", PAGE_APPLE_VENDOR, 0x09, 0, 0, makeGyroEvent },
	{ SENSOR_MAG, "darwin-lisa compass", PAGE_APPLE_VENDOR, 0x0a, 0, 0, makeCompassEvent },
	{ SENSOR_LIGHT, "darwin-lisa ambient light", PAGE_APPLE_VENDOR, 0x04, PAGE_SENSOR, 0x41, makeLightEvent },
	{ SENSOR_PROXIMITY, "darwin-lisa proximity", PAGE_APPLE_VENDOR, 0x08, 0, 0, makeProximityEvent },
};
#define SENSOR_SERVICES (sizeof(sensorServices) / sizeof(sensorServices[0]))

/* ---- the services ---------------------------------------------------------------------------------------------- */

struct sensorServiceState {
	const struct sensorService *service;
	CFMutableDictionaryRef properties;
	IOHIDServiceClientRef client;
	uint32_t sent;
	volatile bool opened;           /* the event server has set DeviceOpenedByEventSystem */
};

static CFNumberRef
cfInt(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

static CFDictionaryRef
usagePair(int page, int usage)
{
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);

	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), cfInt(page));
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), cfInt(usage));
	return pair;
}

static CFMutableDictionaryRef
serviceProperties(const struct sensorService *service)
{
	CFMutableDictionaryRef p = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableArrayRef pairs = CFArrayCreateMutable(NULL, 2, &kCFTypeArrayCallBacks);

	CFArrayAppendValue(pairs, usagePair(service->page, service->usage));
	if (service->alsoPage != 0) {
		CFArrayAppendValue(pairs, usagePair(service->alsoPage, service->alsoUsage));
	}
	CFDictionarySetValue(p, CFSTR("PrimaryUsagePage"), cfInt(service->page));
	CFDictionarySetValue(p, CFSTR("PrimaryUsage"), cfInt(service->usage));
	CFDictionarySetValue(p, CFSTR("DeviceUsagePage"), cfInt(service->page));
	CFDictionarySetValue(p, CFSTR("DeviceUsage"), cfInt(service->usage));
	CFDictionarySetValue(p, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(p, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(p, CFSTR("Transport"), CFSTR("SPU"));
	CFDictionarySetValue(p, CFSTR("Manufacturer"), CFSTR("darwin-lisa"));
	CFDictionarySetValue(p, CFSTR("Product"), CFStringCreateWithCString(NULL, service->product, kCFStringEncodingUTF8));
	CFDictionarySetValue(p, CFSTR("VendorID"), cfInt(0));
	CFDictionarySetValue(p, CFSTR("ProductID"), cfInt(0));
	return p;
}

/* What the event server and its clients ask of a service and it lacks, once per key, for the test plan. */
static CFTypeRef
sensorCopyProperty(__unused void *target, void *refcon, __unused IOHIDServiceClientRef client, CFStringRef key)
{
	static CFMutableSetRef asked;
	struct sensorServiceState *state = refcon;
	CFTypeRef value = CFDictionaryGetValue(state->properties, key);

	if (asked == NULL) {
		asked = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (value == NULL && !CFSetContainsValue(asked, key) && CFSetGetCount(asked) < 64) {
		char name[96] = "?";

		CFSetAddValue(asked, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		say("dlisa-motion: hid: \"%s\" asked for \"%s\", which it does not have\n", state->service->product, name);
	}
	return value != NULL ? CFRetain(value) : NULL;
}

/* (ReportInterval and the like: kept and logged; the kernel's rate is sensors_rate=, the services follow it) */
static Boolean
sensorSetProperty(__unused void *target, void *refcon, __unused IOHIDServiceClientRef client, CFStringRef key, CFTypeRef value)
{
	struct sensorServiceState *state = refcon;
	char name[96] = "?", text[96] = "?";
	CFStringRef description;

	if (key == NULL || value == NULL) {
		return true;
	}
	CFDictionarySetValue(state->properties, key, value);
	if (CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) && CFGetTypeID(value) == CFBooleanGetTypeID() &&
	    CFBooleanGetValue(value)) {
		state->opened = true;
	}
	CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
	description = CFCopyDescription(value);
	if (description != NULL) {
		CFStringGetCString(description, text, sizeof(text), kCFStringEncodingUTF8);
		CFRelease(description);
	}
	say("dlisa-motion: hid: \"%s\" set \"%s\" = %s\n", state->service->product, name, text);
	return true;
}

static void
sensorNotification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef client, __unused uint32_t type,
    __unused CFDictionaryRef property)
{
}

static const struct hidServiceCallbacks sensorCallbacks = {
	.version = 2, .notification = sensorNotification, .setProperty = sensorSetProperty, .copyProperty = sensorCopyProperty,
};

static void
sendSensorEvent(struct sensorServiceState *state, const struct dlisa_sensors *s)
{
	IOHIDEventRef event = state->service->make(s, mach_absolute_time());

	if (event == NULL) {
		return;
	}
	iohid.setInteger(event, FIELD_IS_BUILT_IN, 1);
	iohid.virtualDispatch(state->client, event);
	CFRelease(event);
	if (state->sent++ < 3 || (state->sent & 1023) == 0) {
		say("dlisa-motion: hid: \"%s\" event %u\n", state->service->product, state->sent);
	}
}

static struct sensorServiceState sensorStates[SENSOR_SERVICES];

/*
 * A service made before the event server is ready is never opened by it (libdlisa's touch screen found this), so it
 * is made again until the server has opened it: 20 tries, 2 s each.
 */
static bool
publishSensorService(IOHIDEventSystemClientRef system, struct sensorServiceState *state)
{
	for (int attempt = 1; attempt <= 20; attempt++) {
		state->opened = false;
		state->client = iohid.virtualCreate(system, state->properties, &sensorCallbacks, NULL, state);
		if (state->client == NULL) {
			break;
		}
		for (int tenths = 0; tenths < 20 && !state->opened; tenths++) {
			usleep(100000);
		}
		if (state->opened) {
			say("dlisa-motion: hid: \"%s\" (%#x, %#x) opened by the event server (attempt %d)\n", state->service->product,
			    state->service->page, state->service->usage, attempt);
			return true;
		}
		iohid.virtualRemove(state->client);
		CFRelease(state->client);
		state->client = NULL;
	}
	say("dlisa-motion: hid: \"%s\" NOT OPENED by the event server\n", state->service->product);
	return false;
}

static void *
sensorServicesThread(__unused void *argument)
{
	uint32_t seen[SENSORS] = { 0 };
	IOHIDEventSystemClientRef system;
	unsigned made = 0;

	pthread_setname_np("dlisa sensors");
	sleep(3);                       /* let backboardd get as far as starting its event server (libdlisa's wait) */
	system = iohid.clientCreateWithType(NULL, HID_CLIENT_SIMPLE, NULL);
	if (system == NULL) {
		say("dlisa-motion: hid: no event system client: no sensor services\n");
		return NULL;
	}
	iohid.clientSchedule(system, dispatch_queue_create("dlisa.sensors", NULL));
	for (size_t i = 0; i < SENSOR_SERVICES; i++) {
		struct sensorServiceState *state = &sensorStates[i];

		state->service = &sensorServices[i];
		state->properties = serviceProperties(state->service);
		made += publishSensorService(system, state);
	}
	if (made == 0) {
		return NULL;
	}
	for (;;) {
		struct dlisa_sensors s;
		uint32_t fresh;

		usleep(20000);
		if (!readSensors(&s) || s.source == 0) {
			continue;
		}
		fresh = takeNewSamples(s.count, seen);
		for (size_t i = 0; i < SENSOR_SERVICES; i++) {
			if (sensorStates[i].client != NULL && (fresh & (1u << sensorStates[i].service->sensor))) {
				sendSensorEvent(&sensorStates[i], &s);
			}
		}
	}
	return NULL;
}

static void
startSensorServices(void)
{
	char args[1024];
	size_t size = sizeof(args) - 1;
	pthread_t thread;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0) {
		args[size] = 0;
		if (bootArgValue(args, "sensors_hid", 1) == 0) {
			say("dlisa-motion: hid: sensors_hid=0: no sensor services\n");
			return;
		}
	}
	if (!findIOHID()) {
		return;
	}
	if (pthread_create(&thread, NULL, sensorServicesThread, NULL) == 0) {
		pthread_detach(thread);
	}
}
