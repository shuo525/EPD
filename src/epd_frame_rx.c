#include <errno.h>
#include <stdbool.h>

#include <zephyr/sys/printk.h>
#include <zephyr/kernel.h>

#include "ble_epd.h"
#include "epd_frame_rx.h"

// 定义三色电子纸（黑/白/红）刷屏接收状态机的状态枚举
enum frame_state {
	FRAME_IDLE,         // 空闲状态：没有正在进行的图像传输
	FRAME_KW_RECEIVING, // 正在接收黑白（K/W - Black/White）通道数据
	FRAME_KW_DONE,	    // 黑白通道数据已完整接收
	FRAME_RED_RECEIVING,// 正在接收红色（RED）通道数据
	FRAME_RED_DONE,		// 红色通道数据已完整接收，准备好刷新
};

static const struct epd_frame_rx_ops *frame_ops;// 硬件操作接口结构体指针
static enum frame_state 			frame_state;// 当前状态机的状态（默认未初始化
static size_t 				plane_bytes_written;// 当前数据面（Plane）已经写入硬件 EPD 的累计字节数
static uint8_t frame_packet[BLE_EPD_FRAME_CHUNK_MAX];// 临时缓冲区，用于从 BLE 队列提取单个数据块

/**
 * @brief 当图像传输出现异常或出错时的统一失败处理函数
 * 
 * @param status 错误状态字符串
 * @param error 错误代码
 */
static void frame_fail(const char *status, int error)
{
	printk("EPD frame transfer failed: %d\n", error);
	ble_epd_frame_stop_data();
	ble_epd_set_status(status);
	frame_state = FRAME_IDLE;
	plane_bytes_written = 0;
}

/**
 * @brief 启动一个新的显示通道/平面（如黑白通道或红色通道）数据传输
 * 
 * @param epd_command EPD 硬件命令
 * @param receiving_state 接收状态机状态
 * @param ready_status 准备就绪状态字符串
 */
static void start_plane(uint8_t epd_command, 
		   enum frame_state receiving_state,
				   const char *ready_status)
{
	int ret;
	// 1.确保先停止并清空旧的数据传输状态
	ble_epd_frame_stop_data();
	// 2.调用硬件 SPI 接口，发送电子纸写入命令（例如 0x10 表示写黑白 RAM，0x13 表示写红 RAM）
	ret = frame_ops->start_plane(epd_command);
	if (ret < 0) 
	{
		frame_fail("FRAME:EPD_ERROR", ret);
		return;
	}

	plane_bytes_written = 0;
	frame_state = receiving_state;
	// 3.通知 BLE 模块允许开始接收数据包
	ble_epd_frame_start_data();
	// 4.通过 BLE 向客户端 Notify 就绪状态（如 "FRAME:KW_READY"）
	ble_epd_set_status(ready_status);
}

/**
 * @brief 处理 BLE 命令
 * 
 * @param command 
 */
static void handle_command(enum ble_epd_frame_command command)
{
	int ret;

	switch (command) {
	case BLE_EPD_FRAME_COMMAND_KW:
		// 1.FRAME:KW 总是开始一个新的图像帧传输，并放弃旧的未完成传输 
		start_plane(0x10, FRAME_KW_RECEIVING, "FRAME:KW_READY");
		break;
	case BLE_EPD_FRAME_COMMAND_RED:
		// 2.校验状态：必须先传完黑白通道数据，才能开始传红色通道数据
		if (frame_state != FRAME_KW_DONE) 
		{
			ble_epd_set_status("ERROR:NEED_KW");
			break;
		}
		start_plane(0x13, FRAME_RED_RECEIVING, "FRAME:RED_READY");
		break;
	case BLE_EPD_FRAME_COMMAND_SHOW:
		// 3.校验状态：必须先传完红色通道数据，才能刷新显示
		if (frame_state != FRAME_RED_DONE) 
		{
			ble_epd_set_status("ERROR:NEED_RED");
			break;
		}
		// 4.调用硬件接口发命令触发电子纸高压物理刷新（耗时操作）
		ret = frame_ops->refresh();
		if (ret < 0) 
		{
			frame_fail("FRAME:EPD_ERROR", ret);
			break;
		}
		frame_state = FRAME_IDLE;
		ble_epd_set_status("FRAME:DONE");
		printk("EPD BLE frame refresh complete\n");
		break;
	case BLE_EPD_FRAME_COMMAND_ABORT:
		// 主动中止当前的传输流程
		ble_epd_frame_stop_data();
		frame_state = FRAME_IDLE;
		plane_bytes_written = 0;
		ble_epd_set_status("FRAME:ABORTED");
		break;
	}
}

