#!/usr/bin/env python3
# darwin-lisa: two exec-time aids from HakiOS (xnu-8796 hakios.patch, kern_exec.c), ported to xnu-12377.
#
# 1. sysctl kern.exec_inject = "LIBRARY NAME NAME ... [; LIBRARY NAME ...]": every program whose file name is
#    one of the NAMEs is started with DYLD_INSERT_LIBRARIES=LIBRARY (several libraries joined with ":").
#    launchd does not pass DYLD_ variables from a job description, so the kernel adds this one. init26 sets it
#    from /etc/dlisa-inject.conf before it hands over to launchd. miniamfi already lets dyld honour it.
# 2. boot-arg applog=1 applog_name=A,B,...: the programs whose names begin with A or B get OS_ACTIVITY_DT_MODE=YES
#    (os_log to stderr as well); applog=2 gives it to everybody (slow). libdlisa.dylib points stderr at the console
#    in the programs it is inserted into. applog_console=1 also sends a spawned program's stdout/stderr that launchd
#    opens on /dev/null to /dev/console, as HakiOS did; on iOS 26 that makes every launchd spawn fail with EACCES
#    (xpcproxy: "posix_spawn ... error 0xd"), so it is off unless asked for.
#
# Idempotent; run inside the toolchain container as root, then .work/run-rebuild.sh. --revert supported.
import sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
F = T + "/bsd/kern/kern_exec.c"

MARK = "darwin-lisa execaids"

HELPERS = r'''/*
 * darwin-lisa execaids (from HakiOS): kern.exec_inject and boot-args applog / applog_name.
 * See darwin-lisa/kernel/execaids/execaids-wire.py.
 */
static char exec_inject_conf[1024];
SYSCTL_STRING(_kern, OID_AUTO, exec_inject, CTLFLAG_RW | CTLFLAG_LOCKED, exec_inject_conf, sizeof(exec_inject_conf),
    "LIBRARY NAME... [; LIBRARY NAME...]: programs started with the library inserted");

static bool
exec_inject_env(struct image_params *imgp, char *env, size_t size)
{
	const char *name, *p = exec_inject_conf;
	size_t namelen, used;
	bool any = false;

	if (*p == 0 || imgp->ip_ndp == NULL) {
		return false;
	}
	name = imgp->ip_ndp->ni_cnd.cn_nameptr;
	namelen = imgp->ip_ndp->ni_cnd.cn_namelen;
	used = strlcpy(env, "DYLD_INSERT_LIBRARIES=", size);
	while (*p != 0) {
		const char *library;
		size_t liblen;
		bool matched = false;

		while (*p == ' ' || *p == ';') {
			p++;
		}
		library = p;
		while (*p != 0 && *p != ' ' && *p != ';') {
			p++;
		}
		liblen = (size_t)(p - library);
		while (*p == ' ') {
			const char *word = ++p;

			while (*p != 0 && *p != ' ' && *p != ';') {
				p++;
			}
			if (!matched && (size_t)(p - word) == namelen && namelen != 0 && liblen != 0 &&
			    strncmp(word, name, namelen) == 0) {
				matched = true;
			}
		}
		if (matched && liblen != 0 && used + 1 + liblen < size) {
			if (any) {
				env[used++] = ':';
			}
			memcpy(env + used, library, liblen);
			used += liblen;
			env[used] = 0;
			any = true;
		}
	}
	return any;
}

static TUNABLE(uint32_t, exec_applog, "applog", 0);
static TUNABLE(uint32_t, exec_applog_console, "applog_console", 0);
static TUNABLE_STR(exec_applog_name, 128, "applog_name", "");

static bool
exec_applog_oslog(struct image_params *imgp)
{
	const char *name, *p = exec_applog_name;

	if (exec_applog >= 2) {
		return true;
	}
	if (exec_applog == 0 || *p == 0 || imgp->ip_ndp == NULL) {
		return false;
	}
	name = imgp->ip_ndp->ni_cnd.cn_nameptr;
	while (*p != 0) {
		const char *word = p;

		while (*p != 0 && *p != ',') {
			p++;
		}
		if (p != word && (size_t)(p - word) <= imgp->ip_ndp->ni_cnd.cn_namelen &&
		    strncmp(word, name, (size_t)(p - word)) == 0) {
			return true;
		}
		if (*p == ',') {
			p++;
		}
	}
	return false;
}

static const char *
exec_applog_path(int fd, const char *path)
{
	if (exec_applog_console && (fd == 1 || fd == 2) && strcmp(path, "/dev/null") == 0) {
		return "/dev/console";
	}
	return path;
}

static int
exec_add_kernel_env(struct image_params *imgp, const char *env, int new_ptr_size)
{
	int error = exec_add_user_string(imgp, CAST_USER_ADDR_T(env), UIO_SYSSPACE, TRUE);

	if (error) {
		return error;
	}
	if (imgp->ip_argspace < new_ptr_size) {
		return E2BIG;
	}
	imgp->ip_argspace -= new_ptr_size;
	imgp->ip_envc++;
	return 0;
}

'''

EDITS = [
    # helpers, before exec_handle_file_actions (the first user)
    ("static int\nexec_handle_file_actions(struct image_params *imgp, short psa_flags)\n{",
     HELPERS + "static int\nexec_handle_file_actions(struct image_params *imgp, short psa_flags)\n{"),
    # posix_spawn file actions: /dev/null on stdout/stderr -> /dev/console with applog
    ("\t\t\t    CAST_USER_ADDR_T(psfa->psfaa_openargs.psfao_path),\n\t\t\t    imgp->ip_vfs_context);\n\n\t\t\terror = open1(",
     "\t\t\t    CAST_USER_ADDR_T(exec_applog_path(psfa->psfaa_filedes, psfa->psfaa_openargs.psfao_path)),\n"
     "\t\t\t    imgp->ip_vfs_context);\n\n\t\t\terror = open1("),
    # the two extra environment strings
    ("\t/* Save space for envv[] NULL terminator */\n",
     "\t/* darwin-lisa execaids: DYLD_INSERT_LIBRARIES (kern.exec_inject), OS_ACTIVITY_DT_MODE (applog) */\n"
     "\t{\n"
     "\t\tchar inject_env[1024];\n\n"
     "\t\tif (exec_inject_env(imgp, inject_env, sizeof(inject_env))) {\n"
     "\t\t\terror = exec_add_kernel_env(imgp, inject_env, new_ptr_size);\n"
     "\t\t\tif (error) {\n\t\t\t\tgoto bad;\n\t\t\t}\n\t\t}\n"
     "\t\tif (exec_applog_oslog(imgp)) {\n"
     "\t\t\terror = exec_add_kernel_env(imgp, \"OS_ACTIVITY_DT_MODE=YES\", new_ptr_size);\n"
     "\t\t\tif (error) {\n\t\t\t\tgoto bad;\n\t\t\t}\n\t\t}\n"
     "\t}\n\n"
     "\t/* Save space for envv[] NULL terminator */\n"),
]

s = open(F).read()
revert = "--revert" in sys.argv
if revert:
    if MARK not in s:
        print("execaids: not applied")
        sys.exit(0)
    for old, new in EDITS:
        s = s.replace(new, old, 1)
elif MARK in s:
    print("execaids: already applied")
    sys.exit(0)
else:
    for old, new in EDITS:
        if s.count(old) != 1:
            print("execaids: anchor not unique/missing:", repr(old[:60]))
            sys.exit(1)
    for old, new in EDITS:
        s = s.replace(old, new, 1)
open(F, "w").write(s)
print("execaids: reverted" if revert else "execaids: patched")
