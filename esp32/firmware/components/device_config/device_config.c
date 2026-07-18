/* 引入设备配置与冻结 TLV v1 声明。 */
#include "device_config.h"

/* 引入 INT64/UINT32 常量和范围宏。 */
#include <stdint.h>
/* 引入 memcpy 和 memset，完成安全按值提交与字节布局。 */
#include <string.h>

/* blob 前四字节固定为 ASCII DCFG。 */
#define DEVICE_CONFIG_BLOB_MAGIC_0 ((uint8_t)'D')
/* blob 魔数第二字节。 */
#define DEVICE_CONFIG_BLOB_MAGIC_1 ((uint8_t)'C')
/* blob 魔数第三字节。 */
#define DEVICE_CONFIG_BLOB_MAGIC_2 ((uint8_t)'F')
/* blob 魔数第四字节。 */
#define DEVICE_CONFIG_BLOB_MAGIC_3 ((uint8_t)'G')
/* blob 格式版本固定为 1。 */
#define DEVICE_CONFIG_BLOB_VERSION UINT16_C(1)
/* 固定 payload 长度为 32 字节。 */
#define DEVICE_CONFIG_BLOB_PAYLOAD_SIZE UINT16_C(32)
/* CRC 覆盖 8 字节头和 32 字节 payload。 */
#define DEVICE_CONFIG_BLOB_CRC_OFFSET ((size_t)40U)
/* flags 只允许 UTC、振动、声音、开发者和原始流五位。 */
#define DEVICE_CONFIG_KNOWN_FLAGS UINT8_C(0x1F)
/* UTC 有效标志位。 */
#define DEVICE_CONFIG_FLAG_UTC_VALID UINT8_C(0x01)
/* 振动启用标志位。 */
#define DEVICE_CONFIG_FLAG_HAPTIC UINT8_C(0x02)
/* 声音启用标志位。 */
#define DEVICE_CONFIG_FLAG_SOUND UINT8_C(0x04)
/* 开发者模式标志位。 */
#define DEVICE_CONFIG_FLAG_DEVELOPER UINT8_C(0x08)
/* 原始流启用标志位。 */
#define DEVICE_CONFIG_FLAG_RAW_STREAM UINT8_C(0x10)

/* 编译期锁定 blob 总尺寸，防止头文件常量与实现偏移漂移。 */
_Static_assert(DEVICE_CONFIG_BLOB_SIZE == 44U, "device config blob size must remain 44 bytes");

/* 从至少 2 字节区域读取无符号小端整数；调用者已完成边界检查。 */
static uint16_t device_read_u16_le(const uint8_t *data)
{
    /* 读取低 8 位。 */
    const uint16_t low = (uint16_t)data[0];
    /* 读取高 8 位并移动到正确位置。 */
    const uint16_t high = (uint16_t)((uint16_t)data[1] << 8U);
    /* 合并并返回。 */
    return (uint16_t)(low | high);
}

/* 从至少 2 字节区域读取有符号小端分钟值。 */
static int16_t device_read_i16_le(const uint8_t *data)
{
    /* 先按无符号读取全部 16 位。 */
    const uint16_t raw = device_read_u16_le(data);
    /* 最高位为 0 时数值可直接安全转换。 */
    if ((raw & UINT16_C(0x8000)) == 0U) {
        /* 返回非负分钟。 */
        return (int16_t)raw;
    }
    /* 最高位为 1 时显式执行二补码符号扩展，避免实现定义转换。 */
    const int32_t signed_value = (int32_t)raw - INT32_C(65536);
    /* 结果保证落入 int16_t。 */
    return (int16_t)signed_value;
}

/* 从至少 4 字节区域读取无符号小端整数。 */
static uint32_t device_read_u32_le(const uint8_t *data)
{
    /* 读取位 0～7。 */
    const uint32_t byte0 = (uint32_t)data[0];
    /* 读取位 8～15。 */
    const uint32_t byte1 = (uint32_t)((uint32_t)data[1] << 8U);
    /* 读取位 16～23。 */
    const uint32_t byte2 = (uint32_t)((uint32_t)data[2] << 16U);
    /* 读取位 24～31。 */
    const uint32_t byte3 = (uint32_t)((uint32_t)data[3] << 24U);
    /* 合并固定宽度结果。 */
    return byte0 | byte1 | byte2 | byte3;
}

/* 从至少 8 字节区域读取非负 int64 小端 Unix 秒。 */
static int64_t device_read_i64_le(const uint8_t *data)
{
    /* 累积完整 64 位原始值。 */
    uint64_t raw = UINT64_C(0);
    /* 遍历八个线上字节。 */
    for (uint8_t index = 0U; index < 8U; ++index) {
        /* 每轮把一个字节移入对应小端位置。 */
        raw |= ((uint64_t)data[index]) << ((uint64_t)index * UINT64_C(8));
    }
    /* 最高位为 1 表示负数或超出 INT64_MAX；返回明确非法哨兵，避免实现定义转换。 */
    if (raw > (uint64_t)INT64_MAX) {
        /* 后续年代范围检查必然拒绝 INT64_MIN。 */
        return INT64_MIN;
    }
    /* 0～INT64_MAX 的无符号值可定义明确地转换为 int64。 */
    return (int64_t)raw;
}

