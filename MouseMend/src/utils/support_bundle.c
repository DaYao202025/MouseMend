#include "support_bundle.h"
#include "zip_writer.h"
#include "../core/block_log.h"
#include "../core/config.h"
#include "../core/debouncer.h"
#include "../../version.h"
#include <shlobj.h>
#include <stdarg.h>
#include <string.h>
#include <strsafe.h>

#pragma comment(lib, "shell32.lib")

#define SUPPORT_DIR_COUNT 4
#define PRUNE_MAX 16
#define UTF8_PATH_MAX (MAX_PATH * 3)

// ------------------------------------------------------------------ helpers

// Convert one wide string to UTF-8. A Chinese user name inside a path would turn
// into question marks if the narrow printf had to do this itself.
static void to_utf8(const wchar_t *source, char *buffer, size_t buffer_count)
{
	if (!buffer || buffer_count == 0)
		return;

	buffer[0] = '\0';

	if (!source || !source[0])
		return;

	if (WideCharToMultiByte(CP_UTF8, 0, source, -1, buffer, (int)buffer_count, NULL, NULL) <= 0)
		buffer[0] = '\0';
}

// Append one formatted line, keeping the buffer terminated at all times
// Truncation is not an error: a partial report still helps, a failed export
// means the user has nothing at all.
static void append(char *buffer, size_t buffer_count, const char *format, ...)
{
	size_t used = 0;

	if (!buffer || buffer_count == 0)
		return;

	if (FAILED(StringCchLengthA(buffer, buffer_count, &used)))
		return;

	if (used + 1 >= buffer_count)
		return;

	va_list args;
	va_start(args, format);
	StringCchVPrintfA(buffer + used, buffer_count - used, format, args);
	va_end(args);
}

static void format_timestamp(wchar_t *stamp, size_t stamp_count,
							 wchar_t *readable, size_t readable_count)
{
	SYSTEMTIME now;
	GetLocalTime(&now);

	StringCchPrintfW(stamp, stamp_count, L"%04u%02u%02u-%02u%02u%02u",
					 (unsigned)now.wYear, (unsigned)now.wMonth, (unsigned)now.wDay,
					 (unsigned)now.wHour, (unsigned)now.wMinute, (unsigned)now.wSecond);

	StringCchPrintfW(readable, readable_count, L"%04u-%02u-%02u %02u:%02u:%02u",
					 (unsigned)now.wYear, (unsigned)now.wMonth, (unsigned)now.wDay,
					 (unsigned)now.wHour, (unsigned)now.wMinute, (unsigned)now.wSecond);
}

// RtlGetVersion is the only call that reports the real build regardless of the
// manifest's compatibility section; GetVersionEx lies without one.
static void get_os_version(wchar_t *buffer, size_t buffer_count)
{
	typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);

	buffer[0] = L'\0';

	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	RtlGetVersionFn get_version = NULL;

	if (ntdll)
		get_version = (RtlGetVersionFn)(void *)GetProcAddress(ntdll, "RtlGetVersion");

	if (get_version)
	{
		OSVERSIONINFOW info;
		SecureZeroMemory(&info, sizeof(info));
		info.dwOSVersionInfoSize = sizeof(info);

		if (get_version(&info) == 0)
		{
			StringCchPrintfW(buffer, buffer_count, L"%lu.%lu.%lu",
							 (unsigned long)info.dwMajorVersion,
							 (unsigned long)info.dwMinorVersion,
							 (unsigned long)info.dwBuildNumber);
			return;
		}
	}

	StringCchCopyW(buffer, buffer_count, L"unknown");
}

void support_bundle_describe_address(uint64_t address,
									 wchar_t *module, size_t module_count,
									 uint64_t *offset)
{
	if (module && module_count > 0)
		module[0] = L'\0';

	if (offset)
		*offset = 0;

	if (address == 0)
		return;

	MEMORY_BASIC_INFORMATION info;
	SecureZeroMemory(&info, sizeof(info));

	if (VirtualQuery((const void *)(uintptr_t)address, &info, sizeof(info)) == 0)
		return;

	HMODULE base = (HMODULE)info.AllocationBase;

	if (base && module && module_count > 0)
	{
		if (GetModuleFileNameW(base, module, (DWORD)module_count) == 0)
			module[0] = L'\0';
	}

	if (offset)
		*offset = address - (uint64_t)(uintptr_t)base;
}

