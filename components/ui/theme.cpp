#include "theme.h"

#include <initializer_list>
#include <utility>

#include "fonts/ui_fonts.h"

namespace ui {

namespace {

Styles s_styles;
Derived s_derived;
int s_index = 0;
lv_theme_t *s_lv_theme = nullptr;
bool s_initialised = false;

const lv_style_prop_t kTransitionProps[] = {LV_STYLE_BG_COLOR, LV_STYLE_TEXT_COLOR, LV_STYLE_BORDER_COLOR,
                                            LV_STYLE_PROP_INV};
lv_style_transition_dsc_t s_transition;

lv_color_t mix(lv_color_t a, lv_color_t b, uint8_t amount_of_a) { return lv_color_mix(a, b, amount_of_a); }

// Every style in Styles, so init/reset can't miss one.
template <typename Fn>
void for_each_style(Fn fn)
{
    Styles &s = s_styles;
    lv_style_t *all[] = {
        &s.screen, &s.card, &s.outline, &s.button_secondary, &s.button_secondary_pressed, &s.button, &s.button_pressed, &s.button_checked, &s.button_disabled,
        &s.button_accent, &s.button_accent_pressed, &s.button_warning, &s.button_danger,
        &s.button_danger_pressed, &s.segmented_track, &s.segmented_option, &s.segmented_checked, &s.progress_fill, &s.text_primary, &s.text_muted, &s.text_faint,
        &s.text_accent, &s.text_on_accent, &s.text_warning, &s.text_danger, &s.text_positive, &s.divider,
        &s.track, &s.indicator, &s.knob, &s.switch_knob, &s.switch_off_knob, &s.scrollbar, &s.toast, &s.dialog, &s.scrim,
        &s.dot_positive, &s.dot_warning, &s.dot_danger, &s.tick, &s.swatch_selected, &s.transition,
    };
    for (lv_style_t *st : all) {
        fn(st);
    }
}

void init_style_objects()
{
    for_each_style([](lv_style_t *st) { lv_style_init(st); });
    lv_style_transition_dsc_init(&s_transition, kTransitionProps, lv_anim_path_ease_out, 120, 0, nullptr);
}

// Fills every style from the palette. Geometry is set here too so a style is
// fully described in one place.
void fill_styles(const Palette &p)
{
    using namespace metrics;
    Styles &s = s_styles;
    Derived &d = s_derived;

    d.accent_soft = mix(p.accent, p.surface, p.dark ? 56 : 40);
    d.accent_pressed = p.dark ? mix(p.accent, lv_color_white(), 210) : mix(p.accent, lv_color_black(), 210);
    d.warning_soft = mix(p.warning, p.surface, p.dark ? 46 : 40);
    d.danger_soft = mix(p.danger, p.surface, p.dark ? 38 : 34);
    d.positive_soft = mix(p.positive, p.surface, p.dark ? 46 : 40);

    for_each_style([](lv_style_t *st) { lv_style_reset(st); });

    lv_style_set_bg_color(&s.screen, p.bg);
    lv_style_set_bg_opa(&s.screen, LV_OPA_COVER);
    lv_style_set_text_color(&s.screen, p.text);
    lv_style_set_text_font(&s.screen, &ui_font_text_16);
    lv_style_set_pad_all(&s.screen, 0);

    lv_style_set_bg_color(&s.card, p.surface);
    lv_style_set_bg_opa(&s.card, LV_OPA_COVER);
    lv_style_set_radius(&s.card, kRadius);
    lv_style_set_pad_all(&s.card, 12);
    lv_style_set_border_width(&s.card, p.dark ? 0 : 1);
    lv_style_set_border_color(&s.card, p.border);

    lv_style_set_border_width(&s.outline, 1);
    lv_style_set_border_color(&s.outline, p.border);

    lv_style_set_bg_color(&s.button_secondary, p.surface_2);
    lv_style_set_border_width(&s.button_secondary, 0);
    lv_style_set_bg_color(&s.button_secondary_pressed, p.dark ? p.border : p.border);

    lv_style_set_bg_color(&s.button, p.surface);
    lv_style_set_bg_opa(&s.button, LV_OPA_COVER);
    lv_style_set_radius(&s.button, kRadius);
    lv_style_set_text_color(&s.button, p.text);
    lv_style_set_pad_all(&s.button, 0);
    lv_style_set_border_width(&s.button, p.dark ? 0 : 1);
    lv_style_set_border_color(&s.button, p.border);
    lv_style_set_border_side(&s.button, LV_BORDER_SIDE_FULL);

    lv_style_set_transition(&s.transition, &s_transition);

    lv_style_set_bg_color(&s.button_pressed, p.surface_2);

    lv_style_set_bg_color(&s.button_checked, d.accent_soft);
    lv_style_set_border_color(&s.button_checked, p.accent);
    lv_style_set_border_width(&s.button_checked, 2);
    lv_style_set_text_color(&s.button_checked, p.dark ? p.text : p.accent);

    lv_style_set_opa(&s.button_disabled, LV_OPA_40);

    lv_style_set_bg_color(&s.button_accent, p.accent);
    lv_style_set_text_color(&s.button_accent, p.on_accent);
    lv_style_set_border_width(&s.button_accent, 0);

    lv_style_set_bg_color(&s.button_accent_pressed, d.accent_pressed);

    lv_style_set_bg_color(&s.button_warning, d.warning_soft);
    lv_style_set_text_color(&s.button_warning, p.warning);
    lv_style_set_border_color(&s.button_warning, p.warning);
    lv_style_set_border_width(&s.button_warning, 0);

    lv_style_set_bg_color(&s.button_danger, d.danger_soft);
    lv_style_set_text_color(&s.button_danger, p.danger);
    lv_style_set_border_color(&s.button_danger, p.danger);
    lv_style_set_border_width(&s.button_danger, 2);
    lv_style_set_border_opa(&s.button_danger, LV_OPA_80);

    lv_style_set_bg_color(&s.button_danger_pressed, p.danger);
    lv_style_set_text_color(&s.button_danger_pressed, p.dark ? p.bg : p.on_accent);

    lv_style_set_bg_color(&s.segmented_track, p.dark ? p.bg : p.surface_2);
    lv_style_set_bg_opa(&s.segmented_track, LV_OPA_COVER);
    lv_style_set_radius(&s.segmented_track, kRadiusSmall + 3);
    lv_style_set_pad_all(&s.segmented_track, 3);
    lv_style_set_border_width(&s.segmented_track, 0);

    lv_style_set_bg_opa(&s.segmented_option, LV_OPA_TRANSP);
    lv_style_set_border_width(&s.segmented_option, 0);
    lv_style_set_text_color(&s.segmented_option, p.text_muted);
    lv_style_set_radius(&s.segmented_option, kRadiusSmall);

    lv_style_set_bg_color(&s.segmented_checked, d.accent_soft);
    lv_style_set_bg_opa(&s.segmented_checked, LV_OPA_COVER);
    lv_style_set_text_color(&s.segmented_checked, p.dark ? p.text : p.accent);
    lv_style_set_border_width(&s.segmented_checked, 0);

    lv_style_set_bg_color(&s.progress_fill, p.accent);
    lv_style_set_bg_opa(&s.progress_fill, LV_OPA_40);
    lv_style_set_radius(&s.progress_fill, 0);
    lv_style_set_border_width(&s.progress_fill, 0);

    lv_style_set_text_color(&s.text_primary, p.text);
    lv_style_set_text_color(&s.text_muted, p.text_muted);
    lv_style_set_text_color(&s.text_faint, p.text_faint);
    lv_style_set_text_color(&s.text_accent, p.accent);
    lv_style_set_text_color(&s.text_on_accent, p.on_accent);
    lv_style_set_text_color(&s.text_warning, p.warning);
    lv_style_set_text_color(&s.text_danger, p.danger);
    lv_style_set_text_color(&s.text_positive, p.positive);

    lv_style_set_bg_color(&s.divider, p.border);
    lv_style_set_bg_opa(&s.divider, LV_OPA_COVER);

    lv_style_set_bg_color(&s.track, p.surface_2);
    lv_style_set_bg_opa(&s.track, LV_OPA_COVER);
    lv_style_set_radius(&s.track, LV_RADIUS_CIRCLE);

    lv_style_set_bg_color(&s.indicator, p.accent);
    lv_style_set_bg_opa(&s.indicator, LV_OPA_COVER);
    lv_style_set_radius(&s.indicator, LV_RADIUS_CIRCLE);

    lv_style_set_bg_color(&s.knob, p.dark ? p.text : lv_color_white());
    lv_style_set_bg_opa(&s.knob, LV_OPA_COVER);
    lv_style_set_radius(&s.knob, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&s.knob, 6);
    lv_style_set_border_width(&s.knob, p.dark ? 0 : 1);
    lv_style_set_border_color(&s.knob, p.border);

    lv_style_set_bg_color(&s.switch_knob, lv_color_white());
    lv_style_set_bg_opa(&s.switch_knob, LV_OPA_COVER);
    lv_style_set_radius(&s.switch_knob, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&s.switch_knob, -4);
    lv_style_set_border_width(&s.switch_knob, 0);
    lv_style_set_shadow_width(&s.switch_knob, p.dark ? 0 : 6);
    lv_style_set_shadow_opa(&s.switch_knob, LV_OPA_20);

    lv_style_set_bg_color(&s.switch_off_knob, p.dark ? p.text_muted : lv_color_white());

    lv_style_set_bg_color(&s.scrollbar, p.text_faint);
    lv_style_set_bg_opa(&s.scrollbar, LV_OPA_60);
    lv_style_set_width(&s.scrollbar, 3);
    lv_style_set_radius(&s.scrollbar, 2);
    lv_style_set_pad_right(&s.scrollbar, 3);
    lv_style_set_pad_top(&s.scrollbar, 6);
    lv_style_set_pad_bottom(&s.scrollbar, 6);

    lv_style_set_bg_color(&s.toast, p.dark ? p.surface_2 : p.text);
    lv_style_set_bg_opa(&s.toast, LV_OPA_COVER);
    lv_style_set_text_color(&s.toast, p.dark ? p.text : p.bg);
    lv_style_set_radius(&s.toast, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&s.toast, p.dark ? 1 : 0);
    lv_style_set_border_color(&s.toast, p.border);
    lv_style_set_pad_hor(&s.toast, 18);
    lv_style_set_pad_ver(&s.toast, 9);
    lv_style_set_shadow_width(&s.toast, 24);
    lv_style_set_shadow_color(&s.toast, lv_color_black());
    lv_style_set_shadow_opa(&s.toast, p.dark ? LV_OPA_60 : LV_OPA_20);

    lv_style_set_bg_color(&s.dialog, p.surface);
    lv_style_set_bg_opa(&s.dialog, LV_OPA_COVER);
    lv_style_set_text_color(&s.dialog, p.text);
    lv_style_set_radius(&s.dialog, kRadius + 2);
    lv_style_set_border_width(&s.dialog, 1);
    lv_style_set_border_color(&s.dialog, p.border);
    lv_style_set_pad_all(&s.dialog, 16);
    lv_style_set_pad_left(&s.dialog, 18);
    lv_style_set_shadow_width(&s.dialog, 40);
    lv_style_set_shadow_color(&s.dialog, lv_color_black());
    lv_style_set_shadow_opa(&s.dialog, p.dark ? LV_OPA_70 : LV_OPA_20);

    lv_style_set_bg_color(&s.scrim, p.dark ? lv_color_black() : p.text);
    lv_style_set_bg_opa(&s.scrim, p.dark ? LV_OPA_70 : LV_OPA_50);

    for (auto [st, c] : {std::pair{&s.dot_positive, p.positive}, std::pair{&s.dot_warning, p.warning},
                         std::pair{&s.dot_danger, p.danger}}) {
        lv_style_set_bg_color(st, c);
        lv_style_set_bg_opa(st, LV_OPA_COVER);
        lv_style_set_radius(st, LV_RADIUS_CIRCLE);
    }

    lv_style_set_border_color(&s.swatch_selected, p.text);
    lv_style_set_border_width(&s.swatch_selected, 3);
    lv_style_set_outline_color(&s.swatch_selected, p.bg);
    lv_style_set_outline_width(&s.swatch_selected, 2);
    lv_style_set_outline_pad(&s.swatch_selected, 0);

    lv_style_set_bg_color(&s.tick, p.text_muted);
    lv_style_set_bg_opa(&s.tick, LV_OPA_COVER);
    lv_style_set_radius(&s.tick, 1);
}

// LVGL theme hook: gives every newly created widget its base look.
void apply_cb(lv_theme_t *, lv_obj_t *obj)
{
    Styles &s = s_styles;

    if (lv_obj_get_parent(obj) == nullptr) {
        lv_obj_add_style(obj, &s.screen, 0);
        lv_obj_add_style(obj, &s.scrollbar, LV_PART_SCROLLBAR);
        return;
    }

    if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_add_style(obj, &s.button, 0);
        lv_obj_add_style(obj, &s.transition, 0);
        lv_obj_add_style(obj, &s.button_pressed, LV_STATE_PRESSED);
        lv_obj_add_style(obj, &s.button_checked, LV_STATE_CHECKED);
        lv_obj_add_style(obj, &s.button_disabled, LV_STATE_DISABLED);
        return;
    }

