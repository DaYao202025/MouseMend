#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>
#include "mouse_hook.h"

// Opt-in history of blocked clicks, written to <app>\Log\YYYYMMDD.log.
//
// The hook callback must never touch the file system: a low-level hook that
// overruns its time budget is silently removed by Windows. Therefore the callback
// only pushes a fixed size record into a ring buffer, and the UI thread drains it
// and appends to the log.
//
// The log has to survive the user deleting it: the option stays on, the directory
// is recreated on the next blocked click.

#define BLOCK_LOG_RING_SIZE 64
#define BLOCK_LOG_DIR_NAME L"Log"
#define BLOCK_LOG_BUTTON_SLUG_SIZE 12

// One blocked event, stored in a form that needs no time API in the hook callback
typedef struct
{
	uint64_t elapsed_ms;  // Milliseconds since the log base was captured
	MouseButton button;	  // Which button was blocked
	bool is_down;		  // true = a press was swallowed, false = a release
	uint32_t threshold_ms; // Threshold that was active at that moment
} BlockLogEntry;

// Ring buffer shared between the hook callback and the UI thread
typedef struct
{
	BlockLogEntry entries[BLOCK_LOG_RING_SIZE];
	int head;			  // Index of the oldest entry
	int count;			  // Number of queued entries
	uint32_t dropped;	  // Entries lost because the consumer fell behind
	CRITICAL_SECTION cs;  // Guards the whole structure
	bool ready;
} BlockLogQueue;

// Initialize / release the queue (safe to call on a zero-initialised struct once)
bool block_log_init(BlockLogQueue *queue);
void block_log_cleanup(BlockLogQueue *queue);

// Queue one blocked event (called from the hook callback, memory only)
bool block_log_push(BlockLogQueue *queue, MouseButton button, bool is_down,
					uint64_t elapsed_ms, uint32_t threshold_ms);

// Dequeue the oldest event (called from the UI thread)
bool block_log_pop(BlockLogQueue *queue, BlockLogEntry *entry);

// Number of events dropped because the queue was full
uint32_t block_log_dropped(BlockLogQueue *queue);

// Number of queued events
int block_log_count(BlockLogQueue *queue);

// Stable, language independent name used in the log file
const char *block_log_button_slug(MouseButton button);

// Format the local date of an entry as YYYYMMDD (used as the file name)
bool block_log_format_day(uint64_t elapsed_ms, const SYSTEMTIME *base,
						  wchar_t *buffer, size_t buffer_count);

// Format a complete log line, including the trailing CRLF
bool block_log_format_line(const BlockLogEntry *entry, const SYSTEMTIME *base,
						   wchar_t *buffer, size_t buffer_count);

// Resolve the log directory: <exe directory>\Log, or %LOCALAPPDATA%\MouseMend\Log
// when the executable directory is not writable. Creates the directory.
bool block_log_resolve_directory(wchar_t *buffer, size_t buffer_count);

// Describe the intended directory without creating it (used by diagnostics)
bool block_log_describe_directory(wchar_t *buffer, size_t buffer_count);

// Append one entry to today's file, creating the directory and file as needed
bool block_log_write(const wchar_t *directory, const SYSTEMTIME *base, const BlockLogEntry *entry);
