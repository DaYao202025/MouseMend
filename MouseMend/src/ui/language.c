#include "language.h"
#include "../../version.h"
#include <windows.h>
#include <strsafe.h>

// Buffer used by the formatting helpers below. Generous enough for a longer
// localized unit ("milliseconds") plus a 3 digit value, so a translation can
// never silently truncate a menu label.
#define LANG_FORMAT_BUFFER_SIZE 48

// English strings
static const wchar_t *const STRINGS_EN[STR_COUNT] = {
	[STR_APP_NAME] = L"MouseMend",
	[STR_STATS_FMT] = L"Blocked %I32u false clicks",
	[STR_BLOCKED_BUTTON_FMT] = L"%s: %I32u",
	[STR_STATUS_FMT] = L"Current status: %s",
	[STR_ENABLE] = L"Protect this button",
	[STR_ENABLE_ALL] = L"Turn protection on",
	[STR_DISABLE_ALL] = L"Turn protection off",
	[STR_SMART_DRAG] = L"Smart Drag Protection",
	[STR_AUTOSTART] = L"Start with Windows",
	[STR_RESET_STATS] = L"Clear blocked count",
	[STR_PRESETS] = L"Presets",
	[STR_PRESET_DEFAULT_FMT] = L"Default (%s)",
	[STR_PRESET_OFFICE_FMT] = L"Office (%s)",
	[STR_PRESET_STRICT_FMT] = L"Gaming (%s)",
	[STR_BUTTON_TITLE_FMT] = L"%s (%s)",
	[STR_ADVANCED] = L"Advanced",
	[STR_RELEASE_DELAY] = L"Release Delay",
	[STR_TUNING_OFF] = L"Off",
	[STR_DRAG_DISTANCE] = L"Drag Distance",
	[STR_NOTIFY_BLOCKS] = L"Show a balloon when a click is blocked",
	[STR_NOTIFY_ON_TITLE_FMT] = L"MouseMend %s",
	[STR_NOTIFY_ON_TEXT_FMT] = L"Sample. Notifications are on: when a false click is blocked you get a balloon like this one, at most once every %d seconds. Fixes double clicks, wheel jumps and dropped drags.",
	[STR_NOTIFY_BLOCKED_FMT] = L"%I32u more blocked since the last notice (top button this session: %s)",
	[STR_LOG_BLOCKS] = L"Save blocked clicks to a file",
	[STR_OPEN_LOG_FOLDER] = L"Open history folder",
	[STR_OPEN_SETTINGS] = L"Check-up...",
	[STR_SETTINGS_TITLE] = L"MouseMend v%s \u2014 Mouse Check-up Repair Tool",
	[STR_GROUP_STATS] = L"Blocked false clicks",
	[STR_GROUP_TEST] = L"Click test",
	[STR_GROUP_DIAG] = L"Self check",
	[STR_STATUS_OK] = L"Protection is on - your mouse is being watched",
	[STR_STATUS_BAD] = L"Protection is NOT running - please restart MouseMend",
	[STR_STATUS_RECOVERED_FMT] = L"%s (Windows paused it %u times, already restored)",
	[STR_DIAG_HINT] = L"If something looks wrong, send this text to the author.",
	[STR_TEST_HINT] = L"Click in this box a few times\nOne click is one row: gap is release to press",
	[STR_STATS_EMPTY] = L"Nothing blocked yet - which is normal for a mouse that does not double click.",
	[STR_REFRESH] = L"Refresh",
	[STR_CLOSE] = L"Close",
	[STR_HOOK_RECOVERED] = L"Windows briefly paused the protection a moment ago. It is running again - only a few seconds were affected.",
	[STR_LOG_DIR_UNAVAILABLE] = L"The folder where MouseMend lives is read-only, so the history cannot be saved there. Move MouseMend to another folder (for example D:\\ or your desktop) and try again.",
	[STR_RESTORE_DEFAULTS] = L"Restore Defaults",
	[STR_CONFIRM_RESTORE] = L"Restore every setting to its default value?",
	[STR_INTRO_TITLE] = L"MouseMend is running",
	[STR_INTRO_TEXT] = L"MouseMend is protecting your mouse in the background - silence means it is working. If you cannot see the icon, click the ^ arrow next to the clock. Right-click the icon to configure it, or turn on Advanced > notifications to see each blocked click.",
	[STR_MS_FMT] = L"%d ms",
	[STR_PX_FMT] = L"%d px",
	[STR_LANGUAGE] = L"Language",
	[STR_MENU_CUSTOM] = L"Custom...",
	[STR_ABOUT] = L"About MouseMend",
	[STR_MENU_ABOUT_FMT] = L"About MouseMend %s...",
	[STR_WEBSITE] = L"Website",
	[STR_EXIT] = L"Exit",
	[STR_TOOLTIP_ON] = L"Protection enabled",
	[STR_TOOLTIP_PARTIAL] = L"Partial protection",
	[STR_TOOLTIP_OFF] = L"Protection disabled",
	[STR_TOOLTIP_SUMMARY_FMT] = L"%s - %s",
	[STR_TOOLTIP_BLOCKED_FMT] = L"%I32u false clicks blocked",
	[STR_TOOLTIP_NO_BLOCKS] = L"nothing blocked yet",
	[STR_STATUS_UNAVAILABLE] = L"Not active (please restart MouseMend)",
	[STR_SAVE_FAILED] = L"Settings could not be saved and will reset when MouseMend closes.",
	[STR_SET_THRESHOLD_TITLE] = L"Set Blocking Time",
	[STR_THRESHOLD_PROMPT_FMT] = L"Enter the blocking time for %s (%d to %d ms):",
	[STR_INVALID_INPUT_FMT] = L"Out of range. Please enter a whole number between %d and %d.",
	[STR_OK] = L"OK",
	[STR_CANCEL] = L"Cancel",
	[STR_ERR_LOGGER] = L"MouseMend could not start. Please restart your computer and try again.",
	[STR_ERR_MUTEX_FMT] = L"MouseMend could not start. Error code: %lu",
	[STR_ERR_ALREADY_RUNNING] = L"MouseMend is already running. Look for its icon in the ^ area next to the clock.",
	[STR_ERR_WINDOW_CLASS_FMT] = L"MouseMend could not start. Error code: %lu",
	[STR_ERR_CREATE_WINDOW_FMT] = L"MouseMend could not start. Error code: %lu",
	[STR_ERR_UNEXPECTED_FMT] = L"MouseMend stopped unexpectedly. Error code: %lu",
	[STR_ERR_AUTOSTART_FMT] = L"The startup setting could not be changed. Error code: %lu",

	// Check-up window
	[STR_GROUP_LOG] = L"Click log",
	[STR_AUTO] = L"Auto",
	[STR_LOG_COL_TIME] = L"Time",
	[STR_LOG_COL_BUTTON] = L"Button",
	[STR_LOG_COL_GAP] = L"Gap ms",
	[STR_LOG_COL_HOLD] = L"Hold ms",
	[STR_LOG_COL_VERDICT] = L"Verdict",
	[STR_LOG_TIME_FMT] = L"%02u:%02u:%02u",
	[STR_EVIDENCE_TIME_FMT] = L"%02u:%02u:%02u.%03u",
	[STR_LOG_GAP_NONE] = L"-",
	[STR_VERDICT_PASS] = L"passed",
	[STR_VERDICT_BLOCK_FAST] = L"too fast",
	[STR_VERDICT_BLOCK_DEFER] = L"drag",
	[STR_VERDICT_BLOCK_RELEASE] = L"swallow",
	[STR_VERDICT_BLOCK_REVERSE] = L"reverse",
	[STR_VERDICT_DEFER_RELEASE] = L"hold",
	[STR_VERDICT_IGNORE_INJECTED] = L"simulated",
	[STR_VERDICT_IGNORE_UNMONITORED] = L"unprotected",
	[STR_VERDICT_IGNORE_WHEEL_ZERO] = L"no scroll",
	[STR_CLEAR] = L"Clear log",
	[STR_LOG_DIR_BUTTON] = L"Log folder",
	[STR_STATS_SUMMARY_FMT] = L"%I32u blocked - %I32u clicks - gap %I32u-%I32u avg %I32u",
	[STR_STATS_CLICKS_FMT] = L"%I32u blocked - %I32u clicks",
	[STR_GROUP_STATS_NEAR_FMT] = L"Blocked (%I32u near miss)",
	[STR_BLOCKED_NONE] = L"Nothing blocked yet - good news",
	[STR_EVIDENCE_HEADER] = L"--- suspected false clicks ---",
	[STR_EVIDENCE_NONE] = L"No suspicious click so far.",
	[STR_EVIDENCE_FMT] = L"%s: %I32u in total, previous %s, latest %s, gap %s, limit %s",
	[STR_EVIDENCE_ENGINE_FMT] = L" (engine judged by %I32u ms)",

	[STR_ABOUT_VERSION_FMT] = L"Version %s",
	[STR_ABOUT_ORIGIN_FMT] = L"started from %s",
	[STR_ABOUT_DESC] = L"Fixes a worn mouse switch in software: a contact that bounces no longer turns one click into two, and a wheel that flips direction within the threshold no longer scrolls back. Nothing is installed and nothing is collected. It reads and filters input that has already happened - it never creates input of its own, apart from releasing a button it held back.",
	[STR_ABOUT_FEATURES] =
		L"What it does\n"
		L"  - Blocks the second half of a bounce, so one click stays one click\n"
		L"  - Ignores a wheel that reverses direction within the threshold\n"
		L"  - Holds a release back briefly when a press turned into a drag\n"
		L"  - Works on 7 buttons, each with its own threshold\n"
		L"  - Repairs itself if Windows drops the hook\n"
		L"\n"
		L"What it never does\n"
		L"  - No driver, no Windows service, no network access, no telemetry\n"
		L"  - No accessibility changes and no input outside the mouse buttons\n"
		L"  - The check-up window is read-only: it cannot change your settings",
	[STR_ABOUT_LIMITS] =
		L"Where the limits are\n"
		L"  - A suppressed click is gone: the system never sees it\n"
		L"  - A drag shorter than the hold time is not protected\n"
		L"  - Games that hide raw input may bypass the hook entirely",
	[STR_ABOUT_WEBSITE_BUTTON] = L"Origin page",
	[STR_ABOUT_CLOSE_BUTTON] = L"Close",

	[STR_FILTER_HINT] = L"Clear Auto first, then pick a single button on the right",
	[STR_TIP_FILTER_AUTO] = L"Show every button. The three choices on the right stay disabled until this is cleared.",
	[STR_TIP_FILTER_LEFT] = L"Log the left button only.",
	[STR_TIP_FILTER_MIDDLE] = L"Log the middle button only.",
	[STR_TIP_FILTER_RIGHT] = L"Log the right button only.",

	[STR_TRAY_BALLOON_TITLE] = L"Sent to the tray",
	[STR_TRAY_BALLOON_TEXT] = L"MouseMend keeps protecting your mouse in the background. Double-click the tray icon to open this window again.",
	[STR_SETTINGS_UNAVAILABLE] = L"The check-up window could not be opened. Right-click the tray icon and pick it from the menu; if that fails too, restart the program.",

	[STR_EXPORT_SUPPORT] = L"Export diagnostics...",
	[STR_EXPORT_SUPPORT_DONE_FMT] = L"Diagnostics saved to:\n%s\n\nSend this file to the author.",
	[STR_EXPORT_SUPPORT_FAILED] = L"Could not write the file: no folder was writable. Move MouseMend somewhere else (for example D:\\ or your desktop) and try again.",
	[STR_CRASH_REPORT_FOUND_FMT] = L"The program closed unexpectedly last time.\nA report was saved to:\n%s\n\nPlease send it to the author.",
	[STR_HANG_REPORT_FOUND_FMT] = L"The program stopped responding last time and had to be signed out.\nA report was saved to:\n%s\n\nPlease send it to the author.",
	[STR_INJECTION_FAILED] = L"One release could not be delivered to the focused window: it most likely runs as administrator. The mouse itself is fine.",

	[STR_VERDICT_HELP_NAME] = L"Verdicts",
	[STR_VERDICT_HELP_TITLE] = L"The verdict column is the engine's conclusion about each mouse event.",
	[STR_VERDICT_HELP_TEXT] =
		L"Every value it can show:\n"
		L"\n"
		L"Pass\n"
		L"  A normal click, handed to the system untouched.\n"
		L"\n"
		L"Pass + release\n"
		L"  A release was being held back in front of it. That release goes out\n"
		L"  first, then the click passes as usual. To you it is one normal click.\n"
		L"\n"
		L"Simulated\n"
		L"  The event came from another program - a macro, a remote desktop\n"
		L"  session, an input method - not from your mouse. It is passed straight\n"
		L"  through on purpose: filtering injected input would fight those tools.\n"
		L"\n"
		L"Release held\n"
		L"  The end of a drag. The release is held back for a few tens of\n"
		L"  milliseconds so switch bounce cannot interrupt the drag, then it is\n"
		L"  delivered automatically.\n"
		L"\n"
		L"Blocked: too fast\n"
		L"  Too close to the previous event. Treated as the second half of a\n"
		L"  bounce and dropped.\n"
		L"\n"
		L"Blocked: drag\n"
		L"  A release was still being held back, so this press was taken for\n"
		L"  bounce.\n"
		L"\n"
		L"Blocked: release\n"
		L"  This release belonged to a click that was already blocked.\n"
		L"\n"
		L"Blocked: reverse\n"
		L"  The wheel reversed within the threshold. Taken for a bounce, ignored.\n"
		L"\n"
		L"Not protected\n"
		L"  This button is not monitored. The event passed through untouched.\n"
		L"\n"
		L"No scroll\n"
		L"  The wheel reported a zero delta, which means nothing. Ignored.",

	[STR_VERSION_NOTES] = L"Versions",
	[STR_VERSION_NOTES_TITLE] = L"What each release fixed.",
	[STR_VERSION_NOTES_TEXT] =
		L"Only published versions are listed, newest first.\n"
		L"\n"
		L"2.4.2  A status bar at the bottom answers a question nothing else did:\n"
		L"       whether the program in front is being intercepted or let through.\n"
		L"       Its second column carries short notices that fade on their own - a\n"
		L"       hook Windows dropped and we restored, a settings save that failed.\n"
		L"       Self-learning advice is announced when it appears instead of waiting\n"
		L"       to be found in the tray menu. Fixed: two rules with the same file\n"
		L"       name could not be told apart in the list; when Windows would not say\n"
		L"       what the foreground program was, the previous program's verdict was\n"
		L"       applied to it silently.\n"
		L"\n"
		L"2.4.1  The status row is five square status lights now, one per module, each\n"
		L"       with its own hue. Hovering a block opens it to the right in a tenth of\n"
		L"       a second; the first block stays open at rest. Fixed: self-learning\n"
		L"       could advise a threshold that then swallowed a working double click;\n"
		L"       adopting could bypass the 50% limit; rules migrated from an older\n"
		L"       version were lost on the next launch; switching to an empty whitelist\n"
		L"       silently turned filtering off.\n"
		L"\n"
		L"2.4.0  Three new features and a round of fixes. Self-learning watches your\n"
		L"       own double clicks and suggests a threshold per button; nothing is\n"
		L"       changed until you adopt it, and every suggestion states how many\n"
		L"       samples it came from. Whitelist and blacklist mode let you filter\n"
		L"       only some programs, or skip the ones that chatter. The menu applies\n"
		L"       the program you were just in with one click, and switches to the\n"
		L"       matching mode so the rule actually takes effect. Windows processes\n"
		L"       that can never be in front are refused; shell hosts warn first.\n"
		L"       Fixes: the file dialog opened behind the check-up window; a list\n"
		L"       edit did not apply until you switched windows; \"adopt all\" could\n"
		L"       be clickable with no suggestion behind it; a pause could not be\n"
		L"       undone from the same menu entry.\n"
		L"\n"
		L"2.3.0  Fixed the button sticking to the pointer. A release held back by\n"
		L"       Smart Drag could be handed back together with a replay of the\n"
		L"       press that followed it - and on a chattering switch that press IS\n"
		L"       the chatter, so the button was pressed again on your behalf and\n"
		L"       stayed down until the next click. The replay is gone, a held\n"
		L"       release can no longer be lost, and the hold is off by default:\n"
		L"       Advanced > Release Delay > Off. A mouse that never chatters now\n"
		L"       never has a release delayed at all.\n"
		L"\n"
		L"2.2.2  Fixed the button feeling stuck. Smart Drag used to hold a release\n"
		L"       back when the press was long *or* had moved, and while it was held\n"
		L"       the system still thought the button was down - so moving the mouse\n"
		L"       dragged the window or extended a selection. Only a press that was\n"
		L"       long *and* moved now counts as a drag; an ordinary click is\n"
		L"       released at once. The hold is 20 ms instead of 50.\n"
		L"\n"
		L"2.2.1  The click ring is smoother: it runs at 60 frames a second, grows\n"
		L"       from thick to thin, and fades into the background rather than to\n"
		L"       white. A single letter sits in the middle - L, M or R - so the\n"
		L"       colour is no longer the only clue.\n"
		L"       The log panes can be selected and copied again: a new click every\n"
		L"       hundred milliseconds used to throw the selection away.\n"
		L"       The diagnostics bundle no longer carries your Windows user name\n"
		L"       or full file paths, and two exports in the same second no longer\n"
		L"       replace each other.\n"
		L"       The hint in the test area now uses the same font as the window.\n"
		L"\n"
		L"2.2.0  Added two help panels: what every value in the verdict column\n"
		L"       means, and this version list. Both scroll, copy and follow the\n"
		L"       interface language.\n"
		L"\n"
		L"2.1.4  A release the system refuses is no longer silent: it is counted,\n"
		L"       written into the report, and mentioned once per run.\n"
		L"\n"
		L"2.1.3  Fixed a freeze: input is no longer injected from inside the mouse\n"
		L"       hook callback. It used to stall all mouse input for a few\n"
		L"       seconds, then leave this program alone dead to the world and\n"
		L"       impossible to end without signing out.\n"
		L"       Added the hang report: ten seconds of silence and the program\n"
		L"       writes down where it stopped.\n"
		L"\n"
		L"2.1.2  Fixed a crash: the evidence line read a duration as a string\n"
		L"       pointer, so any blocked click could close the program.\n"
		L"       The minidump now loads dbghelp from System32 by full path -\n"
		L"       another program had been putting its own copy in the way.\n"
		L"       The window no longer opens off the screen.\n"
		L"       Added the crash report bundle.\n"
		L"\n"
		L"2.1.0 and 2.1.1 were internal steps and were never published.\n"
		L"\n"
		L"2.0.0  First public build: double clicks, wheel reversal, drags that\n"
		L"       broke in the middle.",

	[STR_HELP_CLOSE] = L"Close",
	[STR_LEARNING] = L"Self-learning thresholds",
	[STR_ADOPT_LEARNING] = L"Adopt all learning suggestions",
	[STR_LEARNING_STATUS_FMT] = L"Learning: L %u / R %u / M %u ms (%u samples)",
	[STR_PAUSE_CONTEXT] = L"Pause filtering for 5 minutes",
	[STR_RESUME_CONTEXT] = L"Resume filtering (paused)",
	[STR_EXCLUDE_CURRENT] = L"Exclude / include the current program",
	[STR_CONTEXT_STATUS_FMT] = L"Current program: %s",

	[STR_WHITELIST_MODE] = L"Whitelist mode",
	[STR_BLACKLIST_MODE] = L"Blacklist mode",
	[STR_CONTEXT_MODE_ALL_WHITE] = L"Off - whitelist inactive",
	[STR_CONTEXT_MODE_ALL_BLACK] = L"Off - blacklist inactive",
	[STR_CONTEXT_MODE_WHITELIST] = L"Filter only these programs",
	[STR_CONTEXT_MODE_BLACKLIST] = L"Do not filter these programs",
	[STR_CONTEXT_ADD] = L"Add program...",
	[STR_CONTEXT_EXCLUDE_FMT] = L"Don't filter %s",
	[STR_CONTEXT_ONLY_FMT] = L"Filter only %s",
	[STR_CONTEXT_EMPTY] = L"No programs added",
	[STR_CONTEXT_WHITE_EMPTY_WARN] = L"Empty: no program is protected",
	[STR_CONTEXT_LEGACY_FMT] = L"%s (legacy rule)",
	[STR_CONTEXT_MIGRATED_FMT] = L"%u rule(s) migrated from an older version",
	[STR_CONTEXT_DELAY] = L"Takes effect immediately",
	[STR_CONTEXT_ADD_TITLE] = L"Select the program to add",
	[STR_CONTEXT_ADDED_FMT] = L"%s added to the %s.",
	[STR_CONTEXT_ERR_SELF] = L"MouseMend itself cannot be added.",
	[STR_CONTEXT_ERR_DUP] = L"That program is already on this list.",
	[STR_CONTEXT_ERR_OTHER] = L"That program is on the other list. Remove it there first.",
	[STR_CONTEXT_ERR_FULL] = L"The list is full (16 programs).",
	[STR_CONTEXT_ERR_INVALID] = L"This file cannot be added.",
	[STR_CONTEXT_ERR_RESERVED] = L"%s is a Windows system process. It never owns a foreground window, so this rule would do nothing.",
	[STR_CONTEXT_WARN_SHELL] = L"%s hosts the desktop, the taskbar or UWP apps.\n\nOne rule here covers that whole surface. Add it anyway?",
	[STR_CONFIRM_EMPTY_WHITELIST] =
		L"The whitelist is empty.\n\n"
		L"Switching to \"only these programs\" means every program is let through, "
		L"which turns filtering off entirely.\n\n"
		L"Switch anyway?",
	[STR_CONTEXT_REMOVE_FMT] = L"Remove this program?\n\n%s",
	[STR_CONTEXT_DISABLED_SUFFIX] = L" (inactive)",
	[STR_WHITELIST_NAME] = L"whitelist",
	[STR_BLACKLIST_NAME] = L"blacklist",
	[STR_LEARNING_ENABLE] = L"Enable self-learning",
	[STR_LEARNING_OFF_HINT] = L"Turn it on to start sampling",
	[STR_LEARNING_ROW_PENDING_FMT] = L"%s  —  %u",
	[STR_LEARNING_KEY_FMT] = L"%s  %u->%u  %u",
	[STR_CAP_HOOK] = L"Hook",
	[STR_CAP_LEARNING] = L"Learn",
	[STR_CAP_CONTEXT] = L"List",
	[STR_CAP_LOG] = L"Log",
	[STR_CAP_STATE_OK] = L"working",
	[STR_CAP_STATE_UNKNOWN] = L"no data",
	[STR_CAP_STATE_STALLED] = L"stopped",
	[STR_CAP_STATE_DISABLED] = L"off",
	[STR_CAP_TIP_FMT] = L"%s: %s",
	[STR_CAP_LABEL_FMT] = L"%s %s",
	[STR_STATUS_SHORT_OK] = L"Protected",
	[STR_STATUS_SHORT_BAD] = L"Not protected",
	[STR_STATUS_STRIP_OK] = L"Mouse protection on",
	[STR_STATUS_STRIP_BAD] = L"Protection problem",
	[STR_STATUS_SHORT_STRIP] = L"P",
	[STR_CAP_SHORT_HOOK] = L"H",
	[STR_CAP_SHORT_LEARNING] = L"L",
	[STR_CAP_SHORT_CONTEXT] = L"R",
	[STR_CAP_SHORT_LOG] = L"G",
	[STR_SB_CONTEXT_NONE] = L"Waiting for a program",
	[STR_SB_CONTEXT_OURS] = L"This window",
	[STR_SB_CONTEXT_EXEMPT_FMT] = L"Exempt: %s",
	[STR_SB_CONTEXT_BLOCKED_FMT] = L"Blocking: %s",
	[STR_SB_HOOK_RECOVERED] = L"Mouse hook restored",
	[STR_SB_SAVE_FAILED] = L"Settings could not be saved and will be lost on restart",
	[STR_SB_ADVICE_FMT] = L"%u threshold suggestion(s) waiting in the tray menu",
	[STR_LEARNING_APPLIED_FMT] = L"Learning applied: %s",
};

