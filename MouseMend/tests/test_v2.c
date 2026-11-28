// Tests for the modules added in 1.2.0:
//   - hook_watchdog: noticing a hook that Windows removed silently
//   - app_controller: single source of truth plus debounced persistence
//   - block_log: queue, formatting and daily file naming

#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <strsafe.h>

#include "test_common.h"
#include "../src/core/app_controller.h"
#include "../src/core/block_log.h"
#include "../src/core/config.h"
#include "../src/core/debouncer.h"
#include "../src/core/hook_watchdog.h"
#include "../src/ui/language.h"

// ---------------------------------------------------------------------------
// Watchdog
// ---------------------------------------------------------------------------
void test_hook_watchdog(void)
{
	printf("hook watchdog\n");

	WatchdogState state;
	hook_watchdog_init(&state);

	const uint64_t cooldown = 30000;

	// First call only establishes a baseline
	CHECK(hook_watchdog_check(&state, false, false, 0, 1000, cooldown) == WATCHDOG_FIRST_CHECK);
	CHECK(state.initialised);
	CHECK(hook_watchdog_is_healthy(&state));

	// Mouse completely idle: nothing can be concluded, even with a frozen counter
	CHECK(hook_watchdog_check(&state, false, false, 0, 4000, cooldown) == WATCHDOG_ALIVE);

	// Cursor moved and the hook saw it: healthy
	CHECK(hook_watchdog_check(&state, true, false, 5, 7000, cooldown) == WATCHDOG_ALIVE);
	CHECK(state.suspect_streak == 0);
	CHECK(hook_watchdog_is_healthy(&state));

	// Cursor moved but the hook saw nothing: first suspicion. The tray has to
	// stop claiming protection from this point, not after the second sample.
	CHECK(hook_watchdog_check(&state, true, false, 5, 10000, cooldown) == WATCHDOG_SUSPECT);
	CHECK(!hook_watchdog_is_healthy(&state));

	// An idle window in between must reset the streak, but silence is not proof
	// that a dead hook came back - only an event from it can clear the flag.
	CHECK(hook_watchdog_check(&state, false, false, 5, 11000, cooldown) == WATCHDOG_ALIVE);
	CHECK(state.suspect_streak == 0);
	CHECK(!hook_watchdog_is_healthy(&state));

	// A click without cursor movement is evidence too (touchpad taps), so a dead
	// hook must be caught even when the pointer never moves
	CHECK(hook_watchdog_check(&state, false, true, 5, 11500, cooldown) == WATCHDOG_SUSPECT);
	CHECK(hook_watchdog_check(&state, false, true, 5, 12000, cooldown) == WATCHDOG_RECOVER);
	CHECK(state.recover_count == 1);
	CHECK(state.last_recover_tick == 12000);
	CHECK(state.suspect_streak == 0);

	// Immediately afterwards the cooldown must suppress another recovery
	CHECK(hook_watchdog_check(&state, true, false, 5, 16000, cooldown) == WATCHDOG_SUSPECT);
	CHECK(hook_watchdog_check(&state, true, false, 5, 19000, cooldown) == WATCHDOG_SUSPECT);
	CHECK(state.recover_count == 1);

	// Once the cooldown elapsed the evidence has to be collected again: the streak
	// is dropped while waiting, so one sample cannot reinstall on its own. Without
	// that, the cooldown ended with a streak already past the limit and the very
	// next check reinstalled - a reinstall every cooldown, forever.
	CHECK(hook_watchdog_check(&state, true, false, 5, 44000, cooldown) == WATCHDOG_SUSPECT);
	CHECK(state.recover_count == 1);

	CHECK(hook_watchdog_check(&state, true, false, 5, 47000, cooldown) == WATCHDOG_RECOVER);
	CHECK(state.recover_count == 2);

	// Events arriving again prove the recovery worked
	CHECK(hook_watchdog_check(&state, true, false, 9, 47000, cooldown) == WATCHDOG_ALIVE);
	CHECK(state.suspect_streak == 0);
	CHECK(hook_watchdog_is_healthy(&state));

	// A button press that the hook did see is equally healthy
	CHECK(hook_watchdog_check(&state, false, true, 12, 50000, cooldown) == WATCHDOG_ALIVE);

	// Degenerate input must not crash
	CHECK(hook_watchdog_check(NULL, false, false, 0, 0, cooldown) == WATCHDOG_ALIVE);
	CHECK(hook_watchdog_is_healthy(NULL));
	hook_watchdog_init(NULL);

	// A fresh init after recovery clears the flag even without another event
	hook_watchdog_check(&state, true, false, 20, 51000, cooldown);
	CHECK(hook_watchdog_check(&state, true, false, 20, 52000, cooldown) == WATCHDOG_SUSPECT);
	CHECK(!hook_watchdog_is_healthy(&state));
	hook_watchdog_init(&state);
	CHECK(hook_watchdog_is_healthy(&state));
}

