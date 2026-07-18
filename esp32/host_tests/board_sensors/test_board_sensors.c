/* 引入被测公开合同，主机 fake 与 ESP-IDF 固件调用完全相同 API。 */
#include "board_sensors.h"

/* 引入浮点比较函数，验证量程换算。 */
#include <math.h>
/* 引入标准输入输出，打印最终断言数量和失败位置。 */
#include <stdio.h>
/* 引入内存清零和字节复制。 */
#include <string.h>

/* 固定 fake 可记录的写事务数量，覆盖初始化和关机序列。 */
#define FAKE_MAX_WRITES (64U)
/* 固定单次事务最大数据长度，RTC 连续写 7 字节可完整记录。 */
#define FAKE_MAX_DATA (16U)

/* 保存一次 fake 写事务。 */
typedef struct {
    /* 保存目标 7 位 I2C 地址。 */
    uint8_t address;
    /* 保存起始寄存器。 */
    uint8_t reg;
    /* 保存连续写入字节数。 */
    size_t length;
    /* 保存写入数据副本。 */
    uint8_t data[FAKE_MAX_DATA];
} fake_write_t;

/* 保存三芯片寄存器镜像与故障注入状态。 */
typedef struct {
    /* 保存按 7 位地址和寄存器索引的 256 字节镜像。 */
    uint8_t registers[128][256];
    /* 保存写事务历史。 */
    fake_write_t writes[FAKE_MAX_WRITES];
    /* 保存已记录写事务数量。 */
    size_t write_count;
    /* 非零时让下一次读返回失败。 */
    bool fail_next_read;
    /* 非零时让下一次写返回失败。 */
    bool fail_next_write;
    /* 累计驱动请求的复位等待毫秒。 */
    uint32_t delayed_ms;
    /* true 时模拟 QMI8658 收到 CTRL9 命令后把 STATUSINT.bit7 置位，并在 ACK 后清零。 */
    bool ctrl9_auto_complete;
} fake_i2c_t;

/* 累计全部断言，便于确认测试确实执行完整。 */
static unsigned int g_assertions = 0U;

