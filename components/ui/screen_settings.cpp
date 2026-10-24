// Settings screen: header with back button and tabs, scrollable pages below.
#include <cmath>
#include <cstring>

#include "ui_internal.h"

namespace ui {

namespace {

using namespace metrics;

constexpr int kHeaderH = 40;
constexpr int kContentY = kPad + kHeaderH + 6;
constexpr int kContentW = kScreenW - 2 * kPad;
constexpr int kRowH = 56;

enum Tab { TabDisplay, TabDesk, TabPresets, TabNetwork, TabAbout, TabCount };
const char *const kTabNames[TabCount] = {"Display", "Desk", "Presets", "Network", "About"};

const uint16_t kDimOptions[] = {15, 30, 60, 300, 0};
const char *const kDimLabels[] = {"15 s", "30 s", "1 min", "5 min", "Never"};
const uint16_t kSleepOptions[] = {120, 300, 600, 1800, 0};
const char *const kSleepLabels[] = {"2 min", "5 min", "10 min", "30 min", "Never"};
const uint16_t kReminderOptions[] = {0, 30, 45, 60, 90};
const char *const kReminderLabels[] = {"Off", "30 min", "45 min", "60 min", "90 min"};
const char *const kUnitLabels[] = {"Centimetres", "Inches"};
const char *const kPresetNames[] = {"Sit", "Stand", "Focus", "Relax", "Walk", "Perch", "Low", "High"};

int index_of(const uint16_t *options, int count, uint16_t value)
{
    for (int i = 0; i < count; i++) {
        if (options[i] == value) {
            return i;
        }
    }
    return -1;
}

// A settings row: rounded surface, icon, title + optional subtitle, and a
// right-aligned slot for a control.
struct Row {
    lv_obj_t *obj;
    lv_obj_t *icon;
    lv_obj_t *title;
    lv_obj_t *subtitle;
};

Row make_row(lv_obj_t *parent, const char *icon, const char *title, const char *subtitle, int h = kRowH)
{
    Styles &st = theme::styles();
    Row r{};
    r.obj = make_card(parent, kContentW, h);
    lv_obj_set_style_radius(r.obj, 12, 0);
    lv_obj_set_style_pad_hor(r.obj, 14, 0);
    lv_obj_set_style_pad_ver(r.obj, 0, 0);
    lv_obj_set_scroll_chain(r.obj, true);
    lv_obj_set_event_bubble(r.obj, true);

    r.icon = make_label(r.obj, font::icon_md(), &st.text_muted, icon);
    lv_obj_align(r.icon, LV_ALIGN_LEFT_MID, 0, 0);
    r.title = make_label(r.obj, font::body(), nullptr, title);
    if (subtitle != nullptr) {
        lv_obj_align(r.title, LV_ALIGN_LEFT_MID, 38, -10);
        r.subtitle = make_label(r.obj, font::caption(), &st.text_muted, subtitle);
        lv_obj_align(r.subtitle, LV_ALIGN_LEFT_MID, 38, 11);
    } else {
        lv_obj_align(r.title, LV_ALIGN_LEFT_MID, 38, 0);
    }
    return r;
}

lv_obj_t *make_small_button(lv_obj_t *parent, const char *text, int w = LV_SIZE_CONTENT)
{
    lv_obj_t *btn = make_button(parent, w, 38);
    lv_obj_set_style_radius(btn, kRadiusSmall, 0);
    lv_obj_set_style_pad_hor(btn, 16, 0);
    lv_obj_add_style(btn, &theme::styles().button_secondary, 0);
    lv_obj_add_style(btn, &theme::styles().button_secondary_pressed, LV_STATE_PRESSED);
    lv_obj_t *label = make_label(btn, font::caption(), nullptr, text);
    lv_obj_center(label);
    return btn;
}

class SettingsScreen : public Screen {
public:
    void build(lv_obj_t *screen) override;
    void update(const app::State &s) override;
    void on_show() override { select_tab(tab_); }

private:
    void select_tab(int tab);
    lv_obj_t *make_page();
    void build_display(lv_obj_t *page);
    void build_desk(lv_obj_t *page);
    void build_presets(lv_obj_t *page);
    void build_network(lv_obj_t *page);
    void build_about(lv_obj_t *page);
    void refresh_theme_swatches(int selected);

