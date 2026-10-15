/*
 * SPDX-FileCopyrightText: 2026 Nelson Ramos
 * SPDX-License-Identifier: MIT
 *
 * Structure adapted from ESP-IDF esp_lcd_panel_st7789.c
 * (Espressif Systems (Shanghai) CO LTD, Apache-2.0); init values from t4v4res/st7735_driver (MIT).
 *
 * esp_lcd panel driver for the Sitronix ST7735S controller (the chip on the Adafruit Mini TFT 0.96" flex). 
 * Board-level details (revision, offsets, backlight) live in adafruit_mini_tft.c.
 */
#include <stdlib.h>
#include <sys/cdefs.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_lcd_st7735s.h"

static const char *TAG = "st7735s";

/* ST7735S-specific commands (not in esp_lcd_panel_commands.h) */
#define ST7735S_FRMCTR1 0xB1
#define ST7735S_FRMCTR2 0xB2
#define ST7735S_FRMCTR3 0xB3
#define ST7735S_INVCTR  0xB4
#define ST7735S_PWCTR1  0xC0
#define ST7735S_PWCTR2  0xC1
#define ST7735S_PWCTR3  0xC2
#define ST7735S_PWCTR4  0xC3
#define ST7735S_PWCTR5  0xC4
#define ST7735S_VMCTR1  0xC5
#define ST7735S_GMCTRP1 0xE0
#define ST7735S_GMCTRN1 0xE1

/* Datasheet timings */
#define ST7735S_SLPOUT_DELAY_MS 120  /* after SLPOUT before next command / SLPIN */
#define ST7735S_SLPIN_DELAY_MS  5
#define ST7735S_RESET_PULSE_MS  10
#define ST7735S_RESET_WAIT_MS   120

/*
 * Default sequence: frame rate, power and gamma values of Adafruit's Rcmd1/Rcmd3 tables for ST7735R/S panels. 
 * Panel-neutral: inversion and offsets are panel specific and set through the esp_lcd API.
 * No SWRESET (that is reset()'s job) and no MADCTL/COLMOD (sent last, from the panel config).
 */
static const st7735s_lcd_init_cmd_t s_default_init[] = {
    {ST7735S_FRMCTR1, (const uint8_t[]){0x01, 0x2C, 0x2D}, 3, 0},
    {ST7735S_FRMCTR2, (const uint8_t[]){0x01, 0x2C, 0x2D}, 3, 0},
    {ST7735S_FRMCTR3, (const uint8_t[]){0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D}, 6, 0},
    {ST7735S_INVCTR,  (const uint8_t[]){0x07}, 1, 0},
    {ST7735S_PWCTR1,  (const uint8_t[]){0xA2, 0x02, 0x84}, 3, 0},
    {ST7735S_PWCTR2,  (const uint8_t[]){0xC5}, 1, 0},
    {ST7735S_PWCTR3,  (const uint8_t[]){0x0A, 0x00}, 2, 0},
    {ST7735S_PWCTR4,  (const uint8_t[]){0x8A, 0x2A}, 2, 0},
    {ST7735S_PWCTR5,  (const uint8_t[]){0x8A, 0xEE}, 2, 0},
    {ST7735S_VMCTR1,  (const uint8_t[]){0x0E}, 1, 0},
    {ST7735S_GMCTRP1, (const uint8_t[]){0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10}, 16, 0},
    {ST7735S_GMCTRN1, (const uint8_t[]){0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10}, 16, 0},
    {LCD_CMD_INVOFF,  NULL, 0, 0},
    {LCD_CMD_NORON,   NULL, 0, 10},
};

typedef struct {
    esp_lcd_panel_t               base;
    esp_lcd_panel_io_handle_t     io;
    int                           reset_gpio_num;
    bool                          reset_level;
    int                           x_gap;
    int                           y_gap;
    uint8_t                       fb_bits_per_pixel;
    uint8_t                       madctl_val;
    uint8_t                       colmod_val;
    const st7735s_lcd_init_cmd_t *init_cmds;
    uint16_t                      init_cmds_size;
} st7735s_panel_t;

