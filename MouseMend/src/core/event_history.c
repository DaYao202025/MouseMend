#include "event_history.h"
#include <string.h>

bool event_history_init(EventHistory *history)
{
	if (!history)
		return false;

	memset(history, 0, sizeof(*history));

	// Ready is set last: copy() keys off it, and a ring whose lock was never
	// initialised must never be treated as readable.
	InitializeCriticalSection(&history->cs);
	history->ready = true;
	return true;
}

void event_history_cleanup(EventHistory *history)
{
	if (!history || !history->ready)
		return;

	DeleteCriticalSection(&history->cs);
	history->ready = false;
}

void event_history_push(EventHistory *history, const TraceEntry *entry)
{
	if (!history || !history->ready || !entry)
		return;

	EnterCriticalSection(&history->cs);

	if (history->count < EVENT_HISTORY_CAPACITY)
	{
		uint32_t slot = (history->head + history->count) % EVENT_HISTORY_CAPACITY;
		history->entries[slot] = *entry;
		history->count++;
	}
	else
	{
		// Full: overwrite the oldest slot and move the window forward
		history->entries[history->head] = *entry;
		history->head = (history->head + 1) % EVENT_HISTORY_CAPACITY;
	}

	InterlockedIncrement(&history->revision);

	LeaveCriticalSection(&history->cs);
}

bool event_history_try_copy(EventHistory *history, TraceEntry *out, uint32_t capacity,
							uint32_t *count)
{
	if (count)
		*count = 0;

	if (!history || !history->ready || !out || capacity == 0)
		return false;

	// TryEnter, not Enter: a crash report must never wait for a thread that may
	// have died while holding this lock.
	if (!TryEnterCriticalSection(&history->cs))
		return false;

	uint32_t written = 0;

	// Walk backwards from the newest entry so the report reads newest first
	for (uint32_t i = 0; i < history->count && written < capacity; i++)
	{
		uint32_t offset = history->count - 1 - i;
		uint32_t slot = (history->head + offset) % EVENT_HISTORY_CAPACITY;
		out[written++] = history->entries[slot];
	}

	LeaveCriticalSection(&history->cs);

	if (count)
		*count = written;

	return true;
}

uint32_t event_history_copy(EventHistory *history, TraceEntry *out, uint32_t capacity)
{
	uint32_t written = 0;

	event_history_try_copy(history, out, capacity, &written);
	return written;
}

uint32_t event_history_revision(EventHistory *history)
{
	if (!history || !history->ready)
		return 0;

	return (uint32_t)InterlockedCompareExchange(&history->revision, 0, 0);
}

void event_history_clear(EventHistory *history)
{
	if (!history || !history->ready)
		return;

	EnterCriticalSection(&history->cs);
	history->head = 0;
	history->count = 0;
	InterlockedIncrement(&history->revision);
	LeaveCriticalSection(&history->cs);
}
