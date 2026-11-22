#include "debouncer.h"
#include "config.h"
#include <string.h>

// Forward declarations (the release helpers are defined below)
static bool release_deferred_button(DebounceManager *manager, int button, bool force);
static void release_deferred_buttons(DebounceManager *manager, bool force);

// Elapsed time between two timestamps, guarded against underflow
// A caller that supplies timestamps from another clock source would otherwise
// turn the subtraction into a huge value and silently disable the debouncing.
static uint64_t elapsed_since(uint64_t now, uint64_t then)
{
	return (now >= then) ? (now - then) : 0;
}

// Initialize debounce manager
// Parameters:
//   manager - Pointer to DebounceManager structure to initialize
// Returns:
//   true on success, false on failure
bool debounce_init(DebounceManager *manager)
{
	if (!manager)
		return false;

	memset(manager, 0, sizeof(DebounceManager));
	manager->use_hybrid_heuristic = true; // Default to enabled

	// Smart Drag Protection tuning defaults
	manager->releaseDelayMs = RELEASE_DELAY_DEFAULT_MS;
	manager->dragHoldTimeMs = DRAG_HOLD_TIME_DEFAULT_MS;
	manager->dragDistancePx = DRAG_DISTANCE_DEFAULT_PX;

	// Initialize critical section for thread safety
	InitializeCriticalSection(&manager->cs);

	return true;
}

// Cleanup and release debounce manager resources
// Parameters:
//   manager - Pointer to DebounceManager structure to cleanup
void debounce_cleanup(DebounceManager *manager)
{
	if (!manager)
		return;

	// Defensive: never destroy the lock while a swallowed UP is still pending,
	// otherwise the button would stay pressed inside the system. Idempotent for
	// callers (ShutdownApp) that already flushed.
	release_deferred_buttons(manager, true);

	DeleteCriticalSection(&manager->cs);
}

// Inject a pending button-up for a single button
// Parameters:
//   manager - Pointer to DebounceManager structure
//   button - Button whose deferred release should be injected
//   force - true to release immediately, false to respect the deferral delay
// Returns:
//   true when a deferred release was actually injected
static bool release_deferred_button(DebounceManager *manager, int button, bool force)
{
	if (!manager || button < 0 || button >= MOUSE_BUTTON_COUNT)
		return false;

	DWORD flags = 0;
	DWORD mouse_data = 0;

	EnterCriticalSection(&manager->cs);

	ButtonDebounceData *data = &manager->buttons[button];
	if (!data->isDeferringRelease)
	{
		LeaveCriticalSection(&manager->cs);
		return false;
	}

	if (!force && GetTickCount64() - data->deferStartTime <= (uint64_t)manager->releaseDelayMs)
	{
		LeaveCriticalSection(&manager->cs);
		return false;
	}

	data->isDeferringRelease = false;

	// The release is being delivered now, so it becomes the reference point for
	// the next bounce check.
	data->previousTime = GetTickCount64();

	switch (button)
	{
	case MOUSE_BUTTON_LEFT:
		flags = MOUSEEVENTF_LEFTUP;
		break;
	case MOUSE_BUTTON_RIGHT:
		flags = MOUSEEVENTF_RIGHTUP;
		break;
	case MOUSE_BUTTON_MIDDLE:
		flags = MOUSEEVENTF_MIDDLEUP;
		break;
	case MOUSE_BUTTON_X1:
		flags = MOUSEEVENTF_XUP;
		mouse_data = XBUTTON1;
		break;
	case MOUSE_BUTTON_X2:
		flags = MOUSEEVENTF_XUP;
		mouse_data = XBUTTON2;
		break;
	default:
		// Wheel axes are never deferred
		break;
	}

	LeaveCriticalSection(&manager->cs);

	if (!flags)
		return false;

	INPUT input = {0};
	input.type = INPUT_MOUSE;
	input.mi.dwFlags = flags;
	input.mi.mouseData = mouse_data;
	SendInput(1, &input, sizeof(INPUT));

	return true;
}

// Whether a decision describes a false click a user would care about
bool debounce_decision_is_false_click(const DebounceDecision *decision)
{
	if (!decision || !decision->blocked)
		return false;

	// A swallowed release belongs to the click that was already counted, and a
	// held release is normal Smart Drag behaviour. Counting either one would
	// report a single false click twice.
	return decision->reason == DEBOUNCE_REASON_BLOCK_THRESHOLD ||
		   decision->reason == DEBOUNCE_REASON_BLOCK_DEFERRING ||
		   decision->reason == DEBOUNCE_REASON_BLOCK_WHEEL_REVERSAL;
}

