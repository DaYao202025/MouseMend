#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

#include "learning.h"
#include "signal_ring.h"

// How the worker asks for the threshold the advice has to be relative to.
//
// A function pointer instead of a DebounceManager pointer on purpose: this
// module has no business knowing what the engine looks like. When the engine
// changes a field name, only the provider in main.c needs to follow - the
// learning service does not even see it.
//
// Implementations MUST NOT take a lock. The worker calls this seven times per
// poll, and a locked accessor would have it competing with the hook thread for
// the engine's global critical section every 100 ms.
typedef uint32_t (*LearningThresholdFn)(int button, void *user);

// Background owner of the learning advice. The hook never touches this type;
// it only writes SignalRing with Interlocked operations. The tray menu reads a
// copied snapshot under the short critical section below.
typedef struct
{
	SignalRing *ring;
	LearningThresholdFn threshold_fn;
	void *user;
	LearningAdvice advice;

	// Total samples seen at the last recompute. Recomputing on this instead of
	// on "the gesture ended" is what makes the menu update at all: a user who is
	// testing the feature chatters continuously, so a quiet window never arrives
	// and the advice would stay empty while the histogram was already full.
	uint32_t last_total;

	// cs_alive: the critical section was initialised at least once. It is never
	// deleted, so once this is true entering it is always safe.
	// ready: the worker thread is running.
	//
	// Learning is opt-in, so the common path is "never started": readers must
	// check cs_alive before EnterCriticalSection, or they fault inside ntdll on
	// a zeroed structure. This is the 2026-11-26 tray-menu crash.
	bool cs_alive;
	bool ready;
	CRITICAL_SECTION cs;
	HANDLE stop_event;
	HANDLE thread;
} LearningService;

bool learning_service_start(LearningService *service, SignalRing *ring,
							LearningThresholdFn threshold_fn, void *user);
void learning_service_stop(LearningService *service);
void learning_service_snapshot(LearningService *service, LearningAdvice *out);

// Same copy, but it never waits for the lock.
//
// For the crash and hang paths: those run while another thread may be holding
// this critical section, including a thread that is already dead. A report with
// "learning: unknown" is a bad report; a report that hangs the dying process is
// no report at all - the same rule event_history_try_copy follows.
// Returns false when the lock was busy, and then leaves *out zeroed.
bool learning_service_try_snapshot(LearningService *service, LearningAdvice *out);

void learning_service_reset(LearningService *service);

// Whether the worker thread is running. Checks "ready" first on purpose:
// "cs_alive" only means the critical section was initialised once, which stays
// true after a start that failed before CreateThread. Safe on a zeroed struct.
bool learning_service_is_started(const LearningService *service);
