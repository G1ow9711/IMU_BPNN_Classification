#ifndef IMU_HANDHELD_BOARD_SENSORS_H
#define IMU_HANDHELD_BOARD_SENSORS_H

/* 引入布尔类型，描述数据就绪、电池连接和时钟有效状态。 */
#include <stdbool.h>
/* 引入尺寸类型，限定 I2C 连续读写长度。 */
#include <stddef.h>
/* 引入定长整数，保证寄存器、原始采样和 Unix 时间位宽跨平台一致。 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 固定 QMI8658 低 7 位 I2C 地址；SA0 地址脚为低时使用 0x6A。 */
#define BOARD_QMI8658_I2C_ADDRESS_LOW (0x6AU)
/* 固定 QMI8658 高 7 位 I2C 地址；SA0 地址脚为高时使用 0x6B。 */
#define BOARD_QMI8658_I2C_ADDRESS_HIGH (0x6BU)
/* 固定兼容默认地址；未执行板级探测的旧调用方继续使用厂家例程的 0x6B。 */
#define BOARD_QMI8658_I2C_ADDRESS (BOARD_QMI8658_I2C_ADDRESS_HIGH)
/* 固定 AXP2101 7 位 I2C 地址；厂家 XPowersLib 使用 0x34。 */
#define BOARD_AXP2101_I2C_ADDRESS (0x34U)
/* 固定 PCF85063 7 位 I2C 地址；厂家 SensorLib 使用 0x51。 */
#define BOARD_PCF85063_I2C_ADDRESS (0x51U)
/* 固定 QMI8658 WHO_AM_I 响应；厂家数据手册和 SensorLib 均给出 0x05。 */
#define BOARD_QMI8658_WHO_AM_I (0x05U)
/* 固定 AXP2101 IC_TYPE 响应；厂家 XPowersLib 常量为 0x4A。 */
#define BOARD_AXP2101_CHIP_ID (0x4AU)
/* 固定训练部署量程为正负 8 g，避免 6~7 g 跳跃冲击饱和。 */
#define BOARD_QMI8658_ACCEL_RANGE_G (8.0F)
/* 固定训练部署量程为正负 1024 deg/s，覆盖已审计约 850 deg/s 峰值。 */
#define BOARD_QMI8658_GYRO_RANGE_DPS (1024.0F)
/* 16 位有符号 ADC 满量程除数为 32768。 */
#define BOARD_QMI8658_ADC_DIVISOR (32768.0F)
/* 加速度每 LSB 对应 g，调用方直接传给 imu_pipeline_config_t。 */
#define BOARD_QMI8658_ACCEL_G_PER_LSB (BOARD_QMI8658_ACCEL_RANGE_G / BOARD_QMI8658_ADC_DIVISOR)
/* 角速度每 LSB 对应 deg/s，调用方直接传给 imu_pipeline_config_t。 */
#define BOARD_QMI8658_GYRO_DPS_PER_LSB (BOARD_QMI8658_GYRO_RANGE_DPS / BOARD_QMI8658_ADC_DIVISOR)
/* 固定 WOM 阈值为 40 mg；CAL1_L 每 LSB 为 1 mg，实机需按携带误唤醒与起手灵敏度复核。 */
#define BOARD_QMI8658_WOM_THRESHOLD_MG (40U)
/* 固定 WOM 消隐为 4 个 21 Hz 样本，约 190 ms；抑制一次动作造成的密集重复中断。 */
#define BOARD_QMI8658_WOM_BLANKING_SAMPLES (4U)

/* 描述驱动统一结果；负值表示没有产生可信输出。 */
typedef enum {
    /* 操作成功，输出字段均可使用。 */
    BOARD_SENSORS_OK = 0,
    /* 空指针、非法时间或不支持范围。 */
    BOARD_SENSORS_ERR_ARGUMENT = -1,
    /* 底层 I2C 事务失败或短读写。 */
    BOARD_SENSORS_ERR_IO = -2,
    /* 芯片身份寄存器与目标型号不符。 */
    BOARD_SENSORS_ERR_CHIP_ID = -3,
    /* 传感器尚未初始化或复位完成标志异常。 */
    BOARD_SENSORS_ERR_STATE = -4,
    /* RTC 的 VL 位、BCD 字段或日历组合无效。 */
    BOARD_SENSORS_ERR_CLOCK_INVALID = -5,
    /* 电量寄存器超出 0~100%，拒绝把损坏值显示给用户。 */
    BOARD_SENSORS_ERR_RANGE = -6
} board_sensors_result_t;

/* 描述 QMI8658 当前寄存器模式；用于防止低功耗状态下误读活动数据寄存器。 */
typedef enum {
    /* 加速度为正负 8 g/125 Hz，陀螺为正负 1024 deg/s/112.1 Hz，供训练推理采样。 */
    BOARD_QMI8658_MODE_ACTIVE = 0,
    /* 仅加速度以正负 2 g/21 Hz 低功耗运行，INT1 在超过阈值时由低翻高。 */
    BOARD_QMI8658_MODE_WAKE_ON_MOTION = 1,
    /* CTRL7 禁用加速度和陀螺，CAL1_L=0 禁用 WOM。 */
    BOARD_QMI8658_MODE_OFF = 2
} board_qmi8658_mode_t;

