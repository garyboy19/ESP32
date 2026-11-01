# The CJK glyph table is embedded in flash rather than RAM: this board has no
# PSRAM and the 16x16 glyph data is 124 kB. EMBED_FILES puts it in the app
# partition, and tft_cjk.c reads it through the _binary_*_start/_end symbols.
idf_component_register(SRCS "esp_lcd_panel_ili9341.c"
                       "tft_disp.c"
                       "tft_cjk.c"
                       INCLUDE_DIRS "include"
                       EMBED_FILES "include/font_cjk16.bin"
                       PRIV_REQUIRES esp_lcd esp_driver_gpio esp_driver_spi
                       REQUIRES esp_lcd)
