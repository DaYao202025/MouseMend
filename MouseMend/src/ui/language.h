#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>
#include "../core/config.h"
#include "../core/mouse_hook.h"

// Localized string identifiers
typedef enum
{
	STR_APP_NAME = 0,
	STR_STATS_FMT,
	STR_BLOCKED_BUTTON_FMT,
	STR_STATUS_FMT,
	STR_ENABLE,
	STR_ENABLE_ALL,
	STR_DISABLE_ALL,
	STR_SMART_DRAG,
	STR_AUTOSTART,
	STR_RESET_STATS,
	STR_PRESETS,
	STR_PRESET_DEFAULT_FMT,
	STR_PRESET_OFFICE_FMT,
	STR_PRESET_STRICT_FMT,
	STR_BUTTON_TITLE_FMT,
	STR_ADVANCED,
	STR_RELEASE_DELAY,
	STR_TUNING_OFF,
	STR_DRAG_DISTANCE,
	STR_NOTIFY_BLOCKS,
	STR_NOTIFY_ON_TITLE_FMT,
	STR_NOTIFY_ON_TEXT_FMT,
	STR_NOTIFY_BLOCKED_FMT,
	STR_LOG_BLOCKS,
	STR_OPEN_LOG_FOLDER,
	STR_OPEN_SETTINGS,
	STR_SETTINGS_TITLE,
	STR_GROUP_STATS,
	STR_GROUP_TEST,
	STR_GROUP_DIAG,
	STR_STATUS_OK,
	STR_STATUS_BAD,
	STR_STATUS_RECOVERED_FMT,
	STR_DIAG_HINT,
	STR_TEST_HINT,
	STR_STATS_EMPTY,
	STR_REFRESH,
	STR_CLOSE,
	STR_HOOK_RECOVERED,
	STR_LOG_DIR_UNAVAILABLE,
	STR_RESTORE_DEFAULTS,
	STR_CONFIRM_RESTORE,
	STR_INTRO_TITLE,
	STR_INTRO_TEXT,
	STR_MS_FMT,
	STR_PX_FMT,
	STR_LANGUAGE,
	STR_MENU_CUSTOM,
	STR_ABOUT,
	STR_MENU_ABOUT_FMT,
	STR_WEBSITE,
	STR_EXIT,
	STR_TOOLTIP_ON,
	STR_TOOLTIP_PARTIAL,
	STR_TOOLTIP_OFF,
	STR_TOOLTIP_SUMMARY_FMT,
	STR_TOOLTIP_BLOCKED_FMT,
	STR_TOOLTIP_NO_BLOCKS,
	STR_STATUS_UNAVAILABLE,
	STR_SAVE_FAILED,
	STR_SET_THRESHOLD_TITLE,
	STR_THRESHOLD_PROMPT_FMT,
	STR_INVALID_INPUT_FMT,
	STR_OK,
	STR_CANCEL,
	STR_ERR_LOGGER,
	STR_ERR_MUTEX_FMT,
	STR_ERR_ALREADY_RUNNING,
	STR_ERR_WINDOW_CLASS_FMT,
	STR_ERR_CREATE_WINDOW_FMT,
	STR_ERR_UNEXPECTED_FMT,
	STR_ERR_AUTOSTART_FMT,

	// Check-up window
	STR_GROUP_LOG,
	STR_GROUP_LIVE,
	STR_LIVE_SECTION_FMT,
	STR_LIVE_UNKNOWN,
	// The label of the "no per button filter" switch. It used to read "Auto",
	// which stopped being true once it sat next to five buttons: what it does is
	// show every button, and that is what it now says.
	STR_FILTER_ALL,

	// The always-on-top entry. It has no visible switch any more: the title row
	// carries an unmarked hot zone that opens a one item menu on right click, and
	// this is that item's label.
	STR_ALWAYS_ON_TOP,

	// Hint drawn inside that hot zone while the pointer is on it
	STR_EGG_HINT,

	// The settings button, which is a placeholder until that window exists
	STR_SETTINGS_BUTTON,
	STR_SETTINGS_PENDING,
	STR_LOG_COL_TIME,
	STR_LOG_COL_BUTTON,
	STR_LOG_COL_GAP,
	STR_LOG_COL_HOLD,
	STR_LOG_COL_VERDICT,
	STR_LOG_TIME_FMT,
	STR_EVIDENCE_TIME_FMT,
	STR_LOG_GAP_NONE,
	STR_VERDICT_PASS,
	STR_VERDICT_BLOCK_FAST,
	STR_VERDICT_BLOCK_DEFER,
	STR_VERDICT_BLOCK_RELEASE,
	STR_VERDICT_BLOCK_REVERSE,
	STR_VERDICT_DEFER_RELEASE,
	STR_VERDICT_IGNORE_INJECTED,
	STR_VERDICT_IGNORE_UNMONITORED,
	STR_VERDICT_IGNORE_WHEEL_ZERO,
	// "Clear view", not "Clear log": the button empties the window, and the
	// files in the log folder are untouched. The old label promised more than it
	// did, and the tip below says what the boundary is.
	STR_CLEAR,
	STR_LOG_DIR_BUTTON,
	STR_TIP_CLEAR,
	STR_TIP_LOG_DIR,
	STR_STATS_SUMMARY_FMT,
	STR_STATS_CLICKS_FMT,
	STR_GROUP_STATS_NEAR_FMT,
	STR_BLOCKED_NONE,
	STR_EVIDENCE_HEADER,
	STR_EVIDENCE_NONE,
	STR_EVIDENCE_FMT,
	STR_EVIDENCE_ENGINE_FMT,

	// About window
	STR_ABOUT_VERSION_FMT,
	STR_ABOUT_DESC,
	STR_ABOUT_FEATURES,
	STR_ABOUT_LIMITS,
	STR_ABOUT_WEBSITE_BUTTON,
	STR_ABOUT_CLOSE_BUTTON,
	STR_ABOUT_ORIGIN_FMT,

	// Filter help (check-up window)
	STR_FILTER_HINT,
	STR_TIP_FILTER_ALL,
	STR_TIP_FILTER_LEFT,
	STR_TIP_FILTER_MIDDLE,
	STR_TIP_FILTER_RIGHT,
	STR_TIP_FILTER_X1,
	STR_TIP_FILTER_X2,

	// Tray balloon shown when the window opens at startup
	STR_TRAY_BALLOON_TITLE,
	STR_TRAY_BALLOON_TEXT,
	STR_SETTINGS_UNAVAILABLE,

	// Support bundle: menu entry, the two outcomes and the notice shown on the
	// first start after a crash
	STR_EXPORT_SUPPORT,
	STR_EXPORT_SUPPORT_DONE_FMT,
	STR_EXPORT_SUPPORT_FAILED,
	STR_CRASH_REPORT_FOUND_FMT,
	STR_HANG_REPORT_FOUND_FMT,
	STR_INJECTION_FAILED,

	// Help windows: what a verdict means, and what each release fixed
	STR_VERDICT_HELP_NAME,
	STR_VERDICT_HELP_TITLE,
	STR_VERDICT_HELP_TEXT,
	STR_VERSION_NOTES,
	STR_VERSION_NOTES_TITLE,
	STR_VERSION_NOTES_TEXT,
	STR_HELP_CLOSE,

	// Self-learning (opt-in, suggestion only)
	STR_LEARNING,
	STR_ADOPT_LEARNING,
	STR_LEARNING_STATUS_FMT,
	STR_PAUSE_CONTEXT,
	STR_RESUME_CONTEXT,
	STR_PAUSE_5,
	STR_PAUSE_10,
	STR_PAUSE_30,
	STR_PAUSE_CUSTOM,
	STR_PAUSE_PROMPT_FMT,
	STR_EXCLUDE_CURRENT,
	STR_CONTEXT_STATUS_FMT,

	// Context modes: whitelist / blacklist
	STR_WHITELIST_MODE,
	STR_BLACKLIST_MODE,
	STR_CONTEXT_MODE_ALL_WHITE,
	STR_CONTEXT_MODE_ALL_BLACK,
	STR_CONTEXT_MODE_WHITELIST,
	STR_CONTEXT_MODE_BLACKLIST,
	STR_CONTEXT_ADD,
	STR_CONTEXT_EXCLUDE_FMT,
	STR_CONTEXT_ONLY_FMT,
	STR_CONTEXT_EMPTY,
	STR_CONTEXT_WHITE_EMPTY_WARN,
	STR_CONTEXT_LEGACY_FMT,
	STR_CONTEXT_MIGRATED_FMT,
	STR_CONTEXT_DELAY,
	STR_CONTEXT_ADD_TITLE,
	STR_CONTEXT_ADDED_FMT,
	STR_CONTEXT_ERR_SELF,
	STR_CONTEXT_ERR_DUP,
	STR_CONTEXT_ERR_OTHER,
	STR_CONTEXT_ERR_FULL,
	STR_CONTEXT_ERR_INVALID,
	STR_CONTEXT_ERR_RESERVED,
	STR_CONTEXT_WARN_SHELL,
	STR_CONFIRM_EMPTY_WHITELIST,
	STR_CONTEXT_REMOVE_FMT,
	STR_CONTEXT_DISABLED_SUFFIX,
	STR_WHITELIST_NAME,
	STR_BLACKLIST_NAME,
	STR_LEARNING_ENABLE,
	STR_LEARNING_OFF_HINT,
	STR_LEARNING_ROW_PENDING_FMT,
	STR_CAP_HOOK,
	STR_CAP_LEARNING,
	STR_CAP_CONTEXT,
	STR_CAP_LOG,
	STR_CAP_STATE_OK,
	STR_CAP_STATE_UNKNOWN,
	STR_CAP_STATE_STALLED,
	STR_CAP_STATE_DISABLED,
	STR_CAP_TIP_FMT,
	STR_CAP_LABEL_FMT,
	STR_STATUS_SHORT_OK,
	STR_STATUS_SHORT_BAD,
	STR_STATUS_STRIP_OK,
	STR_STATUS_STRIP_BAD,
	STR_STATUS_STRIP_PAUSED_FMT,
	STR_STRIP_TIP_EXPAND,	// "click to expand" - a collapsed block
	STR_STRIP_TIP_COLLAPSE, // "click to collapse" - the block that is open
	STR_STATUS_SHORT_STRIP,
	STR_CAP_SHORT_HOOK,
	STR_CAP_SHORT_LEARNING,
	STR_CAP_SHORT_CONTEXT,
	STR_CAP_SHORT_LOG,
	STR_SB_CONTEXT_NONE,
	STR_SB_CONTEXT_OURS,
	STR_SB_CONTEXT_EXEMPT_FMT,
	STR_SB_CONTEXT_BLOCKED_FMT,
	STR_SB_HOOK_RECOVERED,
	STR_SB_SAVE_FAILED,
	STR_SB_ADVICE_FMT,
	STR_LEARNING_KEY_FMT,
	STR_LEARNING_APPLIED_FMT,

	STR_COUNT
} LangStringId;

