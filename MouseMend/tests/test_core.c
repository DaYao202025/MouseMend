// Console test suite for the pure (non-UI) parts of MouseMend.
// It covers the tray menu ID codec, the language tables and the debounce state
// machine, which are the places where a silent mismatch would break behaviour.

#include <stdio.h>
#include <wchar.h>
#include <windows.h>
#include <strsafe.h>

#include "test_common.h"
#include "../menu_ids.h"
#include "../version.h"
#include "../src/core/app_snapshot.h"
#include "../src/core/config.h"
#include "../src/core/debouncer.h"
#include "../src/core/event_trace.h"
#include "../src/core/mouse_hook.h"
#include "../src/core/notify_policy.h"
#include "../src/core/event_history.h"
#include "../src/ui/language.h"
#include "../src/ui/log_format.h"
#include "../src/utils/support_bundle.h"
#include "../src/utils/zip_writer.h"
#include "../src/ui/ripple_view.h"
#include "../src/ui/tray_icon.h"
#include "../src/ui/window_layout.h"

int g_checks = 0;
int g_failures = 0;

void test_smart_drag_run(void);

// Implemented in test_v2.c
void test_hook_watchdog(void);
void test_app_controller(void);
void test_block_log(void);
void test_block_log_files(void);

// Count printf placeholders in a format string
static int count_placeholders(const wchar_t *text)
{
	int count = 0;

	for (const wchar_t *p = text; *p; p++)
	{
		if (*p == L'%')
			count++;
	}

	return count;
}

// Normalized shape of the placeholders in a format string: 's' for a string, 'n'
// for a number, in order.
//
// Counting '%' is not enough: replacing a %s with %I32u keeps the count and still
// makes the printf read a pointer as an integer, which prints garbage or crashes.
static void placeholder_signature(const wchar_t *text, char *buffer, size_t buffer_count)
{
	size_t written = 0;

	if (!buffer || buffer_count == 0)
		return;

	for (const wchar_t *p = text; p && *p && written + 1 < buffer_count; p++)
	{
		if (*p != L'%')
			continue;

		// Skip flags, width and precision to reach the conversion character
		const wchar_t *conversion = p + 1;
		while (*conversion && !iswalpha((wint_t)*conversion))
			conversion++;

		if (*conversion == L'\0')
			break;

		buffer[written++] = (*conversion == L's' || *conversion == L'S') ? 's' : 'n';
		p = conversion;
	}

	buffer[written] = '\0';
}

// Verify that every menu ID produced by the generator round-trips through the decoder
static void test_menu_id_codec(void)
{
	printf("menu id codec\n");

	for (int button = 0; button < MOUSE_BUTTON_COUNT; button++)
	{
		for (int index = 0; index < 3; index++)
		{
			UINT id = IDM_THRESHOLD_PRESET_ID(button, index);
			int decoded_button = -1;
			int decoded_index = -1;

			CHECK(menu_id_is_threshold_preset(id, &decoded_button, &decoded_index));
			CHECK(decoded_button == button);
			CHECK(decoded_index == index);

			// A preset ID must never be mistaken for a custom threshold
			CHECK(!menu_id_is_threshold_custom(id, &decoded_button, MOUSE_BUTTON_COUNT));
		}

		UINT custom_id = IDM_THRESHOLD_CUSTOM_ID(button);
		int decoded_button = -1;
		int decoded_index = -1;

		CHECK(menu_id_is_threshold_custom(custom_id, &decoded_button, MOUSE_BUTTON_COUNT));
		CHECK(decoded_button == button);

		decoded_button = -1;
		CHECK(!menu_id_is_threshold_preset(custom_id, &decoded_button, &decoded_index));
	}
}

// Verify that no two generated IDs collide with each other or with command IDs
static void test_menu_id_uniqueness(void)
{
	printf("menu id uniqueness\n");

	UINT ids[MOUSE_BUTTON_COUNT * 5];
	int count = 0;

	for (int button = 0; button < MOUSE_BUTTON_COUNT; button++)
	{
		ids[count++] = (UINT)(IDM_TOGGLE_BASE + button);
		ids[count++] = IDM_THRESHOLD_CUSTOM_ID(button);

		for (int index = 0; index < 3; index++)
			ids[count++] = IDM_THRESHOLD_PRESET_ID(button, index);
	}

	for (int i = 0; i < count; i++)
	{
		for (int j = i + 1; j < count; j++)
			CHECK(ids[i] != ids[j]);
	}

	// Command IDs and threshold IDs must live in different ranges
	CHECK((UINT)IDM_EXIT < (UINT)IDM_THRESHOLD_BASE);
	CHECK((UINT)IDM_LANG_ZH < (UINT)IDM_THRESHOLD_BASE);
	CHECK((UINT)IDM_THRESHOLD_BASE < (UINT)IDM_THRESHOLD_CUSTOM);

	// Every generated ID must be well above the WM_USER boundary
	for (int i = 0; i < count; i++)
		CHECK(ids[i] >= (UINT)WM_USER);

	// Out-of-range IDs must be rejected by both decoders
	int button = -1;
	int index = -1;

	CHECK(!menu_id_is_threshold_preset(0, &button, &index));
	CHECK(!menu_id_is_threshold_custom(0, &button, MOUSE_BUTTON_COUNT));
	CHECK(!menu_id_is_threshold_preset(IDM_THRESHOLD_CUSTOM, &button, &index));
}

// Verify that both language tables are complete and consistent
static void test_language_tables(void)
{
	printf("language tables\n");

	Language saved = language_get();

	for (int language = 0; language < LANG_COUNT; language++)
	{
		language_set((Language)language);
		CHECK(language_get() == (Language)language);

		for (int id = 0; id < STR_COUNT; id++)
		{
			const wchar_t *text = lang_str((LangStringId)id);

			CHECK(text != NULL);
			if (text)
				CHECK(text[0] != L'\0');
		}

		for (int button = 0; button < MOUSE_BUTTON_COUNT; button++)
		{
			const wchar_t *name = lang_button_name((MouseButton)button);

			CHECK(name != NULL);
			if (name)
				CHECK(name[0] != L'\0');
		}
	}

	// Placeholder counts and types must match between the two translations
	for (int id = 0; id < STR_COUNT; id++)
	{
		language_set(LANG_EN);
		int english = count_placeholders(lang_str((LangStringId)id));
		char english_shape[24];
		placeholder_signature(lang_str((LangStringId)id), english_shape, ARRAYSIZE(english_shape));

		language_set(LANG_ZH_CN);
		int chinese = count_placeholders(lang_str((LangStringId)id));
		char chinese_shape[24];
		placeholder_signature(lang_str((LangStringId)id), chinese_shape, ARRAYSIZE(chinese_shape));

		if (english != chinese)
			printf("[FAIL] placeholder mismatch for string %d\n", id);

		if (strcmp(english_shape, chinese_shape) != 0)
			printf("[FAIL] placeholder types differ for string %d: %s vs %s\n", id, english_shape, chinese_shape);

		CHECK(english == chinese);
		CHECK(strcmp(english_shape, chinese_shape) == 0);
	}

	// A few strings must actually differ, otherwise the switch is a no-op
	language_set(LANG_EN);
	const wchar_t *english_exit = lang_str(STR_EXIT);
	const wchar_t *english_enable = lang_str(STR_ENABLE);

	language_set(LANG_ZH_CN);
	CHECK(wcscmp(english_exit, lang_str(STR_EXIT)) != 0);
	CHECK(wcscmp(english_enable, lang_str(STR_ENABLE)) != 0);

	// The caption carries the version, which comes from version.h rather than
	// from the translation table. If that wiring ever breaks, the title bar shows
	// a stale number while the executable's resources show the new one.
	wchar_t title[LANG_TITLE_MAX_CHARS];

	language_set(LANG_EN);
	lang_format_settings_title(title, ARRAYSIZE(title));
	CHECK(wcsstr(title, APP_VERSION_STRING_W) != NULL);
	CHECK(wcsstr(title, APP_NAME_W) != NULL);
	CHECK(wcslen(title) < LANG_TITLE_MAX_CHARS - 1);

	language_set(LANG_ZH_CN);
	lang_format_settings_title(title, ARRAYSIZE(title));
	CHECK(wcsstr(title, APP_VERSION_STRING_W) != NULL);
	CHECK(wcsstr(title, APP_NAME_W) != NULL);
	CHECK(wcsstr(title, L"\u2014") != NULL);
	CHECK(wcslen(title) < LANG_TITLE_MAX_CHARS - 1);

	// A truncated destination must still produce a terminated string, never a
	// half written version number. The variable is not called "small" because
	// rpcndr.h still defines that as char for MIDL compatibility.
	wchar_t narrow[16];
	lang_format_settings_title(narrow, ARRAYSIZE(narrow));
	CHECK(wcslen(narrow) < ARRAYSIZE(narrow));

	// Out-of-range access must not crash and must return an empty string
	CHECK(lang_str((LangStringId)STR_COUNT) != NULL);
	CHECK(lang_button_name((MouseButton)MOUSE_BUTTON_COUNT) != NULL);

	// Invalid language values must be ignored
	language_set((Language)LANG_COUNT);
	CHECK(language_get() == LANG_ZH_CN);

	// Language names are endonyms and must never be empty
	CHECK(lang_language_name(LANG_EN)[0] != L'\0');
	CHECK(lang_language_name(LANG_ZH_CN)[0] != L'\0');

	// System detection must produce a supported language
	Language detected = language_detect_system();
	CHECK(detected >= 0 && detected < LANG_COUNT);

	language_set(saved);
}

