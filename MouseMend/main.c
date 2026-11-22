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
#include "src/core/app_snapshot.h"
#include "src/core/event_trace.h"
#include "src/core/block_log.h"
#include "src/core/hook_watchdog.h"
#include "src/ui/tray_icon.h"
#include "src/ui/context_menu.h"
#include "src/ui/input_box.h"
#include "src/ui/language.h"
#include "src/ui/settings_window.h"
#include "src/ui/about_window.h"
#include "src/utils/autostart.h"
#include "src/utils/crash_dump.h"
#include "src/utils/diagnostics.h"
#include "src/utils/logger.h"

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
	uint32_t log_written;

	// Decisions the engine made, shown by the check-up window
	EventTrace trace;
	AppBlockEvidence evidence[MOUSE_BUTTON_COUNT];

	// Hook health
	WatchdogState watchdog;
	POINT watchdog_cursor;
	uint32_t watchdog_buttons;
	uint32_t hook_events;	 // Button / wheel events, used by the test area
	uint32_t hook_recoveries;
	bool state_msg_pending;

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

// Make sure the deferred-release timer is running while a release is pending.
// This must be evaluated even when the current event is blocked, because the
// event that starts a deferral is itself swallowed by the hook.
static void EnsureDeferredReleaseTimer(void)
{
	if (g_app.deferred_timer_armed || !debounce_has_pending_deferrals(&g_app.debounce))
		return;

	if (SetTimer(g_app.hWnd, TIMER_DEFERRED_RELEASE, TIMER_INTERVAL_MS, NULL))
	{
		g_app.deferred_timer_armed = true;
		return;
	}

	// Timer could not be created: never leave the button logically stuck.
	// Release now instead of applying the debounce delay.
	debounce_check_deferred_releases(&g_app.debounce);
	debounce_flush_deferred_releases(&g_app.debounce);
}

// Mouse hook callback
static LRESULT CALLBACK OnMouseHookCallback(const MouseEvent *event, void *user_data)
{
	AppState *app = (AppState *)user_data;

	// Process event directly - avoid creating intermediate structure
	// The explanation is kept because the check-up window renders the engine's
	// own verdict instead of repeating the rules somewhere else.
	DebounceDecision decision;
	bool blocked = debounce_process_event_ex(&app->debounce, event, &decision);

	// Record the decision for the check-up window. Memory only: the hook has to
	// return within its budget or Windows silently removes it.
	{
		TraceEntry entry;
		entry.button = event->button;
		entry.timestamp = event->timestamp;
		entry.x = event->x;
		entry.y = event->y;
		entry.is_down = event->is_down;
		entry.decision = decision;
		event_trace_push(&app->trace, &entry);
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

	// Arm the deferred-release timer before returning, even for blocked events.
	EnsureDeferredReleaseTimer();

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

			if (block_log_push(&app->log_queue, event->button, event->is_down, elapsed, threshold))
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
	wchar_t local_app_data[MAX_PATH] = {0};

	if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, ARRAYSIZE(local_app_data)) > 0)
	{
		wchar_t directory[MAX_PATH];
		StringCchPrintfW(directory, ARRAYSIZE(directory), L"%s\\%s", local_app_data, LOG_DIRECTORY_NAME);
		CreateDirectoryW(directory, NULL);

		StringCchPrintfW(path, path_count, L"%s\\%s", directory, LOG_FILE_NAME);
		return;
	}

	StringCchCopyW(path, path_count, LOG_FILE_NAME);
}

