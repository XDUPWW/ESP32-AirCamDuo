#include "app_camera.h"
#include "app_http.h"
#include "app_udp.h"
#include "app_wifi.h"
#include "cam_selftest.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_psram.h"
#include "nvs_flash.h"

static const char *TAG = "esp32cam";

static void log_board(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "chip: %s, %d core(s), rev v%d.%d", CONFIG_IDF_TARGET, chip.cores,
             chip.revision / 100, chip.revision % 100);
    ESP_LOGI(TAG, "flash: %u MB", (unsigned)(flash_size / (1024 * 1024)));
    ESP_LOGI(TAG, "psram: %u KB (driver reported %u KB)",
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(esp_psram_get_size() / 1024));
}

void app_main(void)
{
    log_board();
    ESP_LOGI(TAG, "reset reason: %d", (int)esp_reset_reason());

    // Wi-Fi keeps its calibration data in NVS and refuses to start without it.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // A missing sensor must not keep the board off the network: the web UI and
    // /status stay useful while the camera problem is being sorted out.
    err = app_cam_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(err));
        app_cam_selftest();
    }

    err = app_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        return;
    }

    err = app_http_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http server failed: %s", esp_err_to_name(err));
        return;
    }

    err = app_udp_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "udp sender failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "web ui on %s, camera %s", app_wifi_url(),
             app_cam_available() ? app_cam_sensor_name() : "UNAVAILABLE");
    ESP_LOGI(TAG, "endpoints: / /stream /jpg /raw /mode?set=jpeg|gray /status");
}
