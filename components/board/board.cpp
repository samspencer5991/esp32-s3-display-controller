#include "board/board.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace board {

namespace {

const char *TAG = "board";

// --- I2C / PMU ---------------------------------------------------------------------

constexpr uint8_t kAddrAxsTouch = 0x3B;
constexpr uint8_t kAddrCstTouch = 0x58;
constexpr uint8_t kAddrCstBoot = 0x5A;
constexpr uint8_t kAddrPmu = 0x6A;

i2c_master_bus_handle_t s_i2c = nullptr;
i2c_master_dev_handle_t s_touch_dev = nullptr;
uint8_t s_scan[16];
size_t s_scan_count = 0;
bool s_pmu = false;
TouchChip s_touch = TouchChip::None;

esp_err_t i2c_init()
{
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = static_cast<gpio_num_t>(pins::kI2cSda);
    cfg.scl_io_num = static_cast<gpio_num_t>(pins::kI2cScl);
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = false;  // 4.7k on the board
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c), TAG, "i2c bus");

    s_scan_count = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_i2c, addr, 20) == ESP_OK && s_scan_count < sizeof(s_scan)) {
            s_scan[s_scan_count++] = addr;
        }
    }
    char line[96] = {};
    size_t pos = 0;
    for (size_t i = 0; i < s_scan_count && pos + 6 < sizeof(line); i++) {
        pos += snprintf(line + pos, sizeof(line) - pos, " 0x%02X", s_scan[i]);
    }
    ESP_LOGI(TAG, "I2C devices:%s", s_scan_count ? line : " none");
    return ESP_OK;
}

bool i2c_found(uint8_t addr)
{
    return std::find(s_scan, s_scan + s_scan_count, addr) != s_scan + s_scan_count;
}

i2c_master_dev_handle_t add_device(uint8_t addr)
{
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = addr;
    dev.scl_speed_hz = 400000;
    i2c_master_dev_handle_t handle = nullptr;
    if (i2c_master_bus_add_device(s_i2c, &dev, &handle) != ESP_OK) {
        return nullptr;
    }
    return handle;
}

void pmu_init()
{
    if (!i2c_found(kAddrPmu)) {
        return;
    }
    i2c_master_dev_handle_t pmu = add_device(kAddrPmu);
    if (pmu == nullptr) {
        return;
    }
    s_pmu = true;

    // SY6970 (BQ25895 compatible):
    //  REG07: disable the charge-status LED (it blinks without a battery) and
    //         the I2C watchdog (which would revert our settings).
    //  REG00: ignore the ILIM pin and raise the input current limit. The
    //         0.53 A hardware default browns the board out under WiFi load
    //         and is linked to phantom touches (LilyGO commit 1f45ea2).
    uint8_t reg = 0x07, val = 0;
    if (i2c_master_transmit_receive(pmu, &reg, 1, &val, 1, 50) == ESP_OK) {
        const uint8_t w07[] = {0x07, static_cast<uint8_t>((val & ~0x30) | 0x40)};
        i2c_master_transmit(pmu, w07, sizeof(w07), 50);
    }
    const uint8_t w00[] = {0x00, 0x3F};
    esp_err_t err = i2c_master_transmit(pmu, w00, sizeof(w00), 50);
    ESP_LOGI(TAG, "PMU SY6970 configured (%s)", esp_err_to_name(err));
}

// --- Display ------------------------------------------------------------------------

esp_lcd_panel_io_handle_t s_io = nullptr;
SemaphoreHandle_t s_flush_done = nullptr;
int64_t s_push_started_us = 0;
volatile uint32_t s_last_transfer_ms = 0;

constexpr int kLcdCmdWrite = 0x02;
constexpr int kLcdCmdPixels = 0x32;

inline int reg_cmd(uint8_t reg) { return (kLcdCmdWrite << 24) | (reg << 8); }

bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *)
{
    BaseType_t woken = pdFALSE;
    s_last_transfer_ms = static_cast<uint32_t>((esp_timer_get_time() - s_push_started_us) / 1000);
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

esp_err_t lcd_cmd(uint8_t reg, const uint8_t *params, size_t len, uint32_t delay_ms)
{
    esp_err_t err = esp_lcd_panel_io_tx_param(s_io, reg_cmd(reg), params, len);
    if (delay_ms) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    return err;
}

esp_err_t display_init()
{
    gpio_config_t rst = {};
    rst.pin_bit_mask = 1ULL << pins::kLcdRst;
    rst.mode = GPIO_MODE_OUTPUT;
    gpio_config(&rst);
    // Reset timing from LilyGO's driver.
    gpio_set_level(static_cast<gpio_num_t>(pins::kLcdRst), 1);
    vTaskDelay(pdMS_TO_TICKS(130));
    gpio_set_level(static_cast<gpio_num_t>(pins::kLcdRst), 0);
    vTaskDelay(pdMS_TO_TICKS(130));
    gpio_set_level(static_cast<gpio_num_t>(pins::kLcdRst), 1);
    vTaskDelay(pdMS_TO_TICKS(300));

    spi_bus_config_t bus = {};
    bus.data0_io_num = pins::kLcdD0;
    bus.data1_io_num = pins::kLcdD1;
    bus.data2_io_num = pins::kLcdD2;
    bus.data3_io_num = pins::kLcdD3;
    bus.sclk_io_num = pins::kLcdSck;
    bus.data4_io_num = -1;
    bus.data5_io_num = -1;
    bus.data6_io_num = -1;
    bus.data7_io_num = -1;
    bus.max_transfer_sz = kPanelWidth * 80 * 2;  // 80 rows per chunk
    bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    s_flush_done = xSemaphoreCreateBinary();
    xSemaphoreGive(s_flush_done);

    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = static_cast<gpio_num_t>(pins::kLcdCs);
    io.dc_gpio_num = GPIO_NUM_NC;
    io.spi_mode = 3;
    io.pclk_hz = 32 * 1000 * 1000;  // 40 MHz shows corruption on this panel
    io.trans_queue_depth = 10;
    io.on_color_trans_done = on_color_done;
    io.lcd_cmd_bits = 32;
    io.lcd_param_bits = 8;
    io.flags.quad_mode = true;
    io.flags.psram_dma_direct = true;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(SPI2_HOST, &io, &s_io), TAG, "panel io");

    // The panel loads its vendor configuration from internal flash; the host
    // only needs to cycle sleep and switch the display on.
    const uint8_t zero = 0x00;
    const uint8_t rgb565 = 0x55;
    const uint8_t caset[] = {0x00, 0x00, 0x00, kPanelWidth - 1};
    const uint8_t raset[] = {0x00, 0x00, (kPanelHeight - 1) >> 8, (kPanelHeight - 1) & 0xFF};
    lcd_cmd(0x28, nullptr, 0, 20);     // DISPOFF
    lcd_cmd(0x10, nullptr, 0, 150);    // SLPIN
    lcd_cmd(0x11, nullptr, 0, 150);    // SLPOUT
    lcd_cmd(0x20, nullptr, 0, 0);      // INVOFF
    lcd_cmd(0x36, &zero, 1, 0);        // MADCTL: RGB, no mirroring
    lcd_cmd(0x3A, &rgb565, 1, 0);      // COLMOD: RGB565
    lcd_cmd(0x2A, caset, sizeof(caset), 0);
    lcd_cmd(0x2B, raset, sizeof(raset), 0);
    lcd_cmd(0x13, nullptr, 0, 0);      // NORON
    lcd_cmd(0x29, nullptr, 0, 20);     // DISPON
    uint8_t power_mode = 0;
    const esp_err_t rd = esp_lcd_panel_io_rx_param(s_io, (0x03 << 24) | (0x0A << 8), &power_mode, 1);
    ESP_LOGI(TAG, "AXS15231B panel initialised (RDDPM=0x%02X %s)", power_mode, esp_err_to_name(rd));
    return ESP_OK;
}

// --- Backlight ------------------------------------------------------------------------

void backlight_init()
{
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = 2000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {};
    ch.gpio_num = pins::kBacklight;
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel = LEDC_CHANNEL_0;
    ch.timer_sel = LEDC_TIMER_0;
    ch.duty = 0;
    ledc_channel_config(&ch);
}

// --- Touch ------------------------------------------------------------------------------

volatile bool s_touch_irq = false;
bool s_touch_was_pressed = false;
TouchPoint s_last_point = {};

uint8_t s_last_raw[9] = {};       // raw bytes of the last controller read
uint8_t s_press_raw[9] = {};      // raw bytes when the current press was confirmed
int64_t s_press_start_us = 0;
uint32_t s_press_samples = 0;

constexpr uint8_t kTouchConfirmSamples = 2;
constexpr int kTouchConfirmRadius = 24;
uint8_t s_pending_samples = 0;
TouchPoint s_pending_point = {};
uint32_t s_rejected_touches = 0;

void IRAM_ATTR touch_isr(void *) { s_touch_irq = true; }

