#include "context_menu.h"
#include "../../menu_ids.h"
#include "../../version.h"
#include "../core/config.h"
#include "../utils/autostart.h"
#include "language.h"
#include <strsafe.h>

// Constants
#define MENU_TEXT_BUFFER_SIZE 64
#define STATISTICS_BUFFER_SIZE 128

#if THRESHOLD_PRESET_COUNT > IDM_THRESHOLD_SLOTS
#error "THRESHOLD_PRESET_COUNT must not exceed IDM_THRESHOLD_SLOTS"
#endif

// Buttons listed in the menu, in MouseButton order (the encoding of
// IDM_TOGGLE_BASE relies on that order, which is why it is generated here too).
static const MouseButton MENU_BUTTONS[] = {
	MOUSE_BUTTON_LEFT,
	MOUSE_BUTTON_RIGHT,
	MOUSE_BUTTON_MIDDLE,
	MOUSE_BUTTON_X1,
	MOUSE_BUTTON_X2,
	MOUSE_BUTTON_WHEEL,
	MOUSE_BUTTON_HWHEEL,
};

#define MENU_BUTTON_COUNT ((int)(sizeof(MENU_BUTTONS) / sizeof(MENU_BUTTONS[0])))

// Add the submenu of a single button (enable toggle + threshold presets)
// Parameters:
//   hMenu - Parent menu
//   button - Button this submenu is for
//   debounce - Pointer to DebounceManager for current state
// Returns:
//   true on success
static bool AddButtonSubmenu(HMENU hMenu, MouseButton button, const DebounceManager *debounce)
{
	HMENU hSubMenu = CreatePopupMenu();
	if (!hSubMenu)
		return false;

	const ButtonDebounceData *data = &debounce->buttons[button];
	wchar_t text[MENU_TEXT_BUFFER_SIZE];

	// Enable/disable this button
	UINT toggle_flags = MF_BYPOSITION | MF_STRING;
	if (data->isMonitored)
		toggle_flags |= MF_CHECKED;

	if (!InsertMenuW(hSubMenu, -1, toggle_flags, IDM_TOGGLE_BASE + (UINT)button, lang_str(STR_ENABLE)))
	{
		DestroyMenu(hSubMenu);
		return false;
	}

	InsertMenuW(hSubMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);

	// Quick-pick thresholds
	const int *presets = config_threshold_presets(button);
	bool is_custom = true;

	for (int i = 0; i < THRESHOLD_PRESET_COUNT; i++)
	{
		if (data->thresholdMs == (uint32_t)presets[i])
			is_custom = false;

		UINT flags = MF_BYPOSITION | MF_STRING;
		if (data->thresholdMs == (uint32_t)presets[i])
			flags |= MF_CHECKED;

		InsertMenuW(hSubMenu, -1, flags, IDM_THRESHOLD_PRESET_ID(button, i), lang_format_ms(presets[i]));
	}

	InsertMenuW(hSubMenu, -1, MF_BYPOSITION | MF_STRING | (is_custom ? MF_CHECKED : 0),
				IDM_THRESHOLD_CUSTOM_ID(button), lang_str(STR_MENU_CUSTOM));

	// Title: "<button name> (<threshold>)" with a localized unit
	StringCchPrintfW(text, MENU_TEXT_BUFFER_SIZE, lang_str(STR_BUTTON_TITLE_FMT),
					 lang_button_name(button), lang_format_ms((int)data->thresholdMs));

	UINT menu_flags = MF_BYPOSITION | MF_POPUP;
	if (data->isMonitored)
		menu_flags |= MF_CHECKED;

	if (!InsertMenuW(hMenu, -1, menu_flags, (UINT_PTR)hSubMenu, text))
	{
		DestroyMenu(hSubMenu);
		return false;
	}

	return true;
}

// Add a preset item to the presets submenu
// Parameters:
//   hMenu - Presets submenu
//   preset - Preset configuration to display
//   format - Localized format string containing a single %d
//   menu_id - Menu ID to assign
static void AddPresetItem(HMENU hMenu, const PresetConfig *preset, const wchar_t *format, UINT menu_id)
{
	wchar_t text[MENU_TEXT_BUFFER_SIZE];
	const wchar_t *value = lang_format_ms(preset->thresholds[MOUSE_BUTTON_LEFT]);
	StringCchPrintfW(text, MENU_TEXT_BUFFER_SIZE, format, value);
	InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, menu_id, text);
}