/* 把 16 位无符号值按小端写入至少 2 字节区域。 */
static void device_write_u16_le(uint8_t *data, const uint16_t value)
{
    /* 写入低 8 位。 */
    data[0] = (uint8_t)(value & UINT16_C(0x00FF));
    /* 写入高 8 位。 */
    data[1] = (uint8_t)((value >> 8U) & UINT16_C(0x00FF));
}

/* 把有符号时区按二补码位模式写为 2 字节小端。 */
static void device_write_i16_le(uint8_t *data, const int16_t value)
{
    /* memcpy 获取标准 int16_t 对应位模式，不做有符号右移。 */
    uint16_t raw = UINT16_C(0);
    /* 复制两个字节的二补码位模式。 */
    (void)memcpy(&raw, &value, sizeof(raw));
    /* 使用统一小端写函数。 */
    device_write_u16_le(data, raw);
}

/* 把 32 位无符号值按小端写入至少 4 字节区域。 */
static void device_write_u32_le(uint8_t *data, const uint32_t value)
{
    /* 写入位 0～7。 */
    data[0] = (uint8_t)(value & UINT32_C(0x000000FF));
    /* 写入位 8～15。 */
    data[1] = (uint8_t)((value >> 8U) & UINT32_C(0x000000FF));
    /* 写入位 16～23。 */
    data[2] = (uint8_t)((value >> 16U) & UINT32_C(0x000000FF));
    /* 写入位 24～31。 */
    data[3] = (uint8_t)((value >> 24U) & UINT32_C(0x000000FF));
}

/* 把非负 int64 Unix 秒按 8 字节小端写入。 */
static void device_write_i64_le(uint8_t *data, const int64_t value)
{
    /* 合法配置恒为非负，转换为无符号后位模式不变。 */
    const uint64_t raw = (uint64_t)value;
    /* 遍历八个输出字节。 */
    for (uint8_t index = 0U; index < 8U; ++index) {
        /* 取出对应 8 位并写入小端位置。 */
        data[index] = (uint8_t)((raw >> ((uint64_t)index * UINT64_C(8))) & UINT64_C(0xFF));
    }
}

/* 计算 CRC32/IEEE，初值和末值均异或 0xFFFFFFFF，多项式反射值为 0xEDB88320。 */
static uint32_t device_crc32_ieee(const uint8_t *data, const size_t length)
{
    /* CRC 初值按 IEEE 802.3 设为全 1。 */
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    /* 遍历全部受保护字节，时间复杂度 O(length)。 */
    for (size_t index = 0U; index < length; ++index) {
        /* 当前字节先与 CRC 低八位合并。 */
        crc ^= (uint32_t)data[index];
        /* 每个字节执行八次反射位更新。 */
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            /* 保存最低位，决定是否应用生成多项式。 */
            const uint32_t lsb = crc & UINT32_C(1);
            /* 先无符号右移一位。 */
            crc >>= 1U;
            /* 原最低位为 1 时异或反射多项式。 */
            if (lsb != 0U) {
                /* 应用 0xEDB88320。 */
                crc ^= UINT32_C(0xEDB88320);
            }
        }
    }
    /* 末值按标准再次异或全 1。 */
    return crc ^ UINT32_C(0xFFFFFFFF);
}

/* 检查命令编号是否属于当前冻结集合。 */
static bool device_command_is_supported(const uint8_t command_id)
{
    /* 五个设置命令以外均由其它组件处理。 */
    return (command_id == (uint8_t)DEVICE_COMMAND_SYNC_TIME) ||
           (command_id == (uint8_t)DEVICE_COMMAND_SET_USER_PROFILE) ||
           (command_id == (uint8_t)DEVICE_COMMAND_SET_GOAL) ||
           (command_id == (uint8_t)DEVICE_COMMAND_SET_PREFERENCES) ||
           (command_id == (uint8_t)DEVICE_COMMAND_SET_RAW_STREAM);
}

/* 验证目标 kind/value 组合。 */
device_config_status_t device_config_validate_goal(
    const uint8_t goal_kind,
    const uint32_t goal_value)
{
    /* kind 必须落入 0～3。 */
    if (goal_kind > (uint8_t)DEVICE_GOAL_MCAL) {
        /* 未知目标类型不能猜测单位。 */
        return DEVICE_CONFIG_ERR_RANGE;
    }
    /* 无目标必须把值清零，保证 blob 字节确定。 */
    if (goal_kind == (uint8_t)DEVICE_GOAL_NONE) {
        /* 非零值与无目标语义冲突。 */
        return goal_value == 0U ? DEVICE_CONFIG_OK : DEVICE_CONFIG_ERR_RANGE;
    }
    /* 次、秒、mcal 目标均必须大于 0。 */
    return goal_value > 0U ? DEVICE_CONFIG_OK : DEVICE_CONFIG_ERR_RANGE;
}

/* 验证冻结布尔编码只使用 0 或 1。 */
static device_config_status_t device_decode_bool(
    const uint8_t raw,
    bool *output)
{
    /* 输出指针由内部调用保证非空。 */
    if (raw > 1U) {
        /* 2～255 均不是 v1 布尔。 */
        return DEVICE_CONFIG_ERR_RANGE;
    }
    /* 1 为 true，0 为 false。 */
    *output = raw == 1U;
    /* 返回成功。 */
    return DEVICE_CONFIG_OK;
}

