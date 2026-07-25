/* 引入板载 QMI8658、AXP2101 与 PCF85063 的公共合同。 */
#include "board_sensors.h"

/* 引入整数范围常量，检查 Unix 秒和有符号原始值。 */
#include <limits.h>
/* 引入内存清零，保证错误返回时输出没有旧数据。 */
#include <string.h>

/* QMI8658 WHO_AM_I 寄存器地址。 */
#define QMI_REG_WHO_AM_I (0x00U)
/* QMI8658 CTRL1 寄存器地址。 */
#define QMI_REG_CTRL1 (0x02U)
/* QMI8658 CTRL2 寄存器地址。 */
#define QMI_REG_CTRL2 (0x03U)
/* QMI8658 CTRL3 寄存器地址。 */
#define QMI_REG_CTRL3 (0x04U)
/* QMI8658 CTRL5 寄存器地址。 */
#define QMI_REG_CTRL5 (0x06U)
/* QMI8658 CTRL7 寄存器地址。 */
#define QMI_REG_CTRL7 (0x08U)
/* QMI8658 CTRL9 命令寄存器地址；命令完成需通过 STATUSINT.bit7 握手。 */
#define QMI_REG_CTRL9 (0x0AU)
/* QMI8658 CAL1_L 在 WOM 命令中保存 1 mg/LSB 的运动阈值。 */
#define QMI_REG_CAL1_L (0x0BU)
/* QMI8658 CAL1_H.bits7:6 选择中断脚/初始电平，bits5:0 保存消隐样本数。 */
#define QMI_REG_CAL1_H (0x0CU)
/* QMI8658 STATUSINT 寄存器地址；bit7 表示 CTRL9 命令完成。 */
#define QMI_REG_STATUSINT (0x2DU)
/* QMI8658 STATUS0 寄存器地址。 */
#define QMI_REG_STATUS0 (0x2EU)
/* QMI8658 STATUS1 寄存器地址；读取可清除已锁存的 WOM 事件。 */
#define QMI_REG_STATUS1 (0x2FU)
/* QMI8658 三字节时间戳起始寄存器地址。 */
#define QMI_REG_TIMESTAMP_L (0x30U)
/* QMI8658 三轴加速度起始寄存器地址。 */
#define QMI_REG_AX_L (0x35U)
/* QMI8658 三轴陀螺仪起始寄存器地址。 */
#define QMI_REG_GX_L (0x3BU)
/* QMI8658 复位完成结果寄存器地址。 */
#define QMI_REG_RESET_RESULT (0x4DU)
/* QMI8658 软复位寄存器地址。 */
#define QMI_REG_RESET (0x60U)
/* QMI8658 软复位命令值。 */
#define QMI_RESET_COMMAND (0xB0U)
/* QMI8658 复位完成固定响应。 */
#define QMI_RESET_COMPLETE (0x80U)
/* STATUS0.bit0 表示加速度样本可用。 */
#define QMI_STATUS_ACCEL_AVAILABLE (0x01U)
/* STATUS0.bit1 表示陀螺仪样本可用。 */
#define QMI_STATUS_GYRO_AVAILABLE (0x02U)
/* CTRL1.bit6 打开地址自增，bit3 打开 INT1。 */
#define QMI_CTRL1_AUTO_INCREMENT_AND_INT1 (0x48U)
/* CTRL1.bit5 为大端选择；清零后数据使用厂家例程的小端格式。 */
#define QMI_CTRL1_BIG_ENDIAN_MASK (0x20U)
/* CTRL2：8 g 范围编码 2 与 125 Hz ODR 编码 6。 */
#define QMI_CTRL2_8G_125HZ (0x26U)
/* CTRL2：2 g 范围编码 0 与 21 Hz 低功耗 ODR 编码 0x0D，供 WOM 待机。 */
#define QMI_CTRL2_2G_21HZ_LOW_POWER (0x0DU)
/* CTRL3：1024 deg/s 范围编码 6 与 112.1 Hz ODR 编码 6。 */
#define QMI_CTRL3_1024DPS_112HZ (0x66U)
/* CTRL7：bit0 加速度使能，bit1 陀螺使能，bit7=0 保持异步。 */
#define QMI_CTRL7_ACCEL_GYRO_ASYNC (0x03U)
/* CTRL7.bit0 单独启用加速度；WOM 不运行陀螺以降低待机电流。 */
#define QMI_CTRL7_ACCEL_ONLY (0x01U)
/* CTRL7=0 在改量程/ODR和关闭模式前停止两个传感器。 */
#define QMI_CTRL7_SENSORS_DISABLED (0x00U)
/* CTRL9 命令 0x08 把 CAL1_L/CAL1_H 写入 WOM 状态机；阈值零表示禁用 WOM。 */
#define QMI_CTRL9_CONFIGURE_WOM (0x08U)
/* CTRL9 写零确认命令完成，随后 STATUSINT.bit7 必须清零。 */
#define QMI_CTRL9_ACKNOWLEDGE (0x00U)
/* STATUSINT.bit7 表示 CTRL9 命令执行完成。 */
#define QMI_STATUSINT_COMMAND_DONE (0x80U)
/* CAL1_H.bits7:6=00 选择 INT1 且初始输出低；运动事件使 GPIO21 翻为高。 */
#define QMI_WOM_INT1_INITIAL_LOW (0x00U)
/* CAL1_H 低六位最多表示 63 个低功耗加速度样本。 */
#define QMI_WOM_BLANKING_MASK (0x3FU)
/* CTRL9 每毫秒轮询一次，最多等待 100 ms；超时拒绝把半配置芯片当作唤醒源。 */
#define QMI_CTRL9_TIMEOUT_MS (100U)

/* AXP2101 STATUS1 寄存器地址。 */
#define AXP_REG_STATUS1 (0x00U)
/* AXP2101 IC_TYPE 寄存器地址。 */
#define AXP_REG_IC_TYPE (0x03U)
/* AXP2101 COMMON_CONFIG 寄存器地址。 */
#define AXP_REG_COMMON_CONFIG (0x10U)
/* AXP2101 BAT_PERCENT_DATA 寄存器地址。 */
#define AXP_REG_BATTERY_PERCENT (0xA4U)
/* STATUS1.bit5 表示 VBUS good。 */
#define AXP_STATUS1_VBUS_GOOD (0x20U)
/* STATUS1.bit3 表示电池连接。 */
#define AXP_STATUS1_BATTERY_PRESENT (0x08U)
/* STATUS2.bits7:5 描述充电、放电或待机方向。 */
#define AXP_STATUS2_FLOW_SHIFT (5U)
/* 方向编码 1 表示充电。 */
#define AXP_STATUS2_FLOW_CHARGING (1U)
/* 方向编码 2 表示放电。 */
#define AXP_STATUS2_FLOW_DISCHARGING (2U)
/* STATUS2 低三位描述充电阶段。 */
#define AXP_STATUS2_CHARGE_PHASE_MASK (0x07U)
/* COMMON_CONFIG.bit0 请求关闭除 VRTC 外所有电源。 */
#define AXP_COMMON_CONFIG_SHUTDOWN (0x01U)

