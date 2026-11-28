#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "Winmm.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#define STRICT
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <Windowsx.h>
#include <Commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>
#include <stdlib.h>
#include <Strsafe.h>
#include <stdio.h>
#include "resource.h"
#include "menu_ids.h"
#include "version.h"

// Include modular headers
#include "src/core/config.h"
#include "src/core/mouse_hook.h"
#include "src/core/debouncer.h"
#include "src/core/notify_policy.h"
#include "src/core/settings.h"
#include "src/core/app_controller.h"
#include "src/core/command_classify.h"
#include "src/core/app_snapshot.h"
#include "src/core/event_trace.h"
#include "src/core/event_history.h"
#include "src/core/block_log.h"
#include "src/core/hook_watchdog.h"
#include "src/core/learning_service.h"
#include "src/core/context_filter.h"
#include "src/core/context_service.h"
#include "src/core/context_store.h"
#include "src/core/status_notice.h"
#include <commdlg.h>

#pragma comment(lib, "comdlg32.lib")
#include "src/ui/tray_icon.h"
#include "src/ui/context_menu.h"
#include "src/ui/input_box.h"
#include "src/ui/language.h"
#include "src/ui/settings_window.h"
#include "src/ui/about_window.h"
#include "src/ui/help_window.h"
#include "src/utils/autostart.h"
#include "src/utils/hang_watchdog.h"
#include "src/utils/crash_dump.h"
#include "src/utils/diagnostics.h"
#include "src/utils/trace_log.h"
#include "src/utils/logger.h"
#include "src/utils/support_bundle.h"

// Global application state
typedef struct
{
	HINSTANCE hInstance;
	HWND hWnd;
	HANDLE mutex;

	// Modules
	MouseHookManager mouse_hook;
	DebounceManager debounce;
	TrayIconManager tray_icon;
	ContextMenuManager context_menu;
	Logger logger;

	// Message sent by a second instance to bring up the menu
	UINT show_menu_msg;
	bool deferred_timer_armed;

	// UI preferences (language lives in the language module)
	Preferences prefs;

	// Single source of truth for every settings change
	AppController controller;

	// Modeless settings window (may not exist)
	SettingsWindow settings;

	// Blocked-click history
	BlockLogQueue log_queue;
	SYSTEMTIME log_base_time;
	uint64_t log_base_tick;
	wchar_t log_directory[APP_SNAPSHOT_LOG_DIR_SIZE];
	bool log_queue_ready;
	bool log_flush_requested;
	bool log_warning_shown;
	bool injection_notice_shown; // One balloon per run for lost injections
	uint32_t log_written;

	// Decisions the engine made, shown by the check-up window
	EventTrace trace;
	AppBlockEvidence evidence[MOUSE_BUTTON_COUNT];

	// The same decisions, recorded regardless of the window: this is the only
	// history a crash report can still show when the user was working elsewhere.
	EventHistory history;
	uint64_t start_tick;

	// Hook health
	WatchdogState watchdog;
	POINT watchdog_cursor;
	uint32_t watchdog_buttons;
	uint32_t hook_events;	 // Button / wheel events, used by the test area
	uint32_t hook_recoveries;
	bool state_msg_pending;

	// Opt-in self-learning: the hook only writes the ring; a background thread
	// reads it and publishes a suggestion snapshot for the tray menu.
	SignalRing signals;
	LearningService learning;
	ContextFilter context_filter;
	ContextService context_service;

	// One short lived message for the status bar at the bottom of whichever
	// window is open. Lives here rather than in a window because the actions it
	// reports happen in this file, often with no window open at all.
	StatusNotice notice;

	// Number of suggestions already announced, so the same count is not repeated
	// on every refresh. Reset to zero when the advice disappears (adopted, or
	// the threshold changed under it), which lets the same number be announced
	// again the next time it appears.
	uint32_t advice_notice_count;

	// Blocked-click notification state
	uint64_t notify_last_tick;
	uint32_t notify_reported_blocks;
	bool notify_requested;

	// Initialization flags, so that shutdown stays safe on partial startup
	bool debounce_ready;
	bool hook_ready;
	bool tray_ready;
	bool shutdown_done;

	// Set by InitializeApp, acted on once the message loop can dispatch: a window
	// created from inside WM_CREATE would be shown before CreateWindowW returned,
	// and its SetForegroundWindow would fight the window that does not exist yet.
	bool open_checkup_on_start;

	// Time of the last left click on the tray icon, so a second click within the
	// system double click time opens the window and a single click does nothing.
	uint64_t tray_last_select_tick;

	// Menu commands that arrived with no group claiming them. Counted in release
	// builds too: "this entry does nothing" is a user report, and a counter is
	// the only thing that can confirm it happened.
	volatile LONG unhandled_menu_ids;
} AppState;

static AppState g_app = {0};

// Constants
#define MUTEX_GUID L"{05B95384-625D-491A-A326-94758957C021}"
#define WINDOW_CLASS_NAME L"MouseMendWndClass"
#define LOG_FILE_NAME L"mouse_debouncer.log"
#define LOG_DIRECTORY_NAME L"MouseMend"
#define INPUT_BOX_PROMPT_BUFFER_SIZE 192
#define INPUT_BOX_INPUT_BUFFER_SIZE 32
#define INPUT_BOX_ERROR_BUFFER_SIZE 1024
#define TIMER_DEFERRED_RELEASE 1
#define TIMER_INTERVAL_MS 15
#define TIMER_WATCHDOG 2
#define TIMER_STATE 3
#define WATCHDOG_INTERVAL_MS 3000
#define WATCHDOG_RECOVER_COOLDOWN_MS 30000
#define STATE_INTERVAL_MS 200

// Maximum history entries written per state tick (each one is an open/write/close)
#define BLOCK_LOG_DRAIN_PER_TICK 16
// Tooltip lines are formatted into full width buffers first, then combined; the
// combined text must stay within what the shell accepts (TRAY_TOOLTIP_MAX_CHARS)
#define TOOLTIP_BUFFER_SIZE TRAY_TOOLTIP_MAX_CHARS

// Function declarations
static LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK OnMouseHookCallback(const MouseEvent *event, void *user_data);
static bool RegisterInvisibleClass(const HINSTANCE hInstance);
static bool InitializeApp(void);
static void ShutdownApp(void);
static void ShowContextMenu(const HWND hWnd, const int x, const int y);
static void UpdateTrayTooltip(void);
static void CDECL ShowErrorMessageBox(LPCWSTR message, ...);

// Declared early: export handlers defined above the helper still need it.
static HWND UiDialogOwner(void);
static void ToggleEnableAll(void);
static void ToggleButton(MouseButton button);
static void ToggleHybridHeuristic(void);
static void ToggleNotifyBlocks(void);
static void NotifyBlockedClick(void);
static void ToggleAutostart(void);
static void ApplyPreset(const PresetConfig *preset, bool save);
static void RestoreDefaults(void);
static void SetButtonThreshold(MouseButton button, int threshold_ms);
static void PromptCustomThreshold(MouseButton button);
static void SetLanguage(Language language);
static void SaveSettings(void);
static bool IsHookActive(void);
static void RecoverHook(void);
static void FlushLogQueue(void);
static void ToggleLogBlocks(void);
static void OpenLogFolder(void);
static void BuildSnapshot(AppSnapshot *snapshot, void *user);
static bool SaveSink(const DebounceManager *debounce, const Preferences *prefs, void *user);
static void ResetStatsSink(void *user);
static void ObserverSink(void *user);
static void RemoveContextPath(ContextMode list, uint32_t index);
static void PromptAddContextPath(ContextMode list);

