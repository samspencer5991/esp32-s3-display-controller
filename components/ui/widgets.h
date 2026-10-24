// Small composable widgets shared by the screens. All styling comes from
// theme::styles() so everything follows theme changes.
#pragma once

#include <functional>

#include "fonts/ui_fonts.h"
#include "lvgl.h"
#include "theme.h"

namespace ui {

namespace font {
inline const lv_font_t *caption() { return &ui_font_text_14; }
inline const lv_font_t *body() { return &ui_font_text_16; }
inline const lv_font_t *title() { return &ui_font_text_20; }
inline const lv_font_t *value() { return &ui_font_text_28; }
inline const lv_font_t *display() { return &ui_font_display_64; }
inline const lv_font_t *icon_sm() { return &ui_font_icon_18; }
inline const lv_font_t *icon_md() { return &ui_font_icon_24; }
inline const lv_font_t *icon_lg() { return &ui_font_icon_32; }
inline const lv_font_t *icon_xl() { return &ui_font_icon_48; }
}  // namespace font

// Transparent, non-scrollable container with no padding.
lv_obj_t *make_box(lv_obj_t *parent);
lv_obj_t *make_button(lv_obj_t *parent, int w, int h);
lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_style_t *color, const char *text);
lv_obj_t *make_card(lv_obj_t *parent, int w, int h);
lv_obj_t *make_dot(lv_obj_t *parent, int size);

// Replace the colour style on a label/object, removing any other colour style.
void set_text_style(lv_obj_t *obj, lv_style_t *style);
void set_label_text_if_changed(lv_obj_t *label, const char *text);
void set_font_if_changed(lv_obj_t *obj, const lv_font_t *font);
void set_opa_if_changed(lv_obj_t *obj, lv_opa_t opa);
void set_style_if(lv_obj_t *obj, lv_style_t *style, bool on, lv_style_selector_t selector = 0);

// Click handler helper that captures a std::function (owned by the object).
void on_event(lv_obj_t *obj, lv_event_code_t code, std::function<void(lv_event_t *)> fn);

// Hold-to-confirm: after `delay_ms` of pressing, a progress bar fills along
// the bottom edge of `button`; reaching the end fires `on_complete`.
// `consumed()` tells click handlers to ignore the release that ended a hold.
class HoldToConfirm {
public:
    void attach(lv_obj_t *button, uint32_t delay_ms, uint32_t duration_ms, std::function<void()> on_complete,
                int bar_inset = 12);
    bool consumed() const { return consumed_; }
    void cancel();

private:
    static void timer_cb(lv_timer_t *t);
    void on_press();
    void on_release();

    lv_obj_t *button_ = nullptr;
    lv_obj_t *bar_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    uint32_t delay_ms_ = 0;
    uint32_t duration_ms_ = 0;
    uint32_t pressed_at_ = 0;
    int inset_ = 12;
    bool consumed_ = false;
    std::function<void()> on_complete_;
};

// Row of mutually exclusive options.
class Segmented {
public:
    void create(lv_obj_t *parent, const char *const *labels, int count, int w, int h,
                std::function<void(int)> on_select);
    void set_selected(int index);
    lv_obj_t *obj() const { return root_; }

private:
    lv_obj_t *root_ = nullptr;
    lv_obj_t *buttons_[8] = {};
    int count_ = 0;
    int selected_ = -1;
    std::function<void(int)> on_select_;
};

// Transient message pill on the top layer.
void toast(const char *icon, const char *text, uint32_t duration_ms = 2200);

// Modal dialog on the top layer. Buttons are laid out right-aligned; the last
// one is styled as the primary (accent) action.
struct ModalButton {
    const char *text;
    std::function<void()> action;
};
enum class Tone : uint8_t { Accent, Warning, Danger };
void modal(const char *icon, const char *title, const char *body, const ModalButton *buttons, int count,
           Tone tone = Tone::Accent);
void modal_close();
bool modal_open();

}  // namespace ui
