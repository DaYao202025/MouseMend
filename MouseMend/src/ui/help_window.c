#include "help_window.h"
#include "../../resource.h"
#include "language.h"
#include "window_layout.h"
#include <strsafe.h>

// The body text is a few kilobytes of wide characters and is rebuilt on every
// open, so a static buffer keeps it off the stack. A read-only edit control
// wants CRLF: a bare LF either shows as a box or does not break the line at all,
// depending on the version of the control.
#define HELP_TEXT_MAX 8192

static wchar_t g_text[HELP_TEXT_MAX];
static HWND g_help_window;

// Copy source into dest, turning LF into CRLF
static void expand_newlines(wchar_t *dest, size_t count, const wchar_t *source)
{
	size_t out = 0;

	for (size_t i = 0; source && source[i] && out + 2 < count; i++)
	{
		if (source[i] == L'\n' && (out == 0 || dest[out - 1] != L'\r'))
			dest[out++] = L'\r';

		dest[out++] = source[i];
	}

	dest[out] = L'\0';
}

static void apply_language(HWND hDlg, HelpTopic topic)
{
	const wchar_t *caption;
	const wchar_t *heading;
	LangStringId body;

	if (topic == HELP_TOPIC_VERSIONS)
	{
		caption = lang_str(STR_VERSION_NOTES);
		heading = lang_str(STR_VERSION_NOTES_TITLE);
		body = STR_VERSION_NOTES_TEXT;
	}
	else
	{
		caption = lang_str(STR_VERDICT_HELP_NAME);
		heading = lang_str(STR_VERDICT_HELP_TITLE);
		body = STR_VERDICT_HELP_TEXT;
	}

	SetWindowTextW(hDlg, caption);
	SetDlgItemTextW(hDlg, IDC_HELP_TITLE, heading);

	expand_newlines(g_text, ARRAYSIZE(g_text), lang_str(body));
	SetDlgItemTextW(hDlg, IDC_HELP_TEXT, g_text);
	SetDlgItemTextW(hDlg, IDC_HELP_CLOSE, lang_str(STR_HELP_CLOSE));

	// A read-only edit selects its whole contents the first time it takes focus,
	// which is why the caret is collapsed again after SetFocus in WM_INITDIALOG,
	// not here: doing it in this order would be undone a moment later.
}

static INT_PTR CALLBACK help_dialog_proc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	(void)wParam;

	switch (message)
	{
	case WM_INITDIALOG:
	{
		// The topic arrives in lParam: that is what DialogBoxParam's dwInitParam
		// is delivered as. wParam holds the control to focus, not the topic.
		g_help_window = hDlg;
		window_center_on_monitor(hDlg);

		// Focus first, fill afterwards. A read-only edit selects everything the
		// moment it takes focus, so filling it later leaves the selection
		// collapsed at the top instead of highlighting the first paragraph - which
		// looks like the window opened in the middle of something.
		apply_language(hDlg, (HelpTopic)lParam);

		// The close button takes the initial focus, not the text.
		//
		// A read-only edit selects its whole contents the moment it takes focus,
		// which is what a user sees as the first paragraph sitting in a highlight
		// box - it looks like the window opened in the middle of something, and it
		// cannot be cleared reliably from here: EM_SETSEL sent right after the
		// focus arrives loses to the selection the control applies afterwards.
		// Not focusing the text at all avoids the whole problem. Clicking into it
		// still selects and copies, and the wheel still scrolls.
		SetFocus(GetDlgItem(hDlg, IDC_HELP_CLOSE));

		// FALSE: the focus was already set above
		return FALSE;
	}

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDOK:
		case IDCANCEL:
		case IDC_HELP_CLOSE:
			EndDialog(hDlg, 0);
			return TRUE;

		default:
			break;
		}
		return FALSE;

	case WM_CLOSE:
		EndDialog(hDlg, 0);
		return TRUE;

	case WM_DESTROY:
		g_help_window = NULL;
		return TRUE;

	default:
		break;
	}

	return FALSE;
}

bool help_window_show(HWND owner, HINSTANCE instance, HelpTopic topic)
{
	if (!instance)
		return false;

	// Already up: bring it forward instead of stacking a second copy. The dialog
	// is modal, so this only happens if a command was posted twice.
	if (g_help_window && IsWindow(g_help_window))
	{
		SetForegroundWindow(g_help_window);
		return true;
	}

	INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_HELP), owner,
									 help_dialog_proc, (LPARAM)topic);

	return result != -1;
}

void help_window_close(void)
{
	if (g_help_window && IsWindow(g_help_window))
		PostMessageW(g_help_window, WM_CLOSE, 0, 0);
}
