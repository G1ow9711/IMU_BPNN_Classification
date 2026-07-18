/* 引入训练引擎公共合同。 */
#include "workout_engine.h"
/* 引入最终模型自动生成的动作段因果累计唯一实现。 */
#include "esp32_bp_features.h"

/* 引入 expf、isfinite，计算数值稳定 softmax 并拒绝异常 logits。 */
#include <math.h>
/* 引入 memset，确定性清空会话和固定数组。 */
#include <string.h>

/* 编译期保证领域引擎和双 M0 头使用完全相同的 11 类输出宽度。 */
_Static_assert(WORKOUT_CLASS_COUNT == CLASS_NUM, "workout/model class count mismatch");

/* 判断动作是否属于 8 个完整周期计数动作。 */
static bool workout_action_is_repetition(const fitness_action_t action)
{
    /* sit、walk、trot 之外的 8 类使用重复计数器。 */
    return (action != FITNESS_ACTION_SIT) &&
           (action != FITNESS_ACTION_WALK) &&
           (action != FITNESS_ACTION_TROT) &&
           (action >= FITNESS_ACTION_GOOD_MORNING) &&
           (action < FITNESS_ACTION_COUNT);
}

/* 判断动作是否按步计数。 */
static bool workout_action_is_step(const fitness_action_t action)
{
    /* 只有 walk 和 trot 使用步峰。 */
    return (action == FITNESS_ACTION_WALK) || (action == FITNESS_ACTION_TROT);
}

/* 返回当前动作主指标类型。 */
static fitness_metric_kind_t workout_metric_kind(const fitness_action_t action)
{
    /* 静坐使用毫秒时长事件。 */
    if (action == FITNESS_ACTION_SIT) {
        /* 返回持续时间。 */
        return FITNESS_METRIC_DURATION_MS;
    }
    /* 行走和小跑使用步数。 */
    if (workout_action_is_step(action)) {
        /* 返回步指标。 */
        return FITNESS_METRIC_STEP;
    }
    /* 其余动作使用重复次数。 */
    return FITNESS_METRIC_REPETITION;
}

