#pragma once

#include <stdbool.h>
#include <windows.h>
#include "support_bundle.h"

// Crash capture for the faults Windows would otherwise swallow.
//
// A report that only says "it closed itself" cannot be turned into a fix, and a
// folder next to the executable is not somewhere a non-technical user looks. So
// one crash produces one zip in a place he can find: the executable's folder
// first, then the desktop, then %LOCALAPPDATA%, then %TEMP%.
//
// The filter deliberately avoids the CRT: a corrupted heap is exactly the
// situation it has to survive, so it only calls Win32 and keeps large buffers
// static.

// Supplies the application specific half of the report. Called from inside the
// exception filter, so it must not allocate, wait on a lock, or call anything
// that can block. May be NULL, in which case only the dump is written.
typedef void (*CrashReportCollectFn)(SupportReport *report);

// Install the filter. Call once, early in WinMain.
void crash_dump_install(void);

// Register the collector above. Call before crash_dump_install.
void crash_dump_set_report_collector(CrashReportCollectFn collector);