/* PCF85063 CTRL1 寄存器地址。 */
#define PCF_REG_CTRL1 (0x00U)
/* PCF85063 CTRL2 寄存器地址。 */
#define PCF_REG_CTRL2 (0x01U)
/* PCF85063 秒寄存器起始地址。 */
#define PCF_REG_SECONDS (0x04U)
/* 秒寄存器 bit7 为 VL，置位表示时钟完整性不能保证。 */
#define PCF_SECONDS_VL (0x80U)
/* CTRL1.bit5 为 STOP，清零表示时钟运行。 */
#define PCF_CTRL1_STOP (0x20U)
/* CTRL1.bit1 选择 12 小时制，清零表示 24 小时制。 */
#define PCF_CTRL1_12H (0x02U)
/* 2000-01-01 00:00:00 UTC 的 Unix 秒。 */
#define PCF_MIN_UNIX_SECONDS (INT64_C(946684800))
/* 2099-12-31 23:59:59 UTC 的 Unix 秒。 */
#define PCF_MAX_UNIX_SECONDS (INT64_C(4102444799))
/* 一天固定秒数。 */
#define SECONDS_PER_DAY (INT64_C(86400))

/* 检查公共 I2C 回调是否完整。 */
static bool board_bus_is_valid(const board_sensors_i2c_t *bus)
{
    /* 读、写和延时均为芯片初始化必要能力。 */
    return (bus != NULL) && (bus->read_register != NULL) && (bus->write_register != NULL) &&
           (bus->delay_ms != NULL);
}

/* 包装连续寄存器读取，把任意底层非零值统一成驱动错误。 */
static board_sensors_result_t board_bus_read(
    const board_sensors_i2c_t *bus,
    uint8_t address,
    uint8_t reg,
    uint8_t *data,
    size_t length)
{
    /* 参数无效时不调用外部函数指针。 */
    if (!board_bus_is_valid(bus) || (data == NULL) || (length == 0U)) {
        /* 返回参数错误。 */
        return BOARD_SENSORS_ERR_ARGUMENT;
    }
    /* 调用注入的实际或 fake 读事务。 */
    if (bus->read_register(bus->context, address, reg, data, length) != 0) {
        /* 统一映射为 I2C 错误。 */
        return BOARD_SENSORS_ERR_IO;
    }
    /* 返回成功。 */
    return BOARD_SENSORS_OK;
}

/* 包装连续寄存器写入。 */
static board_sensors_result_t board_bus_write(
    const board_sensors_i2c_t *bus,
    uint8_t address,
    uint8_t reg,
    const uint8_t *data,
    size_t length)
{
    /* 参数无效时拒绝事务。 */
    if (!board_bus_is_valid(bus) || (data == NULL) || (length == 0U)) {
        /* 返回参数错误。 */
        return BOARD_SENSORS_ERR_ARGUMENT;
    }
    /* 调用注入的实际或 fake 写事务。 */
    if (bus->write_register(bus->context, address, reg, data, length) != 0) {
        /* 统一映射为 I2C 错误。 */
        return BOARD_SENSORS_ERR_IO;
    }
    /* 返回成功。 */
    return BOARD_SENSORS_OK;
}

/* 读取单字节寄存器。 */
static board_sensors_result_t board_bus_read_u8(
    const board_sensors_i2c_t *bus,
    uint8_t address,
    uint8_t reg,
    uint8_t *value)
{
    /* 复用连续读，长度固定为 1。 */
    return board_bus_read(bus, address, reg, value, 1U);
}

/* 写入单字节寄存器。 */
static board_sensors_result_t board_bus_write_u8(
    const board_sensors_i2c_t *bus,
    uint8_t address,
    uint8_t reg,
    uint8_t value)
{
    /* 复用连续写，长度固定为 1。 */
    return board_bus_write(bus, address, reg, &value, 1U);
}

/* 按 QMI 小端格式解析一个有符号 16 位轴。 */
static int16_t qmi_decode_i16(const uint8_t *bytes)
{
    /* 先在无符号域组合，避免左移负数的未定义行为。 */
    uint16_t combined = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U);
    /* 按二补码位模式转换为有符号值。 */
    return (int16_t)combined;
}

/* 检查 QMI8658 地址是否为芯片数据手册允许的两个 7 位地址之一。 */
static bool board_qmi8658_address_is_valid(uint8_t address)
{
    /* SA0 地址脚只允许选择 0x6A 或 0x6B，不接受其它总线设备地址。 */
    return (address == BOARD_QMI8658_I2C_ADDRESS_LOW) || (address == BOARD_QMI8658_I2C_ADDRESS_HIGH);
}

/* 检查 QMI 驱动已完成身份、复位和至少一次完整模式配置。 */
static bool board_qmi8658_is_ready(const board_qmi8658_t *device)
{
    /* 设备、端口和初始化标志必须同时有效，避免通过空函数指针访问 I2C。 */
    return (device != NULL) && device->initialized && board_bus_is_valid(device->bus) &&
           board_qmi8658_address_is_valid(device->address);
}