// Add a quick-pick list to a submenu
// Parameters:
//   hMenu - Target submenu
//   values - Value list
//   count - Number of values
//   current - Currently active value
//   base_id - Menu ID of the first value
//   use_px - true to render "<n> px", false for "<n> ms"
static void AddTuningItems(HMENU hMenu, const int *values, int count, uint32_t current, UINT base_id, bool use_px)
{
	for (int i = 0; i < count; i++)
	{
		UINT flags = MF_BYPOSITION | MF_STRING;
		if (current == (uint32_t)values[i])
			flags |= MF_CHECKED;

		const wchar_t *label = use_px ? lang_format_px(values[i]) : lang_format_ms(values[i]);
		InsertMenuW(hMenu, -1, flags, base_id + (UINT)i, label);
	}
}

// Create and populate menu items
bool context_menu_create(ContextMenuManager *manager, DebounceManager *debounce,
						 const Preferences *prefs, bool hook_active)
{
	if (!manager || !debounce || !prefs)
		return false;

	if (manager->menu)
		context_menu_destroy(manager);

	manager->menu = CreatePopupMenu();
	if (!manager->menu)
		return false;

	wchar_t text[STATISTICS_BUFFER_SIZE];

	// Current state (informational, not clickable) so the user can tell at a
	// glance whether the protection is actually doing something. These lines are
	// intentionally NOT grayed: gray text reads as "broken" to most users.
	const wchar_t *state_text;
	if (!hook_active)
		state_text = lang_str(STR_STATUS_UNAVAILABLE);
	else if (!debounce_is_any_monitored(debounce))
		state_text = lang_str(STR_TOOLTIP_OFF);
	else if (debounce_is_all_monitored(debounce))
		state_text = lang_str(STR_TOOLTIP_ON);
	else
		state_text = lang_str(STR_TOOLTIP_PARTIAL);

	StringCchPrintfW(text, STATISTICS_BUFFER_SIZE, lang_str(STR_STATUS_FMT), state_text);
	InsertMenuW(manager->menu, -1, MF_BYPOSITION | MF_STRING, 0, text);

	// Blocked statistics. When something has actually been blocked the line turns
	// into a submenu that breaks the total down per button, so hovering it reveals
	// which switch is failing. With a zero counter there is nothing to break down,
	// so it stays a plain label with no arrow - exactly the "cleared, so nothing
	// to show" behaviour the rest of the app already follows.
	StringCchPrintfW(text, STATISTICS_BUFFER_SIZE, lang_str(STR_STATS_FMT), debounce_get_total_blocks(debounce));

	MouseButton blocked_buttons[MOUSE_BUTTON_COUNT];
	int blocked_count = debounce_collect_blocked_buttons(debounce, blocked_buttons, MOUSE_BUTTON_COUNT);

	if (blocked_count == 0)
	{
		InsertMenuW(manager->menu, -1, MF_BYPOSITION | MF_STRING, 0, text);
	}
	else
	{
		HMENU hStats = CreatePopupMenu();

		if (!hStats)
		{
			context_menu_destroy(manager);
			return false;
		}

		for (int i = 0; i < blocked_count; i++)
		{
			MouseButton button = blocked_buttons[i];
			wchar_t line[MENU_TEXT_BUFFER_SIZE];

			lang_format_blocked_button(line, ARRAYSIZE(line), button,
									   debounce_get_button_blocks(debounce, button));

			// ID 0 keeps the row inert but looking like a normal row; a grayed row
			// would read as "this is broken" instead of "this is information".
			InsertMenuW(hStats, -1, MF_BYPOSITION | MF_STRING, 0, line);
		}

		if (!InsertMenuW(manager->menu, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hStats, text))
		{
			// InsertMenu does not take ownership when it fails, so the submenu has
			// to be released here or it would leak a menu handle.
			DestroyMenu(hStats);
			context_menu_destroy(manager);
			return false;
		}
	}

	InsertMenuW(manager->menu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);

	// Per-button submenus
	for (int i = 0; i < MENU_BUTTON_COUNT; i++)
	{
		if (!AddButtonSubmenu(manager->menu, MENU_BUTTONS[i], debounce))
		{
			context_menu_destroy(manager);
			return false;
		}
	}

	InsertMenuW(manager->menu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);

	// Global toggles
	bool is_enabled = debounce_is_any_monitored(debounce);
	InsertMenuW(manager->menu, -1, MF_BYPOSITION | MF_STRING, IDM_TOGGLE_ENABLE,
				is_enabled ? lang_str(STR_DISABLE_ALL) : lang_str(STR_ENABLE_ALL));

	UINT hybrid_flags = MF_BYPOSITION | MF_STRING;
	if (debounce->use_hybrid_heuristic)
		hybrid_flags |= MF_CHECKED;
	InsertMenuW(manager->menu, -1, hybrid_flags, IDM_TOGGLE_HYBRID, lang_str(STR_SMART_DRAG));

	UINT autostart_flags = MF_BYPOSITION | MF_STRING;
	if (autostart_is_enabled())
		autostart_flags |= MF_CHECKED;
	InsertMenuW(manager->menu, -1, autostart_flags, IDM_TOGGLE_AUTOSTART, lang_str(STR_AUTOSTART));

	InsertMenuW(manager->menu, -1, MF_BYPOSITION | MF_STRING, IDM_RESET_STATS, lang_str(STR_RESET_STATS));

	InsertMenuW(manager->menu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);

	// Presets submenu
	HMENU hPresets = CreatePopupMenu();
	if (!hPresets)
	{
		context_menu_destroy(manager);
		return false;
	}

	AddPresetItem(hPresets, &CONFIG_PRESET_DEFAULT, lang_str(STR_PRESET_DEFAULT_FMT), IDM_PRESET_DEFAULT);
	AddPresetItem(hPresets, &CONFIG_PRESET_OFFICE, lang_str(STR_PRESET_OFFICE_FMT), IDM_PRESET_OFFICE);
	AddPresetItem(hPresets, &CONFIG_PRESET_STRICT, lang_str(STR_PRESET_STRICT_FMT), IDM_PRESET_STRICT);

	InsertMenuW(manager->menu, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hPresets, lang_str(STR_PRESETS));

	// Advanced submenu: Smart Drag Protection tuning + restore defaults
	HMENU hAdvanced = CreatePopupMenu();
	if (!hAdvanced)
	{
		context_menu_destroy(manager);
		return false;
	}

	HMENU hReleaseDelay = CreatePopupMenu();
	if (!hReleaseDelay)
	{
		DestroyMenu(hAdvanced);
		context_menu_destroy(manager);
		return false;
	}

	AddTuningItems(hReleaseDelay, config_release_delay_presets(), RELEASE_DELAY_PRESET_COUNT,
				   debounce->releaseDelayMs, IDM_RELEASE_DELAY_BASE, false);

	InsertMenuW(hAdvanced, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hReleaseDelay, lang_str(STR_RELEASE_DELAY));

	HMENU hDragDistance = CreatePopupMenu();
	if (!hDragDistance)
	{
		DestroyMenu(hAdvanced);
		context_menu_destroy(manager);
		return false;
	}

	AddTuningItems(hDragDistance, config_drag_distance_presets(), DRAG_DISTANCE_PRESET_COUNT,
				   debounce->dragDistancePx, IDM_DRAG_DISTANCE_BASE, true);

	InsertMenuW(hAdvanced, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hDragDistance, lang_str(STR_DRAG_DISTANCE));

	InsertMenuW(hAdvanced, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);

	// Optional feedback: the whole point of the app is that it works silently,
	// so the user has to opt in to being told about it.
	UINT notify_flags = MF_BYPOSITION | MF_STRING;
	if (prefs->notify_blocks)
		notify_flags |= MF_CHECKED;
	InsertMenuW(hAdvanced, -1, notify_flags, IDM_TOGGLE_NOTIFY, lang_str(STR_NOTIFY_BLOCKS));

	UINT log_flags = MF_BYPOSITION | MF_STRING;
	if (prefs->log_blocks)
		log_flags |= MF_CHECKED;
	InsertMenuW(hAdvanced, -1, log_flags, IDM_TOGGLE_LOG, lang_str(STR_LOG_BLOCKS));

	// Only meaningful while the history is enabled
	UINT open_log_flags = MF_BYPOSITION | MF_STRING;
	if (!prefs->log_blocks)
		open_log_flags |= MF_GRAYED;
	InsertMenuW(hAdvanced, -1, open_log_flags, IDM_OPEN_LOG_FOLDER, lang_str(STR_OPEN_LOG_FOLDER));

	// Always enabled, even when the history is off: this is how a user asks for
	// help, and it must not depend on an unrelated setting.
	InsertMenuW(hAdvanced, -1, MF_BYPOSITION | MF_STRING, IDM_EXPORT_SUPPORT, lang_str(STR_EXPORT_SUPPORT));

	InsertMenuW(hAdvanced, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);

	InsertMenuW(hAdvanced, -1, MF_BYPOSITION | MF_STRING, IDM_RESTORE_DEFAULTS, lang_str(STR_RESTORE_DEFAULTS));

	InsertMenuW(manager->menu, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hAdvanced, lang_str(STR_ADVANCED));

	// Language submenu
	Language current_language = language_get();

	HMENU hLanguages = CreatePopupMenu();
	if (!hLanguages)
	{
		context_menu_destroy(manager);
		return false;
	}

	UINT english_flags = MF_BYPOSITION | MF_STRING;
	if (current_language == LANG_EN)
		english_flags |= MF_CHECKED;
	InsertMenuW(hLanguages, -1, english_flags, IDM_LANG_EN, lang_language_name(LANG_EN));

	UINT chinese_flags = MF_BYPOSITION | MF_STRING;
	if (current_language == LANG_ZH_CN)
		chinese_flags |= MF_CHECKED;
	InsertMenuW(hLanguages, -1, chinese_flags, IDM_LANG_ZH, lang_language_name(LANG_ZH_CN));

	InsertMenuW(manager->menu, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hLanguages, lang_str(STR_LANGUAGE));

	InsertMenuW(manager->menu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);

	// About: a real command, not a submenu title
	// A popup title cannot be clicked, so the old submenu left the user staring at
	// a menu that did nothing when he picked "About".
	StringCchPrintfW(text, STATISTICS_BUFFER_SIZE, lang_str(STR_MENU_ABOUT_FMT),
					 APP_VERSION_STRING_W);

	InsertMenuW(manager->menu, -1, MF_BYPOSITION | MF_STRING, IDM_ABOUT, text);

	InsertMenuW(manager->menu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);

	// Settings window (the menu stays the fast path for everyday changes)
	InsertMenuW(manager->menu, -1, MF_BYPOSITION | MF_STRING, IDM_OPEN_SETTINGS, lang_str(STR_OPEN_SETTINGS));

	// Exit
	InsertMenuW(manager->menu, -1, MF_BYPOSITION | MF_STRING, IDM_EXIT, lang_str(STR_EXIT));

	return true;
}

// Show context menu at specified position
bool context_menu_show(ContextMenuManager *manager, HWND hwnd, int x, int y,
					   DebounceManager *debounce, const Preferences *prefs, bool hook_active)
{
	if (!manager || !hwnd)
		return false;

	if (!context_menu_create(manager, debounce, prefs, hook_active))
		return false;

	SetForegroundWindow(hwnd);
	TrackPopupMenu(manager->menu, TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, x, y, 0, hwnd, NULL);

	// Required so the menu closes when the user clicks elsewhere
	PostMessageW(hwnd, WM_NULL, 0, 0);

	return true;
}

// Destroy context menu
void context_menu_destroy(ContextMenuManager *manager)
{
	if (!manager)
		return;

	if (manager->menu)
	{
		DestroyMenu(manager->menu);
		manager->menu = NULL;
	}
}
