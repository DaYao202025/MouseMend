#include "trace_log.h"

#ifdef MOUSEMEND_TRACE

#include <stdio.h>
#include <string.h>
#include <strsafe.h>

// Room for about a minute of a very busy pointer
#define TRACE_CAPACITY 32768

// One movement sample per 10 ms: enough to redraw the path, few enough that a
// normal desktop records a few thousand lines instead of millions
#define TRACE_MOVE_INTERVAL_US 10000ull

typedef struct
{
	uint64_t t_us;	// microseconds since tracing started
	uint32_t kind;	// 0 button, 1 move, 2 injection, 3 lifecycle
	uint32_t button;
	uint32_t flags;
	uint32_t count;
	int32_t x, y;
	int32_t wx, wy, ww, wh; // foreground window rect
	char verdict[24];
} TraceRecord;

static TraceRecord g_ring[TRACE_CAPACITY];
static volatile LONG g_write_index = 0;
static volatile LONG g_read_index = 0;
static volatile LONG g_enabled = 0;
static volatile LONG g_stopping = 0;
static volatile LONG g_dropped = 0;

static uint64_t g_frequency = 0;
static uint64_t g_origin = 0;
static uint64_t g_last_move_us = 0;
static HANDLE g_thread = NULL;
static HANDLE g_wake = NULL;
static FILE *g_file = NULL;
static wchar_t g_path[MAX_PATH];

// g_enabled is written by the Interlocked family, so it has to be read the same
// way everywhere - a plain read is a different kind of access to the same object.
static __inline bool tracing(void)
{
	return InterlockedCompareExchange(&g_enabled, 0, 0) != 0;
}

// Microseconds since tracing started
static uint64_t now_us(void)
{
	LARGE_INTEGER counter;

	if (!QueryPerformanceCounter(&counter) || g_frequency == 0)
		return GetTickCount64();

	return (uint64_t)((counter.QuadPart - (LONGLONG)g_origin) * 1000000ull / g_frequency);
}

static bool window_is_ours(HWND window)
{
	DWORD pid = 0;

	if (!window)
		return false;

	GetWindowThreadProcessId(window, &pid);
	return pid == GetCurrentProcessId();
}

// Publish one record. Two writers are impossible: the hook path is the only
// producer, and it runs on one thread.
static void push(const TraceRecord *record)
{
	LONG write = InterlockedCompareExchange(&g_write_index, 0, 0);
	LONG read = InterlockedCompareExchange(&g_read_index, 0, 0);

	if ((uint32_t)(write - read) >= TRACE_CAPACITY)
	{
		// The writer fell behind. Dropping a record is better than blocking the
		// thread that is delivering mouse input to the whole desktop.
		InterlockedIncrement(&g_dropped);
		return;
	}

	g_ring[write % TRACE_CAPACITY] = *record;
	MemoryBarrier();
	InterlockedExchange(&g_write_index, write + 1);
}

