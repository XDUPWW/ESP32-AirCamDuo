#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// The AI-Thinker board does not route the microSD card-detect switch to a GPIO,
// so presence is inferred from the bus: mount attempts find an inserted card,
// and a periodic CMD13 on the mounted card finds a removal.
typedef struct {
    bool present;      /* mounted and answering */
    uint32_t mounts;   /* successful mounts since boot */
    uint32_t capacity_mb;
    uint32_t free_mb;
} app_sd_state_t;

esp_err_t app_sd_start(void);

void app_sd_get(app_sd_state_t *out);
