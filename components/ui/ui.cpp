// UI manager: screen navigation, state sync, notifications and display power.
#include <cmath>
#include <cstring>

#include "ui_internal.h"

namespace ui {

namespace {

constexpr uint32_t kSyncPeriodMs = 40;
constexpr uint32_t kPowerPeriodMs = 200;
constexpr uint32_t kReturnHomeAfterMs = 90 * 1000;  // secondary screens time out
constexpr uint8_t kDimPercentOfBrightness = 25;

enum class Power : uint8_t { Active, Dimmed, Asleep };

Context s_ctx;
Screen *s_screens[3] = {};
lv_obj_t *s_screen_objs[3] = {};
Page s_page = Page::Home;
uint32_t s_version = 0;
Power s_power = Power::Active;
bool s_swallow_touch = false;
uint8_t s_applied_backlight = 255;
bool s_applied_rotation = false;
bool s_rotation_known = false;

int page_index(Page p) { return static_cast<int>(p); }

void apply_backlight(uint8_t percent)
{
    if (percent != s_applied_backlight) {
        s_applied_backlight = percent;
        s_ctx.hal->set_backlight(percent);
    }
}

uint8_t active_brightness() { return std::max<uint8_t>(s_ctx.state.settings.brightness, 5); }

void wake()
{
    lv_display_trigger_activity(s_ctx.display);
    if (s_power != Power::Active) {
        s_power = Power::Active;
        apply_backlight(active_brightness());
    }
}

void handle_event(const app::Event &ev)
{
    const app::Settings &settings = s_ctx.state.settings;
    char text[96];
    switch (ev.type) {
    case app::EventType::PresetSaved: {
        char h[16] = "";
        if (s_ctx.state.desk.height_valid) {
            format_height(s_ctx.state.desk.height_cm, settings.units, h, sizeof(h));
        }
        snprintf(text, sizeof(text), "Saved %s  %s %s", preset_display_name(settings, ev.arg), h,
                 h[0] ? app::unit_suffix(settings.units) : "");
        toast(UI_ICON_SAVE, text);
        break;
    }
    case app::EventType::Locked:
        toast(UI_ICON_LOCK, "Child lock on");
        break;
    case app::EventType::Unlocked:
        toast(UI_ICON_LOCK_OPEN, "Child lock off");
        break;
    case app::EventType::BlockedByLock:
        toast(UI_ICON_LOCK, "Locked - hold the lock button to unlock");
        break;
    case app::EventType::PresetUnknown:
        snprintf(text, sizeof(text), "%s didn't move the desk - hold it to save here",
                 preset_display_name(settings, ev.arg));
        toast(UI_ICON_INFO, text, 3000);
        break;
    case app::EventType::DeskOffline:
        toast(UI_ICON_LINK_OFF, "Desk disconnected");
        break;
    case app::EventType::DeskOnline:
        toast(UI_ICON_LINK, "Desk connected");
        break;
    case app::EventType::ReminderDue: {
        const bool standing = s_ctx.state.reminder.standing;
        int target = -1;
        for (int i = 0; i < app::kPresetCount; i++) {
            const app::PresetIcon want = standing ? app::PresetIcon::Sit : app::PresetIcon::Stand;
            if (settings.presets[i].icon == want && settings.presets[i].height_cm > 0) {
                target = i;
                break;
            }
        }
        char dur[16];
        format_duration_short(s_ctx.state.reminder.elapsed_s, dur, sizeof(dur));
        snprintf(text, sizeof(text), "You've been %s for %s.", standing ? "standing" : "sitting", dur);
        static char action[32];
        snprintf(action, sizeof(action), target >= 0 ? (standing ? "Sit down" : "Stand up") : "Done");
        const ModalButton buttons[] = {
            {"Snooze 10 min", []() { ctx().app->acknowledge_reminder(true); }},
            {action,
             [target]() {
                 ctx().app->acknowledge_reminder(false);
                 if (target >= 0) {
                     ctx().app->goto_preset(target);
                 }
             }},
        };
        modal(standing ? UI_ICON_CHAIR : UI_ICON_STAND, standing ? "Time to sit down" : "Time to stand up", text,
              buttons, 2);
        break;
    }
    case app::EventType::DeskError: {
        snprintf(text, sizeof(text), "The control box shows %s. Check the cables, then power-cycle the desk.",
                 s_ctx.state.desk.display);
        const ModalButton ok[] = {{"OK", nullptr}};
        modal(UI_ICON_ERROR, "Desk error", text, ok, 1, Tone::Danger);
        break;
    }
    case app::EventType::DeskReset: {
        const ModalButton ok[] = {{"OK", nullptr}};
        modal(UI_ICON_WARNING, "Desk needs a reset",
              "Hold the down button until the desk reaches its lowest point and bounces back.", ok, 1,
              Tone::Warning);
        break;
    }
    case app::EventType::BoxLocked: {
        const ModalButton buttons[] = {
            {"Cancel", nullptr},
            {"Unlock desk",
             []() {
                 if (ctx().app->toggle_box_lock()) {
                     toast(UI_ICON_LOCK_OPEN, "Unlocking the desk (5 s)...", 5500);
                 }
             }},
        };
        modal(UI_ICON_LOCK, "Desk is locked",
              "The desk's own child lock (LoC) is on. Unlocking holds M on the control box for 5 seconds.",
              buttons, 2, Tone::Warning);
        break;
    }
    case app::EventType::Busy:
        toast(UI_ICON_HOURGLASS, ev.arg == app::BusySavingPreset ? "Saving a preset - try again in a moment"
                                                                 : "Stop the desk first");
        break;
    case app::EventType::MovingToPreset:
        break;
    }
}

void sync_timer_cb(lv_timer_t *)
{
    app::Event ev;
    bool had_event = false;
    const uint32_t v = s_ctx.app->version();
    if (v != s_version) {
        s_version = v;
        s_ctx.app->snapshot(s_ctx.state);
        const app::Settings &st = s_ctx.state.settings;
        if (st.theme != theme::current_index()) {
            theme::apply(st.theme);
        }
        if (!s_rotation_known || st.rotate_180 != s_applied_rotation) {
            s_rotation_known = true;
            s_applied_rotation = st.rotate_180;
            s_ctx.hal->set_rotation_180(st.rotate_180);
        }
        if (s_power == Power::Active) {
            apply_backlight(active_brightness());
        }
        for (Screen *screen : s_screens) {
            if (screen != nullptr) {
                screen->update(s_ctx.state);
            }
        }
    }
    while (s_ctx.app->pop_event(ev)) {
        had_event = true;
        handle_event(ev);
    }
    if (had_event || s_ctx.state.reminder.due) {
        wake();
    }
}

void power_timer_cb(lv_timer_t *)
{
    const app::Settings &st = s_ctx.state.settings;
    const uint32_t idle_ms = lv_display_get_inactive_time(s_ctx.display);

    Power target = Power::Active;
    if (st.sleep_after_s > 0 && idle_ms >= st.sleep_after_s * 1000UL) {
        target = Power::Asleep;
    } else if (st.dim_after_s > 0 && idle_ms >= st.dim_after_s * 1000UL) {
        target = Power::Dimmed;
    }
    if (target != s_power) {
        s_power = target;
        switch (target) {
        case Power::Active:
            apply_backlight(active_brightness());
            break;
        case Power::Dimmed:
            apply_backlight(std::max<uint8_t>(3, active_brightness() * kDimPercentOfBrightness / 100));
            break;
        case Power::Asleep:
            apply_backlight(0);
            modal_close();
            if (s_page != Page::Home) {
                navigate(Page::Home);
            }
            break;
        }
    }
    if (s_page != Page::Home && idle_ms >= kReturnHomeAfterMs && !modal_open()) {
        navigate(Page::Home);
    }
}

}  // namespace

Context &ctx() { return s_ctx; }

void keep_awake() { wake(); }

void navigate(Page page)
{
    const int idx = page_index(page);
    if (s_screens[idx] == nullptr) {
        return;
    }
    const bool forward = page != Page::Home;
    s_page = page;
    s_screens[idx]->update(s_ctx.state);
    s_screens[idx]->on_show();
    // Instant switch: a slide is only ~3 full-frame redraws on this panel and
    // looks choppier than no animation at all.
    (void)forward;
    lv_screen_load(s_screen_objs[idx]);
}

void show(Page page) { navigate(page); }

void init(lv_display_t *disp, app::App &app, Hal &hal)
{
    s_ctx.display = disp;
    s_ctx.app = &app;
    s_ctx.hal = &hal;
    app.snapshot(s_ctx.state);
    s_version = app.version();

    theme::init(disp, s_ctx.state.settings.theme);

    s_screens[page_index(Page::Home)] = create_home_screen();
    s_screens[page_index(Page::Settings)] = create_settings_screen();
    s_screens[page_index(Page::Lights)] = create_lights_screen();
    for (int i = 0; i < 3; i++) {
        s_screen_objs[i] = i == 0 ? lv_screen_active() : lv_obj_create(nullptr);
        if (i == 0) {
            // The default screen predates the theme; style it explicitly.
            lv_obj_add_style(s_screen_objs[i], &theme::styles().screen, 0);
            lv_obj_add_style(s_screen_objs[i], &theme::styles().scrollbar, LV_PART_SCROLLBAR);
        }
        s_screens[i]->build(s_screen_objs[i]);
        s_screens[i]->update(s_ctx.state);
    }

    s_rotation_known = true;
    s_applied_rotation = s_ctx.state.settings.rotate_180;
    apply_backlight(active_brightness());

    lv_timer_create(sync_timer_cb, kSyncPeriodMs, nullptr);
    lv_timer_create(power_timer_cb, kPowerPeriodMs, nullptr);
}

bool touch_gate(bool pressed)
{
    if (s_swallow_touch) {
        if (!pressed) {
            s_swallow_touch = false;
        }
        return false;
    }
    // A dimmed or sleeping screen only wakes on the first touch. Besides
    // avoiding accidental presses, this contains the AXS15231B's phantom
    // touches, which on this board only occur while the backlight is dimmed
    // (low PWM duty): at worst a phantom brightens the screen.
    if (pressed && s_power != Power::Active) {
        wake();
        s_swallow_touch = true;
        return false;
    }
    return true;
}

// --- Formatting & shared widgets ------------------------------------------------------

void format_length(float cm, app::Units units, char *buf, size_t len)
{
    snprintf(buf, len, "%.1f", static_cast<double>(units == app::Units::Imperial ? cm / app::kCmPerInch : cm));
}

void format_height(float cm, app::Units units, char *buf, size_t len)
{
    if (units == app::Units::Imperial) {
        snprintf(buf, len, "%.1f", static_cast<double>(cm / app::kCmPerInch));
    } else if (cm < 99.95f) {
        snprintf(buf, len, "%.1f", static_cast<double>(cm));
    } else {
        snprintf(buf, len, "%d", static_cast<int>(std::lround(cm)));
    }
}

const char *preset_icon_glyph(app::PresetIcon icon)
{
    switch (icon) {
    case app::PresetIcon::Sit:
        return UI_ICON_CHAIR;
    case app::PresetIcon::Stand:
        return UI_ICON_STAND;
    case app::PresetIcon::Walk:
        return UI_ICON_WALK;
    case app::PresetIcon::Focus:
        return UI_ICON_BOLT;
    default:
        return UI_ICON_HEIGHT;
    }
}

const char *preset_display_name(const app::Settings &s, int index)
{
    if (index < 0 || index >= app::kPresetCount) {
        return "";
    }
    return s.presets[index].name[0] ? s.presets[index].name : "Preset";
}

void format_duration_short(uint32_t seconds, char *buf, size_t len)
{
    if (seconds < 60) {
        snprintf(buf, len, "<1 min");
    } else if (seconds < 3600) {
        snprintf(buf, len, "%u min", static_cast<unsigned>(seconds / 60));
    } else {
        snprintf(buf, len, "%uh %02um", static_cast<unsigned>(seconds / 3600), static_cast<unsigned>(seconds / 60 % 60));
    }
}

lv_obj_t *make_header(lv_obj_t *parent, const char *title)
{
    using namespace metrics;
    lv_obj_t *back = make_button(parent, 52, 40);
    lv_obj_set_pos(back, kPad, kPad);
    lv_obj_t *icon = make_label(back, font::icon_md(), nullptr, UI_ICON_BACK);
    lv_obj_center(icon);
    on_event(back, LV_EVENT_CLICKED, [](lv_event_t *) { navigate(Page::Home); });

    lv_obj_t *label = make_label(parent, font::title(), nullptr, title);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, kPad + 52 + 14, kPad + 9);
    return back;
}

}  // namespace ui
