#pragma once

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

// One short lived message for the status bar at the bottom of a window.
//
// The alternative - every action posting straight to a control - would mean the
// action site knows which window is open, whether it is visible, and how long
// the text should stay. This does not: a caller says what happened, and any
// window that is open picks it up on its next refresh.
//
// Only the most recent message is kept. A queue was considered and rejected:
// these are confirmations ("already saved", "hook restored"), and a user who
// performs three actions in a row wants to see the last one, not three
// messages fading in sequence.

#define STATUS_NOTICE_CHARS 128

// A message identical to the one on screen is ignored within this window. The
// hook watchdog can recover every 30 seconds while the user works in an
// elevated program; without this the same line would blink back forever.
#define STATUS_NOTICE_REPEAT_MS 30000u

typedef struct
{
	wchar_t text[STATUS_NOTICE_CHARS];
	uint64_t until_tick; // GetTickCount64 value when the message stops showing
	uint64_t shown_tick; // when it was posted, for repeat suppression
	SRWLOCK lock;
	bool ready;
} StatusNotice;

// Safe on a zero-initialised structure, but must be called once: the lock is
// not valid until it is.
void status_notice_init(StatusNotice *notice);
void status_notice_cleanup(StatusNotice *notice);

// Publish a message for ttl_ms milliseconds.
//
// An empty text clears the slot, which is how a caller retracts a message early.
//
// ttl_ms = 0 is not "forever": the deadline becomes the current tick, so the
// next read drops the message. Use a real duration; the only caller that passes
// zero is the retraction above, where the text is empty anyway.
void status_notice_post(StatusNotice *notice, const wchar_t *text, uint32_t ttl_ms);

// Copy the current message, and return false when there is none.
//
// Expiry is checked against the same clock the poster used, and an expired
// message is dropped as a side effect: a caller polling every 400 ms therefore
// needs no timer of its own to make the text disappear.
bool status_notice_current(StatusNotice *notice, wchar_t *out, size_t count);
