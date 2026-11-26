#pragma once

#include <stdbool.h>
#include <stdint.h>

// Whether each optional capability is actually working, as opposed to merely
// "was configured". The tray icon can say "protection on" while the hook has
// been silently removed, the lists can say "configured" while the polling
// thread never started, and learning can say "on" while its worker has nothing
// to compute with. Those are the cases users report as "it does not work" and
// nobody can tell which part broke.
//
// Pure logic with a POD input: main.c fills the inputs, this decides the state,
// log_format renders it. That keeps it testable without linking main.c.
typedef enum
{
	CAP_HOOK = 0, // the low-level mouse hook
	CAP_LEARNING, // self-learning worker
	CAP_CONTEXT,  // foreground-program polling
	CAP_LOG,	  // block log writer
	CAP_COUNT
} AppCapability;

typedef enum
{
	CAP_STATE_DISABLED = 0, // the user turned it off: nothing to report
	CAP_STATE_OK,		    // on and demonstrably working
	CAP_STATE_UNKNOWN,	    // on, but nothing observed yet
	CAP_STATE_STALLED	    // on, but not working (or paused)
} CapabilityState;

// Everything the decision depends on, copied by value. No pointers into live
// structures: a snapshot taken while another thread writes must not be able to
// crash this, and it must be constructible in a test with memset 0.
typedef struct
{
	bool hook_ready;
	bool hook_installed;
	bool hook_healthy;

	bool learning_enabled;
	bool learning_started;
	bool learning_has_samples; // samples > 0 at the last observation

	bool context_active; // mode != ALL
	bool context_started;
	bool context_paused;
	bool context_observed; // a foreground program was resolved at least once

	bool log_enabled;
	bool log_ready;
	bool log_directory_resolved;
} CapabilityInputs;

// Safe with a NULL input (everything disabled) and a NULL output (no-op).
void app_capabilities_evaluate(const CapabilityInputs *in,
							   CapabilityState out[CAP_COUNT]);
