# adafruit_mini_tft

[![Component Registry](https://components.espressif.com/components/nelsonramosua/adafruit_mini_tft/badge.svg)](https://components.espressif.com/components/nelsonramosua/adafruit_mini_tft)
[![Build](https://github.com/nelsonramosua/adafruit_mini_tft/actions/workflows/build.yml/badge.svg)](https://github.com/nelsonramosua/adafruit_mini_tft/actions/workflows/build.yml)

ESP-IDF driver for the **Adafruit Mini TFT 0.96" 160x80** breakout ([product 3533](https://learn.adafruit.com/adafruit-mini-tft-0-dot-96-inch-180x60-breakout)).

Developed by Nelson Ramos for the *Dispositivos Conectados* (DC) course, under the supervision of Prof. Dr. Paulo Bartolomeu.

It drives the whole module, not just the display controller:

| Feature | Notes |
|---|---|
| Both board revisions | Rev A (before 2023) and Rev B (2023+) need different colour order, inversion and offsets |
| Rotation 0-3 | Same numbering as `Adafruit_ST7735::setRotation()` |
| PWM backlight | LEDC on the `Lite` pin, can keep running in light sleep |
| Low-power sleep | Backlight off + controller sleep-in (RAM is kept; wake takes 120 ms) |
| Drawing | Fill, rectangles, 5x7 text (scale 1-5): blocking and thread-safe |
| `esp_lcd` underneath | Shares the SPI bus with the breakout's microSD slot, uses DMA, works with LVGL |

The display controller on the module is a Sitronix ST7735S. 
Its `esp_lcd` panel driver is included (`esp_lcd_st7735s.h`) and can be used on its own with other ST7735S panels.

## Which revision do I have?

Look at the back of the TFT flex:

- **Rev A**: larger chip, an "A" in a circle in the upper-right corner.
- **Rev B**: smaller chip, plug-in flex, a "B" in a circle at the top centre.

| | Rev A | Rev B |
|---|---|---|
| MADCTL BGR bit | clear | set |
| Colour inversion | off | on |
| RAM offset (col, row) | 24, 0 | 26, 1 |

Source: Adafruit's guide ("Display Versions") and their Arduino/CircuitPython drivers. 
Note that the guide's CircuitPython `bgr=True` for Rev A actually *clears* the BGR bit in their driver... 
The Arduino library agrees.

## Usage

```c
#include "adafruit_mini_tft.h"
#include "driver/spi_master.h"

spi_bus_config_t bus = {
    .mosi_io_num = 19, .miso_io_num = 20, .sclk_io_num = 21,  /* MISO only for the SD card */
    .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096,
};
ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

adafruit_mini_tft_config_t cfg = ADAFRUIT_MINI_TFT_DEFAULT_CONFIG();
cfg.pin_cs = 22;
cfg.pin_dc = 23;
cfg.pin_rst = -1;                           /* auto-reset on the breakout, SWRESET used */
cfg.pin_lite = 11;
cfg.revision = ADAFRUIT_MINI_TFT_REV_B;
cfg.rotation = ADAFRUIT_MINI_TFT_ROT_1;     /* landscape 160x80 */

adafruit_mini_tft_handle_t tft;
ESP_ERROR_CHECK(adafruit_mini_tft_new(&cfg, &tft));

adafruit_mini_tft_draw_text(tft, 2, 2, "23.5C", ADAFRUIT_MINI_TFT_YELLOW, ADAFRUIT_MINI_TFT_BLACK, 3);
adafruit_mini_tft_set_backlight(tft, 40);
adafruit_mini_tft_sleep(tft, true);         /* idle: panel in µA range */
```

Colours are native RGB565 (`ADAFRUIT_MINI_TFT_RGB(r, g, b)` or the `ADAFRUIT_MINI_TFT_*` constants). 
The byte swap for the wire is done for you. :)

### LVGL

Pass `adafruit_mini_tft_get_panel()` and `adafruit_mini_tft_get_io()` to `esp_lvgl_port`, and enable byte swapping there. 
Do not mix LVGL with this driver's drawing API: LVGL installs its own "transfer done" callback.

## Hardware check

`examples/panel_test` runs a loop that checks edges/offsets, colour bars, text, all four rotations, the backlight fade, and sleep/wake. 
It logs what you should see at each step:

```sh
cd examples/panel_test
idf.py set-target esp32c6 menuconfig   # "Panel test": revision, rotation, pins
idf.py build flash monitor
```

| Symptom | Fix |
|---|---|
| Missing edge line or garbage line on one side | Wrong revision (offsets) |
| Red and blue swapped | Wrong revision (colour order) |
| Colours look like a photo negative | Wrong revision (inversion) |
| Text upside down or mirrored | Choose another rotation |

## Requirements

ESP-IDF >= 5.4 (LEDC light-sleep modes). 
Built and tested on ESP-IDF 6.1 / ESP32-C6.

## Status

Validated on hardware (ESP32-C6-DevKitC-1, Rev B breakout, ESP-IDF 6.1):
offsets, colours, text, all four rotations, backlight PWM and sleep/wake.

## Installation

```sh
idf.py add-dependency "nelsonramosua/adafruit_mini_tft^0.1.0"
```

## Contributing

Bug reports, hardware results (other chips, Rev A boards) and pull requests are welcome. 
See [CONTRIBUTING.md](CONTRIBUTING.md) and [CHANGELOG.md](CHANGELOG.md).

## Credits

Author: Nelson Ramos, MIT License (see `LICENSE`). Developed for the
*Dispositivos Conectados* (DC) course, under the supervision of
Prof. Paulo Bartolomeu.

Adapted from:

| Source | License | What was used |
|---|---|---|
| [t4v4res/st7735_driver](https://components.espressif.com/components/t4v4res/st7735_driver) (T4V4RES) | MIT | Initialisation sequence (frame rate, power, gamma) and the 5x7 font |
| [Adafruit-ST7735-Library](https://github.com/adafruit/Adafruit-ST7735-Library) and [Adafruit_CircuitPython_ST7735R](https://github.com/adafruit/Adafruit_CircuitPython_ST7735R) (Adafruit Industries) | MIT | Per-revision parameters (colour order, inversion, offsets) and rotation mapping |
| ESP-IDF `esp_lcd_panel_st7789.c` (Espressif Systems) | Apache-2.0 | Structure of the `esp_lcd` panel driver |

Full notices in `LICENSE`.