// Process a mouse event and determine if it should be blocked
// Parameters:
//   manager - Pointer to DebounceManager structure
//   event - Pointer to MouseEvent structure containing event data
// Returns:
//   true if event should be blocked, false if it should pass through
bool debounce_process_event(DebounceManager *manager, const MouseEvent *event)
{
	return debounce_process_event_ex(manager, event, NULL);
}

// Process a mouse event and explain the decision
// Parameters:
//   manager - Pointer to DebounceManager structure
//   event - Pointer to MouseEvent structure containing event data
//   decision - Optional output; filled even when the event is ignored
// Returns:
//   true if event should be blocked, false if it should pass through
bool debounce_process_event_ex(DebounceManager *manager, const MouseEvent *event, DebounceDecision *decision)
{
	if (decision)
		memset(decision, 0, sizeof(*decision));

	// Early return for invalid inputs
	if (!manager || !event)
		return false;

	if (event->button < 0 || event->button >= MOUSE_BUTTON_COUNT)
		return false;

	// A fresh press must never be swallowed by a release that is still deferred.
	// Flush that release first (outside the critical section) so the new press is
	// evaluated normally instead of being treated as switch bounce.
	bool flushed_on_press = false;
	if (!event->is_injected && event->is_down)
		flushed_on_press = release_deferred_button(manager, event->button, true);

	if (decision)
		decision->flushed_deferred = flushed_on_press;

	EnterCriticalSection(&manager->cs);

	ButtonDebounceData *data = &manager->buttons[event->button];

	// The gap is measured before any timestamp is updated, so the value describes
	// this event rather than the one that follows it.
	if (decision)
	{
		decision->threshold_ms = data->thresholdMs;
		decision->first_event = (data->previousTime == 0);
		decision->elapsed_ms = decision->first_event
								   ? 0
								   : elapsed_since(event->timestamp, data->previousTime);

		// A release belongs to the press before it, so it reports that press's
		// rhythm. A press overwrites this below once its own gap is known.
		decision->press_gap_ms = data->pressGapMs;
		decision->has_press_gap = data->hasPressGap;
	}

	// Ignore our own injected events and buttons that are not monitored
	if (event->is_injected || !data->isMonitored)
	{
		if (decision)
		{
			decision->reason = event->is_injected ? DEBOUNCE_REASON_IGNORE_INJECTED
												  : DEBOUNCE_REASON_IGNORE_UNMONITORED;
		}

		LeaveCriticalSection(&manager->cs);
		return false;
	}

	bool should_block = false;
	DebounceReason reason = DEBOUNCE_REASON_PASS;

	// Handle wheel events specially
	if (event->button == MOUSE_BUTTON_WHEEL || event->button == MOUSE_BUTTON_HWHEEL)
	{
		// For wheel events, the delta is stored in the data field
		int32_t wheel_delta = event->data;
		int32_t direction_sign = (wheel_delta > 0) ? 1 : (wheel_delta < 0) ? -1
																			   : 0;

		if (direction_sign == 0)
		{
			if (decision)
				decision->reason = DEBOUNCE_REASON_IGNORE_WHEEL_ZERO;

			LeaveCriticalSection(&manager->cs);
			return false; // Ignore zero delta
		}

		// Block a direction reversal that happens within the debounce threshold
		if (data->wheelDirection != 0 && data->wheelDirection != direction_sign)
		{
			uint64_t elapsed_time = elapsed_since(event->timestamp, data->previousTime);
			if (elapsed_time <= (uint64_t)data->thresholdMs)
			{
				data->blocks++;
				should_block = true;
				reason = DEBOUNCE_REASON_BLOCK_WHEEL_REVERSAL;
			}
		}

		// Update direction and timestamp for the next comparison
		data->wheelDirection = direction_sign;
		data->previousTime = event->timestamp;
	}
	else if (event->is_down)
	{
		// Check if we are deferring a release (Hybrid Heuristic)
		if (manager->use_hybrid_heuristic && data->isDeferringRelease)
		{
			data->isDeferringRelease = false;
			data->blocks++;
			should_block = true;
			reason = DEBOUNCE_REASON_BLOCK_DEFERRING;
			data->previousTime = event->timestamp;

			// A blocked press is still a press: the release that follows belongs to
			// it, so the drag baseline has to move with it. Leaving the old values
			// would measure the next release against the previous press and could
			// turn an ordinary click into a Smart Drag release.
			data->downTime = event->timestamp;
			data->downPoint.x = event->x;
			data->downPoint.y = event->y;
		}
		else
		{
			// Check if this press is a bounce right after the previous one.
			// A press that just forced out a deferred release is a user action,
			// never switch bounce, so it is exempt from the check.
			if (!data->isBlocked && !flushed_on_press)
			{
				uint64_t elapsed_time = elapsed_since(event->timestamp, data->previousTime);
				if (elapsed_time <= (uint64_t)data->thresholdMs)
				{
					data->isBlocked = true;
					data->blockedTime = event->timestamp;
					data->blocks++;
					should_block = true;
					reason = DEBOUNCE_REASON_BLOCK_THRESHOLD;
				}
			}
			else if (flushed_on_press)
			{
				reason = DEBOUNCE_REASON_PASS_FLUSHED;
			}

			data->previousTime = event->timestamp;
			data->downTime = event->timestamp;
			data->downPoint.x = event->x;
			data->downPoint.y = event->y;
		}

		// Press rhythm for the diagnostics window. Tracked for every press, even
		// the ones that are blocked: physically the user pressed, and the next
		// press has to be measured from it.
		if (data->lastDownTime != 0)
		{
			data->pressGapMs = elapsed_since(event->timestamp, data->lastDownTime);
			data->hasPressGap = true;
		}
		else
		{
			data->pressGapMs = 0;
			data->hasPressGap = false;
		}

		data->lastDownTime = event->timestamp;

		if (decision)
		{
			decision->press_gap_ms = data->pressGapMs;
			decision->has_press_gap = data->hasPressGap;
		}
	}
	else
	{
		// Button up event
		//
		// A blocked press expects its release right behind it. If that release
		// never arrived, the flag is stale: trusting the pairing here would
		// swallow a genuine release and leave the button stuck inside the
		// system. Expire it and deliver this release normally.
		if (data->isBlocked &&
			elapsed_since(event->timestamp, data->blockedTime) > (uint64_t)DEBOUNCE_BLOCK_TIMEOUT_MS)
			data->isBlocked = false;

		// Swallow the release belonging to a blocked press
		if (data->isBlocked)
		{
			data->isBlocked = false;
			should_block = true;
			reason = DEBOUNCE_REASON_BLOCK_RELEASE;
		}

		data->previousTime = event->timestamp;

		// Hybrid Heuristic: hold the release back briefly so that a bouncing
		// switch cannot interrupt an ongoing drag, then re-inject it.
		if (!should_block && manager->use_hybrid_heuristic)
		{
			uint64_t hold_time = elapsed_since(event->timestamp, data->downTime);
			long dx = event->x - data->downPoint.x;
			long dy = event->y - data->downPoint.y;
			long dist_sq = dx * dx + dy * dy;
			long drag_distance = (long)manager->dragDistancePx;

			if (hold_time > (uint64_t)manager->dragHoldTimeMs || dist_sq > drag_distance * drag_distance)
			{
				data->isDeferringRelease = true;
				data->deferStartTime = event->timestamp;
				data->deferUpPoint.x = event->x;
				data->deferUpPoint.y = event->y;
				should_block = true;
				reason = DEBOUNCE_REASON_DEFER_RELEASE;
			}
		}
	}

	LeaveCriticalSection(&manager->cs);

	if (decision)
	{
		decision->reason = reason;
		decision->blocked = should_block;
		decision->deferred = (reason == DEBOUNCE_REASON_DEFER_RELEASE);

		// A press that was delivered but only just cleared the threshold.
		//
		// Measured from the same two numbers the decision used, so the evidence
		// line and the verdict can never disagree. A press that forced out a
		// deferred release is excluded on purpose: its timestamp baseline was
		// rewritten by that flush, so its gap says nothing about the switch.
		if (event->is_down && !event->is_injected && !should_block &&
			!decision->first_event && !decision->flushed_deferred &&
			decision->threshold_ms > 0 &&
			decision->elapsed_ms <= (uint64_t)decision->threshold_ms * 2u)
		{
			decision->near_miss = true;
			decision->near_miss_ms = decision->elapsed_ms;
		}
	}

	return should_block;
}

