/*
 * Hardware check for the Adafruit Mini TFT 0.96" driver.
 *
 * Each step logs what you should see. 
 * If something is off:
 *   - frame edge missing or a garbage line on one side  -> wrong revision (offsets)
 *   - red and blue swapped                              -> wrong revision (colour order)
 *   - colours look like a photo negative                -> wrong revision (inversion)
 *   - text upside down / mirrored                       -> pick another rotation
 */
#include <stdio.h>

#include "adafruit_mini_tft.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "panel_test";

#define STEP_MS 5000

static void step(const char *expect)
{
    ESP_LOGI(TAG, "EXPECT: %s", expect);
    vTaskDelay(pdMS_TO_TICKS(STEP_MS));
}

static void frame_test(adafruit_mini_tft_handle_t tft)
{
    int w = adafruit_mini_tft_width(tft), h = adafruit_mini_tft_height(tft);
    adafruit_mini_tft_fill(tft, ADAFRUIT_MINI_TFT_BLACK);
    adafruit_mini_tft_draw_rect(tft, 0, 0, w, h, ADAFRUIT_MINI_TFT_WHITE);
    adafruit_mini_tft_draw_rect(tft, 2, 2, w - 4, h - 4, ADAFRUIT_MINI_TFT_YELLOW);
    adafruit_mini_tft_draw_text(tft, 6, 6, "EDGES", ADAFRUIT_MINI_TFT_WHITE, ADAFRUIT_MINI_TFT_BLACK, 1);
    step("black screen, white 1px frame on ALL four edges, 1px yellow frame 1px inside it, 'EDGES' top-left");
}

static void colour_test(adafruit_mini_tft_handle_t tft)
{
    static const struct {
        uint16_t    color;
        const char *label;
    } bars[] = {
        {ADAFRUIT_MINI_TFT_RED, "R"},
        {ADAFRUIT_MINI_TFT_GREEN, "G"},
        {ADAFRUIT_MINI_TFT_BLUE, "B"},
        {ADAFRUIT_MINI_TFT_WHITE, "W"},
    };
    int w = adafruit_mini_tft_width(tft), h = adafruit_mini_tft_height(tft);
    bool landscape = w > h;
    int n = sizeof(bars) / sizeof(bars[0]);

    for (int i = 0; i < n; i++) {
        int x = landscape ? i * w / n : 0;
        int y = landscape ? 0 : i * h / n;
        int bw = landscape ? w / n : w;
        int bh = landscape ? h : h / n;
        adafruit_mini_tft_fill_rect(tft, x, y, bw, bh, bars[i].color);
        adafruit_mini_tft_draw_text(tft, x + 2, y + 2, bars[i].label, ADAFRUIT_MINI_TFT_BLACK, bars[i].color, 2);
    }
    step("bars labelled R G B W in red, green, blue, white (not swapped, not negative)");
}

static void text_test(adafruit_mini_tft_handle_t tft)
{
    adafruit_mini_tft_fill(tft, ADAFRUIT_MINI_TFT_BLACK);
    adafruit_mini_tft_draw_text(tft, 0, 0, "scale 1: 0123456789\n!\"#$%&'()*+,-./:;<=>?@", ADAFRUIT_MINI_TFT_WHITE, ADAFRUIT_MINI_TFT_BLACK, 1);
    adafruit_mini_tft_draw_text(tft, 0, 20, "scale 2", ADAFRUIT_MINI_TFT_CYAN, ADAFRUIT_MINI_TFT_BLACK, 2);
    adafruit_mini_tft_draw_text(tft, 0, 40, "23.5C", ADAFRUIT_MINI_TFT_YELLOW, ADAFRUIT_MINI_TFT_BLACK, 4);
    step("crisp text at three sizes, '23.5C' large and yellow");
}

static void rotation_test(adafruit_mini_tft_handle_t tft, adafruit_mini_tft_rotation_t original)
{
    char label[16];
    for (int r = 0; r < 4; r++) {
        adafruit_mini_tft_set_rotation(tft, r);
        adafruit_mini_tft_fill(tft, ADAFRUIT_MINI_TFT_BLACK);
        adafruit_mini_tft_draw_rect(tft, 0, 0, adafruit_mini_tft_width(tft), adafruit_mini_tft_height(tft),  ADAFRUIT_MINI_TFT_WHITE);
        snprintf(label, sizeof(label), "ROT %d", r);
        adafruit_mini_tft_draw_text(tft, 4, 4, label, ADAFRUIT_MINI_TFT_GREEN, ADAFRUIT_MINI_TFT_BLACK, 2);
        ESP_LOGI(TAG, "rotation %d: %ux%u", r, adafruit_mini_tft_width(tft), adafruit_mini_tft_height(tft));
        step("'ROT n' readable at the top-left of the current orientation, full white frame");
    }
    adafruit_mini_tft_set_rotation(tft, original);
}

static void backlight_and_sleep_test(adafruit_mini_tft_handle_t tft)
{
    adafruit_mini_tft_fill(tft, ADAFRUIT_MINI_TFT_WHITE);
    adafruit_mini_tft_draw_text(tft, 4, 4, "BACKLIGHT", ADAFRUIT_MINI_TFT_BLACK, ADAFRUIT_MINI_TFT_WHITE, 2);

    ESP_LOGI(TAG, "EXPECT: backlight fades out and back in");
    for (int p = 100; p >= 0; p -= 5) {
        if (adafruit_mini_tft_set_backlight(tft, p) == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Lite pin not wired, skipping backlight test");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    for (int p = 0; p <= 100; p += 5) {
        adafruit_mini_tft_set_backlight(tft, p);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    adafruit_mini_tft_sleep(tft, true);
    step("screen dark (panel asleep, backlight off)");
    adafruit_mini_tft_sleep(tft, false);
    step("'BACKLIGHT' screen back exactly as before (RAM kept during sleep)");
}

void app_main(void)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_TEST_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = CONFIG_TEST_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    adafruit_mini_tft_config_t cfg = ADAFRUIT_MINI_TFT_DEFAULT_CONFIG();
    cfg.pin_cs = CONFIG_TEST_PIN_CS;
    cfg.pin_dc = CONFIG_TEST_PIN_DC;
    cfg.pin_rst = CONFIG_TEST_PIN_RST;
    cfg.pin_lite = CONFIG_TEST_PIN_LITE;
#if CONFIG_TEST_TFT_REV_A
    cfg.revision = ADAFRUIT_MINI_TFT_REV_A;
#else
    cfg.revision = ADAFRUIT_MINI_TFT_REV_B;
#endif
    cfg.rotation = CONFIG_TEST_TFT_ROTATION;

    adafruit_mini_tft_handle_t tft;
    ESP_ERROR_CHECK(adafruit_mini_tft_new(&cfg, &tft));

    for (;;) {
        frame_test(tft);
        colour_test(tft);
        text_test(tft);
        rotation_test(tft, cfg.rotation);
        backlight_and_sleep_test(tft);
    }
}