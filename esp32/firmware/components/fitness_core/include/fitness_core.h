#ifndef FITNESS_CORE_H
#define FITNESS_CORE_H

/*
 * 健身领域核心公共接口。
 *
 * 本组件不读取 IMU、不调用 GPIO、不依赖 FreeRTOS，因而既能被 ESP-IDF 固件调用，
 * 也能由 Windows/Linux 主机测试直接编译。上游算法负责把连续 IMU 信号解释为
 * “主相位、次相位、腾空、落地”等离散相位；本组件只确认完整动作周期、维护
 * 会话指标、估算热量并生成振动请求。
 *
 * 详细公式、单位、边界和复杂度见：docs/计数卡路里与振动算法.md。
 */

/* size_t 用于返回一次最多输出多少个修复后的六轴采样点。 */
#include <stddef.h>
/* bool 用于表达计数是否成立、队列是否入队成功等二值结果。 */
#include <stdbool.h>
/* uint32_t/uint64_t 等定宽整数保证 ESP32 与主机端字段宽度一致。 */
#include <stdint.h>

#ifdef __cplusplus
/* C++ 调用方按 C ABI 链接，避免函数名改编导致 ESP-IDF 链接失败。 */
extern "C" {
#endif

/* 六轴采样固定顺序为 gx、gy、gz、ax、ay、az；前三轴单位 deg/s，后三轴单位 g。 */
#define FITNESS_IMU_AXIS_COUNT 6U
/* 最多缓存 3 个受振动污染的采样点；超过该长度时停止计数而不伪造长段波形。 */
#define FITNESS_HAPTIC_MAX_INTERPOLATED_SAMPLES 3U
/* 一次干净采样到来时最多输出 3 个插值点和当前干净点，共 4 点。 */
#define FITNESS_HAPTIC_MAX_OUTPUT_SAMPLES 4U
/* 振动请求环形队列固定 16 项；静态分配避免运行期堆碎片。 */
#define FITNESS_HAPTIC_QUEUE_CAPACITY 16U
/* 电机停止后额外屏蔽 80 ms，覆盖机械余振和电源纹波。 */
#define FITNESS_HAPTIC_TAIL_GUARD_MS 80U
/* 热量公式的整数分母；推导见 docs/计数卡路里与振动算法.md。 */
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
    FITNESS_STATUS_INVALID_TIME = 3,
    /* 固定容量振动队列已满，本次请求未写入。 */
    FITNESS_STATUS_QUEUE_FULL = 4
} fitness_status_t;

/* 事件/采样质量位可以组合；0 表示当前数据无需额外说明。 */
typedef enum {
    /* 当前原始采样落在电机振动及其 80 ms 余振保护区内。 */
    FITNESS_QUALITY_HAPTIC_CONTAMINATED = 1U << 0,
    /* 当前采样由前后干净点线性插值得到，不是原始测量值。 */
    FITNESS_QUALITY_INTERPOLATED = 1U << 1,
    /* 污染连续超过 3 点，计数器应冻结到下一干净点。 */
    FITNESS_QUALITY_COUNTER_FROZEN = 1U << 2,
    /* 上游识别稳定度不足，但仍保留事件供诊断。 */
    FITNESS_QUALITY_LOW_STABILITY = 1U << 3
} fitness_quality_flag_t;

/* MetricEvent 是 UI、BLE、存储、振动的唯一指标事实源。 */
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
    /* 普通往返动作依次经历主向、反向并回到主向起点或稳定基线。 */
    FITNESS_REP_MODE_TWO_PHASE = 0,
    /* 跳跃动作依次经历静止、起跳、腾空、落地、恢复并回到静止。 */
    FITNESS_REP_MODE_JUMP = 1
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

/* 振动原因决定波形，电机驱动层只消费时序参数。 */
typedef enum {
    /* 会话开始：2 次 20 ms。 */
    FITNESS_HAPTIC_REASON_START = 0,
    /* 每次有效重复：1 次 30 ms。 */
    FITNESS_HAPTIC_REASON_REPETITION = 1,
    /* walk/trot 每满 10 步：1 次 30 ms。 */
    FITNESS_HAPTIC_REASON_STEP_BATCH = 2,
    /* 暂停或结束：1 次 40 ms。 */
    FITNESS_HAPTIC_REASON_PAUSE_OR_END = 3,
    /* 达成目标：3 次 25 ms。 */
    FITNESS_HAPTIC_REASON_GOAL = 4,
    /* 低电量提醒：2 次 40 ms。 */
    FITNESS_HAPTIC_REASON_LOW_BATTERY = 5,
    /* 原因总数，仅用于参数校验。 */
    FITNESS_HAPTIC_REASON_COUNT = 6
} fitness_haptic_reason_t;

/* 单个振动请求，不包含 GPIO 细节或阻塞延时。 */
typedef struct {
    /* 单次通电时间，单位 ms。 */
    uint16_t on_ms;
    /* 相邻脉冲间关闭时间，单位 ms；单次脉冲时为 0。 */
    uint16_t off_ms;
    /* 脉冲重复次数，范围 1..3。 */
    uint8_t repeat_count;
    /* 业务原因，供日志和优先级策略使用。 */
    fitness_haptic_reason_t reason;
} fitness_haptic_request_t;