// Set debounce threshold for a specific button
// Parameters:
//   manager - Pointer to DebounceManager structure
//   button - Mouse button to set threshold for
//   threshold_ms - Threshold value in milliseconds
//   min_threshold_ms - Minimum allowed threshold value
//   max_threshold_ms - Maximum allowed threshold value
void debounce_set_threshold(DebounceManager *manager, MouseButton button, uint32_t threshold_ms, uint32_t min_threshold_ms, uint32_t max_threshold_ms)
{
	if (!manager || button < 0 || button >= MOUSE_BUTTON_COUNT)
		return;

	if (threshold_ms < min_threshold_ms || threshold_ms > max_threshold_ms)
		return;

	EnterCriticalSection(&manager->cs);

	manager->buttons[button].thresholdMs = threshold_ms;

	LeaveCriticalSection(&manager->cs);
}

// Set whether to use Hybrid Heuristic Scheme
// Parameters:
//   manager - Pointer to DebounceManager structure
//   use_hybrid - true to enable, false to disable
void debounce_set_hybrid_heuristic(DebounceManager *manager, bool use_hybrid)
{
	if (!manager)
		return;

	// Disabling the heuristic must not drop a release that was already swallowed
	if (!use_hybrid)
		release_deferred_buttons(manager, true);

	EnterCriticalSection(&manager->cs);
	manager->use_hybrid_heuristic = use_hybrid;

	// Defensive: the flush above clears the flag, this keeps the invariant even
	// if a future reordering sneaks in.
	if (!use_hybrid)
	{
		for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
			manager->buttons[i].isDeferringRelease = false;
	}

	LeaveCriticalSection(&manager->cs);
}

