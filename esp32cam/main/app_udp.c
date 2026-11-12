#include "app_udp.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "app_camera.h"
#include "camera_pins.h"
#include "esp_camera.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tjpgd_gray.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "app_udp";

#define UDP_MAX_PAYLOAD   1400
#define UDP_MAGIC         0x4341  // "CA"
#define UDP_TASK_STACK    6144
#define UDP_TASK_PRIORITY 5

#define PANEL_W 128
#define PANEL_H 64

// Wire format shared with the viewer (esp32_oled/main/cam_client.c).
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint16_t frame_id;
    uint16_t width;
    uint16_t height;
    uint8_t format;      // 0 = grey8, 1 = jpeg
    uint8_t frag_index;
    uint8_t frag_count;
    uint8_t reserved;
    uint32_t payload_len;
} cam_udp_header_t;

#define FEEDBACK_MAGIC 0x4342  // "CB", viewer -> camera
#define INTERVAL_MIN_MS 0
#define INTERVAL_MAX_MS 120

// Sent by the viewer every second so the camera can pace itself.
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint16_t fps_x10;
    uint16_t drops;
    uint16_t reserved;
} cam_feedback_t;

static volatile int s_interval_ms = CONFIG_APP_UDP_INTERVAL_MS;

static uint8_t *s_gray;  // JPEG decode target (8-bit luma, descaled)
static size_t s_gray_cap;
static uint8_t s_panel[PANEL_W * PANEL_H];

static bool s_flash_on;
static int64_t s_flash_changed_us;
static int s_last_avg;
static int s_aec_value;
static int s_agc_gain;
static sensor_t *s_sensor;

static void *psram_alloc(size_t bytes)
{
    void *p = heap_caps_malloc_prefer(bytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                      MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    return p;
}

// Centre-crops the source to the panel's 2:1 aspect ratio and writes one luma
// sample per panel pixel. Sampling straight out of the decoded bitstream avoids
// a full-size grey intermediate, and only 8192 panel pixels are touched.
static void sample_panel_from_gray(const uint8_t *gray, int width, int height)
{
    int crop_w = width;
    int crop_h = height;
    int x0 = 0;
    int y0 = 0;
    if (width * PANEL_H > height * PANEL_W) {
        crop_w = (height * PANEL_W + PANEL_H / 2) / PANEL_H;
        x0 = (width - crop_w) / 2;
    } else {
        crop_h = (width * PANEL_H + PANEL_W / 2) / PANEL_W;
        y0 = (height - crop_h) / 2;
    }

    for (int y = 0; y < PANEL_H; y++) {
        const int sy = y0 + (y * crop_h) / PANEL_H;
        const uint8_t *row = gray + (size_t)sy * width;
        uint8_t *dst = s_panel + (size_t)y * PANEL_W;
        for (int x = 0; x < PANEL_W; x++) {
            const int sx = x0 + (x * crop_w) / PANEL_W;
            dst[x] = row[sx];
        }
    }
}

// Auto flash: AEC keeps the panel average glued around mid-grey, so ambient
// brightness is taken from the sensor's exposure value instead (rises in dim
// light). Wide thresholds plus a dwell keep the light from ping-ponging.
static void update_flash(void)
{
    uint32_t sum = 0;
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        sum += s_panel[i];
    }
    s_last_avg = (int)(sum / (PANEL_W * PANEL_H));

    const int64_t now = esp_timer_get_time();
    const int64_t dwell_us = (int64_t)CONFIG_APP_FLASH_DWELL_MS * 1000;
    const bool dark = s_aec_value > CONFIG_APP_FLASH_DARK_AEC || s_last_avg < 20;
    const bool bright = s_aec_value < CONFIG_APP_FLASH_BRIGHT_AEC && s_last_avg > 60;
    if (!s_flash_on && dark && now - s_flash_changed_us > dwell_us) {
        gpio_set_level(CAM_PIN_FLASH_LED, 1);
        s_flash_on = true;
        s_flash_changed_us = now;
        ESP_LOGI(TAG, "flash on (aec %d, gain %d, avg %d)", s_aec_value, s_agc_gain,
                 s_last_avg);
    } else if (s_flash_on && bright && now - s_flash_changed_us > dwell_us) {
        gpio_set_level(CAM_PIN_FLASH_LED, 0);
        s_flash_on = false;
        s_flash_changed_us = now;
        ESP_LOGI(TAG, "flash off (aec %d, gain %d, avg %d)", s_aec_value, s_agc_gain,
                 s_last_avg);
    }
}