// Simplified Chinese strings
static const wchar_t *const STRINGS_ZH_CN[STR_COUNT] = {
	[STR_APP_NAME] = L"MouseMend",
	[STR_STATS_FMT] = L"已拦截误触 %I32u 次",
	[STR_BLOCKED_BUTTON_FMT] = L"%s：%I32u 次",
	[STR_STATUS_FMT] = L"当前状态：%s",
	[STR_ENABLE] = L"保护这个按键",
	[STR_ENABLE_ALL] = L"开启全部防护",
	[STR_DISABLE_ALL] = L"关闭全部防护",
	[STR_SMART_DRAG] = L"智能拖拽保护",
	[STR_AUTOSTART] = L"开机自动启动",
	[STR_RESET_STATS] = L"清零拦截次数",
	[STR_PRESETS] = L"预设方案",
	[STR_PRESET_DEFAULT_FMT] = L"默认（%s）",
	[STR_PRESET_OFFICE_FMT] = L"办公（%s）",
	[STR_PRESET_STRICT_FMT] = L"游戏（%s）",
	[STR_BUTTON_TITLE_FMT] = L"%s（%s）",
	[STR_ADVANCED] = L"高级设置",
	[STR_RELEASE_DELAY] = L"松手延迟",
	[STR_TUNING_OFF] = L"关闭",
	[STR_DRAG_DISTANCE] = L"拖拽距离",
	[STR_NOTIFY_BLOCKS] = L"拦截时弹气泡提示我",
	[STR_NOTIFY_ON_TITLE_FMT] = L"MouseMend %s",
	[STR_NOTIFY_ON_TEXT_FMT] = L"示例。拦截提示已开启：今后拦下误触时会弹一个这样的气泡，最快每 %d 秒一次。功能：修复双击、滚轮回跳、拖拽断触。",
	[STR_NOTIFY_BLOCKED_FMT] = L"此后又拦下 %I32u 次误触（本会话最多：%s）",
	[STR_LOG_BLOCKS] = L"把拦截记录保存成文件",
	[STR_OPEN_LOG_FOLDER] = L"打开历史记录目录",
	[STR_OPEN_SETTINGS] = L"鼠标体检…",
	[STR_SETTINGS_TITLE] = L"MouseMend v%s —— 鼠标体检修复工具",
	[STR_GROUP_STATS] = L"已拦截的误触",
	[STR_GROUP_TEST] = L"测试区",
	[STR_GROUP_DIAG] = L"自检",
	[STR_STATUS_OK] = L"防护正常，正在保护你的鼠标",
	[STR_STATUS_BAD] = L"防护没有生效，请重启 MouseMend",
	[STR_STATUS_RECOVERED_FMT] = L"%s（被系统打断过 %u 次，已自动恢复）",
	[STR_DIAG_HINT] = L"觉得不对劲时，把下面这段文字发给作者即可。",
	[STR_TEST_HINT] = L"在这个方框里点几下，一次点击记 1 行\n间隔＝上次松开到这次按下，按住＝按下到松开",
	[STR_STATS_EMPTY] = L"还没有拦截到误触——鼠标不双击的话，这里是空的属于正常。",
	[STR_REFRESH] = L"刷新",
	[STR_CLOSE] = L"关闭",
	[STR_HOOK_RECOVERED] = L"刚才系统忙了一下，防护停了不到几秒，现在已经自动恢复。",
	[STR_LOG_DIR_UNAVAILABLE] = L"MouseMend 所在的文件夹不能写入，历史记录存不下来。请把程序挪到别的文件夹（比如 D 盘或桌面）再试一次。",
	[STR_RESTORE_DEFAULTS] = L"恢复默认设置",
	[STR_CONFIRM_RESTORE] = L"是否将所有设置恢复为默认值？",
	[STR_INTRO_TITLE] = L"MouseMend 已启动",
	[STR_INTRO_TEXT] = L"MouseMend 已在后台保护你的鼠标，它不出声就说明在工作。若看不到图标，请点任务栏右下角的 ^ 展开隐藏图标；右键图标即可设置，也可在「高级设置」里打开拦截提示，看到每一次拦截。",
	[STR_MS_FMT] = L"%d 毫秒",
	[STR_PX_FMT] = L"%d 像素",
	[STR_LANGUAGE] = L"语言",
	[STR_MENU_CUSTOM] = L"自定义…",
	[STR_ABOUT] = L"关于 MouseMend",
	[STR_MENU_ABOUT_FMT] = L"关于 MouseMend %s…",
	[STR_WEBSITE] = L"项目主页",
	[STR_EXIT] = L"退出",
	[STR_TOOLTIP_ON] = L"防护已启用",
	[STR_TOOLTIP_PARTIAL] = L"部分按键已启用",
	[STR_TOOLTIP_OFF] = L"防护已停用",
	[STR_TOOLTIP_SUMMARY_FMT] = L"%s · %s",
	[STR_TOOLTIP_BLOCKED_FMT] = L"已拦截误触 %I32u 次",
	[STR_TOOLTIP_NO_BLOCKS] = L"还没有拦截到误触",
	[STR_STATUS_UNAVAILABLE] = L"防护未生效（请重启程序）",
	[STR_SAVE_FAILED] = L"设置没能保存，关闭程序后会恢复原样。",
	[STR_SET_THRESHOLD_TITLE] = L"设置拦截时长",
	[STR_THRESHOLD_PROMPT_FMT] = L"请输入%s的拦截时长（%d 到 %d 毫秒）：",
	[STR_INVALID_INPUT_FMT] = L"数值超出范围，请输入 %d 到 %d 之间的整数。",
	[STR_OK] = L"确定",
	[STR_CANCEL] = L"取消",
	[STR_ERR_LOGGER] = L"程序启动失败，请重启电脑后再试。",
	[STR_ERR_MUTEX_FMT] = L"程序启动失败，错误码：%lu",
	[STR_ERR_ALREADY_RUNNING] = L"MouseMend 已经在运行，请查看任务栏右下角 ^ 折叠区里的图标。",
	[STR_ERR_WINDOW_CLASS_FMT] = L"程序启动失败，错误码：%lu",
	[STR_ERR_CREATE_WINDOW_FMT] = L"程序启动失败，错误码：%lu",
	[STR_ERR_UNEXPECTED_FMT] = L"程序意外退出，错误码：%lu",
	[STR_ERR_AUTOSTART_FMT] = L"无法修改开机启动设置，错误码：%lu",

	// Check-up window
	[STR_GROUP_LOG] = L"测试日志",
	[STR_AUTO] = L"自动",
	[STR_LOG_COL_TIME] = L"时间",
	[STR_LOG_COL_BUTTON] = L"按键",
	[STR_LOG_COL_GAP] = L"间隔ms",
	[STR_LOG_COL_HOLD] = L"按住ms",
	[STR_LOG_COL_VERDICT] = L"判定",
	[STR_LOG_TIME_FMT] = L"%02u:%02u:%02u",
	[STR_EVIDENCE_TIME_FMT] = L"%02u:%02u:%02u.%03u",
	[STR_LOG_GAP_NONE] = L"—",
	[STR_VERDICT_PASS] = L"通过",
	[STR_VERDICT_BLOCK_FAST] = L"拦截：太快",
	[STR_VERDICT_BLOCK_DEFER] = L"拦截：拖拽",
	[STR_VERDICT_BLOCK_RELEASE] = L"拦截：松手",
	[STR_VERDICT_BLOCK_REVERSE] = L"拦截：回跳",
	[STR_VERDICT_DEFER_RELEASE] = L"推迟松手",
	[STR_VERDICT_IGNORE_INJECTED] = L"程序模拟",
	[STR_VERDICT_IGNORE_UNMONITORED] = L"此键未防护",
	[STR_VERDICT_IGNORE_WHEEL_ZERO] = L"滚轮无位移",
	[STR_CLEAR] = L"清空记录",
	[STR_LOG_DIR_BUTTON] = L"历史目录",
	[STR_STATS_SUMMARY_FMT] = L"共拦截 %I32u 次 · 点击 %I32u 次 · 间隔 %I32u-%I32u 均 %I32u",
	[STR_STATS_CLICKS_FMT] = L"共拦截 %I32u 次 · 点击 %I32u 次",
	[STR_GROUP_STATS_NEAR_FMT] = L"已拦截的误触（临界 %I32u 次）",
	[STR_BLOCKED_NONE] = L"还没有拦截到误触，这是好事",
	[STR_EVIDENCE_HEADER] = L"—— 疑似误触证据 ——",
	[STR_EVIDENCE_NONE] = L"目前没有发现疑似误触。",
	[STR_EVIDENCE_FMT] = L"%s：累计 %I32u 次，上次 %s，本次 %s，间隔 %s，阈值 %s",
	[STR_EVIDENCE_ENGINE_FMT] = L"（引擎按 %I32u 毫秒判定）",

	[STR_ABOUT_VERSION_FMT] = L"版本 %s",
	[STR_ABOUT_ORIGIN_FMT] = L"源自 %s",
	[STR_ABOUT_DESC] = L"用软件修好磨损的鼠标微动：触点弹跳不会再把一次点击变成两次，滚轮在阈值内反向也不会再往回跳。不装驱动、不收集任何数据。它只读取和过滤已经发生的输入，不会凭空造出按键——唯一会发给系统的，是它刚才拦下没放行的那次松手。",
	[STR_ABOUT_FEATURES] =
		L"它能做什么\n"
		L"  · 拦掉弹跳的后半截，一次点击就是一次点击\n"
		L"  · 滚轮在阈值内反向，判定为回跳并忽略\n"
		L"  · 按下变成拖拽时，把松手轻轻压后再放行\n"
		L"  · 7 个按键全部支持，每个键可以单独设阈值\n"
		L"  · 被系统摘掉钩子后会自动装回来\n"
		L"\n"
		L"它绝不会做什么\n"
		L"  · 不装驱动、不装系统服务、不联网、不收集数据\n"
		L"  · 不改辅助功能设置，也不碰鼠标按键以外的任何输入\n"
		L"  · 鼠标体检窗口是只读的，改不了你的设置",
	[STR_ABOUT_LIMITS] =
		L"它做不到什么\n"
		L"  · 被拦下的那次点击是真的没有了，系统从未收到\n"
		L"  · 短于按住时长的拖拽不会被保护\n"
		L"  · 部分游戏直接读原始输入，可能完全绕过钩子",
	[STR_ABOUT_WEBSITE_BUTTON] = L"原项目地址",
	[STR_ABOUT_CLOSE_BUTTON] = L"关闭",

	[STR_FILTER_HINT] = L"先取消勾选「自动」，才能选右边的单个按键",
	[STR_TIP_FILTER_AUTO] = L"显示所有按键的点击。\n取消勾选后，右边的三个选项才能选。",
	[STR_TIP_FILTER_LEFT] = L"只记录左键的点击。",
	[STR_TIP_FILTER_MIDDLE] = L"只记录中键（滚轮按下去）的点击。",
	[STR_TIP_FILTER_RIGHT] = L"只记录右键的点击。",

	[STR_TRAY_BALLOON_TITLE] = L"已收进托盘图标",
	[STR_TRAY_BALLOON_TEXT] = L"MouseMend 仍在后台保护鼠标。双击托盘图标，可以再打开这个窗口。",
	[STR_SETTINGS_UNAVAILABLE] = L"体检窗口打不开。请右键托盘图标从菜单里选一次；如果还是打不开，请重启程序。",

	[STR_EXPORT_SUPPORT] = L"导出诊断报告…",
	[STR_EXPORT_SUPPORT_DONE_FMT] = L"诊断包已保存到：\n%s\n\n请把这个文件发给作者。",
	[STR_EXPORT_SUPPORT_FAILED] = L"导出失败：没有可写的文件夹。请把程序挪到别处（比如 D 盘或桌面）再试一次。",
	[STR_CRASH_REPORT_FOUND_FMT] = L"上次程序异常退出了。\n诊断包已保存到：\n%s\n\n请把它发给作者。",
	[STR_HANG_REPORT_FOUND_FMT] = L"上次程序卡住了（窗口不再响应），只能注销才能结束。\n诊断包已保存到：\n%s\n\n请把它发给作者。",
	[STR_INJECTION_FAILED] = L"有一次松手没能送进当前窗口，那个窗口大概是以管理员身份运行的。鼠标本身没有问题。",

	[STR_VERDICT_HELP_NAME] = L"判定说明",
	[STR_VERDICT_HELP_TITLE] = L"判定列是引擎对每一次鼠标事件的结论。全部可能的取值如下。",
	[STR_VERDICT_HELP_TEXT] =
		L"通过\n"
		L"  一次正常点击，原样交给系统，没有任何干预。\n"
		L"\n"
		L"通过+补发\n"
		L"  它前面有一次被压住的松手。先把那次松手补发出去，这次点击照常通过，\n"
		L"  对你来说就是一次正常点击。\n"
		L"\n"
		L"程序模拟\n"
		L"  这次事件不是你的鼠标发出的，是别的程序（宏、远程桌面、输入法）注入的。\n"
		L"  这里刻意直接放行 —— 去过滤注入事件会和那些工具互相干扰。\n"
		L"\n"
		L"推迟松手\n"
		L"  一次拖拽的结束。为了不让触点弹跳打断拖拽，松手被压住几十毫秒，\n"
		L"  之后会自动补发。\n"
		L"\n"
		L"拦截：太快\n"
		L"  和上一次事件间隔太短，当作触点弹跳的后半截丢弃。\n"
		L"\n"
		L"拦截：拖拽\n"
		L"  上一次松手还压着没发出去，这次按下被判为弹跳。\n"
		L"\n"
		L"拦截：松手\n"
		L"  这次松开属于一个已经被拦下的点击。\n"
		L"\n"
		L"拦截：回跳\n"
		L"  滚轮在阈值内反向，判定为回跳，忽略。\n"
		L"\n"
		L"此键未防护\n"
		L"  这个按键没有开启防护，事件原样通过。\n"
		L"\n"
		L"滚轮无位移\n"
		L"  滚轮上报的位移是 0，没有意义，忽略。",

	[STR_VERSION_NOTES] = L"版本说明",
	[STR_VERSION_NOTES_TITLE] = L"每个版本修了什么。",
	[STR_VERSION_NOTES_TEXT] =
		L"只列公开发布过的版本，新的在上面。\n"
		L"\n"
		L"2.4.2  窗口底部加了状态栏，回答一个别处没有答案的问题：当前这个程序正在\n"
		L"       被拦截，还是被放行。第二格放临时提示，几秒后自己消失（钩子被系统\n"
		L"       丢弃后自动恢复、配置写入失败）。自学习有了建议会在状态栏说一声，\n"
		L"       不用再去托盘菜单里翻。修复：名单里同名不同路径的两条规则分不出来；\n"
		L"       Windows 说不出前台程序是什么时，会静默套用上一个程序的判定。\n"
		L"\n"
		L"2.4.1  状态行改成五盏方块状态灯，每个模块一种颜色。鼠标移上去，那一块在\n"
		L"       十分之一秒内向右展开；第一个块在静止时常开。修复：自学习可能建议出\n"
		L"       一个会把正常双击吞掉的阈值；「采用」能绕过自己的 50% 限幅；从旧版\n"
		L"       迁移来的名单规则在下次启动时丢失；白名单为空时切过去会静默关闭过滤。\n"
		L"\n"
		L"2.4.0  三个新功能，外加一轮修复。自学习观察你自己的双击，按每个按键给\n"
		L"       出阈值建议；不采纳就不改动任何设置，每条建议都会标出样本数。\n"
		L"       白名单和黑名单可以「只在这些程序里拦截」或「这些程序不拦截」，\n"
		L"       菜单里一键就能把刚才用的程序加进去，并自动切到对应的模式，规则\n"
		L"       才会真的生效。永远不会出现在前台的 Windows 系统进程会被直接\n"
		L"       拒绝；桌面与 UWP 宿主在添加前会先说明影响范围。\n"
		L"       修复：体检窗口开着时文件框弹在它后面；改了名单要切窗口才生效；\n"
		L"       没有建议时「采用」仍可点击；暂停之后无法从同一处撤销。\n"
		L"\n"
		L"2.3.0  修复「按键粘住鼠标」。智能拖拽扣住的松手，可能和紧随其后的按下\n"
		L"       一起补发；对抖动的开关来说，那个按下就是抖动本身，等于替你重新\n"
		L"       按住，直到再点一次才松开。现在重放已删除、被扣住的松手不会再丢，\n"
		L"       并且扣留默认关闭（高级 → 松手延迟 → 关闭）。不抖的鼠标，松手\n"
		L"       不会再被延迟哪怕一毫秒。\n"
		L"\n"
		L"2.2.2  修复左键粘连：智能拖拽原来只要「按得久」或「移动过」就扣住松手，\n"
		L"       扣住期间系统仍认为按键是按下的，一动鼠标就拖窗口、拉选区。现在两个\n"
		L"       条件都满足才算拖拽，普通点击立即放行；扣留时间 50 → 20 毫秒。\n"
		L"\n"
		L"2.2.1  点击光环更顺：60 帧、由粗变细、淡向背景色而不是白色；圆心多了\n"
		L"       一个字（左 / 中 / 右），不用再靠颜色区分按键。\n"
		L"       日志框恢复「选中并复制」：以前每来一次新点击，正在拖的选区就会被\n"
		L"       清掉。\n"
		L"       诊断包不再带 Windows 用户名和完整路径；同一秒内导出两次也不再\n"
		L"       互相覆盖。\n"
		L"       测试区的提示文字改为和窗口其他文字同一字体。\n"
		L"\n"
		L"2.2.0  新增两个说明窗口：判定列每个取值的含义，以及这份版本列表。\n"
		L"       都能滚动、能复制，跟随界面语言。\n"
		L"\n"
		L"2.1.4  送不出去的松手不再静默：会计数、写进诊断报告，并在每次运行提示\n"
		L"       一次。\n"
		L"\n"
		L"2.1.3  修复卡死：注入不再在鼠标钩子回调里发送。之前会先把全系统鼠标\n"
		L"       卡住几秒，然后只有本程序点不动，进程还结束不掉，只能注销。\n"
		L"       新增卡死报告：安静 10 秒就把停在哪写下来。\n"
		L"\n"
		L"2.1.2  修复闪退：拦截证据那行把毫秒数当成字符串指针去读，任何一次\n"
		L"       被拦下的点击都可能让程序直接退出。\n"
		L"       崩溃转储改为从 System32 按全路径加载 dbghelp —— 之前会被\n"
		L"       别的软件放进来的副本顶替。\n"
		L"       窗口不再跑到屏幕外。\n"
		L"       新增崩溃报告包。\n"
		L"\n"
		L"2.1.0 与 2.1.1 是内部版本，没有发布。\n"
		L"\n"
		L"2.0.0  第一个公开版本：修双击、滚轮回跳、拖拽断触。",

	[STR_HELP_CLOSE] = L"关闭",
	[STR_LEARNING] = L"自学习阈值（需手动采用）",
	[STR_ADOPT_LEARNING] = L"采用全部学习建议",
	[STR_LEARNING_STATUS_FMT] = L"学习：左 %u / 右 %u / 中 %u ms（%u 样本）",
	[STR_PAUSE_CONTEXT] = L"暂停过滤 5 分钟",
	[STR_RESUME_CONTEXT] = L"恢复过滤（已暂停）",
	[STR_EXCLUDE_CURRENT] = L"排除 / 取消排除当前程序",
	[STR_CONTEXT_STATUS_FMT] = L"当前程序：%s",

	[STR_WHITELIST_MODE] = L"白名单模式",
	[STR_BLACKLIST_MODE] = L"黑名单模式",
	[STR_CONTEXT_MODE_ALL_WHITE] = L"关闭 — 白名单不生效",
	[STR_CONTEXT_MODE_ALL_BLACK] = L"关闭 — 黑名单不生效",
	[STR_CONTEXT_MODE_WHITELIST] = L"只在白名单程序里拦截",
	[STR_CONTEXT_MODE_BLACKLIST] = L"黑名单程序不拦截",
	[STR_CONTEXT_ADD] = L"添加程序…",
	[STR_CONTEXT_EXCLUDE_FMT] = L"不拦截 %s",
	[STR_CONTEXT_ONLY_FMT] = L"只对 %s 生效",
	[STR_CONTEXT_EMPTY] = L"尚未添加程序",
	[STR_CONTEXT_WHITE_EMPTY_WARN] = L"名单为空：所有程序都不拦截",
	[STR_CONTEXT_LEGACY_FMT] = L"%s（旧版规则）",
	[STR_CONTEXT_MIGRATED_FMT] = L"已从旧版迁移 %u 条规则",
	[STR_CONTEXT_DELAY] = L"改动立即生效",
	[STR_CONTEXT_ADD_TITLE] = L"选择要添加的程序",
	[STR_CONTEXT_ADDED_FMT] = L"已把 %s 添加到%s。",
	[STR_CONTEXT_ERR_SELF] = L"不能添加本程序。",
	[STR_CONTEXT_ERR_DUP] = L"该程序已在这个名单中。",
	[STR_CONTEXT_ERR_OTHER] = L"该程序已在另一个名单中，请先在那里移除。",
	[STR_CONTEXT_ERR_FULL] = L"名单已满（16 个程序）。",
	[STR_CONTEXT_ERR_INVALID] = L"无法添加这个文件。",
	[STR_CONTEXT_ERR_RESERVED] = L"%s 是 Windows 系统进程，永远不会出现在前台，这条规则不会有任何效果。",
	[STR_CONTEXT_WARN_SHELL] = L"%s 承载桌面、任务栏或 UWP 应用。\n\n一条规则会影响这一整类场景，仍要添加吗？",
	[STR_CONFIRM_EMPTY_WHITELIST] =
		L"白名单里还没有程序。\n\n"
		L"切到「只在白名单程序里拦截」后，所有程序都会被放行，等于完全关闭过滤。\n\n"
		L"仍要切换吗？",
	[STR_CONTEXT_REMOVE_FMT] = L"要移除这个程序吗？\n\n%s",
	[STR_CONTEXT_DISABLED_SUFFIX] = L"（未启用）",
	[STR_WHITELIST_NAME] = L"白名单",
	[STR_BLACKLIST_NAME] = L"黑名单",
	[STR_LEARNING_ENABLE] = L"启用自学习",
	[STR_LEARNING_OFF_HINT] = L"开启后开始采样",
	[STR_LEARNING_ROW_PENDING_FMT] = L"%s  —  %u",
	[STR_LEARNING_KEY_FMT] = L"%s  %u→%u  %u",
	[STR_CAP_HOOK] = L"钩子",
	[STR_CAP_LEARNING] = L"学习",
	[STR_CAP_CONTEXT] = L"名单",
	[STR_CAP_LOG] = L"日志",
	[STR_CAP_STATE_OK] = L"正常",
	[STR_CAP_STATE_UNKNOWN] = L"待数据",
	[STR_CAP_STATE_STALLED] = L"未工作",
	[STR_CAP_STATE_DISABLED] = L"未启用",
	[STR_CAP_TIP_FMT] = L"%s：%s",
	[STR_CAP_LABEL_FMT] = L"%s %s",
	[STR_STATUS_SHORT_OK] = L"防护中",
	[STR_STATUS_SHORT_BAD] = L"未防护",
	[STR_STATUS_STRIP_OK] = L"鼠标防护中",
	[STR_STATUS_STRIP_BAD] = L"鼠标防护异常",
	[STR_STATUS_SHORT_STRIP] = L"防",
	[STR_CAP_SHORT_HOOK] = L"钩",
	[STR_CAP_SHORT_LEARNING] = L"学",
	[STR_CAP_SHORT_CONTEXT] = L"名",
	[STR_CAP_SHORT_LOG] = L"志",
	[STR_SB_CONTEXT_NONE] = L"等待前台程序",
	[STR_SB_CONTEXT_OURS] = L"本程序窗口",
	[STR_SB_CONTEXT_EXEMPT_FMT] = L"已排除：%s",
	[STR_SB_CONTEXT_BLOCKED_FMT] = L"已拦截：%s",
	[STR_SB_HOOK_RECOVERED] = L"已自动恢复鼠标钩子",
	[STR_SB_SAVE_FAILED] = L"设置保存失败，重启后会丢失",
	[STR_SB_ADVICE_FMT] = L"有 %u 项阈值建议可在托盘菜单采纳",
	[STR_LEARNING_APPLIED_FMT] = L"已采用学习建议：%s",
};

