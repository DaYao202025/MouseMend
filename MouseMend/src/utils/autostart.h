#pragma once

#include <stdbool.h>
#include <windows.h>

// Check whether MouseMend is registered to start with Windows
bool autostart_is_enabled(void);

// Enable or disable starting with Windows
// Returns:
//   ERROR_SUCCESS on success, otherwise a Win32 error code
LONG autostart_set_enabled(bool enable);
