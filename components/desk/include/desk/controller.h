// Desk controller: turns high-level intents (hold up, go to preset, save
// preset, toggle the control box lock) into correctly timed key frames, and
// decodes the control box's display frames into a height + status.
//
// Key transmission follows the stock handset:
//  * The control box polls (type 0x11, ~every 40 ms) and the handset answers
//    each poll with the current key mask ("00 00" when idle).
//  * If no polls are seen, keys are sent unsolicited every 108 ms instead
//    (the rate most community implementations use successfully).
//
// Platform independent: time is passed in, I/O goes through Transport.
#pragma once

#include <cstddef>
#include <cstdint>

#include "desk/protocol.h"

namespace desk {

class Transport {
public:
    virtual ~Transport() = default;
    // Non-blocking read of whatever bytes are available.
    virtual size_t read(uint8_t *buf, size_t max) = 0;
    virtual void write(const uint8_t *data, size_t len) = 0;
    // RJ45 pin 4 ("PIN 20"). Held high while the handset is present; a
    // low->high edge turns the control box display (and height reports) on.
    virtual void set_wake_pin(bool high) { (void)high; }
    // Drop any buffered input (stale bytes received while not in use).
    virtual void flush_input() {}
};

struct Timing {
    uint32_t unsolicited_interval_ms = 108;  // key repeat when the box is not polling
    uint32_t poll_mode_timeout_ms = 400;     // polls seen within this -> answer polls only
    uint32_t hold_timeout_ms = 400;          // held keys auto-release unless refreshed
    uint32_t preset_press_ms = 500;          // preset key press length
    uint32_t memory_press_ms = 300;          // M key press length when storing a preset
    uint32_t stop_press_ms = 80;             // brief Up/Down tap that interrupts a preset move
    uint32_t sequence_gap_ms = 400;          // released gap between keys of a sequence
    uint32_t lock_press_ms = 5500;           // M held this long toggles the control box lock
    uint32_t offline_after_ms = 2000;        // no valid frames for this long -> offline
    uint32_t idle_after_ms = 800;            // height unchanged this long -> not moving
    uint32_t wake_low_ms = 150;              // wake pin low pulse to switch the display on
    uint32_t wake_settle_ms = 600;           // delay after waking before pressing a key
    uint8_t min_frames_per_press = 3;        // a timed press is never shorter than this many frames
};

enum class Direction : int8_t { Down = -1, None = 0, Up = 1 };

struct Status {
    bool online = false;
    bool polled = false;            // control box is polling us
    bool height_valid = false;
    float height = 0;               // control box units (cm unless configured for inches)
    Direction moving = Direction::None;
    DisplayReading display;         // last decoded display frame
    uint32_t display_changed_ms = 0;
    bool box_locked = false;        // "LoC" seen and not cleared by movement since
    bool sleeping = false;          // sleep frame (0x13) seen, display blank
    uint32_t rx_frames = 0;
    uint32_t tx_frames = 0;
    uint32_t rx_errors = 0;
    uint32_t last_rx_ms = 0;
    uint32_t last_poll_ms = 0;
    uint32_t last_height_change_ms = 0;
    uint8_t last_frame[kMaxFrame] = {};
    size_t last_frame_len = 0;
};

class Controller {
public:
    explicit Controller(Transport *transport = nullptr, Timing timing = Timing());

    // Swapping transports (e.g. demo <-> real) resets link state.
    void set_transport(Transport *transport, uint32_t now_ms);
    Transport *transport() const { return transport_; }

    // Call frequently (every 2-10 ms) - poll answers are sent from here.
    void poll(uint32_t now_ms);

    // Continuous keys (up/down). Must be refreshed at least every
    // Timing::hold_timeout_ms or they release by themselves: a stuck UI can
    // never leave the desk driving.
    void hold(uint16_t keys, uint32_t now_ms);
    void release();

    // Timed press of `keys` for `duration_ms`. Cancels holds and sequences.
    // The duration counts from the first frame actually sent, and a press
    // always lasts at least Timing::min_frames_per_press frames, so a stalled
    // task can shorten nothing and skip nothing. `allow_wake` lets the press
    // first wake a sleeping control box (never wanted for a stop tap).
    void press(uint16_t keys, uint32_t duration_ms, uint32_t now_ms, bool allow_wake = true);
    // Two presses separated by a released gap (e.g. M then Preset N).
    void press_sequence(uint16_t first, uint32_t first_ms, uint16_t second, uint32_t second_ms, uint32_t now_ms);
    // Drops pending presses and holds without sending anything further.
    void cancel();

    // Pulse the wake pin low->high so the control box switches its display on.
    void wake(uint32_t now_ms);

    // Observer for every valid received frame and every transmitted frame
    // (protocol sniffing from the console). Called from poll().
    using FrameTap = void (*)(void *ctx, bool tx, const uint8_t *data, size_t len);
    void set_frame_tap(FrameTap tap, void *ctx)
    {
        tap_ = tap;
        tap_ctx_ = ctx;
    }

    bool busy() const;
    // True if any pending (current or future) press includes a bit of `keys`.
    bool pending_keys(uint16_t keys) const;
    // True if a frame containing a bit of `keys` was sent at or after `since_ms`.
    bool sent_since(uint16_t keys, uint32_t since_ms) const;
    // Time a frame containing a bit of `keys` was last sent; false if never.
    bool last_sent(uint16_t keys, uint32_t &ms_out) const;
    uint16_t current_keys() const { return current_keys_; }
    const Status &status() const { return status_; }
    const Timing &timing() const { return timing_; }

private:
    struct Step {
        uint16_t keys;
        uint32_t duration_ms;
    };

    uint16_t compute_keys(uint32_t now_ms);
    void begin_steps(uint32_t now_ms, bool allow_wake);
    void add_wake_step_if_asleep(uint32_t now_ms);
    void send_keys(uint16_t keys, uint32_t now_ms);
    void handle_frame(const Frame &frame, uint32_t now_ms);

    Transport *transport_;
    Timing timing_;
    Status status_;
    FrameParser parser_;

    uint16_t hold_keys_ = 0;
    uint32_t hold_refreshed_ms_ = 0;

    Step steps_[4] = {};
    uint8_t step_count_ = 0;
    uint8_t step_index_ = 0;
    bool step_started_ = false;      // gap entered / key step's first frame sent
    uint32_t step_started_ms_ = 0;
    uint8_t step_frames_ = 0;        // frames sent for the current key step

    uint32_t key_sent_ms_[16] = {};  // per key bit: last time a frame carried it
    uint16_t keys_ever_sent_ = 0;

    uint16_t current_keys_ = 0;
    uint16_t last_sent_keys_ = 0;
    uint32_t last_tx_ms_ = 0;
    bool poll_pending_ = false;

    bool wake_pin_high_ = false;
    uint32_t wake_low_started_ms_ = 0;

    FrameTap tap_ = nullptr;
    void *tap_ctx_ = nullptr;
};

}  // namespace desk
