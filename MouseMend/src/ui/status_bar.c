#include "status_bar.h"

#include <commctrl.h>
#include <strsafe.h>
#include <wchar.h>

#pragma comment(lib, "comctl32.lib")

// The control keeps a border between its edge and the first part, and another
// between neighbouring parts. It reports both through SB_GETBORDERS; this is
// only the fallback for a control that refuses to answer.
#define STATUS_BAR_FALLBACK_BORDER 2

#define STATUS_BAR_MAX_PARTS 32

static HWND status_bar_control(HWND hDlg, int control_id)
{
	if (!hDlg)
		return NULL;

	return GetDlgItem(hDlg, control_id);
}

void status_bar_resize(HWND hDlg, int control_id)
{
	HWND bar = status_bar_control(hDlg, control_id);

	if (!bar)
		return;

	RECT client;
	if (!GetClientRect(hDlg, &client))
		return;

	// Let the control place itself. It moves to the bottom of the parent's
	// client area and sizes itself from the font it was given, which already
	// accounts for DPI. Doing the same arithmetic by hand is how a window ends
	// up with a one-pixel strip of background showing under the bar at 125%.
	SendMessageW(bar, WM_SIZE, 0, MAKELPARAM(client.right - client.left,
											client.bottom - client.top));
}

static void status_bar_layout(HWND hDlg, HWND bar, const StatusBarPart *parts,
							  int count)
{
	RECT client;
	if (!GetClientRect(bar, &client))
		return;

	int total = client.right - client.left;

	int borders[3] = {STATUS_BAR_FALLBACK_BORDER, 0, STATUS_BAR_FALLBACK_BORDER};
	SendMessageW(bar, SB_GETBORDERS, 0, (LPARAM)borders);

	// What is left for the parts after the frame's own margins. Without this the
	// last caption runs past the right edge and gets clipped mid character.
	int usable = total - 2 * borders[0] - borders[2] * (count - 1);
	if (usable < count)
		usable = count;

	int widths[STATUS_BAR_MAX_PARTS] = {0};
	int fixed = 0;
	int fill = -1;

	for (int i = 0; i < count; i++)
	{
		if (parts[i].width <= 0)
		{
			if (fill < 0)
				fill = i;

			// A second "fill" part gets nothing: only one column can absorb the
			// remainder without the layout becoming ambiguous.
			widths[i] = 0;
			continue;
		}

		// Dialog units through the dialog itself, so the caller's numbers mean
		// the same thing they would in the .rc and follow the user's DPI.
		RECT unit = {0, 0, parts[i].width, 0};
		MapDialogRect(hDlg, &unit);

		widths[i] = unit.right;
		fixed += widths[i];
	}

	if (fill >= 0)
		widths[fill] = usable - fixed;

	int edges[STATUS_BAR_MAX_PARTS] = {0};
	int accumulated = 0;

	for (int i = 0; i < count; i++)
	{
		if (widths[i] < 1)
			widths[i] = 1;

		accumulated += widths[i];

		// -1 means "to the right edge", which is what SB_SETPARTS wants for the
		// last column: computing it instead would leave a seam whenever rounding
		// disagrees with the control by one pixel. The cost is that a last column
		// with a fixed width ignores the frame margin subtracted above and loses
		// its right padding - invisible in practice, and cheaper than the seam.
		edges[i] = (i == count - 1) ? -1 : accumulated;
	}

	SendMessageW(bar, SB_SETPARTS, (WPARAM)count, (LPARAM)edges);
}

bool status_bar_attach(HWND hDlg, int control_id, const StatusBarPart *parts,
					   int count)
{
	HWND bar = status_bar_control(hDlg, control_id);

	if (!bar || !parts || count <= 0 || count > STATUS_BAR_MAX_PARTS)
		return false;

	// Inherit the dialog font. A status bar defaults to the stock GUI font, one
	// size smaller than the dialog, which is immediately visible as text that
	// does not belong to the rest of the window.
	HFONT font = (HFONT)SendMessageW(hDlg, WM_GETFONT, 0, 0);
	if (font)
		SendMessageW(bar, WM_SETFONT, (WPARAM)font, FALSE);

	// Sized first, laid out second: the parts are computed from the client width
	// the control has *after* it has been placed.
	status_bar_resize(hDlg, control_id);
	status_bar_layout(hDlg, bar, parts, count);

	for (int i = 0; i < count; i++)
	{
		if (parts[i].text && parts[i].text[0])
			SendMessageW(bar, SB_SETTEXTW, (WPARAM)i, (LPARAM)parts[i].text);
	}

	return true;
}

bool status_bar_set(HWND hDlg, int control_id, int part, const wchar_t *text)
{
	HWND bar = status_bar_control(hDlg, control_id);

	if (!bar || part < 0 || part >= STATUS_BAR_MAX_PARTS)
		return false;

	const wchar_t *caption = text ? text : L"";

	// Compare before writing. SB_SETTEXT repaints the part even when the caption
	// is identical, and this is called from a 400 ms refresh whose answer is
	// usually the same every time.
	//
	// The buffer is a fixed 256 characters, and the comparison is only as good as
	// what fits in it: a caption longer than that would be truncated on read,
	// never match, and repaint on every refresh. Every caption in this project is
	// shorter than 40 characters, so this is a limit worth stating rather than
	// handling.
	wchar_t current[256];
	current[0] = L'\0';

	SendMessageW(bar, SB_GETTEXTW, (WPARAM)part, (LPARAM)current);

	if (wcscmp(current, caption) == 0)
		return true;

	SendMessageW(bar, SB_SETTEXTW, (WPARAM)part, (LPARAM)caption);
	return true;
}
