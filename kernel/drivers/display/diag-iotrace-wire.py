#!/usr/bin/env python3
# darwin-lisa diagnostic: boot-arg iotrace=NAME logs what processes whose name begins with NAME ask of IOKit
# ("*" = everyone): service matching and notifications (the matching dictionary), registry paths, property reads
# (entry class + key + found or not) and user-client opens (service class, type, result). For finding out what the
# window server (backboardd) looks for on iOS 26 and does not find. At most iotrace_max lines (default 600).
# Idempotent; run in docker as root, after display-wire.py, then .work/run-rebuild.sh.
import sys

T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"
F = T + "/iokit/Kernel/IOUserClient.cpp"

s = open(F).read()
SKIP_IOTRACE = "darwin-lisa iotrace" in s
if SKIP_IOTRACE:
    print("  [skip] IOUserClient.cpp: iotrace already in")

HELPER = r'''
/* darwin-lisa iotrace (boot-arg iotrace=NAME, iotrace_max=N): what a process asks of IOKit. */
#include <pexpert/pexpert.h>
static char     gIOTraceProc[32];
static uint32_t gIOTraceMax = 600, gIOTraceLines;
static bool     gIOTraceInit;
static bool
iotraceOn(void)
{
	char name[32];

	if (!gIOTraceInit) {
		if (!PE_parse_boot_arg_str("iotrace", gIOTraceProc, sizeof(gIOTraceProc))) {
			gIOTraceProc[0] = 0;
		}
		PE_parse_boot_argn("iotrace_max", &gIOTraceMax, sizeof(gIOTraceMax));
		gIOTraceInit = true;
	}
	if (gIOTraceProc[0] == 0 || gIOTraceLines >= gIOTraceMax) {
		return false;
	}
	proc_selfname(name, sizeof(name));
	if (strcmp(gIOTraceProc, "*") != 0 && strncmp(name, gIOTraceProc, strlen(gIOTraceProc)) != 0) {
		return false;
	}
	gIOTraceLines++;
	return true;
}
/* a matching dictionary's XML, without its blanks, cut to fit a line */
static void
iotraceXML(const char * what, const char * xml, size_t size)
{
	char line[240], name[32];
	size_t n = 0;

	if (!iotraceOn()) {
		return;
	}
	for (size_t i = 0; xml != NULL && i < size && xml[i] != 0 && n + 1 < sizeof(line); i++) {
		if (xml[i] != '\n' && xml[i] != '\t' && !(xml[i] == ' ' && n > 0 && line[n - 1] == ' ')) {
			line[n++] = xml[i];
		}
	}
	line[n] = 0;
	proc_selfname(name, sizeof(name));
	IOLog("iotrace %s[%d] %s %s\n", name, proc_selfpid(), what, line);
}
static void
iotraceLine(const char * format, ...) __printflike(1, 2);
static void
iotraceLine(const char * format, ...)
{
	char line[240], name[32];
	va_list ap;

	if (!iotraceOn()) {
		return;
	}
	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	proc_selfname(name, sizeof(name));
	IOLog("iotrace %s[%d] %s\n", name, proc_selfpid(), line);
}

'''


def after_in_function(src, func_head, anchor, insert):
    i = src.find(func_head)
    if i < 0:
        print("  [FAIL] no", func_head)
        sys.exit(2)
    j = src.find(anchor, i)
    if j < 0:
        print("  [FAIL] no anchor in", func_head)
        sys.exit(2)
    j += len(anchor)
    print("  [ok]  ", func_head.strip().split("(")[0])
    return src[:j] + insert + src[j:]


