#include "app_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "sdkconfig.h"

static const char *TAG = "app_wifi";

#define WIFI_GOT_IP_BIT BIT0
#define STA_CONNECT_TIMEOUT_MS 20000

static EventGroupHandle_t s_events;
static TimerHandle_t s_reconnect_timer;
static char s_url[40] = "http://192.168.4.1/";
static bool s_wifi_started;
static unsigned s_retries;

static void reconnect_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)data;
        // The viewer is the only network the camera is supposed to be on, so it
        // keeps trying: a viewer reset must not leave the camera stranded.
        // Reconnecting from a timer, not from the event handler, also covers the
        // case where the driver is still busy when the event arrives.
        s_retries++;
        if (s_retries % 5 == 1) {
            ESP_LOGW(TAG, "sta disconnected (reason %d), reconnecting (attempt %u)",
                     event->reason, s_retries);
        }
        if (s_reconnect_timer != NULL) {
            xTimerStart(s_reconnect_timer, 0);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "joined \"%s\", ip " IPSTR, CONFIG_APP_WIFI_STA_SSID,
                 IP2STR(&event->ip_info.ip));
        snprintf(s_url, sizeof(s_url), "http://" IPSTR "/", IP2STR(&event->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_events, WIFI_GOT_IP_BIT);
    }
}

static esp_err_t wifi_stack_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_reconnect_timer = xTimerCreate("wifi-retry", pdMS_TO_TICKS(1000), pdFALSE, NULL,
                                     reconnect_timer_cb);
    if (s_reconnect_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }
    return esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
}

static esp_err_t start_sta(void)
{
    esp_netif_create_default_wifi_sta();

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, CONFIG_APP_WIFI_STA_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, CONFIG_APP_WIFI_STA_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "starting station mode failed: %s", esp_err_to_name(err));
        return err;
    }
    s_wifi_started = true;

    // A sleeping station only listens at beacon intervals, which turns every
    // HTTP request into a 100-300 ms wait. The camera is mains powered, so
    // latency wins over power here.
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "disabling wifi power save failed: %s", esp_err_to_name(ps_err));
    }

    ESP_LOGI(TAG, "joining \"%s\"...", CONFIG_APP_WIFI_STA_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_GOT_IP_BIT, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));
    if (bits & WIFI_GOT_IP_BIT) {
        return ESP_OK;
    }

    // The disconnect handler keeps reconnecting; the caller may add a SoftAP
    // next to the station interface so the board stays reachable meanwhile.
    ESP_LOGW(TAG, "no ip after %d ms, still trying \"%s\" in the background",
             STA_CONNECT_TIMEOUT_MS, CONFIG_APP_WIFI_STA_SSID);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t start_ap(bool keep_sta)
{
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_config_t cfg = { 0 };
    const char *ssid = CONFIG_APP_WIFI_AP_SSID;
    const char *password = CONFIG_APP_WIFI_AP_PASSWORD;

    strlcpy((char *)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(ssid);
    cfg.ap.channel = 6;
    cfg.ap.max_connection = 4;
    cfg.ap.ssid_hidden = 0;

    if (strlen(password) >= 8) {
        strlcpy((char *)cfg.ap.password, password, sizeof(cfg.ap.password));
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
        ESP_LOGW(TAG, "password shorter than 8 characters, starting an open network");
    }

    esp_err_t err = esp_wifi_set_mode(keep_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    }
    if (err == ESP_OK && !s_wifi_started) {
        err = esp_wifi_start();
        s_wifi_started = err == ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "starting softap failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_ip_info_t ip = { 0 };
    if (esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK) {
        snprintf(s_url, sizeof(s_url), "http://" IPSTR "/", IP2STR(&ip.ip));
        ESP_LOGI(TAG, "softap \"%s\" up, %s%s", ssid, s_url,
                 keep_sta ? " (station retries continue)" : "");
    }
    return ESP_OK;
}

esp_err_t app_wifi_start(void)
{
    esp_err_t err = wifi_stack_init();
    if (err != ESP_OK) {
        return err;
    }

    bool want_sta = strlen(CONFIG_APP_WIFI_STA_SSID) > 0;
#if defined(CONFIG_APP_WIFI_MODE_AP)
    want_sta = false;
#endif

    if (!want_sta) {
        return start_ap(false);
    }

    err = start_sta();
    if (err == ESP_OK) {
        return ESP_OK;
    }

    // Keep the station interface and add the SoftAP on top, so the web UI is
    // reachable while the configured network is still out of range.
    return start_ap(true);
}

const char *app_wifi_url(void)
{
    return s_url;
}
