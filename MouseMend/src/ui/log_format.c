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

// The release that closes one press, or NULL.
//
// entries are newest first, so a press's release sits at a smaller index than the
// press itself. The walk stops at the first entry of the same button: if that is
// another press, this press never got a release (the button was pressed again
// first), which is exactly the case the test log reports as an expired click.
static const TraceEntry *find_live_release(const TraceEntry *entries, bool *claimed, uint32_t index)
{
	for (uint32_t j = index; j-- > 0;)
	{
		if (entries[j].button != entries[index].button)
			continue;

		if (entries[j].is_down || claimed[j])
			return NULL;

		claimed[j] = true;
		return &entries[j];
	}

	return NULL;
}

void log_format_live_pane(const AppSnapshot *snapshot, const TraceEntry *entries, uint32_t count,
						  wchar_t *buffer, size_t buffer_count)
{
	if (!buffer || buffer_count == 0)
		return;

	buffer[0] = L'\0';

	if (!snapshot || !entries)
		return;

	bool wrote_anything = false;
	const wchar_t *current_program = NULL;

	// Entries already used as the release of an older press. Static, like the
	// other buffers here: one window, one UI thread, 96 bytes.
	static bool claimed[EVENT_HISTORY_CAPACITY];

	if (count > ARRAYSIZE(claimed))
		count = ARRAYSIZE(claimed);

	memset(claimed, 0, sizeof(claimed));

	for (uint32_t i = 0; i < count; i++)
	{
		const TraceEntry *entry = &entries[i];

		// One row per action. A release is never a row of its own: it is the
		// second half of a press, consumed below. A scroll has no press at all,
		// so it is an action on its own.
		if (!entry->is_down && !config_is_wheel_button(entry->button))
			continue;

		// Newest first, so entries[0] is the most recent row and a group starts
		// when this row's program differs from the last one written above it. The
		// label therefore sits above the rows it describes and reads as a
		// heading, which is why it names the program instead of saying "switched
		// to": a transition verb would point the wrong way up the list.
		if (!current_program || wcscmp(entry->process, current_program) != 0)
		{
			wchar_t section[SETTINGS_LINE_SIZE];

			lang_format_live_section(section, ARRAYSIZE(section), entry->process);

			if (wrote_anything && FAILED(StringCchCatW(buffer, buffer_count, L"\r\n")))
				break;

			if (FAILED(StringCchCatW(buffer, buffer_count, section)))
				break;

			current_program = entry->process;
			wrote_anything = true;
		}

		wchar_t time_text[SETTINGS_TIME_SIZE];
		wchar_t gap_text[SETTINGS_CELL_SIZE];
		wchar_t hold_text[SETTINGS_CELL_SIZE];
		wchar_t line[SETTINGS_LINE_SIZE];

		log_format_time(snapshot, entry->timestamp, STR_LOG_TIME_FMT,
						time_text, ARRAYSIZE(time_text));

		// The first event of a session has nothing to compare against, and the
		// dash says so instead of inventing a zero.
		format_cell_number(entry->decision.elapsed_ms, !entry->decision.first_event,
						   gap_text, ARRAYSIZE(gap_text));

		// The hold time of this click. Nothing here is estimated: it is the
		// difference between two timestamps the hook actually recorded, the same
		// subtraction the test log makes - only the pairing happens here instead
		// of in the window. Three outcomes, and only one of them is a number:
		//   - a release was found        -> the measured milliseconds;
		//   - the button is still down   -> a dash, "not known yet";
		//   - no release and past the    -> ">2000", a lower bound, exactly the
		//     engine's pairing timeout      wording the test log uses for a lost
		//                                    release. Never a made up number.
		if (config_is_wheel_button(entry->button))
		{
			StringCchCopyW(hold_text, ARRAYSIZE(hold_text), lang_str(STR_LOG_GAP_NONE));
		}
		else
		{
			const TraceEntry *release = find_live_release(entries, claimed, i);

			if (release)
			{
				uint64_t hold = (release->timestamp >= entry->timestamp)
									? (release->timestamp - entry->timestamp)
									: 0;

				format_cell_number(hold, true, hold_text, ARRAYSIZE(hold_text));
			}
			else
			{
				bool expired = snapshot->now_tick >= entry->timestamp &&
							   (snapshot->now_tick - entry->timestamp) >= (uint64_t)SETTINGS_CLICK_EXPIRE_MS;

				if (expired)
					StringCchPrintfW(hold_text, ARRAYSIZE(hold_text), L">%I32u",
									 (uint32_t)SETTINGS_CLICK_EXPIRE_MS);
				else
					StringCchCopyW(hold_text, ARRAYSIZE(hold_text), lang_str(STR_LOG_GAP_NONE));
			}
		}

		lang_format_log_row(line, ARRAYSIZE(line), time_text,
							lang_button_short_name(entry->button), gap_text, hold_text,
							log_format_verdict(entry->decision.reason));

		if (wrote_anything && FAILED(StringCchCatW(buffer, buffer_count, L"\r\n")))
			break;

		if (FAILED(StringCchCatW(buffer, buffer_count, line)))
			break;

		wrote_anything = true;
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