void device_config_set_defaults(device_config_t *config)
{
    /* 空指针没有可初始化对象，安全返回。 */
    if (config == NULL) {
        /* 不发生写入。 */
        return;
    }
    /* 先清空包括结构填充字节，保证 blob 前的对象比较可重复。 */
    (void)memset(config, 0, sizeof(*config));
    /* 默认体重为 70 kg。 */
    config->weight_g = UINT32_C(70000);
    /* 默认资料修订号从 1 开始。 */
    config->profile_revision = UINT32_C(1);
    /* 默认不设置训练目标。 */
    config->goal_kind = (uint8_t)DEVICE_GOAL_NONE;
    /* 无目标值固定为 0。 */
    config->goal_value = UINT32_C(0);
    /* 默认 AMOLED 亮度为 35%。 */
    config->brightness_percent = UINT8_C(35);
    /* 默认开启计次振动。 */
    config->haptic_enabled = true;
    /* 默认关闭声音，避免公共环境打扰。 */
    config->sound_enabled = false;
    /* 默认 30 秒无交互熄屏。 */
    config->screen_timeout_seconds = UINT16_C(30);
    /* 默认偏好修订号从 1 开始。 */
    config->preferences_revision = UINT32_C(1);
    /* 默认关闭开发者模式。 */
    config->developer_mode = false;
    /* 默认关闭高带宽原始流。 */
    config->raw_stream_enabled = false;
}

device_config_status_t device_config_validate(const device_config_t *config)
{
    /* 配置对象必须存在。 */
    if (config == NULL) {
        /* 返回空参数。 */
        return DEVICE_CONFIG_ERR_NULL;
    }
    /* 体重统一限制在 30～250 kg。 */
    if ((config->weight_g < DEVICE_CONFIG_WEIGHT_MIN_G) ||
        (config->weight_g > DEVICE_CONFIG_WEIGHT_MAX_G)) {
        /* 拒绝越界体重。 */
        return DEVICE_CONFIG_ERR_RANGE;
    }
    /* 两类修订号均必须非零。 */
    if ((config->profile_revision == 0U) || (config->preferences_revision == 0U)) {
        /* 零表示未初始化或损坏。 */
        return DEVICE_CONFIG_ERR_RANGE;
    }
    /* 验证目标组合。 */
    const device_config_status_t goal_status = device_config_validate_goal(
        config->goal_kind,
        config->goal_value);
    /* 目标失败时原样返回。 */
    if (goal_status != DEVICE_CONFIG_OK) {
        /* 返回范围错误。 */
        return goal_status;
    }
    /* 亮度限制为 5～100%。 */
    if ((config->brightness_percent < DEVICE_CONFIG_BRIGHTNESS_MIN_PERCENT) ||
        (config->brightness_percent > DEVICE_CONFIG_BRIGHTNESS_MAX_PERCENT)) {
        /* 拒绝黑屏值或超过 100%。 */
        return DEVICE_CONFIG_ERR_RANGE;
    }
    /* 熄屏时间限制为 10～600 秒。 */
    if ((config->screen_timeout_seconds < DEVICE_CONFIG_SCREEN_TIMEOUT_MIN_SECONDS) ||
        (config->screen_timeout_seconds > DEVICE_CONFIG_SCREEN_TIMEOUT_MAX_SECONDS)) {
        /* 拒绝范围外时间。 */
        return DEVICE_CONFIG_ERR_RANGE;
    }
    /* UTC 已同步时必须同时验证秒和时区。 */
    if (config->utc_valid) {
        /* Unix 秒只允许 2000～2099。 */
        if ((config->utc_unix_seconds < DEVICE_CONFIG_UTC_MIN_SECONDS) ||
            (config->utc_unix_seconds > DEVICE_CONFIG_UTC_MAX_SECONDS)) {
            /* 拒绝超出 RTC 产品年代。 */
            return DEVICE_CONFIG_ERR_RANGE;
        }
        /* 时区只允许 UTC-12:00～UTC+14:00。 */
        if ((config->timezone_minutes < DEVICE_CONFIG_TIMEZONE_MIN_MINUTES) ||
            (config->timezone_minutes > DEVICE_CONFIG_TIMEZONE_MAX_MINUTES)) {
            /* 拒绝无效时区。 */
            return DEVICE_CONFIG_ERR_RANGE;
        }
    } else if ((config->utc_unix_seconds != 0) || (config->timezone_minutes != 0)) {
        /* 未同步状态必须清零两个时间字段，保证持久化唯一表示。 */
        return DEVICE_CONFIG_ERR_RANGE;
    }
    /* 所有字段语义有效。 */
    return DEVICE_CONFIG_OK;
}

/* 返回某命令已知 type 的固定长度；0 表示未知 type。 */
static uint8_t device_known_tlv_length(
    const uint8_t command_id,
    const uint8_t type)
{
    /* Cmd6 只定义 int64 UTC 和 int16 时区。 */
    if (command_id == (uint8_t)DEVICE_COMMAND_SYNC_TIME) {
        /* type1 为 8 字节，type2 为 2 字节。 */
        return type == 1U ? 8U : (type == 2U ? 2U : 0U);
    }
    /* Cmd7 两项均为 u32。 */
    if (command_id == (uint8_t)DEVICE_COMMAND_SET_USER_PROFILE) {
        /* type1/2 固定四字节。 */
        return ((type == 1U) || (type == 2U)) ? 4U : 0U;
    }
    /* Cmd8 type1 为 u8，type2 为 u32。 */
    if (command_id == (uint8_t)DEVICE_COMMAND_SET_GOAL) {
        /* 返回冻结长度。 */
        return type == 1U ? 1U : (type == 2U ? 4U : 0U);
    }
    /* Cmd9 定义六个偏好字段。 */
    if (command_id == (uint8_t)DEVICE_COMMAND_SET_PREFERENCES) {
        /* 亮度、振动、声音和开发者模式均为一字节。 */
        if ((type == 1U) || (type == 2U) || (type == 3U) || (type == 6U)) {
            /* 返回一字节。 */
            return 1U;
        }
        /* 熄屏为 u16。 */
        if (type == 4U) {
            /* 返回两字节。 */
            return 2U;
        }
        /* revision 为 u32。 */
        return type == 5U ? 4U : 0U;
    }
    /* Cmd11 只定义一个布尔字段。 */
    if (command_id == (uint8_t)DEVICE_COMMAND_SET_RAW_STREAM) {
        /* type1 固定一字节。 */
        return type == 1U ? 1U : 0U;
    }
    /* 未支持命令没有已知 type。 */
    return 0U;
}

