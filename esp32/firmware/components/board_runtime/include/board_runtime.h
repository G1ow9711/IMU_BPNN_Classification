#ifndef IMU_HANDHELD_BOARD_RUNTIME_H
#define IMU_HANDHELD_BOARD_RUNTIME_H

/* 引入板级抽象合同，使真实 BSP 与主机 Mock 对上层暴露同一接口。 */
#include "board_adapter.h"

/* 引入布尔、尺寸和定长整数类型，保证主机与 ESP32 的结构布局一致。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 固定 AXP2101 的 7 位 I2C 地址；这里只探测 ACK，不猜测芯片寄存器。 */
#define BOARD_RUNTIME_AXP2101_ADDRESS (0x34U)
/* 固定 PCF85063 的 7 位 I2C 地址；RTC 读写由独立驱动回调完成。 */
#define BOARD_RUNTIME_PCF85063_ADDRESS (0x51U)
/* 固定 QMI8658 低地址；厂家实物通常使用该地址。 */
#define BOARD_RUNTIME_QMI8658_LOW_ADDRESS (0x6AU)
/* 固定 QMI8658 高地址；启动探测允许硬件地址脚选择另一状态。 */
#define BOARD_RUNTIME_QMI8658_HIGH_ADDRESS (0x6BU)
/* 描述板级运行时结果；零为成功，负值按失败原因区分。 */
typedef enum {
    /* 板级操作成功，输出和运行时状态有效。 */
    BOARD_RUNTIME_OK = 0,
    /* 必填指针为空、百分比越界或枚举值非法。 */
    BOARD_RUNTIME_ERR_ARGUMENT = -1,
    /* 厂家 BSP、GPIO 或 I2C 返回平台错误。 */
    BOARD_RUNTIME_ERR_PLATFORM = -2,
    /* 当前后端未注册请求的 QMI、RTC、PMIC 或存储能力。 */
    BOARD_RUNTIME_ERR_UNSUPPORTED = -3,
    /* 运行时或具体外设尚未成功初始化。 */
    BOARD_RUNTIME_ERR_NOT_READY = -4,
    /* 已初始化驱动执行读写时发生 I/O 失败。 */
    BOARD_RUNTIME_ERR_IO = -5
} board_runtime_result_t;

/* 描述上层要求 QMI8658 进入的三态功耗模式；不得退化为布尔值。 */
typedef enum {
    /* 完全关闭加速度、陀螺和 WOM，适用于无有效 Deep-sleep 唤醒源的安全回退。 */
    BOARD_RUNTIME_QMI_OFF = 0,
    /* 恢复训练推理所需的双传感器活动量程和 ODR。 */
    BOARD_RUNTIME_QMI_ACTIVE = 1,
    /* 仅保留 2 g/21 Hz 低功耗加速度和 INT1 运动唤醒。 */
    BOARD_RUNTIME_QMI_WAKE_ON_MOTION = 2
} board_runtime_qmi_mode_t;

/* 独立传感器/PMIC 驱动回调；厂商 BSP v1.0.7 没有这些高层 API，禁止在本层虚构函数。 */
typedef struct {
    /* 保存独立驱动私有上下文；指针生命周期必须覆盖 board_runtime。 */
    void *context;
    /* 读取 AXP2101 电量百分比与充电状态；百分比输出范围为 0~100。 */
    int (*read_battery)(void *context, uint8_t *percent, bool *charging);
    /* 切换 QMI8658 OFF/ACTIVE/WOM；回调必须完成寄存器握手后才返回零。 */
    int (*set_qmi_mode)(void *context, board_runtime_qmi_mode_t mode);
    /* 读取 PCF85063 Unix 秒；输出为 UTC 秒，不做时区换算。 */
    int (*read_rtc_unix_seconds)(void *context, uint64_t *unix_seconds);
    /* 请求 AXP2101 安全断电；调用前必须由上层完成会话刷盘。 */
    int (*request_pmic_shutdown)(void *context);
} board_runtime_external_ops_t;

/* 保存主机 Mock 初始状态；真实后端忽略这些字段。 */
typedef struct {
    /* 指定 Mock 电量百分比，合法范围为 0~100。 */
    uint8_t battery_percent;
    /* 指定 Mock 是否处于充电状态。 */
    bool charging;
    /* 指定 Mock 是否模拟 TF 卡存在。 */
    bool storage_present;
    /* 指定 Mock 是否模拟 QMI8658、AXP2101 和 PCF85063 全部应答。 */
    bool sensor_devices_present;
} board_runtime_mock_config_t;

/* 保存启动参数；结构按值复制，回调上下文仍由调用方持有。 */
typedef struct {
    /* 保存 QMI/PMIC/RTC 独立驱动合同；允许部分回调为空。 */
    board_runtime_external_ops_t external_ops;
    /* 保存 Mock 初始状态；仅主机测试或 CONFIG_BOARD_RUNTIME_MOCK 使用。 */
    board_runtime_mock_config_t mock;
    /* 保存 AMOLED 启动后的默认亮度百分比，范围为 1~100。 */
    uint8_t initial_brightness_percent;
} board_runtime_config_t;

