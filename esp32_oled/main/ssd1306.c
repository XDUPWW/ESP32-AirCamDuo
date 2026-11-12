#include <string.h>

#include "ssd1306.h"
#include "font5x7.h"

#define CTRL_CMD  0x00
#define CTRL_DATA 0x40
#define I2C_TIMEOUT_MS 200
#define DATA_CHUNK 64

static const uint8_t init_cmds_64[] = {
    0xAE,              /* display off */
    0xD5, 0x80,        /* clock divide / oscillator */
    0xA8, 0x3F,        /* multiplex ratio 1/64 */
    0xD3, 0x00,        /* display offset 0 */
    0x40,              /* start line 0 */
    0x8D, 0x14,        /* charge pump on */
    0x20, 0x00,        /* horizontal addressing mode */
    0xA1,              /* segment remap (column 127 -> SEG0) */
    0xC8,              /* COM scan direction remapped */
    0xDA, 0x12,        /* COM pins for 128x64 */
    0x81, 0xCF,        /* contrast */
    0xD9, 0xF1,        /* pre-charge period */
    0xDB, 0x40,        /* VCOMH deselect */
    0xA4,              /* output follows RAM */
    0xA6,              /* normal (non-inverted) */
    0x2E,              /* deactivate scrolling */
    0xAF,              /* display on */
};

static const uint8_t init_cmds_32[] = {
    0xAE,
    0xD5, 0x80,
    0xA8, 0x1F,        /* multiplex ratio 1/32 */
    0xD3, 0x00,
    0x40,
    0x8D, 0x14,
    0x20, 0x00,
    0xA1,
    0xC8,
    0xDA, 0x02,        /* COM pins for 128x32 */
    0x81, 0x8F,
    0xD9, 0xF1,
    0xDB, 0x40,
    0xA4,
    0xA6,
    0x2E,
    0xAF,
};

static esp_err_t write_cmds(ssd1306_t *dev, const uint8_t *cmds, size_t len)
{
    uint8_t buf[1 + sizeof(init_cmds_64)];
    if (len > sizeof(buf) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = CTRL_CMD;
    memcpy(&buf[1], cmds, len);
    return i2c_master_transmit(dev->i2c_dev, buf, len + 1, I2C_TIMEOUT_MS);
}

esp_err_t ssd1306_init(ssd1306_t *dev, i2c_master_bus_handle_t bus, uint8_t addr,
                       uint8_t height, uint32_t scl_hz)
{
    if (height != 32 && height != 64) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(dev, 0, sizeof(*dev));
    dev->addr = addr;
    dev->height = height;
    dev->pages = height / 8;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = scl_hz,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev->i2c_dev);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t *cmds = height == 32 ? init_cmds_32 : init_cmds_64;
    size_t len = height == 32 ? sizeof(init_cmds_32) : sizeof(init_cmds_64);
    err = write_cmds(dev, cmds, len);
    if (err != ESP_OK) {
        return err;
    }
    return ssd1306_flush(dev);
}

esp_err_t ssd1306_flush(ssd1306_t *dev)
{
    const uint8_t window_cmds[] = {
        0x21, 0x00, SSD1306_WIDTH - 1,       /* column range */
        0x22, 0x00, (uint8_t)(dev->pages - 1) /* page range */
    };
    esp_err_t err = write_cmds(dev, window_cmds, sizeof(window_cmds));
    if (err != ESP_OK) {
        return err;
    }

    const size_t fb_len = (size_t)SSD1306_WIDTH * dev->pages;
    uint8_t buf[1 + DATA_CHUNK];
    buf[0] = CTRL_DATA;
    for (size_t off = 0; off < fb_len; off += DATA_CHUNK) {
        size_t n = (fb_len - off) < DATA_CHUNK ? (fb_len - off) : DATA_CHUNK;
        memcpy(&buf[1], dev->fb + off, n);
        err = i2c_master_transmit(dev->i2c_dev, buf, n + 1, I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

void ssd1306_clear(ssd1306_t *dev)
{
    memset(dev->fb, 0, sizeof(dev->fb));
}

void ssd1306_pixel(ssd1306_t *dev, int x, int y, bool on)
{
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= dev->height) {
        return;
    }
    uint8_t *byte = &dev->fb[(y / 8) * SSD1306_WIDTH + x];
    if (on) {
        *byte |= (uint8_t)(1u << (y & 7));
    } else {
        *byte &= (uint8_t)~(1u << (y & 7));
    }
}

void ssd1306_fill_rect(ssd1306_t *dev, int x, int y, int w, int h, bool on)
{
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            ssd1306_pixel(dev, col, row, on);
        }
    }
}

void ssd1306_text(ssd1306_t *dev, int x, int y, const char *text, uint8_t scale)
{
    if (scale == 0) {
        scale = 1;
    }

    int cursor = x;
    for (const char *p = text; *p != '\0'; p++) {
        unsigned ch = (unsigned char)*p;
        if (ch < FONT5X7_FIRST_CHAR || ch > FONT5X7_LAST_CHAR) {
            ch = '?';
        }
        const uint8_t *glyph = font5x7[ch - FONT5X7_FIRST_CHAR];

        for (int col = 0; col < FONT5X7_WIDTH; col++) {
            for (int row = 0; row < FONT5X7_HEIGHT; row++) {
                if ((glyph[col] >> row) & 1u) {
                    if (scale == 1) {
                        ssd1306_pixel(dev, cursor + col, y + row, true);
                    } else {
                        ssd1306_fill_rect(dev, cursor + col * scale, y + row * scale,
                                          scale, scale, true);
                    }
                }
            }
        }

        cursor += FONT5X7_ADVANCE * scale;
        if (cursor >= SSD1306_WIDTH) {
            break;
        }
    }
}
