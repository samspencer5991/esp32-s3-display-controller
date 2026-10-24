idf_component_register(SRCS "board.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd esp_driver_i2c esp_driver_gpio esp_driver_ledc esp_driver_spi esp_timer)
