#include "ripple_view.h"
#include "language.h"
#include <string.h>

#pragma comment(lib, "gdi32.lib")

// Ring colour of a button
//
// Seven colours is more than anyone can name, so the colour is a second cue and
// the mark is the first: the side buttons get their own hues only so a ring from
// one of them is not mistaken for a main button at a glance. The two wheels share
// the middle button's green - it is the same physical control - and are told
// apart by their up/down mark.
COLORREF ripple_view_color(MouseButton button)
{
	switch (button)
	{
	case MOUSE_BUTTON_LEFT:
		return RGB(0x15, 0x65, 0xC0); // Blue
	case MOUSE_BUTTON_MIDDLE:
	case MOUSE_BUTTON_WHEEL:
	case MOUSE_BUTTON_HWHEEL:
		return RGB(0x2E, 0x7D, 0x32); // Green
	case MOUSE_BUTTON_RIGHT:
		return RGB(0xC6, 0x28, 0x28); // Red
	case MOUSE_BUTTON_X1:
		return RGB(0x00, 0x83, 0x8F); // Teal
	case MOUSE_BUTTON_X2:
		return RGB(0x6A, 0x1B, 0x9A); // Purple
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

// Start a ring for a press or a wheel notch
void ripple_view_trigger(RippleView *view, MouseButton button, int32_t data, int x, int y,
						 uint64_t now_tick)
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
	view->ripples[slot].data = data;
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

// Blend a colour towards the canvas background; percent 0 keeps it, 100 makes it
// invisible.
//
// It used to blend towards pure white, which is the same thing only on a light
// theme: under a high contrast theme the pane is black or dark, so the last
// third of the ring's life was drawn in the background colour by accident and
// the ring appeared to vanish instead of fading.
static COLORREF fade_to_background(COLORREF color, int percent)
{
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;

	COLORREF background = GetSysColor(COLOR_WINDOW);

	int red = GetRValue(color) + (GetRValue(background) - GetRValue(color)) * percent / 100;
	int green = GetGValue(color) + (GetGValue(background) - GetGValue(color)) * percent / 100;
	int blue = GetBValue(color) + (GetBValue(background) - GetBValue(color)) * percent / 100;

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

// Draw the button mark centred in the ring
//
// One glyph is square, so it sits exactly in the middle of a circle. It carries
// the same information as the colour, which on its own is no use to a user who
// cannot tell red from green.
static void draw_mark(HDC dc, HFONT font, int center_x, int center_y,
					  const wchar_t *mark, COLORREF color)
{
	if (!mark || !mark[0])
		return;

	HGDIOBJ old_font = font ? SelectObject(dc, font) : NULL;

	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, color);

	// Wide enough for a CJK glyph at the largest size the dialog font reaches
	RECT box = {center_x - 24, center_y - 14, center_x + 24, center_y + 14};

	DrawTextW(dc, mark, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

	if (old_font)
		SelectObject(dc, old_font);
}

// One ring, aged to now
static void draw_ripple(HDC dc, HFONT font, const Ripple *ripple, uint64_t now_tick, int max_radius)
{
	uint64_t age = (now_tick >= ripple->start_tick) ? (now_tick - ripple->start_tick) : 0;

	if (age >= RIPPLE_LIFETIME_MS)
		return;

	// Permille: the life is short, and whole percents make the first frames jump
	int t = (int)(age * 1000 / RIPPLE_LIFETIME_MS);

	// Ease out, most of the distance in the first third. Linear growth is what
	// made the old ring feel mechanical - it read as a line being dragged open.
	int ease = 1000 - ((1000 - t) * (1000 - t)) / 1000;

	COLORREF base = ripple_view_color(ripple->button);
	int radius = RIPPLE_START_RADIUS + (max_radius - RIPPLE_START_RADIUS) * ease / 1000;
	int width = RIPPLE_START_WIDTH - (RIPPLE_START_WIDTH - RIPPLE_END_WIDTH) * ease / 1000;

	if (width < 1)
		width = 1;

	// The fade is eased in, not linear: a ring that keeps its colour while it
	// travels and only dissolves near the end reads as a wave. Fading at a
	// constant rate made it look like it was running out of ink from the start.
	//
	// t is permille, the blend wants a percentage, and the square has to land on
	// 100 at the end: 1000 * 1000 / 10000.
	int fade = t * t / 10000;

	draw_ring(dc, ripple->x, ripple->y, radius, width, fade_to_background(base, fade));

	// The mark fades in over the first frames and then leaves with the ring, so
	// the two read as one gesture rather than a label stuck on top of an effect
	int appear_end = RIPPLE_MARK_FADE_MS * 1000 / RIPPLE_LIFETIME_MS;
	int appear = 100;

	if (appear_end > 0 && t < appear_end)
		appear = t * 100 / appear_end;

	// A wheel has no mark of its own: the useful thing to say is which way this
	// notch went, and that lives in the event's delta, not in its button.
	const wchar_t *mark = lang_button_mark(ripple->button);

	if (!mark[0])
		mark = lang_scroll_mark(ripple->data);

	draw_mark(dc, font, ripple->x, ripple->y, mark,
			  fade_to_background(base, 100 - ((100 - fade) * appear) / 100));
}

// One reusable bitmap.
//
// The pane is repainted sixty times a second while a ring is alive. Drawing
// straight onto the window means the background fill and the ring are two
// separate writes to the screen, and the gap between them is visible as a
// shimmer. A single bitmap, kept until the pane changes size, costs one blit and
// removes it. One window draws with this view, so one buffer is enough.
static struct
{
	HDC dc;
	HBITMAP bitmap;
	HGDIOBJ old_bitmap;
	int width;
	int height;
} g_off_screen;

static void release_off_screen(void)
{
	if (!g_off_screen.dc)
		return;

	if (g_off_screen.old_bitmap)
		SelectObject(g_off_screen.dc, g_off_screen.old_bitmap);

	if (g_off_screen.bitmap)
		DeleteObject(g_off_screen.bitmap);

	DeleteDC(g_off_screen.dc);

	memset(&g_off_screen, 0, sizeof(g_off_screen));
}

// Give up the buffer when the window goes away
void ripple_view_release(void)
{
	release_off_screen();
}

// HDC to draw into, or NULL when the buffer cannot be made
static HDC acquire_off_screen(HDC target, int width, int height)
{
	if (width <= 0 || height <= 0)
		return NULL;

	if (g_off_screen.dc && g_off_screen.width == width && g_off_screen.height == height)
		return g_off_screen.dc;

	release_off_screen();

	HDC dc = CreateCompatibleDC(target);

	if (!dc)
		return NULL;

	HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);

	if (!bitmap)
	{
		DeleteDC(dc);
		return NULL;
	}

	g_off_screen.old_bitmap = SelectObject(dc, bitmap);
	g_off_screen.dc = dc;
	g_off_screen.bitmap = bitmap;
	g_off_screen.width = width;
	g_off_screen.height = height;

	return dc;
}

// Draw the whole area
void ripple_view_draw(const RippleView *view, const DRAWITEMSTRUCT *item, uint64_t now_tick,
					  const wchar_t *hint)
{
	if (!view || !item)
		return;

	RECT bounds = item->rcItem;
	int width = bounds.right - bounds.left;
	int height = bounds.bottom - bounds.top;

	HDC target = item->hDC;
	HDC dc = acquire_off_screen(target, width, height);
	bool buffered = (dc != NULL);

	if (buffered)
	{
		// The caller's coordinates are the control's; the bitmap starts at zero
		SetViewportOrgEx(dc, -bounds.left, -bounds.top, NULL);
	}
	else
	{
		// No buffer: draw straight onto the window rather than draw nothing
		dc = target;
	}

	FillRect(dc, &bounds, (HBRUSH)(COLOR_WINDOW + 1));

	int max_radius = ((width < height) ? width : height) / 2 - 6;

	// A ring marks one click, it does not fill the room. Without the cap a large
	// test area turns every press into a circle the size of the pane, which reads
	// as a decoration instead of feedback.
	if (max_radius > RIPPLE_MAX_RADIUS)
		max_radius = RIPPLE_MAX_RADIUS;

	if (max_radius < 12)
		max_radius = 12;

	bool any = false;

	// The marks are drawn in the control font, same as every label around the
	// pane, so they belong to the window rather than to the effect
	HFONT control_font = (HFONT)SendMessageW(item->hwndItem, WM_GETFONT, 0, 0);

	for (int i = 0; i < RIPPLE_MAX; i++)
	{
		const Ripple *ripple = &view->ripples[i];

		if (!ripple->active)
			continue;

		draw_ripple(dc, control_font, ripple, now_tick, max_radius);
		any = true;
	}

	if (!any && hint && hint[0] != L'\0')
	{
		// Drawn in the control's own font. An owner draw static gets the stock
		// system font from the DC, which is a different typeface from every label
		// around it, and the hint is the one line a new user actually reads.
		HGDIOBJ old_font = control_font ? SelectObject(dc, control_font) : NULL;

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

		if (old_font)
			SelectObject(dc, old_font);
	}

	FrameRect(dc, &bounds, GetSysColorBrush(COLOR_BTNSHADOW));

	if (buffered)
	{
		SetViewportOrgEx(dc, 0, 0, NULL);
		BitBlt(target, bounds.left, bounds.top, width, height, dc, 0, 0, SRCCOPY);
	}
}
