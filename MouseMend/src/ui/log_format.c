#include "log_format.h"

#include <strsafe.h>
#include <string.h>

// Format a millisecond count, or a dash when there is nothing to measure
static void format_cell_number(uint64_t value, bool valid, wchar_t *buffer, size_t buffer_count)
{
	if (!valid)
		StringCchCopyW(buffer, buffer_count, lang_str(STR_LOG_GAP_NONE));
	else if (value > 999999)
		StringCchCopyW(buffer, buffer_count, L">999999");
	else
		StringCchPrintfW(buffer, buffer_count, L"%I32u", (uint32_t)value);
}

// Build one log row from a completed click
static void build_log_line(const AppSnapshot *snapshot, const ClickRow *row,
						   wchar_t *buffer, size_t buffer_count)
{
	wchar_t time_text[SETTINGS_TIME_SIZE];
	wchar_t gap_text[SETTINGS_CELL_SIZE];
	wchar_t hold_text[SETTINGS_CELL_SIZE];

	log_format_time(snapshot, row->down_tick, STR_LOG_TIME_FMT,
					time_text, ARRAYSIZE(time_text));

	format_cell_number(row->gap_ms, row->has_gap, gap_text, ARRAYSIZE(gap_text));

	// A scroll has no hold time, and a release that never arrived can only be
	// reported as a lower bound. Saying "0" would read like a perfect click.
	if (config_is_wheel_button(row->button))
		StringCchCopyW(hold_text, ARRAYSIZE(hold_text), lang_str(STR_LOG_GAP_NONE));
	else if (row->expired)
		StringCchPrintfW(hold_text, ARRAYSIZE(hold_text), L">%I32u",
						 (uint32_t)SETTINGS_CLICK_EXPIRE_MS);
	else
		format_cell_number(row->hold_ms, row->released, hold_text, ARRAYSIZE(hold_text));

	lang_format_log_row(buffer, buffer_count, time_text,
						lang_button_short_name(row->button), gap_text, hold_text,
						log_format_verdict(row->reason));
}

void log_format_time(const AppSnapshot *snapshot, uint64_t tick, LangStringId format,
					 wchar_t *buffer, size_t buffer_count)
{
	if (!buffer || buffer_count == 0)
		return;

	buffer[0] = L'\0';

	FILETIME file_time;
	SYSTEMTIME local;

	if (!snapshot || !SystemTimeToFileTime(&snapshot->now_local, &file_time))
	{
		StringCchCopyW(buffer, buffer_count, L"--:--:--.---");
		return;
	}

	uint64_t delta_ms = (snapshot->now_tick >= tick) ? (snapshot->now_tick - tick) : 0;
	uint64_t offset = delta_ms * 10000; // Milliseconds to 100 ns units

	ULARGE_INTEGER value;
	value.LowPart = file_time.dwLowDateTime;
	value.HighPart = file_time.dwHighDateTime;

	// A stamp older than the sampled clock is impossible unless the tick wrapped;
	// clamping keeps the display sane instead of showing a time far in the future
	value.QuadPart = (value.QuadPart > offset) ? (value.QuadPart - offset) : value.QuadPart;

	file_time.dwLowDateTime = value.LowPart;
	file_time.dwHighDateTime = value.HighPart;

	if (!FileTimeToSystemTime(&file_time, &local))
	{
		StringCchCopyW(buffer, buffer_count, L"--:--:--.---");
		return;
	}

	// The log pane only has room for whole seconds; the self check, which is a
	// block of text, keeps the milliseconds.
	if (format == STR_EVIDENCE_TIME_FMT)
	{
		StringCchPrintfW(buffer, buffer_count, lang_str(format),
						 (unsigned)local.wHour, (unsigned)local.wMinute,
						 (unsigned)local.wSecond, (unsigned)local.wMilliseconds);
	}
	else
	{
		StringCchPrintfW(buffer, buffer_count, lang_str(format),
						 (unsigned)local.wHour, (unsigned)local.wMinute,
						 (unsigned)local.wSecond);
	}
}

const wchar_t *log_format_verdict(DebounceReason reason)
{
	switch (reason)
	{
	case DEBOUNCE_REASON_BLOCK_THRESHOLD:
		return lang_str(STR_VERDICT_BLOCK_FAST);
	case DEBOUNCE_REASON_BLOCK_DEFERRING:
		return lang_str(STR_VERDICT_BLOCK_DEFER);
	case DEBOUNCE_REASON_BLOCK_RELEASE:
		return lang_str(STR_VERDICT_BLOCK_RELEASE);
	case DEBOUNCE_REASON_BLOCK_WHEEL_REVERSAL:
		return lang_str(STR_VERDICT_BLOCK_REVERSE);
	case DEBOUNCE_REASON_DEFER_RELEASE:
		return lang_str(STR_VERDICT_DEFER_RELEASE);
	case DEBOUNCE_REASON_IGNORE_INJECTED:
		return lang_str(STR_VERDICT_IGNORE_INJECTED);
	case DEBOUNCE_REASON_IGNORE_UNMONITORED:
		return lang_str(STR_VERDICT_IGNORE_UNMONITORED);
	case DEBOUNCE_REASON_IGNORE_WHEEL_ZERO:
		return lang_str(STR_VERDICT_IGNORE_WHEEL_ZERO);
	case DEBOUNCE_REASON_PASS:
	default:
		return lang_str(STR_VERDICT_PASS);
	}
}