// Poll the physical state of the mouse buttons that can trigger a block
// GetAsyncKeyState works without a hook, so it is the evidence used to prove that
// a dead hook is really dead rather than merely idle.
static uint32_t PollMouseButtons(void)
{
	static const int BUTTON_KEYS[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
	uint32_t mask = 0;

	for (int i = 0; i < (int)(sizeof(BUTTON_KEYS) / sizeof(BUTTON_KEYS[0])); i++)
	{
		if (GetAsyncKeyState(BUTTON_KEYS[i]) & 0x8000)
			mask |= (1u << i);
	}

	return mask;
}

// Make sure the timer that gives a held release back can run at all.
//
// Called on a physical press, i.e. before the engine is allowed to hold anything
// back. If the timer cannot be created the engine is told never to defer:
// holding a release with no way to deliver it leaves the button pressed inside
// the system, and the old fallback - deliver it right here - was itself a
// SendInput from inside the hook callback. That fallback is what used to make a
// timer failure indistinguishable from the deadlock it was meant to prevent.
static void EnsureSmartDragTimer(void)
{
	// Without a window the timer would be a thread timer whose WM_TIMER nobody
	// dispatches: the release would be held forever. Without a live engine there
	// is no state to touch either.
	if (!g_app.debounce_ready || !g_app.hWnd)
		return;

	if (g_app.deferred_timer_armed)
		return;

	if (SetTimer(g_app.hWnd, TIMER_DEFERRED_RELEASE, TIMER_INTERVAL_MS, NULL))
	{
		g_app.deferred_timer_armed = true;
		debounce_set_defer_available(&g_app.debounce, true);
		trace_lifecycle(TRACE_LIFE_TIMER_ARM);
		return;
	}

	// No timer: Smart Drag stops holding releases, everything else keeps working.
	debounce_set_defer_available(&g_app.debounce, false);
}

// Threshold provider for the learning service.
//
// Reads the engine's field directly on purpose. DebounceManager's public
// accessors take the engine's critical section, and the worker asks for seven
// thresholds on every poll - going through them would put it in contention with
// the hook thread a hundred times a second. The engine is the only writer, and
// a uint32_t read is atomic, so a plain field read is both safe and correct.
static uint32_t LearningThresholdFor(int button, void *user)
{
	const AppState *app = (const AppState *)user;

	if (!app || button < 0 || button >= MOUSE_BUTTON_COUNT)
		return 0;

	return app->debounce.buttons[button].thresholdMs;
}

// Mouse hook callback
static LRESULT CALLBACK OnMouseHookCallback(const MouseEvent *event, void *user_data)
{
	AppState *app = (AppState *)user_data;

	// A release can be held back, and only a running timer can give it back.
	//
	// The timer stops itself when it has nothing to do, so a press from two
	// seconds ago is no promise that one is alive now. Without it the engine
	// still believes the release will be delivered, swallows it, and the system
	// keeps the button down - a window then follows the pointer after the user
	// has let go, until the next click clears the state. Arming here, before the
	// verdict is reached, keeps "may defer" and "a timer exists" the same fact.
	// SetTimer from a hook callback is a plain kernel call, unlike SendInput.
	if (!event->is_down && !event->is_injected && app->debounce.use_hybrid_heuristic)
		EnsureSmartDragTimer();

	// Process event directly - avoid creating intermediate structure
	// The explanation is kept because the check-up window renders the engine's
	// own verdict instead of repeating the rules somewhere else.
	// One atomic read of the published slot; no process/window API on this stack.
	bool suppress = context_filter_should_suppress(&app->context_filter.snap, event->timestamp);
	DebounceDecision decision;
	bool blocked = debounce_process_event_ex_suppressed(&app->debounce, event, &decision, suppress);

	// Opt-in learning: one Interlocked increment, no lock, no allocation, no API
	// call. elapsed_ms is the exact quantity the threshold compared, so the
	// learned distribution cannot drift away from the decision it explains.
	//
	// Only a press that was actually blocked is sampled.
	//
	// Sampling every fast press inside a fixed window fixed one problem (the
	// histogram used to describe the threshold instead of the switch) and created
	// a worse one: a normal 55 ms double click under a 50 ms threshold passes,
	// still gets sampled, and pulls the advice up to 56 - adopting it would then
	// start swallowing those very clicks. "It worked until I turned on
	// self-learning" is the worst possible outcome for this feature.
	//
	// A blocked press is the only evidence that matters: the user lost a click,
	// and the advice answers "how much higher would have absorbed it". A press
	// that passed produced no complaint, so there is nothing to learn from it.
	// The window is still fixed at 64 ms, so the distribution never depends on
	// the current threshold.
	//
	// A zero gap is a real sample, not a missing one: GetTickCount64 advances in
	// roughly 15.6 ms steps, so a 5 ms chatter almost always lands in the same
	// tick as the release before it. Dropping those samples threw away the most
	// certain chatter there is. Only the very first press has no gap at all, and
	// that one is excluded by first_event rather than by the value.
	if (app->prefs.learning_enabled && blocked && event->is_down && !event->is_injected &&
		decision.reason != DEBOUNCE_REASON_IGNORE_INJECTED &&
		decision.reason != DEBOUNCE_REASON_IGNORE_UNMONITORED &&
		!decision.first_event &&
		decision.elapsed_ms <= (uint64_t)SIGNAL_RING_SAMPLE_MAX_MS)
	{
		signal_ring_note_interval(&app->signals, event->button, decision.elapsed_ms);
	}

	// Trace build only: what arrived, what was decided, and what Windows thinks
	// about the button a moment later. That last part is the whole point - it is
	// the difference between "we let it through" and "the system saw it".
	trace_button(event->button, event->is_down, event->is_injected, event->x, event->y,
				 debounce_reason_slug(decision.reason), blocked);

	// The program the verdict was made for. Read once per event, because two
	// readers below describe the same click and must not disagree: the history
	// ring and the block log both carry this name, and two separate reads could
	// straddle an update of the slot.
	//
	// Copied from the published slot, never resolved here: a user32 call on this
	// stack risks the hook being removed. See the known limitation on
	// TraceEntry.process.
	wchar_t program[EVENT_TRACE_PROCESS_CHARS] = {0};
	int32_t program_pid = 0;
	context_snapshot_copy_program(&app->context_filter.snap, program, ARRAYSIZE(program), &program_pid);

	// Record the decision for the check-up window. Memory only: the hook has to
	// return within its budget or Windows silently removes it.
	{
		TraceEntry entry;
		entry.button = event->button;
		entry.timestamp = event->timestamp;
		entry.x = event->x;
		entry.y = event->y;
		entry.is_down = event->is_down;
		entry.data = event->data;
		entry.decision = decision;
		entry.pid = program_pid;

		StringCchCopyW(entry.process, ARRAYSIZE(entry.process), program);

		event_trace_push(&app->trace, &entry);

		// Always on, unlike the trace above: a crash report has to describe what
		// the user was doing even when that was outside the check-up window.
		event_history_push(&app->history, &entry);
	}

	// Evidence for the self check. Only a press the engine refused counts, and
	// only for the reasons that mean "this looked like a false click": a
	// swallowed release belongs to the same click and a held release is normal
	// Smart Drag behaviour.
	if (event->is_down && !event->is_injected && debounce_decision_is_false_click(&decision))
	{
		AppBlockEvidence *evidence = &app->evidence[event->button];
		evidence->valid = true;
		evidence->timestamp = event->timestamp;
		evidence->elapsed_ms = decision.elapsed_ms;
		evidence->press_gap_ms = decision.press_gap_ms;
		evidence->has_press_gap = decision.has_press_gap;
		evidence->threshold_ms = decision.threshold_ms;
	}

	// Feed the health watchdog: it compares this counter with the system wide
	// last-input time to notice a hook that Windows removed behind our back.
	app->hook_events++;

	// Arm the timer that can give a held release back, on the press rather than
	// on the release, so the engine only ever holds one it is able to return.
	if (event->is_down && !event->is_injected)
		EnsureSmartDragTimer();

	if (blocked)
	{
		// Queue the history entry. Only memory writes happen here; the file is
		// written on the UI thread after the hook has returned.
		if (app->prefs.log_blocks && app->log_queue_ready)
		{
			uint32_t threshold = app->debounce.buttons[event->button].thresholdMs;

			// Guard the subtraction: a timestamp older than the captured base would
			// wrap into a gigantic elapsed value and poison the log timestamps
			uint64_t elapsed = (event->timestamp >= app->log_base_tick)
								   ? (event->timestamp - app->log_base_tick)
								   : 0;

			if (block_log_push(&app->log_queue, event->button, event->is_down, elapsed, threshold,
							   program))
				app->log_flush_requested = true;
		}

		// No PostMessage and no shell call here on purpose: both can block (the
		// message queue has a timeout and Shell_NotifyIcon is cross-process) and a
		// low-level hook that overruns its budget is silently uninstalled by
		// Windows. Setting a flag is free; the 200 ms state timer picks it up.
		// Wheel reversals are skipped because they happen during normal scrolling.
		if (app->prefs.notify_blocks &&
			event->button != MOUSE_BUTTON_WHEEL && event->button != MOUSE_BUTTON_HWHEEL)
		{
			app->notify_requested = true;
		}

		// Event should be blocked
		return 1;
	}

	// Event should pass through
	return CallNextHookEx(NULL, 0, 0, 0);
}

// Check whether now is a bad moment for a balloon
// Games and presentations treat any notification as an interruption (and some
// switch the display mode), so those states are treated as "do not disturb".
static bool NotificationsSuppressed(void)
{
	QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;

	if (FAILED(SHQueryUserNotificationState(&state)))
		return false;

	return state != QUNS_ACCEPTS_NOTIFICATIONS && state != QUNS_APP;
}

// Blocked counts of the buttons that can produce a balloon.
// Wheel axes are excluded on purpose: a wheel reversal happens during normal
// scrolling, so it must neither raise a balloon nor inflate its numbers.
static uint32_t NotifyButtonBlocks(void)
{
	uint32_t total = 0;

	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		MouseButton button = (MouseButton)i;

		if (config_is_wheel_button(button))
			continue;

		total += debounce_get_button_blocks(&g_app.debounce, button);
	}

	return total;
}

// Find the button that is producing most blocks - that is the switch that is
// actually failing, which is the one piece of information a user can act on.
static const wchar_t *MostBlockedButtonName(void)
{
	const wchar_t *name = lang_button_name(MOUSE_BUTTON_LEFT);
	uint32_t highest = 0;

	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		MouseButton button = (MouseButton)i;

		if (config_is_wheel_button(button))
			continue;

		uint32_t blocks = debounce_get_button_blocks(&g_app.debounce, button);

		if (blocks > highest)
		{
			highest = blocks;
			name = lang_button_name(button);
		}
	}

	return name;
}

// Show the "false click blocked" balloon, throttled so a badly worn switch
// cannot turn the feedback into spam.
static void NotifyBlockedClick(void)
{
	if (!g_app.prefs.notify_blocks || !g_app.tray_ready)
		return;

	uint32_t total = NotifyButtonBlocks();
	uint64_t now = GetTickCount64();

	NotifyState state;
	state.last_tick = g_app.notify_last_tick;
	state.reported_blocks = g_app.notify_reported_blocks;

	NotifyDecision decision;
	if (!notify_policy_decide(state, total, now, NOTIFY_MIN_INTERVAL_MS, &decision))
		return;

	if (!decision.show)
		return;

	// Do not interrupt a game, a presentation or a locked screen.
	// Only the throttle timestamp moves forward (so closing a game is not
	// immediately followed by a balloon); the reported counter keeps its old
	// value, therefore the next balloon after the interruption reports every
	// block that accumulated in between instead of dropping them.
	if (NotificationsSuppressed())
	{
		g_app.notify_last_tick = now;
		return;
	}

	wchar_t text[TRAY_BALLOON_MAX_CHARS];
	StringCchPrintfW(text, ARRAYSIZE(text), lang_str(STR_NOTIFY_BLOCKED_FMT), decision.delta, MostBlockedButtonName());

	// A failed balloon is not treated as reported, but the throttle is still
	// advanced so a broken shell cannot turn every click into a retry.
	if (!tray_icon_show_balloon(&g_app.tray_icon, lang_str(STR_APP_NAME), text, NIIF_INFO))
	{
		g_app.notify_last_tick = now;
		return;
	}

	g_app.notify_reported_blocks = decision.next_state.reported_blocks;
	g_app.notify_last_tick = decision.next_state.last_tick;
}

// Build the debug log path: %LOCALAPPDATA%\MouseMend\mouse_debouncer.log
// Roaming a log file is pointless, and the working directory of an auto-started
// process is not the application directory.
static void BuildLogPath(wchar_t *path, size_t path_count)
{
	wchar_t local_app_data[SUPPORT_PATH_CHARS] = {0};

	if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, ARRAYSIZE(local_app_data)) > 0)
	{
		wchar_t directory[SUPPORT_PATH_CHARS];
		StringCchPrintfW(directory, ARRAYSIZE(directory), L"%s\\%s", local_app_data, LOG_DIRECTORY_NAME);
		CreateDirectoryW(directory, NULL);

		StringCchPrintfW(path, path_count, L"%s\\%s", directory, LOG_FILE_NAME);
		return;
	}

	StringCchCopyW(path, path_count, LOG_FILE_NAME);
}

// ------------------------------------------------------------ support bundle

// Registry value remembering which crash report the user was already told about,
// so the notice appears once per crash and not on every start
#define REG_VALUE_LAST_CRASH L"LastCrashNotice"

static void ReadLastCrashNotice(wchar_t *buffer, size_t buffer_count)
{
	if (!buffer || buffer_count == 0)
		return;

	buffer[0] = L'\0';

	HKEY key = NULL;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_SETTINGS_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
		return;

	DWORD type = 0;
	DWORD bytes = (DWORD)(buffer_count * sizeof(wchar_t));
	LONG result = RegQueryValueExW(key, REG_VALUE_LAST_CRASH, NULL, &type,
								   (LPBYTE)buffer, &bytes);
	RegCloseKey(key);

	if (result != ERROR_SUCCESS || type != REG_SZ)
	{
		buffer[0] = L'\0';
		return;
	}

	// A value truncated by the registry would not be terminated
	buffer[buffer_count - 1] = L'\0';
}

static void WriteLastCrashNotice(const wchar_t *value)
{
	if (!value)
		return;

	HKEY key = NULL;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_SETTINGS_KEY, 0, NULL, 0,
						KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
		return;

	RegSetValueExW(key, REG_VALUE_LAST_CRASH, 0, REG_SZ,
				   (const BYTE *)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
	RegCloseKey(key);
}

// Everything the per-module capability verdict depends on.
//
// Shared by the window snapshot and the support bundle on purpose: two copies of
// this list is how the two reports start disagreeing about which module broke,
// and the copy that is wrong will be the one nobody is looking at.
static void FillCapabilityInputs(CapabilityInputs *caps)
{
	if (!caps)
		return;

	memset(caps, 0, sizeof(*caps));

	caps->hook_ready = g_app.hook_ready;
	caps->hook_installed = g_app.mouse_hook.installed;
	caps->hook_healthy = hook_watchdog_is_healthy(&g_app.watchdog);

	caps->learning_enabled = g_app.prefs.learning_enabled;
	caps->learning_started = learning_service_is_started(&g_app.learning);

	uint32_t samples = 0;
	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
		samples += signal_ring_samples(&g_app.signals, i);
	caps->learning_has_samples = samples > 0;

	caps->context_active =
		context_filter_get_mode(&g_app.context_filter) != CONTEXT_MODE_ALL;
	caps->context_started = context_service_is_running(&g_app.context_service);
	caps->context_paused = context_filter_is_paused(&g_app.context_filter, GetTickCount64());
	caps->context_observed = g_app.context_service.observed != 0;

	caps->log_enabled = g_app.prefs.log_blocks;
	caps->log_ready = g_app.log_queue_ready;
	caps->log_directory_resolved = g_app.log_directory[0] != L'\0';
}

// Copy the live state into a report.
//
// The engine fields are read without their critical section on purpose: this is
// also called from the crash handler, where the lock may be held by a thread
// that no longer exists. A value read a few instructions late is harmless; a
// report that waits forever is not a report.
static void CollectSupportReport(SupportReport *report)
{
	if (!report)
		return;

	// "not a crash" by default; the crash handler flips this after collecting
	support_bundle_prepare(report, false);
	report->app_start_tick = g_app.start_tick;

	StringCchCopyW(report->language, ARRAYSIZE(report->language),
				   (language_get() == LANG_ZH_CN) ? L"zh-CN" : L"en");

	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		report->thresholds[i] = g_app.debounce.buttons[i].thresholdMs;
		report->monitored[i] = g_app.debounce.buttons[i].isMonitored;
		report->blocks[i] = g_app.debounce.buttons[i].blocks;
		report->total_blocks += report->blocks[i];
	}

	report->hybrid = g_app.debounce.use_hybrid_heuristic;
	report->release_delay_ms = g_app.debounce.releaseDelayMs;
	report->drag_distance_px = g_app.debounce.dragDistancePx;

	report->notify_blocks = g_app.prefs.notify_blocks;
	report->log_blocks = g_app.prefs.log_blocks;
	StringCchCopyW(report->log_directory, ARRAYSIZE(report->log_directory), g_app.log_directory);

	report->hook_active = IsHookActive();
	report->hook_events = g_app.hook_events;
	report->hook_recoveries = g_app.hook_recoveries;

	report->injection_failures = debounce_injection_failures();
	report->injection_refused = debounce_injection_refusals();

	report->event_count = event_history_copy(&g_app.history, report->events, SUPPORT_EVENT_MAX);

	// Runtime state. Everything above is configuration; this block is what the
	// engine is actually doing at this moment, which is the part a "it does not
	// work" report needs and the part `--diagnose` structurally cannot provide.
	{
		CapabilityInputs caps;
		CapabilityState states[CAP_COUNT];
		LearningAdvice advice;
		uint64_t now = GetTickCount64();
		const ContextSnapshot *context = &g_app.context_filter.snap;
		LONG current = InterlockedCompareExchange(&g_app.context_filter.snap.current, -1, -1);

		FillCapabilityInputs(&caps);
		app_capabilities_evaluate(&caps, states);

		for (int i = 0; i < CAP_COUNT; i++)
			report->capabilities[i] = (uint8_t)states[i];

		// A pause that is still counting down explains "it let a click through"
		// better than any other field in this report.
		report->context_paused = context_filter_is_paused(&g_app.context_filter, now);
		report->context_pause_left_ms = 0;

		if (report->context_paused)
		{
			LONG64 left = InterlockedCompareExchange64(&g_app.context_filter.snap.pause_until, 0, 0) -
						  (LONG64)now;

			if (left > 0 && left <= 24ull * 60ull * 60ull * 1000ull)
				report->context_pause_left_ms = (uint32_t)left;
		}

		// Try, never wait: this runs from the crash and hang paths too, where the
		// lock may belong to a thread that is already dead. A missing advice count
		// is a gap in the report; a report that hangs the dying process is nothing.
		if (!learning_service_try_snapshot(&g_app.learning, &advice))
			report->advice_count = 0; // the worker was mid update; report "none"
		else
			report->advice_count = learning_advice_visible_count(&advice);

		if (current >= 0 && current <= 1)
			StringCchCopyW(report->foreground, ARRAYSIZE(report->foreground),
						   context->slots[current].name);
	}
}

