#include "cam_selftest.h"

#include <stdint.h>
#include <string.h>

#include "camera_pins.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cam_selftest";

#define SCCB_PORT I2C_NUM_1
#define SCCB_SPEED_HZ 100000
#define SCCB_TIMEOUT_MS 30

// The camera driver owns LEDC_TIMER_0/LEDC_CHANNEL_0 for XCLK, so reusing that
// channel keeps the GPIO reservation valid after a failed esp_camera_init().
static void drive_xclk(int pin)
{
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) {
        return;
    }

    ledc_channel_config_t channel = {
        .gpio_num = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 1,
        .hpoint = 0,
    };
    ledc_channel_config(&channel);
    vTaskDelay(pdMS_TO_TICKS(15));
}

// Returns the number of devices that answered; logs the OV2640 id registers for
// anything found.
static int scan_bus(int sda, int scl)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = SCCB_PORT,
        .scl_io_num = scl,
        .sda_io_num = sda,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        return 0;
    }

    int count = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, SCCB_TIMEOUT_MS) != ESP_OK) {
            continue;
        }
        count++;

        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = SCCB_SPEED_HZ,
        };
        i2c_master_dev_handle_t dev = NULL;
        if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) == ESP_OK) {
            uint8_t reg = 0x0A;
            uint8_t pid = 0;
            uint8_t ver = 0;
            esp_err_t e1 = i2c_master_transmit_receive(dev, &reg, 1, &pid, 1, SCCB_TIMEOUT_MS);
            reg = 0x0B;
            esp_err_t e2 = i2c_master_transmit_receive(dev, &reg, 1, &ver, 1, SCCB_TIMEOUT_MS);
            ESP_LOGW(TAG, "  0x%02x acked; PID reg0x0A=%s/0x%02x VER reg0x0B=%s/0x%02x", addr,
                     esp_err_to_name(e1), pid, esp_err_to_name(e2), ver);
            i2c_master_bus_rm_device(dev);
        }
    }

    i2c_del_master_bus(bus);
    return count;
}

void app_cam_selftest(void)
{
    ESP_LOGI(TAG, "no sensor found; brute-forcing XCLK x SCCB pin combinations");
    // GPIO16/17 carry PSRAM, 6-11 flash and 1/3 the console, so they are skipped.
    static const int xclk_pins[] = { 0, 21, 22, 23, 25, 26, 27, 13, 14 };

    static const struct {
        int sda;
        int scl;
        const char *name;
    } pairs[] = {
        { 26, 27, "AI-Thinker" },
        { 27, 26, "AI-Thinker swapped" },
        { 25, 23, "M5Stack" },
        { 23, 25, "M5Stack swapped" },
        { 22, 21, "ESP-EYE" },
        { 21, 22, "ESP-EYE swapped" },
        { 14, 15, "generic 14/15" },
        { 13, 14, "generic 13/14" },
        { 18, 23, "generic 18/23" },
    };

    int total = 0;
    int ai_hits = 0;
    for (size_t x = 0; x < sizeof(xclk_pins) / sizeof(xclk_pins[0]); x++) {
        for (size_t p = 0; p < sizeof(pairs) / sizeof(pairs[0]); p++) {
            if (xclk_pins[x] == pairs[p].sda || xclk_pins[x] == pairs[p].scl) {
                continue;
            }
            drive_xclk(xclk_pins[x]);
            int found = scan_bus(pairs[p].sda, pairs[p].scl);
            if (found > 0) {
                ESP_LOGW(TAG, "xclk=%d %s (sda=%d scl=%d): %d device(s)", xclk_pins[x],
                         pairs[p].name, pairs[p].sda, pairs[p].scl, found);
                total += found;
                if (pairs[p].sda == CAM_PIN_SIOD && pairs[p].scl == CAM_PIN_SIOC) {
                    ai_hits += found;
                }
            }
        }
    }

    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);

    // Some ESP32-CAM clones wire the sensor's PWDN to a GPIO instead of leaving
    // it unconnected, so hold each candidate low and retry the AI-Thinker bus.
    static const int pwdn_candidates[] = { 32, 33, 2, 4, 13, 14, 15 };
    int pwdn_hits = 0;
    {
        drive_xclk(CAM_PIN_XCLK);
        for (size_t i = 0; i < sizeof(pwdn_candidates) / sizeof(pwdn_candidates[0]); i++) {
            int pin = pwdn_candidates[i];
            gpio_config_t cfg = {
                .pin_bit_mask = 1ULL << pin,
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE,
            };
            gpio_config(&cfg);
            gpio_set_level(pin, 0);
            vTaskDelay(pdMS_TO_TICKS(20));

            int found = scan_bus(CAM_PIN_SIOD, CAM_PIN_SIOC);
            if (found > 0) {
                ESP_LOGW(TAG, "PWDN held low on GPIO%d -> %d device(s) appeared", pin, found);
                pwdn_hits += found;
            }
            gpio_set_level(pin, 1);
            gpio_reset_pin(pin);
        }
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        if (pwdn_hits > 0) {
            ESP_LOGW(TAG, "selftest: PWDN is wired to a GPIO on this board");
        }
    }

    if (ai_hits == 0 && pwdn_hits == 0) {
        ESP_LOGE(TAG, "selftest: nothing on the AI-Thinker bus with any XCLK/PWDN -> wiring/power");
    } else {
        ESP_LOGW(TAG, "selftest: %d response(s) on the standard bus", ai_hits + pwdn_hits);
    }
    if (total > ai_hits) {
        ESP_LOGW(TAG, "selftest: %d response(s) elsewhere are floating-pin noise", total - ai_hits);
    }
}
