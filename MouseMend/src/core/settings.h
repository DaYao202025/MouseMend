#pragma once

#include <stdbool.h>
#include <windows.h>
#include "debouncer.h"
#include "config.h"

// Persistence of the user settings.
// The live debounce state lives in DebounceManager, the remaining preferences
// live in Preferences. These helpers only translate them to and from the registry.

// Preferences that are not part of the debounce state
typedef struct
{
	Language language;	 // UI language
	bool intro_shown;	 // Whether the first-run balloon was already displayed
	bool notify_blocks;	 // Show a balloon when a false click is blocked
	bool log_blocks;	 // Append blocked clicks to Log\YYYYMMDD.log
} Preferences;

// Load the persisted settings
// Parameters:
//   debounce - Receives the stored configuration
//   prefs - Receives the stored preferences
// Returns:
//   true when a stored profile was found, false on first run
bool settings_load(DebounceManager *debounce, Preferences *prefs);

// Persist the current state
// Returns:
//   false when the registry is not writable, so the caller can warn the user
//   that the change will not survive a restart
bool settings_save(const DebounceManager *debounce, const Preferences *prefs);
