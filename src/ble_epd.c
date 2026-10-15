#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>

#include "ble_epd.h"
//定义128位服务UUID
#define BT_UUID_EPD_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0ULL)
//定义128位control UUID
#define BT_UUID_EPD_CONTROL_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1ULL)
//定义128位status UUID
#define BT_UUID_EPD_STATUS_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef2ULL)
//定义128位发送帧 UUID
#define BT_UUID_EPD_FRAME_DATA_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef3ULL)

#define EPD_ADVERTISING_DATA_COUNT   2 // 广播数据项的数量
#define EPD_SCAN_RESPONSE_DATA_COUNT 1 // 扫描响应数据项的数量
#define FRAME_QUEUE_DEPTH 			 8 // 图像帧数据缓存队列深度

struct frame_chunk {
	uint16_t 					   length;// 当前数据块的实际字节长度
	uint8_t data[BLE_EPD_FRAME_CHUNK_MAX];// 存放图像二进制块的数组
};


// 将上面定义的 128 位 UUID 值的宏实例化为结构体对象，供 GATT 服务注册使用
static struct bt_uuid_128 epd_service_uuid =
			BT_UUID_INIT_128(BT_UUID_EPD_SERVICE_VAL)	;
static struct bt_uuid_128 epd_control_uuid =
			BT_UUID_INIT_128(BT_UUID_EPD_CONTROL_VAL)   ;
static struct bt_uuid_128 epd_status_uuid =
			BT_UUID_INIT_128(BT_UUID_EPD_STATUS_VAL)    ;
static struct bt_uuid_128 epd_frame_data_uuid =
			BT_UUID_INIT_128(BT_UUID_EPD_FRAME_DATA_VAL);

// 定义并初始化一个消息队列：用于向 EPD 主线程传递控制指令（容量 4 个，单个大小为枚举类型，4字节对齐）
K_MSGQ_DEFINE(frame_command_queue, sizeof(enum ble_epd_frame_command), 4, 4);

// 定义并初始化一个消息队列：用于向 EPD 主线程传递图像数据块（容量 8 个，每个为 struct frame_chunk，4字节对齐）
K_MSGQ_DEFINE(frame_data_queue, sizeof(struct frame_chunk), FRAME_QUEUE_DEPTH, 4);

static char status_text[32] = "READY";// 保存当前 BLE 状态的字符串缓冲（默认 "READY")
static uint8_t 		 pending_text[96];// 暂存待显示文本的缓冲区
static size_t     pending_text_length;// 待显示文本的实际长度
static bool 			 text_pending;// 标志位：是否有新的文本待处理
static bool 	 frame_accepting_data;// 标志位：当前是否处于允许接收图像数据的状态
static size_t 	 frame_accepted_bytes;// 当前已接收到的图像字节总数
static struct 	   k_mutex text_mutex;// 用于保护文本缓冲区的互斥锁
static struct     k_mutex frame_mutex;// 用于保护帧接收状态机的互斥锁
static struct    k_mutex status_mutex;// 用于保护状态字符串的互斥锁
static atomic_t   ble_connected_state;// 原子变量：记录 BLE 连接状态（1:已连接, 0:未连接）
static atomic_t status_notify_enabled;// 原子变量：记录客户端是否开启了 Status 的 Notify 通知（1:开启, 0:关闭）

static const struct   bt_data advertising_data[];// 声明广播数据数组
static const struct bt_data scan_response_data[];// 声明扫描响应数据数组

// 前置声明：状态读取回调函数
static ssize_t status_read(struct bt_conn *conn,
				const struct bt_gatt_attr *attr,
				   					  void *buf, 
								   uint16_t len, 
							   uint16_t offset);
// 前置声明：控制指令写入回调函数
static ssize_t control_write(struct bt_conn *conn,
				  const struct bt_gatt_attr *attr,
				    			  const void *buf, 
								 	 uint16_t len,
				    			  uint16_t offset, 
								   uint8_t flags);
// 前置声明：图像帧数据写入回调函数
static ssize_t frame_data_write(struct bt_conn *conn,
					 const struct bt_gatt_attr *attr,
									 const void *buf, 
										uint16_t len,
									 uint16_t offset, 
									  uint8_t flags);
