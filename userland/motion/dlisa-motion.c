/*
 * libdlisa-motion.dylib: the screen's rotation on lisa, inside backboardd.
 *
 * How iOS 26 turns the phone's attitude into the interface's orientation (read from backboardd and the 23F84 cache):
 * backboardd's BKOrientationManager asks CoreMotion for device orientation updates,
 *     -[CMDeviceOrientationManager startDeviceOrientationUpdatesToQueue:withHandler:]   handler(CMDeviceOrientation *, NSError *)
 * and posts what comes ("Current raw accelerometer device orientation changed to: ...") to SpringBoard and the apps
 * (BKSHIDServices, FrontBoard), which rotate the interface unless the orientation is locked. On an iPhone, CoreMotion
 * gets that from locationd, which gets it from the always-on processor's HID services (AppleSPU, usage page 0xff0c).
 * lisa has none of that: no SPU, and locationd is left out of the lean image (its CoreLocation migrator hangs).
 *
 * So the manager is replaced here, in backboardd only: the two start methods keep the queue and the handler, a thread
 * reads the kernel's sensors (sysctl kern.hakios.sensors: the ADSP's accelerometer with sensors=1, or a made-up
 * turning gravity with sensors_fake=1; kernel/drivers/adsp/dlisa.ssc.c) ten times a second, works out the device
 * orientation from gravity with hysteresis, and calls every handler on its queue when it changes. Nothing of locationd
 * or of the SPU is needed.
 *
 * The orientation object: CMDeviceOrientation's own -initWithDeviceOrientation: when the cache has it with a number
 * argument (checked by its type encoding), else an instance of a subclass made here whose -orientation returns ours.
 *
 * Inert unless the kernel has kern.hakios.sensors (the hakios_adsp tag) and the sensors give an accelerometer.
 * Logs to /dev/console as "dlisa-motion: ...".
 */
#include <Block.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern const char *getprogname(void);

static void
say(const char *format, ...)
{
	char line[512];
	va_list ap;
	int n, fd = open("/dev/console", O_WRONLY | O_NOCTTY);

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	n = n > (int)sizeof(line) - 1 ? (int)sizeof(line) - 1 : n;
	write(fd >= 0 ? fd : 2, line, (size_t)n);
	if (fd >= 0) {
		close(fd);
	}
}

/* ---- the kernel's sensors: the same structure as kernel/drivers/adsp/dlisa.ssc.c -------------------------- */

enum { SENSOR_ACCEL, SENSOR_GYRO, SENSOR_MAG, SENSOR_LIGHT, SENSOR_PROXIMITY, SENSORS };

struct dlisa_sensors {
	uint32_t size;
	uint32_t gen;
	uint32_t source;
	uint32_t found;
	uint32_t count[SENSORS];
	uint32_t accel[3];              /* float bits, m/s^2, Android axes */
	uint32_t gyro[3];
	uint32_t mag[3];
	uint32_t light;
	uint32_t proximity;
	int32_t proximity_near;
	uint32_t reserved;
	uint64_t accel_time;
};

static bool
readSensors(struct dlisa_sensors *s)
{
	for (int attempt = 0; attempt < 4; attempt++) {
		size_t size = sizeof(*s);

		if (sysctlbyname("kern.hakios.sensors", s, &size, NULL, 0) != 0 || size != sizeof(*s) || s->size != sizeof(*s)) {
			return false;
		}
		if (!(s->gen & 1)) {
			struct dlisa_sensors again;

			size = sizeof(again);
			if (sysctlbyname("kern.hakios.sensors", &again, &size, NULL, 0) == 0 && again.gen == s->gen) {
				return true;
			}
		}
		usleep(1000);
	}
	return false;
}

static float
asFloat(uint32_t bits)
{
	float f;

	memcpy(&f, &bits, sizeof(f));
	return f;
}

/* ---- gravity -> UIDeviceOrientation ------------------------------------------------------------------------ */

