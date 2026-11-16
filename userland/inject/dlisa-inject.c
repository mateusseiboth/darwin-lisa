/*
 * libdlisa.dylib: code of ours inside stock iOS 26 daemons (the iOS 26 counterpart of HakiOS's libhakios.dylib).
 *
 * The kernel starts the programs named in /etc/dlisa-inject.conf with DYLD_INSERT_LIBRARIES pointing here
 * (sysctl kern.exec_inject, set by init26; kernel/execaids). What is done depends on the program; each fix is
 * one entry of FIXES below.
 *
 *   backboardd
 *       The HID event server. With no Apple multitouch driver it has no touch screen: a virtual HID service made
 *       from inside it turns the samples of /dev/hakios-touch into digitizer events (HakiOS 16.5's way). A lone
 *       contact is always path 1 / identity 2, whatever its finger number (touch-slots.c). Boot-arg dlisa_scroll=SECS
 *       drags a list up and down like a finger from SECS seconds of uptime (scroll-stroke.c).
 *
 *   Preferences, SpringBoard
 *       Boot-arg dlisa_uiprobe=NAME,...: touches the app is handed, its main run loop, a display link of its own and
 *       backtraces of a stuck main thread, every 2 s (startUiProbe()).
 *
 *   every program it is in
 *       Boot-arg dlisa_launchprobe=NAME,...: the main thread's frames at 1.5, 4, 7 and 9.5 s after the start, for the
 *       extensions chronod's watchdog kills at 10 s (launch-probe.c, startLaunchProbe()).
 *
 *   SpringBoard
 *       iOS 26 decides at start whether the Setup Assistant has to run (BYSetupAssistantNeedsToRun: nothing in a
 *       new /private/var says it ran) and whether the device is "bricked" (not activated: mobileactivationd has no
 *       Secure Enclave or Apple server to answer). Either one puts SpringBoard in setup mode, which refuses the
 *       home pans (the swipe up from the home bar springs back), app launches and the unlock. HakiOS 16.5 had the
 *       same two fixes: setup is never required and the device counts as activated. Then, with boot-args:
 *       dlisa_unlock=1 unlocks once at boot (=2: again whenever it locks), dlisa_noidle=1 holds an idle timer
 *       assertion (no auto-lock or dimming: Settings' "Auto-Lock: Never"), dlisa_watchdog=N scales FrontBoard's
 *       launch watchdog by N (apps are slow to start here). App snapshots are JPEG/PNG, not ASTC (HakiOS 16.5's fix).
 *       The status bar: the display context's notch is the one the split bar of the display's width wants
 *       (status-bar.c; dlisa_sbprov=0 leaves it), and 30 s in, two "screen:" lines say what UIKit chooses from
 *       (artwork, home button type, SpringBoard's screen type, UIScreen's sizes and notch, UIKit's own pick).
 *
 *   every program with UIKit
 *       +[_UIStatusBarVisualProvider_Split visualProviderSubclassForScreen:...] answers the split bar of the display's
 *       width when UIKit found none (no "rdar:45025538" status bar).
 *
 *   containermanagerd (both of them)
 *       It asks the user manager for the "personas" of the user. This port has no data volume, no keybag and
 *       no Secure Enclave, so the persona usermanagerd --init writes is not one containermanagerd can resolve a
 *       container for: the user container lookup lsd makes at every start (class 10, persona "primary") fails
 *       and lsd aborts ("Could not get user container URL! Must abort."), and runningboardd, backboardd and
 *       SpringBoard wait on lsd. Told that personas are not supported, containermanagerd answers with the
 *       POSIX user's home (/var/mobile), as HakiOS did on iOS 16.5.
 *
 *   PosterBoard
 *       Its poster data store (wallpaper, lock screen and Notification Center clock) wants every store directory
 *       to read NSURLFileProtectionKey = NSURLFileProtectionNone, and the HFSX data volume has no content
 *       protection, so the key reads nil and the 59 to 61 migration never gets past 59 (file-protection.c). On a
 *       volume without MNT_CPROTECT a nil class reads as NSURLFileProtectionNone. Then every new poster
 *       descriptor's store coordinator unmarks its files as purgeable, an APFS-only fsctl that fails on HFS, and
 *       no descriptor is made (purgeable.c): on a volume that is not APFS the purgeable marks succeed as no-ops.
 *       Last, donotdisturbd never answers here, and Focus's synchronous queries to it (FCActivityManager, under
 *       the data store's state lock in the first role coordinator setup) hung PosterBoard before it made the
 *       default poster: those queries get a deadline and answer "no Focus" past it (bounded-call.c).
 *
 *   CollectionsPoster
 *       The same purgeable marks (PosterFoundation's), as no-ops on a volume that is not APFS.
 *
 * Only Objective-C runtime calls: nothing here depends on addresses inside the dyld shared cache.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include "bounded-call.c"
#include "bt-virtual.c"
#include "file-protection.c"
#include "launch-probe.c"
#include "name-list.c"
#include "purgeable.c"
#include "scroll-stroke.c"
#include "status-bar.c"
#include "torch-level.c"
#include "touch-slots.c"
#include "trait-names.c"

_Static_assert(DLISA_MNT_CPROTECT == MNT_CPROTECT, "file-protection.c's MNT_CPROTECT is <sys/mount.h>'s");

extern const char *getprogname(void);

static void
say(const char *format, ...)
{
	char line[640];
	va_list ap;
	int n;
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY);

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(fd >= 0 ? fd : 2, line, (size_t)n);
	if (fd >= 0) {
		close(fd);
	}
}

/* Replace a class method; true when it was there to replace. */
static bool
replaceClassMethod(const char *className, const char *selectorName, IMP replacement)
{
	Class cls = objc_getClass(className);
	Method method = cls ? class_getClassMethod(cls, sel_registerName(selectorName)) : NULL;

	if (method == NULL) {
		say("dlisa[%s]: +[%s %s] not found; left alone\n", getprogname(), className, selectorName);
		return false;
	}
	method_setImplementation(method, replacement);
	say("dlisa[%s]: +[%s %s] replaced\n", getprogname(), className, selectorName);
	return true;
}

/* ---- containermanagerd ---------------------------------------------------------------------------- */

static bool
personasAreNotSupported(__unused id self, __unused SEL selector)
{
	return false;
}

/*
 * What happens to a client's request (lsd's user container lookup came back XPC_INTERRUPTED_REPLY): each step of the
 * dispatcher says what it got, and a refusal says its error. Diagnostics, on the console.
 */
static IMP origDispatch, origCommandClass, origClientIdentity, origSetup;

static const char *
describe(id object)
{
	id text;

	if (object == nil) {
		return "nil";
	}
	text = ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("description"));
	return text != nil ? ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String")) : "?";
}

static void
sayDispatch(id self, SEL selector, id message, id context, id reply)
{
	say("dlisa[%s]: dispatch message %p\n", getprogname(), message);
	((void (*)(id, SEL, id, id, id))origDispatch)(self, selector, message, context, reply);
	say("dlisa[%s]: dispatch message %p done\n", getprogname(), message);
}

static Class
sayCommandClass(id self, SEL selector, id message, id context, id *error)
{
	Class found = ((Class (*)(id, SEL, id, id, id *))origCommandClass)(self, selector, message, context, error);

	say("dlisa[%s]: command class %s%s%s\n", getprogname(), found ? class_getName(found) : "nil",
	    found ? "" : ", error ", found ? "" : describe(error ? *error : nil));
	return found;
}

static id
sayClientIdentity(id self, SEL selector, id context, id message, id *error)
{
	id identity = ((id (*)(id, SEL, id, id, id *))origClientIdentity)(self, selector, context, message, error);

	if (identity == nil) {
		say("dlisa[%s]: client identity refused: %s\n", getprogname(), describe(error ? *error : nil));
	}
	return identity;
}

static void
saySetup(id self, SEL selector)
{
	say("dlisa[%s]: containerManagerSetup\n", getprogname());
	((void (*)(id, SEL))origSetup)(self, selector);
	say("dlisa[%s]: containerManagerSetup done\n", getprogname());
}

static IMP
replaceInstanceMethod(const char *className, const char *selectorName, IMP replacement)
{
	Class cls = objc_getClass(className);
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName(selectorName)) : NULL;

	if (method == NULL) {
		say("dlisa[%s]: -[%s %s] not found\n", getprogname(), className, selectorName);
		return NULL;
	}
	return method_setImplementation(method, replacement);
}

/* darwin-lisa, boot-arg dlisa_personas=1: containermanagerd keeps Apple's answer to personasAreSupported. The
 * replacement below (NO) dates from the chase of lsd's user-container lookup, before the XPC check-in bug was found;
 * with it containermanagerd believes there are no personas while CoreServices still asks usermanagerd for the persona
 * list, and MobileInstallation registered only the applications that need no data container (2026-11-11). */
static bool
isBootArgSet(const char *what)
{
	static char arguments[1024];
	size_t size = sizeof(arguments) - 1;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return false;
	}
	arguments[size] = 0;
	return strstr(arguments, what) != NULL;
}

static void
fixContainerManager(void)
{
	if (isBootArgSet("dlisa_personas=1")) {
		say("dlisa[%s]: personasAreSupported left to Apple (dlisa_personas=1)\n", getprogname());
	} else {
		replaceClassMethod("MCMUserIdentityCache", "personasAreSupported", (IMP)personasAreNotSupported);
	}
	origDispatch = replaceInstanceMethod("MCMCommandDispatcher", "dispatchMessage:context:reply:", (IMP)sayDispatch);
	origCommandClass = replaceInstanceMethod("MCMCommandDispatcher", "_commandClassFromXPCMessage:context:error:",
	    (IMP)sayCommandClass);
	origClientIdentity = replaceInstanceMethod("MCMClientFactoryWithXPC",
	    "clientIdentityWithClientMessageContext:xpcMessage:error:", (IMP)sayClientIdentity);
	origSetup = replaceInstanceMethod("MCMClientConnection", "containerManagerSetup", (IMP)saySetup);
}

/* ---- lsd: the user container lookup ---------------------------------------------------------------- */

/*
 * lsd asks for its user container at start (CoreServices userContainerURL: class 10, CONTAINER_PERSONA_PRIMARY,
 * flags 0x900000001) and aborts when containermanagerd says no; the reason only goes to os_log. CoreServices
 * branches straight into libsystem_containermanager (no GOT to rebind, and interposing does not reach calls inside
 * the shared cache), so the same lookup is made here first, from inside lsd (its entitlements), and what
 * containermanagerd answers is printed on the console: with the primary persona, and without any.
 */
typedef void *container_query_t;
typedef void *container_object_t;
typedef void *container_error_t;

extern const char *CONTAINER_PERSONA_PRIMARY;
extern container_query_t container_query_create(void);
extern void container_query_free(container_query_t query);
extern void container_query_set_class(container_query_t query, uint64_t containerClass);
extern void container_query_set_persona_unique_string(container_query_t query, const char *persona);
extern void container_query_operation_set_flags(container_query_t query, uint64_t flags);
extern container_object_t container_query_get_single_result(container_query_t query);
extern container_error_t container_query_get_last_error(container_query_t query);
extern char *container_error_copy_unlocalized_description(container_error_t error);
extern const char *container_get_path(container_object_t container);

static void
probeUserContainer(const char *persona)
{
	container_query_t query = container_query_create();
	container_object_t result;
	container_error_t error;
	char *text;

	container_query_set_class(query, 10);
	if (persona != NULL) {
		container_query_set_persona_unique_string(query, persona);
	}
	container_query_operation_set_flags(query, 0x900000001ULL);
	result = container_query_get_single_result(query);
	if (result != NULL) {
		say("dlisa[%s]: user container (persona %s): %s\n", getprogname(), persona ? persona : "none",
		    container_get_path(result));
		container_query_free(query);
		return;
	}
	error = container_query_get_last_error(query);
	text = error != NULL ? container_error_copy_unlocalized_description(error) : NULL;
	say("dlisa[%s]: user container (persona %s) refused: %s\n", getprogname(), persona ? persona : "none",
	    text != NULL ? text : "(no error)");
	free(text);
	container_query_free(query);
}

static void
fixLaunchServices(void)
{
	probeUserContainer(CONTAINER_PERSONA_PRIMARY);
	probeUserContainer(NULL);
}

/* ---- backboardd: the touch screen ----------------------------------------------------------------------- */