/* 检查布尔条件；失败时返回当前测试函数。 */
#define TEST_ASSERT(condition)                                                                                          \
    do {                                                                                                                \
        g_assertions += 1U;                                                                                             \
        if (!(condition)) {                                                                                            \
            (void)fprintf(stderr, "assertion failed: %s:%d: %s\n", __FILE__, __LINE__, #condition);                  \
            return false;                                                                                               \
        }                                                                                                               \
    } while (0)

/* 模拟连续寄存器读取；地址、范围和输出均先校验。 */
static int fake_read(void *context, uint8_t address, uint8_t reg, uint8_t *data, size_t length)
{
    /* 把不透明上下文恢复为 fake 总线。 */
    fake_i2c_t *fake = (fake_i2c_t *)context;
    /* 拒绝空指针、非法地址和寄存器越界。 */
    if ((fake == NULL) || (data == NULL) || (address >= 128U) || (((size_t)reg + length) > 256U)) {
        /* 返回非零模拟底层参数错误。 */
        return -1;
    }
    /* 消费一次性读故障，验证生产代码不会继续使用旧输出。 */
    if (fake->fail_next_read) {
        /* 清除故障，允许后续恢复测试。 */
        fake->fail_next_read = false;
        /* 返回失败。 */
        return -1;
    }
    /* 从寄存器镜像复制连续字节。 */
    (void)memcpy(data, &fake->registers[address][reg], length);
    /* 返回零表示读成功。 */
    return 0;
}

/* 模拟连续寄存器写入，并记录事务顺序供断言。 */
static int fake_write(void *context, uint8_t address, uint8_t reg, const uint8_t *data, size_t length)
{
    /* 把不透明上下文恢复为 fake 总线。 */
    fake_i2c_t *fake = (fake_i2c_t *)context;
    /* 拒绝空指针、非法地址、记录溢出和寄存器越界。 */
    if ((fake == NULL) || (data == NULL) || (address >= 128U) || (length > FAKE_MAX_DATA) ||
        (((size_t)reg + length) > 256U) || (fake->write_count >= FAKE_MAX_WRITES)) {
        /* 返回失败。 */
        return -1;
    }
    /* 消费一次性写故障。 */
    if (fake->fail_next_write) {
        /* 清除故障，便于后续恢复。 */
        fake->fail_next_write = false;
        /* 返回失败。 */
        return -1;
    }
    /* 取得下一条写记录。 */
    fake_write_t *record = &fake->writes[fake->write_count];
    /* 写入目标地址。 */
    record->address = address;
    /* 写入起始寄存器。 */
    record->reg = reg;
    /* 写入数据长度。 */
    record->length = length;
    /* 保存数据副本。 */
    (void)memcpy(record->data, data, length);
    /* 推进写记录数量。 */
    fake->write_count += 1U;
    /* 更新寄存器镜像，模拟真实芯片写后读。 */
    (void)memcpy(&fake->registers[address][reg], data, length);
    /* 只对 QMI8658 的单字节 CTRL9 写模拟官方命令完成握手，其他芯片事务保持普通镜像语义。 */
    if (((address == BOARD_QMI8658_I2C_ADDRESS_LOW) || (address == BOARD_QMI8658_I2C_ADDRESS_HIGH)) &&
        (reg == 0x0AU) && (length == 1U) && fake->ctrl9_auto_complete) {
        /* 非零命令表示芯片已执行并把 STATUSINT.bit7 置一，等待主控写零确认。 */
        if (data[0] != 0U) {
            /* STATUSINT 地址 0x2D 的 bit7 表示 CTRL9 命令完成。 */
            fake->registers[address][0x2DU] |= 0x80U;
        } else {
            /* ACK=0 表示主控已确认，芯片清除 STATUSINT.bit7。 */
            fake->registers[address][0x2DU] &= (uint8_t)(~0x80U);
        }
    }
    /* 返回零表示写成功。 */
    return 0;
}

/* 模拟毫秒延时，不阻塞主机测试。 */
static void fake_delay(void *context, uint32_t delay_ms)
{
    /* 把不透明上下文恢复为 fake 总线。 */
    fake_i2c_t *fake = (fake_i2c_t *)context;
    /* 只有上下文有效时才累计延时。 */
    if (fake != NULL) {
        /* 累加请求毫秒数。 */
        fake->delayed_ms += delay_ms;
    }
}

/* 创建指向 fake 的回调端口。 */
static board_sensors_i2c_t make_bus(fake_i2c_t *fake)
{
    /* 零初始化端口，避免填充字节未定义。 */
    board_sensors_i2c_t bus = {0};
    /* 注入 fake 上下文。 */
    bus.context = fake;
    /* 注入读函数。 */
    bus.read_register = fake_read;
    /* 注入写函数。 */
    bus.write_register = fake_write;
    /* 注入非阻塞延时记录函数。 */
    bus.delay_ms = fake_delay;
    /* 返回端口副本。 */
    return bus;
}

/* 把 int16 写成 QMI 输出寄存器使用的小端字节。 */
static void put_le_i16(uint8_t *target, int16_t value)
{
    /* 写低 8 位。 */
    target[0] = (uint8_t)((uint16_t)value & 0xFFU);
    /* 写高 8 位。 */
    target[1] = (uint8_t)(((uint16_t)value >> 8U) & 0xFFU);
}

/* 验证 QMI 身份、复位和固定量程/ODR 写入。 */
static bool test_qmi_init_and_scale(void)
{
    /* 清零 fake。 */
    fake_i2c_t fake = {0};
    /* 准备正确 WHO_AM_I。 */
    fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x00U] = BOARD_QMI8658_WHO_AM_I;
    /* 准备复位完成值 0x80。 */
    fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x4DU] = 0x80U;
    /* 构造回调端口。 */
    board_sensors_i2c_t bus = make_bus(&fake);
    /* 零初始化设备。 */
    board_qmi8658_t qmi = {0};
    /* 初始化应成功。 */
    TEST_ASSERT(board_qmi8658_init(&qmi, &bus) == BOARD_SENSORS_OK);
    /* 旧初始化接口必须继续选择默认高地址 0x6B，保证现有调用方源码兼容。 */
    TEST_ASSERT(qmi.address == BOARD_QMI8658_I2C_ADDRESS);
    /* 设备应标记已初始化。 */
    TEST_ASSERT(qmi.initialized);
    /* 复位后至少等待 20 ms。 */
    TEST_ASSERT(fake.delayed_ms >= 20U);
    /* CTRL1 应打开地址自增和 INT1。 */
    TEST_ASSERT(fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x02U] == 0x48U);
    /* CTRL2 应为 range=8g(2<<4) 与 ODR=125Hz(6)。 */
    TEST_ASSERT(fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x03U] == 0x26U);
    /* CTRL3 应为 range=1024dps(6<<4) 与 ODR=112.1Hz(6)。 */
    TEST_ASSERT(fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x04U] == 0x66U);
    /* CTRL5 应关闭芯片 LPF，统一由现有 imu_pipeline 执行 8 Hz 数字滤波。 */
    TEST_ASSERT(fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x06U] == 0x00U);
    /* CTRL7 应启用加速度和陀螺，保持异步模式。 */
    TEST_ASSERT(fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x08U] == 0x03U);
    /* 8g 换算应等于 8/32768 g/LSB。 */
    TEST_ASSERT(fabsf(BOARD_QMI8658_ACCEL_G_PER_LSB - 0.000244140625F) < 1.0e-9F);
    /* 1024dps 换算应等于 1024/32768=0.03125 deg/s/LSB。 */
    TEST_ASSERT(fabsf(BOARD_QMI8658_GYRO_DPS_PER_LSB - 0.03125F) < 1.0e-9F);
    /* 返回通过。 */
    return true;
}

