#include "autostart.h"
#include <strsafe.h>

#pragma comment(lib, "advapi32.lib")

#define AUTOSTART_RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define AUTOSTART_VALUE_NAME L"MouseMend"

// Check whether MouseMend is registered to start with Windows
bool autostart_is_enabled(void)
{
	HKEY hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_RUN_KEY, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
		return false;

	wchar_t value[MAX_PATH * 2] = {0};
	DWORD type = 0;
	DWORD size = sizeof(value);
	LONG result = RegQueryValueExW(hKey, AUTOSTART_VALUE_NAME, NULL, &type, (BYTE *)value, &size);

	RegCloseKey(hKey);

	if (result != ERROR_SUCCESS)
		return false;

	if (type != REG_SZ && type != REG_EXPAND_SZ)
		return false;

	// Reject values that do not fit or were not NUL terminated inside the buffer
	if (size < sizeof(wchar_t) || size > sizeof(value) - sizeof(wchar_t))
		return false;

	value[ARRAYSIZE(value) - 1] = L'\0';

	// The stored command is quoted, compare it with the running executable.
	// A stale entry (the user moved the exe) must not be reported as enabled,
	// otherwise the menu shows a check mark for something that will never start.
	const wchar_t *stored = value;
	while (*stored == L' ' || *stored == L'\t')
		stored++;

	if (*stored == L'"')
		stored++;

	wchar_t stored_path[MAX_PATH];
	size_t length = 0;

	while (stored[length] != L'\0' && stored[length] != L'"' && length < ARRAYSIZE(stored_path) - 1)
	{
		stored_path[length] = stored[length];
		length++;
	}
	stored_path[length] = L'\0';

	if (length == 0)
		return false;

	wchar_t exe_path[MAX_PATH];
	DWORD exe_length = GetModuleFileNameW(NULL, exe_path, ARRAYSIZE(exe_path));
	if (exe_length == 0 || exe_length >= ARRAYSIZE(exe_path))
		return false;

	return _wcsicmp(stored_path, exe_path) == 0;
}

// Enable or disable starting with Windows
LONG autostart_set_enabled(bool enable)
{
	HKEY hKey;
	LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, AUTOSTART_RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL);
	if (result != ERROR_SUCCESS)
		return result;

	if (enable)
	{
		wchar_t exe_path[MAX_PATH];
		DWORD length = GetModuleFileNameW(NULL, exe_path, ARRAYSIZE(exe_path));
		if (length == 0 || length >= ARRAYSIZE(exe_path))
		{
			RegCloseKey(hKey);
			return ERROR_BUFFER_OVERFLOW;
		}

		// Quote the path so that it survives being stored in a REG_SZ value
		wchar_t quoted[MAX_PATH + 2];
		result = StringCchPrintfW(quoted, ARRAYSIZE(quoted), L"\"%s\"", exe_path);
		if (FAILED(result))
		{
			RegCloseKey(hKey);
			return ERROR_BUFFER_OVERFLOW;
		}

		DWORD byte_size = (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t));
		result = RegSetValueExW(hKey, AUTOSTART_VALUE_NAME, 0, REG_SZ, (const BYTE *)quoted, byte_size);
	}
	else
	{
		result = RegDeleteValueW(hKey, AUTOSTART_VALUE_NAME);
		if (result == ERROR_FILE_NOT_FOUND)
			result = ERROR_SUCCESS;
	}

	RegCloseKey(hKey);
	return result;
}
