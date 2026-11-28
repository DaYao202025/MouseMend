#pragma once

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include "../core/app_snapshot.h"
#include "../core/config.h"
#include "../core/debouncer.h"
#include "../core/event_history.h"
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
#define SETTINGS_STATUS_SIZE 384

// Blocks of the status strip: the overall verdict followed by the four
// optional modules.
#define SETTINGS_STATUS_BLOCKS 5

// One block of the status strip.
//
// The strip is a single owner-draw control that paints five blocks. Idle, every
// block is a square showing one dim character; the block under the pointer
// widens until its full caption fits and lights up.
//
// Width and light are animated by exponential approach, not by a timed
// interpolation: each frame covers a fixed fraction of the remaining distance.
// A pointer that jumps between blocks therefore re-targets smoothly from
// wherever the block currently is, instead of restarting and snapping.
typedef struct
{
	float width;	   // current width, pixels
	float from;		   // width when the running animation started
	float to;		   // width the running animation is heading to
	uint64_t start_ms; // when the running animation started
	float duration_ms; // budget of the running animation: opening costs more
					   // than closing, so the row clears space quickly
	float target;	   // width that fits the full caption
	float idle;		   // square width, equal to the control height
	float level;	   // 0 = collapsed, 1 = open; derived from width
} StatusBlock;

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

// Told to put one line into the status bar for a few seconds
//
// The window owns no notice slot: the notice has to survive this dialog being
// hidden and be visible in the same second cell as the hook recoveries, so it
// lives in the application and the window only asks.
typedef void (*SettingsNoticeFn)(void *user, const wchar_t *text, uint32_t milliseconds);

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

	// Always-on ring the hook writes into, and the one the live pane reads.
	//
	// Separate from trace on purpose: trace only records inside the test area and
	// only while this window is open, which is right for a log about the test
	// area and useless for a log about the programs the user actually uses.
	EventHistory *history;

	// Revision of the live pane's source when it was last painted
	uint32_t live_revision;

	// Filter: "All" shows every button, otherwise only filter_button.
	// The name stays filter_auto because the flag means "no per button filter";
	// only the label under the check box changed.
	bool filter_auto;
	MouseButton filter_button;

	// Last wheel that drew a ring, and when. A high resolution wheel reports a
	// stream of events for one gesture, so the ring is throttled per wheel; see
	// trigger_ripple.
	MouseButton wheel_ripple_button;
	uint64_t wheel_ripple_tick;

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

	// Last rendered status sentence and capability states. Re-setting identical
	// text on the status control every 400 ms repainted the whole line, and that
	// is exactly what made it flicker.
	wchar_t last_status[SETTINGS_STATUS_SIZE];
	uint8_t last_caps[CAP_COUNT];

	// Status strip, painted by one owner-draw static. Block 0 is the overall
	// verdict, blocks 1..4 are the optional modules.
	//
	// The captions live here rather than in the control: an owner-draw static has
	// no text of its own, and WM_DRAWITEM can arrive long after the refresh that
	// produced the state.
	StatusBlock strip[SETTINGS_STATUS_BLOCKS];
	uint8_t strip_state[SETTINGS_STATUS_BLOCKS];
	wchar_t strip_full[SETTINGS_STATUS_BLOCKS][64];

	// Pointer is inside the unmarked hot zone next to the strip, which is what
	// makes its hint appear. The zone draws nothing while this is false.
	bool egg_hover;

	// Block under the pointer, or -1. Only drives the "this one is clickable"
	// highlight and the hover tip now - it no longer decides any width.
	int strip_hover;

	// Which block is open, and therefore the only thing that can re-lay the row
	// out. Changed by a click, never by the pointer moving across the strip.
	//
	// The pointer used to open blocks, and it could not be made to feel smooth:
	// a block's left edge is the sum of the widths before it, so opening whatever
	// the pointer is over pushes every block after it away - the target keeps
	// moving as the pointer approaches it. A click is a deliberate act, so the
	// row is allowed to move once and then stay still.
	int strip_pinned;

	// Hover tip of the block under the pointer.
	//
	// A collapsed block shows one character, so the words have to come from
	// somewhere; and TTM_UPDATETIPTEXTW keeps the pointer instead of copying the
	// string, so the buffer has to live as long as the tooltip does.
	wchar_t strip_tip[192];

	// Pause state of context filtering, mirrored from the snapshot.
	//
	// Cached because the caption is built in status_strip_set_caption, which has
	// no snapshot at hand, and because the countdown only needs a repaint when the
	// displayed second changes - not on every 400 ms tick.
	bool strip_paused;
	uint32_t strip_pause_left_s;

	// Dialog font. The pills are drawn by hand, so they have to select this
	// explicitly or they would come out in the system font and look foreign.
	HFONT base_font;

	// Last rendered caption of the blocked pane (it carries the near miss count)
	wchar_t last_caption[SETTINGS_CAPTION_SIZE];

	RippleView ripple;

	// Panes waiting for a rewrite, and when the last one happened. A burst of
	// clicks is shown in one go instead of one rewrite per frame.
	unsigned pending_panes;
	uint64_t panes_painted_ms;

	// Whether the animation timer is currently running at the fast rate, and
	// when the last frame was drawn
	bool anim_fast;
	uint64_t last_anim_ms;

	// Hover tips of the filter controls. One tooltip window serves all four, so a
	// new user can learn what "Auto" does without clicking anything.
	HWND tooltip;

	// Language the captions were last painted with; a change from the tray menu
	// is picked up on the next refresh tick
	Language applied_language;

	// Told when the window is hidden into the tray; may be NULL
	SettingsHiddenFn on_hidden;
	void *hidden_user;

	// Told to put a line into the status bar; may be NULL
	SettingsNoticeFn notice;
	void *notice_user;

	// Bold font used for the status line
	HFONT status_font;

	// Fixed pitch font of the three text panes, so the log columns line up
	HFONT log_font;
} SettingsWindow;

// Create the window, or bring the existing one to the front
// Parameters:
//   trace   - Decision ring the hook writes into; may be NULL (empty log then)
//   history - Always-on ring the live pane reads; may be NULL (pane stays empty)
// Returns:
//   true when a window is visible afterwards
bool settings_window_open(SettingsWindow *window, HINSTANCE hInstance, HWND owner,
						  SettingsSnapshotFn snapshot, void *user, EventTrace *trace,
						  EventHistory *history);

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

// Let the window put a line into the status bar
void settings_window_set_notice_handler(SettingsWindow *window,
										SettingsNoticeFn handler, void *user);
