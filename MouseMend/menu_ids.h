#pragma once

#include <stdbool.h>
#include <windows.h>

// Notification / custom messages
#define WM_NOTIFYICON (WM_USER + 1)

// Registered message used to bring up the menu of an already running instance
#define APP_SHOW_MENU_MSG_NAME L"MouseMend.ShowMenu"

// Command IDs (tray menu items)
#define IDM_EXIT (WM_USER + 10)
#define IDM_TOGGLE_ENABLE (WM_USER + 11)
#define IDM_RESET_STATS (WM_USER + 12)
#define IDM_TOGGLE_HYBRID (WM_USER + 13)
#define IDM_TOGGLE_AUTOSTART (WM_USER + 14)
#define IDM_TOGGLE_NOTIFY (WM_USER + 15)
#define IDM_TOGGLE_LOG (WM_USER + 16)
#define IDM_OPEN_SETTINGS (WM_USER + 17)
#define IDM_OPEN_LOG_FOLDER (WM_USER + 18)
#define IDM_EXPORT_SUPPORT (WM_USER + 19)

// Application private messages
// Posted from the mouse hook callback so the balloon is composed outside the
// hook (a low-level hook that takes too long is silently removed by Windows).
#define WM_APP_NOTIFY_BLOCKED (WM_APP + 10)
#define WM_APP_LOG_FLUSH (WM_APP + 11)
#define WM_APP_STATE_CHANGED (WM_APP + 12)

// Preset menu IDs
#define IDM_PRESET_DEFAULT (WM_USER + 20)
#define IDM_PRESET_OFFICE (WM_USER + 22)
#define IDM_PRESET_STRICT (WM_USER + 23)

// Per-button toggle items: IDM_TOGGLE_BASE + (int)MouseButton
// The encoding relies on the MouseButton enum order, which is also used when the
// menu is generated, so both sides can never drift apart.
#define IDM_TOGGLE_BASE (WM_USER + 30)

// Smart Drag Protection tuning menu IDs
#define IDM_RELEASE_DELAY_BASE (WM_USER + 60)
#define IDM_DRAG_DISTANCE_BASE (WM_USER + 70)
#define IDM_RESTORE_DEFAULTS (WM_USER + 80)

// Self-learning (opt-in). New IDs go after the existing ranges so the smoke
// test's hardcoded IDs and the threshold codec keep their values.
#define IDM_TOGGLE_LEARNING (WM_USER + 81)
#define IDM_ADOPT_LEARNING (WM_USER + 82)
#define IDM_PAUSE_CONTEXT (WM_USER + 83)

// One-click rule for the program that was in front a moment ago. The polling
// thread already keeps its name, so the user does not have to hunt for the exe
// in a file dialog. Two IDs because the same click means opposite things in the
// two lists: "never filter this" vs "only filter this".
#define IDM_EXCLUDE_CURRENT (WM_USER + 84)
#define IDM_ONLY_CURRENT (WM_USER + 91)

// Context modes. The two submenus each have their own "off" entry so a click
// can never be ambiguous about which list the user was looking at.
#define IDM_CONTEXT_MODE_ALL_WHITE (WM_USER + 85)
#define IDM_CONTEXT_MODE_WHITELIST (WM_USER + 86)
#define IDM_CONTEXT_MODE_BLACKLIST (WM_USER + 87)
#define IDM_CONTEXT_ADD_WHITE (WM_USER + 88)
#define IDM_CONTEXT_ADD_BLACK (WM_USER + 89)
#define IDM_CONTEXT_MODE_ALL_BLACK (WM_USER + 90)

// Per-entry list IDs. Declared here so the compile-time guard below can see
// them; keep in sync with CONTEXT_LIST_MAX in context_filter.h.
#define IDM_CONTEXT_LIST_WHITE (WM_USER + 3000)
#define IDM_CONTEXT_LIST_BLACK (WM_USER + 3100)
#define IDM_CONTEXT_LIST_MAX 16

