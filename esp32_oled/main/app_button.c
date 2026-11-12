#include "app_button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LONG_PRESS_US 500000
#define POLL_MS       10
#define DEBOUNCE_MS   30
#define TASK_STACK    3072
#define TASK_PRIORITY 4

static volatile bool s_short;
static volatile bool s_long;

static inline bool is_pressed(void)
{
#if defined(CONFIG_APP_BUTTON_ACTIVE_LOW)
    return gpio_get_level(APP_BUTTON_GPIO) == 0;
#else
    return gpio_get_level(APP_BUTTON_GPIO) == 1;
#endif
}

static void on_edge(bool pressed, int64_t now, bool *long_fired, int64_t *press_us)
{
    if (pressed) {
        *press_us = now;
        *long_fired = false;
    } else if (!*long_fired && now - *press_us < LONG_PRESS_US) {
        s_short = true;
    }
}

static void button_task(void *arg)
{
    (void)arg;

    bool stable = is_pressed();
    int64_t press_us = esp_timer_get_time();
    bool long_fired = false;
    int mismatch_ms = 0;

    while (true) {
        const bool raw = is_pressed();
        const int64_t now = esp_timer_get_time();

        if (raw == stable) {
            mismatch_ms = 0;
        } else if (++mismatch_ms * POLL_MS >= DEBOUNCE_MS) {
            stable = raw;
            mismatch_ms = 0;
            if (stable) {
                ESP_LOGI("app_button", "pressed");
            } else {
                ESP_LOGI("app_button", "released after %lld ms",
                         (long long)((now - press_us) / 1000));
            }
            on_edge(stable, now, &long_fired, &press_us);
        }

        // A long press acts immediately, without waiting for the release.
        if (stable && !long_fired && now - press_us >= LONG_PRESS_US) {
            long_fired = true;
            s_long = true;
            ESP_LOGI("app_button", "long press");
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t app_button_start(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << APP_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
#if defined(CONFIG_APP_BUTTON_ACTIVE_LOW)
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#else
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE("app_button", "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI("app_button", "GPIO%d, %s, idle level %d", APP_BUTTON_GPIO,
             is_pressed() ? "pressed at start" : "released at start",
             gpio_get_level(APP_BUTTON_GPIO));
    if (xTaskCreate(button_task, "button", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool app_button_take_short(void)
{
    if (s_short) {
        s_short = false;
        return true;
    }
    return false;
}

bool app_button_take_long(void)
{
    if (s_long) {
        s_long = false;
        return true;
    }
    return false;
}
