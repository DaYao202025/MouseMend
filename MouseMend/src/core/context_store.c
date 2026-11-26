#include "context_store.h"
#include "config.h"
#include <string.h>
#include <wchar.h>
#include <strsafe.h>

#define CONTEXT_RAW_CHARS (CONTEXT_LIST_MAX * CONTEXT_PATH_CHARS + 2)

// One static buffer shared by the reader and the writer. The store is only
// touched from the UI thread (startup and menu actions), never from the hook.
static wchar_t g_raw[CONTEXT_RAW_CHARS];

static bool read_dword(HKEY key, LPCWSTR name, DWORD *out)
{
	DWORD type = 0;
	DWORD size = sizeof(DWORD);
	DWORD data = 0;

	if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&data, &size) != ERROR_SUCCESS)
		return false;

	if (type != REG_DWORD || size != sizeof(DWORD))
		return false;

	*out = data;
	return true;
}

static bool write_dword(HKEY key, LPCWSTR name, DWORD value)
{
	return RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(DWORD)) == ERROR_SUCCESS;
}

static void read_multi_sz(HKEY key, LPCWSTR name, ContextPathList *list)
{
	context_path_list_clear(list);

	DWORD type = 0;
	DWORD bytes = sizeof(g_raw);

	if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)g_raw, &bytes) != ERROR_SUCCESS)
		return;

	if (type != REG_MULTI_SZ || bytes < 4 || (bytes % sizeof(wchar_t)) != 0)
		return;

	size_t total = bytes / sizeof(wchar_t);
	if (total > ARRAYSIZE(g_raw))
		total = ARRAYSIZE(g_raw);

	// Walk the double-NUL terminated block; the first empty string ends it.
	// A malformed value from the registry may not be terminated at all, so the
	// length is bounded by what is actually in the buffer.
	const wchar_t *cursor = g_raw;
	const wchar_t *end = g_raw + total;

	while (cursor < end && list->count < CONTEXT_LIST_MAX)
	{
		size_t remaining = (size_t)(end - cursor);
		size_t length = 0;

		if (FAILED(StringCchLengthW(cursor, remaining, &length)) || length == 0)
			break;

		if (length < CONTEXT_PATH_CHARS && length + 1 <= remaining)
		{
			// Both bounds are visible in the loop, which keeps the analyser
			// happy where a memcpy of a computed length does not.
			for (size_t k = 0; k <= length && k < remaining; k++)
				list->paths[list->count][k] = cursor[k];

			list->count++;
		}

		cursor += length + 1;
	}
}

static bool write_multi_sz(HKEY key, LPCWSTR name, const ContextPathList *list)
{
	// Start clean: never let a previous, longer value leave stale bytes behind,
	// and keep the whole buffer "initialised" for the static analyser.
	memset(g_raw, 0, sizeof(g_raw));

	size_t used = 0;

	for (uint32_t i = 0; i < list->count && i < CONTEXT_LIST_MAX; i++)
	{
		size_t length = wcslen(list->paths[i]);

		// An empty entry would terminate the list on the next read and silently
		// drop everything after it.
		if (length == 0 || length >= CONTEXT_PATH_CHARS)
			return false;

		if (used + length + 2 > ARRAYSIZE(g_raw))
			return false;

		memcpy(g_raw + used, list->paths[i], (length + 1) * sizeof(wchar_t));
		used += length + 1;
	}

	if (list->count == 0)
	{
		g_raw[0] = L'\0';
		g_raw[1] = L'\0';
		used = 2;
	}
	else
	{
		g_raw[used] = L'\0'; // extra terminator
		used += 1;
	}

	size_t bytes = used * sizeof(wchar_t);
	if (bytes > sizeof(g_raw))
		return false;

	// The analyser treats g_raw as "partially written by the loop above" and
	// reports C6385 even though `bytes` was just range-checked against the
	// whole array. This is a known false positive on a static buffer.
#pragma warning(push)
#pragma warning(suppress : 6385)
	bool ok = RegSetValueExW(key, name, 0, REG_MULTI_SZ, (const BYTE *)g_raw, (DWORD)bytes) == ERROR_SUCCESS;
#pragma warning(pop)

	return ok;
}

bool context_store_read_legacy(wchar_t *buffer, size_t count)
{
	if (!buffer || count == 0)
		return false;

	buffer[0] = L'\0';

	HKEY key;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_SETTINGS_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
		return false;

	DWORD type = 0;
	DWORD bytes = (DWORD)(count * sizeof(wchar_t));
	bool ok = RegQueryValueExW(key, L"ExcludedExes", NULL, &type, (BYTE *)buffer, &bytes) == ERROR_SUCCESS &&
			  type == REG_SZ && bytes >= sizeof(wchar_t);

	RegCloseKey(key);

	if (!ok)
		buffer[0] = L'\0';

	buffer[count - 1] = L'\0';
	return ok;
}

