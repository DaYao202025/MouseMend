#include "settings_window.h"
#include "language.h"
#include "window_layout.h"
#include "../../resource.h"
#include "../core/config.h"
#include "../utils/diagnostics.h"
#include <strsafe.h>
#include <shellapi.h>
#include <commctrl.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "comctl32.lib")

#define SETTINGS_TIMER_REFRESH 1
#define SETTINGS_REFRESH_MS 400

// The log panes are drained on a fast timer so a press shows up immediately;
// the same timer drives the expanding rings.
#define SETTINGS_TIMER_ANIM 2
#define SETTINGS_ANIM_MS 33

#define SETTINGS_DIAG_SIZE 4096
#define SETTINGS_EVIDENCE_SIZE 1536
#define SETTINGS_STATUS_SIZE 384

// Minimum time between two event driven self check rebuilds. A badly bouncing
// button can block dozens of clicks per second and each rebuild repaints the
// whole pane, which is pure waste: nobody reads it that fast.
#define SETTINGS_DIAG_MIN_INTERVAL_MS 500

// Entries moved from the ring into the panes per animation tick. Each one costs
// a row format and, when the pane is rewritten, a copy of a 16 KB buffer, so the
// work per tick has to stay bounded no matter how long the log was closed.
#define SETTINGS_TRACE_DRAIN_PER_TICK 8

// Cell height of the fixed pitch font used by the text panes, in pixels at 96 dpi
// The log column widths in language.c are measured against this size: one cell
// is about 7.3 px here, which is what makes a whole row fit the pane.
#define SETTINGS_LOG_FONT_PIXELS 11

