#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>
#include "../core/event_history.h"
#include "../core/mouse_hook.h"

// One file a user can attach to a bug report.
//
// A crash message that only says "it closed itself" cannot be acted on, and the
// folders the program writes dumps into are exactly the folders a non-technical
// user never opens. So a crash, or an explicit "export diagnostics", produces a
// single zip next to the executable that contains the minidump and a text
// report: the dump says where it died, the report says what it was doing.
//
// Everything here is allocation free and CRT free, because the crash path runs
// inside a process whose heap may already be destroyed.

#define SUPPORT_TEXT_MAX 24576
#define SUPPORT_README_MAX 8192
#define SUPPORT_EVENT_MAX EVENT_HISTORY_CAPACITY
#define SUPPORT_KEEP_BUNDLES 3

// File name prefixes. They are also the retention groups: pruning never deletes
// a file it did not create.
#define SUPPORT_PREFIX_CRASH L"MouseMend-crash"

// Used to be "MouseMend-support". Renamed because there is no support desk to
// speak of: one person writes this, and a file called "support" reads like a
// ticket number. Bundles written by older versions keep the old name - pruning
// only ever touches the current prefix, so they are left where they are instead
// of being matched and deleted by a name the user never saw us use.
#define SUPPORT_PREFIX_MANUAL L"MouseMend-diagnostics"

// Everything the report is made of. Filled by the application (which is the only
// place that knows g_app) and by support_bundle_prepare, then rendered once.
typedef struct
{
	// When
	wchar_t stamp[20];	   // 20261123-081500, reused in the file name
	wchar_t generated[32]; // 2026-11-23 08:15:00
	uint64_t now_tick;
	uint64_t app_start_tick; // 0 when the application never got that far

	// Where
	wchar_t exe_path[MAX_PATH];
	wchar_t os_version[64];
	wchar_t arch[8];
	wchar_t language[8];

	// What happened
	bool crashed;
	bool complete; // false when a value could not be read without waiting
	uint32_t exception_code;
	uint64_t exception_address;
	wchar_t faulting_module[MAX_PATH];
	uint64_t faulting_offset;

	// Configuration in force at the time
	bool hybrid;
	uint32_t release_delay_ms;
	uint32_t drag_distance_px;
	bool notify_blocks;
	bool log_blocks;
	wchar_t log_directory[MAX_PATH];
	uint32_t thresholds[MOUSE_BUTTON_COUNT];
	bool monitored[MOUSE_BUTTON_COUNT];
	uint32_t blocks[MOUSE_BUTTON_COUNT];
	uint32_t total_blocks;

	// Hook health
	bool hook_active;
	uint32_t hook_events;
	uint32_t hook_recoveries;

	// Process resources: a leak shows up here long before it kills the process
	uint32_t handle_count;
	uint32_t gdi_objects;
	uint32_t user_objects;

	// Recent decisions, newest first
	TraceEntry events[SUPPORT_EVENT_MAX];
	uint32_t event_count;
} SupportReport;

// Fill the parts that do not come from the application: clock, paths, OS,
// architecture and the current resource counts. Safe to call on a zeroed struct.
void support_bundle_prepare(SupportReport *report, bool crashed);

// Describe an address as "module + offset", which stays meaningful across builds
// unlike the raw address a crash dialog shows. Buffer may stay empty when the
// address does not belong to any image.
void support_bundle_describe_address(uint64_t address,
									 wchar_t *module, size_t module_count,
									 uint64_t *offset);

// Render the machine readable report. ASCII only, one value per line: it has to
// survive a round trip through a forum post, an email and a text editor.
bool support_bundle_build_text(const SupportReport *report, char *buffer, size_t buffer_count);

// Write the bundle, trying <exe folder>, then the desktop, then
// %LOCALAPPDATA%\MouseMend, then %TEMP%. Returns the path actually used.
//
// dump_source may be NULL, in which case the bundle carries the text report only.
bool support_bundle_write(SupportReport *report, const wchar_t *dump_source,
						  wchar_t *path, size_t path_count);

// Newest bundle with this prefix in any of the folders support_bundle_write
// would use. Used on the next start to tell the user where the file went.
bool support_bundle_find_newest(const wchar_t *prefix, wchar_t *path, size_t path_count);