/* 执行一个 QMI CTRL9 命令，并按数据手册完成置位等待、写零 ACK 与清零等待。 */
static board_sensors_result_t qmi_execute_ctrl9_command(
    board_qmi8658_t *device,
    uint8_t command)
{
    /* 调用方必须提供已初始化设备；公开 API 在进入此函数前统一检查。 */
    if (!board_qmi8658_is_ready(device)) {
        /* 未初始化或地址损坏返回状态错误，不发起寄存器事务。 */
        return BOARD_SENSORS_ERR_STATE;
    }
    /* 写入非零 CTRL9 命令，启动芯片内部 WOM 参数装载。 */
    board_sensors_result_t result = board_bus_write_u8(
        device->bus,
        device->address,
        QMI_REG_CTRL9,
        command);
    /* I2C 写失败时不能继续等待虚假的完成位。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传总线错误。 */
        return result;
    }
    /* 保存 STATUSINT 快照；bit7 以外位不参与本命令握手。 */
    uint8_t status_int = 0U;
    /* 标记命令完成位是否在 100 ms 窗口内出现。 */
    bool command_done = false;
    /* 每 1 ms 读取一次 STATUSINT，最多执行 QMI_CTRL9_TIMEOUT_MS 次。 */
    for (uint32_t elapsed_ms = 0U; elapsed_ms < QMI_CTRL9_TIMEOUT_MS; ++elapsed_ms) {
        /* 读取 STATUSINT，输入输出均为一个无符号字节。 */
        result = board_bus_read_u8(device->bus, device->address, QMI_REG_STATUSINT, &status_int);
        /* 总线错误立即退出，避免把上一次状态误判为完成。 */
        if (result != BOARD_SENSORS_OK) {
            /* 透传读错误。 */
            return result;
        }
        /* bit7 置位表示芯片已执行当前 CTRL9 命令。 */
        if ((status_int & QMI_STATUSINT_COMMAND_DONE) != 0U) {
            /* 记录已完成并结束置位等待。 */
            command_done = true;
            /* 跳出轮询。 */
            break;
        }
        /* 未完成时等待 1 ms，限制 I2C 和 CPU 占用。 */
        device->bus->delay_ms(device->bus->context, 1U);
    }
    /* 100 ms 内没有完成位表示芯片状态不可信。 */
    if (!command_done) {
        /* 返回状态错误；调用方保持 OFF，不允许进入 Deep-sleep。 */
        return BOARD_SENSORS_ERR_STATE;
    }
    /* 写 CTRL9=0 确认已消费命令完成事件。 */
    result = board_bus_write_u8(
        device->bus,
        device->address,
        QMI_REG_CTRL9,
        QMI_CTRL9_ACKNOWLEDGE);
    /* ACK 写失败时芯片可能保持命令完成锁存，必须返回错误。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传写错误。 */
        return result;
    }
    /* 标记 ACK 后完成位是否已清零。 */
    bool acknowledge_cleared = false;
    /* 每 1 ms 轮询 STATUSINT，确认芯片接受 ACK。 */
    for (uint32_t elapsed_ms = 0U; elapsed_ms < QMI_CTRL9_TIMEOUT_MS; ++elapsed_ms) {
        /* 读取 ACK 后的 STATUSINT。 */
        result = board_bus_read_u8(device->bus, device->address, QMI_REG_STATUSINT, &status_int);
        /* I2C 失败时不能宣称命令握手完成。 */
        if (result != BOARD_SENSORS_OK) {
            /* 透传读错误。 */
            return result;
        }
        /* bit7 清零表示 ACK 已完成。 */
        if ((status_int & QMI_STATUSINT_COMMAND_DONE) == 0U) {
            /* 记录清零并退出。 */
            acknowledge_cleared = true;
            /* 跳出轮询。 */
            break;
        }
        /* 尚未清零时等待 1 ms，避免忙轮询。 */
        device->bus->delay_ms(device->bus->context, 1U);
    }
    /* ACK 后仍未清零表示芯片内部命令状态异常。 */
    if (!acknowledge_cleared) {
        /* 返回状态错误。 */
        return BOARD_SENSORS_ERR_STATE;
    }
    /* 命令和 ACK 两阶段均完成。 */
    return BOARD_SENSORS_OK;
}

/* 禁用 WOM；CTRL7 先停传感器，再把阈值写零并执行同一 CTRL9 装载命令。 */
static board_sensors_result_t qmi_disable_wake_on_motion(board_qmi8658_t *device)
{
    /* 先禁用加速度与陀螺，避免 CAL1 参数更新期间产生不完整事件。 */
    board_sensors_result_t result = board_bus_write_u8(
        device->bus,
        device->address,
        QMI_REG_CTRL7,
        QMI_CTRL7_SENSORS_DISABLED);
    /* 第一步失败时停止转换。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传 I2C 错误。 */
        return result;
    }
    /* 从此处开始驱动只把设备视为 OFF，后续任一步失败都不会伪报 ACTIVE/WOM。 */
    device->mode = BOARD_QMI8658_MODE_OFF;
    /* CAL1_L=0 按数据手册禁用 WOM 阈值比较器。 */
    result = board_bus_write_u8(device->bus, device->address, QMI_REG_CAL1_L, 0U);
    /* 写失败时保持 OFF。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传 I2C 错误。 */
        return result;
    }
    /* 清除中断选择与消隐字段，保证下次 WOM 从确定状态开始。 */
    result = board_bus_write_u8(device->bus, device->address, QMI_REG_CAL1_H, 0U);
    /* 写失败时保持 OFF。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传 I2C 错误。 */
        return result;
    }
    /* 通过 CTRL9=0x08 把零阈值装载到 WOM 状态机。 */
    return qmi_execute_ctrl9_command(device, QMI_CTRL9_CONFIGURE_WOM);
}

/* 恢复训练部署所需的活动量程、ODR、滤波和双传感器异步采样。 */
board_sensors_result_t board_qmi8658_set_active(board_qmi8658_t *device)
{
    /* 未初始化设备不允许切换模式。 */
    if (!board_qmi8658_is_ready(device)) {
        /* 返回状态错误。 */
        return BOARD_SENSORS_ERR_STATE;
    }
    /* 先用零阈值 CTRL9 命令关闭 WOM，并保持 CTRL7=0。 */
    board_sensors_result_t result = qmi_disable_wake_on_motion(device);
    /* 禁用 WOM 失败时保持 OFF。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 读取 CTRL1 以保留未由项目管理的芯片控制位。 */
    uint8_t ctrl1 = 0U;
    /* 执行 CTRL1 读取。 */
    result = board_bus_read_u8(device->bus, device->address, QMI_REG_CTRL1, &ctrl1);
    /* 读取失败时保持 OFF。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 清大端位，并确保地址自增与 INT1 输出功能开启。 */
    ctrl1 = (uint8_t)((ctrl1 & (uint8_t)(~QMI_CTRL1_BIG_ENDIAN_MASK)) | QMI_CTRL1_AUTO_INCREMENT_AND_INT1);
    /* 写回活动模式 CTRL1。 */
    result = board_bus_write_u8(device->bus, device->address, QMI_REG_CTRL1, ctrl1);
    /* 写失败时保持 OFF。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 还原加速度正负 8 g、125 Hz，匹配训练量程和重采样器。 */
    result = board_bus_write_u8(device->bus, device->address, QMI_REG_CTRL2, QMI_CTRL2_8G_125HZ);
    /* 写失败时保持 OFF。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 还原陀螺正负 1024 deg/s、112.1 Hz。 */
    result = board_bus_write_u8(device->bus, device->address, QMI_REG_CTRL3, QMI_CTRL3_1024DPS_112HZ);
    /* 写失败时保持 OFF。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 关闭芯片内部 LPF，继续由统一 8 Hz 软件链处理。 */
    result = board_bus_write_u8(device->bus, device->address, QMI_REG_CTRL5, 0U);
    /* 写失败时保持 OFF。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 最后同时启用加速度与陀螺，避免上层读到半配置数据。 */
    result = board_bus_write_u8(device->bus, device->address, QMI_REG_CTRL7, QMI_CTRL7_ACCEL_GYRO_ASYNC);
    /* 写失败时保持 OFF。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 只有全部寄存器写成功后才公开 ACTIVE。 */
    device->mode = BOARD_QMI8658_MODE_ACTIVE;
    /* 返回成功。 */
    return BOARD_SENSORS_OK;
}

