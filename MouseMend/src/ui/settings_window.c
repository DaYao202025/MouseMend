#include "settings_window.h"
#include "language.h"
#include "help_window.h"
#include "status_bar.h"
#include "window_layout.h"
#include "../../resource.h"
#include "../core/config.h"
#include "../core/context_filter.h"
#include <strsafe.h>
#include <shellapi.h>
#include <commctrl.h>
#include <windowsx.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "comctl32.lib")

#define SETTINGS_TIMER_REFRESH 1
#define SETTINGS_REFRESH_MS 400

// The log panes are drained on a fast timer so a press shows up immediately;
// the same timer drives the expanding rings.
//
// 16 ms is one frame at 60 Hz. The rings are time based, so a late tick draws
// the ring where it should be rather than where a frame counter thinks it is;
// running at 30 Hz was what made the expansion look like it was stepping.
#define SETTINGS_TIMER_ANIM 2
#define SETTINGS_ANIM_MS 16

// Idle rate of the same timer, used when nothing is moving.
//
// The window shares its thread with the mouse hook, and a low level hook is
// dropped by the system when its callback overruns. Sixty two wake-ups a second
// for a frame nobody is looking at costs battery and puts that many chances of
// a late hook delivery into every second the window is open.
#define SETTINGS_ANIM_IDLE_MS 50

// How long the fast rate is kept after the last frame, so the ring that follows
// a pause still starts on the frame the click happened on
#define SETTINGS_ANIM_GRACE_MS 1500

// Minimum time between two pane rewrites. A pane is a plain EDIT control and
// rewriting one pushes its whole content through the message system, which is
// the most expensive thing the animation tick does. A burst of clicks used to
// rewrite it on every frame and starve the rings; five updates a second is
// indistinguishable in a log only a human reads.
#define SETTINGS_PANE_MS 100

// The one item of the hot zone's menu. Local to this file: the menu is built and
// consumed here, and TrackPopupMenu returns the id instead of posting it.
#define SETTINGS_EGG_ALWAYS_ON_TOP 1

// Entries moved from the ring into the panes per animation tick. Each one costs
// a row format and, when the pane is rewritten, a copy of a 16 KB buffer, so the
// work per tick has to stay bounded no matter how long the log was closed.
#define SETTINGS_TRACE_DRAIN_PER_TICK 8

// Cell height of the fixed pitch font used by the text panes, in pixels at 96 dpi
// The log column widths in language.c are measured against this size: one cell
// is about 7.3 px here, which is what makes a whole row fit the pane.
#define SETTINGS_LOG_FONT_PIXELS 11

// How long a block takes to open, in milliseconds.
//
// Fixed duration rather than fixed distance: the user asked for "fully open
// within 100 ms", and a duration is also what makes every block finish at the
// same moment instead of the wider ones trailing behind.
#define SETTINGS_STATUS_OPEN_MS 100.0f

// How long a block takes to close. Zero, on purpose.
//
// A block's left edge is the sum of the current widths of the blocks before it,
// so as long as the closing block is still mid-animation, every block after it
// slides left while it grows - which reads as "it opens from the left". Closing
// instantly reflows the layout before a single frame of the opening animation is
// drawn, leaving only the right edge in motion.
#define SETTINGS_STATUS_CLOSE_MS 0.0f

// Gap between two blocks, and the padding inside a block once it is expanded.
#define SETTINGS_STATUS_GAP 3
#define SETTINGS_STATUS_PAD 4

// Extra width reserved past the measured caption, 3 px on each side. Text
// measurement and the actual glyph run do not agree to the pixel, and without
// this the last character of a long caption gets clipped by a hair.
#define SETTINGS_STATUS_SLACK 6

static void apply_language(HWND hDlg);
static void update_control_tooltips(HWND hDlg, SettingsWindow *window);
static void refresh_capabilities(HWND hDlg, SettingsWindow *window,
								 const uint8_t states[CAP_COUNT], bool hook_active);
static void status_strip_set_caption(SettingsWindow *window, int index);
static void status_strip_measure(HWND hDlg, SettingsWindow *window);
static void update_status_bar(HWND hDlg, SettingsWindow *window,
							  const AppSnapshot *snapshot);
static void set_anim_rate(HWND hDlg, SettingsWindow *window, bool fast);

// Store the window state on the dialog (modeless dialogs have no DWLP_USER by
// default, so it is set in WM_INITDIALOG and cleared in WM_DESTROY).
static SettingsWindow *get_window(HWND hDlg)
{
	return (SettingsWindow *)GetWindowLongPtrW(hDlg, DWLP_USER);
}

// "Which decisions count as a false click" lives in the engine, so the log, the
// self check and the notification can never disagree about it.
static bool is_false_click(const DebounceDecision *decision)
{
	return debounce_decision_is_false_click(decision);
}

// Whether the current filter wants this entry
static bool entry_is_visible(const SettingsWindow *window, const TraceEntry *entry)
{
	if (window->filter_auto)
		return true;

	// "Middle" also means the wheel: the wheel is the middle button pushed in a
	// different direction, and someone who filtered for the middle button wants
	// its scrolling too. The engine still judges the three as independent
	// buttons - this is a display filter, not a merge.
	if (window->filter_button == MOUSE_BUTTON_MIDDLE)
		return entry->button == MOUSE_BUTTON_MIDDLE || entry->button == MOUSE_BUTTON_WHEEL ||
			   entry->button == MOUSE_BUTTON_HWHEEL;

	return entry->button == window->filter_button;
}

// Start a ring when the press happened inside the test area
static void trigger_ripple(HWND hDlg, SettingsWindow *window, const TraceEntry *entry)
{
	HWND canvas = GetDlgItem(hDlg, IDC_RIPPLE);
	if (!canvas)
		return;

	RECT bounds;
	if (!GetWindowRect(canvas, &bounds))
		return;

	POINT point;
	point.x = entry->x;
	point.y = entry->y;

	// The hook reports screen coordinates, and the click may well have landed in
	// another window: only a point inside the canvas deserves a ring.
	if (!PtInRect(&bounds, point))
		return;

	// A wheel reports a stream of events for one gesture - a high resolution
	// wheel sends dozens for a single flick - and one ring per event would look
	// like a machine gun. While the ring for that wheel is still on screen the
	// next notch stays quiet. Scrolling the other way is a different button, so
	// it gets its own ring immediately.
	if (config_is_wheel_button(entry->button))
	{
		if (window->wheel_ripple_button == entry->button &&
			entry->timestamp - window->wheel_ripple_tick < RIPPLE_LIFETIME_MS)
			return;

		window->wheel_ripple_button = entry->button;
		window->wheel_ripple_tick = entry->timestamp;
	}

	// data carries the wheel delta, which is the only way to tell up from down
	ripple_view_trigger(&window->ripple, entry->button, entry->data, point.x - bounds.left,
						point.y - bounds.top, entry->timestamp);
}

// Tell the trace which part of the screen belongs to the test area
//
// Only presses inside it may be logged. Without this the window's own controls
// feed the log: pressing a radio button, pulling a scrollbar or selecting text
// in a pane would each add a row, drop the user's scroll position and rewrite
// the text under his hand while he is trying to read it.
static void update_capture_rect(HWND hDlg, SettingsWindow *window)
{
	if (!window->trace)
		return;

	HWND canvas = GetDlgItem(hDlg, IDC_RIPPLE);
	RECT bounds;

	// A minimized or hidden window has no usable rectangle: GetWindowRect would
	// report the off-screen icon position and clicks there would be recorded.
	if (!canvas || IsIconic(hDlg) || !IsWindowVisible(hDlg) || !GetWindowRect(canvas, &bounds))
	{
		event_trace_set_capture(window->trace, NULL);
		return;
	}

	event_trace_set_capture(window->trace, &bounds);
}

// Which log pane has new content
// Rewriting the other pane would throw away the selection and scroll position a
// user is reading, so each pane is only touched when its own text changed.
#define SETTINGS_LOG_CHANGED 0x1
#define SETTINGS_BLOCK_CHANGED 0x2

// Remember how close a delivered press came to being blocked
//
// Only presses, and only ones the engine actually delivered: a blocked press is
// already in the blocked counter, and a release has no meaningful gap.
static void record_near_miss(SettingsWindow *window, const ClickRow *row)
{
	if (!row->near_miss)
		return;

	window->near_miss_count++;

	if (window->near_miss_count == 1 || row->gap_ms < window->near_miss_min_ms)
		window->near_miss_min_ms = row->gap_ms;
}

// Record the press to press gap of this session
// The fastest and the slowest are what a user compares his own mouse against:
// a bounce shows up as a suspiciously small minimum even before it is blocked.
static void record_gap(SettingsWindow *window, const ClickRow *row)
{
	// A scroll rhythm is not a click rhythm; mixing them would let a slow wheel
	// drag the average up and hide a real double click
	if (!row->has_gap || config_is_wheel_button(row->button))
		return;

	if (window->gap_samples == 0 || row->gap_ms < window->gap_min_ms)
		window->gap_min_ms = row->gap_ms;

	if (window->gap_samples == 0 || row->gap_ms > window->gap_max_ms)
		window->gap_max_ms = row->gap_ms;

	window->gap_sum_ms += row->gap_ms;
	window->gap_samples++;
}

// Move one finished click into the panes
// Returns which panes changed.
static unsigned commit_click(SettingsWindow *window, ClickRow *row)
{
	unsigned changed = 0;

	log_pane_push(&window->test_log, row);
	changed |= SETTINGS_LOG_CHANGED;

	if (row->blocked)
	{
		log_pane_push(&window->block_log, row);
		changed |= SETTINGS_BLOCK_CHANGED;
	}

	row->active = false;
	return changed;
}