// Verify that every preset and quick-pick value is inside the allowed range
static void test_config_tables(void)
{
	printf("config tables\n");

	const PresetConfig *presets[] = {&CONFIG_PRESET_DEFAULT, &CONFIG_PRESET_OFFICE, &CONFIG_PRESET_STRICT};

	for (int p = 0; p < (int)(sizeof(presets) / sizeof(presets[0])); p++)
	{
		for (int button = 0; button < MOUSE_BUTTON_COUNT; button++)
		{
			int value = presets[p]->thresholds[button];

			CHECK(value >= THRESHOLD_MIN_VALUE);
			CHECK(value <= THRESHOLD_MAX_VALUE);
		}
	}

	for (int button = 0; button < MOUSE_BUTTON_COUNT; button++)
	{
		const int *thresholds = config_threshold_presets((MouseButton)button);

		for (int i = 0; i < THRESHOLD_PRESET_COUNT; i++)
		{
			CHECK(thresholds[i] >= THRESHOLD_MIN_VALUE);
			CHECK(thresholds[i] <= THRESHOLD_MAX_VALUE);

			for (int j = i + 1; j < THRESHOLD_PRESET_COUNT; j++)
				CHECK(thresholds[i] != thresholds[j]);
		}
	}

	// Wheel axes share one list, buttons share another
	CHECK(config_is_wheel_button(MOUSE_BUTTON_WHEEL));
	CHECK(config_is_wheel_button(MOUSE_BUTTON_HWHEEL));
	CHECK(!config_is_wheel_button(MOUSE_BUTTON_LEFT));
	CHECK(config_threshold_presets(MOUSE_BUTTON_WHEEL) == config_threshold_presets(MOUSE_BUTTON_HWHEEL));
	CHECK(config_threshold_presets(MOUSE_BUTTON_LEFT) != config_threshold_presets(MOUSE_BUTTON_WHEEL));

	// Tuning quick-picks must be usable and must contain the defaults
	const int *delays = config_release_delay_presets();
	bool delay_has_default = false;

	for (int i = 0; i < RELEASE_DELAY_PRESET_COUNT; i++)
	{
		CHECK(delays[i] >= RELEASE_DELAY_MIN_MS);
		CHECK(delays[i] <= RELEASE_DELAY_MAX_MS);

		if (delays[i] == RELEASE_DELAY_DEFAULT_MS)
			delay_has_default = true;

		for (int j = i + 1; j < RELEASE_DELAY_PRESET_COUNT; j++)
			CHECK(delays[i] != delays[j]);
	}
	CHECK(delay_has_default);

	const int *distances = config_drag_distance_presets();
	bool distance_has_default = false;

	for (int i = 0; i < DRAG_DISTANCE_PRESET_COUNT; i++)
	{
		CHECK(distances[i] >= DRAG_DISTANCE_MIN_PX);
		CHECK(distances[i] <= DRAG_DISTANCE_MAX_PX);

		if (distances[i] == DRAG_DISTANCE_DEFAULT_PX)
			distance_has_default = true;

		for (int j = i + 1; j < DRAG_DISTANCE_PRESET_COUNT; j++)
			CHECK(distances[i] != distances[j]);
	}
	CHECK(distance_has_default);

	// The defaults themselves must be valid
	CHECK(RELEASE_DELAY_DEFAULT_MS >= RELEASE_DELAY_MIN_MS);
	CHECK(RELEASE_DELAY_DEFAULT_MS <= RELEASE_DELAY_MAX_MS);
	CHECK(DRAG_DISTANCE_DEFAULT_PX >= DRAG_DISTANCE_MIN_PX);
	CHECK(DRAG_DISTANCE_DEFAULT_PX <= DRAG_DISTANCE_MAX_PX);
	CHECK(DRAG_HOLD_TIME_DEFAULT_MS >= DRAG_HOLD_TIME_MIN_MS);
	CHECK(DRAG_HOLD_TIME_DEFAULT_MS <= DRAG_HOLD_TIME_MAX_MS);
}

// Verify the tray hover text: length limit, version, state and counters
static void test_tooltip_text(void)
{
	printf("tooltip text\n");

	Language saved = language_get();
	wchar_t tooltip[256];

	for (int language = 0; language < LANG_COUNT; language++)
	{
		language_set((Language)language);

		// All buttons on, hook active, with a blocked count
		lang_format_tooltip(tooltip, ARRAYSIZE(tooltip), MOUSE_BUTTON_COUNT, MOUSE_BUTTON_COUNT, 12, true);
		CHECK(tooltip[0] != L'\0');
		CHECK(wcslen(tooltip) < TRAY_TOOLTIP_MAX_CHARS);
		CHECK(wcsstr(tooltip, APP_NAME_W) != NULL);
		CHECK(wcsstr(tooltip, APP_VERSION_STRING_W) != NULL);
		CHECK(wcsstr(tooltip, lang_str(STR_TOOLTIP_ON)) != NULL);
		CHECK(wcsstr(tooltip, L"12") != NULL);
		CHECK(wcsstr(tooltip, L"\n") != NULL);

		// Exactly two lines: more wraps badly in some shell versions
		int newlines = 0;
		for (const wchar_t *p = tooltip; *p; p++)
		{
			if (*p == L'\n')
				newlines++;
		}
		CHECK(newlines == 1);

		// Everything off
		lang_format_tooltip(tooltip, ARRAYSIZE(tooltip), 0, MOUSE_BUTTON_COUNT, 0, true);
		CHECK(wcsstr(tooltip, lang_str(STR_TOOLTIP_OFF)) != NULL);
		CHECK(wcsstr(tooltip, lang_str(STR_TOOLTIP_NO_BLOCKS)) != NULL);

		// Partially enabled
		lang_format_tooltip(tooltip, ARRAYSIZE(tooltip), 3, MOUSE_BUTTON_COUNT, 5, true);
		CHECK(wcsstr(tooltip, lang_str(STR_TOOLTIP_PARTIAL)) != NULL);

		// Hook inactive: one explanatory line, no blocked counter that would
		// contradict "protection is not active"
		lang_format_tooltip(tooltip, ARRAYSIZE(tooltip), MOUSE_BUTTON_COUNT, MOUSE_BUTTON_COUNT, 5, false);
		CHECK(wcsstr(tooltip, lang_str(STR_STATUS_UNAVAILABLE)) != NULL);
		CHECK(wcsstr(tooltip, L"5") == NULL);
		CHECK(wcsstr(tooltip, lang_str(STR_TOOLTIP_BLOCKED_FMT)) == NULL);

		// A huge counter must not overflow the shell limit
		lang_format_tooltip(tooltip, ARRAYSIZE(tooltip), MOUSE_BUTTON_COUNT, MOUSE_BUTTON_COUNT, UINT32_MAX, true);
		CHECK(wcslen(tooltip) < TRAY_TOOLTIP_MAX_CHARS);

		// Out-of-range values are clamped instead of producing nonsense
		lang_format_tooltip(tooltip, ARRAYSIZE(tooltip), -5, MOUSE_BUTTON_COUNT, 0, true);
		CHECK(wcsstr(tooltip, lang_str(STR_TOOLTIP_OFF)) != NULL);

		lang_format_tooltip(tooltip, ARRAYSIZE(tooltip), 99, MOUSE_BUTTON_COUNT, 0, true);
		CHECK(wcsstr(tooltip, lang_str(STR_TOOLTIP_ON)) != NULL);
	}

	// A short destination must be truncated, not overflowed
	// (not named "small": rpcndr.h defines that as a legacy macro)
	wchar_t tiny_buffer[8];
	lang_format_tooltip(tiny_buffer, ARRAYSIZE(tiny_buffer), MOUSE_BUTTON_COUNT, MOUSE_BUTTON_COUNT, 12345, true);
	CHECK(wcslen(tiny_buffer) < ARRAYSIZE(tiny_buffer));

	// NULL and zero-size destinations must be tolerated
	lang_format_tooltip(NULL, 0, 0, 0, 0, true);
	lang_format_tooltip(tooltip, 0, 0, 0, 0, true);

	language_set(saved);
}

// Verify that both balloon texts fit the shell limit in every language
// A translation that grows past szInfo would be silently cut in half on screen.
static void test_balloon_text_fits(void)
{
	printf("balloon text fits\n");

	Language saved = language_get();
	wchar_t text[TRAY_BALLOON_MAX_CHARS + 64];

	for (int language = 0; language < LANG_COUNT; language++)
	{
		language_set((Language)language);

		// Worst case for the counter text: largest number, longest button name
		StringCchPrintfW(text, ARRAYSIZE(text), lang_str(STR_NOTIFY_BLOCKED_FMT),
						 UINT32_MAX, lang_button_name(MOUSE_BUTTON_HWHEEL));

		if (wcslen(text) >= TRAY_BALLOON_MAX_CHARS)
			printf("[FAIL] blocked text is %u chars in language %d\n", (unsigned)wcslen(text), language);

		CHECK(wcslen(text) < TRAY_BALLOON_MAX_CHARS);

		// Worst case for the "notifications are on" text
		StringCchPrintfW(text, ARRAYSIZE(text), lang_str(STR_NOTIFY_ON_TEXT_FMT), NOTIFY_MIN_INTERVAL_MS / 1000);

		if (wcslen(text) >= TRAY_BALLOON_MAX_CHARS)
			printf("[FAIL] notification text is %u chars in language %d\n", (unsigned)wcslen(text), language);

		CHECK(wcslen(text) < TRAY_BALLOON_MAX_CHARS);
		CHECK(wcslen(text) > 0);
	}

	language_set(saved);
}