/*
 * backboardd is the HID event server, and without Apple's multitouch driver it has no touch screen. We give it one,
 * as HakiOS did on 16.5 (userland/src/inject.c): a "virtual" HID service, created from inside backboardd, that turns
 * the samples of /dev/hakios-touch (the Goodix driver, or the kernel's touch_synth test hook) into the digitizer
 * events Apple's driver would have sent. The IOHID functions are private (no SDK header or stub), so they are looked
 * up at run time; all of them are still exported by iOS 26's IOKit.
 */
/* sys/hakios.input.h (kernel/drivers/sys): one sample of /dev/hakios-touch */
struct hakiosTouch {
	uint64_t time;                  /* mach_absolute_time() when the kernel took it */
	uint32_t type;                  /* 1 down, 2 move, 3 up, 4/5 a button down/up */
	uint32_t finger;
	uint32_t x, y;                  /* 0..65535, 0 = left/top */
};
#define TOUCH_DEVICE    "/dev/hakios-touch"
#define TOUCH_DOWN      1
#define TOUCH_MOVE      2
#define TOUCH_UP        3
#define BUTTON_DOWN     4
#define BUTTON_LOCK     1
#define BUTTON_VOLUME_UP 2
#define BUTTON_VOLUME_DOWN 3
#define BUTTON_HOME     4

typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

/* IOHIDVirtualServiceClientCallbacksV2 */
struct virtualServiceCallbacks {
	uint64_t version;               /* 2 */
	void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
	Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
	CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
	IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching,
	    uint32_t options);
	int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
	CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};

static struct {
	IOHIDEventSystemClientRef (*clientCreateWithType)(CFAllocatorRef, int, CFDictionaryRef);
	void (*clientScheduleWithDispatchQueue)(IOHIDEventSystemClientRef, dispatch_queue_t);
	IOHIDServiceClientRef (*virtualCreate)(IOHIDEventSystemClientRef, CFDictionaryRef, const struct virtualServiceCallbacks *,
	    void *, void *);
	Boolean (*virtualRemove)(IOHIDServiceClientRef);
	Boolean (*virtualDispatch)(IOHIDServiceClientRef, IOHIDEventRef);
	IOHIDEventRef (*createDigitizer)(CFAllocatorRef, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, double, double,
	    double, double, double, double, Boolean, Boolean, uint32_t);
	IOHIDEventRef (*createFinger)(CFAllocatorRef, uint64_t, uint32_t, uint32_t, uint32_t, double, double, double, double, double,
	    Boolean, Boolean, uint32_t);
	IOHIDEventRef (*createKeyboard)(CFAllocatorRef, uint64_t, uint32_t, uint32_t, Boolean, uint32_t);
	void (*appendEvent)(IOHIDEventRef, IOHIDEventRef, uint32_t);
	void (*setInteger)(IOHIDEventRef, uint32_t, CFIndex);
} hid;

static bool
findHidFunctions(void)
{
	static const struct {
		const char *name;
		void **slot;
	} wanted[] = {
		{ "IOHIDEventSystemClientCreateWithType", (void **)&hid.clientCreateWithType },
		{ "IOHIDEventSystemClientScheduleWithDispatchQueue", (void **)&hid.clientScheduleWithDispatchQueue },
		{ "IOHIDVirtualServiceClientCreateWithCallbacks", (void **)&hid.virtualCreate },
		{ "IOHIDVirtualServiceClientRemove", (void **)&hid.virtualRemove },
		{ "IOHIDVirtualServiceClientDispatchEvent", (void **)&hid.virtualDispatch },
		{ "IOHIDEventCreateDigitizerEvent", (void **)&hid.createDigitizer },
		{ "IOHIDEventCreateDigitizerFingerEvent", (void **)&hid.createFinger },
		{ "IOHIDEventCreateKeyboardEvent", (void **)&hid.createKeyboard },
		{ "IOHIDEventAppendEvent", (void **)&hid.appendEvent },
		{ "IOHIDEventSetIntegerValue", (void **)&hid.setInteger },
	};
	void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);

	for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
		*wanted[i].slot = dlsym(iokit != NULL ? iokit : RTLD_DEFAULT, wanted[i].name);
		if (*wanted[i].slot == NULL) {
			say("dlisa[%s]: input: %s not found\n", getprogname(), wanted[i].name);
			return false;
		}
	}
	return true;
}

#define HID_CLIENT_SIMPLE               4       /* a client that only sends */
#define HID_CLIENT_RATE_CONTROLLED      3       /* what IOHIDEventSystemClientCreate() makes */
#define HID_PAGE_DIGITIZER              0x0d
#define HID_USAGE_TOUCH_SCREEN          0x04
#define HID_PAGE_CONSUMER               0x0c
#define HID_USAGE_CONSUMER_CONTROL      0x01
#define HID_USAGE_POWER                 0x30
#define HID_USAGE_MENU                  0x40
#define HID_USAGE_VOLUME_UP             0xe9
#define HID_USAGE_VOLUME_DOWN           0xea
#define DIGITIZER_HAND                  3       /* kIOHIDDigitizerTransducerTypeHand */
#define DIGITIZER_RANGE                 0x1     /* event mask: what has changed */
#define DIGITIZER_TOUCH                 0x2
#define DIGITIZER_POSITION              0x4
#define DIGITIZER_FROM_EDGE_TIP         0x800   /* this finger came in over an edge of the screen */
#define FIELD_IS_BUILT_IN               0x4
#define FIELD_DIGITIZER_DISPLAY_INTEGRATED 0xb0019
#define TOUCH_MODE_WAKE                 0xfe    /* "TouchDetectionMode": the display is off, a tap is to wake it */

#define MAX_FINGERS     TOUCH_FINGERS

/*
 * A finger that lands this close to an edge (a fraction of the width or height) came in over that edge. Apple's
 * controller tells that from the contact's shape; UIKit's screen-edge gestures (the swipe up from the home bar that
 * opens the phone) take no touch that is not so marked.
 */
#define EDGE_SIDE       (16.0 / 375)
#define EDGE_TOP        (16.0 / 812)
#define EDGE_BOTTOM     (34.0 / 812)

static struct {
	bool touching, edge;
	double x, y;
} fingers[MAX_FINGERS];

static volatile int touchMode;

/* the path slot of each contact (touch-slots.c); fingers[] and slots are the input thread's and dlisa_scroll's */
static struct touchSlots slots;
static pthread_mutex_t touchLock = PTHREAD_MUTEX_INITIALIZER;

struct hidService {
	CFMutableDictionaryRef properties;
	volatile bool opened;
};

