/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ILI9341 LCD panel driver for ESP-IDF (esp_lcd compatible).
 *
 * ESP-IDF v6.1 ships ST7789 / SSD1306 panels only, so this component implements
 * the ILI9341 MIPI-DBI command set on top of the generic
 * `esp_lcd_panel_io` SPI transport (esp_lcd_new_panel_io_spi()).
 *
 * Supported:
 *   - 240x320 (default) and 240x400 via vendor config
 *   - RGB565 (16 bpp) and RGB666 (18 bpp -> 3 bytes/pixel)
 *   - MADCTL rotation (0/90/180/270) through mirror() + swap_xy()
 *   - Display on/off, sleep, invert, brightness (0-255)
 *
 * Reference: ILI9341 datasheet v1.11
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Vendor specific configuration for the ILI9341 panel
 *
 * Pass this as `esp_lcd_panel_dev_config_t.vendor_config`.
 */
typedef struct {
    int x_gap;              /*!< Extra gap on x axis (pixels), default 0 */
    int y_gap;              /*!< Extra gap on y axis (pixels), default 0 */
    bool use_rgb666;        /*!< true  -> 18bpp (3 bytes/pixel), false -> 16bpp RGB565 */
    bool madctl_mirror_x;   /*!< Initial horizontal mirror */
    bool madctl_mirror_y;   /*!< Initial vertical mirror */
    bool madctl_swap_xy;    /*!< Initial row/column swap (90/270 degree rotation) */
    bool madctl_bgr;        /*!< true -> BGR color order */
    bool invert_color;      /*!< true -> enable display inversion (INVON) */
    bool use_hw_reset;      /*!< true -> drive the RST pin; false -> use software reset */
    uint8_t gamma;          /*!< Gamma curve index 0..3 (GAMCT), default 0 */
} ili9341_vendor_config_t;

/**
 * @brief Create an LCD panel for the ILI9341 controller
 *
 * @param[in] io                 LCD panel IO handle (SPI)
 * @param[in] panel_dev_config   General panel device configuration
 * @param[out] ret_panel         Returned LCD panel handle
 *
 * @return
 *  - ESP_ERR_INVALID_ARG   if any argument is invalid
 *  - ESP_ERR_NO_MEM        if out of memory
 *  - ESP_OK                on success
 */
esp_err_t esp_lcd_new_panel_ili9341(const esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief Read back the 8-bit display ID (RDID, 0x04)
 *
 * Useful to confirm the controller is really an ILI9341 and not a clone.
 * The 4th byte of the ID identifies the variant:
 *   0x93 -> ILI9341, 0x00 -> ILI9341 (rev B), 0xFF -> generic 240x320
 *
 * @param[in]  panel    LCD panel handle
 * @param[out] id       Buffer receiving at least 4 bytes
 * @param[in]  id_size  Size of the `id` buffer in bytes
 *
 * @return
 *  - ESP_ERR_INVALID_ARG   if arguments are invalid
 *  - ESP_ERR_NOT_SUPPORTED if the transport cannot receive data
 *  - ESP_OK                on success
 */
esp_err_t ili9341_read_display_id(esp_lcd_panel_handle_t panel, uint8_t *id, size_t id_size);

#ifdef __cplusplus
}
#endif
