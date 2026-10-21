/*
 * SPDX-FileCopyrightText: 2026 Nelson Ramos
 * SPDX-License-Identifier: MIT
 *
 * Adafruit Mini TFT 0.96" 160x80 breakout (product 3533).
 * Revision parameters and rotation mapping adapted from Adafruit-ST7735-Library and Adafruit_CircuitPython_ST7735R (Adafruit Industries, MIT).
 */
#include "adafruit_mini_tft.h"

#include <stdlib.h>
#include <string.h>

#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_lcd_st7735s.h"
#include "font5x7.h"

static const char *TAG = "mini_tft";

/* Native (rotation 0) geometry of the glass */
#define NATIVE_W 80
#define NATIVE_H 160

/* Scratch buffer: enough for 8 full landscape lines or one scale-5 glyph */
#define BUF_PIXELS (160 * 8)

#define TX_TIMEOUT_MS 1000

#define BL_RESOLUTION LEDC_TIMER_10_BIT
#define BL_MAX_DUTY   ((1 << 10) - 1)
#define BL_FREQ_HZ    5000

/*
 * Per-revision settings, from Adafruit's guide ("Display Versions") and their Arduino library (INITR_MINI160x80 vs INITR_MINI160x80_PLUGIN).
 *
 * Note on colour order: the guide's CircuitPython example passes bgr=True for Rev A, but that flag *clears* the MADCTL BGR bit in Adafruit's driver. 
 * The Arduino library agrees: Rev A -> bit clear, Rev B -> bit set.
 *
 * Offsets are symmetric (Rev A: 24+80+24 = 128 columns, 0+160+0 = 160 rows; Rev B: 26+80+26 = 132, 1+160+1 = 162), so mirroring never changes them.
 */
typedef struct {
    lcd_rgb_element_order_t order;
    bool                    invert;
    uint8_t                 colstart;
    uint8_t                 rowstart;
} rev_params_t;

static const rev_params_t s_rev[] = {
    [ADAFRUIT_MINI_TFT_REV_A] = {LCD_RGB_ELEMENT_ORDER_RGB, false, 24, 0},
    [ADAFRUIT_MINI_TFT_REV_B] = {LCD_RGB_ELEMENT_ORDER_BGR, true,  26, 1},
};

struct adafruit_mini_tft {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t    panel;
    adafruit_mini_tft_config_t cfg;
    const rev_params_t       *rev;
    uint16_t                  width;
    uint16_t                  height;
    uint8_t                   backlight;   /* last requested %, restored on wake */
    bool                      bl_ready;
    bool                      sleeping;
    SemaphoreHandle_t         lock;
    SemaphoreHandle_t         tx_done;
    uint16_t                 *buf;         /* DMA-capable, big-endian RGB565 */
};

/* ---------------------------------------------------------------- helpers */

static bool IRAM_ATTR on_tx_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    adafruit_mini_tft_handle_t tft = ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(tft->tx_done, &woken);
    return woken == pdTRUE;
}

static inline uint16_t to_wire(uint16_t color)
{
    return (uint16_t)((color >> 8) | (color << 8));  /* RGB565 goes MSB first */
}

