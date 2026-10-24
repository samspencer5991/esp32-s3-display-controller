// Theme system.
//
// A theme is just a Palette (plain colour data, see themes.cpp). All widgets
// are styled exclusively through the shared lv_style_t objects below, which
// are (re)filled from the active palette. Switching theme therefore rewrites
// the styles in place and asks LVGL to refresh - no screens are rebuilt.
//
// To add a theme: append a Palette to kPalettes in themes.cpp. That's it.
#pragma once

#include "lvgl.h"

namespace ui {

struct Palette {
    const char *name;
    bool dark;
    lv_color_t bg;         // screen background
    lv_color_t surface;    // cards, buttons
    lv_color_t surface_2;  // pressed / raised surfaces, tracks
    lv_color_t border;     // hairlines, dividers
    lv_color_t text;       // primary text
    lv_color_t text_muted; // secondary text
    lv_color_t text_faint; // tertiary text, disabled glyphs
    lv_color_t accent;     // brand / interactive highlight
    lv_color_t on_accent;  // text & icons drawn on accent
    lv_color_t positive;
    lv_color_t warning;
    lv_color_t danger;
};

int palette_count();
const Palette &palette(int index);

// Colours derived from the active palette (computed once per theme change).
struct Derived {
    lv_color_t accent_soft;     // tinted surface for selected items
    lv_color_t accent_pressed;
    lv_color_t warning_soft;
    lv_color_t danger_soft;
    lv_color_t positive_soft;
};

// Shared styles. Widgets add these with lv_obj_add_style(); never set colours
// on objects directly or they will not follow theme changes.
struct Styles {
    lv_style_t screen;
    lv_style_t card;
    lv_style_t outline;          // hairline border (dialogs)
    lv_style_t button;
    lv_style_t button_pressed;
    lv_style_t button_checked;
    lv_style_t button_disabled;
    lv_style_t button_secondary;  // raised button inside cards / dialogs
    lv_style_t button_secondary_pressed;
    lv_style_t button_accent;
    lv_style_t button_accent_pressed;
    lv_style_t button_warning;   // e.g. child lock engaged
    lv_style_t button_danger;    // e.g. stop
    lv_style_t button_danger_pressed;
    lv_style_t segmented_track;  // background behind segmented options
    lv_style_t segmented_option;
    lv_style_t segmented_checked;
    lv_style_t progress_fill;    // hold-to-confirm fill overlay
    lv_style_t text_primary;
    lv_style_t text_muted;
    lv_style_t text_faint;
    lv_style_t text_accent;
    lv_style_t text_on_accent;
    lv_style_t text_warning;
    lv_style_t text_danger;
    lv_style_t text_positive;
    lv_style_t divider;
    lv_style_t track;            // slider / bar background
    lv_style_t indicator;        // slider / bar / switch fill
    lv_style_t knob;             // slider knob
    lv_style_t switch_knob;
    lv_style_t switch_off_knob;
    lv_style_t scrollbar;
    lv_style_t toast;
    lv_style_t dialog;
    lv_style_t scrim;
    lv_style_t dot_positive;
    lv_style_t dot_warning;
    lv_style_t dot_danger;
    lv_style_t tick;             // gauge preset markers
    lv_style_t swatch_selected;  // ring around a selected colour swatch
    lv_style_t transition;       // shared press transition
};

namespace theme {

// Initialises styles and installs the LVGL theme on `disp`.
void init(lv_display_t *disp, int palette_index);
// Switches palette and refreshes every styled object.
void apply(int palette_index);
int current_index();
const Palette &current();
const Derived &derived();
Styles &styles();

}  // namespace theme

// Geometry tokens shared by all screens.
namespace metrics {
constexpr int kScreenW = 640;
constexpr int kScreenH = 180;
constexpr int kPad = 8;
constexpr int kGap = 8;
constexpr int kRadius = 14;
constexpr int kRadiusSmall = 10;
}  // namespace metrics

}  // namespace ui
