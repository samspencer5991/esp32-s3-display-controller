// Home screen (640x180):
//
//  +-----------------+ +--------+--------+ +----------+ +----+
//  | ● Demo    ⌁ ☁  | |   1    |   2    | |    ▲     | | 🔒 |
//  |                 | |  chair |  stand | |          | +----+
//  | 72.5 cm         | +--------+--------+ +----------+ | 💡 |
//  | Sit             | |   3    |   4    | |    ▼     | +----+
//  | ━━━━━━━░░░░░░░  | |        |        | |          | | ⚙  |
//  +-----------------+ +--------+--------+ +----------+ +----+
//     height card          presets 2x2       up/down     rail
#include <algorithm>
#include <cmath>
#include <tuple>

#include "ui_internal.h"

namespace ui {

namespace {

using namespace metrics;

constexpr int kCardW = 204;
constexpr int kPresetW = 104;
constexpr int kRowH = 78;
constexpr int kMoveW = 120;
constexpr int kRailW = 60;
constexpr int kPresetsX = kPad + kCardW + kGap;
constexpr int kMoveX = kPresetsX + 2 * kPresetW + kGap + kGap;
constexpr int kRailX = kMoveX + kMoveW + kGap;
constexpr int kContentH = kScreenH - 2 * kPad;
constexpr int kGaugeW = kCardW - 28;
constexpr uint32_t kMoveArmMs = 150;      // hold time before up/down starts driving
constexpr uint32_t kMinTapMs = 60;        // shorter "taps" are treated as touch glitches

class HomeScreen : public Screen {
public:
    void build(lv_obj_t *screen) override;
    void update(const app::State &s) override;

private:
    struct PresetButton {
        lv_obj_t *btn = nullptr;
        lv_obj_t *icon = nullptr;
        lv_obj_t *index = nullptr;
        lv_obj_t *value = nullptr;
        HoldToConfirm hold;
    };

    void build_height_card(lv_obj_t *parent);
    void build_presets(lv_obj_t *parent);
    void build_move_buttons(lv_obj_t *parent);
    void build_rail(lv_obj_t *parent);
    void attach_hold_to_move(lv_obj_t *btn, app::Motion dir);
    void update_gauge(const app::State &s);

    // Height card
    lv_obj_t *status_dot_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *wifi_icon_ = nullptr;
    lv_obj_t *ha_icon_ = nullptr;
    lv_obj_t *height_label_ = nullptr;
    lv_obj_t *unit_label_ = nullptr;
    lv_obj_t *sub_icon_ = nullptr;
    lv_obj_t *sub_label_ = nullptr;
    lv_obj_t *gauge_ = nullptr;
    lv_obj_t *ticks_[app::kPresetCount] = {};

    PresetButton presets_[app::kPresetCount];
    lv_obj_t *up_btn_ = nullptr;
    lv_obj_t *down_btn_ = nullptr;
    lv_obj_t *stop_btn_ = nullptr;
    lv_obj_t *lock_btn_ = nullptr;
    lv_obj_t *lock_icon_ = nullptr;
    HoldToConfirm unlock_hold_;
    lv_obj_t *lights_btn_ = nullptr;
    lv_obj_t *lights_icon_ = nullptr;

