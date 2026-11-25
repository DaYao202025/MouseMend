#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>
#include "mouse_hook.h"

// How long a blocked press may wait for its release before the pairing is
// abandoned.
//
// A blocked press is expected to be followed by its release within a few
// milliseconds. When that release never arrives - a dropped event, or the
// button was switched off and on again while it was held - the flag would
// swallow the next genuine release instead, which is a stuck button for the
// user. Expiring it is always safe: a blocked press that is still held simply
// gets its release delivered, and a release the system never saw a press for is
// ignored by Windows anyway.
#define DEBOUNCE_BLOCK_TIMEOUT_MS 2000u

// Debounce configuration for a single button
// Aligned to a cache line so that per-button updates never share a cache line.
typedef struct
{
	uint64_t previousTime;	 // Timestamp of the last processed event
	uint64_t downTime;		 // Timestamp when the button was pressed
	uint64_t lastDownTime;	 // Timestamp of the previous press (diagnostics only)
	uint64_t pressGapMs;	 // Gap between the last two presses (diagnostics only)
	uint64_t blockedTime;	 // Timestamp of the press that is waiting for its release
	uint64_t deferStartTime; // Timestamp when release deferral started
	POINT downPoint;		 // Coordinates when the button was pressed
	POINT deferUpPoint;		 // Coordinates where the release happened
	uint32_t thresholdMs;	 // Debounce threshold in milliseconds
	uint32_t blocks;		 // Total number of events blocked for this button
	int32_t wheelDirection;	 // Current wheel scroll direction (1=up, -1=down, 0=none)
	bool isMonitored;		 // Whether this button is being monitored for debounce
	bool isBlocked;			 // Whether the next event should be blocked
	bool isDeferringRelease; // Whether we are waiting to confirm a release
	bool hasPressGap;		 // Whether pressGapMs is valid (diagnostics only)

	// Replay: a press that arrived while a release was still held back is
	// swallowed and re-injected by the timer together with that release.
	//
	// It cannot be injected here. The callback runs on the thread that owns the
	// hook, and injecting from that stack makes SendInput wait for itself.
	bool pending_replay;	 // Swallowed press waiting for the timer
	bool replay_in_flight;	 // Timer is injecting it right now
	bool release_after_replay; // A real release arrived during the injection
	uint32_t replay_down_flags;
	uint32_t replay_down_data;
	POINT replay_point;		 // Where the swallowed press happened
	uint64_t replay_press_time;
} __declspec(align(64)) ButtonDebounceData;

// Debounce manager with cache line alignment for performance
typedef struct
{
	ButtonDebounceData buttons[MOUSE_BUTTON_COUNT]; // Array of button debounce data
	bool use_hybrid_heuristic;						// Whether to use the Hybrid Heuristic Scheme

	// Whether a release may be held back at all. Cleared when the application
	// could not arm the timer that gives the release back: holding one without
	// a way to deliver it would leave the button pressed inside the system.
	bool defer_available;
	uint32_t releaseDelayMs;						// Deferred release delay (Smart Drag)
	uint32_t dragHoldTimeMs;						// Hold time that counts as a drag
	uint32_t dragDistancePx;						// Movement that counts as a drag
	CRITICAL_SECTION cs;							// Critical section for thread safety
} __declspec(align(64)) DebounceManager;

// Initialize debounce manager
bool debounce_init(DebounceManager *manager);

// Cleanup debounce manager
void debounce_cleanup(DebounceManager *manager);

// Why the engine reached its verdict.
// The check-up window renders this value instead of comparing thresholds of its
// own: there is exactly one copy of the debouncing rules, and it lives here.
typedef enum
{
	DEBOUNCE_REASON_PASS = 0,		   // Delivered to the system
	DEBOUNCE_REASON_BLOCK_THRESHOLD,   // Press too close to the previous event
	DEBOUNCE_REASON_BLOCK_DEFERRING,   // Press while a drag release is still held
	DEBOUNCE_REASON_BLOCK_RELEASE,	   // Release belonging to a blocked press
	DEBOUNCE_REASON_BLOCK_WHEEL_REVERSAL,
	DEBOUNCE_REASON_DEFER_RELEASE,	   // Release held back on purpose (Smart Drag)
	DEBOUNCE_REASON_PASS_FLUSHED,	   // Press that first delivered a held release
	DEBOUNCE_REASON_IGNORE_INJECTED,
	DEBOUNCE_REASON_IGNORE_UNMONITORED,
	DEBOUNCE_REASON_IGNORE_WHEEL_ZERO,
	DEBOUNCE_REASON_COUNT
} DebounceReason;