// Move every queued decision into the visible logs
// Returns which panes changed.
//
// The ring holds events, the panes hold clicks. A press opens a row, the release
// that belongs to it closes the row, and only a closed row is shown: that is
// what keeps one click on one line instead of a "press" line plus a "release"
// line the user has to pair up himself.
static unsigned drain_trace(HWND hDlg, SettingsWindow *window)
{
	unsigned changed = 0;

	if (!window->trace)
		return 0;

	TraceEntry entry;
	AppSnapshot snapshot = {0};

	// Bounded drain: a backlog (or a button that bounces faster than the timer)
	// would otherwise rebuild the pane once per entry in a single tick and
	// freeze the window. Whatever is left waits for the next tick.
	int budget = SETTINGS_TRACE_DRAIN_PER_TICK;

	while (budget-- > 0 && event_trace_pop(window->trace, &entry))
	{
		// The ring is physical feedback, not log content: it appears for every
		// press inside the test area even when the filter hides that button. A
		// wheel has no press, so it qualifies by being one discrete notch.
		if (entry.is_down || config_is_wheel_button(entry.button))
			trigger_ripple(hDlg, window, &entry);

		if (!entry_is_visible(window, &entry))
			continue;

		if (snapshot.now_tick == 0 && window->snapshot)
			window->snapshot(&snapshot, window->snapshot_user);

		ClickRow *pending = &window->pending[entry.button];

		// A scroll has no press and no release: the event is the whole action
		if (config_is_wheel_button(entry.button))
		{
			ClickRow row = {0};
			row.button = entry.button;
			row.down_tick = entry.timestamp;
			row.gap_ms = entry.decision.elapsed_ms;
			row.has_gap = !entry.decision.first_event;
			row.released = true;
			row.blocked = is_false_click(&entry.decision);
			row.near_miss = entry.decision.near_miss;
			row.reason = entry.decision.reason;

			window->click_counts[entry.button]++;
			record_gap(window, &row);
			record_near_miss(window, &row);
			changed |= commit_click(window, &row);
			continue;
		}

		if (entry.is_down)
		{
			// A second press while the previous one is still open means its
			// release never arrived. Report that row as incomplete instead of
			// letting this press overwrite it.
			if (pending->active)
			{
				pending->released = false;
				pending->expired = true;
				changed |= commit_click(window, pending);
			}

			pending->button = entry.button;
			pending->down_tick = entry.timestamp;
			pending->gap_ms = entry.decision.elapsed_ms;
			pending->has_gap = !entry.decision.first_event;
			pending->hold_ms = 0;
			pending->released = false;
			pending->expired = false;
			pending->blocked = is_false_click(&entry.decision);
			pending->near_miss = entry.decision.near_miss;
			pending->reason = entry.decision.reason;
			pending->active = true;

			window->click_counts[entry.button]++;
			record_gap(window, pending);
			record_near_miss(window, pending);
		}
		else if (pending->active)
		{
			pending->released = true;
			pending->hold_ms = (entry.timestamp >= pending->down_tick)
								   ? (entry.timestamp - pending->down_tick)
								   : 0;
			changed |= commit_click(window, pending);
		}
	}

	// A press whose release was swallowed or lost must not stay open forever
	uint64_t now = GetTickCount64();

	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		ClickRow *row = &window->pending[i];

		if (!row->active)
			continue;

		if (now >= row->down_tick && (now - row->down_tick) < SETTINGS_CLICK_EXPIRE_MS)
			continue;

		row->released = false;
		row->expired = true;
		changed |= commit_click(window, row);
	}

	return changed;
}

// Replace the text of a log pane and keep the newest line visible
// The newest line is the first one, so "visible" means scrolled to the top.
static void show_log(HWND hDlg, int control_id, const wchar_t *text)
{
	HWND edit = GetDlgItem(hDlg, control_id);
	if (!edit)
		return;

	// Nothing else on purpose: SetWindowTextW already leaves the caret and the
	// scroll position at the top, which is where the newest row is. Forcing them
	// with EM_SETSEL/EM_SCROLLCARET used to run ten times a second and threw away
	// whatever the user had selected - the pane is read only precisely so a row
	// can be copied out of it, and that copy could never be finished.
	SetWindowTextW(edit, text);
}

// Render one pane and put it on screen
// The buffer is static because the window is a single UI thread object and a
// 32 KB stack frame is not worth the risk on a thread that also runs the hook.
static void show_pane_text(HWND hDlg, SettingsWindow *window, int control_id, const LogPane *pane)
{
	static wchar_t text[SETTINGS_LOG_CHARS];
	AppSnapshot snapshot = {0};

	if (window->snapshot)
		window->snapshot(&snapshot, window->snapshot_user);

	log_pane_render(&snapshot, pane, text, ARRAYSIZE(text));
	show_log(hDlg, control_id, text);
}

// Build the one line summary above the blocked pane
//
// It answers the question the log raises: "is my mouse actually bad?". The
// fastest gap is the tell - a switch that bounces produces a minimum far below
// anything a finger can do - and the average is what makes that number readable.
static void build_stats_text(const SettingsWindow *window, const AppSnapshot *snapshot,
							 wchar_t *buffer, size_t buffer_count)
{
	if (!buffer || buffer_count == 0)
		return;

	buffer[0] = L'\0';

	uint32_t clicks = 0;
	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
		clicks += window->click_counts[i];

	// Nothing has happened yet: the reassuring line beats a row of zeros
	if (snapshot->total_blocks == 0 && clicks == 0)
	{
		StringCchCopyW(buffer, buffer_count, lang_str(STR_BLOCKED_NONE));
		return;
	}

	// Only scrolls so far: there is no click rhythm to report, but the click
	// count is still worth showing
	if (window->gap_samples == 0)
	{
		StringCchPrintfW(buffer, buffer_count, lang_str(STR_STATS_CLICKS_FMT),
						 (unsigned)snapshot->total_blocks, (unsigned)clicks);
		return;
	}

	uint64_t average = window->gap_sum_ms / window->gap_samples;

	StringCchPrintfW(buffer, buffer_count, lang_str(STR_STATS_SUMMARY_FMT),
					 (unsigned)snapshot->total_blocks, (unsigned)clicks,
					 (unsigned)window->gap_min_ms, (unsigned)window->gap_max_ms,
					 (unsigned)average);
}

// Caption of the blocked pane, with the near miss count when there is one
//
// The count belongs next to the blocked number, not in the summary line: a user
// reads "0 blocked" and stops. "0 blocked (7 near miss)" is the sentence that
// tells him the switch is failing and the threshold is simply too low.
static void build_block_caption(const SettingsWindow *window, wchar_t *buffer, size_t buffer_count)
{
	if (!buffer || buffer_count == 0)
		return;

	if (window->near_miss_count == 0)
	{
		StringCchCopyW(buffer, buffer_count, lang_str(STR_GROUP_STATS));
		return;
	}

	StringCchPrintfW(buffer, buffer_count, lang_str(STR_GROUP_STATS_NEAR_FMT),
					 (unsigned)window->near_miss_count);
}

// Refresh the live parts of the window
static void refresh_live(HWND hDlg, SettingsWindow *window)
{
	if (!window->snapshot)
		return;

	// The language can change from the tray menu while this window is open. The
	// captions are not repainted by anything else, so the check lives here: it is
	// the one place that already runs on a timer.
	Language language = language_get();

	if (language != window->applied_language)
	{
		window->applied_language = language;
		apply_language(hDlg);
		update_control_tooltips(hDlg, window);

		// The chart hint and the pane captions are drawn or formatted elsewhere
		window->last_stats[0] = L'\0';
		window->last_caption[0] = L'\0';

		// Strip captions are built from the language table, so the cached states
		// have to be thrown away or the old language would stay on screen until
		// the next state change. Block 0 is not driven by last_caps, hence the
		// explicit refill.
		memset(window->last_caps, 0xFF, sizeof(window->last_caps));
		status_strip_set_caption(window, 0);

		InvalidateRect(GetDlgItem(hDlg, IDC_RIPPLE), NULL, TRUE);
	}

	AppSnapshot snapshot = {0};
	window->snapshot(&snapshot, window->snapshot_user);

	// The verdict used to be a separate control with its own cached string. The
	// strip derives block 0 from this same flag, so there is nothing to set here;
	// refresh_capabilities takes care of it and only repaints on a real change.
	refresh_capabilities(hDlg, window, snapshot.capabilities, snapshot.hook_active);

	// Bottom bar: the foreground verdict is the one decision that used to have no
	// visible answer anywhere. status_bar_set compares before writing, so this
	// 400 ms path does not repaint a caption that has not changed.
	update_status_bar(hDlg, window, &snapshot);

	// Pause state drives the verdict block. Placed after refresh_capabilities on
	// purpose: that call owns strip_state[0], and running this first would let it
	// overwrite the pause caption with "protection on".
	{
		uint32_t left_s = snapshot.context_paused
							  ? (snapshot.context_pause_left_ms + 999u) / 1000u
							  : 0u;

		if (snapshot.context_paused != window->strip_paused ||
			left_s != window->strip_pause_left_s)
		{
			window->strip_paused = snapshot.context_paused;
			window->strip_pause_left_s = left_s;

			status_strip_set_caption(window, 0);

			// The caption gets wider with every extra digit, so the block has to be
			// measured again or the countdown is clipped at the last character.
			status_strip_measure(hDlg, window);
			InvalidateRect(GetDlgItem(hDlg, IDC_STATUS), NULL, FALSE);
		}
	}

	// Only touch a control when its text actually changed: repainting identical
	// strings every tick is pointless work on the thread that also runs the hook.
	wchar_t stats[SETTINGS_STATS_SIZE];
	build_stats_text(window, &snapshot, stats, ARRAYSIZE(stats));

	if (wcscmp(stats, window->last_stats) != 0)
	{
		StringCchCopyW(window->last_stats, ARRAYSIZE(window->last_stats), stats);
		SetDlgItemTextW(hDlg, IDC_STATS, stats);
	}

	// The caption changes as soon as a press comes close to the threshold, which
	// is the moment the user most needs to see the number
	wchar_t caption[SETTINGS_CAPTION_SIZE];
	build_block_caption(window, caption, ARRAYSIZE(caption));

	if (wcscmp(caption, window->last_caption) != 0)
	{
		StringCchCopyW(window->last_caption, ARRAYSIZE(window->last_caption), caption);
		SetDlgItemTextW(hDlg, IDC_GROUP_STATS, caption);
	}

	// The log folder button is only meaningful while logging is enabled
	EnableWindow(GetDlgItem(hDlg, IDC_OPEN_LOG), snapshot.logging && snapshot.log_directory[0] != L'\0');
}

// Rewrite the live pane from the always-on history
//
// The history is the only record that covers every program rather than just the
// test area, and the hook already writes it for the crash report. The pane is a
// reader of that ring, not a second recorder.
//
// The paint is skipped unless the history actually changed. That is the rule the
// self check used to break: re-setting identical text ten times a second makes an
// EDIT control flicker, and this pane is redrawn far more often than that.
static void render_live_log(HWND hDlg, SettingsWindow *window)
{
	if (!window || !window->history)
		return;

	if (event_history_revision(window->history) == window->live_revision)
		return;

	static TraceEntry entries[EVENT_HISTORY_CAPACITY];
	static wchar_t text[SETTINGS_LOG_CHARS];

	uint32_t count = 0;

	// A busy ring means the hook is pushing right now: leave the pane alone and
	// take it on the next tick. Blurring this with "empty" would blank the pane
	// every time a click arrived at the wrong moment.
	if (!event_history_try_copy(window->history, entries, EVENT_HISTORY_CAPACITY, &count))
		return;

	window->live_revision = event_history_revision(window->history);

	AppSnapshot snapshot = {0};
	if (window->snapshot)
		window->snapshot(&snapshot, window->snapshot_user);

	log_format_live_pane(&snapshot, entries, count, text, ARRAYSIZE(text));
	show_log(hDlg, IDC_LIVELOG, text);
}