/* 定义可注入 I2C 端口；主机 fake 与 ESP-IDF 适配器共用同一接口。 */
typedef struct {
    /* 保存底层总线上下文；驱动只透传且不拥有其生命周期。 */
    void *context;
    /* 从 7 位地址和 8 位寄存器连续读取 length 字节；零表示成功。 */
    int (*read_register)(void *context, uint8_t address, uint8_t reg, uint8_t *data, size_t length);
    /* 向 7 位地址和 8 位寄存器连续写入 length 字节；零表示成功。 */
    int (*write_register)(void *context, uint8_t address, uint8_t reg, const uint8_t *data, size_t length);
    /* 等待芯片复位完成；毫秒单位，主机 fake 可只累计时间。 */
    void (*delay_ms)(void *context, uint32_t delay_ms);
} board_sensors_i2c_t;

/* 保存 QMI8658 驱动状态；调用方可静态分配，不使用动态内存。 */
typedef struct {
    /* 指向共享 I2C 端口；端口生命周期必须覆盖驱动。 */
    const board_sensors_i2c_t *bus;
    /* 保存板级探测注入的实际 7 位地址；合法值仅为 0x6A 或 0x6B。 */
    uint8_t address;
    /* 标记 WHO_AM_I、复位和控制寄存器配置均已成功。 */
    bool initialized;
    /* 保存最后完整成功的寄存器模式；转换失败时保持 OFF，禁止把半配置状态视为可唤醒。 */
    board_qmi8658_mode_t mode;
} board_qmi8658_t;

/* 保存一次异步数据就绪读取；可分别提交加速度和陀螺到 imu_pipeline。 */
typedef struct {
    /* 保存调用方在中断/轮询时捕获的单调微秒，禁止使用可跳变 RTC。 */
    uint64_t timestamp_us;
    /* 保存 QMI 24 位原始时间戳，仅作丢样诊断，不猜测其到微秒的换算。 */
    uint32_t sensor_timestamp_ticks;
    /* 标记本帧是否包含新的三轴加速度。 */
    bool accel_available;
    /* 标记本帧是否包含新的三轴角速度。 */
    bool gyro_available;
    /* 保存 ax、ay、az 原始小端 int16，量程为正负 8 g。 */
    int16_t accel_raw[3];
    /* 保存 gx、gy、gz 原始小端 int16，量程为正负 1024 deg/s。 */
    int16_t gyro_raw[3];
} board_qmi8658_frame_t;

/* 描述 AXP2101 充电阶段；数值与 STATUS2 低三位一致。 */
typedef enum {
    /* 涓流充电。 */
    BOARD_AXP_CHARGE_TRICKLE = 0,
    /* 预充电。 */
    BOARD_AXP_CHARGE_PRE = 1,
    /* 恒流充电。 */
    BOARD_AXP_CHARGE_CONSTANT_CURRENT = 2,
    /* 恒压充电。 */
    BOARD_AXP_CHARGE_CONSTANT_VOLTAGE = 3,
    /* 充电完成。 */
    BOARD_AXP_CHARGE_DONE = 4,
    /* 未充电或停止充电。 */
    BOARD_AXP_CHARGE_STOPPED = 5,
    /* 芯片返回保留值 6~7。 */
    BOARD_AXP_CHARGE_UNKNOWN = 255
} board_axp_charge_phase_t;

/* 保存 AXP2101 驱动状态。 */
typedef struct {
    /* 指向共享 I2C 端口。 */
    const board_sensors_i2c_t *bus;
    /* 标记芯片 ID 已验证。 */
    bool initialized;
} board_axp2101_t;

/* 保存一次 PMIC 快照；所有布尔值均来自同一组 STATUS 寄存器。 */
typedef struct {
    /* 保存燃料计百分比，范围 0~100。 */
    uint8_t soc_percent;
    /* 标记电池是否连接，来自 STATUS1.bit3。 */
    bool battery_present;
    /* 标记 VBUS 电压是否有效，来自 STATUS1.bit5。 */
    bool vbus_good;
    /* 标记 PMIC 当前处于充电方向，来自 STATUS2.bits7:5=001。 */
    bool charging;
    /* 标记 PMIC 当前处于放电方向，来自 STATUS2.bits7:5=010。 */
    bool discharging;
    /* 保存 STATUS2 低三位给出的充电阶段。 */
    board_axp_charge_phase_t charge_phase;
} board_axp2101_status_t;

/* 保存 PCF85063 驱动状态。 */
typedef struct {
    /* 指向共享 I2C 端口。 */
    const board_sensors_i2c_t *bus;
    /* 标记控制寄存器探测已成功。 */
    bool initialized;
} board_pcf85063_t;

