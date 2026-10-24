#include "lvgl_port.h"

#include <algorithm>
#include <cstring>

#include "board/board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace lvgl_port {

namespace {

const char *TAG = "lvgl";

constexpr int kPanelW = board::kPanelWidth;   // 180
constexpr int kPanelH = board::kPanelHeight;  // 640

SemaphoreHandle_t s_mutex = nullptr;
lv_display_t *s_disp = nullptr;
uint16_t *s_fb = nullptr;      // landscape RGB565 (LVGL draw buffer)
uint8_t *s_xfer = nullptr;     // portrait RGB565 big-endian (panel frame)
bool s_rotate_180 = false;
bool s_full_redraw_pending = true;

int s_dirty_x1 = kWidth, s_dirty_x2 = -1;
Stats s_stats = {};
int64_t s_last_flush_end_us = 0;

bool (*s_touch_filter)(bool) = nullptr;
volatile bool s_inject_active = false;
volatile bool s_inject_pressed = false;
volatile int s_inject_x = 0, s_inject_y = 0;

uint32_t tick_cb() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// Rotate landscape columns [x1, x2] into the portrait transfer buffer.
//
// Both buffers live in PSRAM behind a small data cache, so the walk is done
// in 32x32 tiles: each tile touches ~40 KB of the framebuffer and ~12 KB of
// the transfer buffer instead of striding across the whole 230 KB.
void convert_columns(int x1, int x2)
{
    constexpr int kTile = 32;
    for (int ty = 0; ty < kHeight; ty += kTile) {
        const int ty_end = std::min(ty + kTile, kHeight);
        for (int tx = x1; tx <= x2; tx += kTile) {
            const int tx_end = std::min(tx + kTile - 1, x2);
            for (int ly = ty; ly < ty_end; ly++) {
                const uint16_t *src = s_fb + ly * kWidth;
                const int px = s_rotate_180 ? ly : (kPanelW - 1 - ly);
                for (int lx = tx; lx <= tx_end; lx++) {
                    const int py = s_rotate_180 ? (kWidth - 1 - lx) : lx;
                    const uint16_t c = src[lx];
                    uint8_t *dst = s_xfer + (py * kPanelW + px) * 2;
                    dst[0] = static_cast<uint8_t>(c >> 8);
                    dst[1] = static_cast<uint8_t>(c);
                }
            }
        }
    }
}

void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *)
{
    s_dirty_x1 = std::min<int>(s_dirty_x1, area->x1);
    s_dirty_x2 = std::max<int>(s_dirty_x2, area->x2);

    if (!lv_display_flush_is_last(disp)) {
        lv_display_flush_ready(disp);
        return;
    }

    const int64_t t0 = esp_timer_get_time();
    board::display_wait_done();  // never touch the transfer buffer mid-DMA
    const int64_t t1 = esp_timer_get_time();

    int x1 = std::clamp(s_dirty_x1, 0, kWidth - 1);
    int x2 = std::clamp(s_dirty_x2, 0, kWidth - 1);
    if (s_full_redraw_pending) {
        x1 = 0;
        x2 = kWidth - 1;
        s_full_redraw_pending = false;
    }
    int rows = kPanelH;
    int64_t t2 = t1;
    if (x1 <= x2) {
        convert_columns(x1, x2);
        t2 = esp_timer_get_time();
        rows = s_rotate_180 ? (kWidth - x1) : (x2 + 1);
        // Keep the transfer length aligned so PSRAM DMA needs no bounce copy.
        rows = std::min(kPanelH, (rows + 7) & ~7);
        board::display_push(s_xfer, rows);
    }
    s_dirty_x1 = kWidth;
    s_dirty_x2 = -1;

    s_stats.frames++;
    s_stats.last_rows = static_cast<uint32_t>(rows);
    s_stats.last_wait_ms = static_cast<uint32_t>((t1 - t0) / 1000);
    s_stats.last_convert_ms = static_cast<uint32_t>((t2 - t1) / 1000);
    s_stats.last_transfer_ms = board::display_last_transfer_ms();
    s_stats.last_render_ms = s_last_flush_end_us ? static_cast<uint32_t>((t0 - s_last_flush_end_us) / 1000) : 0;
    s_last_flush_end_us = esp_timer_get_time();
    lv_display_flush_ready(disp);
}

