#pragma once

#include "esp_err.h"

// Starts the HTTP server: streaming, snapshots and status.
esp_err_t app_http_start(void);