// Detect the language matching the current Windows display language
Language language_detect_system(void);

// Get / set the active language
Language language_get(void);
void language_set(Language language);

// Get a localized string; never returns NULL, falls back to English
const wchar_t *lang_str(LangStringId id);

// Get the localized name of a mouse button; never returns NULL
const wchar_t *lang_button_name(MouseButton button);

// Get the short label of a mouse button for the filter row (never NULL).
// Shorter than lang_button_name on purpose: six of them share one line.
const wchar_t *lang_filter_label(MouseButton button);

// Get the single glyph drawn inside the test area ring
const wchar_t *lang_button_mark(MouseButton button);

// Which way a wheel notch went, for the ring mark; empty when delta is zero.
// Positive delta is away from the user, exactly as the hook reports it.
const wchar_t *lang_scroll_mark(int32_t delta);

// Name used by the log panes: the same word without the space, because the
// column has room for eight cells and "Side 1" would push the rest of the row.
const wchar_t *lang_button_short_name(MouseButton button);

// Get the endonym of a language (always shown in its own language)
const wchar_t *lang_language_name(Language language);

// NOTIFYICONDATA::szTip holds 128 wchar_t including the terminator and the shell
// truncates anything longer. Both the formatter and the tray API use this value.
#define TRAY_TOOLTIP_MAX_CHARS 128

