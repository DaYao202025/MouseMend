#pragma once

#include <stdbool.h>
#include <windows.h>

// Read-only self check used by support and by deployment scripts.
// Prints a fixed set of "key=value" lines and never changes the user profile.
// Returns:
//   process exit code: 0 when everything looks healthy, 1 otherwise

// Print the version banner (--version)
int diagnostics_print_version(void);

// Print the full diagnostic report (--diagnose)
int diagnostics_run(void);

// Build the same report into a buffer, for the settings window
// Returns:
//   true when everything looks healthy
bool diagnostics_build(wchar_t *buffer, size_t buffer_count);
