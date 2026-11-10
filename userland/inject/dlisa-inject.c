/*
 * libdlisa.dylib: code of ours inside stock iOS 26 daemons (the iOS 26 counterpart of HakiOS's libhakios.dylib).
 *
 * The kernel starts the programs named in /etc/dlisa-inject.conf with DYLD_INSERT_LIBRARIES pointing here
 * (sysctl kern.exec_inject, set by init26; kernel/execaids). What is done depends on the program; each fix is
 * one entry of FIXES below.
 *
 *   containermanagerd (both of them)
 *       It asks the user manager for the "personas" of the user. This port has no data volume, no keybag and
 *       no Secure Enclave, so the persona usermanagerd --init writes is not one containermanagerd can resolve a
 *       container for: the user container lookup lsd makes at every start (class 10, persona "primary") fails
 *       and lsd aborts ("Could not get user container URL! Must abort."), and runningboardd, backboardd and
 *       SpringBoard wait on lsd. Told that personas are not supported, containermanagerd answers with the
 *       POSIX user's home (/var/mobile), as HakiOS did on iOS 16.5.
 *
 * Only Objective-C runtime calls: nothing here depends on addresses inside the dyld shared cache.
 */
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
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

static void
fixContainerManager(void)
{
	replaceClassMethod("MCMUserIdentityCache", "personasAreSupported", (IMP)personasAreNotSupported);
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

/* ---- which program gets which fix ------------------------------------------------------------------ */

static const struct {
	const char *program;
	void (*fix)(void);
} FIXES[] = {
	{ "containermanagerd", fixContainerManager },
	{ "containermanagerd_system", fixContainerManager },
	{ "lsd", fixLaunchServices },
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

/* dlisa_oslog=all, or dlisa_oslog=NAME,NAME,...: is this program one of them? */
static bool
isLogWanted(void)
{
	static char arguments[1024];
	size_t size = sizeof(arguments) - 1;
	const char *name = getprogname();
	size_t length = strlen(name);
	char *at;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return false;
	}
	arguments[size] = 0;
	at = strstr(arguments, "dlisa_oslog=");
	if (at == NULL) {
		return false;
	}
	at += strlen("dlisa_oslog=");
	if (strncmp(at, "all", 3) == 0 || strncmp(at, "1", 1) == 0) {
		return true;
	}
	while (*at != 0 && *at != ' ') {
		if (strncmp(at, name, length) == 0 && (at[length] == ',' || at[length] == ' ' || at[length] == 0)) {
			return true;
		}
		at += strcspn(at, ", ");
		at += *at == ',';
	}
	return false;
}

static void
copyLogToConsole(void)
{
	if (!isLogWanted()) {
		return;
	}
	previousHook = os_log_set_hook(0 /* OS_LOG_TYPE_DEFAULT and above */, ^(uint8_t type, os_log_message_t message) {
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

__attribute__((constructor)) static void
dlisaInjectInit(void)
{
	const char *name = getprogname();

	pointStderrAtConsole();
	copyLogToConsole();

	for (size_t i = 0; i < sizeof(FIXES) / sizeof(FIXES[0]); i++) {
		if (strcmp(name, FIXES[i].program) == 0) {
			FIXES[i].fix();
			return;
		}
	}
	say("dlisa[%s]: loaded, nothing to do here\n", name);
}