// Newest first: a user opening the pane wants to see what just happened, and
// with the newest row on top no scrolling is needed to see it.
void log_pane_push(LogPane *pane, const ClickRow *row)
{
	if (!pane || !row)
		return;

	uint32_t move = (pane->count < SETTINGS_LOG_ROWS) ? pane->count : (SETTINGS_LOG_ROWS - 1);

	if (move > 0)
		memmove(&pane->rows[1], &pane->rows[0], move * sizeof(ClickRow));

	pane->rows[0] = *row;

	if (pane->count < SETTINGS_LOG_ROWS)
		pane->count++;
}

void log_pane_render(const AppSnapshot *snapshot, const LogPane *pane,
					 wchar_t *buffer, size_t buffer_count)
{
	if (!buffer || buffer_count == 0)
		return;

	buffer[0] = L'\0';

	if (!pane)
		return;

	for (uint32_t i = 0; i < pane->count; i++)
	{
		wchar_t line[SETTINGS_LINE_SIZE];
		build_log_line(snapshot, &pane->rows[i], line, ARRAYSIZE(line));

		if (i > 0 && FAILED(StringCchCatW(buffer, buffer_count, L"\r\n")))
			break;

		if (FAILED(StringCchCatW(buffer, buffer_count, line)))
			break;
	}
}

void log_format_status(const AppSnapshot *snapshot, wchar_t *buffer, size_t buffer_count)
{
	const wchar_t *base = snapshot->hook_active ? lang_str(STR_STATUS_OK) : lang_str(STR_STATUS_BAD);

	if (snapshot->hook_recoveries > 0)
		StringCchPrintfW(buffer, buffer_count, lang_str(STR_STATUS_RECOVERED_FMT), base,
						 snapshot->hook_recoveries);
	else
		StringCchCopyW(buffer, buffer_count, base);

	// The capability summary is NOT appended here any more: it has its own four
	// controls, each with its own colour. Keeping it inside this string meant the
	// whole sentence had to be repainted whenever any one of them changed.
}

void log_format_lf_to_crlf(const wchar_t *source, wchar_t *destination, size_t destination_count)
{
	if (!destination || destination_count == 0)
		return;

	size_t written = 0;

	for (const wchar_t *p = source; p && *p && written + 3 < destination_count; p++)
	{
		if (*p == L'\r')
			continue;

		if (*p == L'\n')
		{
			destination[written++] = L'\r';
			destination[written++] = L'\n';
			continue;
		}

		destination[written++] = *p;
	}

	destination[written] = L'\0';
}

void log_format_evidence(const AppSnapshot *snapshot, wchar_t *buffer, size_t buffer_count)
{
	if (!buffer || buffer_count == 0)
		return;

	buffer[0] = L'\0';
	StringCchCatW(buffer, buffer_count, lang_str(STR_EVIDENCE_HEADER));
	StringCchCatW(buffer, buffer_count, L"\n");

	bool any = false;

	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		const AppBlockEvidence *evidence = &snapshot->evidence[i];

		if (!evidence->valid || evidence->blocks == 0)
			continue;

		// The evidence speaks the user's language: the time of the press before
		// and the time of the one that was blocked, and the gap between those two
		// presses. The engine compares against the previous event of any kind, so
		// when the two numbers differ the report says what the engine used.
		uint64_t gap_ms = evidence->has_press_gap ? evidence->press_gap_ms : evidence->elapsed_ms;
		uint64_t previous_tick = (evidence->timestamp >= gap_ms) ? (evidence->timestamp - gap_ms) : 0;

		wchar_t previous[SETTINGS_TIME_SIZE];
		wchar_t latest[SETTINGS_TIME_SIZE];
		wchar_t gap[32];
		wchar_t limit[32];

		log_format_time(snapshot, previous_tick, STR_EVIDENCE_TIME_FMT,
						previous, ARRAYSIZE(previous));
		log_format_time(snapshot, evidence->timestamp, STR_EVIDENCE_TIME_FMT,
						latest, ARRAYSIZE(latest));

		StringCchPrintfW(gap, ARRAYSIZE(gap), lang_str(STR_MS_FMT), (int)gap_ms);
		StringCchPrintfW(limit, ARRAYSIZE(limit), lang_str(STR_MS_FMT), (int)evidence->threshold_ms);

		wchar_t line[256];
		StringCchPrintfW(line, ARRAYSIZE(line), lang_str(STR_EVIDENCE_FMT),
						 lang_button_name((MouseButton)i), evidence->blocks,
						 previous, latest, gap, limit);

		StringCchCatW(buffer, buffer_count, line);

		if (evidence->has_press_gap && evidence->elapsed_ms != gap_ms)
		{
			// The argument is a duration, and the format says so: passing it to a
			// %s made printf read the millisecond count as a string pointer and
			// walk into address 0x2E. Keep the two in step - the translation test
			// only compares the two languages with each other, so it cannot see a
			// mismatch between a format and its call site.
			unsigned engine_ms = (evidence->elapsed_ms > 999999)
									 ? 999999u
									 : (unsigned)evidence->elapsed_ms;

			wchar_t engine[64];
			StringCchPrintfW(engine, ARRAYSIZE(engine), lang_str(STR_EVIDENCE_ENGINE_FMT),
							 engine_ms);
			StringCchCatW(buffer, buffer_count, engine);
		}

		StringCchCatW(buffer, buffer_count, L"\n");
		any = true;
	}

	if (!any)
	{
		StringCchCatW(buffer, buffer_count, lang_str(STR_EVIDENCE_NONE));
		StringCchCatW(buffer, buffer_count, L"\n");
	}
}
