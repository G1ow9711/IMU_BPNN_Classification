/* 只有 ESP-IDF 固件构建会编译本适配器；主机测试只链接纯 C 核心。 */
#ifdef ESP_PLATFORM

/* 引入板载传感器公共合同和 ESP-IDF 句柄类型。 */
#include "board_sensors.h"

/* 引入 ESP-IDF 错误码。 */
#include "esp_err.h"
/* 引入 FreeRTOS 毫秒延时。 */
#include "freertos/FreeRTOS.h"
/* 引入 FreeRTOS 任务延时函数。 */
#include "freertos/task.h"
/* 引入内存清零和写缓冲复制。 */
#include <string.h>

/* 固定三芯片总线速度为 400 kHz，符合板载短走线快速模式。 */
#define BOARD_SENSORS_I2C_SPEED_HZ (400000U)
/* 固定单次事务超时为 50 ms，避免坏设备永久阻塞采样任务。 */
#define BOARD_SENSORS_I2C_TIMEOUT_MS (50)
/* 连续写最大 16 字节；当前最长 RTC 写为 7 字节。 */
#define BOARD_SENSORS_I2C_MAX_WRITE_BYTES (16U)

/* 按 7 位地址选择已注册 ESP-IDF 设备句柄。 */
static i2c_master_dev_handle_t board_sensors_esp_idf_find_handle(
    const board_sensors_esp_idf_i2c_t *adapter,
    uint8_t address)
{
    /* 适配器为空时没有可用句柄。 */
    if (adapter == NULL) {
        /* 返回空句柄。 */
        return NULL;
    }
    /* 匹配初始化时注册的 QMI8658 实际地址，允许 0x6A 或 0x6B。 */
    if (address == adapter->qmi8658_address) {
        /* 返回 QMI 句柄。 */
        return adapter->qmi8658_handle;
    }
    /* 匹配 AXP2101。 */
    if (address == BOARD_AXP2101_I2C_ADDRESS) {
        /* 返回 AXP 句柄。 */
        return adapter->axp2101_handle;
    }
    /* 匹配 PCF85063。 */
    if (address == BOARD_PCF85063_I2C_ADDRESS) {
        /* 返回 RTC 句柄。 */
        return adapter->pcf85063_handle;
    }
    /* 其它地址不属于本组件。 */
    return NULL;
}

/* 把核心驱动读回调映射到 ESP-IDF transmit_receive。 */
static int board_sensors_esp_idf_read(
    void *context,
    uint8_t address,
    uint8_t reg,
    uint8_t *data,
    size_t length)
{
    /* 恢复 ESP-IDF 适配器上下文。 */
    board_sensors_esp_idf_i2c_t *adapter = (board_sensors_esp_idf_i2c_t *)context;
    /* 根据地址取得设备句柄。 */
    i2c_master_dev_handle_t handle = board_sensors_esp_idf_find_handle(adapter, address);
    /* 检查句柄、输出和长度。 */
    if ((handle == NULL) || (data == NULL) || (length == 0U)) {
        /* 返回非零，让核心映射为 BOARD_SENSORS_ERR_IO。 */
        return -1;
    }
    /* 先发送 8 位寄存器地址，再重复起始读取连续数据。 */
    esp_err_t error =
        i2c_master_transmit_receive(handle, &reg, 1U, data, length, BOARD_SENSORS_I2C_TIMEOUT_MS);
    /* ESP_OK 映射零，其它错误映射非零。 */
    return error == ESP_OK ? 0 : -1;
}

