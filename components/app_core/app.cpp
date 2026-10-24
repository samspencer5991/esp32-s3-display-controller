#include "app/app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace app {

Settings default_settings()
{
    Settings s{};
    s.theme = 0;
    s.brightness = 80;
    s.dim_after_s = 60;
    s.sleep_after_s = 600;
    s.rotate_180 = false;
    s.units = Units::Metric;
    s.demo_mode = true;  // until a real desk is connected
    s.child_lock = false;
    s.reminder_min = 0;
    s.range_min_cm = 63.5f;  // Flexispot E7 Pro travel range
    s.range_max_cm = 128.5f;
    s.last_height_cm = 0;
    const struct {
        const char *name;
        PresetIcon icon;
    } presets[kPresetCount] = {
        {"Sit", PresetIcon::Sit},
        {"Stand", PresetIcon::Stand},
        {"Preset 3", PresetIcon::Number},
        {"Preset 4", PresetIcon::Number},
    };
    for (int i = 0; i < kPresetCount; i++) {
        snprintf(s.presets[i].name, sizeof(s.presets[i].name), "%s", presets[i].name);
        s.presets[i].icon = presets[i].icon;
        s.presets[i].height_cm = 0;
    }
    s.lights = LightSettings{.on = false, .brightness = 60, .r = 0x4F, .g = 0x8C, .b = 0xFF, .effect = LightEffect::Solid};
    return s;
}

namespace {

bool same_desk(const DeskState &a, const DeskState &b)
{
    return a.link == b.link && a.height_valid == b.height_valid && a.height_cm == b.height_cm &&
           a.motion == b.motion && a.target_preset == b.target_preset && a.at_preset == b.at_preset &&
           a.box == b.box && a.box_display_on == b.box_display_on && strcmp(a.display, b.display) == 0;
}

BoxState box_state_for(const desk::Status &st)
{
    switch (st.display.kind) {
    case desk::DisplayKind::SaveMode:
        return BoxState::SaveMode;
    case desk::DisplayKind::Reset:
        return BoxState::Reset;
    case desk::DisplayKind::Error:
        return BoxState::Error;
    default:
        return st.box_locked ? BoxState::Locked : BoxState::Normal;
    }
}

}  // namespace

App::App(Platform &platform, desk::Transport *uart, AppConfig config)
    : platform_(platform), uart_(uart), config_(config), controller_(nullptr, config.desk_timing)
{
    settings_ = default_settings();
}

void App::begin()
{
    std::lock_guard<std::mutex> lock(mu_);
    Settings loaded;
    if (platform_.load_settings(loaded)) {
        settings_ = loaded;
    } else {
        settings_.demo_mode = config_.demo_mode_default;
    }
    now_ = now();
    boot_ms_ = now_;
    desk_.target_preset = -1;
    desk_.at_preset = -1;
    if (settings_.last_height_cm > 0) {
        desk_.height_valid = true;  // shown until the control box reports
        desk_.height_cm = settings_.last_height_cm;
    }
    posture_since_ms_ = now_;
    apply_transport();
    version_++;
}

void App::apply_transport()
{
    desk::Transport *t = settings_.demo_mode ? &sim_ : uart_;
    controller_.set_transport(t, now_);
    target_preset_ = -1;
    hold_dir_ = Motion::Idle;
}

void App::tick()
{
    Settings to_save;
    bool save = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        now_ = now();
        if (settings_.demo_mode) {
            sim_.tick(now_);
        }
        if (hold_dir_ != Motion::Idle && controller_.current_keys() == 0 && !controller_.busy()) {
            hold_dir_ = Motion::Idle;  // hold expired inside the controller
        }
        controller_.poll(now_);
        update_desk_state();
        update_reminder();

        if (dirty_ && now_ - dirty_since_ >= config_.settings_save_delay_ms) {
            dirty_ = false;
            to_save = settings_;
            save = true;
        }
    }
    if (save) {
        platform_.save_settings(to_save);
    }
}

// --- Derived state ----------------------------------------------------------------

