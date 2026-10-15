#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "ble_epd.h"
#include "epd_frame_rx.h"
#include "epd_text.h"
#include "image_asset.h"

#define EPD_NODE DT_NODELABEL(epd)

#define EPD_WIDTH 800   //水墨屏宽度像素
#define EPD_HEIGHT 480  //水墨屏高度像素
#define EPD_ROW_BYTES (EPD_WIDTH / 8) //水墨屏宽度字节
#define EPD_FRAME_BYTES (EPD_ROW_BYTES * EPD_HEIGHT)//水墨屏总字节
#define EPD_BUSY_TIMEOUT_MS 30000  //等待busy引脚由忙转为空闲状态
#define EPD_BUSY_ASSERT_TIMEOUT_MS 1000  //等待busy引脚由空闲状态转为忙

static const struct spi_dt_spec epd_spi =
	SPI_DT_SPEC_GET(EPD_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0);
static const struct gpio_dt_spec epd_dc =
	GPIO_DT_SPEC_GET(EPD_NODE, dc_gpios);
static const struct gpio_dt_spec epd_res =
	GPIO_DT_SPEC_GET(EPD_NODE, res_gpios);
static const struct gpio_dt_spec epd_busy =
	GPIO_DT_SPEC_GET(EPD_NODE, busy_gpios);

/**
 * @brief 得到busy引脚物理电平状态.
 *
 * @return 0 if the pin is low, 1 if the pin is high, or a negative error code.
 */
static int epd_busy_raw(void)
{
	return gpio_pin_get_raw(epd_busy.port, epd_busy.pin);
}
/**
 * @brief 向EPD写入数据.
 *
 * @param data 是否为数据模式, false: 命令模式, true: 数据模式.
 * @param buffer 数据缓冲区.
 * @param length 数据长度.
 * @return 0: 成功, 其他: 错误码.
 */
static int epd_write(bool data, const uint8_t *buffer, size_t length)
{
	// 1.首先分配一个SPI buffer内存空间
	struct spi_buf spi_buf = {
		.buf = (void *)buffer,
		.len = length,
	};
	// 2.然后将这个SPI buffer内存空间放到一个内存数组中
	struct spi_buf_set spi_buf_set = {
		.buffers = &spi_buf,
		.count = 1,
	};
	int ret;
	// 3.设置DC引脚的值
	ret = gpio_pin_set_dt(&epd_dc, data ? 1 : 0);
	if (ret < 0) {
		return ret;
	}
	// 4.调用底层spi写入
	return spi_write_dt(&epd_spi, &spi_buf_set);
}

/**
 * @brief 向EPD发送命令.
 *
 * @param command 要发送的命令字.
 * @return 0: 成功, 其他: 错误码.
 */
static int epd_command(uint8_t command)
{
	return epd_write(false, &command, 1);
}

/**
 * @brief 向EPD发送命令.
 *
 * @param data 要发送的数据.
 * @param length 数据长度.
 * @return 0: 成功, 其他: 错误码.
 */
static int epd_data(const uint8_t *data, size_t length)
{
	return epd_write(true, data, length);
}


static int epd_wait_ready(void)
{
	// 1. 获取当前时间
	int64_t deadline = k_uptime_get() + EPD_BUSY_TIMEOUT_MS;
	int busy;
	// 2. 等待EPD就绪
	while (true)
	{
		busy = gpio_pin_get_dt(&epd_busy);
		if (busy < 0) {
			return busy;
		}
		if (busy == 0) {
			return 0;
		}
		if (k_uptime_get() >= deadline) {
			printk("EPD BUSY timeout: logical=%d raw=%d\n",
			       busy, epd_busy_raw());
			return -ETIMEDOUT;
		}
		k_msleep(10);
	}
}