static bool jpeg_to_panel(const uint8_t *jpeg, size_t len)
{
    // 160x120 is the largest descaled output (QVGA at 1/2); QQVGA gives 80x60.
    const size_t need = 160 * 120;
    if (s_gray_cap < need) {
        uint8_t *grown = psram_alloc(need);
        if (grown == NULL) {
            return false;
        }
        heap_caps_free(s_gray);
        s_gray = grown;
        s_gray_cap = need;
    }

    int width = 0;
    int height = 0;
    if (tjpgd_gray_decode(jpeg, len, s_gray, s_gray_cap, 1, &width, &height) != ESP_OK) {
        return false;
    }
    sample_panel_from_gray(s_gray, width, height);
    return true;
}

// The sensor's YUV422 frame is YUYV: the luma sample is the first byte of
// every pixel pair. Sampling the panel positions straight out of that plane
// skips both the JPEG decode and a full-size grey intermediate.
static bool raw_to_panel(const camera_fb_t *fb)
{
    if (fb->format != PIXFORMAT_YUV422 || fb->width <= 0 || fb->height <= 0) {
        return false;
    }

    const int width = fb->width;
    const int height = fb->height;
    int crop_w = width;
    int crop_h = height;
    int x0 = 0;
    int y0 = 0;
    if (width * PANEL_H > height * PANEL_W) {
        crop_w = (height * PANEL_W + PANEL_H / 2) / PANEL_H;
        x0 = (width - crop_w) / 2;
    } else {
        crop_h = (width * PANEL_H + PANEL_W / 2) / PANEL_W;
        y0 = (height - crop_h) / 2;
    }

    const uint8_t *src = fb->buf;
    for (int y = 0; y < PANEL_H; y++) {
        const int sy = y0 + (y * crop_h) / PANEL_H;
        const uint8_t *row = src + (size_t)sy * width * 2;
        uint8_t *dst = s_panel + (size_t)y * PANEL_W;
        for (int x = 0; x < PANEL_W; x++) {
            const int sx = x0 + (x * crop_w) / PANEL_W;
            dst[x] = row[sx * 2];
        }
    }
    return true;
}