// Button names, indexed by MouseButton
static const wchar_t *const BUTTON_NAMES_EN[MOUSE_BUTTON_COUNT] = {
	[MOUSE_BUTTON_LEFT] = L"Left",
	[MOUSE_BUTTON_RIGHT] = L"Right",
	[MOUSE_BUTTON_MIDDLE] = L"Middle",
	[MOUSE_BUTTON_X1] = L"X1",
	[MOUSE_BUTTON_X2] = L"X2",
	[MOUSE_BUTTON_WHEEL] = L"Wheel",
	[MOUSE_BUTTON_HWHEEL] = L"H-Wheel",
};

static const wchar_t *const BUTTON_NAMES_ZH_CN[MOUSE_BUTTON_COUNT] = {
	[MOUSE_BUTTON_LEFT] = L"左键",
	[MOUSE_BUTTON_RIGHT] = L"右键",
	[MOUSE_BUTTON_MIDDLE] = L"中键",
	[MOUSE_BUTTON_X1] = L"侧键 1",
	[MOUSE_BUTTON_X2] = L"侧键 2",
	[MOUSE_BUTTON_WHEEL] = L"滚轮",
	[MOUSE_BUTTON_HWHEEL] = L"横向滚轮",
};

// Compact names for the log panes.
// The name column is eight cells wide and a double width glyph costs two, so
// the spaced out menu names do not fit: "Side 1" is seven cells but that leaves
// no room for a wider label, and every cell here is spaces that a row wastes.
static const wchar_t *const BUTTON_SHORT_NAMES_EN[MOUSE_BUTTON_COUNT] = {
	[MOUSE_BUTTON_LEFT] = L"Left",
	[MOUSE_BUTTON_RIGHT] = L"Right",
	[MOUSE_BUTTON_MIDDLE] = L"Middle",
	[MOUSE_BUTTON_X1] = L"X1",
	[MOUSE_BUTTON_X2] = L"X2",
	[MOUSE_BUTTON_WHEEL] = L"Wheel",
	[MOUSE_BUTTON_HWHEEL] = L"H-Wheel",
};