// Empty both logs
// The column header is a separate control above each pane, so it survives a
// clear and stays visible while the text is scrolled.
static void clear_logs(HWND hDlg, SettingsWindow *window)
{
	memset(&window->test_log, 0, sizeof(window->test_log));
	memset(&window->block_log, 0, sizeof(window->block_log));

	// A press that is still waiting for its release belongs to the view that is
	// being thrown away; keeping it would put an orphan row in the new one.
	memset(window->pending, 0, sizeof(window->pending));
	memset(window->click_counts, 0, sizeof(window->click_counts));

	window->gap_min_ms = 0;
	window->gap_max_ms = 0;
	window->gap_sum_ms = 0;
	window->gap_samples = 0;
	window->near_miss_count = 0;
	window->near_miss_min_ms = 0;

	// The summary is part of the view: it must not describe clicks that are no
	// longer in the log
	window->last_stats[0] = L'\0';
	window->last_caption[0] = L'\0';

	show_log(hDlg, IDC_TESTLOG, L"");
	show_log(hDlg, IDC_BLOCKLOG, L"");

	ripple_view_reset(&window->ripple);
	InvalidateRect(GetDlgItem(hDlg, IDC_RIPPLE), NULL, TRUE);

	if (window->trace)
		event_trace_clear(window->trace);

	// The live pane's source goes too: the button means "start over", and a pane
	// that kept its rows would read as a click that did nothing. The cost is that
	// a crash in the next minutes reports an empty history - the user's own
	// choice at the moment he pressed it.
	if (window->history)
		event_history_clear(window->history);

	window->live_revision = 0xFFFFFFFFu;
	show_log(hDlg, IDC_LIVELOG, L"");

	refresh_live(hDlg, window);
}

// The five single button filters, in the order they sit in the dialog.
//
// One table for all three jobs: the captions, the enabled/checked state, and the
// click handler. Five copies of the same list would drift apart, and the copy
// nobody looks at is the one that drifts.
static const struct
{
	int control;
	MouseButton button;
	LangStringId tip;
} FILTER_BUTTONS[] = {
	{IDC_FILTER_LEFT, MOUSE_BUTTON_LEFT, STR_TIP_FILTER_LEFT},
	{IDC_FILTER_MIDDLE, MOUSE_BUTTON_MIDDLE, STR_TIP_FILTER_MIDDLE},
	{IDC_FILTER_RIGHT, MOUSE_BUTTON_RIGHT, STR_TIP_FILTER_RIGHT},
	{IDC_FILTER_X1, MOUSE_BUTTON_X1, STR_TIP_FILTER_X1},
	{IDC_FILTER_X2, MOUSE_BUTTON_X2, STR_TIP_FILTER_X2},
};

#define FILTER_BUTTON_COUNT ((int)(sizeof(FILTER_BUTTONS) / sizeof(FILTER_BUTTONS[0])))

// Controls that carry a hover tip, and the string each one shows
//
// It started as the filter row's list and now also covers the two buttons whose
// label cannot carry a whole sentence: "Clear view" has to say that it leaves the
// files on disk alone, and "Block log" has to say that it holds blocked clicks
// only. A tip is the cheapest place for both - the alternative was a longer
// label, and there is no room for one next to four other buttons.
static const struct
{
	int control;
	LangStringId text;
} CONTROL_TIPS[] = {
	{IDC_FILTER_AUTO, STR_TIP_FILTER_ALL},
	{IDC_FILTER_LEFT, STR_TIP_FILTER_LEFT},
	{IDC_FILTER_MIDDLE, STR_TIP_FILTER_MIDDLE},
	{IDC_FILTER_RIGHT, STR_TIP_FILTER_RIGHT},
	{IDC_FILTER_X1, STR_TIP_FILTER_X1},
	{IDC_FILTER_X2, STR_TIP_FILTER_X2},
	{IDC_CLEAR, STR_TIP_CLEAR},
	{IDC_OPEN_LOG, STR_TIP_LOG_DIR},
};

#define CONTROL_TIP_COUNT ((int)(sizeof(CONTROL_TIPS) / sizeof(CONTROL_TIPS[0])))

// Create the hover tips listed in CONTROL_TIPS
//
// A tooltip is the cheapest way to explain a control that is disabled most of the
// time: a new user hovers "Middle" and immediately learns why it cannot be
// clicked yet, instead of clicking around until something happens.
static void create_control_tooltips(HWND hDlg, SettingsWindow *window)
{
	window->tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
									  WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
									  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
									  hDlg, NULL, window->hInstance, NULL);

	if (!window->tooltip)
		return;

	// Without a max width a long tip becomes a single line across the screen
	SendMessageW(window->tooltip, TTM_SETMAXTIPWIDTH, 0, 320);

	for (int i = 0; i < CONTROL_TIP_COUNT; i++)
	{
		TOOLINFOW tool = {0};
		tool.cbSize = sizeof(tool);
		tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
		tool.hwnd = hDlg;
		tool.uId = (UINT_PTR)GetDlgItem(hDlg, CONTROL_TIPS[i].control);
		tool.lpszText = (LPWSTR)lang_str(CONTROL_TIPS[i].text);

		if (tool.uId)
			SendMessageW(window->tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool);
	}

	// The status strip needs a tip of its own now that a block is opened by a
	// click: a square with one character in it cannot explain itself, and the
	// text is per block, so the tool is added once and its text is replaced as
	// the pointer moves (see strip_tip_refresh).
	{
		TOOLINFOW strip = {0};
		strip.cbSize = sizeof(strip);
		strip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
		strip.hwnd = hDlg;
		strip.uId = (UINT_PTR)GetDlgItem(hDlg, IDC_STATUS);
		strip.lpszText = window->strip_tip;

		if (strip.uId)
			SendMessageW(window->tooltip, TTM_ADDTOOLW, 0, (LPARAM)&strip);
	}
}

// --------------------------------------------------------------- status strip ---
//
// One owner-draw control paints five blocks: the overall verdict, then one per
// optional module. The previous shape was five separate controls, which cannot
// change width without moving five windows on every frame - that is also what
// makes them drift apart. Here all five blocks are laid out in one client
// rectangle, so a block starts exactly where the last frame ended, at every DPI,
// with no cross-control alignment to get wrong.
//
// The pointer used to open whatever block it was over. That was replaced by a
// click on 2026-11-28, after real use showed it cannot be made to feel smooth:
//
//   * a block's left edge is the sum of the widths before it, so opening the
//     block under the pointer pushes every block after it away - the thing the
//     pointer is aiming at keeps moving as the pointer approaches it, which no
//     easing curve can hide;
//   * while filtering is paused the verdict block counts down, and a countdown
//     is a width: a narrower digit is enough to move every hit rectangle to the
//     right of it, so the block the pointer is over changes by itself;
//   * the seams made it worse: the pointer is between two blocks for a few
//     milliseconds on every crossing.
//
// So the pointer no longer decides any width. It highlights the block it is over
// and it is what asks for the tip; a click opens a block. Width only changes
// once per deliberate action, which is what makes the row still.

static const LangStringId CAP_NAMES[CAP_COUNT] = {
	STR_CAP_HOOK, STR_CAP_LEARNING, STR_CAP_CONTEXT, STR_CAP_LOG};

// One character per block while it is collapsed. English needs distinct initials
// because "Learn" and "List" would otherwise both read "L".
static const LangStringId CAP_SHORT[CAP_COUNT] = {
	STR_CAP_SHORT_HOOK, STR_CAP_SHORT_LEARNING, STR_CAP_SHORT_CONTEXT, STR_CAP_SHORT_LOG};

// One palette per block, four states each: five hues, twenty colours, no two
// alike. The hue identifies which block it is (green verdict, blue hook, violet
// learning, amber lists, teal log); the four shades inside a hue identify the
// state, always in the same order - disabled palest, stalled darkest and most
// saturated.
//
// Colour can no longer carry the whole verdict by itself, which is the price of
// five distinct hues: "stalled" is the darkest block of its own hue instead of a
// universal red. That is acceptable because the caption states the state in
// words, and because one dark block among four paler ones is still the odd one
// out at a glance.
//
// Column order matches CapabilityState: DISABLED, OK, UNKNOWN, STALLED.
//
// Disabled is desaturated, not lightened: a pale tint would end up brighter
// than a dimmed "ok" block and read as the healthiest one on the row.
// The verdict block while filtering is paused. Amber, one step warmer than its
// normal green: "not broken, but not protecting either".
//
// Kept out of the table above on purpose. Paused is a state of one block, not of
// the four capability states, and adding a fifth column would leave four unused
// slots that someone will eventually read by mistake.
static const COLORREF STRIP_PAUSED_COLOR = RGB(0xC7, 0x7A, 0x2B);

static const COLORREF STRIP_LIT[SETTINGS_STATUS_BLOCKS][4] = {
	// 0: verdict, green
	{RGB(0x7A, 0x94, 0x84), RGB(0x2E, 0x8B, 0x57), RGB(0x6F, 0xA6, 0x88), RGB(0x1B, 0x5E, 0x3A)},
	// 1: hook, blue
	{RGB(0x7A, 0x8A, 0x9C), RGB(0x2B, 0x6C, 0xB0), RGB(0x6B, 0x9B, 0xC8), RGB(0x1A, 0x45, 0x73)},
	// 2: learning, violet
	{RGB(0x8E, 0x84, 0xA0), RGB(0x7C, 0x4D, 0xBE), RGB(0xA2, 0x84, 0xD4), RGB(0x4E, 0x2C, 0x7E)},
	// 3: lists, amber
	{RGB(0xA8, 0x94, 0x84), RGB(0xC7, 0x7A, 0x2B), RGB(0xD9, 0xA6, 0x6C), RGB(0x8A, 0x4E, 0x14)},
	// 4: log, teal
	{RGB(0x7E, 0x94, 0x94), RGB(0x1F, 0x8A, 0x8A), RGB(0x63, 0xAE, 0xAE), RGB(0x10, 0x56, 0x56)},
};