/* 返回一条命令的必填 known-type 位图。 */
static uint8_t device_required_tlv_mask(const uint8_t command_id)
{
    /* Cmd6、7、8 均要求 type1 和 type2。 */
    if ((command_id == (uint8_t)DEVICE_COMMAND_SYNC_TIME) ||
        (command_id == (uint8_t)DEVICE_COMMAND_SET_USER_PROFILE) ||
        (command_id == (uint8_t)DEVICE_COMMAND_SET_GOAL)) {
        /* 位0/1 对应 type1/2。 */
        return UINT8_C(0x03);
    }
    /* Cmd9 要求 type1～6。 */
    if (command_id == (uint8_t)DEVICE_COMMAND_SET_PREFERENCES) {
        /* 低六位全部置一。 */
        return UINT8_C(0x3F);
    }
    /* Cmd11 只要求 type1。 */
    return UINT8_C(0x01);
}

/* 把一个已完成长度检查的已知 TLV 写入命令候选。 */
static device_config_status_t device_assign_known_tlv(
    device_command_v1_t *command,
    const uint8_t type,
    const uint8_t *value)
{
    /* 按命令编号选择有效 union 成员。 */
    switch (command->command_id) {
        /* Cmd6 写入 UTC 或时区。 */
        case DEVICE_COMMAND_SYNC_TIME:
            /* type1 为 Unix 秒。 */
            if (type == 1U) {
                /* 解码 8 字节非负秒。 */
                command->value.sync_time.utc_unix_seconds = device_read_i64_le(value);
            } else {
                /* type2 解码有符号时区分钟。 */
                command->value.sync_time.timezone_minutes = device_read_i16_le(value);
            }
            /* 字段写入成功。 */
            return DEVICE_CONFIG_OK;
        /* Cmd7 写入体重或资料修订。 */
        case DEVICE_COMMAND_SET_USER_PROFILE:
            /* type1 为体重。 */
            if (type == 1U) {
                /* 解码克。 */
                command->value.user_profile.weight_g = device_read_u32_le(value);
            } else {
                /* type2 为资料修订。 */
                command->value.user_profile.profile_revision = device_read_u32_le(value);
            }
            /* 字段写入成功。 */
            return DEVICE_CONFIG_OK;
        /* Cmd8 写入目标种类或目标值。 */
        case DEVICE_COMMAND_SET_GOAL:
            /* type1 为一字节枚举。 */
            if (type == 1U) {
                /* 复制枚举原值，稍后统一范围检查。 */
                command->value.goal.goal_kind = value[0];
            } else {
                /* type2 为 u32 目标值。 */
                command->value.goal.goal_value = device_read_u32_le(value);
            }
            /* 字段写入成功。 */
            return DEVICE_CONFIG_OK;
        /* Cmd9 写入六项偏好。 */
        case DEVICE_COMMAND_SET_PREFERENCES:
            /* type1 为亮度。 */
            if (type == 1U) {
                /* 复制百分比。 */
                command->value.preferences.brightness_percent = value[0];
                /* 返回成功。 */
                return DEVICE_CONFIG_OK;
            }
            /* type2 为振动布尔。 */
            if (type == 2U) {
                /* 严格解码 0/1。 */
                return device_decode_bool(value[0], &command->value.preferences.haptic_enabled);
            }
            /* type3 为声音布尔。 */
            if (type == 3U) {
                /* 严格解码 0/1。 */
                return device_decode_bool(value[0], &command->value.preferences.sound_enabled);
            }
            /* type4 为熄屏秒。 */
            if (type == 4U) {
                /* 解码 u16 秒。 */
                command->value.preferences.screen_timeout_seconds = device_read_u16_le(value);
                /* 返回成功。 */
                return DEVICE_CONFIG_OK;
            }
            /* type5 为偏好修订。 */
            if (type == 5U) {
                /* 解码 u32 revision。 */
                command->value.preferences.preferences_revision = device_read_u32_le(value);
                /* 返回成功。 */
                return DEVICE_CONFIG_OK;
            }
            /* type6 为开发者模式布尔。 */
            return device_decode_bool(value[0], &command->value.preferences.developer_mode);
        /* Cmd11 写入原始流布尔。 */
        case DEVICE_COMMAND_SET_RAW_STREAM:
            /* type1 严格解码 0/1。 */
            return device_decode_bool(value[0], &command->value.raw_stream.raw_stream_enabled);
        /* 调用前已验证命令，默认分支仅作防御。 */
        default:
            /* 返回不支持命令。 */
            return DEVICE_CONFIG_ERR_UNSUPPORTED_COMMAND;
    }
}

