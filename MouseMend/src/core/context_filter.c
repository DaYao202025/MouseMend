#include "context_filter.h"
#include <string.h>
#include <wchar.h>
#include <strsafe.h>

// The executable that is running right now. Filled once, used to refuse adding
// this program to its own lists.
static wchar_t g_self_path[CONTEXT_PATH_CHARS];
static bool g_self_ready;

void context_file_name_from_path(const wchar_t *path, wchar_t *name, size_t count)
{
	if (!name || count == 0)
		return;

	name[0] = L'\0';

	if (!path || !path[0])
		return;

	const wchar_t *last = path;
	for (const wchar_t *p = path; *p; p++)
	{
		if (*p == L'\\' || *p == L'/')
			last = p + 1;
	}

	StringCchCopyW(name, count, last);
}

void context_shorten_path(const wchar_t *full_path, wchar_t *out, size_t count)
{
	wchar_t head[CONTEXT_MENU_PATH_CHARS];
	size_t length;
	size_t tail;

	if (!out || count == 0)
		return;

	out[0] = L'\0';

	if (!full_path || !full_path[0])
		return;

	length = wcslen(full_path);

	if (length < CONTEXT_MENU_PATH_CHARS)
	{
		StringCchCopyW(out, count, full_path);
		return;
	}

	// Keep the tail, because that is where the file name is - clipping from the
	// right, which is what a menu does on its own, removes the only part that
	// tells two same-named rules apart. Keep part of the head as well so the
	// drive and the first directory stay readable.
	tail = 28;

	StringCchCopyNW(head, sizeof(head) / sizeof(head[0]), full_path, 26);

	if (FAILED(StringCchPrintfW(out, count, L"%s…%s", head, full_path + length - tail)))
	{
		// The caller sized the buffer for less than a shortened path. Falling
		// back to the bare name is better than an empty menu entry.
		context_file_name_from_path(full_path, out, count);
	}
}

bool context_path_matches(const wchar_t *rule, const wchar_t *full_path, const wchar_t *file_name)
{
	if (!rule || !rule[0] || !full_path)
		return false;

	// Legacy rules look like "*\game.exe": match the file name only.
	if (rule[0] == L'*' && rule[1] == L'\\')
		return file_name && file_name[0] && _wcsicmp(rule + 2, file_name) == 0;

	if (_wcsicmp(rule, full_path) == 0)
		return true;

	// Short (8.3) names can differ between the file dialog and the process
	// path, so fall back to the long form before giving up.
	wchar_t long_path[CONTEXT_PATH_CHARS];
	DWORD length = GetLongPathNameW(full_path, long_path, ARRAYSIZE(long_path));
	if (length > 0 && length < ARRAYSIZE(long_path))
		return _wcsicmp(rule, long_path) == 0;

	return false;
}

// Session-0 and kernel-side processes: they never own a foreground window, so
// a rule about them would do nothing while looking like it did.
static const wchar_t *const CONTEXT_RESERVED_NAMES[] = {
	L"System",
	L"Registry",
	L"smss.exe",
	L"csrss.exe",
	L"wininit.exe",
	L"winlogon.exe",
	L"services.exe",
	L"lsass.exe",
	L"svchost.exe",
};

// Shell and UWP hosts. These are legitimate rules - desktop chatter is a real
// problem - but one entry covers a whole surface, so the UI asks first.
static const wchar_t *const CONTEXT_SHELL_NAMES[] = {
	L"explorer.exe",
	L"dwm.exe",
	L"sihost.exe",
	L"taskhostw.exe",
	L"StartMenuExperienceHost.exe",
	L"SearchHost.exe",
	L"SearchUI.exe",
	L"ShellExperienceHost.exe",
	L"ApplicationFrameHost.exe",
	L"TextInputHost.exe",
	L"conhost.exe",
	L"WindowsTerminal.exe",
};

