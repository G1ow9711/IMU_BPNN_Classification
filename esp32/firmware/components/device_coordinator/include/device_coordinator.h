#ifndef IMU_HANDHELD_DEVICE_COORDINATOR_H
#define IMU_HANDHELD_DEVICE_COORDINATOR_H

/*
 * 产品应用协调器：把训练引擎、UI、电源、BLE 和会话摘要串成单一事实链。
 *
 * 本组件只生成按值 effect，不调用 LVGL、NimBLE、LittleFS、GPIO 或 PMIC。
 * 主程序将 effect 放入各任务队列；次数、步数和热量始终仅来自 workout_engine 的
 * fitness_metric_event_t，协调器不重算任何业务指标。
 *
 * 公式、单位、幂等键、复杂度和错误回滚见：
 * docs/应用协调器与事件事实链.md。
 */

/* 引入 BLE LiveState 和 command 1..11 稳定线上合同。 */
#include "ble_service_core.h"
/* 引入统一 30～250 kg 体重和训练目标合同。 */
#include "device_config.h"
/* 引入电源状态、400 mAh 门槛和外设策略。 */
#include "power_manager.h"
/* 引入幂等会话摘要持久化结构。 */
#include "session_store.h"
/* 引入纯 C UI 页面状态和可见快照。 */
#include "ui_state_machine.h"
/* 引入推理锁类、25 Hz 样本、MetricEvent 和振动 FIFO。 */
#include "workout_engine.h"

/* 引入 bool 表达充电、连接和效果开关。 */
#include <stdbool.h>
/* 引入定长整数，保证 ESP32 和主机测试布局稳定。 */
#include <stdint.h>