static const wchar_t *const BUTTON_SHORT_NAMES_ZH_CN[MOUSE_BUTTON_COUNT] = {
	[MOUSE_BUTTON_LEFT] = L"左键",
	[MOUSE_BUTTON_RIGHT] = L"右键",
	[MOUSE_BUTTON_MIDDLE] = L"中键",
	[MOUSE_BUTTON_X1] = L"侧键1",
	[MOUSE_BUTTON_X2] = L"侧键2",
	[MOUSE_BUTTON_WHEEL] = L"滚轮",
	[MOUSE_BUTTON_HWHEEL] = L"横滚",
};

// One glyph per button, drawn inside the ring of the test area.
//
// A single glyph is square, so it centres exactly in a circle. It also says which
// button was pressed without the user having to learn the colour code, which is
// the only cue left to someone who cannot tell red from green.
static const wchar_t *const BUTTON_MARKS_EN[MOUSE_BUTTON_COUNT] = {
	[MOUSE_BUTTON_LEFT] = L"L",
	[MOUSE_BUTTON_RIGHT] = L"R",
	[MOUSE_BUTTON_MIDDLE] = L"M",
};

static const wchar_t *const BUTTON_MARKS_ZH_CN[MOUSE_BUTTON_COUNT] = {
	[MOUSE_BUTTON_LEFT] = L"左",
	[MOUSE_BUTTON_RIGHT] = L"右",
	[MOUSE_BUTTON_MIDDLE] = L"中",
};

