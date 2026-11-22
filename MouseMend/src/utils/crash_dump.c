#define _CRT_SECURE_NO_WARNINGS
#include "crash_dump.h"
#include "../core/block_log.h"
#include <windows.h>
#include <dbghelp.h>
#include <strsafe.h>

// dbghelp is resolved at run time instead of being linked. Linking it statically
// would turn a missing DLL into a start-up failure, and no diagnostic feature is
// allowed to be the reason the program refuses to start.
typedef BOOL(WINAPI *MiniDumpWriteDumpFn)(
	HANDLE process,
	DWORD process_id,
	HANDLE file,
	MINIDUMP_TYPE dump_type,
	PMINIDUMP_EXCEPTION_INFORMATION exception_param,
	PMINIDUMP_USER_STREAM_INFORMATION user_stream_param,
	PMINIDUMP_CALLBACK_INFORMATION callback_param);

static bool g_installed;

// Guards against a fault inside the handler itself
//
// LoadLibraryW takes the loader lock, and a damaged heap can fault inside it.
// That second fault calls this filter again, which would try to load the DLL
// again, and the recursion ends where the stack does. The first thread in owns
// the handler; everyone else - including a re-entry from the same thread -
// declines. InterlockedExchange is safe without a CRT.
static volatile LONG g_handling = 0;

// Second chance for a machine where the executable folder is read-only
// (<Program Files>, a locked USB stick). Mirrors the fallback the block log uses.
static bool build_fallback_directory(wchar_t *buffer, size_t buffer_count)
{
	wchar_t local_app_data[MAX_PATH];
	DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, ARRAYSIZE(local_app_data));

	if (length == 0 || length >= ARRAYSIZE(local_app_data))
		return false;

	wchar_t base[MAX_PATH];
	if (FAILED(StringCchPrintfW(base, ARRAYSIZE(base), L"%s\\MouseMend", local_app_data)))
		return false;

	// Ignored on purpose: "already exists" is the normal case
	CreateDirectoryW(base, NULL);

	return SUCCEEDED(StringCchPrintfW(buffer, buffer_count, L"%s\\%s", base, BLOCK_LOG_DIR_NAME));
}

// Create the dump, trying the preferred folder first
static bool write_dump(const EXCEPTION_POINTERS *info, wchar_t *path, size_t path_count)
{
	SYSTEMTIME stamp;
	GetLocalTime(&stamp);

	wchar_t preferred[MAX_PATH];
	bool have_preferred = block_log_describe_directory(preferred, ARRAYSIZE(preferred));

	wchar_t fallback[MAX_PATH];
	bool have_fallback = build_fallback_directory(fallback, ARRAYSIZE(fallback));

	for (int attempt = 0; attempt < 2; attempt++)
	{
		const wchar_t *directory = (attempt == 0 && have_preferred) ? preferred : fallback;

		if (attempt == 0 && !have_preferred)
			continue;

		if (attempt == 1 && !have_fallback)
			break;

		// The crash may well be the first time anything is written here
		CreateDirectoryW(directory, NULL);

		if (FAILED(StringCchPrintfW(path, path_count,
									L"%s\\crash_%04u%02u%02u_%02u%02u%02u.dmp",
									directory,
									(unsigned)stamp.wYear, (unsigned)stamp.wMonth,
									(unsigned)stamp.wDay, (unsigned)stamp.wHour,
									(unsigned)stamp.wMinute, (unsigned)stamp.wSecond)))
			return false;

		HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
								  FILE_ATTRIBUTE_NORMAL, NULL);

		if (file == INVALID_HANDLE_VALUE)
			continue;

		HMODULE library = LoadLibraryW(L"dbghelp.dll");
		bool written = false;

		if (library)
		{
			MiniDumpWriteDumpFn write_minidump =
				(MiniDumpWriteDumpFn)(void *)GetProcAddress(library, "MiniDumpWriteDump");

			if (write_minidump)
			{
				MINIDUMP_EXCEPTION_INFORMATION exception_info;
				exception_info.ThreadId = GetCurrentThreadId();
				exception_info.ExceptionPointers = (PEXCEPTION_POINTERS)info;
				exception_info.ClientPointers = FALSE;

				written = write_minidump(GetCurrentProcess(), GetCurrentProcessId(), file,
										 MiniDumpNormal, &exception_info, NULL, NULL) != FALSE;
			}

			// Not unloaded: the process is going away and the loader lock is a
			// hostile place to be during a crash
		}

		CloseHandle(file);

		if (written)
			return true;

		// A zero byte file is worse than no file: it looks like a usable dump
		DeleteFileW(path);
	}

	return false;
}

static LONG WINAPI crash_dump_filter(EXCEPTION_POINTERS *info)
{
	// Only the first entry writes. A fault raised inside the handler (a damaged
	// heap during LoadLibrary, say) arrives here again and is passed straight on.
	if (InterlockedExchange(&g_handling, 1) != 0)
		return EXCEPTION_CONTINUE_SEARCH;

	wchar_t path[MAX_PATH];
	write_dump(info, path, ARRAYSIZE(path));

	// Let Windows handle the crash as it always did: the dump is a bonus, not a
	// change in what the user sees.
	return EXCEPTION_CONTINUE_SEARCH;
}

void crash_dump_install(void)
{
	if (g_installed)
		return;

	g_installed = true;
	SetUnhandledExceptionFilter(crash_dump_filter);
}
