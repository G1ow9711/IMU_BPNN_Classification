#ifndef IMU_HANDHELD_WORKOUT_ENGINE_H
#define IMU_HANDHELD_WORKOUT_ENGINE_H

/*
 * 设备训练引擎：把双 M0 融合 logits、25 Hz 六轴点、计数器和热量串成单一事实链。
 *
 * 产品合同：用户点击开始后立即采集和缓存 25 Hz 六轴点，独立窗口类别连续两窗且概率过门后锁定；
 * 最迟第四窗按累计平均类别结束准备。锁定后 selected_action 固定本轮计数器和界面动作；后续
 * inferred_action 只进入诊断日志，不再切断计数。计数许可由训练端同源的 25 点运动/静止门和
 * 数据质量共同控制；休息只清除未完成半周期，恢复活动后继续同一动作。锁定时按原时间顺序
 * 补算准备期完整 160 点，使点击开始后已经完成的动作仍能进入权威累计。
 */

/* 引入次数、步数、热量和 MetricEvent。 */
#include "fitness_core.h"
/* 引入自适应完整往返、开合跳峰谷和 walk/trot 施密特步峰检测器。 */
#include "motion_phase.h"

/* 引入 bool 表达事件、锁定和运行状态。 */
#include <stdbool.h>
/* 引入定长整数，保证会话、时间和 Q15 字段宽度稳定。 */
#include <stdint.h>