static CFNumberRef
cfNumber(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

static const char *
cfText(CFTypeRef value, char *buffer, size_t size)
{
	CFStringRef description = value != NULL ? CFCopyDescription(value) : NULL;

	snprintf(buffer, size, "?");
	if (description != NULL) {
		CFStringGetCString(description, buffer, (CFIndex)size, kCFStringEncodingUTF8);
		CFRelease(description);
	}
	return buffer;
}

static CFTypeRef
serviceCopyProperty(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
	static CFMutableSetRef asked;
	CFTypeRef value = CFDictionaryGetValue(((struct hidService *)refcon)->properties, key);

	if (asked == NULL) {
		asked = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (value == NULL && !CFSetContainsValue(asked, key) && CFSetGetCount(asked) < 40) {
		char name[96] = "?";

		CFSetAddValue(asked, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		say("dlisa[%s]: input: the event server asks for \"%s\", which we do not have\n", getprogname(), name);
	}
	return value != NULL ? CFRetain(value) : NULL;
}

static Boolean
serviceSetProperty(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value)
{
	static CFMutableSetRef told;
	char name[96] = "?", text[160];

	if (key == NULL) {
		return true;
	}
	/* the last thing the event server does when it opens a service; see makeService() */
	if (value != NULL && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) && CFGetTypeID(value) == CFBooleanGetTypeID() &&
	    CFBooleanGetValue(value)) {
		((struct hidService *)refcon)->opened = true;
	}
	if (value != NULL && CFEqual(key, CFSTR("TouchDetectionMode")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
		int mode = 0;

		CFNumberGetValue(value, kCFNumberIntType, &mode);
		if (mode != touchMode) {
			say("dlisa[%s]: input: touch detection mode %#x\n", getprogname(), mode);
		}
		touchMode = mode;
	}
	if (told == NULL) {
		told = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (!CFSetContainsValue(told, key) && CFSetGetCount(told) < 24) {
		CFSetAddValue(told, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		say("dlisa[%s]: input: the event server sets \"%s\" = %s\n", getprogname(), name, cfText(value, text, sizeof(text)));
	}
	return true;
}

static void
serviceNotification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service, __unused uint32_t type,
    __unused CFDictionaryRef property)
{
}

static const struct virtualServiceCallbacks serviceCallbacks = {
	.version = 2, .notification = serviceNotification, .setProperty = serviceSetProperty, .copyProperty = serviceCopyProperty,
};

/*
 * A built-in HID service of ours inside the event server (this very program, still starting). A client that reaches
 * it at the wrong moment is left with a service the server never opens ("still waiting for a main screen
 * digitizer"), and when that moment comes depends on the machine's speed, so the service is made again until the
 * server has opened it.
 */
static IOHIDServiceClientRef
makeService(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display)
{
	struct hidService *made = calloc(1, sizeof(*made));
	IOHIDServiceClientRef service = NULL;
	CFMutableDictionaryRef properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
	CFNumberRef pageNumber = cfNumber(page), usageNumber = cfNumber(usage), zero = cfNumber(0);
	CFArrayRef pairs;

	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), pageNumber);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usageNumber);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsagePage"), pageNumber);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsage"), usageNumber);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePage"), pageNumber);
	CFDictionarySetValue(properties, CFSTR("DeviceUsage"), usageNumber);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(properties, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("Transport"), CFSTR("SPI"));
	CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("darwin-lisa"));
	CFDictionarySetValue(properties, CFSTR("Product"), name);
	CFDictionarySetValue(properties, CFSTR("VendorID"), zero);
	CFDictionarySetValue(properties, CFSTR("ProductID"), zero);
	if (display) {
		CFDictionarySetValue(properties, CFSTR("DisplayIntegrated"), kCFBooleanTrue);
	}
	made->properties = properties;  /* stays: serviceCopyProperty() answers from it */
	for (int attempt = 1; attempt <= 20; attempt++) {
		made->opened = false;
		service = hid.virtualCreate(client, properties, &serviceCallbacks, NULL, made);
		if (service == NULL) {
			break;
		}
		for (int tenths = 0; tenths < 20 && !made->opened; tenths++) {
			usleep(100000);
		}
		if (made->opened) {
			say("dlisa[%s]: input: \"%s\" opened by the event server (attempt %d)\n", getprogname(), product, attempt);
			break;
		}
		say("dlisa[%s]: input: the event server has not opened \"%s\" (attempt %d): making it again\n", getprogname(), product,
		    attempt);
		hid.virtualRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

/* One frame of the touch screen: the hand, and under it every finger that is down or has just lifted. */
static void
sendTouch(IOHIDServiceClientRef screen, const struct hakiosTouch *t)
{
	uint32_t change = t->type == TOUCH_MOVE ? DIGITIZER_POSITION : DIGITIZER_RANGE | DIGITIZER_TOUCH;
	IOHIDEventRef hand, finger;
	bool any = false;
	unsigned f = t->finger;

	if (f >= MAX_FINGERS || (t->type != TOUCH_DOWN && !fingers[f].touching)) {
		return;
	}
	if (t->type == TOUCH_DOWN) {
		takeTouchSlot(&slots, f);
	}
	fingers[f].touching = t->type != TOUCH_UP;
	fingers[f].x = t->x / 65535.0;
	fingers[f].y = t->y / 65535.0;
	if (t->type == TOUCH_DOWN) {
		fingers[f].edge = fingers[f].x < EDGE_SIDE || fingers[f].x > 1 - EDGE_SIDE || fingers[f].y < EDGE_TOP ||
		    fingers[f].y > 1 - EDGE_BOTTOM;
	}
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		any |= fingers[i].touching;
	}
	hand = hid.createDigitizer(NULL, t->time, DIGITIZER_HAND, 0, 0, change & ~DIGITIZER_RANGE, 0, fingers[f].x, fingers[f].y, 0, 0,
	    0, 0, any, any, 0);
	hid.setInteger(hand, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
	hid.setInteger(hand, FIELD_IS_BUILT_IN, 1);
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		if (!fingers[i].touching && i != f) {
			continue;
		}
		int slot = findTouchSlot(&slots, i);

		finger = hid.createFinger(NULL, t->time, touchPathIndex(slot), touchPathIdentity(slot),
		    (i == f ? change : 0) | (fingers[i].edge ? DIGITIZER_FROM_EDGE_TIP : 0), fingers[i].x, fingers[i].y, 0,
		    fingers[i].touching ? 1.0 : 0.0, 0, fingers[i].touching, fingers[i].touching, 0);
		hid.setInteger(finger, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
		hid.appendEvent(hand, finger, 0);
		CFRelease(finger);
	}
	hid.virtualDispatch(screen, hand);
	CFRelease(hand);
	if (t->type == TOUCH_UP) {
		releaseTouchSlot(&slots, f);
	}
}

static void
sendTouchLocked(IOHIDServiceClientRef screen, const struct hakiosTouch *t)
{
	pthread_mutex_lock(&touchLock);
	sendTouch(screen, t);
	pthread_mutex_unlock(&touchLock);
}

static void
sendButton(IOHIDServiceClientRef buttons, const struct hakiosTouch *t)
{
	static const uint32_t usages[] = { [BUTTON_LOCK] = HID_USAGE_POWER, [BUTTON_HOME] = HID_USAGE_MENU,
		                           [BUTTON_VOLUME_UP] = HID_USAGE_VOLUME_UP, [BUTTON_VOLUME_DOWN] = HID_USAGE_VOLUME_DOWN };
	IOHIDEventRef event;

	if (t->finger >= sizeof(usages) / sizeof(usages[0]) || usages[t->finger] == 0) {
		return;
	}
	event = hid.createKeyboard(NULL, t->time, HID_PAGE_CONSUMER, usages[t->finger], t->type == BUTTON_DOWN, 0);
	hid.setInteger(event, FIELD_IS_BUILT_IN, 1);
	hid.virtualDispatch(buttons, event);
	CFRelease(event);
}

/* The display is off and a tap is to wake it: the lock button wakes it; the rest of that touch is kept back. */
static bool
isWakingTap(IOHIDServiceClientRef buttons, const struct hakiosTouch *t, unsigned *waking)
{
	struct hakiosTouch press = { .time = t->time, .type = BUTTON_DOWN, .finger = BUTTON_LOCK };
	unsigned bit = 1u << (t->finger & 31);

	if (*waking & bit) {
		*waking &= t->type == TOUCH_UP ? ~bit : ~0u;
		return true;
	}
	if (t->type != TOUCH_DOWN || (touchMode & 0xff) != TOUCH_MODE_WAKE || buttons == NULL) {
		return false;
	}
	sendButton(buttons, &press);
	press.type = BUTTON_DOWN + 1;
	sendButton(buttons, &press);
	*waking |= bit;
	return true;
}

static long bootArgNumber(const char *name);

static uint64_t
uptimeMillis(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
}

/*
 * Boot-arg dlisa_scroll=SECS: from SECS seconds of uptime (the kernel log's clock), dlisa_scroll_n=N strokes (default
 * 8, about 17 s) of a finger dragging the list in the middle of the screen up and down, at the panel's rate and held
 * before each lift (scroll-stroke.c). Sent as finger 8 through the same service as the panel's samples, so it is a
 * real finger to the event server; touch_synth (finger 9) and a hand on the glass should keep out of its window.
 */
#define SCROLL_FINGER   8
#define SCROLL_PAUSE_MS 400

static void
sendStroke(IOHIDServiceClientRef screen, unsigned index)
{
	struct strokePlan plan = buildStrokePlan(index);
	size_t count = strokeSampleCount(&plan);

	for (size_t k = 0; k < count; k++) {
		struct strokeSample s = strokeSampleAt(&plan, k);
		struct hakiosTouch t = { .time = mach_absolute_time(), .type = s.type, .finger = SCROLL_FINGER, .x = s.x, .y = s.y };

		sendTouchLocked(screen, &t);
		usleep(STROKE_SAMPLE_MS * 1000);
	}
}

static void *
scrollThread(void *argument)
{
	IOHIDServiceClientRef screen = argument;
	uint64_t start = (uint64_t)bootArgNumber("dlisa_scroll") * 1000, begun;
	long strokes = bootArgNumber("dlisa_scroll_n");

	pthread_setname_np("dlisa scroll");
	strokes = strokes > 0 && strokes <= 200 ? strokes : 8;
	while (uptimeMillis() < start) {
		usleep(100000);
	}
	begun = uptimeMillis();
	say("dlisa[%s]: scroll: %ld strokes from uptime %llu.%03llu s\n", getprogname(), strokes, begun / 1000, begun % 1000);
	for (long i = 0; i < strokes; i++) {
		sendStroke(screen, (unsigned)i);
		usleep(SCROLL_PAUSE_MS * 1000);
	}
	say("dlisa[%s]: scroll: %ld strokes from uptime %llu.%03llu to %llu.%03llu s (%u moves %u ms apart, held %u ms, "
	    "each)\n", getprogname(), strokes, begun / 1000, begun % 1000, uptimeMillis() / 1000, uptimeMillis() % 1000,
	    STROKE_MOVES, STROKE_SAMPLE_MS, STROKE_HELD * STROKE_SAMPLE_MS);
	return NULL;
}

static void
startScriptedScroll(IOHIDServiceClientRef screen)
{
	pthread_t thread;

	if (bootArgNumber("dlisa_scroll") <= 0) {
		return;
	}
	if (pthread_create(&thread, NULL, scrollThread, (void *)screen) == 0) {
		pthread_detach(thread);
	}
}

static void *
inputThread(__unused void *argument)
{
	static const int types[] = { HID_CLIENT_SIMPLE, HID_CLIENT_RATE_CONTROLLED };
	IOHIDEventSystemClientRef client = NULL;
	IOHIDServiceClientRef screen = NULL, buttons = NULL;
	struct hakiosTouch t;
	unsigned count = 0, waking = 0;
	int device;

	pthread_setname_np("dlisa input");
	initTouchSlots(&slots);         /* before any sample: a zeroed table would have every finger in slot 0 */
	if (!findHidFunctions()) {
		return NULL;
	}
	device = open(TOUCH_DEVICE, O_RDONLY);
	if (device < 0) {
		say("dlisa[%s]: input: %s: %s\n", getprogname(), TOUCH_DEVICE, strerror(errno));
		return NULL;
	}
	sleep(3);                       /* let backboardd get as far as starting its event server */
	for (size_t i = 0; screen == NULL && i < sizeof(types) / sizeof(types[0]); i++) {
		client = hid.clientCreateWithType(NULL, types[i], NULL);      /* waits until the server (this program) answers */
		if (client == NULL) {
			say("dlisa[%s]: input: no HID client of type %d\n", getprogname(), types[i]);
			continue;
		}
		hid.clientScheduleWithDispatchQueue(client, dispatch_queue_create("dlisa.input", NULL));
		screen = makeService(client, "darwin-lisa touch screen", HID_PAGE_DIGITIZER, HID_USAGE_TOUCH_SCREEN, true);
	}
	if (screen != NULL) {
		buttons = makeService(client, "darwin-lisa buttons", HID_PAGE_CONSUMER, HID_USAGE_CONSUMER_CONTROL, false);
	}
	say("dlisa[%s]: input: touch screen service %p, buttons %p\n", getprogname(), (void *)screen, (void *)buttons);
	if (screen == NULL) {
		return NULL;
	}
	startScriptedScroll(screen);
	while (read(device, &t, sizeof(t)) == (ssize_t)sizeof(t)) {
		if (count++ < 64) {
			say("dlisa[%s]: input: sample %u type %u finger %u at %u %u (mode %#x)\n", getprogname(), count, t.type, t.finger,
			    t.x, t.y, touchMode);
		}
		if (t.type > TOUCH_UP) {
			if (buttons != NULL) {
				sendButton(buttons, &t);
			}
			continue;
		}
		if (!isWakingTap(buttons, &t, &waking)) {
			sendTouchLocked(screen, &t);
		}
	}
	say("dlisa[%s]: input: %s: %s\n", getprogname(), TOUCH_DEVICE, strerror(errno));
	return NULL;
}

static void
startTouchScreen(void)
{
	pthread_t thread;

	if (pthread_create(&thread, NULL, inputThread, NULL) == 0) {
		pthread_detach(thread);
	}
}


/* ---- SpringBoard: setup mode, activation, unlock -------------------------------------------------------- */

#define SEND0(type, object, name)       ((type (*)(id, SEL))objc_msgSend)((id)(object), sel_registerName(name))

/* boot-arg NAME=VALUE (a number): the value, or 0 when it is not there */
static long
bootArgNumber(const char *name)
{
	static char arguments[1024];
	size_t size = sizeof(arguments) - 1, length = strlen(name);
	const char *at = arguments;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return 0;
	}
	arguments[size] = 0;
	while ((at = strstr(at, name)) != NULL) {
		if ((at == arguments || at[-1] == ' ') && at[length] == '=') {
			return strtol(at + length + 1, NULL, 0);
		}
		at += length;
	}
	return 0;
}

static IMP
replaceMethod(char kind, const char *className, const char *selectorName, IMP replacement)
{
	Class cls = objc_getClass(className);
	SEL selector = sel_registerName(selectorName);
	Method method = cls == NULL ? NULL : kind == '+' ? class_getClassMethod(cls, selector) : class_getInstanceMethod(cls, selector);

	if (method == NULL) {
		say("dlisa[%s]: %c[%s %s] not found; left alone\n", getprogname(), kind, className, selectorName);
		return NULL;
	}
	say("dlisa[%s]: %c[%s %s] replaced\n", getprogname(), kind, className, selectorName);
	return method_setImplementation(method, replacement);
}

/* -[SBSetupManager _setSetupRequiredReason:] (1 Setup Assistant, 2 bricked, 3 migration): 0, never in setup mode */
static void (*origSetSetupRequiredReason)(id, SEL, unsigned long);

static void
setSetupRequiredReason(id self, SEL selector, unsigned long reason)
{
	if (reason != 0) {
		say("dlisa[%s]: SpringBoard wants the Setup Assistant (reason %lu): not here\n", getprogname(), reason);
	}
	origSetSetupRequiredReason(self, selector, 0);
}

/* -[SBLockdownManager _setupActivationState] asks MobileActivation and keeps the answer in _state: 2 = activated */
#define LOCKDOWN_ACTIVATED      2

static void
setupActivationState(id self, __unused SEL selector)
{
	Ivar state = class_getInstanceVariable(object_getClass(self), "_state");

	if (state == NULL) {
		say("dlisa[%s]: SBLockdownManager has no _state; activation left alone\n", getprogname());
		return;
	}
	*(int *)((char *)self + ivar_getOffset(state)) = LOCKDOWN_ACTIVATED;
	say("dlisa[%s]: the device counts as activated\n", getprogname());
}

static bool
notBricked(__unused id self, __unused SEL selector)
{
	return false;
}

/* What the unlock path decides, on the console (the reasons SpringBoard gives otherwise go only to os_log). */
static bool (*origUnlockUIFromSource)(id, SEL, long, id);
static bool (*origIsUnlockDisabled)(id, SEL);

static bool
unlockUIFromSource(id self, SEL selector, long source, id options)
{
	bool done = origUnlockUIFromSource(self, selector, source, options);

	say("dlisa[%s]: unlock: unlockUIFromSource:%ld -> %d (still locked: %d)\n", getprogname(), source, done,
	    SEND0(bool, self, "isUILocked"));
	return done;
}

static bool
isUnlockDisabled(id self, SEL selector)
{
	static int said;
	bool disabled = origIsUnlockDisabled(self, selector);

	if (disabled && said++ < 8) {
		say("dlisa[%s]: unlock: SpringBoard says the unlock is disabled\n", getprogname());
	}
	return disabled;
}

/* FrontBoard's launch and resume watchdogs (a first-party app gets about 20 s to show its scene), times N */
static double (*origWatchdogScale)(id, SEL, id, bool);
static long watchdogScale;

static double
watchdogScalingFactor(id self, SEL selector, id appInfo, bool resume)
{
	return origWatchdogScale(self, selector, appInfo, resume) * (double)watchdogScale;
}

