#include "learning.h"
#include "config.h"

static uint32_t clamp_u32(uint32_t value, uint32_t low, uint32_t high)
{
	if (value < low)
		return low;
	if (value > high)
		return high;
	return value;
}

uint32_t learning_suggest_threshold(const SignalRing *ring, int button,
									uint32_t current_threshold, uint32_t *out_samples)
{
	uint32_t samples = signal_ring_samples(ring, button);
	if (out_samples)
		*out_samples = samples;

	if (samples < LEARNING_MIN_SAMPLES)
		return 0;

	// One outlier out of 32 must not decide the result. P80 with 32 samples
	// needs 26 hits, so a single 64 ms bucket cannot move the answer.
	uint32_t target = (samples * LEARNING_PERCENTILE + 99u) / 100u;

	uint32_t cumulative = 0;
	uint32_t raw = 0;

	for (uint32_t bucket = 1; bucket <= SIGNAL_RING_BUCKET_COUNT; bucket++)
	{
		cumulative += signal_ring_bucket_value(ring, button, bucket);
		if (cumulative >= target)
		{
			raw = bucket + 1u; // just above the observed chatter interval
			break;
		}
	}

	if (raw == 0)
		return 0;

	if (current_threshold == 0)
		current_threshold = THRESHOLD_MIN_VALUE;

	uint32_t low = (current_threshold / 2u < THRESHOLD_MIN_VALUE)
					   ? THRESHOLD_MIN_VALUE
					   : current_threshold / 2u;
	uint32_t high = current_threshold + current_threshold / 2u;
	if (high > THRESHOLD_MAX_VALUE)
		high = THRESHOLD_MAX_VALUE;
	if (high < low)
		high = low;

	uint32_t suggestion = clamp_u32(raw, low, high);

	// A suggestion equal to the threshold already in force is not a suggestion.
	// Adopting it would change nothing while still wiping the samples and
	// reporting success, which reads as "I clicked adopt and nothing happened".
	// The clamp makes this common: at current = 1 the window collapses to [1,1],
	// and inside 2..65 a bucket landing on current-1 clamps straight back.
	return suggestion == current_threshold ? 0u : suggestion;
}

void learning_advice_from_ring(LearningAdvice *advice, const SignalRing *ring,
							   const uint32_t current_thresholds[MOUSE_BUTTON_COUNT])
{
	if (!advice)
		return;

	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		advice->suggested_ms[i] = 0;
		advice->samples[i] = 0;
	}

	advice->seq = 0;

	if (!ring || !current_thresholds)
		return;

	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		advice->suggested_ms[i] = learning_suggest_threshold(ring, i, current_thresholds[i],
														   &advice->samples[i]);
		advice->seq += advice->samples[i];
	}
}