void touch_init()
{
    if (i2c_found(kAddrAxsTouch)) {
        s_touch = TouchChip::Axs15231b;
        s_touch_dev = add_device(kAddrAxsTouch);
    } else {
        // CST3530 revision: its reset line is GPIO2. Only drive it when the
        // AXS15231B touch is absent (on older boards GPIO2 is the battery ADC).
        gpio_config_t rst = {};
        rst.pin_bit_mask = 1ULL << pins::kTouchRstCst;
        rst.mode = GPIO_MODE_OUTPUT;
        gpio_config(&rst);
        gpio_set_level(static_cast<gpio_num_t>(pins::kTouchRstCst), 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(static_cast<gpio_num_t>(pins::kTouchRstCst), 1);
        vTaskDelay(pdMS_TO_TICKS(60));
        for (uint8_t addr : {kAddrCstTouch, kAddrCstBoot}) {
            if (i2c_master_probe(s_i2c, addr, 20) == ESP_OK) {
                s_touch = TouchChip::Cst3530;
                s_touch_dev = add_device(addr);
                break;
            }
        }
    }

    if (s_touch == TouchChip::None || s_touch_dev == nullptr) {
        ESP_LOGW(TAG, "no touch controller found");
        s_touch = TouchChip::None;
        return;
    }
    ESP_LOGI(TAG, "touch controller: %s", s_touch == TouchChip::Axs15231b ? "AXS15231B" : "CST3530");

    gpio_config_t irq = {};
    irq.pin_bit_mask = 1ULL << pins::kTouchInt;
    irq.mode = GPIO_MODE_INPUT;
    irq.pull_up_en = GPIO_PULLUP_ENABLE;
    irq.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&irq);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(static_cast<gpio_num_t>(pins::kTouchInt), touch_isr, nullptr);
}

bool read_axs(TouchPoint &out)
{
    static const uint8_t cmd[] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
    uint8_t buf[8] = {};
    if (i2c_master_transmit(s_touch_dev, cmd, sizeof(cmd), 30) != ESP_OK ||
        i2c_master_receive(s_touch_dev, buf, sizeof(buf), 30) != ESP_OK) {
        return false;
    }
    memcpy(s_last_raw, buf, sizeof(buf));
    const uint8_t fingers = buf[1];
    const uint8_t event = buf[2] >> 4;
    out.pressed = fingers == 1 && event == 0x08;
    if (out.pressed) {
        const int short_axis = ((buf[4] & 0x0F) << 8) | buf[5];
        const int long_axis = ((buf[2] & 0x0F) << 8) | buf[3];
        out.x = static_cast<uint16_t>(std::clamp(short_axis, 0, kPanelWidth - 1));
        out.y = static_cast<uint16_t>(std::clamp(kPanelHeight - long_axis, 0, kPanelHeight - 1));
    }
    return true;
}

bool read_cst(TouchPoint &out)
{
    static const uint8_t cmd[] = {0xD0, 0x07, 0x00, 0x00};
    static const uint8_t ack[] = {0xD0, 0x00, 0x02, 0xAB};
    uint8_t buf[9] = {};
    if (i2c_master_transmit_receive(s_touch_dev, cmd, sizeof(cmd), buf, sizeof(buf), 30) != ESP_OK) {
        return false;
    }
    i2c_master_transmit(s_touch_dev, ack, sizeof(ack), 30);
    memcpy(s_last_raw, buf, sizeof(buf));
    out.pressed = false;
    if (buf[2] == 0xFF && (buf[3] & 0x0F) > 0) {
        const int x = buf[4] | ((buf[7] & 0x0F) << 8);
        const int y = buf[5] | ((buf[7] & 0xF0) << 4);
        out.pressed = true;
        out.x = static_cast<uint16_t>(std::clamp(x, 0, kPanelWidth - 1));
        out.y = static_cast<uint16_t>(std::clamp(y, 0, kPanelHeight - 1));
    }
    return true;
}

}  // namespace

esp_err_t init()
{
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "i2c");
    pmu_init();
    backlight_init();
    ESP_RETURN_ON_ERROR(display_init(), TAG, "display");
    touch_init();
    return ESP_OK;
}

esp_err_t display_push(const uint8_t *frame_be565, int rows)
{
    rows = std::clamp(rows, 1, kPanelHeight);
    display_wait_done();
    xSemaphoreTake(s_flush_done, 0);
    s_push_started_us = esp_timer_get_time();
    const esp_err_t err = esp_lcd_panel_io_tx_color(s_io, (kLcdCmdPixels << 24) | (0x2C << 8), frame_be565,
                                                    static_cast<size_t>(rows) * kPanelWidth * 2);
    if (err != ESP_OK) {
        xSemaphoreGive(s_flush_done);
    }
    return err;
}

