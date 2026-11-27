#pragma once

#include <windows.h>
#include <stdbool.h>

// A thin wrapper over the native status bar (msctls_statusbar32).
//
// Every window that wants one puts the control in its own .rc at the very
// bottom and calls status_bar_attach once, from WM_INITDIALOG. Everything that
// is easy to get wrong lives here instead of being repeated per window:
//
//  - Registering ICC_BAR_CLASSES. Without it the class is not registered and the
//    dialog simply shows no control - no error anywhere, because a failed child
//    creation inside a dialog template is silent.
//  - Positioning. A dialog does not forward WM_SIZE to its children, so the bar
//    never places itself; it has to be moved to the client bottom by hand, and
//    its height has to be asked for, not assumed.
//  - Font. A status bar defaults to the stock font, one size off from the dialog
//    font, which makes it look like a widget from another program.
//
// Part geometry is data, not code: each window describes its own columns and
// this module applies them, so two windows cannot drift into two different
// looks.

// Part widths are in dialog units, the same unit the .rc uses, so a layout can
// be read against the template without doing pixel arithmetic.
//
// A width of 0 means "take the remaining space". At most one part should use
// it; a second one gets nothing.
typedef struct
{
	int width;            // dialog units, 0 for "fill what is left"
	const wchar_t *text;  // initial caption, may be NULL
} StatusBarPart;

// Register the class, size, font and fill the bar.
//
// Returns false when the control is absent, which is the case on a system where
// ICC_BAR_CLASSES could not be registered. Callers treat that as "no status bar"
// rather than as an error: the window works, it just has one less place to
// report.
bool status_bar_attach(HWND hDlg, int control_id, const StatusBarPart *parts, int count);

// Replace the caption of one part. Cheap, and a no-op when the text is already
// the same, so a periodic refresh does not repaint four times a second.
bool status_bar_set(HWND hDlg, int control_id, int part, const wchar_t *text);

// Re-place the bar against the current client rectangle. status_bar_attach
// calls this once; a window that can be resized calls it again from WM_SIZE.
void status_bar_resize(HWND hDlg, int control_id);
