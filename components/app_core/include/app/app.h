// Application core: owns the desk controller, settings and derived state.
//
// Thread-safety: every public method locks an internal mutex, so the UI task,
// the desk task and network tasks may all call in. Observers poll version()
// and take a snapshot() when it changes.
//
// Safety rules enforced here (not in the UI) so every caller gets them:
//  * a preset key is never sent while the control box may still be armed by
//    M ("S-"), because it would overwrite that preset instead of moving;
//  * a preset request during an automatic move stops the desk (as on the
//    stock handset);
//  * saving, locking the box and switching demo mode are refused while the
//    desk moves.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

#include "app/state.h"
#include "desk/controller.h"
#include "desk/sim_control_box.h"

namespace app {

class Platform {
public:
    virtual ~Platform() = default;
    virtual uint32_t now_ms() = 0;
    virtual bool load_settings(Settings &out) = 0;
    // Called from the desk task: must not block for long (queue the write).
    virtual void save_settings(const Settings &s) = 0;
    virtual const char *firmware_version() { return "dev"; }
};

struct AppConfig {
    bool desk_reports_inches = false;  // control box configured for inches
    uint32_t settings_save_delay_ms = 1500;
    float preset_match_tolerance_cm = 0.6f;
    uint32_t reminder_snooze_ms = 10 * 60 * 1000;
    uint32_t memory_armed_ms = 5000;   // box may treat a preset key as "store" this long after M
    bool demo_mode_default = true;     // first boot, before settings exist
    desk::Timing desk_timing;
};

class App {
public:
    App(Platform &platform, desk::Transport *uart, AppConfig config = AppConfig());

    void begin();
    // Drive from a single task every few ms.
    void tick();

    // --- Observation ---------------------------------------------------------
    uint32_t version() const;
    void snapshot(State &out) const;
    bool pop_event(Event &out);

    // --- Desk ------------------------------------------------------------------
    // Hold-to-move. Call repeatedly (<= 300 ms apart) while the button is held;
    // Motion::Idle releases. Returns false if blocked (child lock).
    bool move(Motion dir);
    // Timed move (Home Assistant "nudge"). Cancelled by stop()/lock.
    bool nudge(Motion dir, uint32_t duration_ms);
    void stop();
    // During an automatic move this stops the desk and returns false.
    bool goto_preset(int index);
    bool save_preset(int index);
    // UI child lock (blocks all movement from this controller and HA).
    void set_child_lock(bool locked);
    // Control box's own lock: holds M for ~5.5 s, which toggles "LoC".
    bool toggle_box_lock();
    bool busy_sending() const;
    bool desk_active() const;  // moving, travelling or sending keys

    // --- Settings --------------------------------------------------------------
    Settings settings() const;
    // Applies `edit` to the current settings atomically (no lost updates when
    // several tasks change different fields). `edit` must not call into App.
    void modify_settings(const std::function<void(Settings &)> &edit);
    void update_settings(const Settings &s);  // whole-struct replace (tests / tools)
    void set_lights(const LightSettings &lights);
    void set_preset_meta(int index, const char *name, PresetIcon icon);
    void reset_range();

    void acknowledge_reminder(bool snooze);

    // --- Platform feedback -------------------------------------------------------
    void set_net_state(const NetState &net);

    // --- Diagnostics ---------------------------------------------------------------
    void send_raw_keys(uint16_t keys, uint32_t duration_ms);
    void wake_desk();
    void set_frame_tap(desk::Controller::FrameTap tap, void *ctx);
    desk::SimControlBox &sim_box() { return sim_; }

private:
    void push_event(EventType type, int arg = 0);
    void mark_dirty();
    void apply_transport();
    void apply_settings_locked(Settings next);
    void set_child_lock_locked(bool locked);
    void stop_locked();
    bool memory_armed_locked() const;
    bool desk_active_locked() const;
    void update_desk_state();
    void update_target(DeskState &d);
    void update_reminder();
    uint32_t now() { return platform_.now_ms(); }

    Platform &platform_;
    desk::Transport *uart_;
    AppConfig config_;
    desk::SimControlBox sim_;
    desk::Controller controller_;

    mutable std::mutex mu_;
    uint32_t version_ = 1;
    uint32_t now_ = 0;
    uint32_t boot_ms_ = 0;
    Settings settings_;
    DeskState desk_{};
    NetState net_{};
    ReminderState reminder_{};

    bool dirty_ = false;
    uint32_t dirty_since_ = 0;

    // Automatic travel to a preset.
    int8_t target_preset_ = -1;
    uint32_t target_started_ms_ = 0;
    bool target_saw_motion_ = false;
    bool target_learn_ = false;  // store the arrival height as the preset height

    Motion hold_dir_ = Motion::Idle;
    uint32_t last_block_event_ms_ = 0;
    uint32_t last_counter_publish_ms_ = 0;

    bool posture_standing_ = false;
    bool posture_known_ = false;
    uint32_t posture_since_ms_ = 0;
    bool snoozed_ = false;
    uint32_t snooze_started_ms_ = 0;

    Event events_[8] = {};
    uint8_t ev_head_ = 0;
    uint8_t ev_count_ = 0;
};

}  // namespace app