/* 使用兼容默认地址 0x6B 初始化 QMI8658；旧调用方无需修改。 */
board_sensors_result_t board_qmi8658_init(board_qmi8658_t *device, const board_sensors_i2c_t *bus);
/* 使用板级探测得到的 0x6A 或 0x6B 初始化 QMI8658；其它地址不发起 I2C 事务。 */
board_sensors_result_t board_qmi8658_init_with_address(
    board_qmi8658_t *device,
    const board_sensors_i2c_t *bus,
    uint8_t qmi8658_address);
/* 恢复训练采样模式；先用 CTRL9 禁用 WOM，再还原量程、ODR、LPF 与 CTRL7。 */
board_sensors_result_t board_qmi8658_set_active(board_qmi8658_t *device);
/* 配置 WOM；threshold_mg 范围 1~255 mg，blanking_samples 范围 0~63 个 21 Hz 样本。 */
board_sensors_result_t board_qmi8658_set_wake_on_motion(
    board_qmi8658_t *device,
    uint8_t threshold_mg,
    uint8_t blanking_samples);
/* 禁用 WOM、加速度和陀螺；成功后芯片不再提供数据或运动中断。 */
board_sensors_result_t board_qmi8658_power_down(board_qmi8658_t *device);
/* 读取 STATUS0、24 位芯片时间戳以及就绪的原始三轴数据。 */
board_sensors_result_t board_qmi8658_read_available(
    board_qmi8658_t *device,
    uint64_t monotonic_timestamp_us,
    board_qmi8658_frame_t *frame);
/* 验证 AXP2101 IC_TYPE 为 0x4A。 */
board_sensors_result_t board_axp2101_init(board_axp2101_t *device, const board_sensors_i2c_t *bus);
/* 读取电池连接、VBUS、充放电方向、充电阶段和 SOC。 */
board_sensors_result_t board_axp2101_read_status(board_axp2101_t *device, board_axp2101_status_t *status);
/* 读改写 COMMON_CONFIG.bit0 请求 PMIC 关闭除 VRTC 外全部电源。 */
board_sensors_result_t board_axp2101_request_shutdown(board_axp2101_t *device);
/* 探测 PCF85063 控制寄存器；该芯片没有独立芯片 ID。 */
board_sensors_result_t board_pcf85063_init(board_pcf85063_t *device, const board_sensors_i2c_t *bus);
/* 把 UTC Unix 秒转换为 2000~2099 日历和 BCD，并连续写入秒到年寄存器。 */
board_sensors_result_t board_pcf85063_write_unix(board_pcf85063_t *device, int64_t unix_seconds);
/* 读取秒到年 BCD；VL 位或非法日期存在时拒绝输出 Unix 秒。 */
board_sensors_result_t board_pcf85063_read_unix(board_pcf85063_t *device, int64_t *unix_seconds);

#ifdef ESP_PLATFORM
/* ESP-IDF 新 I2C 驱动的句柄类型只在固件构建可见。 */
#include "driver/i2c_master.h"

/* 保存 ESP-IDF 总线和三个预注册设备句柄。 */
typedef struct {
    /* 保存调用方创建的共享 I2C 主总线，不由本组件删除。 */
    i2c_master_bus_handle_t bus_handle;
    /* 保存 QMI8658 设备句柄。 */
    i2c_master_dev_handle_t qmi8658_handle;
    /* 保存注册到 ESP-IDF 总线的 QMI8658 实际 7 位地址；合法值仅为 0x6A 或 0x6B。 */
    uint8_t qmi8658_address;
    /* 保存 AXP2101 设备句柄。 */
    i2c_master_dev_handle_t axp2101_handle;
    /* 保存 PCF85063 设备句柄。 */
    i2c_master_dev_handle_t pcf85063_handle;
    /* 保存供核心驱动注入的回调表。 */
    board_sensors_i2c_t port;
} board_sensors_esp_idf_i2c_t;

/* 在已有 ESP-IDF I2C 主总线上注册三个 400 kHz 设备并生成可注入端口。 */
board_sensors_result_t board_sensors_esp_idf_i2c_init(
    board_sensors_esp_idf_i2c_t *adapter,
    i2c_master_bus_handle_t bus_handle);
/* 使用板级探测得到的 QMI8658 地址注册三个 400 kHz 设备；其它地址直接拒绝。 */
board_sensors_result_t board_sensors_esp_idf_i2c_init_with_qmi_address(
    board_sensors_esp_idf_i2c_t *adapter,
    i2c_master_bus_handle_t bus_handle,
    uint8_t qmi8658_address);
/* 删除三个设备句柄；共享主总线仍由调用方管理。 */
void board_sensors_esp_idf_i2c_deinit(board_sensors_esp_idf_i2c_t *adapter);
#endif

#ifdef __cplusplus
}
#endif

#endif
