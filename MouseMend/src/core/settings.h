#pragma once

#include <stdbool.h>
#include <windows.h>
#include "debouncer.h"
#include "config.h"

// Persistence of the user settings.
// The live debounce state lives in DebounceManager, the remaining preferences
// live in Preferences. These helpers only translate them to and from the registry.
//
// Registry layout, first half (HKCU\Software\MouseMend). Together with the table
// at the top of context_store.h this is the complete set of value names - a new
// setting belongs in one of the two tables, and adding a row is part of adding
// the setting, not an afterthought.
//
//   DragDistance      DWORD    px    movement above which a press counts as a drag
//   HybridHeuristic   DWORD    0/1   hybrid drag detection
//   IntroShown        DWORD    0/1   first-run balloon already displayed
//   Language          DWORD    0/1   zh-CN / en
//   LearningEnabled   DWORD    0/1   opt-in self-learning
//   LogBlocked        DWORD    0/1   append blocked clicks to a daily file
//   NotifyBlocked     DWORD    0/1   balloon on every blocked click
//   ReleaseDelay      DWORD    ms    deferred release used by smart dragging

// Preferences that are not part of the debounce state
typedef struct
{
	Language language;	 // UI language
	bool intro_shown;	 // Whether the first-run balloon was already displayed
	bool notify_blocks;	 // Show a balloon when a false click is blocked
	bool log_blocks;	 // Append blocked clicks to Log\YYYYMMDD.log
	bool learning_enabled; // Opt-in: collect chatter samples and offer suggestions
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