// 前置声明：CCC (Client Characteristic Configuration) 改变时的回调函数（订阅/取消订阅 Notify）
static void status_ccc_changed(const struct bt_gatt_attr *attr, 
											   uint16_t value);
// 前置声明：连接断开后的异步延时处理函数，并定义相关的 Work 任务
static void disconnected_work_handler(struct k_work *work);
K_WORK_DEFINE(disconnected_work, disconnected_work_handler);

// 使用宏注册 BLE GATT 服务及其特征值
BT_GATT_SERVICE_DEFINE(epd_service,
	// 定义主服务 (Primary Service)
	BT_GATT_PRIMARY_SERVICE(&epd_service_uuid.uuid),
	// 1. 定义 Control 特征值：可写、无需响应写，写权限为可写，绑定 control_write 回调
	BT_GATT_CHARACTERISTIC(&epd_control_uuid.uuid  ,
	BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
	BT_GATT_PERM_WRITE, NULL, control_write, NULL) ,
	// 2. 定义 Status 特征值：可读、可通知 (Notify)，读权限为可读，绑定 status_read 回调
	BT_GATT_CHARACTERISTIC(&epd_status_uuid.uuid   ,
	BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY    ,
	BT_GATT_PERM_READ, status_read, NULL, NULL),
	// 定义 Status 特征值的 CCCD (用于控制 Notify 的订阅状态)
	BT_GATT_CCC(status_ccc_changed,
	BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	// 3. 定义 Frame Data 特征值：可写，绑定 frame_data_write 回调
	BT_GATT_CHARACTERISTIC(&epd_frame_data_uuid.uuid,
	BT_GATT_CHRC_WRITE,
	BT_GATT_PERM_WRITE, NULL, frame_data_write, NULL));

/**
 * @brief 当中央设备（如手机）读取 Status 特征值时的回调
 * 
 * @param conn 连接对象指针
 * @param attr 属性对象指针
 * @param buf 输出缓冲区指针
 * @param len 输出缓冲区长度
 * @param offset 偏移量
 * @return ssize_t 实际读取的字节数
 */
static ssize_t status_read(struct bt_conn *conn,
				const struct bt_gatt_attr *attr,
				   					  void *buf, 
								   uint16_t len, 
							    uint16_t offset)
{
	char value[sizeof(status_text)];
	size_t value_length;
	// 1.加锁安全拷贝当前的状态字符串
	k_mutex_lock(&status_mutex, K_FOREVER);
	value_length = strlen(status_text);
	memcpy(value, status_text, value_length + 1);
	k_mutex_unlock(&status_mutex);
	// 2.调用 Zephyr API 将数据返回给中央设备
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, value_length);
}

/**
 * @brief 供外部/系统调用的函数：设置当前状态，并通过 Notify 主动推送给手机
 * 
 * @param text 要设置的状态字符串
 */
void ble_epd_set_status(const char *text)
{
	size_t length;
	int ret;
	// 1. 检查输入指针是否为空
	if (text == NULL) 
	{
		return;
	}
	length = strlen(text);
	// 2. 检查输入字符串长度是否超过限制
	if (length >= sizeof(status_text)) 
	{
		length = sizeof(status_text) - 1;
	}
	// 3. 加锁安全拷贝状态字符串
	k_mutex_lock(&status_mutex, K_FOREVER);
	memcpy(status_text, text, length);
	status_text[length] = '\0';
	k_mutex_unlock(&status_mutex);
	// 4. 检查连接状态和通知启用状态
	if (!atomic_get(&ble_connected_state) ||
	    !atomic_get(&status_notify_enabled)) 
	{
		return;
	}

	// 5.参数 NULL 表示将通知发送给所有订阅了该特征的已连接主设备 
	//   &epd_service.attrs[4] 对应 Status 特征的属性句柄
	ret = bt_gatt_notify(NULL, &epd_service.attrs[4], status_text, length);

	if (ret != 0 && atomic_get(&ble_connected_state)) 
	{
		printk("BLE status notify failed: %d (%s)\n", ret, status_text);
	}
}

/**
 * @brief 将命令放入控制命令队列
 * 
 * @param command 进入队列的命令
 * @return 0 成功，-ENOMEM 内存不足
 */
static int queue_frame_command(enum ble_epd_frame_command command)
{
	if (k_msgq_put(&frame_command_queue, &command, K_NO_WAIT) != 0) 
	{
		return -ENOMEM;
	}
	return 0;
}