// Set the delay applied to a deferred release
// Parameters:
//   manager - Pointer to DebounceManager structure
//   delay_ms - Delay in milliseconds
//   min_ms - Minimum allowed value
//   max_ms - Maximum allowed value
void debounce_set_release_delay(DebounceManager *manager, uint32_t delay_ms, uint32_t min_ms, uint32_t max_ms)
{
	if (!manager || delay_ms < min_ms || delay_ms > max_ms)
		return;

	EnterCriticalSection(&manager->cs);
	manager->releaseDelayMs = delay_ms;
	LeaveCriticalSection(&manager->cs);
}

// Set the movement that counts as a drag
// Parameters:
//   manager - Pointer to DebounceManager structure
//   distance_px - Distance in pixels
//   min_px - Minimum allowed value
//   max_px - Maximum allowed value
void debounce_set_drag_distance(DebounceManager *manager, uint32_t distance_px, uint32_t min_px, uint32_t max_px)
{
	if (!manager || distance_px < min_px || distance_px > max_px)
		return;

	EnterCriticalSection(&manager->cs);
	manager->dragDistancePx = distance_px;
	LeaveCriticalSection(&manager->cs);
}

// Enable or disable monitoring for a specific button
// Parameters:
//   manager - Pointer to DebounceManager structure
//   button - Mouse button to enable/disable monitoring for
//   monitored - true to enable monitoring, false to disable
void debounce_set_monitored(DebounceManager *manager, MouseButton button, bool monitored)
{
	if (!manager || button < 0 || button >= MOUSE_BUTTON_COUNT)
		return;

	// Turning monitoring off must not strand a release that was already swallowed
	if (!monitored)
		release_deferred_button(manager, button, true);

	EnterCriticalSection(&manager->cs);

	manager->buttons[button].isMonitored = monitored;

	if (!monitored)
	{
		// Drop transient state so that re-enabling starts from a clean slate;
		// otherwise the first real press would be mistaken for a bounce.
		manager->buttons[button].isBlocked = false;
	}

	LeaveCriticalSection(&manager->cs);
}

// Enable or disable monitoring for all buttons
// Parameters:
//   manager - Pointer to DebounceManager structure
//   monitored - true to enable monitoring, false to disable
void debounce_set_all_monitored(DebounceManager *manager, bool monitored)
{
	if (!manager)
		return;

	// Turning monitoring off must not strand a release that was already swallowed
	if (!monitored)
		release_deferred_buttons(manager, true);

	EnterCriticalSection(&manager->cs);

	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		manager->buttons[i].isMonitored = monitored;

		if (!monitored)
			manager->buttons[i].isBlocked = false;
	}

	LeaveCriticalSection(&manager->cs);
}

// Get total number of blocked events across all buttons
// Parameters:
//   manager - Pointer to DebounceManager structure
// Returns:
//   Total number of blocked events
uint32_t debounce_get_total_blocks(DebounceManager *manager)
{
	if (!manager)
		return 0;

	EnterCriticalSection(&manager->cs);

	uint32_t total = 0;

	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
		total += manager->buttons[i].blocks;

	LeaveCriticalSection(&manager->cs);

	return total;
}

