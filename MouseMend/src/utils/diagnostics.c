#include "diagnostics.h"
#include "../core/app_snapshot.h"
#include "../core/block_log.h"
#include "../core/config.h"
#include "../core/debouncer.h"
#include "../core/mouse_hook.h"
#include "../core/settings.h"
#include "../ui/language.h"
#include "../utils/autostart.h"
#include "../../version.h"
#include <strsafe.h>
#include <wchar.h>

#pragma comment(lib, "advapi32.lib")

#define DIAGNOSTICS_BUFFER_SIZE 2048

// Find a usable output stream.
// A GUI subsystem application has no console of its own, but when it is started
// from PowerShell or cmd the standard handles are already valid - either a pipe
// (captured output) or the parent console. Only when both are absent do we attach
// to the parent console explicitly.
static HANDLE open_output(bool *must_close)
{
	*must_close = false;

	HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
	if (handle && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN)
		return handle;

	if (!AttachConsole(ATTACH_PARENT_PROCESS))
		return NULL;

	handle = CreateFileW(L"CONOUT$", GENERIC_WRITE,
						 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
						 OPEN_EXISTING, 0, NULL);

	if (handle == INVALID_HANDLE_VALUE)
		return NULL;

	*must_close = true;
	return handle;
}

// Write text to the console, falling back to the redirected stream
static void write_text(HANDLE console, const wchar_t *text)
{
	if (!console || !text)
		return;

	DWORD written = 0;
	if (WriteConsoleW(console, text, (DWORD)wcslen(text), &written, NULL))
		return;

	// Redirected output: emit UTF-8 bytes instead
	char utf8[DIAGNOSTICS_BUFFER_SIZE * 2];
	int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8) - 1, NULL, NULL);
	if (length > 1)
		WriteFile(console, utf8, (DWORD)(length - 1), &written, NULL);
}

// Emit the report, either to the console or, when launched from Explorer, as a
// message box so the user can copy it into a ticket.
static void emit(const wchar_t *text)
{
	bool must_close = false;
	HANDLE output = open_output(&must_close);

	if (output)
	{
		write_text(output, text);
		if (must_close)
			CloseHandle(output);
		return;
	}

	MessageBoxW(NULL, text, APP_NAME_W, MB_OK | MB_ICONINFORMATION);
}

// Build the "MouseMend <version> (x64)" banner
// Plain ASCII on purpose: the report is read through a pipe, and a non-ASCII
// name would arrive as mojibake in a Windows PowerShell console.
static void build_banner(wchar_t *buffer, size_t buffer_count)
{
	StringCchPrintfW(buffer, buffer_count, L"%S %S (%s)\n",
					 APP_NAME, APP_VERSION_STRING, (sizeof(void *) == 8) ? L"x64" : L"x86");
}

// Print the version banner only
int diagnostics_print_version(void)
{
	wchar_t banner[128];
	build_banner(banner, ARRAYSIZE(banner));
	emit(banner);
	return 0;
}

