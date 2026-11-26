#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mouse_hook.h"
#include "signal_ring.h"

// Turns a histogram of rejected intervals into a *suggestion*. It never
// changes a threshold by itself: the user has to adopt it from the tray menu.
//
// Pure logic, no Win32, no allocation, no locks - the learning thread calls it
// after the hook has published a completed gesture.

#define LEARNING_MIN_SAMPLES 32
#define LEARNING_PERCENTILE 80

typedef struct
{
	uint32_t suggested_ms[MOUSE_BUTTON_COUNT]; // 0 = not enough evidence
	uint32_t samples[MOUSE_BUTTON_COUNT];
	uint64_t seq;
} LearningAdvice;

uint32_t learning_suggest_threshold(const SignalRing *ring, int button,
									uint32_t current_threshold, uint32_t *out_samples);

void learning_advice_from_ring(LearningAdvice *advice, const SignalRing *ring,
							   const uint32_t current_thresholds[MOUSE_BUTTON_COUNT]);