void support_bundle_prepare(SupportReport *report, bool crashed)
{
	if (!report)
		return;

	SecureZeroMemory(report, sizeof(*report));

	report->crashed = crashed;
	report->complete = true;
	report->now_tick = GetTickCount64();

	format_timestamp(report->stamp, ARRAYSIZE(report->stamp),
					 report->generated, ARRAYSIZE(report->generated));

	DWORD length = GetModuleFileNameW(NULL, report->exe_path, ARRAYSIZE(report->exe_path));
	if (length == 0 || length >= ARRAYSIZE(report->exe_path))
		report->exe_path[ARRAYSIZE(report->exe_path) - 1] = L'\0';

	get_os_version(report->os_version, ARRAYSIZE(report->os_version));
	StringCchCopyW(report->arch, ARRAYSIZE(report->arch),
				   (sizeof(void *) == 8) ? L"x64" : L"x86");

	DWORD handles = 0;
	if (GetProcessHandleCount(GetCurrentProcess(), &handles))
		report->handle_count = handles;

	report->gdi_objects = (uint32_t)GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
	report->user_objects = (uint32_t)GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
}

// ------------------------------------------------------------- text report

static void append_events(char *buffer, size_t buffer_count, const SupportReport *report)
{
	append(buffer, buffer_count, "recent events (newest first, %u of %u max)\n",
		   (unsigned)report->event_count, (unsigned)SUPPORT_EVENT_MAX);
	append(buffer, buffer_count, "-----\n");

	if (report->event_count == 0)
	{
		append(buffer, buffer_count, "none recorded\n");
		return;
	}

	append(buffer, buffer_count, "  ago_ms  button   dir      x      y     gap   thr  flags  verdict\n");

	for (uint32_t i = 0; i < report->event_count; i++)
	{
		const TraceEntry *entry = &report->events[i];

		uint64_t ago = (report->now_tick >= entry->timestamp)
						   ? (report->now_tick - entry->timestamp)
						   : 0;

		// One character per interesting property, so a reader can scan the column
		char flags[4];
		flags[0] = entry->decision.blocked ? 'B' : '-';
		flags[1] = entry->decision.deferred ? 'D' : '-';
		flags[2] = entry->decision.near_miss ? 'N' : '-';
		flags[3] = '\0';

		append(buffer, buffer_count,
			   "%8I64u  %-6s  %-4s  %5ld  %5ld  %5I64u  %4u  %s  %s\n",
			   ago,
			   block_log_button_slug(entry->button),
			   entry->is_down ? "down" : "up",
			   entry->x,
			   entry->y,
			   entry->decision.elapsed_ms,
			   (unsigned)entry->decision.threshold_ms,
			   flags,
			   debounce_reason_slug(entry->decision.reason));
	}
}

