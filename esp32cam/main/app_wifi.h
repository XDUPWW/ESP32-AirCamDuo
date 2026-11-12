#pragma once

#include "esp_err.h"

// Joins the configured network; keeps retrying in the background when the
// network is not up yet. Falls back to running a SoftAP (next to the station
// interface) so the web UI stays reachable. Without a configured SSID the
// camera only runs its own SoftAP.
esp_err_t app_wifi_start(void);

// "http://<ip>/" of whichever interface ended up serving the web UI.
const char *app_wifi_url(void);