static void format_record(const TraceRecord *record)
{
	if (record->kind == 0)
	{
		static const char *names[] = {"left", "right", "middle", "x1", "x2", "wheel", "hwheel", "?"};
		const char *name = (record->button < ARRAYSIZE(names)) ? names[record->button] : "?";
		uint32_t f = record->flags;

		fprintf(g_file,
				"E %10I64u %-6s %-4s pos=%5d,%5d verdict=%-18s sw=%u rp=%u inj=%u "
				"| lbtn=%u cap=%s fg=%s hit=%s fgrect=%d,%d,%dx%d\n",
				record->t_us, name, (f & 1) ? "down" : "up", record->x, record->y,
				record->verdict, (f & 4) ? 1u : 0u, (f & 8) ? 1u : 0u, (f & 2) ? 1u : 0u,
				(f & 16) ? 1u : 0u,
				(f & 32) ? "ours" : "other",
				(f & 64) ? "ours" : "other",
				(f & 128) ? "ours" : "other",
				record->wx, record->wy, record->ww, record->wh);
	}
	else if (record->kind == 1)
	{
		// The rectangle is what turns "the pointer moved" into "a window moved
		// while the button was up", which is the report this file exists for.
		fprintf(g_file, "M %10I64u pos=%5d,%5d lbtn=%u cap=%s fg=%s rect=%d,%d,%dx%d\n",
				record->t_us, record->x, record->y, (record->flags & 16) ? 1u : 0u,
				(record->flags & 32) ? "ours" : "other",
				(record->flags & 64) ? "ours" : "other",
				record->wx, record->wy, record->ww, record->wh);
	}
	else if (record->kind == 2)
	{
		fprintf(g_file, "I %10I64u seq=%-6s count=%u delivered=%u\n",
				record->t_us, record->verdict, record->count, record->button);
	}
	else
	{
		static const char *names[TRACE_LIFE_COUNT] = {
			"app-start", "app-stop", "hook-install", "hook-remove",
			"hook-restart", "settings-open", "settings-close",
			"timer-arm", "timer-stop"};

		uint32_t index = record->button;
		fprintf(g_file, "L %10I64u %s\n", record->t_us,
				(index < TRACE_LIFE_COUNT) ? names[index] : "?");
	}
}

static void drain(void)
{
	LONG write = InterlockedCompareExchange(&g_write_index, 0, 0);
	LONG read = InterlockedCompareExchange(&g_read_index, 0, 0);

	if (!g_file)
		return;

	while (read < write)
	{
		format_record(&g_ring[read % TRACE_CAPACITY]);
		read++;
	}

	InterlockedExchange(&g_read_index, read);
	fflush(g_file);
}

static DWORD WINAPI writer_thread(LPVOID unused)
{
	(void)unused;

	while (!g_stopping)
	{
		WaitForSingleObject(g_wake, 200);
		drain();
	}

	drain();
	return 0;
}

bool trace_start(void)
{
	wchar_t directory[MAX_PATH];
	SYSTEMTIME now;

	if (tracing())
		return true;

	if (!GetTempPathW(ARRAYSIZE(directory), directory))
		return false;

	if (FAILED(StringCchCatW(directory, ARRAYSIZE(directory), L"MouseMend-trace")))
		return false;

	CreateDirectoryW(directory, NULL);

	GetLocalTime(&now);

	if (FAILED(StringCchPrintfW(g_path, ARRAYSIZE(g_path),
								L"%s\\trace-%04u%02u%02u-%02u%02u%02u.log", directory,
								now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond)))
		return false;

	_wfopen_s(&g_file, g_path, L"wb");

	if (!g_file)
		return false;

	fprintf(g_file, "# MouseMend trace, built %s %s\n", __DATE__, __TIME__);
	fprintf(g_file, "# E=button M=move I=injection L=lifecycle   sw=swallowed rp=replayed\n");
	fprintf(g_file, "# lbtn/cap/fg/hit are sampled right after the decision\n");

	LARGE_INTEGER frequency, counter;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&counter);
	g_frequency = (uint64_t)frequency.QuadPart;
	g_origin = (uint64_t)counter.QuadPart;

	g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);

	if (g_wake)
		g_thread = CreateThread(NULL, 0, writer_thread, NULL, 0, NULL);

	InterlockedExchange(&g_write_index, 0);
	InterlockedExchange(&g_read_index, 0);
	InterlockedExchange(&g_dropped, 0);
	InterlockedExchange(&g_stopping, 0);
	InterlockedExchange(&g_enabled, 1);

	return true;
}

void trace_stop(void)
{
	if (!tracing())
		return;

	InterlockedExchange(&g_enabled, 0);
	InterlockedExchange(&g_stopping, 1);

	if (g_wake)
		SetEvent(g_wake);

	if (g_thread)
	{
		WaitForSingleObject(g_thread, 3000);
		CloseHandle(g_thread);
		g_thread = NULL;
	}

	if (g_wake)
	{
		CloseHandle(g_wake);
		g_wake = NULL;
	}

	if (g_file)
	{
		fprintf(g_file, "# dropped %ld record(s)\n", InterlockedCompareExchange(&g_dropped, 0, 0));
		fclose(g_file);
		g_file = NULL;
	}
}

