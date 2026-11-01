/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RGB565 drawing layer implementation.
 *
 * The frame buffer holds native-endian uint16_t pixels. The ILI9341 expects
 * the high byte first on the wire, so `tft_flush*()` swaps the bytes while
 * copying into the DMA buffer (DMA needs a separate, cache-aligned buffer).
 *
 * NOTE ON MEMORY: this board is an ESP32 rev 1.1 without PSRAM, so a
 * 240x320x2 = 153600 byte frame buffer cannot be served as one contiguous
 * malloc() block (the largest DRAM heap region is only ~154 KiB and the Wi-Fi
 * driver needs a large slice of it). The frame buffer is therefore split into
 * several smaller chunks that are allocated independently. The pixel API is
 * unchanged - callers still address the frame buffer as a flat 2D array.
 */
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_ops.h"
#include "tft_disp.h"
#include "font6x8.h"
#include "font11x16.h"

static const char *TAG = "tft_disp";

/* Pixels per frame-buffer chunk. 8192 px == 16 KiB per allocation. */
#define FB_CHUNK_PIXELS 8192

/* Map a character code to its glyph bitmap in the requested font. */
static const uint8_t *font_small_data(uint8_t code, uint8_t *w, uint8_t *h, uint8_t *bpr)
{
    if (code < font6x8_FIRST || code > font6x8_LAST) {
        code = '?';
    }
    *w = font6x8_W; *h = font6x8_H; *bpr = font6x8_BPR;
    return font6x8[code - font6x8_FIRST];
}

static const uint8_t *font_medium_data(uint8_t code, uint8_t *w, uint8_t *h, uint8_t *bpr)
{
    if (code < font11x16_FIRST || code > font11x16_LAST) {
        code = '?';
    }
    *w = font11x16_W; *h = font11x16_H; *bpr = font11x16_BPR;
    return font11x16[code - font11x16_FIRST];
}

struct tft_disp {
    esp_lcd_panel_handle_t panel;
    int width;
    int height;
    uint16_t **chunks;    /* array of frame-buffer chunk pointers */
    int nchunks;          /* number of chunks */
    uint8_t *dma_buf;     /* byte-swapped staging buffer for DMA */
    size_t dma_buf_size;
};

/* ------------------------------------------------------------------ */
/* Chunked frame buffer access                                        */
/* ------------------------------------------------------------------ */

/* Resolve a flat pixel index to its address. Returns NULL when out of range. */
static inline uint16_t *fb_at(tft_disp_t *disp, size_t idx)
{
    if (idx >= (size_t)disp->width * disp->height) {
        return NULL;
    }
    int chunk = (int)(idx / FB_CHUNK_PIXELS);
    size_t off = idx % FB_CHUNK_PIXELS;
    return &disp->chunks[chunk][off];
}

static inline void fb_set(tft_disp_t *disp, int x, int y, uint16_t c)
{
    if (x < 0 || x >= disp->width || y < 0 || y >= disp->height) {
        return;
    }
    uint16_t *p = fb_at(disp, (size_t)y * disp->width + x);
    if (p) {
        *p = c;
    }
}

int tft_disp_width(const tft_disp_t *disp) { return disp ? disp->width : 0; }
int tft_disp_height(const tft_disp_t *disp) { return disp ? disp->height : 0; }
int tft_disp_bpp(const tft_disp_t *disp) { return 2; }