/**
 * @brief 当手机向 Control 特征值写入数据时的回调
 * 
 * @param conn 连接指针
 * @param attr 特征属性指针
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @param offset 偏移量
 * @param flags 标志
 * @return 写操作结果
 */
static ssize_t control_write(struct bt_conn *conn,
				  const struct bt_gatt_attr *attr,
				    			  const void *buf, 
								 	 uint16_t len,
				    			  uint16_t offset, 
								   uint8_t flags)
{
	char command[96];
	int ret = 0;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);
	// 1.不支持带偏移量的分段写入
	if (offset != 0) 
	{
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	// 2.检查数据长度是否有效
	if (len == 0 || len >= sizeof(command))
	{
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(command, buf, len);
	command[len] = '\0';
	if (len == 4 && memcmp(command, "PING", 4) == 0) 
	{
		ble_epd_set_status("PONG");
	} 
	else if (len == 8 && memcmp(command, "FRAME:KW", 8) == 0) 
	{
		ret = queue_frame_command(BLE_EPD_FRAME_COMMAND_KW);
	} 
	else if (len == 9 && memcmp(command, "FRAME:RED", 9) == 0) 
	{
		ret = queue_frame_command(BLE_EPD_FRAME_COMMAND_RED);
	} 
	else if (len == 10 && memcmp(command, "FRAME:SHOW", 10) == 0) 
	{
		ret = queue_frame_command(BLE_EPD_FRAME_COMMAND_SHOW);
	} 
	else if (len == 11 && memcmp(command, "FRAME:ABORT", 11) == 0) 
	{
		ret = queue_frame_command(BLE_EPD_FRAME_COMMAND_ABORT);
	} 
	else if (len > 5 && memcmp(command, "TEXT:", 5) == 0) 
	{
		size_t text_length = len - 5;

		while (text_length                   >   0 	&&
			  (command[5 + text_length - 1] == '\r' ||
			   command[5 + text_length - 1] == '\n')) 
		{
			text_length--;
		}
		if (text_length == 0 || text_length >= sizeof(pending_text)) 
		{
			ble_epd_set_status("ERROR:TEXT_LEN");
			return len;
		}
		// 3. 处理文本命令
		k_mutex_lock(&text_mutex, K_FOREVER);
		memcpy(pending_text, &command[5], text_length);

		pending_text[text_length] =  		'\0';
		pending_text_length 	  =  text_length;
		text_pending        	  = 		true;

		k_mutex_unlock(&text_mutex)  ;
		ble_epd_set_status("TEXT_OK");
	} 
	else 
	{
		ble_epd_set_status("ERROR:UNKNOWN");
		return len;
	}
	// 4. 检查命令队列状态
	if (ret < 0) 
	{
		ble_epd_set_status("ERROR:CMD_QUEUE");
		return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
	}
	return len;
}

/**
 * @brief 当手机向 Frame Data 特征值写入图像数据块时的回调
 * 
 * @param conn 连接指针
 * @param attr 特征属性指针
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @param offset 偏移量
 * @param flags 标志
 * @return 写操作结果
 */
static ssize_t frame_data_write(struct bt_conn *conn,
					 const struct bt_gatt_attr *attr,
									 const void *buf, 
										uint16_t len,
									 uint16_t offset, 
									  uint8_t flags)
{
	struct frame_chunk chunk;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);
	// 1.不支持带偏移量的分段写入
	if (offset != 0) 
	{
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	// 2. 检查数据长度是否有效
	if (len == 0 || len > sizeof(chunk.data)) 
	{
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	k_mutex_lock(&frame_mutex, K_FOREVER);
	// 3. 检查是否允许接受数据
	if (!frame_accepting_data) 
	{
		k_mutex_unlock(&frame_mutex);
		return BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED);
	}
	// 4. 检查是否超过最大接受字节数
	if (frame_accepted_bytes + len > BLE_EPD_FRAME_BYTES) 
	{
		k_mutex_unlock(&frame_mutex);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	chunk.length = len;
	memcpy(chunk.data, buf, len);
	// 5. 将数据块推入消息队列供底层显示线程读取
	if (k_msgq_put(&frame_data_queue, &chunk, K_NO_WAIT) != 0) 
	{
		printk("BLE frame queue full at %u/%u bytes\n",
		       (unsigned int)frame_accepted_bytes, BLE_EPD_FRAME_BYTES);
		k_mutex_unlock(&frame_mutex);
		return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
	}
	frame_accepted_bytes += len;
	// 6. 检查是否已接收完所有数据
	if (frame_accepted_bytes == BLE_EPD_FRAME_BYTES) 
	{
		frame_accepting_data = false;
	}
	k_mutex_unlock(&frame_mutex);
	return len;
}

// 客户端改变订阅状态（CCCD 变化）时的回调
static void status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	// 判断客户端是否开启了 Notify (BT_GATT_CCC_NOTIFY)
	atomic_set(&status_notify_enabled,
		       value == BT_GATT_CCC_NOTIFY ? 1 : 0);
}

/**
 * @brief 清空/复位图像传输状态及消息队列
 */
static void reset_frame_transport(void)
{
	k_mutex_lock(&frame_mutex, K_FOREVER);
	frame_accepting_data = false;
	frame_accepted_bytes = 	   0;
	// 清空消息队列
	k_msgq_purge(&frame_data_queue);
	k_msgq_purge(&frame_command_queue);
	k_mutex_unlock(&frame_mutex);
}

/**
 * @brief 连接建立时的回调
 * 
 * @param conn 连接指针
 * @param err 错误码
 */
static void connected(struct bt_conn *conn, uint8_t err)
{
	ARG_UNUSED(conn);
	if (err != 0) 
	{
		printk("BLE connection failed: %u\n", err);
		return;
	}
	// 标记已连接
	atomic_set(&ble_connected_state, 1);
	// 复位 Notify 订阅状态（等待客户端重新订阅）
	atomic_set(&status_notify_enabled, 0);
	printk("BLE connected\n");
}

/**
 * @brief 连接建立时的回调
 * 
 * @brief 连接断开时的回调
 * 
 * @param conn 连接指针
 * @param reason 断开原因
 */
static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	// 标记已断开连接
	atomic_set(&ble_connected_state, 0);
	// 复位 Notify 订阅状态（等待客户端重新订阅）
	atomic_set(&status_notify_enabled, 0);
	printk("BLE disconnected: reason 0x%02x\n", reason);
	// 将断开后的清理与重新广播逻辑提交到内核 Work 工作队列异步执行，不在中断/协议栈上下文强行操作
	k_work_submit(&disconnected_work);
}

