#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "debouncer.h"
#include "settings.h"

// Single entry point for every state change.
//
// The tray menu used to mutate the debounce manager directly and call
// settings_save() after every click (18 synchronous registry writes). As soon as
// a second editor exists - the settings window - that design breaks in three
// ways: two editors overwrite each other's unsaved work, "clear blocked count"
// forgets to reset the notification baseline, and a slider drag issues dozens of
// registry writes per second on the UI thread, which is exactly the stall that
// gets a low-level hook removed by Windows.
//
// Therefore: every mutation goes through this layer, setters only touch memory,
// and persistence is debounced.

// How long after the last change the state is written to disk
#define APP_CONTROLLER_COMMIT_DELAY_MS 400

// Disk writer. Returning false keeps the state dirty so the next tick retries.
typedef bool (*AppControllerSaveFn)(const DebounceManager *debounce, const Preferences *prefs, void *user);

// Called after the statistics were cleared, so the owner can reset its
// notification baseline in the same breath.
typedef void (*AppControllerResetFn)(void *user);

// Called after every mutation, so a visible window can refresh immediately
typedef void (*AppControllerObserverFn)(void *user);

typedef struct
{
	DebounceManager *debounce; // Borrowed, owned by the application
	Preferences *prefs;		   // Borrowed, owned by the application

	AppControllerSaveFn save;
	void *save_user;

	AppControllerResetFn reset_statistics;
	void *reset_user;

	AppControllerObserverFn observer;
	void *observer_user;

	uint64_t dirty_since; // 0 = countdown not started yet
	bool dirty;
	uint32_t commit_count;
} AppController;

// Bind the controller to the state it manages (no ownership transfer)
void app_controller_attach(AppController *controller, DebounceManager *debounce, Preferences *prefs);

// Install the persistence and notification hooks
void app_controller_set_save_sink(AppController *controller, AppControllerSaveFn save, void *user);
void app_controller_set_reset_sink(AppController *controller, AppControllerResetFn reset, void *user);
void app_controller_set_observer(AppController *controller, AppControllerObserverFn observer, void *user);

// Mutations. They validate, update memory, mark the state dirty and notify the
// observer. They never touch the disk.
void app_controller_set_threshold(AppController *controller, MouseButton button, uint32_t threshold_ms);
void app_controller_set_monitored(AppController *controller, MouseButton button, bool monitored);
void app_controller_set_all_monitored(AppController *controller, bool monitored);
void app_controller_set_hybrid(AppController *controller, bool enabled);
void app_controller_set_release_delay(AppController *controller, uint32_t delay_ms);
void app_controller_set_drag_distance(AppController *controller, uint32_t distance_px);
void app_controller_set_language(AppController *controller, Language language);
void app_controller_set_notify_blocks(AppController *controller, bool enabled);
void app_controller_set_log_blocks(AppController *controller, bool enabled);
void app_controller_apply_preset(AppController *controller, const PresetConfig *preset);

// Clear all counters and tell the owner so it can reset its own baselines
void app_controller_reset_statistics(AppController *controller);

// Mark the state dirty without changing anything, for settings that are edited
// outside this layer (for example the "first run balloon already shown" flag)
void app_controller_touch(AppController *controller);

// Persistence control
bool app_controller_is_dirty(const AppController *controller);
bool app_controller_commit(AppController *controller);
void app_controller_tick(AppController *controller, uint64_t now);