static LangStringId cap_state_text(uint8_t state)
{
	switch (state)
	{
	case CAP_STATE_OK:
		return STR_CAP_STATE_OK;
	case CAP_STATE_UNKNOWN:
		return STR_CAP_STATE_UNKNOWN;
	case CAP_STATE_STALLED:
		return STR_CAP_STATE_STALLED;
	default:
		return STR_CAP_STATE_DISABLED;
	}
}

static uint8_t cap_clamp(uint8_t state)
{
	return state > (uint8_t)CAP_STATE_STALLED ? (uint8_t)CAP_STATE_UNKNOWN : state;
}

// No tooltip on the strip: hovering already reveals the full caption, and a tip
// repeating what is now on screen would only cover the block being read.

// Draw text vertically centred inside rc.
//
// DrawText with DT_VCENTER is the only reliable way to put hand-drawn text on
// the same baseline as a real button. Guessing at y offsets from the font
// metrics is what makes owner-draw drift between DPI settings.
static void draw_centered_text(HDC dc, const RECT *rc, const wchar_t *text,
							   COLORREF color, HFONT font, UINT align)
{
	if (!dc || !rc || !text || !text[0])
		return;

	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, color);

	HGDIOBJ old_font = font ? SelectObject(dc, font) : NULL;

	RECT r = *rc;
	DrawTextW(dc, text, -1, &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

	if (old_font)
		SelectObject(dc, old_font);
}

// Small helpers instead of <math.h>: the project builds as plain C, and pulling
// in the CRT float helpers for two functions is not worth the dependency.
static float float_abs(float value)
{
	return value < 0.0f ? -value : value;
}

static int color_channel(int value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;

	return value;
}

static COLORREF lerp_color(COLORREF from, COLORREF to, float t)
{
	int red = (int)(GetRValue(from) + (GetRValue(to) - GetRValue(from)) * t + 0.5f);
	int green = (int)(GetGValue(from) + (GetGValue(to) - GetGValue(from)) * t + 0.5f);
	int blue = (int)(GetBValue(from) + (GetBValue(to) - GetBValue(from)) * t + 0.5f);

	return RGB(color_channel(red), color_channel(green), color_channel(blue));
}

static LangStringId block_short_name(int index)
{
	return index == 0 ? STR_STATUS_SHORT_STRIP : CAP_SHORT[index - 1];
}

// Hover tip of the block under the pointer.
//
// A collapsed block shows one character, so the words have to come from
// somewhere. This is also the only place that can say what a click will do,
// because that depends on whether the block is already open.
static void strip_tip_refresh(SettingsWindow *window)
{
	if (!window || !window->tooltip || !window->hwnd)
		return;

	window->strip_tip[0] = L'\0';

	int index = window->strip_hover;

	if (index >= 0 && index < SETTINGS_STATUS_BLOCKS)
	{
		// The open verdict block is the resting state: a click on it does nothing,
		// so it gets the caption alone rather than a promise it cannot keep.
		bool resting_verdict = (index == 0 && window->strip_pinned == 0);

		if (resting_verdict)
		{
			StringCchCopyW(window->strip_tip, ARRAYSIZE(window->strip_tip),
						   window->strip_full[index]);
		}
		else
		{
			LangStringId hint = (index == window->strip_pinned) ? STR_STRIP_TIP_COLLAPSE
															   : STR_STRIP_TIP_EXPAND;

			StringCchPrintfW(window->strip_tip, ARRAYSIZE(window->strip_tip), L"%s\n%s",
							 window->strip_full[index], lang_str(hint));
		}
	}

	TOOLINFOW tool = {0};
	tool.cbSize = sizeof(tool);
	tool.hwnd = window->hwnd;
	tool.uFlags = TTF_IDISHWND;
	tool.uId = (UINT_PTR)GetDlgItem(window->hwnd, IDC_STATUS);
	tool.lpszText = window->strip_tip;

	SendMessageW(window->tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&tool);
}

// Fill the caption of one block from its stored state.
static void status_strip_set_caption(SettingsWindow *window, int index)
{
	if (!window || index < 0 || index >= SETTINGS_STATUS_BLOCKS)
		return;

	// Paused is neither "protection on" nor "broken": filtering is off because
	// the user asked it to be, and the block says so with the time left. The
	// countdown is in minutes and seconds; the pause is capped at 720 minutes,
	// so the widest caption this can produce is "已暂停 · 剩余 720:00".
	if (index == 0 && window->strip_paused)
	{
		uint32_t left = window->strip_pause_left_s;

		StringCchPrintfW(window->strip_full[0], ARRAYSIZE(window->strip_full[0]),
						 lang_str(STR_STATUS_STRIP_PAUSED_FMT), (int)(left / 60u),
						 (int)(left % 60u));
	}
	else if (index == 0)
	{
		LangStringId text = window->strip_state[0] == (uint8_t)CAP_STATE_OK
								? STR_STATUS_STRIP_OK
								: STR_STATUS_STRIP_BAD;

		StringCchCopyW(window->strip_full[0], ARRAYSIZE(window->strip_full[0]),
					   lang_str(text));
	}
	else
	{
		uint8_t state = cap_clamp(window->strip_state[index]);

		StringCchPrintfW(window->strip_full[index], ARRAYSIZE(window->strip_full[index]),
						 lang_str(STR_CAP_LABEL_FMT),
						 lang_str(CAP_NAMES[index - 1]), lang_str(cap_state_text(state)));
	}

	// The tip quotes the caption, so a caption that changes while the pointer is
	// on it has to be re-sent. The countdown does exactly that, once a second.
	if (window->strip_hover == index)
		strip_tip_refresh(window);
}

// Controls whose clicks mean "I am still reading this": the read-only panes and
// the test area. A click there must not fold the strip back - selecting a line in
// the log or clicking into the test area is part of the same look, and yanking
// the row away under the pointer would be the jitter again, only slower.
static bool keeps_strip_pinned(int control_id)
{
	switch (control_id)
	{
	case IDC_TESTLOG:
	case IDC_BLOCKLOG:
	case IDC_LIVELOG:
	case IDC_RIPPLE:
		return true;

	default:
		return false;
	}
}

// Fold the strip back to the verdict block.
//
// The expanded block is a drawer, not a setting: it is a temporary look at one
// module, and there are exactly two moments that put it away - the user clicks
// somewhere that is not this strip, or the user leaves the window. No timer does
// it: a row that re-lays itself out while nobody touched it is precisely what
// the hover model was removed for.
static void status_strip_reset(HWND hDlg, SettingsWindow *window)
{
	if (!window || window->strip_pinned == 0)
		return;

	window->strip_pinned = 0;

	strip_tip_refresh(window);
	set_anim_rate(hDlg, window, true);
	InvalidateRect(GetDlgItem(hDlg, IDC_STATUS), NULL, FALSE);
}

// Measure every caption and decide how wide its block has to be.
//
// Called on each state or language change: the same state is a different width
// in the other language, and a stale target would clip the caption.
static void status_strip_measure(HWND hDlg, SettingsWindow *window)
{
	if (!hDlg || !window)
		return;

	HWND control = GetDlgItem(hDlg, IDC_STATUS);
	if (!control)
		return;

	RECT client;
	if (!GetClientRect(control, &client))
		return;

	float height = (float)(client.bottom - client.top);
	if (height < 1.0f)
		height = 14.0f;

	HDC dc = GetDC(control);
	HGDIOBJ old_font = (dc && window->base_font) ? SelectObject(dc, window->base_font) : NULL;

	for (int i = 0; i < SETTINGS_STATUS_BLOCKS; i++)
	{
		StatusBlock *block = &window->strip[i];
		SIZE extent = {0};
		const wchar_t *caption = window->strip_full[i];
		wchar_t widest[64];

		// While paused the verdict block counts down, and a countdown is a width:
		// one narrower digit, or the tenth minute, moves its right edge. Because
		// every block after it starts where it ends, that single pixel would shift
		// the whole row - and the hit rectangles with it. Reserving the width of
		// the longest form keeps the row still for the entire pause; only the
		// glyphs inside it change.
		if (i == 0 && window->strip_paused &&
			SUCCEEDED(StringCchPrintfW(widest, ARRAYSIZE(widest),
									   lang_str(STR_STATUS_STRIP_PAUSED_FMT),
									   CONTEXT_PAUSE_MAX_MINUTES, 0)))
		{
			caption = widest;
		}

		if (dc && caption && caption[0])
			GetTextExtentPoint32W(dc, caption, (int)wcslen(caption), &extent);

		// 3 px of slack per side: measured width and the real glyph run do not
		// agree to the pixel, and without it the last character loses a hair.
		float target = (float)(extent.cx + 2 * SETTINGS_STATUS_PAD + SETTINGS_STATUS_SLACK);

		// A block grows out of a square and can never outgrow the strip: the
		// clamp is what stops a long caption in the last block from pushing the
		// row outside its control.
		if (target < height)
			target = height;
		if (target > (float)(client.right - client.left))
			target = (float)(client.right - client.left);

		block->idle = height;
		block->target = target;

		// First measurement: start settled, so opening the window does not play
		// five blocks growing out of nothing.
		if (block->width <= 0.0f)
			block->width = height;
	}

	if (dc && old_font)
		SelectObject(dc, old_font);

	if (dc)
		ReleaseDC(control, dc);
}

// Place the five blocks for the widths of the current frame.
static void layout_status_strip(const SettingsWindow *window, const RECT *area,
								RECT blocks[SETTINGS_STATUS_BLOCKS])
{
	int x = area->left;

	for (int i = 0; i < SETTINGS_STATUS_BLOCKS; i++)
	{
		int width = (int)(window->strip[i].width + 0.5f);

		if (width < 1)
			width = 1;

		blocks[i].left = x;
		blocks[i].top = area->top;
		blocks[i].right = x + width;
		blocks[i].bottom = area->bottom;

		x += width + SETTINGS_STATUS_GAP;
	}
}

// Same blocks, but each one also owns the seam to its right.
//
// This is what makes the strip behave like one object instead of five. With the
// painted rectangles used for hit testing, the pointer crosses a gap where
// nothing is hovered, strip_hover drops to -1, and the highlight jumps to
// nothing for an instant - the "it bounces back and forth" the user reported
// when the pointer used to open blocks. Every pixel on the strip belongs to
// exactly one block, so that state is unreachable while the pointer is inside.
//
// The painted rectangles are deliberately left alone: the gap stays the dialog
// background colour, so the five blocks still read as five.
static void layout_status_strip_hit(const SettingsWindow *window, const RECT *area,
									RECT blocks[SETTINGS_STATUS_BLOCKS])
{
	layout_status_strip(window, area, blocks);

	for (int i = 0; i < SETTINGS_STATUS_BLOCKS - 1; i++)
		blocks[i].right += SETTINGS_STATUS_GAP;

	// The last block has no seam to its right, so it keeps its painted width
}

