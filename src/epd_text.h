#ifndef EPD_TEXT_H_
#define EPD_TEXT_H_

#include <stddef.h>
#include <stdint.h>

#define EPD_TEXT_MAX_BYTES 96
#define EPD_TEXT_ROW_BYTES 100

/**
 * @brief Render one row of centered UTF-8 text on a white background.
 *
 * ASCII and the Chinese glyphs in font_cn.c are supported. K/W and RED are
 * filled using the project's CDI=0x12 encoding.
 */
void epd_text_render_row(const uint8_t *text, size_t length, size_t row,
				 uint8_t kw[EPD_TEXT_ROW_BYTES],
				 uint8_t red[EPD_TEXT_ROW_BYTES]);

#endif
