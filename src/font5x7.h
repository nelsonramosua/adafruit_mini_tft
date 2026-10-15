#pragma once

#include <stdint.h>

#define FONT5X7_FIRST  0x20
#define FONT5X7_GLYPHS 96
#define FONT5X7_COLS   5
#define FONT5X7_ROWS   7

extern const uint8_t adafruit_mini_tft_font5x7[FONT5X7_GLYPHS * FONT5X7_COLS];