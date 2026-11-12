#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    const uint8_t *data;  // always 8-bit grey
    size_t len;
    uint16_t width;
    uint16_t height;
    bool from_jpeg;       // true when a JPEG frame had to be decoded locally
} cam_frame_t;

// Starts the UDP receiver: the camera sends panel-sized grey frames to
// CONFIG_APP_UDP_PORT, they are reassembled here and handed to the display.
esp_err_t cam_client_start(void);

// Blocks until a new frame is available (at most timeout_ms) and copies it into
// dst. The receiver is only released after the copy, so the frame buffers
// cannot be overwritten while they are in use.
esp_err_t cam_client_wait_frame(cam_frame_t *frame, uint8_t *dst, size_t dst_cap,
                                uint32_t timeout_ms);
