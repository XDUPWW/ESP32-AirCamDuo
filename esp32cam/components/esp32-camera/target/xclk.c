#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "xclk.h"
#include "esp_camera.h"

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#else
#include "esp_log.h"
static const char* TAG = "camera_xclk";
#endif

#define NO_CAMERA_LEDC_CHANNEL 0xFF
static ledc_channel_t g_ledc_channel = NO_CAMERA_LEDC_CHANNEL;
static int g_xclk_pin = -1;

esp_err_t xclk_timer_conf(int ledc_timer, int xclk_freq_hz)
{
    ledc_timer_config_t timer_conf;
    timer_conf.duty_resolution = LEDC_TIMER_1_BIT;
    timer_conf.freq_hz = xclk_freq_hz;
    timer_conf.speed_mode = LEDC_LOW_SPEED_MODE;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)   
    timer_conf.deconfigure = false;
#endif
   
#if ESP_IDF_VERSION_MAJOR >= 4
    timer_conf.clk_cfg = LEDC_AUTO_CLK;
#endif
    timer_conf.timer_num = (ledc_timer_t)ledc_timer;
    esp_err_t err = ledc_timer_config(&timer_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed for freq %d, rc=%x", xclk_freq_hz, err);
    }
    return err;
}

esp_err_t camera_enable_out_clock(const camera_config_t* config)
{
    // XCLK is owned by the application (see app_camera.c) and runs from boot to
    // reboot: the LEDC pad reservation cannot be released reliably once the
    // driver tears the clock down, and a sensor without XCLK never answers SCCB.
    (void)config;
    return ESP_OK;
}

void camera_disable_out_clock()
{
    // Paired with camera_enable_out_clock(): the clock keeps running, so nothing
    // to stop here.
}