bool trace_active(void)
{
	return tracing();
}

const wchar_t *trace_path(void)
{
	return g_path;
}

void trace_button(int button, bool is_down, bool injected, int x, int y,
				  const char *verdict, bool swallowed, bool replayed)
{
	if (!tracing())
		return;

	TraceRecord record;
	memset(&record, 0, sizeof(record));

	record.t_us = now_us();
	record.kind = 0;
	record.button = (uint32_t)button;
	record.x = x;
	record.y = y;

	POINT point = {x, y};
	HWND capture = GetCapture();
	HWND foreground = GetForegroundWindow();
	HWND hit = WindowFromPoint(point);

	record.flags = (is_down ? 1u : 0u) | (injected ? 2u : 0u) | (swallowed ? 4u : 0u) |
				   (replayed ? 8u : 0u) |
				   ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 16u : 0u) |
				   (window_is_ours(capture) ? 32u : 0u) |
				   (window_is_ours(foreground) ? 64u : 0u) |
				   (window_is_ours(hit) ? 128u : 0u);

	if (foreground)
	{
		RECT box;

		if (GetWindowRect(foreground, &box))
		{
			record.wx = box.left;
			record.wy = box.top;
			record.ww = box.right - box.left;
			record.wh = box.bottom - box.top;
		}
	}

	StringCchCopyA(record.verdict, ARRAYSIZE(record.verdict), verdict ? verdict : "?");

	push(&record);
}

void trace_move(int x, int y)
{
	if (!tracing())
		return;

	uint64_t now = now_us();

	if (now - g_last_move_us < TRACE_MOVE_INTERVAL_US)
		return;

	g_last_move_us = now;

	TraceRecord record;
	memset(&record, 0, sizeof(record));
	record.t_us = now;
	record.kind = 1;
	record.x = x;
	record.y = y;

	HWND capture = GetCapture();
	HWND foreground = GetForegroundWindow();

	record.flags = ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 16u : 0u) |
				   (window_is_ours(capture) ? 32u : 0u) |
				   (window_is_ours(foreground) ? 64u : 0u);

	if (foreground)
	{
		RECT box;

		if (GetWindowRect(foreground, &box))
		{
			record.wx = box.left;
			record.wy = box.top;
			record.ww = box.right - box.left;
			record.wh = box.bottom - box.top;
		}
	}

	push(&record);
}

void trace_injection(const INPUT *inputs, UINT count, bool delivered)
{
	if (!tracing() || !inputs)
		return;

	TraceRecord record;
	memset(&record, 0, sizeof(record));

	record.t_us = now_us();
	record.kind = 2;
	record.count = count;
	record.button = delivered ? 1u : 0u;

	size_t used = 0;

	for (UINT i = 0; i < count && i < 6; i++)
	{
		DWORD flags = inputs[i].mi.dwFlags;
		char code = '?';

		if (flags & (MOUSEEVENTF_LEFTUP | MOUSEEVENTF_RIGHTUP | MOUSEEVENTF_MIDDLEUP | MOUSEEVENTF_XUP))
			code = 'U';
		else if (flags & (MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_RIGHTDOWN |
						  MOUSEEVENTF_MIDDLEDOWN | MOUSEEVENTF_XDOWN))
			code = 'D';
		else if (flags & (MOUSEEVENTF_WHEEL | MOUSEEVENTF_HWHEEL))
			code = 'W';

		record.verdict[used++] = code;
	}

	record.verdict[used] = '\0';

	push(&record);
}

void trace_lifecycle(TraceLifecycle what)
{
	if (!tracing())
		return;

	TraceRecord record;
	memset(&record, 0, sizeof(record));
	record.t_us = now_us();
	record.kind = 3;
	record.button = (uint32_t)what;

	push(&record);
}

#endif
