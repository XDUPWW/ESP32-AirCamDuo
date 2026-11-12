#pragma once

#include <stdint.h>

#define FONT5X7_FIRST_CHAR 0x20
#define FONT5X7_LAST_CHAR  0x7E
#define FONT5X7_COUNT      (FONT5X7_LAST_CHAR - FONT5X7_FIRST_CHAR + 1)
#define FONT5X7_WIDTH      5
#define FONT5X7_HEIGHT     7
#define FONT5X7_ADVANCE    6

/* column-major bitmaps: one byte per column, bit 0 is the top row */
extern const uint8_t font5x7[FONT5X7_COUNT][FONT5X7_WIDTH];
