#include "tray_icon.h"
#include "language.h"
#include "../../menu_ids.h"
#include "../../resource.h"
#include <shellapi.h>
#include <strsafe.h>
#include <commctrl.h>

// The shared sizes must match the shell structure
typedef char tray_tooltip_size_check[(ARRAYSIZE(((NOTIFYICONDATA *)0)->szTip) == TRAY_TOOLTIP_MAX_CHARS) ? 1 : -1];
typedef char tray_balloon_size_check[(ARRAYSIZE(((NOTIFYICONDATA *)0)->szInfo) == TRAY_BALLOON_MAX_CHARS) ? 1 : -1];

// Initialize tray icon manager
bool tray_icon_init(TrayIconManager *manager, HWND hwnd, HINSTANCE hInstance, int icon_id)
{
	if (!manager || !hwnd)
		return false;

	manager->hwnd = hwnd;
	manager->hInstance = hInstance;
	manager->icon_id = icon_id;
	manager->added = false;

	// Initialize NOTIFYICONDATA
	memset(&manager->notify_data, 0, sizeof(NOTIFYICONDATA));
	manager->notify_data.cbSize = sizeof(NOTIFYICONDATA);
	manager->notify_data.hWnd = hwnd;
	manager->notify_data.uID = (UINT)icon_id;
	manager->notify_data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
	manager->notify_data.uCallbackMessage = WM_NOTIFYICON;
	manager->notify_data.uVersion = NOTIFYICON_VERSION_4;

	// Load icon
	if (FAILED(LoadIconMetric(hInstance, MAKEINTRESOURCE(icon_id), LIM_SMALL, &manager->notify_data.hIcon)))
	{
		return false;
	}

	// Register taskbar created message (sent after an Explorer restart)
	manager->taskbar_created_msg = RegisterWindowMessage(L"TaskbarCreated");

	return true;
}

// Add icon to system tray
bool tray_icon_add(TrayIconManager *manager)
{
	if (!manager)
		return false;

	if (!manager->notify_data.hIcon)
		return false;

	if (!Shell_NotifyIcon(NIM_ADD, &manager->notify_data))
		return false;

	// NOTIFYICON_VERSION_4 is only honoured through NIM_SETVERSION
	Shell_NotifyIcon(NIM_SETVERSION, &manager->notify_data);

	manager->added = true;
	return true;
}

// Remove icon from system tray
void tray_icon_remove(TrayIconManager *manager)
{
	if (!manager)
		return;

	if (manager->added)
	{
		Shell_NotifyIcon(NIM_DELETE, &manager->notify_data);
		manager->added = false;
	}

	if (manager->notify_data.hIcon)
	{
		DestroyIcon(manager->notify_data.hIcon);
		manager->notify_data.hIcon = NULL;
	}
}

// Restore icon after explorer crash or screen resolution change
// The icon handle is reloaded when missing, otherwise the tray icon would stay
// blank after every Explorer restart.
bool tray_icon_restore(TrayIconManager *manager)
{
	if (!manager)
		return false;

	if (manager->added)
	{
		Shell_NotifyIcon(NIM_DELETE, &manager->notify_data);
		manager->added = false;
	}

	if (!manager->notify_data.hIcon)
	{
		if (FAILED(LoadIconMetric(manager->hInstance, MAKEINTRESOURCE(manager->icon_id), LIM_SMALL, &manager->notify_data.hIcon)))
			return false;
	}

	return tray_icon_add(manager);
}

// Update tooltip text
bool tray_icon_set_tooltip(TrayIconManager *manager, const wchar_t *tooltip)
{
	if (!manager || !tooltip)
		return false;

	StringCchCopy(manager->notify_data.szTip, ARRAYSIZE(manager->notify_data.szTip), tooltip);

	if (manager->added)
		return Shell_NotifyIcon(NIM_MODIFY, &manager->notify_data) != 0;

	return true;
}

// Show a balloon notification
// The sizes passed to StringCchCopy are character counts taken from the struct, so
// an overlong title or text is truncated instead of overflowing the buffers.
bool tray_icon_show_balloon(TrayIconManager *manager, const wchar_t *title, const wchar_t *text, DWORD info_flags)
{
	if (!manager || !text)
		return false;

	// A balloon needs a live icon in the tray
	if (!manager->added)
		return false;

	StringCchCopy(manager->notify_data.szInfoTitle, ARRAYSIZE(manager->notify_data.szInfoTitle), title ? title : L"");
	StringCchCopy(manager->notify_data.szInfo, ARRAYSIZE(manager->notify_data.szInfo), text);
	manager->notify_data.dwInfoFlags = info_flags;
	manager->notify_data.uTimeout = 5000;
	manager->notify_data.uFlags |= NIF_INFO;

	bool shown = Shell_NotifyIcon(NIM_MODIFY, &manager->notify_data) != 0;

	// Do not repeat the balloon on every later NIM_MODIFY call
	manager->notify_data.uFlags &= ~NIF_INFO;

	return shown;
}
