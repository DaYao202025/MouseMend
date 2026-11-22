#include "about_window.h"
#include "../../resource.h"
#include "../../version.h"
#include "language.h"
#include "window_layout.h"
#include <strsafe.h>
#include <shellapi.h>

// Wide buffer for the version line
#define ABOUT_TEXT_SIZE 64

// The window while it is on screen, so shutdown can dismiss it
static HWND g_about_window;

// Apply the localized texts of the active language
static void apply_language(HWND hDlg)
{
	wchar_t version_text[ABOUT_TEXT_SIZE];

	StringCchPrintfW(version_text, ARRAYSIZE(version_text),
					 lang_str(STR_ABOUT_VERSION_FMT), APP_VERSION_STRING_W);

	// The only thing this dialog says about where the code came from. A user
	// asking "where did this come from" gets a working link, not a name.
	wchar_t origin_text[256];
	StringCchPrintfW(origin_text, ARRAYSIZE(origin_text),
					 lang_str(STR_ABOUT_ORIGIN_FMT), APP_ORIGIN_URL_W);

	SetWindowTextW(hDlg, lang_str(STR_ABOUT));

	SetDlgItemTextW(hDlg, IDC_ABOUT_TITLE, lang_str(STR_APP_NAME));
	SetDlgItemTextW(hDlg, IDC_ABOUT_VERSION, version_text);
	SetDlgItemTextW(hDlg, IDC_ABOUT_DESC, lang_str(STR_ABOUT_DESC));
	SetDlgItemTextW(hDlg, IDC_ABOUT_FEATURES, lang_str(STR_ABOUT_FEATURES));
	SetDlgItemTextW(hDlg, IDC_ABOUT_LIMITS, lang_str(STR_ABOUT_LIMITS));
	SetDlgItemTextW(hDlg, IDC_ABOUT_ORIGIN, origin_text);
	SetDlgItemTextW(hDlg, IDC_ABOUT_WEBSITE, lang_str(STR_ABOUT_WEBSITE_BUTTON));
	SetDlgItemTextW(hDlg, IDC_ABOUT_CLOSE, lang_str(STR_ABOUT_CLOSE_BUTTON));
}

// Dialog procedure
static INT_PTR CALLBACK about_dialog_proc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	(void)lParam;

	switch (message)
	{
	case WM_INITDIALOG:
	{
		g_about_window = hDlg;
		apply_language(hDlg);

		// The icon is set at run time because a DIALOGEX resource would need the
		// .ico embedded a second time under its own identifier
		HINSTANCE instance = (HINSTANCE)GetWindowLongPtrW(hDlg, GWLP_HINSTANCE);
		HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_MAINICON));

		if (icon)
		{
			SendDlgItemMessageW(hDlg, IDC_ABOUT_ICON, STM_SETICON, (WPARAM)icon, 0);
			SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)icon);
			SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)icon);
		}

		window_center_on_monitor(hDlg);
		return TRUE;
	}

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDOK:
		case IDCANCEL:
		case IDC_ABOUT_CLOSE:
			EndDialog(hDlg, 0);
			return TRUE;

		case IDC_ABOUT_WEBSITE:
			// The project page is the only link out of the application, and it is
			// never opened automatically: the user has to click it.
			ShellExecuteW(hDlg, L"open", APP_ORIGIN_URL_W, NULL, NULL, SW_SHOWNORMAL);
			return TRUE;

		default:
			break;
		}
		return FALSE;

	case WM_CLOSE:
		EndDialog(hDlg, 0);
		return TRUE;

	case WM_DESTROY:
		g_about_window = NULL;
		return TRUE;

	default:
		break;
	}

	return FALSE;
}

// Show the window and wait until it is closed
bool about_window_show(HWND owner, HINSTANCE hInstance)
{
	if (!hInstance)
		return false;

	// Already up: bring it forward instead of opening a second copy. The dialog is
	// modal, so this only happens if something posted the command twice.
	if (g_about_window && IsWindow(g_about_window))
	{
		SetForegroundWindow(g_about_window);
		return true;
	}

	INT_PTR result = DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_ABOUT), owner,
									 about_dialog_proc, 0);

	return result != -1;
}

// Close the window if it is open
void about_window_close(void)
{
	if (g_about_window && IsWindow(g_about_window))
		PostMessageW(g_about_window, WM_CLOSE, 0, 0);
}
