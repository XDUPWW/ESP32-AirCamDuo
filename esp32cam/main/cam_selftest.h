#pragma once

// Drives XCLK and scans the SCCB bus. Used when esp_camera_init() cannot find a
// sensor, to tell a wiring problem from a driver problem.
void app_cam_selftest(void);
