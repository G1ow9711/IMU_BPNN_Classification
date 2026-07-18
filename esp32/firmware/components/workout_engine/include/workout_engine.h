#ifndef IMU_HANDHELD_WORKOUT_ENGINE_H
#define IMU_HANDHELD_WORKOUT_ENGINE_H

/*
 * 设备训练引擎：把双 M0 融合 logits、25 Hz 六轴点、计数器、热量和振动串成单一事实链。
 *
 * v1 产品合同：用户点击开始后进入准备页，连续三个高置信因果累计结果锁定一个计数动作；
 * 运行阶段继续累计最终双 M0 证据，但不重新初始化计数状态。累计分类与锁定动作不一致时，
 * 原始点仍推进时间和热量，但冻结相位与次数；切换动作必须停止并开始新会话。
 */

/* 引入次数、步数、热量、MetricEvent 和振动 FIFO。 */
#include "fitness_core.h"
/* 引入普通往返、跳跃五阶段和 walk/trot 步峰检测器。 */
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
/* 准备阶段同一高置信动作需连续出现三个推理窗口才锁定。 */
#define WORKOUT_ACTION_LOCK_WINDOWS (3U)
/* 锁定最低 softmax 置信度为 55%，Q16 近似取 36044/65535。 */
#define WORKOUT_ACTION_LOCK_CONFIDENCE_Q15 (36044U)
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
    /* 下游 fitness_core、motion_phase 或振动队列返回错误。 */
    WORKOUT_STATUS_ERR_DOMAIN = -4
} workout_status_t;

/* 保存轻量只读快照；可直接映射到 UI 和 BLE LiveState。 */
typedef struct {
    /* 保存当前训练状态。 */
    workout_state_t state;
    /* 保存会话持久化序号。 */
    uint32_t session_seq;
    /* 保存锁定动作 0..10；准备阶段为 255。 */
    uint8_t action_id;
    /* 保存当前动作段因果累计分类 0..10；尚无有效窗口时为 255。 */
    uint8_t inferred_action_id;
    /* true 表示当前累计分类与锁定计数动作一致；准备阶段固定为 false。 */
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
    /* 保存 11 类准备阶段累计 logits。 */
    float bout_logit_sum[WORKOUT_CLASS_COUNT];
    /* 保存准备阶段已累计窗口数。 */
    uint32_t bout_window_count;
    /* 保存动作段当前因果累计分类 0..10；没有有效窗口时为 255。 */
    uint8_t inferred_action;
    /* true 表示 inferred_action 与 selected_action 一致，允许推进相位和次数。 */
    bool classification_consistent;
    /* 保存当前连续候选动作 0..10；255 表示没有候选。 */
    uint8_t candidate_action;
    /* 保存候选连续高置信窗口数。 */
    uint8_t candidate_windows;
    /* 保存最终锁定动作 0..10；255 表示尚未锁定。 */
    uint8_t selected_action;
    /* 保存最近 softmax 最优类别置信度，范围 0..65535。 */
    uint16_t confidence_q15;
    /* 保存最近质量位。 */
    uint16_t quality_flags;
    /* 保存单动作次数/步数/热量领域会话。 */
    fitness_session_t fitness_session;
    /* 保存 8 类重复动作状态；非重复动作不读取。 */
    fitness_rep_counter_t rep_counter;
    /* 保存 walk/trot 步峰去重器；其它动作不读取。 */
    fitness_step_counter_t step_counter;
    /* 保存锁定动作的原始点相位/步峰检测器。 */
    motion_phase_detector_t phase_detector;
    /* 保存所有业务振动请求；马达任务异步消费。 */
    fitness_haptic_queue_t haptic_queue;
} workout_engine_t;

/* 初始化为空闲状态和空振动队列。 */
void workout_engine_init(workout_engine_t *engine);
/* 从 Idle/Summary 开始准备；weight_g 合理范围统一为 0 或 30～250 kg。 */
workout_status_t workout_engine_start(
    workout_engine_t *engine,
    uint32_t session_seq,
    uint32_t weight_g,
    uint64_t now_ms);
/*
 * 只清空双 M0 动作段分类证据；保留 selected_action、fitness_session、已计次数、热量和振动队列。
 * 暂停、IMU 连续性重置、设备重连或用户明确开始新动作段时调用；空指针安全无操作。
 */
void workout_engine_reset_bout_evidence(workout_engine_t *engine);
/*
 * 加入一次双 M0 融合 logits；PREPARING 和 RUNNING 都使用同一动作段因果累计器。
 * logits 必须非空，形状固定为 [11]，按 FITNESS_ACTION_* 顺序保存无量纲融合分数；
 * 数组生命周期只需覆盖本次同步调用，函数按值累计且不会保存其地址；RUNNING 不切换
 * selected_action，只更新 inferred_action、classification_consistent 和累计置信度。
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
 * emitted=true 时 event 是 UI、BLE、存储和振动的唯一新指标事实。
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
/* 从引擎振动 FIFO 取出最早请求；空队列返回 false。 */
bool workout_engine_pop_haptic(
    workout_engine_t *engine,
    fitness_haptic_request_t *request);

#ifdef __cplusplus
/* 结束 C ABI 声明区。 */
}
#endif

#endif /* IMU_HANDHELD_WORKOUT_ENGINE_H */
