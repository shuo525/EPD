#ifndef EPD_FRAME_RX_H_
#define EPD_FRAME_RX_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct epd_frame_rx_ops {
	int (*start_plane)(uint8_t command);
	int (*write_data)(const uint8_t *data, size_t length);
	int (*refresh)(void);
};

void epd_frame_rx_init(const struct epd_frame_rx_ops *ops);
void epd_frame_rx_process(void);
bool epd_frame_rx_is_busy(void);

#endif
