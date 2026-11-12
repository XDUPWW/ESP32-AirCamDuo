#include "image_view.h"

#include <string.h>

#include "sdkconfig.h"

// Floyd-Steinberg error rows. Two rows are enough: the current one receives the
// right-hand and lower-left neighbours' error, the next one the rest.
static int16_t s_err_row[SSD1306_WIDTH + 2];
static int16_t s_err_next[SSD1306_WIDTH + 2];

void image_view_render(ssd1306_t *disp, const uint8_t *gray, int width, int height)
{
    if (gray == NULL || width <= 0 || height <= 0) {
        return;
    }

    const int dw = SSD1306_WIDTH;
    const int dh = disp->height;

    // Fill the panel without distorting the picture: scale the shorter side to
    // fit and cut the overhang in the middle.
    int x0 = 0;
    int y0 = 0;
    int crop_w = width;
    int crop_h = height;
    if (width * dh > height * dw) {
        crop_w = (height * dw + dh / 2) / dh;
        x0 = (width - crop_w) / 2;
    } else {
        crop_h = (width * dh + dw / 2) / dw;
        y0 = (height - crop_h) / 2;
    }

    uint8_t line[SSD1306_WIDTH];
    memset(s_err_row, 0, sizeof(s_err_row));

    for (int y = 0; y < dh; y++) {
        const int sy = y0 + (int)((int64_t)y * crop_h / dh);
        const uint8_t *src = gray + (size_t)sy * width;

        for (int x = 0; x < dw; x++) {
            const int sx = x0 + (int)((int64_t)x * crop_w / dw);
            int value = (src[sx] * CONFIG_APP_VIEW_GAIN) / 100;
            line[x] = (uint8_t)(value > 255 ? 255 : value);
        }

        memset(s_err_next, 0, sizeof(s_err_next));

        for (int x = 0; x < dw; x++) {
            int value = line[x];
#if defined(CONFIG_APP_VIEW_DITHER)
            value += s_err_row[x] / 16;
            if (value < 0) {
                value = 0;
            } else if (value > 255) {
                value = 255;
            }
#endif
            const bool on = value >= 128;

#if defined(CONFIG_APP_VIEW_DITHER)
            const int err = value - (on ? 255 : 0);
            s_err_row[x + 1] += err * 7 / 16;
            if (x > 0) {
                s_err_next[x - 1] += err * 3 / 16;
            }
            s_err_next[x] += err * 5 / 16;
            s_err_next[x + 1] += err * 1 / 16;
#endif

            ssd1306_pixel(disp, x, y, on);
        }

#if defined(CONFIG_APP_VIEW_DITHER)
        memcpy(s_err_row, s_err_next, sizeof(s_err_row));
#endif
    }
}