/* 把 uint64 指标饱和到 BLE/UI 常用 uint32 范围。 */
static uint32_t workout_saturate_u64_to_u32(const uint64_t value)
{
    /* 超过上限时固定为 UINT32_MAX，避免截断回绕。 */
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

/* 把模型 0..65535 置信度转换为 fitness_core 0..32767 稳定度。 */
static uint16_t workout_confidence_to_stability(const uint16_t confidence_q15)
{
    /* 右移一位把满量程 65535 精确映射到 32767。 */
    return (uint16_t)(confidence_q15 >> 1U);
}

/* 根据动作段累计器已输出的平均 logits 计算最优类 softmax Q15 置信度。 */
static workout_status_t workout_confidence_from_averaged_logits(
    const float averaged_logits[WORKOUT_CLASS_COUNT],
    const uint8_t best_class,
    uint16_t *confidence_q15)
{
    /* 输入、输出和累计器返回的类别索引必须有效。 */
    if ((averaged_logits == NULL) || (confidence_q15 == NULL) ||
        (best_class >= WORKOUT_CLASS_COUNT)) {
        /* 无法形成概率。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* 读取累计器已按 NumPy 平局规则选择的最佳平均 logit。 */
    const float best_average = averaged_logits[best_class];
    /* 最佳类必须有限。 */
    if (!isfinite(best_average)) {
        /* 拒绝溢出或异常累计。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* 用减最大值 softmax 计算分母，避免 expf 溢出。 */
    float denominator = 0.0F;
    /* 遍历全部类别累加 exp(z-max)。 */
    for (uint8_t class_index = 0U; class_index < WORKOUT_CLASS_COUNT; ++class_index) {
        /* 读取当前类别无量纲平均 logit。 */
        const float average = averaged_logits[class_index];
        /* 任一非有限值使本次置信度无效。 */
        if (!isfinite(average)) {
            /* 返回参数错误，调用方不会提交本次局部累计状态。 */
            return WORKOUT_STATUS_ERR_ARGUMENT;
        }
        /* 平移后的指数范围不大于 1。 */
        denominator += expf(average - best_average);
    }
    /* 分母必须有限且至少包含最佳类的 1。 */
    if (!isfinite(denominator) || (denominator < 1.0F)) {
        /* 返回异常。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* 最佳类减自身最大值后的指数为 1，因此概率为 1/分母。 */
    const float probability = 1.0F / denominator;
    /* 四舍五入到 0..65535，使用 float 已足够 Q15 精度。 */
    float scaled = (probability * 65535.0F) + 0.5F;
    /* 下界保护，理论上概率非负。 */
    if (scaled < 0.0F) {
        /* 固定为零。 */
        scaled = 0.0F;
    }
    /* 上界保护浮点舍入。 */
    if (scaled > 65535.0F) {
        /* 固定为满量程。 */
        scaled = 65535.0F;
    }
    /* 输出 Q15 置信度。 */
    *confidence_q15 = (uint16_t)scaled;
    /* 计算成功。 */
    return WORKOUT_STATUS_OK;
}

/*
 * 使用自动生成 BpBoutAccumulator 更新引擎的公开静态存储。
 * 局部状态先完整计算并检查，再复制回引擎；NaN、Inf 或浮点溢出不会污染长期会话。
 */
static int workout_update_bout_accumulator(
    workout_engine_t *engine,
    const float logits[WORKOUT_CLASS_COUNT],
    float averaged_logits[WORKOUT_CLASS_COUNT])
{
    /* 创建与生成头完全同布局语义的局部累计器，避免公共头暴露整份特征实现。 */
    BpBoutAccumulator accumulator;
    /* 复制 11 类历史和，单位为无量纲融合 logit。 */
    (void)memcpy(
        accumulator.running_sum,
        engine->bout_logit_sum,
        sizeof(accumulator.running_sum));
    /* 复制当前动作段窗口数。 */
    accumulator.count = engine->bout_window_count;
    /* 调用最终 Python/C 已验证的唯一因果累计实现。 */
    const int best_class = bp_bout_accumulator_update(
        &accumulator,
        logits,
        averaged_logits);
    /* 负返回值表示空指针或非有限输入，原引擎保持不变。 */
    if (best_class < 0) {
        /* 返回失败。 */
        return -1;
    }
    /* 检查累计加法没有把有限输入放大为无穷值。 */
    for (uint8_t class_index = 0U; class_index < WORKOUT_CLASS_COUNT; ++class_index) {
        /* 任一平均值非有限时拒绝整个局部状态。 */
        if (!isfinite(averaged_logits[class_index])) {
            /* 返回失败且不复制局部累计器。 */
            return -1;
        }
    }
    /* 全部检查通过后原子式复制 11 类累计和。 */
    (void)memcpy(
        engine->bout_logit_sum,
        accumulator.running_sum,
        sizeof(engine->bout_logit_sum));
    /* 提交更新后的动作段窗口数。 */
    engine->bout_window_count = accumulator.count;
    /* 返回当前累计平均 argmax 类别 0..10。 */
    return best_class;
}

/* 清空准备阶段证据，不影响产品状态字段。 */
static void workout_reset_bout(workout_engine_t *engine)
{
    /* 创建局部生成累计器，确保重置公式与最终模型头保持同源。 */
    BpBoutAccumulator accumulator;
    /* 调用生成头清零 11 类累计和和窗口数。 */
    bp_bout_accumulator_reset(&accumulator);
    /* 把确定性零累计和复制到引擎静态存储。 */
    (void)memcpy(
        engine->bout_logit_sum,
        accumulator.running_sum,
        sizeof(engine->bout_logit_sum));
    /* 复制零窗口数。 */
    engine->bout_window_count = accumulator.count;
    /* 尚无窗口时当前累计分类无效。 */
    engine->inferred_action = WORKOUT_ACTION_UNKNOWN;
    /* 新证据重新确认锁定动作前冻结相位与次数。 */
    engine->classification_consistent = false;
    /* 清空候选动作。 */
    engine->candidate_action = WORKOUT_ACTION_UNKNOWN;
    /* 清空候选连续数。 */
    engine->candidate_windows = 0U;
    /* 清空当前置信度。 */
    engine->confidence_q15 = 0U;
}

/* 锁定准备阶段动作并初始化唯一领域会话。 */
static workout_status_t workout_lock_action(
    workout_engine_t *engine,
    const fitness_action_t action,
    const uint64_t now_ms)
{
    /* 先初始化领域会话，失败时不进入 RUNNING。 */
    if (fitness_session_start(
            &engine->fitness_session,
            engine->session_seq,
            action,
            engine->weight_g,
            now_ms) != FITNESS_STATUS_OK) {
        /* 返回下游错误。 */
        return WORKOUT_STATUS_ERR_DOMAIN;
    }
    /* 初始化该动作的原始点相位或步峰检测器。 */
    if (motion_phase_init(&engine->phase_detector, action) != MOTION_PHASE_OK) {
        /* 终止已建会话，避免半初始化。 */
        (void)fitness_session_stop(&engine->fitness_session);
        /* 返回下游错误。 */
        return WORKOUT_STATUS_ERR_DOMAIN;
    }
    /* 8 个周期动作初始化重复状态机。 */
    if (workout_action_is_repetition(action)) {
        /* 初始化失败时回滚。 */
        if (fitness_rep_counter_init(&engine->rep_counter, action) != FITNESS_STATUS_OK) {
            /* 停止领域会话。 */
            (void)fitness_session_stop(&engine->fitness_session);
            /* 返回下游错误。 */
            return WORKOUT_STATUS_ERR_DOMAIN;
        }
    }
    /* walk/trot 初始化步峰生理去重器。 */
    if (workout_action_is_step(action)) {
        /* 初始化失败时回滚。 */
        if (fitness_step_counter_init(&engine->step_counter, action) != FITNESS_STATUS_OK) {
            /* 停止领域会话。 */
            (void)fitness_session_stop(&engine->fitness_session);
            /* 返回下游错误。 */
            return WORKOUT_STATUS_ERR_DOMAIN;
        }
    }
    /* 保存锁定动作。 */
    engine->selected_action = (uint8_t)action;
    /* 锁定来源就是当前累计分类，因此初始计数分类一致。 */
    engine->classification_consistent =
        engine->inferred_action == engine->selected_action;
    /* 切换到运行状态。 */
    engine->state = WORKOUT_STATE_RUNNING;
    /* 入队开始双脉冲；队列满不会撤销已锁定会话。 */
    (void)fitness_haptic_enqueue_reason(
        &engine->haptic_queue,
        FITNESS_HAPTIC_REASON_START);
    /* 锁定成功。 */
    return WORKOUT_STATUS_OK;
}

void workout_engine_init(workout_engine_t *engine)
{
    /* 空指针安全无操作。 */
    if (engine == NULL) {
        /* 没有状态可初始化。 */
        return;
    }
    /* 清空全部会话、证据和下游状态。 */
    (void)memset(engine, 0, sizeof(*engine));
    /* 初始状态为空闲。 */
    engine->state = WORKOUT_STATE_IDLE;
    /* 初始候选未知。 */
    engine->candidate_action = WORKOUT_ACTION_UNKNOWN;
    /* 初始没有动作段累计分类。 */
    engine->inferred_action = WORKOUT_ACTION_UNKNOWN;
    /* 初始锁定动作未知。 */
    engine->selected_action = WORKOUT_ACTION_UNKNOWN;
    /* 未锁定时不得推进相位和次数。 */
    engine->classification_consistent = false;
    /* 显式初始化振动 FIFO。 */
    fitness_haptic_queue_init(&engine->haptic_queue);
}

workout_status_t workout_engine_start(
    workout_engine_t *engine,
    const uint32_t session_seq,
    const uint32_t weight_g,
    const uint64_t now_ms)
{
    /* 引擎必须存在，会话序号不能为零；体重允许内部未知或 30～250 kg。 */
    if ((engine == NULL) || (session_seq == 0U) ||
        ((weight_g != 0U) && ((weight_g < 30000U) || (weight_g > 250000U)))) {
        /* 参数不满足产品合同。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* 只允许从 Idle 或 Summary 开始新会话。 */
    if ((engine->state != WORKOUT_STATE_IDLE) &&
        (engine->state != WORKOUT_STATE_SUMMARY)) {
        /* 已有准备/运行/暂停会话不能覆盖。 */
        return WORKOUT_STATUS_ERR_STATE;
    }
    /* 清空旧对象并恢复空闲默认值。 */
    workout_engine_init(engine);
    /* 显式重置准备证据，保持该 helper 与未来“重新识别”路径共用。 */
    workout_reset_bout(engine);
    /* 保存新会话序号。 */
    engine->session_seq = session_seq;
    /* 保存用户体重，单位 g。 */
    engine->weight_g = weight_g;
    /* 保存准备起点。 */
    engine->started_ms = now_ms;
    /* 切换到准备状态。 */
    engine->state = WORKOUT_STATE_PREPARING;
    /* 返回成功。 */
    return WORKOUT_STATUS_OK;
}

void workout_engine_reset_bout_evidence(workout_engine_t *engine)
{
    /* 空指针没有可重置状态，安全返回。 */
    if (engine == NULL) {
        /* 不访问空对象。 */
        return;
    }
    /* 只清分类累计、候选和置信度；锁定动作、次数、热量及振动队列保持原值。 */
    workout_reset_bout(engine);
}

workout_status_t workout_engine_push_inference(
    workout_engine_t *engine,
    const float logits[WORKOUT_CLASS_COUNT],
    const uint64_t now_ms,
    const uint16_t quality_flags,
    bool *action_locked)
{
    /* 必填对象和输出必须有效。 */
    if ((engine == NULL) || (logits == NULL) || (action_locked == NULL)) {
        /* 返回参数错误。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* 默认本次没有新锁定。 */
    *action_locked = false;
    /* 只有准备或运行状态接受推理。 */
    if ((engine->state != WORKOUT_STATE_PREPARING) &&
        (engine->state != WORKOUT_STATE_RUNNING)) {
        /* 暂停/空闲/总结状态不应积累模型证据。 */
        return WORKOUT_STATUS_ERR_STATE;
    }
    /* 推理时间不得早于会话开始。 */
    if (now_ms < engine->started_ms) {
        /* 拒绝倒退时间。 */
        return WORKOUT_STATUS_ERR_TIME;
    }
    /* 保存最新质量位供 UI/BLE 快照。 */
    engine->quality_flags = quality_flags;

    /* 保存当前窗口加入后的 11 类动作段平均 logits。 */
    float averaged_logits[WORKOUT_CLASS_COUNT];
    /* PREPARING 与 RUNNING 共用最终生成的因果累计器。 */
    const int inferred_class = workout_update_bout_accumulator(
        engine,
        logits,
        averaged_logits);
    /* 非有限输入或累计溢出时保持旧引擎状态并拒绝本帧。 */
    if (inferred_class < 0) {
        /* 返回参数错误。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* 把生成累计器返回的全局类别索引保存为当前产品分类。 */
    const uint8_t best_class = (uint8_t)inferred_class;
    /* 根据同一平均 logits 计算 UI/BLE 使用的 softmax Q15 置信度。 */
    const workout_status_t confidence_status = workout_confidence_from_averaged_logits(
        averaged_logits,
        best_class,
        &engine->confidence_q15);
    /* 计算异常直接返回。 */
    if (confidence_status != WORKOUT_STATUS_OK) {
        /* 传播错误。 */
        return confidence_status;
    }
    /* 保存动作段从开始到当前的严格因果累计类别。 */
    engine->inferred_action = best_class;
    /* 已锁定会话只比较类别一致性，绝不重建计数动作和领域状态。 */
    if (engine->state == WORKOUT_STATE_RUNNING) {
        /* 同类允许相位/次数继续；不同类冻结计数直到累计结果恢复或用户结束会话。 */
        engine->classification_consistent =
            best_class == engine->selected_action;
        /* 运行推理只更新累计分类、置信度和一致性。 */
        return WORKOUT_STATUS_OK;
    }
    /* 低于 55% 时清空连续候选，但保留历史平均供后续稳定。 */
    if (engine->confidence_q15 < WORKOUT_ACTION_LOCK_CONFIDENCE_Q15) {
        /* 清空候选动作。 */
        engine->candidate_action = WORKOUT_ACTION_UNKNOWN;
        /* 清空连续数。 */
        engine->candidate_windows = 0U;
        /* 当前只表示尚未锁定。 */
        return WORKOUT_STATUS_IGNORED;
    }
    /* 同一候选继续出现时增加连续数。 */
    if (engine->candidate_action == best_class) {
        /* 饱和到 255；锁定只需要 3。 */
        if (engine->candidate_windows < UINT8_MAX) {
            /* 增加一次连续确认。 */
            engine->candidate_windows += 1U;
        }
    } else {
        /* 新候选覆盖旧候选。 */
        engine->candidate_action = best_class;
        /* 当前窗口计为第一次。 */
        engine->candidate_windows = 1U;
    }
    /* 不满三个连续窗口时保持准备。 */
    if (engine->candidate_windows < WORKOUT_ACTION_LOCK_WINDOWS) {
        /* 尚未锁定。 */
        return WORKOUT_STATUS_IGNORED;
    }
    /* 三个高置信窗口后初始化单动作领域会话。 */
    const workout_status_t lock_status = workout_lock_action(
        engine,
        (fitness_action_t)best_class,
        now_ms);
    /* 锁定失败时返回下游错误。 */
    if (lock_status != WORKOUT_STATUS_OK) {
        /* 传播错误。 */
        return lock_status;
    }
    /* 标记本次发生锁定，UI 从 PREPARE 进入 RUNNING。 */
    *action_locked = true;
    /* 返回成功。 */
    return WORKOUT_STATUS_OK;
}

workout_status_t workout_engine_push_sample(
    workout_engine_t *engine,
    const motion_phase_sample_t *sample,
    const bool count_input_valid,
    const uint16_t quality_flags,
    fitness_metric_event_t *event,
    bool *emitted)
{
    /* 所有对象必须有效。 */
    if ((engine == NULL) || (sample == NULL) || (event == NULL) || (emitted == NULL)) {
        /* 返回参数错误。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* 默认没有新 MetricEvent。 */
    *emitted = false;
    /* 只有已锁定运行状态接受 25 Hz 点。 */
    if (engine->state != WORKOUT_STATE_RUNNING) {
        /* 准备、暂停和总结状态有意不累计。 */
        return WORKOUT_STATUS_IGNORED;
    }
    /* 保存最新质量位。 */
    engine->quality_flags = quality_flags;
    /* 把模型置信度压缩到 fitness_core 的 0..32767 稳定度。 */
    const uint16_t stability_q15 = workout_confidence_to_stability(
        engine->confidence_q15);
    /* 每个干净或污染点都推进会话时间和热量；sit 可能直接产生整秒事件。 */
    const fitness_status_t tick_status = fitness_session_tick(
        &engine->fitness_session,
        sample->monotonic_ms,
        stability_q15,
        quality_flags,
        event,
        emitted);
    /* 时间或会话错误必须上报。 */
    if (tick_status == FITNESS_STATUS_INVALID_TIME) {
        /* 返回时间错误。 */
        return WORKOUT_STATUS_ERR_TIME;
    }
    /* 其它领域错误统一映射。 */
    if (tick_status != FITNESS_STATUS_OK) {
        /* 返回领域错误。 */
        return WORKOUT_STATUS_ERR_DOMAIN;
    }
    /* sit 整秒事件已经产生，只需按指标规则确认不振动。 */
    if (*emitted) {
        /* sit 不会入队；函数仍执行统一规则。 */
        bool request_enqueued = false;
        /* 忽略队列满，权威 MetricEvent 不能回滚。 */
        (void)fitness_haptic_enqueue_for_metric(
            &engine->haptic_queue,
            event,
            &request_enqueued);
        /* 返回成功。 */
        return WORKOUT_STATUS_OK;
    }
    /* 污染、插值不足、上层冻结或累计分类不一致时，不推进相位/步峰。 */
    if (!count_input_valid || !engine->classification_consistent) {
        /* 热量已按锁定会话推进，但计数安全跳过且不修改未完成相位。 */
        return WORKOUT_STATUS_OK;
    }
    /* 计算当前原始点的相位或步峰。 */
    motion_phase_observation_t observation;
    /* 保存相位检测状态。 */
    const motion_phase_status_t phase_status = motion_phase_push(
        &engine->phase_detector,
        sample,
        &observation);
    /* 长间断时同步重置下游不完整周期。 */
    if (phase_status == MOTION_PHASE_GAP_RESET) {
        /* 周期动作清空候选相位但保留已计次数。 */
        if (workout_action_is_repetition((fitness_action_t)engine->selected_action)) {
            /* 忽略理论上不应失败的已初始化重置。 */
            (void)fitness_rep_counter_reset_cycle(&engine->rep_counter);
        }
        /* 告知调用方本点因间断未参与计数。 */
        return WORKOUT_STATUS_ERR_TIME;
    }
    /* 其它相位错误映射为参数或领域错误。 */
    if (phase_status != MOTION_PHASE_OK) {
        /* 返回领域错误。 */
        return WORKOUT_STATUS_ERR_DOMAIN;
    }

    /* 8 个周期动作只在有效相位时推进计数器。 */
    if (workout_action_is_repetition((fitness_action_t)engine->selected_action) &&
        observation.phase_valid) {
        /* 保存是否完成完整周期。 */
        bool rep_completed = false;
        /* 推进两相位或五阶段计数器。 */
        const fitness_status_t rep_status = fitness_rep_counter_update(
            &engine->rep_counter,
            observation.phase,
            sample->monotonic_ms,
            &rep_completed);
        /* 非法相位时间属于领域错误。 */
        if (rep_status != FITNESS_STATUS_OK) {
            /* 返回错误。 */
            return WORKOUT_STATUS_ERR_DOMAIN;
        }
        /* 尚未闭合周期时没有指标事件。 */
        if (!rep_completed) {
            /* 返回成功。 */
            return WORKOUT_STATUS_OK;
        }
        /* 完整周期写入唯一 REP MetricEvent。 */
        if (fitness_session_record_count(
                &engine->fitness_session,
                FITNESS_METRIC_REPETITION,
                1U,
                sample->monotonic_ms,
                stability_q15,
                quality_flags,
                event) != FITNESS_STATUS_OK) {
            /* 领域会话与计数器不同步。 */
            return WORKOUT_STATUS_ERR_DOMAIN;
        }
        /* 标记事件有效。 */
        *emitted = true;
    }

    /* walk/trot 的局部冲击峰交给生理不应期去重器。 */
    if (workout_action_is_step((fitness_action_t)engine->selected_action) &&
        observation.step_peak) {
        /* 保存步峰是否被接受。 */
        bool step_accepted = false;
        /* 保存每 10 步提示点；实际振动仍由 MetricEvent 统一规则生成。 */
        bool haptic_due = false;
        /* 推进步峰去重器。 */
        const fitness_status_t step_status = fitness_step_counter_accept(
            &engine->step_counter,
            sample->monotonic_ms,
            &step_accepted,
            &haptic_due);
        /* 显式读取 haptic_due，实际队列由事件规则决定，避免两处振动。 */
        (void)haptic_due;
        /* 时间或初始化错误必须上报。 */
        if (step_status != FITNESS_STATUS_OK) {
            /* 返回领域错误。 */
            return WORKOUT_STATUS_ERR_DOMAIN;
        }
        /* 被不应期拒绝的峰不产生事件。 */
        if (!step_accepted) {
            /* 返回成功。 */
            return WORKOUT_STATUS_OK;
        }
        /* 接受一步后写入唯一 STEP MetricEvent。 */
        if (fitness_session_record_count(
                &engine->fitness_session,
                FITNESS_METRIC_STEP,
                1U,
                sample->monotonic_ms,
                stability_q15,
                quality_flags,
                event) != FITNESS_STATUS_OK) {
            /* 返回领域错误。 */
            return WORKOUT_STATUS_ERR_DOMAIN;
        }
        /* 标记事件有效。 */
        *emitted = true;
    }

    /* 新指标统一决定每次 REP、每 10 STEP 或 sit 无振动。 */
    if (*emitted) {
        /* 保存本次是否成功入队。 */
        bool request_enqueued = false;
        /* 队列满只丢反馈，不回滚权威次数。 */
        (void)fitness_haptic_enqueue_for_metric(
            &engine->haptic_queue,
            event,
            &request_enqueued);
    }
    /* 当前点处理成功。 */
    return WORKOUT_STATUS_OK;
}

workout_status_t workout_engine_pause(
    workout_engine_t *engine,
    const uint64_t now_ms)
{
    /* 对象必须有效。 */
    if (engine == NULL) {
        /* 返回参数错误。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* 重复暂停保持幂等。 */
    if (engine->state == WORKOUT_STATE_PAUSED) {
        /* 不重复振动或修改时间。 */
        return WORKOUT_STATUS_OK;
    }
    /* 只允许运行状态暂停。 */
    if (engine->state != WORKOUT_STATE_RUNNING) {
        /* 其它状态没有可暂停的已锁定会话。 */
        return WORKOUT_STATUS_ERR_STATE;
    }
    /* 把积分基准移动到暂停时刻，不额外计算缺失区间。 */
    if (fitness_session_rebase_time(&engine->fitness_session, now_ms) != FITNESS_STATUS_OK) {
        /* 时间倒退或会话错误。 */
        return WORKOUT_STATUS_ERR_TIME;
    }
    /* 清空相位检测器不完整周期。 */
    motion_phase_reset(&engine->phase_detector);
    /* 重复动作同步清空下游候选周期。 */
    if (workout_action_is_repetition((fitness_action_t)engine->selected_action)) {
        /* 已初始化计数器重置应成功。 */
        (void)fitness_rep_counter_reset_cycle(&engine->rep_counter);
    }
    /* 暂停结束当前连续动作证据；恢复后由新窗口重新确认锁定动作。 */
    workout_reset_bout(engine);
    /* 切换到暂停状态。 */
    engine->state = WORKOUT_STATE_PAUSED;
    /* 入队一次 40 ms 暂停反馈。 */
    (void)fitness_haptic_enqueue_reason(
        &engine->haptic_queue,
        FITNESS_HAPTIC_REASON_PAUSE_OR_END);
    /* 返回成功。 */
    return WORKOUT_STATUS_OK;
}

workout_status_t workout_engine_resume(
    workout_engine_t *engine,
    const uint64_t now_ms)
{
    /* 对象必须有效。 */
    if (engine == NULL) {
        /* 返回参数错误。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* 重复恢复保持幂等。 */
    if (engine->state == WORKOUT_STATE_RUNNING) {
        /* 已运行无需修改。 */
        return WORKOUT_STATUS_OK;
    }
    /* 只允许 Paused 恢复。 */
    if (engine->state != WORKOUT_STATE_PAUSED) {
        /* 返回状态错误。 */
        return WORKOUT_STATUS_ERR_STATE;
    }
    /* 把热量积分基准跳到恢复时刻，排除完整暂停区间。 */
    if (fitness_session_rebase_time(&engine->fitness_session, now_ms) != FITNESS_STATUS_OK) {
        /* 时间倒退或领域状态错误。 */
        return WORKOUT_STATUS_ERR_TIME;
    }
    /* 清空相位方向和步峰历史，避免暂停两侧拼接。 */
    motion_phase_reset(&engine->phase_detector);
    /* 进入运行状态。 */
    engine->state = WORKOUT_STATE_RUNNING;
    /* 返回成功。 */
    return WORKOUT_STATUS_OK;
}

workout_status_t workout_engine_stop(
    workout_engine_t *engine,
    const uint64_t now_ms)
{
    /* 对象必须有效。 */
    if (engine == NULL) {
        /* 返回参数错误。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* Summary 重复停止保持幂等。 */
    if (engine->state == WORKOUT_STATE_SUMMARY) {
        /* 不重复保存或振动。 */
        return WORKOUT_STATUS_OK;
    }
    /* Idle 没有会话可停止。 */
    if (engine->state == WORKOUT_STATE_IDLE) {
        /* 返回状态错误。 */
        return WORKOUT_STATUS_ERR_STATE;
    }
    /* PREPARING 尚未创建领域会话，可直接结束为空摘要。 */
    if (engine->state == WORKOUT_STATE_PREPARING) {
        /* 切换到总结。 */
        engine->state = WORKOUT_STATE_SUMMARY;
        /* 返回成功。 */
        return WORKOUT_STATUS_OK;
    }
    /* Running 停止时先把时间基准移动到停止时刻；Paused 已在暂停时重设。 */
    if (engine->state == WORKOUT_STATE_RUNNING) {
        /* 时间必须单调。 */
        if (fitness_session_rebase_time(&engine->fitness_session, now_ms) != FITNESS_STATUS_OK) {
            /* 返回时间错误。 */
            return WORKOUT_STATUS_ERR_TIME;
        }
    }
    /* 停止领域会话，使后续样本不能再修改累计。 */
    if (fitness_session_stop(&engine->fitness_session) != FITNESS_STATUS_OK) {
        /* 返回领域错误。 */
        return WORKOUT_STATUS_ERR_DOMAIN;
    }
    /* 清空未完成相位。 */
    motion_phase_reset(&engine->phase_detector);
    /* 切换到总结状态。 */
    engine->state = WORKOUT_STATE_SUMMARY;
    /* 入队一次结束反馈；重复 stop 不再入队。 */
    (void)fitness_haptic_enqueue_reason(
        &engine->haptic_queue,
        FITNESS_HAPTIC_REASON_PAUSE_OR_END);
    /* 返回成功。 */
    return WORKOUT_STATUS_OK;
}

void workout_engine_return_idle(workout_engine_t *engine)
{
    /* 空指针安全无操作。 */
    if (engine == NULL) {
        /* 没有状态可清理。 */
        return;
    }
    /* 重新初始化为空闲并清空振动队列。 */
    workout_engine_init(engine);
}

workout_status_t workout_engine_snapshot(
    const workout_engine_t *engine,
    workout_snapshot_t *snapshot)
{
    /* 输入输出都必须有效。 */
    if ((engine == NULL) || (snapshot == NULL)) {
        /* 返回参数错误。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* 先清零，确保准备状态没有旧领域值。 */
    (void)memset(snapshot, 0, sizeof(*snapshot));
    /* 复制产品状态。 */
    snapshot->state = engine->state;
    /* 复制会话序号。 */
    snapshot->session_seq = engine->session_seq;
    /* 复制锁定动作或 255。 */
    snapshot->action_id = engine->selected_action;
    /* 复制当前动作段因果累计分类或 255。 */
    snapshot->inferred_action_id = engine->inferred_action;
    /* 复制分类与计数动作一致性，供上层显示动作变化提示。 */
    snapshot->classification_consistent = engine->classification_consistent;
    /* 复制模型置信度。 */
    snapshot->confidence_q15 = engine->confidence_q15;
    /* 复制质量位。 */
    snapshot->quality_flags = engine->quality_flags;
    /* 未锁定时没有领域会话指标。 */
    if (engine->selected_action == WORKOUT_ACTION_UNKNOWN) {
        /* 使用重复单位占位，action_id=255 使调用方忽略。 */
        snapshot->metric_kind = FITNESS_METRIC_REPETITION;
        /* 返回准备/空闲快照。 */
        return WORKOUT_STATUS_OK;
    }
    /* 转换锁定动作。 */
    const fitness_action_t action = (fitness_action_t)engine->selected_action;
    /* 计算主指标类型。 */
    snapshot->metric_kind = workout_metric_kind(action);
    /* 重复类复制累计次数。 */
    if (snapshot->metric_kind == FITNESS_METRIC_REPETITION) {
        /* 饱和复制次数。 */
        snapshot->metric_value = engine->fitness_session.repetitions;
    } else if (snapshot->metric_kind == FITNESS_METRIC_STEP) {
        /* 步类复制累计步数。 */
        snapshot->metric_value = engine->fitness_session.steps;
    } else {
        /* sit 对 UI 输出完整秒，内部仍保留毫秒。 */
        snapshot->metric_value = engine->fitness_session.sit_duration_ms / 1000ULL;
    }
    /* 复制毛热量。 */
    snapshot->gross_microkcal = engine->fitness_session.gross_microkcal;
    /* 复制活动热量。 */
    snapshot->active_microkcal = engine->fitness_session.active_microkcal;
    /* 防止静态分析把转换辅助函数视为未使用；同时证明 UI uint32 可安全取得。 */
    (void)workout_saturate_u64_to_u32(snapshot->metric_value);
    /* 返回成功。 */
    return WORKOUT_STATUS_OK;
}

bool workout_engine_pop_haptic(
    workout_engine_t *engine,
    fitness_haptic_request_t *request)
{
    /* 空对象或输出不能消费队列。 */
    if ((engine == NULL) || (request == NULL)) {
        /* 返回无请求。 */
        return false;
    }
    /* 委托固定 FIFO 取最早业务振动。 */
    return fitness_haptic_queue_pop(&engine->haptic_queue, request);
}
