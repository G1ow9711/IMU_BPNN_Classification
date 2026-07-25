#ifndef FITNESS_CORE_H
#define FITNESS_CORE_H

/*
 * 健身领域核心公共接口。
 *
 * 本组件不读取 IMU、不调用 GPIO、不依赖 FreeRTOS，因而既能被 ESP-IDF 固件调用，
 * 也能由 Windows/Linux 主机测试直接编译。上游算法负责把连续 IMU 信号解释为
 * “主相位、次相位、腾空、落地”等离散相位；本组件只确认完整动作周期、维护
 * 会话指标并估算热量。
 *
 * 详细公式、单位、边界和复杂度见：docs/计数卡路里算法.md。
 */

/* bool 用于表达计数是否成立等二值结果。 */
#include <stdbool.h>
/* uint32_t/uint64_t 等定宽整数保证 ESP32 与主机端字段宽度一致。 */
#include <stdint.h>

#ifdef __cplusplus
/* C++ 调用方按 C ABI 链接，避免函数名改编导致 ESP-IDF 链接失败。 */
extern "C" {
#endif

/* 六轴采样固定顺序为 gx、gy、gz、ax、ay、az；前三轴单位 deg/s，后三轴单位 g。 */
#define FITNESS_IMU_AXIS_COUNT 6U
/* 热量公式的整数分母；推导见 docs/计数卡路里算法.md。 */
#define FITNESS_CALORIE_DENOMINATOR 24000000ULL

/* 11 类动作顺序必须与 Python 模型输出和 BLE 协议完全一致。 */
typedef enum {
    /* 早安式体前屈，使用两相位完整周期计数。 */
    FITNESS_ACTION_GOOD_MORNING = 0,
    /* 开合跳，腕戴场景使用手臂张开与合拢的主向/回向完整周期计数。 */
    FITNESS_ACTION_JUMPING_JACK = 1,
    /* 跳跃弓步，使用起跳-腾空-落地完整周期计数。 */
    FITNESS_ACTION_JUMPING_LUNGE = 2,
    /* 跳跃深蹲，使用起跳-腾空-落地完整周期计数。 */
    FITNESS_ACTION_JUMPING_SQUAT = 3,
    /* 普通弓步，使用两相位完整周期计数。 */
    FITNESS_ACTION_LUNGE = 4,
    /* 静坐，不计次数，只累计持续时长。 */
    FITNESS_ACTION_SIT = 5,
    /* 普通深蹲，使用两相位完整周期计数。 */
    FITNESS_ACTION_SQUAT = 6,
    /* 小跑，接受上游步峰并按步累计。 */
    FITNESS_ACTION_TROT = 7,
    /* 收腹跳，使用起跳-腾空-落地完整周期计数。 */
    FITNESS_ACTION_TUCK_JUMP = 8,
    /* 行走，接受上游步峰并按步累计。 */
    FITNESS_ACTION_WALK = 9,
    /* 挥手，使用两相位完整周期计数。 */
    FITNESS_ACTION_WAVE = 10,
    /* 动作类别总数，用于边界检查，不能作为真实类别。 */
    FITNESS_ACTION_COUNT = 11
} fitness_action_t;

/* MetricEvent 的业务指标类型；同一事件只允许表达一种主指标增量。 */
typedef enum {
    /* 力量或跳跃动作完成一次完整重复。 */
    FITNESS_METRIC_REPETITION = 0,
    /* walk/trot 接受一个经上游确认的步峰。 */
    FITNESS_METRIC_STEP = 1,
    /* sit 累计持续时间，delta 和 total 的单位均为 ms。 */
    FITNESS_METRIC_DURATION_MS = 2
} fitness_metric_kind_t;

/* 统一错误码；所有公开函数都用可审计错误而不是静默失败。 */
typedef enum {
    /* 调用成功，输出字段有效。 */
    FITNESS_STATUS_OK = 0,
    /* 指针为空、枚举越界或数值超出合同。 */
    FITNESS_STATUS_INVALID_ARGUMENT = 1,
    /* 会话未启动、动作不匹配或状态顺序不允许。 */
    FITNESS_STATUS_INVALID_STATE = 2,
    /* 单调时钟倒退或一次 tick 间隔超过允许上限。 */
    FITNESS_STATUS_INVALID_TIME = 3
} fitness_status_t;

/* MetricEvent 是 UI、BLE 和存储的唯一指标事实源。 */
typedef struct {
    /* 会话序号由持久化层分配；重启后也应保持递增。 */
    uint32_t session_seq;
    /* 会话内事件序号从 1 递增，用于 BLE 去重和日志排序。 */
    uint32_t event_seq;
    /* 设备启动后的单调毫秒时钟；不表示公历时间。 */
    uint64_t monotonic_ms;
    /* 产生指标的动作类别，必须与当前会话动作一致。 */
    fitness_action_t action;
    /* 指标类型决定 delta_value 和 total_value 的单位。 */
    fitness_metric_kind_t metric_kind;
    /* 本事件增量：次数/步数通常为 1，静坐时长单位为 ms。 */
    uint32_t delta_value;
    /* 当前会话该指标累计值：次数、步数或 ms。 */
    uint64_t total_value;
    /* Q15 稳定度范围 0..32767，仅表示时间决策稳定性，不冒充概率。 */
    uint16_t stability_q15;
    /* fitness_quality_flag_t 按位组合，描述污染、插值或冻结状态。 */
    uint16_t quality_flags;
    /* 毛热量累计值，单位 microkcal，即 10^-6 kcal。 */
    uint64_t gross_microkcal;
    /* 活动热量累计值，扣除 1 MET 静息部分，单位 microkcal。 */
    uint64_t active_microkcal;
} fitness_metric_event_t;

/* 会话状态只保存领域数据，不持有 UI、BLE 或文件句柄。 */
typedef struct {
    /* true 表示 start 已成功且 stop 尚未调用。 */
    bool active;
    /* 当前识别动作；产品训练引擎在有界累计确认后保持到本次会话停止。 */
    fitness_action_t action;
    /* 会话唯一序号，原样复制到每个 MetricEvent。 */
    uint32_t session_seq;
    /* 下一个事件使用的序号；首次事件为 1。 */
    uint32_t next_event_seq;
    /* 上次时间推进的单调毫秒，用于计算 dt。 */
    uint64_t last_tick_ms;
    /* 用户体重单位 g；0 表示未设置，此时热量保持为 0。 */
    uint32_t weight_g;
    /* 当前会话完成次数，仅供 REPETITION 事件使用。 */
    uint64_t repetitions;
    /* 当前会话步数，仅供 STEP 事件使用。 */
    uint64_t steps;
    /* 当前会话静坐持续时间，单位 ms。 */
    uint64_t sit_duration_ms;
    /* 已通过事件发布的静坐时长，单位 ms，用于每整秒发布一次。 */
    uint64_t published_sit_duration_ms;
    /* 毛热量累计整数部分，单位 microkcal。 */
    uint64_t gross_microkcal;
    /* 活动热量累计整数部分，单位 microkcal。 */
    uint64_t active_microkcal;
    /* 毛热量除以固定分母后的余数，避免短 tick 截断损失。 */
    uint64_t gross_calorie_remainder;
    /* 活动热量除以固定分母后的余数，避免短 tick 截断损失。 */
    uint64_t active_calorie_remainder;
} fitness_session_t;

/* 上游动作相位分类结果；本组件不定义相位如何从 IMU 得出。 */
typedef enum {
    /* 动作基线或返回端点；连续两相位动作可由主方向重新越界确认返回。 */
    FITNESS_PHASE_REST = 0,
    /* 两相位动作的第一方向，例如下蹲或向一侧挥动。 */
    FITNESS_PHASE_PRIMARY = 1,
    /* 两相位动作的第二方向，例如最低点或反向挥动。 */
    FITNESS_PHASE_SECONDARY = 2,
    /* 跳跃动作开始离地前的主动推进。 */
    FITNESS_PHASE_TAKEOFF = 3,
    /* 低支持力/腾空阶段。 */
    FITNESS_PHASE_FLIGHT = 4,
    /* 落地冲击阶段。 */
    FITNESS_PHASE_LANDING = 5,
    /* 落地后恢复稳定但尚未完全回到基线。 */
    FITNESS_PHASE_RECOVERY = 6,
    /* 相位总数，仅用于边界检查。 */
    FITNESS_PHASE_COUNT = 7
} fitness_motion_phase_t;

/* 8 类重复动作被归纳为两种有限状态机。 */
typedef enum {
    /* 腕戴重复动作依次经历主向、反向并回到主向起点或稳定基线。 */
    FITNESS_REP_MODE_TWO_PHASE = 0
} fitness_rep_mode_t;

/* 重复动作状态机；字段公开便于静态分配，但调用方不得直接改写。 */
typedef struct {
    /* true 表示 action 属于 8 个可重复计数动作且初始化成功。 */
    bool initialized;
    /* 当前计数动作。 */
    fitness_action_t action;
    /* 两相位或跳跃状态机模式。 */
    fitness_rep_mode_t mode;
    /* 内部状态编号，只由 fitness_rep_counter_update 修改。 */
    uint8_t state;
    /* 尚未确认稳定的候选相位。 */
    fitness_motion_phase_t candidate_phase;
    /* 候选相位连续出现次数；达到 2 才触发状态转移。 */
    uint8_t candidate_samples;
    /* 最近一次输入的单调毫秒，用于拒绝时间倒退。 */
    uint64_t last_update_ms;
    /* 当前周期起始时间，用于最短/最长周期约束。 */
    uint64_t cycle_start_ms;
    /* 上一次有效计数时间，用于不应期去重。 */
    uint64_t last_rep_ms;
    /* 动作允许的最短完整周期，单位 ms。 */
    uint32_t min_cycle_ms;
    /* 动作允许的最长完整周期，单位 ms。 */
    uint32_t max_cycle_ms;
    /* 两次有效计数之间的最短间隔，单位 ms。 */
    uint32_t refractory_ms;
    /* 已确认完整重复总数。 */
    uint64_t total_repetitions;
} fitness_rep_counter_t;

/* walk/trot 步峰去重器；实际峰检测由上游 IMU 算法完成。 */
typedef struct {
    /* true 表示 action 为 walk 或 trot 且初始化成功。 */
    bool initialized;
    /* 当前步态类别，只允许 WALK 或 TROT。 */
    fitness_action_t action;
    /* 上一次接受步峰的单调时间，单位 ms。 */
    uint64_t last_step_ms;
    /* 两个步峰的最短间隔，避免同一冲击重复计步。 */
    uint32_t min_step_interval_ms;
    /* 已接受步数。 */
    uint64_t total_steps;
} fitness_step_counter_t;

/* 返回动作的固定 milliMET；例如 3.8 MET 返回 3800。 */
uint32_t fitness_action_met_milli(fitness_action_t action);

/* 以指定动作、会话序号、体重和起始单调时间启动领域会话。 */
fitness_status_t fitness_session_start(
    fitness_session_t *session,
    uint32_t session_seq,
    fitness_action_t action,
    uint32_t weight_g,
    uint64_t start_ms);

/* 停止会话；停止后任何 tick 或指标写入都会返回 INVALID_STATE。 */
fitness_status_t fitness_session_stop(fitness_session_t *session);

/* 推进会话时间并累计热量；sit 每累计完整 1000 ms 可输出一个时长事件。 */
fitness_status_t fitness_session_tick(
    fitness_session_t *session,
    uint64_t now_ms,
    uint16_t stability_q15,
    uint16_t quality_flags,
    fitness_metric_event_t *optional_sit_event,
    bool *event_emitted);

/*
 * 暂停/恢复时只把热量积分时间基准移动到 now_ms，不增加热量或时长。
 * now_ms 不得早于 last_tick_ms；该接口防止暂停时间被下一次 tick 计入训练。
 */
fitness_status_t fitness_session_rebase_time(
    fitness_session_t *session,
    uint64_t now_ms);

/*
 * 在同一训练会话内切换动作；保留次数、步数、时长、热量和事件序号，只更新当前动作与时间基准。
 * now_ms 使用设备单调毫秒且不得早于 last_tick_ms；调用方必须先完成新动作连续确认。
 */
fitness_status_t fitness_session_switch_action(
    /* 非空且 active 的会话对象；成功后 action 和 last_tick_ms 会更新。 */
    fitness_session_t *session,
    /* 新动作必须位于 FITNESS_ACTION_GOOD_MORNING..FITNESS_ACTION_WAVE。 */
    fitness_action_t action,
    /* 动作确认时刻，单位毫秒；切换本身不补算未覆盖区间的热量。 */
    uint64_t now_ms);

/* 把已确认的 REP/STEP 增量写入会话并生成唯一 MetricEvent。 */
fitness_status_t fitness_session_record_count(
    fitness_session_t *session,
    fitness_metric_kind_t metric_kind,
    uint32_t delta_value,
    uint64_t now_ms,
    uint16_t stability_q15,
    uint16_t quality_flags,
    fitness_metric_event_t *event);

/* 初始化 8 类重复动作的有限状态机；sit/walk/trot 会被拒绝。 */
fitness_status_t fitness_rep_counter_init(
    fitness_rep_counter_t *counter,
    fitness_action_t action);

/* 输入一个相位观测；只有完整且满足时长约束的周期才令 rep_completed=true。 */
fitness_status_t fitness_rep_counter_update(
    fitness_rep_counter_t *counter,
    fitness_motion_phase_t phase,
    uint64_t now_ms,
    bool *rep_completed);

/* 清空不完整周期但保留累计次数，用于动作段重置或质量冻结。 */
fitness_status_t fitness_rep_counter_reset_cycle(fitness_rep_counter_t *counter);

/* 初始化 walk/trot 步峰去重器；其它动作会被拒绝。 */
fitness_status_t fitness_step_counter_init(
    fitness_step_counter_t *counter,
    fitness_action_t action);

/* 接受一个上游步峰；过密或时间倒退的步峰不会增加计数。 */
fitness_status_t fitness_step_counter_accept(
    fitness_step_counter_t *counter,
    uint64_t now_ms,
    bool *step_accepted);

#ifdef __cplusplus
/* 结束 C ABI 声明区。 */
}
#endif

#endif /* FITNESS_CORE_H */