bool support_bundle_build_text(const SupportReport *report, char *buffer, size_t buffer_count)
{
	if (!report || !buffer || buffer_count == 0)
		return false;

	buffer[0] = '\0';

	char exe[UTF8_PATH_MAX];
	to_utf8(report->exe_path, exe, ARRAYSIZE(exe));

	char os[128];
	to_utf8(report->os_version, os, ARRAYSIZE(os));

	char arch[16];
	to_utf8(report->arch, arch, ARRAYSIZE(arch));

	char language[16];
	to_utf8(report->language, language, ARRAYSIZE(language));

	// Every wide string has to be converted first: a "%s" in a narrow format
	// reads UTF-16 as if it were ANSI and stops at the first embedded NUL, which
	// silently truncates the value to one character.
	char generated[40];
	to_utf8(report->generated, generated, ARRAYSIZE(generated));

	append(buffer, buffer_count, "%s %s support report\n", APP_NAME, APP_VERSION_STRING);
	append(buffer, buffer_count, "=====\n\n");
	append(buffer, buffer_count, "generated   : %s\n", generated);
	append(buffer, buffer_count, "kind        : %s\n",
		   report->hung ? "hang" : (report->crashed ? "crash" : "manual export"));
	append(buffer, buffer_count, "version     : %s (%s)\n", APP_VERSION_STRING, arch);
	append(buffer, buffer_count, "exe         : %s\n", exe);
	append(buffer, buffer_count, "os          : %s\n", os);
	append(buffer, buffer_count, "language    : %s\n", language);
	append(buffer, buffer_count, "complete    : %s\n",
		   report->complete ? "yes" : "NO - a lock or query timed out");

	if (report->app_start_tick != 0 && report->now_tick >= report->app_start_tick)
	{
		append(buffer, buffer_count, "uptime_ms   : %I64u\n",
			   report->now_tick - report->app_start_tick);
	}

	// The exception comes first: if the report is ever truncated, this is the
	// part that identifies the failure.
	if (report->crashed)
	{
		char module[UTF8_PATH_MAX];
		to_utf8(report->faulting_module, module, ARRAYSIZE(module));

		append(buffer, buffer_count, "\nexception\n-----\n");
		append(buffer, buffer_count, "code        : 0x%08lX\n", (unsigned long)report->exception_code);
		append(buffer, buffer_count, "address     : 0x%016I64X\n", report->exception_address);
		append(buffer, buffer_count, "module      : %s\n",
			   module[0] ? module : "(not inside any loaded image)");

		if (module[0])
			append(buffer, buffer_count, "offset      : 0x%I64X\n", report->faulting_offset);
	}

	append(buffer, buffer_count, "\n");
	append_events(buffer, buffer_count, report);

	append(buffer, buffer_count, "\nsettings\n-----\n");
	append(buffer, buffer_count, "hybrid      : %s\n", report->hybrid ? "on" : "off");
	append(buffer, buffer_count, "release_ms  : %u\n", (unsigned)report->release_delay_ms);
	append(buffer, buffer_count, "drag_px     : %u\n", (unsigned)report->drag_distance_px);
	append(buffer, buffer_count, "notify      : %s\n", report->notify_blocks ? "on" : "off");
	append(buffer, buffer_count, "log_blocks  : %s\n", report->log_blocks ? "on" : "off");

	char log_directory[UTF8_PATH_MAX];
	to_utf8(report->log_directory, log_directory, ARRAYSIZE(log_directory));
	if (log_directory[0])
		append(buffer, buffer_count, "log_dir     : %s\n", log_directory);

	append(buffer, buffer_count, "total_blocks: %u\n", (unsigned)report->total_blocks);

	append(buffer, buffer_count, "\nbutton   monitored  threshold  blocks\n");
	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		append(buffer, buffer_count, "%-8s %-10s %9u %7u\n",
			   block_log_button_slug((MouseButton)i),
			   report->monitored[i] ? "yes" : "no",
			   (unsigned)report->thresholds[i],
			   (unsigned)report->blocks[i]);
	}

	append(buffer, buffer_count, "\nhealth\n-----\n");
	append(buffer, buffer_count, "hook_active : %s\n", report->hook_active ? "yes" : "NO");
	append(buffer, buffer_count, "hook_events : %u\n", (unsigned)report->hook_events);
	append(buffer, buffer_count, "recoveries  : %u\n", (unsigned)report->hook_recoveries);

	append(buffer, buffer_count, "\nresources\n-----\n");
	append(buffer, buffer_count, "handles     : %u\n", (unsigned)report->handle_count);
	append(buffer, buffer_count, "gdi_objects : %u\n", (unsigned)report->gdi_objects);
	append(buffer, buffer_count, "user_objects: %u\n", (unsigned)report->user_objects);

	return true;
}

// --------------------------------------------------------------- the bundle

// Chinese note for the user who opens the zip, encoded as UTF-8 with a BOM so
// that Notepad on a non-Chinese system still shows it as text.
static size_t build_readme(char *buffer, size_t buffer_count)
{
	static const wchar_t NOTE[] =
		L"MouseMend 诊断报告\r\n"
		L"========================\r\n"
		L"\r\n"
		L"这是 MouseMend 自动生成的诊断包，用来查明程序崩溃或异常的原因。\r\n"
		L"\r\n"
		L"包里有什么：\r\n"
		L"\r\n"
		L"  report.txt   程序当时的状态、设置和最近 96 条鼠标事件（英文，给作者看）\r\n"
		L"  crash.dmp    崩溃时的内存转储。只有崩溃时才有，手动导出时没有。\r\n"
		L"\r\n"
		L"关于隐私（请先看一眼再决定）：\r\n"
		L"\r\n"
		L"  crash.dmp 里含有程序运行时的内存片段，可能夹带文件路径、窗口标题等文字；\r\n"
		L"  report.txt 里含有鼠标点击的屏幕坐标。\r\n"
		L"\r\n"
		L"  不想公开这些的话，可以只发 report.txt，或者私下发给作者。\r\n"
		L"\r\n"
		L"怎么反馈：\r\n"
		L"\r\n"
		L"  把整个 zip 发给作者即可；如果是论坛帖子，直接作为附件上传。\r\n"
		L"\r\n"
		L"文件名里的时间是崩溃或导出的本地时间。\r\n"
		L"同一目录下最多保留 3 个，更早的会被自动删除。\r\n";

	if (!buffer || buffer_count < 8)
		return 0;

	buffer[0] = (char)0xEF;
	buffer[1] = (char)0xBB;
	buffer[2] = (char)0xBF;

	int written = WideCharToMultiByte(CP_UTF8, 0, NOTE, -1,
									  buffer + 3, (int)(buffer_count - 4), NULL, NULL);

	if (written <= 1)
	{
		buffer[0] = '\0';
		return 0;
	}

	return 3 + (size_t)(written - 1);
}

