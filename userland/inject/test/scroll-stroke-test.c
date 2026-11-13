/* Host test of scroll-stroke.c (libdlisa in backboardd: the scripted finger scroll of boot-arg dlisa_scroll) */
#include <stdio.h>

#include "scroll-stroke.c"

static int failures;

static void
expect(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

static void
expectStroke(unsigned index)
{
	struct strokePlan plan = buildStrokePlan(index);
	size_t count = strokeSampleCount(&plan);
	struct strokeSample first = strokeSampleAt(&plan, 0), last = strokeSampleAt(&plan, count - 1);
	uint32_t lastY = first.y;
	bool monotonic = true, heldStill = true, onlyMoves = true;

	expect(first.type == STROKE_DOWN && first.y == plan.fromY, "a stroke lands where it starts");
	expect(last.type == STROKE_UP && last.y == plan.toY, "and lifts where it ends");
	for (size_t k = 1; k + 1 < count; k++) {
		struct strokeSample s = strokeSampleAt(&plan, k);

		onlyMoves &= s.type == STROKE_MOVE;
		monotonic &= plan.toY < plan.fromY ? s.y <= lastY : s.y >= lastY;
		if (k > plan.moves) {
			heldStill &= s.y == plan.toY;
		}
		lastY = s.y;
	}
	expect(onlyMoves, "between landing and lifting there are only moves");
	expect(monotonic, "the finger goes one way, never back");
	expect(strokeSampleAt(&plan, plan.moves).y == plan.toY, "the last travelling move reaches the end");
	expect(heldStill, "then it stays put before lifting (no fling: every frame of the stroke is the finger's)");
	expect(plan.x == STROKE_X, "the stroke runs down the middle of the screen");
}

int
main(void)
{
	struct strokePlan up = buildStrokePlan(0), down = buildStrokePlan(1);

	expect(up.toY < up.fromY, "stroke 0 drags the list up");
	expect(down.fromY == up.toY && down.toY == up.fromY, "stroke 1 drags it back down the same way");
	expect(buildStrokePlan(2).fromY == up.fromY, "and so on, alternating");
	expectStroke(0);
	expectStroke(1);
	/* a finger on the panel reports every 8 ms or so: the stroke moves at that rate, for over a second */
	expect(STROKE_SAMPLE_MS == 8, "a sample every 8 ms, as the panel reports");
	expect(up.moves * STROKE_SAMPLE_MS >= 1000, "each stroke travels for at least a second");
	expect(up.held * STROKE_SAMPLE_MS >= 150, "and is held for at least 150 ms before lifting");
	expect(strokeSampleCount(&up) == up.moves + up.held + 2, "landing, the moves, the held samples and the lift");
	/* the start at the middle of the list, well inside the screen: no edge gesture of SpringBoard's takes it */
	expect(up.fromY > 0.1 * 65535 && up.fromY < 0.9 * 65535 && up.toY > 0.1 * 65535, "away from the screen's edges");
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}
