#pragma once

#include <stdbool.h>
#include "mouse_hook.h"

// Registry location for all persistent settings
#define REG_SETTINGS_KEY L"Software\\MouseMend"

// Supported user interface languages
typedef enum
{
	LANG_EN = 0,
	LANG_ZH_CN = 1,
	LANG_COUNT
} Language;

// Allowed custom threshold range in milliseconds
#define THRESHOLD_MIN_VALUE 1
#define THRESHOLD_MAX_VALUE 200

// Number of quick-pick thresholds offered per button in the tray menu
#define THRESHOLD_PRESET_COUNT 3

// Minimum time between two "false click blocked" balloons.
// A worn switch can bounce dozens of times per minute; without throttling the
// notification itself would become the annoyance the app is supposed to remove.
#define NOTIFY_MIN_INTERVAL_MS 20000

// Smart Drag Protection: delay applied to a deferred release
#define RELEASE_DELAY_MIN_MS 0
#define RELEASE_DELAY_MAX_MS 200
// Short on purpose. The release is held back to outlast a chattering switch,
// and while it is held the system still thinks the button is down - so every
// millisecond here is a millisecond of window drag or text selection that the
// user did not ask for. Twenty is already longer than a bounce and short enough
// to be lost between two frames.
#define RELEASE_DELAY_DEFAULT_MS 20
#define RELEASE_DELAY_PRESET_COUNT 3

// Smart Drag Protection: movement (in pixels) above which a press counts as a drag
#define DRAG_DISTANCE_MIN_PX 1
#define DRAG_DISTANCE_MAX_PX 50
#define DRAG_DISTANCE_DEFAULT_PX 5
#define DRAG_DISTANCE_PRESET_COUNT 3

// Smart Drag Protection: hold time above which a press counts as a drag
#define DRAG_HOLD_TIME_DEFAULT_MS 200
#define DRAG_HOLD_TIME_MIN_MS 50
#define DRAG_HOLD_TIME_MAX_MS 1000

// Full preset configuration (one threshold per button)
typedef struct
{
	int thresholds[MOUSE_BUTTON_COUNT];
} PresetConfig;

extern const PresetConfig CONFIG_PRESET_DEFAULT;
extern const PresetConfig CONFIG_PRESET_OFFICE;
extern const PresetConfig CONFIG_PRESET_STRICT;

// Check whether a button is a wheel axis (both use the "reverse direction" logic)
bool config_is_wheel_button(MouseButton button);

// Get the quick-pick threshold list for a button (buttons or wheel axis)
const int *config_threshold_presets(MouseButton button);

// Get the quick-pick lists for the Smart Drag Protection tuning
const int *config_release_delay_presets(void);
const int *config_drag_distance_presets(void);
