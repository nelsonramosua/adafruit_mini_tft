# Changelog

All notable changes to this component. 
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.1] - 2026-10-15

### Added

- CI: example build on ESP-IDF 5.4, 5.5, 6.0 and 6.1, plus a registry package
  check; automatic publishing on `v*` tags.
- `CONTRIBUTING.md`, issue and pull request templates.
- README: registry and build badges, installation and contributing sections.

## [0.1.0] - 2026-10-15

### Added

- `esp_lcd` panel driver for the ST7735S controller (`esp_lcd_st7735s.h`), including `disp_sleep`.
- Adafruit Mini TFT 0.96" module driver (`adafruit_mini_tft.h`): Rev A/B presets, rotation 0-3, LEDC backlight PWM (kept in light sleep), panel sleep, fill/rect/5x7 text drawing.
- `examples/panel_test` hardware check.
- Validated on ESP32-C6-DevKitC-1 with a Rev B breakout, ESP-IDF 6.1.