// Build one bundle from the menu and tell the user where it landed
static void ExportSupportBundle(void)
{
	// Static: the report is ~10 KB and the crash path must not touch the stack
	// more than it has to.
	static SupportReport report;

	CollectSupportReport(&report);

	wchar_t path[SUPPORT_PATH_CHARS];
	bool written = support_bundle_write(&report, NULL, path, ARRAYSIZE(path));

	if (!written)
	{
		MessageBoxW(UiDialogOwner(), lang_str(STR_EXPORT_SUPPORT_FAILED), APP_NAME_W,
					MB_OK | MB_ICONWARNING);
		return;
	}

	wchar_t text[TRAY_BALLOON_MAX_CHARS * 2];
	StringCchPrintfW(text, ARRAYSIZE(text), lang_str(STR_EXPORT_SUPPORT_DONE_FMT), path);
	MessageBoxW(UiDialogOwner(), text, APP_NAME_W, MB_OK | MB_ICONINFORMATION);
}

// Tell the user, once, that the previous run left a report behind
static void NotifyPreviousCrash(void)
{
	wchar_t path[SUPPORT_PATH_CHARS];
	LangStringId message = STR_CRASH_REPORT_FOUND_FMT;

	// A hang is reported in preference to a crash: it is the case where the
	// process stayed alive and left no exception behind, which is the one the
	// user is least likely to be able to describe.
	if (support_bundle_find_newest(SUPPORT_PREFIX_HANG, path, ARRAYSIZE(path)))
	{
		message = STR_HANG_REPORT_FOUND_FMT;
	}
	else if (!support_bundle_find_newest(SUPPORT_PREFIX_CRASH, path, ARRAYSIZE(path)))
	{
		return;
	}

	wchar_t notified[SUPPORT_PATH_CHARS];
	ReadLastCrashNotice(notified, ARRAYSIZE(notified));

	if (notified[0] != L'\0' && wcscmp(notified, path) == 0)
		return;

	WriteLastCrashNotice(path);

	if (!g_app.tray_ready)
		return;

	wchar_t text[TRAY_BALLOON_MAX_CHARS * 2];
	StringCchPrintfW(text, ARRAYSIZE(text), lang_str(message), path);
	tray_icon_show_balloon(&g_app.tray_icon, APP_NAME_W, text, NIIF_INFO);
}

// Initialize application
static bool InitializeApp(void)
{
#ifndef NDEBUG
	// Initialize logger (only in Debug mode)
	wchar_t log_path[SUPPORT_PATH_CHARS];
	BuildLogPath(log_path, ARRAYSIZE(log_path));

	if (!logger_init(&g_app.logger, LOG_LEVEL_INFO, log_path))
	{
		ShowErrorMessageBox(lang_str(STR_ERR_LOGGER));
		return false;
	}

	LOG_INFO(&g_app.logger, "MouseMend starting...");
#endif

	// Initialize debounce manager
	if (!debounce_init(&g_app.debounce))
	{
#ifndef NDEBUG
		LOG_ERROR(&g_app.logger, "Failed to initialize debounce manager");
#endif
		return false;
	}
	g_app.debounce_ready = true;

	// Decision ring for the check-up window (plain memory, no IO)
	event_trace_init(&g_app.trace);

	// The same decisions, kept for the support report even when the window is
	// closed or the user clicks outside the test area.
	event_history_init(&g_app.history);
	g_app.start_tick = GetTickCount64();

	// Attach the controller before the first preset is applied: every mutation
	// goes through it, so applying a preset earlier would silently do nothing.
	app_controller_attach(&g_app.controller, &g_app.debounce, &g_app.prefs);

	// Baseline configuration; saved settings override it below
	ApplyPreset(&CONFIG_PRESET_DEFAULT, false);
	debounce_set_release_delay(&g_app.debounce, RELEASE_DELAY_DEFAULT_MS, RELEASE_DELAY_MIN_MS, RELEASE_DELAY_MAX_MS);
	debounce_set_drag_distance(&g_app.debounce, DRAG_DISTANCE_DEFAULT_PX, DRAG_DISTANCE_MIN_PX, DRAG_DISTANCE_MAX_PX);

	g_app.prefs.language = language_get();
	g_app.prefs.intro_shown = false;
	g_app.prefs.notify_blocks = false;

	if (!settings_load(&g_app.debounce, &g_app.prefs))
	{
		// First run (or a fresh profile): enable every button so that the
		// protection is actually active out of the box.
		debounce_set_all_monitored(&g_app.debounce, true);

#ifndef NDEBUG
		LOG_INFO(&g_app.logger, "No saved settings found, enabled all buttons");
#endif
	}

	language_set(g_app.prefs.language);

	// Persistence and change notification are installed once the preferences are
	// loaded, so an early save cannot write a half-initialised profile
	app_controller_set_save_sink(&g_app.controller, SaveSink, &g_app);
	app_controller_set_reset_sink(&g_app.controller, ResetStatsSink, &g_app);
	app_controller_set_observer(&g_app.controller, ObserverSink, &g_app);

	// Blocked-click history. The clock base is captured once so that the hook
	// callback only has to store an offset and never calls a time API.
	GetLocalTime(&g_app.log_base_time);
	g_app.log_base_tick = GetTickCount64();

	if (block_log_init(&g_app.log_queue))
		g_app.log_queue_ready = true;

	signal_ring_init(&g_app.signals);
	context_filter_init(&g_app.context_filter);
	status_notice_init(&g_app.notice);
	context_store_load(&g_app.context_filter.policy);
	app_controller_attach_context(&g_app.controller, &g_app.context_filter);

	hook_watchdog_init(&g_app.watchdog);
	GetCursorPos(&g_app.watchdog_cursor);

	// Initialize mouse hook
	if (!mouse_hook_init(&g_app.mouse_hook, OnMouseHookCallback, &g_app))
	{
#ifndef NDEBUG
		LOG_ERROR(&g_app.logger, "Failed to initialize mouse hook");
#endif
		return false;
	}

	// Install mouse hook
	if (!mouse_hook_install(&g_app.mouse_hook))
	{
#ifndef NDEBUG
		LOG_ERROR(&g_app.logger, "Failed to install mouse hook");
#endif
		return false;
	}
	g_app.hook_ready = true;

#ifndef NDEBUG
	LOG_INFO(&g_app.logger, "Mouse hook installed successfully");
#endif

	// Start watching for a hang. Before this point the message loop has never
	// run, so a heartbeat would be missing for a reason that is not a hang.
	// Self-learning is opt-in. The service is cheap when disabled and keeps the
	// hook free of all aggregation: the callback only increments two counters.
	if (g_app.prefs.learning_enabled &&
		!learning_service_start(&g_app.learning, &g_app.signals, LearningThresholdFor, &g_app))
	{
		g_app.prefs.learning_enabled = false;
	}

	// Rule table is empty by default, so this thread only publishes "not
	// excluded" until the user adds a rule. It never blocks the hook thread.
	context_service_start(&g_app.context_service, &g_app.context_filter);

	if (!hang_watchdog_start(CollectSupportReport))
	{
#ifndef NDEBUG
		LOG_ERROR(&g_app.logger, "Hang watchdog could not be started");
#endif
	}

	// Initialize tray icon
	if (!tray_icon_init(&g_app.tray_icon, g_app.hWnd, g_app.hInstance, IDI_NOTIFYICON))
	{
#ifndef NDEBUG
		LOG_ERROR(&g_app.logger, "Failed to initialize tray icon");
#endif
		return false;
	}
	g_app.tray_ready = true;

	// Add tray icon
	if (!tray_icon_add(&g_app.tray_icon))
	{
#ifndef NDEBUG
		LOG_ERROR(&g_app.logger, "Failed to add tray icon");
#endif
		return false;
	}

	// If the previous run died, the report it left behind is the only way the
	// user will ever find it: the folder it lands in is not one he would search.
	// Shown once per file, so a crash loop does not turn into a balloon loop.
	NotifyPreviousCrash();

	// The check-up window opens on every start: until now the application was
	// invisible until the user found the tray icon, which made a working program
	// look like a dead one. It is only *requested* here - see ShowStartupWindow,
	// which runs once the message loop can dispatch, because a dialog created
	// from inside WM_CREATE is shown before its owner has finished being created.
	g_app.open_checkup_on_start = true;

	// The old first-run text is no longer shown, but the flag is still kept so an
	// existing profile stays meaningful. It is marked dirty instead of written
	// here: WM_CREATE runs after the hook is installed, and a slow registry write
	// on this thread would let the hook miss its time budget.
	if (!g_app.prefs.intro_shown)
	{
		g_app.prefs.intro_shown = true;
		app_controller_touch(&g_app.controller);
	}

	// Hook health checks and the debounced settings commit
	SetTimer(g_app.hWnd, TIMER_WATCHDOG, WATCHDOG_INTERVAL_MS, NULL);
	SetTimer(g_app.hWnd, TIMER_STATE, STATE_INTERVAL_MS, NULL);

#ifndef NDEBUG
	LOG_INFO(&g_app.logger, "Application initialized successfully");
#endif

	return true;
}

// Open the check-up window, or bring it back from the tray
//
// One entry point for the menu, the tray double click and the balloon, so no
// caller can forget to restore the hidden window instead of creating a second.
static void OpenCheckupWindow(void)
{
	if (!settings_window_open(&g_app.settings, g_app.hInstance, g_app.hWnd,
							  BuildSnapshot, &g_app, &g_app.trace, &g_app.history))
	{
		// Silence here is what a user would read as "the program is broken".
#ifndef NDEBUG
		LOG_ERROR(&g_app.logger, "Failed to open the check-up window");
#endif
		tray_icon_show_balloon(&g_app.tray_icon, lang_str(STR_APP_NAME),
							   lang_str(STR_SETTINGS_UNAVAILABLE), NIIF_WARNING);
	}
}

// Tell the user where the window went
//
// Called by the window itself when the user clicks the X or the Close button.
// The window is the main window, so closing it is not the end of anything: this
// is the sentence that stops the user from thinking he just quit the program.
static void OnCheckupWindowHidden(void *user)
{
	(void)user;

	tray_icon_show_balloon(&g_app.tray_icon, lang_str(STR_TRAY_BALLOON_TITLE),
						   lang_str(STR_TRAY_BALLOON_TEXT), NIIF_INFO);
}

