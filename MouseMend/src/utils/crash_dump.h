#pragma once

#include <stdbool.h>

// Minidump capture for the crash Windows would otherwise swallow.
//
// A bug report that only says "fault offset 0x17975" cannot be turned into a
// fix without a stack, and the offset changes with every build. One dump per
// crash is enough to name the function that violated the access.
//
// The filter deliberately avoids the CRT: a corrupted heap is exactly the
// situation it has to survive, so it only calls Win32.

// Install the filter. Call once, early in WinMain.
// The directory is resolved lazily, on the crash itself: a user who never
// crashes does not get an empty Log folder next to the executable.
//
// The dump is written next to the executable's Log folder, or under
// %LOCALAPPDATA%\MouseMend\Log when that folder is read-only. Its name carries
// the local time of the crash, so a support case only has to say "it happened
// at 23:18" to identify the file.
void crash_dump_install(void);
