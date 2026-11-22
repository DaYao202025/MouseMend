#pragma once

#include <windows.h>
#include <stdbool.h>

// NOTIFYICONDATA::szInfo holds 256 WCHAR including the terminator; anything
// longer is silently cut by the shell, so callers size their buffers with this.
#define TRAY_BALLOON_MAX_CHARS 256

// Tray icon manager
typedef struct
{
	HWND hwnd;
	NOTIFYICONDATA notify_data;
	UINT taskbar_created_msg;
	HINSTANCE hInstance;
	int icon_id;
	bool added;
} TrayIconManager;

// Initialize tray icon manager (loads the icon and registers TaskbarCreated)
bool tray_icon_init(TrayIconManager *manager, HWND hwnd, HINSTANCE hInstance, int icon_id);

// Add icon to system tray
bool tray_icon_add(TrayIconManager *manager);

// Remove icon from system tray and release the icon handle
void tray_icon_remove(TrayIconManager *manager);

// Restore icon after an Explorer restart (reloads the icon if needed)
bool tray_icon_restore(TrayIconManager *manager);

// Update tooltip text
bool tray_icon_set_tooltip(TrayIconManager *manager, const wchar_t *tooltip);

// Show a balloon notification (title and text are truncated to the shell limits)
bool tray_icon_show_balloon(TrayIconManager *manager, const wchar_t *title, const wchar_t *text, DWORD info_flags);
