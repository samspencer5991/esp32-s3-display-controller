#include "platform.h"

#include <cstring>

#include "board/board.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl_port.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace platform {

namespace {
const char *TAG = "platform";
constexpr const char *kNvsNamespace = "desk";
constexpr const char *kNvsSettingsKey = "settings";
constexpr uart_port_t kDeskUart = UART_NUM_1;

struct StoredSettings {
    uint16_t version;
    uint16_t size;
    app::Settings settings;
};
}  // namespace

// --- Platform ---------------------------------------------------------------------------

uint32_t EspPlatform::now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

bool EspPlatform::load_settings(app::Settings &out)
{
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    StoredSettings stored{};
    size_t len = sizeof(stored);
    const esp_err_t err = nvs_get_blob(h, kNvsSettingsKey, &stored, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(stored) || stored.version != app::kSettingsVersion ||
        stored.size != sizeof(app::Settings)) {
        ESP_LOGW(TAG, "no compatible stored settings (%s), using defaults", esp_err_to_name(err));
        return false;
    }
    out = stored.settings;
    ESP_LOGI(TAG, "settings loaded");
    return true;
}

namespace {

QueueHandle_t s_save_queue = nullptr;

void write_settings(const app::Settings &s)
{
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    StoredSettings stored{};
    stored.version = app::kSettingsVersion;
    stored.size = sizeof(app::Settings);
    stored.settings = s;
    esp_err_t err = nvs_set_blob(h, kNvsSettingsKey, &stored, sizeof(stored));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "settings saved (%s)", esp_err_to_name(err));
}

void settings_writer_task(void *)
{
    app::Settings s;
    while (true) {
        if (xQueueReceive(s_save_queue, &s, portMAX_DELAY) == pdTRUE) {
            write_settings(s);
        }
    }
}

}  // namespace

void EspPlatform::begin()
{
    // Depth 1 + overwrite: only the latest settings matter.
    s_save_queue = xQueueCreate(1, sizeof(app::Settings));
    xTaskCreate(settings_writer_task, "settings", 4096, nullptr, 2, nullptr);
}

void EspPlatform::save_settings(const app::Settings &s)
{
    if (s_save_queue != nullptr) {
        xQueueOverwrite(s_save_queue, &s);
    } else {
        write_settings(s);
    }
}

bool settings_write_pending() { return s_save_queue != nullptr && uxQueueMessagesWaiting(s_save_queue) > 0; }

const char *EspPlatform::firmware_version() { return esp_app_get_description()->version; }

// --- HAL ------------------------------------------------------------------------------------

void EspHal::set_backlight(uint8_t percent) { board::backlight_set(percent); }

void EspHal::set_rotation_180(bool rotated) { lvgl_port::set_rotation_180(rotated); }

void EspHal::restart()
{
    // Called from the LVGL task: restart from a separate task so the UI lock
    // is not held while we wait for the desk to settle.
    xTaskCreate(
        [](void *arg) {
            safe_restart(static_cast<app::App *>(arg));  // only returns if the desk would not stop
            vTaskDelete(nullptr);
        },
        "restart", 4096, app_, 5, nullptr);
}

void EspHal::describe_system(char *buf, size_t len)
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const board::TouchChip chip = board::touch_chip();
    snprintf(buf, len, "ESP-IDF %s  -  RAM %uk free, PSRAM %uk free  -  touch %s  -  MAC %02X:%02X:%02X:%02X:%02X:%02X",
             esp_app_get_description()->idf_ver,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             chip == board::TouchChip::Axs15231b ? "AXS15231B" : chip == board::TouchChip::Cst3530 ? "CST3530" : "none",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void safe_restart(app::App *app)
{
    if (app != nullptr) {
        // Never drop off the bus while the desk moves: the control box can
        // fall into its reset state. Keep stopping until it is idle.
        const int64_t deadline = esp_timer_get_time() + 20 * 1000 * 1000;
        int64_t last_stop = 0;
        while (app->desk_active() && esp_timer_get_time() < deadline) {
            if (esp_timer_get_time() - last_stop > 1500 * 1000) {
                ESP_LOGW(TAG, "stopping the desk before restarting");
                app->stop();
                last_stop = esp_timer_get_time();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (app->desk_active()) {
            ESP_LOGE(TAG, "desk still active after 20 s; not restarting");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1700));  // let a pending settings save get queued (1.5 s debounce)
        for (int i = 0; i < 30 && settings_write_pending(); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(300));  // an in-flight NVS write
    }
    board::backlight_set(0);
    // Keep "handset present" asserted through the restart.
    gpio_hold_en(static_cast<gpio_num_t>(CONFIG_DESK_WAKE_GPIO));
    gpio_deep_sleep_hold_en();
    esp_sleep_enable_timer_wakeup(50 * 1000);
    esp_deep_sleep_start();
}

// --- Desk UART -------------------------------------------------------------------------------

void DeskUart::begin()
{
    uart_config_t cfg = {};
    cfg.baud_rate = 9600;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(kDeskUart, 1024, 512, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(kDeskUart, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(kDeskUart, CONFIG_DESK_UART_TX_GPIO, CONFIG_DESK_UART_RX_GPIO, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    // Deliver bytes promptly: poll answers should go out within a few ms.
    uart_set_rx_full_threshold(kDeskUart, 1);
    uart_set_rx_timeout(kDeskUart, 1);
    // Idle-high RX so an unplugged cable doesn't produce noise.
    gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_DESK_UART_RX_GPIO), GPIO_PULLUP_ONLY);

    // Drive the wake line high (handset present) before releasing any hold
    // left over from a restart, so it never glitches low.
    gpio_config_t wake = {};
    wake.pin_bit_mask = 1ULL << CONFIG_DESK_WAKE_GPIO;
    wake.mode = GPIO_MODE_OUTPUT;
    gpio_config(&wake);
    started_ = true;
    set_wake_pin(true);
    gpio_hold_dis(static_cast<gpio_num_t>(CONFIG_DESK_WAKE_GPIO));
    gpio_deep_sleep_hold_dis();
    ESP_LOGI(TAG, "desk UART1 TX=%d RX=%d wake=%d", CONFIG_DESK_UART_TX_GPIO, CONFIG_DESK_UART_RX_GPIO,
             CONFIG_DESK_WAKE_GPIO);
}

size_t DeskUart::read(uint8_t *buf, size_t max)
{
    if (!started_) {
        return 0;
    }
    const int n = uart_read_bytes(kDeskUart, buf, max, 0);
    return n > 0 ? static_cast<size_t>(n) : 0;
}

void DeskUart::write(const uint8_t *data, size_t len)
{
    if (started_) {
        uart_write_bytes(kDeskUart, data, len);
    }
}

void DeskUart::set_wake_pin(bool high)
{
    if (!started_) {
        return;
    }
#if CONFIG_DESK_WAKE_ACTIVE_LOW
    high = !high;
#endif
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_DESK_WAKE_GPIO), high ? 1 : 0);
}

void DeskUart::flush_input()
{
    if (started_) {
        uart_flush_input(kDeskUart);
    }
}

void DeskUart::wait_for_data(uint32_t timeout_ms)
{
    size_t available = 0;
    if (started_ && uart_get_buffered_data_len(kDeskUart, &available) == ESP_OK && available > 0) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(timeout_ms));
}

}  // namespace platform
