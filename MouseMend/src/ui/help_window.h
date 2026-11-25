#pragma once

#include <windows.h>
#include <stdbool.h>

// The two explanations the check-up window cannot fit inline.
//
// Both are long lists, so they live in a read-only edit inside one shared
// dialog: a static control would be cut off at the first long paragraph, and a
// user who wants to quote a line should be able to select it.
typedef enum
{
	HELP_TOPIC_VERDICTS = 0, // What each value in the verdict column means
	HELP_TOPIC_VERSIONS		 // What each published release fixed
} HelpTopic;

// Show the help window and wait until it is closed
bool help_window_show(HWND owner, HINSTANCE instance, HelpTopic topic);

// Close it if it is up, used on shutdown
void help_window_close(void);