// Table lookup per language, so adding a language is a one-line change here
static const wchar_t *const *const STRING_TABLES[LANG_COUNT] = {
	[LANG_EN] = STRINGS_EN,
	[LANG_ZH_CN] = STRINGS_ZH_CN,
};

static const wchar_t *const *const BUTTON_NAME_TABLES[LANG_COUNT] = {
	[LANG_EN] = BUTTON_NAMES_EN,
	[LANG_ZH_CN] = BUTTON_NAMES_ZH_CN,
};

static const wchar_t *const *const BUTTON_SHORT_NAME_TABLES[LANG_COUNT] = {
	[LANG_EN] = BUTTON_SHORT_NAMES_EN,
	[LANG_ZH_CN] = BUTTON_SHORT_NAMES_ZH_CN,
};

static const wchar_t *const *const BUTTON_MARK_TABLES[LANG_COUNT] = {
	[LANG_EN] = BUTTON_MARKS_EN,
	[LANG_ZH_CN] = BUTTON_MARKS_ZH_CN,
};

// Currently active language
static Language g_language = LANG_EN;

// Formatting buffers, one per language so a cached pointer never changes meaning.
// Only the UI thread calls these helpers.
static wchar_t g_ms_buffers[LANG_COUNT][LANG_FORMAT_BUFFER_SIZE];
static wchar_t g_px_buffers[LANG_COUNT][LANG_FORMAT_BUFFER_SIZE];

