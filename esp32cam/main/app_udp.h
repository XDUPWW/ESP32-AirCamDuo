#pragma once

#include "esp_err.h"

// Starts the UDP frame sender: splits camera frames into datagrams and sends
// them to CONFIG_APP_UDP_HOST:CONFIG_APP_UDP_PORT. Returns ESP_OK when the task
// is running (or when UDP output is disabled).
esp_err_t app_udp_start(void);