// Get the blocked event count of a single button
uint32_t debounce_get_button_blocks(DebounceManager *manager, MouseButton button)
{
	if (!manager || button < 0 || button >= MOUSE_BUTTON_COUNT)
		return 0;

	EnterCriticalSection(&manager->cs);
	uint32_t blocks = manager->buttons[button].blocks;
	LeaveCriticalSection(&manager->cs);

	return blocks;
}

// Collect the buttons that have blocked at least one event
int debounce_collect_blocked_buttons(DebounceManager *manager, MouseButton *out_buttons, int max_count)
{
	if (!manager || !out_buttons || max_count <= 0)
		return 0;

	EnterCriticalSection(&manager->cs);

	int count = 0;

	for (int i = 0; i < MOUSE_BUTTON_COUNT && count < max_count; i++)
	{
		if (manager->buttons[i].blocks > 0)
			out_buttons[count++] = (MouseButton)i;
	}

	LeaveCriticalSection(&manager->cs);
	return count;
}

// Check if any button is currently being monitored
// Parameters:
//   manager - Pointer to DebounceManager structure
// Returns:
//   true if at least one button is monitored, false otherwise
bool debounce_is_any_monitored(DebounceManager *manager)
{
	if (!manager)
		return false;

	EnterCriticalSection(&manager->cs);

	bool any_monitored = false;
	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		if (manager->buttons[i].isMonitored)
		{
			any_monitored = true;
			break;
		}
	}

	LeaveCriticalSection(&manager->cs);
	return any_monitored;
}

// Check if every button is currently being monitored
// Parameters:
//   manager - Pointer to DebounceManager structure
// Returns:
//   true if all buttons are monitored, false otherwise
bool debounce_is_all_monitored(DebounceManager *manager)
{
	if (!manager)
		return false;

	EnterCriticalSection(&manager->cs);

	bool all_monitored = true;
	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		if (!manager->buttons[i].isMonitored)
		{
			all_monitored = false;
			break;
		}
	}

	LeaveCriticalSection(&manager->cs);
	return all_monitored;
}

// Count the buttons that are currently monitored
// Parameters:
//   manager - Pointer to DebounceManager structure
// Returns:
//   Number of monitored buttons (0 when the manager is invalid)
int debounce_count_monitored(DebounceManager *manager)
{
	if (!manager)
		return 0;

	EnterCriticalSection(&manager->cs);

	int count = 0;
	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		if (manager->buttons[i].isMonitored)
			count++;
	}

	LeaveCriticalSection(&manager->cs);
	return count;
}

// Reset all statistics (blocked counts, blocked states, wheel directions)
// Parameters:
//   manager - Pointer to DebounceManager structure
void debounce_reset_statistics(DebounceManager *manager)
{
	if (!manager)
		return;

	// A deferred release must be delivered before its bookkeeping is dropped,
	// otherwise the button would stay pressed inside the system.
	release_deferred_buttons(manager, true);

	EnterCriticalSection(&manager->cs);

	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		manager->buttons[i].blocks = 0;
		manager->buttons[i].isBlocked = false;
		manager->buttons[i].isDeferringRelease = false;
		manager->buttons[i].wheelDirection = 0;
	}

	LeaveCriticalSection(&manager->cs);
}

// Check whether at least one release is currently deferred
// Parameters:
//   manager - Pointer to DebounceManager structure
// Returns:
//   true if a deferred release is pending
bool debounce_has_pending_deferrals(DebounceManager *manager)
{
	if (!manager)
		return false;

	EnterCriticalSection(&manager->cs);

	bool pending = false;
	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		if (manager->buttons[i].isDeferringRelease)
		{
			pending = true;
			break;
		}
	}

	LeaveCriticalSection(&manager->cs);
	return pending;
}

// Release every deferred button
// Parameters:
//   manager - Pointer to DebounceManager structure
//   force - true to release immediately, false to respect the deferral delay
static void release_deferred_buttons(DebounceManager *manager, bool force)
{
	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
		release_deferred_button(manager, i, force);
}

// Check for deferred releases and inject events if necessary
void debounce_check_deferred_releases(DebounceManager *manager)
{
	if (!manager)
		return;

	release_deferred_buttons(manager, false);
}

// Release every deferred button immediately, ignoring the deferral delay
// Used as a safety net when the deferred-release timer cannot be started.
void debounce_flush_deferred_releases(DebounceManager *manager)
{
	if (!manager)
		return;

	release_deferred_buttons(manager, true);
}
