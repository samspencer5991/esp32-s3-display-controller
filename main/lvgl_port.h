// LVGL integration for the T-Display-S3-Long.
//
// LVGL renders a 640x180 landscape frame (DIRECT mode, PSRAM). On flush the
// dirty columns are rotated into a portrait big-endian transfer buffer and
// pushed to the panel from row 0 down to the last dirty row, which is what
// the AXS15231B handles reliably.
#pragma once

#include <cstdint>

#include "lvgl.h"

namespace lvgl_port {

constexpr int kWidth = 640;
constexpr int kHeight = 180;

lv_display_t *init(bool rotate_180);
void set_rotation_180(bool rotate_180);

// LVGL is not thread safe: hold this lock around any lv_* call made outside
// the LVGL task.
bool lock(uint32_t timeout_ms = UINT32_MAX);
void unlock();

// Filter applied to every touch sample before LVGL sees it (see ui::touch_gate).
void set_touch_filter(bool (*filter)(bool pressed));

// Console / test hooks. Coordinates are landscape.
void inject_touch(bool pressed, int x, int y);
// Copies the landscape RGB565 framebuffer (little endian) into `out`
// (kWidth * kHeight * 2 bytes). Takes the LVGL lock.
void snapshot(uint8_t *out);

struct Stats {
    uint32_t frames;
    uint32_t last_wait_ms;      // waiting for the previous DMA transfer
    uint32_t last_convert_ms;   // rotate + byte swap
    uint32_t last_transfer_ms;  // SPI transfer of the previous frame
    uint32_t last_render_ms;    // time between the previous flush and this one
    uint32_t max_handler_ms;    // longest lv_timer_handler() run (reset by `stats`)
    uint32_t last_rows;
};
Stats stats(bool reset_max = false);

}  // namespace lvgl_port