enum {
	ORIENTATION_UNKNOWN = 0, ORIENTATION_PORTRAIT = 1, ORIENTATION_UPSIDE_DOWN = 2, ORIENTATION_LANDSCAPE_LEFT = 3,
	ORIENTATION_LANDSCAPE_RIGHT = 4, ORIENTATION_FACE_UP = 5, ORIENTATION_FACE_DOWN = 6,
};

static const char *const orientationNames[] = { "unknown", "portrait", "upside down", "landscape left (bottom right)",
	                                        "landscape right (bottom left)", "face up", "face down" };

/*
 * Android's axes (x to the right, y to the top, z out of the screen); the accelerometer reads +g on the axis that
 * points UP. Upright: y = +g, portrait. Top to the left (bottom to the right): x = +g, UIDeviceOrientationLandscapeLeft.
 * A candidate must win clearly (its axis 1.3x the other's: about 52 degrees from the old one) before it is taken.
 */
static int
orientationFor(float x, float y, float z, int current)
{
	float g = sqrtf(x * x + y * y + z * z), ax = fabsf(x), ay = fabsf(y);

	if (g < 4.0f || g > 16.0f) {
		return current;                 /* moving, or nonsense */
	}
	if (z > 0.85f * g) {
		return ORIENTATION_FACE_UP;
	}
	if (z < -0.85f * g) {
		return ORIENTATION_FACE_DOWN;
	}
	if (ay >= 1.3f * ax) {
		return y > 0 ? ORIENTATION_PORTRAIT : ORIENTATION_UPSIDE_DOWN;
	}
	if (ax >= 1.3f * ay) {
		return x > 0 ? ORIENTATION_LANDSCAPE_LEFT : ORIENTATION_LANDSCAPE_RIGHT;
	}
	/* between the two: keep a side orientation that still fits, else the larger axis */
	if ((current == ORIENTATION_PORTRAIT && y > 0) || (current == ORIENTATION_UPSIDE_DOWN && y < 0) ||
	    (current == ORIENTATION_LANDSCAPE_LEFT && x > 0) || (current == ORIENTATION_LANDSCAPE_RIGHT && x < 0)) {
		return current;
	}
	return ay >= ax ? (y > 0 ? ORIENTATION_PORTRAIT : ORIENTATION_UPSIDE_DOWN) :
	       (x > 0 ? ORIENTATION_LANDSCAPE_LEFT : ORIENTATION_LANDSCAPE_RIGHT);
}

/* ---- the orientation object ------------------------------------------------------------------------------- */

#define SEND0(type, object, name)       ((type (*)(id, SEL))objc_msgSend)((id)(object), sel_registerName(name))

static Class ourOrientationClass;
static ptrdiff_t ourOrientationOffset;

static long
ourOrientation(id self, __unused SEL selector)
{
	return *(long *)((char *)self + ourOrientationOffset);
}

static id
makeOrientation(long orientation)
{
	Class real = objc_getClass("CMDeviceOrientation");
	SEL init = sel_registerName("initWithDeviceOrientation:");
	Method method = real ? class_getInstanceMethod(real, init) : NULL;
	const char *types = method ? method_getTypeEncoding(method) : NULL;
	id object;

	/* the class's own initializer, when its one argument is a number (type encoding "@24@0:8q16" or the like) */
	if (types != NULL) {
		const char *last = types + strlen(types);

		while (last > types && last[-1] >= '0' && last[-1] <= '9') {
			last--;
		}
		if (last > types && strchr("qilQIL", last[-1]) != NULL &&
		    class_respondsToSelector(real, sel_registerName("orientation"))) {
			id allocated = SEND0(id, real, "alloc");

			object = ((id (*)(id, SEL, long))objc_msgSend)(allocated, init, orientation);
			if (object != NULL && SEND0(long, object, "orientation") == orientation) {
				return object;
			}
		}
	}
	if (ourOrientationClass == NULL) {
		Class super = real != NULL ? real : objc_getClass("NSObject");
		Class made = objc_allocateClassPair(super, "DLisaDeviceOrientation", 0);

		if (made == NULL) {
			return NULL;
		}
		class_addIvar(made, "_dlisaOrientation", sizeof(long), 3, "q");
		class_addMethod(made, sel_registerName("orientation"), (IMP)ourOrientation, "q16@0:8");
		objc_registerClassPair(made);
		ourOrientationOffset = ivar_getOffset(class_getInstanceVariable(made, "_dlisaOrientation"));
		ourOrientationClass = made;
		say("dlisa-motion: orientation objects are DLisaDeviceOrientation (a subclass of %s)\n", class_getName(super));
	}
	object = class_createInstance(ourOrientationClass, 0);
	if (object != NULL) {
		*(long *)((char *)object + ourOrientationOffset) = orientation;
	}
	return object;
}

