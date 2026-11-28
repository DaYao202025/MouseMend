#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_capabilities.h"
#include "debouncer.h"
#include "mouse_hook.h"
#include "status_notice.h"

// Read-only view of the application state, used by the settings window.
//
// The window never touches the debounce manager or the hook directly: it reads a
// snapshot and writes through the AppController. That keeps the UI thin and
// keeps the core testable without a window.

// A Chinese account name plus a deep profile directory can exceed MAX_PATH.
#define APP_SNAPSHOT_LOG_DIR_SIZE 1024

// Program name of the foreground context, same length the filter uses. Kept as
// a literal here so the window does not have to include the filter header just
// to size a buffer.
#define APP_SNAPSHOT_CONTEXT_NAME 64

// What the context filter decided for the program currently in front.
//
// The distinction that matters is the one the user cannot see today: whether
// this very program is being intercepted or let through. The strip on top says
// whether the module is alive, not what it decided.
typedef enum
{
	APP_CONTEXT_NONE = 0, // nothing published yet, or the lookup failed
	APP_CONTEXT_OURS,	  // our own window, never filtered
	APP_CONTEXT_EXEMPT,	  // the filter is letting it through
	APP_CONTEXT_BLOCKED	  // the filter is intercepting it
} AppContextVerdict;

// Evidence for the self check: the most recent press the engine refused for a
// button, plus the gap that produced the decision. Recorded by the hook, read by
// the window, never recomputed there.
typedef struct
{
	bool valid;
	uint64_t timestamp;	   // GetTickCount64 of the blocked press
	uint64_t elapsed_ms;   // Gap the engine measured for that press
	uint64_t press_gap_ms; // Gap since the previous press, 0 when unknown
	bool has_press_gap;
	uint32_t threshold_ms; // Threshold that was in force
	uint32_t blocks;	   // Lifetime count of the button
} AppBlockEvidence;

typedef struct
{
	// Per button state
	uint32_t blocks[MOUSE_BUTTON_COUNT];
	uint32_t thresholds[MOUSE_BUTTON_COUNT];
	bool monitored[MOUSE_BUTTON_COUNT];
	AppBlockEvidence evidence[MOUSE_BUTTON_COUNT];

	// Globals
	uint32_t total_blocks;
	uint32_t hook_events;	  // Events delivered by the hook since start
	uint32_t hook_recoveries; // Times Windows dropped the hook and we reinstalled
	bool hook_active;
	bool hybrid;
	bool logging;
	uint32_t release_delay_ms;
	uint32_t drag_distance_px;

	// State of each optional capability, so the window can show which part is
	// not working instead of a single "on" that hides a dead module.
	uint8_t capabilities[CAP_COUNT];

	// Commands that arrived with no handler. Non-zero means a menu entry exists
	// but nothing claims it - the "this does nothing" report, made countable.
	uint32_t unhandled_menu_ids;

	// What the context filter decided for the program currently in front, and
	// its name. Filled by the caller that owns the filter, not by
	// app_snapshot_fill: the engine half has no reason to know about it.
	uint8_t context_verdict;
	wchar_t context_name[APP_SNAPSHOT_CONTEXT_NAME];

	// The most recent status bar message, empty when there is none. Copied here
	// from the notice slot so the window never reaches into application state.
	wchar_t notice_text[STATUS_NOTICE_CHARS];

	// Context filtering paused by the user, and how long is left.
	//
	// The window only reads a snapshot and has no access to the ContextFilter, so
	// without these two fields the top block would keep saying "protection on"
	// while filtering is actually off - the one place the user looks for a verdict
	// would be wrong exactly when something is off.
	bool context_paused;
	uint32_t context_pause_left_ms;

	// Log directory, empty when it could not be resolved
	wchar_t log_directory[APP_SNAPSHOT_LOG_DIR_SIZE];

	// Wall clock sampled together with now_tick, so the window can turn a
	// GetTickCount64 stamp into a readable time without keeping its own base
	SYSTEMTIME now_local;
	uint64_t now_tick;
} AppSnapshot;

// Fill the part of the snapshot that comes straight from the engine.
//
// Clears the whole struct first, so a caller always starts from a defined
// state, then copies per button counters and the Hybrid Heuristic settings and
// samples the wall clock and the tick together. Everything the engine does not
// know about (evidence, hook health, logging) is left zeroed for the caller.
//
// Passing NULL still clears the snapshot: a half started application then
// reports "nothing known" instead of uninitialised memory.
void app_snapshot_fill(AppSnapshot *snapshot, DebounceManager *debounce);