/* 把核心驱动写回调映射到 ESP-IDF transmit。 */
static int board_sensors_esp_idf_write(
    void *context,
    uint8_t address,
    uint8_t reg,
    const uint8_t *data,
    size_t length)
{
    /* 恢复 ESP-IDF 适配器上下文。 */
    board_sensors_esp_idf_i2c_t *adapter = (board_sensors_esp_idf_i2c_t *)context;
    /* 根据地址取得设备句柄。 */
    i2c_master_dev_handle_t handle = board_sensors_esp_idf_find_handle(adapter, address);
    /* 检查句柄、输入和固定栈缓冲上限。 */
    if ((handle == NULL) || (data == NULL) || (length == 0U) ||
        (length > BOARD_SENSORS_I2C_MAX_WRITE_BYTES)) {
        /* 返回非零。 */
        return -1;
    }
    /* 栈上保存寄存器地址和最多 16 字节数据，共 17 字节。 */
    uint8_t transmit[1U + BOARD_SENSORS_I2C_MAX_WRITE_BYTES] = {0U};
    /* 第一字节写寄存器地址。 */
    transmit[0] = reg;
    /* 后续字节复制调用方数据。 */
    (void)memcpy(&transmit[1], data, length);
    /* 一次发送寄存器地址和数据，避免中间 STOP。 */
    esp_err_t error = i2c_master_transmit(handle, transmit, length + 1U, BOARD_SENSORS_I2C_TIMEOUT_MS);
    /* ESP_OK 映射零。 */
    return error == ESP_OK ? 0 : -1;
}

/* 把核心毫秒等待映射到 FreeRTOS，不使用忙等。 */
static void board_sensors_esp_idf_delay(void *context, uint32_t delay_ms)
{
    /* 当前延时不需要 I2C 上下文，显式忽略避免告警。 */
    (void)context;
    /* 把毫秒转换为 RTOS tick。 */
    TickType_t ticks = pdMS_TO_TICKS(delay_ms);
    /* 非零毫秒被向下取整为零 tick 时至少等待一 tick。 */
    if ((delay_ms > 0U) && (ticks == 0U)) {
        /* 设置最小等待。 */
        ticks = 1U;
    }
    /* 让出 CPU，等待芯片复位。 */
    vTaskDelay(ticks);
}

/* 在共享主总线上添加单个 7 位地址设备。 */
static esp_err_t board_sensors_esp_idf_add_device(
    i2c_master_bus_handle_t bus_handle,
    uint8_t address,
    i2c_master_dev_handle_t *device_handle)
{
    /* 检查输入句柄。 */
    if ((bus_handle == NULL) || (device_handle == NULL)) {
        /* 返回 ESP-IDF 参数错误。 */
        return ESP_ERR_INVALID_ARG;
    }
    /* 构造 7 位地址、400 kHz 设备配置。 */
    const i2c_device_config_t config = {
        /* 指定 7 位地址格式。 */
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        /* 写入当前设备地址。 */
        .device_address = address,
        /* 写入 SCL 频率。 */
        .scl_speed_hz = BOARD_SENSORS_I2C_SPEED_HZ,
    };
    /* 让 ESP-IDF 创建设备句柄。 */
    return i2c_master_bus_add_device(bus_handle, &config, device_handle);
}

