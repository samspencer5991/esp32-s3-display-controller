idf_component_register(SRCS "main.cpp" "lvgl_port.cpp" "console.cpp" "platform.cpp" "leds.cpp" "net.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES board app_core desk ui lvgl__lvgl console nvs_flash esp_timer esp_rom
                                esp_driver_uart esp_driver_gpio esp_wifi esp_netif esp_event esp_app_format
                                espressif__mqtt espressif__cjson espressif__led_strip)
