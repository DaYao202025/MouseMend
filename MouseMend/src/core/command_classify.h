#pragma once

#include <stdbool.h>
#include <windows.h>

// Which handler owns a menu command.
//
// This exists because the WM_COMMAND body used to hold every branch inline:
// menu toggles, per-button thresholds, program lists and learning all shared
// one 305-line chain, so adding a feature meant editing code that belonged to
// three unrelated ones. Splitting the body into four handlers fixes the editing
// radius; this classifier fixes the discoverability - every range test lives in
// one file instead of being scattered through the middle of that chain.
//
// It does NOT guarantee that a classified ID is actually handled. That is what
// the four handlers and the smoke test are for.
typedef enum
{
	COMMAND_GROUP_NONE = 0, // the inert label row (id 0) and anything unknown
	COMMAND_GROUP_MENU,		// app-level toggles, presets, language, about, exit
	COMMAND_GROUP_THRESHOLD,// per-button switch, threshold presets, release delay, drag distance
	COMMAND_GROUP_LIST,		// whitelist/blacklist: mode, add, remove, one-click, pause
	COMMAND_GROUP_LEARNING	// learning on/off, adopt suggestions
} CommandGroup;

CommandGroup command_classify(UINT menu_id);
