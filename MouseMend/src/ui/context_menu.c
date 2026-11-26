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

		// Zero is not "0 ms", it is the setting that turns the behaviour off
		const wchar_t *label = use_px ? lang_format_px(values[i])
								 : (values[i] == 0 ? lang_str(STR_TUNING_OFF) : lang_format_ms(values[i]));
		InsertMenuW(hMenu, -1, flags, base_id + (UINT)i, label);
	}
}

// Build the whitelist or blacklist submenu: the two mode entries, the entries
// themselves and the "add" entry. Returns false when a menu handle was refused.
static bool AddContextSubmenu(HMENU parent, const ContextPolicy *policy, bool whitelist)
{
	const ContextPathList *list = whitelist ? &policy->whitelist : &policy->blacklist;
	ContextMode own_mode = whitelist ? CONTEXT_MODE_WHITELIST : CONTEXT_MODE_BLACKLIST;
	LangStringId own_name = whitelist ? STR_CONTEXT_MODE_WHITELIST : STR_CONTEXT_MODE_BLACKLIST;
	LangStringId off_name = whitelist ? STR_CONTEXT_MODE_ALL_WHITE : STR_CONTEXT_MODE_ALL_BLACK;
	UINT off_id = whitelist ? IDM_CONTEXT_MODE_ALL_WHITE : IDM_CONTEXT_MODE_ALL_BLACK;
	UINT add_id = whitelist ? IDM_CONTEXT_ADD_WHITE : IDM_CONTEXT_ADD_BLACK;
	UINT item_base = whitelist ? IDM_CONTEXT_LIST_WHITE : IDM_CONTEXT_LIST_BLACK;

	HMENU submenu = CreatePopupMenu();
	if (!submenu)
		return false;

	InsertMenuW(submenu, -1, MF_BYPOSITION | MF_STRING, off_id, lang_str(off_name));
	InsertMenuW(submenu, -1, MF_BYPOSITION | MF_STRING, own_mode, lang_str(own_name));

	// Radio inside this submenu only; Win32 has no cross-submenu group.
	//
	// The check follows THIS list's mode, not "anything but ALL". The old test
	// put the check on the blacklist entry whenever the whitelist was in force,
	// so the submenu said both "(inactive)" in its title and "selected" on the
	// radio inside it.
	CheckMenuRadioItem(submenu, 0, 1, policy->mode == own_mode ? 1 : 0, MF_BYPOSITION);

	InsertMenuW(submenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);

	uint32_t legacy = 0;
	for (uint32_t i = 0; i < list->count; i++)
	{
		if (list->paths[i][0] == L'*' && list->paths[i][1] == L'\\')
			legacy++;
	}

	if (legacy > 0)
	{
		wchar_t line[128];
		if (SUCCEEDED(StringCchPrintfW(line, ARRAYSIZE(line), lang_str(STR_CONTEXT_MIGRATED_FMT), legacy)))
			InsertMenuW(submenu, -1, MF_BYPOSITION | MF_STRING, 0, line);
	}

	if (list->count == 0)
	{
		InsertMenuW(submenu, -1, MF_BYPOSITION | MF_STRING, 0, lang_str(STR_CONTEXT_EMPTY));

		// An empty whitelist is not "nothing configured yet": with the whitelist
		// active it means no program is protected at all. Saying only "no programs
		// added" let the user believe the filter was working.
		if (whitelist && policy->mode == CONTEXT_MODE_WHITELIST)
			InsertMenuW(submenu, -1, MF_BYPOSITION | MF_STRING, 0,
						lang_str(STR_CONTEXT_WHITE_EMPTY_WARN));
	}
	else
	{
		for (uint32_t i = 0; i < list->count; i++)
		{
			wchar_t line[CONTEXT_NAME_CHARS + 32];

			if (list->paths[i][0] == L'*' && list->paths[i][1] == L'\\')
			{
				if (FAILED(StringCchPrintfW(line, ARRAYSIZE(line), lang_str(STR_CONTEXT_LEGACY_FMT),
											list->paths[i] + 2)))
					continue;
			}
			else
			{
				context_file_name_from_path(list->paths[i], line, ARRAYSIZE(line));
			}

			InsertMenuW(submenu, -1, MF_BYPOSITION | MF_STRING, item_base + i, line);
		}
	}

	InsertMenuW(submenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);

	// One-click rule for the program that was in front a moment ago. The polling
	// thread already resolved its name, so the user does not have to find the exe
	// in a file dialog - which is the whole difficulty of using these lists.
	if (policy->recent_name[0])
	{
		wchar_t line[CONTEXT_NAME_CHARS + 64];
		LangStringId recent_fmt = whitelist ? STR_CONTEXT_ONLY_FMT : STR_CONTEXT_EXCLUDE_FMT;
		UINT recent_id = whitelist ? IDM_ONLY_CURRENT : IDM_EXCLUDE_CURRENT;

		if (SUCCEEDED(StringCchPrintfW(line, ARRAYSIZE(line), lang_str(recent_fmt),
									   policy->recent_name)))
			InsertMenuW(submenu, -1, MF_BYPOSITION | MF_STRING, recent_id, line);
	}

	// A full list greys the entry out here, instead of letting the user hunt for
	// an exe in the file dialog and only then be told there was no room.
	UINT add_flags = MF_BYPOSITION | MF_STRING;
	if (list->count >= CONTEXT_LIST_MAX)
		add_flags |= MF_GRAYED;

	InsertMenuW(submenu, -1, add_flags, add_id, lang_str(STR_CONTEXT_ADD));
	InsertMenuW(submenu, -1, MF_BYPOSITION | MF_STRING, 0, lang_str(STR_CONTEXT_DELAY));

	wchar_t title[128];
	LangStringId mode_name = whitelist ? STR_WHITELIST_MODE : STR_BLACKLIST_MODE;

	// "Inactive" is about this submenu, not about "all programs". Marking both
	// submenus as idle only when the mode is ALL made the blacklist look enabled
	// while the whitelist was the one in force.
	if (policy->mode != own_mode)
	{
		StringCchPrintfW(title, ARRAYSIZE(title), L"%s%s (%u)", lang_str(mode_name),
						 lang_str(STR_CONTEXT_DISABLED_SUFFIX), (unsigned)list->count);
	}
	else
	{
		StringCchPrintfW(title, ARRAYSIZE(title), L"%s (%u)", lang_str(mode_name),
						 (unsigned)list->count);
	}

	if (!InsertMenuW(parent, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)submenu, title))
	{
		DestroyMenu(submenu);
		return false;
	}

	return true;
}