/* 验证调用方可把板级探测得到的低地址 0x6A 注入核心驱动。 */
static bool test_qmi_low_address_init_and_read(void)
{
    /* 清零 fake；二维寄存器表按 7 位地址隔离 0x6A 与 0x6B。 */
    fake_i2c_t fake = {0};
    /* 仅在低地址写入正确身份，确保测试不会误用默认高地址。 */
    fake.registers[BOARD_QMI8658_I2C_ADDRESS_LOW][0x00U] = BOARD_QMI8658_WHO_AM_I;
    /* 仅在低地址写入复位完成响应。 */
    fake.registers[BOARD_QMI8658_I2C_ADDRESS_LOW][0x4DU] = 0x80U;
    /* 构造可记录每次事务地址的 fake I2C 端口。 */
    board_sensors_i2c_t bus = make_bus(&fake);
    /* 零初始化 QMI 驱动状态。 */
    board_qmi8658_t qmi = {0};
    /* 使用板级探测地址初始化，驱动必须在 0x6A 完成身份验证和配置。 */
    TEST_ASSERT(
        board_qmi8658_init_with_address(&qmi, &bus, BOARD_QMI8658_I2C_ADDRESS_LOW) ==
        BOARD_SENSORS_OK);
    /* 驱动状态必须保存实际地址，供所有后续寄存器事务复用。 */
    TEST_ASSERT(qmi.address == BOARD_QMI8658_I2C_ADDRESS_LOW);
    /* 低地址 CTRL2 必须写入正负 8 g、125 Hz 配置。 */
    TEST_ASSERT(fake.registers[BOARD_QMI8658_I2C_ADDRESS_LOW][0x03U] == 0x26U);
    /* 默认高地址寄存器必须保持零，证明初始化没有访问 0x6B。 */
    TEST_ASSERT(fake.registers[BOARD_QMI8658_I2C_ADDRESS_HIGH][0x03U] == 0x00U);
    /* 在低地址提供仅加速度就绪状态。 */
    fake.registers[BOARD_QMI8658_I2C_ADDRESS_LOW][0x2EU] = 0x01U;
    /* 在低地址提供 24 位小端时间戳 0x000102。 */
    fake.registers[BOARD_QMI8658_I2C_ADDRESS_LOW][0x30U] = 0x02U;
    fake.registers[BOARD_QMI8658_I2C_ADDRESS_LOW][0x31U] = 0x01U;
    fake.registers[BOARD_QMI8658_I2C_ADDRESS_LOW][0x32U] = 0x00U;
    /* 在低地址写入 ax=123，验证数据读取同样沿用探测地址。 */
    put_le_i16(&fake.registers[BOARD_QMI8658_I2C_ADDRESS_LOW][0x35U], 123);
    /* 准备读取输出。 */
    board_qmi8658_frame_t frame = {0};
    /* 读取必须成功，且不得回退到 0x6B。 */
    TEST_ASSERT(board_qmi8658_read_available(&qmi, 42ULL, &frame) == BOARD_SENSORS_OK);
    /* 时间戳和低地址轴数据必须正确解码。 */
    TEST_ASSERT((frame.sensor_timestamp_ticks == 0x000102U) && (frame.accel_raw[0] == 123));
    /* 0x69 既不是 QMI8658 低地址也不是高地址，必须在发起 I2C 前拒绝。 */
    TEST_ASSERT(board_qmi8658_init_with_address(&qmi, &bus, 0x69U) == BOARD_SENSORS_ERR_ARGUMENT);
    /* 非法地址初始化不得留下可读取状态。 */
    TEST_ASSERT(!qmi.initialized);
    /* 返回通过。 */
    return true;
}

/* 验证错误芯片身份被拒绝。 */
static bool test_qmi_wrong_id(void)
{
    /* 清零 fake，使 WHO_AM_I 默认为错误的 0。 */
    fake_i2c_t fake = {0};
    /* 构造端口。 */
    board_sensors_i2c_t bus = make_bus(&fake);
    /* 零初始化设备。 */
    board_qmi8658_t qmi = {0};
    /* 初始化必须返回芯片 ID 错误。 */
    TEST_ASSERT(board_qmi8658_init(&qmi, &bus) == BOARD_SENSORS_ERR_CHIP_ID);
    /* 错误设备不得标记初始化。 */
    TEST_ASSERT(!qmi.initialized);
    /* 准备另一条总线，身份正确但复位完成值错误。 */
    fake_i2c_t reset_fake = {0};
    /* 写入正确 WHO_AM_I。 */
    reset_fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x00U] = BOARD_QMI8658_WHO_AM_I;
    /* 复位结果保持 0，模拟芯片没有完成复位。 */
    board_sensors_i2c_t reset_bus = make_bus(&reset_fake);
    /* 准备第二个设备。 */
    board_qmi8658_t reset_qmi = {0};
    /* 复位结果错误必须返回状态错误。 */
    TEST_ASSERT(board_qmi8658_init(&reset_qmi, &reset_bus) == BOARD_SENSORS_ERR_STATE);
    /* 未完成复位不得标记初始化。 */
    TEST_ASSERT(!reset_qmi.initialized);
    /* 返回通过。 */
    return true;
}

