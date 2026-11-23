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
static CrashReportCollectFn g_collector;

// Guards against a fault inside the handler itself
//
// LoadLibraryW takes the loader lock, and a damaged heap can fault inside it.
// That second fault calls this filter again, which would try to load the DLL
// again, and the recursion ends where the stack does. The first thread in owns
// the handler; everyone else - including a re-entry from the same thread -
// declines. InterlockedExchange is safe without a CRT.
static volatile LONG g_handling = 0;

// Write the minidump into the given folder
//
// Producing the dump costs one file handle and no memory, so a process that is
// already damaged can still manage it. A zero byte file is deleted again: it
// would look like a usable dump and waste a round trip.
static bool write_dump_to(const wchar_t *directory, const EXCEPTION_POINTERS *info,
						  wchar_t *path, size_t path_count)
{
	if (!directory || !directory[0] || !path || path_count == 0)
		return false;

	SYSTEMTIME stamp;
	GetLocalTime(&stamp);

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
		return false;

	// Loaded from System32 by absolute path.
	//
	// A plain LoadLibraryW("dbghelp.dll") respects the search order, and any
	// product that already pulled a copy into the process wins: a real dump from
	// the field listed the input method's bundled dbghelp.dll, and the file it
	// produced held a single thread. The system copy is the only one that
	// understands the running build.
	wchar_t system_directory[MAX_PATH];
	wchar_t library_path[MAX_PATH];
	HMODULE library = NULL;

	if (GetSystemDirectoryW(system_directory, ARRAYSIZE(system_directory)) > 0 &&
		SUCCEEDED(StringCchPrintfW(library_path, ARRAYSIZE(library_path),
								   L"%s\\dbghelp.dll", system_directory)))
	{
		library = LoadLibraryExW(library_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	}

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

			// MiniDumpNormal keeps the package small enough to attach to a forum
			// post. It still carries every thread's stack and the module list,
			// which is what identifies the faulting function; the heap contents
			// a larger dump would add are not worth a file nobody can upload.
			written = write_minidump(GetCurrentProcess(), GetCurrentProcessId(), file,
									 MiniDumpNormal, &exception_info, NULL, NULL) != FALSE;
		}

		// Not unloaded: the process is going away and the loader lock is a
		// hostile place to be during a crash
	}

	CloseHandle(file);

	if (!written)
	{
		DeleteFileW(path);
		return false;
	}

	return true;
}

// Last resort: keep the dump where the previous versions put it, which is worse
// than a bundle but better than nothing.
static void move_dump_next_to_log(const wchar_t *dump)
{
	wchar_t directory[MAX_PATH];
	if (!block_log_describe_directory(directory, ARRAYSIZE(directory)))
		return;

	CreateDirectoryW(directory, NULL);

	const wchar_t *name = wcsrchr(dump, L'\\');
	name = (name && name[1]) ? name + 1 : dump;

	wchar_t target[MAX_PATH];
	if (FAILED(StringCchPrintfW(target, ARRAYSIZE(target), L"%s\\%s", directory, name)))
		return;

	MoveFileExW(dump, target, MOVEFILE_REPLACE_EXISTING);
}

static LONG WINAPI crash_dump_filter(EXCEPTION_POINTERS *info)
{
	// Only the first entry writes. A fault raised inside the handler (a damaged
	// heap during LoadLibrary, say) arrives here again and is passed straight on.
	if (InterlockedExchange(&g_handling, 1) != 0)
		return EXCEPTION_CONTINUE_SEARCH;

	// The dump is staged in %TEMP% and only survives when the bundle cannot be
	// written: the user asked for one file, and an empty Log folder appearing
	// next to a program that never logs anything is just confusing.
	wchar_t temp[MAX_PATH];
	DWORD temp_length = GetTempPathW(ARRAYSIZE(temp), temp);

	wchar_t dump[MAX_PATH];
	dump[0] = L'\0';

	bool have_dump = temp_length > 0 && temp_length < ARRAYSIZE(temp) &&
					 write_dump_to(temp, info, dump, ARRAYSIZE(dump));

	// A text report is worth producing even when the dump failed: it is the part
	// that says what the program was doing.
	if (g_collector)
	{
		static SupportReport report;
		g_collector(&report);

		report.crashed = true;

		if (info && info->ExceptionRecord)
		{
			report.exception_code = info->ExceptionRecord->ExceptionCode;
			report.exception_address =
				(uint64_t)(uintptr_t)info->ExceptionRecord->ExceptionAddress;

			support_bundle_describe_address(report.exception_address,
											report.faulting_module,
											ARRAYSIZE(report.faulting_module),
											&report.faulting_offset);
		}

		wchar_t bundle[MAX_PATH];

		if (support_bundle_write(&report, have_dump ? dump : NULL,
								 bundle, ARRAYSIZE(bundle)))
		{
			if (have_dump)
				DeleteFileW(dump);
		}
		else if (have_dump)
		{
			move_dump_next_to_log(dump);
		}
	}
	else if (have_dump)
	{
		move_dump_next_to_log(dump);
	}

	// Let Windows handle the crash as it always did: the report is a bonus, not a
	// change in what the user sees.
	return EXCEPTION_CONTINUE_SEARCH;
}

void crash_dump_set_report_collector(CrashReportCollectFn collector)
{
	g_collector = collector;
}

void crash_dump_install(void)
{
	if (g_installed)
		return;

	g_installed = true;
	SetUnhandledExceptionFilter(crash_dump_filter);
}
