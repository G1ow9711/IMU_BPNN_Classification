/* 命令行未定义时才公开 ESP-IDF 适配器合同，避免严格编译器报告宏重复定义。 */
#ifndef ESP_PLATFORM
/* 本文件只链接主机 stub，不伪装真实硬件事务。 */
#define ESP_PLATFORM (1)
#endif

/* 引入待测适配器公开类型、动态地址 API 和地址常量。 */
#include "board_sensors.h"
/* 引入主机 stub 的 TickType_t，匹配生产适配器调用的 FreeRTOS 延时原型。 */
#include "freertos/FreeRTOS.h"
/* 引入 vTaskDelay 声明，确保测试实现与生产适配器调用签名一致。 */
#include "freertos/task.h"

/* 引入格式化输出，报告断言位置和最终数量。 */
#include <stdio.h>
/* 引入内存清零，保证每个测试互不污染。 */
#include <string.h>

/* 定义 stub 主总线实体；生产头仅公开其不透明指针。 */
struct i2c_master_bus_stub {
    /* 保存非零标记，防止测试误把合法总线当成空指针。 */
    uint32_t marker;
};

/* 定义 stub 设备实体；每个句柄保存 ESP-IDF 注册时的实际 7 位地址。 */
struct i2c_master_device_stub {
    /* 保存 0x6A、0x6B、0x34 或 0x51。 */
    uint8_t address;
};

/* 最多注册 QMI8658、AXP2101、PCF85063 三个设备。 */
static struct i2c_master_device_stub g_devices[3];
/* 按注册顺序保存三个 7 位地址，用于验证 QMI 地址注入。 */
static uint8_t g_added_addresses[3];
/* 保存成功注册设备数量，范围 0~3。 */
static size_t g_add_count = 0U;
/* 保存反初始化删除句柄数量，正常完整路径应为 3。 */
static size_t g_remove_count = 0U;
/* 保存最近一次读写回调实际选择的设备地址。 */
static uint8_t g_last_transaction_address = 0U;
/* 保存累计断言数，便于总测试入口汇总。 */
static unsigned int g_assertions = 0U;