void App::update_desk_state()
{
    const desk::Status &st = controller_.status();
    DeskState d = desk_;

    if (controller_.transport() == nullptr || !st.online) {
        d.link = DeskLink::Offline;
    } else {
        d.link = settings_.demo_mode ? DeskLink::Demo : DeskLink::Online;
    }
    if (d.link != desk_.link) {
        if (d.link == DeskLink::Offline && desk_.link != DeskLink::Offline) {
            push_event(EventType::DeskOffline);
        } else if (desk_.link == DeskLink::Offline && now_ - boot_ms_ > 3000) {
            push_event(EventType::DeskOnline);
        }
    }

    if (st.height_valid) {
        d.height_valid = true;
        d.height_cm = config_.desk_reports_inches ? st.height * kCmPerInch : st.height;
    }
    d.motion = st.moving == desk::Direction::Up     ? Motion::Up
               : st.moving == desk::Direction::Down ? Motion::Down
                                                    : Motion::Idle;

    snprintf(d.display, sizeof(d.display), "%s", st.display.text);
    d.box_display_on = st.online && st.display.kind != desk::DisplayKind::Blank;
    d.box = st.online ? box_state_for(st) : BoxState::Normal;
    if (d.box != desk_.box) {
        if (d.box == BoxState::Error) push_event(EventType::DeskError);
        if (d.box == BoxState::Reset) push_event(EventType::DeskReset);
        if (d.box == BoxState::Locked) push_event(EventType::BoxLocked);
    }
    d.rx_frames = st.rx_frames;
    d.tx_frames = st.tx_frames;
    d.rx_errors = st.rx_errors;
    d.last_frame[0] = '\0';
    for (size_t i = 0, pos = 0; i < st.last_frame_len && pos + 3 < sizeof(d.last_frame); i++) {
        pos += snprintf(d.last_frame + pos, sizeof(d.last_frame) - pos, "%02X ", st.last_frame[i]);
    }

    const bool settled = st.height_valid && d.motion == Motion::Idle && now_ - st.last_height_change_ms > 3000;

    // Remember the height once the desk settles (not while moving: flash wear).
    if (settled && !settings_.demo_mode && std::fabs(settings_.last_height_cm - d.height_cm) > 0.05f) {
        settings_.last_height_cm = d.height_cm;
        mark_dirty();
    }

    update_target(d);

    d.at_preset = -1;
    if (d.height_valid && d.motion == Motion::Idle && d.link != DeskLink::Offline) {
        for (int i = 0; i < kPresetCount; i++) {
            const float h = settings_.presets[i].height_cm;
            if (h > 0 && std::fabs(h - d.height_cm) <= config_.preset_match_tolerance_cm) {
                d.at_preset = static_cast<int8_t>(i);
                break;
            }
        }
    }

    // Learn the travel range for the height gauge (once settled: one write).
    if (settled && d.link != DeskLink::Offline) {
        if (d.height_cm < settings_.range_min_cm - 0.4f) {
            settings_.range_min_cm = d.height_cm;
            mark_dirty();
        }
        if (d.height_cm > settings_.range_max_cm + 0.4f) {
            settings_.range_max_cm = d.height_cm;
            mark_dirty();
        }
    }

    // Frame counters change ~50 times a second; only let them wake observers
    // once a second so the UI doesn't redraw for diagnostics alone.
    const bool counters_changed = d.rx_frames != desk_.rx_frames || d.tx_frames != desk_.tx_frames ||
                                  d.rx_errors != desk_.rx_errors;
    if (!same_desk(d, desk_)) {
        desk_ = d;
        version_++;
    } else if (counters_changed && now_ - last_counter_publish_ms_ >= 1000) {
        desk_ = d;
        last_counter_publish_ms_ = now_;
        version_++;
    }
}

void App::update_target(DeskState &d)
{
    if (target_preset_ >= 0) {
        const uint16_t key = desk::preset_key(target_preset_);
        uint32_t key_ms = 0;
        const bool key_sent = controller_.last_sent(key, key_ms) && static_cast<int32_t>(key_ms - target_started_ms_) >= 0;

        if (key_sent && d.motion != Motion::Idle) {
            target_saw_motion_ = true;
        }
        if (target_saw_motion_ && d.motion == Motion::Idle && !controller_.pending_keys(key)) {
            // Arrived (or stopped by the box). Only a move that started from
            // rest, uninterrupted, tells us the stored preset height.
            if (target_learn_ && d.height_valid) {
                Preset &p = settings_.presets[target_preset_];
                if (std::fabs(p.height_cm - d.height_cm) > 0.05f) {
                    p.height_cm = d.height_cm;
                    mark_dirty();
                }
            }
            target_preset_ = -1;
        } else if (!target_saw_motion_ && key_sent && now_ - key_ms > 2500) {
            // The box did nothing: preset not stored, or already there.
            if (settings_.presets[target_preset_].height_cm <= 0 && d.link != DeskLink::Offline) {
                push_event(EventType::PresetUnknown, target_preset_);
            }
            target_preset_ = -1;
        } else if (!key_sent && now_ - target_started_ms_ > 5000) {
            target_preset_ = -1;  // key never went out (no transport)
        }
    }
    d.target_preset = target_preset_;
}

