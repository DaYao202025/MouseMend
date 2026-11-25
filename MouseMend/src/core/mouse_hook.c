#include "mouse_hook.h"
#include <stdint.h>
#include <windows.h>

// Install / uninstall and the hook callback always run on the same (UI) thread,
// because low-level hooks are dispatched to the thread that installed them.
static MouseHookManager *g_manager = NULL;

// How deep the current thread is inside the callback. Non-zero means an
// injection from this stack would wait on itself - see mouse_hook_in_callback.
static volatile LONG g_callback_depth = 0;

// Low-level mouse hook callback
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	MouseHookManager *manager = g_manager;
	LRESULT outcome = 0;
	bool handled = false;

	InterlockedIncrement(&g_callback_depth);

	if (nCode == HC_ACTION && manager)
	{
		// Count every event, movement included, so the watchdog can tell a live
		// hook from a removed one even when the user only moves the mouse.
		InterlockedIncrement(&manager->event_count);

		if (wParam != WM_MOUSEMOVE && manager->callback)
		{
			PMSLLHOOKSTRUCT pdata = (PMSLLHOOKSTRUCT)lParam;
			MouseButton button = mouse_hook_get_button(wParam, pdata);

			// Both ends are checked: the callback indexes per-button arrays with
			// this value, so an out of range button would corrupt memory silently.
			if (button >= 0 && button < MOUSE_BUTTON_COUNT)
			{
				MouseEvent event;
				event.button = button;
				// GetTickCount64 is used instead of MSLLHOOKSTRUCT.time to stay
				// immune to the 49.7 day wrap-around of the 32-bit tick counter.
				event.timestamp = GetTickCount64();
				event.data = 0;

				// For wheel events, store the signed delta in the data field
				if (wParam == WM_MOUSEWHEEL)
					event.data = (int32_t)GET_WHEEL_DELTA_WPARAM(pdata->mouseData);
				else if (wParam == WM_MOUSEHWHEEL)
					event.data = (int32_t)GET_WHEEL_DELTA_WPARAM(pdata->mouseData);

				event.is_down = mouse_hook_is_button_down(wParam);
				event.x = pdata->pt.x;
				event.y = pdata->pt.y;
				event.is_injected = (pdata->flags & LLMHF_INJECTED) || (pdata->flags & LLMHF_LOWER_IL_INJECTED);

				LRESULT result = manager->callback(&event, manager->user_data);
				if (result != 0)
				{
					outcome = result;
					handled = true;
				}
			}
		}
	}

	if (!handled)
		outcome = CallNextHookEx(manager ? manager->hook : NULL, nCode, wParam, lParam);

	// Both exits pass through here, so the depth can never be left behind by an
	// early return: a stale depth would silently disable every later injection.
	InterlockedDecrement(&g_callback_depth);

	return outcome;
}

bool mouse_hook_in_callback(void)
{
	return g_callback_depth != 0;
}

// Initialize mouse hook manager
bool mouse_hook_init(MouseHookManager *manager, MouseHookCallback callback, void *user_data)
{
	if (!manager || !callback)
		return false;

	manager->hook = NULL;
	manager->callback = callback;
	manager->user_data = user_data;
	manager->installed = false;

	// Reset the counter so a reinstall starts from zero instead of carrying the
	// previous hook's totals into the health watchdog
	InterlockedExchange(&manager->event_count, 0);

	g_manager = manager;

	return true;
}

// Install mouse hook
bool mouse_hook_install(MouseHookManager *manager)
{
	if (!manager)
		return false;

	manager->hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(NULL), 0);
	if (!manager->hook)
		return false;

	manager->installed = true;
	return true;
}

// Uninstall mouse hook
void mouse_hook_uninstall(MouseHookManager *manager)
{
	if (!manager)
		return;

	if (manager->hook)
	{
		UnhookWindowsHookEx(manager->hook);
		manager->hook = NULL;
	}

	manager->installed = false;
	g_manager = NULL;
}

// Get button from WPARAM and MSLLHOOKSTRUCT
MouseButton mouse_hook_get_button(WPARAM wParam, PMSLLHOOKSTRUCT pdata)
{
	if (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP)
		return MOUSE_BUTTON_LEFT;
	if (wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP)
		return MOUSE_BUTTON_RIGHT;
	if (wParam == WM_MBUTTONDOWN || wParam == WM_MBUTTONUP)
		return MOUSE_BUTTON_MIDDLE;
	if (wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP)
		return HIWORD(pdata->mouseData) == XBUTTON1 ? MOUSE_BUTTON_X1 : MOUSE_BUTTON_X2;
	if (wParam == WM_MOUSEWHEEL)
		return MOUSE_BUTTON_WHEEL;
	if (wParam == WM_MOUSEHWHEEL)
		return MOUSE_BUTTON_HWHEEL;

	return MOUSE_BUTTON_UNKNOWN;
}

// Check if button is pressed
bool mouse_hook_is_button_down(WPARAM wParam)
{
	return (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
			wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN);
}

// Number of events the hook has delivered, movement included
LONG mouse_hook_event_count(const MouseHookManager *manager)
{
	return manager ? manager->event_count : 0;
}

// Pass-through callback for the probe below
static LRESULT CALLBACK ProbeProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Check whether a low-level mouse hook can be installed at all
// Deliberately does not go through mouse_hook_init/install: those set a module
// level pointer, so probing from inside a running instance would replace the
// live manager and silently disable the real hook.
bool mouse_hook_probe(void)
{
	HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, ProbeProc, GetModuleHandleW(NULL), 0);

	if (!hook)
		return false;

	UnhookWindowsHookEx(hook);
	return true;
}
