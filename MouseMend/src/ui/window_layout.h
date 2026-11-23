#pragma once

#include <windows.h>

// Place a fixed-size window in the centre of the work area of its monitor.
//
// The work area, not the screen: a window centred on the full monitor would sit
// half behind the taskbar. Both of the application's dialogs are fixed-size, so
// the size is kept and only the position changes.
void window_center_on_monitor(HWND window);

// The arithmetic above, separated from the window so it can be tested.
//
// A window larger than the work area cannot be centred without pushing its title
// bar off screen, and on a small or heavily scaled display that is exactly what
// happens: 851x600 at 200% on a 1080p screen is taller than the desktop. Such a
// window is pinned to the top left instead, so it stays draggable.
//
// work_area - Monitor work area in screen coordinates
// position  - Receives the top left corner
void window_layout_center(int window_width, int window_height,
						  const RECT *work_area, POINT *position);
