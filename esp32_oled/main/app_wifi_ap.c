#include "app_wifi_ap.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "app_ap";

#define AP_CHANNEL      CONFIG_APP_AP_CHANNEL
#define AP_MAX_STATIONS 4

static volatile int s_sta_count;
static char s_ip[16] = "192.168.4.1";

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base != WIFI_EVENT) {
        return;
    }

    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)data;
        s_sta_count++;
        ESP_LOGI(TAG, "station " MACSTR " joined, %d client(s)", MAC2STR(event->mac), s_sta_count);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)data;
        if (s_sta_count > 0) {
            s_sta_count--;
        }
        ESP_LOGI(TAG, "station " MACSTR " left (reason %d), %d client(s)",
                 MAC2STR(event->mac), event->reason, s_sta_count);
    }
}

esp_err_t app_wifi_ap_start(const char *ssid, const char *password)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Channel 6 keeps the access point in the 2.4 GHz band, which is all a
    // classic ESP32-CAM can see.
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(ssid);
    cfg.ap.channel = AP_CHANNEL;
    cfg.ap.max_connection = AP_MAX_STATIONS;
#if defined(CONFIG_APP_AP_HIDDEN)
    cfg.ap.ssid_hidden = 1;
#else
    cfg.ap.ssid_hidden = 0;
#endif

    if (password != NULL && strlen(password) >= 8) {
        strlcpy((char *)cfg.ap.password, password, sizeof(cfg.ap.password));
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
        ESP_LOGW(TAG, "password shorter than 8 characters, starting an open network");
    }

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "starting softap failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_ip_info_t ip = { 0 };
    if (esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
    }

    // HT40 doubles the airtime of every frame on 2.4 GHz, where the band is
    // usually crowded; HT20 is more reliable for a steady video stream. The C5
    // defaults to the 2.4+5 GHz band mode, where the per-band API is required.
    wifi_bandwidths_t bandwidths = {
        .ghz_2g = WIFI_BW20,
        .ghz_5g = WIFI_BW20,
    };
    err = esp_wifi_set_bandwidths(WIFI_IF_AP, &bandwidths);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HT20 request failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "softap \"%s\" up at %s, channel %d", ssid, s_ip, AP_CHANNEL);
    return ESP_OK;
}

const char *app_wifi_ap_ip(void)
{
    return s_ip;
}

int app_wifi_ap_sta_count(void)
{
    return s_sta_count;
}

int app_wifi_ap_sta_rssi(void)
{
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK || list.num <= 0) {
        return -127;
    }
    return list.sta[0].rssi;
}
