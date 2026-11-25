// Behaviour tests for the debounce state machine (click bounce, drag protection
// and wheel reversal). These run without any Windows hook, feeding synthetic
// events straight into the manager.
//
// Timestamps are derived from GetTickCount64() because the manager itself uses
// that clock when it injects a deferred release; mixing clocks would make the
// sequence look like it came from the past.

#include <stdio.h>

#include "test_common.h"
#include "../src/core/config.h"
#include "../src/core/debouncer.h"

#define THRESHOLD_MS 50
#define WHEEL_THRESHOLD_MS 30

// Time base for all synthetic events
static uint64_t g_base = 0;

// Recorded injections, so the replay can be asserted without clicking anything
static INPUT g_injected[4];
static int g_injected_count = 0;

static bool record_injection(const INPUT *inputs, UINT count)
{
	g_injected_count = (int)count;

	for (UINT i = 0; i < count && i < ARRAYSIZE(g_injected); i++)
		g_injected[i] = inputs[i];

	return true;
}

// Build a synthetic event, offset in milliseconds from the time base
static MouseEvent make_event(MouseButton button, uint64_t offset, bool is_down, long x, long y, int32_t data)
{
	MouseEvent event = {0};
	event.button = button;
	event.timestamp = g_base + offset;
	event.is_down = is_down;
	event.x = x;
	event.y = y;
	event.data = data;
	return event;
}

// Reset the per-button transient state without touching the configuration
static void reset_state(DebounceManager *manager)
{
	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		manager->buttons[i].previousTime = 0;
		manager->buttons[i].downTime = 0;
		manager->buttons[i].isBlocked = false;
		manager->buttons[i].isDeferringRelease = false;
		manager->buttons[i].pending_replay = false;
		manager->buttons[i].replay_in_flight = false;
		manager->buttons[i].release_after_replay = false;
		manager->buttons[i].wheelDirection = 0;
	}
}