static int epd_wait_busy(void)
{
	// 1. 获取当前时间
	int64_t deadline = k_uptime_get() + EPD_BUSY_ASSERT_TIMEOUT_MS;
	int busy;
	// 2. 等待EPD忙信号断开
	while (true) {
		busy = gpio_pin_get_dt(&epd_busy);
		if (busy < 0) {
			return busy;
		}
		if (busy != 0) {
			return 0;
		}
		if (k_uptime_get() >= deadline) {
			printk("EPD BUSY assert timeout: logical=%d raw=%d\n",
			       busy, epd_busy_raw());
			return -ETIMEDOUT;
		}
		k_msleep(1);
	}
}

static int epd_write_register(uint8_t command, const uint8_t *data, size_t length)
{
	int ret = epd_command(command);

	if (ret == 0 && length != 0) 
	{
		ret = epd_data(data, length);
	}

	return ret;
}

static int epd_reset(void)
{
	int ret;

	ret = gpio_pin_set_dt(&epd_res, 0);
	if (ret < 0) {
		return ret;
	}
	/* Keep RES# low for a conservative 10 ms. */
	k_msleep(10);

	ret = gpio_pin_set_dt(&epd_res, 1);
	if (ret < 0) {
		return ret;
	}
	k_msleep(10);

	/* UC8179 ignores commands for at least 1 ms after reset release. */
	k_msleep(20);
	return 0;
}

static int epd_init(void)
{
	static const uint8_t panel_setting[] = {0x0F};
	/* Common 800x480 UC8179 panel power profile. */
	static const uint8_t power_setting[] = {0x07, 0x17, 0x3F, 0x3F, 0x03};
	static const uint8_t booster_soft_start[] = {0x17, 0x17, 0x17, 0x17};
	static const uint8_t pll_setting[] = {0x39};
	static const uint8_t resolution[] = {0x03, 0x20, 0x01, 0xE0};
	static const uint8_t vcom_setting[] = {0x28};
	static const uint8_t cdi_setting[] = {0x12, 0x07};
	int ret;

	ret = epd_reset();
	if (ret < 0) {
		return ret;
	}

	ret = epd_write_register(0x00, panel_setting, sizeof(panel_setting));
	if (ret == 0) {
		ret = epd_write_register(0x01, power_setting, sizeof(power_setting));
	}
	if (ret == 0) {
		ret = epd_write_register(0x06, booster_soft_start,
					 sizeof(booster_soft_start));
	}
	if (ret == 0) {
		ret = epd_write_register(0x30, pll_setting, sizeof(pll_setting));
	}
	if (ret == 0) {
		ret = epd_write_register(0x61, resolution, sizeof(resolution));
	}
	if (ret == 0) {
		ret = epd_write_register(0x82, vcom_setting,
					 sizeof(vcom_setting));
	}
	if (ret == 0) {
		ret = epd_write_register(0x50, cdi_setting, sizeof(cdi_setting));
	}
	if (ret == 0) {
		ret = epd_command(0x04);
	}
	if (ret == 0) {
		/* Do not send frame data until the booster and regulators are ready. */
		ret = epd_wait_busy();
	}
	if (ret == 0) {
		ret = epd_wait_ready();
	}

	return ret;
}


static int epd_write_test_plane(uint8_t command, const uint8_t *plane)
{
	int ret = epd_command(command);

	if (ret < 0)
	{
		return ret;
	}

	/* One SPI transaction keeps CS# asserted for all 48,000 bytes. */
	return epd_data(plane, EPD_FRAME_BYTES);
}

static int epd_refresh_test_pattern(void)
{
	int ret;

	ret = epd_wait_ready();
	if (ret == 0) {
		ret = epd_write_test_plane(0x10, epd_image_kw);
	}
	if (ret == 0) {
		ret = epd_write_test_plane(0x13, epd_image_red);
	}
	if (ret == 0) {
		/* UC8179 refresh sequence is DTM1 -> DTM2 -> DRF. */
		ret = epd_command(0x12);
	}
	if (ret == 0) {
		ret = epd_wait_busy();
	}
	if (ret == 0) {
		ret = epd_wait_ready();
	}

	return ret;
}

