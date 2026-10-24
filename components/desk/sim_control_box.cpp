#include "desk/sim_control_box.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace desk {

namespace {
constexpr uint32_t kKeyReleaseMs = 250;      // no key frames for this long = released
constexpr uint32_t kMemoryArmedMs = 4000;    // "S-" window after tapping M
constexpr uint32_t kLockHoldMs = 5000;       // hold M to toggle the child lock
}  // namespace

SimControlBox::SimControlBox(SimDeskConfig cfg) : cfg_(cfg), height_(cfg.start_height) {}

size_t SimControlBox::read(uint8_t *buf, size_t max)
{
    const size_t n = std::min(max, out_len_);
    memcpy(buf, out_, n);
    memmove(out_, out_ + n, out_len_ - n);
    out_len_ -= n;
    return n;
}

void SimControlBox::write(const uint8_t *data, size_t len)
{
    if (unplugged_) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        if (parser_.feed(data[i]) != FrameParser::Result::Frame) {
            continue;
        }
        const Frame &f = parser_.frame();
        if (f.type != kTypeKeys || f.len < 1) {
            continue;
        }
        const uint16_t keys = static_cast<uint16_t>(f.payload[0] | (f.len > 1 ? f.payload[1] << 8 : 0));
        last_key_frame_ms_ = now_;
        if (keys != keys_) {
            on_keys_changed(keys);
        }
    }
}

void SimControlBox::set_wake_pin(bool high)
{
    if (high && !wake_pin_) {
        wake_display();  // a low->high edge switches the display on
    }
    wake_pin_ = high;
}

void SimControlBox::show_text(const char *text3, uint32_t duration_ms) { flash(text3, duration_ms); }

void SimControlBox::flash(const char *text3, uint32_t duration_ms)
{
    memcpy(flash_text_, text3, 3);
    flash_.open(now_, duration_ms);
    wake_display();
}

void SimControlBox::wake_display()
{
    display_on_ = true;
    awake_.open(now_, cfg_.awake_ms);
}

void SimControlBox::on_keys_changed(uint16_t keys)
{
    keys_ = keys;
    keys_since_ms_ = now_;
    lock_toggled_ = false;

    if (keys == KeyNone) {
        press_consumed_ = false;
        return;
    }

    wake_display();

    // Any new press interrupts an automatic move and is otherwise ignored.
    if (has_target_) {
        has_target_ = false;
        press_consumed_ = true;
        return;
    }
    if (locked_ && keys != KeyMemory) {
        flash("LoC", 2000);
        press_consumed_ = true;
        return;
    }

    if (keys == KeyMemory) {
        memory_armed_.open(now_, kMemoryArmedMs);
        return;
    }
    for (int i = 0; i < 4; i++) {
        if (keys != preset_key(i)) {
            continue;
        }
        if (memory_armed_.contains(now_)) {
            cfg_.presets[i] = height_;
            memory_armed_.close();
            const char text[3] = {'S', '-', static_cast<char>('1' + i)};
            flash(text, 1500);
        } else if (cfg_.presets[i] > 0 && std::fabs(cfg_.presets[i] - height_) > 0.05f) {
            target_ = cfg_.presets[i];
            has_target_ = true;
        }
        return;
    }
    if (keys & (KeyUp | KeyDown)) {
        memory_armed_.close();
    }
}

void SimControlBox::emit(uint8_t type, const uint8_t *payload, uint8_t len)
{
    Frame f;
    f.type = type;
    f.len = len;
    if (len > 0) {
        memcpy(f.payload, payload, len);
    }
    uint8_t buf[kMaxFrame];
    const size_t n = encode_frame(f, buf);
    if (out_len_ + n <= sizeof(out_)) {
        memcpy(out_ + out_len_, buf, n);
        out_len_ += n;
    }
}

void SimControlBox::tick(uint32_t now_ms)
{
    if (last_tick_ == 0) {
        last_tick_ = now_ms;
    }
    now_ = now_ms;
    const float dt = static_cast<float>(now_ms - last_tick_) / 1000.0f;
    last_tick_ = now_ms;

    // Unsolicited senders stop transmitting on release instead of sending 00 00.
    if (keys_ != KeyNone && now_ms - last_key_frame_ms_ > kKeyReleaseMs) {
        on_keys_changed(KeyNone);
    }

    // Holding M toggles the child lock.
    if (keys_ == KeyMemory && !lock_toggled_ && now_ms - keys_since_ms_ >= kLockHoldMs) {
        lock_toggled_ = true;
        locked_ = !locked_;
        memory_armed_.close();
        if (locked_) {
            flash("LoC", 1500);
        } else {
            flash_.close();
        }
    }

    // --- Motion ------------------------------------------------------------------
    float desired = 0;
    const int manual = (keys_ & KeyUp) ? 1 : (keys_ & KeyDown) ? -1 : 0;
    if (manual != 0 && !press_consumed_ && !locked_) {
        desired = manual * cfg_.speed;
    } else if (has_target_) {
        const float remaining = target_ - height_;
        const float vmax = std::sqrt(2.0f * cfg_.accel * std::fabs(remaining));
        desired = std::copysign(std::min(cfg_.speed, std::max(vmax, 0.5f)), remaining);
    }

    const float dv = cfg_.accel * dt;
    velocity_ = velocity_ < desired ? std::min(desired, velocity_ + dv) : std::max(desired, velocity_ - dv);
    if (desired == 0 && std::fabs(velocity_) < 0.3f) {
        velocity_ = 0;
    }

    if (velocity_ != 0) {
        float next = height_ + velocity_ * dt;
        if (has_target_ && (velocity_ > 0) == (next >= target_)) {
            next = target_;
            velocity_ = 0;
            has_target_ = false;
        }
        if (next <= cfg_.min_height || next >= cfg_.max_height) {
            next = std::clamp(next, cfg_.min_height, cfg_.max_height);
            velocity_ = 0;
            has_target_ = false;
        }
        height_ = next;
        wake_display();
    }

    // --- Display timeout -----------------------------------------------------------
    if (display_on_ && keys_ == KeyNone && !awake_.contains(now_ms)) {
        display_on_ = false;
        emit(kTypeSleep, nullptr, 0);
    }

    // --- Output: the box only talks while the handset holds the wake pin high -----
    if (!wake_pin_ || unplugged_) {
        return;
    }
    if (polling_ && now_ms - last_poll_ >= cfg_.poll_period_ms) {
        last_poll_ = now_ms;
        emit(kTypePoll, nullptr, 0);
    }
    if (now_ms - last_keepalive_ >= cfg_.keepalive_period_ms) {
        last_keepalive_ = now_ms;
        emit(kTypeKeepAlive, nullptr, 0);
    }
    if (now_ms - last_display_ >= cfg_.display_period_ms) {
        last_display_ = now_ms;
        uint8_t seg[3] = {0, 0, 0};
        if (flash_.contains(now_ms)) {
            encode_text_display(flash_text_, seg);
        } else if (memory_armed_.contains(now_ms)) {
            encode_text_display("S- ", seg);
        } else if (display_on_) {
            encode_height_display(height_, seg);
        }
        emit(kTypeDisplay, seg, 3);
    }
}

}  // namespace desk
