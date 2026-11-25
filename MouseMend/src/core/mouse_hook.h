#pragma once

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

// Mouse button types
// The order is part of the persisted settings layout (Btn<N>_* registry values)
// and of the tray menu ID encoding, so do not reorder existing entries.
typedef enum
{
	MOUSE_BUTTON_UNKNOWN = -1,
	MOUSE_BUTTON_LEFT = 0,
	MOUSE_BUTTON_RIGHT,
	MOUSE_BUTTON_MIDDLE,
	MOUSE_BUTTON_X1,
	MOUSE_BUTTON_X2,
	MOUSE_BUTTON_WHEEL,
	MOUSE_BUTTON_HWHEEL,
	MOUSE_BUTTON_COUNT
} MouseButton;

// Mouse event structure
typedef struct
{
	MouseButton button;
	uint64_t timestamp;
	bool is_down;
	long x;
	long y;
	bool is_injected;
	int32_t data;
} MouseEvent;

// Mouse hook callback function type
typedef LRESULT(CALLBACK *MouseHookCallback)(const MouseEvent *event, void *user_data);

// Mouse hook manager
typedef struct
{
	HHOOK hook;
	MouseHookCallback callback;
	void *user_data;
	bool installed;

	// Every event the hook delivered, including mouse movement.
	// The health watchdog compares this with cursor movement: if the cursor
	// moves but this counter does not, the hook is no longer on the chain.
	volatile LONG event_count;
} MouseHookManager;

// Initialize mouse hook manager
bool mouse_hook_init(MouseHookManager *manager, MouseHookCallback callback, void *user_data);

// Install mouse hook
bool mouse_hook_install(MouseHookManager *manager);

// Uninstall mouse hook
void mouse_hook_uninstall(MouseHookManager *manager);

// Get button from WPARAM and MSLLHOOKSTRUCT
MouseButton mouse_hook_get_button(WPARAM wParam, PMSLLHOOKSTRUCT pdata);

// Check if button is pressed
bool mouse_hook_is_button_down(WPARAM wParam);

// Number of events the hook has delivered, movement included
LONG mouse_hook_event_count(const MouseHookManager *manager);

// Check whether a low-level mouse hook can be installed at all
// Uses its own local hook and never touches the manager, so it is safe to call
// from a process that is already filtering input.
bool mouse_hook_probe(void);

// Whether the calling thread is currently inside the hook callback.
//
// Injecting input from inside a low-level hook deadlocks: the injected event has
// to travel the hook chain again, and the chain head is the very thread that is
// already waiting inside SendInput. Windows only removes the hook after the
// timeout, so the rest of the system recovers while the blocked thread never
// does - the process can then only be removed by signing out.
//
// Every injection therefore asks this first. It is deliberately not an assert:
// refusing costs one lost release, deadlocking costs a reboot.
bool mouse_hook_in_callback(void);