static void draw_status_strip(const DRAWITEMSTRUCT *dis, SettingsWindow *window)
{
	RECT area = dis->rcItem;
	HDC dc = dis->hDC;

	if (area.right <= area.left || area.bottom <= area.top)
		return;

	// An owner-draw static does not erase itself
	FillRect(dc, &area, GetSysColorBrush(COLOR_BTNFACE));

	if (!window)
		return;

	RECT blocks[SETTINGS_STATUS_BLOCKS];
	layout_status_strip(window, &area, blocks);

	HGDIOBJ old_font = window->base_font ? SelectObject(dc, window->base_font) : NULL;
	HGDIOBJ old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));

	SetBkMode(dc, TRANSPARENT);

	for (int i = 0; i < SETTINGS_STATUS_BLOCKS; i++)
	{
		const RECT *block = &blocks[i];
		uint8_t state = cap_clamp(window->strip_state[i]);
		float level = window->strip[i].level;

		if (block->right <= block->left)
			continue;

		// Full brightness at rest, by request: the row is meant to read as five
		// solid colours, and the open block is identified by its width and its
		// brighter caption instead of by a dimmed background.
		COLORREF lit = STRIP_LIT[i][state];
		COLORREF background = lit;

		// Paused only ever applies to the verdict block, so it is not a column in
		// STRIP_LIT: widening that table for one block would leave four unused
		// slots and invite the wrong one being read.
		if (i == 0 && window->strip_paused)
			background = STRIP_PAUSED_COLOR;

		SetDCBrushColor(dc, background);
		FillRect(dc, block, (HBRUSH)GetStockObject(DC_BRUSH));

		// The single character and the full caption cross fade. One is gone
		// before the other becomes readable, so the two never overlap into a
		// blur in the middle of the animation.
		float reveal = (level - 0.35f) / 0.65f;

		if (reveal < 0.0f)
			reveal = 0.0f;
		if (reveal > 1.0f)
			reveal = 1.0f;

		RECT text = *block;
		text.left += SETTINGS_STATUS_PAD;
		text.right -= SETTINGS_STATUS_PAD;

		if (text.right <= text.left)
			continue;

		// Both captions are anchored to the same left edge on purpose. Drawing
		// the short one centred and the long one left-aligned made the text slide
		// from the middle outwards while the block grew, which read as "it opens
		// from both ends". Anchored left, only the right edge moves: the block
		// slides open to the right, and the text never moves at all.
		if (reveal < 0.98f)
		{
			// 35% ink on a full-brightness block: the character is a hint of what
			// the block is, not a label competing with the open one.
			//
			// The block under the pointer lights its character instead of growing:
			// widening is the one thing the click model removed, and this costs no
			// layout at all, so the pointer cannot move what it is pointing at.
			float ink_base = (i == window->strip_hover) ? 0.85f : 0.35f;
			COLORREF ink = lerp_color(background, RGB(255, 255, 255),
									  ink_base * (1.0f - reveal));

			draw_centered_text(dc, &text, lang_str(block_short_name(i)), ink,
							   window->base_font, DT_LEFT);
		}

		if (reveal > 0.02f)
		{
			COLORREF ink = lerp_color(background, RGB(255, 255, 255),
									  0.35f + 0.65f * reveal);

			draw_centered_text(dc, &text, window->strip_full[i], ink,
							   window->base_font, DT_LEFT);
		}
	}

	if (old_brush)
		SelectObject(dc, old_brush);
	if (old_font)
		SelectObject(dc, old_font);
}

static int status_strip_hit(HWND control, const SettingsWindow *window, POINT point)
{
	RECT client;

	if (!control || !window || !GetClientRect(control, &client))
		return -1;

	// Hit rectangles, not painted ones: the seam to the right of each block
	// belongs to that block, so the pointer is never over "nothing" while it is
	// inside the strip.
	RECT blocks[SETTINGS_STATUS_BLOCKS];
	layout_status_strip_hit(window, &client, blocks);

	for (int i = 0; i < SETTINGS_STATUS_BLOCKS; i++)
	{
		if (point.x >= blocks[i].left && point.x < blocks[i].right &&
			point.y >= blocks[i].top && point.y < blocks[i].bottom)
			return i;
	}

	return -1;
}

// Advance every block to where it should be at now.
//
// Fixed duration, linear: whatever the distance, the block is fully open after
// SETTINGS_STATUS_OPEN_MS and fully closed after SETTINGS_STATUS_CLOSE_MS.
// Nothing eases out, so the last pixels cross at the same speed as the first
// ones - the "it hesitates before it finishes" look came from an exponential
// approach, not from the layout.
//
// A pointer that changes target mid-way restarts from wherever the block
// currently is, with a fresh budget, so the movement stays continuous.
//
// Returns true while anything still moves, so the caller keeps the fast timer.
static bool status_strip_tick(SettingsWindow *window, uint64_t now)
{
	if (!window)
		return false;

	bool moving = false;

	for (int i = 0; i < SETTINGS_STATUS_BLOCKS; i++)
	{
		StatusBlock *block = &window->strip[i];

		// Exactly one block is open, and only a click changes which one. The
		// pointer deliberately has no say: with widths that add up, opening
		// whatever it is over keeps moving the target away from it.
		//
		// Block 0 is the resting state, so the verdict is the one thing that never
		// needs a click to read and the row keeps one silhouette instead of
		// collapsing into five anonymous squares.
		bool expanded = (i == window->strip_pinned);

		float want = expanded ? block->target : block->idle;

		if (float_abs(want - block->to) > 0.01f)
		{
			// Opening gets the full budget; closing gets less, so the block that
			// frees the space is already out of the way while its neighbour is
			// still expanding.
			block->duration_ms = (want > block->width) ? SETTINGS_STATUS_OPEN_MS
													   : SETTINGS_STATUS_CLOSE_MS;

			block->from = block->width;
			block->to = want;
			block->start_ms = now;
		}

		// A zero budget means "already done": closing has to land in the same
		// frame it starts, or the blocks after it would slide left while the
		// opening one grows.
		float progress = (block->duration_ms > 0.0f)
							 ? (float)(now - block->start_ms) / block->duration_ms
							 : 1.0f;

		if (progress >= 1.0f)
		{
			// The frame that ends the animation still has to be painted. Without
			// this the last painted frame is the one before the end - a few
			// percent short - and then the timer stops, so the caption stays
			// clipped until something else invalidates the control. That is
			// exactly the "it only finishes when I move the mouse" the user saw.
			if (block->width != block->to)
			{
				block->width = block->to;
				moving = true;
			}
		}
		else
		{
			// Linear on purpose: an ease-out curve is exactly what made the final
			// pixels feel slower than the rest of the movement.
			block->width = block->from + (block->to - block->from) * progress;
			moving = true;
		}

		// The text brightness is the same progress, not a second animation, so a
		// block is fully readable exactly when it is fully open.
		float span = block->target - block->idle;

		if (span < 1.0f)
			span = 1.0f;

		float level = (block->width - block->idle) / span;

		if (level < 0.0f)
			level = 0.0f;
		if (level > 1.0f)
			level = 1.0f;

		block->level = level;
	}

	return moving;
}

