#include "app_camera.h"

#include <stdio.h>

#include "camera_pins.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_cam";

#define CAM_XCLK_HZ 20000000
#define CAM_JPEG_QUALITY 12
// The driver's AUTO size is width*height/5 = 3.8 kB at QQVGA, which a busy
// scene overflows: frames get truncated and the viewer goes silent. 32 kB
// leaves room for the largest QQVGA JPEG.
#define CAM_JPEG_BUFFER_SIZE (32 * 1024)
#if defined(CONFIG_APP_JPEG_QQVGA)
// Smallest JPEG the sensor can produce: a fraction of the QVGA bytes, which is
// what limits the frame rate over Wi-Fi.
#define CAM_JPEG_FRAMESIZE FRAMESIZE_QQVGA
#define CAM_JPEG_WIDTH 160
#define CAM_JPEG_HEIGHT 120
#else
#define CAM_JPEG_FRAMESIZE FRAMESIZE_QVGA
#define CAM_JPEG_WIDTH 320
#define CAM_JPEG_HEIGHT 240
#endif
#define CAM_RAW_FRAMESIZE FRAMESIZE_QQVGA
#define CAM_RAW_WIDTH 160
#define CAM_RAW_HEIGHT 120

static bool s_available;
static app_cam_mode_t s_mode = APP_CAM_MODE_JPEG;
static char s_sensor_name[24] = "not detected";

static SemaphoreHandle_t s_lock;
static int s_stream_users;
static bool s_switching;

static camera_config_t s_cfg = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,
    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,
    .xclk_freq_hz = CAM_XCLK_HZ,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = CAM_JPEG_FRAMESIZE,
    .jpeg_quality = CAM_JPEG_QUALITY,
    .jpeg_buffer_size = CAM_JPEG_BUFFER_SIZE,
    .fb_count = 3,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_LATEST,
};

static void apply_mode(app_cam_mode_t mode)
{
    if (mode == APP_CAM_MODE_RAW) {
        // Uncompressed frames travel as 8-bit grey, so the geometry is kept small
        // enough to stay smooth over Wi-Fi.
        s_cfg.pixel_format = PIXFORMAT_YUV422;
        s_cfg.frame_size = CAM_RAW_FRAMESIZE;
        s_cfg.fb_count = 2;
    } else {
        s_cfg.pixel_format = PIXFORMAT_JPEG;
        s_cfg.frame_size = CAM_JPEG_FRAMESIZE;
        s_cfg.jpeg_quality = CAM_JPEG_QUALITY;
        s_cfg.jpeg_buffer_size = CAM_JPEG_BUFFER_SIZE;
        s_cfg.fb_count = 3;
    }
}

static esp_err_t start_camera(app_cam_mode_t mode)
{
    apply_mode(mode);

    esp_err_t err = esp_camera_init(&s_cfg);
    for (int attempt = 0; err != ESP_OK && attempt < 2; attempt++) {
        ESP_LOGW(TAG, "init failed (%s), retrying", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(100));
        err = esp_camera_init(&s_cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init(mode=%s) failed: %s", mode == APP_CAM_MODE_RAW ? "raw" : "jpeg",
                 esp_err_to_name(err));
        s_available = false;
        return err;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        snprintf(s_sensor_name, sizeof(s_sensor_name), "PID=0x%02x VER=%d", sensor->id.PID,
                 sensor->id.VER);
        // The OV2640 comes up with auto exposure/gain disabled on some modules,
        // which leaves the image fixed and makes ambient sensing impossible.
        sensor->set_exposure_ctrl(sensor, 1);
        sensor->set_gain_ctrl(sensor, 1);
        sensor->set_awb_gain(sensor, 1);
        ESP_LOGI(TAG, "sensor: %s, aec/agc/awb gain enabled", s_sensor_name);
    }

    s_mode = mode;
    s_available = true;
    return ESP_OK;
}

