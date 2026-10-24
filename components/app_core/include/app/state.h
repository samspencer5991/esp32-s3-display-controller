// Application state shared between the desk logic, UI and connectivity layers.
//
// Everything here is plain data so it can be copied as a snapshot under a
// lock and compared cheaply. No platform or LVGL dependencies.
#pragma once

#include <cstdint>

namespace app {

constexpr int kPresetCount = 4;

enum class Units : uint8_t { Metric, Imperial };

enum class PresetIcon : uint8_t { Number, Sit, Stand, Walk, Focus, Count };

enum class LightEffect : uint8_t { Solid, Breathe, Rainbow, DeskStatus, Count };

struct Preset {
    char name[16];
    PresetIcon icon;
    float height_cm;  // <= 0 when unknown (never saved/observed)
};

struct LightSettings {
    bool on;
    uint8_t brightness;  // 1..100 %
    uint8_t r, g, b;
    LightEffect effect;
};

// Persisted settings. Bump kSettingsVersion when the layout changes so stale
// blobs are discarded instead of misinterpreted.
constexpr uint16_t kSettingsVersion = 4;

struct Settings {
    uint8_t theme;           // index into ui theme registry
    uint8_t brightness;      // backlight 5..100 %
    uint16_t dim_after_s;    // 0 = never
    uint16_t sleep_after_s;  // backlight off, 0 = never
    bool rotate_180;
    Units units;
    bool demo_mode;          // drive a simulated control box instead of the UART
    bool child_lock;
    uint16_t reminder_min;   // sit/stand reminder interval, 0 = off
    float range_min_cm;      // learned desk travel limits (for the gauge)
    float range_max_cm;
    float last_height_cm;    // last known height, shown until the box reports again
    Preset presets[kPresetCount];
    LightSettings lights;
};

Settings default_settings();

enum class DeskLink : uint8_t {
    Offline,  // no valid frames from the control box
    Online,   // real control box talking to us
    Demo,     // simulated control box
};

enum class Motion : uint8_t { Idle, Up, Down };

// What the control box's own display is showing besides a height.
enum class BoxState : uint8_t {
    Normal,
    Locked,    // "LoC": control box child lock engaged (hold M 5 s to toggle)
    SaveMode,  // "S-": M pressed, next preset key stores the current height
    Reset,     // "ASr"/"rSt": hold Down until the desk bottoms out
    Error,     // "E01", "E02"...
};

struct DeskState {
    DeskLink link;
    bool height_valid;
    float height_cm;
    Motion motion;
    int8_t target_preset;  // preset the desk is travelling to, -1 = none
    int8_t at_preset;      // preset matching the current height, -1 = none
    BoxState box;
    bool box_display_on;   // control box display awake (height reports flowing)
    char display[8];       // text on the control box display ("72.5", "LoC", "E01")
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t rx_errors;
    char last_frame[48];   // hex dump of last received frame (diagnostics)
};

enum class WifiStatus : uint8_t { Disabled, Connecting, Connected, Failed };

struct NetState {
    WifiStatus wifi;
    int8_t rssi;
    char ssid[33];
    char ip[16];
    bool mqtt_enabled;
    bool mqtt_connected;
};

struct ReminderState {
    bool due;             // reminder currently asking for attention
    bool standing;        // current posture guess
    uint32_t elapsed_s;   // time in current posture
};

struct State {
    Settings settings;
    DeskState desk;
    NetState net;
    ReminderState reminder;
    uint32_t uptime_s;
    const char *firmware_version;
};

// One-shot notifications for the UI (toasts, modals).
enum class EventType : uint8_t {
    PresetSaved,      // arg = preset index
    MovingToPreset,   // arg = preset index
    Locked,
    Unlocked,
    BlockedByLock,    // user tried to move while locked
    PresetUnknown,    // arg = preset index
    DeskOffline,
    DeskOnline,
    ReminderDue,
    DeskError,        // control box displays an error code
    DeskReset,        // control box needs a reset run
    BoxLocked,        // control box refused to move: its own child lock is on
    Busy,             // request refused for safety, arg = BusyReason
};

enum BusyReason : int16_t {
    BusySavingPreset = 0,  // control box may still be armed by M
    BusyDeskMoving = 1,    // action not allowed while the desk moves
};

struct Event {
    EventType type;
    int16_t arg;
};

// Unit helpers ---------------------------------------------------------------

constexpr float kCmPerInch = 2.54f;

inline float to_display_units(float cm, Units u) { return u == Units::Imperial ? cm / kCmPerInch : cm; }
inline const char *unit_suffix(Units u) { return u == Units::Imperial ? "in" : "cm"; }

}  // namespace app
