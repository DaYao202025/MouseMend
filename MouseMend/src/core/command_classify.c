#include "command_classify.h"

#include "../../menu_ids.h"
#include "config.h"
#include "mouse_hook.h"

CommandGroup command_classify(UINT menu_id)
{
	// Id 0 is the inert label row used throughout the menus ("no programs
	// added", "not enough samples", ...). It is not a command and not an error.
	if (menu_id == 0)
		return COMMAND_GROUP_NONE;

	switch (menu_id)
	{
	case IDM_ABOUT:
	case IDM_EXIT:
	case IDM_TOGGLE_ENABLE:
	case IDM_TOGGLE_HYBRID:
	case IDM_TOGGLE_AUTOSTART:
	case IDM_TOGGLE_NOTIFY:
	case IDM_TOGGLE_LOG:
	case IDM_OPEN_LOG_FOLDER:
	case IDM_EXPORT_SUPPORT:
	case IDM_OPEN_SETTINGS:
	case IDM_RESET_STATS:
	case IDM_PRESET_DEFAULT:
	case IDM_PRESET_OFFICE:
	case IDM_PRESET_STRICT:
	case IDM_RESTORE_DEFAULTS:
	case IDM_LANG_EN:
	case IDM_LANG_ZH:
		return COMMAND_GROUP_MENU;

	case IDM_TOGGLE_LEARNING:
	case IDM_ADOPT_LEARNING:
		return COMMAND_GROUP_LEARNING;

	case IDM_PAUSE_CONTEXT:
	case IDM_PAUSE_RESUME:
	case IDM_PAUSE_5:
	case IDM_PAUSE_10:
	case IDM_PAUSE_30:
	case IDM_PAUSE_CUSTOM:
	case IDM_EXCLUDE_CURRENT:
	case IDM_ONLY_CURRENT:
	case IDM_CONTEXT_MODE_ALL_WHITE:
	case IDM_CONTEXT_MODE_WHITELIST:
	case IDM_CONTEXT_MODE_BLACKLIST:
	case IDM_CONTEXT_MODE_ALL_BLACK:
	case IDM_CONTEXT_ADD_WHITE:
	case IDM_CONTEXT_ADD_BLACK:
		return COMMAND_GROUP_LIST;

	default:
		break;
	}

	// Ranges. Every one of them carries its upper bound on purpose: a bare
	// "menu_id >= BASE" would pull every higher id into this group, and the
	// decoder inside the handler would then reject it without a word - exactly
	// the silent failure this classifier is supposed to make impossible.
	if (menu_id >= (UINT)IDM_TOGGLE_BASE &&
		menu_id < (UINT)IDM_TOGGLE_BASE + (UINT)MOUSE_BUTTON_COUNT)
		return COMMAND_GROUP_THRESHOLD;

	if (menu_id >= (UINT)IDM_RELEASE_DELAY_BASE &&
		menu_id < (UINT)IDM_RELEASE_DELAY_BASE + (UINT)RELEASE_DELAY_PRESET_COUNT)
		return COMMAND_GROUP_THRESHOLD;

	if (menu_id >= (UINT)IDM_DRAG_DISTANCE_BASE &&
		menu_id < (UINT)IDM_DRAG_DISTANCE_BASE + (UINT)DRAG_DISTANCE_PRESET_COUNT)
		return COMMAND_GROUP_THRESHOLD;

	// The list ranges use IDM_CONTEXT_LIST_MAX, not CONTEXT_LIST_MAX. They happen
	// to be equal today; the menu-id one is the one the ids are generated from.
	if (menu_id >= (UINT)IDM_CONTEXT_LIST_WHITE &&
		menu_id < (UINT)IDM_CONTEXT_LIST_WHITE + (UINT)IDM_CONTEXT_LIST_MAX)
		return COMMAND_GROUP_LIST;

	if (menu_id >= (UINT)IDM_CONTEXT_LIST_BLACK &&
		menu_id < (UINT)IDM_CONTEXT_LIST_BLACK + (UINT)IDM_CONTEXT_LIST_MAX)
		return COMMAND_GROUP_LIST;

	int button = 0;
	int index = 0;

	if (menu_id_is_threshold_preset(menu_id, &button, &index))
		return COMMAND_GROUP_THRESHOLD;

	if (menu_id_is_threshold_custom(menu_id, &button, MOUSE_BUTTON_COUNT))
		return COMMAND_GROUP_THRESHOLD;

	return COMMAND_GROUP_NONE;
}
