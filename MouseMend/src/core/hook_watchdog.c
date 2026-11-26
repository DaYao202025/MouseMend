#include "hook_watchdog.h"

// Reset the state so the next check only records a baseline
void hook_watchdog_init(WatchdogState *state)
{
	if (!state)
		return;

	state->last_event_count = 0;
	state->last_recover_tick = 0;
	state->recover_count = 0;
	state->suspect_streak = 0;
	state->suspecting = false;
	state->initialised = false;
}

bool hook_watchdog_is_healthy(const WatchdogState *state)
{
	return !state || !state->suspecting;
}

// Judge the health of the hook
WatchdogVerdict hook_watchdog_check(WatchdogState *state, bool cursor_moved, bool button_changed,
									int64_t event_count, uint64_t now, uint64_t recover_cooldown_ms)
{
	if (!state)
		return WATCHDOG_ALIVE;

	// The very first call only establishes a baseline
	if (!state->initialised)
	{
		state->initialised = true;
		state->last_event_count = event_count;
		return WATCHDOG_FIRST_CHECK;
	}

	bool events_advanced = (event_count != state->last_event_count);
	state->last_event_count = event_count;

	// No mouse activity at all: this window says nothing about the hook
	if (!cursor_moved && !button_changed)
	{
		state->suspect_streak = 0;
		return WATCHDOG_ALIVE;
	}

	// The cursor moved and the hook saw it: the hook is alive
	if (events_advanced)
	{
		state->suspect_streak = 0;
		state->suspecting = false;
		return WATCHDOG_ALIVE;
	}

	// The cursor moved but nothing reached us. One such window can be a sampling
	// artefact (the movement landed between the two reads), so require a streak.
	state->suspect_streak++;
	state->suspecting = true;

	if (state->suspect_streak < WATCHDOG_SUSPECT_LIMIT)
		return WATCHDOG_SUSPECT;

	// Do not thrash: a recovery attempt itself needs time to prove itself.
	// The streak is dropped while waiting, otherwise the cooldown would end with
	// a streak that already passed the limit and the next check would reinstall
	// immediately - which is exactly the thrash the cooldown exists to prevent.
	if (state->last_recover_tick != 0 && now >= state->last_recover_tick &&
		now - state->last_recover_tick < recover_cooldown_ms)
	{
		state->suspect_streak = 0;
		return WATCHDOG_SUSPECT;
	}

	state->suspect_streak = 0;
	state->last_recover_tick = now;
	state->recover_count++;

	return WATCHDOG_RECOVER;
}
