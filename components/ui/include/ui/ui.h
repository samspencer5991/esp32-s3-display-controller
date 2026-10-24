// Desk controller touch UI (LVGL 9, 640x180 landscape).
//
// Platform independent: the ESP32 firmware and the desktop simulator both
// call ui::init() with an LVGL display and implement ui::Hal.
#pragma once

#include <cstdint>

#include "app/app.h"
#include "lvgl.h"

namespace ui {

class Hal {
public:
    virtual ~Hal() = default;
    // 0 = backlight off, 100 = full.
    virtual void set_backlight(uint8_t percent) = 0;
    // Mount orientation (flips display output and touch input).
    virtual void set_rotation_180(bool rotated) = 0;
    virtual void restart() {}
    // Free-form multi-line text for the About page (heap, MAC, IDF version...).
    virtual void describe_system(char *buf, size_t len) { if (len) buf[0] = '\0'; }
};

// Builds all screens on `disp` and starts the state sync timer.
void init(lv_display_t *disp, app::App &app, Hal &hal);

// Call from the touch driver's read callback before reporting to LVGL.
// Returns false when the touch must be swallowed (it woke a dimmed/sleeping
// screen), so the first tap never triggers a control by accident.
bool touch_gate(bool pressed);

// Programmatic navigation (used by tests / console).
enum class Page : uint8_t { Home, Settings, Lights };
void show(Page page);

}  // namespace ui
