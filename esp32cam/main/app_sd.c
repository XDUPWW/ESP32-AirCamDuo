#include "app_sd.h"

#include <stdio.h>
#include <string.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"

static const char *TAG = "app_sd";

#define SD_MOUNT_POINT "/sdcard"
#define SD_TASK_STACK  4096
#define SD_TASK_PRIO   2
// A CMD13 every second is enough to notice a card being pulled, and it does not
// touch the filesystem, so it cannot corrupt anything on the way out.
#define SD_PROBE_MS    CONFIG_APP_SD_PROBE_MS
#define SD_RETRY_MS    CONFIG_APP_SD_RETRY_MS
#define SD_FAILS_TO_REMOVE 2

static const sdmmc_host_t s_host = SDMMC_HOST_DEFAULT();
static sdmmc_slot_config_t s_slot;

// Built at run time: a partially filled sdmmc_slot_config_t leaves cd/wp/d1-d3
// at zero, and GPIO0 is the camera's XCLK on this board, so the slot init used to
// take the camera's clock pin away and the frame grab timed out.
static void slot_config(void)
{
    s_slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    s_slot.clk = CONFIG_APP_SD_PIN_CLK;
    s_slot.cmd = CONFIG_APP_SD_PIN_CMD;
    s_slot.d0 = CONFIG_APP_SD_PIN_D0;
    // 1-bit only: D1 shares GPIO4 with the flash LED on this board.
    s_slot.width = 1;
    s_slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    s_slot.cd = SDMMC_SLOT_NO_CD;
    s_slot.wp = SDMMC_SLOT_NO_WP;
    s_slot.d1 = GPIO_NUM_NC;
    s_slot.d2 = GPIO_NUM_NC;
    s_slot.d3 = GPIO_NUM_NC;
}

static SemaphoreHandle_t s_lock;
static sdmmc_card_t *s_card;
static app_sd_state_t s_state;
static int s_fail_streak;
static bool s_complained;

static void publish(bool present, uint32_t capacity_mb, uint32_t free_mb)
{
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_state.present = present;
        if (present) {
            s_state.mounts++;
            s_state.capacity_mb = capacity_mb;
            s_state.free_mb = free_mb;
        } else {
            s_state.capacity_mb = 0;
            s_state.free_mb = 0;
        }
        xSemaphoreGive(s_lock);
    }
}

static bool sd_mount(void)
{
    sdmmc_card_t *card = NULL;
    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &s_host, &s_slot, &mount_cfg, &card);
    if (err != ESP_OK) {
        // The common case is "no card": complain once per absence, then stay
        // quiet while the retry loop runs.
        if (!s_complained) {
            s_complained = true;
            ESP_LOGI(TAG, "no card answering (%s), retrying every %d ms",
                     esp_err_to_name(err), SD_RETRY_MS);
        }
        ESP_LOGD(TAG, "mount failed: %s", esp_err_to_name(err));
        return false;
    }

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_bytes);
    s_card = card;
    s_fail_streak = 0;
    s_complained = false;
    publish(true, (uint32_t)(total / (1024 * 1024)), (uint32_t)(free_bytes / (1024 * 1024)));
    ESP_LOGI(TAG, "card in: %u MB (%u MB free), %s, bus %d kHz",
             (unsigned)(total / (1024 * 1024)), (unsigned)(free_bytes / (1024 * 1024)),
             card->is_mmc ? "MMC" : "SD", card->real_freq_khz);
    return true;
}

static void sd_unmount(const char *reason)
{
    sdmmc_card_t *card = s_card;
    s_card = NULL;
    if (card != NULL) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
        free(card);
    }
    s_fail_streak = 0;
    publish(false, 0, 0);
    ESP_LOGW(TAG, "card out (%s)", reason);
}

static void sd_task(void *arg)
{
    (void)arg;
    // The mount allocates its own buffers; doing it while Wi-Fi and the first
    // frames are coming up costs pbufs (measured: 116 send ENOMEM retries and
    // 45 dropped frames in the first 10 s).
    vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_SD_START_DELAY_MS));
    while (true) {
        if (s_card == NULL) {
            if (!sd_mount()) {
                vTaskDelay(pdMS_TO_TICKS(SD_RETRY_MS));
            }
            continue;
        }

        // CMD13 answers even when the filesystem is idle; a pulled card makes it
        // fail, and two failures in a row are needed to declare the card gone.
        if (sdmmc_get_status(s_card) == ESP_OK) {
            s_fail_streak = 0;
        } else if (++s_fail_streak >= SD_FAILS_TO_REMOVE) {
            sd_unmount("bus timeout");
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(SD_PROBE_MS));
    }
}

esp_err_t app_sd_start(void)
{
#if !defined(CONFIG_APP_SD_ENABLE)
    ESP_LOGI(TAG, "sd card support disabled");
    return ESP_OK;
#else
    // The sdmmc/fatfs drivers log an error per attempt; a board with no card
    // would flood the console, so their tags are muted and the monitor reports
    // the transitions itself.
#if defined(CONFIG_APP_SD_LOG_DEBUG)
    esp_log_level_set("sdmmc_req", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_common", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_init", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_sd", ESP_LOG_DEBUG);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_DEBUG);
#else
    esp_log_level_set("sdmmc_req", ESP_LOG_NONE);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
    esp_log_level_set("SD_HOST", ESP_LOG_NONE);
#endif

    slot_config();
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(sd_task, "app_sd", SD_TASK_STACK, NULL, SD_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "watching sd: clk GPIO%d, cmd GPIO%d, d0 GPIO%d, 1-bit, probe %d ms",
             CONFIG_APP_SD_PIN_CLK, CONFIG_APP_SD_PIN_CMD, CONFIG_APP_SD_PIN_D0, SD_PROBE_MS);
    return ESP_OK;
#endif
}

void app_sd_get(app_sd_state_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_state;
        xSemaphoreGive(s_lock);
    }
}
