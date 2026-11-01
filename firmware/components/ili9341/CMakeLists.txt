idf_component_register(SRCS "esp_lcd_panel_ili9341.c"
                       "tft_disp.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_lcd esp_driver_gpio esp_driver_spi
                       REQUIRES esp_lcd)
