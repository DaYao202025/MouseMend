#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>
#include "debouncer.h"
#include "mouse_hook.h"

// Bounded record of the decisions the debounce engine made for button and wheel
// events.
//
// The hook callback appends (memory only, no allocation, no IO), the check-up
// window drains from the UI thread. Movement events are never recorded: they
// would push the interesting events out of the ring within a second.
//
// When the ring is full the oldest entry is overwritten. A window that was
// closed therefore starts with an empty history instead of replaying stale
// clicks, because it clears the ring when it opens.

#define EVENT_TRACE_CAPACITY 128

typedef struct
{
	MouseButton button;
	uint64_t timestamp; // GetTickCount64 of the event, straight from the hook
	long x;				// Screen coordinates, so the window can hit test a ripple
	long y;
	bool is_down; // Wheel events report false here, exactly like the hook
	DebounceDecision decision;
} TraceEntry;

typedef struct
{
	TraceEntry entries[EVENT_TRACE_CAPACITY];
	uint32_t head; // Next slot to write
	uint32_t count;
	uint32_t dropped;

	// Only events whose screen position lies inside this rectangle are recorded
	// while a check-up window is open. Clicks on the window's own controls - the
	// filter check boxes, the scrollbars, the log text - have no place in a log
	// about the mouse, and they would scroll the pane away under the user's hand.
	RECT capture;
	bool capturing;

	CRITICAL_SECTION cs;
	bool ready;
} EventTrace;

// Initialize / destroy the ring
bool event_trace_init(EventTrace *trace);
void event_trace_cleanup(EventTrace *trace);

// Set the rectangle that is allowed to record, in screen coordinates
// Pass NULL to stop recording altogether (window closed).
void event_trace_set_capture(EventTrace *trace, const RECT *screen_rect);

// Append one entry (hook thread). Returns false when the trace is not ready, is
// not capturing, or the event happened outside the capture rectangle.
bool event_trace_push(EventTrace *trace, const TraceEntry *entry);

// Take the oldest entry (UI thread). Returns false when there is nothing left.
bool event_trace_pop(EventTrace *trace, TraceEntry *entry);

// Drop every queued entry, so a freshly opened window starts from zero
void event_trace_clear(EventTrace *trace);

// Number of queued entries
uint32_t event_trace_count(EventTrace *trace);
