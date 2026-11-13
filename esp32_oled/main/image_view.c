#include "image_view.h"

#include <string.h>

#include "sdkconfig.h"

// Floyd-Steinberg error rows. Two rows are enough: the current one receives the
// right-hand and lower-left neighbours' error, the next one the rest.
static int16_t s_err_row[SSD1306_WIDTH + 2];
static int16_t s_err_next[SSD1306_WIDTH + 2];

#if defined(CONFIG_APP_VIEW_AGC)
// A flat wall would be stretched into noise, so the auto contrast gives up when
// the 2%-98% window is narrower than this.
#define AGC_MIN_RANGE 24
#define AGC_FLOOR     6
#define AGC_CEILING   249
// Ceiling on the stretch itself: a very dark scene would otherwise be amplified
// until the dither is mostly sensor noise. The auto flash covers real darkness.
#define AGC_MAX_GAIN_X10 30
// One quarter of the way to the new window per frame: fast enough for a scene
// change, slow enough that the dither does not visibly pump.
#define AGC_SLEW      4

static uint16_t s_hist[256];
static uint8_t s_lut[256];
static bool s_lut_valid;
static int s_lo = -1;
static int s_hi = -1;
static int s_gain_x10 = 100;

// Percentile stretch instead of a fixed gain. The 2% and 98% points of the
// histogram become the black and white levels, so a dim scene is lifted and a
// bright one is pulled down until the one-bit dither has detail at both ends.
static void agc_update(const uint8_t *gray, size_t pixels)
{
    memset(s_hist, 0, sizeof(s_hist));
    for (size_t i = 0; i < pixels; i++) {
        s_hist[gray[i]]++;
    }

    const uint32_t low_at = (uint32_t)(pixels / 50);
    const uint32_t high_at = (uint32_t)(pixels - pixels / 50);

    int lo = 0;
    int hi = 255;
    uint32_t acc = 0;
    for (int v = 0; v < 256; v++) {
        acc += s_hist[v];
        if (acc >= low_at) {
            lo = v;
            break;
        }
    }
    acc = 0;
    for (int v = 0; v < 256; v++) {
        acc += s_hist[v];
        if (acc >= high_at) {
            hi = v;
            break;
        }
    }
    if (hi - lo < AGC_MIN_RANGE) {
        return;  // nothing to stretch; keep the last mapping
    }

    if (s_lo < 0) {
        s_lo = lo;
        s_hi = hi;
    } else {
        s_lo += (lo - s_lo) / AGC_SLEW;
        s_hi += (hi - s_hi) / AGC_SLEW;
    }

    // Hold the stretch under the ceiling by widening the window around its
    // middle; a scene flatter than that keeps its own contrast (the flash will
    // deal with it) instead of turning into amplified noise.
    const int range = AGC_CEILING - AGC_FLOOR;
    const int min_span = range * 10 / AGC_MAX_GAIN_X10;
    if (s_hi - s_lo < min_span) {
        const int mid = (s_lo + s_hi) / 2;
        s_lo = mid - min_span / 2;
        s_hi = s_lo + min_span;
        if (s_lo < 0) {
            s_lo = 0;
            s_hi = min_span;
        } else if (s_hi > 255) {
            s_hi = 255;
            s_lo = 255 - min_span;
        }
    }

    const int span = s_hi - s_lo;
    s_gain_x10 = (range * 10 + span / 2) / span;
    for (int v = 0; v < 256; v++) {
        const int out = AGC_FLOOR + (v - s_lo) * range / span;
        s_lut[v] = out < 0 ? 0 : (out > 255 ? 255 : (uint8_t)out);
    }
    s_lut_valid = true;
}
#endif

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

#if defined(CONFIG_APP_VIEW_AGC)
    agc_update(gray, (size_t)width * height);
#endif

    uint8_t line[SSD1306_WIDTH];
    memset(s_err_row, 0, sizeof(s_err_row));

    for (int y = 0; y < dh; y++) {
        const int sy = y0 + (int)((int64_t)y * crop_h / dh);
        const uint8_t *src = gray + (size_t)sy * width;

        for (int x = 0; x < dw; x++) {
            const int sx = x0 + (int)((int64_t)x * crop_w / dw);
#if defined(CONFIG_APP_VIEW_AGC)
            line[x] = s_lut_valid ? s_lut[src[sx]] : src[sx];
#else
            const int value = (src[sx] * CONFIG_APP_VIEW_GAIN) / 100;
            line[x] = (uint8_t)(value > 255 ? 255 : value);
#endif
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

bool image_view_agc_state(int *lo, int *hi, int *gain_x10)
{
#if defined(CONFIG_APP_VIEW_AGC)
    if (!s_lut_valid) {
        return false;
    }
    if (lo != NULL) {
        *lo = s_lo;
    }
    if (hi != NULL) {
        *hi = s_hi;
    }
    if (gain_x10 != NULL) {
        *gain_x10 = s_gain_x10;
    }
    return true;
#else
    (void)lo;
    (void)hi;
    (void)gain_x10;
    return false;
#endif
}