/* 配置仅加速度 WOM；阈值单位 mg，消隐单位为 21 Hz 低功耗加速度样本。 */
board_sensors_result_t board_qmi8658_set_wake_on_motion(
    board_qmi8658_t *device,
    uint8_t threshold_mg,
    uint8_t blanking_samples)
{
    /* 驱动必须初始化，阈值零会禁用 WOM，消隐只能占 CAL1_H 低六位。 */
    if (!board_qmi8658_is_ready(device) || (threshold_mg == 0U) ||
        (blanking_samples > QMI_WOM_BLANKING_MASK)) {
        /* 未初始化属于状态错误；非法阈值或消隐属于参数错误。 */
        return board_qmi8658_is_ready(device) ? BOARD_SENSORS_ERR_ARGUMENT : BOARD_SENSORS_ERR_STATE;
    }
    /* 第一步禁用两路传感器，保证后续量程和 WOM 参数原子生效。 */
    board_sensors_result_t result = board_bus_write_u8(
        device->bus,
        device->address,
        QMI_REG_CTRL7,
        QMI_CTRL7_SENSORS_DISABLED);
    /* 写失败时保留旧模式，因为硬件尚未确认关闭。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* CTRL7 已确认关闭后先标记 OFF；后续半配置不可作为 Deep-sleep 唤醒源。 */
    device->mode = BOARD_QMI8658_MODE_OFF;
    /* 配置正负 2 g 与 21 Hz 低功耗 ODR，提高轻微抬腕检测灵敏度并降低电流。 */
    result = board_bus_write_u8(device->bus, device->address, QMI_REG_CTRL2, QMI_CTRL2_2G_21HZ_LOW_POWER);
    /* 写失败时保持 OFF。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 写入 1 mg/LSB 阈值；40 表示 40 mg。 */
    result = board_bus_write_u8(device->bus, device->address, QMI_REG_CAL1_L, threshold_mg);
    /* 写失败时保持 OFF。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 选择 INT1 初始低电平，并把 0~63 消隐样本写入低六位。 */
    const uint8_t cal1_high = (uint8_t)(QMI_WOM_INT1_INITIAL_LOW | (blanking_samples & QMI_WOM_BLANKING_MASK));
    /* 写入中断路由与消隐参数。 */
    result = board_bus_write_u8(device->bus, device->address, QMI_REG_CAL1_H, cal1_high);
    /* 写失败时保持 OFF。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 执行 CTRL9 WOM 装载命令，并完成 STATUSINT 双阶段握手。 */
    result = qmi_execute_ctrl9_command(device, QMI_CTRL9_CONFIGURE_WOM);
    /* 命令超时或 I2C 失败时保持 OFF，主入口必须拒绝 Deep-sleep。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 仅开启加速度；陀螺保持关闭。 */
    result = board_bus_write_u8(device->bus, device->address, QMI_REG_CTRL7, QMI_CTRL7_ACCEL_ONLY);
    /* 启用失败时保持 OFF。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 读取 STATUS1 清除配置前遗留 WOM 锁存；返回值不解释，只执行清事件副作用。 */
    uint8_t status1 = 0U;
    /* 执行单字节读取。 */
    result = board_bus_read_u8(device->bus, device->address, QMI_REG_STATUS1, &status1);
    /* 清事件失败时不允许把 GPIO21 当作可信唤醒源。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 显式丢弃 STATUS1 位值；本接口只依赖读取副作用。 */
    (void)status1;
    /* 全部配置和清事件成功后才标记 WOM。 */
    device->mode = BOARD_QMI8658_MODE_WAKE_ON_MOTION;
    /* 返回成功。 */
    return BOARD_SENSORS_OK;
}

/* 关闭 QMI8658 两路传感器及 WOM。 */
board_sensors_result_t board_qmi8658_power_down(board_qmi8658_t *device)
{
    /* 未初始化设备不能关闭寄存器。 */
    if (!board_qmi8658_is_ready(device)) {
        /* 返回状态错误。 */
        return BOARD_SENSORS_ERR_STATE;
    }
    /* 零阈值 WOM 命令已包含 CTRL7=0，并在成功前保持模式 OFF。 */
    return qmi_disable_wake_on_motion(device);
}