// Detect the language matching the current Windows display language
Language language_detect_system(void)
{
	LANGID lang_id = GetUserDefaultUILanguage();

	if (PRIMARYLANGID(lang_id) == LANG_CHINESE)
		return LANG_ZH_CN;

	return LANG_EN;
}

// Get the active language
Language language_get(void)
{
	return g_language;
}

// Set the active language
void language_set(Language language)
{
	if (language < 0 || language >= LANG_COUNT)
		return;

	g_language = language;
}

// Get the string table of the active language
static const wchar_t *const *active_string_table(void)
{
	Language language = (g_language >= 0 && g_language < LANG_COUNT) ? g_language : LANG_EN;
	return STRING_TABLES[language];
}

// Get the button name table of the active language
static const wchar_t *const *active_button_name_table(void)
{
	Language language = (g_language >= 0 && g_language < LANG_COUNT) ? g_language : LANG_EN;
	return BUTTON_NAME_TABLES[language];
}

// Get the compact button name table of the active language
static const wchar_t *const *active_button_short_name_table(void)
{
	Language language = (g_language >= 0 && g_language < LANG_COUNT) ? g_language : LANG_EN;
	return BUTTON_SHORT_NAME_TABLES[language];
}

// Get a localized string
const wchar_t *lang_str(LangStringId id)
{
	if (id < 0 || id >= STR_COUNT)
		return L"";

	const wchar_t *text = active_string_table()[id];

	// Fall back to English when a translation is missing
	if (!text)
		text = STRINGS_EN[id];

	return text ? text : L"";
}

