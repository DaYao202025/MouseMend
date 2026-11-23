#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>
#include "event_trace.h"

// Always on record of the most recent button and wheel events.
//
// EventTrace is deliberately restricted to what the check-up window shows: it
// only records inside the capture rectangle, and only while the window is open.
// That is the right behaviour for a log the user reads, but it is exactly wrong
// for a crash report - the user whose program dies while working in another
// application would hand us an empty history.
//
// So the hook writes every decision into this second ring as well. It costs one
// fixed allocation and a few memory writes per event, never touches the disk,
// and is the only record of what the engine was doing right before a crash.

#define EVENT_HISTORY_CAPACITY 96

typedef struct
{
	TraceEntry entries[EVENT_HISTORY_CAPACITY];
	uint32_t head;	// Index of the oldest entry
	uint32_t count; // Number of valid entries
	CRITICAL_SECTION cs;
	bool ready;
} EventHistory;

// Initialize / release. Safe on a zero-initialised structure.
bool event_history_init(EventHistory *history);
void event_history_cleanup(EventHistory *history);

// Append one entry. Oldest is overwritten when the ring is full.
// Called from the hook callback, so this is memory only.
void event_history_push(EventHistory *history, const TraceEntry *entry);

// Copy the entries, newest first, and return how many were written.
//
// Takes the lock with TryEnter on purpose: this is called from the crash handler,
// where the missing lock may belong to a thread that is already dead. A report
// with no history is a bad report; a report that hangs the dying process is no
// report at all.
uint32_t event_history_copy(EventHistory *history, TraceEntry *out, uint32_t capacity);

// Drop everything, so a manual export starts from a known state
void event_history_clear(EventHistory *history);