/* draw_bitmap is asynchronous: wait until the buffer may be reused */
static esp_err_t blit(adafruit_mini_tft_handle_t tft, int x0, int y0, int x1, int y1)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(tft->panel, x0, y0, x1, y1, tft->buf), TAG, "draw");
    if (xSemaphoreTake(tft->tx_done, pdMS_TO_TICKS(TX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "transfer timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static bool clip(adafruit_mini_tft_handle_t tft, int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > tft->width)  { *w = tft->width - *x; }
    if (*y + *h > tft->height) { *h = tft->height - *y; }
    return *w > 0 && *h > 0;
}

static esp_err_t fill_rect_locked(adafruit_mini_tft_handle_t tft, int x, int y, int w, int h, uint16_t color)
{
    if (!clip(tft, &x, &y, &w, &h)) {
        return ESP_OK;
    }
    const int rows_per_chunk = BUF_PIXELS / w;
    const uint16_t c = to_wire(color);
    for (int i = 0; i < w * (h < rows_per_chunk ? h : rows_per_chunk); i++) {
        tft->buf[i] = c;
    }
    for (int row = y; row < y + h; row += rows_per_chunk) {
        int end = row + rows_per_chunk > y + h ? y + h : row + rows_per_chunk;
        ESP_RETURN_ON_ERROR(blit(tft, x, row, x + w, end), TAG, "fill");
    }
    return ESP_OK;
}

static esp_err_t apply_rotation(adafruit_mini_tft_handle_t tft, adafruit_mini_tft_rotation_t rot)
{
    /* Same MADCTL/offset mapping as Adafruit_ST7735::setRotation() */
    bool swap, mx, my;
    switch (rot) {
        case ADAFRUIT_MINI_TFT_ROT_0: swap = false; mx = true;  my = true;  break;
        case ADAFRUIT_MINI_TFT_ROT_1: swap = true;  mx = false; my = true;  break;
        case ADAFRUIT_MINI_TFT_ROT_2: swap = false; mx = false; my = false; break;
        case ADAFRUIT_MINI_TFT_ROT_3: swap = true;  mx = true;  my = false; break;
        default: return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(tft->panel, swap), TAG, "swap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(tft->panel, mx, my), TAG, "mirror");
    if (swap) {
        esp_lcd_panel_set_gap(tft->panel, tft->rev->rowstart, tft->rev->colstart);
        tft->width = NATIVE_H;
        tft->height = NATIVE_W;
    } else {
        esp_lcd_panel_set_gap(tft->panel, tft->rev->colstart, tft->rev->rowstart);
        tft->width = NATIVE_W;
        tft->height = NATIVE_H;
    }
    tft->cfg.rotation = rot;
    return ESP_OK;
}

static esp_err_t backlight_init(adafruit_mini_tft_handle_t tft)
{
    const bool keep = tft->cfg.backlight_in_light_sleep;
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_RESOLUTION,
        .timer_num = tft->cfg.ledc_timer,
        .freq_hz = BL_FREQ_HZ,
        /* Only RC_FAST keeps running in light sleep */
        .clk_cfg = keep ? LEDC_USE_RC_FAST_CLK : LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    const ledc_channel_config_t ch = {
        .gpio_num = tft->cfg.pin_lite,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = tft->cfg.ledc_channel,
        .timer_sel = tft->cfg.ledc_timer,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = keep ? LEDC_SLEEP_MODE_KEEP_ALIVE : LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc channel");
    tft->bl_ready = true;
    return ESP_OK;
}

static esp_err_t backlight_write(adafruit_mini_tft_handle_t tft, uint8_t percent)
{
    if (!tft->bl_ready) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint32_t duty = (uint32_t)percent * BL_MAX_DUTY / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, tft->cfg.ledc_channel, duty), TAG, "duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, tft->cfg.ledc_channel);
}

/* ------------------------------------------------------------- lifecycle */

esp_err_t adafruit_mini_tft_del(adafruit_mini_tft_handle_t tft)
{
    if (tft == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tft->bl_ready) {
        backlight_write(tft, 0);
        ledc_stop(LEDC_LOW_SPEED_MODE, tft->cfg.ledc_channel, 0);
    }
    if (tft->panel) {
        esp_lcd_panel_disp_on_off(tft->panel, false);
        esp_lcd_panel_del(tft->panel);
    }
    if (tft->io) {
        esp_lcd_panel_io_del(tft->io);
    }
    if (tft->lock) {
        vSemaphoreDelete(tft->lock);
    }
    if (tft->tx_done) {
        vSemaphoreDelete(tft->tx_done);
    }
    heap_caps_free(tft->buf);
    free(tft);
    return ESP_OK;
}

esp_err_t adafruit_mini_tft_new(const adafruit_mini_tft_config_t *cfg, adafruit_mini_tft_handle_t *ret_tft)
{
    ESP_RETURN_ON_FALSE(cfg && ret_tft, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(cfg->pin_cs >= 0 && cfg->pin_dc >= 0, ESP_ERR_INVALID_ARG, TAG, "CS and DC are required");
    ESP_RETURN_ON_FALSE(cfg->revision <= ADAFRUIT_MINI_TFT_REV_B, ESP_ERR_INVALID_ARG, TAG, "revision");

    esp_err_t ret = ESP_OK;  /* name required by ESP_GOTO_ON_* */
    adafruit_mini_tft_handle_t tft = calloc(1, sizeof(*tft));
    ESP_RETURN_ON_FALSE(tft, ESP_ERR_NO_MEM, TAG, "no mem");
    tft->cfg = *cfg;
    tft->rev = &s_rev[cfg->revision];
    tft->lock = xSemaphoreCreateMutex();
    tft->tx_done = xSemaphoreCreateBinary();
    tft->buf = heap_caps_malloc(BUF_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_GOTO_ON_FALSE(tft->lock && tft->tx_done && tft->buf, ESP_ERR_NO_MEM, fail, TAG, "no mem");

    esp_lcd_panel_io_spi_config_t io_cfg = ST7735S_PANEL_IO_SPI_CONFIG(cfg->pin_cs, cfg->pin_dc, on_tx_done, tft);
    if (cfg->pclk_hz > 0) {
        io_cfg.pclk_hz = cfg->pclk_hz;
    }
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi(cfg->spi_host, &io_cfg, &tft->io), fail, TAG, "panel io");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = cfg->pin_rst,
        .rgb_ele_order = tft->rev->order,
        .bits_per_pixel = 16,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7735s(tft->io, &panel_cfg, &tft->panel), fail, TAG, "panel");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(tft->panel), fail, TAG, "reset");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(tft->panel), fail, TAG, "init");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_invert_color(tft->panel, tft->rev->invert), fail, TAG, "invert");
    ESP_GOTO_ON_ERROR(apply_rotation(tft, cfg->rotation), fail, TAG, "rotation");

    /* Clear RAM before showing it, so no garbage flashes on screen */
    ESP_GOTO_ON_ERROR(fill_rect_locked(tft, 0, 0, tft->width, tft->height, ADAFRUIT_MINI_TFT_BLACK), fail, TAG, "clear");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(tft->panel, true), fail, TAG, "on");

    tft->backlight = 100;
    if (cfg->pin_lite >= 0) {
        ESP_GOTO_ON_ERROR(backlight_init(tft), fail, TAG, "backlight");
        backlight_write(tft, tft->backlight);
    }

    ESP_LOGI(TAG, "Rev %c, %ux%u, rotation %d", cfg->revision == ADAFRUIT_MINI_TFT_REV_A ? 'A' : 'B',
             tft->width, tft->height, cfg->rotation);
    *ret_tft = tft;
    return ESP_OK;

