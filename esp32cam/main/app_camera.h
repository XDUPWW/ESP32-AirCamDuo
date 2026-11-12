#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_camera.h"

// The device does no image processing beyond copying the luma plane out of the
// sensor's YUV422 frames; everything else is left to the client.
typedef enum {
    APP_CAM_MODE_JPEG,  // sensor JPEG engine, used for the MJPEG stream
    APP_CAM_MODE_RAW,   // sensor YUV422 at QQVGA, luma is sent as 8-bit grey
} app_cam_mode_t;

// Probes the sensor and initialises it for JPEG capture; the framesize is a
// Kconfig choice.
esp_err_t app_cam_init(void);

// False when the last init attempt did not find a sensor.
bool app_cam_available(void);

// Switching format means re-initialising the driver, so it is refused while a
// stream is running.
esp_err_t app_cam_set_mode(app_cam_mode_t mode);
app_cam_mode_t app_cam_mode(void);
const char *app_cam_mode_name(void);

// Geometry of the active mode. Raw mode runs at QQVGA: uncompressed frames have
// to fit the Wi-Fi link budget, JPEG does not.
int app_cam_frame_width(void);
int app_cam_frame_height(void);
const char *app_cam_frame_format(void);

// Stream bookkeeping: a stream holds a framebuffer open, so mode switches and
// stream starts are mutually exclusive.
bool app_cam_begin_stream(void);
void app_cam_end_stream(void);
int app_cam_stream_users(void);

camera_fb_t *app_cam_fb_get(void);
void app_cam_fb_return(camera_fb_t *fb);

// Copies the luma plane of a YUV422 frame into dst (one byte per pixel).
// Returns the number of bytes written, or 0 on a format mismatch.
size_t app_cam_luma_from_fb(const camera_fb_t *fb, uint8_t *dst, size_t dst_size);

const char *app_cam_sensor_name(void);
