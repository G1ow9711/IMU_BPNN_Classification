#ifndef BOARD_SENSORS_TEST_STUB_I2C_MASTER_H
#define BOARD_SENSORS_TEST_STUB_I2C_MASTER_H

/* 引入 ESP-IDF 错误码 stub。 */
#include "esp_err.h"
/* 引入尺寸类型，匹配 ESP-IDF 连续传输长度。 */
#include <stddef.h>
/* 引入定长字节类型。 */
#include <stdint.h>

/* 主总线句柄在语法测试中保持不透明指针。 */
typedef struct i2c_master_bus_stub *i2c_master_bus_handle_t;
/* 设备句柄在语法测试中保持不透明指针。 */
typedef struct i2c_master_device_stub *i2c_master_dev_handle_t;
/* 7 位地址长度枚举只需提供生产源使用的常量。 */
#define I2C_ADDR_BIT_LEN_7 (0)

/* 模拟 ESP-IDF 设备配置字段。 */
typedef struct {
    /* 保存地址位宽枚举。 */
    int dev_addr_length;
    /* 保存 7 位设备地址。 */
    uint16_t device_address;
    /* 保存 SCL 频率。 */
    uint32_t scl_speed_hz;
} i2c_device_config_t;

/* 声明添加设备函数，语法测试不提供实现。 */
esp_err_t i2c_master_bus_add_device(
    i2c_master_bus_handle_t bus_handle,
    const i2c_device_config_t *config,
    i2c_master_dev_handle_t *device_handle);
/* 声明删除设备函数。 */
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device_handle);
/* 声明先写寄存器地址再读数据函数。 */
esp_err_t i2c_master_transmit_receive(
    i2c_master_dev_handle_t device_handle,
    const uint8_t *write_buffer,
    size_t write_size,
    uint8_t *read_buffer,
    size_t read_size,
    int timeout_ms);
/* 声明连续发送函数。 */
esp_err_t i2c_master_transmit(
    i2c_master_dev_handle_t device_handle,
    const uint8_t *write_buffer,
    size_t write_size,
    int timeout_ms);

#endif