/* 使用板级探测地址初始化 QMI8658，并写入与现有重采样器匹配的异步 ODR。 */
board_sensors_result_t board_qmi8658_init_with_address(
    board_qmi8658_t *device,
    const board_sensors_i2c_t *bus,
    uint8_t qmi8658_address)
{
    /* 空设备或不完整端口不能初始化。 */
    if ((device == NULL) || !board_bus_is_valid(bus)) {
        /* 返回参数错误。 */
        return BOARD_SENSORS_ERR_ARGUMENT;
    }
    /* 清空旧状态，失败后 initialized 保持 false。 */
    (void)memset(device, 0, sizeof(*device));
    /* 地址不是 0x6A/0x6B 时在发起任何 I2C 事务前拒绝。 */
    if (!board_qmi8658_address_is_valid(qmi8658_address)) {
        /* 返回参数错误；设备已清零，不会继续使用上次成功状态。 */
        return BOARD_SENSORS_ERR_ARGUMENT;
    }
    /* 保存共享端口。 */
    device->bus = bus;
    /* 保存板级运行时探测到的实际地址，后续全部读写都复用该值。 */
    device->address = qmi8658_address;
    /* 读取身份寄存器。 */
    uint8_t who_am_i = 0U;
    /* I2C 失败时直接返回。 */
    board_sensors_result_t result = board_bus_read_u8(bus, device->address, QMI_REG_WHO_AM_I, &who_am_i);
    /* 检查事务结果。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传 I2C 错误。 */
        return result;
    }
    /* 验证目标确为 QMI8658。 */
    if (who_am_i != BOARD_QMI8658_WHO_AM_I) {
        /* 返回芯片身份错误。 */
        return BOARD_SENSORS_ERR_CHIP_ID;
    }
    /* 发送厂家定义的 0xB0 软复位。 */
    result = board_bus_write_u8(bus, device->address, QMI_REG_RESET, QMI_RESET_COMMAND);
    /* 写失败时退出。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传 I2C 错误。 */
        return result;
    }
    /* 等待超过数据手册最大约 15 ms 的复位时间。 */
    bus->delay_ms(bus->context, 20U);
    /* 读取复位完成标志。 */
    uint8_t reset_result = 0U;
    /* 执行复位结果读取。 */
    result = board_bus_read_u8(bus, device->address, QMI_REG_RESET_RESULT, &reset_result);
    /* 读取失败时退出。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传 I2C 错误。 */
        return result;
    }
    /* 响应不是 0x80 表示芯片尚未完成复位。 */
    if (reset_result != QMI_RESET_COMPLETE) {
        /* 返回状态错误。 */
        return BOARD_SENSORS_ERR_STATE;
    }
    /* 读取 CTRL1，以保留厂家未涉及的控制位。 */
    uint8_t ctrl1 = 0U;
    /* 读取 CTRL1。 */
    result = board_bus_read_u8(bus, device->address, QMI_REG_CTRL1, &ctrl1);
    /* 读取失败时退出。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 清大端位，并开启地址自增与 INT1 输出。 */
    ctrl1 = (uint8_t)((ctrl1 & (uint8_t)(~QMI_CTRL1_BIG_ENDIAN_MASK)) | QMI_CTRL1_AUTO_INCREMENT_AND_INT1);
    /* 写回 CTRL1。 */
    result = board_bus_write_u8(bus, device->address, QMI_REG_CTRL1, ctrl1);
    /* 写失败时退出。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 配置加速度正负 8 g、125 Hz。 */
    result = board_bus_write_u8(bus, device->address, QMI_REG_CTRL2, QMI_CTRL2_8G_125HZ);
    /* 写失败时退出。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 配置陀螺正负 1024 deg/s、112.1 Hz。 */
    result = board_bus_write_u8(bus, device->address, QMI_REG_CTRL3, QMI_CTRL3_1024DPS_112HZ);
    /* 写失败时退出。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 关闭芯片内部 LPF，避免与 imu_pipeline 的统一 8 Hz 滤波重复。 */
    result = board_bus_write_u8(bus, device->address, QMI_REG_CTRL5, 0x00U);
    /* 写失败时退出。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 同时启用加速度和陀螺，并保持异步采样。 */
    result = board_bus_write_u8(bus, device->address, QMI_REG_CTRL7, QMI_CTRL7_ACCEL_GYRO_ASYNC);
    /* 写失败时退出。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 标记配置完成。 */
    device->initialized = true;
    /* 初始化寄存器已完整进入训练采样模式。 */
    device->mode = BOARD_QMI8658_MODE_ACTIVE;
    /* 返回成功。 */
    return BOARD_SENSORS_OK;
}

/* 使用兼容默认高地址 0x6B 初始化 QMI8658。 */
board_sensors_result_t board_qmi8658_init(board_qmi8658_t *device, const board_sensors_i2c_t *bus)
{
    /* 转调显式地址接口，保持全部旧调用方和厂家例程行为不变。 */
    return board_qmi8658_init_with_address(device, bus, BOARD_QMI8658_I2C_ADDRESS);
}

/* 读取 QMI8658 当前就绪的异步原始帧。 */
board_sensors_result_t board_qmi8658_read_available(
    board_qmi8658_t *device,
    uint64_t monotonic_timestamp_us,
    board_qmi8658_frame_t *frame)
{
    /* 输出指针和设备指针不能为空。 */
    if ((device == NULL) || (frame == NULL)) {
        /* 返回参数错误。 */
        return BOARD_SENSORS_ERR_ARGUMENT;
    }
    /* 未初始化设备不能访问寄存器。 */
    if (!device->initialized || !board_bus_is_valid(device->bus) ||
        (device->mode != BOARD_QMI8658_MODE_ACTIVE)) {
        /* 返回状态错误。 */
        return BOARD_SENSORS_ERR_STATE;
    }
    /* 先清空输出，任何后续失败都不会泄漏旧样本。 */
    (void)memset(frame, 0, sizeof(*frame));
    /* 保存调用方在 GPIO21 中断或轮询处捕获的单调微秒。 */
    frame->timestamp_us = monotonic_timestamp_us;
    /* 读取异步可用位。 */
    uint8_t status0 = 0U;
    /* 执行 STATUS0 读取。 */
    board_sensors_result_t result = board_bus_read_u8(device->bus, device->address, QMI_REG_STATUS0, &status0);
    /* 读取失败时退出。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 读取 24 位芯片原始时间戳。 */
    uint8_t timestamp_bytes[3] = {0U, 0U, 0U};
    /* 执行连续三字节读取。 */
    result = board_bus_read(device->bus, device->address, QMI_REG_TIMESTAMP_L, timestamp_bytes, 3U);
    /* 读取失败时退出。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 按小端组合 24 位计数，不对未核实 tick 单位作假设。 */
    frame->sensor_timestamp_ticks = (uint32_t)timestamp_bytes[0] | ((uint32_t)timestamp_bytes[1] << 8U) |
                                    ((uint32_t)timestamp_bytes[2] << 16U);
    /* 提取加速度就绪位。 */
    frame->accel_available = (status0 & QMI_STATUS_ACCEL_AVAILABLE) != 0U;
    /* 提取陀螺仪就绪位。 */
    frame->gyro_available = (status0 & QMI_STATUS_GYRO_AVAILABLE) != 0U;
    /* 加速度就绪时读取 6 个小端字节。 */
    if (frame->accel_available) {
        /* 保存 ax、ay、az 原始字节。 */
        uint8_t raw_accel[6] = {0U};
        /* 连续读取三轴。 */
        result = board_bus_read(device->bus, device->address, QMI_REG_AX_L, raw_accel, sizeof(raw_accel));
        /* 读取失败时退出。 */
        if (result != BOARD_SENSORS_OK) {
            /* 清空可用位，防止调用方消费半帧。 */
            frame->accel_available = false;
            /* 返回错误。 */
            return result;
        }
        /* 解析 ax。 */
        frame->accel_raw[0] = qmi_decode_i16(&raw_accel[0]);
        /* 解析 ay。 */
        frame->accel_raw[1] = qmi_decode_i16(&raw_accel[2]);
        /* 解析 az。 */
        frame->accel_raw[2] = qmi_decode_i16(&raw_accel[4]);
    }
    /* 陀螺就绪时读取 6 个小端字节。 */
    if (frame->gyro_available) {
        /* 保存 gx、gy、gz 原始字节。 */
        uint8_t raw_gyro[6] = {0U};
        /* 连续读取三轴。 */
        result = board_bus_read(device->bus, device->address, QMI_REG_GX_L, raw_gyro, sizeof(raw_gyro));
        /* 读取失败时退出。 */
        if (result != BOARD_SENSORS_OK) {
            /* 清空可用位，防止调用方消费半帧。 */
            frame->gyro_available = false;
            /* 返回错误。 */
            return result;
        }
        /* 解析 gx。 */
        frame->gyro_raw[0] = qmi_decode_i16(&raw_gyro[0]);
        /* 解析 gy。 */
        frame->gyro_raw[1] = qmi_decode_i16(&raw_gyro[2]);
        /* 解析 gz。 */
        frame->gyro_raw[2] = qmi_decode_i16(&raw_gyro[4]);
    }
    /* 返回成功；两路均未就绪也是有效轮询结果。 */
    return BOARD_SENSORS_OK;
}