/*
 * dlisa_unlock=1: once SpringBoard is up, unlock the way the lock screen does after "swipe up to open"
 * (-[SBLockScreenManager lockScreenViewControllerRequestsUnlock]); if that is refused, unlockUIFromSource: with the
 * lock screen's source, and last _finishUIUnlockFromSource:withOptions:, which is what follows a successful
 * authentication. dlisa_unlock=2 keeps doing it whenever the phone locks again.
 */
#define UNLOCK_SOURCE_LOCK_SCREEN       0x19    /* what -lockScreenViewControllerRequestsUnlock passes on iOS 26 */
#define UNLOCK_FIRST_SECONDS            20
#define UNLOCK_EVERY_SECONDS            5

static long unlockMode;
static id idleAssertion;

static void
holdIdleTimerAssertion(void)
{
	Class coordinator = objc_getClass("SBIdleTimerGlobalCoordinator");
	id shared = coordinator != NULL ? SEND0(id, coordinator, "sharedInstanceIfExists") : nil;
	SEL acquire = sel_registerName("acquireIdleTimerDisableAssertionForReason:");

	if (idleAssertion != nil || shared == nil || class_getInstanceMethod(object_getClass(shared), acquire) == NULL) {
		return;
	}
	idleAssertion = ((id (*)(id, SEL, CFStringRef))objc_msgSend)(shared, acquire, CFSTR("darwin-lisa dlisa_noidle"));
	if (idleAssertion != nil) {
		CFRetain((CFTypeRef)idleAssertion);
		say("dlisa[%s]: idle timer: disabled (no auto-lock)\n", getprogname());
	}
}

static void
unlockPass(unsigned attempt)
{
	Class cls = objc_getClass("SBLockScreenManager");
	id manager = cls != NULL ? SEND0(id, cls, "sharedInstanceIfExists") : nil;
	unsigned next = attempt + 1;

	if (manager == nil) {
		next = attempt;                 /* SpringBoard is not that far yet */
	} else if (!SEND0(bool, manager, "isUILocked")) {
		if (attempt > 0) {
			say("dlisa[%s]: unlock: unlocked\n", getprogname());
		}
		if (unlockMode < 2) {
			return;
		}
		next = 0;                       /* dlisa_unlock=2: keep watching */
	} else if (attempt < 3) {
		say("dlisa[%s]: unlock: attempt %u: as the lock screen asks it\n", getprogname(), attempt + 1);
		SEND0(void, manager, "lockScreenViewControllerRequestsUnlock");
	} else if (attempt < 6) {
		say("dlisa[%s]: unlock: attempt %u: unlockUIFromSource\n", getprogname(), attempt + 1);
		((bool (*)(id, SEL, long, id))objc_msgSend)(manager, sel_registerName("unlockUIFromSource:withOptions:"),
		    UNLOCK_SOURCE_LOCK_SCREEN, nil);
	} else if (attempt == 6) {
		say("dlisa[%s]: unlock: attempt %u: finishing the unlock without asking\n", getprogname(), attempt + 1);
		((void (*)(id, SEL, long, id))objc_msgSend)(manager, sel_registerName("_finishUIUnlockFromSource:withOptions:"),
		    UNLOCK_SOURCE_LOCK_SCREEN, nil);
	} else if (attempt >= 60) {
		say("dlisa[%s]: unlock: still locked after %u attempts; giving up\n", getprogname(), attempt);
		return;
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, UNLOCK_EVERY_SECONDS * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		unlockPass(next);
	});
}

/* dlisa_noidle=1: the assertion, once SpringBoard's idle timer coordinator exists */
static void
idlePass(unsigned attempt)
{
	holdIdleTimerAssertion();
	if (idleAssertion != nil || attempt >= 60) {
		return;
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, UNLOCK_EVERY_SECONDS * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		idlePass(attempt + 1);
	});
}

/*
 * +[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:]: the picture SpringBoard keeps of an app that leaves
 * the screen. ATX (an ASTC texture) is compressed on the processor here and takes longer than the scene watchdog
 * (HakiOS 16.5 measured more than 10 s): JPEG for an opaque picture, PNG otherwise, as on a device without ASTC.
 */
#define SNAPSHOT_FORMAT_PNG     0
#define SNAPSHOT_FORMAT_JPEG    3

static long
snapshotOutputFormat(__unused id self, __unused SEL selector, id snapshot)
{
	SEL opaque = sel_registerName("isImageOpaque");

	if (snapshot != nil && class_getInstanceMethod(object_getClass(snapshot), opaque) != NULL &&
	    ((bool (*)(id, SEL))objc_msgSend)(snapshot, opaque)) {
		return SNAPSHOT_FORMAT_JPEG;
	}
	return SNAPSHOT_FORMAT_PNG;
}

/*
 * ForceNoBuddy in com.apple.purplebuddy.notbackedup makes BYSetupAssistantNeedsToRun() answer NO in every process
 * (SetupAssistant.framework checks it first). Written once, through cfprefsd, onto the data volume.
 */
static void
setForceNoBuddy(void)
{
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		CFStringRef domain = CFSTR("com.apple.purplebuddy.notbackedup");

		CFPreferencesSetAppValue(CFSTR("ForceNoBuddy"), kCFBooleanTrue, domain);
		say("dlisa[%s]: ForceNoBuddy: %s\n", getprogname(), CFPreferencesAppSynchronize(domain) ? "set" : "not saved");
	});
}

/*
 * -[CALayerHost setRendersAsynchronously:] gets NO. A layer host that renders asynchronously makes backboardd's
 * CoreAnimation render its context on the side into a surface of its own (CA::OGL::AsynchronousNode); with no GPU that
 * surface is not one the display's software context tracks, and backboardd aborts in CA::OGL::Context::push_surface
 * ("found != slist->end ()"): the screen freezes, backboardd and SpringBoard start again on the lock screen (seen
 * after the "Misconfigured Device" alert, 2026-11-10). Rendered with the rest of the frame it is the same picture.
 * dlisa_async=1 leaves it as asked (for the GPU work).
 */
static void (*origSetRendersAsynchronously)(id, SEL, bool);

static void
setRendersAsynchronously(id self, SEL selector, bool asynchronously)
{
	static int said;

	if (asynchronously && said++ < 4) {
		say("dlisa[%s]: a layer host asked to render asynchronously: rendered with the frame instead\n", getprogname());
	}
	origSetRendersAsynchronously(self, selector, false);
}

static void
withoutAsynchronousLayerHosts(void)
{
	if (bootArgNumber("dlisa_async") != 0) {
		return;
	}
	origSetRendersAsynchronously = (void (*)(id, SEL, bool))replaceMethod('-', "CALayerHost",
	    "setRendersAsynchronously:", (IMP)setRendersAsynchronously);
}

/* kern.bootargs as it is (a static buffer, "" if the kernel will not say) */
static const char *
readBootArgs(void)
{
	static char arguments[1024];
	size_t size = sizeof(arguments) - 1;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		size = 0;
	}
	arguments[size] = 0;
	return arguments;
}

/*
 * The status bar (docs/home-button-type.md, part 5). UIKit takes a split bar when its native width and its notch
 * match the screen's: on this 1080 x 2400 at 3 (lisa=1) SpringBoard falls back to "iPhone X (D22, Zoomed)", whose
 * notch fits no split bar, and "rdar:45025538" stood in for the time, battery and Wi-Fi (com.apple.UIKit's
 * UIStatusBarVisualProviderClassName, parts 3 and 4, is not honoured on this path). So:
 *   - SpringBoard hands every process the notch the split bar of the display's width wants (status-bar.c,
 *     computeSplitNotch): +[UISDisplaySingleRectShape sb_thisDeviceDisplayShape], the exclusion area of the display
 *     context apps get. UIKit then picks that split bar by itself, in every app.
 *   - and wherever this library is, +[_UIStatusBarVisualProvider_Split visualProviderSubclassForScreen:...] answers
 *     that split bar when UIKit found none.
 * dlisa_sbprov=0 leaves both alone.
 */
#define UIKIT_DOMAIN                    CFSTR("com.apple.UIKit")
#define STATUS_BAR_PROVIDER_KEY         CFSTR("UIStatusBarVisualProviderClassName")
#define SCREEN_FACTS_SECONDS            30

/* parts 3 and 4 wrote that key; it does nothing here and goes */
static void
removeStatusBarProviderKey(void)
{
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		CFPropertyListRef kept = CFPreferencesCopyAppValue(STATUS_BAR_PROVIDER_KEY, UIKIT_DOMAIN);

		if (kept == NULL) {
			return;
		}
		CFRelease(kept);
		CFPreferencesSetAppValue(STATUS_BAR_PROVIDER_KEY, NULL, UIKIT_DOMAIN);
		say("dlisa[%s]: status bar: com.apple.UIKit UIStatusBarVisualProviderClassName removed (%s)\n", getprogname(),
		    CFPreferencesAppSynchronize(UIKIT_DOMAIN) ? "saved" : "NOT saved");
	});
}

/* What the choice is made from, once UIKit is up: MobileGestalt, SpringBoard's screen type, UIScreen, UIKit's own pick */
typedef struct {
	double x, y, width, height;
} DlisaRect;

static bool
isAnswering(id object, const char *selector)
{
	return object != nil && class_respondsToSelector(object_getClass(object), sel_registerName(selector));
}

static DlisaRect
readRect(id object, const char *selector)
{
	DlisaRect none = { 0, 0, 0, 0 };

	return isAnswering(object, selector) ? ((DlisaRect (*)(id, SEL))objc_msgSend)(object, sel_registerName(selector)) : none;
}

static double
readDouble(id object, const char *selector)
{
	return isAnswering(object, selector) ? ((double (*)(id, SEL))objc_msgSend)(object, sel_registerName(selector)) : 0;
}

typedef struct {
	double width, height;
} DlisaSize;

static Class (*origSplitSubclass)(Class, SEL, id, id);

/* the split bar for the display's width, or Nil (dlisa_sbprov=0, a width with none, a UIKit without it) */
static Class
findSplitProviderClass(void)
{
	const char *name = findStatusBarProvider(readBootArgs());

	return name != NULL ? objc_getClass(name) : Nil;
}

static Class
splitSubclassOrOurs(Class self, SEL selector, id screen, id info)
{
	static int said;
	Class chosen = origSplitSubclass(self, selector, screen, info);
	Class ours = chosen == Nil ? findSplitProviderClass() : Nil;

	if (ours != Nil && said++ < 2) {
		say("dlisa[%s]: status bar: UIKit found no split bar for this screen; %s it is\n", getprogname(), class_getName(ours));
	}
	return chosen != Nil ? chosen : ours;
}

/* in every program that has UIKit */
static void
fixSplitStatusBar(void)
{
	if (objc_getClass("_UIStatusBarVisualProvider_Split") == Nil || findSplitProviderClass() == Nil) {
		return;
	}
	origSplitSubclass = (Class (*)(Class, SEL, id, id))replaceMethod('+', "_UIStatusBarVisualProvider_Split",
	    "visualProviderSubclassForScreen:visualProviderInfo:", (IMP)splitSubclassOrOurs);
}

/* SpringBoard: the notch of the split bar for the display's width, for the display context of every process */
static id (*origDisplayShape)(id, SEL);

static id
makeSplitNotchShape(id original)
{
	Class split = findSplitProviderClass(), shape = objc_getClass("UISDisplaySingleRectShape");
	const char *line = readBootArgs();
	SEL notchSize = sel_registerName("notchSize"), reference = sel_registerName("referenceScreenScale");
	DlisaSize size;
	DlisaNotch notch;
	DlisaRect was = readRect(original, "rect");

	if (split == Nil || shape == Nil || class_getClassMethod(split, notchSize) == NULL) {
		return original;
	}
	size = ((DlisaSize (*)(Class, SEL))objc_msgSend)(split, notchSize);
	notch = computeSplitNotch((double)readDisplayWidth(line), (double)readDisplayScale(line), size.width, size.height,
	    class_getClassMethod(split, reference) != NULL ? ((double (*)(Class, SEL))objc_msgSend)(split, reference) : 0);
	say("dlisa[%s]: status bar: notch %.2f,%.2f %.2fx%.2f instead of SpringBoard's %.2f,%.2f %.2fx%.2f (what %s wants)\n",
	    getprogname(), notch.x, notch.y, notch.width, notch.height, was.x, was.y, was.width, was.height, class_getName(split));
	return ((id (*)(id, SEL, DlisaNotch))objc_msgSend)(((id (*)(Class, SEL))objc_msgSend)(shape, sel_registerName("alloc")),
	    sel_registerName("initWithRect:"), notch);
}

