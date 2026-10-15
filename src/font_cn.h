#ifndef FONT_CN_H_
#define FONT_CN_H_

#include <stdint.h>

#define FONT_CN_WIDTH 32U
#define FONT_CN_HEIGHT 32U
#define FONT_CN_ROW_BYTES (FONT_CN_WIDTH / 8U)
#define FONT_CN_GLYPH_BYTES (FONT_CN_ROW_BYTES * FONT_CN_HEIGHT)

/**
 * Look up one 32 x 32 monochrome Chinese glyph by Unicode code point.
 * A set bit is a foreground (black) pixel.
 *
 * @return Pointer to FONT_CN_GLYPH_BYTES bytes, or NULL when unsupported.
 */
const uint8_t *font_cn_find(uint32_t codepoint);

#endif
