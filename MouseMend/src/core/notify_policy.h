#pragma once

#include <stdbool.h>
#include <stdint.h>

// Throttling policy for the "false click blocked" balloon.
//
// A worn switch can bounce dozens of times per minute. Without throttling the
// notification would become the annoyance the application exists to remove, so
// the decision is kept in a small pure function that can be unit tested.

// Remembered state between decisions
typedef struct
{
	// Last moment a balloon was shown OR the last moment a balloon was withheld
	// on purpose (fullscreen game, presentation, locked screen). Both uses share
	// one field because they answer the same question: "may we interrupt now?".
	uint64_t last_tick;

	// Blocked counter value that has already been reported. It is deliberately
	// NOT advanced while a balloon is withheld, so the next balloon reports the
	// whole accumulated amount instead of losing it.
	uint32_t reported_blocks;
} NotifyState;

// Result of a decision
typedef struct
{
	bool show;			  // Whether a balloon should be shown now
	uint32_t delta;		  // Blocks accumulated since the last balloon
	bool update_state;	  // Whether the caller should commit the new state
	NotifyState next_state; // State to commit when update_state is true
} NotifyDecision;

// Decide whether a balloon should be shown
// Parameters:
//   state - Previous notification state
//   total_blocks - Current total blocked counter
//   now - Current tick in milliseconds
//   min_interval_ms - Minimum time between two balloons
//   decision - Receives the result
// Returns:
//   true when the decision struct was filled in
bool notify_policy_decide(NotifyState state, uint32_t total_blocks, uint64_t now,
						  uint64_t min_interval_ms, NotifyDecision *decision);
