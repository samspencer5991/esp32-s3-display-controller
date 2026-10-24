// ESP32 implementations of the portable interfaces: settings storage, clock,
// display HAL and the desk UART transport.
#pragma once

#include "app/app.h"
#include "desk/controller.h"
#include "ui/ui.h"

namespace platform {

class EspPlatform : public app::Platform {
public:
    // Starts the settings writer task (NVS writes can block for a long time
    // during page erases; they must never stall the desk task).
    void begin();
    uint32_t now_ms() override;
    bool load_settings(app::Settings &out) override;
    void save_settings(const app::Settings &s) override;  // queued, returns immediately
    const char *firmware_version() override;
};

class EspHal : public ui::Hal {
public:
    explicit EspHal(app::App *app) : app_(app) {}
    void set_backlight(uint8_t percent) override;
    void set_rotation_180(bool rotated) override;
    void restart() override;
    void describe_system(char *buf, size_t len) override;

private:
    app::App *app_;
};

// UART1 + wake GPIO to the control box's handset port.
class DeskUart : public desk::Transport {
public:
    void begin();
    size_t read(uint8_t *buf, size_t max) override;
    void write(const uint8_t *data, size_t len) override;
    void set_wake_pin(bool high) override;
    void flush_input() override;
    // Blocks until bytes arrive or `timeout_ms` passes (desk task pacing).
    void wait_for_data(uint32_t timeout_ms);

private:
    bool started_ = false;
};

// Stops any desk motion (re-sending stop until the desk is idle), holds the
// wake pin high, then restarts via a short deep sleep (a plain esp_restart
// can leave the AXS15231B panel dark).
void safe_restart(app::App *app);

}  // namespace platform