void touch_read_cb(lv_indev_t *, lv_indev_data_t *data)
{
    board::TouchPoint p = {};
    board::touch_read(p);

    bool pressed = p.pressed;
    int lx = 0, ly = 0;
    if (s_inject_active) {
        pressed = s_inject_pressed;
        lx = s_inject_x;
        ly = s_inject_y;
        if (!pressed) {
            s_inject_active = false;
        }
    } else if (s_rotate_180) {
        lx = kWidth - 1 - p.y;
        ly = p.x;
    } else {
        lx = p.y;
        ly = kHeight - 1 - p.x;
    }

    static bool was_pressed = false;
    if (pressed && !was_pressed && !s_inject_active) {
        ESP_LOGI(TAG, "touch down at %d,%d (panel %u,%u)", lx, ly, p.x, p.y);
    }
    was_pressed = pressed;

    if (s_touch_filter != nullptr && !s_touch_filter(pressed)) {
        pressed = false;
    }
    data->point.x = std::clamp(lx, 0, kWidth - 1);
    data->point.y = std::clamp(ly, 0, kHeight - 1);
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void lvgl_task(void *)
{
    while (true) {
        uint32_t delay_ms = 10;
        if (lock(100)) {
            const int64_t t0 = esp_timer_get_time();
            delay_ms = lv_timer_handler();
            const uint32_t spent = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
            s_stats.max_handler_ms = std::max(s_stats.max_handler_ms, spent);
            unlock();
        }
        delay_ms = std::clamp<uint32_t>(delay_ms, 4, 30);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

}  // namespace

lv_display_t *init(bool rotate_180)
{
    s_rotate_180 = rotate_180;
    s_mutex = xSemaphoreCreateRecursiveMutex();

    lv_init();
    lv_tick_set_cb(tick_cb);

    const size_t fb_bytes = kWidth * kHeight * sizeof(uint16_t);
    s_fb = static_cast<uint16_t *>(heap_caps_aligned_calloc(64, 1, fb_bytes, MALLOC_CAP_SPIRAM));
    s_xfer = static_cast<uint8_t *>(heap_caps_aligned_calloc(64, 1, kPanelW * kPanelH * 2, MALLOC_CAP_SPIRAM));
    assert(s_fb && s_xfer);

    s_disp = lv_display_create(kWidth, kHeight);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, s_fb, nullptr, fb_bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush_cb);

    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read_cb);
    lv_indev_set_display(touch, s_disp);
    lv_indev_set_long_press_time(touch, 500);
    lv_timer_set_period(lv_indev_get_read_timer(touch), 20);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, nullptr, 4, nullptr, 1);
    ESP_LOGI(TAG, "LVGL %d.%d.%d running (%dx%d, rotate_180=%d)", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
             LVGL_VERSION_PATCH, kWidth, kHeight, rotate_180);
    return s_disp;
}

void set_rotation_180(bool rotate_180)
{
    if (!lock()) {
        return;
    }
    if (rotate_180 != s_rotate_180) {
        s_rotate_180 = rotate_180;
        s_full_redraw_pending = true;
        lv_obj_invalidate(lv_screen_active());
    }
    unlock();
}

bool lock(uint32_t timeout_ms)
{
    const TickType_t ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_mutex, ticks) == pdTRUE;
}

void unlock() { xSemaphoreGiveRecursive(s_mutex); }

void set_touch_filter(bool (*filter)(bool)) { s_touch_filter = filter; }

void inject_touch(bool pressed, int x, int y)
{
    s_inject_x = x;
    s_inject_y = y;
    s_inject_pressed = pressed;
    s_inject_active = true;
}

void snapshot(uint8_t *out)
{
    lock();
    memcpy(out, s_fb, kWidth * kHeight * 2);
    unlock();
}

Stats stats(bool reset_max)
{
    const Stats st = s_stats;
    if (reset_max) {
        s_stats.max_handler_ms = 0;
    }
    return st;
}

}  // namespace lvgl_port
