// A simulated LoctekMotion control box.
//
// Implements Transport from the handset's point of view: key frames written
// by the Controller are interpreted as button presses, and the box's frames
// (polls, display, keep-alive, sleep) are generated back the way the real
// control box behaves according to published captures. Used for the
// on-device demo mode and by the desktop simulator, so the full protocol path
// (encode -> parse -> decode) is exercised without a desk attached.
#pragma once

#include <cstdint>

#include "desk/controller.h"

namespace desk {

struct SimDeskConfig {
    float min_height = 63.5f;    // Flexispot E7 Pro range 63.5 - 128.5 cm
    float max_height = 128.5f;
    float speed = 3.8f;          // cm/s (E7 Pro: 38 mm/s; 2026 edition 50 mm/s)
    float accel = 14.0f;         // cm/s^2 soft start/stop
    float start_height = 74.0f;
    float presets[4] = {72.5f, 112.0f, 0.0f, 0.0f};  // 0 = not stored
    uint32_t awake_ms = 12000;   // display stays on after last activity
    uint32_t poll_period_ms = 40;
    uint32_t display_period_ms = 70;
    uint32_t keepalive_period_ms = 500;
};

// A time window that is safe across the 32-bit millisecond wrap.
struct Window {
    bool active = false;
    uint32_t start = 0;
    uint32_t duration = 0;
    void open(uint32_t now, uint32_t ms)
    {
        active = true;
        start = now;
        duration = ms;
    }
    void close() { active = false; }
    bool contains(uint32_t now) const { return active && now - start < duration; }
};

class SimControlBox : public Transport {
public:
    explicit SimControlBox(SimDeskConfig cfg = SimDeskConfig());

    // Transport (handset side)
    size_t read(uint8_t *buf, size_t max) override;
    void write(const uint8_t *data, size_t len) override;
    void set_wake_pin(bool high) override;

    void tick(uint32_t now_ms);

    float height() const { return height_; }
    float preset(int i) const { return (i >= 0 && i < 4) ? cfg_.presets[i] : 0; }
    bool display_on() const { return display_on_; }
    bool locked() const { return locked_; }
    bool moving() const { return velocity_ != 0; }

    // Test hooks
    void set_height(float h) { height_ = h; }
    void set_poll_enabled(bool on) { polling_ = on; }  // emulate boxes that don't poll
    void set_unplugged(bool unplugged) { unplugged_ = unplugged; }
    void show_text(const char *text3, uint32_t duration_ms);  // e.g. "E01"

private:
    void on_keys_changed(uint16_t keys);
    void wake_display();
    void emit(uint8_t type, const uint8_t *payload, uint8_t len);
    void flash(const char *text3, uint32_t duration_ms);

    SimDeskConfig cfg_;
    FrameParser parser_;
    uint8_t out_[512] = {};
    size_t out_len_ = 0;

    uint32_t now_ = 0;
    uint32_t last_tick_ = 0;
    float height_;
    float velocity_ = 0;
    bool has_target_ = false;
    float target_ = 0;

    uint16_t keys_ = 0;
    uint32_t keys_since_ms_ = 0;
    uint32_t last_key_frame_ms_ = 0;
    bool press_consumed_ = false;
    bool lock_toggled_ = false;
    bool locked_ = false;
    Window memory_armed_;
    char flash_text_[4] = {};
    Window flash_;

    bool wake_pin_ = false;
    bool display_on_ = false;
    Window awake_;  // display stays on while open
    bool polling_ = true;
    bool unplugged_ = false;
    uint32_t last_poll_ = 0;
    uint32_t last_display_ = 0;
    uint32_t last_keepalive_ = 0;
};

}  // namespace desk