static id
displayShapeForSplitBar(id self, SEL selector)
{
	static id shape;
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		shape = makeSplitNotchShape(origDisplayShape(self, selector));
	});
	return shape;
}

static void
fixSpringBoardNotch(void)
{
	if (findSplitProviderClass() == Nil) {
		return;
	}
	origDisplayShape = (id (*)(id, SEL))replaceMethod('+', "UISDisplaySingleRectShape", "sb_thisDeviceDisplayShape",
	    (IMP)displayShapeForSplitBar);
}

static long
readArtworkSubtype(void)
{
	CFTypeRef (*copyAnswer)(CFStringRef) = (CFTypeRef (*)(CFStringRef))dlsym(RTLD_DEFAULT, "MGCopyAnswer");
	CFTypeRef traits = copyAnswer != NULL ? copyAnswer(CFSTR("oPeik/9e8lQWMszEjbPzng")) : NULL;   /* ArtworkTraits */
	CFTypeRef subtype = traits != NULL && CFGetTypeID(traits) == CFDictionaryGetTypeID() ?
	    CFDictionaryGetValue((CFDictionaryRef)traits, CFSTR("ArtworkDeviceSubType")) : NULL;
	long value = -1;

	if (subtype != NULL && CFGetTypeID(subtype) == CFNumberGetTypeID()) {
		CFNumberGetValue((CFNumberRef)subtype, kCFNumberLongType, &value);
	}
	if (traits != NULL) {
		CFRelease(traits);
	}
	return value;
}

static int
readHomeButtonType(void)
{
	int (*getNumber)(CFStringRef, int) = (int (*)(CFStringRef, int))dlsym(RTLD_DEFAULT, "MGGetSInt32Answer");

	return getNumber != NULL ? getNumber(CFSTR("JwLB44/jEB8aFDpXQ16Tuw"), -1) : -1;     /* HomeButtonType */
}

/* SpringBoardHome's screen type and its name ("iPhone X (D22)") into name */
static long
readScreenType(char *name, size_t size)
{
	long (*currentType)(void) = (long (*)(void))dlsym(RTLD_DEFAULT, "SBHScreenTypeForCurrentDevice");
	CFStringRef (*nameOf)(long) = (CFStringRef (*)(long))dlsym(RTLD_DEFAULT, "SBHStringForScreenType");
	long type = currentType != NULL ? currentType() : -1;
	CFStringRef text = nameOf != NULL && type >= 0 ? nameOf(type) : NULL;

	snprintf(name, size, "?");
	if (text != NULL) {
		CFStringGetCString(text, name, (CFIndex)size, kCFStringEncodingUTF8);
	}
	return type;
}

/* the split bar UIKit itself picks for that screen (the override is not looked at there), or "none" */
static const char *
readOwnSplitProvider(id screen)
{
	Class split = objc_getClass("_UIStatusBarVisualProvider_Split");
	SEL choose = sel_registerName("visualProviderSubclassForScreen:visualProviderInfo:");
	Class (*own)(Class, SEL, id, id) = origSplitSubclass != NULL ? origSplitSubclass :
	    split != Nil && class_getClassMethod(split, choose) != NULL ? (Class (*)(Class, SEL, id, id))objc_msgSend : NULL;
	Class chosen = own != NULL && screen != nil ? own(split, choose, screen, nil) : Nil;

	return chosen != Nil ? class_getName(chosen) : "none (rdar:45025538)";
}

static void
sayScreenFacts(void)
{
	id screen = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("UIScreen"), sel_registerName("mainScreen"));
	id notch = isAnswering(screen, "_exclusionArea") ? ((id (*)(id, SEL))objc_msgSend)(screen, sel_registerName("_exclusionArea")) : nil;
	DlisaRect points = readRect(screen, "_referenceBounds"), native = readRect(screen, "_nativeDisplayBounds");
	DlisaRect cut = readRect(notch, "rect");
	char typeName[96];
	long type = readScreenType(typeName, sizeof(typeName));

	say("dlisa[%s]: screen: artwork %ld, home button type %d, SpringBoard type %ld (%s)\n", getprogname(),
	    readArtworkSubtype(), readHomeButtonType(), type, typeName);
	say("dlisa[%s]: screen: %.0fx%.0f points at %.2f, native %.0fx%.0f at %.3f, corner %.2f, notch %s %.2f,%.2f %.2fx%.2f; "
	    "UIKit's own split bar: %s\n", getprogname(), points.width, points.height, readDouble(screen, "scale"),
	    native.width, native.height, readDouble(screen, "_nativeScale"), readDouble(screen, "_displayCornerRadius"),
	    notch != nil ? class_getName(object_getClass(notch)) : "none", cut.x, cut.y, cut.width, cut.height,
	    readOwnSplitProvider(screen));
}

static void
startStatusBarFix(void)
{
	removeStatusBarProviderKey();
	fixSpringBoardNotch();
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, SCREEN_FACTS_SECONDS * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		sayScreenFacts();
	});
}

/* ---- SpringBoard: Control Center's flashlight on the kernel torch (torch-level.c, kernel torch=3) ----------- */

/*
 * AVFlashlight (AVFCapture) asks cameracaptured, whose capture stack has no camera here. Its instance methods are
 * answered in SpringBoard instead and the level goes to /dev/hakios-torch. SBUIFlashlightController observes
 * flashlightLevel by KVO, so the change is announced. Availability itself (camera-flash) is MobileGestalt's.
 */
static float flashlightLevel;

static void
writeTorch(uint32_t permille)
{
	int fd = open("/dev/hakios-torch", O_WRONLY);

	if (fd < 0) {
		say("dlisa[%s]: flashlight: /dev/hakios-torch: %s\n", getprogname(), strerror(errno));
		return;
	}
	if (write(fd, &permille, sizeof(permille)) != (ssize_t)sizeof(permille)) {
		say("dlisa[%s]: flashlight: write: %s\n", getprogname(), strerror(errno));
	}
	close(fd);
}

static void
setTorchLevel(id flashlight, float level)
{
	id key = (id)CFSTR("flashlightLevel");

	((void (*)(id, SEL, id))objc_msgSend)(flashlight, sel_registerName("willChangeValueForKey:"), key);
	flashlightLevel = level > 0.0f ? (level > 1.0f ? 1.0f : level) : 0.0f;
	writeTorch(torchPermilleForLevel(flashlightLevel));
	((void (*)(id, SEL, id))objc_msgSend)(flashlight, sel_registerName("didChangeValueForKey:"), key);
	say("dlisa[%s]: flashlight: level %.2f (%u of 1000)\n", getprogname(), (double)flashlightLevel,
	    torchPermilleForLevel(flashlightLevel));
}

static bool
flashlightYes(__unused id self, __unused SEL selector)
{
	return true;
}

static bool
flashlightNo(__unused id self, __unused SEL selector)
{
	return false;
}

static bool
flashlightPowerOn(__unused id self, __unused SEL selector, __unused void *error)
{
	return true;
}

static bool
flashlightSetLevel(id self, __unused SEL selector, float level, __unused void *error)
{
	setTorchLevel(self, level);
	return true;
}

static void
flashlightPowerOff(id self, __unused SEL selector)
{
	setTorchLevel(self, 0.0f);
}

static float
flashlightGetLevel(__unused id self, __unused SEL selector)
{
	return flashlightLevel;
}

static void
withKernelFlashlight(void)
{
	static const struct {
		const char *selector;
		IMP replacement;
	} methods[] = {
		{ "isAvailable", (IMP)flashlightYes }, { "isOverheated", (IMP)flashlightNo },
		{ "turnPowerOnWithError:", (IMP)flashlightPowerOn }, { "setFlashlightLevel:withError:", (IMP)flashlightSetLevel },
		{ "turnPowerOff", (IMP)flashlightPowerOff }, { "flashlightLevel", (IMP)flashlightGetLevel },
		{ "beamWidthControlSupported", (IMP)flashlightNo },
	};
	char status[128];
	size_t size = sizeof(status) - 1;
	unsigned replaced = 0;

	if (sysctlbyname("kern.hakios.torch_status", status, &size, NULL, 0) != 0 || !isTorchServing(status)) {
		return;                 /* (no torch=3 kernel driver: AVFlashlight is left as it is) */
	}
	if (dlopen("/System/Library/PrivateFrameworks/AVFCapture.framework/AVFCapture", RTLD_NOW) == NULL) {
		say("dlisa[%s]: flashlight: AVFCapture not loaded: %s\n", getprogname(), dlerror());
		return;
	}
	for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
		replaced += replaceMethod('-', "AVFlashlight", methods[i].selector, methods[i].replacement) != NULL;
	}
	say("dlisa[%s]: flashlight: AVFlashlight on /dev/hakios-torch (%u of %zu methods)\n", getprogname(), replaced,
	    sizeof(methods) / sizeof(methods[0]));
}

static void
fixSpringBoard(void)
{
	withoutAsynchronousLayerHosts();
	origSetSetupRequiredReason = (void (*)(id, SEL, unsigned long))replaceMethod('-', "SBSetupManager",
	    "_setSetupRequiredReason:", (IMP)setSetupRequiredReason);
	replaceMethod('-', "SBLockdownManager", "_setupActivationState", (IMP)setupActivationState);
	replaceMethod('-', "SBLockdownManager", "brickedDevice", (IMP)notBricked);
	origUnlockUIFromSource = (bool (*)(id, SEL, long, id))replaceMethod('-', "SBLockScreenManager",
	    "unlockUIFromSource:withOptions:", (IMP)unlockUIFromSource);
	origIsUnlockDisabled = (bool (*)(id, SEL))replaceMethod('-', "SBLockScreenManager", "_isUnlockDisabled",
	    (IMP)isUnlockDisabled);
	/* (a hook whose original was not found must not be called through a NULL) */
	if (origSetSetupRequiredReason == NULL) {
		say("dlisa[%s]: setup mode not fixed: the Setup Assistant may block the home screen\n", getprogname());
	}
	watchdogScale = bootArgNumber("dlisa_watchdog");
	if (watchdogScale > 1) {
		origWatchdogScale = (double (*)(id, SEL, id, bool))replaceMethod('-', "FBApplicationProcessWatchdogPolicy",
		    "_queue_watchdogScalingFactorForAppInfo:isResume:", (IMP)watchdogScalingFactor);
		if (origWatchdogScale == NULL) {
			watchdogScale = 0;
		}
	}
	replaceMethod('+', "XBApplicationSnapshotManifestImpl", "_outputFormatForSnapshot:", (IMP)snapshotOutputFormat);
	setForceNoBuddy();
	startStatusBarFix();
	withKernelFlashlight();
	unlockMode = bootArgNumber("dlisa_unlock");
	if (unlockMode != 0) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, UNLOCK_FIRST_SECONDS * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			unlockPass(0);
		});
	}
	if (bootArgNumber("dlisa_noidle") != 0) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, UNLOCK_FIRST_SECONDS * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			idlePass(0);
		});
	}
}

/* ---- PosterBoard ----------------------------------------------------------------------------------- */

static bool (*origGetResourceValue)(id, SEL, id *, id, id *);
static id (*origResourceValuesForKeys)(id, SEL, id, id *);

/* statfs's f_flags of the volume holding a file URL; MNT_CPROTECT (left alone) when it cannot be told */
static uint64_t
readVolumeFlags(id url)
{
	struct statfs volume;
	const char *path;

	if (!((bool (*)(id, SEL))objc_msgSend)(url, sel_registerName("isFileURL"))) {
		return MNT_CPROTECT;
	}
	path = ((const char *(*)(id, SEL))objc_msgSend)(url, sel_registerName("fileSystemRepresentation"));
	if (path == NULL || statfs(path, &volume) != 0) {
		return MNT_CPROTECT;
	}
	return volume.f_flags;
}

static bool
isFileProtectionKey(id key)
{
	return key != nil && CFEqual((CFTypeRef)key, kCFURLFileProtectionKey);
}

static void
sayProtectionNone(id url)
{
	static int said;

	if (said++ < 4) {
		say("dlisa[%s]: %s: no file protection class on this volume, read as NSURLFileProtectionNone\n", getprogname(),
		    describe(url));
	}
}

