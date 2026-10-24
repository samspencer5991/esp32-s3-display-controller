// Lights screen: LED strip power, effect, brightness and colour.
#include "ui_internal.h"

namespace ui {

namespace {

using namespace metrics;

struct Swatch {
    uint8_t r, g, b;
};

// Curated colours that read well on LED strips (warm white first).
const Swatch kSwatches[] = {
    {0xFF, 0xB8, 0x6C},  // warm white
    {0xFF, 0xFF, 0xFF},  // cool white
    {0xFF, 0x45, 0x3A},  // red
    {0xFF, 0x8A, 0x1F},  // orange
    {0xFF, 0xD4, 0x1F},  // amber
    {0x3D, 0xDC, 0x84},  // green
    {0x1F, 0xD6, 0xD6},  // teal
    {0x4F, 0x8C, 0xFF},  // blue
    {0x8B, 0x5C, 0xFF},  // violet
    {0xFF, 0x4F, 0xB5},  // pink
};
constexpr int kSwatchCount = sizeof(kSwatches) / sizeof(kSwatches[0]);

const char *const kEffectLabels[] = {"Solid", "Breathe", "Rainbow", "Status"};

class LightsScreen : public Screen {
public:
    void build(lv_obj_t *screen) override;
    void update(const app::State &s) override;

private:
    template <typename Fn>
    static void edit(Fn fn)
    {
        app::LightSettings l = ctx().app->settings().lights;
        fn(l);
        ctx().app->set_lights(l);
    }

    lv_obj_t *power_ = nullptr;
    Segmented effect_;
    lv_obj_t *brightness_ = nullptr;
    lv_obj_t *brightness_value_ = nullptr;
    lv_obj_t *swatches_[kSwatchCount] = {};
    lv_obj_t *controls_ = nullptr;
};

void LightsScreen::build(lv_obj_t *screen)
{
    Styles &st = theme::styles();
    root_ = screen;
    lv_obj_set_scrollable(screen, false);
    make_header(screen, "Lights");

    power_ = lv_switch_create(screen);
    lv_obj_set_size(power_, 60, 32);
    lv_obj_align(power_, LV_ALIGN_TOP_RIGHT, -kPad - 6, kPad + 4);
    on_event(power_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t *) {
        const bool on = lv_obj_has_state(power_, LV_STATE_CHECKED);
        edit([on](app::LightSettings &l) { l.on = on; });
    });

    effect_.create(screen, kEffectLabels, 4, 380, 40, [](int i) {
        edit([i](app::LightSettings &l) {
            l.effect = static_cast<app::LightEffect>(i);
            l.on = true;
        });
    });
    lv_obj_align(effect_.obj(), LV_ALIGN_TOP_RIGHT, -kPad - 60 - 22, kPad);

    controls_ = make_card(screen, kScreenW - 2 * kPad, kScreenH - kPad - (kPad + 40 + 8));
    lv_obj_set_pos(controls_, kPad, kPad + 40 + 8);
    lv_obj_set_style_pad_hor(controls_, 16, 0);
    lv_obj_set_style_pad_ver(controls_, 10, 0);

    lv_obj_t *low = make_label(controls_, font::icon_md(), &st.text_muted, UI_ICON_BRIGHTNESS_LOW);
    lv_obj_align(low, LV_ALIGN_TOP_LEFT, 0, 2);
    brightness_ = lv_slider_create(controls_);
    lv_obj_set_size(brightness_, 470, 10);
    lv_obj_align(brightness_, LV_ALIGN_TOP_LEFT, 40, 9);
    lv_slider_set_range(brightness_, 1, 100);
    lv_obj_set_ext_click_area(brightness_, 16);
    brightness_value_ = make_label(controls_, font::body(), &st.text_muted, "");
    lv_obj_set_width(brightness_value_, 56);
    lv_obj_set_style_text_align(brightness_value_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(brightness_value_, LV_ALIGN_TOP_RIGHT, 0, 3);
    on_event(brightness_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t *) {
        lv_label_set_text_fmt(brightness_value_, "%d%%", static_cast<int>(lv_slider_get_value(brightness_)));
        const auto v = static_cast<uint8_t>(lv_slider_get_value(brightness_));
        edit([v](app::LightSettings &l) { l.brightness = v; });
    });

    // Colour swatches: fixed colours by design (they are the LED colours).
    constexpr int kSize = 44;
    const int spacing = (kScreenW - 2 * kPad - 32 - kSize) / (kSwatchCount - 1);
    for (int i = 0; i < kSwatchCount; i++) {
        lv_obj_t *sw = lv_button_create(controls_);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, kSize, kSize);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sw, lv_color_make(kSwatches[i].r, kSwatches[i].g, kSwatches[i].b), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_add_style(sw, &st.swatch_selected, LV_STATE_CHECKED);
        lv_obj_align(sw, LV_ALIGN_BOTTOM_LEFT, i * spacing, 0);
        on_event(sw, LV_EVENT_CLICKED, [i](lv_event_t *) {
            edit([i](app::LightSettings &l) {
                l.r = kSwatches[i].r;
                l.g = kSwatches[i].g;
                l.b = kSwatches[i].b;
                l.on = true;
                if (l.effect == app::LightEffect::Rainbow || l.effect == app::LightEffect::DeskStatus) {
                    l.effect = app::LightEffect::Solid;
                }
            });
        });
        swatches_[i] = sw;
    }
}

void LightsScreen::update(const app::State &s)
{
    const app::LightSettings &l = s.settings.lights;
    lv_obj_set_checked(power_, l.on);
    effect_.set_selected(static_cast<int>(l.effect));
    if (!lv_obj_has_state(brightness_, LV_STATE_PRESSED)) {
        lv_slider_set_value(brightness_, l.brightness, LV_ANIM_OFF);
        lv_label_set_text_fmt(brightness_value_, "%d%%", l.brightness);
    }
    const bool colour_matters = l.effect == app::LightEffect::Solid || l.effect == app::LightEffect::Breathe;
    for (int i = 0; i < kSwatchCount; i++) {
        const bool selected = kSwatches[i].r == l.r && kSwatches[i].g == l.g && kSwatches[i].b == l.b;
        lv_obj_set_checked(swatches_[i], selected && colour_matters);
        set_opa_if_changed(swatches_[i], colour_matters ? LV_OPA_COVER : LV_OPA_50);
    }
    set_opa_if_changed(controls_, l.on ? LV_OPA_COVER : LV_OPA_60);
}

}  // namespace

Screen *create_lights_screen() { return new LightsScreen(); }

}  // namespace ui