// Verify the per-button breakdown shown under the blocked counter
static void test_blocked_button_list(void)
{
	printf("blocked button list\n");

	DebounceManager manager;
	if (!debounce_init(&manager))
	{
		printf("[FAIL] debounce_init\n");
		g_failures++;
		return;
	}

	MouseButton buttons[MOUSE_BUTTON_COUNT];

	// Nothing blocked yet: the caller must get zero entries so it can keep the
	// counter a plain label instead of showing an empty submenu.
	CHECK(debounce_collect_blocked_buttons(&manager, buttons, MOUSE_BUTTON_COUNT) == 0);
	CHECK(debounce_collect_blocked_buttons(NULL, buttons, MOUSE_BUTTON_COUNT) == 0);
	CHECK(debounce_collect_blocked_buttons(&manager, NULL, MOUSE_BUTTON_COUNT) == 0);
	CHECK(debounce_collect_blocked_buttons(&manager, buttons, 0) == 0);

	debounce_set_monitored(&manager, MOUSE_BUTTON_LEFT, true);
	debounce_set_monitored(&manager, MOUSE_BUTTON_RIGHT, true);
	debounce_set_threshold(&manager, MOUSE_BUTTON_LEFT, 50, 1, 200);
	debounce_set_threshold(&manager, MOUSE_BUTTON_RIGHT, 50, 1, 200);

	// Drag protection would defer the release and add a second blocked event,
	// which would make the expected count harder to read in this test.
	debounce_set_hybrid_heuristic(&manager, false);

	uint64_t base = GetTickCount64();

	// One bounce on the left button: press, release, then a press inside the window.
	// Every element must be zero initialised: an uninitialised is_injected flag
	// silently turns the event into "injected" and the test then fails only on
	// some architectures, depending on what the stack happens to contain.
	MouseEvent events[4] = {0};

	for (int i = 0; i < 4; i++)
	{
		events[i].x = 10;
		events[i].y = 10;
	}

	events[0].button = MOUSE_BUTTON_LEFT;
	events[0].timestamp = base + 1000;
	events[0].is_down = true;

	events[1].button = MOUSE_BUTTON_LEFT;
	events[1].timestamp = base + 1010;
	events[1].is_down = false;

	events[2].button = MOUSE_BUTTON_LEFT;
	events[2].timestamp = base + 1020;
	events[2].is_down = true;

	events[3].button = MOUSE_BUTTON_LEFT;
	events[3].timestamp = base + 1030;
	events[3].is_down = false;

	for (int i = 0; i < 4; i++)
		debounce_process_event(&manager, &events[i]);

	CHECK(debounce_get_button_blocks(&manager, MOUSE_BUTTON_LEFT) == 1);
	CHECK(debounce_collect_blocked_buttons(&manager, buttons, MOUSE_BUTTON_COUNT) == 1);
	CHECK(buttons[0] == MOUSE_BUTTON_LEFT);

	// The parent row shows the total and the submenu shows the breakdown, so the
	// two numbers must always agree - a menu that says "5 blocked" while listing
	// 1 + 2 would make the whole feature look broken.
	{
		uint32_t total = debounce_get_total_blocks(&manager);
		uint32_t sum = 0;

		for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
			sum += debounce_get_button_blocks(&manager, (MouseButton)i);

		CHECK(total == sum);
		CHECK(total == 1);
	}

	// A right-button bounce must be appended, and the order must stay the enum order
	events[0].button = MOUSE_BUTTON_RIGHT;
	events[1].button = MOUSE_BUTTON_RIGHT;
	events[2].button = MOUSE_BUTTON_RIGHT;
	events[3].button = MOUSE_BUTTON_RIGHT;

	events[0].timestamp = base + 2000;
	events[1].timestamp = base + 2010;
	events[2].timestamp = base + 2020;
	events[3].timestamp = base + 2030;

	for (int i = 0; i < 4; i++)
		debounce_process_event(&manager, &events[i]);

	CHECK(debounce_collect_blocked_buttons(&manager, buttons, MOUSE_BUTTON_COUNT) == 2);
	CHECK(buttons[0] == MOUSE_BUTTON_LEFT);
	CHECK(buttons[1] == MOUSE_BUTTON_RIGHT);

	// The total must still equal the sum once two buttons are involved
	CHECK(debounce_get_total_blocks(&manager) == 2);

	// Respect the caller's capacity instead of overrunning the buffer
	CHECK(debounce_collect_blocked_buttons(&manager, buttons, 1) == 1);

	// A capacity smaller than the button count keeps the earliest entries in enum
	// order, so a truncated list is still a prefix of the full one
	{
		MouseButton prefix[MOUSE_BUTTON_COUNT];
		int prefix_count = debounce_collect_blocked_buttons(&manager, prefix, 1);

		CHECK(prefix_count == 1);
		CHECK(prefix[0] == buttons[0]);
	}

	// Clearing the counter must empty the breakdown, which is what makes the
	// submenu disappear again
	debounce_reset_statistics(&manager);
	CHECK(debounce_collect_blocked_buttons(&manager, buttons, MOUSE_BUTTON_COUNT) == 0);

	// Label formatting must name the button and carry the number, in both languages
	Language saved = language_get();
	wchar_t line[64];

	for (int language = 0; language < LANG_COUNT; language++)
	{
		language_set((Language)language);

		const wchar_t *result = lang_format_blocked_button(line, ARRAYSIZE(line), MOUSE_BUTTON_X1, 7);
		CHECK(result == line);
		CHECK(wcslen(line) > 0);
		CHECK(wcsstr(line, lang_button_name(MOUSE_BUTTON_X1)) != NULL);
		CHECK(wcsstr(line, L"7") != NULL);
	}

	// Degenerate arguments must not crash or leave garbage
	CHECK(lang_format_blocked_button(NULL, 0, MOUSE_BUTTON_LEFT, 1) != NULL);
	CHECK(lang_format_blocked_button(line, 0, MOUSE_BUTTON_LEFT, 1) != NULL);
	CHECK(lang_format_blocked_button(line, ARRAYSIZE(line), (MouseButton)MOUSE_BUTTON_COUNT, 1) != NULL);

	language_set(saved);
	debounce_cleanup(&manager);
}

// Verify the balloon throttling policy
static void test_notify_policy(void)
{
	printf("notification policy\n");

	const uint64_t interval = 20000;
	NotifyDecision decision;

	// First block ever: show immediately, no throttle history
	NotifyState state = {0, 0};
	CHECK(notify_policy_decide(state, 1, 5000, interval, &decision));
	CHECK(decision.show);
	CHECK(decision.delta == 1);
	CHECK(decision.update_state);
	CHECK(decision.next_state.last_tick == 5000);
	CHECK(decision.next_state.reported_blocks == 1);

	// Nothing new: stay quiet and keep the state
	state.last_tick = 5000;
	state.reported_blocks = 1;
	CHECK(notify_policy_decide(state, 1, 6000, interval, &decision));
	CHECK(!decision.show);
	CHECK(!decision.update_state);

	// New blocks but inside the throttle window: quiet, state untouched so the
	// pending blocks are reported later instead of being lost
	CHECK(notify_policy_decide(state, 4, 9000, interval, &decision));
	CHECK(!decision.show);
	CHECK(!decision.update_state);
	CHECK(decision.next_state.reported_blocks == 1);

	// Window elapsed: report the accumulated delta (4 - 1 = 3), not just one
	CHECK(notify_policy_decide(state, 4, 25000, interval, &decision));
	CHECK(decision.show);
	CHECK(decision.delta == 3);
	CHECK(decision.update_state);
	CHECK(decision.next_state.reported_blocks == 4);

	// Exactly at the boundary must be allowed (uses < for the comparison)
	state.last_tick = 25000;
	state.reported_blocks = 4;
	CHECK(notify_policy_decide(state, 5, 25000 + interval, interval, &decision));
	CHECK(decision.show);
	CHECK(decision.delta == 1);

	// One tick earlier must still be throttled
	CHECK(notify_policy_decide(state, 5, 25000 + interval - 1, interval, &decision));
	CHECK(!decision.show);

	// Counter reset (user pressed "clear blocked count") must not underflow
	state.last_tick = 25000;
	state.reported_blocks = 40;
	CHECK(notify_policy_decide(state, 0, 60000, interval, &decision));
	CHECK(!decision.show);
	CHECK(decision.delta == 0);

	// A clock that appears to go backwards must not fire
	state.last_tick = 90000;
	state.reported_blocks = 1;
	CHECK(notify_policy_decide(state, 5, 1000, interval, &decision));
	CHECK(!decision.show);

	// Invalid output pointer is tolerated
	CHECK(!notify_policy_decide(state, 5, 200000, interval, NULL));

	// Huge counters must not overflow the delta
	state.last_tick = 0;
	state.reported_blocks = 0;
	CHECK(notify_policy_decide(state, UINT32_MAX, 1000, interval, &decision));
	CHECK(decision.show);
	CHECK(decision.delta == UINT32_MAX);
}