/* 验证已完整解析的命令值。 */
static device_config_status_t device_validate_command(const device_command_v1_t *command)
{
    /* Cmd6 校验年代与时区。 */
    if (command->command_id == (uint8_t)DEVICE_COMMAND_SYNC_TIME) {
        /* UTC 秒必须位于 2000～2099。 */
        if ((command->value.sync_time.utc_unix_seconds < DEVICE_CONFIG_UTC_MIN_SECONDS) ||
            (command->value.sync_time.utc_unix_seconds > DEVICE_CONFIG_UTC_MAX_SECONDS)) {
            /* 返回范围错误。 */
            return DEVICE_CONFIG_ERR_RANGE;
        }
        /* 时区分钟必须位于 -720～840。 */
        if ((command->value.sync_time.timezone_minutes < DEVICE_CONFIG_TIMEZONE_MIN_MINUTES) ||
            (command->value.sync_time.timezone_minutes > DEVICE_CONFIG_TIMEZONE_MAX_MINUTES)) {
            /* 返回范围错误。 */
            return DEVICE_CONFIG_ERR_RANGE;
        }
        /* 时间命令有效。 */
        return DEVICE_CONFIG_OK;
    }
    /* Cmd7 校验体重和资料 revision。 */
    if (command->command_id == (uint8_t)DEVICE_COMMAND_SET_USER_PROFILE) {
        /* 体重统一为 30～250 kg，revision 必须非零。 */
        if ((command->value.user_profile.weight_g < DEVICE_CONFIG_WEIGHT_MIN_G) ||
            (command->value.user_profile.weight_g > DEVICE_CONFIG_WEIGHT_MAX_G) ||
            (command->value.user_profile.profile_revision == 0U)) {
            /* 返回范围错误。 */
            return DEVICE_CONFIG_ERR_RANGE;
        }
        /* 资料命令有效。 */
        return DEVICE_CONFIG_OK;
    }
    /* Cmd8 校验 kind/value 组合。 */
    if (command->command_id == (uint8_t)DEVICE_COMMAND_SET_GOAL) {
        /* 复用目标语义函数。 */
        return device_config_validate_goal(command->value.goal.goal_kind, command->value.goal.goal_value);
    }
    /* Cmd9 校验亮度、熄屏和 revision；三个布尔已在解析时检查。 */
    if (command->command_id == (uint8_t)DEVICE_COMMAND_SET_PREFERENCES) {
        /* 检查冻结范围。 */
        if ((command->value.preferences.brightness_percent < DEVICE_CONFIG_BRIGHTNESS_MIN_PERCENT) ||
            (command->value.preferences.brightness_percent > DEVICE_CONFIG_BRIGHTNESS_MAX_PERCENT) ||
            (command->value.preferences.screen_timeout_seconds < DEVICE_CONFIG_SCREEN_TIMEOUT_MIN_SECONDS) ||
            (command->value.preferences.screen_timeout_seconds > DEVICE_CONFIG_SCREEN_TIMEOUT_MAX_SECONDS) ||
            (command->value.preferences.preferences_revision == 0U)) {
            /* 返回范围错误。 */
            return DEVICE_CONFIG_ERR_RANGE;
        }
        /* 偏好命令有效。 */
        return DEVICE_CONFIG_OK;
    }
    /* Cmd11 的布尔值已由解码器严格检查。 */
    if (command->command_id == (uint8_t)DEVICE_COMMAND_SET_RAW_STREAM) {
        /* 原始流命令有效。 */
        return DEVICE_CONFIG_OK;
    }
    /* 未支持命令不能进入应用路径。 */
    return DEVICE_CONFIG_ERR_UNSUPPORTED_COMMAND;
}