// Returns true when policy was changed and therefore has to be written back.
static bool migrate_legacy(ContextPolicy *policy, HKEY key)
{
	DWORD migrated = 0;
	if (read_dword(key, L"ContextMigrated", &migrated) && migrated != 0)
		return false;

	// Anything already on the new list means a migration happened before the
	// marker existed; record it and stop.
	if (policy->blacklist.count > 0)
	{
		write_dword(key, L"ContextMigrated", 1);
		return false;
	}

	wchar_t legacy[512];
	if (!context_store_read_legacy(legacy, ARRAYSIZE(legacy)) || !legacy[0])
	{
		write_dword(key, L"ContextMigrated", 1);
		return false;
	}

	// Semicolon separated entries. A bare name becomes the wildcard rule the
	// matcher already understands; an entry that carries a path is stored as a
	// full path, because "*\C:\Games\game.exe" can never match - a file name
	// never contains a backslash, so that rule would be dead on arrival.
	wchar_t wildcard[CONTEXT_PATH_CHARS];
	const wchar_t *cursor = legacy;
	bool any = false;

	while (*cursor && policy->blacklist.count < CONTEXT_LIST_MAX)
	{
		const wchar_t *separator = wcschr(cursor, L';');
		const wchar_t *start = cursor;
		const wchar_t *end = separator ? separator : cursor + wcslen(cursor);

		// Trim: the old value could come from hand editing or another tool, and
		// a stray space makes the rule never match.
		while (start < end && (*start == L' ' || *start == L'\t'))
			start++;
		while (end > start && (end[-1] == L' ' || end[-1] == L'\t'))
			end--;

		size_t length = (size_t)(end - start);

		if (length > 0 && length + 2 < CONTEXT_PATH_CHARS)
		{
			wchar_t *target = policy->blacklist.paths[policy->blacklist.count];

			if (wcschr(start, L'\\') != NULL || wcschr(start, L':') != NULL)
			{
				memcpy(target, start, length * sizeof(wchar_t));
				target[length] = L'\0';
			}
			else
			{
				wildcard[0] = L'*';
				wildcard[1] = L'\\';
				memcpy(wildcard + 2, start, length * sizeof(wchar_t));
				wildcard[2 + length] = L'\0';
				memcpy(target, wildcard, (length + 3) * sizeof(wchar_t));
			}

			policy->blacklist.count++;
			any = true;
		}

		if (!separator)
			break;
		cursor = separator + 1;
	}

	if (any && policy->mode == CONTEXT_MODE_ALL)
		policy->mode = CONTEXT_MODE_BLACKLIST;

	write_dword(key, L"ContextMigrated", 1);
	return any;
}

void context_store_load(ContextPolicy *policy)
{
	if (!policy)
		return;

	memset(policy, 0, sizeof(*policy));
	policy->mode = CONTEXT_MODE_ALL;

	// Read with KEY_READ only: a key locked down by policy or security software
	// must not cost the user the lists he already saved. The migration opens the
	// key a second time for its marker, and simply skips when that fails.
	HKEY key;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_SETTINGS_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
		return;

	DWORD mode = 0;
	if (read_dword(key, L"ContextMode", &mode) && mode < (DWORD)CONTEXT_MODE_COUNT)
		policy->mode = (ContextMode)mode;

	read_multi_sz(key, L"WhitelistExes", &policy->whitelist);
	read_multi_sz(key, L"BlacklistExes", &policy->blacklist);

	RegCloseKey(key);

	bool migrated = false;

	if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_SETTINGS_KEY, 0, KEY_READ | KEY_WRITE, &key) == ERROR_SUCCESS)
	{
		migrated = migrate_legacy(policy, key);
		RegCloseKey(key);
	}

	// The migration changed memory only. Without writing it back here, the rules
	// an upgraded user sees on the first launch are gone on the next one: the
	// marker is already set, so they are never migrated again, and nothing else
	// marks the settings dirty on a run where the user changes nothing.
	if (migrated)
		context_store_save(policy);
}

bool context_store_save(const ContextPolicy *policy)
{
	if (!policy)
		return false;

	HKEY key;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_SETTINGS_KEY, 0, NULL, REG_OPTION_NON_VOLATILE,
						KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS)
		return false;

	bool ok = write_dword(key, L"ContextMode", (DWORD)policy->mode);
	ok = write_multi_sz(key, L"WhitelistExes", &policy->whitelist) && ok;
	ok = write_multi_sz(key, L"BlacklistExes", &policy->blacklist) && ok;
	ok = write_dword(key, L"ContextMigrated", 1) && ok;

	RegCloseKey(key);
	return ok;
}
