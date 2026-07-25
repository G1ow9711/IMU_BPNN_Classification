#ifndef IMU_HANDHELD_POWER_MANAGER_H
#define IMU_HANDHELD_POWER_MANAGER_H

/* 引入布尔和定长整数，保证策略结构在主机和 ESP32 上布局一致。 */
#include <stdbool.h>
#include <stdint.h>

/* ESP-IDF 构建读取 Kconfig；纯 C 主机测试不依赖生成的 sdkconfig.h。 */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ESP-IDF 使用 Kconfig 电池容量，默认 400 mAh；主机测试固定同一默认值。 */
#ifdef ESP_PLATFORM
#define POWER_BATTERY_CAPACITY_MAH ((float)CONFIG_POWER_BATTERY_CAPACITY_MAH)
#else
#define POWER_BATTERY_CAPACITY_MAH (400.0F)
#endif
/* 仅按额定容量的 80% 计算可用能量，预留老化、温度和截止电压余量。 */
#define POWER_USABLE_CAPACITY_RATIO (0.80F)
/* ESP-IDF 从 Kconfig 读取训练续航小时；主机测试固定默认 6 小时。 */
#ifdef ESP_PLATFORM
#define POWER_ACTIVE_RUNTIME_TARGET_HOURS ((float)CONFIG_POWER_ACTIVE_RUNTIME_TARGET_HOURS)
#else
#define POWER_ACTIVE_RUNTIME_TARGET_HOURS (6.0F)
#endif
/* ESP-IDF 把 Kconfig 深待机天数换算为小时；主机测试固定默认 168 小时。 */
#ifdef ESP_PLATFORM
#define POWER_DEEP_STANDBY_TARGET_HOURS \
    ((float)CONFIG_POWER_DEEP_STANDBY_TARGET_DAYS * 24.0F)
#else
#define POWER_DEEP_STANDBY_TARGET_HOURS (168.0F)
#endif
/* 厂家原配 400 mAh 电池降到 15% 时提示，但仍允许完成当前会话。 */
#define POWER_BATTERY_WARNING_PERCENT (15U)
/* 厂家原配电池降到 8% 时禁止启动新会话；充电时可解除限制。 */
#define POWER_BATTERY_BLOCK_START_PERCENT (8U)
/* 厂家原配电池降到 5% 时触发安全保存和关机；充电时不强制关机。 */
#define POWER_BATTERY_CRITICAL_PERCENT (5U)

/* 描述设备功耗状态；充电是上下文属性，不单独破坏会话状态。 */
typedef enum {
    /* 上电后初始化板卡、存储、算法和通信，尚不接受训练命令。 */
    POWER_STATE_BOOT = 0,
    /* 首页交互态；屏幕点亮且等待用户开始训练或进入设置。 */
    POWER_STATE_HOME,
    /* 训练运行态；IMU、推理、计数、界面和按需 BLE 全部工作。 */
    POWER_STATE_RUNNING,
    /* 训练仍运行但 AMOLED 关闭，计数和 BLE 不因熄屏停止。 */
    POWER_STATE_RUNNING_SCREEN_OFF,
    /* 会话暂停态；保留会话累计值但暂停新增动作指标。 */
    POWER_STATE_PAUSED,
    /* 无训练但 BLE 已连接的待机态；保持链路并允许自动 Light-sleep。 */
    POWER_STATE_CONNECTED_STANDBY,
    /* 无连接的低频广播待机态；等待 PC 或用户唤醒。 */
    POWER_STATE_ADVERTISING_STANDBY,
    /* ESP Deep-sleep 态；仅保留配置的触摸、RTC 或运动唤醒源。 */
    POWER_STATE_DEEP_STANDBY,
    /* 已请求保存并关闭外设的安全关机态，最终交给 PMIC 断电。 */
    POWER_STATE_SAFE_SHUTDOWN
} power_state_t;