// ---------------------------------------------------------------------------
// App controller
// ---------------------------------------------------------------------------
static int g_save_calls;
static int g_reset_calls;
static int g_observer_calls;
static bool g_save_result;

static bool test_save_sink(const DebounceManager *debounce, const Preferences *prefs, void *user)
{
	(void)debounce;
	(void)prefs;
	(void)user;

	g_save_calls++;
	return g_save_result;
}

static void test_reset_sink(void *user)
{
	(void)user;
	g_reset_calls++;
}

static void test_observer(void *user)
{
	(void)user;
	g_observer_calls++;
}

void test_app_controller(void)
{
	printf("app controller\n");

	DebounceManager debounce;
	Preferences prefs;
	memset(&prefs, 0, sizeof(prefs));

	if (!debounce_init(&debounce))
	{
		printf("[FAIL] debounce_init\n");
		g_failures++;
		return;
	}

	debounce_set_threshold(&debounce, MOUSE_BUTTON_LEFT, 50, THRESHOLD_MIN_VALUE, THRESHOLD_MAX_VALUE);

	AppController controller;
	app_controller_attach(&controller, &debounce, &prefs);
	app_controller_set_save_sink(&controller, test_save_sink, NULL);
	app_controller_set_reset_sink(&controller, test_reset_sink, NULL);
	app_controller_set_observer(&controller, test_observer, NULL);

	g_save_calls = 0;
	g_reset_calls = 0;
	g_observer_calls = 0;
	g_save_result = true;

	// A setter updates memory, marks dirty and notifies, but never writes
	app_controller_set_threshold(&controller, MOUSE_BUTTON_LEFT, 77);
	CHECK(debounce.buttons[MOUSE_BUTTON_LEFT].thresholdMs == 77);
	CHECK(app_controller_is_dirty(&controller));
	CHECK(g_save_calls == 0);
	CHECK(g_observer_calls == 1);

	// The first tick only starts the countdown
	app_controller_tick(&controller, 1000);
	CHECK(g_save_calls == 0);

	// Still inside the delay
	app_controller_tick(&controller, 1000 + APP_CONTROLLER_COMMIT_DELAY_MS - 1);
	CHECK(g_save_calls == 0);

	// Delay elapsed: exactly one write
	app_controller_tick(&controller, 1000 + APP_CONTROLLER_COMMIT_DELAY_MS);
	CHECK(g_save_calls == 1);
	CHECK(!app_controller_is_dirty(&controller));

	// Further ticks with a clean state must not write
	app_controller_tick(&controller, 90000);
	CHECK(g_save_calls == 1);

	// Out-of-range values are rejected without dirtying the state
	app_controller_set_threshold(&controller, MOUSE_BUTTON_LEFT, 0);
	app_controller_set_threshold(&controller, MOUSE_BUTTON_LEFT, 5000);
	CHECK(debounce.buttons[MOUSE_BUTTON_LEFT].thresholdMs == 77);
	CHECK(!app_controller_is_dirty(&controller));

	// Invalid button index is ignored
	app_controller_set_threshold(&controller, (MouseButton)MOUSE_BUTTON_COUNT, 60);
	CHECK(!app_controller_is_dirty(&controller));

	// Each new change restarts the countdown, so a slider drag writes once
	app_controller_set_threshold(&controller, MOUSE_BUTTON_LEFT, 60);
	app_controller_tick(&controller, 10000);
	app_controller_set_threshold(&controller, MOUSE_BUTTON_LEFT, 61);
	app_controller_tick(&controller, 10200);
	app_controller_set_threshold(&controller, MOUSE_BUTTON_LEFT, 62);
	app_controller_tick(&controller, 10400);
	CHECK(g_save_calls == 1);

	app_controller_tick(&controller, 10400 + APP_CONTROLLER_COMMIT_DELAY_MS);
	CHECK(g_save_calls == 2);
	CHECK(debounce.buttons[MOUSE_BUTTON_LEFT].thresholdMs == 62);

	// A failing writer keeps the state dirty so a later tick can retry
	g_save_result = false;
	app_controller_set_threshold(&controller, MOUSE_BUTTON_LEFT, 65);
	CHECK(app_controller_commit(&controller) == false);
	CHECK(app_controller_is_dirty(&controller));

	g_save_result = true;
	CHECK(app_controller_commit(&controller) == true);
	CHECK(!app_controller_is_dirty(&controller));

	// Toggling buttons, presets and the Smart Drag tuning all go through here
	app_controller_set_monitored(&controller, MOUSE_BUTTON_RIGHT, true);
	CHECK(debounce.buttons[MOUSE_BUTTON_RIGHT].isMonitored);

	app_controller_set_all_monitored(&controller, true);
	CHECK(debounce_is_all_monitored(&debounce));

	app_controller_set_hybrid(&controller, false);
	CHECK(!debounce.use_hybrid_heuristic);

	app_controller_set_release_delay(&controller, 80);
	CHECK(debounce.releaseDelayMs == 80);

	// Out of range delay is rejected
	app_controller_set_release_delay(&controller, 100000);
	CHECK(debounce.releaseDelayMs == 80);

	app_controller_set_drag_distance(&controller, 10);
	CHECK(debounce.dragDistancePx == 10);

	app_controller_apply_preset(&controller, &CONFIG_PRESET_STRICT);
	CHECK(debounce.buttons[MOUSE_BUTTON_LEFT].thresholdMs == 40);

	// Language and preference flags
	Language saved_language = language_get();
	app_controller_set_language(&controller, LANG_ZH_CN);
	CHECK(prefs.language == LANG_ZH_CN);
	CHECK(language_get() == LANG_ZH_CN);

	app_controller_set_language(&controller, (Language)LANG_COUNT);
	CHECK(prefs.language == LANG_ZH_CN);
	language_set(saved_language);

	app_controller_set_notify_blocks(&controller, true);
	CHECK(prefs.notify_blocks);

	app_controller_set_log_blocks(&controller, true);
	CHECK(prefs.log_blocks);

	// Clearing the statistics reaches the reset sink exactly once
	g_reset_calls = 0;
	debounce_set_monitored(&debounce, MOUSE_BUTTON_LEFT, true);
	app_controller_reset_statistics(&controller);
	CHECK(g_reset_calls == 1);
	CHECK(debounce_get_total_blocks(&debounce) == 0);

	// A controller without a save sink cannot commit, and stays dirty
	AppController bare;
	app_controller_attach(&bare, &debounce, &prefs);
	app_controller_set_threshold(&bare, MOUSE_BUTTON_LEFT, 45);
	CHECK(app_controller_commit(&bare) == false);
	CHECK(app_controller_is_dirty(&bare));

	// Degenerate calls must not crash
	app_controller_attach(NULL, NULL, NULL);
	app_controller_set_threshold(NULL, MOUSE_BUTTON_LEFT, 50);
	app_controller_tick(NULL, 0);
	CHECK(app_controller_commit(NULL) == false);

	debounce_cleanup(&debounce);
}

