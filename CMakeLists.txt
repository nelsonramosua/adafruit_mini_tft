idf_component_register(
    SRCS "src/esp_lcd_st7735s.c" "src/adafruit_mini_tft.c" "src/font5x7.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_lcd esp_driver_ledc esp_driver_spi
    PRIV_REQUIRES esp_driver_gpio
)