/* 描述电源状态输入；事件由 UI、BLE、PMIC 和空闲计时器产生。 */
typedef enum {
    /* 启动自检和资源初始化完成，可从 BOOT 进入 HOME。 */
    POWER_EVENT_BOOT_COMPLETED = 0,
    /* 用户或 BLE 命令成功启动训练会话。 */
    POWER_EVENT_SESSION_STARTED,
    /* 当前训练被用户暂停，但会话数据仍保留。 */
    POWER_EVENT_SESSION_PAUSED,
    /* 暂停会话恢复计数与推理。 */
    POWER_EVENT_SESSION_RESUMED,
    /* 当前会话完成保存并回到非训练状态。 */
    POWER_EVENT_SESSION_STOPPED,
    /* 屏幕空闲计时到期，仅关闭显示而不结束会话。 */
    POWER_EVENT_SCREEN_TIMEOUT,
    /* 按键、触摸或允许的运动源请求唤醒界面。 */
    POWER_EVENT_USER_WAKE,
    /* PC 建立并恢复 BLE 应用层连接。 */
    POWER_EVENT_BLE_CONNECTED,
    /* BLE 链路断开；是否继续训练由当前会话状态决定。 */
    POWER_EVENT_BLE_DISCONNECTED,
    /* 无会话且长时间无交互，允许进入更深待机。 */
    POWER_EVENT_LONG_IDLE,
    /* 用户明确请求关机，必须先持久化关键状态。 */
    POWER_EVENT_SHUTDOWN_REQUESTED,
    /* 非充电时电池达到 5% 临界门槛，强制保存并安全关机。 */
    POWER_EVENT_CRITICAL_BATTERY,
    /* PMIC 充电状态变化；事件 charging 字段携带新状态。 */
    POWER_EVENT_CHARGING_CHANGED
} power_event_type_t;

/* 描述 BLE 射频策略；CONNECTED_MODEM_SLEEP 保持连接且允许自动 Light-sleep。 */
typedef enum {
    /* 关闭 BLE 控制器和广播，适用于 Deep-sleep 或安全关机。 */
    POWER_BLE_OFF = 0,
    /* 使用短间隔快速广播，缩短用户主动连接等待时间。 */
    POWER_BLE_FAST_ADVERTISING,
    /* 使用长间隔慢广播，降低长时间待机射频平均电流。 */
    POWER_BLE_SLOW_ADVERTISING,
    /* 已连接且处于高交互速率，允许及时发送训练状态。 */
    POWER_BLE_CONNECTED_ACTIVE,
    /* 已连接但空闲，启用 modem-sleep 并保留 GATT 会话。 */
    POWER_BLE_CONNECTED_MODEM_SLEEP
} power_ble_mode_t;

/* 描述 QMI8658 策略；ACTIVE_25HZ 对应算法严格 25 Hz 输入。 */
typedef enum {
    /* 关闭 QMI8658 采样，适用于安全关机或不允许运动唤醒的深待机。 */
    POWER_IMU_OFF = 0,
    /* 仅启用 QMI8658 WOM 低功耗运动检测，不输出 25 Hz 算法流。 */
    POWER_IMU_WAKE_ON_MOTION,
    /* 以严格 25 Hz 运行六轴采样和重采样，供 62 点推理窗口使用。 */
    POWER_IMU_ACTIVE_25HZ
} power_imu_mode_t;

/* 描述低电量级别；策略不使用未经电芯规格确认的固定电压阈值。 */
typedef enum {
    /* 电量高于 15%，允许全部常规功能。 */
    POWER_BATTERY_NORMAL = 0,
    /* 电量 8%～15%，提示用户但允许完成或开始会话。 */
    POWER_BATTERY_WARNING,
    /* 电量 5%～8%，非充电时禁止启动新会话。 */
    POWER_BATTERY_BLOCK_START,
    /* 电量不高于 5%，非充电时必须保存并进入安全关机。 */
    POWER_BATTERY_CRITICAL
} power_battery_level_t;

/* 描述一次电源事件；charging 仅用于 CHARGING_CHANGED。 */
typedef struct {
    /* 指明事件类型。 */
    power_event_type_t type;
    /* 保存单调毫秒时间，供日志记录状态切换。 */
    uint32_t monotonic_ms;
    /* 保存新的充电状态；仅充电变化事件读取。 */
    bool charging;
} power_event_t;

/* 汇总每个功耗状态对应的外设和系统策略。 */
typedef struct {
    /* 保存 CPU 最低频率 MHz；空闲时允许降到 40 MHz。 */
    uint16_t min_cpu_mhz;
    /* 保存 CPU 最高频率 MHz；特征和双模型计算可临时锁到 240 MHz。 */
    uint16_t max_cpu_mhz;
    /* 标记是否允许 FreeRTOS Tickless Idle 自动进入 Light-sleep。 */
    bool automatic_light_sleep;
    /* 标记是否启用动态频率缩放。 */
    bool dynamic_frequency_scaling;
    /* 标记 AMOLED 是否应点亮。 */
    bool display_on;
    /* 保存 AMOLED 目标亮度百分比；关闭时为 0。 */
    uint8_t display_brightness_percent;
    /* 保存 QMI8658 工作模式。 */
    power_imu_mode_t imu_mode;
    /* 保存 BLE 广播/连接/省电模式。 */
    power_ble_mode_t ble_mode;
    /* 标记触摸是否保持活动扫描；false 表示低功耗监视。 */
    bool touch_active;
    /* 标记 TF/SDMMC 是否保持挂载；常规状态默认关闭。 */
    bool storage_active;
    /* 标记功放/codec 是否上电；默认只有用户提示音时临时开启。 */
    bool speaker_active;
    /* 标记麦克风/ES7210 是否上电；v1 固定关闭。 */
    bool microphones_active;
    /* 标记是否应进入 ESP Deep-sleep。 */
    bool request_deep_sleep;
    /* 标记是否应调用已验证的 AXP2101 软关机。 */
    bool request_pmic_shutdown;
    /* 标记是否允许 QMI GPIO21 作为 Deep-sleep 唤醒源。 */
    bool enable_imu_deep_wake;
    /* 标记触摸 GPIO38 是否可作 Light-sleep GPIO 唤醒。 */
    bool enable_touch_light_wake;
    /* 标记 RTC GPIO39 是否可作 Light-sleep GPIO 唤醒。 */
    bool enable_rtc_light_wake;
} power_policy_t;

