#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_button.h"
#include "app_wifi_ap.h"
#include "cam_client.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "image_view.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "ssd1306.h"

static const char *TAG = "c5view";

/* Same wiring as the bring-up firmware: the module silkscreen is reversed, so
   the panel only answers with SDA on GPIO8 and SCL on GPIO9. Both orders are
   still tried, in case a different module is plugged in. */
#define PIN_SDA         8
#define PIN_SCL         9

#define I2C_PORT        I2C_NUM_0
#define I2C_FREQ_HZ     400000

#define SSD1306_ADDR_A  0x3C
#define SSD1306_ADDR_B  0x3D

#define PANEL_HEIGHT    64
#define LINE_H          8
#define MAX_LINES       (PANEL_HEIGHT / LINE_H)

/* Room for a 176x144 QCIF grey frame, the largest the pipeline can produce. */
#define FRAME_BUF_BYTES (32 * 1024)

static esp_err_t bus_open(int sda, int scl, i2c_master_bus_handle_t *out)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, out);
}

static int bus_scan(i2c_master_bus_handle_t bus, uint8_t *found, int max)
{
    int count = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            if (count < max) {
                found[count] = addr;
            }
            count++;
        }
    }
    return count > max ? max : count;
}

static uint8_t pick_display_addr(const uint8_t *found, int count)
{
    for (int i = 0; i < count; i++) {
        if (found[i] == SSD1306_ADDR_A) {
            return SSD1306_ADDR_A;
        }
    }
    for (int i = 0; i < count; i++) {
        if (found[i] == SSD1306_ADDR_B) {
            return SSD1306_ADDR_B;
        }
    }
    return found[0];
}

static ssd1306_t *oled_open(void)
{
    for (int swapped = 0; swapped < 2; swapped++) {
        const int sda = swapped ? PIN_SCL : PIN_SDA;
        const int scl = swapped ? PIN_SDA : PIN_SCL;

        i2c_master_bus_handle_t bus = NULL;
        if (bus_open(sda, scl, &bus) != ESP_OK) {
            continue;
        }

        uint8_t found[8];
        const int count = bus_scan(bus, found, sizeof(found));
        if (count > 0) {
            const uint8_t addr = pick_display_addr(found, count);
            ssd1306_t *disp = calloc(1, sizeof(*disp));
            if (disp != NULL &&
                ssd1306_init(disp, bus, addr, PANEL_HEIGHT, I2C_FREQ_HZ) == ESP_OK) {
                ESP_LOGI(TAG, "ssd1306 %dx%d at 0x%02X, SDA=GPIO%d SCL=GPIO%d",
                         SSD1306_WIDTH, PANEL_HEIGHT, addr, sda, scl);
                if (swapped) {
                    ESP_LOGW(TAG, "panel answers with the i2c pins swapped");
                }
                return disp;
            }
            free(disp);
        }

        ESP_ERROR_CHECK(i2c_del_master_bus(bus));
    }
    return NULL;
}

static void show_lines(ssd1306_t *disp, const char *const *lines, int count)
{
    ssd1306_clear(disp);
    for (int i = 0; i < count && i < MAX_LINES; i++) {
        if (lines[i] != NULL && lines[i][0] != '\0') {
            ssd1306_text(disp, 0, i * LINE_H, lines[i], 1);
        }
    }
    if (ssd1306_flush(disp) != ESP_OK) {
        ESP_LOGW(TAG, "display write failed");
    }
}

static void show_boot(ssd1306_t *disp)
{
    char ap[40];
    char pw[40];
    char ip[40];
    snprintf(ap, sizeof(ap), "AP %s", CONFIG_APP_AP_SSID);
    snprintf(pw, sizeof(pw), "PW %s", CONFIG_APP_AP_PASSWORD);
    snprintf(ip, sizeof(ip), "IP %s", app_wifi_ap_ip());

    const char *lines[] = {
        "ESP32-C5 CAM VIEWER", "", ap, pw, ip, "", "CAM: WAITING",
    };
    show_lines(disp, lines, sizeof(lines) / sizeof(lines[0]));
}

static void show_waiting(ssd1306_t *disp, int stations)
{
    char sta[40];
    snprintf(sta, sizeof(sta), "STATIONS %d", stations);

    const char *lines[] = {
        "ESP32-C5 CAM VIEWER", "", "WAITING FOR CAM...", sta, "", "UDP PORT 5000",
    };
    show_lines(disp, lines, sizeof(lines) / sizeof(lines[0]));
}

