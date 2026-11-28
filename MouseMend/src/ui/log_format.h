#pragma once

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include "../core/app_snapshot.h"
#include "../core/config.h"
#include "../core/debouncer.h"
#include "../core/event_history.h"
#include "../core/event_trace.h"
#include "language.h"

// Pure text rendering for the check-up window.
//
// Everything in here turns engine output into user-visible strings. Nothing
// here owns a window, a device context or a font, so the formatting can be
// read, reasoned about and unit tested without a message loop. The window
// itself (settings_window.c) only moves these strings into controls.
//
// The verdict is never decided here: it comes from DebounceDecision. Keeping
// the decision in the engine and the wording here is what stops the log and
// the self check from disagreeing about the same event.

// The log is a rolling history, not a file: the oldest row is dropped
#define SETTINGS_LOG_ROWS 24

// Room for one rendered row, one time stamp and one numeric cell
#define SETTINGS_LINE_SIZE 192
#define SETTINGS_TIME_SIZE 24
#define SETTINGS_CELL_SIZE 16

// A press that has not been released yet is invisible in the log
// Filling the row in afterwards would mean rewriting a line inside a text
// buffer, so the row is held by the window and only written once it is
// complete.
//
// The bound is the engine's own pairing timeout, not a copy: the log prints
// ">2000" as the hold time of a click whose release never came, and that
// number must be the age at which the engine gave up, or the two panes would
// disagree about the same event.
#define SETTINGS_CLICK_EXPIRE_MS DEBOUNCE_BLOCK_TIMEOUT_MS

// One row of a log pane, which is one physical click
//
// The press creates the row, the release that belongs to it fills in the hold
// time. A click therefore never shows up as two rows that the user has to pair
// up in his head.
typedef struct
{
	MouseButton button;
	uint64_t down_tick; // GetTickCount64 of the press

	// Time since the previous event of this button, which is exactly what the
	// engine compares a press against. For a plain click sequence that is
	// "release of the last click to this press". Showing anything else here
	// (for example press to press) would make a blocked click look innocent.
	uint64_t gap_ms;

	uint64_t hold_ms; // This press to its release
	bool has_gap;	  // The row has a previous press to compare against
	bool near_miss;	  // Delivered, but only just cleared the threshold
	bool released;	  // The matching release was seen
	bool expired;	  // The release never came (a swallowed or lost event)
	bool blocked;	  // Counts as a false click
	bool active;	  // Used by pending[], not by the panes
	DebounceReason reason;
} ClickRow;

// Newest first, fixed capacity
typedef struct
{
	ClickRow rows[SETTINGS_LOG_ROWS];
	uint32_t count;
} LogPane;

// Turn a GetTickCount64 stamp into a readable local time
// The snapshot carries a wall clock and a tick that were sampled together, so
// the difference between two ticks can be subtracted from a real time.
void log_format_time(const AppSnapshot *snapshot, uint64_t tick, LangStringId format,
					 wchar_t *buffer, size_t buffer_count);

// Localized verdict for one engine decision
const wchar_t *log_format_verdict(DebounceReason reason);

// Put one completed click at the top of a pane, dropping the oldest row
void log_pane_push(LogPane *pane, const ClickRow *row);

// Render a whole pane into the text the edit control displays
void log_pane_render(const AppSnapshot *snapshot, const LogPane *pane,
					 wchar_t *buffer, size_t buffer_count);

// Render the live pane from the always-on history, newest first.
//
// entries come from EventHistory (already newest first). One row per event and a
// section label whenever the program changes - the label carries the name so the
// rows do not have to repeat it on every line. No hold column: a live row is an
// event, and the release that would give it a hold time may still be coming.
void log_format_live_pane(const AppSnapshot *snapshot, const TraceEntry *entries, uint32_t count,
						  wchar_t *buffer, size_t buffer_count);

// Build the conclusion line: the one thing a user actually wants to know
void log_format_status(const AppSnapshot *snapshot, wchar_t *buffer, size_t buffer_count);

// Convert the report's line feeds into CRLF for the edit control
void log_format_lf_to_crlf(const wchar_t *source, wchar_t *destination, size_t destination_count);

// Build the localized evidence block: which button looked faulty, when it was
// pressed the time before and the time that got blocked, and the gap between.
void log_format_evidence(const AppSnapshot *snapshot, wchar_t *buffer, size_t buffer_count);
