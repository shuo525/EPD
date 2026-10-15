#ifndef BLE_EPD_H_
#define BLE_EPD_H_

#include <stddef.h>
#include <stdint.h>

#define BLE_EPD_FRAME_BYTES 48000U
#define BLE_EPD_FRAME_CHUNK_MAX 244U

enum ble_epd_frame_command {
	BLE_EPD_FRAME_COMMAND_KW,
	BLE_EPD_FRAME_COMMAND_RED,
	BLE_EPD_FRAME_COMMAND_SHOW,
	BLE_EPD_FRAME_COMMAND_ABORT,
};

/**
 * @brief Initialize the EPD BLE peripheral and start advertising.
 *
 * @return 0 on success, otherwise a negative error code.
 */
int ble_epd_init(void);

/**
 * @brief Take one pending text command received over BLE.
 *
 * The returned text is a NUL-terminated UTF-8 string without the TEXT: prefix.
 *
 * @return 0 when text was copied, -EAGAIN when no text is pending.
 */
int ble_epd_take_text(uint8_t *buffer, size_t capacity, size_t *length);

/** Take one queued FRAME:* control command. */
int ble_epd_take_frame_command(enum ble_epd_frame_command *command);

/** Enable the frame-data characteristic for exactly one 48,000-byte plane. */
void ble_epd_frame_start_data(void);

/** Disable frame writes and discard data that has not been consumed. */
void ble_epd_frame_stop_data(void);

/** Take one reliable data packet received on the frame-data characteristic. */
int ble_epd_take_frame_data(uint8_t *buffer, size_t capacity, size_t *length);

/** Update the readable/notifiable Status characteristic. */
void ble_epd_set_status(const char *text);

#endif