/* -[NSURL getResourceValue:forKey:error:]: PosterBoard's store viability check asks the key by key */
static bool
getResourceValueWithProtectionNone(id self, SEL selector, id *value, id key, id *error)
{
	bool succeeded = origGetResourceValue(self, selector, value, key, error);

	if (value == NULL || !isFileProtectionKey(key)) {
		return succeeded;
	}
	if (isProtectionNoneImplied(succeeded, *value == nil, readVolumeFlags(self))) {
		*value = (id)kCFURLFileProtectionNone;
		sayProtectionNone(self);
	}
	return succeeded;
}

/* -[NSURL resourceValuesForKeys:error:]: the same answer when the key comes in a set */
static id
resourceValuesWithProtectionNone(id self, SEL selector, id keys, id *error)
{
	id values = origResourceValuesForKeys(self, selector, keys, error);
	id key = (id)kCFURLFileProtectionKey;
	id copy;

	if (values == nil || keys == nil || !((bool (*)(id, SEL, id))objc_msgSend)(keys, sel_registerName("containsObject:"), key)) {
		return values;
	}
	if (!isProtectionNoneImplied(true, ((id (*)(id, SEL, id))objc_msgSend)(values, sel_registerName("objectForKey:"), key) == nil,
	    readVolumeFlags(self))) {
		return values;
	}
	copy = ((id (*)(id, SEL))objc_msgSend)(values, sel_registerName("mutableCopy"));
	((void (*)(id, SEL, id, id))objc_msgSend)(copy, sel_registerName("setObject:forKey:"), (id)kCFURLFileProtectionNone, key);
	sayProtectionNone(self);
	return ((id (*)(id, SEL))objc_msgSend)(copy, sel_registerName("autorelease"));
}

/* purgeable.c: the APFS purgeable marks, a no-op that succeeds on a volume that is not APFS */
static bool (*origPbfSetPurgable)(id, SEL, bool, id *);
static bool (*origPfSetPurgable)(id, SEL, bool, id, id *);
static bool (*origPfUnmarkAsPurgable)(id, SEL, id *);

/* true when the file's volume has no purgeable state; false (Apple's call) when it cannot be told */
static bool
isOnVolumeWithoutPurgeable(id url)
{
	struct statfs volume;
	const char *path;

	if (!((bool (*)(id, SEL))objc_msgSend)(url, sel_registerName("isFileURL"))) {
		return false;
	}
	path = ((const char *(*)(id, SEL))objc_msgSend)(url, sel_registerName("fileSystemRepresentation"));
	if (path == NULL || statfs(path, &volume) != 0) {
		return false;
	}
	return isPurgeableUnsupported(volume.f_fstypename);
}

static void
sayPurgeableSkipped(id url, bool purgeable)
{
	static int said;

	if (said++ < 4) {
		say("dlisa[%s]: %s: no purgeable state on this volume, %s taken as done\n", getprogname(), describe(url),
		    purgeable ? "mark" : "unmark");
	}
}

/* -[NSURL pbf_setPurgable:error:] (PosterBoard.framework, ffsctl APFSIOC_MARK_PURGEABLE) */
static bool
setPurgableWithoutApfs(id self, SEL selector, bool purgeable, id *error)
{
	if (isOnVolumeWithoutPurgeable(self)) {
		sayPurgeableSkipped(self, purgeable);
		return true;
	}
	return origPbfSetPurgable(self, selector, purgeable, error);
}

/* -[NSURL pf_setPurgable:afterDate:error:] (PosterFoundation; pf_markPurgableInOneHourWithError: comes here too) */
static bool
setPurgableAfterDateWithoutApfs(id self, SEL selector, bool purgeable, id date, id *error)
{
	if (isOnVolumeWithoutPurgeable(self)) {
		sayPurgeableSkipped(self, purgeable);
		return true;
	}
	return origPfSetPurgable(self, selector, purgeable, date, error);
}

/* -[NSURL pf_unmarkAsPurgableWithError:] (PosterFoundation) */
static bool
unmarkAsPurgableWithoutApfs(id self, SEL selector, id *error)
{
	if (isOnVolumeWithoutPurgeable(self)) {
		sayPurgeableSkipped(self, false);
		return true;
	}
	return origPfUnmarkAsPurgable(self, selector, error);
}

static void
withoutApfsPurgeable(void)
{
	origPbfSetPurgable = (bool (*)(id, SEL, bool, id *))replaceMethod('-', "NSURL", "pbf_setPurgable:error:",
	    (IMP)setPurgableWithoutApfs);
	origPfSetPurgable = (bool (*)(id, SEL, bool, id, id *))replaceMethod('-', "NSURL", "pf_setPurgable:afterDate:error:",
	    (IMP)setPurgableAfterDateWithoutApfs);
	origPfUnmarkAsPurgable = (bool (*)(id, SEL, id *))replaceMethod('-', "NSURL", "pf_unmarkAsPurgableWithError:",
	    (IMP)unmarkAsPurgableWithoutApfs);
}

/* bounded-call.c: Focus's synchronous Do Not Disturb queries, with a deadline. Past it the answer is "no Focus":
 * no active mode assertion, no modes, no state. */
#define DND_DEADLINE_MS 2500

static IMP origActiveModeAssertion, origAllModes, origAvailableModes, origQueryCurrentState;

extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *pool);
extern id objc_retain(id object);
extern id objc_autorelease(id object);

struct dndQuery {
	IMP original;
	id self;
	SEL selector;
	id result;  /* retained */
	id error;   /* retained */
};

static void
runDndQuery(void *argument)
{
	struct dndQuery *query = argument;
	void *pool = objc_autoreleasePoolPush();
	id error = nil;
	id result = ((id (*)(id, SEL, id *))query->original)(query->self, query->selector, &error);

	query->result = result != nil ? objc_retain(result) : nil;
	query->error = error != nil ? objc_retain(error) : nil;
	objc_autoreleasePoolPop(pool);
}

/* the original, given DND_DEADLINE_MS; past it FALLBACK (a late answer is dropped with its thread's query) */
static id
queryDndBounded(IMP original, id self, SEL selector, id *error, id fallback)
{
	static int said;
	struct dndQuery *query = calloc(1, sizeof(*query));
	id result;

	if (query == NULL) {
		return ((id (*)(id, SEL, id *))original)(self, selector, error);
	}
	query->original = original;
	query->self = objc_retain(self);
	query->selector = selector;
	if (!runBounded(runDndQuery, query, DND_DEADLINE_MS)) {
		if (said++ < 8) {
			say("dlisa[%s]: -%s: donotdisturbd did not answer in %d ms; taken as no Focus\n", getprogname(),
			    sel_getName(selector), DND_DEADLINE_MS);
		}
		return fallback;
	}
	if (error != NULL) {
		*error = query->error != nil ? objc_autorelease(query->error) : nil;
	}
	result = query->result != nil ? objc_autorelease(query->result) : nil;
	free(query);
	return result;
}

static id
emptySet(void)
{
	return ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSSet"), sel_registerName("set"));
}

/* -[DNDModeSelectionService activeModeAssertionWithError:] (FCActivityManager's _updateActiveModeAssertionIfNecessary) */
static id
activeModeAssertionBounded(id self, SEL selector, id *error)
{
	return queryDndBounded(origActiveModeAssertion, self, selector, error, nil);
}

/* -[DNDModeSelectionService allModesWithError:] (FCActivityManager's _availableActivities) */
static id
allModesBounded(id self, SEL selector, id *error)
{
	return queryDndBounded(origAllModes, self, selector, error, emptySet());
}

/* -[DNDModeSelectionService availableModesWithError:] */
static id
availableModesBounded(id self, SEL selector, id *error)
{
	return queryDndBounded(origAvailableModes, self, selector, error, emptySet());
}

/* -[DNDStateService queryCurrentStateWithError:] */
static id
queryCurrentStateBounded(id self, SEL selector, id *error)
{
	return queryDndBounded(origQueryCurrentState, self, selector, error, nil);
}

static void
withDndDeadline(void)
{
	origActiveModeAssertion = replaceMethod('-', "DNDModeSelectionService", "activeModeAssertionWithError:",
	    (IMP)activeModeAssertionBounded);
	origAllModes = replaceMethod('-', "DNDModeSelectionService", "allModesWithError:", (IMP)allModesBounded);
	origAvailableModes = replaceMethod('-', "DNDModeSelectionService", "availableModesWithError:",
	    (IMP)availableModesBounded);
	origQueryCurrentState = replaceMethod('-', "DNDStateService", "queryCurrentStateWithError:",
	    (IMP)queryCurrentStateBounded);
}

static void
fixPosterBoard(void)
{
	origGetResourceValue = (bool (*)(id, SEL, id *, id, id *))replaceMethod('-', "NSURL", "getResourceValue:forKey:error:",
	    (IMP)getResourceValueWithProtectionNone);
	origResourceValuesForKeys = (id (*)(id, SEL, id, id *))replaceMethod('-', "NSURL", "resourceValuesForKeys:error:",
	    (IMP)resourceValuesWithProtectionNone);
	withoutApfsPurgeable();
	withDndDeadline();
}

/* ---- bluetoothd: its built-in virtual controller (bt-virtual.c, boot-arg dlisa_btvirt=1) ------------------- */

static int
sysctlbynameForBluetoothd(const char *name, void *old, size_t *oldLength, void *new, size_t newLength)
{
	static char arguments[1024];
	static bool said;
	size_t size = sizeof(arguments) - 1;

	if (arguments[0] == 0 && sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		arguments[size] = 0;
	}
	if (!isFakedVmmQuery(getprogname(), name, arguments) || old == NULL || oldLength == NULL || *oldLength < sizeof(int)) {
		return sysctlbyname(name, old, oldLength, new, newLength);
	}
	*(int *)old = 1;
	*oldLength = sizeof(int);
	if (!said) {
		said = true;
		say("dlisa[%s]: bt: kern.hv_vmm_present answered 1: the virtual controller (transport 10, APPLE_VIRTUAL)\n",
		    getprogname());
	}
	return 0;
}

/* the main program's imports of `symbol` (its GOT, both kinds) point at `replacement` from now on */
static unsigned
rebindMainImport(const char *symbol, void *replacement)
{
	static const char *const sections[][2] = { { "__DATA_CONST", "__auth_got" }, { "__DATA_CONST", "__got" },
		                                   { "__DATA", "__got" }, { "__AUTH_CONST", "__auth_got" } };
	const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(0);
	void *target = dlsym(RTLD_DEFAULT, symbol);
	unsigned replaced = 0;

	for (size_t i = 0; target != NULL && i < sizeof(sections) / sizeof(sections[0]); i++) {
		unsigned long size = 0;
		uint8_t *slots = getsectiondata(header, sections[i][0], sections[i][1], &size);
		vm_address_t page = (vm_address_t)slots & ~(vm_address_t)vm_page_mask;
		vm_size_t length = ((vm_address_t)slots + size - page + vm_page_mask) & ~(vm_size_t)vm_page_mask;

		if (slots == NULL || size == 0 ||
		    vm_protect(mach_task_self(), page, length, false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
			continue;
		}
		replaced += replacePointers((void **)slots, size / sizeof(void *), target, replacement);
		vm_protect(mach_task_self(), page, length, false, VM_PROT_READ);
	}
	return replaced;
}

static void
fixBluetoothd(void)
{
	char arguments[1024];
	size_t size = sizeof(arguments) - 1;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	arguments[size] = 0;
	if (!isFakedVmmQuery("bluetoothd", "kern.hv_vmm_present", arguments)) {
		say("dlisa[%s]: bt: dlisa_btvirt is not on: bluetoothd looks for its controller as usual\n", getprogname());
		return;
	}
	say("dlisa[%s]: bt: sysctlbyname rebound in %u slot(s) of bluetoothd's imports\n", getprogname(),
	    rebindMainImport("sysctlbyname", (void *)sysctlbynameForBluetoothd));
}

/* ---- which program gets which fix ------------------------------------------------------------------ */

static const struct {
	const char *program;
	void (*fix)(void);
} FIXES[] = {
	{ "containermanagerd", fixContainerManager },
	{ "containermanagerd_system", fixContainerManager },
	{ "lsd", fixLaunchServices },
	{ "backboardd", startTouchScreen },
	{ "SpringBoard", fixSpringBoard },
	{ "Preferences", withoutAsynchronousLayerHosts },
	{ "PosterBoard", fixPosterBoard },
	{ "CollectionsPoster", withoutApfsPurgeable },
	{ "bluetoothd", fixBluetoothd },
};

/*
 * stderr to the console: launchd opens it on /dev/null. With boot-arg applog=1 applog_name=NAME the kernel gives
 * NAME OS_ACTIVITY_DT_MODE=YES, and os_log then writes to stderr as well, so the daemon's own log shows up here.
 */
static void
pointStderrAtConsole(void)
{
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY);

	if (fd < 0) {
		return;
	}
	dup2(fd, 2);
	close(fd);
}

