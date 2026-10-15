#include <ctype.h>
#include <string.h>

#include "epd_text.h"
#include "font_cn.h"

#define EPD_WIDTH 800 //EPD长度像素
#define EPD_HEIGHT 480//EPD宽度像素
#define ASCII_FONT_WIDTH 5  //5×7字体
#define ASCII_FONT_HEIGHT 7 //5×7字体
#define ASCII_FONT_SCALE 4  //EPD字体缩放比例
#define GLYPH_SPACING    4	//
#define ASCII_GLYPH_WIDTH (ASCII_FONT_WIDTH * ASCII_FONT_SCALE)
#define ASCII_GLYPH_HEIGHT (ASCII_FONT_HEIGHT * ASCII_FONT_SCALE)
#define ASCII_GLYPH_ADVANCE (ASCII_GLYPH_WIDTH + GLYPH_SPACING)
#define CN_GLYPH_ADVANCE (FONT_CN_WIDTH + GLYPH_SPACING)
#define TEXT_HEIGHT FONT_CN_HEIGHT
#define TEXT_Y ((EPD_HEIGHT - TEXT_HEIGHT) / 2)
#define ASCII_TEXT_Y (TEXT_Y + (TEXT_HEIGHT - ASCII_GLYPH_HEIGHT) / 2)

/* Columns are stored top-to-bottom in bits 0..6. */
static const uint8_t font_digits[10][ASCII_FONT_WIDTH] = {
	{0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
	{0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
	{0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
	{0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
	{0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
};

static const uint8_t font_upper[26][ASCII_FONT_WIDTH] = {
	{0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
	{0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C},
	{0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
	{0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
	{0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01},
	{0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
	{0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
	{0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
	{0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
	{0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
	{0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
	{0x7F, 0x20, 0x18, 0x20, 0x7F}, {0x63, 0x14, 0x08, 0x14, 0x63},
	{0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
};

static const uint8_t font_question[ASCII_FONT_WIDTH] = {0x02, 0x01, 0x51, 0x09, 0x06};

static const uint8_t *ascii_glyph_for(uint8_t ch)
{
	static const uint8_t space[ASCII_FONT_WIDTH] = {0, 0, 0, 0, 0};

	if (ch >= 'a' && ch <= 'z') {
		ch = (uint8_t)toupper(ch);
	}
	if (ch >= '0' && ch <= '9') {
		return font_digits[ch - '0'];
	}
	if (ch >= 'A' && ch <= 'Z') {
		return font_upper[ch - 'A'];
	}
	if (ch == ' ') {
		return space;
	}
	return font_question;
}

static size_t utf8_decode(const uint8_t *text, size_t length, uint32_t *codepoint)
{
	uint8_t first;

	if (length == 0) {
		return 0;
	}

	first = text[0];
	if (first < 0x80) {
		*codepoint = first;
		return 1;
	}
	if (first >= 0xC2 && first <= 0xDF && length >= 2 &&
	    (text[1] & 0xC0) == 0x80) {
		*codepoint = ((uint32_t)(first & 0x1F) << 6) |
			    (uint32_t)(text[1] & 0x3F);
		return 2;
	}
	if (first >= 0xE0 && first <= 0xEF && length >= 3 &&
	    (text[1] & 0xC0) == 0x80 && (text[2] & 0xC0) == 0x80) {
		uint32_t value = ((uint32_t)(first & 0x0F) << 12) |
				 ((uint32_t)(text[1] & 0x3F) << 6) |
				 (uint32_t)(text[2] & 0x3F);

		if (value >= 0x800 && !(value >= 0xD800 && value <= 0xDFFF)) {
			*codepoint = value;
			return 3;
		}
	}
	if (first >= 0xF0 && first <= 0xF4 && length >= 4 &&
	    (text[1] & 0xC0) == 0x80 && (text[2] & 0xC0) == 0x80 &&
	    (text[3] & 0xC0) == 0x80) {
		uint32_t value = ((uint32_t)(first & 0x07) << 18) |
				 ((uint32_t)(text[1] & 0x3F) << 12) |
				 ((uint32_t)(text[2] & 0x3F) << 6) |
				 (uint32_t)(text[3] & 0x3F);

		if (value >= 0x10000 && value <= 0x10FFFF) {
			*codepoint = value;
			return 4;
		}
	}

	*codepoint = '?';
	return 1;
}

static size_t glyph_advance(uint32_t codepoint)
{
	return font_cn_find(codepoint) != NULL ? CN_GLYPH_ADVANCE : ASCII_GLYPH_ADVANCE;
}

static size_t text_pixel_width(const uint8_t *text, size_t length)
{
	size_t offset = 0;
	size_t width = 0;

	while (offset < length) {
		uint32_t codepoint;
		size_t consumed = utf8_decode(&text[offset], length - offset, &codepoint);

		if (consumed == 0) {
			break;
		}
		width += glyph_advance(codepoint);
		offset += consumed;
	}

	return width >= GLYPH_SPACING ? width - GLYPH_SPACING : 0;
}

static void set_black_pixel(uint8_t kw[EPD_TEXT_ROW_BYTES], size_t x)
{
	if (x < EPD_WIDTH) {
		kw[x / 8] |= (uint8_t)(1U << (7 - (x % 8)));
	}
}

void epd_text_render_row(const uint8_t *text, size_t length, size_t row,
				 uint8_t kw[EPD_TEXT_ROW_BYTES],
				 uint8_t red[EPD_TEXT_ROW_BYTES])
{
	size_t width = text_pixel_width(text, length);
	size_t x0 = width < EPD_WIDTH ? (EPD_WIDTH - width) / 2 : 0;
	size_t offset = 0;
	size_t glyph_x = x0;

	memset(kw, 0x00, EPD_TEXT_ROW_BYTES);
	memset(red, 0xFF, EPD_TEXT_ROW_BYTES);
	if (row < TEXT_Y || row >= TEXT_Y + TEXT_HEIGHT) {
		return;
	}

	while (offset < length) {
		uint32_t codepoint;
		size_t consumed = utf8_decode(&text[offset], length - offset, &codepoint);
		const uint8_t *cn_glyph = font_cn_find(codepoint);

		if (consumed == 0) {
			break;
		}

		if (cn_glyph != NULL) {
			size_t glyph_row = row - TEXT_Y;

			for (size_t col = 0; col < FONT_CN_WIDTH; col++) {
				uint8_t mask = (uint8_t)(1U << (7 - (col % 8)));
				if ((cn_glyph[glyph_row * FONT_CN_ROW_BYTES + col / 8] & mask) != 0) {
					set_black_pixel(kw, glyph_x + col);
				}
			}
			glyph_x += CN_GLYPH_ADVANCE;
		} else {
			uint8_t ascii = codepoint <= 0x7F ? (uint8_t)codepoint : '?';
			const uint8_t *glyph = ascii_glyph_for(ascii);

			if (row >= ASCII_TEXT_Y && row < ASCII_TEXT_Y + ASCII_GLYPH_HEIGHT) {
				size_t glyph_row = (row - ASCII_TEXT_Y) / ASCII_FONT_SCALE;

				for (size_t col = 0; col < ASCII_FONT_WIDTH; col++) {
					if ((glyph[col] & (1U << glyph_row)) == 0) {
						continue;
					}
					for (size_t scale_x = 0; scale_x < ASCII_FONT_SCALE; scale_x++) {
						set_black_pixel(kw, glyph_x + col * ASCII_FONT_SCALE + scale_x);
					}
				}
			}
			glyph_x += ASCII_GLYPH_ADVANCE;
		}
		offset += consumed;
	}
}
