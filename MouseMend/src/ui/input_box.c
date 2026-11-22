#include "input_box.h"
#include "language.h"
#include <strsafe.h>
#include <stdlib.h>
#include <wchar.h>

#define INPUT_BOX_CLASS_NAME L"MouseMendInputBoxClass"
#define INPUT_BOX_WIDTH 340
#define INPUT_BOX_HEIGHT 170
#define INPUT_BOX_MESSAGE_BUFFER_SIZE 256
#define IDC_INPUT_EDIT 1001

// Dialog state, valid for the lifetime of the window
typedef struct
{
	LPWSTR buffer;
	int buffer_size;
	int min_value;
	int max_value;
	int value; // Parsed value, valid when ok_pressed is true
	bool ok_pressed;
} InputBoxState;

// Center a window on the monitor that currently has the cursor
static void center_on_active_monitor(HWND hWnd)
{
	RECT window_rect;
	GetWindowRect(hWnd, &window_rect);

	RECT work_area;
	POINT cursor;
	GetCursorPos(&cursor);

	MONITORINFO monitor_info = {0};
	monitor_info.cbSize = sizeof(monitor_info);

	HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
	if (monitor && GetMonitorInfoW(monitor, &monitor_info))
		work_area = monitor_info.rcWork;
	else
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);

	int width = window_rect.right - window_rect.left;
	int height = window_rect.bottom - window_rect.top;

	SetWindowPos(hWnd, NULL,
				 work_area.left + ((work_area.right - work_area.left) - width) / 2,
				 work_area.top + ((work_area.bottom - work_area.top) - height) / 2,
				 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

// Convert full-width digits (and the full-width minus) to ASCII in place.
// A Chinese IME produces "５０" where the user means "50", which wcstol would
// otherwise reject with a confusing "out of range" message.
static void normalize_input(wchar_t *text)
{
	for (wchar_t *p = text; *p; p++)
	{
		if (*p >= 0xFF10 && *p <= 0xFF19)
			*p = (wchar_t)(L'0' + (*p - 0xFF10));
		else if (*p == 0xFF0D)
			*p = L'-';
	}
}

// Dialog window procedure
static LRESULT CALLBACK input_box_proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	InputBoxState *state = (InputBoxState *)GetWindowLongPtr(hWnd, GWLP_USERDATA);

	switch (message)
	{
	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK)
		{
			HWND hEdit = GetDlgItem(hWnd, IDC_INPUT_EDIT);
			if (hEdit && state)
			{
				GetWindowTextW(hEdit, state->buffer, state->buffer_size);
				normalize_input(state->buffer);

				// Validate: the whole text must be a number inside the allowed range
				wchar_t *end = NULL;
				long value = wcstol(state->buffer, &end, 10);
				while (end && *end == L' ')
					end++;

				bool valid = (end && end != state->buffer && *end == L'\0' &&
							  value >= state->min_value && value <= state->max_value);

				if (!valid)
				{
					wchar_t message_text[INPUT_BOX_MESSAGE_BUFFER_SIZE];
					StringCchPrintfW(message_text, ARRAYSIZE(message_text), lang_str(STR_INVALID_INPUT_FMT),
									 state->min_value, state->max_value);
					MessageBoxW(hWnd, message_text, lang_str(STR_SET_THRESHOLD_TITLE), MB_OK | MB_ICONWARNING);

					SendMessageW(hEdit, EM_SETSEL, 0, -1);
					SetFocus(hEdit);
					return 0;
				}

				state->value = (int)value;
				state->ok_pressed = true;
				DestroyWindow(hWnd);
			}
			return 0;
		}
		if (LOWORD(wParam) == IDCANCEL)
		{
			if (state)
				state->ok_pressed = false;
			DestroyWindow(hWnd);
			return 0;
		}
		break;

	case WM_CLOSE:
		if (state)
			state->ok_pressed = false;
		DestroyWindow(hWnd);
		return 0;

	case WM_NCDESTROY:
		// Drop the pointer to the caller's stack frame before the window dies
		SetWindowLongPtr(hWnd, GWLP_USERDATA, 0);
		break;
	}

	return DefWindowProcW(hWnd, message, wParam, lParam);
}

