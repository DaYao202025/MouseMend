#include "config.h"

// Quick-pick thresholds shown in the per-button submenus
static const int BUTTON_THRESHOLD_PRESETS[THRESHOLD_PRESET_COUNT] = {40, 50, 60};
static const int WHEEL_THRESHOLD_PRESETS[THRESHOLD_PRESET_COUNT] = {20, 30, 35};

// Quick-pick values for the Smart Drag Protection tuning
// The delay list starts where the default is, so the smallest step is also the
// least intrusive one; only a switch that really does chatter needs 40 or 80.
static const int RELEASE_DELAY_PRESETS[RELEASE_DELAY_PRESET_COUNT] = {20, 40, 80};
static const int DRAG_DISTANCE_PRESETS[DRAG_DISTANCE_PRESET_COUNT] = {3, 5, 10};

// Preset definitions
const PresetConfig CONFIG_PRESET_DEFAULT = {
	.thresholds = {
		[MOUSE_BUTTON_LEFT] = 50,
		[MOUSE_BUTTON_RIGHT] = 50,
		[MOUSE_BUTTON_MIDDLE] = 50,
		[MOUSE_BUTTON_X1] = 50,
		[MOUSE_BUTTON_X2] = 50,
		[MOUSE_BUTTON_WHEEL] = 30,
		[MOUSE_BUTTON_HWHEEL] = 30}};

const PresetConfig CONFIG_PRESET_OFFICE = {
	.thresholds = {
		[MOUSE_BUTTON_LEFT] = 60,
		[MOUSE_BUTTON_RIGHT] = 60,
		[MOUSE_BUTTON_MIDDLE] = 60,
		[MOUSE_BUTTON_X1] = 60,
		[MOUSE_BUTTON_X2] = 60,
		[MOUSE_BUTTON_WHEEL] = 35,
		[MOUSE_BUTTON_HWHEEL] = 35}};

const PresetConfig CONFIG_PRESET_STRICT = {
	.thresholds = {
		[MOUSE_BUTTON_LEFT] = 40,
		[MOUSE_BUTTON_RIGHT] = 40,
		[MOUSE_BUTTON_MIDDLE] = 40,
		[MOUSE_BUTTON_X1] = 40,
		[MOUSE_BUTTON_X2] = 40,
		[MOUSE_BUTTON_WHEEL] = 20,
		[MOUSE_BUTTON_HWHEEL] = 20}};

// Check whether a button is a wheel axis
bool config_is_wheel_button(MouseButton button)
{
	return button == MOUSE_BUTTON_WHEEL || button == MOUSE_BUTTON_HWHEEL;
}

// Get the quick-pick threshold list for a button
const int *config_threshold_presets(MouseButton button)
{
	return config_is_wheel_button(button) ? WHEEL_THRESHOLD_PRESETS : BUTTON_THRESHOLD_PRESETS;
}

// Get the quick-pick release delay list
const int *config_release_delay_presets(void)
{
	return RELEASE_DELAY_PRESETS;
}

// Get the quick-pick drag distance list
const int *config_drag_distance_presets(void)
{
	return DRAG_DISTANCE_PRESETS;
}