// Verify that the version is spelled the same way everywhere it is duplicated
static void test_version_consistency(void)
{
	printf("version consistency\n");

	wchar_t wide[32] = {0};
	size_t converted = 0;

	CHECK(mbstowcs_s(&converted, wide, ARRAYSIZE(wide), APP_VERSION_STRING, _TRUNCATE) == 0);
	CHECK(wcscmp(wide, APP_VERSION_STRING_W) == 0);

	// The menu and the About entry compose the version from the numbers
	wchar_t composed[32];
	StringCchPrintfW(composed, ARRAYSIZE(composed), L"%d.%d.%d",
					 APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH);
	CHECK(wcscmp(composed, APP_VERSION_STRING_W) == 0);
}

// Verify the button enum layout that the ID encoding and the registry keys rely on
static void test_button_enum_layout(void)
{
	printf("button enum layout\n");

	CHECK(MOUSE_BUTTON_LEFT == 0);
	CHECK(MOUSE_BUTTON_RIGHT == 1);
	CHECK(MOUSE_BUTTON_MIDDLE == 2);
	CHECK(MOUSE_BUTTON_X1 == 3);
	CHECK(MOUSE_BUTTON_X2 == 4);
	CHECK(MOUSE_BUTTON_WHEEL == 5);
	CHECK(MOUSE_BUTTON_HWHEEL == 6);
	CHECK(MOUSE_BUTTON_COUNT == 7);
}

// The check-up window renders the engine's verdict, so the decision struct has
// to carry the gap and the threshold that produced it. If this drifts, the
// window would tell a user something the hook never did.
static void test_debounce_decision(void)
{
	printf("debounce decision\n");

	DebounceManager manager;
	if (!debounce_init(&manager))
	{
		printf("[FAIL] debounce_init\n");
		g_failures++;
		return;
	}

	debounce_set_monitored(&manager, MOUSE_BUTTON_LEFT, true);
	debounce_set_monitored(&manager, MOUSE_BUTTON_RIGHT, false);
	debounce_set_monitored(&manager, MOUSE_BUTTON_WHEEL, true);
	debounce_set_threshold(&manager, MOUSE_BUTTON_LEFT, 50, 1, 200);
	debounce_set_threshold(&manager, MOUSE_BUTTON_WHEEL, 50, 1, 200);

	// Drag protection would hold the release and change what the release reports
	debounce_set_hybrid_heuristic(&manager, false);

	MouseEvent events[4] = {0};
	for (int i = 0; i < 4; i++)
	{
		events[i].button = MOUSE_BUTTON_LEFT;
		events[i].x = 100;
		events[i].y = 100;
	}

	events[0].timestamp = 1000;
	events[0].is_down = true;
	events[1].timestamp = 1040;
	events[1].is_down = false;
	events[2].timestamp = 1060;
	events[2].is_down = true;
	events[3].timestamp = 1080;
	events[3].is_down = false;

	DebounceDecision decision;

	// First event ever: no previous event to measure against
	CHECK(!debounce_process_event_ex(&manager, &events[0], &decision));
	CHECK(decision.first_event);
	CHECK(!decision.blocked);
	CHECK(decision.reason == DEBOUNCE_REASON_PASS);
	CHECK(decision.threshold_ms == 50);
	CHECK(decision.elapsed_ms == 0);

	// The very first press has nothing to measure against
	CHECK(!decision.has_press_gap);

	CHECK(!debounce_process_event_ex(&manager, &events[1], &decision));
	CHECK(!decision.first_event);
	CHECK(decision.reason == DEBOUNCE_REASON_PASS);
	CHECK(decision.elapsed_ms == 40);

	// The gap the log shows is press to press (60 ms), not the engine's 20 ms
	// since the release: a user asking "how far apart were my clicks" means the
	// former, while the verdict is decided by the latter.
	CHECK(debounce_process_event_ex(&manager, &events[2], &decision));
	CHECK(decision.blocked);
	CHECK(decision.reason == DEBOUNCE_REASON_BLOCK_THRESHOLD);
	CHECK(decision.elapsed_ms == 20);
	CHECK(decision.threshold_ms == 50);
	CHECK(decision.has_press_gap);
	CHECK(decision.press_gap_ms == 60);

	// The release that belongs to it is swallowed too, but it is not a second
	// false click - the window must be able to tell the two apart
	CHECK(debounce_process_event_ex(&manager, &events[3], &decision));
	CHECK(decision.reason == DEBOUNCE_REASON_BLOCK_RELEASE);
	CHECK(decision.elapsed_ms == 20);

	// A release reports the rhythm of the press it belongs to, so both rows of
	// one click carry the same number
	CHECK(decision.has_press_gap);
	CHECK(decision.press_gap_ms == 60);

	// A press that was delivered but only just cleared the threshold is a near
	// miss: the engine keeps it, the window reports it. This is the evidence a
	// user needs when nothing has been blocked yet but the mouse is failing.
	{
		DebounceManager near_manager;
		debounce_init(&near_manager);
		debounce_set_monitored(&near_manager, MOUSE_BUTTON_LEFT, true);
		debounce_set_threshold(&near_manager, MOUSE_BUTTON_LEFT, 50, 1, 200);

		MouseEvent near_events[6] = {0};
		for (int i = 0; i < 6; i++)
		{
			near_events[i].button = MOUSE_BUTTON_LEFT;
			near_events[i].x = 100;
			near_events[i].y = 100;
		}

		// first press, its release, then presses at 60 / 70 / 101 / 400 ms after
		// the release: 60 and 70 are inside 2x threshold, 101 is just outside
		near_events[0].timestamp = 1000;
		near_events[0].is_down = true;
		near_events[1].timestamp = 1030;
		near_events[1].is_down = false;
		near_events[2].timestamp = 1090; // 60 ms after the release
		near_events[2].is_down = true;
		near_events[3].timestamp = 1120;
		near_events[3].is_down = false;
		near_events[4].timestamp = 1190; // 70 ms after the release
		near_events[4].is_down = true;
		near_events[5].timestamp = 1220;
		near_events[5].is_down = false;

		CHECK(!debounce_process_event_ex(&near_manager, &near_events[0], &decision));
		CHECK(!decision.near_miss); // nothing to compare against
		CHECK(!debounce_process_event_ex(&near_manager, &near_events[1], &decision));

		CHECK(!debounce_process_event_ex(&near_manager, &near_events[2], &decision));
		CHECK(decision.elapsed_ms == 60);
		CHECK(decision.near_miss);
		CHECK(decision.near_miss_ms == 60);

		CHECK(!debounce_process_event_ex(&near_manager, &near_events[3], &decision));
		CHECK(!decision.near_miss); // a release is never a near miss

		CHECK(!debounce_process_event_ex(&near_manager, &near_events[4], &decision));
		CHECK(decision.elapsed_ms == 70);
		CHECK(decision.near_miss);

		CHECK(!debounce_process_event_ex(&near_manager, &near_events[5], &decision));

		// Past twice the threshold: an ordinary click, not a near miss
		MouseEvent outside = {0};
		outside.button = MOUSE_BUTTON_LEFT;
		outside.timestamp = 1321; // 101 ms after the last release
		outside.is_down = true;
		CHECK(!debounce_process_event_ex(&near_manager, &outside, &decision));
		CHECK(decision.elapsed_ms == 101);
		CHECK(!decision.near_miss);

		// A blocked press is never reported as a near miss: it is already counted
		// in the blocked counter and the two must not double count it
		MouseEvent again = {0};
		again.button = MOUSE_BUTTON_LEFT;
		again.timestamp = 1330; // 9 ms after the press above
		again.is_down = true;
		CHECK(debounce_process_event_ex(&near_manager, &again, &decision));
		CHECK(decision.blocked);
		CHECK(!decision.near_miss);

		// An injected press says nothing about the switch
		MouseEvent injected = {0};
		injected.button = MOUSE_BUTTON_LEFT;
		injected.timestamp = 5000;
		injected.is_down = true;
		injected.is_injected = true;
		CHECK(!debounce_process_event_ex(&near_manager, &injected, &decision));
		CHECK(!decision.near_miss);

		debounce_cleanup(&near_manager);
	}

	// A wheel reversal within the threshold is a wheel jump
	MouseEvent wheel = {0};
	wheel.button = MOUSE_BUTTON_WHEEL;
	wheel.timestamp = 2000;
	wheel.data = 120;
	CHECK(!debounce_process_event_ex(&manager, &wheel, &decision));
	CHECK(decision.reason == DEBOUNCE_REASON_PASS);

	wheel.timestamp = 2020;
	wheel.data = -120;
	CHECK(debounce_process_event_ex(&manager, &wheel, &decision));
	CHECK(decision.reason == DEBOUNCE_REASON_BLOCK_WHEEL_REVERSAL);
	CHECK(decision.elapsed_ms == 20);

	wheel.timestamp = 2100;
	wheel.data = 0;
	CHECK(!debounce_process_event_ex(&manager, &wheel, &decision));
	CHECK(decision.reason == DEBOUNCE_REASON_IGNORE_WHEEL_ZERO);

	// Injected events and buttons that are switched off are reported as ignored
	// rather than as a pass, otherwise the log would look like the engine saw a
	// click it never acted on.
	MouseEvent injected = {0};
	injected.button = MOUSE_BUTTON_LEFT;
	injected.is_down = true;
	injected.is_injected = true;
	injected.timestamp = 3000;
	CHECK(!debounce_process_event_ex(&manager, &injected, &decision));
	CHECK(decision.reason == DEBOUNCE_REASON_IGNORE_INJECTED);

	MouseEvent switched_off = {0};
	switched_off.button = MOUSE_BUTTON_RIGHT;
	switched_off.is_down = true;
	switched_off.timestamp = 3100;
	CHECK(!debounce_process_event_ex(&manager, &switched_off, &decision));
	CHECK(decision.reason == DEBOUNCE_REASON_IGNORE_UNMONITORED);

	// Smart Drag: a long hold defers the release, and the press that follows
	// delivers it before it is judged - that press is a real user action and must
	// never be reported as a bounce.
	debounce_set_hybrid_heuristic(&manager, true);

	MouseEvent hold_down = {0};
	hold_down.button = MOUSE_BUTTON_LEFT;
	hold_down.is_down = true;
	hold_down.timestamp = 4000;
	hold_down.x = 10;
	hold_down.y = 10;

	MouseEvent hold_up = {0};
	hold_up.button = MOUSE_BUTTON_LEFT;
	hold_up.is_down = false;
	hold_up.timestamp = 4300;
	hold_up.x = 10;
	hold_up.y = 10;

	CHECK(!debounce_process_event_ex(&manager, &hold_down, &decision));
	CHECK(debounce_process_event_ex(&manager, &hold_up, &decision));
	CHECK(decision.reason == DEBOUNCE_REASON_DEFER_RELEASE);
	CHECK(decision.deferred);

	MouseEvent next_press = {0};
	next_press.button = MOUSE_BUTTON_LEFT;
	next_press.is_down = true;
	next_press.timestamp = 4340;
	next_press.x = 10;
	next_press.y = 10;

	// The press is swallowed and replayed by the timer, not passed through: the
	// release it was held behind has to reach the system first, and injecting it
	// from inside the hook callback is what used to deadlock the whole process.
	CHECK(debounce_process_event_ex(&manager, &next_press, &decision));
	CHECK(decision.reason == DEBOUNCE_REASON_PASS_FLUSHED);
	CHECK(decision.flushed_deferred);
	CHECK(decision.replay);

	// Still not a false click: the counters, the block log and the balloon all
	// have to skip it, otherwise the user is told a click was blocked when it
	// was only held back for a few milliseconds.
	CHECK(!decision.blocked);
	CHECK(manager.buttons[MOUSE_BUTTON_LEFT].pending_replay);

	// A long hold makes the two intervals diverge, and the log must show the
	// press-to-press one rather than the engine's much smaller number. The engine
	// measures from the press itself, which is the only writer of previousTime.
	CHECK(decision.has_press_gap);
	CHECK(decision.press_gap_ms == 340);
	CHECK(decision.elapsed_ms < decision.press_gap_ms);

	// The plain call must stay equivalent to the detailed one: two managers in
	// the same state have to reach the same verdict
	{
		DebounceManager plain;
		DebounceManager explained;
		debounce_init(&plain);
		debounce_init(&explained);
		debounce_set_monitored(&plain, MOUSE_BUTTON_LEFT, true);
		debounce_set_monitored(&explained, MOUSE_BUTTON_LEFT, true);
		debounce_set_threshold(&plain, MOUSE_BUTTON_LEFT, 50, 1, 200);
		debounce_set_threshold(&explained, MOUSE_BUTTON_LEFT, 50, 1, 200);
		debounce_set_hybrid_heuristic(&plain, false);
		debounce_set_hybrid_heuristic(&explained, false);

		for (int i = 0; i < 4; i++)
		{
			bool plain_result = debounce_process_event(&plain, &events[i]);
			bool explained_result = debounce_process_event_ex(&explained, &events[i], &decision);

			CHECK(plain_result == explained_result);
			CHECK(decision.blocked == explained_result);
		}

		debounce_cleanup(&plain);
		debounce_cleanup(&explained);
	}

	// "What counts as a false click" is defined once, in the engine: the log, the
	// evidence and the bubble must agree, or the window reports a block while the
	// evidence block stays empty.
	{
		DebounceDecision sample = {0};

		CHECK(!debounce_decision_is_false_click(NULL));

		sample.blocked = false;
		sample.reason = DEBOUNCE_REASON_BLOCK_THRESHOLD;
		CHECK(!debounce_decision_is_false_click(&sample));

		sample.blocked = true;
		sample.reason = DEBOUNCE_REASON_BLOCK_THRESHOLD;
		CHECK(debounce_decision_is_false_click(&sample));

		sample.reason = DEBOUNCE_REASON_BLOCK_DEFERRING;
		CHECK(debounce_decision_is_false_click(&sample));

		sample.reason = DEBOUNCE_REASON_BLOCK_WHEEL_REVERSAL;
		CHECK(debounce_decision_is_false_click(&sample));

		// The release of a blocked press and a deliberately held release belong to
		// a click that was already counted
		sample.reason = DEBOUNCE_REASON_BLOCK_RELEASE;
		CHECK(!debounce_decision_is_false_click(&sample));

		sample.reason = DEBOUNCE_REASON_DEFER_RELEASE;
		CHECK(!debounce_decision_is_false_click(&sample));

		sample.reason = DEBOUNCE_REASON_PASS;
		CHECK(!debounce_decision_is_false_click(&sample));
	}

	// Degenerate arguments
	CHECK(!debounce_process_event_ex(NULL, &events[0], &decision));
	CHECK(!debounce_process_event_ex(&manager, NULL, &decision));
	CHECK(!decision.blocked);

	// The explanation is optional: the hook can ask for the verdict alone
	debounce_process_event_ex(&manager, &events[0], NULL);

	debounce_cleanup(&manager);
}