// Build the full report into a caller supplied buffer
// Returns:
//   true when everything looks healthy
bool diagnostics_build(wchar_t *report, size_t report_count)
{
	wchar_t line[512];

	if (!report || report_count == 0)
		return false;

	report[0] = L'\0';

	build_banner(report, report_count);

	// Executable location (a truncated path must still be NUL terminated)
	wchar_t exe_path[MAX_PATH] = {0};
	DWORD exe_length = GetModuleFileNameW(NULL, exe_path, ARRAYSIZE(exe_path));
	if (exe_length == 0 || exe_length >= ARRAYSIZE(exe_path))
		exe_path[ARRAYSIZE(exe_path) - 1] = L'\0';
	StringCchPrintfW(line, ARRAYSIZE(line), L"exe=%s\n", exe_path);
	StringCchCatW(report, report_count, line);

	// Configuration state
	DebounceManager debounce;
	bool healthy = true;

	if (!debounce_init(&debounce))
	{
		StringCchCatW(report, report_count, L"debounce=init-failed\nresult=ATTENTION\n");
		return false;
	}

	Preferences prefs;
	memset(&prefs, 0, sizeof(prefs));
	prefs.language = language_detect_system();

	bool profile_loaded = settings_load(&debounce, &prefs);

	// A missing profile means "first run", and a real first run enables every
	// button (see InitializeApp). Without this the report would claim that none
	// of the buttons are protected while the running program protects all seven,
	// which is the one thing a diagnostic must never do: lie.
	if (!profile_loaded)
		debounce_set_all_monitored(&debounce, true);

	Language language = prefs.language;

	StringCchPrintfW(line, ARRAYSIZE(line), L"language=%s\n",
					 (language == LANG_ZH_CN) ? L"zh-CN" : L"en");
	StringCchCatW(report, report_count, line);

	StringCchPrintfW(line, ARRAYSIZE(line), L"profile=%s\n",
					 profile_loaded ? L"loaded" : L"missing (defaults applied)");
	StringCchCatW(report, report_count, line);

	int enabled = 0;
	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		if (debounce.buttons[i].isMonitored)
			enabled++;
	}

	StringCchPrintfW(line, ARRAYSIZE(line), L"protective_buttons=%d/%d\n", enabled, MOUSE_BUTTON_COUNT);
	StringCchCatW(report, report_count, line);
	healthy = healthy && enabled > 0;

	StringCchPrintfW(line, ARRAYSIZE(line), L"hybrid=%s\n", debounce.use_hybrid_heuristic ? L"on" : L"off");
	StringCchCatW(report, report_count, line);

	StringCchPrintfW(line, ARRAYSIZE(line), L"release_delay_ms=%u\n", (unsigned)debounce.releaseDelayMs);
	StringCchCatW(report, report_count, line);

	StringCchPrintfW(line, ARRAYSIZE(line), L"drag_distance_px=%u\n", (unsigned)debounce.dragDistancePx);
	StringCchCatW(report, report_count, line);

	StringCchPrintfW(line, ARRAYSIZE(line), L"notify_blocks=%s\n", prefs.notify_blocks ? L"on" : L"off");
	StringCchCatW(report, report_count, line);

	StringCchPrintfW(line, ARRAYSIZE(line), L"logging=%s\n", prefs.log_blocks ? L"on" : L"off");
	StringCchCatW(report, report_count, line);

	// Described, not created: a diagnostic run must not leave a Log folder behind
	wchar_t log_directory[APP_SNAPSHOT_LOG_DIR_SIZE];
	if (block_log_describe_directory(log_directory, ARRAYSIZE(log_directory)))
	{
		StringCchPrintfW(line, ARRAYSIZE(line), L"log_dir=%s\n", log_directory);
		StringCchCatW(report, report_count, line);
	}

	// Hook installation: prove that a low-level hook is allowed at all (security
	// software and group policy can block it), which is the number one reason for
	// "it does nothing". The probe uses its own local hook, so calling the report
	// from inside the running application cannot replace the live manager.
	DWORD hook_error = ERROR_SUCCESS;
	bool hook_ok = true;

	if (mouse_hook_probe())
	{
		StringCchCatW(report, report_count, L"hook_install=ok\n");
	}
	else
	{
		hook_ok = false;
		hook_error = GetLastError();
		StringCchPrintfW(line, ARRAYSIZE(line), L"hook_install=failed (error %lu)\n",
						 (unsigned long)hook_error);
		StringCchCatW(report, report_count, line);
		healthy = false;
	}

	// State as a stable ASCII token. The report is deliberately ASCII only: a
	// Chinese tooltip routed through a pipe is rendered with the system ANSI code
	// page by Windows PowerShell 5.1 and comes out as mojibake, which is worse
	// than useless in a support ticket.
	const wchar_t *status_token;
	if (!hook_ok)
		status_token = L"inactive";
	else if (enabled == 0)
		status_token = L"disabled";
	else if (enabled >= MOUSE_BUTTON_COUNT)
		status_token = L"enabled";
	else
		status_token = L"partial";

	StringCchPrintfW(line, ARRAYSIZE(line), L"status=%s\n", status_token);
	StringCchCatW(report, report_count, line);

	// Prove that the localized hover text fits the shell limit without printing it
	wchar_t hover[TRAY_TOOLTIP_MAX_CHARS];
	lang_format_tooltip(hover, ARRAYSIZE(hover), enabled, MOUSE_BUTTON_COUNT, 0, hook_ok);

	StringCchPrintfW(line, ARRAYSIZE(line), L"hover_text_chars=%u (limit %u)\n",
					 (unsigned)wcslen(hover), (unsigned)TRAY_TOOLTIP_MAX_CHARS);
	StringCchCatW(report, report_count, line);

	// Settings writability, without creating anything on a fresh profile
	HKEY hKey = NULL;
	LONG key_result = RegOpenKeyExW(HKEY_CURRENT_USER, REG_SETTINGS_KEY, 0,
									KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey);

	if (key_result == ERROR_FILE_NOT_FOUND)
	{
		StringCchCatW(report, report_count, L"settings_writable=no-profile-yet\n");
	}
	else if (key_result != ERROR_SUCCESS)
	{
		StringCchPrintfW(line, ARRAYSIZE(line), L"settings_writable=no (error %ld)\n", (long)key_result);
		StringCchCatW(report, report_count, line);
		healthy = false;
	}
	else
	{
		DWORD probe = 1;
		LONG write_result = RegSetValueExW(hKey, L"DiagnosticProbe", 0, REG_DWORD,
										   (const BYTE *)&probe, sizeof(probe));
		RegDeleteValueW(hKey, L"DiagnosticProbe");
		RegCloseKey(hKey);

		StringCchPrintfW(line, ARRAYSIZE(line), L"settings_writable=%s\n",
						 (write_result == ERROR_SUCCESS) ? L"yes" : L"no");
		StringCchCatW(report, report_count, line);
		healthy = healthy && write_result == ERROR_SUCCESS;
	}

	// Autostart entry
	StringCchPrintfW(line, ARRAYSIZE(line), L"autostart=%s\n",
					 autostart_is_enabled() ? L"enabled (matches this exe)" : L"not enabled or stale path");
	StringCchCatW(report, report_count, line);

	if (hook_error != ERROR_SUCCESS)
	{
		StringCchPrintfW(line, ARRAYSIZE(line), L"hook_error=%ld\n", (long)hook_error);
		StringCchCatW(report, report_count, line);
	}

	StringCchPrintfW(line, ARRAYSIZE(line), L"result=%s\n", healthy ? L"OK" : L"ATTENTION");
	StringCchCatW(report, report_count, line);

	debounce_cleanup(&debounce);

	return healthy;
}

// Print the full diagnostic report
int diagnostics_run(void)
{
	wchar_t report[DIAGNOSTICS_BUFFER_SIZE];
	bool healthy = diagnostics_build(report, ARRAYSIZE(report));

	emit(report);
	return healthy ? 0 : 1;
}
