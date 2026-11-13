idf_component_register(
    SRCS "main.c" "app_camera.c" "app_wifi.c" "app_http.c" "app_udp.c" "app_sd.c"
         "cam_selftest.c"
    INCLUDE_DIRS "."
    REQUIRES esp32-camera
    PRIV_REQUIRES spi_flash esp_psram esp_http_server esp_wifi esp_netif esp_event esp_timer
                  esp_driver_i2c esp_driver_ledc esp_driver_gpio nvs_flash tjpgd_gray
                  esp_driver_sdmmc sdmmc fatfs
    EMBED_TXTFILES "www/index.html"
)
