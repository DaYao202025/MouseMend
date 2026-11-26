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

// Zero means the release is never held back, and it is the default.
//
// While a release is held the system still believes the button is down, so
// every millisecond here is a millisecond of window drag or text selection the
// user did not ask for. Measured on a switch that chatters at the moment of
// release, the hold cost 22-79 ms on every single drag - the "it sticks to the
// pointer" complaint - and bought nothing, because that chatter happens after
// the release rather than during the drag the hold exists to protect.
//
// It also cannot be made short enough to be invisible: the delivery rides on
// WM_TIMER, which is a low priority message and starves while the pointer is
// moving. So the cost is either paid in full or not paid at all.
//
// 20 / 40 / 80 stay on the menu for the switch that really does chatter in the
// middle of a drag.
#define RELEASE_DELAY_DEFAULT_MS 0
#define RELEASE_DELAY_PRESET_COUNT 4

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
