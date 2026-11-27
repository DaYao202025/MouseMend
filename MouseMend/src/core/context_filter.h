#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

// Cheap "should this event be filtered at all" decision for the mouse hook.
//
// Split in two on purpose:
//   ContextSnapshot - written by the 2 s polling thread, read by the hook.
//                     The hook never takes a lock.
//   ContextPolicy   - the mode and the two path lists. Modified by the UI
//                     thread, read by the polling thread under ContextFilter.lock.
//
// The lock lives in ContextFilter (not in the policy) so the policy stays pure
// POD: memcpy, memset and copy are all safe. This is the lesson from the
// 2026-11-26 tray-menu crash, where an uninitialised critical section was
// entered from a zeroed structure.

#define CONTEXT_LIST_MAX 16
#define CONTEXT_PATH_CHARS 1024
#define CONTEXT_NAME_CHARS 64

// Longest path rendered into a menu line before it is shortened. A menu can
// hold more, but past this the line starts competing with the rest of the
// submenu for width.
#define CONTEXT_MENU_PATH_CHARS 64
#define CONTEXT_PAUSE_MS (5u * 60u * 1000u)

typedef enum
{
	CONTEXT_MODE_ALL = 0,	// every program is filtered (default)
	CONTEXT_MODE_WHITELIST, // only the listed programs are filtered
	CONTEXT_MODE_BLACKLIST, // the listed programs are not filtered
	CONTEXT_MODE_COUNT
} ContextMode;

typedef enum
{
	CONTEXT_ADD_OK = 0,
	CONTEXT_ADD_SELF,		// this program itself
	CONTEXT_ADD_RESERVED,	// a Windows process that is never a foreground window
	CONTEXT_ADD_DUPLICATE,	// already in the same list
	CONTEXT_ADD_IN_OTHER,	// already in the other list
	CONTEXT_ADD_FULL,		// list is full
	CONTEXT_ADD_INVALID		// empty, too long or contains a wildcard
} ContextAddResult;

typedef struct
{
	LONG pid;		 // 0 = unknown
	LONG suppressed; // 0/1, the only field the hook reads
	LONG ours;		 // our own window is never suppressed
	wchar_t name[CONTEXT_NAME_CHARS];
} ContextSlot;

// Hot path: the hook reads current and slots[current].suppressed only.
typedef struct
{
	ContextSlot slots[2];
	volatile LONG current;
	volatile LONG64 pause_until; // in-process tick, never persisted
} ContextSnapshot;

typedef struct
{
	wchar_t paths[CONTEXT_LIST_MAX][CONTEXT_PATH_CHARS];
	uint32_t count;
} ContextPathList;

// Pure data. No lock inside, so it can be copied and zeroed freely.
typedef struct
{
	ContextMode mode;
	ContextPathList whitelist;
	ContextPathList blacklist;
	wchar_t recent_path[CONTEXT_PATH_CHARS]; // for "exclude the current program"
	wchar_t recent_name[CONTEXT_NAME_CHARS];
} ContextPolicy;

typedef struct
{
	ContextSnapshot snap;
	SRWLOCK lock; // the only lock; initialised explicitly in context_filter_init
	__declspec(align(64)) ContextPolicy policy;

	// Bumped on every change to mode or to either list. The polling thread
	// caches the foreground pid, and without this it would keep serving the
	// verdict it computed before the user edited the lists - "I added it and
	// nothing changed" until he switched to another window and back.
	volatile LONG revision;
} ContextFilter;

void context_filter_init(ContextFilter *filter);

// ---------------------------------------------------------------- policy side
// Called by the UI thread (add/remove/mode) and by the polling thread (compute).

// True when this rule matches the program. Rule forms:
//   "C:\Games\game.exe"  full path, compared case-insensitively (then via long name)
//   "*\game.exe"         legacy rule, compared against the file name only
bool context_path_matches(const wchar_t *rule, const wchar_t *full_path, const wchar_t *file_name);

// Windows processes that are never a foreground window. Adding one would do
// nothing while looking like it did, so the UI refuses them outright.
bool context_program_is_reserved(const wchar_t *file_name);

// Shell and host processes: explorer hosts the desktop, ApplicationFrameHost
// hosts every UWP app. They are legitimate choices (desktop chatter is a real
// problem) but the consequence is large, so the UI asks first.
bool context_program_is_shell(const wchar_t *file_name);

void context_path_list_clear(ContextPathList *list);
bool context_path_list_contains(const ContextPathList *list, const wchar_t *full_path,
								const wchar_t *file_name);
const wchar_t *context_path_list_at(const ContextPathList *list, uint32_t index);

// What the current mode means for one program.
bool context_mode_suppresses(ContextMode mode, bool in_whitelist, bool in_blacklist);

// Add/remove with the full five-step check. The caller does not take the lock.
ContextAddResult context_filter_add_path(ContextFilter *filter, ContextMode list,
										 const wchar_t *full_path);
bool context_filter_remove_path(ContextFilter *filter, ContextMode list, const wchar_t *full_path);
void context_filter_set_mode(ContextFilter *filter, ContextMode mode);
ContextMode context_filter_get_mode(ContextFilter *filter);
void context_filter_reset(ContextFilter *filter); // mode = ALL, both lists empty

// Compute whether the programme should be suppressed right now. Takes the lock.
bool context_filter_compute_suppress(ContextFilter *filter, const wchar_t *full_path,
									 const wchar_t *file_name);

// Copy the policy out for display (takes the lock).
void context_filter_copy_policy(ContextFilter *filter, ContextPolicy *out);

// ------------------------------------------------------------ snapshot side
void context_filter_publish(ContextFilter *filter, uint32_t pid, bool suppressed, bool ours,
							const wchar_t *name);

void context_filter_set_pause(ContextFilter *filter, uint64_t now_tick, uint32_t duration_ms);
bool context_filter_is_paused(const ContextFilter *filter, uint64_t now_tick);

// Hook side. Reads one atomic index and one aligned LONG. Must never lock.
bool context_filter_should_suppress(const ContextSnapshot *snapshot, uint64_t now_tick);
bool context_snapshot_is_paused(const ContextSnapshot *snapshot, uint64_t now_tick);

// Extract the last component of a path. Shared by the service and the tests.
void context_file_name_from_path(const wchar_t *path, wchar_t *name, size_t count);

// Render a rule for a menu line: the path, shortened in the middle when it is
// long.
//
// A bare file name made two rules on different paths look identical, and the
// only way to learn which one a click would delete was to click it. A full path
// is not the answer either: menus clip from the right, which is exactly where
// the file name sits, so an overlong entry would lose the one part that
// identifies it. Shortening in the middle keeps the drive and the name.
void context_shorten_path(const wchar_t *full_path, wchar_t *out, size_t count);