/* ---- the replaced manager ----------------------------------------------------------------------------------- */

typedef void (^handler_t)(id orientation, id error);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	id manager, queue;
	handler_t handler;
} clients[8];
static id current;                      /* the last orientation object; retained */
static int currentOrientation = ORIENTATION_UNKNOWN;
static bool threadStarted;

/* (no ARC here: the object is retained for the block, and given back after the handler has run) */
static void
deliver(id queue, handler_t handler, id orientation)
{
	id kept = SEND0(id, orientation, "retain");
	void (^call)(void) = ^{
		handler(kept, NULL);
		SEND0(void, kept, "release");
	};

	if (queue != NULL) {
		((void (*)(id, SEL, id))objc_msgSend)(queue, sel_registerName("addOperationWithBlock:"), (id)call);
		return;
	}
	dispatch_async(dispatch_get_main_queue(), call);
}

static void
announce(int orientation)
{
	id object = makeOrientation(orientation);

	if (object == NULL) {
		return;
	}
	pthread_mutex_lock(&lock);
	if (current != NULL) {
		SEND0(void, current, "release");
	}
	current = object;
	currentOrientation = orientation;
	for (size_t i = 0; i < sizeof(clients) / sizeof(clients[0]); i++) {
		if (clients[i].handler != NULL) {
			deliver(clients[i].queue, clients[i].handler, object);
		}
	}
	pthread_mutex_unlock(&lock);
}

static void *
pollThread(__unused void *argument)
{
	int candidate = ORIENTATION_UNKNOWN, stable = 0, said = 0;
	uint32_t lastCount = 0;

	for (;;) {
		struct dlisa_sensors s;

		usleep(100000);
		if (!readSensors(&s) || s.source == 0 || s.count[SENSOR_ACCEL] == lastCount) {
			continue;
		}
		lastCount = s.count[SENSOR_ACCEL];
		{
			float x = asFloat(s.accel[0]), y = asFloat(s.accel[1]), z = asFloat(s.accel[2]);
			int next = orientationFor(x, y, z, currentOrientation);

			if (said < 3) {
				said++;
				say("dlisa-motion: gravity %.2f %.2f %.2f m/s2 (%s) -> %s\n", x, y, z, s.source == 2 ? "made up" : "ADSP",
				    orientationNames[next]);
			}
			stable = next == candidate ? stable + 1 : 0;
			candidate = next;
			/* three readings in a row (300 ms), so that a shake does not turn the screen */
			if (stable >= 2 && next != currentOrientation && next != ORIENTATION_UNKNOWN) {
				say("dlisa-motion: the device is now %s (gravity %.2f %.2f %.2f)\n", orientationNames[next], x, y, z);
				announce(next);
			}
		}
	}
	return NULL;
}

static void
startThread(void)
{
	pthread_t thread;

	if (!threadStarted && pthread_create(&thread, NULL, pollThread, NULL) == 0) {
		threadStarted = true;
		pthread_detach(thread);
	}
}