void App::update_reminder()
{
    ReminderState r = reminder_;
    const uint32_t interval_ms = static_cast<uint32_t>(settings_.reminder_min) * 60 * 1000;

    if (desk_.height_valid && desk_.motion == Motion::Idle) {
        float sit = 0, stand = 0;
        for (const Preset &p : settings_.presets) {
            if (p.height_cm <= 0) continue;
            if (p.icon == PresetIcon::Sit) sit = p.height_cm;
            if (p.icon == PresetIcon::Stand) stand = p.height_cm;
        }
        const float threshold = (sit > 0 && stand > sit) ? (sit + stand) / 2 : 92.0f;
        const bool standing = desk_.height_cm >= threshold;
        if (!posture_known_ || standing != posture_standing_) {
            posture_known_ = true;
            posture_standing_ = standing;
            posture_since_ms_ = now_;
            r.due = false;
        }
    }
    r.standing = posture_standing_;
    r.elapsed_s = (now_ - posture_since_ms_) / 1000;

    if (snoozed_ && now_ - snooze_started_ms_ >= config_.reminder_snooze_ms) {
        snoozed_ = false;
    }
    if (interval_ms == 0) {
        r.due = false;
    } else if (!r.due && posture_known_ && !snoozed_ && now_ - posture_since_ms_ >= interval_ms) {
        r.due = true;
        push_event(EventType::ReminderDue);
    }

    if (r.due != reminder_.due || r.standing != reminder_.standing ||
        r.elapsed_s / 60 != reminder_.elapsed_s / 60) {
        version_++;
    }
    reminder_ = r;
}

// --- Observation -------------------------------------------------------------------

uint32_t App::version() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return version_;
}

void App::snapshot(State &out) const
{
    std::lock_guard<std::mutex> lock(mu_);
    out.settings = settings_;
    out.desk = desk_;
    out.net = net_;
    out.reminder = reminder_;
    out.uptime_s = (now_ - boot_ms_) / 1000;
    out.firmware_version = platform_.firmware_version();
}

bool App::pop_event(Event &out)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (ev_count_ == 0) {
        return false;
    }
    out = events_[ev_head_];
    ev_head_ = static_cast<uint8_t>((ev_head_ + 1) % 8);
    ev_count_--;
    return true;
}

void App::push_event(EventType type, int arg)
{
    const uint8_t idx = static_cast<uint8_t>((ev_head_ + ev_count_) % 8);
    events_[idx] = Event{type, static_cast<int16_t>(arg)};
    if (ev_count_ < 8) {
        ev_count_++;
    } else {
        ev_head_ = static_cast<uint8_t>((ev_head_ + 1) % 8);  // drop oldest
    }
}

void App::mark_dirty()
{
    if (!dirty_) {
        dirty_since_ = now_;
    }
    dirty_ = true;
    version_++;
}

// --- Desk actions --------------------------------------------------------------------

bool App::memory_armed_locked() const
{
    // While M is being sent, or the box shows "S-", or shortly after M went out
    // without a following preset key, a preset key would *store* a preset.
    if (controller_.pending_keys(desk::KeyMemory) || desk_.box == BoxState::SaveMode) {
        return true;
    }
    uint32_t memory_ms = 0;
    if (!controller_.last_sent(desk::KeyMemory, memory_ms) || now_ - memory_ms >= config_.memory_armed_ms) {
        return false;
    }
    constexpr uint16_t kPresetKeys = desk::KeyPreset1 | desk::KeyPreset2 | desk::KeyPreset3 | desk::KeyPreset4;
    uint32_t preset_ms = 0;
    const bool preset_after = controller_.last_sent(kPresetKeys, preset_ms) &&
                              static_cast<int32_t>(preset_ms - memory_ms) > 0;
    return !preset_after;  // a preset key after M consumed the arm
}

bool App::desk_active_locked() const
{
    return target_preset_ >= 0 || desk_.motion != Motion::Idle || hold_dir_ != Motion::Idle || controller_.busy();
}

bool App::desk_active() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return desk_active_locked();
}

bool App::busy_sending() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return controller_.busy();
}