/* 固定容量 FIFO；单生产者/单消费者可由外部临界区保护。 */
typedef struct {
    /* 静态请求槽位，占用约 FITNESS_HAPTIC_QUEUE_CAPACITY*8 字节。 */
    fitness_haptic_request_t items[FITNESS_HAPTIC_QUEUE_CAPACITY];
    /* 下一次读取位置，范围 0..15。 */
    uint8_t head;
    /* 下一次写入位置，范围 0..15。 */
    uint8_t tail;
    /* 当前队列元素个数，范围 0..16。 */
    uint8_t count;
} fitness_haptic_queue_t;

/* 六轴样本及质量标记；axis 顺序和单位由 FITNESS_IMU_AXIS_COUNT 约束。 */
typedef struct {
    /* 采样点的单调毫秒时间。 */
    uint64_t monotonic_ms;
    /* [gx,gy,gz,ax,ay,az]；前三轴 deg/s，后三轴 g。 */
    float axis[FITNESS_IMU_AXIS_COUNT];
    /* fitness_quality_flag_t 按位组合。 */
    uint16_t quality_flags;
} fitness_imu_sample_t;

/* 振动污染保护状态；可在 25 Hz IMU 任务中静态分配。 */
typedef struct {
    /* true 表示已经保存一个可作为插值左端点的干净采样。 */
    bool has_previous_clean;
    /* true 表示连续污染超过 3 点，必须冻结计数器。 */
    bool hard_freeze;
    /* 最近一次干净采样，用作线性插值左端点。 */
    fitness_imu_sample_t previous_clean;
    /* 待插值污染点的时间戳；污染轴值不可信，因此不保存。 */
    uint64_t pending_timestamps[FITNESS_HAPTIC_MAX_INTERPOLATED_SAMPLES];
    /* 当前可插值污染点数量，范围 0..3。 */
    uint8_t pending_count;
    /* 当前污染保护区起始时刻；区间外的更早采样不能被误标为污染。 */
    uint64_t contaminated_from_ms;
    /* 当前污染保护区结束时刻，含电机通电和 80 ms 余振。 */
    uint64_t contaminated_until_ms;
} fitness_haptic_guard_t;

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
    bool *step_accepted,
    bool *haptic_due);

/* 清空固定容量振动 FIFO。 */
void fitness_haptic_queue_init(fitness_haptic_queue_t *queue);

/* 按业务原因生成固定波形并入队。 */
fitness_status_t fitness_haptic_enqueue_reason(
    fitness_haptic_queue_t *queue,
    fitness_haptic_reason_t reason);

/* 根据唯一 MetricEvent 决定是否入队：每次 REP、每 10 STEP，sit 不振动。 */
fitness_status_t fitness_haptic_enqueue_for_metric(
    fitness_haptic_queue_t *queue,
    const fitness_metric_event_t *event,
    bool *request_enqueued);

/* 从 FIFO 取出最早请求；空队列返回 false。 */
bool fitness_haptic_queue_pop(
    fitness_haptic_queue_t *queue,
    fitness_haptic_request_t *request);

/* 初始化振动污染保护状态。 */
void fitness_haptic_guard_init(fitness_haptic_guard_t *guard);

/* 登记一次电机通电脉冲，并把保护区延长至 pulse_end+80 ms。 */
fitness_status_t fitness_haptic_guard_mark_pulse(
    fitness_haptic_guard_t *guard,
    uint64_t pulse_start_ms,
    uint32_t pulse_on_ms);

/* 查询某个采样时刻是否处于振动污染保护区。 */
bool fitness_haptic_guard_is_contaminated(
    const fitness_haptic_guard_t *guard,
    uint64_t sample_ms);

/*
 * 输入一个六轴采样，并输出 0..4 个可供算法消费的采样。
 * input_sample 的 axis 形状为 [6]，顺序 gx、gy、gz、ax、ay、az；前三轴单位 deg/s，后三轴单位 g。
 * output_samples 必须非空，形状为 [FITNESS_HAPTIC_MAX_OUTPUT_SAMPLES] 个 fitness_imu_sample_t；
 * 每个元素继承相同六轴顺序与单位，缓冲区生命周期和所有权均由调用者管理，仅在本次调用写入。
 * counter_frozen 表示污染过长，计数器在本次调用后仍应保持冻结。
 */
fitness_status_t fitness_haptic_guard_push_sample(
    /* 非空保护状态；函数会更新污染区和待插值样本。 */
    fitness_haptic_guard_t *guard,
    /* 非空只读当前 25 Hz 六轴点，生命周期覆盖本次同步调用。 */
    const fitness_imu_sample_t *input_sample,
    /* 非空输出数组，形状 [4]，每项六轴单位依次为 deg/s 与 g。 */
    fitness_imu_sample_t output_samples[FITNESS_HAPTIC_MAX_OUTPUT_SAMPLES],
    /* 非空输出；写入本次有效输出元素数，范围 0～4。 */
    size_t *output_count,
    /* 非空输出；污染连续过长时写 true，要求计数器保持冻结。 */
    bool *counter_frozen);

#ifdef __cplusplus
/* 结束 C ABI 声明区。 */
}
#endif

#endif /* FITNESS_CORE_H */