static void
addClient(id manager, id queue, handler_t handler, const char *how)
{
	handler_t kept = Block_copy(handler);
	bool added = false;

	pthread_mutex_lock(&lock);
	for (size_t i = 0; i < sizeof(clients) / sizeof(clients[0]) && !added; i++) {
		if (clients[i].handler == NULL || clients[i].manager == manager) {
			if (clients[i].handler != NULL) {
				Block_release(clients[i].handler);
			}
			clients[i].manager = manager;
			clients[i].queue = queue != NULL ? SEND0(id, queue, "retain") : NULL;
			clients[i].handler = kept;
			added = true;
		}
	}
	if (added && current != NULL) {
		deliver(queue, kept, current);  /* (the orientation there is now, at once) */
	}
	pthread_mutex_unlock(&lock);
	say("dlisa-motion: %s: backboardd asks for device orientation updates (%s)\n", how, added ? "kept" : "NO ROOM");
	startThread();
}

static void
startUpdates(id self, __unused SEL selector, id queue, handler_t handler)
{
	addClient(self, queue, handler, "start");
}

static void
startUpdatesPrivate(id self, __unused SEL selector, id queue, handler_t handler)
{
	addClient(self, queue, handler, "start (private)");
}

static void
stopUpdates(id self, __unused SEL selector)
{
	pthread_mutex_lock(&lock);
	for (size_t i = 0; i < sizeof(clients) / sizeof(clients[0]); i++) {
		if (clients[i].manager == self && clients[i].handler != NULL) {
			Block_release(clients[i].handler);
			clients[i].handler = NULL;
			clients[i].manager = NULL;
		}
	}
	pthread_mutex_unlock(&lock);
}

static id
currentOrientationObject(__unused id self, __unused SEL selector)
{
	id object;

	pthread_mutex_lock(&lock);
	object = current != NULL ? SEND0(id, SEND0(id, current, "retain"), "autorelease") : NULL;
	pthread_mutex_unlock(&lock);
	return object;
}

static bool
available(__unused id self, __unused SEL selector)
{
	return true;
}

/* kind '-': an instance method, '+': a class method */
static void
replace(char kind, const char *selectorName, IMP replacement)
{
	Class cls = objc_getClass("CMDeviceOrientationManager");
	SEL selector = sel_registerName(selectorName);
	Method method = cls == NULL ? NULL : kind == '+' ? class_getClassMethod(cls, selector) : class_getInstanceMethod(cls, selector);

	if (method == NULL) {
		say("dlisa-motion: %c[CMDeviceOrientationManager %s] not found; left alone\n", kind, selectorName);
		return;
	}
	method_setImplementation(method, replacement);
	say("dlisa-motion: %c[CMDeviceOrientationManager %s] replaced\n", kind, selectorName);
}

__attribute__((constructor)) static void
dlisaMotionInit(void)
{
	struct dlisa_sensors s;

	if (strcmp(getprogname(), "backboardd") != 0) {
		return;
	}
	if (!readSensors(&s)) {
		say("dlisa-motion: no kern.hakios.sensors in this kernel (or a different structure): the screen does not rotate\n");
		return;
	}
	dlopen("/System/Library/Frameworks/CoreMotion.framework/CoreMotion", RTLD_NOW);
	if (objc_getClass("CMDeviceOrientationManager") == NULL) {
		say("dlisa-motion: CoreMotion has no CMDeviceOrientationManager: left alone\n");
		return;
	}
	replace('-', "startDeviceOrientationUpdatesToQueue:withHandler:", (IMP)startUpdates);
	replace('-', "startDeviceOrientationUpdatesPrivateToQueue:withHandler:", (IMP)startUpdatesPrivate);
	replace('-', "stopDeviceOrientationUpdates", (IMP)stopUpdates);
	replace('-', "stopDeviceOrientationUpdatesPrivate", (IMP)stopUpdates);
	replace('-', "deviceOrientationBlocking", (IMP)currentOrientationObject);
	replace('-', "deviceOrientation", (IMP)currentOrientationObject);
	replace('+', "isDeviceOrientationAvailable", (IMP)available);
	replace('-', "isDeviceOrientationAvailable", (IMP)available);
	say("dlisa-motion: the device orientation comes from the kernel's sensors (source %u, found %#x)\n", s.source, s.found);
}
