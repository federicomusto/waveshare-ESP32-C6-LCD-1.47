idf_component_register(SRCS "main.c" "wifi_manager.c" "shell.c" "ble_scan.c"
                       INCLUDE_DIRS "."
                       REQUIRES st7789 led_strip esp_wifi esp_netif esp_event
                                 nvs_flash lwip esp_timer esp_driver_gpio
                                 esp_driver_i2c spi_flash bt)