static void show_cam_lost(ssd1306_t *disp)
{
    const char *lines[] = {
        "ESP32-C5 CAM VIEWER", "", "CAMERA LOST", "RECONNECTING...",
    };
    show_lines(disp, lines, sizeof(lines) / sizeof(lines[0]));
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ssd1306_t *disp = oled_open();
    if (disp == NULL) {
        ESP_LOGE(TAG, "no ssd1306 found on GPIO%d/GPIO%d", PIN_SDA, PIN_SCL);
        abort();
    }

    err = app_wifi_ap_start(CONFIG_APP_AP_SSID, CONFIG_APP_AP_PASSWORD);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "softap failed: %s", esp_err_to_name(err));
        abort();
    }

    show_boot(disp);
    vTaskDelay(pdMS_TO_TICKS(1000));

    uint8_t *frame_buf = malloc(FRAME_BUF_BYTES);
    if (frame_buf == NULL) {
        ESP_LOGE(TAG, "no memory for the display buffer");
        abort();
    }

    ESP_ERROR_CHECK(cam_client_start());
    ESP_ERROR_CHECK(app_button_start());

    bool photo_mode = false;
    bool have_frame = false;
    bool lost_shown = false;
    int consecutive_timeouts = 0;
    int last_stations = -1;
    unsigned long frames = 0;
    unsigned long frames_since_log = 0;
    unsigned long errors = 0;
    unsigned long timeouts_since_log = 0;
    int64_t last_log = esp_timer_get_time();
    int64_t grab_us_sum = 0;
    int64_t render_us_sum = 0;
    int64_t flush_us_sum = 0;

    while (true) {
        if (app_button_take_short() && !photo_mode) {
            photo_mode = true;
            ESP_LOGI(TAG, "photo: frame frozen (hold IO0 500 ms to resume)");
        }
        if (app_button_take_long() && photo_mode) {
            photo_mode = false;
            ESP_LOGI(TAG, "photo: back to video");
        }
        if (photo_mode) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        cam_frame_t frame;
        const int64_t grab_start = esp_timer_get_time();
        err = cam_client_wait_frame(&frame, frame_buf, FRAME_BUF_BYTES, 500);
        const int64_t grab_us = esp_timer_get_time() - grab_start;

        if (err != ESP_OK) {
            if (err == ESP_ERR_TIMEOUT) {
                timeouts_since_log++;
                consecutive_timeouts++;
            } else {
                consecutive_timeouts = 0;
                errors++;
                if (err == ESP_ERR_INVALID_SIZE) {
                    ESP_LOGW(TAG, "dropped a frame that does not fit the display buffer");
                }
            }

            if (!have_frame) {
                const int stations = app_wifi_ap_sta_count();
                if (stations != last_stations) {
                    last_stations = stations;
                    show_waiting(disp, stations);
                    ESP_LOGI(TAG, "%d station(s) on the softap, free heap %lu", stations,
                             (unsigned long)esp_get_free_heap_size());
                }
            } else if (!lost_shown && consecutive_timeouts >= 3) {
                lost_shown = true;
                show_cam_lost(disp);
            }
            continue;
        }

        have_frame = true;
        lost_shown = false;
        consecutive_timeouts = 0;

        const int64_t render_start = esp_timer_get_time();
        image_view_render(disp, frame.data, frame.width, frame.height);
        const int64_t render_end = esp_timer_get_time();
        err = ssd1306_flush(disp);
        const int64_t flush_end = esp_timer_get_time();
        if (err != ESP_OK) {
            errors++;
            ESP_LOGW(TAG, "display write failed: %s", esp_err_to_name(err));
        }

        frames++;
        frames_since_log++;
        grab_us_sum += grab_us;
        render_us_sum += render_end - render_start;
        flush_us_sum += flush_end - render_end;

        const int64_t now = esp_timer_get_time();
        if (now - last_log >= 5000000) {
            const float fps = frames_since_log * 1e6f / (float)(now - last_log);
            const float avg_grab = frames_since_log ? grab_us_sum / 1000.0f / frames_since_log : 0;
            const float avg_render = frames_since_log ? render_us_sum / 1000.0f / frames_since_log : 0;
            const float avg_flush = frames_since_log ? flush_us_sum / 1000.0f / frames_since_log : 0;
            ESP_LOGI(TAG, "%lu frames, %.1f fps, fetch %.0f ms, render %.0f ms, flush %.0f ms, "
                     "%dx%d %s, timeouts %lu, errors %lu, cam rssi %d, heap %lu", frames, fps,
                     avg_grab, avg_render, avg_flush, frame.width, frame.height,
                     frame.from_jpeg ? "jpeg" : "gray8", timeouts_since_log, errors,
                     app_wifi_ap_sta_rssi(), (unsigned long)esp_get_free_heap_size());
            last_log = now;
            frames_since_log = 0;
            grab_us_sum = 0;
            render_us_sum = 0;
            flush_us_sum = 0;
            timeouts_since_log = 0;
        }
    }
}
