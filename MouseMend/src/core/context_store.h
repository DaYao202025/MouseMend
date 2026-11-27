#pragma once

#include <stdbool.h>
#include "context_filter.h"

// Persistence for the context policy, kept out of the main settings module.
//
// The policy (mode + two path lists) is the single source of truth: it is not
// duplicated inside Preferences, so the menu and the polling thread can never
// disagree about what is on the lists.
//
// Registry layout, second half (HKCU\Software\MouseMend). The first half is the
// table at the top of settings.h; between the two of them they name every value
// this program writes.
//
//   ContextMode       DWORD     0=all 1=whitelist 2=blacklist
//   WhitelistExes     MULTI_SZ  full paths
//   BlacklistExes     MULTI_SZ  full paths, or "*\name" produced by the migration
//   ContextMigrated   DWORD     0/1, one-time migration marker
//
//   ExcludedExes      REG_SZ    legacy, semicolon separated file names. Read once
//                               by the migration and never written again.

// Read the three keys and run the one-time migration from the old
// ExcludedExes value. Never fails in a way that leaves the policy inconsistent:
// absent or malformed values produce an empty list / ALL mode.
void context_store_load(ContextPolicy *policy);

// Write the three keys. Returns false when the registry is not writable.
bool context_store_save(const ContextPolicy *policy);

// Read the legacy semicolon-separated file-name list. Only used by migration.
bool context_store_read_legacy(wchar_t *buffer, size_t count);

// Read the same three keys without the migration side effect.
//
// context_store_load() may rewrite the registry when it migrates an old
// ExcludedExes value, and a diagnostic run must never change settings - a smoke
// test asserts exactly that. So the report reads through here instead.
void context_store_inspect(ContextPolicy *policy);