static void udp_task(void *arg)
{
    (void)arg;

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_APP_UDP_PORT),
    };
    dest.sin_addr.s_addr = inet_addr(CONFIG_APP_UDP_HOST);
    if (dest.sin_addr.s_addr == INADDR_NONE) {
        ESP_LOGE(TAG, "invalid destination \"%s\"", CONFIG_APP_UDP_HOST);
        vTaskDelete(NULL);
        return;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    // A frame leaves as six back-to-back datagrams; a larger send buffer keeps
    // pbuf exhaustion (ENOMEM) from eating frames.
    int sndbuf = 16 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    ESP_LOGI(TAG, "sending %dx%d grey frames to %s:%d", PANEL_W, PANEL_H, CONFIG_APP_UDP_HOST,
             CONFIG_APP_UDP_PORT);

    uint8_t packet[sizeof(cam_udp_header_t) + UDP_MAX_PAYLOAD];
    uint16_t frame_id = 0;
    uint32_t frames = 0;
    uint32_t errors = 0;
    uint32_t window_frames = 0;
    int64_t cap_us = 0;
    int64_t enc_us = 0;
    int64_t send_us = 0;
    uint32_t jpeg_min = UINT32_MAX;
    uint32_t jpeg_max = 0;
    uint32_t jpeg_sum = 0;
    int64_t window_start = esp_timer_get_time();

    while (true) {
        const int64_t t0 = esp_timer_get_time();
        camera_fb_t *fb = app_cam_fb_get();
        const int64_t t1 = esp_timer_get_time();
        if (fb == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (fb->format == PIXFORMAT_JPEG) {
            if (fb->len < jpeg_min) {
                jpeg_min = fb->len;
            }
            if (fb->len > jpeg_max) {
                jpeg_max = fb->len;
            }
            jpeg_sum += fb->len;
        }
        const bool ok = fb->format == PIXFORMAT_JPEG ? jpeg_to_panel(fb->buf, fb->len)
                                                     : raw_to_panel(fb);
        const int64_t t2 = esp_timer_get_time();
        if (!ok) {
            errors++;
            if (errors % 5 == 1) {
                ESP_LOGW(TAG, "frame decode failed (%u B, %s)", (unsigned)fb->len,
                         fb->format == PIXFORMAT_JPEG ? "jpeg" : "raw");
            }
            app_cam_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        app_cam_fb_return(fb);
        if (s_sensor != NULL && (frames % 10) == 0) {
            s_sensor->init_status(s_sensor);
            s_aec_value = s_sensor->status.aec_value;
            s_agc_gain = s_sensor->status.agc_gain;
        }
        update_flash();

        const size_t len = sizeof(s_panel);
        const int frags = (int)((len + UDP_MAX_PAYLOAD - 1) / UDP_MAX_PAYLOAD);
        for (int i = 0; i < frags; i++) {
            const size_t off = (size_t)i * UDP_MAX_PAYLOAD;
            size_t n = len - off;
            if (n > UDP_MAX_PAYLOAD) {
                n = UDP_MAX_PAYLOAD;
            }

            cam_udp_header_t header = {
                .magic = UDP_MAGIC,
                .frame_id = frame_id,
                .width = PANEL_W,
                .height = PANEL_H,
                .format = 0, /* grey8 */
                .frag_index = (uint8_t)i,
                .frag_count = (uint8_t)frags,
                .reserved = 0,
                .payload_len = (uint32_t)n,
            };
            memcpy(packet, &header, sizeof(header));
            memcpy(packet + sizeof(header), s_panel + off, n);

            int sent = -1;
            for (int attempt = 0; attempt < 3 && sent < 0; attempt++) {
                sent = sendto(sock, packet, sizeof(header) + n, 0, (struct sockaddr *)&dest,
                              sizeof(dest));
                if (sent < 0) {
                    // The pbuf pool can run dry for a moment during a burst.
                    vTaskDelay(pdMS_TO_TICKS(2));
                }
            }
            if (sent < 0) {
                errors++;
                if (errors % 20 == 1) {
                    ESP_LOGW(TAG, "sendto failed: errno %d", errno);
                }
                break;
            }
            if (i + 1 < frags) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
        const int64_t t3 = esp_timer_get_time();
        frame_id++;

        cap_us += t1 - t0;
        enc_us += t2 - t1;
        send_us += t3 - t2;
        frames++;
        window_frames++;
        const int64_t now = esp_timer_get_time();
        if (now - window_start >= 5000000) {
            wifi_ap_record_t ap = { 0 };
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                ESP_LOGI(TAG, "%u frames, %.1f fps, %u errors, interval %d ms, "
                         "capture %.0f ms, encode %.0f ms, send %.0f ms, "
                         "jpeg %u/%u/%u B, flash %d aec %d gain %d avg %d, "
                         "rssi %d dBm, ch %d, %s",
                         frames,
                         window_frames * 1e6f / (now - window_start), errors, s_interval_ms,
                         cap_us / 1000.0f / window_frames, enc_us / 1000.0f / window_frames,
                         send_us / 1000.0f / window_frames,
                         window_frames ? jpeg_min : 0,
                         window_frames ? jpeg_sum / window_frames : 0, jpeg_max,
                         s_flash_on, s_aec_value, s_agc_gain, s_last_avg, ap.rssi, ap.primary,
                         ap.second == WIFI_SECOND_CHAN_NONE ? "20MHz" : "40MHz");
            } else {
                ESP_LOGI(TAG, "%u frames, %.1f fps, %u errors, interval %d ms, "
                         "capture %.0f ms, encode %.0f ms, send %.0f ms", frames,
                         window_frames * 1e6f / (now - window_start), errors, s_interval_ms,
                         cap_us / 1000.0f / window_frames, enc_us / 1000.0f / window_frames,
                         send_us / 1000.0f / window_frames);
            }
            window_frames = 0;
            cap_us = 0;
            enc_us = 0;
            send_us = 0;
            jpeg_min = UINT32_MAX;
            jpeg_max = 0;
            jpeg_sum = 0;
            window_start = now;
        }

        if (s_interval_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(s_interval_ms));
        }
    }
}

// Listens for the viewer's rate report and moves the send interval so the
// viewer stays in its 14-18 fps window without building a lag queue.
static void feedback_task(void *arg)
{
    (void)arg;

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "feedback socket failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_APP_UDP_PORT + 1),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "feedback bind failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "listening for viewer feedback on udp/%d", CONFIG_APP_UDP_PORT + 1);

    while (true) {
        cam_feedback_t feedback;
        const int n = recvfrom(sock, &feedback, sizeof(feedback), 0, NULL, NULL);
        if (n != (int)sizeof(feedback) || feedback.magic != FEEDBACK_MAGIC) {
            continue;
        }

        int interval = s_interval_ms;
        if (feedback.drops > 2) {
            interval += 5;
        } else if (feedback.fps_x10 < 180) {
            interval -= 5;
        } else if (feedback.fps_x10 > 210) {
            interval += 5;
        }
        if (interval < INTERVAL_MIN_MS) {
            interval = INTERVAL_MIN_MS;
        } else if (interval > INTERVAL_MAX_MS) {
            interval = INTERVAL_MAX_MS;
        }
        if (interval != s_interval_ms) {
            ESP_LOGI(TAG, "viewer %u.%u fps, %u drops -> interval %d ms",
                     feedback.fps_x10 / 10, feedback.fps_x10 % 10, feedback.drops, interval);
        }
        s_interval_ms = interval;
    }
}

