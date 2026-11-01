/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lightweight drawing layer on top of an esp_lcd panel.
 *
 * The layer keeps a full-screen RGB565 frame buffer in PSRAM (or internal
 * RAM as fallback) and flushes dirty regions to the panel. This keeps the
 * SPI bus traffic low and makes flicker-free animation possible.
 *
 * Coordinates are always given in the *native* panel orientation
 * (width x height as configured at creation time).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- RGB565 colors (MSB first: RRRRRGGG GGGBBBBB) ---- */
#define TFT_COLOR_BLACK      0x0000
#define TFT_COLOR_WHITE      0xFFFF
#define TFT_COLOR_RED        0xF800
#define TFT_COLOR_GREEN      0x07E0
#define TFT_COLOR_BLUE       0x001F
#define TFT_COLOR_YELLOW     0xFFE0
#define TFT_COLOR_CYAN       0x07FF
#define TFT_COLOR_MAGENTA    0xF81F
#define TFT_COLOR_ORANGE     0xFD20
#define TFT_COLOR_GRAY       0x8410
#define TFT_COLOR_DARKGRAY   0x4208
#define TFT_COLOR_LIGHTGRAY  0xC618

typedef enum {
    TFT_FONT_SMALL = 0,   /*!< 6x8   */
    TFT_FONT_MEDIUM,      /*!< 11x16 */
    TFT_FONT_LARGE,       /*!< 9x13  - one step below MEDIUM, more legible
                               than SMALL on a 2.8" panel */
    TFT_FONT_COUNT,
} tft_font_t;

/*
 * The CJK font is a separate, variable-width font: Chinese characters are
 * 16 px wide, ASCII is 8 px. It cannot be mixed into tft_font_t because the
 * advance width depends on the character, so it gets its own API.
 */

typedef struct tft_disp tft_disp_t;

/**
 * @brief Create a drawing surface bound to an LCD panel
 *
 * @param[in] panel      LCD panel handle (ILI9341)
 * @param[in] width      Frame buffer width in pixels
 * @param[in] height     Frame buffer height in pixels
 * @param[out] ret_disp  Returned drawing surface
 *
 * @return ESP_OK on success
 */
esp_err_t tft_disp_create(esp_lcd_panel_handle_t panel, int width, int height,
                           tft_disp_t **ret_disp);

/**
 * @brief Destroy the drawing surface and release its frame buffer
 */
esp_err_t tft_disp_del(tft_disp_t *disp);

/** @brief Frame buffer width in pixels */
int tft_disp_width(const tft_disp_t *disp);
/** @brief Frame buffer height in pixels */
int tft_disp_height(const tft_disp_t *disp);
/** @brief Bytes per pixel of the frame buffer (2 for RGB565) */
int tft_disp_bpp(const tft_disp_t *disp);

/** @brief Fill the whole frame buffer with a color (does not flush) */
void tft_clear(tft_disp_t *disp, uint16_t color);

/** @brief Fill a rectangle (clipped to the frame buffer) */
void tft_fill_rect(tft_disp_t *disp, int x, int y, int w, int h, uint16_t color);

/** @brief Draw a rectangle outline, `thickness` pixels wide */
void tft_draw_rect(tft_disp_t *disp, int x, int y, int w, int h, uint16_t color, int thickness);

/** @brief Draw a horizontal line */
void tft_draw_hline(tft_disp_t *disp, int x, int y, int len, uint16_t color, int thickness);
/** @brief Draw a vertical line */
void tft_draw_vline(tft_disp_t *disp, int x, int y, int len, uint16_t color, int thickness);

/** @brief Draw a line between two points (Bresenham) */
void tft_draw_line(tft_disp_t *disp, int x0, int y0, int x1, int y1, uint16_t color, int thickness);

/**
 * @brief Draw text at (x, y) with the top-left corner of the glyph box
 *
 * @param[in] scale  Integer magnification factor (1 = native size)
 * @return Width in pixels the string would occupy
 */
int tft_draw_text(tft_disp_t *disp, int x, int y, const char *text, uint16_t color,
                  tft_font_t font, int scale);

/** @brief Draw text with a background box behind it */
int tft_draw_text_bg(tft_disp_t *disp, int x, int y, const char *text, uint16_t color,
                     uint16_t bg, tft_font_t font, int scale);

/** @brief Draw text centered horizontally inside a rectangle */
void tft_draw_text_center(tft_disp_t *disp, int x, int y, int w, int h, const char *text,
                          uint16_t color, tft_font_t font, int scale);

/** @brief Width in pixels of a string in the given font/scale */
int tft_text_width(const char *text, tft_font_t font, int scale);

/**
 * @brief Draw UTF-8 text using the CJK font (16 px cells, 8 px ASCII)
 *
 * Chinese characters are drawn full width (16 px), ASCII half width (8 px),
 * so mixed Chinese/English lines up correctly.
 *
 * @param[in] scale  Integer magnification factor (1 = native size)
 * @return Width in pixels the string would occupy
 */
int tft_draw_text_cjk(tft_disp_t *disp, int x, int y, const char *text, uint16_t color,
                      int scale);

/**
 * @brief Draw UTF-8 text centred horizontally inside a rectangle
 */
void tft_draw_text_cjk_center(tft_disp_t *disp, int x, int y, int w, int h,
                              const char *text, uint16_t color, int scale);

/**
 * @brief Width in pixels of a UTF-8 string in the CJK font
 */
int tft_text_width_cjk(const char *text, int scale);

/**
 * @brief Clip a UTF-8 string so that it fits into `max_width` pixels
 *
 * Writes at most `dst_size - 1` bytes plus a NUL. When the text does not
 * fit, the last complete character is replaced by a horizontal ellipsis
 * ("...") if there is room for it.
 *
 * @return true when the whole string fitted
 */
bool tft_clip_text_cjk(char *dst, size_t dst_size, const char *text, int max_width);

/**
 * @brief Draw a polyline chart
 *
 * @param[in] values  Array of y values (already scaled to pixel space)
 * @param[in] count   Number of values
 * @param[in] x0      Left edge of the plot area
 * @param[in] y0      Top edge of the plot area
 * @param[in] w       Width of the plot area
 * @param[in] h       Height of the plot area
 * @param[in] color   Line color
 * @param[in] fill    If true, fill the area under the line
 */
void tft_draw_chart(tft_disp_t *disp, const int *values, int count,
                    int x0, int y0, int w, int h, uint16_t color, bool fill);

/** @brief Push the whole frame buffer to the panel */
esp_err_t tft_flush(tft_disp_t *disp);

/** @brief Push a rectangular region of the frame buffer to the panel */
esp_err_t tft_flush_region(tft_disp_t *disp, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif
