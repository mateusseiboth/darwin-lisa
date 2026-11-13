/*
 * libdlisa, backboardd: the scripted finger scroll of boot-arg dlisa_scroll.
 *
 * The kernel's `scr` is a fling: 20 moves 16 ms apart and a lift with the finger still at speed, so after 0.3 s every
 * frame is the scroll view's own deceleration. A finger that reads a list drags it, at the panel's report rate, and
 * stops before lifting. A stroke here is that: down in the middle of the screen, 150 moves 8 ms apart (1.2 s), held
 * 20 samples (160 ms) at the end, then up. Strokes alternate up and down over the same 38 % of the screen, so the
 * list ends where it started. Coordinates are /dev/hakios-touch's, 0..65535 with 0 at the top.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STROKE_SAMPLE_MS        8
#define STROKE_MOVES            150
#define STROKE_HELD             20
#define STROKE_X                32768
#define STROKE_LOW_Y            45000   /* 69 % down */
#define STROKE_HIGH_Y           20000   /* 31 % down */

#define STROKE_DOWN             1       /* /dev/hakios-touch's types */
#define STROKE_MOVE             2
#define STROKE_UP               3

struct strokePlan {
	uint32_t x, fromY, toY;
	uint32_t moves, held;
};

struct strokeSample {
	uint32_t type, x, y;
};

/* stroke 0, 2, 4...: the finger goes up the screen (the list scrolls on); 1, 3, 5...: back down */
static struct strokePlan
buildStrokePlan(unsigned index)
{
	bool isUp = index % 2 == 0;

	return (struct strokePlan){
		.x = STROKE_X,
		.fromY = isUp ? STROKE_LOW_Y : STROKE_HIGH_Y,
		.toY = isUp ? STROKE_HIGH_Y : STROKE_LOW_Y,
		.moves = STROKE_MOVES,
		.held = STROKE_HELD,
	};
}

static size_t
strokeSampleCount(const struct strokePlan *plan)
{
	return (size_t)plan->moves + plan->held + 2;
}

static uint32_t
strokeYAt(const struct strokePlan *plan, size_t move)
{
	int64_t span = (int64_t)plan->toY - plan->fromY;

	return (uint32_t)((int64_t)plan->fromY + span * (int64_t)move / plan->moves);
}

/* sample k: 0 lands, 1..moves travel, then the held samples at the end, and the last one lifts */
static struct strokeSample
strokeSampleAt(const struct strokePlan *plan, size_t k)
{
	size_t last = strokeSampleCount(plan) - 1;

	if (k == 0) {
		return (struct strokeSample){ STROKE_DOWN, plan->x, plan->fromY };
	}
	if (k >= last) {
		return (struct strokeSample){ STROKE_UP, plan->x, plan->toY };
	}
	if (k > plan->moves) {
		return (struct strokeSample){ STROKE_MOVE, plan->x, plan->toY };
	}
	return (struct strokeSample){ STROKE_MOVE, plan->x, strokeYAt(plan, k) };
}
