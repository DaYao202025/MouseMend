#include "window_layout.h"

void window_layout_center(int window_width, int window_height,
						  const RECT *work_area, POINT *position)
{
	if (!work_area || !position)
		return;

	int work_width = work_area->right - work_area->left;
	int work_height = work_area->bottom - work_area->top;

	// Centred when there is room; pinned to the top left when there is not. A
	// negative offset would put the title bar above the screen, where it cannot
	// be grabbed and the window cannot be moved at all.
	int x = (window_width >= work_width)
				? work_area->left
				: work_area->left + (work_width - window_width) / 2;

	int y = (window_height >= work_height)
				? work_area->top
				: work_area->top + (work_height - window_height) / 2;

	position->x = x;
	position->y = y;
}

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

	POINT position;
	window_layout_center(bounds.right - bounds.left, bounds.bottom - bounds.top,
						 &info.rcWork, &position);

	SetWindowPos(window, NULL, position.x, position.y, 0, 0,
				 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