// The self-learning submenu: one switch, then the per-key suggestion.
static bool AddLearningSubmenu(HMENU parent, const DebounceManager *debounce,
							   const Preferences *prefs, const LearningAdvice *advice)
{
	HMENU submenu = CreatePopupMenu();
	if (!submenu)
		return false;

	UINT flags = MF_BYPOSITION | MF_STRING;
	if (prefs->learning_enabled)
		flags |= MF_CHECKED;
	InsertMenuW(submenu, -1, flags, IDM_TOGGLE_LEARNING, lang_str(STR_LEARNING_ENABLE));

	InsertMenuW(submenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);

	static const MouseButton shown[] = {MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE};
	bool any = false;

	if (!prefs->learning_enabled)
	{
		// Off is a state, not an empty result. Saying "not enough samples" while
		// nothing was being sampled sent the user waiting for data that could
		// never arrive - which is exactly how it looked broken.
		InsertMenuW(submenu, -1, MF_BYPOSITION | MF_STRING, 0,
					lang_str(STR_LEARNING_OFF_HINT));
	}
	else
	{
		// Three columns, one row per button: name, current->suggested, samples.
		//
		// The suggested value is the whole reason to open this menu - without it
		// the user cannot tell what "adopt" would do. The sample count is the
		// confidence. Everything else (hold time, units) lives in the check-up
		// window; this menu has to stay narrow.
		//
		// A button with no suggestion still gets a row. A key that disappears is
		// indistinguishable from a key the build forgot about, and the sample
		// count is what tells "0, never sampled" from "12, not enough yet".
		for (int i = 0; i < (int)ARRAYSIZE(shown); i++)
		{
			MouseButton button = shown[i];
			uint32_t suggested = advice ? advice->suggested_ms[button] : 0;
			uint32_t samples = advice ? advice->samples[button] : 0;

			wchar_t line[128];

			if (suggested != 0)
			{
				if (SUCCEEDED(StringCchPrintfW(line, ARRAYSIZE(line), lang_str(STR_LEARNING_KEY_FMT),
											   lang_button_short_name(button),
											   (unsigned)debounce->buttons[button].thresholdMs,
											   (unsigned)suggested,
											   (unsigned)samples)))
				{
					InsertMenuW(submenu, -1, MF_BYPOSITION | MF_STRING, 0, line);
					any = true;
				}
			}
			else if (SUCCEEDED(StringCchPrintfW(line, ARRAYSIZE(line),
												lang_str(STR_LEARNING_ROW_PENDING_FMT),
												lang_button_short_name(button),
												(unsigned)samples)))
			{
				// Deliberately does NOT set "any": the adopt entry has to stay
				// gray while there is nothing to adopt. Setting it here was the
				// difference between "grayed out" and "clickable but does nothing".
				InsertMenuW(submenu, -1, MF_BYPOSITION | MF_STRING, 0, line);
			}
		}
	}

	InsertMenuW(submenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);

	// Each button has its own sample minimum. A shared counter used to enable
	// this entry once the three buttons together reached it, which happened long
	// before any single button had enough data - clicking then did nothing.
	UINT adopt_flags = MF_BYPOSITION | MF_STRING;
	if (!any)
		adopt_flags |= MF_GRAYED;
	InsertMenuW(submenu, -1, adopt_flags, IDM_ADOPT_LEARNING, lang_str(STR_ADOPT_LEARNING));

	if (!InsertMenuW(parent, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)submenu, lang_str(STR_LEARNING)))
	{
		DestroyMenu(submenu);
		return false;
	}

	return true;
}