    uint32_t lights_icon_rgb_ = 0xFFFFFFFF;
    int32_t unit_x_ = -1;
    int32_t sub_x_ = 20;
    app::Motion holding_ = app::Motion::Idle;
    uint32_t hold_started_ms_ = 0;
    uint32_t preset_pressed_ms_ = 0;
    bool locked_ = false;
};

void HomeScreen::build(lv_obj_t *screen)
{
    root_ = screen;
    lv_obj_set_scrollable(screen, false);
    build_height_card(screen);
    build_presets(screen);
    build_move_buttons(screen);
    build_rail(screen);
}

void HomeScreen::build_height_card(lv_obj_t *parent)
{
    Styles &st = theme::styles();
    lv_obj_t *card = make_card(parent, kCardW, kContentH);
    lv_obj_set_pos(card, kPad, kPad);
    lv_obj_set_style_pad_hor(card, 14, 0);
    lv_obj_set_style_pad_top(card, 12, 0);
    lv_obj_set_style_pad_bottom(card, 14, 0);

    status_dot_ = make_dot(card, 8);
    lv_obj_align(status_dot_, LV_ALIGN_TOP_LEFT, 0, 5);
    status_label_ = make_label(card, font::caption(), &st.text_muted, "");
    lv_obj_align(status_label_, LV_ALIGN_TOP_LEFT, 14, 0);

    wifi_icon_ = make_label(card, font::icon_sm(), &st.text_faint, UI_ICON_WIFI_OFF);
    lv_obj_align(wifi_icon_, LV_ALIGN_TOP_RIGHT, 0, 0);
    ha_icon_ = make_label(card, font::icon_sm(), &st.text_faint, UI_ICON_HOME_IOT);
    lv_obj_align(ha_icon_, LV_ALIGN_TOP_RIGHT, -24, 0);

    height_label_ = make_label(card, font::display(), &st.text_primary, "--.-");
    lv_obj_align(height_label_, LV_ALIGN_TOP_LEFT, -3, 20);
    unit_label_ = make_label(card, font::title(), &st.text_muted, "cm");
    lv_obj_align_to(unit_label_, height_label_, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -14);

    sub_icon_ = make_label(card, font::icon_sm(), &st.text_accent, "");
    lv_obj_align(sub_icon_, LV_ALIGN_TOP_LEFT, -2, 101);
    sub_label_ = make_label(card, font::caption(), &st.text_muted, "");
    lv_obj_set_width(sub_label_, kGaugeW - 20);
    lv_label_set_long_mode(sub_label_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(sub_label_, LV_ALIGN_TOP_LEFT, 20, 102);

    gauge_ = lv_bar_create(card);
    lv_obj_set_size(gauge_, kGaugeW, 6);
    lv_obj_align(gauge_, LV_ALIGN_BOTTOM_LEFT, 0, -2);
    lv_bar_set_range(gauge_, 0, 1000);
    lv_obj_set_clickable(gauge_, false);

    for (int i = 0; i < app::kPresetCount; i++) {
        ticks_[i] = make_box(card);
        lv_obj_add_style(ticks_[i], &st.tick, 0);
        lv_obj_set_size(ticks_[i], 2, 12);
        lv_obj_set_hidden(ticks_[i], true);
    }
}

void HomeScreen::build_presets(lv_obj_t *parent)
{
    Styles &st = theme::styles();
    for (int i = 0; i < app::kPresetCount; i++) {
        PresetButton &p = presets_[i];
        const int col = i % 2, row = i / 2;
        p.btn = make_button(parent, kPresetW, kRowH);
        lv_obj_set_pos(p.btn, kPresetsX + col * (kPresetW + kGap), kPad + row * (kRowH + kGap));

        p.index = make_label(p.btn, font::caption(), &st.text_faint, "");
        lv_label_set_text_fmt(p.index, "%d", i + 1);
        lv_obj_align(p.index, LV_ALIGN_TOP_LEFT, 10, 7);

        p.icon = make_label(p.btn, font::icon_lg(), nullptr, "");
        lv_obj_align(p.icon, LV_ALIGN_CENTER, 0, -9);

        p.value = make_label(p.btn, font::caption(), &st.text_muted, "");
        lv_obj_align(p.value, LV_ALIGN_BOTTOM_MID, 0, -9);

        on_event(p.btn, LV_EVENT_PRESSED, [this](lv_event_t *) { preset_pressed_ms_ = lv_tick_get(); });
        on_event(p.btn, LV_EVENT_SHORT_CLICKED, [this, i](lv_event_t *) {
            if (lv_tick_elaps(preset_pressed_ms_) < kMinTapMs) {
                return;
            }
            // During an automatic move any preset tap stops the desk; the
            // app enforces that (and the other safety rules) itself.
            if (ctx().state.desk.target_preset >= 0) {
                ctx().app->stop();
            } else {
                ctx().app->goto_preset(i);
            }
        });
        // Saving is refused (with a hint) by the app while the child lock is on.
        p.hold.attach(p.btn, 550, 1100, [i]() { ctx().app->save_preset(i); }, 14);
    }
}

void HomeScreen::attach_hold_to_move(lv_obj_t *btn, app::Motion dir)
{
    // Safety: sliding the finger off the button must stop the desk, so the
    // button may not stay pressed once the touch leaves it.
    lv_obj_set_press_lock(btn, false);
    // Motion starts only after the finger has been down for kMoveArmMs: a
    // brief touch glitch must never drive the desk.
    on_event(btn, LV_EVENT_PRESSED, [this, dir](lv_event_t *) {
        holding_ = dir;
        hold_started_ms_ = lv_tick_get();
    });
    on_event(btn, LV_EVENT_PRESSING, [this, dir](lv_event_t *) {
        if (holding_ == dir && lv_tick_elaps(hold_started_ms_) >= kMoveArmMs) {
            ctx().app->move(dir);  // refresh: the controller auto-releases otherwise
            keep_awake();
        }
    });
    auto release = [this](lv_event_t *) {
        if (holding_ != app::Motion::Idle) {
            holding_ = app::Motion::Idle;
            ctx().app->move(app::Motion::Idle);
        }
    };
    on_event(btn, LV_EVENT_RELEASED, release);
    on_event(btn, LV_EVENT_PRESS_LOST, release);
}

void HomeScreen::build_move_buttons(lv_obj_t *parent)
{
    Styles &st = theme::styles();
    up_btn_ = make_button(parent, kMoveW, kRowH);
    lv_obj_set_pos(up_btn_, kMoveX, kPad);
    down_btn_ = make_button(parent, kMoveW, kRowH);
    lv_obj_set_pos(down_btn_, kMoveX, kPad + kRowH + kGap);

    for (auto [btn, glyph, dir] : {std::tuple{up_btn_, UI_ICON_ARROW_UP, app::Motion::Up},
                                   std::tuple{down_btn_, UI_ICON_ARROW_DOWN, app::Motion::Down}}) {
        lv_obj_add_style(btn, &st.button_accent, LV_STATE_PRESSED);
        lv_obj_t *icon = make_label(btn, font::icon_xl(), nullptr, glyph);
        lv_obj_center(icon);
        attach_hold_to_move(btn, dir);
    }

    // Shown instead of up/down while the desk drives to a preset.
    stop_btn_ = make_button(parent, kMoveW, kContentH);
    lv_obj_set_pos(stop_btn_, kMoveX, kPad);
    lv_obj_add_style(stop_btn_, &st.button_danger, 0);
    lv_obj_add_style(stop_btn_, &st.button_danger_pressed, LV_STATE_PRESSED);
    lv_obj_t *stop_icon = make_label(stop_btn_, font::icon_xl(), nullptr, UI_ICON_STOP);
    lv_obj_align(stop_icon, LV_ALIGN_CENTER, 0, -12);
    lv_obj_t *stop_text = make_label(stop_btn_, font::title(), nullptr, "Stop");
    lv_obj_align(stop_text, LV_ALIGN_CENTER, 0, 26);
    lv_obj_set_hidden(stop_btn_, true);
    on_event(stop_btn_, LV_EVENT_PRESSED, [](lv_event_t *) { ctx().app->stop(); });
}

void HomeScreen::build_rail(lv_obj_t *parent)
{
    const int h = (kContentH - 2 * kGap) / 3;
    const int ys[3] = {kPad, kPad + h + kGap, kScreenH - kPad - h};
    const int hs[3] = {h, ys[2] - kGap - ys[1], h};

    lock_btn_ = make_button(parent, kRailW, hs[0]);
    lv_obj_set_pos(lock_btn_, kRailX, ys[0]);
    lock_icon_ = make_label(lock_btn_, font::icon_md(), nullptr, UI_ICON_LOCK_OPEN);
    lv_obj_center(lock_icon_);
    on_event(lock_btn_, LV_EVENT_SHORT_CLICKED, [this](lv_event_t *) {
        if (!locked_) {
            ctx().app->set_child_lock(true);
        } else if (!unlock_hold_.consumed()) {
            toast(UI_ICON_LOCK, "Hold the lock button to unlock");
        }
    });
    unlock_hold_.attach(
        lock_btn_, 150, 1200,
        [this]() {
            if (locked_) {
                ctx().app->set_child_lock(false);
            }
        },
        8);

    lights_btn_ = make_button(parent, kRailW, hs[1]);
    lv_obj_set_pos(lights_btn_, kRailX, ys[1]);
    lights_icon_ = make_label(lights_btn_, font::icon_md(), nullptr, UI_ICON_LIGHTBULB);
    lv_obj_center(lights_icon_);
    on_event(lights_btn_, LV_EVENT_CLICKED, [](lv_event_t *) { navigate(Page::Lights); });

    lv_obj_t *settings = make_button(parent, kRailW, hs[2]);
    lv_obj_set_pos(settings, kRailX, ys[2]);
    lv_obj_t *gear = make_label(settings, font::icon_md(), nullptr, UI_ICON_SETTINGS);
    lv_obj_center(gear);
    on_event(settings, LV_EVENT_CLICKED, [](lv_event_t *) { navigate(Page::Settings); });
}

void HomeScreen::update_gauge(const app::State &s)
{
    const float lo = s.settings.range_min_cm;
    const float hi = std::max(s.settings.range_max_cm, lo + 1.0f);
    auto to_permille = [&](float cm) {
        return static_cast<int32_t>(std::clamp((cm - lo) / (hi - lo), 0.0f, 1.0f) * 1000.0f);
    };
    lv_bar_set_value(gauge_, s.desk.height_valid ? to_permille(s.desk.height_cm) : 0, LV_ANIM_OFF);

    const int32_t bar_x = lv_obj_get_x(gauge_);
    const int32_t bar_y = lv_obj_get_y(gauge_);
    for (int i = 0; i < app::kPresetCount; i++) {
        const float h = s.settings.presets[i].height_cm;
        lv_obj_set_hidden(ticks_[i], h <= 0);
        if (h > 0) {
            const int32_t x = bar_x + to_permille(h) * (kGaugeW - 2) / 1000;
            if (lv_obj_get_x(ticks_[i]) != x || lv_obj_get_y(ticks_[i]) != bar_y - 3) {
                lv_obj_set_pos(ticks_[i], x, bar_y - 3);
            }
        }
    }
}

void HomeScreen::update(const app::State &s)
{
    Styles &st = theme::styles();
    const app::DeskState &d = s.desk;
    char buf[48];

    // --- Status line ---------------------------------------------------------------
    lv_style_t *dot = &st.dot_positive;
    const char *status = "Connected";
    if (d.link == app::DeskLink::Offline) {
        dot = &st.dot_danger;
        status = "Desk offline";
    } else if (d.link == app::DeskLink::Demo) {
        dot = &st.dot_warning;
        status = "Demo mode";
    }
    for (lv_style_t *candidate : {&st.dot_positive, &st.dot_warning, &st.dot_danger}) {
        set_style_if(status_dot_, candidate, candidate == dot);
    }
    set_label_text_if_changed(status_label_, status);

    const bool wifi_up = s.net.wifi == app::WifiStatus::Connected;
    set_label_text_if_changed(wifi_icon_, wifi_up ? UI_ICON_WIFI : UI_ICON_WIFI_OFF);
    set_text_style(wifi_icon_, wifi_up ? &st.text_muted : &st.text_faint);
    lv_obj_set_hidden(wifi_icon_, s.net.wifi == app::WifiStatus::Disabled);
    set_text_style(ha_icon_, s.net.mqtt_connected ? &st.text_muted : &st.text_faint);
    lv_obj_set_hidden(ha_icon_, !s.net.mqtt_enabled);

    // --- Height ------------------------------------------------------------------------
    if (d.height_valid) {
        format_height(d.height_cm, s.settings.units, buf, sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    set_label_text_if_changed(height_label_, buf);
    set_text_style(height_label_, d.link == app::DeskLink::Offline ? &st.text_faint : &st.text_primary);
    set_label_text_if_changed(unit_label_, app::unit_suffix(s.settings.units));
    lv_obj_update_layout(height_label_);
    const int32_t unit_x = lv_obj_get_x(height_label_) + lv_obj_get_width(height_label_) + 4;
    if (unit_x != unit_x_) {  // re-aligning restyles the label, so only when it moved
        unit_x_ = unit_x;
        lv_obj_align_to(unit_label_, height_label_, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -14);
    }

    // --- Sub line: what is the desk doing? ------------------------------------------------
    const char *sub_icon = "";
    lv_style_t *sub_style = &st.text_accent;
    buf[0] = '\0';
    if (d.target_preset >= 0) {
        sub_icon = d.motion == app::Motion::Down ? UI_ICON_ARROW_DOWN : UI_ICON_ARROW_UP;
        snprintf(buf, sizeof(buf), "To %s", preset_display_name(s.settings, d.target_preset));
    } else if (d.motion != app::Motion::Idle) {
        sub_icon = d.motion == app::Motion::Up ? UI_ICON_ARROW_UP : UI_ICON_ARROW_DOWN;
        snprintf(buf, sizeof(buf), d.motion == app::Motion::Up ? "Moving up" : "Moving down");
    } else if (d.box == app::BoxState::Locked) {
        sub_icon = UI_ICON_LOCK;
        sub_style = &st.text_warning;
        snprintf(buf, sizeof(buf), "Desk locked (LoC)");
    } else if (d.box == app::BoxState::Error || d.box == app::BoxState::Reset) {
        sub_icon = UI_ICON_ERROR;
        sub_style = &st.text_danger;
        snprintf(buf, sizeof(buf), d.box == app::BoxState::Reset ? "Reset required" : "Error %s", d.display);
    } else if (d.box == app::BoxState::SaveMode) {
        sub_icon = UI_ICON_SAVE;
        snprintf(buf, sizeof(buf), "Saving position");
    } else if (d.link == app::DeskLink::Offline) {
        sub_icon = UI_ICON_LINK_OFF;
        sub_style = &st.text_faint;
        snprintf(buf, sizeof(buf), "Check desk cable");
    } else if (d.at_preset >= 0) {
        sub_icon = preset_icon_glyph(s.settings.presets[d.at_preset].icon);
        if (s.settings.presets[d.at_preset].icon == app::PresetIcon::Number) {
            sub_icon = UI_ICON_CHECK;
        }
        snprintf(buf, sizeof(buf), "%s", preset_display_name(s.settings, d.at_preset));
    } else if (s.settings.reminder_min > 0) {
        sub_icon = UI_ICON_TIMER;
        sub_style = &st.text_muted;
        char dur[16];
        format_duration_short(s.reminder.elapsed_s, dur, sizeof(dur));
        snprintf(buf, sizeof(buf), "%s %s", s.reminder.standing ? "Standing" : "Sitting", dur);
    }
    set_label_text_if_changed(sub_icon_, sub_icon);
    set_text_style(sub_icon_, sub_style);
    set_label_text_if_changed(sub_label_, buf);
    const int32_t sub_x = sub_icon[0] ? 20 : 0;
    if (sub_x != sub_x_) {
        sub_x_ = sub_x;
        lv_obj_align(sub_label_, LV_ALIGN_TOP_LEFT, sub_x, 102);
    }

    update_gauge(s);

    // --- Presets ---------------------------------------------------------------------------
    locked_ = s.settings.child_lock;
    const bool auto_moving = d.target_preset >= 0;
    for (int i = 0; i < app::kPresetCount; i++) {
        PresetButton &p = presets_[i];
        const app::Preset &preset = s.settings.presets[i];
        if (preset.icon == app::PresetIcon::Number) {
            snprintf(buf, sizeof(buf), "%d", i + 1);
            set_font_if_changed(p.icon, font::value());
            set_label_text_if_changed(p.icon, buf);
            lv_obj_set_hidden(p.index, true);
        } else {
            set_font_if_changed(p.icon, font::icon_lg());
            set_label_text_if_changed(p.icon, preset_icon_glyph(preset.icon));
            lv_obj_set_hidden(p.index, false);
        }
        if (preset.height_cm > 0) {
            format_height(preset.height_cm, s.settings.units, buf, sizeof(buf));
            char with_unit[24];
            snprintf(with_unit, sizeof(with_unit), "%s %s", buf, app::unit_suffix(s.settings.units));
            set_label_text_if_changed(p.value, with_unit);
            set_text_style(p.value, &st.text_muted);
        } else {
            set_label_text_if_changed(p.value, "Hold to save");
            set_text_style(p.value, &st.text_faint);
        }
        const bool active = auto_moving ? d.target_preset == i : d.at_preset == i;
        lv_obj_set_checked(p.btn, active);
        set_text_style(p.icon, active ? &st.text_accent : &st.text_primary);
        set_style_if(p.btn, &st.button_disabled, locked_);
    }

    // --- Movement ------------------------------------------------------------------------------
    lv_obj_set_hidden(stop_btn_, !auto_moving);
    lv_obj_set_hidden(up_btn_, auto_moving);
    lv_obj_set_hidden(down_btn_, auto_moving);
    set_style_if(up_btn_, &st.button_disabled, locked_);
    set_style_if(down_btn_, &st.button_disabled, locked_);
    if (d.motion != app::Motion::Idle) {
        keep_awake();
    }

    // --- Rail --------------------------------------------------------------------------------------
    set_label_text_if_changed(lock_icon_, locked_ ? UI_ICON_LOCK : UI_ICON_LOCK_OPEN);
    set_style_if(lock_btn_, &st.button_warning, locked_);

    const bool lights_on = s.settings.lights.on;
    set_label_text_if_changed(lights_icon_, lights_on ? UI_ICON_LIGHTBULB : UI_ICON_LIGHT_OFF);
    const app::LightSettings &l = s.settings.lights;
    const uint32_t light_rgb = lights_on ? (l.r << 16 | l.g << 8 | l.b) : 0xFF000000;
    if (light_rgb != lights_icon_rgb_) {
        lights_icon_rgb_ = light_rgb;
        if (lights_on) {
            lv_obj_set_style_text_color(lights_icon_, lv_color_make(l.r, l.g, l.b), 0);
        } else {
            lv_obj_remove_local_style_prop(lights_icon_, LV_STYLE_TEXT_COLOR, 0);
        }
    }
}

}  // namespace

Screen *create_home_screen() { return new HomeScreen(); }

}  // namespace ui
