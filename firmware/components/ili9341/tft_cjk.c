/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CJK bitmap font support - see tft_cjk.h.
 */
#include "tft_cjk.h"

#include <string.h>

#include "font_cjk16_cp.h"

/* The glyph table is embedded in flash by the build system (EMBED_FILES). */
extern const uint8_t font_cjk16_bin_start[] asm("_binary_font_cjk16_bin_start");
extern const uint8_t font_cjk16_bin_end[]   asm("_binary_font_cjk16_bin_end");

#define FONT_CJK_MAGIC   0x4331

struct font_header {
    uint16_t magic;
    uint16_t version;
    uint16_t glyph_w;
    uint16_t glyph_h;
    uint16_t bytes_per_glyph;
    uint16_t first_cp;
    uint16_t count;
    uint16_t reserved;
};

static const struct font_header *font_header_get(void)
{
    return (const struct font_header *)font_cjk16_bin_start;
}

bool tft_cjk_glyph(uint32_t cp, const uint8_t **glyph)
{
    if (glyph) {
        *glyph = NULL;
    }
    const struct font_header *hdr = font_header_get();
    if (hdr->magic != FONT_CJK_MAGIC) {
        return false;
    }

    /* The code point table is sorted, so a binary search finds the glyph. */
    uint16_t lo = 0;
    uint16_t hi = (uint16_t)(hdr->count - 1);
    while (lo <= hi) {
        uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
        uint16_t v = font_cjk16_cps[mid];
        if (v == cp) {
            /* bytes_per_glyph in the header is the BYTES PER ROW (BPR);
             * a glyph occupies glyph_h rows, so the stride is
             * glyph_h * bytes_per_row. */
            size_t stride = (size_t)hdr->glyph_h * hdr->bytes_per_glyph;
            size_t offset = sizeof(struct font_header) + (size_t)mid * stride;
            if (glyph) {
                *glyph = font_cjk16_bin_start + offset;
            }
            return true;
        }
        if (v < cp) {
            lo = (uint16_t)(mid + 1);
        } else {
            if (mid == 0) {
                break;
            }
            hi = (uint16_t)(mid - 1);
        }
    }
    return false;
}

bool tft_utf8_decode(const char *s, uint32_t *cp, int *len)
{
    if (!s || !*s) {
        return false;
    }
    const uint8_t *u = (const uint8_t *)s;
    uint8_t b0 = u[0];

    if (b0 < 0x80) {
        if (cp)  *cp = b0;
        if (len) *len = 1;
        return true;
    }
    if ((b0 & 0xE0) == 0xC0) {
        if ((u[1] & 0xC0) != 0x80) return false;
        if (cp)  *cp = ((uint32_t)(b0 & 0x1F) << 6) | (u[1] & 0x3F);
        if (len) *len = 2;
        return true;
    }
    if ((b0 & 0xF0) == 0xE0) {
        if ((u[1] & 0xC0) != 0x80 || (u[2] & 0xC0) != 0x80) return false;
        if (cp)  *cp = ((uint32_t)(b0 & 0x0F) << 12) |
                      ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        if (len) *len = 3;
        return true;
    }
    if ((b0 & 0xF8) == 0xF0) {
        if ((u[1] & 0xC0) != 0x80 || (u[2] & 0xC0) != 0x80 ||
            (u[3] & 0xC0) != 0x80) return false;
        if (cp)  *cp = ((uint32_t)(b0 & 0x07) << 18) |
                      ((uint32_t)(u[1] & 0x3F) << 12) |
                      ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        if (len) *len = 4;
        return true;
    }
    return false;
}

int tft_cjk_text_width(const char *text)
{
    if (!text) {
        return 0;
    }
    int width = 0;
    while (*text) {
        uint32_t cp = 0;
        int len = 0;
        if (!tft_utf8_decode(text, &cp, &len)) {
            /* malformed byte: skip it, treat as one narrow blank */
            text++;
            width += TFT_CJK_HALF_W;
            continue;
        }
        width += (cp < 0x80) ? TFT_CJK_HALF_W : TFT_CJK_CELL_W;
        text += len;
    }
    return width;
}
