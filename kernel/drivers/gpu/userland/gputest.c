/*
 * gputest: the GPU checks of one boot, in order, under one init_run= entry (the boot-arg line is short: the loader
 * keeps 607 characters, its own words included). Run by init26 before launchd:
 *
 *   init_run=/usr/libexec/usermanagerd:--init;/usr/local/bin/gputest init_run_secs=150
 *
 * 1. /usr/local/bin/hakios-metal-probe   the GPU through the kernel's HakiosGPU: copies, fills, kernels, triangles
 * 2. /usr/local/bin/metal_test           Metal's discovery chain and the device it gives (with metal=1
 *                                        metal_plugin=HakiosMetalProbe the device is shown to metal_test alone)
 *
 * Each runs for at most its own share of the time (PROBE_SECS, TEST_SECS) and is killed after it. One line per program
 * with its exit status ("gputest: ... exit status N"), and a last one with both. Their own output goes to the console
 * as it comes. Nothing else is done: no file is written.
 */
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define PROBE_SECS 90
#define TEST_SECS  45

static int
run(const char *path, unsigned secs)
{
	char *argv[] = { (char *)path, NULL };
	pid_t pid;
	int status = 0, err = posix_spawn(&pid, path, NULL, NULL, argv, environ);

	if (err != 0) {
		printf("gputest: cannot start %s: %s\n", path, strerror(err));
		return -1;
	}
	printf("gputest: %s started (pid %d, %u s at most)\n", path, pid, secs);
	fflush(stdout);
	for (unsigned t = 0; t < secs * 10; t++) {
		pid_t done = waitpid(pid, &status, WNOHANG);

		if (done == pid) {
			printf("gputest: %s: exit status %d%s\n", path, WIFEXITED(status) ? WEXITSTATUS(status) : -1,
			    WIFSIGNALED(status) ? " (killed by a signal)" : "");
			fflush(stdout);
			return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		}
		usleep(100000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	printf("gputest: %s: still running after %u s, killed\n", path, secs);
	fflush(stdout);
	return -1;
}

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	int probe = run("/usr/local/bin/hakios-metal-probe", PROBE_SECS);
	int test = run("/usr/local/bin/metal_test", TEST_SECS);

	printf("gputest: done: hakios-metal-probe %d (its failures), metal_test %d\n", probe, test);
	return probe == 0 && test == 0 ? 0 : 1;
}
