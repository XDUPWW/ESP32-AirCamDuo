#pragma once

#include <stdint.h>

#include "esp_err.h"

// Opens a SoftAP the camera is expected to join. The AP always runs in the
// 2.4 GHz band so an ESP32-CAM can associate with it.
esp_err_t app_wifi_ap_start(const char *ssid, const char *password);

// IPv4 address handed out to the AP interface ("192.168.4.1" on a stock config).
const char *app_wifi_ap_ip(void);

// Number of stations currently associated; used to know when to look for the
// camera instead of polling an empty network.
int app_wifi_ap_sta_count(void);

// RSSI of the first associated station in dBm, or -127 when nobody is joined.
int app_wifi_ap_sta_rssi(void);
