#include "block_log.h"
#include <stdio.h>
#include <wchar.h>
#include <strsafe.h>

// Number of 100 ns units in one millisecond
#define LOG_FILETIME_UNITS_PER_MS 10000ULL

// Initialize the queue
bool block_log_init(BlockLogQueue *queue)
{
	if (!queue)
		return false;

	queue->head = 0;
	queue->count = 0;
	queue->dropped = 0;

	// Ready is set last: cleanup() keys off it, and a queue whose critical
	// section was never initialised must not be treated as usable.
	InitializeCriticalSection(&queue->cs);
	queue->ready = true;
	return true;
}

// Release the queue
void block_log_cleanup(BlockLogQueue *queue)
{
	if (!queue || !queue->ready)
		return;

	DeleteCriticalSection(&queue->cs);
	queue->ready = false;
}

// Queue one blocked event
bool block_log_push(BlockLogQueue *queue, MouseButton button, bool is_down,
					uint64_t elapsed_ms, uint32_t threshold_ms)
{
	if (!queue || !queue->ready)
		return false;

	if (button < 0 || button >= MOUSE_BUTTON_COUNT)
		return false;

	EnterCriticalSection(&queue->cs);

	// Full: drop the oldest entry, the newest information is the useful one
	if (queue->count == BLOCK_LOG_RING_SIZE)
	{
		queue->head = (queue->head + 1) % BLOCK_LOG_RING_SIZE;
		queue->count--;
		queue->dropped++;
	}

	BlockLogEntry *slot = &queue->entries[(queue->head + queue->count) % BLOCK_LOG_RING_SIZE];
	slot->elapsed_ms = elapsed_ms;
	slot->button = button;
	slot->is_down = is_down;
	slot->threshold_ms = threshold_ms;

	queue->count++;

	LeaveCriticalSection(&queue->cs);
	return true;
}

// Dequeue the oldest event
bool block_log_pop(BlockLogQueue *queue, BlockLogEntry *entry)
{
	if (!queue || !entry || !queue->ready)
		return false;

	bool available = false;

	EnterCriticalSection(&queue->cs);

	if (queue->count > 0)
	{
		*entry = queue->entries[queue->head];
		queue->head = (queue->head + 1) % BLOCK_LOG_RING_SIZE;
		queue->count--;
		available = true;
	}

	LeaveCriticalSection(&queue->cs);
	return available;
}

// Number of events dropped because the queue was full
uint32_t block_log_dropped(BlockLogQueue *queue)
{
	if (!queue || !queue->ready)
		return 0;

	EnterCriticalSection(&queue->cs);
	uint32_t dropped = queue->dropped;
	LeaveCriticalSection(&queue->cs);

	return dropped;
}

// Number of queued events
int block_log_count(BlockLogQueue *queue)
{
	if (!queue || !queue->ready)
		return 0;

	EnterCriticalSection(&queue->cs);
	int count = queue->count;
	LeaveCriticalSection(&queue->cs);

	return count;
}

// Stable, language independent name used in the log file
const char *block_log_button_slug(MouseButton button)
{
	switch (button)
	{
	case MOUSE_BUTTON_LEFT:
		return "left";
	case MOUSE_BUTTON_RIGHT:
		return "right";
	case MOUSE_BUTTON_MIDDLE:
		return "middle";
	case MOUSE_BUTTON_X1:
		return "x1";
	case MOUSE_BUTTON_X2:
		return "x2";
	case MOUSE_BUTTON_WHEEL:
		return "wheel";
	case MOUSE_BUTTON_HWHEEL:
		return "hwheel";
	default:
		return "unknown";
	}
}

// Convert "base + elapsed" into a broken down local time
static bool elapsed_to_systemtime(uint64_t elapsed_ms, const SYSTEMTIME *base, SYSTEMTIME *out)
{
	if (!base || !out)
		return false;

	FILETIME file_time;
	if (!SystemTimeToFileTime(base, &file_time))
		return false;

	ULARGE_INTEGER value;
	value.LowPart = file_time.dwLowDateTime;
	value.HighPart = file_time.dwHighDateTime;
	value.QuadPart += elapsed_ms * LOG_FILETIME_UNITS_PER_MS;

	file_time.dwLowDateTime = value.LowPart;
	file_time.dwHighDateTime = value.HighPart;

	return FileTimeToSystemTime(&file_time, out) != 0;
}

