#include "settings.h"
#include <strsafe.h>

#define SETTINGS_VALUE_NAME_SIZE 64

// Read a REG_DWORD value, validating both type and size
// Parameters:
//   hKey - Open key
//   value_name - Name of the value
//   value - Receives the stored value
// Returns:
//   true when a valid DWORD was read
static bool read_dword(HKEY hKey, LPCWSTR value_name, DWORD *value)
{
	DWORD type = 0;
	DWORD size = sizeof(DWORD);
	DWORD data = 0;

	if (RegQueryValueExW(hKey, value_name, NULL, &type, (BYTE *)&data, &size) != ERROR_SUCCESS)
		return false;

	if (type != REG_DWORD || size != sizeof(DWORD))
		return false;

	*value = data;
	return true;
}

// Write a REG_DWORD value
// Returns:
//   true when the value was stored
static bool write_dword(HKEY hKey, LPCWSTR value_name, DWORD value)
{
	return RegSetValueExW(hKey, value_name, 0, REG_DWORD, (const BYTE *)&value, sizeof(DWORD)) == ERROR_SUCCESS;
}

// Load the persisted settings
bool settings_load(DebounceManager *debounce, Preferences *prefs)
{
	if (!debounce)
		return false;

	HKEY hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_SETTINGS_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
		return false;

	bool loaded = false;
	int button_entries = 0;
	DWORD value = 0;

	if (read_dword(hKey, L"Language", &value))
	{
		if (prefs && value < (DWORD)LANG_COUNT)
			prefs->language = (Language)value;

		loaded = true;
	}

	if (read_dword(hKey, L"HybridHeuristic", &value))
	{
		debounce_set_hybrid_heuristic(debounce, value != 0);
		loaded = true;
	}

	if (read_dword(hKey, L"ReleaseDelay", &value))
	{
		// 50 and 20 were the defaults before this build, and every save wrote all
		// values out, so a stored one of those says nothing about intent - it is
		// what the old build put there on its first save. Both were also long
		// enough to leave a dragged window following the cursor after the release,
		// which is the complaint the default of 0 answers. Treat them as "never
		// chosen" and start from the current default; 40 and 80 are left alone,
		// they can only be a deliberate choice.
		if (value == 50 || value == 20)
			value = RELEASE_DELAY_DEFAULT_MS;

		debounce_set_release_delay(debounce, value, RELEASE_DELAY_MIN_MS, RELEASE_DELAY_MAX_MS);
		loaded = true;
	}

	if (read_dword(hKey, L"DragDistance", &value))
	{
		debounce_set_drag_distance(debounce, value, DRAG_DISTANCE_MIN_PX, DRAG_DISTANCE_MAX_PX);
		loaded = true;
	}

	if (read_dword(hKey, L"IntroShown", &value))
	{
		if (prefs)
			prefs->intro_shown = (value != 0);

		loaded = true;
	}

	if (read_dword(hKey, L"NotifyBlocked", &value))
	{
		if (prefs)
			prefs->notify_blocks = (value != 0);

		loaded = true;
	}

	if (read_dword(hKey, L"LogBlocked", &value))
	{
		if (prefs)
			prefs->log_blocks = (value != 0);

		loaded = true;
	}

	if (read_dword(hKey, L"LearningEnabled", &value))
	{
		if (prefs)
			prefs->learning_enabled = (value != 0);

		loaded = true;
	}

	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		wchar_t value_name[SETTINGS_VALUE_NAME_SIZE];

		StringCchPrintfW(value_name, ARRAYSIZE(value_name), L"Btn%d_Threshold", i);
		if (read_dword(hKey, value_name, &value))
		{
			debounce_set_threshold(debounce, (MouseButton)i, value, THRESHOLD_MIN_VALUE, THRESHOLD_MAX_VALUE);
			loaded = true;
		}

		StringCchPrintfW(value_name, ARRAYSIZE(value_name), L"Btn%d_Enabled", i);
		if (read_dword(hKey, value_name, &value))
		{
			debounce_set_monitored(debounce, (MouseButton)i, value != 0);
			loaded = true;
			button_entries++;
		}
	}

	RegCloseKey(hKey);

	// A partially deleted profile (cleanup tools, migrating from an older build)
	// must count as "not configured": otherwise every button stays disabled while
	// the tray icon still claims that the protection is on.
	return loaded && button_entries > 0;
}

// Persist the current state
bool settings_save(const DebounceManager *debounce, const Preferences *prefs)
{
	if (!debounce || !prefs)
		return false;

	HKEY hKey;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_SETTINGS_KEY, 0, NULL, REG_OPTION_NON_VOLATILE,
						KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS)
		return false;

	bool success = true;

	success = write_dword(hKey, L"HybridHeuristic", debounce->use_hybrid_heuristic ? 1 : 0) && success;
	success = write_dword(hKey, L"Language", (DWORD)prefs->language) && success;
	success = write_dword(hKey, L"ReleaseDelay", debounce->releaseDelayMs) && success;
	success = write_dword(hKey, L"DragDistance", debounce->dragDistancePx) && success;
	success = write_dword(hKey, L"IntroShown", prefs->intro_shown ? 1 : 0) && success;
	success = write_dword(hKey, L"NotifyBlocked", prefs->notify_blocks ? 1 : 0) && success;
	success = write_dword(hKey, L"LogBlocked", prefs->log_blocks ? 1 : 0) && success;
	success = write_dword(hKey, L"LearningEnabled", prefs->learning_enabled ? 1 : 0) && success;

	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		wchar_t value_name[SETTINGS_VALUE_NAME_SIZE];

		StringCchPrintfW(value_name, ARRAYSIZE(value_name), L"Btn%d_Threshold", i);
		success = write_dword(hKey, value_name, debounce->buttons[i].thresholdMs) && success;

		StringCchPrintfW(value_name, ARRAYSIZE(value_name), L"Btn%d_Enabled", i);
		success = write_dword(hKey, value_name, debounce->buttons[i].isMonitored ? 1 : 0) && success;
	}

	RegCloseKey(hKey);
	return success;
}
