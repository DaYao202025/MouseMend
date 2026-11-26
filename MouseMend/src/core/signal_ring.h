#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

#include "mouse_hook.h"

// Lock-free histogram of the interval the threshold decision compares.
//
// The engine compares decision.elapsed_ms (the gap from the previous event of
// any kind) against the button threshold. press_gap_ms is a different quantity
// - a press-to-press rhythm - and must never be learned into this histogram.
// See doc/design-learning-and-context-filter.md, finding P0-2.
//
// What gets recorded is deliberately NOT "the presses the threshold rejected".
// That was the original design and it starved itself: a switch that chatters at
// 60 ms while the threshold is 50 ms is never rejected, so it was never
// sampled, so the advice could never suggest raising the threshold to 60. The
// window below is fixed and independent of the current threshold - a gap that
// short cannot be two deliberate clicks - so the histogram can still fill up
// while the filter is doing nothing at all.
//
// Hook side: one Interlocked increment, no allocation, no lock, no API call.
// Reader side: plain snapshots, eventually consistent by design.

#define SIGNAL_RING_BUCKET_COUNT 64 // 1..64 ms; larger gaps saturate into bucket 64

// A gap above this is a considered click, not chatter. Sampling wider would
// teach the filter to swallow fast but deliberate double clicks.
#define SIGNAL_RING_SAMPLE_MAX_MS 64

typedef struct
{
	volatile LONG buckets[MOUSE_BUTTON_COUNT][SIGNAL_RING_BUCKET_COUNT];
	volatile LONG samples[MOUSE_BUTTON_COUNT];
} SignalRing;

void signal_ring_init(SignalRing *ring);

// Hook side: one call per press whose gap from the previous event was within
// 1..SIGNAL_RING_SAMPLE_MAX_MS, whether or not the engine blocked it.
void signal_ring_note_interval(SignalRing *ring, int button, uint64_t elapsed_ms);

// Reader side (learning thread and tests).
uint32_t signal_ring_bucket(uint64_t gap_ms);
uint32_t signal_ring_samples(const SignalRing *ring, int button);
uint32_t signal_ring_bucket_value(const SignalRing *ring, int button, uint32_t bucket);

// Discard everything collected for one button (settings change or explicit reset).
void signal_ring_reset(SignalRing *ring, int button);
