idf_component_register(SRCS "main.c" "wifi_manager.c" "shell.c" "ota.c"
                       INCLUDE_DIRS "."
                       REQUIRES st7789 sd_card led_strip esp_wifi esp_netif esp_event
                                 nvs_flash lwip esp_timer esp_driver_gpio
                                 esp_driver_i2c esp_http_client app_update
                                 esp_partition spi_flash)