// Pause durations. IDM_PAUSE_CONTEXT became the submenu handle, so the click
// targets moved here: 92-96 was the first free run above IDM_ONLY_CURRENT (91)
// and below IDM_THRESHOLD_BASE (1000), and nothing in the smoke scripts
// hardcodes these.
#define IDM_PAUSE_RESUME (WM_USER + 92)
#define IDM_PAUSE_5 (WM_USER + 93)
#define IDM_PAUSE_10 (WM_USER + 94)
#define IDM_PAUSE_30 (WM_USER + 95)
#define IDM_PAUSE_CUSTOM (WM_USER + 96)

// Posted after a menu closes, so the file dialog does not fight the modal
// TrackPopupMenu loop. A second hop keeps it out of the WM_NULL that follows.
#define WM_APP_ADD_EXE (WM_APP + 13)
#define WM_APP_ADD_EXE_FILE (WM_APP + 14)

// Language menu IDs
#define IDM_LANG_EN (WM_USER + 50)
#define IDM_LANG_ZH (WM_USER + 51)

// About menu IDs
// IDM_ABOUT opens the About window. The project page lives inside that window
// now: it used to be a submenu item, which meant clicking "About" itself did
// nothing at all.
#define IDM_ABOUT (WM_USER + 41)

// Threshold items: IDM_THRESHOLD_BASE + button * IDM_THRESHOLD_SLOTS + index
#define IDM_THRESHOLD_BASE (WM_USER + 1000)
#define IDM_THRESHOLD_SLOTS 10
#define IDM_THRESHOLD_CUSTOM (WM_USER + 2000)

// The preset and custom ranges must never overlap. 8 is the highest button count
// this layout supports (the project currently uses 7).
#if (IDM_THRESHOLD_BASE + 8 * IDM_THRESHOLD_SLOTS) > IDM_THRESHOLD_CUSTOM
#error "IDM threshold ranges overlap: raise IDM_THRESHOLD_CUSTOM"
#endif

// The context lists live after every existing range. These guards exist
// because a silently overlapping ID is very hard to notice: the threshold
// decoder would claim the click first.
#if (IDM_CONTEXT_LIST_WHITE + IDM_CONTEXT_LIST_MAX) > IDM_CONTEXT_LIST_BLACK
#error "context list ID ranges overlap"
#endif
#if (IDM_CONTEXT_LIST_BLACK + IDM_CONTEXT_LIST_MAX) > (WM_USER + 4000)
#error "context list range is too wide"
#endif
#if (IDM_CONTEXT_MODE_ALL_BLACK) >= IDM_THRESHOLD_BASE
#error "context mode IDs collide with the threshold range"
#endif

#define IDM_THRESHOLD_PRESET_ID(button, index) \
	((UINT)(IDM_THRESHOLD_BASE + (int)(button) * IDM_THRESHOLD_SLOTS + (int)(index)))
#define IDM_THRESHOLD_CUSTOM_ID(button) ((UINT)(IDM_THRESHOLD_CUSTOM + (int)(button)))

// Decode a threshold preset menu ID
// Parameters:
//   id - Menu ID received in WM_COMMAND
//   button - Receives the button index
//   index - Receives the preset index
// Returns:
//   true if the ID belongs to the threshold preset range
static __inline bool menu_id_is_threshold_preset(UINT id, int *button, int *index)
{
	if (id < (UINT)IDM_THRESHOLD_BASE || id >= (UINT)IDM_THRESHOLD_CUSTOM)
		return false;

	UINT offset = id - (UINT)IDM_THRESHOLD_BASE;
	*button = (int)(offset / IDM_THRESHOLD_SLOTS);
	*index = (int)(offset % IDM_THRESHOLD_SLOTS);
	return true;
}

// Decode a custom threshold menu ID
// Parameters:
//   id - Menu ID received in WM_COMMAND
//   button - Receives the button index
//   button_count - Number of valid buttons
// Returns:
//   true if the ID belongs to the custom threshold range
static __inline bool menu_id_is_threshold_custom(UINT id, int *button, int button_count)
{
	if (id < (UINT)IDM_THRESHOLD_CUSTOM || id >= (UINT)IDM_THRESHOLD_CUSTOM + (UINT)button_count)
		return false;

	*button = (int)(id - (UINT)IDM_THRESHOLD_CUSTOM);
	return true;
}