    if (lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_add_style(obj, &s.track, 0);
        lv_obj_add_style(obj, &s.indicator, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &s.knob, LV_PART_KNOB);
        lv_obj_set_style_pad_all(obj, 0, 0);
        return;
    }

    if (lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_add_style(obj, &s.track, 0);
        lv_obj_add_style(obj, &s.transition, 0);
        lv_obj_add_style(obj, &s.indicator, static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | LV_STATE_CHECKED);
        lv_obj_add_style(obj, &s.switch_knob, LV_PART_KNOB);
        lv_obj_add_style(obj, &s.switch_off_knob, LV_PART_KNOB);
        lv_obj_add_style(obj, &s.switch_knob, static_cast<lv_style_selector_t>(LV_PART_KNOB) | LV_STATE_CHECKED);
        return;
    }

    if (lv_obj_check_type(obj, &lv_bar_class)) {
        lv_obj_add_style(obj, &s.track, 0);
        lv_obj_add_style(obj, &s.indicator, LV_PART_INDICATOR);
        return;
    }

    if (lv_obj_check_type(obj, &lv_label_class)) {
        return;
    }

    // Plain containers stay transparent but get the themed scrollbar.
    lv_obj_add_style(obj, &s.scrollbar, LV_PART_SCROLLBAR);
}

}  // namespace

namespace theme {

void init(lv_display_t *disp, int palette_index)
{
    if (!s_initialised) {
        init_style_objects();
        s_initialised = true;
    }
    s_index = (palette_index >= 0 && palette_index < palette_count()) ? palette_index : 0;
    fill_styles(palette(s_index));

    if (s_lv_theme == nullptr) {
        s_lv_theme = lv_theme_create();
        lv_theme_set_apply_cb(s_lv_theme, apply_cb);
    }
    lv_display_set_theme(disp, s_lv_theme);
}

void apply(int palette_index)
{
    if (palette_index < 0 || palette_index >= palette_count() || palette_index == s_index) {
        return;
    }
    s_index = palette_index;
    fill_styles(palette(s_index));
    lv_obj_report_style_change(nullptr);
}

int current_index() { return s_index; }
const Palette &current() { return palette(s_index); }
const Derived &derived() { return s_derived; }
Styles &styles() { return s_styles; }

}  // namespace theme

}  // namespace ui
