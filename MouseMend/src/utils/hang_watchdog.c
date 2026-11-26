#include "hang_watchdog.h"
#include "crash_dump.h"
#include <stdint.h>

static HANDLE g_thread = NULL;
static HANDLE g_stop_event = NULL;

// Last heartbeat from the UI thread. Interlocked because the two sides are
// different threads; a torn read on x86 would look like a huge stall.
static volatile LONG64 g_beat = 0;

// Latched once a report has been written: one hang, one file. Repeating it
// every two seconds would fill the folder with copies of the same thing.
static volatile LONG g_reported = 0;

static HangReportCollectFn g_collector = NULL;

// Write the dump and the report for a hang that is happening right now.
static void capture_hang(void)
{
	static SupportReport report;

	static wchar_t dump[SUPPORT_PATH_CHARS];
	bool have_dump = crash_dump_write_current(dump, ARRAYSIZE(dump));

	if (g_collector)
		g_collector(&report);
	else
		support_bundle_prepare(&report, false);

	report.hung = true;

	static wchar_t bundle[SUPPORT_PATH_CHARS];
	if (support_bundle_write(&report, have_dump ? dump : NULL, bundle, ARRAYSIZE(bundle)))
	{
		if (have_dump)
			DeleteFileW(dump);

		return;
	}

	// The bundle could not be written: no writable folder, or the crash handler
	// was already inside the writer. Keep the dump where it is - it is the part
	// that says where the program stopped, and %TEMP% is still findable.
}

static DWORD WINAPI watchdog_thread(LPVOID param)
{
	(void)param;

	for (;;)
	{
		if (WaitForSingleObject(g_stop_event, HANG_WATCHDOG_POLL_MS) == WAIT_OBJECT_0)
			break;

		if (InterlockedCompareExchange(&g_reported, 0, 0) != 0)
			continue;

		uint64_t beat = (uint64_t)InterlockedCompareExchange64(&g_beat, 0, 0);
		if (beat == 0)
			continue; // No heartbeat yet: startup, nothing to judge

		uint64_t now = GetTickCount64();
		if (now - beat < HANG_WATCHDOG_TIMEOUT_MS)
			continue;

		InterlockedExchange(&g_reported, 1);
		capture_hang();
	}

	return 0;
}

bool hang_watchdog_start(HangReportCollectFn collector)
{
	if (g_thread)
		return true;

	g_collector = collector;
	InterlockedExchange64(&g_beat, 0);
	InterlockedExchange(&g_reported, 0);

	g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!g_stop_event)
		return false;

	g_thread = CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
	if (!g_thread)
	{
		CloseHandle(g_stop_event);
		g_stop_event = NULL;
		return false;
	}

	// Watching is not urgent work: it must never compete with the threads that
	// answer the user.
	SetThreadPriority(g_thread, THREAD_PRIORITY_BELOW_NORMAL);
	return true;
}

void hang_watchdog_stop(void)
{
	if (g_stop_event)
		SetEvent(g_stop_event);

	if (g_thread)
	{
		// Bounded on purpose. If the watcher happens to be inside the dump
		// writer, waiting for it without a limit would turn a clean exit into a
		// hang caused by the thing that watches for hangs.
		WaitForSingleObject(g_thread, 3000);
		CloseHandle(g_thread);
		g_thread = NULL;
	}

	if (g_stop_event)
	{
		CloseHandle(g_stop_event);
		g_stop_event = NULL;
	}
}

void hang_watchdog_beat(void)
{
	InterlockedExchange64(&g_beat, (LONG64)GetTickCount64());
}

bool hang_watchdog_triggered(void)
{
	return InterlockedCompareExchange(&g_reported, 0, 0) != 0;
}