// ---------------------------------------------------------------------------
// Block log
// ---------------------------------------------------------------------------
void test_block_log(void)
{
	printf("block log\n");

	BlockLogQueue queue;
	CHECK(block_log_init(&queue));

	BlockLogEntry entry;

	// Empty queue
	CHECK(block_log_count(&queue) == 0);
	CHECK(!block_log_pop(&queue, &entry));

	// FIFO order
	CHECK(block_log_push(&queue, MOUSE_BUTTON_LEFT, true, 100, 50, L"chrome.exe"));
	CHECK(block_log_push(&queue, MOUSE_BUTTON_RIGHT, false, 200, 60, NULL));
	CHECK(block_log_count(&queue) == 2);

	CHECK(block_log_pop(&queue, &entry));
	CHECK(entry.button == MOUSE_BUTTON_LEFT);
	CHECK(entry.is_down);
	CHECK(entry.elapsed_ms == 100);
	CHECK(entry.threshold_ms == 50);

	// The name is copied into the slot: the caller's buffer is a stack local on
	// the hook, gone long before the UI thread drains the queue
	CHECK(wcscmp(entry.process, L"chrome.exe") == 0);

	CHECK(block_log_pop(&queue, &entry));
	CHECK(entry.button == MOUSE_BUTTON_RIGHT);
	CHECK(!entry.is_down);
	CHECK(entry.elapsed_ms == 200);

	// A missing name is stored as an empty string, not as a stale one
	CHECK(entry.process[0] == L'\0');

	CHECK(!block_log_pop(&queue, &entry));

	// Overflow drops the oldest entry and counts it
	for (int i = 0; i < BLOCK_LOG_RING_SIZE + 5; i++)
		CHECK(block_log_push(&queue, MOUSE_BUTTON_MIDDLE, true, (uint64_t)i, 50, NULL));

	CHECK(block_log_count(&queue) == BLOCK_LOG_RING_SIZE);
	CHECK(block_log_dropped(&queue) == 5);

	CHECK(block_log_pop(&queue, &entry));
	CHECK(entry.elapsed_ms == 5); // the first five were dropped

	// Invalid buttons are rejected
	CHECK(!block_log_push(&queue, (MouseButton)MOUSE_BUTTON_COUNT, true, 0, 50, L"a.exe"));
	CHECK(!block_log_push(NULL, MOUSE_BUTTON_LEFT, true, 0, 50, L"a.exe"));
	CHECK(!block_log_pop(NULL, &entry));

	// Slugs are stable and language independent
	Language saved_language = language_get();
	language_set(LANG_ZH_CN);
	CHECK(strcmp(block_log_button_slug(MOUSE_BUTTON_LEFT), "left") == 0);
	CHECK(strcmp(block_log_button_slug(MOUSE_BUTTON_RIGHT), "right") == 0);
	CHECK(strcmp(block_log_button_slug(MOUSE_BUTTON_MIDDLE), "middle") == 0);
	CHECK(strcmp(block_log_button_slug(MOUSE_BUTTON_X1), "x1") == 0);
	CHECK(strcmp(block_log_button_slug(MOUSE_BUTTON_X2), "x2") == 0);
	CHECK(strcmp(block_log_button_slug(MOUSE_BUTTON_WHEEL), "wheel") == 0);
	CHECK(strcmp(block_log_button_slug(MOUSE_BUTTON_HWHEEL), "hwheel") == 0);
	language_set(saved_language);

	// Day naming and the line format
	SYSTEMTIME base;
	memset(&base, 0, sizeof(base));
	base.wYear = 2026;
	base.wMonth = 11;
	base.wDay = 21;
	base.wHour = 15;
	base.wMinute = 4;
	base.wSecond = 3;
	base.wMilliseconds = 123;

	wchar_t day[16];
	CHECK(block_log_format_day(0, &base, day, ARRAYSIZE(day)));
	CHECK(wcscmp(day, L"20261121") == 0);

	// Crossing midnight must roll the file name over
	CHECK(block_log_format_day((uint64_t)9 * 3600 * 1000, &base, day, ARRAYSIZE(day)));
	CHECK(wcscmp(day, L"20261122") == 0);

	BlockLogEntry sample;
	sample.elapsed_ms = 0;
	sample.button = MOUSE_BUTTON_X1;
	sample.is_down = true;
	sample.threshold_ms = 55;
	sample.process[0] = L'\0';

	wchar_t line[256];
	CHECK(block_log_format_line(&sample, &base, line, ARRAYSIZE(line)));
	CHECK(wcsstr(line, L"2026-11-21 15:04:03.123") != NULL);
	CHECK(wcsstr(line, L"x1") != NULL);
	CHECK(wcsstr(line, L"pressed") != NULL);
	CHECK(wcsstr(line, L"threshold=55") != NULL);
	CHECK(wcsstr(line, L"\r\n") != NULL);

	// The program name has a column of its own, straight after the time. This is
	// the whole point of the change: "left pressed" alone never said in which
	// program the click was blocked.
	StringCchCopyW(sample.process, ARRAYSIZE(sample.process), L"notepad.exe");
	CHECK(block_log_format_line(&sample, &base, line, ARRAYSIZE(line)));
	CHECK(wcsstr(line, L".123\tnotepad.exe\tx1\tpressed") != NULL);

	// No name reads as "-" rather than as an empty field, so every line still has
	// the same five columns and the header describes all of them
	sample.process[0] = L'\0';
	CHECK(block_log_format_line(&sample, &base, line, ARRAYSIZE(line)));
	CHECK(wcsstr(line, L".123\t-\tx1\tpressed") != NULL);

	// A release is labelled as such
	sample.is_down = false;
	CHECK(block_log_format_line(&sample, &base, line, ARRAYSIZE(line)));
	CHECK(wcsstr(line, L"released") != NULL);

	// Degenerate arguments
	CHECK(!block_log_format_day(0, NULL, day, ARRAYSIZE(day)));
	CHECK(!block_log_format_line(NULL, &base, line, ARRAYSIZE(line)));

	// A destination that is too small must report failure, not overflow
	wchar_t tiny[4];
	CHECK(!block_log_format_day(0, &base, tiny, ARRAYSIZE(tiny)));

	// A name longer than the slot is cut, not allowed to overrun it
	wchar_t long_name[BLOCK_LOG_PROCESS_CHARS + 12];
	for (int i = 0; i < (int)ARRAYSIZE(long_name) - 1; i++)
		long_name[i] = (wchar_t)(L'a' + (i % 26));
	long_name[ARRAYSIZE(long_name) - 1] = L'\0';

	// Not named "small": rpcndr.h still defines that as char for MIDL compatibility,
	// and `BlockLogQueue small;` then reads as two types in a row.
	BlockLogQueue long_name_queue;
	CHECK(block_log_init(&long_name_queue));
	CHECK(block_log_push(&long_name_queue, MOUSE_BUTTON_LEFT, true, 0, 50, long_name));
	CHECK(block_log_pop(&long_name_queue, &entry));
	CHECK(wcslen(entry.process) == BLOCK_LOG_PROCESS_CHARS - 1);
	CHECK(wcsncmp(entry.process, long_name, BLOCK_LOG_PROCESS_CHARS - 1) == 0);
	block_log_cleanup(&long_name_queue);

	block_log_cleanup(&queue);
	CHECK(block_log_count(&queue) == 0);
	CHECK(!block_log_push(&queue, MOUSE_BUTTON_LEFT, true, 0, 50, NULL));
}

