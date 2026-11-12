#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define SSD1306_WIDTH       128
#define SSD1306_MAX_HEIGHT  64

typedef struct {
    i2c_master_dev_handle_t i2c_dev;
    uint8_t addr;
    uint8_t height; /* 32 or 64 */
    uint8_t pages;
    uint8_t fb[SSD1306_WIDTH * SSD1306_MAX_HEIGHT / 8];
} ssd1306_t;

esp_err_t ssd1306_init(ssd1306_t *dev, i2c_master_bus_handle_t bus, uint8_t addr,
                       uint8_t height, uint32_t scl_hz);

esp_err_t ssd1306_flush(ssd1306_t *dev);

void ssd1306_clear(ssd1306_t *dev);
void ssd1306_pixel(ssd1306_t *dev, int x, int y, bool on);
void ssd1306_fill_rect(ssd1306_t *dev, int x, int y, int w, int h, bool on);
void ssd1306_text(ssd1306_t *dev, int x, int y, const char *text, uint8_t scale);