void display_wait_done()
{
    if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(500)) == pdTRUE) {
        xSemaphoreGive(s_flush_done);
    } else {
        ESP_LOGW(TAG, "display flush timeout");
    }
}

uint32_t display_last_transfer_ms() { return s_last_transfer_ms; }

uint8_t display_read_register(uint8_t reg)
{
    display_wait_done();
    uint8_t value = 0;
    esp_lcd_panel_io_rx_param(s_io, (0x03 << 24) | (reg << 8), &value, 1);
    return value;
}

esp_err_t display_sleep(bool sleep)
{
    display_wait_done();
    return lcd_cmd(sleep ? 0x10 : 0x11, nullptr, 0, 120);
}

void backlight_set(uint8_t percent)
{
    percent = std::min<uint8_t>(percent, 100);
    // Perceptual curve so low settings are still usable.
    uint32_t duty = (static_cast<uint32_t>(percent) * percent * 1023) / 10000;
    if (percent > 0 && duty < 2) {
        duty = 2;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

TouchChip touch_chip() { return s_touch; }

bool touch_read(TouchPoint &out)
{
    if (s_touch == TouchChip::None) {
        out.pressed = false;
        return false;
    }
    // Only talk to the controller after an interrupt or while a finger is
    // (possibly) down.
    if (!s_touch_irq && !s_touch_was_pressed && s_pending_samples == 0) {
        out = s_last_point;
        out.pressed = false;
        return true;
    }
    s_touch_irq = false;
    TouchPoint p = s_last_point;
    const bool ok = s_touch == TouchChip::Axs15231b ? read_axs(p) : read_cst(p);
    if (!ok) {
        s_pending_samples = 0;
        s_touch_was_pressed = false;
        out = s_last_point;
        out.pressed = false;
        return false;
    }

    // Debounce: the AXS15231B occasionally reports single-sample phantom
    // touches. A press is only accepted once it has been seen in
    // kTouchConfirmSamples consecutive reads at a consistent position.
    if (p.pressed && !s_touch_was_pressed) {
        const bool consistent = s_pending_samples == 0 ||
                                (std::abs(p.x - s_pending_point.x) <= kTouchConfirmRadius &&
                                 std::abs(p.y - s_pending_point.y) <= kTouchConfirmRadius);
        s_pending_samples = consistent ? s_pending_samples + 1 : 1;
        s_pending_point = p;
        if (s_pending_samples < kTouchConfirmSamples) {
            out = s_last_point;
            out.pressed = false;
            return true;
        }
    } else if (!p.pressed) {
        if (s_pending_samples > 0 && !s_touch_was_pressed) {
            s_rejected_touches++;
            ESP_LOGW(TAG, "rejected touch glitch at panel %u,%u (%u sample%s)", s_pending_point.x,
                     s_pending_point.y, s_pending_samples, s_pending_samples == 1 ? "" : "s");
        }
        s_pending_samples = 0;
    }

    // Diagnostics: duration and raw bytes of every accepted press, to tell
    // phantom touches (fixed spot, fixed duration) from real fingers.
    if (p.pressed && !s_touch_was_pressed) {
        s_press_start_us = esp_timer_get_time();
        s_press_samples = 0;
        memcpy(s_press_raw, s_last_raw, sizeof(s_press_raw));
    }
    if (p.pressed) {
        s_press_samples++;
    } else if (s_touch_was_pressed) {
        ESP_LOGI(TAG, "touch up: panel %u,%u held %lld ms (%lu samples) raw %02X %02X %02X %02X %02X %02X %02X %02X",
                 s_last_point.x, s_last_point.y, (esp_timer_get_time() - s_press_start_us) / 1000,
                 static_cast<unsigned long>(s_press_samples), s_press_raw[0], s_press_raw[1], s_press_raw[2],
                 s_press_raw[3], s_press_raw[4], s_press_raw[5], s_press_raw[6], s_press_raw[7]);
    }
    if (p.pressed) {
        s_last_point = p;
    }
    s_touch_was_pressed = p.pressed;
    s_pending_samples = p.pressed ? s_pending_samples : 0;
    out = p.pressed ? p : s_last_point;
    out.pressed = p.pressed;
    return true;
}

uint32_t touch_rejected_count() { return s_rejected_touches; }

size_t i2c_scan_results(uint8_t *out, size_t max)
{
    const size_t n = std::min(max, s_scan_count);
    memcpy(out, s_scan, n);
    return n;
}

bool pmu_present() { return s_pmu; }

}  // namespace board
