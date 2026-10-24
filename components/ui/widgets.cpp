#include "widgets.h"

#include <cstring>

#include "lvgl_private.h"

namespace ui {

lv_obj_t *make_box(lv_obj_t *parent)
{
    // Unthemed lv_obj defaults are already transparent with no border or
    // padding; setting them locally would override styles added later.
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_event_bubble(obj, true);
    return obj;
}

lv_obj_t *make_button(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_scrollable(btn, false);
    lv_obj_set_scroll_on_focus(btn, false);
    lv_obj_set_scroll_chain(btn, true);
    return btn;
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_style_t *color, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    if (color != nullptr) {
        lv_obj_add_style(label, color, 0);
    }
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_obj_set_event_bubble(label, true);
    return label;
}

lv_obj_t *make_card(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_add_style(card, &theme::styles().card, 0);
    lv_obj_set_size(card, w, h);
    lv_obj_set_scrollable(card, false);
    return card;
}

lv_obj_t *make_dot(lv_obj_t *parent, int size)
{
    lv_obj_t *dot = make_box(parent);
    lv_obj_set_size(dot, size, size);
    return dot;
}

void set_text_style(lv_obj_t *obj, lv_style_t *style)
{
    // Idempotent: restyling an object forces a redraw, and screens call this
    // on every state update.
    Styles &s = theme::styles();
    for (lv_style_t *st : {&s.text_primary, &s.text_muted, &s.text_faint, &s.text_accent, &s.text_on_accent,
                           &s.text_warning, &s.text_danger, &s.text_positive}) {
        set_style_if(obj, st, st == style);
    }
}

void set_font_if_changed(lv_obj_t *obj, const lv_font_t *font)
{
    if (lv_obj_get_style_text_font(obj, LV_PART_MAIN) != font) {
        lv_obj_set_style_text_font(obj, font, 0);
    }
}

void set_opa_if_changed(lv_obj_t *obj, lv_opa_t opa)
{
    if (lv_obj_get_style_opa(obj, LV_PART_MAIN) != opa) {
        lv_obj_set_style_opa(obj, opa, 0);
    }
}

void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    const char *current = lv_label_get_text(label);
    if (current == nullptr || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}

namespace {

bool has_style(lv_obj_t *obj, lv_style_t *style, lv_style_selector_t selector)
{
    // LVGL has no public query for this; removing and re-adding on every
    // update would restyle (and redraw) the object needlessly.
    for (uint32_t i = 0; i < obj->style_cnt; i++) {
        if (obj->styles[i].style == style && obj->styles[i].selector == selector) {
            return true;
        }
    }
    return false;
}

}  // namespace

void set_style_if(lv_obj_t *obj, lv_style_t *style, bool on, lv_style_selector_t selector)
{
    const bool present = has_style(obj, style, selector);
    if (on && !present) {
        lv_obj_add_style(obj, style, selector);
    } else if (!on && present) {
        lv_obj_remove_style(obj, style, selector);
    }
}

void on_event(lv_obj_t *obj, lv_event_code_t code, std::function<void(lv_event_t *)> fn)
{
    auto *heap_fn = new std::function<void(lv_event_t *)>(std::move(fn));
    lv_obj_add_event_cb(
        obj,
        [](lv_event_t *e) {
            auto *f = static_cast<std::function<void(lv_event_t *)> *>(lv_event_get_user_data(e));
            (*f)(e);
        },
        code, heap_fn);
    lv_obj_add_event_cb(
        obj,
        [](lv_event_t *e) { delete static_cast<std::function<void(lv_event_t *)> *>(lv_event_get_user_data(e)); },
        LV_EVENT_DELETE, heap_fn);
}

// --- HoldToConfirm -------------------------------------------------------------

void HoldToConfirm::attach(lv_obj_t *button, uint32_t delay_ms, uint32_t duration_ms,
                           std::function<void()> on_complete, int bar_inset)
{
    button_ = button;
    delay_ms_ = delay_ms;
    duration_ms_ = duration_ms;
    on_complete_ = std::move(on_complete);
    inset_ = bar_inset;

    bar_ = make_box(button);
    lv_obj_add_style(bar_, &theme::styles().indicator, 0);
    lv_obj_set_ignore_layout(bar_, true);
    lv_obj_set_hidden(bar_, true);
    lv_obj_set_size(bar_, 0, 4);
    lv_obj_align(bar_, LV_ALIGN_BOTTOM_LEFT, inset_, -7);

    lv_obj_add_event_cb(
        button, [](lv_event_t *e) { static_cast<HoldToConfirm *>(lv_event_get_user_data(e))->on_press(); },
        LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(
        button, [](lv_event_t *e) { static_cast<HoldToConfirm *>(lv_event_get_user_data(e))->on_release(); },
        LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(
        button, [](lv_event_t *e) { static_cast<HoldToConfirm *>(lv_event_get_user_data(e))->on_release(); },
        LV_EVENT_PRESS_LOST, this);
}

void HoldToConfirm::on_press()
{
    consumed_ = false;
    pressed_at_ = lv_tick_get();
    if (timer_ == nullptr) {
        timer_ = lv_timer_create(timer_cb, 16, this);
    }
}

void HoldToConfirm::on_release()
{
    cancel();
}

void HoldToConfirm::cancel()
{
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (bar_ != nullptr) {
        lv_obj_set_hidden(bar_, true);
        lv_obj_set_width(bar_, 0);
    }
}

void HoldToConfirm::timer_cb(lv_timer_t *t)
{
    auto *self = static_cast<HoldToConfirm *>(lv_timer_get_user_data(t));
    const uint32_t elapsed = lv_tick_elaps(self->pressed_at_);
    if (elapsed < self->delay_ms_) {
        return;
    }
    const int32_t full = lv_obj_get_width(self->button_) - 2 * self->inset_;
    uint32_t progress = elapsed - self->delay_ms_;
    if (progress >= self->duration_ms_) {
        self->consumed_ = true;
        self->cancel();
        if (self->on_complete_) {
            self->on_complete_();
        }
        return;
    }
    lv_obj_set_hidden(self->bar_, false);
    lv_obj_set_width(self->bar_, static_cast<int32_t>(full * progress / self->duration_ms_));
}

// --- Segmented -------------------------------------------------------------------

void Segmented::create(lv_obj_t *parent, const char *const *labels, int count, int w, int h,
                       std::function<void(int)> on_select)
{
    constexpr int kGap = 2;
    constexpr int kInset = 3;
    Styles &st = theme::styles();
    count_ = count > 8 ? 8 : count;
    on_select_ = std::move(on_select);
    root_ = make_box(parent);
    lv_obj_add_style(root_, &st.segmented_track, 0);
    lv_obj_set_size(root_, w, h);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(root_, kGap, 0);

    const int bw = (w - 2 * kInset - kGap * (count_ - 1)) / count_;
    for (int i = 0; i < count_; i++) {
        lv_obj_t *btn = make_button(root_, bw, h - 2 * kInset);
        lv_obj_add_style(btn, &st.segmented_option, 0);
        lv_obj_add_style(btn, &st.segmented_checked, LV_STATE_CHECKED);
        lv_obj_add_style(btn, &st.button_pressed, LV_STATE_PRESSED);
        lv_obj_t *label = make_label(btn, font::caption(), nullptr, labels[i]);
        lv_obj_center(label);
        buttons_[i] = btn;
        on_event(btn, LV_EVENT_CLICKED, [this, i](lv_event_t *) {
            set_selected(i);
            if (on_select_) {
                on_select_(i);
            }
        });
    }
}

void Segmented::set_selected(int index)
{
    if (index == selected_) {
        return;
    }
    selected_ = index;
    for (int i = 0; i < count_; i++) {
        lv_obj_set_checked(buttons_[i], i == index);
    }
}

// --- Toast -------------------------------------------------------------------------

namespace {

lv_obj_t *s_toast = nullptr;
lv_obj_t *s_toast_icon = nullptr;
lv_obj_t *s_toast_text = nullptr;
lv_timer_t *s_toast_timer = nullptr;

void toast_opa_cb(void *obj, int32_t v) { lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), v, 0); }

void toast_hide(lv_timer_t *)
{
    s_toast_timer = nullptr;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_exec_cb(&a, toast_opa_cb);
    lv_anim_set_values(&a, lv_obj_get_style_opa(s_toast, LV_PART_MAIN), LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_completed_cb(&a, [](lv_anim_t *anim) {
        lv_obj_set_hidden(static_cast<lv_obj_t *>(anim->var), true);
    });
    lv_anim_start(&a);
}

lv_obj_t *s_modal = nullptr;

}  // namespace

void toast(const char *icon, const char *text, uint32_t duration_ms)
{
    if (s_toast == nullptr) {
        s_toast = lv_obj_create(lv_layer_top());
        lv_obj_add_style(s_toast, &theme::styles().toast, 0);
        lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_scrollable(s_toast, false);
        lv_obj_set_clickable(s_toast, false);
        lv_obj_set_flex_flow(s_toast, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_toast, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(s_toast, 8, 0);
        s_toast_icon = make_label(s_toast, font::icon_md(), &theme::styles().text_accent, "");
        s_toast_text = make_label(s_toast, font::body(), nullptr, "");
        lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -14);
    }
    lv_label_set_text(s_toast_icon, icon != nullptr ? icon : "");
    lv_obj_set_hidden(s_toast_icon, icon == nullptr);
    lv_label_set_text(s_toast_text, text);

    lv_anim_delete(s_toast, toast_opa_cb);
    lv_obj_set_hidden(s_toast, false);
    lv_obj_move_foreground(s_toast);
    lv_obj_set_style_opa(s_toast, LV_OPA_COVER, 0);

    if (s_toast_timer != nullptr) {
        lv_timer_delete(s_toast_timer);
    }
    s_toast_timer = lv_timer_create(toast_hide, duration_ms, nullptr);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

// --- Modal ---------------------------------------------------------------------------

void modal_close()
{
    if (s_modal != nullptr) {
        lv_obj_delete(s_modal);
        s_modal = nullptr;
    }
}

bool modal_open() { return s_modal != nullptr; }

void modal(const char *icon, const char *title, const char *body, const ModalButton *buttons, int count, Tone tone)
{
    modal_close();
    Styles &s = theme::styles();

    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_add_style(s_modal, &s.scrim, 0);
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(s_modal, false);

    // Dialog: [icon] title / body / right-aligned buttons, height follows content.
    lv_obj_t *card = lv_obj_create(s_modal);
    lv_obj_add_style(card, &s.dialog, 0);
    lv_obj_set_size(card, 520, LV_SIZE_CONTENT);
    lv_obj_set_scrollable(card, false);
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(card, 14, 0);

    lv_style_t *icon_style = tone == Tone::Danger    ? &s.text_danger
                             : tone == Tone::Warning ? &s.text_warning
                                                     : &s.text_accent;
    lv_obj_t *icon_label = make_label(card, font::icon_lg(), icon_style, icon != nullptr ? icon : "");

    lv_obj_t *column = make_box(card);
    lv_obj_set_size(column, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(column, 1);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 4, 0);
    (void)icon_label;

    make_label(column, font::title(), nullptr, title);
    lv_obj_t *body_label = make_label(column, font::body(), &s.text_muted, body);
    lv_obj_set_width(body_label, LV_PCT(100));
    lv_label_set_long_mode(body_label, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *row = make_box(column);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    for (int i = 0; i < count; i++) {
        lv_obj_t *btn = make_button(row, LV_SIZE_CONTENT, 42);
        lv_obj_set_style_pad_hor(btn, 22, 0);
        lv_obj_set_style_radius(btn, metrics::kRadiusSmall, 0);
        if (i == count - 1) {
            lv_obj_add_style(btn, &s.button_accent, 0);
            lv_obj_add_style(btn, &s.button_accent_pressed, LV_STATE_PRESSED);
        } else {
            lv_obj_add_style(btn, &s.button_secondary, 0);
            lv_obj_add_style(btn, &s.button_secondary_pressed, LV_STATE_PRESSED);
        }
        lv_obj_t *label = make_label(btn, font::body(), nullptr, buttons[i].text);
        lv_obj_center(label);
        std::function<void()> action = buttons[i].action;
        on_event(btn, LV_EVENT_CLICKED, [action](lv_event_t *) {
            // Close after the event finishes; the action may open another modal.
            lv_async_call([](void *) { modal_close(); }, nullptr);
            if (action) {
                action();
            }
        });
    }
    lv_obj_update_layout(s_modal);
}

}  // namespace ui
