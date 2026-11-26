#include "signal_ring.h"

void signal_ring_init(SignalRing *ring)
{
	if (!ring)
		return;

	for (int button = 0; button < MOUSE_BUTTON_COUNT; button++)
		signal_ring_reset(ring, button);
}

uint32_t signal_ring_bucket(uint64_t gap_ms)
{
	if (gap_ms == 0)
		return 1;

	if (gap_ms > SIGNAL_RING_BUCKET_COUNT)
		return SIGNAL_RING_BUCKET_COUNT;

	return (uint32_t)gap_ms;
}

void signal_ring_note_interval(SignalRing *ring, int button, uint64_t elapsed_ms)
{
	if (!ring || button < 0 || button >= MOUSE_BUTTON_COUNT)
		return;

	// The caller decides what is worth sampling; clamping here only guarantees
	// the index stays inside the histogram.
	uint32_t bucket = signal_ring_bucket(elapsed_ms);

	// Bucket first, count second. The reader trusts the count: once it sees N it
	// walks the buckets looking for the P80 bucket, so the Nth bucket has to be
	// on record before the count exposes it. The opposite order leaves a window
	// where the count says 33 while only 32 buckets exist, the walk never
	// reaches the percentile target, and the advice silently becomes zero - the
	// "33 samples but still no suggestion" state.
	InterlockedIncrement(&ring->buckets[button][bucket - 1]);
	InterlockedIncrement(&ring->samples[button]);
}

uint32_t signal_ring_samples(const SignalRing *ring, int button)
{
	if (!ring || button < 0 || button >= MOUSE_BUTTON_COUNT)
		return 0;

	// Interlocked does not write here (exchange and comparand are both zero),
	// the cast only drops const for the API signature.
	return (uint32_t)InterlockedCompareExchange((volatile LONG *)&ring->samples[button], 0, 0);
}

uint32_t signal_ring_bucket_value(const SignalRing *ring, int button, uint32_t bucket)
{
	if (!ring || button < 0 || button >= MOUSE_BUTTON_COUNT ||
		bucket < 1 || bucket > SIGNAL_RING_BUCKET_COUNT)
		return 0;

	return (uint32_t)InterlockedCompareExchange(
		(volatile LONG *)&ring->buckets[button][bucket - 1], 0, 0);
}

void signal_ring_reset(SignalRing *ring, int button)
{
	if (!ring || button < 0 || button >= MOUSE_BUTTON_COUNT)
		return;

	InterlockedExchange(&ring->samples[button], 0);

	for (uint32_t i = 0; i < SIGNAL_RING_BUCKET_COUNT; i++)
		InterlockedExchange(&ring->buckets[button][i], 0);
}
