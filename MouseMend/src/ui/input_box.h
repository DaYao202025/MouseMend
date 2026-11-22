#pragma once

#include <windows.h>
#include <stdbool.h>

// Small modal input dialog used for the custom debounce threshold.
// Everything (title, buttons, validation message) is localized internally.

// Show the dialog and wait for the user to confirm or cancel.
// Parameters:
//   hInstance - Module instance that owns the dialog window
//   title - Window title (also used as caption of the validation message)
//   prompt - Text shown above the edit box
//   buffer - In: initial text, Out: text as confirmed by the user
//   buffer_size - Size of buffer in characters
//   min_value - Smallest accepted value
//   max_value - Largest accepted value
//   out_value - Receives the parsed value when the user confirms
// Returns:
//   true when the user confirmed a value inside [min_value, max_value]
bool input_box_show(HINSTANCE hInstance, LPCWSTR title, LPCWSTR prompt,
					LPWSTR buffer, int buffer_size, int min_value, int max_value, int *out_value);

// Unregister the dialog window class (called once at shutdown)
void input_box_unregister(HINSTANCE hInstance);
