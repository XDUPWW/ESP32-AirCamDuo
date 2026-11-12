#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "sdkconfig.h"

// The viewer board's BOOT/IO0 button. GPIO0 is also a strapping pin: holding
// it during reset enters download mode.
#define APP_BUTTON_GPIO CONFIG_APP_BUTTON_GPIO

esp_err_t app_button_start(void);

// Consumes a short press (released before 500 ms).
bool app_button_take_short(void);

// Consumes a long press (held for 500 ms, fires once while still held).
bool app_button_take_long(void);
