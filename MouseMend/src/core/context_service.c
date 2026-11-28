#include "context_service.h"
#include <wchar.h>
#include <strsafe.h>

// Short enough that a change made in the menu is already in effect by the time
// the user is back in the program he was editing. The loop only touches the
// foreground window when the pid or the policy revision actually changed, so a
// short period costs almost nothing.
#define CONTEXT_POLL_MS 300

static void note_recent(ContextFilter *filter, const wchar_t *full_path, const wchar_t *name)
{
	if (!filter)
		return;

	AcquireSRWLockExclusive(&filter->lock);
	StringCchCopyW(filter->policy.recent_path, ARRAYSIZE(filter->policy.recent_path),
				   full_path ? full_path : L"");
	StringCchCopyW(filter->policy.recent_name, ARRAYSIZE(filter->policy.recent_name),
				   name ? name : L"");
	ReleaseSRWLockExclusive(&filter->lock);
}

static DWORD WINAPI context_thread(LPVOID param)
{
	ContextService *service = (ContextService *)param;
	DWORD last_pid = 0;
	LONG last_revision = -1;

	for (;;)
	{
		if (WaitForSingleObject(service->stop_event, CONTEXT_POLL_MS) == WAIT_OBJECT_0)
			break;

		HWND foreground = GetForegroundWindow();
		if (!foreground)
			continue;

		DWORD pid = 0;
		GetWindowThreadProcessId(foreground, &pid);
		if (pid == 0)
			continue;

		// Caching by pid alone is wrong: the lists can change while the very same
		// window stays in front. Without the revision check, adding a program did
		// nothing until the user switched to another window and back.
		LONG revision = InterlockedCompareExchange(&service->filter->revision, 0, 0);
		if (pid == last_pid && revision == last_revision)
			continue;

		last_pid = pid;
		last_revision = revision;

		// Our own window is never filtered, whatever the lists say. It is still
		// named: the live log says which program a decision was made for, and
		// "unknown" for the one window we know for certain would be a lie.
		if (pid == GetCurrentProcessId())
		{
			context_filter_publish(service->filter, pid, false, true, service->own_name);
			continue;
		}

		wchar_t path[CONTEXT_PATH_CHARS] = {0};
		wchar_t name[CONTEXT_NAME_CHARS] = {0};
		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

		if (process)
		{
			DWORD length = ARRAYSIZE(path);

			if (QueryFullProcessImageNameW(process, 0, path, &length))
				context_file_name_from_path(path, name, ARRAYSIZE(name));

			CloseHandle(process);
		}

		// The program in front could not be identified: elevated, protected, or
		// a process that simply did not answer this tick.
		//
		// Keeping the previous slot - which is what this used to do - applies
		// some other program's verdict to this one, in both directions and
		// silently: whatever the last foreground programme was exempt from, this
		// one is exempt from too, and vice versa. That is worse than either
		// choice, because nothing on screen says it happened.
		//
		// So publish "not exempt" instead. pid 0 marks the slot as belonging to
		// nobody, and suppressed = false keeps the filter armed, which is the
		// rule this project follows everywhere else: block rather than let
		// through. The pid is still forgotten so the next tick retries - an
		// elevated programme that did not answer once may answer the second time.
		//
		// observed is deliberately left untouched: the status strip then reports
		// the context module as waiting for data instead of claiming it works.
		//
		// The one-click rule is cleared with it. This used to be left alone "so the
		// menu keeps offering something", but that entry names the *previous*
		// program: with an elevated window in front, "do not filter chrome.exe"
		// (written from the stale name) is a rule for a program the user is not
		// looking at. A missing shortcut is a nuisance; a rule for the wrong
		// program changes what gets filtered and leaves no trace.
		if (!path[0] || !name[0])
		{
			last_pid = 0;
			note_recent(service->filter, L"", L"");
			context_filter_publish(service->filter, 0, false, false, L"");
			continue;
		}

		note_recent(service->filter, path, name);

		bool suppress = context_filter_compute_suppress(service->filter, path, name);
		context_filter_publish(service->filter, pid, suppress, false, name);
		InterlockedExchange(&service->observed, 1);
	}

	return 0;
}

bool context_service_start(ContextService *service, ContextFilter *filter)
{
	if (!service || !filter || service->thread)
		return false;

	memset(service, 0, sizeof(*service));
	service->filter = filter;

	// Once, not per tick: the answer cannot change while the program runs, and the
	// polling thread has better things to do every 300 ms.
	{
		wchar_t self[CONTEXT_PATH_CHARS] = {0};
		DWORD length = GetModuleFileNameW(NULL, self, ARRAYSIZE(self));

		if (length > 0 && length < ARRAYSIZE(self))
			context_file_name_from_path(self, service->own_name, ARRAYSIZE(service->own_name));
	}

	service->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!service->stop_event)
		return false;

	service->thread = CreateThread(NULL, 0, context_thread, service, 0, NULL);
	if (!service->thread)
	{
		CloseHandle(service->stop_event);
		service->stop_event = NULL;
		return false;
	}

	SetThreadPriority(service->thread, THREAD_PRIORITY_BELOW_NORMAL);
	return true;
}

bool context_service_is_running(const ContextService *service)
{
	return service && service->filter && service->thread;
}

void context_service_stop(ContextService *service)
{
	if (!service)
		return;

	if (service->stop_event)
		SetEvent(service->stop_event);

	if (service->thread)
	{
		WaitForSingleObject(service->thread, 3000);
		CloseHandle(service->thread);
		service->thread = NULL;
	}

	if (service->stop_event)
	{
		CloseHandle(service->stop_event);
		service->stop_event = NULL;
	}
}