static esp_err_t tx_param(st7735s_panel_t *p, int cmd, const void *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(p->io, cmd, data, len);
}

static esp_err_t panel_del(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    if (p->reset_gpio_num >= 0) {
        gpio_reset_pin(p->reset_gpio_num);
    }
    free(p);
    return ESP_OK;
}

static esp_err_t panel_reset(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    if (p->reset_gpio_num >= 0) {
        gpio_set_level(p->reset_gpio_num, p->reset_level);
        vTaskDelay(pdMS_TO_TICKS(ST7735S_RESET_PULSE_MS));
        gpio_set_level(p->reset_gpio_num, !p->reset_level);
    } else {
        ESP_RETURN_ON_ERROR(tx_param(p, LCD_CMD_SWRESET, NULL, 0), TAG, "SWRESET");
    }
    /* Reset leaves the panel in sleep-in; SLPOUT must wait 120 ms after reset */
    vTaskDelay(pdMS_TO_TICKS(ST7735S_RESET_WAIT_MS));
    return ESP_OK;
}

static esp_err_t panel_init(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);

    ESP_RETURN_ON_ERROR(tx_param(p, LCD_CMD_SLPOUT, NULL, 0), TAG, "SLPOUT");
    vTaskDelay(pdMS_TO_TICKS(ST7735S_SLPOUT_DELAY_MS));

    for (uint16_t i = 0; i < p->init_cmds_size; i++) {
        const st7735s_lcd_init_cmd_t *c = &p->init_cmds[i];
        /* Table values for these win, and are re-sent below */
        if (c->cmd == LCD_CMD_MADCTL && c->data_bytes >= 1) {
            p->madctl_val = ((const uint8_t *)c->data)[0];
            continue;
        }
        if (c->cmd == LCD_CMD_COLMOD && c->data_bytes >= 1) {
            p->colmod_val = ((const uint8_t *)c->data)[0];
            continue;
        }
        ESP_RETURN_ON_ERROR(tx_param(p, c->cmd, c->data, c->data_bytes), TAG, "cmd 0x%02X", c->cmd);
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }

    /* Last, so nothing in the table can silently undo them */
    ESP_RETURN_ON_ERROR(tx_param(p, LCD_CMD_MADCTL, (uint8_t[]){p->madctl_val}, 1), TAG, "MADCTL");
    ESP_RETURN_ON_ERROR(tx_param(p, LCD_CMD_COLMOD, (uint8_t[]){p->colmod_val}, 1), TAG, "COLMOD");
    ESP_LOGD(TAG, "init done (madctl=0x%02X colmod=0x%02X)", p->madctl_val, p->colmod_val);
    return ESP_OK;
}