esp_err_t tft_disp_create(esp_lcd_panel_handle_t panel, int width, int height, tft_disp_t **ret_disp)
{
    if (!panel || width <= 0 || height <= 0 || !ret_disp) {
        ESP_LOGE(TAG, "invalid argument");
        return ESP_ERR_INVALID_ARG;
    }
    tft_disp_t *disp = calloc(1, sizeof(tft_disp_t));
    if (!disp) {
        ESP_LOGE(TAG, "no mem for disp");
        return ESP_ERR_NO_MEM;
    }

    size_t total_px = (size_t)width * height;
    int nchunks = (int)((total_px + FB_CHUNK_PIXELS - 1) / FB_CHUNK_PIXELS);

    disp->chunks = calloc(nchunks, sizeof(uint16_t *));
    if (!disp->chunks) {
        ESP_LOGE(TAG, "no mem for chunk table");
        free(disp);
        return ESP_ERR_NO_MEM;
    }
    disp->nchunks = nchunks;

    bool psram = false;
    for (int i = 0; i < nchunks; i++) {
        size_t px = total_px - (size_t)i * FB_CHUNK_PIXELS;
        if (px > FB_CHUNK_PIXELS) {
            px = FB_CHUNK_PIXELS;
        }
        size_t bytes = px * 2;
        disp->chunks[i] = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (disp->chunks[i]) {
            psram = true;
            continue;
        }
        disp->chunks[i] = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!disp->chunks[i]) {
            ESP_LOGE(TAG, "no mem for frame buffer chunk %d (%u bytes)", i, (unsigned)bytes);
            for (int j = 0; j < i; j++) {
                free(disp->chunks[j]);
            }
            free(disp->chunks);
            free(disp);
            return ESP_ERR_NO_MEM;
        }
    }
    if (!psram) {
        ESP_LOGW(TAG, "PSRAM unavailable, frame buffer in internal RAM");
    }

    /* DMA staging buffer: a few rows at a time, cache aligned */
    size_t row_bytes = (size_t)width * 2;
    int rows_per_chunk = 8;
    if (rows_per_chunk > height) {
        rows_per_chunk = height;
    }
    size_t chunk = row_bytes * rows_per_chunk;
    disp->dma_buf_size = chunk;
    disp->dma_buf = heap_caps_malloc(chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!disp->dma_buf) {
        ESP_LOGE(TAG, "no mem for dma buffer");
        tft_disp_del(disp);
        return ESP_ERR_NO_MEM;
    }

    disp->panel = panel;
    disp->width = width;
    disp->height = height;
    for (int i = 0; i < nchunks; i++) {
        size_t px = total_px - (size_t)i * FB_CHUNK_PIXELS;
        if (px > FB_CHUNK_PIXELS) {
            px = FB_CHUNK_PIXELS;
        }
        memset(disp->chunks[i], 0, px * 2);
    }
    ESP_LOGI(TAG, "disp created: %dx%d, %u bytes in %d chunks, dma chunk=%u bytes",
             width, height, (unsigned)(total_px * 2), nchunks, (unsigned)chunk);

    *ret_disp = disp;
    return ESP_OK;
}

esp_err_t tft_disp_del(tft_disp_t *disp)
{
    if (!disp) {
        return ESP_ERR_INVALID_ARG;
    }
    if (disp->chunks) {
        for (int i = 0; i < disp->nchunks; i++) {
            free(disp->chunks[i]);
        }
        free(disp->chunks);
    }
    free(disp->dma_buf);
    free(disp);
    return ESP_OK;
}

void tft_clear(tft_disp_t *disp, uint16_t color)
{
    if (!disp) {
        return;
    }
    size_t total_px = (size_t)disp->width * disp->height;
    for (int i = 0; i < disp->nchunks; i++) {
        size_t px = total_px - (size_t)i * FB_CHUNK_PIXELS;
        if (px > FB_CHUNK_PIXELS) {
            px = FB_CHUNK_PIXELS;
        }
        uint16_t *c = disp->chunks[i];
        for (size_t k = 0; k < px; k++) {
            c[k] = color;
        }
    }
}

void tft_fill_rect(tft_disp_t *disp, int x, int y, int w, int h, uint16_t color)
{
    if (!disp || w <= 0 || h <= 0) {
        return;
    }
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = x + w > disp->width ? disp->width : x + w;
    int y2 = y + h > disp->height ? disp->height : y + h;
    for (int yy = y1; yy < y2; yy++) {
        for (int xx = x1; xx < x2; xx++) {
            fb_set(disp, xx, yy, color);
        }
    }
}

void tft_draw_hline(tft_disp_t *disp, int x, int y, int len, uint16_t color, int thickness)
{
    if (thickness <= 0) {
        thickness = 1;
    }
    for (int t = 0; t < thickness; t++) {
        tft_fill_rect(disp, x, y + t, len, 1, color);
    }
}