// Full explanation of one decision, for diagnostics only.
typedef struct
{
	DebounceReason reason;
	bool blocked;		  // Same value debounce_process_event returns
	bool deferred;		  // This release was held back
	bool flushed_deferred; // A held release was delivered before this press

	// This press was swallowed and will be replayed by the timer right after
	// that release. It is NOT a blocked false click: counters, the block log and
	// the balloon all have to skip it.
	bool replay;
	bool first_event;	  // No earlier event for this button in this session
	uint64_t elapsed_ms;   // Time since the previous event of this button

	// Time since the previous *press* of this button, which is what a user means
	// by "the gap between two clicks". It differs from elapsed_ms whenever the
	// previous event was a release: the engine compares against the previous
	// event of any kind (that is what catches a bounce right after a long hold),
	// while this value is only ever a press-to-press rhythm.
	bool has_press_gap;
	uint64_t press_gap_ms;

	uint32_t threshold_ms; // Threshold in force when the decision was made

	// A press that was delivered but only just cleared the threshold.
	//
	// This changes no decision - the press went through - it exists because of
	// what a user concludes without it. A switch that bounces every 47 ms against
	// a 40 ms threshold produces a log full of "passed" and a count of zero
	// blocked, and the user walks away believing the program does nothing. The
	// near miss count is the evidence that the hardware is already failing, and
	// it gives the user something to act on: raise the threshold.
	bool near_miss;

	// How far below the doubled threshold the press was, for the display
	uint64_t near_miss_ms;
} DebounceDecision;

// Whether a decision describes a false click a user would care about
//
// This is the single definition of "counts as a false click": the block log, the
// self check evidence and the notification all have to agree, otherwise the
// window could report one blocked click while the evidence block stays empty.
bool debounce_decision_is_false_click(const DebounceDecision *decision);

// Stable, language independent name of a verdict, used by the support report
// The check-up window shows localized text; a report that lands in a bug tracker
// has to stay greppable, so the two never share a string.
const char *debounce_reason_slug(DebounceReason reason);

// Process a mouse event and determine if it should be blocked
bool debounce_process_event(DebounceManager *manager, const MouseEvent *event);

// Same as above, but also reports why. decision may be NULL.
bool debounce_process_event_ex(DebounceManager *manager, const MouseEvent *event, DebounceDecision *decision);

// Set threshold for a specific button
void debounce_set_threshold(DebounceManager *manager, MouseButton button, uint32_t threshold_ms, uint32_t min_threshold_ms, uint32_t max_threshold_ms);

// Enable/disable monitoring for a specific button
// Disabling delivers any pending deferred release first, so a swallowed UP can
// never be stranded when a button is switched off.
void debounce_set_monitored(DebounceManager *manager, MouseButton button, bool monitored);

// Enable/disable monitoring for all buttons (same guarantee as above)
void debounce_set_all_monitored(DebounceManager *manager, bool monitored);

// Get total blocked events count
uint32_t debounce_get_total_blocks(DebounceManager *manager);

// Get the blocked event count of a single button
uint32_t debounce_get_button_blocks(DebounceManager *manager, MouseButton button);

// Collect the buttons that have blocked at least one event
// The order follows the MouseButton enum so the menu keeps a stable layout.
// Parameters:
//   manager - Pointer to DebounceManager structure
//   out_buttons - Receives the button indices
//   max_count - Capacity of out_buttons
// Returns:
//   Number of entries written (0 when nothing has been blocked yet)
int debounce_collect_blocked_buttons(DebounceManager *manager, MouseButton *out_buttons, int max_count);

// Check if any button is being monitored
bool debounce_is_any_monitored(DebounceManager *manager);

// Check if every button is being monitored
bool debounce_is_all_monitored(DebounceManager *manager);

// Count the buttons that are currently monitored
int debounce_count_monitored(DebounceManager *manager);

// Reset statistics
void debounce_reset_statistics(DebounceManager *manager);

// Set whether to use Hybrid Heuristic Scheme
void debounce_set_hybrid_heuristic(DebounceManager *manager, bool use_hybrid);

// Set the Smart Drag Protection tuning.
// Values outside the allowed range are ignored; the caller supplies the bounds.
void debounce_set_release_delay(DebounceManager *manager, uint32_t delay_ms, uint32_t min_ms, uint32_t max_ms);
void debounce_set_drag_distance(DebounceManager *manager, uint32_t distance_px, uint32_t min_px, uint32_t max_px);

// Check whether at least one release is currently deferred
bool debounce_has_pending_deferrals(DebounceManager *manager);

// Check for deferred releases and inject events if necessary
void debounce_check_deferred_releases(DebounceManager *manager);

// Release every deferred button immediately (safety net when the timer is unavailable)
void debounce_flush_deferred_releases(DebounceManager *manager);

// Tell the engine whether a held release could be given back at all
// Call with false when the deferred-release timer cannot be armed: holding a
// release without a way to deliver it is how a button ends up pressed forever.
void debounce_set_defer_available(DebounceManager *manager, bool available);

// Whether anything still needs the timer: a held release or a pending replay
bool debounce_has_pending_work(DebounceManager *manager);

// Run queued replays. Must only be called from the timer, never from the hook.
// Injects the held release first and then the press that was swallowed behind
// it, so the system sees the same order it would have seen without the filter.
void debounce_service_replay(DebounceManager *manager);

// Forget queued replays without running them. Used on shutdown, where a press
// replayed into a dying process is pointless and only risks a stuck button.
void debounce_drop_replays(DebounceManager *manager);
