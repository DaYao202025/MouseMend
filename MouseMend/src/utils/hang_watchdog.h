#pragma once

#include <stdbool.h>
#include <windows.h>
#include "support_bundle.h"

// Capture for a program that did not crash, it stopped answering.
//
// A hang never reaches the exception filter, so before this existed a frozen
// MouseMend left nothing behind at all: no dump, no report, and the only way to
// remove the process was to sign out. The machine this was written for froze
// three times and each one cost a hand-made dump collected over a chat window.
//
// The thread below only watches. It cannot rescue a wedged process - a thread
// stuck in the kernel cannot be pulled out of it from user mode - but it can
// write down what the process looked like at that moment, which is the
// difference between a fixable bug and a mystery.
//
// Ten seconds without a heartbeat is chosen against the timer that produces the
// heartbeat: it already fires every 200 ms, so a real stall is obvious long
// before that, and a machine under heavy load will not produce a false report.

#define HANG_WATCHDOG_TIMEOUT_MS 10000

// How often the watcher looks. Also the latency of a report, so keeping it well
// below the timeout costs nothing and shortens the window.
#define HANG_WATCHDOG_POLL_MS 2000

// Supplies the application specific half of the report, the same one the crash
// handler uses. May be NULL, in which case only the dump-based part is written.
typedef void (*HangReportCollectFn)(SupportReport *report);

// Create the watcher thread. Returns false when it could not be created, which
// is not fatal: everything else keeps working, only hang capture is missing.
bool hang_watchdog_start(HangReportCollectFn collector);

// Stop the watcher and wait for it, with a bounded wait so shutdown cannot hang.
void hang_watchdog_stop(void);

// Heartbeat. Call from the UI thread's timer; silence is what triggers a report.
void hang_watchdog_beat(void);

// Whether a hang report was written during this run (diagnostics only)
bool hang_watchdog_triggered(void);