device_config_status_t device_command_v1_decode(
    const uint8_t command_id,
    const uint8_t command_version,
    const uint8_t *tlv,
    const size_t tlv_length,
    device_command_v1_t *output)
{
    /* 输出必须存在；非空 TLV 也必须提供输入地址。 */
    if ((output == NULL) || ((tlv_length > 0U) && (tlv == NULL))) {
        /* 返回空参数且不写输出。 */
        return DEVICE_CONFIG_ERR_NULL;
    }
    /* 只处理五条冻结设置命令。 */
    if (!device_command_is_supported(command_id)) {
        /* 其它会话命令交给原协调器。 */
        return DEVICE_CONFIG_ERR_UNSUPPORTED_COMMAND;
    }
    /* 只支持命令版本 1。 */
    if (command_version != DEVICE_COMMAND_VERSION_V1) {
        /* 返回版本错误。 */
        return DEVICE_CONFIG_ERR_VERSION;
    }
    /* 在局部对象解析，任何错误都不污染调用者输出。 */
    device_command_v1_t candidate;
    /* 清空 union 的全部字节。 */
    (void)memset(&candidate, 0, sizeof(candidate));
    /* 保存命令编号。 */
    candidate.command_id = command_id;
    /* seen_mask 的位 type-1 标记已出现已知项。 */
    uint8_t seen_mask = UINT8_C(0);
    /* offset 遍历 TLV 字节，时间复杂度 O(tlv_length)。 */
    size_t offset = 0U;
    /* 处理到输入末尾。 */
    while (offset < tlv_length) {
        /* 至少剩余 type 和 len 两字节。 */
        if ((tlv_length - offset) < 2U) {
            /* 截断头属于畸形 TLV。 */
            return DEVICE_CONFIG_ERR_MALFORMED_TLV;
        }
        /* 读取 type。 */
        const uint8_t type = tlv[offset];
        /* 读取 len。 */
        const uint8_t length = tlv[offset + 1U];
        /* 跳过两字节头。 */
        offset += 2U;
        /* value 必须完整落在输入中，未知项也不能越界跳过。 */
        if ((size_t)length > (tlv_length - offset)) {
            /* 返回畸形 TLV。 */
            return DEVICE_CONFIG_ERR_MALFORMED_TLV;
        }
        /* 查询当前命令是否认识该 type。 */
        const uint8_t expected_length = device_known_tlv_length(command_id, type);
        /* expected=0 表示未知项，按冻结规则安全跳过。 */
        if (expected_length == 0U) {
            /* 跳过完整未知 value。 */
            offset += (size_t)length;
            /* 继续下一项。 */
            continue;
        }
        /* 已知字段长度必须完全一致。 */
        if (length != expected_length) {
            /* 返回固定长度错误。 */
            return DEVICE_CONFIG_ERR_TLV_LENGTH;
        }
        /* 已知 type 最大为 6，可安全映射到 uint8 位图。 */
        const uint8_t field_bit = (uint8_t)(UINT8_C(1) << (type - 1U));
        /* 同一已知项不得重复。 */
        if ((seen_mask & field_bit) != 0U) {
            /* 返回重复错误。 */
            return DEVICE_CONFIG_ERR_DUPLICATE_TLV;
        }
        /* 写入对应字段并严格解码布尔。 */
        const device_config_status_t assign_status = device_assign_known_tlv(
            &candidate,
            type,
            &tlv[offset]);
        /* 字段值基础编码错误立即拒绝。 */
        if (assign_status != DEVICE_CONFIG_OK) {
            /* 原样返回范围或命令错误。 */
            return assign_status;
        }
        /* 标记字段已见。 */
        seen_mask = (uint8_t)(seen_mask | field_bit);
        /* 前进到下一项。 */
        offset += (size_t)length;
    }
    /* 所有必填字段必须恰好出现一次。 */
    if ((seen_mask & device_required_tlv_mask(command_id)) != device_required_tlv_mask(command_id)) {
        /* 返回缺必填项。 */
        return DEVICE_CONFIG_ERR_MISSING_TLV;
    }
    /* 完整校验数值与组合语义。 */
    const device_config_status_t validate_status = device_validate_command(&candidate);
    /* 越界时不提交输出。 */
    if (validate_status != DEVICE_CONFIG_OK) {
        /* 返回具体错误。 */
        return validate_status;
    }
    /* 全部检查通过后一次性提交。 */
    *output = candidate;
    /* 返回成功。 */
    return DEVICE_CONFIG_OK;
}

device_config_status_t device_config_apply_command(
    device_config_t *config,
    const device_command_v1_t *command)
{
    /* 两个对象均为必填。 */
    if ((config == NULL) || (command == NULL)) {
        /* 返回空参数。 */
        return DEVICE_CONFIG_ERR_NULL;
    }
    /* 先验证输入命令，防止手工构造绕过 decoder。 */
    const device_config_status_t command_status = device_validate_command(command);
    /* 非法命令不改变配置。 */
    if (command_status != DEVICE_CONFIG_OK) {
        /* 返回具体错误。 */
        return command_status;
    }
    /* 在局部副本应用，后续完整配置验证失败时可回滚。 */
    device_config_t candidate = *config;
    /* 按命令写入对应字段。 */
    switch (command->command_id) {
        /* Cmd6 设置时间有效标志和两个时间字段。 */
        case DEVICE_COMMAND_SYNC_TIME:
            /* 标记时钟已同步。 */
            candidate.utc_valid = true;
            /* 保存 UTC Unix 秒。 */
            candidate.utc_unix_seconds = command->value.sync_time.utc_unix_seconds;
            /* 保存时区分钟。 */
            candidate.timezone_minutes = command->value.sync_time.timezone_minutes;
            /* 结束本分支。 */
            break;
        /* Cmd7 更新下次会话体重与资料修订。 */
        case DEVICE_COMMAND_SET_USER_PROFILE:
            /* 保存克。 */
            candidate.weight_g = command->value.user_profile.weight_g;
            /* 保存 revision。 */
            candidate.profile_revision = command->value.user_profile.profile_revision;
            /* 结束本分支。 */
            break;
        /* Cmd8 设置或清除目标。 */
        case DEVICE_COMMAND_SET_GOAL:
            /* 保存目标枚举。 */
            candidate.goal_kind = command->value.goal.goal_kind;
            /* 保存目标值。 */
            candidate.goal_value = command->value.goal.goal_value;
            /* 结束本分支。 */
            break;
        /* Cmd9 原子替换六项偏好。 */
        case DEVICE_COMMAND_SET_PREFERENCES:
            /* 保存亮度。 */
            candidate.brightness_percent = command->value.preferences.brightness_percent;
            /* 保存振动开关。 */
            candidate.haptic_enabled = command->value.preferences.haptic_enabled;
            /* 保存声音开关。 */
            candidate.sound_enabled = command->value.preferences.sound_enabled;
            /* 保存熄屏秒。 */
            candidate.screen_timeout_seconds = command->value.preferences.screen_timeout_seconds;
            /* 保存偏好 revision。 */
            candidate.preferences_revision = command->value.preferences.preferences_revision;
            /* 保存开发者开关。 */
            candidate.developer_mode = command->value.preferences.developer_mode;
            /* 结束本分支。 */
            break;
        /* Cmd11 更新原始流开关。 */
        case DEVICE_COMMAND_SET_RAW_STREAM:
            /* 保存原始流开关。 */
            candidate.raw_stream_enabled = command->value.raw_stream.raw_stream_enabled;
            /* 结束本分支。 */
            break;
        /* validate_command 已拦截，默认分支仅作防御。 */
        default:
            /* 返回不支持命令。 */
            return DEVICE_CONFIG_ERR_UNSUPPORTED_COMMAND;
    }
    /* 验证更新后的完整对象。 */
    const device_config_status_t config_status = device_config_validate(&candidate);
    /* 组合失败时保持原配置。 */
    if (config_status != DEVICE_CONFIG_OK) {
        /* 返回具体错误。 */
        return config_status;
    }
    /* 完整成功后提交。 */
    *config = candidate;
    /* 返回成功。 */
    return DEVICE_CONFIG_OK;
}

