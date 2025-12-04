idf_component_register(SRCS "sensor_system.c"
                              "button_handler.c" 
                              "oled_manager.c"
                              "ssd1306.c"
                              "ssd1306_i2c_new.c"
                              "ssd1306_spi.c"
                              "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_adc)