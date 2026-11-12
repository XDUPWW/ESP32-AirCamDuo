idf_component_register(SRCS "main.c" "ssd1306.c" "font5x7.c"
                            "app_wifi_ap.c" "app_button.c" "cam_client.c" "image_view.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_i2c esp_driver_gpio esp_timer nvs_flash
                                esp_wifi esp_netif esp_event
                       PRIV_REQUIRES lwip esp_jpeg)