// Put a line from the check-up window into the status bar
//
// The window owns no notice slot: a line it posts has to outlive the dialog being
// hidden and share the cell with the hook recoveries, so it goes through the
// application's slot like every other notice.
static void OnWindowNotice(void *user, const wchar_t *text, uint32_t milliseconds)
{
	(void)user;

	status_notice_post(&g_app.notice, text, milliseconds);
}

// Show what the user should see right after a successful start
//
// Called once from wWinMain, between CreateWindowW and the message loop: at that
// point the owner window exists, so the dialog can take the foreground.
static void ShowStartupWindow(void)
{
	// Registered once, before any window can exist: the handler is what tells the
	// user where the window went, and it has to be in place before the first
	// close button is ever clicked.
	settings_window_set_hidden_handler(&g_app.settings, OnCheckupWindowHidden, &g_app);

	// The window has no notice slot of its own: a line it posts has to outlive the
	// dialog being hidden and share the cell with the hook recoveries, so it is
	// written into the application's slot.
	settings_window_set_notice_handler(&g_app.settings, OnWindowNotice, &g_app);

	if (!g_app.open_checkup_on_start)
		return;

	g_app.open_checkup_on_start = false;

	// No balloon here on purpose: the window is the main window of the program
	// and it is already on screen. "It went to the tray" is only true later, when
	// the user closes it.
	OpenCheckupWindow();
}

// Shutdown application
// Safe to call after a partial initialization: every step checks its own flag.
// Only the first call does any work, so the GetMessage failure path, the
// WM_DESTROY path and the CreateWindow failure path can all call it.
static void ShutdownApp(void)
{
	if (g_app.shutdown_done)
		return;

	g_app.shutdown_done = true;

#ifndef NDEBUG
	LOG_INFO(&g_app.logger, "Shutting down application...");
#endif

	// Stop the deferred-release timer
	if (g_app.deferred_timer_armed && g_app.hWnd)
	{
		KillTimer(g_app.hWnd, TIMER_DEFERRED_RELEASE);
		g_app.deferred_timer_armed = false;
	}

	if (g_app.hWnd)
	{
		KillTimer(g_app.hWnd, TIMER_WATCHDOG);
		KillTimer(g_app.hWnd, TIMER_STATE);
	}

	// Stop the learning service before the debounce manager it reads. Bounded
	// internally, and still inside the hang watchdog's coverage window.
	learning_service_stop(&g_app.learning);
	context_service_stop(&g_app.context_service);

	// Stop watching before anything it reads is torn down. Bounded internally:
	// a shutdown must never wait on the watchdog.
	hang_watchdog_stop();

	// Close the optional settings window before the modules it reads disappear
	settings_window_close(&g_app.settings);

	// Persist a pending slider change and write out any queued history entry
	if (g_app.debounce_ready)
	{
		app_controller_commit(&g_app.controller);
		FlushLogQueue();
	}

	// A pending request must not survive into a state where nothing would service
	// it any more (the state timer is being killed right below)
	g_app.notify_requested = false;
	g_app.log_flush_requested = false;

	// Release anything still held back, otherwise the button would stay
	// logically pressed after this process is gone.
	if (g_app.debounce_ready)
		debounce_flush_deferred_releases(&g_app.debounce);

	// Remove tray icon
	if (g_app.tray_ready)
	{
		tray_icon_remove(&g_app.tray_icon);
		g_app.tray_ready = false;
	}

	// Uninstall mouse hook
	if (g_app.hook_ready)
	{
		mouse_hook_uninstall(&g_app.mouse_hook);
		g_app.hook_ready = false;
	}

	// Release menu resources
	context_menu_destroy(&g_app.context_menu);

	// Cleanup modules
	if (g_app.log_queue_ready)
	{
		block_log_cleanup(&g_app.log_queue);
		g_app.log_queue_ready = false;
	}

	event_trace_cleanup(&g_app.trace);
	event_history_cleanup(&g_app.history);

	if (g_app.debounce_ready)
	{
		debounce_cleanup(&g_app.debounce);
		g_app.debounce_ready = false;
	}

	// Last, so everything above still lands in the file
	trace_lifecycle(TRACE_LIFE_APP_STOP);
	trace_stop();

#ifndef NDEBUG
	LOG_INFO(&g_app.logger, "Application shutdown complete");
	logger_cleanup(&g_app.logger);
#endif
}

// Persist the current state immediately.
// Menu actions are deliberate single clicks, so they should hit the disk right
// away; only continuous input (the settings window sliders) uses the debounced
// app_controller_tick() path.
static void SaveSettings(void)
{
	// The language module is the single source of truth for the active language,
	// so mirror it here instead of relying on every caller to keep prefs in sync.
	g_app.prefs.language = language_get();

	// Settings edited outside the controller (intro_shown) still need a write
	app_controller_touch(&g_app.controller);
	app_controller_commit(&g_app.controller);
}

// Persistence sink used by the controller
// A read-only registry (policy, security software) would otherwise make every
// change silently disappear on the next start, so warn once per session.
static bool SaveSink(const DebounceManager *debounce, const Preferences *prefs, void *user)
{
	static bool warning_shown = false;
	AppState *app = (AppState *)user;

	// The context policy lives outside Preferences, so one save covers both.
	bool saved = settings_save(debounce, prefs);
	if (app)
		saved = context_store_save(&app->context_filter.policy) && saved;

	if (saved)
		return true;

	// A read-only registry makes every change disappear on the next start. The
	// balloon says it once; this says it every time it happens, because "I
	// changed it and it did not stick" is worth repeating.
	status_notice_post(&g_app.notice, lang_str(STR_SB_SAVE_FAILED), 10000);

	if (!warning_shown && g_app.tray_ready)
	{
		tray_icon_show_balloon(&g_app.tray_icon, lang_str(STR_APP_NAME), lang_str(STR_SAVE_FAILED), NIIF_WARNING);
		warning_shown = true;
	}

	return false;
}

// Owner for every dialog the user is supposed to see.
//
// The tray window is hidden at 0,0, so a dialog owned by it is technically fine
// but has no visible parent to sit on top of. When the check-up window is open,
// both it and the new dialog are owned by the same hidden window - they are
// siblings, and Windows stacks a freshly created dialog *under* the visible one.
// That is why "add a program" looked like it did nothing: the file dialog was
// there, behind the check-up window. Owning the dialog to the visible window
// also makes it modal to that window, which is what the user expects.
static HWND UiDialogOwner(void)
{
	if (settings_window_is_visible(&g_app.settings))
		return g_app.settings.hwnd;

	return g_app.hWnd;
}