void test_smart_drag_run(void)
{
	printf("debounce state machine\n");

	g_base = GetTickCount64();

	// Nothing in this file needs the real input queue, and a test that clicks the
	// desktop it happens to run on is a test nobody dares to run.
	debounce_set_injection_sink(record_injection);

	DebounceManager manager;

	if (!debounce_init(&manager))
	{
		printf("[FAIL] debounce_init\n");
		g_failures++;
		return;
	}

	debounce_set_monitored(&manager, MOUSE_BUTTON_LEFT, true);
	debounce_set_threshold(&manager, MOUSE_BUTTON_LEFT, THRESHOLD_MS, 1, 200);
	debounce_set_monitored(&manager, MOUSE_BUTTON_WHEEL, true);
	debounce_set_threshold(&manager, MOUSE_BUTTON_WHEEL, WHEEL_THRESHOLD_MS, 1, 200);
	debounce_set_hybrid_heuristic(&manager, true);

	// ------------------------------------------------------------------
	// A normal click must pass through untouched
	// ------------------------------------------------------------------
	reset_state(&manager);

	MouseEvent down = make_event(MOUSE_BUTTON_LEFT, 1000, true, 100, 100, 0);
	MouseEvent up = make_event(MOUSE_BUTTON_LEFT, 1060, false, 100, 100, 0);

	CHECK(!debounce_process_event(&manager, &down));
	CHECK(!debounce_process_event(&manager, &up));
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].isBlocked);

	// ------------------------------------------------------------------
	// A bounce (press shortly after a release) must be swallowed together
	// with its own release
	// ------------------------------------------------------------------
	reset_state(&manager);

	MouseEvent down1 = make_event(MOUSE_BUTTON_LEFT, 2000, true, 100, 100, 0);
	MouseEvent up1 = make_event(MOUSE_BUTTON_LEFT, 2010, false, 100, 100, 0);
	MouseEvent down2 = make_event(MOUSE_BUTTON_LEFT, 2020, true, 100, 100, 0);
	MouseEvent up2 = make_event(MOUSE_BUTTON_LEFT, 2030, false, 100, 100, 0);
	MouseEvent down3 = make_event(MOUSE_BUTTON_LEFT, 2200, true, 100, 100, 0);

	CHECK(!debounce_process_event(&manager, &down1));
	CHECK(!debounce_process_event(&manager, &up1));
	CHECK(debounce_process_event(&manager, &down2));
	CHECK(debounce_process_event(&manager, &up2));
	CHECK(!debounce_process_event(&manager, &down3));

	// ------------------------------------------------------------------
	// A long press that never moved is a long click, not a drag: the release
	// goes through at once. Holding it keeps the system believing the button is
	// down, and everything the pointer does in that time becomes a drag - that
	// is what users felt as the button sticking.
	// ------------------------------------------------------------------
	reset_state(&manager);

	MouseEvent long_down = make_event(MOUSE_BUTTON_LEFT, 3000, true, 100, 100, 0);
	MouseEvent long_up = make_event(MOUSE_BUTTON_LEFT, 3300, false, 100, 100, 0);

	CHECK(!debounce_process_event(&manager, &long_down));
	CHECK(!debounce_process_event(&manager, &long_up));
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);
	CHECK(!debounce_has_pending_deferrals(&manager));

	// ------------------------------------------------------------------
	// A drag is long AND moved. Only then is the release held back, so that a
	// chattering switch cannot end the drag early.
	// ------------------------------------------------------------------
	reset_state(&manager);

	MouseEvent drag_down = make_event(MOUSE_BUTTON_LEFT, 3400, true, 100, 100, 0);
	MouseEvent drag_up = make_event(MOUSE_BUTTON_LEFT, 3700, false, 140, 100, 0);

	CHECK(!debounce_process_event(&manager, &drag_down));
	CHECK(debounce_process_event(&manager, &drag_up));
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);
	CHECK(debounce_has_pending_deferrals(&manager));

	// A new press while a release is held back is swallowed and replayed by the
	// timer together with that release. It is deliberately not passed straight
	// through: the release has to reach the system first, and injecting it from
	// here - inside the hook callback - is what used to freeze the whole process.
	MouseEvent drag_down2 = make_event(MOUSE_BUTTON_LEFT, 3720, true, 140, 100, 0);
	CHECK(debounce_process_event(&manager, &drag_down2));
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].pending_replay);
	CHECK(!debounce_has_pending_deferrals(&manager));

	// The timer has to keep running for the replay to happen at all
	CHECK(debounce_has_pending_work(&manager));

	debounce_drop_replays(&manager);
	CHECK(!debounce_has_pending_work(&manager));

	// ------------------------------------------------------------------
	// A short press that moved is a flick, not a drag: released at once
	// ------------------------------------------------------------------
	reset_state(&manager);

	MouseEvent flick_down = make_event(MOUSE_BUTTON_LEFT, 4000, true, 100, 100, 0);
	MouseEvent flick_up = make_event(MOUSE_BUTTON_LEFT, 4060, false, 140, 100, 0);

	CHECK(!debounce_process_event(&manager, &flick_down));
	CHECK(!debounce_process_event(&manager, &flick_up));
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);
	CHECK(!debounce_has_pending_deferrals(&manager));

	// Disabling the hybrid heuristic must deliver a pending release
	reset_state(&manager);

	MouseEvent held_down = make_event(MOUSE_BUTTON_LEFT, 5000, true, 100, 100, 0);
	MouseEvent held_up = make_event(MOUSE_BUTTON_LEFT, 5300, false, 140, 100, 0);

	CHECK(!debounce_process_event(&manager, &held_down));
	CHECK(debounce_process_event(&manager, &held_up));
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);

	debounce_set_hybrid_heuristic(&manager, false);
	CHECK(!debounce_has_pending_deferrals(&manager));

	// ------------------------------------------------------------------
	// Wheel: a direction reversal inside the threshold is blocked,
	// a reversal after the threshold is not
	// ------------------------------------------------------------------
	reset_state(&manager);

	MouseEvent wheel_down = make_event(MOUSE_BUTTON_WHEEL, 5000, true, 0, 0, -120);
	MouseEvent wheel_up_fast = make_event(MOUSE_BUTTON_WHEEL, 5010, true, 0, 0, 120);
	MouseEvent wheel_up_slow = make_event(MOUSE_BUTTON_WHEEL, 6000, true, 0, 0, 120);

	CHECK(!debounce_process_event(&manager, &wheel_down));
	CHECK(debounce_process_event(&manager, &wheel_up_fast));
	CHECK(!debounce_process_event(&manager, &wheel_up_slow));

	// A zero delta must be ignored even when the direction is set
	MouseEvent wheel_zero = make_event(MOUSE_BUTTON_WHEEL, 6010, true, 0, 0, 0);
	CHECK(!debounce_process_event(&manager, &wheel_zero));

	// ------------------------------------------------------------------
	// Unmonitored buttons and injected events never get processed
	// ------------------------------------------------------------------
	debounce_set_monitored(&manager, MOUSE_BUTTON_RIGHT, false);

	MouseEvent unmonitored = make_event(MOUSE_BUTTON_RIGHT, 7000, true, 0, 0, 0);
	CHECK(!debounce_process_event(&manager, &unmonitored));

	MouseEvent injected = make_event(MOUSE_BUTTON_LEFT, 8000, true, 0, 0, 0);
	injected.is_injected = true;
	CHECK(!debounce_process_event(&manager, &injected));

	// Out-of-range button indices must be rejected without touching memory
	MouseEvent invalid = make_event((MouseButton)MOUSE_BUTTON_COUNT, 9000, true, 0, 0, 0);
	CHECK(!debounce_process_event(&manager, &invalid));

	// ------------------------------------------------------------------
	// Switching a button off must deliver its swallowed release and must not
	// leave transient state behind
	// ------------------------------------------------------------------
	debounce_set_hybrid_heuristic(&manager, true);
	debounce_set_monitored(&manager, MOUSE_BUTTON_LEFT, true);
	reset_state(&manager);

	// Long and moved: a drag, so the release really is held back here
	MouseEvent off_down = make_event(MOUSE_BUTTON_LEFT, 10000, true, 100, 100, 0);
	MouseEvent off_up = make_event(MOUSE_BUTTON_LEFT, 10400, false, 140, 100, 0);

	CHECK(!debounce_process_event(&manager, &off_down));
	CHECK(debounce_process_event(&manager, &off_up));
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);

	debounce_set_monitored(&manager, MOUSE_BUTTON_LEFT, false);
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);
	CHECK(!debounce_has_pending_deferrals(&manager));
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].isBlocked);

	// Re-enabling starts from a clean slate: a normal click is not blocked
	debounce_set_monitored(&manager, MOUSE_BUTTON_LEFT, true);

	MouseEvent clean_down = make_event(MOUSE_BUTTON_LEFT, 12000, true, 100, 100, 0);
	MouseEvent clean_up = make_event(MOUSE_BUTTON_LEFT, 12060, false, 100, 100, 0);

	CHECK(!debounce_process_event(&manager, &clean_down));
	CHECK(!debounce_process_event(&manager, &clean_up));
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);

	// ------------------------------------------------------------------
	// Resetting statistics during a deferral must deliver the release too
	// ------------------------------------------------------------------
	reset_state(&manager);

	MouseEvent reset_down = make_event(MOUSE_BUTTON_LEFT, 14000, true, 100, 100, 0);
	MouseEvent reset_up = make_event(MOUSE_BUTTON_LEFT, 14400, false, 140, 100, 0);

	CHECK(!debounce_process_event(&manager, &reset_down));
	CHECK(debounce_process_event(&manager, &reset_up));
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);

	debounce_reset_statistics(&manager);
	CHECK(!debounce_has_pending_deferrals(&manager));
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);
	CHECK(debounce_get_total_blocks(&manager) == 0);

	// ------------------------------------------------------------------
	// Turning every button off during a deferral is safe as well
	// ------------------------------------------------------------------
	reset_state(&manager);

	MouseEvent all_down = make_event(MOUSE_BUTTON_LEFT, 16000, true, 100, 100, 0);
	MouseEvent all_up = make_event(MOUSE_BUTTON_LEFT, 16400, false, 140, 100, 0);

	CHECK(!debounce_process_event(&manager, &all_down));
	CHECK(debounce_process_event(&manager, &all_up));
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);

	debounce_set_all_monitored(&manager, false);
	CHECK(!debounce_has_pending_deferrals(&manager));
	CHECK(!debounce_is_any_monitored(&manager));

	debounce_set_all_monitored(&manager, true);
	CHECK(debounce_is_all_monitored(&manager));
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].isBlocked);

	// ------------------------------------------------------------------
	// Smart Drag tuning: defaults, accepted values and rejected values
	// ------------------------------------------------------------------
	CHECK(manager.releaseDelayMs == RELEASE_DELAY_DEFAULT_MS);
	CHECK(manager.dragHoldTimeMs == DRAG_HOLD_TIME_DEFAULT_MS);
	CHECK(manager.dragDistancePx == DRAG_DISTANCE_DEFAULT_PX);

	// The delay is time the system spends still believing the button is down, so
	// the default has to stay short and has to be offered in the menu as is:
	// otherwise the smallest thing a user can pick is worse than the default.
	CHECK(RELEASE_DELAY_DEFAULT_MS == 20);
	CHECK(config_release_delay_presets()[0] == RELEASE_DELAY_DEFAULT_MS);

	debounce_set_release_delay(&manager, 80, RELEASE_DELAY_MIN_MS, RELEASE_DELAY_MAX_MS);
	CHECK(manager.releaseDelayMs == 80);

	// Out of range values must be ignored, not clamped to something else
	debounce_set_release_delay(&manager, 500, RELEASE_DELAY_MIN_MS, RELEASE_DELAY_MAX_MS);
	CHECK(manager.releaseDelayMs == 80);

	debounce_set_drag_distance(&manager, 10, DRAG_DISTANCE_MIN_PX, DRAG_DISTANCE_MAX_PX);
	CHECK(manager.dragDistancePx == 10);

	debounce_set_drag_distance(&manager, 0, DRAG_DISTANCE_MIN_PX, DRAG_DISTANCE_MAX_PX);
	CHECK(manager.dragDistancePx == 10);

	// Both presses here are long, so only the distance decides. A larger drag
	// distance must not defer a 6 px release any more.
	reset_state(&manager);
	debounce_set_hybrid_heuristic(&manager, true);

	MouseEvent wide_down = make_event(MOUSE_BUTTON_LEFT, 18000, true, 100, 100, 0);
	MouseEvent wide_up = make_event(MOUSE_BUTTON_LEFT, 18400, false, 106, 100, 0);

	CHECK(!debounce_process_event(&manager, &wide_down));
	CHECK(!debounce_process_event(&manager, &wide_up));
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);

	// Tightening it back to 3 px must defer the same movement
	debounce_set_drag_distance(&manager, 3, DRAG_DISTANCE_MIN_PX, DRAG_DISTANCE_MAX_PX);
	reset_state(&manager);

	MouseEvent tight_down = make_event(MOUSE_BUTTON_LEFT, 19000, true, 100, 100, 0);
	MouseEvent tight_up = make_event(MOUSE_BUTTON_LEFT, 19400, false, 106, 100, 0);

	CHECK(!debounce_process_event(&manager, &tight_down));
	CHECK(debounce_process_event(&manager, &tight_up));
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);

	// ------------------------------------------------------------------
	// A queued replay is dropped when the user lets go before it runs
	// ------------------------------------------------------------------
	reset_state(&manager);
	debounce_set_defer_available(&manager, true);

	MouseEvent replay_down = make_event(MOUSE_BUTTON_LEFT, 20000, true, 100, 100, 0);
	MouseEvent replay_up = make_event(MOUSE_BUTTON_LEFT, 20300, false, 140, 100, 0);
	MouseEvent replay_press = make_event(MOUSE_BUTTON_LEFT, 20340, true, 140, 100, 0);

	CHECK(!debounce_process_event(&manager, &replay_down));
	CHECK(debounce_process_event(&manager, &replay_up));
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);

	CHECK(debounce_process_event(&manager, &replay_press));
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].pending_replay);

	// The release arrives before the timer ever ran. Replaying the press now
	// would leave the button held with nothing left to release it.
	MouseEvent replay_release = make_event(MOUSE_BUTTON_LEFT, 20350, false, 100, 100, 0);
	debounce_process_event(&manager, &replay_release);
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].pending_replay);
	CHECK(!debounce_has_pending_work(&manager));

	// ------------------------------------------------------------------
	// With no timer to give a release back, nothing may be held back
	// ------------------------------------------------------------------
	reset_state(&manager);
	debounce_set_defer_available(&manager, false);

	MouseEvent nohold_down = make_event(MOUSE_BUTTON_LEFT, 21000, true, 100, 100, 0);
	MouseEvent nohold_up = make_event(MOUSE_BUTTON_LEFT, 21300, false, 100, 100, 0);

	CHECK(!debounce_process_event(&manager, &nohold_down));
	CHECK(!debounce_process_event(&manager, &nohold_up));
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);
	CHECK(!debounce_has_pending_work(&manager));

	debounce_set_defer_available(&manager, true);

	// ------------------------------------------------------------------
	// The replay itself: what actually reaches the system, and in what order
	// ------------------------------------------------------------------
	reset_state(&manager);
	g_injected_count = 0;

	// Long and moved, so this release is held back and there is something to replay
	MouseEvent replay2_down = make_event(MOUSE_BUTTON_LEFT, 22000, true, 300, 200, 0);
	MouseEvent replay2_up = make_event(MOUSE_BUTTON_LEFT, 22300, false, 340, 200, 0);
	MouseEvent replay2_press = make_event(MOUSE_BUTTON_LEFT, 22340, true, 305, 205, 0);

	CHECK(!debounce_process_event(&manager, &replay2_down));
	CHECK(debounce_process_event(&manager, &replay2_up));
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);

	// The press is swallowed here and stays swallowed: nothing may be injected
	// from this stack, it is the one the hook callback runs on.
	CHECK(debounce_process_event(&manager, &replay2_press));
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].pending_replay);
	CHECK(g_injected_count == 0);

	debounce_service_replay(&manager);

	// One call carrying two events: the held release first, the press second
	CHECK(g_injected_count == 2);
	CHECK(g_injected[0].mi.dwFlags == MOUSEEVENTF_LEFTUP);
	CHECK(g_injected[1].mi.dwFlags == (MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE));
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].pending_replay);
	CHECK(!debounce_has_pending_work(&manager));
	CHECK(debounce_injection_failures() == 0);
	CHECK(debounce_injection_refusals() == 0);

	// The press has to land where it happened. Somewhere else would make the
	// next release look like a drag and defer a plain click.
	int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
	int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
	int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

	CHECK(g_injected[1].mi.dx == MulDiv(305 - vx, 65535, vw - 1));
	CHECK(g_injected[1].mi.dy == MulDiv(205 - vy, 65535, vh - 1));

	debounce_flush_deferred_releases(&manager);

	debounce_cleanup(&manager);
	debounce_set_injection_sink(NULL);
}