/* 保存启动自检结果；present 仅表示 I2C ACK，不等同于驱动功能已经验收。 */
typedef struct {
    /* true 表示当前固件编译为 Waveshare 真实 BSP 后端。 */
    bool real_backend;
    /* true 表示 410x502 AMOLED 已由 BSP 启动。 */
    bool display_ready;
    /* true 表示 LVGL 输入设备已由 BSP 创建。 */
    bool touch_ready;
    /* true 表示共用 I2C 主总线已初始化。 */
    bool i2c_ready;
    /* true 表示 QMI8658 在 0x6A 或 0x6B 收到 ACK。 */
    bool qmi_present;
    /* 保存实际应答的 QMI8658 地址；零表示未探测到。 */
    uint8_t qmi_i2c_address;
    /* true 表示 AXP2101 地址 0x34 收到 ACK。 */
    bool axp2101_present;
    /* true 表示 PCF85063 地址 0x51 收到 ACK。 */
    bool rtc_present;
    /* true 表示 TF 已成功挂载；卡未插入时保持 false。 */
    bool storage_mounted;
    /* true 表示 GPIO46 扬声器功放使能脚已配置且当前默认拉低。 */
    bool speaker_gate_ready;
    /* 保存最近平台错误码；零表示最近操作成功。 */
    int32_t last_platform_error;
} board_runtime_diagnostics_t;

/* 保存统一板级运行时；所有字段可静态分配，平台句柄以不透明指针保存。 */
typedef struct {
    /* 保存上层使用的板级适配器；其回调 context 指向本结构。 */
    board_adapter_t adapter;
    /* 保存启动配置副本，避免调用方临时结构失效。 */
    board_runtime_config_t config;
    /* 保存实时诊断快照。 */
    board_runtime_diagnostics_t diagnostics;
    /* 保存 AMOLED 物理显示开关；false 时亮度为零且面板已收到 Display Off。 */
    bool display_on;
    /* 保存触摸硬件是否主动扫描且允许 LVGL 分派。 */
    bool touch_active;
    /* 保存扬声器功放 GPIO46 逻辑状态；产品默认 false。 */
    bool speaker_enabled;
    /* 保存最近非零亮度百分比，屏幕恢复时复用。 */
    uint8_t brightness_percent;
    /* 保存平台 LVGL 显示句柄；真实后端指向 lv_display_t，Mock 可为空。 */
    void *platform_display;
    /* 保存平台 LVGL 输入句柄；真实后端指向 lv_indev_t，Mock 可为空。 */
    void *platform_touch;
    /* 保存平台 I2C 主总线句柄；真实后端指向 ESP-IDF 总线对象。 */
    void *platform_i2c;
    /* 标记初始化完成；未完成时所有操作返回 NOT_READY。 */
    bool initialized;
} board_runtime_t;

/* 初始化真实 BSP 或 Mock 后端，并建立 board_adapter 回调表。 */
board_runtime_result_t board_runtime_init(
    board_runtime_t *runtime,
    const board_runtime_config_t *config);
/* 返回内部 board_adapter；未初始化时返回 NULL。 */
board_adapter_t *board_runtime_adapter(board_runtime_t *runtime);
/* 返回 BSP 共用 I2C 主总线不透明句柄，供独立 QMI/AXP/RTC 驱动初始化。 */
void *board_runtime_i2c_handle(board_runtime_t *runtime);
/* 复制启动/运行诊断；输出指针必须有效。 */
board_runtime_result_t board_runtime_get_diagnostics(
    const board_runtime_t *runtime,
    board_runtime_diagnostics_t *diagnostics);
/* 启用或禁用 FT3168 硬件与 LVGL 输入；禁用仅用于无需触摸唤醒的 Deep-sleep/关机路径。 */
board_runtime_result_t board_runtime_set_touch_active(board_runtime_t *runtime, bool active);
/* 挂载或卸载 TF；真实后端使用厂家 BSP 的 1-bit SDMMC 接口。 */
board_runtime_result_t board_runtime_set_storage_active(board_runtime_t *runtime, bool active);
/* 控制 GPIO46 扬声器功放使能；产品默认关闭。 */
board_runtime_result_t board_runtime_set_speaker_enabled(board_runtime_t *runtime, bool enabled);
/* 通过独立 QMI 驱动切换 OFF/ACTIVE/WOM；非法枚举或未注册驱动会明确失败。 */
board_runtime_result_t board_runtime_set_qmi_mode(
    board_runtime_t *runtime,
    board_runtime_qmi_mode_t mode);
/* 仅当板型、策略、QMI 探测、GPIO21 RTC 能力和 WOM 配置均有效时允许 Deep-sleep。 */
bool board_runtime_qmi_deep_wake_ready(
    const board_runtime_t *runtime,
    bool policy_enabled,
    bool wom_configured);
/* 通过独立 RTC 驱动读取 Unix 秒；未注册驱动时返回 UNSUPPORTED。 */
board_runtime_result_t board_runtime_read_rtc_unix_seconds(
    board_runtime_t *runtime,
    uint64_t *unix_seconds);
/* 请求 AXP2101 安全关机；未注册 PMIC 驱动时返回 UNSUPPORTED。 */
board_runtime_result_t board_runtime_request_pmic_shutdown(board_runtime_t *runtime);
/* 获取 LVGL 互斥锁；runtime_context 必须非空且指向有效 board_runtime_t，生命周期覆盖同步调用；timeout_ms=0 表示无限等待。 */
bool board_runtime_lvgl_lock(void *runtime_context, uint32_t timeout_ms);
/* 释放 LVGL 互斥锁；runtime_context 必须非空且生命周期覆盖同步调用，必须与成功的 lock 成对调用。 */
void board_runtime_lvgl_unlock(void *runtime_context);

#ifdef __cplusplus
}
#endif

#endif
