// Smart desk controller firmware entry point.
//
//  desk task  (core 0): App::tick - desk protocol, settings, reminders
//  lvgl task  (core 1): LVGL rendering + UI
//  leds / net / console tasks: see leds.cpp, net.cpp, console.cpp
#include "app/app.h"
#include "board/board.h"
#include "console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leds.h"
#include "lvgl_port.h"
#include "net.h"
#include "nvs_flash.h"
#include "platform.h"
#include "sdkconfig.h"
#include "ui/ui.h"

static const char *TAG = "main";

static void desk_task(void *arg)
{
    auto *app = static_cast<app::App *>(arg);
    while (true) {
        app->tick();
        // ~4 ms: control box polls must be answered promptly.
        vTaskDelay(pdMS_TO_TICKS(4));
    }
}

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // The desk wake line first: "handset present" as early as possible.
    static platform::DeskUart uart;
    uart.begin();
    static platform::EspPlatform platform;
    platform.begin();

    app::AppConfig config;
#ifdef CONFIG_DESK_REPORTS_INCHES
    config.desk_reports_inches = true;
#endif
#ifndef CONFIG_DESK_DEMO_MODE_DEFAULT
    config.demo_mode_default = false;
#endif
    static app::App app(platform, &uart, config);
    app.begin();
    xTaskCreatePinnedToCore(desk_task, "desk", 4096, &app, 6, nullptr, 0);

    static platform::EspHal hal(&app);
    ESP_ERROR_CHECK(board::init());
    lv_display_t *disp = lvgl_port::init(app.settings().rotate_180);
    if (lvgl_port::lock()) {
        ui::init(disp, app, hal);
        lvgl_port::unlock();
    }
    lvgl_port::set_touch_filter(ui::touch_gate);

    leds::start(&app);
    net::start(&app);
    console::start(&app);
    ESP_LOGI(TAG, "desk controller running");
}