device_config_status_t device_config_blob_encode(
    const device_config_t *config,
    uint8_t *output,
    const size_t output_capacity,
    size_t *output_length)
{
    /* 配置、输出和长度均为必填。 */
    if ((config == NULL) || (output == NULL) || (output_length == NULL)) {
        /* 返回空参数。 */
        return DEVICE_CONFIG_ERR_NULL;
    }
    /* 无论容量是否足够，都返回冻结所需长度。 */
    *output_length = DEVICE_CONFIG_BLOB_SIZE;
    /* 先验证配置，禁止把非法对象持久化。 */
    const device_config_status_t validate_status = device_config_validate(config);
    /* 语义错误原样返回。 */
    if (validate_status != DEVICE_CONFIG_OK) {
        /* 返回范围错误。 */
        return validate_status;
    }
    /* 容量不足时不写部分 blob。 */
    if (output_capacity < DEVICE_CONFIG_BLOB_SIZE) {
        /* 提示调用者扩大缓冲区。 */
        return DEVICE_CONFIG_ERR_BUFFER_TOO_SMALL;
    }
    /* 先清零全部 44 字节，包括保留字节。 */
    (void)memset(output, 0, DEVICE_CONFIG_BLOB_SIZE);
    /* 写入 ASCII DCFG 魔数。 */
    output[0] = DEVICE_CONFIG_BLOB_MAGIC_0;
    /* 写入第二魔数字节。 */
    output[1] = DEVICE_CONFIG_BLOB_MAGIC_1;
    /* 写入第三魔数字节。 */
    output[2] = DEVICE_CONFIG_BLOB_MAGIC_2;
    /* 写入第四魔数字节。 */
    output[3] = DEVICE_CONFIG_BLOB_MAGIC_3;
    /* 偏移 4 写入 u16 格式版本。 */
    device_write_u16_le(&output[4], DEVICE_CONFIG_BLOB_VERSION);
    /* 偏移 6 写入 u16 payload 长度。 */
    device_write_u16_le(&output[6], DEVICE_CONFIG_BLOB_PAYLOAD_SIZE);
    /* 组装五个布尔标志。 */
    uint8_t flags = UINT8_C(0);
    /* UTC 已同步时置位 bit0。 */
    if (config->utc_valid) {
        /* 写入 UTC 有效位。 */
        flags |= DEVICE_CONFIG_FLAG_UTC_VALID;
    }
    /* 振动开启时置位 bit1。 */
    if (config->haptic_enabled) {
        /* 写入振动位。 */
        flags |= DEVICE_CONFIG_FLAG_HAPTIC;
    }
    /* 声音开启时置位 bit2。 */
    if (config->sound_enabled) {
        /* 写入声音位。 */
        flags |= DEVICE_CONFIG_FLAG_SOUND;
    }
    /* 开发者模式开启时置位 bit3。 */
    if (config->developer_mode) {
        /* 写入开发者位。 */
        flags |= DEVICE_CONFIG_FLAG_DEVELOPER;
    }
    /* 原始流开启时置位 bit4。 */
    if (config->raw_stream_enabled) {
        /* 写入原始流位。 */
        flags |= DEVICE_CONFIG_FLAG_RAW_STREAM;
    }
    /* payload 偏移 0 写入 flags。 */
    output[8] = flags;
    /* payload 偏移 1 写入亮度百分比。 */
    output[9] = config->brightness_percent;
    /* payload 偏移 2 写入目标种类。 */
    output[10] = config->goal_kind;
    /* payload 偏移 3 为保留字节，已由 memset 置零。 */
    /* payload 偏移 4 写入时区分钟二补码。 */
    device_write_i16_le(&output[12], config->timezone_minutes);
    /* payload 偏移 6 写入熄屏秒。 */
    device_write_u16_le(&output[14], config->screen_timeout_seconds);
    /* payload 偏移 8 写入体重克。 */
    device_write_u32_le(&output[16], config->weight_g);
    /* payload 偏移 12 写入资料修订。 */
    device_write_u32_le(&output[20], config->profile_revision);
    /* payload 偏移 16 写入目标值。 */
    device_write_u32_le(&output[24], config->goal_value);
    /* payload 偏移 20 写入偏好修订。 */
    device_write_u32_le(&output[28], config->preferences_revision);
    /* payload 偏移 24 写入 UTC Unix 秒。 */
    device_write_i64_le(&output[32], config->utc_unix_seconds);
    /* 计算前 40 字节 CRC32。 */
    const uint32_t crc = device_crc32_ieee(output, DEVICE_CONFIG_BLOB_CRC_OFFSET);
    /* 偏移 40 写入 CRC32 小端值。 */
    device_write_u32_le(&output[40], crc);
    /* 返回成功。 */
    return DEVICE_CONFIG_OK;
}

