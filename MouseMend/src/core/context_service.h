#pragma once

#include <stdbool.h>
#include <windows.h>

#include "context_filter.h"

// Low-frequency owner of the foreground-program cache. It never runs on the
// hook thread: OpenProcess and QueryFullProcessImageName are far too expensive
// (and can block) for an input filter.
typedef struct
{
	ContextFilter *filter;
	HANDLE stop_event;
	HANDLE thread;

	// Set once the polling thread has resolved a foreground program at least
	// once. Without it "the thread exists" and "the filter has ever seen a
	// program" look identical, and the second one is what the health line needs.
	volatile LONG observed;
} ContextService;

bool context_service_start(ContextService *service, ContextFilter *filter);
void context_service_stop(ContextService *service);

// Safe on a zeroed struct: both pointers are checked before use.
bool context_service_is_running(const ContextService *service);

// The most recently observed foreground program is published into
// ContextPolicy.recent_path / recent_name by the polling thread: the tray menu
// itself is the foreground window by the time a click arrives, so the UI has to
// read a copy instead of looking it up again.
