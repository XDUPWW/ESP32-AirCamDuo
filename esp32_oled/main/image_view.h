#pragma once

#include <stdint.h>

#include "ssd1306.h"

// Draws an 8-bit grey frame over the whole panel: the frame is cropped to the
// panel's aspect ratio and reduced to one bit per pixel.
void image_view_render(ssd1306_t *disp, const uint8_t *gray, int width, int height);