#ifdef __cplusplus
/* C++ 固件以 C ABI 调用，避免符号改名。 */
extern "C" {
#endif

/* 单次业务调用最多产生 4 个振动请求，超出表示上游异常积压。 */
#define DEVICE_COORDINATOR_MAX_HAPTIC_EFFECTS (4U)
/* 一次锁类最多交付准备期 12 条 MetricEvent，容量与 workout 固定补算 FIFO 一致。 */
#define DEVICE_COORDINATOR_MAX_REPLAY_METRIC_EFFECTS (WORKOUT_REPLAY_EVENT_QUEUE_CAPACITY)
/* 未设置 UTC 时使用 0，不把单调毫秒冒充 Unix 时间。 */
#define DEVICE_COORDINATOR_UNIX_TIME_UNKNOWN (0ULL)

/* 会话摘要标志位；数值属于产品持久化合同。 */
typedef enum device_summary_flag {
    /* 用户正常点击停止，会话完整结束。 */
    DEVICE_SUMMARY_FLAG_COMPLETED = 1U << 0,
    /* 长按关机或系统事件在会话中断时保存。 */
    DEVICE_SUMMARY_FLAG_INTERRUPTED = 1U << 1,
    /* 电量不高于 5% 触发保存后安全关机。 */
    DEVICE_SUMMARY_FLAG_CRITICAL_BATTERY = 1U << 2
} device_summary_flag_t;

/* 主程序只消费 flags 指定的字段，未置位字段固定为零。 */
typedef enum device_effect_flag {
    /* ui 含本次需提交给 LVGL 任务的完整快照。 */
    DEVICE_EFFECT_UI_RENDER = 1U << 0,
    /* live_state 含需通知或读取的权威 BLE LiveStateV1。 */
    DEVICE_EFFECT_BLE_LIVE_STATE = 1U << 1,
    /* metric_event 含用于 PC 动画的低延迟事件，不允许 PC 再加一次。 */
    DEVICE_EFFECT_BLE_EVENT = 1U << 2,
    /* summary 含需交给 session_store_upsert 的幂等摘要。 */
    DEVICE_EFFECT_SUMMARY_WRITE = 1U << 3,
    /* haptics[0..haptic_count) 含需交给非阻塞马达任务的请求。 */
    DEVICE_EFFECT_HAPTIC = 1U << 4,
    /* power_policy 含需由板级层差分应用的完整策略。 */
    DEVICE_EFFECT_POWER_POLICY = 1U << 5,
    /* 必须等 SUMMARY_WRITE 成功刷盘后再执行 PMIC 断电。 */
    DEVICE_EFFECT_SHUTDOWN_AFTER_PERSIST = 1U << 6
} device_effect_flag_t;

/* 统一 UI 与 BLE 控制语义；不直接暴露来源特有事件号。 */
typedef enum device_control {
    /* 创建新会话并进入推理锁类准备页。 */
    DEVICE_CONTROL_START = 0,
    /* 冻结计数、相位和热量时间基准。 */
    DEVICE_CONTROL_PAUSE,
    /* 从当前单调时刻继续，不补算暂停区间。 */
    DEVICE_CONTROL_RESUME,
    /* 停止并产生最终摘要写入 effect。 */
    DEVICE_CONTROL_STOP,
    /* 丢弃 Paused 会话并回主页，对应 BLE command 5。 */
    DEVICE_CONTROL_RESET,
    /* 摘要已落盘，从 Summary 回到 Idle/Home。 */
    DEVICE_CONTROL_DONE,
    /* 仅生成当前 UI 和 LiveState，对应 BLE command 10。 */
    DEVICE_CONTROL_SNAPSHOT,
    /* 主动安全关机；有会话时先产生中断摘要。 */
    DEVICE_CONTROL_SHUTDOWN,
    /* 枚举上界，不可作为输入。 */
    DEVICE_CONTROL_COUNT
} device_control_t;

/* 协调器返回码；负责把下游错误转为可测试产品语义。 */
typedef enum device_coordinator_status {
    /* 输入已提交，effect 按 flags 有效。 */
    DEVICE_COORDINATOR_OK = 0,
    /* 输入在当前状态被安全忽略，没有业务效果。 */
    DEVICE_COORDINATOR_IGNORED = 1,
    /* 空指针、非有限样本/logits、枚举或配置越界。 */
    DEVICE_COORDINATOR_ERR_ARGUMENT = -1,
    /* 当前页面或会话状态不允许该操作。 */
    DEVICE_COORDINATOR_ERR_STATE = -2,
    /* 单调时间倒退或 25 Hz 数据间断不可用。 */
    DEVICE_COORDINATOR_ERR_TIME = -3,
    /* 非充电且电量未知或不高于 8%，禁止新会话。 */
    DEVICE_COORDINATOR_ERR_LOW_BATTERY = -4,
    /* workout/UI/power 领域合同不一致，本次状态已回滚。 */
    DEVICE_COORDINATOR_ERR_DOMAIN = -5
} device_coordinator_status_t;

/* 初始化参数来自已持久化设置；函数不保存该指针。 */
typedef struct device_coordinator_config {
    /* 下一会话序号，必须非零且重启后递增。 */
    uint32_t next_session_seq;
    /* 用户体重，单位 g；合法范围统一为 30000～250000。 */
    uint32_t weight_g;
    /* 初始目标种类取 device_goal_kind_t，默认 0 表示无目标。 */
    uint8_t goal_kind;
    /* 初始目标值；无目标必须为 0，其余目标必须大于 0。 */
    uint32_t goal_value;
    /* 是否允许 GPIO21/QMI 作 Deep-sleep 运动唤醒源。 */
    bool allow_imu_deep_wake;
} device_coordinator_config_t;

/* 一次调用的全部外部副作用以值拷贝表达，可安全跨 FreeRTOS 队列。 */
typedef struct device_effects {
    /* device_effect_flag_t 按位组合，决定以下哪些字段有效。 */
    uint32_t flags;
    /* LVGL 任务只读完整 UI 快照，不反向修改领域状态。 */
    ui_context_t ui;
    /* BLE 任务将该结构交给 ble_service_encode_live_state_v1。 */
    ble_service_live_state_v1_t live_state;
    /* Event 直接拷贝 workout_engine 产生的唯一 MetricEvent。 */
    fitness_metric_event_t metric_event;
    /* 锁类补算事件按 event_seq 递增排列；每条都保留原始 IMU 设备毫秒。 */
    fitness_metric_event_t replay_metric_events[DEVICE_COORDINATOR_MAX_REPLAY_METRIC_EFFECTS];
    /* replay_metric_events 的有效数量，范围 0..12。 */
    uint8_t replay_metric_event_count;
    /* 存储任务将该值交给 session_store_upsert，last_event_seq 是幂等键。 */
    session_summary_t summary;
    /* 一次调用中最多交付 4 个已排序振动请求。 */
    fitness_haptic_request_t haptics[DEVICE_COORDINATOR_MAX_HAPTIC_EFFECTS];
    /* haptics 的有效数量，范围 0..4。 */
    uint8_t haptic_count;
    /* 板级电源任务差分应用该策略，不在协调器内访问硬件。 */
    power_policy_t power_policy;
} device_effects_t;

/* 协调器保存纯数据状态；无堆分配、无 OS 句柄、无硬件指针。 */
typedef struct device_coordinator {
    /* 保存推理锁类、计数、步数、热量和振动 FIFO。 */
    workout_engine_t workout;
    /* 保存纯 UI 页面和当前展示值。 */
    ui_context_t ui;
    /* 保存电量门槛、BLE 连接和外设策略状态。 */
    power_manager_t power;
    /* 保存下一持久化会话序号，成功 start 后递增且跳过 0。 */
    uint32_t next_session_seq;
    /* 保存用户体重，单位 g。 */
    uint32_t weight_g;
    /* 保存训练目标种类，跨会话保持直到再次设置。 */
    uint8_t goal_kind;
    /* 保存次数、秒或 mcal 目标值。 */
    uint32_t goal_value;
    /* BLE/PC 使用的权威状态修订号。 */
    uint32_t state_revision;
    /* 保存最近已提交输入时间，单位单调 ms。 */
    uint64_t last_monotonic_ms;
    /* 保存当前会话开始时间，单位单调 ms。 */
    uint64_t session_started_ms;
    /* 保存排除暂停后的活动累计时长，单位 ms。 */
    uint64_t active_elapsed_ms;
    /* 保存上次活动时长记账点，单位单调 ms。 */
    uint64_t last_active_accounted_ms;
    /* 保存摘要已吸收的最大 MetricEvent 序号。 */
    uint32_t last_event_seq;
    /* 保存已吸收 MetricEvent 数量。 */
    uint32_t event_count;
    /* 保存稳定度 Q15 总和，仅用于摘要平均值。 */
    uint64_t stability_sum_q15;
    /* 保存本会话最低稳定度 Q15。 */
    uint16_t minimum_stability_q15;
    /* 标记是否已有稳定度样本，避免空会话误报 32767。 */
    bool has_stability;
    /* 保存低电提醒是否已发送，同一下降区间不重复振动。 */
    bool low_battery_warning_sent;
} device_coordinator_t;

/* 自检成功后初始化到 Idle/Home；空指针或非法配置返回参数错误。 */
device_coordinator_status_t device_coordinator_init(
    device_coordinator_t *coordinator,
    const device_coordinator_config_t *config,
    uint64_t monotonic_ms);
/* 统一处理开始、暂停、继续、停止、完成、快照、重置和关机。 */
device_coordinator_status_t device_coordinator_handle_control(
    device_coordinator_t *coordinator,
    device_control_t control,
    uint64_t monotonic_ms,
    device_effects_t *effects);
/*
 * 接收一次双 M0 融合 logits：logits 形状固定为 `[WORKOUT_CLASS_COUNT]=[11]`，
 * 必须指向连续 float 数组，
 * 顺序固定为训练清单 11 类，数值为无量纲未归一化分类分数且必须全部有限。
 * 指针不能为空，仅在本次调用期间借用；协调器不会保存数组地址。
 */
device_coordinator_status_t device_coordinator_push_inference(
    device_coordinator_t *coordinator,
    const float logits[WORKOUT_CLASS_COUNT],
    uint64_t monotonic_ms,
    uint16_t quality_flags,
    device_effects_t *effects);
/* 更新下次新会话使用的体重；当前 PREPARING/RUNNING/PAUSED 会话保持启动时体重。 */
device_coordinator_status_t device_coordinator_set_next_session_weight(
    device_coordinator_t *coordinator,
    uint32_t weight_g,
    uint64_t monotonic_ms,
    device_effects_t *effects);
/* 保存训练目标并立即刷新 LiveState 0～100% 或无目标 255。 */
device_coordinator_status_t device_coordinator_set_goal(
    device_coordinator_t *coordinator,
    uint8_t goal_kind,
    uint32_t goal_value,
    uint64_t monotonic_ms,
    device_effects_t *effects);
/* 接收一个严格 25 Hz 六轴点；仅 workout_engine 可产生 MetricEvent。 */
device_coordinator_status_t device_coordinator_push_sample(
    device_coordinator_t *coordinator,
    const motion_phase_sample_t *sample,
    bool count_input_valid,
    uint16_t quality_flags,
    device_effects_t *effects);
/* 更新 PMIC 百分比和充电状态；15/8/5% 分别提醒、拒绝启动、保存关机。 */
device_coordinator_status_t device_coordinator_update_battery(
    device_coordinator_t *coordinator,
    uint8_t battery_percent,
    bool charging,
    uint64_t monotonic_ms,
    device_effects_t *effects);
/* 更新 PC BLE 连接属性，同步 UI 图标、LiveState 和功耗策略。 */
device_coordinator_status_t device_coordinator_set_ble_connected(
    device_coordinator_t *coordinator,
    bool connected,
    uint64_t monotonic_ms,
    device_effects_t *effects);
/* 原子处理 SCREEN_TIMEOUT、USER_WAKE 或 LONG_IDLE，使 UI 与功耗状态始终同步提交。 */
device_coordinator_status_t device_coordinator_handle_idle_power_event(
    device_coordinator_t *coordinator,
    power_event_type_t event_type,
    uint64_t monotonic_ms,
    device_effects_t *effects);
/* 把可控的 UI 事件映射为统一命令；不是控制事件时返回 false。 */
bool device_coordinator_control_from_ui(ui_event_type_t event, device_control_t *control);
/* 把 BLE command 1..5/10 映射为统一命令；其它命令交给设置/传输组件。 */
bool device_coordinator_control_from_ble(uint8_t command_id, device_control_t *control);

#ifdef __cplusplus
/* 结束 C ABI 声明区。 */
}
#endif

#endif /* IMU_HANDHELD_DEVICE_COORDINATOR_H */