void tft_draw_vline(tft_disp_t *disp, int x, int y, int len, uint16_t color, int thickness)
{
    if (thickness <= 0) {
        thickness = 1;
    }
    for (int t = 0; t < thickness; t++) {
        tft_fill_rect(disp, x + t, y, 1, len, color);
    }
}

void tft_draw_rect(tft_disp_t *disp, int x, int y, int w, int h, uint16_t color, int thickness)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    if (thickness <= 0) {
        thickness = 1;
    }
    tft_fill_rect(disp, x, y, w, thickness, color);                       /* top */
    tft_fill_rect(disp, x, y + h - thickness, w, thickness, color);        /* bottom */
    tft_fill_rect(disp, x, y, thickness, h, color);                       /* left */
    tft_fill_rect(disp, x + w - thickness, y, thickness, h, color);       /* right */
}

void tft_draw_line(tft_disp_t *disp, int x0, int y0, int x1, int y1, uint16_t color, int thickness)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        if (thickness <= 1) {
            fb_set(disp, x0, y0, color);
        } else {
            int half = thickness / 2;
            tft_fill_rect(disp, x0 - half, y0 - half, thickness, thickness, color);
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static const uint8_t *glyph_for(tft_font_t font, char ch, uint8_t *w, uint8_t *h, uint8_t *bpr)
{
    uint8_t code = (uint8_t)ch;
    if (font == TFT_FONT_SMALL) {
        return font_small_data(code, w, h, bpr);
    }
    return font_medium_data(code, w, h, bpr);
}

int tft_text_width(const char *text, tft_font_t font, int scale)
{
    if (!text) {
        return 0;
    }
    if (scale < 1) {
        scale = 1;
    }
    uint8_t w = 0, h = 0, bpr = 0;
    glyph_for(font, 'M', &w, &h, &bpr);
    return (int)strlen(text) * w * scale;
}

static void draw_glyph(tft_disp_t *disp, int x, int y, char ch, uint16_t color, tft_font_t font, int scale)
{
    uint8_t w = 0, h = 0, bpr = 0;
    const uint8_t *data = glyph_for(font, ch, &w, &h, &bpr);
    if (!data) {
        return;
    }
    for (uint8_t gy = 0; gy < h; gy++) {
        for (uint8_t gx = 0; gx < w; gx++) {
            uint8_t byte = data[(size_t)gy * bpr + gx / 8];
            if (!(byte & (1 << (7 - (gx % 8))))) {
                continue;
            }
            if (scale == 1) {
                fb_set(disp, x + gx, y + gy, color);
            } else {
                tft_fill_rect(disp, x + gx * scale, y + gy * scale, scale, scale, color);
            }
        }
    }
}

int tft_draw_text(tft_disp_t *disp, int x, int y, const char *text, uint16_t color,
                  tft_font_t font, int scale)
{
    if (!disp || !text) {
        return 0;
    }
    if (scale < 1) {
        scale = 1;
    }
    uint8_t w = 0, h = 0, bpr = 0;
    glyph_for(font, 'M', &w, &h, &bpr);
    int cursor = x;
    for (const char *p = text; *p; p++) {
        draw_glyph(disp, cursor, y, *p, color, font, scale);
        cursor += w * scale;
    }
    return cursor - x;
}

int tft_draw_text_bg(tft_disp_t *disp, int x, int y, const char *text, uint16_t color,
                     uint16_t bg, tft_font_t font, int scale)
{
    if (!disp || !text) {
        return 0;
    }
    if (scale < 1) {
        scale = 1;
    }
    uint8_t w = 0, h = 0, bpr = 0;
    glyph_for(font, 'M', &w, &h, &bpr);
    int text_w = (int)strlen(text) * w * scale;
    tft_fill_rect(disp, x - 2, y - 1, text_w + 4, h * scale + 2, bg);
    return tft_draw_text(disp, x, y, text, color, font, scale);
}

void tft_draw_text_center(tft_disp_t *disp, int x, int y, int w, int h, const char *text,
                          uint16_t color, tft_font_t font, int scale)
{
    if (!disp || !text) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }
    uint8_t gw = 0, gh = 0, bpr = 0;
    glyph_for(font, 'M', &gw, &gh, &bpr);
    int text_w = (int)strlen(text) * gw * scale;
    int text_h = gh * scale;
    int tx = x + (w - text_w) / 2;
    int ty = y + (h - text_h) / 2;
    if (tx < x) {
        tx = x;
    }
    if (ty < y) {
        ty = y;
    }
    tft_draw_text(disp, tx, ty, text, color, font, scale);
}

void tft_draw_chart(tft_disp_t *disp, const int *values, int count,
                    int x0, int y0, int w, int h, uint16_t color, bool fill)
{
    if (!disp || !values || count < 2 || w <= 1 || h <= 1) {
        return;
    }
    int step_x = w / (count - 1);
    if (step_x < 1) {
        step_x = 1;
    }

    if (fill) {
        /* find the minimum to use as the baseline */
        int vmin = values[0];
        for (int i = 1; i < count; i++) {
            if (values[i] < vmin) {
                vmin = values[i];
            }
        }
        int prev_x = x0;
        int prev_y = y0 + h - 1;
        for (int i = 0; i < count; i++) {
            int px = x0 + i * step_x;
            if (px >= x0 + w) {
                px = x0 + w - 1;
            }
            int py = y0 + h - 1 - (values[i] - vmin);
            if (py < y0) {
                py = y0;
            }
            if (py > y0 + h - 1) {
                py = y0 + h - 1;
            }
            if (i == 0) {
                prev_x = px;
                prev_y = py;
                continue;
            }
            /* fill the quad between the two points and the baseline */
            int min_y = py < prev_y ? py : prev_y;
            for (int yy = min_y; yy <= y0 + h - 1; yy++) {
                tft_draw_line(disp, prev_x, yy, px, yy, color, 1);
            }
            prev_x = px;
            prev_y = py;
        }
    }

    int prev_x = x0;
    int prev_y = y0 + h - 1;
    for (int i = 0; i < count; i++) {
        int px = x0 + i * step_x;
        if (px >= x0 + w) {
            px = x0 + w - 1;
        }
        int py = y0 + h - 1 - (values[i]);
        if (py < y0) {
            py = y0;
        }
        if (py > y0 + h - 1) {
            py = y0 + h - 1;
        }
        if (i > 0) {
            tft_draw_line(disp, prev_x, prev_y, px, py, color, 2);
        }
        prev_x = px;
        prev_y = py;
    }
}

esp_err_t tft_flush(tft_disp_t *disp)
{
    return tft_flush_region(disp, 0, 0, disp ? disp->width : 0, disp ? disp->height : 0);
}

esp_err_t tft_flush_region(tft_disp_t *disp, int x, int y, int w, int h)
{
    if (!disp) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x < 0) { x = 0; }
    if (y < 0) { y = 0; }
    if (x + w > disp->width) { w = disp->width - x; }
    if (y + h > disp->height) { h = disp->height - y; }
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    size_t row_bytes = (size_t)w * 2;
    size_t rows_per_chunk = disp->dma_buf_size / row_bytes;
    if (rows_per_chunk == 0) {
        rows_per_chunk = 1;
    }

    for (int row = 0; row < h; ) {
        int rows = (int)((h - row) < (int)rows_per_chunk ? (h - row) : rows_per_chunk);
        /* Copy with byte swap: panel wants MSB first, CPU stores LE.
         * The source may span frame-buffer chunks, so copy pixel by pixel
         * while resolving the chunk for each pixel. */
        uint8_t *dst = disp->dma_buf;
        for (int r = 0; r < rows; r++) {
            size_t base = (size_t)(y + row + r) * disp->width + x;
            for (int c = 0; c < w; c++) {
                uint16_t px = *fb_at(disp, base + c);
                dst[0] = (uint8_t)(px >> 8);
                dst[1] = (uint8_t)(px & 0xFF);
                dst += 2;
            }
        }
        esp_err_t ret = esp_lcd_panel_draw_bitmap(disp->panel, x, y + row, x + w, y + row + rows,
                                                   disp->dma_buf);
        ESP_RETURN_ON_ERROR(ret, TAG, "draw bitmap failed");
        row += rows;
    }
    return ESP_OK;
}