// Exercise the real file writer in a throwaway directory.
// This covers the requirement "deleting the log must not disable the feature":
// the folder has to come back on the next blocked click.
void test_block_log_files(void)
{
	printf("block log files\n");

	wchar_t temp_path[MAX_PATH];
	DWORD length = GetTempPathW(ARRAYSIZE(temp_path), temp_path);

	if (length == 0 || length >= ARRAYSIZE(temp_path))
	{
		printf("[SKIP] no temp directory\n");
		return;
	}

	wchar_t directory[MAX_PATH];
	StringCchPrintfW(directory, ARRAYSIZE(directory), L"%smousemend_test_%u", temp_path, (unsigned)GetCurrentProcessId());

	// A leftover from a previous run must not influence the test
	RemoveDirectoryW(directory);

	SYSTEMTIME base;
	memset(&base, 0, sizeof(base));
	base.wYear = 2026;
	base.wMonth = 11;
	base.wDay = 21;
	base.wHour = 10;
	base.wMinute = 0;
	base.wSecond = 0;

	BlockLogEntry entry;
	entry.elapsed_ms = 5000;
	entry.button = MOUSE_BUTTON_LEFT;
	entry.is_down = true;
	entry.threshold_ms = 50;
	StringCchCopyW(entry.process, ARRAYSIZE(entry.process), L"chrome.exe");

	// First write creates both the directory and today's file
	CHECK(block_log_write(directory, &base, &entry));

	wchar_t file_path[MAX_PATH];
	StringCchPrintfW(file_path, ARRAYSIZE(file_path), L"%s\\20261121.log", directory);

	HANDLE file = CreateFileW(file_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
							  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(file != INVALID_HANDLE_VALUE);

	if (file != INVALID_HANDLE_VALUE)
	{
		char content[512] = {0};
		DWORD read = 0;
		if (!ReadFile(file, content, sizeof(content) - 1, &read, NULL))
			content[0] = '\0';
		CloseHandle(file);

		CHECK(strstr(content, "left") != NULL);
		CHECK(strstr(content, "pressed") != NULL);
		CHECK(strstr(content, "threshold=50") != NULL);
		CHECK(strstr(content, "2026-11-21 10:00:05.000") != NULL);

		// The program is written as a column, and the header names all five
		CHECK(strstr(content, "chrome.exe") != NULL);
		CHECK(strstr(content, "# time\tprogram\tbutton\tevent\tthreshold") != NULL);

		// A new file starts with a UTF-8 byte order mark, or Excel would read a
		// Chinese program name as the ANSI code page and show garbage
		CHECK((unsigned char)content[0] == 0xEF);
		CHECK((unsigned char)content[1] == 0xBB);
		CHECK((unsigned char)content[2] == 0xBF);
	}

	// The user deletes the folder: the next blocked click must recreate it
	CHECK(DeleteFileW(file_path));
	CHECK(RemoveDirectoryW(directory));
	CHECK(GetFileAttributesW(directory) == INVALID_FILE_ATTRIBUTES);

	CHECK(block_log_write(directory, &base, &entry));
	CHECK(GetFileAttributesW(directory) != INVALID_FILE_ATTRIBUTES);

	// Appending must keep the earlier content instead of truncating the file
	entry.elapsed_ms = 6000;
	entry.button = MOUSE_BUTTON_RIGHT;
	CHECK(block_log_write(directory, &base, &entry));

	file = CreateFileW(file_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
					   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(file != INVALID_HANDLE_VALUE);

	if (file != INVALID_HANDLE_VALUE)
	{
		char content[512] = {0};
		DWORD read = 0;
		if (!ReadFile(file, content, sizeof(content) - 1, &read, NULL))
			content[0] = '\0';
		CloseHandle(file);

		CHECK(strstr(content, "left") != NULL);
		CHECK(strstr(content, "right") != NULL);
	}

	// A second entry on a different day goes to a different file
	entry.elapsed_ms = (uint64_t)20 * 3600 * 1000;
	CHECK(block_log_write(directory, &base, &entry));

	wchar_t next_day_path[MAX_PATH];
	StringCchPrintfW(next_day_path, ARRAYSIZE(next_day_path), L"%s\\20261122.log", directory);
	CHECK(GetFileAttributesW(next_day_path) != INVALID_FILE_ATTRIBUTES);

	// Invalid arguments are rejected
	CHECK(!block_log_write(NULL, &base, &entry));
	CHECK(!block_log_write(directory, NULL, &entry));
	CHECK(!block_log_write(directory, &base, NULL));

	// A batch is written with one open, and the header still appears exactly once
	CHECK(DeleteFileW(file_path));
	CHECK(DeleteFileW(next_day_path));
	CHECK(RemoveDirectoryW(directory));

	BlockLogEntry batch[3];
	memset(batch, 0, sizeof(batch));

	for (int i = 0; i < 3; i++)
	{
		batch[i].elapsed_ms = 1000 + (uint64_t)i * 1000;
		batch[i].button = MOUSE_BUTTON_LEFT;
		batch[i].is_down = (i % 2) == 0;
		batch[i].threshold_ms = 50;
	}

	CHECK(block_log_write_many(directory, &base, batch, 3) == 3);

	file = CreateFileW(file_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
					   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(file != INVALID_HANDLE_VALUE);

	if (file != INVALID_HANDLE_VALUE)
	{
		char content[1024] = {0};
		DWORD read = 0;
		if (!ReadFile(file, content, sizeof(content) - 1, &read, NULL))
			content[0] = '\0';
		CloseHandle(file);

		int headers = 0;

		for (const char *cursor = content; (cursor = strstr(cursor, "# time")) != NULL; cursor++)
			headers++;

		// One header for the whole batch, not one per entry
		CHECK(headers == 1);

		CHECK(strstr(content, "10:00:01.000") != NULL);
		CHECK(strstr(content, "10:00:02.000") != NULL);
		CHECK(strstr(content, "10:00:03.000") != NULL);

		// Press and release alternate, so the batch is not collapsed into one row
		CHECK(strstr(content, "pressed") != NULL);
		CHECK(strstr(content, "released") != NULL);
	}

	// A batch that straddles midnight has to switch files part way through.
	// The base is 10:00:00, so 13 h 59 m 59 s lands on 23:59:59 and one second
	// later is the next day.
	memset(batch, 0, sizeof(batch));
	batch[0].elapsed_ms = (uint64_t)50399 * 1000; // 23:59:59, day one
	batch[1].elapsed_ms = (uint64_t)50400 * 1000; // 00:00:00, day two
	batch[2].elapsed_ms = (uint64_t)50401 * 1000;

	CHECK(block_log_write_many(directory, &base, batch, 3) == 3);
	CHECK(GetFileAttributesW(file_path) != INVALID_FILE_ATTRIBUTES);
	CHECK(GetFileAttributesW(next_day_path) != INVALID_FILE_ATTRIBUTES);

	// An empty batch is not a failure
	CHECK(block_log_write_many(directory, &base, batch, 0) == 0);
	CHECK(block_log_write_many(NULL, &base, batch, 3) == 0);

	// Clean up
	DeleteFileW(file_path);
	DeleteFileW(next_day_path);
	RemoveDirectoryW(directory);
}