// Compose one "left button: 9" line for the blocked-statistics submenu
// Returns:
//   buffer, so the call can be used directly inside a printf argument
const wchar_t *lang_format_blocked_button(wchar_t *buffer, size_t buffer_count,
										  MouseButton button, uint32_t blocks);

// Compose the tray hover text:
//   MouseMend <version>
//   Protection enabled - 7/7 buttons
//   12 false clicks blocked
// Parameters:
//   buffer / buffer_count - Destination, truncated to what the shell accepts
//   monitored / total - Button counts
//   blocked - Total blocked events
//   hook_active - Whether the mouse hook is installed
void lang_format_tooltip(wchar_t *buffer, size_t buffer_count,
						 int monitored, int total, uint32_t blocked, bool hook_active);

// Longest possible caption plus room for a version string
#define LANG_TITLE_MAX_CHARS 96

// Compose the check-up window caption, e.g. "MouseMend v<version> —— 鼠标体检修复工具"
//
// The version is taken from version.h at run time, so bumping APP_VERSION_STRING
// updates the caption in every language without touching the translations and
// without a chance of the number drifting away from the executable's resources.
void lang_format_settings_title(wchar_t *buffer, size_t buffer_count);

// Build a localized "value + unit" label, e.g. "50 ms" / "50 毫秒"
const wchar_t *lang_format_ms(int value);
const wchar_t *lang_format_px(int value);

// Width of a string in cells of a fixed pitch font (a CJK glyph is two cells).
// The log panes are drawn with a monospaced font, so padding by this width makes
// the columns line up in every language without relying on tab stops.
size_t lang_display_width(const wchar_t *text);

// Build the column header of the log panes
const wchar_t *lang_format_log_header(wchar_t *buffer, size_t buffer_count);

// Section label that introduces the rows of one program in the live pane.
//
// A label rather than a "switched to" transition: the pane lists newest first,
// so the label sits above the rows it describes and is read as a heading.
const wchar_t *lang_format_live_section(wchar_t *buffer, size_t buffer_count, const wchar_t *name);

// Build one log row; every argument is a finished cell text (or NULL)
//
// One row is one physical click, not one event: the press creates it and the
// release fills in the hold time, so a click is never split over two rows.
// Returns:
//   buffer, so the call can be used directly inside a printf argument
const wchar_t *lang_format_log_row(wchar_t *buffer, size_t buffer_count,
								   const wchar_t *time_text, const wchar_t *button,
								   const wchar_t *gap, const wchar_t *hold,
								   const wchar_t *verdict);
