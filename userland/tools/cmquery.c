/*
 * cmquery: asks containermanagerd for a container the way lsd does at its start (CoreServices, the user container
 * URL: class 10, persona "primary", create-if-needed flags 0x900000001) and prints what comes back, the error
 * included -- the error lsd only writes to os_log before it aborts ("Could not get user container URL! Must abort.").
 * Started by launchd (com.darwinlisa.cmquery.plist) with its output on the console; diagnostics only.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *query_t;
typedef void *result_t;

/*
 * cmquery tail FILE [SECONDS]: copy what FILE gains to stdout (the console) for SECONDS (default 600) -- the
 * stderr of a daemon that runs as mobile and may not open the console itself (lsd's, with OS_ACTIVITY_DT_MODE).
 */
static int
tail_file(const char *path, int seconds)
{
	long at = 0;
	char buf[1024];

	for (int t = 0; t < seconds; t += 2) {
		FILE *f = fopen(path, "r");

		if (f != NULL) {
			size_t n;

			fseek(f, at, SEEK_SET);
			while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
				fwrite(buf, 1, n, stdout);
				at += (long)n;
			}
			fclose(f);
		}
		sleep(2);
	}
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc > 2 && strcmp(argv[1], "tail") == 0) {
		setvbuf(stdout, NULL, _IONBF, 0);
		return tail_file(argv[2], argc > 3 ? atoi(argv[3]) : 600);
	}
	void *cm = dlopen("/usr/lib/system/libsystem_containermanager.dylib", RTLD_NOW);
	query_t (*create)(void);
	void (*set_class)(query_t, uint64_t);
	void (*set_persona)(query_t, const char *);
	void (*set_flags)(query_t, uint64_t);
	result_t (*single)(query_t);
	void *(*last_error)(query_t);
	char *(*describe)(void *);
	const char *(*get_path)(result_t);
	const char **primary;
	uint64_t cls = argc > 1 ? strtoull(argv[1], NULL, 0) : 10, flags = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x900000001ULL;

	setvbuf(stdout, NULL, _IONBF, 0);
	sleep(argc > 3 ? atoi(argv[3]) : 0);
	if (cm == NULL) {
		printf("cmquery: %s\n", dlerror());
		return 1;
	}
	create = dlsym(cm, "container_query_create");
	set_class = dlsym(cm, "container_query_set_class");
	set_persona = dlsym(cm, "container_query_set_persona_unique_string");
	set_flags = dlsym(cm, "container_query_operation_set_flags");
	single = dlsym(cm, "container_query_get_single_result");
	last_error = dlsym(cm, "container_query_get_last_error");
	describe = dlsym(cm, "container_error_copy_unlocalized_description");
	get_path = dlsym(cm, "container_get_path");
	primary = dlsym(cm, "CONTAINER_PERSONA_PRIMARY");
	if (!create || !set_class || !set_persona || !set_flags || !single || !last_error || !describe || !get_path || !primary) {
		printf("cmquery: libsystem_containermanager lacks a function\n");
		return 1;
	}
	printf("cmquery: uid %d, class %llu, flags 0x%llx, persona %s\n", getuid(), (unsigned long long)cls,
	    (unsigned long long)flags, *primary);
	query_t q = create();
	set_class(q, cls);
	set_persona(q, *primary);
	set_flags(q, flags);
	result_t r = single(q);
	if (r == NULL) {
		void *e = last_error(q);
		char *d = e ? describe(e) : NULL;

		printf("cmquery: NO RESULT: %s\n", d ? d : "(no error)");
		return 2;
	}
	printf("cmquery: path %s\n", get_path(r));
	return 0;
}