// Get the localized name of a mouse button
const wchar_t *lang_button_name(MouseButton button)
{
	if (button < 0 || button >= MOUSE_BUTTON_COUNT)
		return L"";

	const wchar_t *name = active_button_name_table()[button];

	if (!name)
		name = BUTTON_NAMES_EN[button];

	return name ? name : L"";
}

// Get the compact name used by the log panes
const wchar_t *lang_button_short_name(MouseButton button)
{
	if (button < 0 || button >= MOUSE_BUTTON_COUNT)
		return L"";

	const wchar_t *name = active_button_short_name_table()[button];

	if (!name)
		name = BUTTON_SHORT_NAMES_EN[button];

	return name ? name : L"";
}

// Get the single glyph drawn inside the test area ring
// Empty for buttons that never draw a ring, so the caller can skip the text
const wchar_t *lang_button_mark(MouseButton button)
{
	if (button < 0 || button >= MOUSE_BUTTON_COUNT)
		return L"";

	Language language = (g_language >= 0 && g_language < LANG_COUNT) ? g_language : LANG_EN;
	const wchar_t *mark = BUTTON_MARK_TABLES[language][button];

	if (!mark)
		mark = BUTTON_MARKS_EN[button];

	return mark ? mark : L"";
}

// Get the endonym of a language
const wchar_t *lang_language_name(Language language)
{
	switch (language)
	{
	case LANG_ZH_CN:
		return L"中文（简体）";
	case LANG_EN:
	default:
		return L"English";
	}
}

