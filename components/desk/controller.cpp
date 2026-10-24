#include "desk/controller.h"

#include <cmath>
#include <cstring>

namespace desk {

Controller::Controller(Transport *transport, Timing timing) : transport_(transport), timing_(timing) {}

void Controller::set_transport(Transport *transport, uint32_t now_ms)
{
    // The previous transport keeps its wake pin high: dropping it would look
    // like the handset being unplugged, which can put the box in reset mode.
    transport_ = transport;
    if (transport_ != nullptr) {
        transport_->flush_input();
    }
    status_ = Status();
    parser_ = FrameParser();
    hold_keys_ = 0;
    step_count_ = 0;
    current_keys_ = 0;
    last_sent_keys_ = 0;
    poll_pending_ = false;
    wake_pin_high_ = false;
    wake(now_ms);
}

void Controller::hold(uint16_t keys, uint32_t now_ms)
{
    step_count_ = 0;
    hold_keys_ = keys;
    hold_refreshed_ms_ = now_ms;
}

void Controller::release() { hold_keys_ = 0; }

void Controller::begin_steps(uint32_t now_ms, bool allow_wake)
{
    hold_keys_ = 0;
    step_count_ = 0;
    step_index_ = 0;
    step_started_ = false;
    step_frames_ = 0;
    if (allow_wake) {
        add_wake_step_if_asleep(now_ms);
    }
}

void Controller::press(uint16_t keys, uint32_t duration_ms, uint32_t now_ms, bool allow_wake)
{
    begin_steps(now_ms, allow_wake);
    steps_[step_count_++] = Step{keys, duration_ms};
}

void Controller::press_sequence(uint16_t first, uint32_t first_ms, uint16_t second, uint32_t second_ms,
                                uint32_t now_ms)
{
    begin_steps(now_ms, true);
    steps_[step_count_++] = Step{first, first_ms};
    steps_[step_count_++] = Step{KeyNone, timing_.sequence_gap_ms};
    steps_[step_count_++] = Step{second, second_ms};
}

void Controller::cancel()
{
    hold_keys_ = 0;
    step_count_ = 0;
}

bool Controller::pending_keys(uint16_t keys) const
{
    for (uint8_t i = step_index_; i < step_count_; i++) {
        if (steps_[i].keys & keys) {
            return true;
        }
    }
    return (hold_keys_ & keys) != 0;
}

bool Controller::last_sent(uint16_t keys, uint32_t &ms_out) const
{
    bool found = false;
    for (int bit = 0; bit < 16; bit++) {
        const uint16_t mask = static_cast<uint16_t>(1u << bit);
        if ((keys & mask) && (keys_ever_sent_ & mask)) {
            // Most recent across the requested bits (relative to the newest).
            if (!found || static_cast<int32_t>(key_sent_ms_[bit] - ms_out) > 0) {
                ms_out = key_sent_ms_[bit];
            }
            found = true;
        }
    }
    return found;
}

bool Controller::sent_since(uint16_t keys, uint32_t since_ms) const
{
    uint32_t t = 0;
    return last_sent(keys, t) && static_cast<int32_t>(t - since_ms) >= 0;
}

void Controller::add_wake_step_if_asleep(uint32_t now_ms)
{
    // Some control boxes ignore the first key after sleeping; wake the display
    // first and give it a moment (community implementations use ~600 ms).
    const bool asleep = status_.sleeping || status_.display.kind == DisplayKind::Blank;
    if (asleep && status_.online) {
        wake(now_ms);
        steps_[step_count_++] = Step{KeyNone, timing_.wake_settle_ms};
    }
}

void Controller::wake(uint32_t now_ms)
{
    if (transport_ == nullptr) {
        return;
    }
    transport_->set_wake_pin(false);
    wake_pin_high_ = false;
    wake_low_started_ms_ = now_ms;
}

bool Controller::busy() const { return hold_keys_ != 0 || step_count_ != 0; }

uint16_t Controller::compute_keys(uint32_t now_ms)
{
    if (hold_keys_ != 0) {
        if (now_ms - hold_refreshed_ms_ > timing_.hold_timeout_ms) {
            hold_keys_ = 0;  // UI stopped refreshing: fail safe
        } else {
            return hold_keys_;
        }
    }
    while (step_count_ != 0) {
        const Step &step = steps_[step_index_];
        if (step.keys == KeyNone) {
            // Released gap / wake settle: timed from when it was entered.
            if (!step_started_) {
                step_started_ = true;
                step_started_ms_ = now_ms;
            }
            if (now_ms - step_started_ms_ < step.duration_ms) {
                return KeyNone;
            }
        } else if (!step_started_ || now_ms - step_started_ms_ < step.duration_ms ||
                   step_frames_ < timing_.min_frames_per_press) {
            // Key step: keep offering the keys until they have really been on
            // the wire for the full duration and enough frames.
            return step.keys;
        }
        step_started_ = false;
        step_frames_ = 0;
        if (++step_index_ >= step_count_) {
            step_count_ = 0;
        }
    }
    return KeyNone;
}

void Controller::send_keys(uint16_t keys, uint32_t now_ms)
{
    uint8_t buf[kMaxFrame];
    const size_t n = encode_keys(keys, buf);
    transport_->write(buf, n);
    if (tap_ != nullptr) {
        tap_(tap_ctx_, true, buf, n);
    }
    status_.tx_frames++;
    last_tx_ms_ = now_ms;
    last_sent_keys_ = keys;
    for (int bit = 0; bit < 16; bit++) {
        if (keys & (1u << bit)) {
            key_sent_ms_[bit] = now_ms;
        }
    }
    keys_ever_sent_ |= keys;
    // A key step's duration starts with its first frame on the wire.
    if (keys != KeyNone && step_count_ != 0 && steps_[step_index_].keys == keys) {
        if (!step_started_) {
            step_started_ = true;
            step_started_ms_ = now_ms;
        }
        if (step_frames_ < 255) {
            step_frames_++;
        }
    }
}

void Controller::handle_frame(const Frame &frame, uint32_t now_ms)
{
    status_.rx_frames++;
    status_.last_rx_ms = now_ms;

    switch (frame.type) {
    case kTypePoll:
        status_.last_poll_ms = now_ms;
        poll_pending_ = true;
        return;
    case kTypeSleep:
        status_.sleeping = true;
        return;
    case kTypeDisplay:
        break;
    default:
        return;
    }
    if (frame.len < 3) {
        return;
    }

    const DisplayReading reading = decode_display(frame.payload);
    if (reading.kind != status_.display.kind || strcmp(reading.text, status_.display.text) != 0) {
        status_.display_changed_ms = now_ms;
    }
    status_.display = reading;

    switch (reading.kind) {
    case DisplayKind::Height:
        status_.sleeping = false;
        if (!status_.height_valid) {
            status_.last_height_change_ms = now_ms;
        } else if (std::fabs(reading.value - status_.height) > 0.01f) {
            status_.moving = reading.value > status_.height ? Direction::Up : Direction::Down;
            status_.last_height_change_ms = now_ms;
            status_.box_locked = false;  // it moved, so it is not locked
        }
        status_.height = reading.value;
        status_.height_valid = true;
        break;
    case DisplayKind::Locked:
        status_.box_locked = true;
        break;
    default:
        break;
    }
}

void Controller::poll(uint32_t now_ms)
{
    if (transport_ == nullptr) {
        return;
    }

    // --- Receive ---------------------------------------------------------------
    uint8_t rx[64];
    size_t n;
    while ((n = transport_->read(rx, sizeof(rx))) > 0) {
        for (size_t i = 0; i < n; i++) {
            switch (parser_.feed(rx[i])) {
            case FrameParser::Result::Frame:
                memcpy(status_.last_frame, parser_.raw(), parser_.raw_len());
                status_.last_frame_len = parser_.raw_len();
                if (tap_ != nullptr) {
                    tap_(tap_ctx_, false, parser_.raw(), parser_.raw_len());
                }
                handle_frame(parser_.frame(), now_ms);
                break;
            case FrameParser::Result::Error:
                status_.rx_errors++;
                break;
            case FrameParser::Result::None:
                break;
            }
        }
    }

    status_.online = status_.rx_frames > 0 && (now_ms - status_.last_rx_ms) < timing_.offline_after_ms;
    status_.polled = status_.last_poll_ms != 0 && (now_ms - status_.last_poll_ms) < timing_.poll_mode_timeout_ms;
    if (status_.moving != Direction::None &&
        (!status_.online || (now_ms - status_.last_height_change_ms) > timing_.idle_after_ms)) {
        status_.moving = Direction::None;
    }

    // --- Wake pin ----------------------------------------------------------------
    if (!wake_pin_high_ && now_ms - wake_low_started_ms_ >= timing_.wake_low_ms) {
        transport_->set_wake_pin(true);
        wake_pin_high_ = true;
    }

    // --- Transmit ----------------------------------------------------------------
    current_keys_ = compute_keys(now_ms);
    if (status_.polled) {
        if (poll_pending_) {
            send_keys(current_keys_, now_ms);
        }
    } else if (current_keys_ != KeyNone) {
        if (current_keys_ != last_sent_keys_ || now_ms - last_tx_ms_ >= timing_.unsolicited_interval_ms) {
            send_keys(current_keys_, now_ms);
        }
    } else if (last_sent_keys_ != KeyNone) {
        send_keys(KeyNone, now_ms);  // explicit release
    }
    poll_pending_ = false;
}

}  // namespace desk
