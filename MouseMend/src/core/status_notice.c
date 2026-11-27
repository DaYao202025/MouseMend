#include "status_notice.h"

#include <strsafe.h>
#include <string.h>
#include <wchar.h>

void status_notice_init(StatusNotice *notice)
{
	if (!notice)
		return;

	// The lock is not valid until this runs, so the structure cannot be used
	// straight out of a memset.
	memset(notice, 0, sizeof(*notice));
	InitializeSRWLock(&notice->lock);
	notice->ready = true;
}

void status_notice_cleanup(StatusNotice *notice)
{
	if (!notice)
		return;

	// SRW locks need no destruction. Clearing ready turns every later call into
	// a no-op, which is what a half torn down application needs.
	AcquireSRWLockExclusive(&notice->lock);
	notice->ready = false;
	notice->text[0] = L'\0';
	notice->until_tick = 0;
	ReleaseSRWLockExclusive(&notice->lock);
}

void status_notice_post(StatusNotice *notice, const wchar_t *text, uint32_t ttl_ms)
{
	uint64_t now;

	if (!notice || !notice->ready)
		return;

	now = GetTickCount64();

	AcquireSRWLockExclusive(&notice->lock);

	if (!text || !text[0])
	{
		// An empty text retracts: the caller found out the message no longer
		// applies and does not want to wait out the timer.
		notice->text[0] = L'\0';
		notice->until_tick = 0;
		ReleaseSRWLockExclusive(&notice->lock);
		return;
	}

	// The same line inside the repeat window is the watchdog talking, not the
	// user. Refreshing it would turn an occasional notice into a permanent one,
	// which is exactly what the balloon in RecoverHook avoids doing.
	if (wcscmp(notice->text, text) == 0 &&
		now - notice->shown_tick < STATUS_NOTICE_REPEAT_MS)
	{
		ReleaseSRWLockExclusive(&notice->lock);
		return;
	}

	StringCchCopyW(notice->text, ARRAYSIZE(notice->text), text);
	notice->shown_tick = now;
	notice->until_tick = now + ttl_ms;

	ReleaseSRWLockExclusive(&notice->lock);
}

bool status_notice_current(StatusNotice *notice, wchar_t *out, size_t count)
{
	uint64_t now;

	if (!notice || !notice->ready || !out || count == 0)
		return false;

	out[0] = L'\0';
	now = GetTickCount64();

	// Exclusive, not shared: an expired message is dropped right here. That is
	// the whole reason the reader needs no timer - the periodic refresh that
	// calls this is also what makes the text disappear.
	AcquireSRWLockExclusive(&notice->lock);

	if (notice->text[0] && now >= notice->until_tick)
	{
		notice->text[0] = L'\0';
		notice->until_tick = 0;
	}

	if (!notice->text[0])
	{
		ReleaseSRWLockExclusive(&notice->lock);
		return false;
	}

	StringCchCopyW(out, count, notice->text);
	ReleaseSRWLockExclusive(&notice->lock);

	return true;
}
