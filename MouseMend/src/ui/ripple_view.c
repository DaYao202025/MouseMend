#include "ripple_view.h"
#include <string.h>

#pragma comment(lib, "gdi32.lib")

// Ring colour of a button
COLORREF ripple_view_color(MouseButton button)
{
	switch (button)
	{
	case MOUSE_BUTTON_LEFT:
		return RGB(0x15, 0x65, 0xC0); // Blue
	case MOUSE_BUTTON_MIDDLE:
		return RGB(0x2E, 0x7D, 0x32); // Green
	case MOUSE_BUTTON_RIGHT:
		return RGB(0xC6, 0x28, 0x28); // Red
	default:
		return RGB(0x55, 0x55, 0x55);
	}
}

// Start from a clean slate
void ripple_view_reset(RippleView *view)
{
	if (!view)
		return;

	memset(view, 0, sizeof(*view));
}

// Start a ring for a press
void ripple_view_trigger(RippleView *view, MouseButton button, int x, int y, uint64_t now_tick)
{
	if (!view)
		return;

	// Reuse the oldest slot once every slot is taken: a burst of clicks then
	// keeps the three most recent ones instead of dropping the newest.
	int slot = -1;

	for (int i = 0; i < RIPPLE_MAX; i++)
	{
		if (!view->ripples[i].active)
		{
			slot = i;
			break;
		}
	}

	if (slot < 0)
	{
		slot = 0;

		for (int i = 1; i < RIPPLE_MAX; i++)
		{
			if (view->ripples[i].start_tick < view->ripples[slot].start_tick)
				slot = i;
		}
	}

	view->ripples[slot].button = button;
	view->ripples[slot].x = x;
	view->ripples[slot].y = y;
	view->ripples[slot].start_tick = now_tick;
	view->ripples[slot].active = true;
	view->animating = true;
}

// Age the rings
bool ripple_view_tick(RippleView *view, uint64_t now_tick)
{
	if (!view)
		return false;

	bool active = false;

	for (int i = 0; i < RIPPLE_MAX; i++)
	{
		Ripple *ripple = &view->ripples[i];

		if (!ripple->active)
			continue;

		uint64_t age = (now_tick >= ripple->start_tick) ? (now_tick - ripple->start_tick) : 0;

		if (age >= RIPPLE_LIFETIME_MS)
			ripple->active = false;
		else
			active = true;
	}

	// The first idle tick still needs a repaint so the last ring disappears
	bool repaint = active || view->animating;
	view->animating = active;

	return repaint;
}

// Number of rings currently alive
int ripple_view_active_count(const RippleView *view)
{
	if (!view)
		return 0;

	int count = 0;

	for (int i = 0; i < RIPPLE_MAX; i++)
	{
		if (view->ripples[i].active)
			count++;
	}

	return count;
}

// Blend a colour towards white; percent 0 keeps it, 100 turns it white
static COLORREF fade_to_white(COLORREF color, int percent)
{
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;

	int red = GetRValue(color) + (255 - GetRValue(color)) * percent / 100;
	int green = GetGValue(color) + (255 - GetGValue(color)) * percent / 100;
	int blue = GetBValue(color) + (255 - GetBValue(color)) * percent / 100;

	return RGB(red, green, blue);
}

// Draw a ring of the given colour and width
static void draw_ring(HDC dc, int center_x, int center_y, int radius, int width, COLORREF color)
{
	if (radius <= 0)
		return;

	HPEN pen = CreatePen(PS_SOLID, width, color);
	if (!pen)
		return;

	HGDIOBJ old_pen = SelectObject(dc, pen);
	HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));

	Ellipse(dc, center_x - radius, center_y - radius, center_x + radius, center_y + radius);

	SelectObject(dc, old_brush);
	SelectObject(dc, old_pen);
	DeleteObject(pen);
}

// Draw a filled dot, used for the moment right after the press
static void draw_dot(HDC dc, int center_x, int center_y, int radius, COLORREF color)
{
	HBRUSH brush = CreateSolidBrush(color);
	if (!brush)
		return;

	HGDIOBJ old_brush = SelectObject(dc, brush);
	HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));

	Ellipse(dc, center_x - radius, center_y - radius, center_x + radius, center_y + radius);

	SelectObject(dc, old_pen);
	SelectObject(dc, old_brush);
	DeleteObject(brush);
}

// Draw the whole area
void ripple_view_draw(const RippleView *view, const DRAWITEMSTRUCT *item, uint64_t now_tick,
					  const wchar_t *hint)
{
	if (!view || !item)
		return;

	HDC dc = item->hDC;
	RECT bounds = item->rcItem;

	FillRect(dc, &bounds, (HBRUSH)(COLOR_WINDOW + 1));

	int width = bounds.right - bounds.left;
	int height = bounds.bottom - bounds.top;
	int max_radius = ((width < height) ? width : height) / 2 - 6;

	// A ring marks one click, it does not fill the room. Without the cap a large
	// test area turns every press into a circle the size of the pane, which reads
	// as a decoration instead of feedback.
	if (max_radius > RIPPLE_MAX_RADIUS)
		max_radius = RIPPLE_MAX_RADIUS;

	if (max_radius < 12)
		max_radius = 12;

	bool any = false;

	for (int i = 0; i < RIPPLE_MAX; i++)
	{
		const Ripple *ripple = &view->ripples[i];

		if (!ripple->active)
			continue;

		uint64_t age = (now_tick >= ripple->start_tick) ? (now_tick - ripple->start_tick) : 0;
		int percent = (int)((age * 100) / RIPPLE_LIFETIME_MS);

		if (percent > 100)
			percent = 100;

		COLORREF base = ripple_view_color(ripple->button);
		int radius = 4 + (max_radius - 4) * percent / 100;

		// Two rings so the expansion reads as motion instead of a thin line
		draw_ring(dc, ripple->x, ripple->y, radius, 3, fade_to_white(base, percent));
		draw_ring(dc, ripple->x, ripple->y, radius * 7 / 10, 1, fade_to_white(base, percent + 30));

		if (age < 120)
			draw_dot(dc, ripple->x, ripple->y, 3, base);

		any = true;
	}

	if (!any && hint && hint[0] != L'\0')
	{
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));

		// The hint is the only instruction in the window, so it gets room for two
		// lines and is centred vertically: measure first, then draw.
		RECT text_bounds = bounds;
		InflateRect(&text_bounds, -12, -12);
		DrawTextW(dc, hint, -1, &text_bounds, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);

		int text_height = text_bounds.bottom - text_bounds.top;
		text_bounds = bounds;
		text_bounds.top = (bounds.top + bounds.bottom - text_height) / 2;
		text_bounds.bottom = text_bounds.top + text_height;

		DrawTextW(dc, hint, -1, &text_bounds, DT_CENTER | DT_WORDBREAK);
	}

	FrameRect(dc, &bounds, GetSysColorBrush(COLOR_BTNSHADOW));
}
