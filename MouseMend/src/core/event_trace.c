#include "event_trace.h"
#include <string.h>

// Initialize the ring
bool event_trace_init(EventTrace *trace)
{
	if (!trace)
		return false;

	memset(trace, 0, sizeof(*trace));
	InitializeCriticalSection(&trace->cs);
	trace->ready = true;

	return true;
}

// Release the ring
void event_trace_cleanup(EventTrace *trace)
{
	if (!trace || !trace->ready)
		return;

	DeleteCriticalSection(&trace->cs);
	trace->ready = false;
}

// Set the rectangle that is allowed to record
void event_trace_set_capture(EventTrace *trace, const RECT *screen_rect)
{
	if (!trace || !trace->ready)
		return;

	EnterCriticalSection(&trace->cs);

	if (screen_rect)
	{
		trace->capture = *screen_rect;
		trace->capturing = true;
	}
	else
	{
		trace->capturing = false;
	}

	LeaveCriticalSection(&trace->cs);
}

// Append one entry
// Called from the hook callback, so it must stay allocation free and fast.
bool event_trace_push(EventTrace *trace, const TraceEntry *entry)
{
	if (!trace || !entry || !trace->ready)
		return false;

	EnterCriticalSection(&trace->cs);

	if (!trace->capturing)
	{
		LeaveCriticalSection(&trace->cs);
		return false;
	}

	POINT point;
	point.x = entry->x;
	point.y = entry->y;

	if (!PtInRect(&trace->capture, point))
	{
		LeaveCriticalSection(&trace->cs);
		return false;
	}

	// A full ring overwrites its oldest entry: the newest decisions are the ones
	// a user is looking at
	if (trace->count == EVENT_TRACE_CAPACITY)
		trace->dropped++;
	else
		trace->count++;

	trace->entries[trace->head] = *entry;
	trace->head = (trace->head + 1) % EVENT_TRACE_CAPACITY;

	LeaveCriticalSection(&trace->cs);

	return true;
}

// Take the oldest entry
bool event_trace_pop(EventTrace *trace, TraceEntry *entry)
{
	if (!trace || !entry || !trace->ready)
		return false;

	bool found = false;

	EnterCriticalSection(&trace->cs);

	if (trace->count > 0)
	{
		uint32_t tail = (trace->head + EVENT_TRACE_CAPACITY - trace->count) % EVENT_TRACE_CAPACITY;
		*entry = trace->entries[tail];
		trace->count--;
		found = true;
	}

	LeaveCriticalSection(&trace->cs);

	return found;
}

// Drop every queued entry
void event_trace_clear(EventTrace *trace)
{
	if (!trace || !trace->ready)
		return;

	EnterCriticalSection(&trace->cs);
	trace->head = 0;
	trace->count = 0;
	trace->dropped = 0;
	LeaveCriticalSection(&trace->cs);
}

// Number of queued entries
uint32_t event_trace_count(EventTrace *trace)
{
	if (!trace || !trace->ready)
		return 0;

	EnterCriticalSection(&trace->cs);
	uint32_t count = trace->count;
	LeaveCriticalSection(&trace->cs);

	return count;
}