fail:
    adafruit_mini_tft_del(tft);
    return ret;
}

/* --------------------------------------------------------------- control */

esp_err_t adafruit_mini_tft_reinit(adafruit_mini_tft_handle_t tft)
{
    ESP_RETURN_ON_FALSE(tft, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    xSemaphoreTake(tft->lock, portMAX_DELAY);
    esp_err_t err = esp_lcd_panel_reset(tft->panel);  /* RST not wired: SWRESET */
    if (err == ESP_OK) err = esp_lcd_panel_init(tft->panel);
    if (err == ESP_OK) err = esp_lcd_panel_invert_color(tft->panel, tft->rev->invert);
    if (err == ESP_OK) err = apply_rotation(tft, tft->cfg.rotation);
    if (err == ESP_OK) err = fill_rect_locked(tft, 0, 0, tft->width, tft->height, ADAFRUIT_MINI_TFT_BLACK);
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(tft->panel, true);
    if (err == ESP_OK) {
        tft->sleeping = false;
        if (tft->bl_ready) {
            backlight_write(tft, tft->backlight);
        }
    }
    xSemaphoreGive(tft->lock);
    return err;
}

esp_err_t adafruit_mini_tft_set_rotation(adafruit_mini_tft_handle_t tft, adafruit_mini_tft_rotation_t rot)
{
    ESP_RETURN_ON_FALSE(tft, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    xSemaphoreTake(tft->lock, portMAX_DELAY);
    esp_err_t err = apply_rotation(tft, rot);
    xSemaphoreGive(tft->lock);
    return err;
}

uint16_t adafruit_mini_tft_width(adafruit_mini_tft_handle_t tft)
{
    return tft ? tft->width : 0;
}

uint16_t adafruit_mini_tft_height(adafruit_mini_tft_handle_t tft)
{
    return tft ? tft->height : 0;
}

esp_err_t adafruit_mini_tft_set_backlight(adafruit_mini_tft_handle_t tft, uint8_t percent)
{
    ESP_RETURN_ON_FALSE(tft, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    if (percent > 100) {
        percent = 100;
    }
    xSemaphoreTake(tft->lock, portMAX_DELAY);
    tft->backlight = percent;
    /* While asleep only remember it; applied on wake */
    esp_err_t err = tft->sleeping ? (tft->bl_ready ? ESP_OK : ESP_ERR_NOT_SUPPORTED)
                                  : backlight_write(tft, percent);
    xSemaphoreGive(tft->lock);
    return err;
}

esp_err_t adafruit_mini_tft_sleep(adafruit_mini_tft_handle_t tft, bool sleep)
{
    ESP_RETURN_ON_FALSE(tft, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    esp_err_t err = ESP_OK;

    xSemaphoreTake(tft->lock, portMAX_DELAY);
    if (sleep != tft->sleeping) {
        if (sleep) {
            if (tft->bl_ready) {
                backlight_write(tft, 0);
            }
            err = esp_lcd_panel_disp_sleep(tft->panel, true);
        } else {
            err = esp_lcd_panel_disp_sleep(tft->panel, false);
            if (err == ESP_OK && tft->bl_ready) {
                backlight_write(tft, tft->backlight);
            }
        }
        if (err == ESP_OK) {
            tft->sleeping = sleep;
        }
    }
    xSemaphoreGive(tft->lock);
    return err;
}

/* --------------------------------------------------------------- drawing */

esp_err_t adafruit_mini_tft_fill_rect(adafruit_mini_tft_handle_t tft, int x, int y, int w, int h, uint16_t color)
{
    ESP_RETURN_ON_FALSE(tft, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    xSemaphoreTake(tft->lock, portMAX_DELAY);
    esp_err_t err = fill_rect_locked(tft, x, y, w, h, color);
    xSemaphoreGive(tft->lock);
    return err;
}

esp_err_t adafruit_mini_tft_fill(adafruit_mini_tft_handle_t tft, uint16_t color)
{
    ESP_RETURN_ON_FALSE(tft, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    return adafruit_mini_tft_fill_rect(tft, 0, 0, tft->width, tft->height, color);
}

esp_err_t adafruit_mini_tft_draw_rect(adafruit_mini_tft_handle_t tft, int x, int y, int w, int h, uint16_t color)
{
    ESP_RETURN_ON_FALSE(tft, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }
    xSemaphoreTake(tft->lock, portMAX_DELAY);
    esp_err_t err = fill_rect_locked(tft, x, y, w, 1, color);
    if (err == ESP_OK) err = fill_rect_locked(tft, x, y + h - 1, w, 1, color);
    if (err == ESP_OK) err = fill_rect_locked(tft, x, y, 1, h, color);
    if (err == ESP_OK) err = fill_rect_locked(tft, x + w - 1, y, 1, h, color);
    xSemaphoreGive(tft->lock);
    return err;
}

/* Render one glyph cell (glyph + spacing) into buf and send it */
static esp_err_t draw_glyph(adafruit_mini_tft_handle_t tft, int x, int y, char c, uint16_t fg, uint16_t bg, uint8_t scale)
{
    if (c < FONT5X7_FIRST || c >= FONT5X7_FIRST + FONT5X7_GLYPHS) {
        c = '?';
    }
    const uint8_t *glyph = &adafruit_mini_tft_font5x7[(c - FONT5X7_FIRST) * FONT5X7_COLS];
    const int cw = ADAFRUIT_MINI_TFT_FONT_W * scale;
    const int ch = ADAFRUIT_MINI_TFT_FONT_H * scale;
    const uint16_t f = to_wire(fg), b = to_wire(bg);

    for (int py = 0; py < ch; py++) {
        const int row = py / scale;
        for (int px = 0; px < cw; px++) {
            const int col = px / scale;
            bool on = col < FONT5X7_COLS && row < FONT5X7_ROWS && ((glyph[col] >> row) & 1);
            tft->buf[py * cw + px] = on ? f : b;
        }
    }
    return blit(tft, x, y, x + cw, y + ch);
}

esp_err_t adafruit_mini_tft_draw_text(adafruit_mini_tft_handle_t tft, int x, int y, const char *text, uint16_t fg, uint16_t bg, uint8_t scale)
{
    ESP_RETURN_ON_FALSE(tft && text, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(scale >= 1 && scale <= ADAFRUIT_MINI_TFT_MAX_SCALE, ESP_ERR_INVALID_ARG, TAG, "scale");
    ESP_RETURN_ON_FALSE(x >= 0 && y >= 0, ESP_ERR_INVALID_ARG, TAG, "negative origin");

    const int cw = ADAFRUIT_MINI_TFT_FONT_W * scale;
    const int ch = ADAFRUIT_MINI_TFT_FONT_H * scale;
    esp_err_t err = ESP_OK;
    int cx = x, cy = y;

    xSemaphoreTake(tft->lock, portMAX_DELAY);
    for (const char *p = text; *p && err == ESP_OK; p++) {
        if (*p == '\n') {
            cx = x;
            cy += ch;
            continue;
        }
        if (cy + ch > tft->height) {
            break;
        }
        if (cx + cw <= tft->width) {
            err = draw_glyph(tft, cx, cy, *p, fg, bg, scale);
        }
        cx += cw;
    }
    xSemaphoreGive(tft->lock);
    return err;
}

esp_lcd_panel_handle_t adafruit_mini_tft_get_panel(adafruit_mini_tft_handle_t tft)
{
    return tft ? tft->panel : NULL;
}

esp_lcd_panel_io_handle_t adafruit_mini_tft_get_io(adafruit_mini_tft_handle_t tft)
{
    return tft ? tft->io : NULL;
}