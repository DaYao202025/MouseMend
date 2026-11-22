#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>
#include "../core/mouse_hook.h"

// Expanding ring animation for the click test area.
//
// Every press paints a ring that grows and fades out. The rings are driven by
// the events the hook delivered, not by window messages: a click that the engine
// blocks never reaches the window, and that is exactly the click a user wants to
// see confirmed.

#define RIPPLE_MAX 3
#define RIPPLE_LIFETIME_MS 600

// Largest radius a ring may grow to, in pixels
#define RIPPLE_MAX_RADIUS 68

typedef struct
{
	MouseButton button;
	int x;
	int y;
	uint64_t start_tick;
	bool active;
} Ripple;

typedef struct
{
	Ripple ripples[RIPPLE_MAX];
	bool animating; // Something was animating on the previous tick
} RippleView;

// Start from a clean slate
void ripple_view_reset(RippleView *view);

// Start a ring for a press at a client coordinate of the test area
void ripple_view_trigger(RippleView *view, MouseButton button, int x, int y, uint64_t now_tick);

// Age the rings.
// Returns:
//   true when the area still needs to be repainted, including the frame that
//   erases the last ring
bool ripple_view_tick(RippleView *view, uint64_t now_tick);

// Number of rings currently alive
int ripple_view_active_count(const RippleView *view);

// Draw the area (used from WM_DRAWITEM)
// Parameters:
//   hint - Idle text, drawn only while no ring is alive; may be NULL
void ripple_view_draw(const RippleView *view, const DRAWITEMSTRUCT *item, uint64_t now_tick,
					  const wchar_t *hint);

// Ring colour of a button; the three main buttons are clearly distinguishable
COLORREF ripple_view_color(MouseButton button);
