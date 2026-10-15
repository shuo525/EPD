#ifndef IMAGE_ASSET_H_
#define IMAGE_ASSET_H_

#include <stdint.h>

#define EPD_IMAGE_WIDTH 800
#define EPD_IMAGE_HEIGHT 480
#define EPD_IMAGE_FRAME_BYTES 48000

extern const uint8_t epd_image_kw[EPD_IMAGE_FRAME_BYTES];
extern const uint8_t epd_image_red[EPD_IMAGE_FRAME_BYTES];

#endif
