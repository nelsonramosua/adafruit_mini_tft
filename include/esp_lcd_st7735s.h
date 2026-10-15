/**
 * @file esp_lcd_st7735s.h
 * @brief esp_lcd panel driver for the Sitronix ST7735S controller.
 *
 * Generic controller layer. 
 * For the Adafruit Mini TFT 0.96" use adafruit_mini_tft.h, which picks the right settings per board revision.
 *
 * Works with any esp_lcd panel IO (normally esp_lcd_new_panel_io_spi), so it shares the SPI bus with other devices (e.g. an SD card) and plugs into LVGL through esp_lvgl_port.
 *
 * Follows the esp_lcd conventions: init() does not turn the display on, call esp_lcd_panel_disp_on_off(panel, true) afterwards.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One step of a vendor init sequence. */
typedef struct {
    uint8_t     cmd;
    const void *data;       /**< parameters, NULL if none */
    uint8_t     data_bytes;
    uint16_t    delay_ms;   /**< wait after the command */
} st7735s_lcd_init_cmd_t;

/**
 * Optional, passed as esp_lcd_panel_dev_config_t::vendor_config.
 * NULL -> built-in sequence (frame rate/power/gamma, no inversion).
 * MADCTL/COLMOD entries in the table override rgb_ele_order/bits_per_pixel.
 */
typedef struct {
    const st7735s_lcd_init_cmd_t *init_cmds;
    uint16_t                      init_cmds_size;
} st7735s_vendor_config_t;

/** Datasheet serial write cycle is >= 66 ns (~15 MHz). */
#define ST7735S_SPI_PCLK_HZ_DEFAULT   (15 * 1000 * 1000)

/** Convenience SPI panel IO config (8-bit commands and parameters, mode 0). */
#define ST7735S_PANEL_IO_SPI_CONFIG(cs, dc, cb, cb_ctx) \
    {                                                   \
        .cs_gpio_num = (cs),                            \
        .dc_gpio_num = (dc),                            \
        .spi_mode = 0,                                  \
        .pclk_hz = ST7735S_SPI_PCLK_HZ_DEFAULT,         \
        .trans_queue_depth = 10,                        \
        .on_color_trans_done = (cb),                    \
        .user_ctx = (cb_ctx),                           \
        .lcd_cmd_bits = 8,                              \
        .lcd_param_bits = 8,                            \
    }

/**
 * Create an ST7735S panel.
 *
 * Supported: bits_per_pixel 16 (RGB565, big-endian on the wire) or 18 (RGB666, 3 bytes/pixel).
 * rgb_ele_order RGB or BGR; reset_gpio_num >= 0 for hardware reset, -1 for software reset.
 */
esp_err_t esp_lcd_new_panel_st7735s(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif