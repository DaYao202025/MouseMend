#include "window_layout.h"

void window_center_on_monitor(HWND window)
{
	RECT bounds;

	if (!window || !GetWindowRect(window, &bounds))
		return;

	MONITORINFO info;
	info.cbSize = sizeof(info);

	HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
	if (!monitor || !GetMonitorInfoW(monitor, &info))
		return;

	int width = bounds.right - bounds.left;
	int height = bounds.bottom - bounds.top;

	int x = info.rcWork.left + ((info.rcWork.right - info.rcWork.left) - width) / 2;
	int y = info.rcWork.top + ((info.rcWork.bottom - info.rcWork.top) - height) / 2;

	SetWindowPos(window, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