/**
 * @brief 断开后的清理与重新广播逻辑
 * 
 * @param work 工作项指针
 */
static void disconnected_work_handler(struct k_work *work)
{
	enum ble_epd_frame_command abort_command = BLE_EPD_FRAME_COMMAND_ABORT;
	int err;

	ARG_UNUSED(work);
	// 1. 临界条件处理：如果在这期间设备迅速重新连接了，说明新连接接管了传输，老连接的清理不能破坏当前状态
	if (atomic_get(&ble_connected_state)) 
	{
		return;
	}
	// 2. 重置帧传输状态
	reset_frame_transport();
	// 3. 向显示线程发送 ABORT 中止指令
	(void)k_msgq_put(&frame_command_queue, &abort_command, K_NO_WAIT);
	// 4.重新开启 BLE 广播，等待下次连接
	err = bt_le_adv_start(BT_LE_ADV_CONN, advertising_data,
				      EPD_ADVERTISING_DATA_COUNT, scan_response_data,
				      EPD_SCAN_RESPONSE_DATA_COUNT);
	if (err != 0 && err != -EALREADY) 
	{
		printk("BLE restart advertising failed: %d\n", err);
	}
}

// 注册连接回调函数结构体
BT_CONN_CB_DEFINE(epd_conn_callbacks) = {
	.connected    =    connected,
	.disconnected = disconnected,
};

// 配置广播数据包 (Advertising Data)
static const struct bt_data advertising_data[] = {
	// 广播 Flag：通用可发现模式，不支持 BR/EDR (经典蓝牙)
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	// 广播主服务的 128 位 UUID
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_EPD_SERVICE_VAL),
};

