# Firmware target selection:
#   CONFIG_FIRMWARE_BOARD_TEST=y   -> board_test.c  (serial telemetry only)
#   CONFIG_FIRMWARE_TFT_DASHBOARD=y -> tft_dashboard.c (ILI9341 TFT dashboard)
#
# Exactly one of the two must be enabled.
#
# NOTE: deliberately NOT listing REQUIRES/PRIV_REQUIRES here.
# IDF treats 'main' specially: when the user declares no requirements, IDF links
# main against every component in the build, which is what gives main access to
# the include directories of esp_driver_spi / esp_lcd / ili9341.
# Declaring REQUIRES manually opts out of that and only propagates includes for
# components that happen to be registered before main.

if(CONFIG_FIRMWARE_TFT_DASHBOARD)
    idf_component_register(SRCS "tft_dashboard.c" "wifi_page.c" INCLUDE_DIRS "")
else()
    idf_component_register(SRCS "board_test.c" INCLUDE_DIRS "")
endif()