// The strip is one control, so the pointer has to be tracked here: a static does
// not forward WM_MOUSEMOVE to its parent, and the click that opens a block lands
// on the control, not on the dialog.
static LRESULT CALLBACK status_strip_subclass(HWND hwnd, UINT msg, WPARAM wParam,
											  LPARAM lParam, UINT_PTR id, DWORD_PTR ref)
{
	SettingsWindow *window = (SettingsWindow *)ref;

	switch (msg)
	{
	case WM_MOUSEMOVE:
	{
		if (!window)
			break;

		// Without this the control never learns the pointer left, and the block
		// stays lit for as long as the window is open.
		TRACKMOUSEEVENT track = {0};
		track.cbSize = sizeof(track);
		track.dwFlags = TME_LEAVE;
		track.hwndTrack = hwnd;
		TrackMouseEvent(&track);

		POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
		int hit = status_strip_hit(hwnd, window, point);

		if (hit != window->strip_hover)
		{
			window->strip_hover = hit;
			strip_tip_refresh(window);
			InvalidateRect(hwnd, NULL, FALSE);

			// No set_anim_rate here on purpose: hovering only lights a character,
			// it does not move anything, so there is no animation to drive. The
			// fast rate would be sixty wake-ups a second for a repaint that has
			// already happened.
		}

		// Fall through to the rest of the subclass chain on purpose: the tip is
		// added with TTF_SUBCLASS, which is itself a subclass installed on this
		// same control, and it only learns that the pointer is there from this
		// very message. Swallowing it here means the tip never appears at all.
		break;
	}

	case WM_MOUSELEAVE:
		if (window && window->strip_hover >= 0)
		{
			window->strip_hover = -1;
			strip_tip_refresh(window);
			InvalidateRect(hwnd, NULL, FALSE);
		}
		break;

	case WM_SETCURSOR:
		// A hand is the only affordance a 21 px square has room for, and it is
		// what tells the user the block can be clicked at all.
		if (LOWORD(lParam) == HTCLIENT)
		{
			SetCursor(LoadCursorW(NULL, IDC_HAND));
			return TRUE;
		}
		break;

	case WM_LBUTTONUP:
	{
		if (!window)
			break;

		POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
		int hit = status_strip_hit(hwnd, window, point);

		if (hit >= 0)
		{
			// Clicking the open block folds it back to the verdict. Clicking the
			// verdict block while it is already open is a no-op: that is the
			// resting state, and there is no "nothing is open" state to fall into.
			window->strip_pinned = (hit == window->strip_pinned) ? 0 : hit;

			strip_tip_refresh(window);
			InvalidateRect(hwnd, NULL, FALSE);

			// This is the only thing that moves the row now, so it is also the
			// only place that has to make sure frames are being drawn.
			set_anim_rate(GetParent(hwnd), window, true);
		}

		// Same reason as WM_MOUSEMOVE: let the tip see the click so it can hide.
		break;
	}

	case WM_NCDESTROY:
		RemoveWindowSubclass(hwnd, status_strip_subclass, id);
		break;

	default:
		break;
	}

	return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Draw the unmarked hot zone
//
// Nothing at all unless the pointer is on it: the zone exists so that the title
// row does not carry a control nobody needs, and a permanently visible hint would
// be exactly that control again. The hint is drawn instead of shown as a tooltip
// because a tooltip arrives half a second later, which is too late to feel like
// an invitation.
static void draw_egg_zone(const DRAWITEMSTRUCT *item, const SettingsWindow *window)
{
	FillRect(item->hDC, &item->rcItem, GetSysColorBrush(COLOR_BTNFACE));

	if (!window || !window->egg_hover)
		return;

	HFONT font = (HFONT)SendMessageW(item->hwndItem, WM_GETFONT, 0, 0);
	HGDIOBJ old_font = font ? SelectObject(item->hDC, font) : NULL;

	SetBkMode(item->hDC, TRANSPARENT);
	SetTextColor(item->hDC, GetSysColor(COLOR_GRAYTEXT));

	RECT box = item->rcItem;
	DrawTextW(item->hDC, lang_str(STR_EGG_HINT), -1, &box,
			  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

	if (old_font)
		SelectObject(item->hDC, old_font);
}

// The hot zone's menu: one item, checked from the window's own style
//
// The state is read here and written there - no copy of "is on top" exists in
// this program, so the check mark cannot disagree with the window.
static void egg_zone_menu(HWND hDlg)
{
	HMENU menu = CreatePopupMenu();

	if (!menu)
		return;

	bool on_top = (GetWindowLongPtrW(hDlg, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;

	AppendMenuW(menu, MF_STRING | (on_top ? MF_CHECKED : 0), SETTINGS_EGG_ALWAYS_ON_TOP,
				lang_str(STR_ALWAYS_ON_TOP));

	POINT point;
	GetCursorPos(&point);

	int command = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
									  point.x, point.y, 0, hDlg, NULL);

	DestroyMenu(menu);

	// Same courtesy the tray menu pays: a click that dismisses a popup can
	// otherwise be swallowed by whatever window is under the pointer.
	PostMessageW(hDlg, WM_NULL, 0, 0);

	if (command != SETTINGS_EGG_ALWAYS_ON_TOP)
		return;

	SetWindowPos(hDlg, on_top ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0,
				 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Hover tracking and the right click for the hot zone
static LRESULT CALLBACK egg_zone_subclass(HWND hwnd, UINT msg, WPARAM wParam,
										  LPARAM lParam, UINT_PTR id, DWORD_PTR ref)
{
	SettingsWindow *window = (SettingsWindow *)ref;

	switch (msg)
	{
	case WM_MOUSEMOVE:
		// Only the first move is interesting: it arms the leave notification, and
		// the rest are handled by DefSubclassProc so nothing else is swallowed
		// (the same trap the strip fell into with its tooltip).
		if (window && !window->egg_hover)
		{
			TRACKMOUSEEVENT track = {0};
			track.cbSize = sizeof(track);
			track.dwFlags = TME_LEAVE;
			track.hwndTrack = hwnd;

			if (TrackMouseEvent(&track))
			{
				window->egg_hover = true;
				InvalidateRect(hwnd, NULL, TRUE);
			}
		}

		break;

	case WM_MOUSELEAVE:
		if (window && window->egg_hover)
		{
			window->egg_hover = false;
			InvalidateRect(hwnd, NULL, TRUE);
		}

		break;

	case WM_RBUTTONUP:
		egg_zone_menu(GetParent(hwnd));
		return 0;

	case WM_SETCURSOR:
		// The zone is empty, so the cursor is the only thing that says "there is
		// something here" before the pointer stops.
		SetCursor(LoadCursorW(NULL, IDC_HAND));
		return TRUE;

	case WM_NCDESTROY:
		RemoveWindowSubclass(hwnd, egg_zone_subclass, id);
		break;

	default:
		break;
	}

	return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Repaint the strip only when one of the states actually changed.
static void refresh_capabilities(HWND hDlg, SettingsWindow *window,
								 const uint8_t states[CAP_COUNT], bool hook_active)
{
	if (!hDlg || !window || !states)
		return;

	bool changed = false;

	// Block 0 answers "is the mouse protected", which is the hook's own state.
	// The four modules can all be healthy while the hook is gone, so this cannot
	// be derived from them.
	uint8_t verdict = hook_active ? (uint8_t)CAP_STATE_OK : (uint8_t)CAP_STATE_STALLED;

	if (verdict != window->strip_state[0])
	{
		window->strip_state[0] = verdict;

		// While paused the caption belongs to the pause, not to the verdict: the
		// hook can be perfectly healthy with filtering switched off, and the pause
		// handler in refresh_live owns the caption in that case.
		if (!window->strip_paused)
			status_strip_set_caption(window, 0);

		changed = true;
	}

	for (int i = 0; i < CAP_COUNT; i++)
	{
		uint8_t state = cap_clamp(states[i]);

		if (state == window->last_caps[i])
			continue;

		window->last_caps[i] = state;
		window->strip_state[i + 1] = state;
		status_strip_set_caption(window, i + 1);
		changed = true;
	}

	if (!changed)
		return;

	// Captions change width with the state and with the language, so the target
	// widths have to be measured again rather than assumed constant.
	status_strip_measure(hDlg, window);

	HWND control = GetDlgItem(hDlg, IDC_STATUS);
	if (control)
		InvalidateRect(control, NULL, FALSE);
}

// The bottom bar: one column for the foreground verdict, one for messages that
// have nowhere else to go.
//
// 120 DLU is enough for "已拦截：某某程序.exe" and deliberately less than a
// quarter of the window. This bar is meant to carry more later, and a first
// column that eats the row would leave those no room.
static const StatusBarPart SETTINGS_STATUS_PARTS[] = {
	{120, NULL}, // foreground verdict
	{0, NULL},	 // messages: takes whatever is left
};

// Fill the first column from the snapshot.
//
// The wording follows the verdict, not the module state. What the user needs to
// know is whether the program in front is being intercepted - the strip on top
// already answers whether the context module is alive, and repeating that here
// would waste the only column that has new information in it.
static void update_status_bar(HWND hDlg, SettingsWindow *window,
							  const AppSnapshot *snapshot)
{
	if (!hDlg || !window || !snapshot)
		return;

	wchar_t text[SETTINGS_STATUS_SIZE];

	switch (snapshot->context_verdict)
	{
	case APP_CONTEXT_OURS:
		StringCchCopyW(text, ARRAYSIZE(text), lang_str(STR_SB_CONTEXT_OURS));
		break;

	case APP_CONTEXT_EXEMPT:
		StringCchPrintfW(text, ARRAYSIZE(text), lang_str(STR_SB_CONTEXT_EXEMPT_FMT),
						 snapshot->context_name);
		break;

	case APP_CONTEXT_BLOCKED:
		StringCchPrintfW(text, ARRAYSIZE(text), lang_str(STR_SB_CONTEXT_BLOCKED_FMT),
						 snapshot->context_name);
		break;

	default:
		// Nothing published yet, or the lookup failed. Both mean "not
		// identified", and saying so is better than inventing a verdict.
		StringCchCopyW(text, ARRAYSIZE(text), lang_str(STR_SB_CONTEXT_NONE));
		break;
	}

	status_bar_set(hDlg, IDC_STATUSBAR, 0, text);

	// Second column: whatever the application last had to report. An empty
	// string clears it, which is what status_bar_set sees once the notice has
	// expired - so the text disappears without a timer of its own.
	status_bar_set(hDlg, IDC_STATUSBAR, 1, snapshot->notice_text);
}

// Repoint the hover tips at the active language
// The tooltip keeps the pointer it was given, so the text has to be replaced as
// well; leaving it alone would show the old language after a switch.
static void update_control_tooltips(HWND hDlg, SettingsWindow *window)
{
	if (!window || !window->tooltip)
		return;

	for (int i = 0; i < CONTROL_TIP_COUNT; i++)
	{
		TOOLINFOW tool = {0};
		tool.cbSize = sizeof(tool);
		tool.hwnd = hDlg;
		tool.uId = (UINT_PTR)GetDlgItem(hDlg, CONTROL_TIPS[i].control);
		tool.lpszText = (LPWSTR)lang_str(CONTROL_TIPS[i].text);

		if (tool.uId)
			SendMessageW(window->tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&tool);
	}
}

// Apply the filter state to the check boxes and the log contents
// The five radios are only usable when "All" is off, and switching the view
// starts a fresh log so the two never contradict each other.
//
// CheckDlgButton per control, not CheckRadioButton: BS_AUTORADIOBUTTON only makes
// the group exclusive for mouse clicks. Code that fills the state back in has to
// clear every other button itself, or two of them end up looking selected.
static void update_filter_ui(HWND hDlg, SettingsWindow *window, bool reset_view)
{
	bool manual = !window->filter_auto;

	for (int i = 0; i < FILTER_BUTTON_COUNT; i++)
	{
		HWND radio = GetDlgItem(hDlg, FILTER_BUTTONS[i].control);
		bool chosen = manual && window->filter_button == FILTER_BUTTONS[i].button;

		EnableWindow(radio, manual);
		CheckDlgButton(hDlg, FILTER_BUTTONS[i].control, chosen ? BST_CHECKED : BST_UNCHECKED);
	}

	CheckDlgButton(hDlg, IDC_FILTER_AUTO, window->filter_auto ? BST_CHECKED : BST_UNCHECKED);

	if (reset_view)
		clear_logs(hDlg, window);
}

// Apply the localized texts
static void apply_language(HWND hDlg)
{
	// The caption carries the version, so it is formatted rather than looked up:
	// one bump in version.h renames the title bar, the taskbar button and the
	// Alt+Tab entry at once.
	wchar_t title[LANG_TITLE_MAX_CHARS];
	lang_format_settings_title(title, ARRAYSIZE(title));
	SetWindowTextW(hDlg, title);

	SetDlgItemTextW(hDlg, IDC_GROUP_TEST, lang_str(STR_GROUP_TEST));
	SetDlgItemTextW(hDlg, IDC_GROUP_LOG, lang_str(STR_GROUP_LOG));
	SetDlgItemTextW(hDlg, IDC_GROUP_LIVE, lang_str(STR_GROUP_LIVE));
	SetDlgItemTextW(hDlg, IDC_GROUP_STATS, lang_str(STR_GROUP_STATS));

	SetDlgItemTextW(hDlg, IDC_FILTER_AUTO, lang_str(STR_FILTER_ALL));

	// Short labels, one row of six. The rc widths were measured against exactly
	// these words - see the comment on the filter controls in MouseMend.rc.
	for (int i = 0; i < FILTER_BUTTON_COUNT; i++)
		SetDlgItemTextW(hDlg, FILTER_BUTTONS[i].control, lang_filter_label(FILTER_BUTTONS[i].button));

	// The one line under the radios that explains why they look dead
	SetDlgItemTextW(hDlg, IDC_FILTER_HINT, lang_str(STR_FILTER_HINT));

	SetDlgItemTextW(hDlg, IDC_VERSION_NOTES, lang_str(STR_VERSION_NOTES));
	SetDlgItemTextW(hDlg, IDC_LOG_HELP, lang_str(STR_VERDICT_HELP_NAME));
	SetDlgItemTextW(hDlg, IDC_SETTINGS, lang_str(STR_SETTINGS_BUTTON));
	SetDlgItemTextW(hDlg, IDC_CLEAR, lang_str(STR_CLEAR));
	SetDlgItemTextW(hDlg, IDC_OPEN_LOG, lang_str(STR_LOG_DIR_BUTTON));
	SetDlgItemTextW(hDlg, IDC_CLOSE, lang_str(STR_CLOSE));

	// The column headers are their own controls so they stay put while the log
	// scrolls; they carry localized column names, so they follow the language.
	wchar_t header[SETTINGS_LINE_SIZE];
	lang_format_log_header(header, ARRAYSIZE(header));

	SetDlgItemTextW(hDlg, IDC_TESTLOG_HDR, header);
	SetDlgItemTextW(hDlg, IDC_BLOCKLOG_HDR, header);

	// All three panes carry the same five columns, so they share one header: the
	// live pane pairs each press with its release, which is what gives it the hold
	// time the test log has always shown.
	SetDlgItemTextW(hDlg, IDC_LIVE_HDR, header);

	// The idle hint lives inside the test area, so it is painted, not set
	InvalidateRect(GetDlgItem(hDlg, IDC_RIPPLE), NULL, TRUE);
}

// Open the history folder in Explorer
static void open_log_folder(HWND hDlg, SettingsWindow *window)
{
	if (!window->snapshot)
		return;

	AppSnapshot snapshot = {0};
	window->snapshot(&snapshot, window->snapshot_user);

	if (snapshot.log_directory[0] == L'\0')
	{
		MessageBoxW(hDlg, lang_str(STR_LOG_DIR_UNAVAILABLE), lang_str(STR_SETTINGS_TITLE),
					MB_OK | MB_ICONWARNING);
		return;
	}

	ShellExecuteW(hDlg, L"open", snapshot.log_directory, NULL, NULL, SW_SHOWNORMAL);
}

// Run the animation timer at the rate the window currently needs
//
// Only two rates exist: full while something moves, and one that merely keeps
// the trace draining while nothing does. Switching is done here so the timer is
// never reset twice for the same state.
static void set_anim_rate(HWND hDlg, SettingsWindow *window, bool fast)
{
	if (!window || window->anim_fast == fast)
		return;

	window->anim_fast = fast;
	SetTimer(hDlg, SETTINGS_TIMER_ANIM, fast ? SETTINGS_ANIM_MS : SETTINGS_ANIM_IDLE_MS, NULL);
}

// Stop the live work of a window that is about to leave the screen
//
// A hidden window keeps its timers: the animation tick would keep draining the
// trace and repainting text nobody can see, four hundred times a minute, for as
// long as the program runs. The capture rectangle goes with it, because a hidden
// window must not record clicks at the place it used to occupy.
static void suspend_live(HWND hDlg, SettingsWindow *window)
{
	KillTimer(hDlg, SETTINGS_TIMER_REFRESH);
	KillTimer(hDlg, SETTINGS_TIMER_ANIM);

	if (window)
		window->anim_fast = false;

	if (window && window->trace)
		event_trace_set_capture(window->trace, NULL);
}

// Resume the live work after the window came back
static void resume_live(HWND hDlg, SettingsWindow *window)
{
	if (!window)
		return;

	// Whatever the trace collected while the rectangle was empty is not part of
	// this session; dropping it keeps a reopened window from showing a burst of
	// rows the user never saw happen.
	if (window->trace)
	{
		event_trace_set_capture(window->trace, NULL);
		event_trace_clear(window->trace);
	}

	update_capture_rect(hDlg, window);

	SetTimer(hDlg, SETTINGS_TIMER_REFRESH, SETTINGS_REFRESH_MS, NULL);

	// Coming back from the tray is a fresh start, so it begins at full rate and
	// falls back to idle by itself
	window->last_anim_ms = GetTickCount64();
	set_anim_rate(hDlg, window, true);
}

// Put the window away into the tray
//
// The window is the main window, so closing it never ends the program: the
// protection has to keep running after the user dismisses its face. Everything
// on screen is kept, so reopening it shows the same session.
static void hide_to_tray(HWND hDlg, SettingsWindow *window)
{
	// The drawer goes back to the verdict while nobody can see it. Doing this on
	// the way out instead of on a timer means the fold never happens in front of
	// the user: the window is already gone when the widths change, and every open
	// starts from the same picture.
	if (window)
		window->strip_pinned = 0;

	suspend_live(hDlg, window);
	ShowWindow(hDlg, SW_HIDE);

	// window may be NULL (a close that arrives before the window registered
	// itself), and the analyser is right that the unchecked dereference below
	// was one path away from a crash.
	if (window && window->on_hidden)
		window->on_hidden(window->hidden_user);
}

// Bring the window back from the tray
static void show_from_tray(HWND hDlg, SettingsWindow *window)
{
	ShowWindow(hDlg, SW_SHOW);
	resume_live(hDlg, window);
}

// Dialog procedure
static INT_PTR CALLBACK settings_dialog_proc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	SettingsWindow *window = get_window(hDlg);

	switch (message)
	{
	case WM_INITDIALOG:
	{
		window = (SettingsWindow *)lParam;
		SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)window);
		window->hwnd = hDlg;

		// Every open starts from a clean slate: the trace may still hold clicks
		// from before the window existed, and they would be dated before the log
		window->filter_auto = true;
		window->filter_button = MOUSE_BUTTON_LEFT;
		memset(&window->test_log, 0, sizeof(window->test_log));
		memset(&window->block_log, 0, sizeof(window->block_log));
		memset(window->pending, 0, sizeof(window->pending));
		window->last_stats[0] = L'\0';
		window->last_caption[0] = L'\0';
		memset(window->click_counts, 0, sizeof(window->click_counts));
		window->gap_min_ms = 0;
		window->gap_max_ms = 0;
		window->gap_sum_ms = 0;
		window->gap_samples = 0;
		window->near_miss_count = 0;
		window->near_miss_min_ms = 0;

		// A revision that cannot match, so the first tick paints the live pane
		// even when the history is still empty
		window->live_revision = 0xFFFFFFFFu;
		window->pending_panes = 0;
		window->panes_painted_ms = 0;
		window->anim_fast = false;
		window->last_anim_ms = GetTickCount64();
		ripple_view_reset(&window->ripple);

		// The strip starts on the verdict block: block 0 is the resting state, so
		// the row opens with the one caption it is supposed to always be able to
		// answer. Every width is still zero here; the first measurement turns them
		// into the real square size before anything is painted.
		memset(window->strip, 0, sizeof(window->strip));
		memset(window->strip_state, 0, sizeof(window->strip_state));
		window->strip_hover = -1;
		window->strip_pinned = 0;
		window->strip_tip[0] = L'\0';

		if (window->trace)
			event_trace_clear(window->trace);

		// The status line answers "is it working", so it gets a bold font
		NONCLIENTMETRICSW metrics = {0};
		metrics.cbSize = sizeof(metrics);

		if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
		{
			metrics.lfMessageFont.lfWeight = FW_BOLD;
			window->status_font = CreateFontIndirectW(&metrics.lfMessageFont);

			if (window->status_font)
				SendDlgItemMessageW(hDlg, IDC_STATUS, WM_SETFONT, (WPARAM)window->status_font, TRUE);
		}

		// The test area draws its idle hint with the control font. An owner draw
		// static does not inherit the dialog font by itself, and the stock one it
		// gets instead is a different typeface from every label around it.
		SendDlgItemMessageW(hDlg, IDC_RIPPLE, WM_SETFONT,
							(WPARAM)SendMessageW(hDlg, WM_GETFONT, 0, 0), TRUE);

		// The three panes use a fixed pitch font: every Latin character is exactly
		// one cell wide and a CJK glyph exactly two, so padding the cells is all it
		// takes to line the columns up in both languages.
		HDC screen = GetDC(hDlg);
		int font_height = -MulDiv(SETTINGS_LOG_FONT_PIXELS, GetDeviceCaps(screen, LOGPIXELSY), 96);
		ReleaseDC(hDlg, screen);

		window->log_font = CreateFontW(font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
									   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
									   CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

		if (window->log_font)
		{
			// Every pane and every column header shares the font: their padding is
			// computed in cells, and a proportional font would break the alignment
			// between a header and the rows underneath it.
			const int pane_controls[] = {IDC_TESTLOG, IDC_BLOCKLOG, IDC_LIVELOG,
										 IDC_TESTLOG_HDR, IDC_BLOCKLOG_HDR, IDC_LIVE_HDR};

			for (int i = 0; i < (int)(sizeof(pane_controls) / sizeof(pane_controls[0])); i++)
			{
				SendDlgItemMessageW(hDlg, pane_controls[i], WM_SETFONT,
									(WPARAM)window->log_font, TRUE);
			}
		}

		apply_language(hDlg);
		window->applied_language = language_get();
		create_control_tooltips(hDlg, window);

		// Hand-drawn text has to select the dialog font itself; without this it
		// would come out in the stock system font and look foreign.
		window->base_font = (HFONT)SendMessageW(hDlg, WM_GETFONT, 0, 0);

		// The status bar inherits that same font, so the two never disagree about
		// text size. A failure here means the control class is not registered;
		// the window still works, it just has one less place to report.
		status_bar_attach(hDlg, IDC_STATUSBAR, SETTINGS_STATUS_PARTS,
						  ARRAYSIZE(SETTINGS_STATUS_PARTS));

		// 0xFF is not a valid state, so the first refresh fills every caption
		// instead of treating "disabled" as "already drawn and unchanged".
		memset(window->last_caps, 0xFF, sizeof(window->last_caps));

		// Hover is tracked on the control itself: a static does not forward
		// WM_MOUSEMOVE to the dialog, and without it no block would ever light.
		HWND strip = GetDlgItem(hDlg, IDC_STATUS);
		if (strip)
			SetWindowSubclass(strip, status_strip_subclass, 1, (DWORD_PTR)window);

		// The hot zone has to see its own mouse messages: a static does not pass
		// them to the dialog, and this one has no text of its own to react to.
		HWND egg = GetDlgItem(hDlg, IDC_EGG_ZONE);
		if (egg)
			SetWindowSubclass(egg, egg_zone_subclass, 3, (DWORD_PTR)window);

		update_filter_ui(hDlg, window, false);
		clear_logs(hDlg, window);

		refresh_live(hDlg, window);

		// The window is a real taskbar window: it carries the application icon and
		// can be found again after the user clicked somewhere else
		HICON icon = LoadIconW(window->hInstance, MAKEINTRESOURCEW(IDI_MAINICON));
		if (icon)
		{
			SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)icon);
			SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)icon);
		}

		window_center_on_monitor(hDlg);

		// Modeless: the owner keeps running (and keeps debouncing) while visible
		ShowWindow(hDlg, SW_SHOWNORMAL);

		// The test area is the only place that may feed the log, and its position
		// is only final once the window is on screen
		update_capture_rect(hDlg, window);

		SetTimer(hDlg, SETTINGS_TIMER_REFRESH, SETTINGS_REFRESH_MS, NULL);
		set_anim_rate(hDlg, window, true);
		return TRUE;
	}

	case WM_TIMER:
		if (!window)
			return TRUE;

		if (wParam == SETTINGS_TIMER_REFRESH)
		{
			// The capture rectangle is screen coordinates, and a monitor being
			// plugged in, removed or rescaled can move this window without a
			// WM_MOVE ever arriving. Recomputing it here costs one GetWindowRect
			// per tick and keeps the log from recording the wrong part of the
			// desktop after a display change.
			update_capture_rect(hDlg, window);

			refresh_live(hDlg, window);

			// The live pane is on the slow timer on purpose: it is a log for a
			// human to read, not an animation, and the revision check inside means
			// an idle window does no work at all here.
			render_live_log(hDlg, window);
			return TRUE;
		}

		if (wParam == SETTINGS_TIMER_ANIM)
		{
			unsigned changed = drain_trace(hDlg, window);
			uint64_t now = GetTickCount64();
			bool repaint = ripple_view_tick(&window->ripple, now);

			// The strip rides the same timer: same 60 Hz while it moves, same
			// fall back to the idle rate once it settles.
			bool strip_moving = status_strip_tick(window, now);

			if (strip_moving)
				InvalidateRect(GetDlgItem(hDlg, IDC_STATUS), NULL, FALSE);

			// Coalesced: every click in the last 100 ms lands in the same rewrite
			window->pending_panes |= changed;

			if (window->pending_panes && now - window->panes_painted_ms >= SETTINGS_PANE_MS)
			{
				// Only the pane that actually gained a row is rewritten
				if (window->pending_panes & SETTINGS_LOG_CHANGED)
					show_pane_text(hDlg, window, IDC_TESTLOG, &window->test_log);

				if (window->pending_panes & SETTINGS_BLOCK_CHANGED)
					show_pane_text(hDlg, window, IDC_BLOCKLOG, &window->block_log);

				window->pending_panes = 0;
				window->panes_painted_ms = now;
			}

			if (changed || repaint)
				InvalidateRect(GetDlgItem(hDlg, IDC_RIPPLE), NULL, FALSE);

			// Full rate only while something is moving, plus a moment after the
			// last frame so a click that follows a pause is still drawn at once.
			bool busy = repaint || changed != 0 || strip_moving;

			if (busy)
				window->last_anim_ms = now;

			bool fast = busy || (now - window->last_anim_ms) < SETTINGS_ANIM_GRACE_MS;

			set_anim_rate(hDlg, window, fast);

			return TRUE;
		}

		return TRUE;

	case WM_DRAWITEM:
	{
		const DRAWITEMSTRUCT *item = (const DRAWITEMSTRUCT *)lParam;

		if (item && item->CtlID == IDC_RIPPLE && window)
		{
			ripple_view_draw(&window->ripple, item, GetTickCount64(), lang_str(STR_TEST_HINT));
			return TRUE;
		}

		// The whole status strip is one control: five blocks, one layout
		if (item && item->CtlID == IDC_STATUS)
		{
			draw_status_strip(item, window);
			return TRUE;
		}

		if (item && item->CtlID == IDC_EGG_ZONE)
		{
			draw_egg_zone(item, window);
			return TRUE;
		}

		break;
	}

	case WM_COMMAND:
	{
		int control = (int)LOWORD(wParam);

		// A command from another control means the user moved on to something
		// else, and that is the standard way out of a drawer. This is deliberately
		// not WM_PARENTNOTIFY: that message does not reach the dialog procedure
		// (measured), and every control worth resetting on - buttons, radios and
		// the text panes - reports a command anyway. The strip's own notification
		// must not fold it back, and the panes only report focus.
		if (window && control != IDC_STATUS && !keeps_strip_pinned(control))
			status_strip_reset(hDlg, window);

		switch (control)
		{
		case IDOK:
		case IDCANCEL:
		case IDC_CLOSE:
			// The close button puts the window away, it does not end the program
			// and it does not throw the session away
			if (window)
				hide_to_tray(hDlg, window);
			return TRUE;

		case IDC_SETTINGS:
			// TODO(2.6.x): this button opens the settings window. That window does
			// not exist yet, so the button explains itself in the status bar - and
			// THIS BRANCH MUST GO when the window is built, together with
			// STR_SETTINGS_PENDING. Leaving it would answer a working button with
			// "coming soon".
			if (window && window->notice)
				window->notice(window->notice_user, lang_str(STR_SETTINGS_PENDING), 10000);
			return TRUE;

		case IDC_VERSION_NOTES:
			help_window_show(hDlg, (HINSTANCE)GetWindowLongPtrW(hDlg, GWLP_HINSTANCE),
							 HELP_TOPIC_VERSIONS);
			return TRUE;

		case IDC_LOG_HELP:
			help_window_show(hDlg, (HINSTANCE)GetWindowLongPtrW(hDlg, GWLP_HINSTANCE),
							 HELP_TOPIC_VERDICTS);
			return TRUE;

		case IDC_CLEAR:
			if (window)
				clear_logs(hDlg, window);
			return TRUE;

		case IDC_OPEN_LOG:
			if (window)
				open_log_folder(hDlg, window);
			return TRUE;

		case IDC_FILTER_AUTO:
			if (window && HIWORD(wParam) == BN_CLICKED)
			{
				window->filter_auto = (IsDlgButtonChecked(hDlg, IDC_FILTER_AUTO) == BST_CHECKED);
				update_filter_ui(hDlg, window, true);
			}
			return TRUE;

		case IDC_FILTER_LEFT:
		case IDC_FILTER_MIDDLE:
		case IDC_FILTER_RIGHT:
		case IDC_FILTER_X1:
		case IDC_FILTER_X2:
			if (window && HIWORD(wParam) == BN_CLICKED)
			{
				// A radio can only be clicked while "All" is off, but the state is
				// set explicitly so the two controls can never disagree
				window->filter_auto = false;

				for (int i = 0; i < FILTER_BUTTON_COUNT; i++)
				{
					if (FILTER_BUTTONS[i].control != control)
						continue;

					window->filter_button = FILTER_BUTTONS[i].button;
					break;
				}

				update_filter_ui(hDlg, window, true);
			}
			return TRUE;

		default:
			break;
		}

		break;
	}

	case WM_LBUTTONDOWN:
		// A click on the dialog itself, away from every control
		if (window)
			status_strip_reset(hDlg, window);
		break;

	case WM_CLOSE:
		// The X in the title bar means "put it away", like every tray application
		if (window)
			hide_to_tray(hDlg, window);
		else
			DestroyWindow(hDlg);
		return TRUE;

	case WM_MOVE:
	case WM_SIZE:
		// The capture rectangle is in screen coordinates, so it has to follow the
		// window. Without this a moved window would record clicks at the place it
		// used to be.
		if (window)
			update_capture_rect(hDlg, window);
		return TRUE;

	case WM_DESTROY:
		KillTimer(hDlg, SETTINGS_TIMER_REFRESH);
		KillTimer(hDlg, SETTINGS_TIMER_ANIM);

		if (window)
		{
			// Stop recording: a closed window must not leave the trace capturing a
			// rectangle that no longer belongs to anything
			if (window->trace)
				event_trace_set_capture(window->trace, NULL);

			window->hwnd = NULL;
			ripple_view_reset(&window->ripple);
			ripple_view_release();

			// The tooltip is owned by the dialog, so it is already gone; dropping
			// the stale handle keeps a later reopen from sending messages to it
			window->tooltip = NULL;

			if (window->status_font)
			{
				DeleteObject(window->status_font);
				window->status_font = NULL;
			}

			if (window->log_font)
			{
				DeleteObject(window->log_font);
				window->log_font = NULL;
			}
		}

		SetWindowLongPtrW(hDlg, DWLP_USER, 0);
		return TRUE;

	default:
		break;
	}

	return FALSE;
}