bool App::move(Motion dir)
{
    std::lock_guard<std::mutex> lock(mu_);
    now_ = now();
    if (dir == Motion::Idle) {
        if (hold_dir_ != Motion::Idle) {
            controller_.release();
            hold_dir_ = Motion::Idle;
        }
        return true;
    }
    if (settings_.child_lock) {
        if (now_ - last_block_event_ms_ > 1500) {
            push_event(EventType::BlockedByLock);
            last_block_event_ms_ = now_;
        }
        return false;
    }
    if (hold_dir_ == Motion::Idle && memory_armed_locked()) {
        // Up/Down would cancel a half-sent save and leave the box armed.
        push_event(EventType::Busy, BusySavingPreset);
        return false;
    }
    target_preset_ = -1;
    hold_dir_ = dir;
    controller_.hold(dir == Motion::Up ? desk::KeyUp : desk::KeyDown, now_);
    return true;
}

bool App::nudge(Motion dir, uint32_t duration_ms)
{
    std::lock_guard<std::mutex> lock(mu_);
    now_ = now();
    if (dir == Motion::Idle) {
        return false;
    }
    if (settings_.child_lock) {
        push_event(EventType::BlockedByLock);
        return false;
    }
    if (target_preset_ >= 0) {
        stop_locked();
        return false;
    }
    if (memory_armed_locked()) {
        push_event(EventType::Busy, BusySavingPreset);
        return false;
    }
    hold_dir_ = Motion::Idle;
    controller_.press(dir == Motion::Up ? desk::KeyUp : desk::KeyDown, duration_ms, now_);
    return true;
}

void App::stop_locked()
{
    hold_dir_ = Motion::Idle;
    controller_.release();

    if (target_preset_ >= 0) {
        const uint16_t key = desk::preset_key(target_preset_);
        if (!controller_.sent_since(key, target_started_ms_)) {
            controller_.cancel();  // the preset key never went out: nothing to interrupt
        } else {
            // "No key" does not cancel an automatic move; a brief tap does. Tap
            // against the direction of travel so any response brakes rather than
            // overshoots. Never wake the box for this.
            bool travelling_up;
            if (desk_.motion != Motion::Idle) {
                travelling_up = desk_.motion == Motion::Up;
            } else {
                const float target = settings_.presets[target_preset_].height_cm;
                travelling_up = !(target > 0 && desk_.height_valid && target < desk_.height_cm);
            }
            controller_.press(travelling_up ? desk::KeyDown : desk::KeyUp, controller_.timing().stop_press_ms, now_,
                              false);
        }
        target_preset_ = -1;
        desk_.target_preset = -1;
        version_++;
    } else if (controller_.busy() && !controller_.pending_keys(desk::KeyMemory)) {
        controller_.cancel();  // timed Up/Down (nudge): stopping sending stops the desk
    }
    // Residual manual motion (coasting after a release) needs nothing more.
}

void App::stop()
{
    std::lock_guard<std::mutex> lock(mu_);
    now_ = now();
    stop_locked();
}

bool App::goto_preset(int index)
{
    if (index < 0 || index >= kPresetCount) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    now_ = now();
    if (settings_.child_lock) {
        push_event(EventType::BlockedByLock);
        return false;
    }
    if (target_preset_ >= 0) {
        stop_locked();  // any preset key during an automatic move stops the desk
        return false;
    }
    if (memory_armed_locked()) {
        push_event(EventType::Busy, BusySavingPreset);
        return false;
    }
    // Coasting after a manual move: the move still works, but the height it
    // ends at is not a trustworthy preset height.
    const bool from_rest = desk_.motion == Motion::Idle && hold_dir_ == Motion::Idle;
    hold_dir_ = Motion::Idle;
    controller_.press(desk::preset_key(index), controller_.timing().preset_press_ms, now_);
    target_preset_ = static_cast<int8_t>(index);
    target_started_ms_ = now_;
    target_saw_motion_ = !from_rest;
    target_learn_ = from_rest;
    desk_.target_preset = target_preset_;
    version_++;
    push_event(EventType::MovingToPreset, index);
    return true;
}

bool App::save_preset(int index)
{
    if (index < 0 || index >= kPresetCount) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    now_ = now();
    if (settings_.child_lock) {
        push_event(EventType::BlockedByLock);
        return false;
    }
    if (desk_active_locked()) {
        push_event(EventType::Busy, BusyDeskMoving);
        return false;
    }
    if (memory_armed_locked()) {
        push_event(EventType::Busy, BusySavingPreset);
        return false;
    }
    const desk::Timing &t = controller_.timing();
    controller_.press_sequence(desk::KeyMemory, t.memory_press_ms, desk::preset_key(index), t.memory_press_ms, now_);
    if (desk_.height_valid) {
        settings_.presets[index].height_cm = desk_.height_cm;
    }
    mark_dirty();
    push_event(EventType::PresetSaved, index);
    return true;
}