// Format the local date of an entry as YYYYMMDD
bool block_log_format_day(uint64_t elapsed_ms, const SYSTEMTIME *base,
						  wchar_t *buffer, size_t buffer_count)
{
	if (!buffer || buffer_count == 0)
		return false;

	buffer[0] = L'\0';

	SYSTEMTIME stamp;
	if (!elapsed_to_systemtime(elapsed_ms, base, &stamp))
		return false;

	return SUCCEEDED(StringCchPrintfW(buffer, buffer_count, L"%04u%02u%02u",
									  (unsigned)stamp.wYear, (unsigned)stamp.wMonth, (unsigned)stamp.wDay));
}

// Format a complete log line, including the trailing CRLF
bool block_log_format_line(const BlockLogEntry *entry, const SYSTEMTIME *base,
						   wchar_t *buffer, size_t buffer_count)
{
	if (!entry || !buffer || buffer_count == 0)
		return false;

	buffer[0] = L'\0';

	SYSTEMTIME stamp;
	if (!elapsed_to_systemtime(entry->elapsed_ms, base, &stamp))
		return false;

	return SUCCEEDED(StringCchPrintfW(buffer, buffer_count,
									  L"%04u-%02u-%02u %02u:%02u:%02u.%03u\t%S\t%S\tthreshold=%ums\r\n",
									  (unsigned)stamp.wYear, (unsigned)stamp.wMonth, (unsigned)stamp.wDay,
									  (unsigned)stamp.wHour, (unsigned)stamp.wMinute, (unsigned)stamp.wSecond,
									  (unsigned)stamp.wMilliseconds,
									  block_log_button_slug(entry->button),
									  entry->is_down ? "pressed" : "released",
									  (unsigned)entry->threshold_ms));
}

// Create a directory, treating "already exists" as success
static bool ensure_directory(const wchar_t *path)
{
	if (!path || !path[0])
		return false;

	if (CreateDirectoryW(path, NULL))
		return true;

	DWORD error = GetLastError();
	if (error == ERROR_ALREADY_EXISTS)
	{
		DWORD attributes = GetFileAttributesW(path);
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
	}

	return false;
}

// Resolve and create the log directory
bool block_log_resolve_directory(wchar_t *buffer, size_t buffer_count)
{
	if (!buffer || buffer_count == 0)
		return false;

	buffer[0] = L'\0';

	wchar_t candidate[MAX_PATH];

	// Preferred location: next to the executable, so the user can find and delete it
	DWORD length = GetModuleFileNameW(NULL, candidate, ARRAYSIZE(candidate));
	if (length > 0 && length < ARRAYSIZE(candidate))
	{
		wchar_t *last_separator = wcsrchr(candidate, L'\\');
		if (last_separator)
		{
			*last_separator = L'\0';

			wchar_t directory[MAX_PATH];
			if (SUCCEEDED(StringCchPrintfW(directory, ARRAYSIZE(directory), L"%s\\%s", candidate, BLOCK_LOG_DIR_NAME)) &&
				ensure_directory(directory))
			{
				return SUCCEEDED(StringCchCopyW(buffer, buffer_count, directory));
			}
		}
	}

	// Fallback: the executable folder is read-only (Program Files, locked USB stick)
	wchar_t local_app_data[MAX_PATH];
	DWORD env_length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, ARRAYSIZE(local_app_data));
	if (env_length == 0 || env_length >= ARRAYSIZE(local_app_data))
		return false;

	wchar_t base[MAX_PATH];
	if (FAILED(StringCchPrintfW(base, ARRAYSIZE(base), L"%s\\MouseMend", local_app_data)))
		return false;

	if (!ensure_directory(base))
		return false;

	wchar_t directory[MAX_PATH];
	if (FAILED(StringCchPrintfW(directory, ARRAYSIZE(directory), L"%s\\%s", base, BLOCK_LOG_DIR_NAME)))
		return false;

	if (!ensure_directory(directory))
		return false;

	return SUCCEEDED(StringCchCopyW(buffer, buffer_count, directory));
}