if not SKIP_IOTRACE:
    head = "static kern_return_t\ninternal_io_service_get_matching_services("
    assert s.count(head) == 1
    s = s.replace(head, HELPER + head)

    s = after_in_function(s, "internal_io_service_get_matching_services(", "obj = OSUnserializeXML(matching, matching_size);\n",
                          "\tiotraceXML(\"matching-services\", matching, matching_size);\n")
    s = after_in_function(s, "internal_io_service_get_matching_service(", "obj = OSUnserializeXML(matching, matching_size);\n",
                          "\tiotraceXML(\"matching-service\", matching, matching_size);\n")
    s = after_in_function(s, "internal_io_service_add_notification(\n", "natural_t                   userMsgType;\n",
                          "\n\tiotraceXML(notification_type, matching, matching_size);\n")
    s = after_in_function(s, "is_io_registry_entry_from_path(\n", "entry = IORegistryEntry::fromPath( path );\n",
                          "\tiotraceLine(\"from-path %s -> %s\", path, entry ? \"found\" : \"NOT FOUND\");\n")
    s = after_in_function(s, "is_io_registry_entry_get_property_bin_buf(\n", "CHECK( IORegistryEntry, registry_entry, entry );\n",
                          "\t{\n\t\tOSObject * o = entry->copyProperty(property_name);\n"
                          "\t\tiotraceLine(\"property %s of %s: %s\", property_name, entry->getMetaClass()->getClassName(), o ? \"yes\" : \"none\");\n"
                          "\t\tOSSafeReleaseNULL(o);\n\t}\n")
    s = after_in_function(s, "is_io_service_open_extended(\n", "CHECK( IOService, _service, service );\n",
                          "\tiotraceLine(\"open %s (%s) type %u\", service->getMetaClass()->getClassName(), service->getName(), connect_type);\n")
    open(F, "w").write(s)

# the thread sampler (boot-arg psample=NAME): osfmk/kern/dlisa_sample.c, started from bsd_init
import os, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
shutil.copyfile(os.path.join(HERE, "dlisa_sample.c"), T + "/osfmk/kern/dlisa_sample.c")
of = T + "/osfmk/conf/files"
o = open(of).read()
if "osfmk/kern/dlisa_sample.c" not in o:
    open(of, "a").write("\n# darwin-lisa diagnostic: user thread sampler (boot-arg psample=NAME)\nosfmk/kern/dlisa_sample.c\t\tstandard\n")
b = T + "/bsd/kern/bsd_init.c"
bs = open(b).read()
old = "\t\t\thakios_display_init();  /* darwin-lisa: IOSurface + IOMobileFramebuffer (boot-arg display=1) */\n"
new = old + "\t\t\t{ extern void dlisa_sample_start(void); dlisa_sample_start(); }  /* darwin-lisa diag: psample=NAME */\n"
if new not in bs:
    assert bs.count(old) == 1
    open(b, "w").write(bs.replace(old, new))
print("  [ok]   dlisa_sample wired")
print("### IOTRACE WIRE DONE")

# syscall errors of a named process (boot-arg errtrace=NAME, errtrace_max=N, default 400): number, error, args, path.
SC = T + "/bsd/dev/arm/systemcalls.c"
sc = open(SC).read()
SC_ANCHOR = "\tAUDIT_SYSCALL_EXIT(code, proc, uthread, error);\n"
SC_ADD = """\t/* darwin-lisa diag errtrace: failing syscalls of a named process */
\tif (__improbable(error != 0 && error != EJUSTRETURN && error != ERESTART)) {
\t\tstatic char et_name[32];
\t\tstatic uint32_t et_max = 400, et_lines;
\t\tstatic int et_init;\n\t\tstatic uint32_t et_noent;
\t\tif (!et_init) {
\t\t\tif (!PE_parse_boot_arg_str("errtrace", et_name, sizeof(et_name))) {
\t\t\t\tet_name[0] = 0;
\t\t\t}
\t\t\tPE_parse_boot_argn("errtrace_max", &et_max, sizeof(et_max));\n\t\t\tPE_parse_boot_argn("errtrace_noent", &et_noent, sizeof(et_noent));
\t\t\tet_init = 1;
\t\t}
\t\tif (et_name[0] != 0 && et_lines < et_max && (error != ENOENT || et_noent) && strncmp(proc->p_comm, et_name, strlen(et_name)) == 0) {
\t\t\tchar et_path[96];
\t\t\tsize_t et_len = 0;
\t\t\tuser_addr_t et_pa = 0;
\t\t\tet_path[0] = 0;
\t\t\tswitch (code) {
\t\t\tcase 5: case 33: case 188: case 190: case 338: case 340: case 220: case 154: case 136: case 128: case 10: case 137: case 398: case 216:
\t\t\t\tet_pa = (user_addr_t)uthread->uu_arg[0]; break;
\t\t\tcase 463: case 469: case 472: case 466: case 461:
\t\t\t\tet_pa = (user_addr_t)uthread->uu_arg[1]; break;
\t\t\tdefault: break;
\t\t\t}
\t\t\tif (et_pa && copyinstr(et_pa, et_path, sizeof(et_path), &et_len) != 0) {
\t\t\t\tet_path[0] = '?'; et_path[1] = 0;
\t\t\t}
\t\t\tet_lines++;
\t\t\tprintf("errtrace %s[%d] sys %u e=%d a=0x%llx,0x%llx,0x%llx %s\\n", proc->p_comm, proc_getpid(proc), code, error,
\t\t\t    (unsigned long long)uthread->uu_arg[0], (unsigned long long)uthread->uu_arg[1],
\t\t\t    (unsigned long long)uthread->uu_arg[2], et_path);
\t\t}
\t}
"""
if "darwin-lisa diag errtrace" in sc:
    print("  [skip] systemcalls.c: errtrace already in")
