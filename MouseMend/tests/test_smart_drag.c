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

// Recorded injections, so a deferred release can be asserted without clicking anything
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

// Reset the per-button transient state without touching the configuration.
//
// The deferral is left armed as of the time base, because a release is only ever
// held back on a switch that has already been seen chattering, and the blocks
// below that expect a held release are all about that switch. A block that needs
// a healthy switch calls disarm_deferral() first, and the arming path itself is
// tested explicitly further down.
static void reset_state(DebounceManager *manager)
{
	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		manager->buttons[i].previousTime = 0;
		manager->buttons[i].downTime = 0;
		manager->buttons[i].isBlocked = false;
		manager->buttons[i].isDeferringRelease = false;
		manager->buttons[i].wheelDirection = 0;
		manager->buttons[i].system_down = false;
		manager->buttons[i].deferArmedUntil = g_base + DEBOUNCE_CHATTER_ARM_MS;
	}
}

static void disarm_deferral(DebounceManager *manager)
{
	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
		manager->buttons[i].deferArmedUntil = 0;
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

	// The shipped default is 0, which never holds a release back. The blocks
	// below are about what happens when the hold is switched on, so they turn it
	// on explicitly; the "off means off" case has its own check further down.
	debounce_set_release_delay(&manager, 20, RELEASE_DELAY_MIN_MS, RELEASE_DELAY_MAX_MS);

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

	// A press 20 ms after the release is the switch chattering, not a click. It
	// is blocked, and the held release stays queued: the timer still owes the
	// system that UP. Injecting a DOWN here - which is what the old replay did -
	// pressed the button again on the user's behalf and left the pointer
	// dragging a window when no release followed.
	MouseEvent drag_down2 = make_event(MOUSE_BUTTON_LEFT, 3720, true, 140, 100, 0);
	CHECK(debounce_process_event(&manager, &drag_down2));
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);
	CHECK(debounce_has_pending_deferrals(&manager));

	// Nothing may be injected from this stack - it is the hook's
	CHECK(g_injected_count == 0);

	// The timer owes exactly one event: the release, never a press
	g_injected_count = 0;
	manager.buttons[MOUSE_BUTTON_LEFT].deferStartTime = GetTickCount64() - 1000;
	debounce_check_deferred_releases(&manager);
	CHECK(g_injected_count == 1);
	CHECK(g_injected[0].mi.dwFlags == MOUSEEVENTF_LEFTUP);
	CHECK(!debounce_has_pending_work(&manager));

	// ------------------------------------------------------------------
	// A switch that has never chattered never has a release delayed
	//
	// This is the whole point of arming by evidence. Holding a release makes
	// the system keep the button down, so the pointer drags whatever it is
	// over - the exact "the window follows me after I let go" complaint. A
	// healthy mouse must never pay that for a fault it does not have.
	// ------------------------------------------------------------------
	reset_state(&manager);
	disarm_deferral(&manager);

	MouseEvent healthy_down = make_event(MOUSE_BUTTON_LEFT, 3380, true, 100, 100, 0);
	MouseEvent healthy_up = make_event(MOUSE_BUTTON_LEFT, 3700, false, 140, 100, 0);

	CHECK(!debounce_process_event(&manager, &healthy_down));
	CHECK(!debounce_process_event(&manager, &healthy_up));
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);
	CHECK(!debounce_has_pending_deferrals(&manager));

	// ------------------------------------------------------------------
	// A switch that double-contacts on the press must not eat the release
	//
	// down, then another down a few milliseconds later (the contact bounces),
	// then the real release. The second down is correctly rejected, but the
	// press before it did reach the system, so the release has to reach it as
	// well: dropping it left the pointer dragging a window - the "it sticks
	// after I let go" complaint - until the user clicked again.
	// ------------------------------------------------------------------
	reset_state(&manager);
	disarm_deferral(&manager);

	MouseEvent contact_down = make_event(MOUSE_BUTTON_LEFT, 3480, true, 100, 100, 0);
	MouseEvent contact_bounce = make_event(MOUSE_BUTTON_LEFT, 3490, true, 100, 100, 0);
	MouseEvent contact_up = make_event(MOUSE_BUTTON_LEFT, 3900, false, 100, 100, 0);

	CHECK(!debounce_process_event(&manager, &contact_down));
	CHECK(debounce_process_event(&manager, &contact_bounce));
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].isBlocked);
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].system_down);

	CHECK(!debounce_process_event(&manager, &contact_up)); // the real release
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].isBlocked);
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].system_down);
	CHECK(!debounce_has_pending_deferrals(&manager));

	// ------------------------------------------------------------------
	// One press the threshold rejects is the evidence, and it arms the
	// deferral for the next minute of dragging
	// ------------------------------------------------------------------
	reset_state(&manager);
	disarm_deferral(&manager);

	MouseEvent chatter_down = make_event(MOUSE_BUTTON_LEFT, 3800, true, 100, 100, 0);
	MouseEvent chatter_up = make_event(MOUSE_BUTTON_LEFT, 3810, false, 100, 100, 0);
	MouseEvent chatter_bounce = make_event(MOUSE_BUTTON_LEFT, 3820, true, 100, 100, 0);
	MouseEvent chatter_bounce_up = make_event(MOUSE_BUTTON_LEFT, 3830, false, 100, 100, 0);

	CHECK(!debounce_process_event(&manager, &chatter_down));
	CHECK(!debounce_process_event(&manager, &chatter_up));
	CHECK(debounce_process_event(&manager, &chatter_bounce));	 // rejected as a bounce
	CHECK(debounce_process_event(&manager, &chatter_bounce_up)); // its own release too
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].deferArmedUntil ==
		  chatter_bounce.timestamp + DEBOUNCE_CHATTER_ARM_MS);

	// The same drag that was let through above is now held back
	MouseEvent armed_down = make_event(MOUSE_BUTTON_LEFT, 4000, true, 100, 100, 0);
	MouseEvent armed_up = make_event(MOUSE_BUTTON_LEFT, 4400, false, 140, 100, 0);

	CHECK(!debounce_process_event(&manager, &armed_down));
	CHECK(debounce_process_event(&manager, &armed_up));
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);

	// ------------------------------------------------------------------
	// The evidence expires: a switch that stops chattering is not punished
	// for the rest of the session
	// ------------------------------------------------------------------
	reset_state(&manager);
	manager.buttons[MOUSE_BUTTON_LEFT].deferArmedUntil = 5000;

	MouseEvent stale_down = make_event(MOUSE_BUTTON_LEFT, 6000, true, 100, 100, 0);
	MouseEvent stale_up = make_event(MOUSE_BUTTON_LEFT, 6400, false, 140, 100, 0);

	CHECK(!debounce_process_event(&manager, &stale_down));
	CHECK(!debounce_process_event(&manager, &stale_up));
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);

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
	CHECK(manager.dragHoldTimeMs == DRAG_HOLD_TIME_DEFAULT_MS);
	CHECK(manager.dragDistancePx == DRAG_DISTANCE_DEFAULT_PX);

	// A fresh manager starts at the shipped default, and that default is zero:
	// a release is never held back unless the user asks for it.
	{
		DebounceManager fresh;
		debounce_init(&fresh);
		CHECK(fresh.releaseDelayMs == RELEASE_DELAY_DEFAULT_MS);
		CHECK(RELEASE_DELAY_DEFAULT_MS == 0);
		CHECK(config_release_delay_presets()[0] == RELEASE_DELAY_DEFAULT_MS);
		debounce_cleanup(&fresh);
	}

	// Zero is not "hold for no time", it is "never hold": a drag release has to
	// go through on the frame it happened.
	reset_state(&manager);
	debounce_set_release_delay(&manager, 0, RELEASE_DELAY_MIN_MS, RELEASE_DELAY_MAX_MS);
	CHECK(manager.releaseDelayMs == 0);

	MouseEvent holdoff_down = make_event(MOUSE_BUTTON_LEFT, 17000, true, 100, 100, 0);
	MouseEvent holdoff_up = make_event(MOUSE_BUTTON_LEFT, 17700, false, 340, 100, 0);

	CHECK(!debounce_process_event(&manager, &holdoff_down));
	CHECK(!debounce_process_event(&manager, &holdoff_up));
	CHECK(!manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);
	CHECK(!debounce_has_pending_deferrals(&manager));

	// Put the hold back for the blocks that are about it
	debounce_set_release_delay(&manager, 20, RELEASE_DELAY_MIN_MS, RELEASE_DELAY_MAX_MS);

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
	// A press inside the release window is chatter, not a click
	// ------------------------------------------------------------------
	reset_state(&manager);
	debounce_set_defer_available(&manager, true);

	MouseEvent late_chatter_down = make_event(MOUSE_BUTTON_LEFT, 20000, true, 100, 100, 0);
	MouseEvent late_chatter_up = make_event(MOUSE_BUTTON_LEFT, 20300, false, 140, 100, 0);
	MouseEvent late_chatter_press = make_event(MOUSE_BUTTON_LEFT, 20340, true, 140, 100, 0);

	CHECK(!debounce_process_event(&manager, &late_chatter_down));
	CHECK(debounce_process_event(&manager, &late_chatter_up));
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);

	// Blocked, and the release is still owed to the system
	CHECK(debounce_process_event(&manager, &late_chatter_press));
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);

	// Its own release is a stray one and delays nothing
	MouseEvent late_chatter_release = make_event(MOUSE_BUTTON_LEFT, 20350, false, 100, 100, 0);
	CHECK(!debounce_process_event(&manager, &late_chatter_release));
	CHECK(debounce_has_pending_work(&manager));

	// The timer delivers that release and nothing else
	g_injected_count = 0;
	manager.buttons[MOUSE_BUTTON_LEFT].deferStartTime = GetTickCount64() - 1000;
	debounce_check_deferred_releases(&manager);
	CHECK(g_injected_count == 1);
	CHECK(g_injected[0].mi.dwFlags == MOUSEEVENTF_LEFTUP);
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
	// Nothing that follows a held release may press the button again
	// ------------------------------------------------------------------
	reset_state(&manager);
	g_injected_count = 0;

	// Long and moved, so this release is held back
	MouseEvent drag2_down = make_event(MOUSE_BUTTON_LEFT, 22000, true, 300, 200, 0);
	MouseEvent drag2_up = make_event(MOUSE_BUTTON_LEFT, 22300, false, 340, 200, 0);
	MouseEvent drag2_press = make_event(MOUSE_BUTTON_LEFT, 22340, true, 305, 205, 0);

	CHECK(!debounce_process_event(&manager, &drag2_down));
	CHECK(debounce_process_event(&manager, &drag2_up));
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].isDeferringRelease);

	// The press is swallowed here and stays swallowed: nothing may be injected
	// from this stack, it is the one the hook callback runs on.
	CHECK(debounce_process_event(&manager, &drag2_press));
	CHECK(g_injected_count == 0);

	// The timer owes one UP. A DOWN here is what kept a window following the
	// pointer after the user had already let go.
	g_injected_count = 0;
	manager.buttons[MOUSE_BUTTON_LEFT].deferStartTime = GetTickCount64() - 1000;
	debounce_check_deferred_releases(&manager);

	CHECK(g_injected_count == 1);
	CHECK(g_injected[0].mi.dwFlags == MOUSEEVENTF_LEFTUP);
	CHECK(!debounce_has_pending_work(&manager));
	CHECK(debounce_injection_failures() == 0);
	CHECK(debounce_injection_refusals() == 0);

	debounce_flush_deferred_releases(&manager);

	debounce_cleanup(&manager);
	debounce_set_injection_sink(NULL);
}