static void refresh_diagnostics(HWND hDlg, SettingsWindow *window, bool rebuild_report);
static void apply_language(HWND hDlg);
static void update_filter_tooltips(HWND hDlg, SettingsWindow *window);

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
static bool entry_selected(const SettingsWindow *window, const TraceEntry *entry)
{
	// The radios only offer the three main buttons, so a wheel entry belongs to
	// the Auto view only.
	return window->filter_auto || (entry->button == window->filter_button);
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

	ripple_view_trigger(&window->ripple, entry->button,
						point.x - bounds.left, point.y - bounds.top, entry->timestamp);
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
static unsigned commit_click(SettingsWindow *window, ClickRow *row, bool *blocked_click)
{
	unsigned changed = 0;

	log_pane_push(&window->test_log, row);
	changed |= SETTINGS_LOG_CHANGED;

	if (row->blocked)
	{
		log_pane_push(&window->block_log, row);
		changed |= SETTINGS_BLOCK_CHANGED;

		if (blocked_click)
			*blocked_click = true;
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
	bool blocked_click = false;

	// Bounded drain: a backlog (or a button that bounces faster than the timer)
	// would otherwise rebuild the pane once per entry in a single tick and
	// freeze the window. Whatever is left waits for the next tick, 33 ms away.
	int budget = SETTINGS_TRACE_DRAIN_PER_TICK;

	while (budget-- > 0 && event_trace_pop(window->trace, &entry))
	{
		// The ring is physical feedback, not log content: it appears for every
		// press inside the test area even when the filter hides that button.
		if (entry.is_down && !config_is_wheel_button(entry.button))
			trigger_ripple(hDlg, window, &entry);

		if (!entry_selected(window, &entry))
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
			changed |= commit_click(window, &row, &blocked_click);
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
				changed |= commit_click(window, pending, &blocked_click);
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
			changed |= commit_click(window, pending, &blocked_click);
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
		changed |= commit_click(window, row, &blocked_click);
	}

	// A false click updates the evidence block, otherwise the self check would
	// keep showing the previous one until somebody pressed refresh
	if (blocked_click)
		refresh_diagnostics(hDlg, window, false);

	return changed;
}

// Replace the text of a log pane and keep the newest line visible
// The newest line is the first one, so "visible" means scrolled to the top.
static void show_log(HWND hDlg, int control_id, const wchar_t *text)
{
	HWND edit = GetDlgItem(hDlg, control_id);
	if (!edit)
		return;

	SetWindowTextW(edit, text);

	SendMessageW(edit, EM_SETSEL, 0, 0);
	SendMessageW(edit, EM_SCROLLCARET, 0, 0);
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
		update_filter_tooltips(hDlg, window);

		// The chart hint and the pane captions are drawn or formatted elsewhere
		window->last_stats[0] = L'\0';
		window->last_caption[0] = L'\0';
		InvalidateRect(GetDlgItem(hDlg, IDC_RIPPLE), NULL, TRUE);
	}

	AppSnapshot snapshot = {0};
	window->snapshot(&snapshot, window->snapshot_user);

	wchar_t status[SETTINGS_STATUS_SIZE];
	log_format_status(&snapshot, status, ARRAYSIZE(status));
	SetDlgItemTextW(hDlg, IDC_STATUS, status);

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

// Refresh the self check pane: the support report plus the evidence above it
// The buffers are static because 20 KB of dialog stack is needlessly large and
// there is only ever one check-up window, on the UI thread.
//
// rebuild_report is false when only the evidence changed. Building the report is
// not cheap - it reads the registry and probes whether a hook can be installed -
// and the hook shares this thread, so a blocked click must not pay for it.
static void refresh_diagnostics(HWND hDlg, SettingsWindow *window, bool rebuild_report)
{
	static wchar_t report[SETTINGS_DIAG_SIZE];
	static wchar_t evidence[SETTINGS_EVIDENCE_SIZE];
	static wchar_t combined[SETTINGS_DIAG_SIZE + SETTINGS_EVIDENCE_SIZE + 8];
	static wchar_t formatted[(SETTINGS_DIAG_SIZE + SETTINGS_EVIDENCE_SIZE + 8) * 2];

	// A button that bounces badly produces dozens of blocked clicks per second,
	// and every one of them used to rebuild and repaint five kilobytes of text.
	// That is a lot of work for a pane the user cannot read that fast anyway, so
	// refreshes triggered by an event are throttled. An explicit refresh (the
	// button, or opening the window) always wins.
	uint64_t now = GetTickCount64();

	if (!rebuild_report && window && window->last_diag_tick != 0 &&
		now >= window->last_diag_tick &&
		(now - window->last_diag_tick) < SETTINGS_DIAG_MIN_INTERVAL_MS)
		return;

	if (window)
		window->last_diag_tick = now;

	if (rebuild_report || report[0] == L'\0')
	{
		if (!diagnostics_build(report, ARRAYSIZE(report)))
			StringCchCopyW(report, ARRAYSIZE(report), L"result=ATTENTION\n");
	}

	AppSnapshot snapshot = {0};
	if (window && window->snapshot)
		window->snapshot(&snapshot, window->snapshot_user);

	log_format_evidence(&snapshot, evidence, ARRAYSIZE(evidence));

	StringCchCopyW(combined, ARRAYSIZE(combined), report);
	StringCchCatW(combined, ARRAYSIZE(combined), L"\n");
	StringCchCatW(combined, ARRAYSIZE(combined), evidence);

	log_format_lf_to_crlf(combined, formatted, ARRAYSIZE(formatted));

	SetDlgItemTextW(hDlg, IDC_DIAG, formatted);
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

	refresh_live(hDlg, window);
}

// Controls that carry a hover tip, and the string each one shows
static const struct
{
	int control;
	LangStringId text;
} FILTER_TIPS[] = {
	{IDC_FILTER_AUTO, STR_TIP_FILTER_AUTO},
	{IDC_FILTER_LEFT, STR_TIP_FILTER_LEFT},
	{IDC_FILTER_MIDDLE, STR_TIP_FILTER_MIDDLE},
	{IDC_FILTER_RIGHT, STR_TIP_FILTER_RIGHT},
};

#define FILTER_TIP_COUNT ((int)(sizeof(FILTER_TIPS) / sizeof(FILTER_TIPS[0])))

// Create the hover tips of the filter controls
//
// A tooltip is the cheapest way to explain three controls that are disabled
// most of the time: a new user hovers "Middle" and immediately learns why it
// cannot be clicked yet, instead of clicking around until something happens.
static void create_filter_tooltips(HWND hDlg, SettingsWindow *window)
{
	window->tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
									  WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
									  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
									  hDlg, NULL, window->hInstance, NULL);

	if (!window->tooltip)
		return;

	// Without a max width a long tip becomes a single line across the screen
	SendMessageW(window->tooltip, TTM_SETMAXTIPWIDTH, 0, 320);

	for (int i = 0; i < FILTER_TIP_COUNT; i++)
	{
		TOOLINFOW tool = {0};
		tool.cbSize = sizeof(tool);
		tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
		tool.hwnd = hDlg;
		tool.uId = (UINT_PTR)GetDlgItem(hDlg, FILTER_TIPS[i].control);
		tool.lpszText = (LPWSTR)lang_str(FILTER_TIPS[i].text);

		if (tool.uId)
			SendMessageW(window->tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool);
	}
}

// Repoint the hover tips at the active language
// The tooltip keeps the pointer it was given, so the text has to be replaced as
// well; leaving it alone would show the old language after a switch.
static void update_filter_tooltips(HWND hDlg, SettingsWindow *window)
{
	if (!window || !window->tooltip)
		return;

	for (int i = 0; i < FILTER_TIP_COUNT; i++)
	{
		TOOLINFOW tool = {0};
		tool.cbSize = sizeof(tool);
		tool.hwnd = hDlg;
		tool.uId = (UINT_PTR)GetDlgItem(hDlg, FILTER_TIPS[i].control);
		tool.lpszText = (LPWSTR)lang_str(FILTER_TIPS[i].text);

		if (tool.uId)
			SendMessageW(window->tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&tool);
	}
}

// Apply the filter state to the check boxes and the log contents
// The three radios are only usable when Auto is off, and switching the view
// starts a fresh log so the two never contradict each other.
static void update_filter_ui(HWND hDlg, SettingsWindow *window, bool reset_view)
{
	bool manual = !window->filter_auto;

	EnableWindow(GetDlgItem(hDlg, IDC_FILTER_LEFT), manual);
	EnableWindow(GetDlgItem(hDlg, IDC_FILTER_MIDDLE), manual);
	EnableWindow(GetDlgItem(hDlg, IDC_FILTER_RIGHT), manual);

	CheckDlgButton(hDlg, IDC_FILTER_AUTO, window->filter_auto ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hDlg, IDC_FILTER_LEFT,
				   (manual && window->filter_button == MOUSE_BUTTON_LEFT) ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hDlg, IDC_FILTER_MIDDLE,
				   (manual && window->filter_button == MOUSE_BUTTON_MIDDLE) ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hDlg, IDC_FILTER_RIGHT,
				   (manual && window->filter_button == MOUSE_BUTTON_RIGHT) ? BST_CHECKED : BST_UNCHECKED);

	if (reset_view)
		clear_logs(hDlg, window);
}

// Apply the localized texts
static void apply_language(HWND hDlg)
{
	SetWindowTextW(hDlg, lang_str(STR_SETTINGS_TITLE));

	SetDlgItemTextW(hDlg, IDC_GROUP_TEST, lang_str(STR_GROUP_TEST));
	SetDlgItemTextW(hDlg, IDC_GROUP_LOG, lang_str(STR_GROUP_LOG));
	SetDlgItemTextW(hDlg, IDC_GROUP_STATS, lang_str(STR_GROUP_STATS));
	SetDlgItemTextW(hDlg, IDC_GROUP_DIAG, lang_str(STR_GROUP_DIAG));

	SetDlgItemTextW(hDlg, IDC_FILTER_AUTO, lang_str(STR_AUTO));
	SetDlgItemTextW(hDlg, IDC_FILTER_LEFT, lang_button_name(MOUSE_BUTTON_LEFT));
	SetDlgItemTextW(hDlg, IDC_FILTER_MIDDLE, lang_button_name(MOUSE_BUTTON_MIDDLE));
	SetDlgItemTextW(hDlg, IDC_FILTER_RIGHT, lang_button_name(MOUSE_BUTTON_RIGHT));

	// The one line under the radios that explains why they look dead
	SetDlgItemTextW(hDlg, IDC_FILTER_HINT, lang_str(STR_FILTER_HINT));

	SetDlgItemTextW(hDlg, IDC_DIAGHINT, lang_str(STR_DIAG_HINT));
	SetDlgItemTextW(hDlg, IDC_REFRESH, lang_str(STR_REFRESH));
	SetDlgItemTextW(hDlg, IDC_CLEAR, lang_str(STR_CLEAR));
	SetDlgItemTextW(hDlg, IDC_OPEN_LOG, lang_str(STR_LOG_DIR_BUTTON));
	SetDlgItemTextW(hDlg, IDC_CLOSE, lang_str(STR_CLOSE));

	// The column headers are their own controls so they stay put while the log
	// scrolls; they carry localized column names, so they follow the language.
	wchar_t header[SETTINGS_LINE_SIZE];
	lang_format_log_header(header, ARRAYSIZE(header));

	SetDlgItemTextW(hDlg, IDC_TESTLOG_HDR, header);
	SetDlgItemTextW(hDlg, IDC_BLOCKLOG_HDR, header);

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
	SetTimer(hDlg, SETTINGS_TIMER_ANIM, SETTINGS_ANIM_MS, NULL);
}

// Put the window away into the tray
//
// The window is the main window, so closing it never ends the program: the
// protection has to keep running after the user dismisses its face. Everything
// on screen is kept, so reopening it shows the same session.
static void hide_to_tray(HWND hDlg, SettingsWindow *window)
{
	suspend_live(hDlg, window);
	ShowWindow(hDlg, SW_HIDE);

	if (window->on_hidden)
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
		window->last_diag_tick = 0;
		ripple_view_reset(&window->ripple);

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
			// The two column headers share the font: their padding is computed in
			// cells, and a proportional font would break the alignment with the
			// rows underneath.
			const int pane_controls[] = {IDC_TESTLOG, IDC_BLOCKLOG, IDC_DIAG,
										 IDC_TESTLOG_HDR, IDC_BLOCKLOG_HDR};

			for (int i = 0; i < (int)(sizeof(pane_controls) / sizeof(pane_controls[0])); i++)
			{
				SendDlgItemMessageW(hDlg, pane_controls[i], WM_SETFONT,
									(WPARAM)window->log_font, TRUE);
			}
		}

		apply_language(hDlg);
		window->applied_language = language_get();
		create_filter_tooltips(hDlg, window);
		update_filter_ui(hDlg, window, false);
		clear_logs(hDlg, window);
		refresh_diagnostics(hDlg, window, true);
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
		SetTimer(hDlg, SETTINGS_TIMER_ANIM, SETTINGS_ANIM_MS, NULL);
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
			return TRUE;
		}

		if (wParam == SETTINGS_TIMER_ANIM)
		{
			unsigned changed = drain_trace(hDlg, window);
			bool repaint = ripple_view_tick(&window->ripple, GetTickCount64());

			// Only the pane that actually gained a row is rewritten
			if (changed & SETTINGS_LOG_CHANGED)
				show_pane_text(hDlg, window, IDC_TESTLOG, &window->test_log);

			if (changed & SETTINGS_BLOCK_CHANGED)
				show_pane_text(hDlg, window, IDC_BLOCKLOG, &window->block_log);

			if (changed || repaint)
				InvalidateRect(GetDlgItem(hDlg, IDC_RIPPLE), NULL, FALSE);

			return TRUE;
		}

		return TRUE;

	case WM_DRAWITEM:
	{
		const DRAWITEMSTRUCT *item = (const DRAWITEMSTRUCT *)lParam;

		if (window && item && item->CtlID == IDC_RIPPLE)
		{
			ripple_view_draw(&window->ripple, item, GetTickCount64(), lang_str(STR_TEST_HINT));
			return TRUE;
		}

		break;
	}

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDOK:
		case IDCANCEL:
		case IDC_CLOSE:
			// The close button puts the window away, it does not end the program
			// and it does not throw the session away
			if (window)
				hide_to_tray(hDlg, window);
			return TRUE;

		case IDC_REFRESH:
			refresh_diagnostics(hDlg, window, true);
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
			if (window && HIWORD(wParam) == BN_CLICKED)
			{
				// A radio can only be clicked while Auto is off, but the state is
				// set explicitly so the two controls can never disagree
				window->filter_auto = false;
				window->filter_button = (LOWORD(wParam) == IDC_FILTER_LEFT)	 ? MOUSE_BUTTON_LEFT
										: (LOWORD(wParam) == IDC_FILTER_MIDDLE) ? MOUSE_BUTTON_MIDDLE
																			   : MOUSE_BUTTON_RIGHT;
				update_filter_ui(hDlg, window, true);
			}
			return TRUE;

		default:
			break;
		}
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
						  SettingsSnapshotFn snapshot, void *user, EventTrace *trace)
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