/* 验证 QMI 24 位时间戳、小端有符号轴和异步可用位。 */
static bool test_qmi_read_available(void)
{
    /* 清零 fake。 */
    fake_i2c_t fake = {0};
    /* 配置正确身份。 */
    fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x00U] = BOARD_QMI8658_WHO_AM_I;
    /* 配置复位完成。 */
    fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x4DU] = 0x80U;
    /* 构造总线与设备。 */
    board_sensors_i2c_t bus = make_bus(&fake);
    board_qmi8658_t qmi = {0};
    /* 初始化设备。 */
    TEST_ASSERT(board_qmi8658_init(&qmi, &bus) == BOARD_SENSORS_OK);
    /* STATUS0.bit0/bit1 表示两路均有新数据。 */
    fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x2EU] = 0x03U;
    /* 写入 24 位小端芯片时间戳 0x123456。 */
    fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x30U] = 0x56U;
    fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x31U] = 0x34U;
    fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x32U] = 0x12U;
    /* 写入 ax=32767。 */
    put_le_i16(&fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x35U], INT16_MAX);
    /* 写入 ay=-32768。 */
    put_le_i16(&fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x37U], INT16_MIN);
    /* 写入 az=-2。 */
    put_le_i16(&fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x39U], -2);
    /* 写入 gx=1。 */
    put_le_i16(&fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x3BU], 1);
    /* 写入 gy=-1。 */
    put_le_i16(&fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x3DU], -1);
    /* 写入 gz=4660。 */
    put_le_i16(&fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x3FU], 0x1234);
    /* 准备输出帧。 */
    board_qmi8658_frame_t frame = {0};
    /* 使用调用方捕获的单调微秒读取。 */
    TEST_ASSERT(board_qmi8658_read_available(&qmi, 987654321ULL, &frame) == BOARD_SENSORS_OK);
    /* 单调微秒必须原样透传给 imu_pipeline。 */
    TEST_ASSERT(frame.timestamp_us == 987654321ULL);
    /* 芯片时间戳必须按 24 位小端解析。 */
    TEST_ASSERT(frame.sensor_timestamp_ticks == 0x123456U);
    /* 两路就绪位必须保留。 */
    TEST_ASSERT(frame.accel_available && frame.gyro_available);
    /* 加速度字节序和符号必须正确。 */
    TEST_ASSERT((frame.accel_raw[0] == INT16_MAX) && (frame.accel_raw[1] == INT16_MIN) &&
                (frame.accel_raw[2] == -2));
    /* 陀螺字节序和符号必须正确。 */
    TEST_ASSERT((frame.gyro_raw[0] == 1) && (frame.gyro_raw[1] == -1) && (frame.gyro_raw[2] == 0x1234));
    /* 清除两路就绪位，验证空轮询不是错误。 */
    fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x2EU] = 0x00U;
    /* 空轮询仍应成功。 */
    TEST_ASSERT(board_qmi8658_read_available(&qmi, 987654322ULL, &frame) == BOARD_SENSORS_OK);
    /* 两路可用位都应清零。 */
    TEST_ASSERT(!frame.accel_available && !frame.gyro_available);
    /* 返回通过。 */
    return true;
}