bool App::toggle_box_lock()
{
    std::lock_guard<std::mutex> lock(mu_);
    now_ = now();
    if (desk_active_locked()) {
        push_event(EventType::Busy, BusyDeskMoving);
        return false;
    }
    controller_.press(desk::KeyMemory, controller_.timing().lock_press_ms, now_);
    return true;
}

void App::set_child_lock_locked(bool locked)
{
    if (settings_.child_lock == locked) {
        return;
    }
    settings_.child_lock = locked;
    if (locked) {
        stop_locked();  // locking freezes the desk
    }
    mark_dirty();
    push_event(locked ? EventType::Locked : EventType::Unlocked);
}

void App::set_child_lock(bool locked)
{
    std::lock_guard<std::mutex> lock(mu_);
    now_ = now();
    set_child_lock_locked(locked);
}

// --- Settings --------------------------------------------------------------------------

Settings App::settings() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return settings_;
}

void App::apply_settings_locked(Settings next)
{
    if (next.demo_mode != settings_.demo_mode && desk_active_locked()) {
        // Switching transports mid-move would strand the real desk moving
        // with nothing left to stop it.
        next.demo_mode = settings_.demo_mode;
        push_event(EventType::Busy, BusyDeskMoving);
        version_++;  // let observers resync their controls
    }
    const bool lock_changed = next.child_lock != settings_.child_lock;
    const bool demo_changed = next.demo_mode != settings_.demo_mode;
    next.child_lock = settings_.child_lock;  // applied below with its side effects
    if (memcmp(&next, &settings_, sizeof(Settings)) != 0) {
        settings_ = next;
        mark_dirty();
    }
    if (demo_changed) {
        apply_transport();
    }
    if (lock_changed) {
        set_child_lock_locked(!settings_.child_lock);
    }
}

void App::modify_settings(const std::function<void(Settings &)> &edit)
{
    std::lock_guard<std::mutex> lock(mu_);
    now_ = now();
    Settings next = settings_;
    edit(next);
    apply_settings_locked(next);
}

void App::update_settings(const Settings &s)
{
    std::lock_guard<std::mutex> lock(mu_);
    now_ = now();
    apply_settings_locked(s);
}

void App::set_lights(const LightSettings &lights)
{
    std::lock_guard<std::mutex> lock(mu_);
    now_ = now();
    settings_.lights = lights;
    settings_.lights.brightness = std::clamp<uint8_t>(lights.brightness, 1, 100);
    mark_dirty();
}

void App::set_preset_meta(int index, const char *name, PresetIcon icon)
{
    if (index < 0 || index >= kPresetCount) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    now_ = now();
    Preset &p = settings_.presets[index];
    if (name != nullptr) {
        snprintf(p.name, sizeof(p.name), "%s", name);
    }
    if (icon < PresetIcon::Count) {
        p.icon = icon;
    }
    mark_dirty();
}

void App::reset_range()
{
    std::lock_guard<std::mutex> lock(mu_);
    now_ = now();
    const Settings d = default_settings();
    settings_.range_min_cm = d.range_min_cm;
    settings_.range_max_cm = d.range_max_cm;
    mark_dirty();
}

void App::acknowledge_reminder(bool snooze)
{
    std::lock_guard<std::mutex> lock(mu_);
    now_ = now();
    reminder_.due = false;
    if (snooze) {
        snoozed_ = true;
        snooze_started_ms_ = now_;
    } else {
        posture_since_ms_ = now_;
    }
    version_++;
}

void App::set_net_state(const NetState &net)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (memcmp(&net, &net_, sizeof(NetState)) != 0) {
        net_ = net;
        version_++;
    }
}

// --- Diagnostics ----------------------------------------------------------------------------

void App::send_raw_keys(uint16_t keys, uint32_t duration_ms)
{
    std::lock_guard<std::mutex> lock(mu_);
    now_ = now();
    controller_.press(keys, duration_ms, now_);
}

void App::set_frame_tap(desk::Controller::FrameTap tap, void *ctx)
{
    std::lock_guard<std::mutex> lock(mu_);
    controller_.set_frame_tap(tap, ctx);
}

void App::wake_desk()
{
    std::lock_guard<std::mutex> lock(mu_);
    now_ = now();
    controller_.wake(now_);
}

}  // namespace app