static bool name_in_table(const wchar_t *const *table, size_t count, const wchar_t *name)
{
	if (!table || !name || !name[0])
		return false;

	for (size_t i = 0; i < count; i++)
	{
		if (_wcsicmp(table[i], name) == 0)
			return true;
	}

	return false;
}

bool context_program_is_reserved(const wchar_t *file_name)
{
	return name_in_table(CONTEXT_RESERVED_NAMES, ARRAYSIZE(CONTEXT_RESERVED_NAMES), file_name);
}

bool context_program_is_shell(const wchar_t *file_name)
{
	return name_in_table(CONTEXT_SHELL_NAMES, ARRAYSIZE(CONTEXT_SHELL_NAMES), file_name);
}

void context_path_list_clear(ContextPathList *list)
{
	if (!list)
		return;

	memset(list, 0, sizeof(*list));
}

bool context_path_list_contains(const ContextPathList *list, const wchar_t *full_path,
								const wchar_t *file_name)
{
	if (!list || !full_path)
		return false;

	for (uint32_t i = 0; i < list->count && i < CONTEXT_LIST_MAX; i++)
	{
		if (context_path_matches(list->paths[i], full_path, file_name))
			return true;
	}

	return false;
}

const wchar_t *context_path_list_at(const ContextPathList *list, uint32_t index)
{
	if (!list || index >= list->count || index >= CONTEXT_LIST_MAX)
		return NULL;

	return list->paths[index];
}

static ContextPathList *list_for_mode(ContextPolicy *policy, ContextMode mode)
{
	if (!policy)
		return NULL;

	if (mode == CONTEXT_MODE_WHITELIST)
		return &policy->whitelist;
	if (mode == CONTEXT_MODE_BLACKLIST)
		return &policy->blacklist;

	return NULL;
}

static bool list_append(ContextPathList *list, const wchar_t *path)
{
	if (!list || !path || !path[0])
		return false;

	if (list->count >= CONTEXT_LIST_MAX)
		return false;

	if (wcschr(path, L'*'))
		return false;

	if (FAILED(StringCchCopyW(list->paths[list->count], CONTEXT_PATH_CHARS, path)))
		return false;

	list->count++;
	return true;
}

// Returns true when this event must NOT be filtered (i.e. pass through).
bool context_mode_suppresses(ContextMode mode, bool in_whitelist, bool in_blacklist)
{
	switch (mode)
	{
	case CONTEXT_MODE_WHITELIST:
		// Only programs on the whitelist are filtered; everything else passes.
		// Defensive: if a program somehow sits in both lists, the whitelist wins
		// so the outcome stays predictable.
		return in_whitelist ? false : true;

	case CONTEXT_MODE_BLACKLIST:
		// Programs on the blacklist pass; everything else is filtered.
		return in_blacklist ? true : false;

	case CONTEXT_MODE_ALL:
	default:
		// Filter everything. Fail safe: an unknown mode behaves like ALL.
		return false;
	}
}

void context_filter_init(ContextFilter *filter)
{
	if (!filter)
		return;

	memset(filter, 0, sizeof(*filter));

	// Explicit, so the initialisation point is greppable. Do not rely on the
	// "all zero is unlocked" implementation detail of SRWLOCK.
	InitializeSRWLock(&filter->lock);

	if (!g_self_ready)
	{
		DWORD length = GetModuleFileNameW(NULL, g_self_path, ARRAYSIZE(g_self_path));
		if (length > 0 && length < ARRAYSIZE(g_self_path))
			g_self_ready = true;
		else
			g_self_path[0] = L'\0';
	}
}

