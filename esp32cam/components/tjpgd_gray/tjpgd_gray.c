#include "tjpgd_gray.h"

#include <string.h>

#include "tjpgd.h"

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint8_t *out;
    int out_w;
    int out_h;
} gray_ctx_t;

static size_t in_cb(JDEC *jd, uint8_t *buff, size_t nbyte)
{
    gray_ctx_t *ctx = (gray_ctx_t *)jd->device;
    size_t remain = ctx->len - ctx->pos;
    if (nbyte > remain) {
        nbyte = remain;
    }
    if (buff != NULL) {
        memcpy(buff, ctx->data + ctx->pos, nbyte);
    }
    ctx->pos += nbyte;
    return nbyte;
}

static int out_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    gray_ctx_t *ctx = (gray_ctx_t *)jd->device;
    const int row = rect->right - rect->left + 1;
    const uint8_t *src = (const uint8_t *)bitmap;

    for (int y = rect->top; y <= rect->bottom; y++) {
        memcpy(ctx->out + (size_t)y * ctx->out_w + rect->left, src, (size_t)row);
        src += row;
    }
    (void)jd;
    return 1;
}

esp_err_t tjpgd_gray_decode(const uint8_t *jpeg, size_t len, uint8_t *out, size_t out_cap,
                            int scale, int *width, int *height)
{
    if (jpeg == NULL || out == NULL || scale < 0 || scale > 3) {
        return ESP_ERR_INVALID_ARG;
    }

    /* JD_FASTDECODE 1 needs roughly 3.1 kB of scratch space. */
    static uint8_t pool[4096];
    gray_ctx_t ctx = { .data = jpeg, .len = len, .pos = 0, .out = out };
    JDEC jd;

    JRESULT res = jd_prepare(&jd, in_cb, pool, sizeof(pool), &ctx);
    if (res != JDR_OK) {
        return ESP_FAIL;
    }

    const int w = jd.width >> scale;
    const int h = jd.height >> scale;
    if (w <= 0 || h <= 0 || (size_t)w * (size_t)h > out_cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    ctx.out_w = w;
    ctx.out_h = h;

    res = jd_decomp(&jd, out_cb, (uint8_t)scale);
    if (res != JDR_OK) {
        return ESP_FAIL;
    }

    if (width != NULL) {
        *width = w;
    }
    if (height != NULL) {
        *height = h;
    }
    return ESP_OK;
}
