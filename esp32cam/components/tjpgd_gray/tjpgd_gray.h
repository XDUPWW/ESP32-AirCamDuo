#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Decodes a baseline JPEG to 8-bit grayscale (Y only).
 *
 * scale: 0 = 1/1, 1 = 1/2, 2 = 1/4, 3 = 1/8 (tjpgd's descaling).
 * The output is written row by row into out; width/height report the decoded
 * (already descaled) size. Returns ESP_ERR_INVALID_SIZE when out is too small.
 */
esp_err_t tjpgd_gray_decode(const uint8_t *jpeg, size_t len, uint8_t *out, size_t out_cap,
                            int scale, int *width, int *height);