// Initialize application
static bool InitializeApp(void)
{
#ifndef NDEBUG
	// Initialize logger (only in Debug mode)
	wchar_t log_path[MAX_PATH];
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
							  BuildSnapshot, &g_app, &g_app.trace))
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

	// Release any release that is still deferred, otherwise the button would
	// stay logically pressed after this process is gone.
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

	if (g_app.debounce_ready)
	{
		debounce_cleanup(&g_app.debounce);
		g_app.debounce_ready = false;
	}

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
	(void)user;

	if (settings_save(debounce, prefs))
		return true;

	if (!warning_shown && g_app.tray_ready)
	{
		tray_icon_show_balloon(&g_app.tray_icon, lang_str(STR_APP_NAME), lang_str(STR_SAVE_FAILED), NIIF_WARNING);
		warning_shown = true;
	}

	return false;
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

	// Bounded drain: each entry costs an open/write/close, and this runs on the
	// same thread as the mouse hook. Whatever is left is picked up by the next
	// state tick instead of blocking here for an unbounded time.
	int budget = BLOCK_LOG_DRAIN_PER_TICK;
	BlockLogEntry entry;

	while (budget-- > 0 && block_log_pop(&g_app.log_queue, &entry))
	{
		if (block_log_write(g_app.log_directory, &g_app.log_base_time, &entry))
		{
			g_app.log_written++;
			continue;
		}

		if (!g_app.log_warning_shown && g_app.tray_ready)
		{
			tray_icon_show_balloon(&g_app.tray_icon, lang_str(STR_APP_NAME),
								   lang_str(STR_LOG_DIR_UNAVAILABLE), NIIF_WARNING);
			g_app.log_warning_shown = true;
		}
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
	if (MessageBoxW(NULL, lang_str(STR_CONFIRM_RESTORE), lang_str(STR_APP_NAME),
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
			if (!debounce_has_pending_deferrals(&g_app.debounce))
			{
				KillTimer(hWnd, TIMER_DEFERRED_RELEASE);
				g_app.deferred_timer_armed = false;
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

	case WM_COMMAND:
	{
		UINT menu_id = LOWORD(wParam);

		if (menu_id == IDM_ABOUT)
		{
			about_window_show(hWnd, g_app.hInstance);
			return 0;
		}
		if (menu_id == IDM_EXIT)
		{
			PostMessage(hWnd, WM_CLOSE, 0, 0);
			return 0;
		}
		if (menu_id == IDM_TOGGLE_ENABLE)
		{
			ToggleEnableAll();
			return 0;
		}
		if (menu_id == IDM_TOGGLE_HYBRID)
		{
			ToggleHybridHeuristic();
			return 0;
		}
		if (menu_id == IDM_TOGGLE_AUTOSTART)
		{
			ToggleAutostart();
			return 0;
		}
		if (menu_id == IDM_TOGGLE_NOTIFY)
		{
			ToggleNotifyBlocks();
			return 0;
		}
		if (menu_id == IDM_TOGGLE_LOG)
		{
			ToggleLogBlocks();
			return 0;
		}
		if (menu_id == IDM_OPEN_LOG_FOLDER)
		{
			OpenLogFolder();
			return 0;
		}
		if (menu_id == IDM_OPEN_SETTINGS)
		{
			OpenCheckupWindow();
			return 0;
		}
		if (menu_id == IDM_RESET_STATS)
		{
			// The controller also resets the notification baseline through its sink
			app_controller_reset_statistics(&g_app.controller);

#ifndef NDEBUG
			LOG_INFO(&g_app.logger, "Statistics reset");
#endif
			return 0;
		}
		if (menu_id == IDM_PRESET_DEFAULT)
		{
			ApplyPreset(&CONFIG_PRESET_DEFAULT, true);
			return 0;
		}
		if (menu_id == IDM_PRESET_OFFICE)
		{
			ApplyPreset(&CONFIG_PRESET_OFFICE, true);
			return 0;
		}
		if (menu_id == IDM_PRESET_STRICT)
		{
			ApplyPreset(&CONFIG_PRESET_STRICT, true);
			return 0;
		}
		if (menu_id == IDM_RESTORE_DEFAULTS)
		{
			RestoreDefaults();
			return 0;
		}
		if (menu_id == IDM_LANG_EN)
		{
			SetLanguage(LANG_EN);
			return 0;
		}
		if (menu_id == IDM_LANG_ZH)
		{
			SetLanguage(LANG_ZH_CN);
			return 0;
		}

		// Smart Drag Protection tuning
		//
		// Through the controller, not the manager: it is the only writer of the
		// configuration, and going around it skipped the range clamping and the
		// dirty flag (the commit below would then persist nothing).
		if (menu_id >= (UINT)IDM_RELEASE_DELAY_BASE && menu_id < (UINT)IDM_RELEASE_DELAY_BASE + RELEASE_DELAY_PRESET_COUNT)
		{
			int value = config_release_delay_presets()[menu_id - (UINT)IDM_RELEASE_DELAY_BASE];
			app_controller_set_release_delay(&g_app.controller, (uint32_t)value);
			SaveSettings();
			return 0;
		}
		if (menu_id >= (UINT)IDM_DRAG_DISTANCE_BASE && menu_id < (UINT)IDM_DRAG_DISTANCE_BASE + DRAG_DISTANCE_PRESET_COUNT)
		{
			int value = config_drag_distance_presets()[menu_id - (UINT)IDM_DRAG_DISTANCE_BASE];
			app_controller_set_drag_distance(&g_app.controller, (uint32_t)value);
			SaveSettings();
			return 0;
		}

		// Individual button toggles: IDM_TOGGLE_BASE + MouseButton
		if (menu_id >= (UINT)IDM_TOGGLE_BASE && menu_id < (UINT)IDM_TOGGLE_BASE + (UINT)MOUSE_BUTTON_COUNT)
		{
			ToggleButton((MouseButton)(menu_id - (UINT)IDM_TOGGLE_BASE));
			return 0;
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
				return 0;
			}

			if (menu_id_is_threshold_custom(menu_id, &button, MOUSE_BUTTON_COUNT))
			{
				PromptCustomThreshold((MouseButton)button);
				return 0;
			}
		}
		break;
	}

	case WM_CLOSE:
		// The About dialog is modal and owns its own message loop, so a quit
		// requested from the tray icon while it is up would otherwise leave the
		// process waiting for a click nobody is going to make.
		about_window_close();
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

// Check whether the protection can actually act on mouse input
static bool IsHookActive(void)
{
	return g_app.hook_ready && g_app.mouse_hook.installed;
}

// Show context menu
static void ShowContextMenu(const HWND hWnd, const int x, const int y)
{
	context_menu_show(&g_app.context_menu, hWnd, x, y, &g_app.debounce, &g_app.prefs, IsHookActive());

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

	MessageBoxW(NULL, buffer, lang_str(STR_APP_NAME), MB_OK | MB_ICONERROR);
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

	// A dump is the only way to turn a user's "it closed itself" into a stack.
	// Installed before anything can fail, and it changes nothing the user sees.
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
