/* 引入待测设备配置、TLV 命令和稳定 blob 合同。 */
#include "device_config.h"

/* 引入 printf/fprintf 输出机器可读测试结果。 */
#include <stdio.h>
/* 引入 memcmp/memset 验证确定性字节与事务不变性。 */
#include <string.h>

/* 保存本次实际执行的断言数量。 */
static unsigned int g_assertions = 0U;

/* 任一条件失败时输出源码行和表达式并终止当前测试。 */
#define CHECK(expression)                                                       \
    do {                                                                        \
        g_assertions += 1U;                                                      \
        if (!(expression)) {                                                    \
            (void)fprintf(stderr, "CHECK failed line=%d: %s\n", __LINE__, #expression); \
            return 1;                                                           \
        }                                                                       \
    } while (0)

/* 在测试 TLV 中写入 u16 小端值，独立于生产解码器构造边界输入。 */
static void test_write_u16_le(uint8_t data[2], const uint16_t value)
{
    /* 写入低八位。 */
    data[0] = (uint8_t)(value & UINT16_C(0x00FF));
    /* 写入高八位。 */
    data[1] = (uint8_t)((value >> 8U) & UINT16_C(0x00FF));
}

/* 在测试 TLV 中写入 u32 小端值。 */
static void test_write_u32_le(uint8_t data[4], const uint32_t value)
{
    /* 写入位 0～7。 */
    data[0] = (uint8_t)(value & UINT32_C(0xFF));
    /* 写入位 8～15。 */
    data[1] = (uint8_t)((value >> 8U) & UINT32_C(0xFF));
    /* 写入位 16～23。 */
    data[2] = (uint8_t)((value >> 16U) & UINT32_C(0xFF));
    /* 写入位 24～31。 */
    data[3] = (uint8_t)((value >> 24U) & UINT32_C(0xFF));
}

/* 在测试 TLV 中写入当前产品年代范围内的非负 i64 小端秒。 */
static void test_write_i64_le(uint8_t data[8], const int64_t value)
{
    /* 合法或刻意越界的测试值仍为正，可安全转为 u64。 */
    const uint64_t raw = (uint64_t)value;
    /* 遍历八个输出字节。 */
    for (uint8_t index = 0U; index < 8U; ++index) {
        /* 写入对应小端字节。 */
        data[index] = (uint8_t)((raw >> ((uint64_t)index * UINT64_C(8))) & UINT64_C(0xFF));
    }
}

/* 验证默认配置采用产品冻结值，不依赖未初始化内存。 */
static int test_defaults(void)
{
    /* 创建待初始化配置。 */
    device_config_t config;
    /* 先写入非零字节，证明默认函数覆盖全部字段。 */
    (void)memset(&config, 0xA5, sizeof(config));
    /* 写入冻结默认值。 */
    device_config_set_defaults(&config);
    /* 默认时钟尚未同步。 */
    CHECK(!config.utc_valid);
    /* 默认 UTC 和时区均为零。 */
    CHECK((config.utc_unix_seconds == 0) && (config.timezone_minutes == 0));
    /* 默认体重固定为 70 kg，资料修订从 1 开始。 */
    CHECK((config.weight_g == 70000U) && (config.profile_revision == 1U));
    /* 默认没有训练目标且目标值必须为零。 */
    CHECK((config.goal_kind == DEVICE_GOAL_NONE) && (config.goal_value == 0U));
    /* 默认 AMOLED 亮度为 35%。 */
    CHECK(config.brightness_percent == 35U);
    /* 默认振动开启、声音关闭。 */
    CHECK(config.haptic_enabled && !config.sound_enabled);
    /* 默认熄屏时间为 30 秒，偏好修订从 1 开始。 */
    CHECK((config.screen_timeout_seconds == 30U) && (config.preferences_revision == 1U));
    /* 默认关闭开发者模式与原始流。 */
    CHECK(!config.developer_mode && !config.raw_stream_enabled);
    /* 完整默认对象必须通过语义验证。 */
    CHECK(device_config_validate(&config) == DEVICE_CONFIG_OK);
    /* 默认测试完成。 */
    return 0;
}

/* 验证命令 6 的 UTC 秒和时区分钟解码。 */
static int test_decode_sync_time(void)
{
    /* TLV 使用 2000-01-01 UTC 秒和 +480 分钟东八区。 */
    const uint8_t tlv[] = {
        1U, 8U, 0x80U, 0x43U, 0x6DU, 0x38U, 0x00U, 0x00U, 0x00U, 0x00U,
        2U, 2U, 0xE0U, 0x01U,
        99U, 1U, 0xA5U,
    };
    /* 保存解码结果。 */
    device_command_v1_t command;
    /* 未知 type=99 必须被跳过，两个已知必填项正常提交。 */
    CHECK(device_command_v1_decode(
              DEVICE_COMMAND_SYNC_TIME,
              DEVICE_COMMAND_VERSION_V1,
              tlv,
              sizeof(tlv),
              &command) == DEVICE_CONFIG_OK);
    /* 命令编号必须保留。 */
    CHECK(command.command_id == DEVICE_COMMAND_SYNC_TIME);
    /* 时间边界等于 2000-01-01 00:00:00 UTC。 */
    CHECK(command.value.sync_time.utc_unix_seconds == DEVICE_CONFIG_UTC_MIN_SECONDS);
    /* 时区正确解码为有符号 +480 分钟。 */
    CHECK(command.value.sync_time.timezone_minutes == 480);
    /* 时间解码测试完成。 */
    return 0;
}

/* 验证命令 7、8、9、11 的正常 TLV。 */
static int test_decode_remaining_commands(void)
{
    /* 体重 75 kg、资料 revision 9。 */
    const uint8_t profile_tlv[] = {
        1U, 4U, 0xF8U, 0x24U, 0x01U, 0x00U,
        2U, 4U, 0x09U, 0x00U, 0x00U, 0x00U,
    };
    /* 目标为 20 次。 */
    const uint8_t goal_tlv[] = {
        1U, 1U, 0x01U,
        2U, 4U, 0x14U, 0x00U, 0x00U, 0x00U,
    };
    /* 偏好为 60% 亮度、振动关、声音开、45 秒、revision 7、开发模式开。 */
    const uint8_t preferences_tlv[] = {
        1U, 1U, 60U,
        2U, 1U, 0U,
        3U, 1U, 1U,
        4U, 2U, 45U, 0U,
        5U, 4U, 7U, 0U, 0U, 0U,
        6U, 1U, 1U,
    };
    /* 原始流开。 */
    const uint8_t raw_tlv[] = {1U, 1U, 1U};
    /* 复用同一个输出，证明每次解码完整覆盖 union。 */
    device_command_v1_t command;
    /* 解码用户资料。 */
    CHECK(device_command_v1_decode(7U, 1U, profile_tlv, sizeof(profile_tlv), &command) == DEVICE_CONFIG_OK);
    /* 校验克和修订号。 */
    CHECK((command.value.user_profile.weight_g == 75000U) &&
          (command.value.user_profile.profile_revision == 9U));
    /* 解码训练目标。 */
    CHECK(device_command_v1_decode(8U, 1U, goal_tlv, sizeof(goal_tlv), &command) == DEVICE_CONFIG_OK);
    /* 校验目标类别和值。 */
    CHECK((command.value.goal.goal_kind == DEVICE_GOAL_REPETITIONS) &&
          (command.value.goal.goal_value == 20U));
    /* 解码偏好。 */
    CHECK(device_command_v1_decode(9U, 1U, preferences_tlv, sizeof(preferences_tlv), &command) == DEVICE_CONFIG_OK);
    /* 校验六个偏好字段。 */
    CHECK((command.value.preferences.brightness_percent == 60U) &&
          !command.value.preferences.haptic_enabled &&
          command.value.preferences.sound_enabled &&
          (command.value.preferences.screen_timeout_seconds == 45U) &&
          (command.value.preferences.preferences_revision == 7U) &&
          command.value.preferences.developer_mode);
    /* 解码原始流开关。 */
    CHECK(device_command_v1_decode(11U, 1U, raw_tlv, sizeof(raw_tlv), &command) == DEVICE_CONFIG_OK);
    /* 校验布尔结果。 */
    CHECK(command.value.raw_stream.raw_stream_enabled);
    /* 其它命令测试完成。 */
    return 0;
}

/* 验证重复、缺失、长度、截断、版本和范围错误全部被拒绝。 */
static int test_reject_invalid_tlv(void)
{
    /* 保存输出，失败时内容不作为有效结果。 */
    device_command_v1_t command;
    /* 同一已知 type=1 重复两次。 */
    const uint8_t duplicate[] = {
        1U, 1U, DEVICE_GOAL_NONE,
        1U, 1U, DEVICE_GOAL_REPETITIONS,
        2U, 4U, 0U, 0U, 0U, 0U,
    };
    /* 重复已知项必须精确报错。 */
    CHECK(device_command_v1_decode(8U, 1U, duplicate, sizeof(duplicate), &command) ==
          DEVICE_CONFIG_ERR_DUPLICATE_TLV);
    /* 只含 goal_kind，缺少必填 value。 */
    const uint8_t missing[] = {1U, 1U, DEVICE_GOAL_REPETITIONS};
    /* 缺必填项必须拒绝。 */
    CHECK(device_command_v1_decode(8U, 1U, missing, sizeof(missing), &command) ==
          DEVICE_CONFIG_ERR_MISSING_TLV);
    /* 已知 type=1 错误声明为 2 字节。 */
    const uint8_t bad_length[] = {1U, 2U, 1U, 0U, 2U, 4U, 1U, 0U, 0U, 0U};
    /* 已知字段长度错误必须拒绝。 */
    CHECK(device_command_v1_decode(8U, 1U, bad_length, sizeof(bad_length), &command) ==
          DEVICE_CONFIG_ERR_TLV_LENGTH);
    /* 未知项声明 4 字节但只提供 1 字节。 */
    const uint8_t truncated[] = {99U, 4U, 0U};
    /* 未知项也必须先通过边界检查，不能越界跳过。 */
    CHECK(device_command_v1_decode(8U, 1U, truncated, sizeof(truncated), &command) ==
          DEVICE_CONFIG_ERR_MALFORMED_TLV);
    /* 不支持的命令版本必须先于内容解析拒绝。 */
    CHECK(device_command_v1_decode(8U, 2U, missing, sizeof(missing), &command) ==
          DEVICE_CONFIG_ERR_VERSION);
    /* 不在冻结集合中的命令必须拒绝。 */
    CHECK(device_command_v1_decode(5U, 1U, NULL, 0U, &command) ==
          DEVICE_CONFIG_ERR_UNSUPPORTED_COMMAND);
    /* 目标 kind=0 却携带 value=1。 */
    const uint8_t bad_none_goal[] = {1U, 1U, 0U, 2U, 4U, 1U, 0U, 0U, 0U};
    /* 无目标必须要求值为零。 */
    CHECK(device_command_v1_decode(8U, 1U, bad_none_goal, sizeof(bad_none_goal), &command) ==
          DEVICE_CONFIG_ERR_RANGE);
    /* 体重低于 30 kg。 */
    const uint8_t low_weight[] = {1U, 4U, 0x2FU, 0x75U, 0U, 0U, 2U, 4U, 1U, 0U, 0U, 0U};
    /* 29999 g 必须拒绝。 */
    CHECK(device_command_v1_decode(7U, 1U, low_weight, sizeof(low_weight), &command) ==
          DEVICE_CONFIG_ERR_RANGE);
    /* 非法布尔值 2。 */
    const uint8_t bad_bool[] = {1U, 1U, 2U};
    /* Cmd11 只允许 0 或 1。 */
    CHECK(device_command_v1_decode(11U, 1U, bad_bool, sizeof(bad_bool), &command) ==
          DEVICE_CONFIG_ERR_RANGE);
    /* 非法 TLV 测试完成。 */
    return 0;
}

/* 验证五条命令全部冻结上下界，防止单侧范围回归。 */
static int test_command_range_boundaries(void)
{
    /* 保存复用解码结果。 */
    device_command_v1_t command;
    /* 构造 Cmd6 的 type1 8字节和 type2 2字节。 */
    uint8_t time_tlv[] = {1U, 8U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 2U, 2U, 0U, 0U};
    /* 写入最小 UTC 秒。 */
    test_write_i64_le(&time_tlv[2], DEVICE_CONFIG_UTC_MIN_SECONDS);
    /* 写入最小时区 -720 的二补码。 */
    test_write_u16_le(&time_tlv[12], UINT16_C(0xFD30));
    /* 两个最小边界必须接受。 */
    CHECK(device_command_v1_decode(6U, 1U, time_tlv, sizeof(time_tlv), &command) == DEVICE_CONFIG_OK);
    /* 写入最大 UTC 秒。 */
    test_write_i64_le(&time_tlv[2], DEVICE_CONFIG_UTC_MAX_SECONDS);
    /* 写入最大时区 +840。 */
    test_write_u16_le(&time_tlv[12], UINT16_C(840));
    /* 两个最大边界必须接受。 */
    CHECK(device_command_v1_decode(6U, 1U, time_tlv, sizeof(time_tlv), &command) == DEVICE_CONFIG_OK);
    /* 写入小于 2000 年一秒。 */
    test_write_i64_le(&time_tlv[2], DEVICE_CONFIG_UTC_MIN_SECONDS - INT64_C(1));
    /* UTC 下界外必须拒绝。 */
    CHECK(device_command_v1_decode(6U, 1U, time_tlv, sizeof(time_tlv), &command) == DEVICE_CONFIG_ERR_RANGE);
    /* 恢复合法 UTC。 */
    test_write_i64_le(&time_tlv[2], DEVICE_CONFIG_UTC_MIN_SECONDS);
    /* 写入 -721 分钟。 */
    test_write_u16_le(&time_tlv[12], UINT16_C(0xFD2F));
    /* 时区下界外必须拒绝。 */
    CHECK(device_command_v1_decode(6U, 1U, time_tlv, sizeof(time_tlv), &command) == DEVICE_CONFIG_ERR_RANGE);
    /* 写入 +841 分钟。 */
    test_write_u16_le(&time_tlv[12], UINT16_C(841));
    /* 时区上界外必须拒绝。 */
    CHECK(device_command_v1_decode(6U, 1U, time_tlv, sizeof(time_tlv), &command) == DEVICE_CONFIG_ERR_RANGE);

    /* 构造 Cmd7 两个 u32 字段。 */
    uint8_t profile_tlv[] = {1U, 4U, 0U, 0U, 0U, 0U, 2U, 4U, 0U, 0U, 0U, 0U};
    /* 写入最小体重 30 kg。 */
    test_write_u32_le(&profile_tlv[2], DEVICE_CONFIG_WEIGHT_MIN_G);
    /* 写入最小有效 revision 1。 */
    test_write_u32_le(&profile_tlv[8], UINT32_C(1));
    /* 最小边界必须接受。 */
    CHECK(device_command_v1_decode(7U, 1U, profile_tlv, sizeof(profile_tlv), &command) == DEVICE_CONFIG_OK);
    /* 写入最大体重 250 kg。 */
    test_write_u32_le(&profile_tlv[2], DEVICE_CONFIG_WEIGHT_MAX_G);
    /* 最大边界必须接受。 */
    CHECK(device_command_v1_decode(7U, 1U, profile_tlv, sizeof(profile_tlv), &command) == DEVICE_CONFIG_OK);
    /* 写入上界外一克。 */
    test_write_u32_le(&profile_tlv[2], DEVICE_CONFIG_WEIGHT_MAX_G + UINT32_C(1));
    /* 体重上界外必须拒绝。 */
    CHECK(device_command_v1_decode(7U, 1U, profile_tlv, sizeof(profile_tlv), &command) == DEVICE_CONFIG_ERR_RANGE);
    /* 恢复合法体重并把 revision 置零。 */
    test_write_u32_le(&profile_tlv[2], DEVICE_CONFIG_WEIGHT_MIN_G);
    /* 写入非法 revision 0。 */
    test_write_u32_le(&profile_tlv[8], UINT32_C(0));
    /* revision 0 必须拒绝。 */
    CHECK(device_command_v1_decode(7U, 1U, profile_tlv, sizeof(profile_tlv), &command) == DEVICE_CONFIG_ERR_RANGE);

    /* 构造 Cmd8 kind 和 value。 */
    uint8_t goal_tlv[] = {1U, 1U, 0U, 2U, 4U, 0U, 0U, 0U, 0U};
    /* 无目标和值 0 必须接受。 */
    CHECK(device_command_v1_decode(8U, 1U, goal_tlv, sizeof(goal_tlv), &command) == DEVICE_CONFIG_OK);
    /* 遍历三个有值目标。 */
    for (uint8_t kind = (uint8_t)DEVICE_GOAL_REPETITIONS;
         kind <= (uint8_t)DEVICE_GOAL_MCAL;
         ++kind) {
        /* 写入目标种类。 */
        goal_tlv[2] = kind;
        /* 写入最小正值 1。 */
        test_write_u32_le(&goal_tlv[5], UINT32_C(1));
        /* 每种最小正目标必须接受。 */
        CHECK(device_command_v1_decode(8U, 1U, goal_tlv, sizeof(goal_tlv), &command) == DEVICE_CONFIG_OK);
        /* 写入非法零值。 */
        test_write_u32_le(&goal_tlv[5], UINT32_C(0));
        /* 每种有值目标的零值必须拒绝。 */
        CHECK(device_command_v1_decode(8U, 1U, goal_tlv, sizeof(goal_tlv), &command) == DEVICE_CONFIG_ERR_RANGE);
    }
    /* 写入未知 kind 4。 */
    goal_tlv[2] = 4U;
    /* 写入非零值。 */
    test_write_u32_le(&goal_tlv[5], UINT32_C(1));
    /* 未知种类必须拒绝。 */
    CHECK(device_command_v1_decode(8U, 1U, goal_tlv, sizeof(goal_tlv), &command) == DEVICE_CONFIG_ERR_RANGE);

    /* 构造 Cmd9 六个已知字段。 */
    uint8_t preferences_tlv[] = {
        1U, 1U, 5U,
        2U, 1U, 0U,
        3U, 1U, 1U,
        4U, 2U, 10U, 0U,
        5U, 4U, 1U, 0U, 0U, 0U,
        6U, 1U, 0U,
    };
    /* 所有最小合法值必须接受。 */
    CHECK(device_command_v1_decode(9U, 1U, preferences_tlv, sizeof(preferences_tlv), &command) == DEVICE_CONFIG_OK);
    /* 写入亮度 100%。 */
    preferences_tlv[2] = 100U;
    /* 写入熄屏 600 秒。 */
    test_write_u16_le(&preferences_tlv[11], UINT16_C(600));
    /* 最大合法值必须接受。 */
    CHECK(device_command_v1_decode(9U, 1U, preferences_tlv, sizeof(preferences_tlv), &command) == DEVICE_CONFIG_OK);
    /* 写入亮度下界外 4%。 */
    preferences_tlv[2] = 4U;
    /* 必须拒绝。 */
    CHECK(device_command_v1_decode(9U, 1U, preferences_tlv, sizeof(preferences_tlv), &command) == DEVICE_CONFIG_ERR_RANGE);
    /* 写入亮度上界外 101%。 */
    preferences_tlv[2] = 101U;
    /* 必须拒绝。 */
    CHECK(device_command_v1_decode(9U, 1U, preferences_tlv, sizeof(preferences_tlv), &command) == DEVICE_CONFIG_ERR_RANGE);
    /* 恢复亮度 35%。 */
    preferences_tlv[2] = 35U;
    /* 写入熄屏 9 秒。 */
    test_write_u16_le(&preferences_tlv[11], UINT16_C(9));
    /* 必须拒绝。 */
    CHECK(device_command_v1_decode(9U, 1U, preferences_tlv, sizeof(preferences_tlv), &command) == DEVICE_CONFIG_ERR_RANGE);
    /* 写入熄屏 601 秒。 */
    test_write_u16_le(&preferences_tlv[11], UINT16_C(601));
    /* 必须拒绝。 */
    CHECK(device_command_v1_decode(9U, 1U, preferences_tlv, sizeof(preferences_tlv), &command) == DEVICE_CONFIG_ERR_RANGE);
    /* 恢复熄屏 30 秒。 */
    test_write_u16_le(&preferences_tlv[11], UINT16_C(30));
    /* 把偏好 revision 置零。 */
    test_write_u32_le(&preferences_tlv[15], UINT32_C(0));
    /* revision 0 必须拒绝。 */
    CHECK(device_command_v1_decode(9U, 1U, preferences_tlv, sizeof(preferences_tlv), &command) == DEVICE_CONFIG_ERR_RANGE);
    /* 恢复 revision 1 并制造 haptic 布尔 2。 */
    test_write_u32_le(&preferences_tlv[15], UINT32_C(1));
    /* 写入非法 haptic。 */
    preferences_tlv[5] = 2U;
    /* 非法布尔必须拒绝。 */
    CHECK(device_command_v1_decode(9U, 1U, preferences_tlv, sizeof(preferences_tlv), &command) == DEVICE_CONFIG_ERR_RANGE);
    /* 恢复 haptic，制造 sound 布尔 2。 */
    preferences_tlv[5] = 1U;
    /* 写入非法 sound。 */
    preferences_tlv[8] = 2U;
    /* 非法布尔必须拒绝。 */
    CHECK(device_command_v1_decode(9U, 1U, preferences_tlv, sizeof(preferences_tlv), &command) == DEVICE_CONFIG_ERR_RANGE);
    /* 恢复 sound，制造 developer 布尔 2。 */
    preferences_tlv[8] = 0U;
    /* 写入非法 developer。 */
    preferences_tlv[21] = 2U;
    /* 非法布尔必须拒绝。 */
    CHECK(device_command_v1_decode(9U, 1U, preferences_tlv, sizeof(preferences_tlv), &command) == DEVICE_CONFIG_ERR_RANGE);
    /* 范围测试完成。 */
    return 0;
}

/* 验证命令应用为事务式配置更新。 */
static int test_apply_commands(void)
{
    /* 创建默认配置。 */
    device_config_t config;
    /* 写入默认值。 */
    device_config_set_defaults(&config);
    /* 构造资料命令。 */
    device_command_v1_t command;
    /* 清零 union 保留区域。 */
    (void)memset(&command, 0, sizeof(command));
    /* 指定资料命令。 */
    command.command_id = DEVICE_COMMAND_SET_USER_PROFILE;
    /* 设置上界 250 kg。 */
    command.value.user_profile.weight_g = 250000U;
    /* 设置非零修订号。 */
    command.value.user_profile.profile_revision = 2U;
    /* 应用成功。 */
    CHECK(device_config_apply_command(&config, &command) == DEVICE_CONFIG_OK);
    /* 配置已提交新资料。 */
    CHECK((config.weight_g == 250000U) && (config.profile_revision == 2U));
    /* 保存成功状态以检查失败回滚。 */
    const device_config_t before = config;
    /* 制造越界体重。 */
    command.value.user_profile.weight_g = 250001U;
    /* 非法命令必须失败。 */
    CHECK(device_config_apply_command(&config, &command) == DEVICE_CONFIG_ERR_RANGE);
    /* 失败后配置逐字节不变。 */
    CHECK(memcmp(&config, &before, sizeof(config)) == 0);
    /* 命令应用测试完成。 */
    return 0;
}

/* 验证固定 44 字节 blob、CRC32 和解码事务。 */
static int test_config_blob(void)
{
    /* 创建默认配置。 */
    device_config_t config;
    /* 初始化默认配置。 */
    device_config_set_defaults(&config);
    /* 保存编码字节。 */
    uint8_t blob[DEVICE_CONFIG_BLOB_SIZE];
    /* 保存实际或所需长度。 */
    size_t blob_length = 0U;
    /* 容量少一字节必须返回所需固定长度。 */
    CHECK(device_config_blob_encode(
              &config,
              blob,
              sizeof(blob) - 1U,
              &blob_length) == DEVICE_CONFIG_ERR_BUFFER_TOO_SMALL);
    /* 所需长度必须稳定为 44。 */
    CHECK(blob_length == DEVICE_CONFIG_BLOB_SIZE);
    /* 足够容量时编码成功。 */
    CHECK(device_config_blob_encode(
              &config,
              blob,
              sizeof(blob),
              &blob_length) == DEVICE_CONFIG_OK);
    /* 实际长度保持 44。 */
    CHECK(blob_length == DEVICE_CONFIG_BLOB_SIZE);
    /* 固定魔数按字节为 DCFG。 */
    CHECK((blob[0] == 'D') && (blob[1] == 'C') && (blob[2] == 'F') && (blob[3] == 'G'));
    /* 版本和 payload 长度采用小端。 */
    CHECK((blob[4] == 1U) && (blob[5] == 0U) && (blob[6] == 32U) && (blob[7] == 0U));
    /* 独立 zlib/CRC32 生成的默认配置黄金向量用于锁定全部偏移和 CRC。 */
    const uint8_t expected_blob[DEVICE_CONFIG_BLOB_SIZE] = {
        0x44U, 0x43U, 0x46U, 0x47U, 0x01U, 0x00U, 0x20U, 0x00U,
        0x02U, 0x23U, 0x00U, 0x00U, 0x00U, 0x00U, 0x1EU, 0x00U,
        0x70U, 0x11U, 0x01U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x46U, 0x67U, 0x3AU, 0xE0U,
    };
    /* 全部 44 字节必须逐字节一致。 */
    CHECK(memcmp(blob, expected_blob, sizeof(expected_blob)) == 0);
    /* 解码到另一对象。 */
    device_config_t decoded;
    /* 先填充脏值，证明成功时完整覆盖。 */
    (void)memset(&decoded, 0x5A, sizeof(decoded));
    /* CRC 和语义有效时解码成功。 */
    CHECK(device_config_blob_decode(blob, blob_length, &decoded) == DEVICE_CONFIG_OK);
    /* 关键字段与默认值一致。 */
    CHECK((decoded.weight_g == config.weight_g) &&
          (decoded.brightness_percent == config.brightness_percent) &&
          (decoded.goal_kind == config.goal_kind) &&
          (decoded.raw_stream_enabled == config.raw_stream_enabled));
    /* 保存成功对象用于失败回滚检查。 */
    const device_config_t before = decoded;
    /* 翻转 payload 中一位，故意破坏 CRC。 */
    blob[20] ^= 0x01U;
    /* CRC 错误必须拒绝。 */
    CHECK(device_config_blob_decode(blob, blob_length, &decoded) == DEVICE_CONFIG_ERR_CRC);
    /* 失败后输出对象不变。 */
    CHECK(memcmp(&decoded, &before, sizeof(decoded)) == 0);
    /* blob 测试完成。 */
    return 0;
}

/* 顺序执行全部边界测试。 */
int main(void)
{
    /* 先验证默认对象。 */
    CHECK(test_defaults() == 0);
    /* 验证时间命令。 */
    CHECK(test_decode_sync_time() == 0);
    /* 验证其它四条命令。 */
    CHECK(test_decode_remaining_commands() == 0);
    /* 验证全部错误类别。 */
    CHECK(test_reject_invalid_tlv() == 0);
    /* 验证每条命令的双侧范围。 */
    CHECK(test_command_range_boundaries() == 0);
    /* 验证配置事务更新。 */
    CHECK(test_apply_commands() == 0);
    /* 验证持久化 blob。 */
    CHECK(test_config_blob() == 0);
    /* 输出稳定通过标志和断言数。 */
    (void)printf("DEVICE_CONFIG_TESTS_OK assertions=%u\n", g_assertions);
    /* 返回成功。 */
    return 0;
}
