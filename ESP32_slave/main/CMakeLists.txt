idf_component_register(SRCS "main.c"
                       "wifi_sta.c"
                       "mqtt_link.c"
                       "dev_cmd.c"
                       "led_ctrl.c"
                       "status_ui.c"
                       "../driver/ST7789.c"
                       "../driver/LVGL_Driver.c"
                       "../driver/Vernon_ST7789T/Vernon_ST7789T.c"
                       INCLUDE_DIRS "." "../driver" "../driver/Vernon_ST7789T"
                       PRIV_REQUIRES mqtt esp_wifi esp_netif esp_event
                                    nvs_flash cjson led_strip
                                    esp_lcd esp_driver_spi esp_driver_gpio
                                    esp_driver_ledc lvgl__lvgl)
