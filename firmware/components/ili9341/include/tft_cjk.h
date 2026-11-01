/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CJK (Chinese) bitmap font support.
 *
 * The glyph data lives in flash (embedded via EMBED_FILES) so it does not
 * consume the scarce internal RAM - this ESP32 has no PSRAM and the
 * dashboard frame buffer already takes 150 kB.
 *
 * The font covers GB2312 level 1 (3755 most common hanzi), CJK punctuation
 * and ASCII, all rendered into a 16x16 cell.
 *
 * ASCII characters are drawn in a 9x13 box at the bottom-left of the
 * 16x16 cell and advance 9 px, so they share the baseline with the
 * Chinese glyphs and mixed text lines up naturally.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TFT_CJK_CELL_W   16
#define TFT_CJK_CELL_H   16
#define TFT_CJK_BPR      2
#define TFT_CJK_HALF_W   9    /* ASCII advance width (9x13 cell) */

/**
 * @brief Look up a Unicode code point in the CJK font
 *
 * @param[in]  cp       Unicode code point
 * @param[out] glyph    Pointer to the 16x16 bitmap (TFT_CJK_BPR * 16 bytes),
 *                      or NULL when the character is not in the font
 *
 * @return true when the glyph was found
 */
bool tft_cjk_glyph(uint32_t cp, const uint8_t **glyph);

/**
 * @brief Decode one UTF-8 code point
 *
 * @param[in]  s      String to read from
 * @param[out] cp     Decoded code point
 * @param[out] len    Number of bytes consumed (1..4)
 *
 * @return true on success, false on malformed input
 */
bool tft_utf8_decode(const char *s, uint32_t *cp, int *len);

/**
 * @brief Width in pixels of a UTF-8 string in the CJK font
 *
 * ASCII characters count as TFT_CJK_HALF_W, everything else as a full cell.
 */
int tft_cjk_text_width(const char *text);

#ifdef __cplusplus
}
#endif