esp_err_t app_udp_start(void)
{
#if defined(CONFIG_APP_UDP_ENABLE)
    const gpio_config_t flash_cfg = {
        .pin_bit_mask = 1ULL << CAM_PIN_FLASH_LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&flash_cfg);
    gpio_set_level(CAM_PIN_FLASH_LED, 0);
    s_flash_changed_us = esp_timer_get_time();
    s_sensor = esp_camera_sensor_get();

#if defined(CONFIG_APP_UDP_SOURCE_RAW)
    // Raw YUV422 avoids the JPEG decoder on the camera entirely.
    esp_err_t mode_err = app_cam_set_mode(APP_CAM_MODE_RAW);
    if (mode_err == ESP_OK || mode_err == ESP_ERR_NOT_FINISHED) {
        ESP_LOGI(TAG, "camera in YUV422 raw mode, luma sampled directly");
    } else {
        ESP_LOGW(TAG, "raw mode request failed: %s (sending whatever the camera has)",
                 esp_err_to_name(mode_err));
    }
#else
    ESP_LOGI(TAG, "camera in JPEG mode, decoded on the camera");
#endif
    if (xTaskCreate(udp_task, "cam_udp", UDP_TASK_STACK, NULL, UDP_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the udp sender");
        return ESP_ERR_NO_MEM;
    }
#if defined(CONFIG_APP_UDP_ADAPT)
    if (xTaskCreate(feedback_task, "cam_fb", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the feedback listener");
        return ESP_ERR_NO_MEM;
    }
#else
    ESP_LOGI(TAG, "adaptive pacing disabled, fixed interval %d ms", s_interval_ms);
#endif
    return ESP_OK;
#else
    ESP_LOGI(TAG, "udp output disabled");
    return ESP_OK;
#endif
}