/*
 * Boot-arg dlisa_oslog=all (or =NAME,NAME,...): every os_log message of those programs is also printed on the console
 * (libsystem_trace's os_log_set_hook SPI; os_log_copy_message_string formats it). OS_ACTIVITY_DT_MODE does not do
 * that on a customer build: libtrace honours it only on an internal one, and even then not for os_log itself.
 */
typedef void *os_log_message_t;
typedef void (^os_log_hook_t)(uint8_t type, os_log_message_t message);
extern os_log_hook_t os_log_set_hook(uint8_t level, os_log_hook_t hook);
extern char *os_log_copy_message_string(os_log_message_t message);

static os_log_hook_t previousHook;

extern int backtrace(void **array, int size);
extern const void *_dyld_get_shared_cache_range(size_t *length);

/* Where a message came from: return addresses, unslid for the shared cache ("ipsw dsc a2s" names them). */
static void
sayBacktrace(void)
{
	void *frames[24];
	size_t length = 0;
	uintptr_t slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - 0x180000000UL;
	int count = backtrace(frames, 24);
	char line[600];
	int used = snprintf(line, sizeof(line), "oslog[%s] backtrace (unslid):", getprogname());

	for (int i = 1; i < count && used < (int)sizeof(line) - 20; i++) {
		used += snprintf(line + used, sizeof(line) - (size_t)used, " %lx", (unsigned long)((uintptr_t)frames[i] - slide));
	}
	say("%s\n", line);
}

/* KEY=all, KEY=1 or KEY=NAME,NAME,... (name-list.c): is this program one of them? */
static bool
isNamedHere(const char *key)
{
	return isNamedByBootArg(readBootArgs(), key, getprogname());
}

static bool
isLogWanted(void)
{
	return isNamedHere("dlisa_oslog");
}

static uint8_t logHookLevel;    /* 0 OS_LOG_TYPE_DEFAULT; 0x02 OS_LOG_TYPE_DEBUG, the lowest: every message */

static void
copyLogToConsole(void)
{
	if (!isLogWanted()) {
		return;
	}
	/* darwin-lisa, boot-arg dlisa_oslog_debug=1: the info and debug messages too (OS_LOG_TYPE_DEBUG is the lowest
	 * level, so a hook at it sees every message): MobileInstallation says why it leaves an application out at those
	 * levels, and at the default level only that it scanned the directory */
	{
		static char arguments[1024];
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
			arguments[size] = 0;
			logHookLevel = strstr(arguments, "dlisa_oslog_debug=1") != NULL ? 0x02 : 0;
		}
	}
	previousHook = os_log_set_hook(logHookLevel /* 0: OS_LOG_TYPE_DEFAULT and above */, ^(uint8_t type, os_log_message_t message) {
		char *text = os_log_copy_message_string(message);

		say("oslog[%s] %02x %s\n", getprogname(), type, text != NULL ? text : "?");
		if (text != NULL && strstr(text, "Bogus check-in") != NULL) {
			sayBacktrace();
		}
		free(text);
		if (previousHook != NULL) {
			previousHook(type, message);
		}
	});
}

/*
 * Boot-arg dlisa_uiprobe=NAME,NAME,... (or all): inside those apps (Preferences and SpringBoard load this library),
 * what the main thread does while the screen is touched, every 2 s that something happened:
 *   dlisa[Preferences]: probe 2.0 s: touches 37 (began 1 moved 35 ended 1 cancelled 0), runloop 80 turns, busiest
 *   14 ms; display link 61 calls in its 1 s
 * - touches: what -[UIApplication sendEvent:] was handed (none while a finger moves: the event server does not
 *   deliver to this app);
 * - runloop: turns of the main run loop and the longest stretch between waking and waiting again;
 * - display link: a CADisplayLink of ours on the main run loop, running 1 s in every 10 (about 60 calls when the app
 *   gets its refresh callbacks; 0 when it does not), and the first callbacks' timestamp, targetTimestamp and duration;
 * - "main thread stuck N ms": the main thread has not gone back to waiting for over 500 ms, and where it is (pc, lr
 *   and the frame chain, unslid: `ipsw dsc a2s` names them).
 */
#define PROBE_EVERY_MS          2000
#define PROBE_STUCK_MS          500
#define PROBE_LINK_EVERY_S      10
#define PROBE_FRAMES            14

static struct {
	_Atomic uint32_t phases[5];             /* UITouchPhase: began, moved, stationary, ended, cancelled */
	_Atomic uint32_t touchEvents, turns, busiestMs, linkCalls, stuck;
	_Atomic uint64_t busySince;             /* uptime ms the main run loop woke, 0 while it waits */
	_Atomic bool waitedOnce;
	thread_act_t mainThread;
	id link;
} probe;

static void (*origSendEvent)(id, SEL, id);

static void
probeSendEvent(id self, SEL selector, id event)
{
	long type = ((long (*)(id, SEL))objc_msgSend)(event, sel_registerName("type"));

	if (type == 0) {                        /* UIEventTypeTouches */
		id touch = SEND0(id, SEND0(id, event, "allTouches"), "anyObject");
		long phase = touch != nil ? ((long (*)(id, SEL))objc_msgSend)(touch, sel_registerName("phase")) : -1;

		probe.touchEvents++;
		if (phase >= 0 && phase < 5) {
			probe.phases[phase]++;
		}
	}
	origSendEvent(self, selector, event);
}

static void
probeTick(__unused id self, __unused SEL selector, id link)
{
	static int said;
	double timestamp = ((double (*)(id, SEL))objc_msgSend)(link, sel_registerName("timestamp"));
	double target = ((double (*)(id, SEL))objc_msgSend)(link, sel_registerName("targetTimestamp"));
	double duration = ((double (*)(id, SEL))objc_msgSend)(link, sel_registerName("duration"));

	probe.linkCalls++;
	if (said++ < 3) {
		say("dlisa[%s]: probe: display link call: timestamp %.4f target %.4f duration %.4f (uptime %.4f)\n", getprogname(),
		    timestamp, target, duration, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9);
	}
}

static void
watchMainRunLoop(void)
{
	CFRunLoopObserverRef observer = CFRunLoopObserverCreateWithHandler(NULL, kCFRunLoopAfterWaiting | kCFRunLoopBeforeWaiting,
	    true, 0, ^(__unused CFRunLoopObserverRef o, CFRunLoopActivity activity) {
		uint64_t now = uptimeMillis();

		if (activity == kCFRunLoopAfterWaiting) {
			probe.busySince = now;
			return;
		}
		if (probe.busySince != 0 && now - probe.busySince > probe.busiestMs) {
			probe.busiestMs = (uint32_t)(now - probe.busySince);
		}
		probe.busySince = 0;
		probe.waitedOnce = true;
		probe.turns++;
	});

	CFRunLoopAddObserver(CFRunLoopGetMain(), observer, kCFRunLoopCommonModes);
}

/* on the main queue: a display link of ours, paused but for 1 s in every PROBE_LINK_EVERY_S */
static void
startProbeDisplayLink(void)
{
	Class targetClass = objc_allocateClassPair(objc_getClass("NSObject"), "DLisaProbeTarget", 0);
	id target, runLoop;

	if (targetClass == Nil || objc_getClass("CADisplayLink") == Nil) {
		say("dlisa[%s]: probe: no display link\n", getprogname());
		return;
	}
	class_addMethod(targetClass, sel_registerName("tick:"), (IMP)probeTick, "v@:@");
	objc_registerClassPair(targetClass);
	target = SEND0(id, SEND0(id, (id)targetClass, "alloc"), "init");
	probe.link = ((id (*)(id, SEL, id, SEL))objc_msgSend)((id)objc_getClass("CADisplayLink"),
	    sel_registerName("displayLinkWithTarget:selector:"), target, sel_registerName("tick:"));
	runLoop = SEND0(id, (id)objc_getClass("NSRunLoop"), "mainRunLoop");
	((void (*)(id, SEL, id, id))objc_msgSend)(probe.link, sel_registerName("addToRunLoop:forMode:"), runLoop,
	    (id)kCFRunLoopCommonModes);
	((void (*)(id, SEL, bool))objc_msgSend)(probe.link, sel_registerName("setPaused:"), true);
}

static void
setProbeLinkPaused(bool paused)
{
	dispatch_async(dispatch_get_main_queue(), ^{
		if (probe.link != nil) {
			((void (*)(id, SEL, bool))objc_msgSend)(probe.link, sel_registerName("setPaused:"), paused);
		}
	});
}

/* the main thread's pc, lr and frame chain, read while it is suspended (vm_read_overwrite: a bad frame pointer
 * ends the walk instead of faulting), after WHAT and MS */
static void
sayMainThreadFrames(const char *what, uint64_t ms)
{
	arm_thread_state64_t state;
	mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
	uint64_t frames[PROBE_FRAMES], fp, pair[2];
	size_t length = 0;
	uintptr_t slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - 0x180000000UL;
	int n = 0, used;
	char line[600];

	if (thread_suspend(probe.mainThread) != KERN_SUCCESS) {
		return;
	}
	if (thread_get_state(probe.mainThread, ARM_THREAD_STATE64, (thread_state_t)&state, &count) == KERN_SUCCESS) {
		frames[n++] = state.__pc;
		frames[n++] = state.__lr;
		for (fp = state.__fp; n < PROBE_FRAMES && fp != 0 && (fp & 7) == 0; fp = pair[0]) {
			vm_size_t got = 0;

			if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof(pair), (vm_address_t)pair, &got) !=
			    KERN_SUCCESS || got != sizeof(pair) || pair[0] <= fp) {
				break;
			}
			frames[n++] = pair[1];
		}
	}
	thread_resume(probe.mainThread);
	used = snprintf(line, sizeof(line), "dlisa[%s]: %s %llu ms; pc, lr, frames (unslid):", getprogname(), what, ms);
	for (int i = 0; i < n && used < (int)sizeof(line) - 20; i++) {
		used += snprintf(line + used, sizeof(line) - (size_t)used, " %lx",
		    (unsigned long)((frames[i] & 0x0000007fffffffffULL) - slide));
	}
	say("%s\n", line);
}

static void
sayProbeWindow(uint64_t elapsedMs, bool linkWindow)
{
	say("dlisa[%s]: probe %llu.%llu s at uptime %llu s: touches %u (began %u moved %u ended %u cancelled %u), runloop %u "
	    "turns, busiest %u ms%s; display link %u calls%s\n", getprogname(), elapsedMs / 1000, elapsedMs % 1000 / 100,
	    uptimeMillis() / 1000, probe.touchEvents, probe.phases[0], probe.phases[1], probe.phases[3], probe.phases[4],
	    probe.turns, probe.busiestMs, probe.stuck ? ", stuck" : "", probe.linkCalls,
	    linkWindow ? " in its 1 s" : " (paused)");
	probe.touchEvents = 0;
	for (int i = 0; i < 5; i++) {
		probe.phases[i] = 0;
	}
	probe.turns = 0;
	probe.busiestMs = 0;
	probe.linkCalls = 0;
	probe.stuck = 0;
}

static void *
probeThread(__unused void *argument)
{
	uint64_t windowStart = uptimeMillis(), linkUntil = 0, nextLink = uptimeMillis() + 5000, sampledSpan = 0;
	int samples = 0, takenThisSpan = 0;
	bool linkWindow = false;

	pthread_setname_np("dlisa probe");
	for (;;) {
		uint64_t now, since, busy;

		usleep(100000);
		now = uptimeMillis();
		since = probe.busySince;
		busy = since != 0 ? now - since : 0;
		takenThisSpan = since == sampledSpan ? takenThisSpan : 0;
		/* a stuck stretch gets a backtrace after 0.5 s and another after 3.5 s; 40 in all */
		if (probe.waitedOnce && samples < 40 && busy > PROBE_STUCK_MS + (uint64_t)takenThisSpan * 3000 &&
		    takenThisSpan < 2) {
			probe.stuck++;
			sayMainThreadFrames("probe: main thread stuck", busy);
			samples++;
			takenThisSpan++;
			sampledSpan = since;
		}
		if (now >= nextLink) {
			setProbeLinkPaused(false);
			linkWindow = true;
			linkUntil = now + 1000;
			nextLink = now + PROBE_LINK_EVERY_S * 1000;
		}
		if (linkUntil != 0 && now >= linkUntil) {
			setProbeLinkPaused(true);
			linkUntil = 0;
		}
		if (now - windowStart < PROBE_EVERY_MS) {
			continue;
		}
		if (probe.touchEvents != 0 || probe.stuck != 0 || probe.linkCalls != 0 || linkWindow) {
			sayProbeWindow(now - windowStart, linkWindow);
		}
		linkWindow = linkUntil != 0;
		windowStart = now;
	}
	return NULL;
}