// Create and populate menu items
bool context_menu_create(ContextMenuManager *manager, DebounceManager *debounce,
						 const Preferences *prefs, bool hook_active,
						 const LearningAdvice *advice, const ContextPolicy *policy,
						 bool paused)
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

	// Three mode submenus replace the "start with Windows" entry, which moved
	// into Advanced: the first level grows by two rows only.
	if (!AddLearningSubmenu(manager->menu, debounce, prefs, advice))
	{
		context_menu_destroy(manager);
		return false;
	}

	if (policy)
	{
		if (!AddContextSubmenu(manager->menu, policy, true) ||
			!AddContextSubmenu(manager->menu, policy, false))
		{
			context_menu_destroy(manager);
			return false;
		}
	}

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

	// One entry, two meanings: pausing something the user cannot see the state of
	// is how a temporary workaround becomes permanent by accident.
	InsertMenuW(hAdvanced, -1, MF_BYPOSITION | MF_STRING, IDM_PAUSE_CONTEXT,
				lang_str(paused ? STR_RESUME_CONTEXT : STR_PAUSE_CONTEXT));

	// Moved down from the first level so the three mode submenus could take
	// its place; it is still one click away.
	UINT autostart_flags = MF_BYPOSITION | MF_STRING;
	if (autostart_is_enabled())
		autostart_flags |= MF_CHECKED;
	InsertMenuW(hAdvanced, -1, autostart_flags, IDM_TOGGLE_AUTOSTART, lang_str(STR_AUTOSTART));

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
					   DebounceManager *debounce, const Preferences *prefs, bool hook_active,
					   const LearningAdvice *advice, const ContextPolicy *policy, bool paused)
{
	if (!manager || !hwnd)
		return false;

	if (!context_menu_create(manager, debounce, prefs, hook_active, advice, policy, paused))
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