// The log pane is fed by a ring the hook writes and the window reads
static void test_event_trace(void)
{
	printf("event trace ring\n");

	EventTrace trace;
	CHECK(event_trace_init(&trace));
	CHECK(event_trace_count(&trace) == 0);

	TraceEntry entry = {0};
	entry.button = MOUSE_BUTTON_LEFT;
	entry.x = 100;
	entry.y = 100;

	TraceEntry out;

	// A closed check-up window records nothing at all
	CHECK(!event_trace_push(&trace, &entry));
	CHECK(event_trace_count(&trace) == 0);

	// Only presses inside the test area may be logged. Everything else - the
	// filter check boxes, the scrollbars, the log text - must be ignored, or a
	// user could never read the pane he is scrolling.
	RECT capture;
	capture.left = 50;
	capture.top = 50;
	capture.right = 150;
	capture.bottom = 150;
	event_trace_set_capture(&trace, &capture);

	entry.x = 49; // One pixel to the left of the area
	CHECK(!event_trace_push(&trace, &entry));

	entry.x = 150; // The right edge belongs to the next window
	CHECK(!event_trace_push(&trace, &entry));

	entry.x = 100;
	entry.y = 149;
	CHECK(event_trace_push(&trace, &entry));
	CHECK(event_trace_count(&trace) == 1);

	CHECK(event_trace_pop(&trace, &out));
	CHECK(out.button == MOUSE_BUTTON_LEFT);
	CHECK(!event_trace_pop(&trace, &out));

	// Moving the window moves the capture area with it
	RECT moved;
	moved.left = 500;
	moved.top = 500;
	moved.right = 600;
	moved.bottom = 600;
	event_trace_set_capture(&trace, &moved);

	entry.x = 100;
	entry.y = 100;
	CHECK(!event_trace_push(&trace, &entry)); // The old place no longer records

	entry.x = 550;
	entry.y = 550;
	CHECK(event_trace_push(&trace, &entry));
	CHECK(event_trace_pop(&trace, &out));

	// Oldest first, so the log reads in the order the clicks happened
	for (uint32_t i = 0; i < 3; i++)
	{
		entry.decision.elapsed_ms = i;
		CHECK(event_trace_push(&trace, &entry));
	}

	CHECK(event_trace_count(&trace) == 3);
	CHECK(event_trace_pop(&trace, &out) && out.decision.elapsed_ms == 0);
	CHECK(event_trace_pop(&trace, &out) && out.decision.elapsed_ms == 1);
	CHECK(event_trace_pop(&trace, &out) && out.decision.elapsed_ms == 2);
	CHECK(event_trace_count(&trace) == 0);

	// Overflow retires the oldest entry, never the newest: a user watching the
	// pane needs the press that just happened
	for (uint32_t i = 0; i < EVENT_TRACE_CAPACITY + 4; i++)
	{
		entry.decision.elapsed_ms = i;
		CHECK(event_trace_push(&trace, &entry));
	}

	CHECK(event_trace_count(&trace) == EVENT_TRACE_CAPACITY);
	CHECK(event_trace_pop(&trace, &out));
	CHECK(out.decision.elapsed_ms == 4);

	event_trace_clear(&trace);
	CHECK(event_trace_count(&trace) == 0);

	// Clearing the log must not stop the capture
	CHECK(event_trace_push(&trace, &entry));

	// Closing the window stops the capture but keeps the module usable
	event_trace_set_capture(&trace, NULL);
	CHECK(!event_trace_push(&trace, &entry));

	// Degenerate arguments must not crash
	CHECK(!event_trace_push(NULL, &entry));
	CHECK(!event_trace_push(&trace, NULL));
	CHECK(!event_trace_pop(NULL, &out));
	CHECK(!event_trace_pop(&trace, NULL));
	event_trace_clear(NULL);
	event_trace_set_capture(NULL, &capture);

	event_trace_cleanup(&trace);
	CHECK(!event_trace_push(&trace, &entry));
	event_trace_cleanup(&trace); // Must be safe to call twice
}

