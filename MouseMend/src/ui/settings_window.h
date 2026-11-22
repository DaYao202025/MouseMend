#pragma once

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include "../core/app_snapshot.h"
#include "../core/config.h"
#include "../core/debouncer.h"
#include "../core/event_trace.h"
#include "log_format.h"
#include "ripple_view.h"

// The check-up window ("mouse check-up", formerly the settings window).
//
// It is read-only on purpose: it proves that the protection works and hands the
// user evidence when it does not. Nothing in here can change the configuration.
//
// Every verdict the window shows comes from the debounce engine (see
// DebounceDecision). The window never compares a gap against a threshold of its
// own, so what a user reads here is exactly what the hook did.
//
// Text rendering lives in log_format.c; this file only decides when to render
// and where the result goes.

#define SETTINGS_STATS_SIZE 512
#define SETTINGS_CAPTION_SIZE 128
#define SETTINGS_LOG_CHARS 16384

// A press that has not been released yet is invisible in the log
// Filling the row in afterwards would mean rewriting a line inside a text
// buffer, so the row is held here and only written once it is complete.
//
// The bound is the engine's own pairing timeout, not a copy: the log prints
// ">2000" as the hold time of a click whose release never came, and that number
// must be the age at which the engine gave up, or the two panes would disagree
// about the same event.
// Fills the snapshot the window renders
typedef void (*SettingsSnapshotFn)(AppSnapshot *snapshot, void *user);

// Called when the window is put away into the tray
//
// The window is the main window of the application, so its close button hides
// it instead of destroying it: the log, the counters and the scroll positions
// are exactly what the user expects to find when he opens it again. This hook
// lets the host say where it went, because the tray icon lives there.
typedef void (*SettingsHiddenFn)(void *user);

typedef struct
{
	HWND hwnd;
	HINSTANCE hInstance;
	SettingsSnapshotFn snapshot;
	void *snapshot_user;
	EventTrace *trace;

	// Filter: Auto shows every button, otherwise only filter_button
	bool filter_auto;
	MouseButton filter_button;

	// Per button press counter; statistics only, the log has no number column
	uint32_t click_counts[MOUSE_BUTTON_COUNT];

	// The press of each button that is still waiting for its release
	ClickRow pending[MOUSE_BUTTON_COUNT];

	// Rolling history of the two log panes, newest first
	LogPane test_log;
	LogPane block_log;

	// Press to press gap statistics of this session
	uint64_t gap_min_ms;
	uint64_t gap_max_ms;
	uint64_t gap_sum_ms;
	uint32_t gap_samples;

	// Presses that passed but only just. This is the number that tells a user his
	// switch is failing even when nothing has been blocked yet.
	uint32_t near_miss_count;
	uint64_t near_miss_min_ms;

	// Last rendered summary, so an idle window does not repaint identical strings
	wchar_t last_stats[SETTINGS_STATS_SIZE];

	// Last rendered caption of the blocked pane (it carries the near miss count)
	wchar_t last_caption[SETTINGS_CAPTION_SIZE];

	// When the self check was rebuilt last, so a burst of blocked clicks cannot
	// rebuild a multi-kilobyte string on every single event
	uint64_t last_diag_tick;

	RippleView ripple;

	// Hover tips of the filter controls. One tooltip window serves all four, so a
	// new user can learn what "Auto" does without clicking anything.
	HWND tooltip;

	// Language the captions were last painted with; a change from the tray menu
	// is picked up on the next refresh tick
	Language applied_language;

	// Told when the window is hidden into the tray; may be NULL
	SettingsHiddenFn on_hidden;
	void *hidden_user;

	// Bold font used for the status line
	HFONT status_font;

	// Fixed pitch font of the three text panes, so the log columns line up
	HFONT log_font;
} SettingsWindow;

// Create the window, or bring the existing one to the front
// Parameters:
//   trace - Decision ring the hook writes into; may be NULL (empty log then)
// Returns:
//   true when a window is visible afterwards
bool settings_window_open(SettingsWindow *window, HINSTANCE hInstance, HWND owner,
						  SettingsSnapshotFn snapshot, void *user, EventTrace *trace);

// Check whether the window currently exists, visible or hidden in the tray
bool settings_window_is_open(const SettingsWindow *window);

// Check whether the window is on screen right now
//
// Needed by the message loop: IsDialogMessageW must only be offered a window
// the user can actually see.
bool settings_window_is_visible(const SettingsWindow *window);

// Destroy the window. Used on shutdown; the close button hides it instead.
void settings_window_close(SettingsWindow *window);

// Receive a notification when the user puts the window into the tray
void settings_window_set_hidden_handler(SettingsWindow *window,
										SettingsHiddenFn handler, void *user);