/* 每次测试失败时输出源码行并立即返回 false。 */
#define TEST_ASSERT(condition)                                                                                         \
    do {                                                                                                               \
        /* 当前断言无论成功或失败都计入总数。 */                                                                       \
        ++g_assertions;                                                                                                \
        /* 条件不成立时输出文件行，便于定位地址适配回归。 */                                                           \
        if (!(condition)) {                                                                                            \
            /* 输出失败位置。 */                                                                                       \
            (void)fprintf(stderr, "adapter assertion failed at line %d\n", __LINE__);                                \
            /* 终止当前测试。 */                                                                                       \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

/* 清空 ESP-IDF stub 的注册、删除和最近事务记录。 */
static void reset_stub_state(void)
{
    /* 清空三个设备实体及其实际地址。 */
    (void)memset(g_devices, 0, sizeof(g_devices));
    /* 清空注册地址序列。 */
    (void)memset(g_added_addresses, 0, sizeof(g_added_addresses));
    /* 重置注册计数。 */
    g_add_count = 0U;
    /* 重置删除计数。 */
    g_remove_count = 0U;
    /* 重置最近事务地址。 */
    g_last_transaction_address = 0U;
}

/* 模拟 ESP-IDF 在共享总线上注册一个设备，并保存配置中的 7 位地址。 */
esp_err_t i2c_master_bus_add_device(
    i2c_master_bus_handle_t bus_handle,
    const i2c_device_config_t *config,
    i2c_master_dev_handle_t *device_handle)
{
    /* 空总线、空配置、空输出或超过三个板载设备时返回参数错误。 */
    if ((bus_handle == NULL) || (config == NULL) || (device_handle == NULL) || (g_add_count >= 3U)) {
        /* 返回 stub 参数错误。 */
        return ESP_ERR_INVALID_ARG;
    }
    /* 保存当前设备的实际地址。 */
    g_devices[g_add_count].address = (uint8_t)config->device_address;
    /* 保存注册顺序，首项应为调用方注入的 QMI 地址。 */
    g_added_addresses[g_add_count] = (uint8_t)config->device_address;
    /* 返回当前实体地址作为不透明设备句柄。 */
    *device_handle = &g_devices[g_add_count];
    /* 累加注册数量。 */
    ++g_add_count;
    /* 返回成功。 */
    return ESP_OK;
}

/* 模拟 ESP-IDF 删除设备句柄。 */
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device_handle)
{
    /* 空句柄不能删除。 */
    if (device_handle == NULL) {
        /* 返回参数错误。 */
        return ESP_ERR_INVALID_ARG;
    }
    /* 累加删除数量，用于验证三个句柄均释放。 */
    ++g_remove_count;
    /* 返回成功。 */
    return ESP_OK;
}

/* 模拟先写寄存器地址再读取数据，并记录适配器选择的设备句柄地址。 */
esp_err_t i2c_master_transmit_receive(
    i2c_master_dev_handle_t device_handle,
    const uint8_t *write_buffer,
    size_t write_size,
    uint8_t *read_buffer,
    size_t read_size,
    int timeout_ms)
{
    /* 未使用的寄存器字节和超时仍显式检查有效性。 */
    if ((device_handle == NULL) || (write_buffer == NULL) || (write_size == 0U) ||
        (read_buffer == NULL) || (read_size == 0U) || (timeout_ms <= 0)) {
        /* 返回参数错误。 */
        return ESP_ERR_INVALID_ARG;
    }
    /* 保存实际设备地址，证明 0x6A 回调没有落到默认 0x6B 句柄。 */
    g_last_transaction_address = device_handle->address;
    /* 用零填充读取输出；本测试只检查路由，不验证芯片寄存器内容。 */
    (void)memset(read_buffer, 0, read_size);
    /* 返回成功。 */
    return ESP_OK;
}

/* 模拟连续发送，并记录适配器选择的设备句柄地址。 */
esp_err_t i2c_master_transmit(
    i2c_master_dev_handle_t device_handle,
    const uint8_t *write_buffer,
    size_t write_size,
    int timeout_ms)
{
    /* 检查句柄、输入、长度和超时。 */
    if ((device_handle == NULL) || (write_buffer == NULL) || (write_size == 0U) || (timeout_ms <= 0)) {
        /* 返回参数错误。 */
        return ESP_ERR_INVALID_ARG;
    }
    /* 保存实际写事务地址。 */
    g_last_transaction_address = device_handle->address;
    /* 返回成功。 */
    return ESP_OK;
}

/* 模拟 FreeRTOS 延时；地址适配测试不需要真实等待。 */
void vTaskDelay(TickType_t ticks)
{
    /* 显式忽略 tick，避免主机严格告警。 */
    (void)ticks;
}

/* 验证显式低地址注册、回调路由和反初始化状态。 */
static bool test_low_address_adapter(void)
{
    /* 清空上一测试状态。 */
    reset_stub_state();
    /* 创建非空共享总线实体。 */
    struct i2c_master_bus_stub bus = {.marker = 1U};
    /* 零初始化适配器。 */
    board_sensors_esp_idf_i2c_t adapter = {0};
    /* 注入板级探测得到的 0x6A 地址并注册三个设备。 */
    TEST_ASSERT(
        board_sensors_esp_idf_i2c_init_with_qmi_address(
            &adapter,
            &bus,
            BOARD_QMI8658_I2C_ADDRESS_LOW) == BOARD_SENSORS_OK);
    /* 必须注册 QMI、AXP、RTC 三个句柄。 */
    TEST_ASSERT(g_add_count == 3U);
    /* 第一个 ESP-IDF 设备必须使用实际低地址 0x6A。 */
    TEST_ASSERT(g_added_addresses[0] == BOARD_QMI8658_I2C_ADDRESS_LOW);
    /* 适配器状态必须保存实际低地址。 */
    TEST_ASSERT(adapter.qmi8658_address == BOARD_QMI8658_I2C_ADDRESS_LOW);
    /* 准备一字节读取输出。 */
    uint8_t value = 0xFFU;
    /* 核心使用 0x6A 时必须找到 QMI 句柄并成功路由。 */
    TEST_ASSERT(adapter.port.read_register(adapter.port.context, BOARD_QMI8658_I2C_ADDRESS_LOW, 0x00U, &value, 1U) == 0);
    /* 实际事务必须命中注册的 0x6A 句柄。 */
    TEST_ASSERT(g_last_transaction_address == BOARD_QMI8658_I2C_ADDRESS_LOW);
    /* 未注册的 0x6B 不得错误复用低地址句柄。 */
    TEST_ASSERT(adapter.port.read_register(adapter.port.context, BOARD_QMI8658_I2C_ADDRESS_HIGH, 0x00U, &value, 1U) != 0);
    /* 删除三个句柄并清空地址状态。 */
    board_sensors_esp_idf_i2c_deinit(&adapter);
    /* QMI、AXP、RTC 三个句柄必须全部删除。 */
    TEST_ASSERT(g_remove_count == 3U);
    /* 反初始化后实际地址必须清零。 */
    TEST_ASSERT(adapter.qmi8658_address == 0U);
    /* 返回通过。 */
    return true;
}

/* 验证旧接口默认 0x6B，非法地址不注册任何设备。 */
static bool test_default_and_invalid_address(void)
{
    /* 清空上一测试状态。 */
    reset_stub_state();
    /* 创建非空共享总线。 */
    struct i2c_master_bus_stub bus = {.marker = 2U};
    /* 零初始化适配器。 */
    board_sensors_esp_idf_i2c_t adapter = {0};
    /* 旧接口必须继续成功。 */
    TEST_ASSERT(board_sensors_esp_idf_i2c_init(&adapter, &bus) == BOARD_SENSORS_OK);
    /* 首个设备必须保持默认高地址 0x6B。 */
    TEST_ASSERT(g_added_addresses[0] == BOARD_QMI8658_I2C_ADDRESS_HIGH);
    /* 先释放默认接口创建的三个句柄。 */
    board_sensors_esp_idf_i2c_deinit(&adapter);
    /* 清空计数，单独验证非法地址路径。 */
    reset_stub_state();
    /* 0x69 不属于 QMI8658，必须返回参数错误。 */
    TEST_ASSERT(board_sensors_esp_idf_i2c_init_with_qmi_address(&adapter, &bus, 0x69U) == BOARD_SENSORS_ERR_ARGUMENT);
    /* 非法地址不得注册 QMI、AXP 或 RTC 任一设备。 */
    TEST_ASSERT(g_add_count == 0U);
    /* 失败适配器必须保持零地址。 */
    TEST_ASSERT(adapter.qmi8658_address == 0U);
    /* 返回通过。 */
    return true;
}

/* 依次运行 ESP-IDF 适配器动态地址测试。 */
int main(void)
{
    /* 运行低地址注册和路由测试。 */
    if (!test_low_address_adapter()) {
        /* 返回失败码。 */
        return 1;
    }
    /* 运行默认与非法地址测试。 */
    if (!test_default_and_invalid_address()) {
        /* 返回失败码。 */
        return 1;
    }
    /* 输出明确通过标志和断言数。 */
    (void)printf("board_sensors_esp_idf_tests passed assertions=%u\n", g_assertions);
    /* 返回成功。 */
    return 0;
}