// The expanding rings are driven by a timer, so they must always expire
static void test_ripple_view(void)
{
	printf("ripple view\n");

	RippleView view;
	ripple_view_reset(&view);
	CHECK(!ripple_view_tick(&view, 1000));
	CHECK(ripple_view_active_count(&view) == 0);

	ripple_view_trigger(&view, MOUSE_BUTTON_LEFT, 10, 20, 1000);
	CHECK(ripple_view_active_count(&view) == 1);
	CHECK(ripple_view_tick(&view, 1010));
	CHECK(ripple_view_active_count(&view) == 1);

	// The frame that erases the last ring still needs a repaint, the one after
	// it must not schedule another
	CHECK(ripple_view_tick(&view, 1000 + RIPPLE_LIFETIME_MS));
	CHECK(ripple_view_active_count(&view) == 0);
	CHECK(!ripple_view_tick(&view, 1000 + RIPPLE_LIFETIME_MS + 100));

	// A burst reuses slots instead of overflowing the array
	for (int i = 0; i < RIPPLE_MAX * 4; i++)
		ripple_view_trigger(&view, MOUSE_BUTTON_RIGHT, i, i, 2000 + (uint64_t)i);

	CHECK(ripple_view_active_count(&view) == RIPPLE_MAX);

	// The three main buttons must be visually distinguishable
	CHECK(ripple_view_color(MOUSE_BUTTON_LEFT) != ripple_view_color(MOUSE_BUTTON_MIDDLE));
	CHECK(ripple_view_color(MOUSE_BUTTON_MIDDLE) != ripple_view_color(MOUSE_BUTTON_RIGHT));
	CHECK(ripple_view_color(MOUSE_BUTTON_LEFT) != ripple_view_color(MOUSE_BUTTON_RIGHT));

	// Degenerate arguments
	ripple_view_reset(NULL);
	ripple_view_trigger(NULL, MOUSE_BUTTON_LEFT, 0, 0, 0);
	CHECK(!ripple_view_tick(NULL, 0));
	CHECK(ripple_view_active_count(NULL) == 0);
}

// The snapshot is the only thing the window and the tray tooltip read, so a
// field that is silently left empty shows up on screen as a lie. app_snapshot.c
// holds no window, so the copy can be verified here.
static void test_snapshot_fill(void)
{
	printf("app snapshot fill\n");

	// Missing arguments are tolerated: a half started application reports
	// "nothing known" instead of reading uninitialised memory
	app_snapshot_fill(NULL, NULL);

	AppSnapshot snapshot;
	memset(&snapshot, 0xAB, sizeof(snapshot));

	app_snapshot_fill(&snapshot, NULL);

	CHECK(snapshot.total_blocks == 0);
	CHECK(snapshot.now_tick == 0);
	CHECK(snapshot.hook_active == false);
	CHECK(snapshot.evidence[0].blocks == 0);
	CHECK(snapshot.log_directory[0] == L'\0');

	DebounceManager manager;
	if (!debounce_init(&manager))
	{
		printf("[FAIL] debounce_init\n");
		g_failures++;
		return;
	}

	debounce_set_threshold(&manager, MOUSE_BUTTON_LEFT, 77, 1, 200);
	debounce_set_monitored(&manager, MOUSE_BUTTON_LEFT, true);
	debounce_set_monitored(&manager, MOUSE_BUTTON_RIGHT, false);
	debounce_set_hybrid_heuristic(&manager, false);

	// Poisoned first: every field the engine does not own has to come back empty,
	// otherwise a caller that forgets to fill one shows stale bytes on screen
	memset(&snapshot, 0xAB, sizeof(snapshot));
	app_snapshot_fill(&snapshot, &manager);

	CHECK(snapshot.thresholds[MOUSE_BUTTON_LEFT] == 77);
	CHECK(snapshot.monitored[MOUSE_BUTTON_LEFT] == true);
	CHECK(snapshot.monitored[MOUSE_BUTTON_RIGHT] == false);
	CHECK(snapshot.hybrid == false);
	CHECK(snapshot.release_delay_ms == manager.releaseDelayMs);
	CHECK(snapshot.drag_distance_px == manager.dragDistancePx);
	CHECK(snapshot.blocks[MOUSE_BUTTON_LEFT] == 0);
	CHECK(snapshot.total_blocks == 0);

	// The clock is sampled here, not by the caller: a zero stamp would degrade
	// every timestamp in the log to the placeholder
	CHECK(snapshot.now_local.wYear >= 2020);
	CHECK(snapshot.now_tick > 0);

	// Fields owned by the application layer stay empty, which is the contract
	// the window relies on when it fills them in afterwards
	CHECK(snapshot.hook_events == 0);
	CHECK(snapshot.hook_active == false);
	CHECK(snapshot.logging == false);
	CHECK(snapshot.log_directory[0] == L'\0');

	debounce_cleanup(&manager);
}

// The support report is written by a process that may already be dying, so the
// two pieces that can silently go wrong are tested directly: the checksum (a
// wrong table produces an archive no tool will open) and the event ring.
static void test_zip_writer(void)
{
	printf("zip writer\n");

	// "123456789" is the published CRC-32 test vector. Round tripping through
	// our own writer would pass even with a broken table, because both sides
	// would be wrong in the same way.
	CHECK(zip_crc32("123456789", 9) == 0xCBF43926u);

	uint32_t streamed = zip_crc32_start();
	streamed = zip_crc32_update(streamed, "1234", 4);
	streamed = zip_crc32_update(streamed, "56789", 5);
	CHECK(zip_crc32_finish(streamed) == 0xCBF43926u);

	// The empty input checksum of a real CRC is zero, not 0xFFFFFFFF
	CHECK(zip_crc32(NULL, 0) == 0x00000000u);

	wchar_t directory[MAX_PATH];
	GetTempPathW(ARRAYSIZE(directory), directory);

	wchar_t path[MAX_PATH];
	StringCchPrintfW(path, ARRAYSIZE(path), L"%smm-zip-test-%u.zip",
					 directory, (unsigned)GetCurrentProcessId());

	ZipWriter writer;
	CHECK(zip_begin(&writer, path) == true);
	CHECK(zip_add_bytes(&writer, "a.txt", "hello", 5) == true);
	CHECK(zip_add_bytes(&writer, "b.txt", "world!", 6) == true);
	CHECK(zip_end(&writer) == true);

	// The end of central directory record is what every extractor looks for
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
							  OPEN_EXISTING, 0, NULL);
	CHECK(file != INVALID_HANDLE_VALUE);

	LARGE_INTEGER size;
	CHECK(GetFileSizeEx(file, &size) != 0);
	CHECK(size.QuadPart > 22);

	LARGE_INTEGER tail_offset;
	tail_offset.QuadPart = size.QuadPart - 22;
	CHECK(SetFilePointerEx(file, tail_offset, NULL, FILE_BEGIN) != 0);

	unsigned char tail[22];
	DWORD read = 0;
	CHECK(ReadFile(file, tail, sizeof(tail), &read, NULL) != 0);
	CHECK(read == sizeof(tail));
	CHECK(tail[0] == 0x50 && tail[1] == 0x4B && tail[2] == 0x05 && tail[3] == 0x06);
	CHECK(tail[8] == 2);  // entries on this disk
	CHECK(tail[10] == 2); // entries in total
	CHECK(tail[12] != 0); // central directory size, so it is not an empty archive

	CloseHandle(file);
	CHECK(DeleteFileW(path) != 0);

	// A path that cannot be created must report failure instead of pretending
	ZipWriter unusable;
	CHECK(zip_begin(&unusable, L"?:\\nowhere\\x.zip") == false);
	CHECK(zip_add_bytes(&unusable, "a.txt", "x", 1) == false);
	CHECK(zip_end(&unusable) == false);
}

static void test_event_history(void)
{
	printf("event history\n");

	EventHistory history;
	CHECK(event_history_init(&history) == true);

	TraceEntry entry;
	memset(&entry, 0, sizeof(entry));

	TraceEntry out[8];
	CHECK(event_history_copy(&history, out, ARRAYSIZE(out)) == 0);

	for (int i = 0; i < 5; i++)
	{
		entry.timestamp = (uint64_t)(i + 1);
		event_history_push(&history, &entry);
	}

	uint32_t copied = event_history_copy(&history, out, ARRAYSIZE(out));
	CHECK(copied == 5);
	CHECK(out[0].timestamp == 5); // newest first
	CHECK(out[4].timestamp == 1);

	// Past the capacity the newest survive and the ring stays bounded
	const int overflow = EVENT_HISTORY_CAPACITY * 2;
	for (int i = 0; i < overflow; i++)
	{
		entry.timestamp = (uint64_t)(1000 + i);
		event_history_push(&history, &entry);
	}

	copied = event_history_copy(&history, out, ARRAYSIZE(out));
	CHECK(copied == ARRAYSIZE(out));
	CHECK(out[0].timestamp == (uint64_t)(1000 + overflow - 1));

	// A destination smaller than the ring keeps the newest, never the oldest
	TraceEntry single;
	CHECK(event_history_copy(&history, &single, 1) == 1);
	CHECK(single.timestamp == (uint64_t)(1000 + overflow - 1));

	event_history_clear(&history);
	CHECK(event_history_copy(&history, out, ARRAYSIZE(out)) == 0);

	event_history_cleanup(&history);

	// After cleanup the ring must refuse to be read rather than touch a lock
	// that no longer exists
	CHECK(event_history_copy(&history, out, ARRAYSIZE(out)) == 0);
}