/* 初始化 AXP2101 并验证 IC_TYPE。 */
board_sensors_result_t board_axp2101_init(board_axp2101_t *device, const board_sensors_i2c_t *bus)
{
    /* 检查设备和端口。 */
    if ((device == NULL) || !board_bus_is_valid(bus)) {
        /* 返回参数错误。 */
        return BOARD_SENSORS_ERR_ARGUMENT;
    }
    /* 清空旧状态。 */
    (void)memset(device, 0, sizeof(*device));
    /* 保存总线。 */
    device->bus = bus;
    /* 读取 IC_TYPE。 */
    uint8_t chip_id = 0U;
    /* 执行读取。 */
    board_sensors_result_t result = board_bus_read_u8(bus, BOARD_AXP2101_I2C_ADDRESS, AXP_REG_IC_TYPE, &chip_id);
    /* I2C 失败时退出。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 验证固定芯片 ID。 */
    if (chip_id != BOARD_AXP2101_CHIP_ID) {
        /* 返回身份错误。 */
        return BOARD_SENSORS_ERR_CHIP_ID;
    }
    /* 标记初始化完成。 */
    device->initialized = true;
    /* 返回成功。 */
    return BOARD_SENSORS_OK;
}

/* 读取 AXP2101 电池和充电状态。 */
board_sensors_result_t board_axp2101_read_status(board_axp2101_t *device, board_axp2101_status_t *status)
{
    /* 检查设备和输出。 */
    if ((device == NULL) || (status == NULL)) {
        /* 返回参数错误。 */
        return BOARD_SENSORS_ERR_ARGUMENT;
    }
    /* 检查初始化状态。 */
    if (!device->initialized || !board_bus_is_valid(device->bus)) {
        /* 返回状态错误。 */
        return BOARD_SENSORS_ERR_STATE;
    }
    /* 先清空输出。 */
    (void)memset(status, 0, sizeof(*status));
    /* 连续读取 STATUS1 和 STATUS2。 */
    uint8_t status_bytes[2] = {0U, 0U};
    /* 执行状态读取。 */
    board_sensors_result_t result =
        board_bus_read(device->bus, BOARD_AXP2101_I2C_ADDRESS, AXP_REG_STATUS1, status_bytes, sizeof(status_bytes));
    /* 读取失败时退出。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 提取电池连接位。 */
    status->battery_present = (status_bytes[0] & AXP_STATUS1_BATTERY_PRESENT) != 0U;
    /* 提取 VBUS good 位。 */
    status->vbus_good = (status_bytes[0] & AXP_STATUS1_VBUS_GOOD) != 0U;
    /* 提取充放电方向编码。 */
    uint8_t power_flow = (uint8_t)(status_bytes[1] >> AXP_STATUS2_FLOW_SHIFT);
    /* 映射充电方向。 */
    status->charging = power_flow == AXP_STATUS2_FLOW_CHARGING;
    /* 映射放电方向。 */
    status->discharging = power_flow == AXP_STATUS2_FLOW_DISCHARGING;
    /* 提取低三位充电阶段。 */
    uint8_t phase = status_bytes[1] & AXP_STATUS2_CHARGE_PHASE_MASK;
    /* 0~5 为厂家已定义阶段。 */
    status->charge_phase = phase <= (uint8_t)BOARD_AXP_CHARGE_STOPPED ? (board_axp_charge_phase_t)phase
                                                                      : BOARD_AXP_CHARGE_UNKNOWN;
    /* 电池连接时才读取燃料计百分比。 */
    if (status->battery_present) {
        /* 保存原始百分比。 */
        uint8_t soc = 0U;
        /* 读取 BAT_PERCENT_DATA。 */
        result = board_bus_read_u8(device->bus, BOARD_AXP2101_I2C_ADDRESS, AXP_REG_BATTERY_PERCENT, &soc);
        /* 读取失败时退出。 */
        if (result != BOARD_SENSORS_OK) {
            /* 透传错误。 */
            return result;
        }
        /* 百分比超过 100 表示通信或燃料计状态异常。 */
        if (soc > 100U) {
            /* 返回范围错误。 */
            return BOARD_SENSORS_ERR_RANGE;
        }
        /* 保存有效 SOC。 */
        status->soc_percent = soc;
    }
    /* 返回成功。 */
    return BOARD_SENSORS_OK;
}

/* 请求 AXP2101 软关机。 */
board_sensors_result_t board_axp2101_request_shutdown(board_axp2101_t *device)
{
    /* 检查设备指针。 */
    if (device == NULL) {
        /* 返回参数错误。 */
        return BOARD_SENSORS_ERR_ARGUMENT;
    }
    /* 只有初始化完成后才允许破坏性电源命令。 */
    if (!device->initialized || !board_bus_is_valid(device->bus)) {
        /* 返回状态错误。 */
        return BOARD_SENSORS_ERR_STATE;
    }
    /* 读取 COMMON_CONFIG，保留复位和放电等其它位。 */
    uint8_t common_config = 0U;
    /* 执行读取。 */
    board_sensors_result_t result =
        board_bus_read_u8(device->bus, BOARD_AXP2101_I2C_ADDRESS, AXP_REG_COMMON_CONFIG, &common_config);
    /* 读取失败时退出。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 仅置位 shutdown bit0。 */
    common_config |= AXP_COMMON_CONFIG_SHUTDOWN;
    /* 写回 COMMON_CONFIG；成功后硬件可能立即掉电。 */
    return board_bus_write_u8(device->bus, BOARD_AXP2101_I2C_ADDRESS, AXP_REG_COMMON_CONFIG, common_config);
}

/* 判断公历闰年。 */
static bool calendar_is_leap_year(int year)
{
    /* 400 整除为闰年；100 整除不是；其余 4 整除是。 */
    return ((year % 400) == 0) || (((year % 4) == 0) && ((year % 100) != 0));
}

/* 返回指定月份天数；参数已由调用方校验。 */
static uint8_t calendar_days_in_month(int year, unsigned int month)
{
    /* 保存平年每月天数。 */
    static const uint8_t days[12] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
    /* 二月闰年增加一天。 */
    if ((month == 2U) && calendar_is_leap_year(year)) {
        /* 返回闰年二月 29 天。 */
        return 29U;
    }
    /* 返回表中月份天数。 */
    return days[month - 1U];
}