#ifdef __cplusplus
/* C++ 固件按 C ABI 调用。 */
extern "C" {
#endif

/* 双 M0 固定输出 11 类，与 FITNESS_ACTION_COUNT 一致。 */
#define WORKOUT_CLASS_COUNT (11U)
/* 独立窗口最优类别必须连续保持两个重叠窗口，抑制动作起步的单窗瞬态误判。 */
#define WORKOUT_ACTION_LOCK_WINDOWS (2U)
/* 准备态最多累计四个窗口；25 Hz、步长 12 点时首窗后最多增加 1.44 秒。 */
#define WORKOUT_ACTION_MAX_PREPARE_WINDOWS (4U)
/*
 * 最坏准备跨度包含一次 62 点窗口重建，再接三次 12 点重叠步进：2*62+3*12=160 点。
 * 25 Hz 下对应 6.40 秒；全部保留才能在锁类后补算点击开始以来的完整动作。
 */
#define WORKOUT_PRELOCK_SAMPLE_CAPACITY (160U)
/* 160 点按 13 点单轴不应期最多形成 12 条权威事件；静态 FIFO 保留每条原始时刻。 */
#define WORKOUT_REPLAY_EVENT_QUEUE_CAPACITY (12U)
/* 活动门使用训练端固定 1 秒因果窗；25 Hz 下恰为 25 个六轴点。 */
#define WORKOUT_ACTIVITY_WINDOW_SAMPLES (25U)
/* 逐点活动分数过门至少 5/25，即训练端 MOTION_TRIGGER_RATIO=20%。 */
#define WORKOUT_ACTIVITY_MIN_ACTIVE_POINTS (5U)
/* 独立窗口正常确认的最低 softmax 概率为 50%，Q15 取 32768/65535。 */
#define WORKOUT_ACTION_LOCK_CONFIDENCE_Q15 (32768U)
/* 未锁定动作使用 255，不能送入 fitness_action_t。 */
#define WORKOUT_ACTION_UNKNOWN (255U)

/* 描述产品训练状态；UI 和 BLE 映射到同名设备状态。 */
typedef enum {
    /* 没有会话，可接受开始命令。 */
    WORKOUT_STATE_IDLE = 0,
    /* 正在累计模型证据并自动锁定用户动作。 */
    WORKOUT_STATE_PREPARING = 1,
    /* 已锁定动作，正在累计指标。 */
    WORKOUT_STATE_RUNNING = 2,
    /* 会话保留但不接受采样和热量积分。 */
    WORKOUT_STATE_PAUSED = 3,
    /* 会话已停止，最终指标可保存或显示。 */
    WORKOUT_STATE_SUMMARY = 4
} workout_state_t;

/* 统一返回码；正值表示安全忽略，负值表示调用错误。 */
typedef enum {
    /* 调用成功，状态或输出有效。 */
    WORKOUT_STATUS_OK = 0,
    /* 当前点被有意忽略，例如 PREPARING 期间尚未锁定。 */
    WORKOUT_STATUS_IGNORED = 1,
    /* 空指针、非有限 logits、非法动作或非法用户资料。 */
    WORKOUT_STATUS_ERR_ARGUMENT = -1,
    /* 当前状态不允许该操作。 */
    WORKOUT_STATUS_ERR_STATE = -2,
    /* 单调时间倒退或间断导致本次输入不可用。 */
    WORKOUT_STATUS_ERR_TIME = -3,
    /* 下游 fitness_core 或 motion_phase 返回错误。 */
    WORKOUT_STATUS_ERR_DOMAIN = -4
} workout_status_t;

/* 保存轻量只读快照；可直接映射到 UI 和 BLE LiveState。 */
typedef struct {
    /* 保存当前训练状态。 */
    workout_state_t state;
    /* 保存会话持久化序号。 */
    uint32_t session_seq;
    /* 保存本轮主动作 0..10；只决定计数器与会话摘要，准备阶段为 255。 */
    uint8_t action_id;
    /* 保存最近推理窗口实时分类 0..10；尚无有效窗口时为 255。 */
    uint8_t inferred_action_id;
    /* true 表示训练同源活动门允许计数；与后续实时分类类别无关。 */
    bool classification_consistent;
    /* 保存次数、步数或完整秒；单位由 metric_kind 决定。 */
    uint64_t metric_value;
    /* 保存 FITNESS_METRIC_*；准备阶段固定为 REPETITION 占位但 action_id=255。 */
    fitness_metric_kind_t metric_kind;
    /* 保存当前模型置信度，0..65535 对应 0..1。 */
    uint16_t confidence_q15;
    /* 保存数据质量位，来源于最近推理/采样。 */
    uint16_t quality_flags;
    /* 保存毛热量，单位 microkcal。 */
    uint64_t gross_microkcal;
    /* 保存活动热量，单位 microkcal。 */
    uint64_t active_microkcal;
} workout_snapshot_t;

/* 保存一个尚未锁类的 25 Hz 计数输入；锁定后按原时间顺序补算。 */
typedef struct {
    /* 保存单调毫秒与 gx、gy、gz、ax、ay、az 六轴物理量。 */
    motion_phase_sample_t sample;
    /* true 表示该点没有间断、队列溢出或旧执行器污染，可推进相位。 */
    bool count_input_valid;
    /* 保存该点质量位，补算产生的 MetricEvent 必须沿用真实来源。 */
    uint16_t quality_flags;
} workout_prelock_sample_t;

/* 保存完整静态训练状态；无运行期堆分配。 */
typedef struct {
    /* 保存当前产品训练状态。 */
    workout_state_t state;
    /* 保存待启动或已启动会话序号。 */
    uint32_t session_seq;
    /* 保存用户体重，单位 g；0 表示未知且热量保持零。 */
    uint32_t weight_g;
    /* 保存开始准备的单调毫秒。 */
    uint64_t started_ms;
    /* 保存准备期完整 160 个 25 Hz 点；覆盖一次重建后的第四窗，固定占用且不使用堆。 */
    workout_prelock_sample_t prelock_samples[WORKOUT_PRELOCK_SAMPLE_CAPACITY];
    /* 指向下一个准备期写入槽，范围 0..159。 */
    uint8_t prelock_write_index;
    /* 保存当前有效准备期点数，范围 0..160。 */
    uint8_t prelock_sample_count;
    /* 保存 11 类准备阶段累计 logits；有界锁类后 RUNNING 只保留单窗诊断，不再参与切类。 */
    float bout_logit_sum[WORKOUT_CLASS_COUNT];
    /* 保存准备阶段已累计窗口数。 */
    uint32_t bout_window_count;
    /* 保存动作段当前因果累计分类 0..10；没有有效窗口时为 255。 */
    uint8_t inferred_action;
    /* true 表示 25 点训练同源活动门允许推进相位和次数；字段名为兼容既有协议保留。 */
    bool classification_consistent;
    /* 保存当前累计候选动作 0..10；255 表示尚无有限证据。 */
    uint8_t candidate_action;
    /* 保存累计最优类连续保持的窗口数；达到 2 且概率过门时锁定。 */
    uint8_t candidate_windows;
    /* 保存最终锁定动作 0..10；255 表示尚未锁定。 */
    uint8_t selected_action;
    /* 保存最近 softmax 最优类别置信度，范围 0..65535。 */
    uint16_t confidence_q15;
    /* 保存最近质量位。 */
    uint16_t quality_flags;
    /* 保存最近 25 点加速度模长，单位 g；数组固定占用 100 字节。 */
    float activity_acceleration_magnitude_g[WORKOUT_ACTIVITY_WINDOW_SAMPLES];
    /* 保存最近 25 点角速度模长，单位 deg/s；数组固定占用 100 字节。 */
    float activity_gyro_magnitude_dps[WORKOUT_ACTIVITY_WINDOW_SAMPLES];
    /* 保存每点活动分数是否超过训练阈值，元素为 0/1，固定占用 25 字节。 */
    uint8_t activity_point_active[WORKOUT_ACTIVITY_WINDOW_SAMPLES];
    /* 指向下一次覆盖的活动窗槽位，范围 0..24。 */
    uint8_t activity_write_index;
    /* 保存活动窗已有有效点数，范围 0..25。 */
    uint8_t activity_sample_count;
    /* 保存当前窗逐点活动标志之和，范围 0..25，避免每点重复统计布尔数组。 */
    uint8_t activity_active_point_count;
    /* 保存前一干净点 ax、ay、az，单位 g，用于计算当前 |Δa|。 */
    float activity_previous_acceleration_g[3];
    /* true 表示前一干净加速度可用于差分；时间线边界后清零。 */
    bool activity_previous_acceleration_valid;
    /* true 表示本轮活动窗已经确认过真实运动；确认前的准备上下文不得提前冻结首个周期。 */
    bool activity_gate_has_seen_motion;
    /* 保存单动作次数/步数/热量领域会话。 */
    fitness_session_t fitness_session;
    /* 保存 8 类重复动作状态；非重复动作不读取。 */
    fitness_rep_counter_t rep_counter;
    /* 保存 walk/trot 步峰去重器；其它动作不读取。 */
    fitness_step_counter_t step_counter;
    /* 保存锁定动作的原始点相位/步峰检测器。 */
    motion_phase_detector_t phase_detector;
    /* 保存锁类补算产生的每条 MetricEvent；数组按 event_seq 递增，无运行期堆分配。 */
    fitness_metric_event_t replay_metric_events[WORKOUT_REPLAY_EVENT_QUEUE_CAPACITY];
    /* 指向最早未交付补算事件槽，范围 0..11。 */
    uint8_t replay_metric_event_head;
    /* 保存当前补算事件数量，范围 0..12。 */
    uint8_t replay_metric_event_count;
} workout_engine_t;

/* 初始化为空闲状态。 */
void workout_engine_init(workout_engine_t *engine);
/* 从 Idle/Summary 开始准备；weight_g 合理范围统一为 0 或 30～250 kg。 */
workout_status_t workout_engine_start(
    workout_engine_t *engine,
    uint32_t session_seq,
    uint32_t weight_g,
    uint64_t now_ms);
/*
 * 标记一次 IMU 连续性边界；PREPARING 保留已闭合动作点和干净分类候选，边界质量点在重放时
 * 只清除未完成半周期。RUNNING 保留 selected_action、次数、热量和当前活动门，只清除跨缺口
 * 半周期并重建活动统计；恢复不依赖下一次 2.48 秒模型窗口。空指针安全无操作。
 */
void workout_engine_reset_bout_evidence(workout_engine_t *engine);
/*
 * 加入一次双 M0 融合 logits；PREPARING 使用有界因果累计确认，RUNNING 只更新实时诊断类别。
 * logits 必须非空，形状固定为 [11]，按 FITNESS_ACTION_* 顺序保存无量纲融合分数；
 * 数组生命周期只需覆盖本次同步调用，函数不会保存其地址；RUNNING 不允许切换 selected_action，
 * PREPARING 每窗公开累计候选；连续两窗且概率至少 50% 时锁定，最迟第四窗按累计 argmax 锁定。
 * RUNNING 不切换 selected_action，也不使用后续 Top-1 开关计数；运动/静止门在逐样本入口按
 * 训练冻结公式控制相位，避免模型窗抖动造成数秒漏计。
 * action_locked 仅在本次由未锁定变为锁定时为 true。
 */
workout_status_t workout_engine_push_inference(
    /* 非空引擎状态；函数可能更新准备累计、锁定动作和置信度。 */
    workout_engine_t *engine,
    /* 非空只读数组，形状 [11]，元素为无量纲 logits，生命周期覆盖本次调用。 */
    const float logits[WORKOUT_CLASS_COUNT],
    /* 当前单调时间，单位毫秒；不得早于先前输入。 */
    uint64_t now_ms,
    /* 当前窗口数据质量位图；位定义来自 fitness_core/IMU pipeline。 */
    uint16_t quality_flags,
    /* 非空输出；仅本次从 PREPARING 锁定动作时写 true。 */
    bool *action_locked);
/*
 * 输入一个 25 Hz 六轴点；count_input_valid=false 时仍累计热量，但不推进相位/步峰。
 * emitted=true 时 event 是 UI、BLE 和存储共享的唯一新指标事实。
 */
workout_status_t workout_engine_push_sample(
    workout_engine_t *engine,
    const motion_phase_sample_t *sample,
    bool count_input_valid,
    uint16_t quality_flags,
    fitness_metric_event_t *event,
    bool *emitted);
/* 暂停运行会话并重置不完整动作周期；暂停区间不累计热量。 */
workout_status_t workout_engine_pause(workout_engine_t *engine, uint64_t now_ms);
/* 恢复暂停会话并把热量积分基准移动到 now_ms。 */
workout_status_t workout_engine_resume(workout_engine_t *engine, uint64_t now_ms);
/* 停止准备/运行/暂停会话，生成 Summary 状态并清除不完整周期。 */
workout_status_t workout_engine_stop(workout_engine_t *engine, uint64_t now_ms);
/* 清除 Summary 并返回 Idle；不修改外部已保存摘要。 */
void workout_engine_return_idle(workout_engine_t *engine);
/* 按值生成当前快照；空指针返回参数错误。 */
workout_status_t workout_engine_snapshot(
    const workout_engine_t *engine,
    workout_snapshot_t *snapshot);
/* 从补算事件 FIFO 取最早 MetricEvent；只用于锁类调用后的协调器 BLE/摘要扇出。 */
bool workout_engine_pop_replay_metric_event(
    workout_engine_t *engine,
    fitness_metric_event_t *event);

#ifdef __cplusplus
/* 结束 C ABI 声明区。 */
}
#endif

#endif /* IMU_HANDHELD_WORKOUT_ENGINE_H */