// Show the dialog
bool input_box_show(HINSTANCE hInstance, LPCWSTR title, LPCWSTR prompt,
					LPWSTR buffer, int buffer_size, int min_value, int max_value, int *out_value)
{
	if (!hInstance || !buffer || buffer_size <= 0)
		return false;

	InputBoxState state = {buffer, buffer_size, min_value, max_value, 0, false};

	// Register a simple window class (idempotent, may already exist)
	WNDCLASSW wc = {0};
	wc.lpfnWndProc = input_box_proc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = INPUT_BOX_CLASS_NAME;
	RegisterClassW(&wc);

	HWND hWnd = CreateWindowExW(
		WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
		INPUT_BOX_CLASS_NAME, title,
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
		CW_USEDEFAULT, CW_USEDEFAULT, INPUT_BOX_WIDTH, INPUT_BOX_HEIGHT,
		NULL, NULL, hInstance, NULL);

	if (!hWnd)
		return false;

	SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)&state);
	center_on_active_monitor(hWnd);

	HWND hLabel = CreateWindowExW(0, L"STATIC", prompt,
								  WS_CHILD | WS_VISIBLE | SS_LEFT,
								  20, 18, 290, 22, hWnd, NULL, hInstance, NULL);

	HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", buffer,
								 WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
								 20, 48, 290, 26, hWnd, (HMENU)IDC_INPUT_EDIT, hInstance, NULL);

	HWND hOK = CreateWindowExW(0, L"BUTTON", lang_str(STR_OK),
							   WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
							   150, 90, 80, 30, hWnd, (HMENU)IDOK, hInstance, NULL);

	HWND hCancel = CreateWindowExW(0, L"BUTTON", lang_str(STR_CANCEL),
								   WS_CHILD | WS_VISIBLE,
								   240, 90, 80, 30, hWnd, (HMENU)IDCANCEL, hInstance, NULL);

	// Use the system message font instead of the ancient bitmap font
	HFONT hFont = NULL;
	NONCLIENTMETRICSW metrics = {0};
	metrics.cbSize = sizeof(metrics);
	if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
		hFont = CreateFontIndirectW(&metrics.lfMessageFont);

	HFONT hControlFont = hFont ? hFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	SendMessage(hLabel, WM_SETFONT, (WPARAM)hControlFont, TRUE);
	SendMessage(hEdit, WM_SETFONT, (WPARAM)hControlFont, TRUE);
	SendMessage(hOK, WM_SETFONT, (WPARAM)hControlFont, TRUE);
	SendMessage(hCancel, WM_SETFONT, (WPARAM)hControlFont, TRUE);

	SendMessageW(hEdit, EM_SETLIMITTEXT, (WPARAM)(buffer_size - 1), 0);
	SendMessageW(hEdit, EM_SETSEL, 0, -1);
	SetFocus(hEdit);
	ShowWindow(hWnd, SW_SHOW);

	MSG msg;
	while (IsWindow(hWnd) && GetMessage(&msg, NULL, 0, 0))
	{
		if (!IsDialogMessage(hWnd, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	if (hFont)
		DeleteObject(hFont);

	// The loop can also end while the window still exists (WM_QUIT / error), so
	// make sure no window keeps pointing at this stack frame.
	if (IsWindow(hWnd))
		SetWindowLongPtr(hWnd, GWLP_USERDATA, 0);

	if (out_value)
		*out_value = state.value;

	return state.ok_pressed;
}

// Unregister the dialog window class
void input_box_unregister(HINSTANCE hInstance)
{
	if (hInstance)
		UnregisterClassW(INPUT_BOX_CLASS_NAME, hInstance);
}
