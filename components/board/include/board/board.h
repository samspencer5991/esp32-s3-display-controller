// LilyGO T-Display-S3-Long board support.
//
//  * 3.4" 180x640 IPS panel, AXS15231B controller over QSPI (portrait native)
//  * Capacitive touch: AXS15231B built-in (I2C 0x3B) on original boards,
//    Hynitron CST3530 (I2C 0x58) on 2026+ revisions - detected at runtime
//  * SY6970 charger/PMU (I2C 0x6A) sharing the touch I2C bus
//  * PT4103 backlight driver, PWM on GPIO1
//
// Pin facts from the LilyGO schematic / examples (see docs/hardware.md).
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace board {

constexpr int kPanelWidth = 180;   // native portrait
constexpr int kPanelHeight = 640;

namespace pins {
constexpr int kLcdCs = 12;
constexpr int kLcdSck = 17;
constexpr int kLcdD0 = 13;
constexpr int kLcdD1 = 18;
constexpr int kLcdD2 = 21;
constexpr int kLcdD3 = 14;
constexpr int kLcdRst = 16;        // also resets the AXS15231B touch
constexpr int kBacklight = 1;
constexpr int kI2cSda = 15;
constexpr int kI2cScl = 10;
constexpr int kTouchInt = 11;
constexpr int kTouchRstCst = 2;    // CST3530 boards only (shared with battery ADC on old boards)
constexpr int kBootButton = 0;
}  // namespace pins

enum class TouchChip : uint8_t { None, Axs15231b, Cst3530 };

struct TouchPoint {
    bool pressed;
    uint16_t x;  // portrait panel coordinates, 0..179
    uint16_t y;  // 0..639
};

// Brings up I2C, PMU tweaks, panel and touch. Safe to call once.
esp_err_t init();

// --- Display ------------------------------------------------------------------
// Sends rows [0, rows) of a portrait 180x640 RGB565 big-endian frame. The
// AXS15231B does not reliably honour partial windows, so updates always start
// at the first row ("draw from origin"). The buffer must stay valid until the
// transfer completes; call wait_frame_done() before modifying it.
esp_err_t display_push(const uint8_t *frame_be565, int rows);
void display_wait_done();
uint32_t display_last_transfer_ms();  // duration of the last completed frame transfer
esp_err_t display_sleep(bool sleep);
// Single-byte register read (diagnostics, e.g. 0x0A power mode, 0x04 ID).
uint8_t display_read_register(uint8_t reg);

// --- Backlight ----------------------------------------------------------------
void backlight_set(uint8_t percent);  // 0 = off

// --- Touch ------------------------------------------------------------------------
TouchChip touch_chip();
bool touch_read(TouchPoint &out);  // false on I2C error; presses are debounced
uint32_t touch_rejected_count();    // single-sample glitches filtered so far

// --- Diagnostics --------------------------------------------------------------------
// Addresses that ACKed during the boot I2C scan (for the console "board" command).
size_t i2c_scan_results(uint8_t *out, size_t max);
bool pmu_present();

}  // namespace board
