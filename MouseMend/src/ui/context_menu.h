#pragma once

#include <windows.h>
#include "../core/debouncer.h"
#include "../core/settings.h"
#include "../core/learning.h"
#include "../core/context_filter.h"

// Context menu manager
// The menu is rebuilt on every request so it always reflects the current state.
typedef struct
{
	HMENU menu;
} ContextMenuManager;

// Create and populate the menu (destroys any previously created menu)
// hook_active tells whether the mouse hook is actually installed, so the status
// line never claims that the protection is on while the hook is gone.
// paused turns the "pause" entry into a resume entry, so a temporary pause can
// always be lifted from the same place it was started.
bool context_menu_create(ContextMenuManager *manager, DebounceManager *debounce,
						 const Preferences *prefs, bool hook_active,
						 const LearningAdvice *advice, const ContextPolicy *policy,
						 bool paused);

// Show the context menu at the specified position
bool context_menu_show(ContextMenuManager *manager, HWND hwnd, int x, int y,
					   DebounceManager *debounce, const Preferences *prefs, bool hook_active,
					   const LearningAdvice *advice, const ContextPolicy *policy, bool paused);

// Destroy the menu and release its resources
void context_menu_destroy(ContextMenuManager *manager);
