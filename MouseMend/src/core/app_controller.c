#include "app_controller.h"
#include "../ui/language.h"

// Mark the state dirty
// dirty_since = 0 restarts the countdown, so a slider that keeps moving postpones
// the commit until the user actually stops.
static void mark_dirty(AppController *controller)
{
	if (!controller)
		return;

	controller->dirty = true;
	controller->dirty_since = 0;
}

// Notify the observer, if any
static void notify(AppController *controller)
{
	if (controller && controller->observer)
		controller->observer(controller->observer_user);
}

// Bind the controller to the state it manages
void app_controller_attach(AppController *controller, DebounceManager *debounce, Preferences *prefs)
{
	if (!controller)
		return;

	controller->debounce = debounce;
	controller->prefs = prefs;
	controller->save = NULL;
	controller->save_user = NULL;
	controller->reset_statistics = NULL;
	controller->reset_user = NULL;
	controller->observer = NULL;
	controller->observer_user = NULL;
	controller->dirty_since = 0;
	controller->dirty = false;
	controller->commit_count = 0;
}

// Install the persistence hook
void app_controller_set_save_sink(AppController *controller, AppControllerSaveFn save, void *user)
{
	if (!controller)
		return;

	controller->save = save;
	controller->save_user = user;
}

// Install the statistics-reset hook
void app_controller_set_reset_sink(AppController *controller, AppControllerResetFn reset, void *user)
{
	if (!controller)
		return;

	controller->reset_statistics = reset;
	controller->reset_user = user;
}

// Install the change observer
void app_controller_set_observer(AppController *controller, AppControllerObserverFn observer, void *user)
{
	if (!controller)
		return;

	controller->observer = observer;
	controller->observer_user = user;
}

// Set the debounce threshold of one button
void app_controller_set_threshold(AppController *controller, MouseButton button, uint32_t threshold_ms)
{
	if (!controller || !controller->debounce)
		return;

	// Reject out-of-range input before marking the state dirty, otherwise a
	// rejected value would still trigger a pointless disk write
	if (button < 0 || button >= MOUSE_BUTTON_COUNT)
		return;

	if (threshold_ms < THRESHOLD_MIN_VALUE || threshold_ms > THRESHOLD_MAX_VALUE)
		return;

	debounce_set_threshold(controller->debounce, button, threshold_ms,
						   THRESHOLD_MIN_VALUE, THRESHOLD_MAX_VALUE);

	mark_dirty(controller);
	notify(controller);
}

// Enable or disable one button
void app_controller_set_monitored(AppController *controller, MouseButton button, bool monitored)
{
	if (!controller || !controller->debounce)
		return;

	debounce_set_monitored(controller->debounce, button, monitored);

	mark_dirty(controller);
	notify(controller);
}

// Enable or disable every button
void app_controller_set_all_monitored(AppController *controller, bool monitored)
{
	if (!controller || !controller->debounce)
		return;

	debounce_set_all_monitored(controller->debounce, monitored);

	mark_dirty(controller);
	notify(controller);
}

// Enable or disable Smart Drag Protection
void app_controller_set_hybrid(AppController *controller, bool enabled)
{
	if (!controller || !controller->debounce)
		return;

	debounce_set_hybrid_heuristic(controller->debounce, enabled);

	mark_dirty(controller);
	notify(controller);
}

// Set how long a release is deferred
void app_controller_set_release_delay(AppController *controller, uint32_t delay_ms)
{
	if (!controller || !controller->debounce)
		return;

	if (delay_ms < RELEASE_DELAY_MIN_MS || delay_ms > RELEASE_DELAY_MAX_MS)
		return;

	debounce_set_release_delay(controller->debounce, delay_ms,
							   RELEASE_DELAY_MIN_MS, RELEASE_DELAY_MAX_MS);

	mark_dirty(controller);
	notify(controller);
}

// Set how far the cursor must move before a press counts as a drag
void app_controller_set_drag_distance(AppController *controller, uint32_t distance_px)
{
	if (!controller || !controller->debounce)
		return;

	if (distance_px < DRAG_DISTANCE_MIN_PX || distance_px > DRAG_DISTANCE_MAX_PX)
		return;

	debounce_set_drag_distance(controller->debounce, distance_px,
							   DRAG_DISTANCE_MIN_PX, DRAG_DISTANCE_MAX_PX);

	mark_dirty(controller);
	notify(controller);
}

// Change the user interface language
void app_controller_set_language(AppController *controller, Language language)
{
	if (!controller || !controller->prefs)
		return;

	if (language < 0 || language >= LANG_COUNT)
		return;

	language_set(language);
	controller->prefs->language = language;

	mark_dirty(controller);
	notify(controller);
}

// Enable or disable the "false click blocked" balloon
void app_controller_set_notify_blocks(AppController *controller, bool enabled)
{
	if (!controller || !controller->prefs)
		return;

	controller->prefs->notify_blocks = enabled;

	mark_dirty(controller);
	notify(controller);
}

// Enable or disable the blocked-history log
void app_controller_set_log_blocks(AppController *controller, bool enabled)
{
	if (!controller || !controller->prefs)
		return;

	controller->prefs->log_blocks = enabled;

	mark_dirty(controller);
	notify(controller);
}

// Apply a preset to every button
void app_controller_apply_preset(AppController *controller, const PresetConfig *preset)
{
	if (!controller || !controller->debounce || !preset)
		return;

	for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
	{
		debounce_set_threshold(controller->debounce, (MouseButton)i, (uint32_t)preset->thresholds[i],
							   THRESHOLD_MIN_VALUE, THRESHOLD_MAX_VALUE);
	}

	mark_dirty(controller);
	notify(controller);
}

// Clear all counters
void app_controller_reset_statistics(AppController *controller)
{
	if (!controller || !controller->debounce)
		return;

	debounce_reset_statistics(controller->debounce);

	if (controller->reset_statistics)
		controller->reset_statistics(controller->reset_user);

	notify(controller);
}

// Mark the state dirty without changing anything
void app_controller_touch(AppController *controller)
{
	mark_dirty(controller);
}

// Check whether there are unsaved changes
bool app_controller_is_dirty(const AppController *controller)
{
	return controller && controller->dirty;
}

// Write the state to disk now
bool app_controller_commit(AppController *controller)
{
	if (!controller)
		return false;

	if (!controller->dirty)
		return true;

	if (!controller->save)
		return false;

	if (!controller->save(controller->debounce, controller->prefs, controller->save_user))
	{
		// Keep the state dirty so a later tick can retry
		return false;
	}

	controller->dirty = false;
	controller->dirty_since = 0;
	controller->commit_count++;

	return true;
}

// Commit when the debounce delay has elapsed since the last change
void app_controller_tick(AppController *controller, uint64_t now)
{
	if (!controller || !controller->dirty)
		return;

	// Start the countdown on the first tick after a change
	if (controller->dirty_since == 0)
	{
		controller->dirty_since = now;
		return;
	}

	if (now < controller->dirty_since)
	{
		// The clock went backwards; restart instead of waiting forever
		controller->dirty_since = now;
		return;
	}

	if (now - controller->dirty_since < APP_CONTROLLER_COMMIT_DELAY_MS)
		return;

	app_controller_commit(controller);
}
