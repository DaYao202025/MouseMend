#pragma once

#include <windows.h>

// Place a fixed-size window in the centre of the work area of its monitor.
//
// The work area, not the screen: a window centred on the full monitor would sit
// half behind the taskbar. Both of the application's dialogs are fixed-size, so
// the size is kept and only the position changes.
void window_center_on_monitor(HWND window);