// Compose one "left button: 9" line for the blocked-statistics submenu
const wchar_t *lang_format_blocked_button(wchar_t *buffer, size_t buffer_count,
										  MouseButton button, uint32_t blocks)
{
	if (!buffer || buffer_count == 0)
		return L"";

	buffer[0] = L'\0';
	StringCchPrintfW(buffer, buffer_count, lang_str(STR_BLOCKED_BUTTON_FMT),
					 lang_button_name(button), blocks);

	return buffer;
}

// Compose the check-up window caption
void lang_format_settings_title(wchar_t *buffer, size_t buffer_count)
{
	if (!buffer || buffer_count == 0)
		return;

	buffer[0] = L'\0';
	StringCchPrintfW(buffer, buffer_count, lang_str(STR_SETTINGS_TITLE), APP_VERSION_STRING_W);
}

// Compose the tray hover text
void lang_format_tooltip(wchar_t *buffer, size_t buffer_count,
						 int monitored, int total, uint32_t blocked, bool hook_active)
{
	if (!buffer || buffer_count == 0)
		return;

	buffer[0] = L'\0';

	if (total <= 0)
		total = 0;

	if (monitored < 0)
		monitored = 0;

	if (monitored > total)
		monitored = total;

	// When the hook is not installed a blocked count is meaningless, so the hover
	// text degrades to a single explanatory line instead of contradicting itself.
	if (!hook_active)
	{
		StringCchPrintfW(buffer, buffer_count, L"%s %s\n%s",
						 lang_str(STR_APP_NAME), APP_VERSION_STRING_W, lang_str(STR_STATUS_UNAVAILABLE));
		return;
	}

	const wchar_t *state_text;
	if (monitored == 0)
		state_text = lang_str(STR_TOOLTIP_OFF);
	else if (monitored >= total)
		state_text = lang_str(STR_TOOLTIP_ON);
	else
		state_text = lang_str(STR_TOOLTIP_PARTIAL);

	// "12" reads as "it is doing something", "0" would read as "it never works",
	// so the zero case gets its own wording instead of a bare number.
	wchar_t blocked_line[96];
	if (blocked == 0)
		StringCchCopyW(blocked_line, ARRAYSIZE(blocked_line), lang_str(STR_TOOLTIP_NO_BLOCKS));
	else
		StringCchPrintfW(blocked_line, ARRAYSIZE(blocked_line), lang_str(STR_TOOLTIP_BLOCKED_FMT), blocked);

	// Two lines: the shell renders short tooltips reliably, and the version on
	// line one lets support read it straight off a screenshot.
	wchar_t summary[160];
	StringCchPrintfW(summary, ARRAYSIZE(summary), lang_str(STR_TOOLTIP_SUMMARY_FMT), state_text, blocked_line);

	StringCchPrintfW(buffer, buffer_count, L"%s %s\n%s",
					 lang_str(STR_APP_NAME), APP_VERSION_STRING_W, summary);
}

// Format a millisecond value with its localized unit
const wchar_t *lang_format_ms(int value)
{
	Language language = (g_language >= 0 && g_language < LANG_COUNT) ? g_language : LANG_EN;

	wchar_t *buffer = g_ms_buffers[language];
	StringCchPrintfW(buffer, LANG_FORMAT_BUFFER_SIZE, lang_str(STR_MS_FMT), value);

	return buffer;
}

// Format a pixel value with its localized unit
const wchar_t *lang_format_px(int value)
{
	Language language = (g_language >= 0 && g_language < LANG_COUNT) ? g_language : LANG_EN;

	wchar_t *buffer = g_px_buffers[language];
	StringCchPrintfW(buffer, LANG_FORMAT_BUFFER_SIZE, lang_str(STR_PX_FMT), value);

	return buffer;
}

// Column widths of the log panes, in cells of the monospaced font
// Every width must be at least as wide as its own header label, otherwise the
// padding would stop working and the columns would drift apart.
// Measured, not guessed: the log font renders one cell every 7.3 px, and the
// pane is 330 px wide once its scrollbar is accounted for, so a row has about
// 45 cells. The header and every verdict must fit in that budget or the rows
// wrap and the table falls apart.
//
// The four fixed columns add up to 33 cells, one more than the three columns
// and the press counter used to take. Dropping the counter paid for the hold
// time column, and the extra cell is what keeps a full "23:18:45" away from the
// button name that follows it.
#define LOG_COL_TIME 9
#define LOG_COL_BUTTON 8
#define LOG_COL_GAP 8
#define LOG_COL_HOLD 8

// Width of a string in fixed pitch cells
// A CJK glyph occupies exactly two cells in a monospaced font, which is what
// makes padding work in both languages.
size_t lang_display_width(const wchar_t *text)
{
	size_t width = 0;

	for (const wchar_t *p = text; p && *p; p++)
	{
		unsigned int code = (unsigned int)*p;

		bool wide = (code >= 0x1100 && code <= 0x115F) ||
					(code >= 0x2E80 && code <= 0xA4CF) ||
					(code >= 0xAC00 && code <= 0xD7A3) ||
					(code >= 0xF900 && code <= 0xFAFF) ||
					(code >= 0xFE30 && code <= 0xFE6F) ||
					(code >= 0xFF00 && code <= 0xFF60) ||
					(code >= 0xFFE0 && code <= 0xFFE6);

		width += wide ? 2 : 1;
	}

	return width;
}

// Append one cell and pad it with spaces to a fixed width
static void append_cell(wchar_t *buffer, size_t buffer_count, const wchar_t *text, size_t width)
{
	if (!text)
		text = L"";

	StringCchCatW(buffer, buffer_count, text);

	size_t used = lang_display_width(text);
	while (used < width)
	{
		if (FAILED(StringCchCatW(buffer, buffer_count, L" ")))
			return;

		used++;
	}
}

// Build the column header of the log panes
const wchar_t *lang_format_log_header(wchar_t *buffer, size_t buffer_count)
{
	if (!buffer || buffer_count == 0)
		return L"";

	buffer[0] = L'\0';

	append_cell(buffer, buffer_count, lang_str(STR_LOG_COL_TIME), LOG_COL_TIME);
	append_cell(buffer, buffer_count, lang_str(STR_LOG_COL_BUTTON), LOG_COL_BUTTON);
	append_cell(buffer, buffer_count, lang_str(STR_LOG_COL_GAP), LOG_COL_GAP);
	append_cell(buffer, buffer_count, lang_str(STR_LOG_COL_HOLD), LOG_COL_HOLD);
	StringCchCatW(buffer, buffer_count, lang_str(STR_LOG_COL_VERDICT));

	return buffer;
}

// Build one log row
const wchar_t *lang_format_log_row(wchar_t *buffer, size_t buffer_count,
								   const wchar_t *time_text, const wchar_t *button,
								   const wchar_t *gap, const wchar_t *hold,
								   const wchar_t *verdict)
{
	if (!buffer || buffer_count == 0)
		return L"";

	buffer[0] = L'\0';

	append_cell(buffer, buffer_count, time_text, LOG_COL_TIME);
	append_cell(buffer, buffer_count, button, LOG_COL_BUTTON);
	append_cell(buffer, buffer_count, gap, LOG_COL_GAP);
	append_cell(buffer, buffer_count, hold, LOG_COL_HOLD);
	StringCchCatW(buffer, buffer_count, verdict ? verdict : L"");

	return buffer;
}
