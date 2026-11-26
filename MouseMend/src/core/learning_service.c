#include "learning_service.h"
#include <string.h>

#define LEARNING_POLL_MS 100

static DWORD WINAPI learning_thread(LPVOID param)
{
	LearningService *service = (LearningService *)param;

	for (;;)
	{
		if (WaitForSingleObject(service->stop_event, LEARNING_POLL_MS) == WAIT_OBJECT_0)
			break;

		// Recompute as soon as the histogram grows, without waiting for a quiet
		// window. The advice is only a suggestion, so a value that moves while
		// the user is still clicking is harmless - an empty advice that never
		// appears is not.
		uint32_t total = 0;
		for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
			total += signal_ring_samples(service->ring, i);

		// A missing provider means "cannot compute a relative suggestion". The
		// thread stays alive; it simply has nothing to publish. Short-circuiting
		// here is the same guard the old `!service->debounce` check provided.
		if (total == service->last_total || !service->threshold_fn)
			continue;

		service->last_total = total;

		uint32_t thresholds[MOUSE_BUTTON_COUNT];
		for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
			thresholds[i] = service->threshold_fn(i, service->user);

		LearningAdvice fresh;
		learning_advice_from_ring(&fresh, service->ring, thresholds);

		EnterCriticalSection(&service->cs);
		memcpy(&service->advice, &fresh, sizeof(fresh));
		LeaveCriticalSection(&service->cs);
	}

	return 0;
}

bool learning_service_start(LearningService *service, SignalRing *ring,
							LearningThresholdFn threshold_fn, void *user)
{
	if (!service || !ring || service->thread)
		return false;

	// The critical section is initialised once and never deleted. It is not
	// part of the per-start reset on purpose: zeroing it here would turn a
	// previously initialised section into the very trap this fixes.
	if (!service->cs_alive)
	{
		InitializeCriticalSection(&service->cs);
		service->cs_alive = true;
	}

	EnterCriticalSection(&service->cs);
	memset(&service->advice, 0, sizeof(service->advice));
	service->last_total = 0;
	LeaveCriticalSection(&service->cs);

	service->ring = ring;
	service->threshold_fn = threshold_fn;
	service->user = user;

	service->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!service->stop_event)
		return false;

	service->thread = CreateThread(NULL, 0, learning_thread, service, 0, NULL);
	if (!service->thread)
	{
		CloseHandle(service->stop_event);
		service->stop_event = NULL;
		return false;
	}

	SetThreadPriority(service->thread, THREAD_PRIORITY_BELOW_NORMAL);

	// Last: readers use this as "the worker is running".
	service->ready = true;
	return true;
}

void learning_service_stop(LearningService *service)
{
	// Never started (learning is opt-in and off by default): there is nothing
	// to stop, and the critical section may not even exist yet.
	if (!service || !service->ready)
		return;

	if (service->stop_event)
		SetEvent(service->stop_event);

	if (service->thread)
	{
		DWORD waited = WaitForSingleObject(service->thread, 3000);

		// The worker did not report within three seconds. Closing its handles now
		// would leave it reading a closed event and entering a critical section
		// it no longer owns - a use-after-free that only surfaces on a machine
		// that was already hung. Keep both handles and the thread field: a later
		// start() then refuses instead of creating a second worker that writes
		// the same ring.
		if (waited != WAIT_OBJECT_0)
		{
			service->ready = false;
			return;
		}

		CloseHandle(service->thread);
		service->thread = NULL;
	}

	if (service->stop_event)
	{
		CloseHandle(service->stop_event);
		service->stop_event = NULL;
	}

	// The critical section stays valid for the rest of the process (see above),
	// so a reader that raced with this call still enters safely.
	service->ready = false;
}

void learning_service_snapshot(LearningService *service, LearningAdvice *out)
{
	if (!service || !out)
		return;

	if (!service->cs_alive)
	{
		// Learning was never enabled in this run: report "no suggestions"
		// instead of entering a critical section that does not exist.
		memset(out, 0, sizeof(*out));
		return;
	}

	EnterCriticalSection(&service->cs);
	memcpy(out, &service->advice, sizeof(*out));
	LeaveCriticalSection(&service->cs);
}

bool learning_service_is_started(const LearningService *service)
{
	// "ready" first: it is the flag set last on success and cleared first on
	// stop. "cs_alive" alone is not "running" - it stays true forever once the
	// critical section was initialised, including after a failed start.
	return service && service->ready && service->cs_alive;
}

void learning_service_reset(LearningService *service)
{
	if (!service || !service->cs_alive)
		return;

	EnterCriticalSection(&service->cs);
	memset(&service->advice, 0, sizeof(service->advice));
	service->last_total = 0;
	LeaveCriticalSection(&service->cs);
}
