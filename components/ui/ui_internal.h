// Internal UI plumbing shared by the screens.
#pragma once

#include <cstdio>

#include "app/app.h"
#include "ui/ui.h"
#include "widgets.h"

namespace ui {

class Screen {
public:
    virtual ~Screen() = default;
    virtual void build(lv_obj_t *screen) = 0;
    // Called whenever the application state changed (and once after build).
    virtual void update(const app::State &state) = 0;
    virtual void on_show() {}
    lv_obj_t *root() const { return root_; }

protected:
    lv_obj_t *root_ = nullptr;
};

struct Context {
    lv_display_t *display = nullptr;
    app::App *app = nullptr;
    Hal *hal = nullptr;
    app::State state{};  // latest snapshot
};

Context &ctx();
void navigate(Page page);
// Keep the screen awake (e.g. while the desk is moving).
void keep_awake();

// Factories (one instance each, owned by ui.cpp).
Screen *create_home_screen();
Screen *create_settings_screen();
Screen *create_lights_screen();

// --- Formatting -----------------------------------------------------------------

// Height as the control box would show it: one decimal below 100 cm (or
// always one decimal in inches).
void format_height(float cm, app::Units units, char *buf, size_t len);
// Always one decimal (ranges, settings).
void format_length(float cm, app::Units units, char *buf, size_t len);
const char *preset_icon_glyph(app::PresetIcon icon);
const char *preset_display_name(const app::Settings &s, int index);
void format_duration_short(uint32_t seconds, char *buf, size_t len);

// Common header used by secondary screens: back button + title.
lv_obj_t *make_header(lv_obj_t *parent, const char *title);

}  // namespace ui