/* 验证 QMI8658 WOM 配置严格遵守禁用传感器、低功耗量程、CTRL9 握手和仅开加速度的顺序。 */
static bool test_qmi_wake_on_motion_and_restore(void)
{
    /* 清零 fake，并允许 CTRL9 命令自动完成。 */
    fake_i2c_t fake = {0};
    /* 提供正确芯片身份。 */
    fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x00U] = BOARD_QMI8658_WHO_AM_I;
    /* 提供软复位完成标志。 */
    fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x4DU] = 0x80U;
    /* 模拟 STATUSINT.bit7 与 CTRL9 写零 ACK 的硬件握手。 */
    fake.ctrl9_auto_complete = true;
    /* 构造注入式 I2C 端口。 */
    board_sensors_i2c_t bus = make_bus(&fake);
    /* 创建静态 QMI 驱动状态。 */
    board_qmi8658_t qmi = {0};
    /* 先进入正常采样模式，保证后续测试覆盖运行态到 WOM 的真实转换。 */
    TEST_ASSERT(board_qmi8658_init(&qmi, &bus) == BOARD_SENSORS_OK);
    /* 记录 WOM 前的写事务索引，后续逐条核对硬件命令顺序。 */
    const size_t wom_start = fake.write_count;
    /* 使用 40 mg 阈值和 4 个 21 Hz 样本消隐，约 190 ms 内抑制重复运动中断。 */
    TEST_ASSERT(board_qmi8658_set_wake_on_motion(&qmi, 40U, 4U) == BOARD_SENSORS_OK);
    /* WOM 必须产生禁用、低功耗配置、CTRL9 命令/ACK、仅开加速度共七次写。 */
    TEST_ASSERT(fake.write_count == (wom_start + 7U));
    /* 第一步必须写 CTRL7=0，防止活动传感器配置被半更新。 */
    TEST_ASSERT((fake.writes[wom_start + 0U].reg == 0x08U) && (fake.writes[wom_start + 0U].data[0] == 0x00U));
    /* 第二步必须写 CTRL2=0x0D，即正负 2 g、21 Hz 低功耗加速度。 */
    TEST_ASSERT((fake.writes[wom_start + 1U].reg == 0x03U) && (fake.writes[wom_start + 1U].data[0] == 0x0DU));
    /* 第三步必须写 CAL1_L=40；该寄存器每 LSB 为 1 mg。 */
    TEST_ASSERT((fake.writes[wom_start + 2U].reg == 0x0BU) && (fake.writes[wom_start + 2U].data[0] == 40U));
    /* 第四步必须写 CAL1_H=4；bits7:6=00 选择初始低电平 INT1，bits5:0=4 为消隐样本数。 */
    TEST_ASSERT((fake.writes[wom_start + 3U].reg == 0x0CU) && (fake.writes[wom_start + 3U].data[0] == 0x04U));
    /* 第五步必须写 CTRL9=0x08 执行 WOM 配置命令。 */
    TEST_ASSERT((fake.writes[wom_start + 4U].reg == 0x0AU) && (fake.writes[wom_start + 4U].data[0] == 0x08U));
    /* 第六步必须写 CTRL9=0x00 确认 STATUSINT.bit7。 */
    TEST_ASSERT((fake.writes[wom_start + 5U].reg == 0x0AU) && (fake.writes[wom_start + 5U].data[0] == 0x00U));
    /* 最后只启用加速度，陀螺保持关闭以降低待机电流。 */
    TEST_ASSERT((fake.writes[wom_start + 6U].reg == 0x08U) && (fake.writes[wom_start + 6U].data[0] == 0x01U));
    /* 驱动状态必须明确记录 WOM，供主入口判断 Deep-sleep 唤醒源是否真实就绪。 */
    TEST_ASSERT(qmi.mode == BOARD_QMI8658_MODE_WAKE_ON_MOTION);
    /* 恢复 ACTIVE 必须先关闭 WOM，再还原训练部署的 8 g/125 Hz 与陀螺量程。 */
    TEST_ASSERT(board_qmi8658_set_active(&qmi) == BOARD_SENSORS_OK);
    /* CTRL2 必须恢复 0x26。 */
    TEST_ASSERT(fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x03U] == 0x26U);
    /* CTRL3 必须恢复 0x66。 */
    TEST_ASSERT(fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x04U] == 0x66U);
    /* CTRL7 必须恢复加速度和陀螺异步采样。 */
    TEST_ASSERT(fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x08U] == 0x03U);
    /* WOM 阈值必须清零，避免 ACTIVE 时仍驱动 GPIO21 运动中断。 */
    TEST_ASSERT(fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x0BU] == 0x00U);
    /* 驱动状态必须恢复 ACTIVE。 */
    TEST_ASSERT(qmi.mode == BOARD_QMI8658_MODE_ACTIVE);
    /* 关闭模式必须禁用加速度、陀螺和 WOM。 */
    TEST_ASSERT(board_qmi8658_power_down(&qmi) == BOARD_SENSORS_OK);
    /* CTRL7=0 表示两个传感器均停采。 */
    TEST_ASSERT(fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x08U] == 0x00U);
    /* 阈值为零表示 WOM 禁用。 */
    TEST_ASSERT(fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x0BU] == 0x00U);
    /* 驱动状态必须记录 OFF。 */
    TEST_ASSERT(qmi.mode == BOARD_QMI8658_MODE_OFF);
    /* 零阈值会等价于禁用 WOM，公开 WOM API 必须拒绝该配置以免制造虚假唤醒源。 */
    TEST_ASSERT(board_qmi8658_set_wake_on_motion(&qmi, 0U, 4U) == BOARD_SENSORS_ERR_ARGUMENT);
    /* CAL1_H 只有六位消隐计数，64 必须在发事务前拒绝。 */
    TEST_ASSERT(board_qmi8658_set_wake_on_motion(&qmi, 40U, 64U) == BOARD_SENSORS_ERR_ARGUMENT);
    /* 返回通过。 */
    return true;
}

/* 验证 CTRL9 命令未完成时返回状态错误，且不会错误标记 WOM 已就绪。 */
static bool test_qmi_ctrl9_timeout(void)
{
    /* 清零 fake；ctrl9_auto_complete 保持 false，模拟 STATUSINT.bit7 永不置位。 */
    fake_i2c_t fake = {0};
    /* 提供正确芯片身份和复位完成值。 */
    fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x00U] = BOARD_QMI8658_WHO_AM_I;
    fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x4DU] = 0x80U;
    /* 构造端口和设备。 */
    board_sensors_i2c_t bus = make_bus(&fake);
    board_qmi8658_t qmi = {0};
    /* 初始化活动模式应成功，因为初始化不依赖 CTRL9 命令。 */
    TEST_ASSERT(board_qmi8658_init(&qmi, &bus) == BOARD_SENSORS_OK);
    /* WOM 命令超时必须返回状态错误。 */
    TEST_ASSERT(board_qmi8658_set_wake_on_motion(&qmi, 40U, 4U) == BOARD_SENSORS_ERR_STATE);
    /* 超时后不得标记 WOM，防止主入口把无效 GPIO21 当作 Deep-sleep 唤醒源。 */
    TEST_ASSERT(qmi.mode != BOARD_QMI8658_MODE_WAKE_ON_MOTION);
    /* 超时轮询必须调用延时，避免真实 I2C 总线忙轮询占满 CPU。 */
    TEST_ASSERT(fake.delayed_ms > 20U);
    /* 返回通过。 */
    return true;
}

