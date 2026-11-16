/*
 * libdlisa: a blocking call given a deadline.
 *
 * runBounded(call, context, ms) runs call(context) on a thread of its own and waits at most ms milliseconds. True when
 * the call finished in time. When it did not, the caller goes on and the call is left to end (or never end) on its
 * thread; the shared state is freed by whichever side is last, so a late call never writes into the caller's stack.
 * The context must therefore outlive a late call: the callers in dlisa-inject.c give a heap context.
 *
 * Why (PosterBoard, host run poster-f3): donotdisturbd never accepts a BSServiceConnection on this port (every client,
 * SpringBoard included, stays at "Pending event until connection established"). Focus's FCActivityManager asks it
 * synchronously (-[DNDModeSelectionService activeModeAssertionWithError:]) while PosterBoard holds its data store's
 * state lock in -_stateLock_initialRoleCoordinatorSetup, so PosterBoard never processes the roles' initial events,
 * never creates the default lock-screen poster, and goes silent.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

struct boundedCall {
	void (*call)(void *);
	void *context;
	pthread_mutex_t lock;
	pthread_cond_t done;
	bool finished;
	int owners;          /* the caller and the thread; the last one frees */
};

static void
releaseBoundedCall(struct boundedCall *state)
{
	bool last;

	pthread_mutex_lock(&state->lock);
	last = --state->owners == 0;
	pthread_mutex_unlock(&state->lock);
	if (last) {
		pthread_cond_destroy(&state->done);
		pthread_mutex_destroy(&state->lock);
		free(state);
	}
}

static void *
runBoundedThread(void *argument)
{
	struct boundedCall *state = argument;

	state->call(state->context);
	pthread_mutex_lock(&state->lock);
	state->finished = true;
	pthread_cond_signal(&state->done);
	pthread_mutex_unlock(&state->lock);
	releaseBoundedCall(state);
	return NULL;
}

static struct timespec
deadlineAfter(unsigned milliseconds)
{
	struct timespec deadline;

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += milliseconds / 1000;
	deadline.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	return deadline;
}

static bool
runBounded(void (*call)(void *), void *context, unsigned milliseconds)
{
	struct boundedCall *state = calloc(1, sizeof(*state));
	struct timespec deadline = deadlineAfter(milliseconds);
	pthread_t thread;
	bool finished;

	if (state == NULL) {
		call(context);
		return true;
	}
	state->call = call;
	state->context = context;
	state->owners = 2;
	pthread_mutex_init(&state->lock, NULL);
	pthread_cond_init(&state->done, NULL);
	if (pthread_create(&thread, NULL, runBoundedThread, state) != 0) {
		free(state);
		call(context);
		return true;
	}
	pthread_detach(thread);
	pthread_mutex_lock(&state->lock);
	while (!state->finished && pthread_cond_timedwait(&state->done, &state->lock, &deadline) != ETIMEDOUT) {
	}
	finished = state->finished;
	pthread_mutex_unlock(&state->lock);
	releaseBoundedCall(state);
	return finished;
}