ContextAddResult context_filter_add_path(ContextFilter *filter, ContextMode list,
										 const wchar_t *full_path)
{
	if (!filter || !full_path || !full_path[0])
		return CONTEXT_ADD_INVALID;

	if (list != CONTEXT_MODE_WHITELIST && list != CONTEXT_MODE_BLACKLIST)
		return CONTEXT_ADD_INVALID;

	if (wcslen(full_path) >= CONTEXT_PATH_CHARS)
		return CONTEXT_ADD_INVALID;

	// The UI never creates wildcard entries; only the legacy migration does.
	if (wcschr(full_path, L'*'))
		return CONTEXT_ADD_INVALID;

	wchar_t name[CONTEXT_NAME_CHARS];
	context_file_name_from_path(full_path, name, ARRAYSIZE(name));

	if (!name[0])
		return CONTEXT_ADD_INVALID;

	if (g_self_ready && g_self_path[0] && _wcsicmp(full_path, g_self_path) == 0)
		return CONTEXT_ADD_SELF;

	// Refuse the processes that can never be in front. Only the add path is
	// guarded: rules migrated from the old version are written directly and
	// must keep working.
	if (context_program_is_reserved(name))
		return CONTEXT_ADD_RESERVED;

	ContextAddResult result = CONTEXT_ADD_OK;

	AcquireSRWLockExclusive(&filter->lock);

	ContextPathList *current = list_for_mode(&filter->policy, list);
	ContextPathList *other = list_for_mode(&filter->policy,
										   list == CONTEXT_MODE_WHITELIST
											   ? CONTEXT_MODE_BLACKLIST
											   : CONTEXT_MODE_WHITELIST);

	if (!current || !other)
	{
		result = CONTEXT_ADD_INVALID;
	}
	else if (context_path_list_contains(current, full_path, name))
	{
		result = CONTEXT_ADD_DUPLICATE;
	}
	else if (context_path_list_contains(other, full_path, name))
	{
		result = CONTEXT_ADD_IN_OTHER;
	}
	else if (current->count >= CONTEXT_LIST_MAX)
	{
		result = CONTEXT_ADD_FULL;
	}
	else if (!list_append(current, full_path))
	{
		result = CONTEXT_ADD_INVALID;
	}

	ReleaseSRWLockExclusive(&filter->lock);

	// Only a real change invalidates the cached verdict
	if (result == CONTEXT_ADD_OK)
		InterlockedIncrement(&filter->revision);

	return result;
}

bool context_filter_remove_path(ContextFilter *filter, ContextMode list, const wchar_t *full_path)
{
	if (!filter || !full_path || !full_path[0])
		return false;

	if (list != CONTEXT_MODE_WHITELIST && list != CONTEXT_MODE_BLACKLIST)
		return false;

	wchar_t name[CONTEXT_NAME_CHARS];
	context_file_name_from_path(full_path, name, ARRAYSIZE(name));

	bool removed = false;

	AcquireSRWLockExclusive(&filter->lock);

	ContextPathList *current = list_for_mode(&filter->policy, list);
	if (current)
	{
		for (uint32_t i = 0; i < current->count; i++)
		{
			if (!context_path_matches(current->paths[i], full_path, name))
				continue;

			// Shift the tail down; order is not meaningful.
			for (uint32_t j = i + 1; j < current->count; j++)
				StringCchCopyW(current->paths[j - 1], CONTEXT_PATH_CHARS, current->paths[j]);

			current->count--;
			current->paths[current->count][0] = L'\0';
			removed = true;
			break;
		}
	}

	ReleaseSRWLockExclusive(&filter->lock);

	if (removed)
		InterlockedIncrement(&filter->revision);

	return removed;
}

void context_filter_set_mode(ContextFilter *filter, ContextMode mode)
{
	if (!filter)
		return;

	if (mode < CONTEXT_MODE_ALL || mode >= CONTEXT_MODE_COUNT)
		mode = CONTEXT_MODE_ALL;

	AcquireSRWLockExclusive(&filter->lock);
	bool changed = filter->policy.mode != mode;
	filter->policy.mode = mode;
	ReleaseSRWLockExclusive(&filter->lock);

	if (changed)
		InterlockedIncrement(&filter->revision);
}