    // Applies a change to the current settings atomically.
    template <typename Fn>
    static void edit(Fn fn) { ctx().app->modify_settings(fn); }

    int tab_ = TabDisplay;
    lv_obj_t *tabs_[TabCount] = {};
    lv_obj_t *pages_[TabCount] = {};

    // Display
    lv_obj_t *swatches_[16] = {};
    int swatch_count_ = 0;
    int swatch_selected_ = -1;
    lv_obj_t *brightness_ = nullptr;
    lv_obj_t *brightness_value_ = nullptr;
    Segmented dim_, sleep_;
    lv_obj_t *rotate_ = nullptr;

    // Desk
    Segmented units_, reminder_;
    lv_obj_t *demo_ = nullptr;
    lv_obj_t *range_value_ = nullptr;
    lv_obj_t *diag_title_ = nullptr;
    lv_obj_t *diag_detail_ = nullptr;

    // Presets
    struct PresetRow {
        lv_obj_t *icon_btn;
        lv_obj_t *icon;
        lv_obj_t *name;
        lv_obj_t *value;
    } preset_rows_[app::kPresetCount] = {};

    // Network
    Row wifi_row_{}, mqtt_row_{};

    // About
    lv_obj_t *about_version_ = nullptr;
    lv_obj_t *about_detail_ = nullptr;
};

void SettingsScreen::build(lv_obj_t *screen)
{
    root_ = screen;
    lv_obj_set_scrollable(screen, false);
    make_header(screen, "");

    const int tabs_x = kPad + 52 + kGap;
    const int tab_w = (kScreenW - kPad - tabs_x - 4 * 6) / TabCount;
    for (int i = 0; i < TabCount; i++) {
        lv_obj_t *tab = make_button(screen, tab_w, kHeaderH);
        lv_obj_set_pos(tab, tabs_x + i * (tab_w + 6), kPad);
        lv_obj_set_style_border_width(tab, 0, LV_STATE_CHECKED);
        lv_obj_add_style(tab, &theme::styles().text_accent, LV_STATE_CHECKED);
        lv_obj_t *label = make_label(tab, font::body(), nullptr, kTabNames[i]);
        lv_obj_center(label);
        on_event(tab, LV_EVENT_CLICKED, [this, i](lv_event_t *) { select_tab(i); });
        tabs_[i] = tab;
    }

    for (int i = 0; i < TabCount; i++) {
        pages_[i] = make_page();
    }
    build_display(pages_[TabDisplay]);
    build_desk(pages_[TabDesk]);
    build_presets(pages_[TabPresets]);
    build_network(pages_[TabNetwork]);
    build_about(pages_[TabAbout]);
    select_tab(TabDisplay);
}

lv_obj_t *SettingsScreen::make_page()
{
    lv_obj_t *page = lv_obj_create(root_);
    lv_obj_set_pos(page, 0, kContentY);
    lv_obj_set_size(page, kScreenW, kScreenH - kContentY);
    lv_obj_set_style_pad_hor(page, kPad, 0);
    lv_obj_set_style_pad_bottom(page, kPad, 0);
    lv_obj_set_style_pad_top(page, 0, 0);
    lv_obj_set_style_pad_row(page, 6, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_ACTIVE);
    return page;
}

void SettingsScreen::select_tab(int tab)
{
    tab_ = tab;
    for (int i = 0; i < TabCount; i++) {
        lv_obj_set_checked(tabs_[i], i == tab);
        lv_obj_set_hidden(pages_[i], i != tab);
    }
    lv_obj_scroll_to_y(pages_[tab], 0, LV_ANIM_OFF);
}

// --- Display ----------------------------------------------------------------------------

void SettingsScreen::build_display(lv_obj_t *page)
{
    Styles &st = theme::styles();

    // Theme picker: each swatch previews its own palette, so it uses local
    // colours on purpose (they must not follow the active theme).
    Row theme_row = make_row(page, UI_ICON_PALETTE, "Theme", nullptr, 72);
    lv_obj_t *strip = lv_obj_create(theme_row.obj);
    lv_obj_set_size(strip, kContentW - 28 - 110, 72);
    lv_obj_align(strip, LV_ALIGN_RIGHT_MID, 14, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_pad_all(strip, 10, 0);
    lv_obj_set_style_pad_column(strip, 8, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_scroll_dir(strip, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(strip, LV_SCROLLBAR_MODE_OFF);
    swatch_count_ = std::min(palette_count(), 16);
    for (int i = 0; i < swatch_count_; i++) {
        const Palette &p = palette(i);
        lv_obj_t *sw = lv_button_create(strip);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, 86, 52);
        lv_obj_set_style_bg_color(sw, p.bg, 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(sw, 10, 0);
        lv_obj_set_style_border_color(sw, p.border, 0);
        lv_obj_set_style_border_width(sw, 2, 0);
        lv_obj_set_scroll_chain_hor(sw, true);

        lv_obj_t *chip = lv_obj_create(sw);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, 26, 12);
        lv_obj_set_style_bg_color(chip, p.accent, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(chip, 6, 0);
        lv_obj_align(chip, LV_ALIGN_TOP_LEFT, 9, 9);
        lv_obj_set_clickable(chip, false);

        lv_obj_t *surface = lv_obj_create(sw);
        lv_obj_remove_style_all(surface);
        lv_obj_set_size(surface, 18, 12);
        lv_obj_set_style_bg_color(surface, p.surface_2, 0);
        lv_obj_set_style_bg_opa(surface, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(surface, 6, 0);
        lv_obj_align(surface, LV_ALIGN_TOP_LEFT, 39, 9);
        lv_obj_set_clickable(surface, false);

        lv_obj_t *name = lv_label_create(sw);
        lv_label_set_text(name, p.name);
        lv_obj_set_style_text_font(name, font::caption(), 0);
        lv_obj_set_style_text_color(name, p.text, 0);
        lv_obj_align(name, LV_ALIGN_BOTTOM_LEFT, 9, -6);

        on_event(sw, LV_EVENT_CLICKED, [i](lv_event_t *) {
            edit([i](app::Settings &s) { s.theme = static_cast<uint8_t>(i); });
        });
        swatches_[i] = sw;
    }

    Row bright = make_row(page, UI_ICON_BRIGHTNESS, "Brightness", nullptr);
    brightness_ = lv_slider_create(bright.obj);
    lv_obj_set_size(brightness_, 330, 10);
    lv_obj_align(brightness_, LV_ALIGN_RIGHT_MID, -64, 0);
    lv_slider_set_range(brightness_, 5, 100);
    lv_obj_set_adv_hittest(brightness_, true);
    lv_obj_set_ext_click_area(brightness_, 18);
    brightness_value_ = make_label(bright.obj, font::body(), &st.text_muted, "");
    lv_obj_set_width(brightness_value_, 48);
    lv_obj_set_style_text_align(brightness_value_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(brightness_value_, LV_ALIGN_RIGHT_MID, 0, 0);
    on_event(brightness_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t *) {
        const int32_t v = lv_slider_get_value(brightness_);
        lv_label_set_text_fmt(brightness_value_, "%d%%", static_cast<int>(v));
        ctx().hal->set_backlight(static_cast<uint8_t>(v));  // live preview
    });
    on_event(brightness_, LV_EVENT_RELEASED, [this](lv_event_t *) {
        const auto v = static_cast<uint8_t>(lv_slider_get_value(brightness_));
        edit([v](app::Settings &s) { s.brightness = v; });
    });

    Row dim = make_row(page, UI_ICON_BRIGHTNESS_LOW, "Dim after", nullptr);
    dim_.create(dim.obj, kDimLabels, 5, 400, 38, [](int i) {
        edit([i](app::Settings &s) { s.dim_after_s = kDimOptions[i]; });
    });
    lv_obj_align(dim_.obj(), LV_ALIGN_RIGHT_MID, 0, 0);

    Row sleep = make_row(page, UI_ICON_BEDTIME, "Screen off", nullptr);
    sleep_.create(sleep.obj, kSleepLabels, 5, 400, 38, [](int i) {
        edit([i](app::Settings &s) { s.sleep_after_s = kSleepOptions[i]; });
    });
    lv_obj_align(sleep_.obj(), LV_ALIGN_RIGHT_MID, 0, 0);

    Row rotate = make_row(page, UI_ICON_ROTATE, "Rotate 180°", "Flip the display for upside-down mounting");
    rotate_ = lv_switch_create(rotate.obj);
    lv_obj_set_size(rotate_, 56, 30);
    lv_obj_align(rotate_, LV_ALIGN_RIGHT_MID, 0, 0);
    on_event(rotate_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t *) {
        const bool on = lv_obj_has_state(rotate_, LV_STATE_CHECKED);
        edit([on](app::Settings &s) { s.rotate_180 = on; });
    });
}

void SettingsScreen::refresh_theme_swatches(int selected)
{
    if (selected == swatch_selected_) {
        return;
    }
    swatch_selected_ = selected;
    for (int i = 0; i < swatch_count_; i++) {
        const Palette &p = palette(i);
        lv_obj_set_style_border_color(swatches_[i], i == selected ? p.accent : p.border, 0);
        lv_obj_set_style_border_width(swatches_[i], i == selected ? 3 : 2, 0);
    }
}

// --- Desk ------------------------------------------------------------------------------------

void SettingsScreen::build_desk(lv_obj_t *page)
{
    Styles &st = theme::styles();

    Row units = make_row(page, UI_ICON_STRAIGHTEN, "Units", nullptr);
    units_.create(units.obj, kUnitLabels, 2, 300, 38, [](int i) {
        edit([i](app::Settings &s) { s.units = static_cast<app::Units>(i); });
    });
    lv_obj_align(units_.obj(), LV_ALIGN_RIGHT_MID, 0, 0);

    Row reminder = make_row(page, UI_ICON_TIMER, "Reminder", "Sit / stand nudge");
    reminder_.create(reminder.obj, kReminderLabels, 5, 380, 38, [](int i) {
        edit([i](app::Settings &s) { s.reminder_min = kReminderOptions[i]; });
    });
    lv_obj_align(reminder_.obj(), LV_ALIGN_RIGHT_MID, 0, 0);

    Row demo = make_row(page, UI_ICON_DEVELOPER_BOARD, "Demo mode", "Simulated desk - turn off once the desk is wired");
    demo_ = lv_switch_create(demo.obj);
    lv_obj_set_size(demo_, 56, 30);
    lv_obj_align(demo_, LV_ALIGN_RIGHT_MID, 0, 0);
    on_event(demo_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t *) {
        const bool on = lv_obj_has_state(demo_, LV_STATE_CHECKED);
        edit([on](app::Settings &s) { s.demo_mode = on; });
    });

    Row lock = make_row(page, UI_ICON_LOCK, "Desk lock", "Toggles the control box lock (holds M for 5 s)");
    lv_obj_t *lock_btn = make_small_button(lock.obj, "Toggle");
    lv_obj_align(lock_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    on_event(lock_btn, LV_EVENT_CLICKED, [](lv_event_t *) {
        const ModalButton buttons[] = {
            {"Cancel", nullptr},
            {"Toggle lock",
             []() {
                 if (ctx().app->toggle_box_lock()) {
                     toast(UI_ICON_LOCK, "Holding M on the desk for 5 s...", 5500);
                 }
             }},
        };
        modal(UI_ICON_LOCK, "Toggle desk lock?",
              "Holds the M key for 5 seconds. The desk shows LoC while its own lock is on.", buttons, 2);
    });

    Row range = make_row(page, UI_ICON_HEIGHT, "Travel range", "Learned from the heights the desk reports");
    lv_obj_t *reset = make_small_button(range.obj, "Reset");
    lv_obj_align(reset, LV_ALIGN_RIGHT_MID, 0, 0);
    on_event(reset, LV_EVENT_CLICKED, [](lv_event_t *) { ctx().app->reset_range(); });
    range_value_ = make_label(range.obj, font::body(), &st.text_muted, "");
    lv_obj_align(range_value_, LV_ALIGN_RIGHT_MID, -96, 0);

    Row diag = make_row(page, UI_ICON_SENSORS, "Diagnostics", "", 76);
    diag_title_ = diag.title;
    diag_detail_ = diag.subtitle;
    lv_obj_align(diag.title, LV_ALIGN_TOP_LEFT, 38, 12);
    lv_label_set_long_mode(diag.subtitle, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(diag.subtitle, kContentW - 28 - 38);
    lv_obj_align(diag.subtitle, LV_ALIGN_TOP_LEFT, 38, 36);
}

// --- Presets ---------------------------------------------------------------------------------

void SettingsScreen::build_presets(lv_obj_t *page)
{
    Styles &st = theme::styles();
    for (int i = 0; i < app::kPresetCount; i++) {
        lv_obj_t *row = make_card(page, kContentW, kRowH);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_ver(row, 0, 0);
        lv_obj_set_scroll_chain(row, true);
        PresetRow &pr = preset_rows_[i];

        lv_obj_t *num = make_label(row, font::caption(), &st.text_faint, "");
        lv_label_set_text_fmt(num, "%d", i + 1);
        lv_obj_align(num, LV_ALIGN_LEFT_MID, 4, 0);

        pr.icon_btn = make_button(row, 52, 40);
        lv_obj_set_style_radius(pr.icon_btn, kRadiusSmall, 0);
        lv_obj_add_style(pr.icon_btn, &st.button_secondary, 0);
        lv_obj_add_style(pr.icon_btn, &st.button_secondary_pressed, LV_STATE_PRESSED);
        lv_obj_align(pr.icon_btn, LV_ALIGN_LEFT_MID, 22, 0);
        pr.icon = make_label(pr.icon_btn, font::icon_md(), nullptr, "");
        lv_obj_center(pr.icon);
        on_event(pr.icon_btn, LV_EVENT_CLICKED, [i](lv_event_t *) {
            const app::Settings s = ctx().app->settings();
            const auto next = static_cast<app::PresetIcon>((static_cast<int>(s.presets[i].icon) + 1) %
                                                           static_cast<int>(app::PresetIcon::Count));
            ctx().app->set_preset_meta(i, nullptr, next);
        });

        lv_obj_t *name_btn = make_button(row, 190, 40);
        lv_obj_set_style_radius(name_btn, kRadiusSmall, 0);
        lv_obj_add_style(name_btn, &st.button_secondary, 0);
        lv_obj_add_style(name_btn, &st.button_secondary_pressed, LV_STATE_PRESSED);
        lv_obj_align(name_btn, LV_ALIGN_LEFT_MID, 82, 0);
        pr.name = make_label(name_btn, font::body(), nullptr, "");
        lv_obj_align(pr.name, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_t *cycle = make_label(name_btn, font::icon_sm(), &st.text_faint, UI_ICON_SWAP_VERT);
        lv_obj_align(cycle, LV_ALIGN_RIGHT_MID, -10, 0);
        on_event(name_btn, LV_EVENT_CLICKED, [i](lv_event_t *) {
            const app::Settings s = ctx().app->settings();
            int next = 0;
            constexpr int count = sizeof(kPresetNames) / sizeof(kPresetNames[0]);
            for (int n = 0; n < count; n++) {
                if (strcmp(s.presets[i].name, kPresetNames[n]) == 0) {
                    next = n + 1;
                }
            }
            char generic[24];
            snprintf(generic, sizeof(generic), "Preset %d", i + 1);
            ctx().app->set_preset_meta(i, next < count ? kPresetNames[next] : generic, app::PresetIcon::Count);
        });

        pr.value = make_label(row, font::body(), &st.text_muted, "");
        lv_obj_align(pr.value, LV_ALIGN_LEFT_MID, 292, 0);

        lv_obj_t *save = make_small_button(row, "Save current height");
        lv_obj_align(save, LV_ALIGN_RIGHT_MID, 0, 0);
        on_event(save, LV_EVENT_CLICKED, [i](lv_event_t *) {
            const app::State &s = ctx().state;
            static char body[96];
            char h[16] = "--";
            if (s.desk.height_valid) {
                format_height(s.desk.height_cm, s.settings.units, h, sizeof(h));
            }
            snprintf(body, sizeof(body), "Store %s %s as %s on the desk?", h, app::unit_suffix(s.settings.units),
                     preset_display_name(s.settings, i));
            const ModalButton buttons[] = {
                {"Cancel", nullptr},
                {"Save", [i]() { ctx().app->save_preset(i); }},
            };
            modal(UI_ICON_SAVE, "Save preset", body, buttons, 2);
        });
    }
    lv_obj_t *hint = make_label(page, font::caption(), &theme::styles().text_faint,
                                "Tip: on the home screen, hold a preset button to save the current height.");
    lv_obj_set_style_pad_left(hint, 6, 0);
}

// --- Network ------------------------------------------------------------------------------------

void SettingsScreen::build_network(lv_obj_t *page)
{
    wifi_row_ = make_row(page, UI_ICON_WIFI, "Wi-Fi", "");
    mqtt_row_ = make_row(page, UI_ICON_HOME_IOT, "Home Assistant", "");
    lv_obj_t *hint = make_label(page, font::caption(), &theme::styles().text_faint,
                                "Set credentials over USB: wifi <ssid> <password>  |  mqtt <uri> [user] [password]");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(hint, kContentW - 12);
    lv_obj_set_style_pad_left(hint, 6, 0);
}

// --- About -----------------------------------------------------------------------------------------

void SettingsScreen::build_about(lv_obj_t *page)
{
    Row version = make_row(page, UI_ICON_INFO, "Smart Desk Controller", "");
    about_version_ = version.subtitle;
    lv_obj_t *restart = make_small_button(version.obj, "Restart");
    lv_obj_align(restart, LV_ALIGN_RIGHT_MID, 0, 0);
    on_event(restart, LV_EVENT_CLICKED, [](lv_event_t *) {
        const ModalButton buttons[] = {
            {"Cancel", nullptr},
            {"Restart", []() { ctx().hal->restart(); }},
        };
        modal(UI_ICON_RESTART, "Restart controller?", "The desk stops if it is moving.", buttons, 2);
    });

    Row system = make_row(page, UI_ICON_MEMORY, "System", "", 76);
    about_detail_ = system.subtitle;
    lv_obj_align(system.title, LV_ALIGN_TOP_LEFT, 38, 12);
    lv_label_set_long_mode(system.subtitle, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(system.subtitle, kContentW - 28 - 38);
    lv_obj_align(system.subtitle, LV_ALIGN_TOP_LEFT, 38, 36);
}

// --- Update ------------------------------------------------------------------------------------------

void SettingsScreen::update(const app::State &s)
{
    Styles &st = theme::styles();
    const app::Settings &cfg = s.settings;
    char buf[160];

    // Display
    refresh_theme_swatches(cfg.theme);
    if (!lv_obj_has_state(brightness_, LV_STATE_PRESSED)) {
        lv_slider_set_value(brightness_, cfg.brightness, LV_ANIM_OFF);
        lv_label_set_text_fmt(brightness_value_, "%d%%", cfg.brightness);
    }
    dim_.set_selected(index_of(kDimOptions, 5, cfg.dim_after_s));
    sleep_.set_selected(index_of(kSleepOptions, 5, cfg.sleep_after_s));
    lv_obj_set_checked(rotate_, cfg.rotate_180);

    // Desk
    units_.set_selected(static_cast<int>(cfg.units));
    reminder_.set_selected(index_of(kReminderOptions, 5, cfg.reminder_min));
    lv_obj_set_checked(demo_, cfg.demo_mode);
    char lo[16], hi[16];
    format_length(cfg.range_min_cm, cfg.units, lo, sizeof(lo));
    format_length(cfg.range_max_cm, cfg.units, hi, sizeof(hi));
    snprintf(buf, sizeof(buf), "%s - %s %s", lo, hi, app::unit_suffix(cfg.units));
    set_label_text_if_changed(range_value_, buf);

    const app::DeskState &d = s.desk;
    const char *link = d.link == app::DeskLink::Online ? "Online" : d.link == app::DeskLink::Demo ? "Demo" : "Offline";
    snprintf(buf, sizeof(buf), "Diagnostics  -  %s, display \"%s\"", link, d.display);
    set_label_text_if_changed(diag_title_, buf);
    snprintf(buf, sizeof(buf), "rx %lu  tx %lu  err %lu   last: %s", static_cast<unsigned long>(d.rx_frames),
             static_cast<unsigned long>(d.tx_frames), static_cast<unsigned long>(d.rx_errors),
             d.last_frame[0] ? d.last_frame : "-");
    set_label_text_if_changed(diag_detail_, buf);

    // Presets
    for (int i = 0; i < app::kPresetCount; i++) {
        const app::Preset &p = cfg.presets[i];
        PresetRow &pr = preset_rows_[i];
        if (p.icon == app::PresetIcon::Number) {
            set_font_if_changed(pr.icon, font::title());
            snprintf(buf, sizeof(buf), "%d", i + 1);
            set_label_text_if_changed(pr.icon, buf);
        } else {
            set_font_if_changed(pr.icon, font::icon_md());
            set_label_text_if_changed(pr.icon, preset_icon_glyph(p.icon));
        }
        set_label_text_if_changed(pr.name, preset_display_name(cfg, i));
        if (p.height_cm > 0) {
            char h[16];
            format_height(p.height_cm, cfg.units, h, sizeof(h));
            snprintf(buf, sizeof(buf), "%s %s", h, app::unit_suffix(cfg.units));
        } else {
            snprintf(buf, sizeof(buf), "Not set");
        }
        set_label_text_if_changed(pr.value, buf);
    }

    // Network
    const app::NetState &n = s.net;
    switch (n.wifi) {
    case app::WifiStatus::Disabled:
        snprintf(buf, sizeof(buf), "Not configured");
        break;
    case app::WifiStatus::Connecting:
        snprintf(buf, sizeof(buf), "Connecting to %s...", n.ssid);
        break;
    case app::WifiStatus::Connected:
        snprintf(buf, sizeof(buf), "%s  -  %s  -  %d dBm", n.ssid, n.ip, n.rssi);
        break;
    case app::WifiStatus::Failed:
        snprintf(buf, sizeof(buf), "Can't connect to %s", n.ssid);
        break;
    }
    set_label_text_if_changed(wifi_row_.subtitle, buf);
    set_label_text_if_changed(wifi_row_.icon, n.wifi == app::WifiStatus::Connected ? UI_ICON_WIFI : UI_ICON_WIFI_OFF);
    set_text_style(wifi_row_.icon, n.wifi == app::WifiStatus::Connected ? &st.text_positive : &st.text_muted);
    set_label_text_if_changed(mqtt_row_.subtitle, !n.mqtt_enabled     ? "MQTT not configured"
                                                  : n.mqtt_connected ? "Connected via MQTT discovery"
                                                                     : "Connecting to MQTT broker...");
    set_text_style(mqtt_row_.icon, n.mqtt_connected ? &st.text_positive : &st.text_muted);

    // About
    char dur[16];
    format_duration_short(s.uptime_s, dur, sizeof(dur));
    snprintf(buf, sizeof(buf), "Firmware %s  -  up %s", s.firmware_version ? s.firmware_version : "dev", dur);
    set_label_text_if_changed(about_version_, buf);
    ctx().hal->describe_system(buf, sizeof(buf));
    set_label_text_if_changed(about_detail_, buf[0] ? buf : "-");
}

}  // namespace

Screen *create_settings_screen() { return new SettingsScreen(); }

}  // namespace ui