// Remove one entry, after a confirmation that shows the full path (the list
// itself only shows the file name, which can be ambiguous).
static void RemoveContextPath(ContextMode list, uint32_t index)
{
	// Static: a policy copy is ~65 KB and must not go on the stack.
	static ContextPolicy policy;
	context_filter_copy_policy(&g_app.context_filter, &policy);

	const ContextPathList *paths = (list == CONTEXT_MODE_WHITELIST) ? &policy.whitelist
																   : &policy.blacklist;
	const wchar_t *path = context_path_list_at(paths, index);

	if (!path || !path[0])
		return;

	wchar_t text[CONTEXT_PATH_CHARS + 128];
	if (FAILED(StringCchPrintfW(text, ARRAYSIZE(text), lang_str(STR_CONTEXT_REMOVE_FMT), path)))
		return;

	if (MessageBoxW(UiDialogOwner(), text, APP_NAME_W,
					MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
		return;

	if (app_controller_remove_context_path(&g_app.controller, list, path))
		SaveSettings();
}

static void ShowContextAddResult(ContextMode list, const wchar_t *file_path, ContextAddResult result)
{
	if (result == CONTEXT_ADD_OK)
	{
		// Name the program too. "Added to the blacklist" never said what was
		// added, and the one-click entry takes whatever was in front a moment
		// ago - which may be explorer or a different window than the user meant.
		wchar_t added[CONTEXT_NAME_CHARS] = {0};
		context_file_name_from_path(file_path, added, ARRAYSIZE(added));

		wchar_t text[256];
		StringCchPrintfW(text, ARRAYSIZE(text), lang_str(STR_CONTEXT_ADDED_FMT), added,
						 lang_str(list == CONTEXT_MODE_WHITELIST ? STR_WHITELIST_NAME
																 : STR_BLACKLIST_NAME));
		MessageBoxW(UiDialogOwner(), text, APP_NAME_W, MB_OK | MB_ICONINFORMATION);
		return;
	}

	wchar_t name[CONTEXT_NAME_CHARS];
	context_file_name_from_path(file_path, name, ARRAYSIZE(name));

	if (result == CONTEXT_ADD_RESERVED)
	{
		wchar_t text[512];
		if (SUCCEEDED(StringCchPrintfW(text, ARRAYSIZE(text), lang_str(STR_CONTEXT_ERR_RESERVED), name)))
			MessageBoxW(UiDialogOwner(), text, APP_NAME_W, MB_OK | MB_ICONWARNING);
		return;
	}

	LangStringId message = STR_CONTEXT_ERR_INVALID;

	switch (result)
	{
	case CONTEXT_ADD_SELF:
		message = STR_CONTEXT_ERR_SELF;
		break;
	case CONTEXT_ADD_DUPLICATE:
		message = STR_CONTEXT_ERR_DUP;
		break;
	case CONTEXT_ADD_IN_OTHER:
		message = STR_CONTEXT_ERR_OTHER;
		break;
	case CONTEXT_ADD_FULL:
		message = STR_CONTEXT_ERR_FULL;
		break;
	default:
		break;
	}

	MessageBoxW(UiDialogOwner(), lang_str(message), APP_NAME_W, MB_OK | MB_ICONWARNING);
}

// Opens the system file dialog. Only reached from WM_APP_ADD_EXE_FILE, i.e.
// after the menu has closed and its WM_NULL has been dispatched.
static void PromptAddContextPath(ContextMode list)
{
	// Static: 2 KB of path plus the dialog structure, off the stack.
	static wchar_t file[CONTEXT_PATH_CHARS];
	file[0] = L'\0';

	OPENFILENAMEW ofn;
	memset(&ofn, 0, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = UiDialogOwner();
	ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All files (*.*)\0*.*\0";
	ofn.lpstrFile = file;
	ofn.nMaxFile = ARRAYSIZE(file);
	ofn.lpstrTitle = lang_str(STR_CONTEXT_ADD_TITLE);
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;

	if (!GetOpenFileNameW(&ofn))
		return;

	// Shell and UWP hosts are allowed on purpose: desktop chatter is a real
	// problem. The rule just covers far more than the name suggests, so ask.
	wchar_t name[CONTEXT_NAME_CHARS];
	context_file_name_from_path(file, name, ARRAYSIZE(name));

	if (context_program_is_shell(name))
	{
		wchar_t warning[512];
		if (SUCCEEDED(StringCchPrintfW(warning, ARRAYSIZE(warning), lang_str(STR_CONTEXT_WARN_SHELL), name)))
		{
			if (MessageBoxW(UiDialogOwner(), warning, APP_NAME_W,
							MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
				return;
		}
	}

	ContextAddResult result = app_controller_add_context_path(&g_app.controller, list, file);

	// Persist immediately. The controller's 400 ms debounce exists for slider
	// drags; losing a list edit to a crash inside that window is not a trade-off
	// worth making for a menu action. Adding and removing now behave the same.
	if (result == CONTEXT_ADD_OK)
		SaveSettings();

	ShowContextAddResult(list, file, result);
}

// Statistics were cleared: the balloon baseline has to follow, otherwise the next
// balloon would report every click since the application started.
static void ResetStatsSink(void *user)
{
	AppState *app = (AppState *)user;
	if (!app)
		return;

	app->notify_reported_blocks = 0;
	app->notify_last_tick = 0;
	app->notify_requested = false;
}

// A setting changed: refresh the tooltip once, not once per change
static void ObserverSink(void *user)
{
	AppState *app = (AppState *)user;
	if (!app || !app->hWnd || app->state_msg_pending)
		return;

	if (PostMessageW(app->hWnd, WM_APP_STATE_CHANGED, 0, 0))
		app->state_msg_pending = true;
}

// Reinstall a hook that Windows removed behind our back
static void RecoverHook(void)
{
	if (!g_app.hook_ready)
		return;

	mouse_hook_uninstall(&g_app.mouse_hook);

	// mouse_hook_uninstall clears the module level pointer, so the manager has to
	// be re-initialised before installing again - otherwise the callback would
	// never be reached and the hook would be a no-op.
	if (!mouse_hook_init(&g_app.mouse_hook, OnMouseHookCallback, &g_app) ||
		!mouse_hook_install(&g_app.mouse_hook))
	{
		g_app.hook_ready = false;
		UpdateTrayTooltip();

#ifndef NDEBUG
		LOG_ERROR(&g_app.logger, "Failed to reinstall the mouse hook");
#endif

		ShowErrorMessageBox(lang_str(STR_STATUS_UNAVAILABLE));
		return;
	}

	g_app.hook_recoveries++;
	UpdateTrayTooltip();

	// The hook can be dropped while the user works in an elevated window, where
	// protection stops and restarts with nothing on screen to say so. The balloon
	// below is once per session; this line is throttled by the notice slot itself
	// (identical text within 30 s is ignored), so a watchdog that trips
	// repeatedly does not blink.
	status_notice_post(&g_app.notice, lang_str(STR_SB_HOOK_RECOVERED), 8000);

	// The counter restarted with the new hook, so the health baseline has to be
	// taken again; otherwise the next check compares against the old hook's totals
	hook_watchdog_init(&g_app.watchdog);
	GetCursorPos(&g_app.watchdog_cursor);
	g_app.watchdog_buttons = PollMouseButtons();

#ifndef NDEBUG
	LOG_INFO(&g_app.logger, "Mouse hook reinstalled (recovery %u)", g_app.hook_recoveries);
#endif

	// Explain it once per session. A user who works in elevated windows will trip
	// the watchdog repeatedly (UIPI hides that input from us) and a balloon every
	// 30 seconds would be worse than the problem it reports.
	if (g_app.tray_ready && g_app.hook_recoveries == 1)
		tray_icon_show_balloon(&g_app.tray_icon, lang_str(STR_APP_NAME),
							   lang_str(STR_HOOK_RECOVERED), NIIF_WARNING);
}

// Append every queued history entry to today's log file
static void FlushLogQueue(void)
{
	g_app.log_flush_requested = false;

	if (!g_app.log_queue_ready)
		return;

	if (!g_app.prefs.log_blocks)
	{
		// Logging was switched off with entries still queued: discard them
		BlockLogEntry discarded;
		while (block_log_pop(&g_app.log_queue, &discarded))
		{
		}
		return;
	}

	// Resolved lazily so that no directory is created until the first real block
	if (g_app.log_directory[0] == L'\0')
	{
		if (!block_log_resolve_directory(g_app.log_directory, ARRAYSIZE(g_app.log_directory)))
		{
			if (!g_app.log_warning_shown && g_app.tray_ready)
			{
				tray_icon_show_balloon(&g_app.tray_icon, lang_str(STR_APP_NAME),
									   lang_str(STR_LOG_DIR_UNAVAILABLE), NIIF_WARNING);
				g_app.log_warning_shown = true;
			}

			BlockLogEntry dropped;
			while (block_log_pop(&g_app.log_queue, &dropped))
			{
			}
			return;
		}
	}

	// Bounded drain: the batch keeps the file open across all of it, and this
	// runs on the same thread as the mouse hook. Whatever is left is picked up by
	// the next state tick instead of blocking here for an unbounded time.
	BlockLogEntry batch[BLOCK_LOG_DRAIN_PER_TICK];
	int budget = BLOCK_LOG_DRAIN_PER_TICK;

	while (budget > 0)
	{
		int filled = 0;

		while (filled < budget && block_log_pop(&g_app.log_queue, &batch[filled]))
			filled++;

		if (filled == 0)
			break;

		size_t done = block_log_write_many(g_app.log_directory, &g_app.log_base_time,
										   batch, (size_t)filled);

		g_app.log_written += (uint32_t)done;

		if (done < (size_t)filled)
		{
			if (!g_app.log_warning_shown && g_app.tray_ready)
			{
				tray_icon_show_balloon(&g_app.tray_icon, lang_str(STR_APP_NAME),
									   lang_str(STR_LOG_DIR_UNAVAILABLE), NIIF_WARNING);
				g_app.log_warning_shown = true;
			}

			// The rest will fail the same way within this tick
			break;
		}

		budget -= filled;
	}

	if (block_log_count(&g_app.log_queue) > 0)
		g_app.log_flush_requested = true;
}

// Toggle the blocked-click history
static void ToggleLogBlocks(void)
{
	bool enable = !g_app.prefs.log_blocks;

	app_controller_set_log_blocks(&g_app.controller, enable);
	SaveSettings();

	if (!enable)
	{
		FlushLogQueue();
		BlockLogEntry discarded;
		while (block_log_pop(&g_app.log_queue, &discarded))
		{
		}
	}
	else if (g_app.log_directory[0] == L'\0')
	{
		// Nothing to do here on purpose: the folder is created by the first
		// blocked click, so enabling the option never litters an empty Log folder.
	}

#ifndef NDEBUG
	LOG_INFO(&g_app.logger, "Blocked-click history %s", enable ? "enabled" : "disabled");
#endif
}

// Open the history folder in Explorer
static void OpenLogFolder(void)
{
	if (g_app.log_directory[0] == L'\0')
	{
		if (!block_log_resolve_directory(g_app.log_directory, ARRAYSIZE(g_app.log_directory)))
		{
			ShowErrorMessageBox(lang_str(STR_LOG_DIR_UNAVAILABLE));
			return;
		}
	}

	ShellExecuteW(g_app.hWnd, L"open", g_app.log_directory, NULL, NULL, SW_SHOWNORMAL);
}

// Fill the read-only view used by the settings window
// The engine owned half lives in app_snapshot.c; this adapter only adds what
// the application layer knows: hook health, logging and the evidence ring.
static void BuildSnapshot(AppSnapshot *snapshot, void *user)
{
	AppState *app = (AppState *)user;

	if (!snapshot)
		return;

	app_snapshot_fill(snapshot, (app && app->debounce_ready) ? &app->debounce : NULL);

	if (!app || !app->debounce_ready)
		return;

	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		snapshot->evidence[i] = app->evidence[i];
		snapshot->evidence[i].blocks = snapshot->blocks[i];
	}

	snapshot->hook_events = app->hook_events;
	snapshot->hook_recoveries = app->hook_recoveries;
	snapshot->hook_active = IsHookActive();
	snapshot->logging = app->prefs.log_blocks;

	// What the filter is doing to the program in front. This is the one decision
	// that had no visible answer anywhere in the window: the strip on top says
	// whether the module is alive, not what it decided.
	//
	// The filter publishes into a double buffer - the writer fills the slot that
	// is not current and then flips the index - so reading slots[current] here
	// cannot observe a half written slot, and no lock is needed on this side.
	{
		const ContextSnapshot *context = &app->context_filter.snap;
		LONG current = InterlockedCompareExchange(&app->context_filter.snap.current, -1, -1);

		if (current < 0 || current > 1)
			current = 0;

		const ContextSlot *slot = &context->slots[current];

		snapshot->context_verdict = slot->ours ? (uint8_t)APP_CONTEXT_OURS
								   : slot->pid == 0 ? (uint8_t)APP_CONTEXT_NONE
								   : slot->suppressed ? (uint8_t)APP_CONTEXT_EXEMPT
													  : (uint8_t)APP_CONTEXT_BLOCKED;

		StringCchCopyW(snapshot->context_name, ARRAYSIZE(snapshot->context_name),
					   slot->name);
	}

	// Self-learning is opt in and its advice only ever reached the user through
	// the tray menu: someone who never opens that menu never learns there is
	// anything to adopt. Announce a change in the count instead - once per
	// change, not once per refresh - through the same slot as the other notices.
	{
		LearningAdvice advice;
		uint32_t count = 0;

		learning_service_snapshot(&app->learning, &advice);
		count = learning_advice_visible_count(&advice);

		if (count > 0 && count > app->advice_notice_count)
		{
			wchar_t text[STATUS_NOTICE_CHARS];

			StringCchPrintfW(text, ARRAYSIZE(text), lang_str(STR_SB_ADVICE_FMT), count);
			status_notice_post(&app->notice, text, 10000);
			app->advice_notice_count = count;
		}
		else
		{
			// Fewer suggestions than before means the user adopted one, or the
			// threshold moved. Both are consequences of something the user just
			// did; announcing "1 left" right after they clicked adopt sounds like
			// the click did not work. Record the new count silently so a later
			// increase is still announced.
			app->advice_notice_count = count;
		}
	}

	// Pause state, so the verdict block can say "paused" instead of claiming the
	// mouse is protected while filtering is off.
	{
		uint64_t now = GetTickCount64();
		LONG64 until = InterlockedCompareExchange64(&app->context_filter.snap.pause_until, 0, 0);

		snapshot->context_paused = context_filter_is_paused(&app->context_filter, now);
		snapshot->context_pause_left_ms = 0;

		if (snapshot->context_paused && until > (LONG64)now)
		{
			LONG64 left = until - (LONG64)now;

			// A pause is capped at 720 minutes; anything larger is a stale or
			// uninitialised value and must not become a six-figure countdown.
			if (left <= 24ll * 60ll * 60ll * 1000ll)
				snapshot->context_pause_left_ms = (uint32_t)left;
		}
	}

	// Status bar message, if one is current. status_notice_current also drops an
	// expired one, so the 400 ms refresh is what makes the text disappear - no
	// second timer anywhere.
	status_notice_current(&app->notice, snapshot->notice_text,
						  ARRAYSIZE(snapshot->notice_text));

	// Capability line: what is switched on, and whether it is actually working.
	// This adapter is the only place that sees every module at once, so it is
	// the only place that can answer "which part broke" instead of "protection
	// is on" while a module sits dead behind it.
	{
		// Filled by the same helper the support bundle uses, so the window and the
		// report can never disagree about which module is broken.
		CapabilityInputs caps;

		FillCapabilityInputs(&caps);

		CapabilityState states[CAP_COUNT];
		app_capabilities_evaluate(&caps, states);

		for (int i = 0; i < CAP_COUNT; i++)
			snapshot->capabilities[i] = (uint8_t)states[i];

		snapshot->unhandled_menu_ids =
			(uint32_t)InterlockedCompareExchange(&app->unhandled_menu_ids, 0, 0);
	}

	StringCchCopyW(snapshot->log_directory, ARRAYSIZE(snapshot->log_directory), app->log_directory);
}

// Toggle enable/disable all buttons
static void ToggleEnableAll(void)
{
	bool enable = !debounce_is_any_monitored(&g_app.debounce);

	app_controller_set_all_monitored(&g_app.controller, enable);
	SaveSettings();
	UpdateTrayTooltip();

#ifndef NDEBUG
	LOG_INFO(&g_app.logger, "All buttons %s", enable ? "enabled" : "disabled");
#endif
}

// Toggle individual button
static void ToggleButton(MouseButton button)
{
	if (button < 0 || button >= MOUSE_BUTTON_COUNT)
		return;

	bool current_state = g_app.debounce.buttons[button].isMonitored;
	app_controller_set_monitored(&g_app.controller, button, !current_state);

	SaveSettings();
	UpdateTrayTooltip();

#ifndef NDEBUG
	LOG_INFO(&g_app.logger, "Button %d %s", (int)button, !current_state ? "enabled" : "disabled");
#endif
}

// Toggle hybrid heuristic (Smart Drag Protection)
static void ToggleHybridHeuristic(void)
{
	bool new_state = !g_app.debounce.use_hybrid_heuristic;

	if (!new_state)
	{
		// Deliver any pending release before the deferral bookkeeping is dropped,
		// otherwise the button would stay logically pressed.
		debounce_flush_deferred_releases(&g_app.debounce);
	}

	app_controller_set_hybrid(&g_app.controller, new_state);
	SaveSettings();

	if (!new_state && g_app.deferred_timer_armed)
	{
		KillTimer(g_app.hWnd, TIMER_DEFERRED_RELEASE);
		g_app.deferred_timer_armed = false;
	}

#ifndef NDEBUG
	LOG_INFO(&g_app.logger, "Hybrid heuristic set to %s", new_state ? "Enabled" : "Disabled");
#endif
}

// Toggle "start with Windows"
static void ToggleAutostart(void)
{
	bool enable = !autostart_is_enabled();
	LONG result = autostart_set_enabled(enable);

	if (result != ERROR_SUCCESS)
	{
		ShowErrorMessageBox(lang_str(STR_ERR_AUTOSTART_FMT), (unsigned long)result);
		return;
	}

#ifndef NDEBUG
	LOG_INFO(&g_app.logger, "Autostart %s", enable ? "enabled" : "disabled");
#endif
}

// Apply preset configuration
// Parameters:
//   preset - Preset to apply
//   save - Whether to persist the new thresholds immediately
static void ApplyPreset(const PresetConfig *preset, bool save)
{
	if (!preset)
		return;

	app_controller_apply_preset(&g_app.controller, preset);

	if (save)
		SaveSettings();

#ifndef NDEBUG
	LOG_INFO(&g_app.logger, "Applied preset: L=%dms, R=%dms, M=%dms, X1=%dms, X2=%dms, W=%dms, HW=%dms",
			 preset->thresholds[MOUSE_BUTTON_LEFT], preset->thresholds[MOUSE_BUTTON_RIGHT],
			 preset->thresholds[MOUSE_BUTTON_MIDDLE], preset->thresholds[MOUSE_BUTTON_X1],
			 preset->thresholds[MOUSE_BUTTON_X2], preset->thresholds[MOUSE_BUTTON_WHEEL],
			 preset->thresholds[MOUSE_BUTTON_HWHEEL]);
#endif
}

// Restore every setting to its default value
static void RestoreDefaults(void)
{
	if (MessageBoxW(UiDialogOwner(), lang_str(STR_CONFIRM_RESTORE), lang_str(STR_APP_NAME),
					MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
		return;

	app_controller_apply_preset(&g_app.controller, &CONFIG_PRESET_DEFAULT);
	app_controller_set_all_monitored(&g_app.controller, true);
	app_controller_set_hybrid(&g_app.controller, true);
	app_controller_set_release_delay(&g_app.controller, RELEASE_DELAY_DEFAULT_MS);
	app_controller_set_drag_distance(&g_app.controller, DRAG_DISTANCE_DEFAULT_PX);

	// Both history and notifications are opt-in, so defaults switch them off
	app_controller_set_notify_blocks(&g_app.controller, false);
	app_controller_set_log_blocks(&g_app.controller, false);
	app_controller_set_learning(&g_app.controller, false);
	app_controller_reset_context(&g_app.controller);
	context_filter_set_pause(&g_app.context_filter, 0, 0);
	learning_service_stop(&g_app.learning);
	signal_ring_init(&g_app.signals);

	g_app.notify_requested = false;
	g_app.notify_last_tick = 0;
	g_app.notify_reported_blocks = 0;
	g_app.log_flush_requested = false;
	g_app.log_directory[0] = L'\0';
	g_app.log_warning_shown = false;

	// Drop anything that was queued for the log
	if (g_app.log_queue_ready)
	{
		BlockLogEntry discarded;
		while (block_log_pop(&g_app.log_queue, &discarded))
		{
		}
	}

	SaveSettings();
	UpdateTrayTooltip();

#ifndef NDEBUG
	LOG_INFO(&g_app.logger, "All settings restored to defaults");
#endif
}

// Toggle the "notify when a click is blocked" preference
static void ToggleNotifyBlocks(void)
{
	bool enable = !g_app.prefs.notify_blocks;
	app_controller_set_notify_blocks(&g_app.controller, enable);

	// Baseline the counter so the first balloon reports new blocks only, and
	// let the next block through immediately instead of waiting out the throttle.
	g_app.notify_reported_blocks = NotifyButtonBlocks();
	g_app.notify_last_tick = 0;
	g_app.notify_requested = false;

	SaveSettings();

	// Turning it on doubles as a preview: the user immediately sees what the
	// balloon looks like, which version they are running and what it does.
	if (g_app.prefs.notify_blocks && g_app.tray_ready)
	{
		wchar_t title[64];
		StringCchPrintfW(title, ARRAYSIZE(title), lang_str(STR_NOTIFY_ON_TITLE_FMT), APP_VERSION_STRING_W);

		wchar_t text[TRAY_BALLOON_MAX_CHARS];
		StringCchPrintfW(text, ARRAYSIZE(text), lang_str(STR_NOTIFY_ON_TEXT_FMT), NOTIFY_MIN_INTERVAL_MS / 1000);

		tray_icon_show_balloon(&g_app.tray_icon, title, text, NIIF_INFO);
	}

#ifndef NDEBUG
	LOG_INFO(&g_app.logger, "Blocked-click notification %s", g_app.prefs.notify_blocks ? "enabled" : "disabled");
#endif
}

// Set threshold for a specific button
static void SetButtonThreshold(MouseButton button, int threshold_ms)
{
	if (button < 0 || button >= MOUSE_BUTTON_COUNT)
		return;

	app_controller_set_threshold(&g_app.controller, button, (uint32_t)threshold_ms);

	SaveSettings();

#ifndef NDEBUG
	LOG_INFO(&g_app.logger, "Button %d threshold set to %dms", (int)button, threshold_ms);
#endif
}

// Ask the user for a custom threshold
static void PromptCustomThreshold(MouseButton button)
{
	if (button < 0 || button >= MOUSE_BUTTON_COUNT)
		return;

	// Pre-fill with the current value so the user can see and tweak it
	wchar_t input[INPUT_BOX_INPUT_BUFFER_SIZE];
	StringCchPrintfW(input, ARRAYSIZE(input), L"%u", (unsigned)g_app.debounce.buttons[button].thresholdMs);

	wchar_t prompt[INPUT_BOX_PROMPT_BUFFER_SIZE];
	StringCchPrintfW(prompt, ARRAYSIZE(prompt), lang_str(STR_THRESHOLD_PROMPT_FMT),
					 lang_button_name(button), THRESHOLD_MIN_VALUE, THRESHOLD_MAX_VALUE);

	int threshold = 0;
	if (!input_box_show(g_app.hInstance, lang_str(STR_SET_THRESHOLD_TITLE), prompt,
						input, ARRAYSIZE(input), THRESHOLD_MIN_VALUE, THRESHOLD_MAX_VALUE, &threshold))
		return;

	// The dialog already validated the value, never parse the text a second time
	SetButtonThreshold(button, threshold);
}

// Change the user interface language
static void SetLanguage(Language language)
{
	if (language_get() == language)
		return;

	app_controller_set_language(&g_app.controller, language);
	SaveSettings();
	UpdateTrayTooltip();

#ifndef NDEBUG
	LOG_INFO(&g_app.logger, "Language set to %d", (int)language);
#endif
}

// Register invisible window class
static bool RegisterInvisibleClass(const HINSTANCE hInstance)
{
	WNDCLASSEX wc = {0};
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.lpfnWndProc = (WNDPROC)WindowProc;
	wc.hInstance = hInstance;
	wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_MAINICON));
	wc.lpszClassName = WINDOW_CLASS_NAME;

	return RegisterClassExW(&wc) != 0;
}

// Window procedure
// ---------------------------------------------------------------- commands ---
//
// WM_COMMAND used to hold every branch inline: app toggles, per-button
// thresholds, program lists and learning shared one 305-line chain, so adding a
// feature meant editing code that belonged to three unrelated ones. The four
// handlers below give each area its own function, which bounds the blast radius
// of any future change to one of them.
//
// They still live in this file on purpose: they drive AppState directly, and
// SaveSettings / ShowContextAddResult / the Toggle* helpers are all local. The
// AppState parameter is not decoration - a later move to another file becomes a
// mechanical rename instead of a rewrite.

static bool HandleMenuCommand(AppState *app, HWND hWnd, UINT menu_id)
{
	if (menu_id == IDM_ABOUT)
	{
		about_window_show(UiDialogOwner(), app->hInstance);
		return true;
	}
	if (menu_id == IDM_EXIT)
	{
		PostMessage(hWnd, WM_CLOSE, 0, 0);
		return true;
	}
	if (menu_id == IDM_TOGGLE_ENABLE)
	{
		ToggleEnableAll();
		return true;
	}
	if (menu_id == IDM_TOGGLE_HYBRID)
	{
		ToggleHybridHeuristic();
		return true;
	}
	if (menu_id == IDM_TOGGLE_AUTOSTART)
	{
		ToggleAutostart();
		return true;
	}
	if (menu_id == IDM_TOGGLE_NOTIFY)
	{
		ToggleNotifyBlocks();
		return true;
	}
	if (menu_id == IDM_TOGGLE_LOG)
	{
		ToggleLogBlocks();
		return true;
	}
	if (menu_id == IDM_OPEN_LOG_FOLDER)
	{
		OpenLogFolder();
		return true;
	}
	if (menu_id == IDM_EXPORT_SUPPORT)
	{
		// No dump: this path exists to be usable *before* anything goes wrong,
		// and a minidump of a healthy process would only add noise.
		ExportSupportBundle();
		return true;
	}
	if (menu_id == IDM_OPEN_SETTINGS)
	{
		OpenCheckupWindow();
		return true;
	}
	if (menu_id == IDM_RESET_STATS)
	{
		// The controller also resets the notification baseline through its sink
		app_controller_reset_statistics(&app->controller);

#ifndef NDEBUG
		LOG_INFO(&app->logger, "Statistics reset");
#endif
		return true;
	}
	if (menu_id == IDM_PRESET_DEFAULT)
	{
		ApplyPreset(&CONFIG_PRESET_DEFAULT, true);
		return true;
	}
	if (menu_id == IDM_PRESET_OFFICE)
	{
		ApplyPreset(&CONFIG_PRESET_OFFICE, true);
		return true;
	}
	if (menu_id == IDM_PRESET_STRICT)
	{
		ApplyPreset(&CONFIG_PRESET_STRICT, true);
		return true;
	}
	if (menu_id == IDM_RESTORE_DEFAULTS)
	{
		RestoreDefaults();
		return true;
	}
	if (menu_id == IDM_LANG_EN)
	{
		SetLanguage(LANG_EN);
		return true;
	}
	if (menu_id == IDM_LANG_ZH)
	{
		SetLanguage(LANG_ZH_CN);
		return true;
	}

	return false;
}

static bool HandleThresholdCommand(AppState *app, UINT menu_id)
{
	// Smart Drag Protection tuning
	//
	// Through the controller, not the manager: it is the only writer of the
	// configuration, and going around it skipped the range clamping and the
	// dirty flag (the commit below would then persist nothing).
	if (menu_id >= (UINT)IDM_RELEASE_DELAY_BASE &&
		menu_id < (UINT)IDM_RELEASE_DELAY_BASE + RELEASE_DELAY_PRESET_COUNT)
	{
		int value = config_release_delay_presets()[menu_id - (UINT)IDM_RELEASE_DELAY_BASE];
		app_controller_set_release_delay(&app->controller, (uint32_t)value);
		SaveSettings();
		return true;
	}
	if (menu_id >= (UINT)IDM_DRAG_DISTANCE_BASE &&
		menu_id < (UINT)IDM_DRAG_DISTANCE_BASE + DRAG_DISTANCE_PRESET_COUNT)
	{
		int value = config_drag_distance_presets()[menu_id - (UINT)IDM_DRAG_DISTANCE_BASE];
		app_controller_set_drag_distance(&app->controller, (uint32_t)value);
		SaveSettings();
		return true;
	}

	// Individual button toggles: IDM_TOGGLE_BASE + MouseButton
	if (menu_id >= (UINT)IDM_TOGGLE_BASE &&
		menu_id < (UINT)IDM_TOGGLE_BASE + (UINT)MOUSE_BUTTON_COUNT)
	{
		ToggleButton((MouseButton)(menu_id - (UINT)IDM_TOGGLE_BASE));
		return true;
	}

	// Threshold presets and custom thresholds
	{
		int button = 0;
		int index = 0;

		if (menu_id_is_threshold_preset(menu_id, &button, &index))
		{
			if (button < MOUSE_BUTTON_COUNT && index < THRESHOLD_PRESET_COUNT)
			{
				const int *presets = config_threshold_presets((MouseButton)button);
				SetButtonThreshold((MouseButton)button, presets[index]);
			}
			return true;
		}

		if (menu_id_is_threshold_custom(menu_id, &button, MOUSE_BUTTON_COUNT))
		{
			PromptCustomThreshold((MouseButton)button);
			return true;
		}
	}

	return false;
}

static bool HandleListCommand(AppState *app, HWND hWnd, UINT menu_id)
{
	// Pause / resume. IDM_PAUSE_CONTEXT is the submenu handle and no longer
	// arrives as a command; the branch is kept so that route still lifts a pause
	// instead of doing nothing if anything ever posts the old id.
	if (menu_id == IDM_PAUSE_CONTEXT || menu_id == IDM_PAUSE_RESUME)
	{
		context_filter_set_pause(&app->context_filter, 0, 0);
		return true;
	}

	if (menu_id == IDM_PAUSE_5 || menu_id == IDM_PAUSE_10 ||
		menu_id == IDM_PAUSE_30 || menu_id == IDM_PAUSE_CUSTOM)
	{
		uint32_t minutes = 5;

		if (menu_id == IDM_PAUSE_10)
			minutes = 10;
		else if (menu_id == IDM_PAUSE_30)
			minutes = 30;
		else if (menu_id == IDM_PAUSE_CUSTOM)
		{
			wchar_t input[16];
			wchar_t prompt[160];
			int value = 0;

			// Pre-filled, so the common case is one click on OK. The dialog
			// rejects anything outside the range on its own - parsing the text
			// here a second time would be a second place to get it wrong.
			StringCchPrintfW(input, ARRAYSIZE(input), L"%d", CONTEXT_PAUSE_MAX_MINUTES / 24);
			StringCchPrintfW(prompt, ARRAYSIZE(prompt), lang_str(STR_PAUSE_PROMPT_FMT),
							 CONTEXT_PAUSE_MIN_MINUTES, CONTEXT_PAUSE_MAX_MINUTES);

			if (!input_box_show(g_app.hInstance, lang_str(STR_PAUSE_CUSTOM), prompt,
								input, ARRAYSIZE(input), CONTEXT_PAUSE_MIN_MINUTES,
								CONTEXT_PAUSE_MAX_MINUTES, &value))
				return true;

			minutes = (uint32_t)value;
		}

		context_filter_set_pause(&app->context_filter, GetTickCount64(),
								 minutes * 60u * 1000u);
		return true;
	}

	// Context mode: three mutually exclusive choices.
	if (menu_id == IDM_CONTEXT_MODE_ALL_WHITE || menu_id == IDM_CONTEXT_MODE_ALL_BLACK)
	{
		app_controller_set_context_mode(&app->controller, CONTEXT_MODE_ALL);
		SaveSettings();
		return true;
	}
	if (menu_id == IDM_CONTEXT_MODE_WHITELIST)
	{
		// An empty whitelist in whitelist mode means "no program is filtered" -
		// switching into it is a silent way to turn protection off for
		// everything. The submenu says so in small print, but the switch itself
		// has to ask, or the user clicks once and loses filtering everywhere.
		static ContextPolicy policy_snapshot;
		context_filter_copy_policy(&app->context_filter, &policy_snapshot);

		if (policy_snapshot.whitelist.count == 0)
		{
			if (MessageBoxW(UiDialogOwner(), lang_str(STR_CONFIRM_EMPTY_WHITELIST),
							lang_str(STR_APP_NAME),
							MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
				return true;
		}

		app_controller_set_context_mode(&app->controller, CONTEXT_MODE_WHITELIST);
		SaveSettings();
		return true;
	}
	if (menu_id == IDM_CONTEXT_MODE_BLACKLIST)
	{
		app_controller_set_context_mode(&app->controller, CONTEXT_MODE_BLACKLIST);
		SaveSettings();
		return true;
	}

	// Adding opens the file dialog, but only after the menu has closed.
	if (menu_id == IDM_CONTEXT_ADD_WHITE || menu_id == IDM_CONTEXT_ADD_BLACK)
	{
		ContextMode list = (menu_id == IDM_CONTEXT_ADD_WHITE) ? CONTEXT_MODE_WHITELIST
															  : CONTEXT_MODE_BLACKLIST;
		PostMessageW(hWnd, WM_APP_ADD_EXE, (WPARAM)list, 0);
		return true;
	}

	// One-click rule for the program that was in front. Adding it while "all
	// programs" is in force would configure something that does nothing, so the
	// mode follows the click: "don't filter this" is only meaningful when the
	// blacklist is the list in force.
	if (menu_id == IDM_EXCLUDE_CURRENT || menu_id == IDM_ONLY_CURRENT)
	{
		ContextMode list = (menu_id == IDM_ONLY_CURRENT) ? CONTEXT_MODE_WHITELIST
														 : CONTEXT_MODE_BLACKLIST;

		// Static: a policy copy is ~65 KB.
		static ContextPolicy snapshot;
		context_filter_copy_policy(&app->context_filter, &snapshot);

		// Claimed even when there is nothing to add: falling through would send
		// this id to DefWindowProc, which is not what "no foreground program"
		// means.
		if (!snapshot.recent_path[0])
			return true;

		ContextAddResult result = context_filter_add_path(&app->context_filter, list,
														  snapshot.recent_path);

		if (result == CONTEXT_ADD_OK)
		{
			if (snapshot.mode == CONTEXT_MODE_ALL)
				app_controller_set_context_mode(&app->controller, list);

			SaveSettings();
		}

		ShowContextAddResult(list, snapshot.recent_path, result);
		return true;
	}

	if (menu_id >= (UINT)IDM_CONTEXT_LIST_WHITE &&
		menu_id < (UINT)IDM_CONTEXT_LIST_WHITE + IDM_CONTEXT_LIST_MAX)
	{
		RemoveContextPath(CONTEXT_MODE_WHITELIST, menu_id - (UINT)IDM_CONTEXT_LIST_WHITE);
		return true;
	}
	if (menu_id >= (UINT)IDM_CONTEXT_LIST_BLACK &&
		menu_id < (UINT)IDM_CONTEXT_LIST_BLACK + IDM_CONTEXT_LIST_MAX)
	{
		RemoveContextPath(CONTEXT_MODE_BLACKLIST, menu_id - (UINT)IDM_CONTEXT_LIST_BLACK);
		return true;
	}

	return false;
}

static bool HandleLearningCommand(AppState *app, UINT menu_id)
{
	if (menu_id == IDM_TOGGLE_LEARNING)
	{
		bool enable = !app->prefs.learning_enabled;
		app_controller_set_learning(&app->controller, enable);

		if (enable)
		{
			signal_ring_init(&app->signals);
			if (!learning_service_start(&app->learning, &app->signals, LearningThresholdFor, app))
			{
				app_controller_set_learning(&app->controller, false);
				ShowErrorMessageBox(lang_str(STR_SETTINGS_UNAVAILABLE));
				return true;
			}
		}
		else
		{
			learning_service_stop(&app->learning);
			signal_ring_init(&app->signals);
		}

		SaveSettings();
		return true;
	}
	if (menu_id == IDM_ADOPT_LEARNING)
	{
		LearningAdvice advice;
		learning_service_snapshot(&app->learning, &advice);

		wchar_t applied[160];
		applied[0] = L'\0';
		bool any = false;

		// Only the buttons the menu actually shows. X1/X2 are sampled like any
		// other button, so they can hold advice that is never rendered - adopting
		// it would move a side button with no visible cause, and when only a side
		// button has advice the entry looks greyed out while the data is there.
		// Sharing this list with the menu keeps "visible" and "adoptable" the
		// same set.
		static const MouseButton shown[] = {MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT,
											MOUSE_BUTTON_MIDDLE};

		for (int k = 0; k < (int)ARRAYSIZE(shown); k++)
		{
			int i = (int)shown[k];

			if (advice.suggested_ms[i] == 0)
				continue;

			// Re-apply the ±50% guard against the threshold in force NOW. The
			// cached advice is not recomputed when the user edits a threshold by
			// hand, so a stale "suggest 100" could otherwise be adopted on top of
			// a threshold since lowered to 5 - the guard meant to prevent exactly
			// that never ran.
			uint32_t current = app->debounce.buttons[i].thresholdMs;
			uint32_t value = advice.suggested_ms[i];
			uint32_t low = current / 2u;
			uint32_t high = current + current / 2u;

			if (low < THRESHOLD_MIN_VALUE)
				low = THRESHOLD_MIN_VALUE;
			if (high > THRESHOLD_MAX_VALUE)
				high = THRESHOLD_MAX_VALUE;

			if (value < low)
				value = low;
			if (value > high)
				value = high;

			if (value == current)
				continue;

			app_controller_set_threshold(&app->controller, (MouseButton)i, value);

			// Name the button: "applied: 40 ms" leaves the user guessing which
			// switch just moved, and the menu shows three numbers at once.
			wchar_t part[64];
			if (SUCCEEDED(StringCchPrintfW(part, ARRAYSIZE(part), L"%s%s %u ms",
										   any ? L", " : L"",
										   lang_button_short_name((MouseButton)i),
										   (unsigned)value)))
			{
				StringCchCatW(applied, ARRAYSIZE(applied), part);
				any = true;
			}
		}

		// The samples that produced these numbers are spent. Keeping them would
		// make the same advice reappear on the next gesture, so the menu would
		// look like the click did nothing.
		//
		// The ring goes first: the worker recomputes whenever the sample total
		// changes, so clearing the cached advice first would let it republish the
		// same advice from the still-full ring for one 100 ms tick - the "it came
		// back right after I adopted it" the user would see.
		signal_ring_init(&app->signals);
		learning_service_reset(&app->learning);
		SaveSettings();

		if (any && app->tray_ready)
		{
			wchar_t text[256];
			if (SUCCEEDED(StringCchPrintfW(text, ARRAYSIZE(text),
										   lang_str(STR_LEARNING_APPLIED_FMT), applied)))
				tray_icon_show_balloon(&app->tray_icon, lang_str(STR_APP_NAME), text,
									   NIIF_INFO);
		}

		return true;
	}

	return false;
}

static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_CREATE:
		g_app.hWnd = hWnd;
		if (!InitializeApp())
			return -1;

		return 0;

	case WM_TIMER:
		if (wParam == TIMER_DEFERRED_RELEASE && g_app.debounce_ready)
		{
			debounce_check_deferred_releases(&g_app.debounce);

			// Stop polling as soon as there is nothing left to release
			if (!debounce_has_pending_work(&g_app.debounce))
			{
				KillTimer(hWnd, TIMER_DEFERRED_RELEASE);
				g_app.deferred_timer_armed = false;

				// "Can defer" and "a timer is running" have to be one fact. Leaving
				// this true while the timer is gone is what let a release be held
				// with nothing left to deliver it: the engine deferred, the system
				// kept the button down, and the pointer dragged a window with it.
				debounce_set_defer_available(&g_app.debounce, false);
				trace_lifecycle(TRACE_LIFE_TIMER_STOP);
			}
		}
		else if (wParam == TIMER_WATCHDOG && g_app.hook_ready)
		{
			// Evidence that the mouse was used: the cursor moved, or a button
			// changed state (a click without movement is common on a touchpad, so
			// movement alone would leave a dead hook undetected indefinitely).
			POINT cursor;
			bool moved = false;

			if (GetCursorPos(&cursor))
			{
				moved = (cursor.x != g_app.watchdog_cursor.x || cursor.y != g_app.watchdog_cursor.y);
				g_app.watchdog_cursor = cursor;
			}

			uint32_t buttons = PollMouseButtons();
			bool button_changed = (buttons != g_app.watchdog_buttons);
			g_app.watchdog_buttons = buttons;

			WatchdogVerdict verdict = hook_watchdog_check(&g_app.watchdog, moved, button_changed,
														 mouse_hook_event_count(&g_app.mouse_hook),
														 GetTickCount64(),
														 WATCHDOG_RECOVER_COOLDOWN_MS);

			if (verdict == WATCHDOG_RECOVER)
				RecoverHook();
		}
		else if (wParam == TIMER_STATE)
		{
			// Silence here is what makes the hang watchdog write a report. This
			// timer is the cheapest honest heartbeat there is: it only runs when
			// the message loop does, which is exactly the thing that stops when
			// the program hangs.
			hang_watchdog_beat();

			// A release that never reached the system is invisible otherwise: the
			// user sees a drag that ended late, and the report used to look
			// perfectly healthy. Say it once, and only once, per run.
			if (!g_app.injection_notice_shown && g_app.tray_ready &&
				debounce_take_injection_failure())
			{
				g_app.injection_notice_shown = true;
				tray_icon_show_balloon(&g_app.tray_icon, APP_NAME_W,
									   lang_str(STR_INJECTION_FAILED), NIIF_WARNING);
			}

			// Write the settings 400 ms after the last change, so dragging a
			// slider does not turn into hundreds of registry writes
			if (g_app.debounce_ready)
				app_controller_tick(&g_app.controller, GetTickCount64());

			// Work deferred out of the hook callback: both items below would have
			// blocked it (disk IO, cross-process shell call)
			if (g_app.log_flush_requested)
				FlushLogQueue();

			if (g_app.notify_requested)
			{
				g_app.notify_requested = false;
				NotifyBlockedClick();
			}
		}
		return 0;

	case WM_APP_STATE_CHANGED:
		g_app.state_msg_pending = false;
		UpdateTrayTooltip();
		return 0;

	case WM_NOTIFYICON:
	{
		UINT notification = LOWORD(lParam);

		// Right click is the menu, and only the menu. Opening a menu on a left
		// click is what a right-click menu is for, and it made the icon feel
		// backwards: the window is the main window, the menu is an accessory.
		if (notification == WM_CONTEXTMENU)
		{
			ShowContextMenu(hWnd, GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
			return 0;
		}

		// A left click does nothing on its own, like every tray icon that has a
		// window: only the second click of a double click opens it. The shell
		// reports both clicks as NIN_SELECT, so the pair is told apart by the gap
		// against the system double click time.
		if (notification == NIN_SELECT)
		{
			uint64_t now = GetTickCount64();
			uint64_t previous = g_app.tray_last_select_tick;
			g_app.tray_last_select_tick = now;

			if (previous != 0 && now >= previous &&
				(now - previous) <= (uint64_t)GetDoubleClickTime())
			{
				// Consumed: a third click starts a new pair instead of reopening
				g_app.tray_last_select_tick = 0;
				OpenCheckupWindow();
			}

			return 0;
		}

		// Keyboard activation has no second click to wait for
		if (notification == NIN_KEYSELECT)
		{
			OpenCheckupWindow();
			return 0;
		}

		// The shell may ask just before the hover text is shown; refresh it so a
		// hover never displays an outdated blocked count. Not all shells send it,
		// which is why the text is also refreshed after the menu closes.
		if (notification == NIN_POPUPOPEN)
		{
			UpdateTrayTooltip();
			return 0;
		}

		// The balloon says the window went to the tray, so clicking it brings the
		// window back. A menu would be a non sequitur here.
		if (notification == NIN_BALLOONUSERCLICK)
		{
			OpenCheckupWindow();
			return 0;
		}
		break;
	}

	// Two hops on purpose: the menu command only asks for this, and the dialog
	// opens after TrackPopupMenu has returned and its trailing WM_NULL has been
	// dispatched. Opening it directly swallows the next tray click.
	case WM_APP_ADD_EXE:
		PostMessageW(hWnd, WM_APP_ADD_EXE_FILE, wParam, 0);
		break;

	case WM_APP_ADD_EXE_FILE:
		PromptAddContextPath((ContextMode)wParam);
		break;

	case WM_COMMAND:
	{
		UINT menu_id = LOWORD(wParam);
		bool handled = false;

		switch (command_classify(menu_id))
		{
		case COMMAND_GROUP_MENU:
			handled = HandleMenuCommand(&g_app, hWnd, menu_id);
			break;
		case COMMAND_GROUP_THRESHOLD:
			handled = HandleThresholdCommand(&g_app, menu_id);
			break;
		case COMMAND_GROUP_LIST:
			handled = HandleListCommand(&g_app, hWnd, menu_id);
			break;
		case COMMAND_GROUP_LEARNING:
			handled = HandleLearningCommand(&g_app, menu_id);
			break;
		default:
			// Id 0 is the inert label row used throughout the menus; anything
			// else is a gap worth counting rather than ignoring. Counted in
			// release builds too: "this entry does nothing" is a user report,
			// and a counter is the only thing that can confirm it happened.
			if (menu_id != 0)
				InterlockedIncrement(&g_app.unhandled_menu_ids);
			break;
		}

		(void)handled;
		break;
	}

	case WM_CLOSE:
		// The About dialog is modal and owns its own message loop, so a quit
		// requested from the tray icon while it is up would otherwise leave the
		// process waiting for a click nobody is going to make.
		about_window_close();
		help_window_close();
		DestroyWindow(hWnd);
		break;

	case WM_DESTROY:
		ShutdownApp();
		PostQuitMessage(0);
		return 0;

	default:
		// A second instance asks us to show the menu at the current cursor position
		if (g_app.show_menu_msg && uMsg == g_app.show_menu_msg)
		{
			POINT cursor;
			GetCursorPos(&cursor);
			ShowContextMenu(hWnd, cursor.x, cursor.y);
			return 0;
		}

		if (g_app.tray_icon.taskbar_created_msg && uMsg == g_app.tray_icon.taskbar_created_msg)
		{
			tray_icon_restore(&g_app.tray_icon);
			UpdateTrayTooltip();
			return 0;
		}
		break;
	}

	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

// Check whether the protection can actually act on mouse input.
// The installed flag alone is a lie: Windows silently removes a low-level hook
// whose callback overran its time budget. The watchdog notices that from real
// mouse input, so trust it before telling the user the protection is working.
static bool IsHookActive(void)
{
	return g_app.hook_ready && g_app.mouse_hook.installed &&
		   hook_watchdog_is_healthy(&g_app.watchdog);
}

// Show context menu
static void ShowContextMenu(const HWND hWnd, const int x, const int y)
{
	// Static: the policy snapshot is ~65 KB.
	static ContextPolicy policy;
	context_filter_copy_policy(&g_app.context_filter, &policy);

	LearningAdvice advice;
	learning_service_snapshot(&g_app.learning, &advice);

	context_menu_show(&g_app.context_menu, hWnd, x, y, &g_app.debounce, &g_app.prefs,
					  IsHookActive(), &advice, &policy,
					  context_filter_is_paused(&g_app.context_filter, GetTickCount64()));

	// The blocked counter may have been reset from the menu; refresh the hover
	// text while we are already on the UI thread.
	UpdateTrayTooltip();
}

// Update the tray tooltip so a hover answers the three questions a user has:
// what is this, is it working, and has it done anything yet.
static void UpdateTrayTooltip(void)
{
	wchar_t tooltip[TRAY_TOOLTIP_MAX_CHARS];

	lang_format_tooltip(tooltip, ARRAYSIZE(tooltip),
						debounce_count_monitored(&g_app.debounce),
						MOUSE_BUTTON_COUNT,
						debounce_get_total_blocks(&g_app.debounce),
						IsHookActive());

	tray_icon_set_tooltip(&g_app.tray_icon, tooltip);
}

// Show error message box
static void CDECL ShowErrorMessageBox(LPCWSTR message, ...)
{
	WCHAR buffer[INPUT_BOX_ERROR_BUFFER_SIZE];
	va_list args;
	va_start(args, message);
	StringCchVPrintfW(buffer, ARRAYSIZE(buffer), message, args);
	va_end(args);

	// NULL only while starting up, before the tray window exists; the helper
	// degrades to NULL on its own in that case.
	MessageBoxW(UiDialogOwner(), buffer, lang_str(STR_APP_NAME), MB_OK | MB_ICONERROR);
}

// Check whether a whitespace separated command line switch is present.
// Quoted tokens are supported so a path argument cannot be mistaken for a switch.
static bool CommandLineHasSwitch(LPCWSTR command_line, LPCWSTR switch_name)
{
	if (!command_line || !switch_name)
		return false;

	size_t switch_length = wcslen(switch_name);
	const wchar_t *cursor = command_line;

	while (*cursor)
	{
		while (*cursor == L' ' || *cursor == L'\t')
			cursor++;

		if (!*cursor)
			break;

		bool quoted = (*cursor == L'"');
		if (quoted)
			cursor++;

		const wchar_t *token = cursor;
		while (*cursor && ((quoted && *cursor != L'"') || (!quoted && *cursor != L' ' && *cursor != L'\t')))
			cursor++;

		size_t token_length = (size_t)(cursor - token);
		if (token_length == switch_length && _wcsnicmp(token, switch_name, switch_length) == 0)
			return true;

		if (*cursor)
			cursor++;
	}

	return false;
}

// Entry point
int CALLBACK wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ PWSTR lpCmdLine, _In_ int nCmdShow)
{
	// Start with the language of the system, saved settings override it below
	language_set(language_detect_system());

	// Read-only switches for support and deployment. They never touch the running
	// instance, the profile or the hook, so they are handled before the mutex.
	if (CommandLineHasSwitch(lpCmdLine, L"--version"))
		return diagnostics_print_version();

	if (CommandLineHasSwitch(lpCmdLine, L"--diagnose"))
		return diagnostics_run();

	// Trace build only. The switch is parsed in every build, but the tracing it
	// turns on compiles to nothing unless MOUSEMEND_TRACE was defined.
	bool trace_requested = CommandLineHasSwitch(lpCmdLine, L"--trace");

	// A dump is the only way to turn a user's "it closed itself" into a stack.
	// Installed before anything can fail, and it changes nothing the user sees.
	// The collector is registered first, so even a crash during start-up produces
	// a report that names the version and the settings.
	crash_dump_set_report_collector(CollectSupportReport);
	crash_dump_install();

	// The check-up window uses a tooltip, which comes from the version 6 common
	// controls the manifest already asks for. Without this call the class is not
	// registered and every hover tip silently fails to appear.
	INITCOMMONCONTROLSEX controls = {0};
	controls.dwSize = sizeof(controls);
	controls.dwICC = ICC_WIN95_CLASSES;
	InitCommonControlsEx(&controls);

	// Registered message used by a second instance to open the menu
	g_app.show_menu_msg = RegisterWindowMessageW(APP_SHOW_MENU_MSG_NAME);

	// Limit to one instance
	g_app.mutex = CreateMutexW(NULL, TRUE, MUTEX_GUID);
	if (!g_app.mutex)
	{
		DWORD error = GetLastError();
		ShowErrorMessageBox(lang_str(STR_ERR_MUTEX_FMT), (unsigned long)error);
		return EXIT_FAILURE;
	}

	DWORD mutex_error = GetLastError();
	if (mutex_error == ERROR_ALREADY_EXISTS || mutex_error == ERROR_ACCESS_DENIED)
	{
		// Ask the running instance to show its menu instead of only complaining
		HWND existing_window = FindWindowW(WINDOW_CLASS_NAME, NULL);

		if (existing_window && g_app.show_menu_msg)
			PostMessageW(existing_window, g_app.show_menu_msg, 0, 0);
		else
			ShowErrorMessageBox(lang_str(STR_ERR_ALREADY_RUNNING));

		CloseHandle(g_app.mutex);
		return EXIT_SUCCESS;
	}

	if (trace_requested && trace_start())
		trace_lifecycle(TRACE_LIFE_APP_START);

	g_app.hInstance = hInstance;

	// Register window class
	if (!RegisterInvisibleClass(hInstance))
	{
		DWORD error = GetLastError();
		ShowErrorMessageBox(lang_str(STR_ERR_WINDOW_CLASS_FMT), (unsigned long)error);
		ReleaseMutex(g_app.mutex);
		CloseHandle(g_app.mutex);
		return EXIT_FAILURE;
	}

	// Create window
	HWND hWnd = CreateWindowW(WINDOW_CLASS_NAME, APP_NAME_W, 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
	if (!hWnd)
	{
		DWORD error = GetLastError();

		// WM_CREATE may already have installed the hook and created the tray icon;
		// tear everything down before showing a modal error box.
		ShutdownApp();

		ShowErrorMessageBox(lang_str(STR_ERR_CREATE_WINDOW_FMT), (unsigned long)error);

		UnregisterClassW(WINDOW_CLASS_NAME, hInstance);
		ReleaseMutex(g_app.mutex);
		CloseHandle(g_app.mutex);
		return EXIT_FAILURE;
	}

	// The window the user sees first. It waits until here so that the owner is a
	// finished window and the message loop can dispatch its timers.
	ShowStartupWindow();

	// Message loop
	MSG msg;
	BOOL ret;
	while ((ret = GetMessage(&msg, NULL, 0, 0)) != 0)
	{
		if (ret != -1)
		{
			// The settings window is a modeless dialog, so it needs IsDialogMessage
			// for Tab navigation and Enter/Escape. The check is scoped to that window
			// so the hidden tray window keeps its normal message handling.
			bool is_settings_message = settings_window_is_visible(&g_app.settings) &&
									   (msg.hwnd == g_app.settings.hwnd || IsChild(g_app.settings.hwnd, msg.hwnd));

			if (is_settings_message && IsDialogMessageW(g_app.settings.hwnd, &msg))
				continue;

			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else
		{
			DWORD error = GetLastError();
			ShutdownApp();
			ShowErrorMessageBox(lang_str(STR_ERR_UNEXPECTED_FMT), (unsigned long)error);
			ReleaseMutex(g_app.mutex);
			CloseHandle(g_app.mutex);
			return (int)error;
		}
	}

	// Cleanup
	UnregisterClassW(WINDOW_CLASS_NAME, hInstance);
	input_box_unregister(hInstance);
	ReleaseMutex(g_app.mutex);
	CloseHandle(g_app.mutex);

	return (int)msg.wParam;
}
