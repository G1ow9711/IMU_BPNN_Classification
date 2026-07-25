#ifndef IMU_HANDHELD_DEVICE_CONFIG_H
#define IMU_HANDHELD_DEVICE_CONFIG_H

/*
 * 设备配置与命令 TLV v1。
 *
 * 线上每项固定为 [type:u8, len:u8, value:len字节]，多字节值采用小端序。
 * 已知项重复、缺少必填项、长度错误或数值越界时拒绝整条命令；未知项只在
 * 完整落入输入边界时跳过。配置 blob 固定 44 字节并使用 CRC32/IEEE 防止
 * 断电、写入截断或介质翻转后加载半有效设置。
 *
 * 完整公式、字节布局、单位、边界和 O(n) 复杂度见：
 * docs/设备配置与命令TLV.md。
 */

/* 引入 bool，表示设置开关和 UTC 是否已同步。 */
#include <stdbool.h>
/* 引入 size_t，所有缓冲容量与 TLV 长度均使用平台尺寸类型。 */
#include <stddef.h>
/* 引入固定宽度整数，保证 ESP32 和主机对字节布局理解一致。 */
#include <stdint.h>

#ifdef __cplusplus
/* C++ 调用者使用 C ABI，避免 ESP-IDF 混合组件发生符号改名。 */
extern "C" {
#endif

/* 当前命令语义版本固定为 1。 */
#define DEVICE_COMMAND_VERSION_V1 UINT8_C(1)
/* 配置 blob 总长固定为 8 字节头、32 字节载荷和 4 字节 CRC。 */
#define DEVICE_CONFIG_BLOB_SIZE ((size_t)44U)
/* 最小合法体重为 30 kg，线上和内存统一使用克。 */
#define DEVICE_CONFIG_WEIGHT_MIN_G UINT32_C(30000)
/* 最大合法体重为 250 kg，防止热量估算溢出或明显误输。 */
#define DEVICE_CONFIG_WEIGHT_MAX_G UINT32_C(250000)
/* 最低 AMOLED 亮度为 5%，避免用户把正常显示误设为黑屏。 */
#define DEVICE_CONFIG_BRIGHTNESS_MIN_PERCENT UINT8_C(5)
/* 最高 AMOLED 亮度为 100%。 */
#define DEVICE_CONFIG_BRIGHTNESS_MAX_PERCENT UINT8_C(100)
/* 最短自动熄屏时间为 10 秒。 */
#define DEVICE_CONFIG_SCREEN_TIMEOUT_MIN_SECONDS UINT16_C(10)
/* 最长自动熄屏时间为 600 秒。 */
#define DEVICE_CONFIG_SCREEN_TIMEOUT_MAX_SECONDS UINT16_C(600)
/* 2000-01-01 00:00:00 UTC 的 Unix 秒，作为 Cmd6 最小值。 */
#define DEVICE_CONFIG_UTC_MIN_SECONDS INT64_C(946684800)
/* 2099-12-31 23:59:59 UTC 的 Unix 秒，作为 Cmd6 最大值。 */
#define DEVICE_CONFIG_UTC_MAX_SECONDS INT64_C(4102444799)
/* 最西时区固定为 UTC-12:00，即 -720 分钟。 */
#define DEVICE_CONFIG_TIMEZONE_MIN_MINUTES INT16_C(-720)
/* 最东时区固定为 UTC+14:00，即 +840 分钟。 */
#define DEVICE_CONFIG_TIMEZONE_MAX_MINUTES INT16_C(840)

/* 纯 C 返回码细分传输错误、语义错误和持久化错误。 */
typedef enum device_config_status {
    /* 操作成功，输出对象或缓冲区已经完整提交。 */
    DEVICE_CONFIG_OK = 0,
    /* 必填对象为空，函数没有读取或写入不安全地址。 */
    DEVICE_CONFIG_ERR_NULL = -1,
    /* command_id 不是冻结的 6、7、8、9 或 11。 */
    DEVICE_CONFIG_ERR_UNSUPPORTED_COMMAND = -2,
    /* command_version 不是 1。 */
    DEVICE_CONFIG_ERR_VERSION = -3,
    /* TLV 头或 value 超出输入边界。 */
    DEVICE_CONFIG_ERR_MALFORMED_TLV = -4,
    /* 已知 type 的 len 与冻结宽度不一致。 */
    DEVICE_CONFIG_ERR_TLV_LENGTH = -5,
    /* 同一已知 type 在一条命令中出现超过一次。 */
    DEVICE_CONFIG_ERR_DUPLICATE_TLV = -6,
    /* 一条命令缺少至少一个必填已知 type。 */
    DEVICE_CONFIG_ERR_MISSING_TLV = -7,
    /* 数值、布尔、枚举或组合语义越界。 */
    DEVICE_CONFIG_ERR_RANGE = -8,
    /* 输出缓冲区不足；编码函数仍返回固定所需长度。 */
    DEVICE_CONFIG_ERR_BUFFER_TOO_SMALL = -9,
    /* blob 魔数、版本、固定长度、保留位或语义非法。 */
    DEVICE_CONFIG_ERR_BLOB_FORMAT = -10,
    /* blob CRC32 与前 40 字节不一致。 */
    DEVICE_CONFIG_ERR_CRC = -11
} device_config_status_t;

/* BLE v1 五条设备设置命令编号与共享协议保持一致。 */
typedef enum device_command_id {
    /* Cmd6：同步 UTC Unix 秒与时区分钟。 */
    DEVICE_COMMAND_SYNC_TIME = 6,
    /* Cmd7：更新下次会话使用的体重和资料修订号。 */
    DEVICE_COMMAND_SET_USER_PROFILE = 7,
    /* Cmd8：设置或清除训练目标。 */
    DEVICE_COMMAND_SET_GOAL = 8,
    /* Cmd9：设置亮度、反馈、熄屏和开发者偏好。 */
    DEVICE_COMMAND_SET_PREFERENCES = 9,
    /* Cmd11：开关开发调试原始六轴流。 */
    DEVICE_COMMAND_SET_RAW_STREAM = 11
} device_command_id_t;

/* 目标种类写入 Cmd8 type1 和配置 blob。 */
typedef enum device_goal_kind {
    /* 未设置目标；此时 goal_value 必须为 0。 */
    DEVICE_GOAL_NONE = 0,
    /* 次数目标；goal_value 单位为次且必须大于 0。 */
    DEVICE_GOAL_REPETITIONS = 1,
    /* 时长目标；goal_value 单位为秒且必须大于 0。 */
    DEVICE_GOAL_SECONDS = 2,
    /* 热量目标；goal_value 单位为 mcal 即 0.001 kcal。 */
    DEVICE_GOAL_MCAL = 3
} device_goal_kind_t;

/* Cmd6 解码结果。 */
typedef struct device_sync_time_command {
    /* UTC Unix 秒，范围覆盖 2000～2099。 */
    int64_t utc_unix_seconds;
    /* 本地时区相对 UTC 的分钟偏移，范围 -720～840。 */
    int16_t timezone_minutes;
} device_sync_time_command_t;

/* Cmd7 解码结果。 */
typedef struct device_user_profile_command {
    /* 用户体重，单位 g，范围 30000～250000。 */
    uint32_t weight_g;
    /* 资料修订号必须大于 0，用于拒绝未初始化配置。 */
    uint32_t profile_revision;
} device_user_profile_command_t;

/* Cmd8 解码结果。 */
typedef struct device_goal_command {
    /* 目标种类取 device_goal_kind_t 的 0～3。 */
    uint8_t goal_kind;
    /* 目标值单位由 goal_kind 决定。 */
    uint32_t goal_value;
} device_goal_command_t;

/* Cmd9 解码结果。 */
typedef struct device_preferences_command {
    /* AMOLED 亮度百分比，范围 5～100。 */
    uint8_t brightness_percent;
    /* 协议 v1 保留位；真表没有马达，解码后始终强制为 false。 */
    bool haptic_enabled;
    /* 是否允许扬声器提示；v1 默认关闭。 */
    bool sound_enabled;
    /* 无交互自动熄屏时间，单位秒，范围 10～600。 */
    uint16_t screen_timeout_seconds;
    /* 偏好修订号必须大于 0。 */
    uint32_t preferences_revision;
    /* 是否显示开发者诊断与允许调试功能。 */
    bool developer_mode;
} device_preferences_command_t;

/* Cmd11 解码结果。 */
typedef struct device_raw_stream_command {
    /* true 表示请求发布开发者六轴流，false 表示关闭。 */
    bool raw_stream_enabled;
} device_raw_stream_command_t;

/* 一条冻结命令的类型安全联合体。 */
typedef struct device_command_v1 {
    /* 保存 device_command_id_t 数值，决定 union 的有效成员。 */
    uint8_t command_id;
    /* 仅 command_id 对应成员有效。 */
    union {
        /* Cmd6 时间字段。 */
        device_sync_time_command_t sync_time;
        /* Cmd7 用户资料字段。 */
        device_user_profile_command_t user_profile;
        /* Cmd8 训练目标字段。 */
        device_goal_command_t goal;
        /* Cmd9 设备偏好字段。 */
        device_preferences_command_t preferences;
        /* Cmd11 原始流字段。 */
        device_raw_stream_command_t raw_stream;
    } value;
} device_command_v1_t;

/* 完整设备配置对象可被稳定编码为 44 字节 blob。 */
typedef struct device_config {
    /* true 表示 UTC 和时区已经由 Cmd6 同步。 */
    bool utc_valid;
    /* 最近同步的 UTC Unix 秒；未同步时固定为 0。 */
    int64_t utc_unix_seconds;
    /* 本地时区分钟；未同步时固定为 0。 */
    int16_t timezone_minutes;
    /* 下次新会话使用的用户体重，单位 g。 */
    uint32_t weight_g;
    /* 用户资料修订号，默认 1。 */
    uint32_t profile_revision;
    /* device_goal_kind_t；默认 DEVICE_GOAL_NONE。 */
    uint8_t goal_kind;
    /* 次、秒或 mcal；无目标时固定为 0。 */
    uint32_t goal_value;
    /* AMOLED 亮度百分比，默认 35。 */
    uint8_t brightness_percent;
    /* 配置 blob v1 保留位；当前产品始终为 false。 */
    bool haptic_enabled;
    /* 是否启用声音，默认 false。 */
    bool sound_enabled;
    /* 自动熄屏秒数，默认 30。 */
    uint16_t screen_timeout_seconds;
    /* 偏好修订号，默认 1。 */
    uint32_t preferences_revision;
    /* 开发者模式，默认 false。 */
    bool developer_mode;
    /* 原始六轴流，默认 false。 */
    bool raw_stream_enabled;
} device_config_t;

/* 用冻结产品值完整初始化配置；config 不能为空。 */
void device_config_set_defaults(device_config_t *config);
/* 验证完整配置的范围和组合语义，不修改输入。 */
device_config_status_t device_config_validate(const device_config_t *config);
/* 验证目标 kind/value 组合；供配置 codec 与协调器共享唯一规则。 */
device_config_status_t device_config_validate_goal(
    uint8_t goal_kind,
    uint32_t goal_value);
/* 解码一条 v1 TLV；成功才完整写入 output。 */
device_config_status_t device_command_v1_decode(
    uint8_t command_id,
    uint8_t command_version,
    const uint8_t *tlv,
    size_t tlv_length,
    device_command_v1_t *output);
/* 在局部副本上应用命令；成功才提交 config。 */
device_config_status_t device_config_apply_command(
    device_config_t *config,
    const device_command_v1_t *command);
/* 把有效配置编码为固定 44 字节小端 blob 并追加 CRC32。 */
device_config_status_t device_config_blob_encode(
    const device_config_t *config,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);
/* 校验固定布局和 CRC32 后解码；失败时 output 不变。 */
device_config_status_t device_config_blob_decode(
    const uint8_t *blob,
    size_t blob_length,
    device_config_t *output);

#ifdef __cplusplus
/* 结束 C ABI 声明。 */
}
#endif

#endif /* IMU_HANDHELD_DEVICE_CONFIG_H */