/* 验证 QMI I2C 异常会向上传播。 */
static bool test_qmi_io_failure(void)
{
    /* 清零 fake。 */
    fake_i2c_t fake = {0};
    /* 配置正确身份和复位值。 */
    fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x00U] = BOARD_QMI8658_WHO_AM_I;
    fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x4DU] = 0x80U;
    /* 构造设备。 */
    board_sensors_i2c_t bus = make_bus(&fake);
    board_qmi8658_t qmi = {0};
    /* 初始化成功。 */
    TEST_ASSERT(board_qmi8658_init(&qmi, &bus) == BOARD_SENSORS_OK);
    /* 让 STATUS0 读取失败。 */
    fake.fail_next_read = true;
    /* 准备输出。 */
    board_qmi8658_frame_t frame = {0};
    /* 驱动必须返回 I/O 错误。 */
    TEST_ASSERT(board_qmi8658_read_available(&qmi, 1ULL, &frame) == BOARD_SENSORS_ERR_IO);
    /* 准备新的 fake，测试初始化写错误。 */
    fake_i2c_t write_fake = {0};
    /* 设置正确身份，使流程进入复位写。 */
    write_fake.registers[BOARD_QMI8658_I2C_ADDRESS][0x00U] = BOARD_QMI8658_WHO_AM_I;
    /* 构造第二条端口。 */
    board_sensors_i2c_t write_bus = make_bus(&write_fake);
    /* 让复位写失败。 */
    write_fake.fail_next_write = true;
    /* 准备第二个设备。 */
    board_qmi8658_t write_qmi = {0};
    /* 初始化必须返回 I/O 错误且不标记成功。 */
    TEST_ASSERT(board_qmi8658_init(&write_qmi, &write_bus) == BOARD_SENSORS_ERR_IO);
    /* 写失败设备必须保持未初始化。 */
    TEST_ASSERT(!write_qmi.initialized);
    /* 返回通过。 */
    return true;
}

/* 验证 AXP2101 身份、状态字段和 SOC 边界。 */
static bool test_axp_status_and_range(void)
{
    /* 清零 fake。 */
    fake_i2c_t fake = {0};
    /* 配置 IC_TYPE=0x4A。 */
    fake.registers[BOARD_AXP2101_I2C_ADDRESS][0x03U] = BOARD_AXP2101_CHIP_ID;
    /* 配置 VBUS good 与 battery present。 */
    fake.registers[BOARD_AXP2101_I2C_ADDRESS][0x00U] = 0x28U;
    /* 配置 charging 方向和恒流阶段。 */
    fake.registers[BOARD_AXP2101_I2C_ADDRESS][0x01U] = 0x22U;
    /* 配置 73% SOC。 */
    fake.registers[BOARD_AXP2101_I2C_ADDRESS][0xA4U] = 73U;
    /* 构造端口和设备。 */
    board_sensors_i2c_t bus = make_bus(&fake);
    board_axp2101_t axp = {0};
    /* 初始化应成功。 */
    TEST_ASSERT(board_axp2101_init(&axp, &bus) == BOARD_SENSORS_OK);
    /* 准备状态输出。 */
    board_axp2101_status_t status = {0};
    /* 状态读取应成功。 */
    TEST_ASSERT(board_axp2101_read_status(&axp, &status) == BOARD_SENSORS_OK);
    /* SOC 应为 73%。 */
    TEST_ASSERT(status.soc_percent == 73U);
    /* 电池和 VBUS 均存在。 */
    TEST_ASSERT(status.battery_present && status.vbus_good);
    /* 当前应充电而非放电。 */
    TEST_ASSERT(status.charging && !status.discharging);
    /* 低三位 2 应映射恒流阶段。 */
    TEST_ASSERT(status.charge_phase == BOARD_AXP_CHARGE_CONSTANT_CURRENT);
    /* 把 SOC 改为非法 101%。 */
    fake.registers[BOARD_AXP2101_I2C_ADDRESS][0xA4U] = 101U;
    /* 驱动必须拒绝越界值。 */
    TEST_ASSERT(board_axp2101_read_status(&axp, &status) == BOARD_SENSORS_ERR_RANGE);
    /* 模拟无电池但 VBUS 存在。 */
    fake.registers[BOARD_AXP2101_I2C_ADDRESS][0x00U] = 0x20U;
    /* 无电池时不应读取损坏 SOC。 */
    TEST_ASSERT(board_axp2101_read_status(&axp, &status) == BOARD_SENSORS_OK);
    /* 无电池 SOC 应明确为零。 */
    TEST_ASSERT(!status.battery_present && (status.soc_percent == 0U));
    /* 返回通过。 */
    return true;
}

