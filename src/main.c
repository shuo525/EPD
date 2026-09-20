#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "image_asset.h"

#define EPD_NODE DT_NODELABEL(epd)

#define EPD_WIDTH 800   //水墨屏宽度像素
#define EPD_HEIGHT 480  //水墨屏高度像素
#define EPD_ROW_BYTES (EPD_WIDTH / 8) //水墨屏宽度字节
#define EPD_FRAME_BYTES (EPD_ROW_BYTES * EPD_HEIGHT)//水墨屏总字节
#define EPD_BUSY_TIMEOUT_MS 30000
#define EPD_BUSY_ASSERT_TIMEOUT_MS 1000

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
	int ret = epd_write(false, &command, 1);

	printk("EPD command 0x%02x ret=%d\n", command, ret);
	return ret;
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
	printk("EPD BUSY after reset: logical=%d raw=%d\n",
	       gpio_pin_get_dt(&epd_busy), epd_busy_raw());
	return 0;
}

static int epd_init(void)
{
	static const uint8_t panel_setting[] = {0x0F};
	/* Common 800x480 UC8179 panel power profile. */
	static const uint8_t power_setting[] = {0x07, 0x17, 0x3F, 0x3F, 0x08};
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
	if (ret == 0) {
		printk("EPD BUSY after power-on: logical=%d raw=%d\n",
		       gpio_pin_get_dt(&epd_busy), epd_busy_raw());
	}

	return ret;
}

/* 32x32 glyphs generated offline from SimHei. Each uint32_t is one row,
 * with the leftmost pixel in bit 31. The order is: 擎、翌、智、能. */
static const uint32_t title_glyphs[4][32] = {
	{
		0x018C0700, 0x018C0E00, 0x3FFFCC00, 0x3FFFCFFE,
		0x018C1FFE, 0x078C1830, 0x07003830, 0x0FFFBC70,
		0x1FFFB660, 0x38018760, 0x37F183C0, 0x063181C0,
		0x063187F0, 0x07F7BE7E, 0x07F7BC1C, 0x00021188,
		0x080FFFC0, 0x07FFFE00, 0x0400C000, 0x0000C000,
		0x07FFFFF0, 0x07FFFFF0, 0x0000C000, 0x3FFFFFFE,
		0x3FFFFFFE, 0x0000C000, 0x0001C000, 0x0007C000,
		0x0007C000, 0x00000000, 0x00000000, 0x00000000,
	},
	{
		0x00000000, 0x00000000, 0x1FFE7FF8, 0x1FFE7FF0,
		0x1FFE7FF0, 0x000E0030, 0x070E1830, 0x070E1C30,
		0x030E1C30, 0x020E0830, 0x007E01F0, 0x07FE1FF0,
		0x3FCE7F30, 0x3C0E7030, 0x000E0030, 0x0001C000,
		0x0001C000, 0x0001C000, 0x1FFFFFF8, 0x1FFFFFF8,
		0x00000000, 0x00600C00, 0x00700E00, 0x00701E00,
		0x00381C00, 0x00383800, 0x00003800, 0x3FFFFFFC,
		0x3FFFFFFC, 0x00000000, 0x00000000, 0x00000000,
	},
	{
		0x00000000, 0x01800000, 0x03800000, 0x03000000,
		0x07FF9FF8, 0x0FFF9FF8, 0x0E301C38, 0x1C301C38,
		0x00301C38, 0x3FFFDC38, 0x3FFFDC38, 0x00601C38,
		0x00F81C38, 0x01FE1FF8, 0x03CFDFF8, 0x07838000,
		0x1F000000, 0x0DFFFFC0, 0x01FFFFC0, 0x01C001C0,
		0x01C001C0, 0x01C001C0, 0x01FFFFC0, 0x01FFFFC0,
		0x01C001C0, 0x01C001C0, 0x01FFFFC0, 0x01FFFFC0,
		0x01FFFFC0, 0x01C001C0, 0x00000000, 0x00000000,
	},
	{
		0x00000000, 0x00003800, 0x01E03800, 0x01C03830,
		0x03803870, 0x070C39F0, 0x061C3FC0, 0x0E0E3E00,
		0x1C073800, 0x1FFFB818, 0x1FE3B81C, 0x00003818,
		0x00001FF8, 0x0FFE0FF0, 0x0FFE0000, 0x0C063800,
		0x0C063800, 0x0C063830, 0x0FFE38F8, 0x0FFE3BE0,
		0x0C063F80, 0x0C063E00, 0x0FFE3800, 0x0FFE3800,
		0x0C063808, 0x0C06381C, 0x0C06381C, 0x0C0E3FF8,
		0x0C3E1FF8, 0x0C1C0000, 0x00000000, 0x00000000,
	},
};

static void make_title_plane(uint8_t *plane)
{
	const size_t x0 = (EPD_WIDTH - 4 * 32) / 2;
	const size_t y0 = (EPD_HEIGHT - 32) / 2;

	/* With CDI DDX=10, K/W=0 and RED=1 is white. */
	memset(plane, 0x00, EPD_FRAME_BYTES);
	for (size_t glyph = 0; glyph < 4; glyph++) 
	{
		for (size_t row = 0; row < 32; row++) 
		{
			uint32_t bits = title_glyphs[glyph][row];
			for (size_t col = 0; col < 32; col++) 
			{
				if (bits & (UINT32_C(1) << (31 - col))) 
				{
					const size_t x = x0 + glyph * 32 + col;
					const size_t y = y0 + row;
					plane[y * EPD_ROW_BYTES + x / 8] |=
						(uint8_t)(1U << (7 - (x % 8)));
				}
			}
		}
	}
}

static void make_white_plane(uint8_t *plane)
{
	/* With CDI DDX=10, RED=1 makes the unmarked area white. */
	memset(plane, 0xFF, EPD_FRAME_BYTES);
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

int main(void)
{
	int ret;

	printk("UC8179 test start\n");

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
	printk("EPD BUSY before reset: logical=%d raw=%d\n",
	       gpio_pin_get_dt(&epd_busy), epd_busy_raw());

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
	printk("EPD BUSY level after refresh: %d\n", gpio_pin_get_dt(&epd_busy));
	printk("EPD kept awake for debug\n");
	k_sleep(K_SECONDS(5));

	return 0;
}
