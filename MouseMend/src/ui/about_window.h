#pragma once

#include <windows.h>
#include <stdbool.h>

// The About window.
//
// It exists because the tray menu used to offer an "About" submenu that a user
// could click without anything happening: a popup menu title is not a command.
// A plain menu item that opens a real window is the behaviour people expect,
// and the window is where the project can say what the program does and, just
// as important, what it does not do.

// Show the window and wait until it is closed
// Parameters:
//   owner - Window the dialog is centered over; may be NULL
//   hInstance - Instance that owns IDD_ABOUT
// Returns:
//   true when the dialog could be created
//
// The dialog is modal, so it runs its own message loop. That loop still
// dispatches the tray icon messages, which means the user can ask the program to
// quit while this window is up - which is why about_window_close exists.
bool about_window_show(HWND owner, HINSTANCE hInstance);

// Close the window if it is open
//
// Called on shutdown: a modal dialog whose owner is being destroyed would keep
// the process alive until the user clicks it away.
void about_window_close(void);