// Folders a report may be written to, most visible first.
//
// The executable folder is what the user sees when he opens the program's folder
// and is therefore the first choice, but it is read-only whenever the program
// lives in Program Files or on a locked stick. Falling back is not cosmetic: the
// alternative is a crash that produces no evidence at all.
static int build_candidates(wchar_t directories[][MAX_PATH], int capacity)
{
	if (!directories || capacity <= 0)
		return 0;

	int count = 0;

	wchar_t exe[MAX_PATH];
	DWORD length = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));

	if (length > 0 && length < ARRAYSIZE(exe))
	{
		wchar_t *separator = wcsrchr(exe, L'\\');
		if (separator)
		{
			*separator = L'\0';
			if (count < capacity)
				StringCchCopyW(directories[count++], MAX_PATH, exe);
		}
	}

	wchar_t desktop[MAX_PATH];
	if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY | CSIDL_FLAG_CREATE,
								   NULL, SHGFP_TYPE_CURRENT, desktop)) &&
		count < capacity)
	{
		StringCchCopyW(directories[count++], MAX_PATH, desktop);
	}

	wchar_t local_app_data[MAX_PATH];
	DWORD env_length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data,
											   ARRAYSIZE(local_app_data));

	if (env_length > 0 && env_length < ARRAYSIZE(local_app_data) && count < capacity)
	{
		wchar_t base[MAX_PATH];
		if (SUCCEEDED(StringCchPrintfW(base, ARRAYSIZE(base), L"%s\\MouseMend", local_app_data)))
		{
			CreateDirectoryW(base, NULL); // Ignored: "already exists" is normal
			StringCchCopyW(directories[count++], MAX_PATH, base);
		}
	}

	wchar_t temp[MAX_PATH];
	DWORD temp_length = GetTempPathW(ARRAYSIZE(temp), temp);

	if (temp_length > 0 && temp_length < ARRAYSIZE(temp) && count < capacity)
	{
		StringCchCopyW(directories[count++], MAX_PATH, temp);
	}

	return count;
}

// Keep the newest few bundles of one prefix.
//
// Only files this program created are ever deleted: pruning matches the exact
// prefix and the .zip suffix, so a user who keeps something else next to the
// executable never loses it.
static void prune_old_bundles(const wchar_t *directory, const wchar_t *prefix)
{
	wchar_t pattern[MAX_PATH];
	if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\%s-*.zip", directory, prefix)))
		return;

	WIN32_FIND_DATAW data;
	HANDLE find = FindFirstFileW(pattern, &data);

	if (find == INVALID_HANDLE_VALUE)
		return;

	wchar_t names[PRUNE_MAX][MAX_PATH];
	FILETIME stamps[PRUNE_MAX];
	int count = 0;

	do
	{
		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;

		if (count >= PRUNE_MAX)
			break;

		StringCchCopyW(names[count], MAX_PATH, data.cFileName);
		stamps[count] = data.ftLastWriteTime;
		count++;
	} while (FindNextFileW(find, &data));

	FindClose(find);

	// Newest first, so everything past the keep limit can simply be deleted
	for (int i = 1; i < count; i++)
	{
		wchar_t name[MAX_PATH];
		StringCchCopyW(name, ARRAYSIZE(name), names[i]);
		FILETIME stamp = stamps[i];

		int j = i - 1;
		while (j >= 0 && CompareFileTime(&stamps[j], &stamp) < 0)
		{
			StringCchCopyW(names[j + 1], MAX_PATH, names[j]);
			stamps[j + 1] = stamps[j];
			j--;
		}

		StringCchCopyW(names[j + 1], MAX_PATH, name);
		stamps[j + 1] = stamp;
	}

	for (int i = SUPPORT_KEEP_BUNDLES; i < count; i++)
	{
		wchar_t path[MAX_PATH];
		if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", directory, names[i])))
			DeleteFileW(path);
	}
}

