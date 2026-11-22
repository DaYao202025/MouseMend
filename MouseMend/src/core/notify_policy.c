#include "notify_policy.h"

// Decide whether a balloon should be shown
bool notify_policy_decide(NotifyState state, uint32_t total_blocks, uint64_t now,
						  uint64_t min_interval_ms, NotifyDecision *decision)
{
	if (!decision)
		return false;

	decision->show = false;
	decision->delta = 0;
	decision->update_state = false;
	decision->next_state = state;

	// The counter can be smaller than the remembered value: the user pressed
	// "clear blocked count". Treat that as "nothing new" instead of letting the
	// unsigned subtraction wrap around into four billion blocked clicks.
	uint32_t delta = (total_blocks > state.reported_blocks) ? (total_blocks - state.reported_blocks) : 0;

	if (delta == 0)
		return true;

	// Throttle, but never suppress the very first balloon
	if (state.last_tick != 0)
	{
		// A tick that appears to go backwards means the caller mixed clocks;
		// stay quiet rather than firing a burst of balloons.
		if (now < state.last_tick)
			return true;

		if (now - state.last_tick < min_interval_ms)
			return true;
	}

	decision->show = true;
	decision->delta = delta;
	decision->update_state = true;
	decision->next_state.last_tick = now;
	decision->next_state.reported_blocks = total_blocks;

	return true;
}