/*
 * dlisa_uiprobe, 15 s after start: what colour the app gives its text (trait-names.c). One line for the key window
 * (its appearance, the screen's, labelColor resolved for it) and one for each of the first 8 UILabels found under the
 * windows (text, resolved textColor, its own appearance, its layer's compositingFilter, and the nearest ancestor whose
 * class names a visual effect, glass or vibrancy). White text with "light" here means the render server colours it;
 * "dark" means the app resolves its colours for a dark appearance.
 * ctl2 answered that (light, labelColor black, yet no title and no SwiftUI row text on screen), so each label also gets
 * "text: label N view alpha ... window ... font NAME SIZE pt; layer CLASS at (x,y wxh) bounds ... opacity ... contents
 * CLASS|nil format ...", and up to 24 layers whose class can hold text (UIKit label layers, CATextLayer, RenderBox,
 * SwiftUI's CG drawing layers) one "text: layer N CLASS (of VIEW) ..." line each. contents nil = never drawn (the app's
 * side); contents present, on screen, opaque = drawn but not composited (backboardd's side); EMPTY or a 0 pt font =
 * laid out to nothing (traits, Dynamic Type, screen metrics).
 */
#define TEXT_PROBE_LABELS       8

static long
readInterfaceStyle(id object)
{
	id traits = object != nil ? SEND0(id, object, "traitCollection") : nil;

	return traits != nil ? SEND0(long, traits, "userInterfaceStyle") : -1;
}

static void
formatColorFor(char *text, size_t size, id color, id traits)
{
	double r = 0, g = 0, b = 0, a = 0;
	id resolved = color;

	if (color != nil && traits != nil) {
		resolved = ((id (*)(id, SEL, id))objc_msgSend)(color, sel_registerName("resolvedColorWithTraitCollection:"), traits);
	}
	if (resolved == nil || !((bool (*)(id, SEL, double *, double *, double *, double *))objc_msgSend)(resolved,
	    sel_registerName("getRed:green:blue:alpha:"), &r, &g, &b, &a)) {
		snprintf(text, size, "?");
		return;
	}
	formatRgba(text, size, r, g, b, a);
}

static const char *
findEffectAncestor(id view)
{
	for (id at = SEND0(id, view, "superview"); at != nil; at = SEND0(id, at, "superview")) {
		const char *name = object_getClassName(at);

		if (strstr(name, "Effect") != NULL || strstr(name, "Glass") != NULL || strstr(name, "Vibran") != NULL) {
			return name;
		}
	}
	return "-";
}

#define TEXT_PROBE_LAYERS       24

struct probeRect {
	double x, y, w, h;
};

static const char *
describeObject(id object)
{
	id text = object != nil ? SEND0(id, object, "description") : nil;
	const char *utf8 = text != nil ? SEND0(const char *, text, "UTF8String") : NULL;

	return utf8 != NULL ? utf8 : "-";
}

/* where a layer is, in its window's layer, and what it holds (frame, bounds, opacity, hidden, contents, format) */
static void
formatLayerState(char *text, size_t size, id layer, id windowLayer)
{
	struct probeRect bounds = ((struct probeRect (*)(id, SEL))objc_msgSend)(layer, sel_registerName("bounds"));
	struct probeRect inWindow = windowLayer != nil ? ((struct probeRect (*)(id, SEL, struct probeRect, id))objc_msgSend)(layer,
	    sel_registerName("convertRect:toLayer:"), bounds, windowLayer) : bounds;
	float opacity = SEND0(float, layer, "opacity");
	bool hidden = SEND0(bool, layer, "isHidden");
	id contents = SEND0(id, layer, "contents");
	char frame[48], box[48];

	formatRect(frame, sizeof(frame), inWindow.x, inWindow.y, inWindow.w, inWindow.h);
	formatRect(box, sizeof(box), bounds.x, bounds.y, bounds.w, bounds.h);
	snprintf(text, size, "at %s bounds %s%s opacity %.2f%s contents %s format %s", frame, box,
	    isRectEmpty(bounds.w, bounds.h) ? " EMPTY" : "", opacity, hidden ? " HIDDEN" : "",
	    contents != nil ? object_getClassName(contents) : "nil", describeObject(SEND0(id, layer, "contentsFormat")));
}

/* the label's view side: alpha, hidden, window, font and point size (0 or a tiny size would draw nothing) */
static void
sayLabelGeometry(id label, int index, id windowLayer)
{
	id font = SEND0(id, label, "font"), layer = SEND0(id, label, "layer");
	double alpha = SEND0(double, label, "alpha");
	char state[320];

	formatLayerState(state, sizeof(state), layer, windowLayer);
	say("dlisa[%s]: text: label %d view alpha %.2f%s window %s font %s %.1f pt; layer %s %s\n", getprogname(), index, alpha,
	    SEND0(bool, label, "isHidden") ? " HIDDEN" : "", SEND0(id, label, "window") != nil ? "yes" : "NONE",
	    font != nil ? describeObject(SEND0(id, font, "fontName")) : "nil", font != nil ? SEND0(double, font, "pointSize") : 0.0,
	    object_getClassName(layer), state);
}

/* every layer under the window whose class can hold text (trait-names.c), with its sublayers' count */
static void
walkTextLayers(id layer, id windowLayer, int depth, int *found, int *layers)
{
	id sublayers = SEND0(id, layer, "sublayers");
	unsigned long count = sublayers != nil ? SEND0(unsigned long, sublayers, "count") : 0;
	const char *name = object_getClassName(layer);

	(*layers)++;
	if (*found < TEXT_PROBE_LAYERS && isTextLayerClassName(name)) {
		char state[320];
		id delegate = SEND0(id, layer, "delegate");

		formatLayerState(state, sizeof(state), layer, windowLayer);
		say("dlisa[%s]: text: layer %d %s (of %s) %s, %lu sublayers\n", getprogname(), (*found)++, name,
		    delegate != nil ? object_getClassName(delegate) : "-", state, count);
	}
	for (unsigned long i = 0; depth < 80 && i < count; i++) {
		walkTextLayers(((id (*)(id, SEL, unsigned long))objc_msgSend)(sublayers, sel_registerName("objectAtIndex:"), i),
		    windowLayer, depth + 1, found, layers);
	}
}

static void
sayLabel(id label, int index)
{
	char color[40], filter[48];
	id text = SEND0(id, label, "text"), compositing = SEND0(id, SEND0(id, label, "layer"), "compositingFilter");
	const char *utf8 = text != nil ? SEND0(const char *, text, "UTF8String") : "(nil)";

	formatColorFor(color, sizeof(color), SEND0(id, label, "textColor"), SEND0(id, label, "traitCollection"));
	snprintf(filter, sizeof(filter), "%s", compositing != nil ? SEND0(const char *, SEND0(id, compositing, "description"),
	    "UTF8String") : "-");
	say("dlisa[%s]: text: label %d \"%.40s\" color %s style %s filter %s in %s (%s)\n", getprogname(), index,
	    utf8 != NULL ? utf8 : "?", color, nameInterfaceStyle(readInterfaceStyle(label)), filter, findEffectAncestor(label),
	    object_getClassName(label));
}

static void
walkLabels(id view, id windowLayer, int depth, int *found, int *views)
{
	id subviews;
	unsigned long count;

	(*views)++;
	if (*found < TEXT_PROBE_LABELS && ((bool (*)(id, SEL, Class))objc_msgSend)(view, sel_registerName("isKindOfClass:"),
	    objc_getClass("UILabel"))) {
		sayLabel(view, *found);
		sayLabelGeometry(view, (*found)++, windowLayer);
	}
	if (depth > 60) {
		return;
	}
	subviews = SEND0(id, view, "subviews");
	count = subviews != nil ? SEND0(unsigned long, subviews, "count") : 0;
	for (unsigned long i = 0; i < count; i++) {
		walkLabels(((id (*)(id, SEL, unsigned long))objc_msgSend)(subviews, sel_registerName("objectAtIndex:"), i), windowLayer,
		    depth + 1, found, views);
	}
}

static void
sayTextColors(void)
{
	id app = SEND0(id, (id)objc_getClass("UIApplication"), "sharedApplication");
	id windows = app != nil ? SEND0(id, app, "windows") : nil;
	unsigned long count = windows != nil ? SEND0(unsigned long, windows, "count") : 0;
	id screen = SEND0(id, (id)objc_getClass("UIScreen"), "mainScreen");
	int found = 0, views = 0, textLayers = 0, layers = 0;

	for (unsigned long i = 0; i < count; i++) {
		id window = ((id (*)(id, SEL, unsigned long))objc_msgSend)(windows, sel_registerName("objectAtIndex:"), i);
		char label[40];

		formatColorFor(label, sizeof(label), SEND0(id, (id)objc_getClass("UIColor"), "labelColor"),
		    SEND0(id, window, "traitCollection"));
		say("dlisa[%s]: text: window %lu %s style %s (screen %s) labelColor %s\n", getprogname(), i,
		    object_getClassName(window), nameInterfaceStyle(readInterfaceStyle(window)),
		    nameInterfaceStyle(readInterfaceStyle(screen)), label);
		walkLabels(window, SEND0(id, window, "layer"), 0, &found, &views);
		walkTextLayers(SEND0(id, window, "layer"), SEND0(id, window, "layer"), 0, &textLayers, &layers);
	}
	say("dlisa[%s]: text: %lu windows, %d views, %d labels shown; %d layers, %d that can hold text shown\n", getprogname(),
	    count, views, found, layers, textLayers);
}

static void
startUiProbe(void)
{
	pthread_t thread;

	if (!isNamedHere("dlisa_uiprobe") || objc_getClass("UIApplication") == Nil) {
		return;
	}
	probe.mainThread = mach_thread_self();  /* the constructor runs on the main thread */
	origSendEvent = (void (*)(id, SEL, id))replaceMethod('-', "UIApplication", "sendEvent:", (IMP)probeSendEvent);
	watchMainRunLoop();
	dispatch_async(dispatch_get_main_queue(), ^{
		startProbeDisplayLink();
	});
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		sayTextColors();
	});
	if (pthread_create(&thread, NULL, probeThread, NULL) == 0) {
		pthread_detach(thread);
	}
	say("dlisa[%s]: probe: on (touches, main run loop, display link, stuck main thread)\n", getprogname());
}

static void *
launchProbeThread(void *argument)
{
	uint64_t start = (uint64_t)(uintptr_t)argument, elapsed;
	int taken = 0;

	pthread_setname_np("dlisa launch probe");
	while (taken < LAUNCH_PROBE_SAMPLES) {
		usleep(100000);
		elapsed = uptimeMillis() - start;
		if (isLaunchProbeDue(elapsed, taken)) {
			sayMainThreadFrames("launch probe: main thread at", elapsed);
			taken++;
		}
	}
	return NULL;
}

static void
startLaunchProbe(void)
{
	pthread_t thread;

	if (!isNamedHere("dlisa_launchprobe")) {
		return;
	}
	probe.mainThread = mach_thread_self();  /* the constructor runs on the main thread */
	if (pthread_create(&thread, NULL, launchProbeThread, (void *)(uintptr_t)uptimeMillis()) == 0) {
		pthread_detach(thread);
	}
	say("dlisa[%s]: launch probe: on (the main thread at 1.5, 4, 7, 9.5 s)\n", getprogname());
}

__attribute__((constructor)) static void
dlisaInjectInit(void)
{
	const char *name = getprogname();

	pointStderrAtConsole();
	copyLogToConsole();
	startUiProbe();
	startLaunchProbe();
	fixSplitStatusBar();

	for (size_t i = 0; i < sizeof(FIXES) / sizeof(FIXES[0]); i++) {
		if (strcmp(name, FIXES[i].program) == 0) {
			FIXES[i].fix();
			return;
		}
	}
	say("dlisa[%s]: loaded, nothing to do here\n", name);
}