/* 把公历日期转换为相对 1970-01-01 的天数，使用 Howard Hinnant 整数算法。 */
static int64_t calendar_days_from_civil(int year, unsigned int month, unsigned int day)
{
    /* 把一、二月视为上一公历年的第 11、12 月，简化闰日公式。 */
    year -= month <= 2U ? 1 : 0;
    /* 计算 400 年周期编号，负年份也使用向下取整。 */
    int era = (year >= 0 ? year : year - 399) / 400;
    /* 计算当前周期内年份 0~399。 */
    unsigned int year_of_era = (unsigned int)(year - era * 400);
    /* 把月份改写为三月=0、四月=1、……、二月=11，避免无符号负常量。 */
    unsigned int march_based_month = month > 2U ? month - 3U : month + 9U;
    /* 计算三月起算的年内日序。 */
    unsigned int day_of_year = (153U * march_based_month + 2U) / 5U + day - 1U;
    /* 计算周期内总日序，包含闰年修正。 */
    unsigned int day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    /* 719468 把算法纪元平移到 1970-01-01。 */
    return (int64_t)era * INT64_C(146097) + (int64_t)day_of_era - INT64_C(719468);
}

/* 把相对 1970-01-01 天数转换为公历日期。 */
static void calendar_civil_from_days(
    int64_t days_since_epoch,
    int *year,
    unsigned int *month,
    unsigned int *day)
{
    /* 平移到算法使用的三月纪元。 */
    int64_t z = days_since_epoch + INT64_C(719468);
    /* 计算 400 年周期编号。 */
    int64_t era = (z >= 0 ? z : z - INT64_C(146096)) / INT64_C(146097);
    /* 计算周期内日序 0~146096。 */
    unsigned int day_of_era = (unsigned int)(z - era * INT64_C(146097));
    /* 反解周期内年份。 */
    unsigned int year_of_era =
        (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
    /* 组合初始年份。 */
    int resolved_year = (int)year_of_era + (int)(era * 400);
    /* 计算年内日序。 */
    unsigned int day_of_year = day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    /* 反解三月起算月份。 */
    unsigned int month_prime = (5U * day_of_year + 2U) / 153U;
    /* 反解日。 */
    unsigned int resolved_day = day_of_year - (153U * month_prime + 2U) / 5U + 1U;
    /* 转回一月至十二月，避免依赖无符号回绕。 */
    unsigned int resolved_month = month_prime < 10U ? month_prime + 3U : month_prime - 9U;
    /* 一、二月属于下一公历年。 */
    resolved_year += resolved_month <= 2U ? 1 : 0;
    /* 输出年份。 */
    *year = resolved_year;
    /* 输出月份。 */
    *month = resolved_month;
    /* 输出日期。 */
    *day = resolved_day;
}

/* 把 0~99 十进制转为压缩 BCD。 */
static uint8_t calendar_to_bcd(unsigned int value)
{
    /* 十位放高半字节，个位放低半字节。 */
    return (uint8_t)(((value / 10U) << 4U) | (value % 10U));
}

/* 校验并解码一个已掩码 BCD 字段。 */
static bool calendar_from_bcd(uint8_t bcd, uint8_t maximum, uint8_t *value)
{
    /* 输出不能为空。 */
    if (value == NULL) {
        /* 返回失败。 */
        return false;
    }
    /* 分离高低十进制位。 */
    uint8_t high = (uint8_t)((bcd >> 4U) & 0x0FU);
    /* 读取低十进制位。 */
    uint8_t low = (uint8_t)(bcd & 0x0FU);
    /* 任一半字节大于 9 都不是合法 BCD。 */
    if ((high > 9U) || (low > 9U)) {
        /* 返回失败。 */
        return false;
    }
    /* 组合十进制值。 */
    uint8_t decoded = (uint8_t)(high * 10U + low);
    /* 检查字段上限。 */
    if (decoded > maximum) {
        /* 返回失败。 */
        return false;
    }
    /* 写入输出。 */
    *value = decoded;
    /* 返回成功。 */
    return true;
}

/* 初始化 PCF85063；该芯片无 ID，因此仅验证控制寄存器可读并强制 24 小时运行模式。 */
board_sensors_result_t board_pcf85063_init(board_pcf85063_t *device, const board_sensors_i2c_t *bus)
{
    /* 检查设备和端口。 */
    if ((device == NULL) || !board_bus_is_valid(bus)) {
        /* 返回参数错误。 */
        return BOARD_SENSORS_ERR_ARGUMENT;
    }
    /* 清空旧状态。 */
    (void)memset(device, 0, sizeof(*device));
    /* 保存总线。 */
    device->bus = bus;
    /* 连续读取 CTRL1 和 CTRL2，收到 ACK 证明 0x51 设备可访问。 */
    uint8_t controls[2] = {0U, 0U};
    /* 执行控制寄存器读取。 */
    board_sensors_result_t result =
        board_bus_read(bus, BOARD_PCF85063_I2C_ADDRESS, PCF_REG_CTRL1, controls, sizeof(controls));
    /* 读取失败时退出。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* 清 12 小时位和 STOP 位，保证后续按 24 小时制读取且振荡器运行。 */
    uint8_t configured_ctrl1 = (uint8_t)(controls[0] & (uint8_t)(~(PCF_CTRL1_STOP | PCF_CTRL1_12H)));
    /* 只有值变化时才写，减少 I2C 和 RTC 寄存器操作。 */
    if (configured_ctrl1 != controls[0]) {
        /* 写回 CTRL1。 */
        result = board_bus_write_u8(bus, BOARD_PCF85063_I2C_ADDRESS, PCF_REG_CTRL1, configured_ctrl1);
        /* 写失败时退出。 */
        if (result != BOARD_SENSORS_OK) {
            /* 透传错误。 */
            return result;
        }
    }
    /* 标记初始化完成。 */
    device->initialized = true;
    /* 返回成功。 */
    return BOARD_SENSORS_OK;
}

/* 写入 UTC Unix 秒到 PCF85063。 */
board_sensors_result_t board_pcf85063_write_unix(board_pcf85063_t *device, int64_t unix_seconds)
{
    /* 检查设备指针。 */
    if (device == NULL) {
        /* 返回参数错误。 */
        return BOARD_SENSORS_ERR_ARGUMENT;
    }
    /* 检查初始化状态。 */
    if (!device->initialized || !board_bus_is_valid(device->bus)) {
        /* 返回状态错误。 */
        return BOARD_SENSORS_ERR_STATE;
    }
    /* 项目合同只允许两位年能无歧义表示的 2000~2099。 */
    if ((unix_seconds < PCF_MIN_UNIX_SECONDS) || (unix_seconds > PCF_MAX_UNIX_SECONDS)) {
        /* 返回范围错误。 */
        return BOARD_SENSORS_ERR_RANGE;
    }
    /* 计算 UTC 整天数。 */
    int64_t days_since_epoch = unix_seconds / SECONDS_PER_DAY;
    /* 计算当天秒数。 */
    int64_t seconds_of_day = unix_seconds % SECONDS_PER_DAY;
    /* 保存年份。 */
    int year = 0;
    /* 保存月份。 */
    unsigned int month = 0U;
    /* 保存日期。 */
    unsigned int day = 0U;
    /* 反解公历年月日。 */
    calendar_civil_from_days(days_since_epoch, &year, &month, &day);
    /* 计算小时。 */
    unsigned int hour = (unsigned int)(seconds_of_day / INT64_C(3600));
    /* 去掉整小时。 */
    seconds_of_day %= INT64_C(3600);
    /* 计算分钟。 */
    unsigned int minute = (unsigned int)(seconds_of_day / INT64_C(60));
    /* 计算秒。 */
    unsigned int second = (unsigned int)(seconds_of_day % INT64_C(60));
    /* Unix epoch 星期四=4；结果规范到 Sunday=0 的 0~6。 */
    unsigned int weekday = (unsigned int)((days_since_epoch + INT64_C(4)) % INT64_C(7));
    /* 保存秒、分、时、日、星期、月、年 7 字节。 */
    uint8_t fields[7] = {0U};
    /* 秒使用 BCD，VL 位保持 0。 */
    fields[0] = calendar_to_bcd(second);
    /* 分使用 BCD。 */
    fields[1] = calendar_to_bcd(minute);
    /* 时使用 24 小时 BCD。 */
    fields[2] = calendar_to_bcd(hour);
    /* 日使用 BCD。 */
    fields[3] = calendar_to_bcd(day);
    /* 星期寄存器是 0~6 二进制，不是 BCD。 */
    fields[4] = (uint8_t)weekday;
    /* 月使用 BCD。 */
    fields[5] = calendar_to_bcd(month);
    /* 年只保存 2000 起的两位 BCD。 */
    fields[6] = calendar_to_bcd((unsigned int)(year - 2000));
    /* 连续写入 0x04~0x0A。 */
    return board_bus_write(device->bus, BOARD_PCF85063_I2C_ADDRESS, PCF_REG_SECONDS, fields, sizeof(fields));
}

/* 读取 PCF85063 并转换为 UTC Unix 秒。 */
board_sensors_result_t board_pcf85063_read_unix(board_pcf85063_t *device, int64_t *unix_seconds)
{
    /* 检查输入输出指针。 */
    if ((device == NULL) || (unix_seconds == NULL)) {
        /* 返回参数错误。 */
        return BOARD_SENSORS_ERR_ARGUMENT;
    }
    /* 检查初始化状态。 */
    if (!device->initialized || !board_bus_is_valid(device->bus)) {
        /* 返回状态错误。 */
        return BOARD_SENSORS_ERR_STATE;
    }
    /* 读取秒至年七字节。 */
    uint8_t fields[7] = {0U};
    /* 执行连续读取。 */
    board_sensors_result_t result =
        board_bus_read(device->bus, BOARD_PCF85063_I2C_ADDRESS, PCF_REG_SECONDS, fields, sizeof(fields));
    /* 读取失败时退出。 */
    if (result != BOARD_SENSORS_OK) {
        /* 透传错误。 */
        return result;
    }
    /* VL 位表示振荡器停止或电压过低，当前日期不可信。 */
    if ((fields[0] & PCF_SECONDS_VL) != 0U) {
        /* 返回时钟无效。 */
        return BOARD_SENSORS_ERR_CLOCK_INVALID;
    }
    /* 保存解码秒。 */
    uint8_t second = 0U;
    /* 保存解码分。 */
    uint8_t minute = 0U;
    /* 保存解码时。 */
    uint8_t hour = 0U;
    /* 保存解码日。 */
    uint8_t day = 0U;
    /* 保存解码月。 */
    uint8_t month = 0U;
    /* 保存解码两位年。 */
    uint8_t year_two_digits = 0U;
    /* 逐字段校验 BCD 和范围。 */
    if (!calendar_from_bcd((uint8_t)(fields[0] & 0x7FU), 59U, &second) ||
        !calendar_from_bcd((uint8_t)(fields[1] & 0x7FU), 59U, &minute) ||
        !calendar_from_bcd((uint8_t)(fields[2] & 0x3FU), 23U, &hour) ||
        !calendar_from_bcd((uint8_t)(fields[3] & 0x3FU), 31U, &day) ||
        !calendar_from_bcd((uint8_t)(fields[5] & 0x1FU), 12U, &month) ||
        !calendar_from_bcd(fields[6], 99U, &year_two_digits)) {
        /* 任一字段无效都拒绝输出。 */
        return BOARD_SENSORS_ERR_CLOCK_INVALID;
    }
    /* 星期寄存器只能为 0~6。 */
    if (fields[4] > 6U) {
        /* 返回时钟无效。 */
        return BOARD_SENSORS_ERR_CLOCK_INVALID;
    }
    /* 月和日不能为零。 */
    if ((month == 0U) || (day == 0U)) {
        /* 返回时钟无效。 */
        return BOARD_SENSORS_ERR_CLOCK_INVALID;
    }
    /* 恢复完整年份。 */
    int year = 2000 + (int)year_two_digits;
    /* 检查日是否超过该月实际天数。 */
    if (day > calendar_days_in_month(year, month)) {
        /* 返回时钟无效。 */
        return BOARD_SENSORS_ERR_CLOCK_INVALID;
    }
    /* 计算相对 epoch 天数。 */
    int64_t days_since_epoch = calendar_days_from_civil(year, month, day);
    /* 组合 Unix 秒，所有项范围已验证不会溢出 int64。 */
    int64_t decoded = days_since_epoch * SECONDS_PER_DAY + (int64_t)hour * INT64_C(3600) +
                      (int64_t)minute * INT64_C(60) + (int64_t)second;
    /* 最终范围再次检查，保护算法或寄存器异常。 */
    if ((decoded < PCF_MIN_UNIX_SECONDS) || (decoded > PCF_MAX_UNIX_SECONDS)) {
        /* 返回范围错误。 */
        return BOARD_SENSORS_ERR_RANGE;
    }
    /* 写出可信 Unix 秒。 */
    *unix_seconds = decoded;
    /* 返回成功。 */
    return BOARD_SENSORS_OK;
}
