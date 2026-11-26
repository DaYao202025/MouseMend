#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

// Temporary instrumentation, built only into a dedicated binary.
//
// Everything here disappears when MOUSEMEND_TRACE is not defined: the shipped
// Release build contains none of this code and the call sites compile to
// nothing. It exists to answer the one question the unit tests cannot - whether
// a release the engine let through is the release the system acted on.
//
// Nothing in this module may allocate, lock or touch the disk on the hook path.
// Records go into a ring buffer with two memory writes; a background thread
// turns them into text.

typedef enum
{
	TRACE_LIFE_APP_START = 0,
	TRACE_LIFE_APP_STOP,
	TRACE_LIFE_HOOK_INSTALL,
	TRACE_LIFE_HOOK_REMOVE,
	TRACE_LIFE_HOOK_RESTART,
	TRACE_LIFE_SETTINGS_OPEN,
	TRACE_LIFE_SETTINGS_CLOSE,
	TRACE_LIFE_TIMER_ARM,
	TRACE_LIFE_TIMER_STOP,
	TRACE_LIFE_COUNT
} TraceLifecycle;

#ifdef MOUSEMEND_TRACE

// Start recording into %TEMP%\MouseMend-trace. False when the file cannot be
// created, in which case the program runs exactly as it does without tracing.
bool trace_start(void);
void trace_stop(void);
bool trace_active(void);

// File the records go to, valid after a successful trace_start
const wchar_t *trace_path(void);

// One button event: what arrived, what the engine decided, and what Windows
// believes about the button immediately afterwards.
void trace_button(int button, bool is_down, bool injected, int x, int y,
				  const char *verdict, bool swallowed);

// Pointer movement, sampled inside this module so the hook stays cheap
void trace_move(int x, int y);

// One of our own injections, as it left the process
void trace_injection(const INPUT *inputs, UINT count, bool delivered);

void trace_lifecycle(TraceLifecycle what);

#else

static __inline bool trace_start(void) { return false; }
static __inline void trace_stop(void) {}
static __inline bool trace_active(void) { return false; }

static __inline void trace_button(int button, bool is_down, bool injected, int x, int y,
								  const char *verdict, bool swallowed)
{
	(void)button; (void)is_down; (void)injected; (void)x; (void)y;
	(void)verdict; (void)swallowed;
}

static __inline void trace_move(int x, int y) { (void)x; (void)y; }
static __inline void trace_injection(const INPUT *inputs, UINT count, bool delivered)
{
	(void)inputs; (void)count; (void)delivered;
}
static __inline void trace_lifecycle(TraceLifecycle what) { (void)what; }

#endif
