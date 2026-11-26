#pragma once

#include <stdbool.h>
#include <stdint.h>

// Detection of a low-level hook that Windows removed without telling anybody.
//
// A WH_MOUSE_LL hook runs on the thread that installed it. If that thread ever
// blocks longer than HKCU\Control Panel\Desktop\LowLevelHooksTimeout (300 ms by
// default), Windows silently unhooks it. Our own "installed" flag stays true, so
// the tray icon and the menu would keep claiming that the protection is active
// while it is not - which is the worst possible failure mode for this product.
//
// Two independent pieces of evidence are used, because neither alone is enough:
//   * cursor movement      - a live mouse hook always sees WM_MOUSEMOVE
//   * button state change  - polled through GetAsyncKeyState, so it is visible
//                            even when the user clicks without moving the mouse
// GetLastInputInfo() is deliberately NOT used: it also counts keyboard input, so
// typing would look like "input happened but the hook saw nothing".
//
// The check below is pure logic so that every branch is unit testable.

// Two consecutive suspicious checks are required: a single one can be produced
// by a movement that happened between reading the cursor and reading the counter.
#define WATCHDOG_SUSPECT_LIMIT 2

typedef struct
{
	int64_t last_event_count; // Hook events (movement included) at the previous check
	uint64_t last_recover_tick;
	uint32_t recover_count;
	int suspect_streak;

	// Sticky health signal for the tray/menu. Unlike suspect_streak this is not
	// cleared by an idle window: inactivity is no proof that a dead hook came
	// back. Only an event the hook actually saw, or a fresh init after recovery,
	// clears it.
	bool suspecting;
	bool initialised;
} WatchdogState;

typedef enum
{
	WATCHDOG_FIRST_CHECK = 0, // Baseline recorded, nothing to judge yet
	WATCHDOG_ALIVE,			  // No mouse activity, or our hook clearly saw it
	WATCHDOG_SUSPECT,		  // Mouse activity happened but the hook saw nothing
	WATCHDOG_RECOVER		  // Confirmed: the hook should be reinstalled
} WatchdogVerdict;

// Reset the state so the next check only records a baseline
void hook_watchdog_init(WatchdogState *state);

// Judge the health of the hook
// Parameters:
//   state - Persistent state between checks (updated by this call)
//   cursor_moved - Whether the cursor position changed since the previous check
//   button_changed - Whether a mouse button changed state since the previous check
//   event_count - Number of events the hook has delivered (movement included)
//   now - Current GetTickCount64() value
//   recover_cooldown_ms - Minimum time between two recovery attempts
// Returns:
//   The verdict for this check
WatchdogVerdict hook_watchdog_check(WatchdogState *state, bool cursor_moved, bool button_changed,
									int64_t event_count, uint64_t now, uint64_t recover_cooldown_ms);

// Whether the hook is known to be working. False from the first check that saw
// input the hook missed, until either the hook reports an event again or the
// state is reinitialised after a successful recovery. The tray uses this so it
// never claims protection while Windows has silently removed the hook.
bool hook_watchdog_is_healthy(const WatchdogState *state);