/**
 * @brief 从 BLE 队列中提取数据包，并写入硬件 EPD 显示器
 * 
 */
static void consume_frame_data(void)
{
	size_t length;
	int ret;
	unsigned int packets = 0;
	// 1.循环从 BLE 数据队列中拉取数据块，直到队列为空 (返回非 0)
	while (ble_epd_take_frame_data(frame_packet, sizeof(frame_packet), &length) == 0) 
	{
		// 2.状态合法性校验：当前必须处于黑白或红色数据接收状态
		if (frame_state != FRAME_KW_RECEIVING &&
		    frame_state != FRAME_RED_RECEIVING) 
		{
			frame_fail("ERROR:FRAME_STATE", -EINVAL);
			return;
		}
		// 3.边界校验：防止客户端发送的数据总量超过单帧所需字节数 (BLE_EPD_FRAME_BYTES)
		if (plane_bytes_written + length > BLE_EPD_FRAME_BYTES) 
		{
			frame_fail("ERROR:FRAME_SIZE", -EFBIG);
			return;
		}
		// 4.通过 SPI 硬件接口，将这一块数据写入电子纸控制器
		ret = frame_ops->write_data(frame_packet, length);
		if (ret < 0) 
		{
			frame_fail("FRAME:EPD_ERROR", ret);
			return;
		}
		plane_bytes_written += length;
		packets++;
		// 5.判断当前平面的数据是否全部接收并写入完毕
		if (plane_bytes_written == BLE_EPD_FRAME_BYTES) 
		{
			ble_epd_frame_stop_data();
			if (frame_state == FRAME_KW_RECEIVING) 
			{
				// 通知客户端黑白通道接收完毕
				frame_state = FRAME_KW_DONE;
				ble_epd_set_status("FRAME:KW_DONE");
				printk("EPD BLE K/W plane received: %u bytes\n",
				       							BLE_EPD_FRAME_BYTES);
			} 
			else 
			{
				// 通知客户端红色通道接收完毕
				frame_state = FRAME_RED_DONE;
				ble_epd_set_status("FRAME:RED_DONE");
				printk("EPD BLE RED plane received: %u bytes\n",
				       BLE_EPD_FRAME_BYTES);
			}
				return;
		}

		// 6.调度让步：每连续消耗 4 个包就主动 sleep 1ms，
        // 避免本线程强占 CPU 导致 BLE 协议栈的中断/高优先任务无法及时收包，引发丢包或队列溢出 */
		if (packets >= 4U) 
		{
			k_msleep(1);
			packets = 0;
		}
	}
}

/**
 * @brief 初始化图像传输状态机
 * 
 * @param ops 硬件操作接口结构体指针
 */
void epd_frame_rx_init(const struct epd_frame_rx_ops *ops)
{
	frame_ops   = ops;
	frame_state = FRAME_IDLE;
	plane_bytes_written = 0;
}

/**
 * @brief 主轮询/处理函数：需要被 EPD 显示线程（如 main 函数的 while(1) 或专属线程）定期调用
 */
void epd_frame_rx_process(void)
{
	enum ble_epd_frame_command command;

	if (frame_ops == NULL) 
	{
		return;
	}
	// 第一步：检查是否有新的控制命令，有则处理命令
	if (ble_epd_take_frame_command(&command) == 0) 
	{
		handle_command(command);
	}
	// 第二步：消费并写入图像数据块
	consume_frame_data();
}

/**
 * @brief 检查图像传输是否忙碌
 */
bool epd_frame_rx_is_busy(void)
{
	// 非 IDLE 状态即表示忙碌
	return frame_state != FRAME_IDLE;
}