static esp_err_t panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                   const void *color_data)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    ESP_RETURN_ON_FALSE(x_start < x_end && y_start < y_end, ESP_ERR_INVALID_ARG, TAG, "empty area");

    x_start += p->x_gap;
    x_end += p->x_gap;
    y_start += p->y_gap;
    y_end += p->y_gap;

    ESP_RETURN_ON_ERROR(tx_param(p, LCD_CMD_CASET, (uint8_t[]){
        (x_start >> 8) & 0xFF, x_start & 0xFF, ((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF,
    }, 4), TAG, "CASET");
    ESP_RETURN_ON_ERROR(tx_param(p, LCD_CMD_RASET, (uint8_t[]){
        (y_start >> 8) & 0xFF, y_start & 0xFF, ((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF,
    }, 4), TAG, "RASET");

    size_t len = (size_t)(x_end - x_start) * (y_end - y_start) * p->fb_bits_per_pixel / 8;
    return esp_lcd_panel_io_tx_color(p->io, LCD_CMD_RAMWR, color_data, len);
}

static esp_err_t panel_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    return tx_param(p, invert ? LCD_CMD_INVON : LCD_CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    p->madctl_val = mirror_x ? (p->madctl_val | LCD_CMD_MX_BIT) : (p->madctl_val & ~LCD_CMD_MX_BIT);
    p->madctl_val = mirror_y ? (p->madctl_val | LCD_CMD_MY_BIT) : (p->madctl_val & ~LCD_CMD_MY_BIT);
    return tx_param(p, LCD_CMD_MADCTL, (uint8_t[]){p->madctl_val}, 1);
}

static esp_err_t panel_swap_xy(esp_lcd_panel_t *panel, bool swap)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    p->madctl_val = swap ? (p->madctl_val | LCD_CMD_MV_BIT) : (p->madctl_val & ~LCD_CMD_MV_BIT);
    return tx_param(p, LCD_CMD_MADCTL, (uint8_t[]){p->madctl_val}, 1);
}

static esp_err_t panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    p->x_gap = x_gap;
    p->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    return tx_param(p, on ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, NULL, 0);
}

/* Sleep-in stops the internal oscillator and DC/DC: panel current drops to µA. */
static esp_err_t panel_disp_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    ESP_RETURN_ON_ERROR(tx_param(p, sleep ? LCD_CMD_SLPIN : LCD_CMD_SLPOUT, NULL, 0), TAG, "sleep");
    vTaskDelay(pdMS_TO_TICKS(sleep ? ST7735S_SLPIN_DELAY_MS : ST7735S_SLPOUT_DELAY_MS));
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_st7735s(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *cfg,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && cfg && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    uint8_t madctl;
    switch (cfg->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB: madctl = 0;               break;
    case LCD_RGB_ELEMENT_ORDER_BGR: madctl = LCD_CMD_BGR_BIT; break;
    default: ESP_RETURN_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, TAG, "unsupported RGB order");
    }

    uint8_t colmod, fb_bpp;
    switch (cfg->bits_per_pixel) {
    case 16: colmod = 0x05; fb_bpp = 16; break;  /* RGB565 */
    case 18: colmod = 0x06; fb_bpp = 24; break;  /* RGB666, one byte per component */
    default: ESP_RETURN_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, TAG, "unsupported bpp");
    }

    if (cfg->reset_gpio_num >= 0) {
        const gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << cfg->reset_gpio_num,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "reset gpio");
    }

    st7735s_panel_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        if (cfg->reset_gpio_num >= 0) {
            gpio_reset_pin(cfg->reset_gpio_num);
        }
        return ESP_ERR_NO_MEM;
    }

    p->io = io;
    p->reset_gpio_num = cfg->reset_gpio_num;
    p->reset_level = cfg->flags.reset_active_high;
    p->madctl_val = madctl;
    p->colmod_val = colmod;
    p->fb_bits_per_pixel = fb_bpp;

    const st7735s_vendor_config_t *vendor = cfg->vendor_config;
    if (vendor && vendor->init_cmds) {
        p->init_cmds = vendor->init_cmds;
        p->init_cmds_size = vendor->init_cmds_size;
    } else {
        p->init_cmds = s_default_init;
        p->init_cmds_size = sizeof(s_default_init) / sizeof(s_default_init[0]);
    }

    p->base.del = panel_del;
    p->base.reset = panel_reset;
    p->base.init = panel_init;
    p->base.draw_bitmap = panel_draw_bitmap;
    p->base.invert_color = panel_invert_color;
    p->base.mirror = panel_mirror;
    p->base.swap_xy = panel_swap_xy;
    p->base.set_gap = panel_set_gap;
    p->base.disp_on_off = panel_disp_on_off;
    p->base.disp_sleep = panel_disp_sleep;

    *ret_panel = &p->base;
    ESP_LOGD(TAG, "new panel @%p", p);
    return ESP_OK;
}