// The sensor needs a continuous MCLK. Generating it here, once, keeps XCLK alive
// across camera format switches: the driver cannot reliably re-acquire the LEDC
// pad after tearing the clock down, and a sensor without XCLK is invisible on SCCB.
static esp_err_t xclk_start(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = CAM_XCLK_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "xclk timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    const ledc_channel_config_t channel = {
        .gpio_num = CAM_PIN_XCLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_1,
        .duty = 1,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "xclk channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "xclk %d Hz on GPIO%d", CAM_XCLK_HZ, CAM_PIN_XCLK);
    return ESP_OK;
}

esp_err_t app_cam_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = xclk_start();
    if (err != ESP_OK) {
        return err;
    }
    return start_camera(APP_CAM_MODE_JPEG);
}

bool app_cam_available(void)
{
    return s_available;
}

app_cam_mode_t app_cam_mode(void)
{
    return s_mode;
}

const char *app_cam_mode_name(void)
{
    return s_mode == APP_CAM_MODE_RAW ? "yuyv422" : "jpeg";
}

bool app_cam_begin_stream(void)
{
    bool ok = false;
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (!s_switching) {
            s_stream_users++;
            ok = true;
        }
        xSemaphoreGive(s_lock);
    }
    return ok;
}

void app_cam_end_stream(void)
{
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        if (s_stream_users > 0) {
            s_stream_users--;
        }
        xSemaphoreGive(s_lock);
    }
}

int app_cam_stream_users(void)
{
    int users = 0;
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        users = s_stream_users;
        xSemaphoreGive(s_lock);
    }
    return users;
}

esp_err_t app_cam_set_mode(app_cam_mode_t mode)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_stream_users > 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_switching = true;
    xSemaphoreGive(s_lock);

    esp_err_t err = ESP_OK;
    if (s_available && s_mode == mode) {
        err = ESP_ERR_NOT_FINISHED;  // already in this mode, nothing to do
    } else {
        const app_cam_mode_t previous = s_mode;
        esp_camera_deinit();
        err = start_camera(mode);
        if (err != ESP_OK) {
            // Never leave the board without a working sensor.
            ESP_LOGW(TAG, "switch to %s failed, restoring %s",
                     mode == APP_CAM_MODE_RAW ? "raw" : "jpeg",
                     previous == APP_CAM_MODE_RAW ? "raw" : "jpeg");
            err = start_camera(previous);
            err = err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_switching = false;
    xSemaphoreGive(s_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "camera mode -> %s", app_cam_mode_name());
    }
    return err;
}

camera_fb_t *app_cam_fb_get(void)
{
    return esp_camera_fb_get();
}

void app_cam_fb_return(camera_fb_t *fb)
{
    if (fb != NULL) {
        esp_camera_fb_return(fb);
    }
}

const char *app_cam_sensor_name(void)
{
    return s_sensor_name;
}

int app_cam_frame_width(void)
{
    return s_mode == APP_CAM_MODE_RAW ? CAM_RAW_WIDTH : CAM_JPEG_WIDTH;
}

int app_cam_frame_height(void)
{
    return s_mode == APP_CAM_MODE_RAW ? CAM_RAW_HEIGHT : CAM_JPEG_HEIGHT;
}

const char *app_cam_frame_format(void)
{
    return s_mode == APP_CAM_MODE_RAW ? "gray8" : "jpeg";
}

size_t app_cam_luma_from_fb(const camera_fb_t *fb, uint8_t *dst, size_t dst_size)
{
    if (fb == NULL || dst == NULL || fb->format != PIXFORMAT_YUV422) {
        return 0;
    }

    const size_t pixels = (size_t)fb->width * fb->height;
    if (dst_size < pixels) {
        return 0;
    }

    // YUYV: the luma sample is the first byte of every pixel pair.
    const uint8_t *src = fb->buf;
    for (size_t i = 0; i < pixels; i++) {
        dst[i] = src[i * 2];
    }
    return pixels;
}