// 配置扫描响应数据包 (Scan Response Data)
static const struct bt_data scan_response_data[] = {
	BT_DATA(
					BT_DATA_NAME_COMPLETE, 
			  		CONFIG_BT_DEVICE_NAME,
  	   sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/** 
 * @brief BLE就绪回调函数
 * @param err 错误码
 */
static void bt_ready(int err)
{
	if (err != 0) 
	{
		printk("BLE enable failed: %d\n", err);
		return;
	}
	// 1. 开始 BLE 广播
	err = bt_le_adv_start(BT_LE_ADV_CONN,
				        advertising_data,
			ARRAY_SIZE(advertising_data),
				      scan_response_data,
		 ARRAY_SIZE(scan_response_data));

	if (err != 0) 
	{
		printk("BLE advertising failed: %d\n", err);
		return;
	}
	printk("BLE advertising as %s\n", CONFIG_BT_DEVICE_NAME);
}

// 模块初始化 API：供 main() 函数调用
int ble_epd_init(void)
{
	int err;

	k_mutex_init(&text_mutex);
	k_mutex_init(&frame_mutex);
	k_mutex_init(&status_mutex);
	atomic_set(&ble_connected_state, 0);
	atomic_set(&status_notify_enabled, 0);
	reset_frame_transport();
	err = bt_enable(bt_ready);
	if (err != 0) 
	{
		printk("BLE init failed: %d\n", err);
	}
	return err;
}

/**
 * @brief 获取待显示文本数据
 * @param buffer 存储文本数据的缓冲区
 * @param capacity 缓冲区容量
 * @param length 实际获取的文本长度
 * @return 0 成功，-EAGAIN 无数据，-EINVAL 参数错误
 */
int ble_epd_take_text(uint8_t *buffer, size_t capacity, size_t *length)
{
	int ret = -EAGAIN;

	if (buffer == NULL || length == NULL || capacity == 0) 
	{
		return -EINVAL;
	}
	k_mutex_lock(&text_mutex, K_FOREVER);
	if (text_pending && pending_text_length < capacity) 
	{
		memcpy(buffer, pending_text, pending_text_length);
		buffer[pending_text_length] = '\0';
		*length = pending_text_length;
		text_pending = false;
		ret = 0;
	}
	k_mutex_unlock(&text_mutex);
	return ret;
}

/**
 * @brief 获取帧命令
 * @param command 存储命令的指针
 * @return 0 成功，-EAGAIN 无数据，-EINVAL 参数错误
 */
int ble_epd_take_frame_command(enum ble_epd_frame_command *command)
{
	if (command == NULL) 
	{
		return -EINVAL;
	}
	// 非阻塞方式从消息队列取出一条指令
	return k_msgq_get(&frame_command_queue, command, K_NO_WAIT) == 0 ? 0 : -EAGAIN;
}

// 显示线程通知 BLE 模块：准备好接收新的图像数据帧
void ble_epd_frame_start_data(void)
{
	k_mutex_lock(&frame_mutex, K_FOREVER);
	k_msgq_purge(&frame_data_queue);
	frame_accepted_bytes = 0;
	frame_accepting_data = true;
	k_mutex_unlock(&frame_mutex);
}
// 显示线程通知 BLE 模块：停止接收图像数据帧
void ble_epd_frame_stop_data(void)
{
	k_mutex_lock(&frame_mutex, K_FOREVER);
	frame_accepting_data = false;
	frame_accepted_bytes = 0;
	k_msgq_purge(&frame_data_queue);
	k_mutex_unlock(&frame_mutex);
}

/**
 * @brief 显示线程从队列中提取一个图像数据块
 * @param buffer 存储数据的缓冲区
 * @param capacity 缓冲区容量
 * @param length 实际数据长度
 * @return 0 成功，-EAGAIN 无数据，-EINVAL 参数错误，-ENOBUFS 缓冲区不足
 */
int ble_epd_take_frame_data(uint8_t *buffer, size_t capacity, size_t *length)
{
	struct frame_chunk chunk;

	if (buffer == NULL || length == NULL) 
	{
		return -EINVAL;
	}
	if (k_msgq_get(&frame_data_queue, &chunk, K_NO_WAIT) != 0) 
	{
		return -EAGAIN;
	}
	if (chunk.length > capacity) 
	{
		return -ENOBUFS;
	}
	memcpy(buffer, chunk.data, chunk.length);
	*length = chunk.length;
	return 0;
}