device_config_status_t device_config_blob_decode(
    const uint8_t *blob,
    const size_t blob_length,
    device_config_t *output)
{
    /* 输入和输出均为必填。 */
    if ((blob == NULL) || (output == NULL)) {
        /* 返回空参数。 */
        return DEVICE_CONFIG_ERR_NULL;
    }
    /* v1 只接受精确 44 字节，拒绝截断和尾随歧义。 */
    if (blob_length != DEVICE_CONFIG_BLOB_SIZE) {
        /* 返回格式错误。 */
        return DEVICE_CONFIG_ERR_BLOB_FORMAT;
    }
    /* 校验四字节魔数。 */
    if ((blob[0] != DEVICE_CONFIG_BLOB_MAGIC_0) ||
        (blob[1] != DEVICE_CONFIG_BLOB_MAGIC_1) ||
        (blob[2] != DEVICE_CONFIG_BLOB_MAGIC_2) ||
        (blob[3] != DEVICE_CONFIG_BLOB_MAGIC_3)) {
        /* 非本组件 blob。 */
        return DEVICE_CONFIG_ERR_BLOB_FORMAT;
    }
    /* 版本和 payload 长度必须精确匹配。 */
    if ((device_read_u16_le(&blob[4]) != DEVICE_CONFIG_BLOB_VERSION) ||
        (device_read_u16_le(&blob[6]) != DEVICE_CONFIG_BLOB_PAYLOAD_SIZE)) {
        /* 旧版或未知版不能猜测解析。 */
        return DEVICE_CONFIG_ERR_BLOB_FORMAT;
    }
    /* 保留字节必须为 0，未知高 flags 也必须拒绝。 */
    if ((blob[11] != 0U) || ((blob[8] & (uint8_t)(~DEVICE_CONFIG_KNOWN_FLAGS)) != 0U)) {
        /* 返回格式错误。 */
        return DEVICE_CONFIG_ERR_BLOB_FORMAT;
    }
    /* 读取线上 CRC。 */
    const uint32_t stored_crc = device_read_u32_le(&blob[40]);
    /* 重新计算受保护区域。 */
    const uint32_t actual_crc = device_crc32_ieee(blob, DEVICE_CONFIG_BLOB_CRC_OFFSET);
    /* CRC 不一致时拒绝加载。 */
    if (stored_crc != actual_crc) {
        /* 返回 CRC 错误。 */
        return DEVICE_CONFIG_ERR_CRC;
    }
    /* 在局部对象解析，失败不污染调用者状态。 */
    device_config_t candidate;
    /* 清零包括填充字节。 */
    (void)memset(&candidate, 0, sizeof(candidate));
    /* 读取五个 flags。 */
    const uint8_t flags = blob[8];
    /* 解码 UTC 有效位。 */
    candidate.utc_valid = (flags & DEVICE_CONFIG_FLAG_UTC_VALID) != 0U;
    /* 解码振动位。 */
    candidate.haptic_enabled = (flags & DEVICE_CONFIG_FLAG_HAPTIC) != 0U;
    /* 解码声音位。 */
    candidate.sound_enabled = (flags & DEVICE_CONFIG_FLAG_SOUND) != 0U;
    /* 解码开发者位。 */
    candidate.developer_mode = (flags & DEVICE_CONFIG_FLAG_DEVELOPER) != 0U;
    /* 解码原始流位。 */
    candidate.raw_stream_enabled = (flags & DEVICE_CONFIG_FLAG_RAW_STREAM) != 0U;
    /* 读取亮度。 */
    candidate.brightness_percent = blob[9];
    /* 读取目标枚举。 */
    candidate.goal_kind = blob[10];
    /* 读取时区分钟。 */
    candidate.timezone_minutes = device_read_i16_le(&blob[12]);
    /* 读取熄屏秒。 */
    candidate.screen_timeout_seconds = device_read_u16_le(&blob[14]);
    /* 读取体重克。 */
    candidate.weight_g = device_read_u32_le(&blob[16]);
    /* 读取资料 revision。 */
    candidate.profile_revision = device_read_u32_le(&blob[20]);
    /* 读取目标值。 */
    candidate.goal_value = device_read_u32_le(&blob[24]);
    /* 读取偏好 revision。 */
    candidate.preferences_revision = device_read_u32_le(&blob[28]);
    /* 读取 UTC Unix 秒。 */
    candidate.utc_unix_seconds = device_read_i64_le(&blob[32]);
    /* 完整验证数值与组合。 */
    const device_config_status_t validate_status = device_config_validate(&candidate);
    /* 非法语义统一归为 blob 格式错误。 */
    if (validate_status != DEVICE_CONFIG_OK) {
        /* 返回格式错误。 */
        return DEVICE_CONFIG_ERR_BLOB_FORMAT;
    }
    /* 所有检查通过后一次性提交。 */
    *output = candidate;
    /* 返回成功。 */
    return DEVICE_CONFIG_OK;
}