/* 验证 AXP 错误 ID 与软关机读改写序列。 */
static bool test_axp_shutdown_sequence(void)
{
    /* 清零 fake。 */
    fake_i2c_t fake = {0};
    /* 构造端口。 */
    board_sensors_i2c_t bus = make_bus(&fake);
    /* 零初始化设备。 */
    board_axp2101_t axp = {0};
    /* 错误 ID 必须被拒绝。 */
    TEST_ASSERT(board_axp2101_init(&axp, &bus) == BOARD_SENSORS_ERR_CHIP_ID);
    /* 配置正确 ID。 */
    fake.registers[BOARD_AXP2101_I2C_ADDRESS][0x03U] = BOARD_AXP2101_CHIP_ID;
    /* 重新初始化应成功。 */
    TEST_ASSERT(board_axp2101_init(&axp, &bus) == BOARD_SENSORS_OK);
    /* COMMON_CONFIG 初值保留其它控制位。 */
    fake.registers[BOARD_AXP2101_I2C_ADDRESS][0x10U] = 0xA0U;
    /* 记录调用前写次数。 */
    size_t writes_before = fake.write_count;
    /* 请求软关机。 */
    TEST_ASSERT(board_axp2101_request_shutdown(&axp) == BOARD_SENSORS_OK);
    /* 必须只增加一次写事务。 */
    TEST_ASSERT(fake.write_count == (writes_before + 1U));
    /* 写寄存器必须是 COMMON_CONFIG 0x10。 */
    TEST_ASSERT(fake.writes[writes_before].reg == 0x10U);
    /* 其它位保持不变，仅 bit0 置一。 */
    TEST_ASSERT(fake.writes[writes_before].data[0] == 0xA1U);
    /* 模拟写失败。 */
    fake.fail_next_write = true;
    /* 写失败必须向上传播。 */
    TEST_ASSERT(board_axp2101_request_shutdown(&axp) == BOARD_SENSORS_ERR_IO);
    /* 返回通过。 */
    return true;
}

/* 验证闰日 Unix 秒与 BCD 连续写读完全一致。 */
static bool test_pcf_unix_round_trip(void)
{
    /* 清零 fake；CTRL1/CTRL2 的零值是合法默认状态。 */
    fake_i2c_t fake = {0};
    /* 构造端口和设备。 */
    board_sensors_i2c_t bus = make_bus(&fake);
    board_pcf85063_t rtc = {0};
    /* 控制寄存器探测应成功。 */
    TEST_ASSERT(board_pcf85063_init(&rtc, &bus) == BOARD_SENSORS_OK);
    /* 2024-02-29 12:34:56 UTC 的固定 Unix 秒。 */
    const int64_t leap_unix = INT64_C(1709210096);
    /* 写入闰日。 */
    TEST_ASSERT(board_pcf85063_write_unix(&rtc, leap_unix) == BOARD_SENSORS_OK);
    /* 秒寄存器应为 BCD 0x56。 */
    TEST_ASSERT(fake.registers[BOARD_PCF85063_I2C_ADDRESS][0x04U] == 0x56U);
    /* 分寄存器应为 BCD 0x34。 */
    TEST_ASSERT(fake.registers[BOARD_PCF85063_I2C_ADDRESS][0x05U] == 0x34U);
    /* 时寄存器应为 24 小时制 BCD 0x12。 */
    TEST_ASSERT(fake.registers[BOARD_PCF85063_I2C_ADDRESS][0x06U] == 0x12U);
    /* 日寄存器应为 BCD 0x29。 */
    TEST_ASSERT(fake.registers[BOARD_PCF85063_I2C_ADDRESS][0x07U] == 0x29U);
    /* 星期四按 Sunday=0 应编码为 4。 */
    TEST_ASSERT(fake.registers[BOARD_PCF85063_I2C_ADDRESS][0x08U] == 0x04U);
    /* 月寄存器应为 BCD 0x02。 */
    TEST_ASSERT(fake.registers[BOARD_PCF85063_I2C_ADDRESS][0x09U] == 0x02U);
    /* 年寄存器应为 BCD 0x24。 */
    TEST_ASSERT(fake.registers[BOARD_PCF85063_I2C_ADDRESS][0x0AU] == 0x24U);
    /* 准备读回变量。 */
    int64_t decoded = 0;
    /* 读回应成功。 */
    TEST_ASSERT(board_pcf85063_read_unix(&rtc, &decoded) == BOARD_SENSORS_OK);
    /* 闰日往返必须精确到秒。 */
    TEST_ASSERT(decoded == leap_unix);
    /* 返回通过。 */
    return true;
}

