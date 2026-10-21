/**
 * @file adafruit_mini_tft.h
 * @brief Driver for the Adafruit Mini TFT 0.96" 160x80 breakout (product 3533).
 *
 * Covers the whole module, not just the controller:
 *  - both board revisions (Adafruit, "Display Versions"): they need different colour order, inversion and RAM offsets;
 *  - rotation, with the same numbering as Adafruit_ST7735::setRotation();
 *  - PWM backlight on the "Lite" pin (kept alive in light sleep if wanted);
 *  - panel sleep for low power;
 *  - a small blocking drawing API (fill, rectangles, 5x7 text).
 *
 * The SPI bus must be initialised by the application (spi_bus_initialize), so it can be shared with the breakout's microSD slot or other devices.
 *
 * For LVGL, use adafruit_mini_tft_get_panel()/get_io() with esp_lvgl_port and do NOT use the drawing API (LVGL replaces the IO "transfer done" callback).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/ledc.h"
#include "driver/spi_common.h"
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Board revision, see the "A"/"B" mark on the back of the TFT flex. */
typedef enum {
    ADAFRUIT_MINI_TFT_REV_A,  /**< before Jan 2023: larger chip, BGR, not inverted, offset 24/0 */
    ADAFRUIT_MINI_TFT_REV_B,  /**< since Jan 2023:  plug-in FPC, RGB, inverted, offset 26/1 */
} adafruit_mini_tft_rev_t;

/** Same numbering as Adafruit_ST7735::setRotation(). */
typedef enum {
    ADAFRUIT_MINI_TFT_ROT_0 = 0,  /**< portrait  80x160 */
    ADAFRUIT_MINI_TFT_ROT_1 = 1,  /**< landscape 160x80 */
    ADAFRUIT_MINI_TFT_ROT_2 = 2,  /**< portrait  80x160, flipped */
    ADAFRUIT_MINI_TFT_ROT_3 = 3,  /**< landscape 160x80, flipped */
} adafruit_mini_tft_rotation_t;

typedef struct {
    spi_host_device_t spi_host;   /**< bus already initialised by the caller */
    int pin_cs;                   /**< TFT_CS */
    int pin_dc;                   /**< D/C */
    int pin_rst;                  /**< RST, -1 if tied (software reset is used) */
    int pin_lite;                 /**< Lite (backlight PWM), -1 if not wired (always on) */
    int pclk_hz;                  /**< 0 -> ST7735S_SPI_PCLK_HZ_DEFAULT */
    adafruit_mini_tft_rev_t       revision;
    adafruit_mini_tft_rotation_t  rotation;
    ledc_timer_t   ledc_timer;    /**< used only if pin_lite >= 0 */
    ledc_channel_t ledc_channel;
    bool backlight_in_light_sleep; /**< keep PWM running in light sleep (RC_FAST clock) */
} adafruit_mini_tft_config_t;

#define ADAFRUIT_MINI_TFT_DEFAULT_CONFIG()           \
    {                                                \
        .spi_host = SPI2_HOST,                       \
        .pin_cs = -1,                                \
        .pin_dc = -1,                                \
        .pin_rst = -1,                               \
        .pin_lite = -1,                              \
        .pclk_hz = 0,                                \
        .revision = ADAFRUIT_MINI_TFT_REV_B,         \
        .rotation = ADAFRUIT_MINI_TFT_ROT_1,         \
        .ledc_timer = LEDC_TIMER_0,                  \
        .ledc_channel = LEDC_CHANNEL_0,              \
        .backlight_in_light_sleep = true,            \
    }

/** 16-bit RGB565 colour from 8-bit components (native endianness). */
#define ADAFRUIT_MINI_TFT_RGB(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define ADAFRUIT_MINI_TFT_BLACK   0x0000
#define ADAFRUIT_MINI_TFT_WHITE   0xFFFF
#define ADAFRUIT_MINI_TFT_RED     0xF800
#define ADAFRUIT_MINI_TFT_GREEN   0x07E0
#define ADAFRUIT_MINI_TFT_BLUE    0x001F
#define ADAFRUIT_MINI_TFT_YELLOW  0xFFE0
#define ADAFRUIT_MINI_TFT_CYAN    0x07FF
#define ADAFRUIT_MINI_TFT_MAGENTA 0xF81F

/** Glyph cell of the built-in font at scale 1 (5x7 glyph + 1px spacing). */
#define ADAFRUIT_MINI_TFT_FONT_W    6
#define ADAFRUIT_MINI_TFT_FONT_H    8
#define ADAFRUIT_MINI_TFT_MAX_SCALE 5

typedef struct adafruit_mini_tft *adafruit_mini_tft_handle_t;

/** Reset, initialise, clear to black, turn on (backlight 100 %). */
esp_err_t adafruit_mini_tft_new(const adafruit_mini_tft_config_t *cfg, adafruit_mini_tft_handle_t *ret_tft);
esp_err_t adafruit_mini_tft_del(adafruit_mini_tft_handle_t tft);

esp_err_t adafruit_mini_tft_set_rotation(adafruit_mini_tft_handle_t tft, adafruit_mini_tft_rotation_t rot);
uint16_t  adafruit_mini_tft_width(adafruit_mini_tft_handle_t tft);
uint16_t  adafruit_mini_tft_height(adafruit_mini_tft_handle_t tft);

/** 0..100 %. Returns ESP_ERR_NOT_SUPPORTED if pin_lite is not wired. */
esp_err_t adafruit_mini_tft_set_backlight(adafruit_mini_tft_handle_t tft, uint8_t percent);

/**
 * true: backlight off + panel sleep-in (controller in µA range, RAM kept).
 * false: sleep-out (120 ms) + backlight back to the last level.
 */
esp_err_t adafruit_mini_tft_sleep(adafruit_mini_tft_handle_t tft, bool sleep);

/**
 * Initialise the controller again (software reset, init sequence, rotation, cleared screen, display on), keeping the handle. 
 * For when the panel may have lost power while the MCU did not: e.g. a microSD card inserted in the slot of the same breakout draws a current spike that can reset the ST7735, which then stays blank. 
 * The panel ends awake; the caller redraws (and puts it back to sleep if it was).
 */
esp_err_t adafruit_mini_tft_reinit(adafruit_mini_tft_handle_t tft);

/* Drawing (blocking, thread-safe, clipped to the screen) */
esp_err_t adafruit_mini_tft_fill(adafruit_mini_tft_handle_t tft, uint16_t color);
esp_err_t adafruit_mini_tft_fill_rect(adafruit_mini_tft_handle_t tft, int x, int y, int w, int h, uint16_t color);
esp_err_t adafruit_mini_tft_draw_rect(adafruit_mini_tft_handle_t tft, int x, int y, int w, int h, uint16_t color);

/**
 * ASCII text with the built-in 5x7 font, scale 1..ADAFRUIT_MINI_TFT_MAX_SCALE.
 * '\n' starts a new line at x. 
 * Characters that do not fit are dropped.
 */
esp_err_t adafruit_mini_tft_draw_text(adafruit_mini_tft_handle_t tft, int x, int y, const char *text, uint16_t fg, uint16_t bg, uint8_t scale);

/* Escape hatches (LVGL, custom drawing with esp_lcd_panel_draw_bitmap) */
esp_lcd_panel_handle_t    adafruit_mini_tft_get_panel(adafruit_mini_tft_handle_t tft);
esp_lcd_panel_io_handle_t adafruit_mini_tft_get_io(adafruit_mini_tft_handle_t tft);

#ifdef __cplusplus
}
#endif