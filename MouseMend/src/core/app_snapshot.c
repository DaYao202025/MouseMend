#include "app_snapshot.h"

#include <string.h>

void app_snapshot_fill(AppSnapshot *snapshot, DebounceManager *debounce)
{
	if (!snapshot)
		return;

	memset(snapshot, 0, sizeof(*snapshot));

	if (!debounce)
		return;

	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		snapshot->blocks[i] = debounce_get_button_blocks(debounce, (MouseButton)i);
		snapshot->thresholds[i] = debounce->buttons[i].thresholdMs;
		snapshot->monitored[i] = debounce->buttons[i].isMonitored;
	}

	// Wall clock and tick sampled together, so the window can turn a decision
	// timestamp into a readable time without keeping a base of its own
	GetLocalTime(&snapshot->now_local);
	snapshot->now_tick = GetTickCount64();

	snapshot->total_blocks = debounce_get_total_blocks(debounce);
	snapshot->hybrid = debounce->use_hybrid_heuristic;
	snapshot->release_delay_ms = debounce->releaseDelayMs;
	snapshot->drag_distance_px = debounce->dragDistancePx;
}