/* 验证 RTC VL、非法 BCD、年份范围和 I2C 错误。 */
static bool test_pcf_boundaries(void)
{
    /* 清零 fake。 */
    fake_i2c_t fake = {0};
    /* 构造端口和设备。 */
    board_sensors_i2c_t bus = make_bus(&fake);
    board_pcf85063_t rtc = {0};
    /* 初始化成功。 */
    TEST_ASSERT(board_pcf85063_init(&rtc, &bus) == BOARD_SENSORS_OK);
    /* 1999-12-31 23:59:59 超出 PCF85063 两位年项目合同。 */
    TEST_ASSERT(board_pcf85063_write_unix(&rtc, INT64_C(946684799)) == BOARD_SENSORS_ERR_RANGE);
    /* 2100-01-01 00:00:00 同样超出范围。 */
    TEST_ASSERT(board_pcf85063_write_unix(&rtc, INT64_C(4102444800)) == BOARD_SENSORS_ERR_RANGE);
    /* 2099-12-31 23:59:59 是可表示上界。 */
    TEST_ASSERT(board_pcf85063_write_unix(&rtc, INT64_C(4102444799)) == BOARD_SENSORS_OK);
    /* 先写入合法 2000-01-01。 */
    TEST_ASSERT(board_pcf85063_write_unix(&rtc, INT64_C(946684800)) == BOARD_SENSORS_OK);
    /* 设置秒寄存器 VL 位，模拟振荡器曾停止。 */
    fake.registers[BOARD_PCF85063_I2C_ADDRESS][0x04U] |= 0x80U;
    /* 准备输出。 */
    int64_t decoded = 123;
    /* VL 位必须导致时钟无效。 */
    TEST_ASSERT(board_pcf85063_read_unix(&rtc, &decoded) == BOARD_SENSORS_ERR_CLOCK_INVALID);
    /* 清 VL 并写入非法秒 BCD 0x6A。 */
    fake.registers[BOARD_PCF85063_I2C_ADDRESS][0x04U] = 0x6AU;
    /* 非法 BCD 必须被拒绝。 */
    TEST_ASSERT(board_pcf85063_read_unix(&rtc, &decoded) == BOARD_SENSORS_ERR_CLOCK_INVALID);
    /* 写入合法 BCD 但非法日期 2000-04-31。 */
    fake.registers[BOARD_PCF85063_I2C_ADDRESS][0x04U] = 0x00U;
    fake.registers[BOARD_PCF85063_I2C_ADDRESS][0x05U] = 0x00U;
    fake.registers[BOARD_PCF85063_I2C_ADDRESS][0x06U] = 0x00U;
    fake.registers[BOARD_PCF85063_I2C_ADDRESS][0x07U] = 0x31U;
    fake.registers[BOARD_PCF85063_I2C_ADDRESS][0x08U] = 0x00U;
    fake.registers[BOARD_PCF85063_I2C_ADDRESS][0x09U] = 0x04U;
    fake.registers[BOARD_PCF85063_I2C_ADDRESS][0x0AU] = 0x00U;
    /* 四月 31 日必须被月天数检查拒绝。 */
    TEST_ASSERT(board_pcf85063_read_unix(&rtc, &decoded) == BOARD_SENSORS_ERR_CLOCK_INVALID);
    /* 注入下一次读失败。 */
    fake.fail_next_read = true;
    /* I2C 错误必须向上传播。 */
    TEST_ASSERT(board_pcf85063_read_unix(&rtc, &decoded) == BOARD_SENSORS_ERR_IO);
    /* 返回通过。 */
    return true;
}

/* 验证空回调端口和未初始化状态不会访问硬件。 */
static bool test_argument_and_state_guards(void)
{
    /* 构造缺少回调的非法端口。 */
    board_sensors_i2c_t bad_bus = {0};
    /* 构造三个设备。 */
    board_qmi8658_t qmi = {0};
    board_axp2101_t axp = {0};
    board_pcf85063_t rtc = {0};
    /* 缺少回调时 QMI 初始化应拒绝。 */
    TEST_ASSERT(board_qmi8658_init(&qmi, &bad_bus) == BOARD_SENSORS_ERR_ARGUMENT);
    /* 缺少回调时 AXP 初始化应拒绝。 */
    TEST_ASSERT(board_axp2101_init(&axp, &bad_bus) == BOARD_SENSORS_ERR_ARGUMENT);
    /* 缺少回调时 RTC 初始化应拒绝。 */
    TEST_ASSERT(board_pcf85063_init(&rtc, &bad_bus) == BOARD_SENSORS_ERR_ARGUMENT);
    /* 未初始化 QMI 不得读取。 */
    board_qmi8658_frame_t frame = {0};
    TEST_ASSERT(board_qmi8658_read_available(&qmi, 1ULL, &frame) == BOARD_SENSORS_ERR_STATE);
    /* 未初始化 AXP 不得关机。 */
    TEST_ASSERT(board_axp2101_request_shutdown(&axp) == BOARD_SENSORS_ERR_STATE);
    /* 未初始化 RTC 不得读时间。 */
    int64_t seconds = 0;
    TEST_ASSERT(board_pcf85063_read_unix(&rtc, &seconds) == BOARD_SENSORS_ERR_STATE);
    /* 返回通过。 */
    return true;
}

/* 依次运行全部独立测试，任一失败立即返回非零。 */
int main(void)
{
    /* 定义测试函数表，避免重复调用样板。 */
    bool (*tests[])(void) = {
        test_qmi_init_and_scale,
        test_qmi_low_address_init_and_read,
        test_qmi_wrong_id,
        test_qmi_read_available,
        test_qmi_wake_on_motion_and_restore,
        test_qmi_ctrl9_timeout,
        test_qmi_io_failure,
        test_axp_status_and_range,
        test_axp_shutdown_sequence,
        test_pcf_unix_round_trip,
        test_pcf_boundaries,
        test_argument_and_state_guards,
    };
    /* 遍历全部测试函数。 */
    for (size_t index = 0U; index < (sizeof(tests) / sizeof(tests[0])); ++index) {
        /* 当前测试失败时返回 1。 */
        if (!tests[index]()) {
            /* 返回失败码。 */
            return 1;
        }
    }
    /* 输出明确通过标志和断言数量。 */
    (void)printf("board_sensors_tests passed assertions=%u\n", g_assertions);
    /* 返回成功。 */
    return 0;
}