static int epd_refresh_text(const uint8_t *text, size_t length)
{
	uint8_t kw_row[EPD_TEXT_ROW_BYTES];
	uint8_t red_row[EPD_TEXT_ROW_BYTES];
	int ret = epd_wait_ready();

	if (ret == 0) {
		ret = epd_command(0x10);
	}
	for (size_t row = 0; ret == 0 && row < EPD_HEIGHT; row++) {
		epd_text_render_row(text, length, row, kw_row, red_row);
		ret = epd_data(kw_row, sizeof(kw_row));
	}
	if (ret == 0) {
		ret = epd_command(0x13);
	}
	for (size_t row = 0; ret == 0 && row < EPD_HEIGHT; row++) {
		epd_text_render_row(text, length, row, kw_row, red_row);
		ret = epd_data(red_row, sizeof(red_row));
	}
	if (ret == 0) {
		ret = epd_command(0x12);
	}
	if (ret == 0) {
		ret = epd_wait_busy();
	}
	if (ret == 0) {
		ret = epd_wait_ready();
	}
	return ret;
}

static int epd_stream_start_plane(uint8_t command)
{
	int ret = epd_wait_ready();

	if (ret == 0) {
		ret = epd_command(command);
	}
	return ret;
}

static int epd_stream_refresh(void)
{
	int ret = epd_command(0x12);

	if (ret == 0) {
		ret = epd_wait_busy();
	}
	if (ret == 0) {
		ret = epd_wait_ready();
	}
	return ret;
}

static const struct epd_frame_rx_ops frame_rx_ops = {
	.start_plane = epd_stream_start_plane,
	.write_data = epd_data,
	.refresh = epd_stream_refresh,
};

static uint8_t pending_text_buffer[EPD_TEXT_MAX_BYTES];

int main(void)
{
	int ret;

	printk("UC8179 test start\n");
	ret = ble_epd_init();
	if (ret < 0) {
		printk("BLE init returned: %d\n", ret);
	}

	if (!spi_is_ready_dt(&epd_spi) || !device_is_ready(epd_dc.port) ||
	    !device_is_ready(epd_res.port) || !device_is_ready(epd_busy.port)) 
	{
		printk("EPD hardware is not ready\n");
		return 0;
	}
	ret = gpio_pin_configure_dt(&epd_dc, GPIO_OUTPUT_INACTIVE);
	if (ret == 0) {
		ret = gpio_pin_configure_dt(&epd_res, GPIO_OUTPUT_INACTIVE);
	}
	if (ret == 0) {
		ret = gpio_pin_configure_dt(&epd_busy, GPIO_INPUT);
	}
	if (ret < 0) {
		printk("GPIO setup failed: %d\n", ret);
		return 0;
	}
	ret = epd_init();
	if (ret < 0) {
		printk("UC8179 init failed: %d\n", ret);
		return 0;
	}

	printk("UC8179 initialized, refreshing test pattern\n");
	ret = epd_refresh_test_pattern();
	if (ret < 0) {
		printk("EPD refresh failed: %d\n", ret);
		return 0;
	}

	printk("EPD refresh complete\n");
	epd_frame_rx_init(&frame_rx_ops);
	while (true)
	{
		size_t text_length;

		epd_frame_rx_process();
		if (!epd_frame_rx_is_busy() &&
		    ble_epd_take_text(pending_text_buffer, sizeof(pending_text_buffer),
				      &text_length) == 0)
		{
			printk("Displaying BLE text: %s\n", pending_text_buffer);
			ret = epd_refresh_text(pending_text_buffer, text_length);
			if (ret < 0)
			{
				printk("EPD text refresh failed: %d\n", ret);
			}
		}
		k_msleep(1);
	}

	return 0;
}