/* 使用板级探测地址注册三个板载 I2C 设备并生成核心回调端口。 */
board_sensors_result_t board_sensors_esp_idf_i2c_init_with_qmi_address(
    board_sensors_esp_idf_i2c_t *adapter,
    i2c_master_bus_handle_t bus_handle,
    uint8_t qmi8658_address)
{
    /* 检查输出和共享主总线。 */
    if ((adapter == NULL) || (bus_handle == NULL)) {
        /* 返回参数错误。 */
        return BOARD_SENSORS_ERR_ARGUMENT;
    }
    /* 清空全部句柄，失败清理可安全判断空值。 */
    (void)memset(adapter, 0, sizeof(*adapter));
    /* QMI8658 只允许 SA0 地址脚定义的 0x6A 或 0x6B。 */
    if ((qmi8658_address != BOARD_QMI8658_I2C_ADDRESS_LOW) &&
        (qmi8658_address != BOARD_QMI8658_I2C_ADDRESS_HIGH)) {
        /* 非法地址不注册任何 ESP-IDF 设备句柄。 */
        return BOARD_SENSORS_ERR_ARGUMENT;
    }
    /* 保存共享总线，但不取得其所有权。 */
    adapter->bus_handle = bus_handle;
    /* 保存板级运行时探测到的实际 QMI 地址，供核心回调按地址选择句柄。 */
    adapter->qmi8658_address = qmi8658_address;
    /* 使用实际地址注册 QMI8658。 */
    if (board_sensors_esp_idf_add_device(bus_handle, qmi8658_address, &adapter->qmi8658_handle) !=
        ESP_OK) {
        /* 初始化失败后统一清理。 */
        board_sensors_esp_idf_i2c_deinit(adapter);
        /* 返回 I/O 错误。 */
        return BOARD_SENSORS_ERR_IO;
    }
    /* 注册 AXP2101。 */
    if (board_sensors_esp_idf_add_device(bus_handle, BOARD_AXP2101_I2C_ADDRESS, &adapter->axp2101_handle) !=
        ESP_OK) {
        /* 初始化失败后统一清理。 */
        board_sensors_esp_idf_i2c_deinit(adapter);
        /* 返回 I/O 错误。 */
        return BOARD_SENSORS_ERR_IO;
    }
    /* 注册 PCF85063。 */
    if (board_sensors_esp_idf_add_device(bus_handle, BOARD_PCF85063_I2C_ADDRESS, &adapter->pcf85063_handle) !=
        ESP_OK) {
        /* 初始化失败后统一清理。 */
        board_sensors_esp_idf_i2c_deinit(adapter);
        /* 返回 I/O 错误。 */
        return BOARD_SENSORS_ERR_IO;
    }
    /* 设置核心回调上下文为适配器本身。 */
    adapter->port.context = adapter;
    /* 注入 ESP-IDF 读取函数。 */
    adapter->port.read_register = board_sensors_esp_idf_read;
    /* 注入 ESP-IDF 写入函数。 */
    adapter->port.write_register = board_sensors_esp_idf_write;
    /* 注入 FreeRTOS 延时函数。 */
    adapter->port.delay_ms = board_sensors_esp_idf_delay;
    /* 返回成功。 */
    return BOARD_SENSORS_OK;
}

/* 使用兼容默认高地址 0x6B 注册三个板载 I2C 设备。 */
board_sensors_result_t board_sensors_esp_idf_i2c_init(
    board_sensors_esp_idf_i2c_t *adapter,
    i2c_master_bus_handle_t bus_handle)
{
    /* 转调显式地址接口，保持旧固件调用方无需修改。 */
    return board_sensors_esp_idf_i2c_init_with_qmi_address(
        adapter,
        bus_handle,
        BOARD_QMI8658_I2C_ADDRESS);
}

/* 删除三个设备句柄；不删除共享 I2C 主总线。 */
void board_sensors_esp_idf_i2c_deinit(board_sensors_esp_idf_i2c_t *adapter)
{
    /* 空适配器无需处理。 */
    if (adapter == NULL) {
        /* 直接返回。 */
        return;
    }
    /* QMI 句柄存在时删除。 */
    if (adapter->qmi8658_handle != NULL) {
        /* 从主总线移除 QMI 设备。 */
        (void)i2c_master_bus_rm_device(adapter->qmi8658_handle);
        /* 清空句柄，防止重复删除。 */
        adapter->qmi8658_handle = NULL;
    }
    /* AXP 句柄存在时删除。 */
    if (adapter->axp2101_handle != NULL) {
        /* 从主总线移除 AXP 设备。 */
        (void)i2c_master_bus_rm_device(adapter->axp2101_handle);
        /* 清空句柄。 */
        adapter->axp2101_handle = NULL;
    }
    /* RTC 句柄存在时删除。 */
    if (adapter->pcf85063_handle != NULL) {
        /* 从主总线移除 RTC 设备。 */
        (void)i2c_master_bus_rm_device(adapter->pcf85063_handle);
        /* 清空句柄。 */
        adapter->pcf85063_handle = NULL;
    }
    /* 清空公共回调端口，使后续核心调用立即失败。 */
    (void)memset(&adapter->port, 0, sizeof(adapter->port));
    /* 清空实际地址，避免反初始化后错误地址仍匹配空句柄。 */
    adapter->qmi8658_address = 0U;
    /* 清空不再拥有的总线引用。 */
    adapter->bus_handle = NULL;
}

#endif
