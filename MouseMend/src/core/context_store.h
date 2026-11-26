#pragma once

#include <stdbool.h>
#include "context_filter.h"

// Persistence for the context policy, kept out of the main settings module.
//
// The policy (mode + two path lists) is the single source of truth: it is not
// duplicated inside Preferences, so the menu and the polling thread can never
// disagree about what is on the lists.

// Read the three keys and run the one-time migration from the old
// ExcludedExes value. Never fails in a way that leaves the policy inconsistent:
// absent or malformed values produce an empty list / ALL mode.
void context_store_load(ContextPolicy *policy);

// Write the three keys. Returns false when the registry is not writable.
bool context_store_save(const ContextPolicy *policy);

// Read the legacy semicolon-separated file-name list. Only used by migration.
bool context_store_read_legacy(wchar_t *buffer, size_t count);