static bool support_bundle_write_inner(SupportReport *report, const wchar_t *dump_source,
									   wchar_t *path, size_t path_count)
{
	// Static on purpose: this runs inside the crash handler, where the stack may
	// be the very thing that failed. Only one bundle is ever built at a time.
	static char text[SUPPORT_TEXT_MAX];
	static char readme[SUPPORT_README_MAX];
	static ZipWriter writer;

	if (!report || !path || path_count == 0)
		return false;

	path[0] = L'\0';

	if (!support_bundle_build_text(report, text, sizeof(text)))
		return false;

	size_t text_length = 0;
	if (FAILED(StringCchLengthA(text, sizeof(text), &text_length)) || text_length == 0)
		return false;

	size_t readme_length = build_readme(readme, sizeof(readme));
	const wchar_t *prefix = report->hung      ? SUPPORT_PREFIX_HANG
							: report->crashed ? SUPPORT_PREFIX_CRASH
											  : SUPPORT_PREFIX_MANUAL;

	wchar_t directories[SUPPORT_DIR_COUNT][MAX_PATH];
	int directory_count = build_candidates(directories, SUPPORT_DIR_COUNT);

	for (int i = 0; i < directory_count; i++)
	{
		wchar_t candidate[MAX_PATH];
		if (FAILED(StringCchPrintfW(candidate, ARRAYSIZE(candidate), L"%s\\%s-%s.zip",
									directories[i], prefix, report->stamp)))
			continue;

		if (!zip_begin(&writer, candidate))
			continue;

		bool ok = zip_add_bytes(&writer, "report.txt", text, (uint32_t)text_length);

		if (ok && readme_length > 0)
			ok = zip_add_bytes(&writer, "README.txt", readme, (uint32_t)readme_length);

		// The dump is last: a missing or unreadable dump must not cost the user
		// the text report, which is the part that explains what happened.
		if (ok && dump_source && dump_source[0])
			ok = zip_add_file(&writer, "crash.dmp", dump_source);

		if (!ok)
		{
			zip_abort(&writer);
			continue;
		}

		if (!zip_end(&writer))
			continue;

		StringCchCopyW(path, path_count, candidate);
		prune_old_bundles(directories[i], prefix);
		return true;
	}

	return false;
}

bool support_bundle_write(SupportReport *report, const wchar_t *dump_source,
						  wchar_t *path, size_t path_count)
{
	// One bundle at a time. The renderer keeps its buffers in statics, and there
	// are two callers now: the crash handler and the hang watchdog. Letting both
	// in at once would produce a file neither of them wrote, so the loser gets
	// false and keeps whatever it staged on its own - the dump is written to
	// %TEMP% first, so nothing is lost that cannot be recovered.
	static volatile LONG busy = 0;

	if (InterlockedExchange(&busy, 1) != 0)
		return false;

	bool ok = support_bundle_write_inner(report, dump_source, path, path_count);

	InterlockedExchange(&busy, 0);
	return ok;
}

bool support_bundle_find_newest(const wchar_t *prefix, wchar_t *path, size_t path_count)
{
	if (!prefix || !path || path_count == 0)
		return false;

	path[0] = L'\0';

	wchar_t directories[SUPPORT_DIR_COUNT][MAX_PATH];
	int directory_count = build_candidates(directories, SUPPORT_DIR_COUNT);

	FILETIME newest;
	SecureZeroMemory(&newest, sizeof(newest));

	for (int i = 0; i < directory_count; i++)
	{
		wchar_t pattern[MAX_PATH];
		if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\%s-*.zip",
									directories[i], prefix)))
			continue;

		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileW(pattern, &data);

		if (find == INVALID_HANDLE_VALUE)
			continue;

		do
		{
			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;

			if (CompareFileTime(&data.ftLastWriteTime, &newest) <= 0)
				continue;

			wchar_t candidate[MAX_PATH];
			if (FAILED(StringCchPrintfW(candidate, ARRAYSIZE(candidate), L"%s\\%s",
										directories[i], data.cFileName)))
				continue;

			newest = data.ftLastWriteTime;
			StringCchCopyW(path, path_count, candidate);
		} while (FindNextFileW(find, &data));

		FindClose(find);
	}

	return path[0] != L'\0';
}