static void test_support_report(void)
{
	printf("support report\n");

	SupportReport report;
	support_bundle_prepare(&report, true);

	CHECK(report.crashed == true);
	CHECK(report.complete == true);
	CHECK(report.now_tick > 0);
	CHECK(report.exe_path[0] != L'\0');
	CHECK(report.arch[0] != L'\0');
	CHECK(report.stamp[0] != L'\0');

	report.exception_code = 0xC0000005;
	report.exception_address = 0;

	// No events: the section has to say so instead of printing a bare header
	// Static because the report is 24 KB and this is a test, not the crash path
	static char text[SUPPORT_TEXT_MAX];
	CHECK(support_bundle_build_text(&report, text, sizeof(text)) == true);
	CHECK(strstr(text, APP_VERSION_STRING) != NULL);
	CHECK(strstr(text, "0xC0000005") != NULL);
	CHECK(strstr(text, "exception") != NULL);
	CHECK(strstr(text, "none recorded") != NULL);

	// A wide string handed to a narrow "%s" is read as ANSI and stops at the
	// first embedded NUL, which silently truncates a timestamp to one character.
	// Length is what catches that; "2026-11-23 10:21:25" plus its label is 33.
	const char *generated_line = strstr(text, "generated   : ");
	CHECK(generated_line != NULL);
	CHECK(generated_line != NULL && strlen(generated_line) > 30);

	// A blocked press and a pass, which must not read the same
	memset(report.events, 0, sizeof(report.events));
	report.event_count = 2;

	report.events[0].button = MOUSE_BUTTON_LEFT;
	report.events[0].timestamp = report.now_tick;
	report.events[0].is_down = true;
	report.events[0].decision.reason = DEBOUNCE_REASON_BLOCK_THRESHOLD;
	report.events[0].decision.blocked = true;
	report.events[0].decision.threshold_ms = 40;
	report.events[0].decision.elapsed_ms = 12;

	report.events[1].button = MOUSE_BUTTON_LEFT;
	report.events[1].timestamp = report.now_tick;
	report.events[1].decision.reason = DEBOUNCE_REASON_PASS;

	CHECK(support_bundle_build_text(&report, text, sizeof(text)) == true);
	CHECK(strstr(text, "blocked-threshold") != NULL);
	CHECK(strstr(text, "passed") != NULL);
	CHECK(strstr(text, "left") != NULL);
	CHECK(strstr(text, "none recorded") == NULL);

	// Every verdict needs its own name: a report that cannot tell them apart is
	// as useless as no report at all
	for (int a = 0; a < DEBOUNCE_REASON_COUNT; a++)
	{
		for (int b = a + 1; b < DEBOUNCE_REASON_COUNT; b++)
		{
			CHECK(strcmp(debounce_reason_slug((DebounceReason)a),
						 debounce_reason_slug((DebounceReason)b)) != 0);
		}
	}
}

// The log panes and their rendering used to live inside the check-up window and
// therefore had no test coverage at all: the test project does not compile a
// file that needs a message loop. log_format.c has no window, so the text a
// user actually reads can be asserted here.
static void test_log_format(void)
{
	printf("log format\n");

	// Newest first, oldest dropped: a pane is a rolling history
	LogPane pane;
	memset(&pane, 0, sizeof(pane));

	ClickRow row;
	memset(&row, 0, sizeof(row));
	row.button = MOUSE_BUTTON_LEFT;
	row.down_tick = 100;

	log_pane_push(&pane, &row);
	CHECK(pane.count == 1);
	CHECK(pane.rows[0].down_tick == 100);

	row.down_tick = 200;
	log_pane_push(&pane, &row);
	CHECK(pane.count == 2);
	CHECK(pane.rows[0].down_tick == 200);
	CHECK(pane.rows[1].down_tick == 100);

	// Past the capacity the newest rows survive and the count stays bounded
	const uint64_t last_tick = 300 + SETTINGS_LOG_ROWS * 2;
	for (uint64_t tick = 300; tick < last_tick; tick++)
	{
		row.down_tick = tick;
		log_pane_push(&pane, &row);
	}

	CHECK(pane.count == SETTINGS_LOG_ROWS);
	CHECK(pane.rows[0].down_tick == last_tick - 1);
	CHECK(pane.rows[SETTINGS_LOG_ROWS - 1].down_tick == last_tick - SETTINGS_LOG_ROWS);

	// An empty pane renders as an empty string, never as leftover bytes
	wchar_t text[1024];
	AppSnapshot snapshot;
	memset(&snapshot, 0, sizeof(snapshot));

	// A real clock, otherwise SystemTimeToFileTime rejects the zeroed struct and
	// every stamp degrades to the placeholder
	snapshot.now_local.wYear = 2026;
	snapshot.now_local.wMonth = 11;
	snapshot.now_local.wDay = 22;
	snapshot.now_local.wHour = 10;
	snapshot.now_local.wMinute = 30;
	snapshot.now_local.wSecond = 15;

	LogPane empty;
	memset(&empty, 0, sizeof(empty));
	memset(text, 0xAB, sizeof(text));
	log_pane_render(&snapshot, &empty, text, ARRAYSIZE(text));
	CHECK(text[0] == L'\0');
	log_pane_render(&snapshot, NULL, text, ARRAYSIZE(text));
	CHECK(text[0] == L'\0');

	// A filled pane renders one row per click, joined with CRLF
	log_pane_render(&snapshot, &pane, text, ARRAYSIZE(text));
	CHECK(text[0] != L'\0');
	CHECK(wcsstr(text, lang_str(STR_VERDICT_PASS)) != NULL);

	// A timestamp needs the sampled clock; without it the pane says so instead
	// of showing a time that never happened
	wchar_t time_text[SETTINGS_TIME_SIZE];
	log_format_time(NULL, 0, STR_LOG_TIME_FMT, time_text, ARRAYSIZE(time_text));
	CHECK(wcscmp(time_text, L"--:--:--.---") == 0);

	snapshot.now_tick = 10000;
	log_format_time(&snapshot, 9000, STR_LOG_TIME_FMT, time_text, ARRAYSIZE(time_text));
	CHECK(wcslen(time_text) > 0);
	CHECK(wcscmp(time_text, L"--:--:--.---") != 0);

	// Every verdict has wording of its own; a click that passed must not read
	// like one that was blocked
	CHECK(log_format_verdict(DEBOUNCE_REASON_PASS) != NULL);
	CHECK(log_format_verdict(DEBOUNCE_REASON_BLOCK_THRESHOLD) != NULL);
	CHECK(wcscmp(log_format_verdict(DEBOUNCE_REASON_PASS),
				 log_format_verdict(DEBOUNCE_REASON_BLOCK_THRESHOLD)) != 0);
	CHECK(wcscmp(log_format_verdict(DEBOUNCE_REASON_BLOCK_THRESHOLD),
				 log_format_verdict(DEBOUNCE_REASON_BLOCK_WHEEL_REVERSAL)) != 0);
	CHECK(wcscmp(log_format_verdict(DEBOUNCE_REASON_IGNORE_INJECTED),
				 log_format_verdict(DEBOUNCE_REASON_IGNORE_UNMONITORED)) != 0);

	// A report uses LF, an edit control needs CRLF, and CRLF must not double
	wchar_t converted[64];
	log_format_lf_to_crlf(L"a\nb\r\nc", converted, ARRAYSIZE(converted));
	CHECK(wcscmp(converted, L"a\r\nb\r\nc") == 0);
	log_format_lf_to_crlf(L"", converted, ARRAYSIZE(converted));
	CHECK(converted[0] == L'\0');
	log_format_lf_to_crlf(NULL, converted, ARRAYSIZE(converted));
	CHECK(converted[0] == L'\0');

	// The status line never claims protection while the hook is gone
	snapshot.hook_active = true;
	snapshot.hook_recoveries = 0;
	log_format_status(&snapshot, text, ARRAYSIZE(text));
	CHECK(wcscmp(text, lang_str(STR_STATUS_OK)) == 0);

	snapshot.hook_active = false;
	log_format_status(&snapshot, text, ARRAYSIZE(text));
	CHECK(wcscmp(text, lang_str(STR_STATUS_BAD)) == 0);

	// A recovery is mentioned on top of the base sentence
	snapshot.hook_active = true;
	snapshot.hook_recoveries = 2;
	log_format_status(&snapshot, text, ARRAYSIZE(text));
	CHECK(wcsstr(text, lang_str(STR_STATUS_OK)) != NULL);
	CHECK(lang_display_width(text) > lang_display_width(lang_str(STR_STATUS_OK)));

	// Evidence with no button recorded says so instead of printing a header alone
	snapshot.hook_recoveries = 0;
	memset(snapshot.evidence, 0, sizeof(snapshot.evidence));
	log_format_evidence(&snapshot, text, ARRAYSIZE(text));
	CHECK(wcsstr(text, lang_str(STR_EVIDENCE_NONE)) != NULL);

	// A recorded false click has to render, not crash.
	//
	// This is the block that killed the program in the field: the "engine judged
	// by" line used %s while the call site passed the millisecond count, so printf
	// read the number as a string pointer and walked into that address. Only the
	// empty evidence case was ever exercised here, which is why nothing caught it.
	AppBlockEvidence *left = &snapshot.evidence[MOUSE_BUTTON_LEFT];
	left->valid = true;
	left->timestamp = snapshot.now_tick;
	left->elapsed_ms = 46;      // the value that was dereferenced as a pointer
	left->has_press_gap = true;
	left->press_gap_ms = 120;   // differs from elapsed_ms, so the extra line prints
	left->threshold_ms = 40;
	left->blocks = 3;

	log_format_evidence(&snapshot, text, ARRAYSIZE(text));
	CHECK(wcsstr(text, lang_button_name(MOUSE_BUTTON_LEFT)) != NULL);
	CHECK(wcsstr(text, lang_str(STR_EVIDENCE_NONE)) == NULL);

	// The engine line must carry the number, printed as a number
	wchar_t expected_engine[80];
	StringCchPrintfW(expected_engine, ARRAYSIZE(expected_engine),
					 lang_str(STR_EVIDENCE_ENGINE_FMT), 46u);
	CHECK(wcsstr(text, expected_engine) != NULL);

	// And the format must stay numeric. Comparing the two translations catches a
	// drift between them but not a mismatch with the call site, which is exactly
	// how this survived the existing tests.
	char engine_shape[24];
	Language before = language_get();

	for (int pass = 0; pass < 2; pass++)
	{
		language_set(pass == 0 ? LANG_EN : LANG_ZH_CN);
		placeholder_signature(lang_str(STR_EVIDENCE_ENGINE_FMT), engine_shape,
							  ARRAYSIZE(engine_shape));
		CHECK(strcmp(engine_shape, "n") == 0);
	}

	language_set(before);
}