ContextMode context_filter_get_mode(ContextFilter *filter)
{
	if (!filter)
		return CONTEXT_MODE_ALL;

	AcquireSRWLockShared(&filter->lock);
	ContextMode mode = filter->policy.mode;
	ReleaseSRWLockShared(&filter->lock);

	if (mode < CONTEXT_MODE_ALL || mode >= CONTEXT_MODE_COUNT)
		mode = CONTEXT_MODE_ALL;

	return mode;
}

void context_filter_reset(ContextFilter *filter)
{
	if (!filter)
		return;

	AcquireSRWLockExclusive(&filter->lock);
	memset(&filter->policy, 0, sizeof(filter->policy));
	filter->policy.mode = CONTEXT_MODE_ALL;
	ReleaseSRWLockExclusive(&filter->lock);

	InterlockedIncrement(&filter->revision);
}

bool context_filter_compute_suppress(ContextFilter *filter, const wchar_t *full_path,
									 const wchar_t *file_name)
{
	if (!filter)
		return false;

	ContextMode mode;
	bool in_white;
	bool in_black;

	// Read the whole decision under one shared lock, so the mode cannot change
	// between "which list matched" and "what the mode means".
	AcquireSRWLockShared(&filter->lock);
	mode = filter->policy.mode;
	in_white = context_path_list_contains(&filter->policy.whitelist, full_path, file_name);
	in_black = context_path_list_contains(&filter->policy.blacklist, full_path, file_name);
	ReleaseSRWLockShared(&filter->lock);

	return context_mode_suppresses(mode, in_white, in_black);
}

void context_filter_copy_policy(ContextFilter *filter, ContextPolicy *out)
{
	if (!filter || !out)
		return;

	AcquireSRWLockShared(&filter->lock);
	memcpy(out, &filter->policy, sizeof(*out));
	ReleaseSRWLockShared(&filter->lock);
}

void context_filter_publish(ContextFilter *filter, uint32_t pid, bool suppressed, bool ours,
							const wchar_t *name)
{
	if (!filter)
		return;

	LONG current = InterlockedCompareExchange(&filter->snap.current, -1, -1);
	if (current != 0 && current != 1)
		current = 0;

	LONG next = current == 0 ? 1 : 0;
	ContextSlot *slot = &filter->snap.slots[next];

	// Our own window and a failed lookup must never turn the filter on.
	bool apply = suppressed && !ours && pid != 0;

	slot->pid = (LONG)pid;
	slot->suppressed = apply ? 1 : 0;
	slot->ours = ours ? 1 : 0;
	StringCchCopyW(slot->name, ARRAYSIZE(slot->name), name ? name : L"");

	MemoryBarrier();
	InterlockedExchange(&filter->snap.current, next);
}

void context_filter_set_pause(ContextFilter *filter, uint64_t now_tick, uint32_t duration_ms)
{
	if (!filter)
		return;

	InterlockedExchange64(&filter->snap.pause_until, (LONG64)(now_tick + duration_ms));
}

bool context_snapshot_is_paused(const ContextSnapshot *snapshot, uint64_t now_tick)
{
	if (!snapshot)
		return false;

	LONG64 until = InterlockedCompareExchange64((volatile LONG64 *)&snapshot->pause_until, 0, 0);
	return until > 0 && (LONG64)now_tick < until;
}

bool context_filter_is_paused(const ContextFilter *filter, uint64_t now_tick)
{
	if (!filter)
		return false;

	return context_snapshot_is_paused(&filter->snap, now_tick);
}

bool context_filter_should_suppress(const ContextSnapshot *snapshot, uint64_t now_tick)
{
	// NOTE: this runs inside the mouse hook. It must not lock, allocate or call
	// any process/window API. Do not add AcquireSRWLock* here.
	if (!snapshot)
		return false;

	if (context_snapshot_is_paused(snapshot, now_tick))
		return true;

	LONG current = InterlockedCompareExchange((volatile LONG *)&snapshot->current, -1, -1);
	if (current != 0 && current != 1)
		return false;

	return InterlockedCompareExchange((volatile LONG *)&snapshot->slots[current].suppressed, 0, 0) != 0;
}