// Create the window, or bring the existing one to the front
bool settings_window_open(SettingsWindow *window, HINSTANCE hInstance, HWND owner,
						  SettingsSnapshotFn snapshot, void *user, EventTrace *trace,
						  EventHistory *history)
{
	if (!window || !snapshot)
		return false;

	// Single instance: a second open must not create a second window
	if (window->hwnd && IsWindow(window->hwnd))
	{
		if (IsWindowVisible(window->hwnd))
			ShowWindow(window->hwnd, SW_RESTORE);
		else
			show_from_tray(window->hwnd, window);

		SetForegroundWindow(window->hwnd);
		return true;
	}

	window->hInstance = hInstance;
	window->snapshot = snapshot;
	window->snapshot_user = user;
	window->trace = trace;
	window->history = history;

	HWND hDlg = CreateDialogParamW(hInstance, MAKEINTRESOURCEW(IDD_SETTINGS), owner,
								   settings_dialog_proc, (LPARAM)window);

	if (!hDlg)
		return false;

	// WM_INITDIALOG already showed it; make sure it is in front
	SetForegroundWindow(hDlg);
	return true;
}

// Check whether the window currently exists, visible or hidden in the tray
bool settings_window_is_open(const SettingsWindow *window)
{
	return window && window->hwnd && IsWindow(window->hwnd);
}

// Check whether the window is on screen right now
bool settings_window_is_visible(const SettingsWindow *window)
{
	return settings_window_is_open(window) && IsWindowVisible(window->hwnd);
}

// Destroy the window, used on shutdown
void settings_window_close(SettingsWindow *window)
{
	if (settings_window_is_open(window))
		DestroyWindow(window->hwnd);
}

// Receive a notification when the user puts the window into the tray
void settings_window_set_hidden_handler(SettingsWindow *window,
										SettingsHiddenFn handler, void *user)
{
	if (!window)
		return;

	window->on_hidden = handler;
	window->hidden_user = user;
}

void settings_window_set_notice_handler(SettingsWindow *window,
										SettingsNoticeFn handler, void *user)
{
	if (!window)
		return;

	window->notice = handler;
	window->notice_user = user;
}