// The window is centred in the work area, and a work area smaller than the
// window must not push the title bar off screen: that is what happens on a small
// display or at a high scaling factor, and an off-screen title bar cannot be
// grabbed at all.
static void test_window_layout(void)
{
	printf("window layout\n");

	RECT work = {0, 0, 1920, 1040};
	POINT position;

	window_layout_center(800, 600, &work, &position);
	CHECK(position.x == 560); // (1920 - 800) / 2
	CHECK(position.y == 220); // (1040 - 600) / 2

	// An odd remainder rounds towards the top left, never past it
	window_layout_center(801, 601, &work, &position);
	CHECK(position.x == 559);
	CHECK(position.y == 219);

	// A work area that does not start at the origin keeps its own offset
	RECT offset = {1920, 0, 3200, 1000};
	window_layout_center(800, 600, &offset, &position);
	CHECK(position.x == 1920 + 240);
	CHECK(position.y == 200);

	// Too wide and too tall: pinned, not pushed to a negative coordinate
	window_layout_center(2000, 1200, &work, &position);
	CHECK(position.x == 0);
	CHECK(position.y == 0);

	// Only one axis fits: the other is still centred
	window_layout_center(2000, 600, &work, &position);
	CHECK(position.x == 0);
	CHECK(position.y == 220);

	// Exactly the size of the work area counts as "does not fit"
	window_layout_center(1920, 1040, &work, &position);
	CHECK(position.x == 0);
	CHECK(position.y == 0);

	// Degenerate input must leave the result untouched instead of crashing
	position.x = 7;
	position.y = 9;
	window_layout_center(800, 600, NULL, &position);
	CHECK(position.x == 7);
	CHECK(position.y == 9);
}

// The log panes are padded with spaces instead of tab stops, which only lines
// up when a CJK glyph is exactly two cells wide and no label overshoots its
// column. Both invariants are easy to break by accident.
static void test_checkup_formats(void)
{
	printf("check-up formats\n");

	CHECK(lang_display_width(NULL) == 0);
	CHECK(lang_display_width(L"") == 0);
	CHECK(lang_display_width(L"abcd") == 4);
	CHECK(lang_display_width(L"12:34:56.789") == 12);
	CHECK(lang_display_width(L"左键") == 4);
	CHECK(lang_display_width(L"横向滚轮") == 8);

	Language saved = language_get();

	for (int language = 0; language < LANG_COUNT; language++)
	{
		language_set((Language)language);

		// Every header label has to fit inside its own column
		CHECK(lang_display_width(lang_str(STR_LOG_COL_TIME)) <= 9);
		CHECK(lang_display_width(lang_str(STR_LOG_COL_BUTTON)) <= 8);
		CHECK(lang_display_width(lang_str(STR_LOG_COL_GAP)) <= 8);
		CHECK(lang_display_width(lang_str(STR_LOG_COL_HOLD)) <= 8);

		// ... and so has every button name, or the columns would drift.
		// The pane uses the compact names: the menu name of the side buttons
		// carries a space that costs a cell the column cannot spare.
		for (int button = 0; button < MOUSE_BUTTON_COUNT; button++)
		{
			CHECK(lang_display_width(lang_button_short_name((MouseButton)button)) <= 8);
			CHECK(wcschr(lang_button_short_name((MouseButton)button), L' ') == NULL);
		}

		// The verdict starts at the same cell in the header and in every row, no
		// matter which button name precedes it. Dropping the press counter paid
		// for the hold time column, so the prefix is what it always was.
		wchar_t header[192];
		lang_format_log_header(header, ARRAYSIZE(header));

		size_t prefix = lang_display_width(header) -
						lang_display_width(lang_str(STR_LOG_COL_VERDICT));

		CHECK(prefix == 33);
		CHECK(wcsstr(header, lang_str(STR_LOG_COL_VERDICT)) != NULL);
		CHECK(wcsstr(header, lang_str(STR_LOG_COL_HOLD)) != NULL);

		for (int button = 0; button < MOUSE_BUTTON_COUNT; button++)
		{
			wchar_t row[192];
			lang_format_log_row(row, ARRAYSIZE(row), L"12:34:56",
								lang_button_short_name((MouseButton)button), L"23", L"65",
								lang_str(STR_VERDICT_BLOCK_FAST));

			CHECK(lang_display_width(row) -
					  lang_display_width(lang_str(STR_VERDICT_BLOCK_FAST)) == prefix);
			CHECK(wcsstr(row, L"12:34:56") != NULL);
			CHECK(wcsstr(row, L"65") != NULL);
			CHECK(wcsstr(row, lang_button_short_name((MouseButton)button)) != NULL);
		}

		// Every verdict of either language must still fit the pane: the log has
		// no horizontal scrollbar, a wrapped row would break the table.
		// 44 cells is the measured capacity of the 210 DLU wide edit minus a
		// safety cell (one cell is ~7.3 px at the log font size).
		for (int id = STR_VERDICT_PASS; id <= STR_VERDICT_IGNORE_WHEEL_ZERO; id++)
			CHECK(prefix + lang_display_width(lang_str((LangStringId)id)) <= 44);

		// An empty cell must still occupy its column, that is what the "-" of the
		// first event is padded from
		wchar_t first_row[192];
		lang_format_log_row(first_row, ARRAYSIZE(first_row), L"12:34:56",
							lang_button_short_name(MOUSE_BUTTON_LEFT),
							lang_str(STR_LOG_GAP_NONE), lang_str(STR_LOG_GAP_NONE),
							lang_str(STR_VERDICT_PASS));

		CHECK(lang_display_width(first_row) -
				  lang_display_width(lang_str(STR_VERDICT_PASS)) == prefix);

		// NULL cells must not crash or swallow the row
		lang_format_log_row(first_row, ARRAYSIZE(first_row), NULL, NULL, NULL, NULL, NULL);
		CHECK(first_row[0] != L'\0');

		// The one line summary above the pane is 210 DLU wide, about 51 cells of
		// the dialog font; the numbers of a busy session must still fit.
		wchar_t summary[256];
		StringCchPrintfW(summary, ARRAYSIZE(summary), lang_str(STR_STATS_SUMMARY_FMT),
						 (unsigned)12, (unsigned)23, (unsigned)183, (unsigned)546, (unsigned)296);
		CHECK(lang_display_width(summary) <= 51);

		// The near miss count rides on the group caption instead of the summary:
		// the summary line has no room for one more number, the caption has.
		wchar_t caption[128];
		StringCchPrintfW(caption, ARRAYSIZE(caption), lang_str(STR_GROUP_STATS_NEAR_FMT),
						 (unsigned)7);
		CHECK(wcslen(caption) > wcslen(lang_str(STR_GROUP_STATS)));
		CHECK(lang_display_width(caption) <= 30);

		// The evidence line names the button and both timestamps
		wchar_t evidence[256];
		StringCchPrintfW(evidence, ARRAYSIZE(evidence), lang_str(STR_EVIDENCE_FMT),
						 lang_button_name(MOUSE_BUTTON_RIGHT), (uint32_t)3,
						 L"10:00:00.100", L"10:00:00.123", L"23 ms", L"50 ms");

		CHECK(wcsstr(evidence, lang_button_name(MOUSE_BUTTON_RIGHT)) != NULL);
		CHECK(wcsstr(evidence, L"10:00:00.123") != NULL);

		// The timestamp format has four fields, both languages
		// The log has room for whole seconds, the evidence keeps milliseconds
		CHECK(count_placeholders(lang_str(STR_LOG_TIME_FMT)) == 3);
		CHECK(count_placeholders(lang_str(STR_EVIDENCE_TIME_FMT)) == 4);
	}

	test_log_format();
	language_set(saved);
}

int main(void)
{
	test_menu_id_codec();
	test_menu_id_uniqueness();
	test_language_tables();
	test_tooltip_text();
	test_balloon_text_fits();
	test_blocked_button_list();
	test_notify_policy();
	test_config_tables();
	test_button_enum_layout();
	test_version_consistency();
	test_smart_drag_run();

	// 1.2.0 modules
	test_hook_watchdog();
	test_app_controller();
	test_block_log();
	test_block_log_files();

	// 1.3.0 check-up window
	test_debounce_decision();
	test_event_trace();
	test_ripple_view();
	test_snapshot_fill();
	test_event_history();
	test_zip_writer();
	test_support_report();
	test_window_layout();
	test_checkup_formats();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);

	return g_failures == 0 ? 0 : 1;
}