else:
    assert sc.count(SC_ANCHOR) >= 1, "errtrace anchor"
    sc = sc.replace(SC_ANCHOR, SC_ANCHOR + SC_ADD, 1)
    if "#include <pexpert/pexpert.h>" not in sc:
        sc = "#include <pexpert/pexpert.h>\n" + sc
    open(SC, "w").write(sc)
    print("  [ok]   systemcalls.c: errtrace")

# abort_with_payload (521) / terminate_with_payload (520): print the reason string of every process (they never return).
CALL = "\terror = (*(callp->sy_call))(proc, &uthread->uu_arg[0], &(uthread->uu_rval[0]));\n"
ABORT_ADD = """\t/* darwin-lisa diag: why a process aborts (abort_with_reason / terminate_with_reason) */
\tif (__improbable(code == 521 || code == 520)) {
\t\tchar ab_why[160];
\t\tsize_t ab_len = 0;
\t\tuser_addr_t ab_str = (user_addr_t)uthread->uu_arg[code == 521 ? 4 : 5];
\t\tab_why[0] = 0;
\t\tif (ab_str == 0 || copyinstr(ab_str, ab_why, sizeof(ab_why), &ab_len) != 0) {
\t\t\tab_why[0] = '?'; ab_why[1] = 0;
\t\t}
\t\tprintf("DLISA abort %s[%d] ns %llu code 0x%llx: %s\\n", proc->p_comm, proc_getpid(proc),
\t\t    (unsigned long long)uthread->uu_arg[code == 521 ? 0 : 1], (unsigned long long)uthread->uu_arg[code == 521 ? 1 : 2], ab_why);
\t}
"""
sc = open(SC).read()
if "darwin-lisa diag: why a process aborts" in sc:
    print("  [skip] systemcalls.c: abort reasons already in")
else:
    assert sc.count(CALL) >= 1, "abort anchor"
    sc = sc.replace(CALL, ABORT_ADD + CALL, 1)
    open(SC, "w").write(sc)
    print("  [ok]   systemcalls.c: abort reasons")

# A process that aborts with plain abort() (pthread_kill(self, SIGABRT), syscall 328): its user stack, in the
# psample line format (symbolize with the "DLISA shared region slide" of the boot).
KILL_ADD = """\t/* darwin-lisa diag: the user stack of a thread that is about to SIGABRT itself */
\tif (__improbable(code == 328 && uthread->uu_arg[1] == 6)) {
\t\tchar kl[1024];
\t\tsize_t kn;
\t\tuint64_t kfp = get_saved_state_fp(state);
\t\tkn = (size_t)snprintf(kl, sizeof(kl), "psample %d t0 st0 hint0 x16=328 x0=0 x1=0 pc %llx lr %llx", proc_getpid(proc),
\t\t    (unsigned long long)get_saved_state_pc(state), (unsigned long long)get_saved_state_lr(state));
\t\tfor (int kf = 0; kf < 40 && kfp != 0 && (kfp & 7) == 0 && kn + 20 < sizeof(kl); kf++) {
\t\t\tuint64_t kfr[2];
\t\t\tif (copyin((user_addr_t)kfp, kfr, sizeof(kfr)) != 0) {
\t\t\t\tbreak;
\t\t\t}
\t\t\tkn += (size_t)snprintf(kl + kn, sizeof(kl) - kn, " %llx", (unsigned long long)kfr[1]);
\t\t\tif (kfr[0] <= kfp) {
\t\t\t\tbreak;
\t\t\t}
\t\t\tkfp = kfr[0];
\t\t}
\t\tprintf("psample %s[%d]: 1 threads, slide abort\\n%s\\n", proc->p_comm, proc_getpid(proc), kl);
\t}
"""
sc = open(SC).read()
if "the user stack of a thread that is about to SIGABRT itself" in sc:
    print("  [skip] systemcalls.c: abort stacks already in")
else:
    sc = sc.replace(CALL, KILL_ADD + CALL, 1)
    open(SC, "w").write(sc)
    print("  [ok]   systemcalls.c: abort stacks")