/* 保存状态机上下文；不包含 ESP-IDF 句柄，主机测试可直接构造。 */
typedef struct {
    /* 保存当前功耗状态。 */
    power_state_t state;
    /* 保存熄屏前状态，用户唤醒后恢复 RUNNING/HOME/PAUSED。 */
    power_state_t screen_off_resume_state;
    /* 标记训练会话是否存在。 */
    bool session_active;
    /* 标记 BLE 是否连接 PC。 */
    bool ble_connected;
    /* 标记 USB/电池是否充电。 */
    bool charging;
    /* 保存最近 PMIC 电量百分比；255 表示未知。 */
    uint8_t battery_percent;
    /* 保存最近状态切换的单调毫秒。 */
    uint32_t state_entered_ms;
    /* 标记产品设置是否允许 QMI Deep-sleep 运动唤醒。 */
    bool allow_imu_deep_wake;
} power_manager_t;

/* 保存生产空闲计时器状态；只使用单调毫秒，不依赖 RTC 或系统时区。 */
typedef struct {
    /* 保存最近一次有效触摸、按键或主动训练控制的单调毫秒。 */
    uint64_t last_activity_ms;
    /* 标记当前活动周期是否已经发出一次熄屏事件，防止每秒重复投递。 */
    bool screen_timeout_emitted;
    /* 标记当前活动周期是否已经发出一次长空闲事件，防止 Deep-sleep 请求重复入队。 */
    bool long_idle_emitted;
} power_idle_timer_t;

/* 初始化到 BOOT 状态；allow_imu_deep_wake 默认应为 false。 */
void power_manager_init(
    power_manager_t *manager,
    uint32_t monotonic_ms,
    bool allow_imu_deep_wake);
/* 处理一个电源事件；返回 true 表示状态或上下文发生变化。 */
bool power_manager_dispatch(power_manager_t *manager, const power_event_t *event);
/* 根据当前状态生成确定性外设策略；函数不直接访问硬件。 */
power_policy_t power_manager_policy(const power_manager_t *manager);
/* 更新 PMIC 电量并返回低电量级别；百分比必须为 0~100。 */
power_battery_level_t power_manager_update_battery(
    power_manager_t *manager,
    uint8_t percent,
    bool charging);
/* 判断是否允许开始新会话；充电时允许，非充电时必须高于 8%。 */
bool power_manager_can_start_session(const power_manager_t *manager);
/* 按额定容量、80% 可用率和目标小时数计算平均电流预算，单位 mA。 */
float power_budget_max_average_current_ma(float target_hours);
/* 按实测平均电流估算续航小时；电流不大于零时返回 0。 */
float power_budget_estimated_runtime_hours(float measured_average_current_ma);
/* 返回功耗状态名称，供日志和诊断页使用。 */
const char *power_state_name(power_state_t state);
/* 初始化生产空闲计时器；now_ms 是启动或业务域就绪时的单调毫秒。 */
void power_idle_timer_init(power_idle_timer_t *timer, uint64_t now_ms);
/* 记录用户活动并重新开始熄屏与长空闲计时；时间倒退也以本次值重新建立基准。 */
void power_idle_timer_note_activity(power_idle_timer_t *timer, uint64_t now_ms);
/*
 * 查询是否到达一个尚未发出的空闲门槛；返回 true 时 event_type 写入 SCREEN_TIMEOUT 或 LONG_IDLE。
 * screen_timeout_ms 和 long_idle_ms 必须非零，且 long_idle_ms 必须严格大于 screen_timeout_ms。
 * 会话存在时只允许熄屏，不允许长空闲 Deep-sleep；同一次查询最多返回一个事件。
 */
bool power_idle_timer_poll(
    power_idle_timer_t *timer,
    uint64_t now_ms,
    uint32_t screen_timeout_ms,
    uint32_t long_idle_ms,
    bool session_active,
    power_event_type_t *event_type);

#ifdef __cplusplus
}
#endif

#endif