// Describe where the history would be written, without creating anything
// Diagnostics and support must be able to report the path without leaving a
// stray folder behind on a machine that never enabled the feature.
bool block_log_describe_directory(wchar_t *buffer, size_t buffer_count)
{
	if (!buffer || buffer_count == 0)
		return false;

	buffer[0] = L'\0';

	wchar_t candidate[MAX_PATH];
	DWORD length = GetModuleFileNameW(NULL, candidate, ARRAYSIZE(candidate));

	if (length > 0 && length < ARRAYSIZE(candidate))
	{
		wchar_t *last_separator = wcsrchr(candidate, L'\\');
		if (last_separator)
		{
			*last_separator = L'\0';
			return SUCCEEDED(StringCchPrintfW(buffer, buffer_count, L"%s\\%s", candidate, BLOCK_LOG_DIR_NAME));
		}
	}

	wchar_t local_app_data[MAX_PATH];
	DWORD env_length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, ARRAYSIZE(local_app_data));
	if (env_length == 0 || env_length >= ARRAYSIZE(local_app_data))
		return false;

	return SUCCEEDED(StringCchPrintfW(buffer, buffer_count, L"%s\\MouseMend\\%s", local_app_data, BLOCK_LOG_DIR_NAME));
}

// Append entries to today's file, one open for the whole batch
size_t block_log_write_many(const wchar_t *directory, const SYSTEMTIME *base,
							const BlockLogEntry *entries, size_t count)
{
	if (!directory || !directory[0] || !base || !entries || count == 0)
		return 0;

	// Recreate the directory once per batch: the user is allowed to delete the
	// folder at any time and the option must keep working afterwards.
	if (!ensure_directory(directory))
		return 0;

	FILE *file = NULL;
	wchar_t open_day[16] = {0};
	size_t written = 0;

	for (size_t i = 0; i < count; i++)
	{
		wchar_t day[16];

		if (!block_log_format_day(entries[i].elapsed_ms, base, day, ARRAYSIZE(day)))
			continue;

		// A batch that straddles midnight writes to two files, so the day is
		// compared per entry rather than assumed constant
		if (!file || wcscmp(open_day, day) != 0)
		{
			if (file)
				fclose(file);

			file = NULL;

			wchar_t path[MAX_PATH];

			if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s.log", directory, day)))
				continue;

			// A header makes the file readable for a non-technical user without
			// breaking the stable column layout that scripts and Excel rely on
			bool new_file = (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES);

			if (_wfopen_s(&file, path, L"ab") != 0)
			{
				file = NULL;
				continue;
			}

			if (new_file)
			{
				static const char HEADER[] = "# time\tbutton\tevent\tthreshold\r\n";
				fwrite(HEADER, 1, sizeof(HEADER) - 1, file);
			}

			StringCchCopyW(open_day, ARRAYSIZE(open_day), day);
		}

		wchar_t line[256];

		if (!block_log_format_line(&entries[i], base, line, ARRAYSIZE(line)))
			continue;

		// The content is pure ASCII, so a straight UTF-8 conversion keeps the
		// file readable in Notepad and greppable from a script.
		char utf8[512];
		int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8) - 1, NULL, NULL);

		if (bytes <= 1)
			continue;

		if (fwrite(utf8, 1, (size_t)(bytes - 1), file) != (size_t)(bytes - 1))
			continue;

		written++;
	}

	if (file && fclose(file) != 0)
	{
		// stdio holds the last block in a buffer until close, so a full disk shows
		// up here and not at fwrite. Claiming success would make the counter in
		// the interface disagree with the file the user opens.
		return 0;
	}

	return written;
}

// Append one entry to today's file
bool block_log_write(const wchar_t *directory, const SYSTEMTIME *base, const BlockLogEntry *entry)
{
	return block_log_write_many(directory, base, entry, 1) == 1;
}
