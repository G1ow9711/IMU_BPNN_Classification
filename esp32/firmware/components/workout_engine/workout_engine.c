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

/* 与 imu_pipeline.h 第 4 位保持一致；该位只表示马达导通污染，不表示采样时间断流。 */
#define WORKOUT_QUALITY_HAPTIC_CONTAMINATED (UINT16_C(1) << 4U)
/* 位 0～3、5、6 分别表示加速度/角速度缺口、乱序、队列溢出、驱动丢样和重采样重置。 */
#define WORKOUT_QUALITY_TIMELINE_BREAK_MASK \
    ((UINT16_C(1) << 0U) | (UINT16_C(1) << 1U) | \
     (UINT16_C(1) << 2U) | (UINT16_C(1) << 3U) | \
     (UINT16_C(1) << 5U) | (UINT16_C(1) << 6U))

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

/* 对单个运行窗口执行有限值校验和稳定 argmax，不把旧动作证据带入新动作判断。 */
static workout_status_t workout_classify_single_window(
    const float logits[WORKOUT_CLASS_COUNT],
    uint8_t *best_class,
    uint16_t *confidence_q15)
{
    /* 三个指针均为同步必填参数，函数不保存它们的地址。 */
    if ((logits == NULL) || (best_class == NULL) || (confidence_q15 == NULL)) {
        /* 缺少输入或输出时拒绝本窗。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* 从类别零开始，严格大于比较保持与 NumPy 首个最大值规则一致。 */
    uint8_t selected_class = 0U;
    /* 首个 logit 必须有限，否则 softmax 和比较都没有定义。 */
    if (!isfinite(logits[0])) {
        /* 非有限输入不会污染切换候选。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* 遍历剩余十类寻找当前窗口最大融合 logit。 */
    for (uint8_t class_index = 1U; class_index < WORKOUT_CLASS_COUNT; ++class_index) {
        /* 任一类别非有限时整窗拒绝，避免错误类被静默忽略。 */
        if (!isfinite(logits[class_index])) {
            /* 返回参数错误。 */
            return WORKOUT_STATUS_ERR_ARGUMENT;
        }
        /* 只在严格更大时更新，平局继续保留更小类别索引。 */
        if (logits[class_index] > logits[selected_class]) {
            /* 保存新的单窗最优类别。 */
            selected_class = class_index;
        }
    }
    /* 复用数值稳定 softmax，把单窗 logits 当作长度一的平均值。 */
    const workout_status_t confidence_status = workout_confidence_from_averaged_logits(
        logits,
        selected_class,
        confidence_q15);
    /* softmax 异常时保持调用方输出不变。 */
    if (confidence_status != WORKOUT_STATUS_OK) {
        /* 传播精确错误。 */
        return confidence_status;
    }
    /* 所有检查完成后提交类别输出。 */
    *best_class = selected_class;
    /* 单窗分类成功。 */
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
    /* 记录调用前是否已经锁定本次会话唯一动作。 */
    const bool action_locked =
        engine->selected_action < (uint8_t)FITNESS_ACTION_COUNT;
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
    /* 已锁类会话继续展示唯一动作；准备状态没有有效分类。 */
    engine->inferred_action =
        action_locked ? engine->selected_action : WORKOUT_ACTION_UNKNOWN;
    /* 重置后必须等待新的干净同类窗口；准备态和已锁类会话都先关闭计数门。 */
    engine->classification_consistent = false;
    /* 清空候选动作。 */
    engine->candidate_action = WORKOUT_ACTION_UNKNOWN;
    /* 清空候选连续数。 */
    engine->candidate_windows = 0U;
    /* 清空当前置信度。 */
    engine->confidence_q15 = 0U;
}

/* 清空准备期环形样本；开始新会话或准备期出现连续性缺口时调用。 */
static void workout_clear_prelock_samples(workout_engine_t *engine)
{
    /* 下一点从数组首槽写入。 */
    engine->prelock_write_index = 0U;
    /* 当前没有可补算样本。 */
    engine->prelock_sample_count = 0U;
}

/* 保存一个准备期点；160 点覆盖一次窗口重建后第四窗锁类前的完整 6.40 秒。 */
static void workout_buffer_prelock_sample(
    workout_engine_t *engine,
    const motion_phase_sample_t *sample,
    const bool count_input_valid,
    const uint16_t quality_flags)
{
    /* 按值复制单调时间和六轴物理量，调用返回后不借用上层指针。 */
    engine->prelock_samples[engine->prelock_write_index].sample = *sample;
    /* 保存真实计数许可，补算不能把污染点伪装成干净点。 */
    engine->prelock_samples[engine->prelock_write_index].count_input_valid =
        count_input_valid;
    /* 保存当前点质量位，事件与诊断沿用原始事实。 */
    engine->prelock_samples[engine->prelock_write_index].quality_flags =
        quality_flags;
    /* 写指针循环移动到下一槽。 */
    engine->prelock_write_index =
        (uint8_t)((engine->prelock_write_index + 1U) %
                  WORKOUT_PRELOCK_SAMPLE_CAPACITY);
    /* 未满时增加有效点数；满后覆盖最旧点且数量保持 160。 */
    if (engine->prelock_sample_count < WORKOUT_PRELOCK_SAMPLE_CAPACITY) {
        /* 记录新增一个准备期点。 */
        engine->prelock_sample_count += 1U;
    }
}

/* 初始化开合跳三轴加速度峰谷状态；任一失败都拒绝进入半初始化会话。 */
static workout_status_t workout_init_jumping_jack_periodic(workout_engine_t *engine)
{
    /* 训练引擎必须存在。 */
    if (engine == NULL) {
        /* 空对象无法保存三个轴状态。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* ax、ay、az 三个加速度轴分别建立 11+5 均值和峰谷配对器。 */
    for (uint8_t axis = 0U; axis < WORKOUT_JUMPING_JACK_AXIS_COUNT; ++axis) {
        /* 当前轴从零建立静态滤波和未配对极值状态。 */
        if (motion_periodic_pair_init(
                &engine->jumping_jack_pair_detectors[axis]) != MOTION_PHASE_OK) {
            /* 任一轴初始化失败都拒绝运行，防止中位数缺轴。 */
            return WORKOUT_STATUS_ERR_DOMAIN;
        }
    }
    /* 新会话尚未把任何三轴中位数写入权威领域会话。 */
    engine->jumping_jack_reported_repetitions = 0ULL;
    /* 三轴峰谷状态初始化完成。 */
    return WORKOUT_STATUS_OK;
}

/* 清空开合跳三个轴的滤波与未完成峰谷；保留各轴累计和已发布中位数。 */
static void workout_reset_jumping_jack_periodic_cycles(workout_engine_t *engine)
{
    /* 空对象没有可重置状态。 */
    if (engine == NULL) {
        /* 防御性返回。 */
        return;
    }
    /* 三个轴必须同步清空，避免暂停或采样间断两侧拼成伪峰谷对。 */
    for (uint8_t axis = 0U; axis < WORKOUT_JUMPING_JACK_AXIS_COUNT; ++axis) {
        /* 清空当前轴滤波、极值、不应期和时间线，但保留 total_pairs。 */
        motion_periodic_pair_reset_cycle(
            &engine->jumping_jack_pair_detectors[axis]);
    }
}

/* 清空当前主动作尚未闭合的计数证据；保留会话主动作、累计次数、步数和事件序号。 */
static void workout_reset_incomplete_counting(workout_engine_t *engine)
{
    /* 空对象或尚未选择会话主动作时没有已初始化的计数链。 */
    if ((engine == NULL) ||
        (engine->selected_action >= (uint8_t)FITNESS_ACTION_COUNT)) {
        /* 防止读取未初始化领域对象。 */
        return;
    }
    /* 清空主相位、方向、步峰和时间历史；动作类型保持不变。 */
    motion_phase_reset(&engine->phase_detector);
    /* 八类重复动作还要清空 fitness_core 内部的未完成相位序列。 */
    if (workout_action_is_repetition((fitness_action_t)engine->selected_action)) {
        /* 已锁类重复动作的计数器必已初始化；失败不回滚历史累计。 */
        (void)fitness_rep_counter_reset_cycle(&engine->rep_counter);
    }
    /* 开合跳三轴峰谷必须同步清空，禁止休息前后两段拼成一个周期。 */
    if (engine->selected_action == (uint8_t)FITNESS_ACTION_JUMPING_JACK) {
        /* 只清滤波、极值和不应期；各轴 total_pairs 与权威累计保留。 */
        workout_reset_jumping_jack_periodic_cycles(engine);
    }
}

/* 锁类后按时间顺序补算准备期样本；MetricEvent 已进入会话累计，振动进入同一 FIFO。 */
static workout_status_t workout_replay_prelock_samples(workout_engine_t *engine)
{
    /* 计算最旧有效点槽位；未满时从 0 开始，满时 write_index 正好指向最旧点。 */
    const uint8_t oldest_index =
        (uint8_t)((engine->prelock_write_index +
                   WORKOUT_PRELOCK_SAMPLE_CAPACITY -
                   engine->prelock_sample_count) %
                  WORKOUT_PRELOCK_SAMPLE_CAPACITY);
    /* 逐点按原始时间递增顺序重放，最多固定 160 次，时间复杂度 O(160)。 */
    for (uint8_t offset = 0U; offset < engine->prelock_sample_count; ++offset) {
        /* 把相对偏移映射到环形数组真实槽位。 */
        const uint8_t sample_index =
            (uint8_t)((oldest_index + offset) %
                      WORKOUT_PRELOCK_SAMPLE_CAPACITY);
        /* 取得只读准备期点；生命周期覆盖当前循环。 */
        const workout_prelock_sample_t *buffered =
            &engine->prelock_samples[sample_index];
        /* 保存可能产生的唯一指标事件；协调器随后从快照读取累计结果。 */
        fitness_metric_event_t replay_event;
        /* 保存当前补算点是否闭合一个动作周期。 */
        bool replay_emitted = false;
        /* 复用正式运行计数链，保证相位、热量、事件和振动规则完全一致。 */
        const workout_status_t replay_status = workout_engine_push_sample(
            engine,
            &buffered->sample,
            buffered->count_input_valid,
            buffered->quality_flags,
            &replay_event,
            &replay_emitted);
        /* 参数、时间或领域错误使本次锁类事务失败，由协调器候选副本整体回滚。 */
        if (replay_status != WORKOUT_STATUS_OK) {
            /* 返回具体失败，不提交半补算会话。 */
            return replay_status;
        }
        /* 补算产生事件时必须保留原始时刻和序号，供协调器逐条发布 BLE EventV1。 */
        if (replay_emitted) {
            /* 固定 FIFO 满表示当前锁类跨度违反容量推导，整次候选事务必须回滚。 */
            if (engine->replay_metric_event_count >= WORKOUT_REPLAY_EVENT_QUEUE_CAPACITY) {
                /* 返回领域错误，禁止只发布后半段事件。 */
                return WORKOUT_STATUS_ERR_DOMAIN;
            }
            /* 计算尾部写槽，范围固定 0..11。 */
            const uint8_t write_index = (uint8_t)(
                (engine->replay_metric_event_head + engine->replay_metric_event_count) %
                WORKOUT_REPLAY_EVENT_QUEUE_CAPACITY);
            /* 按值保存完整 MetricEvent，包含原始设备毫秒和 event_seq。 */
            engine->replay_metric_events[write_index] = replay_event;
            /* 有效事件数增加一。 */
            engine->replay_metric_event_count += 1U;
        }
    }
    /* 补算完成后清空缓存，避免后续推理重复计数同一批样本。 */
    workout_clear_prelock_samples(engine);
    /* 全部点成功重放。 */
    return WORKOUT_STATUS_OK;
}

/* 锁定准备阶段动作并初始化唯一领域会话。 */
static workout_status_t workout_lock_action(
    workout_engine_t *engine,
    const fitness_action_t action)
{
    /* 会话从用户点击开始的时刻计时，使准备期补算样本时间合法且热量不丢失。 */
    if (fitness_session_start(
            &engine->fitness_session,
            engine->session_seq,
            action,
            engine->weight_g,
            engine->started_ms) != FITNESS_STATUS_OK) {
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
    /* 开合跳额外初始化 gx、gy、gz 三条独立周期链；通用计数器保留给统一快照合同。 */
    if (action == FITNESS_ACTION_JUMPING_JACK) {
        /* 初始化失败时停止已建领域会话，不能带部分轴状态进入补算。 */
        if (workout_init_jumping_jack_periodic(engine) != WORKOUT_STATUS_OK) {
            /* 停止领域会话。 */
            (void)fitness_session_stop(&engine->fitness_session);
            /* 返回领域错误。 */
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
    /* 补算首窗内已经完成的动作；失败时协调器不会提交候选引擎副本。 */
    const workout_status_t replay_status = workout_replay_prelock_samples(engine);
    /* 补算错误不能带着部分次数进入运行状态。 */
    if (replay_status != WORKOUT_STATUS_OK) {
        /* 停止当前局部领域会话，调用方随后丢弃候选副本。 */
        (void)fitness_session_stop(&engine->fitness_session);
        /* 返回真实补算错误。 */
        return replay_status;
    }
    /* 不发送“开始”马达反馈，避免锁类时的振动污染紧接着的第一个实时动作。 */
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
    /* PREPARING 已闭合动作和干净模型候选仍属于本次开始后的有效事实。 */
    if (engine->state == WORKOUT_STATE_PREPARING) {
        /* 边界质量点随后写入准备缓存；重放时它只切断未完成半周期。 */
        return;
    }
    /* RUNNING 的连续性边界必须冻结计数，直到新的干净同类窗口恢复。 */
    if (engine->state == WORKOUT_STATE_RUNNING) {
        /* 清空过期诊断累计并关闭计数门；selected_action 和权威累计保持。 */
        workout_reset_bout(engine);
        /* 清除边界前未完成相位，禁止与边界后的腕部动作拼接。 */
        workout_reset_incomplete_counting(engine);
    }
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

    /* RUNNING 保持会话主动作不变，但实时类别决定该主动作计数器是否可继续。 */
    if (engine->state == WORKOUT_STATE_RUNNING) {
        /* 保存当前窗口诊断最优类别，但该类别不成为新的 selected_action。 */
        uint8_t window_class = WORKOUT_ACTION_UNKNOWN;
        /* 对单窗做有限值校验、argmax 和数值稳定 softmax。 */
        const workout_status_t classify_status = workout_classify_single_window(
            logits,
            &window_class,
            &engine->confidence_q15);
        /* 非有限值或 softmax 异常时拒绝本窗，不能用错误 logits 恢复计数。 */
        if (classify_status != WORKOUT_STATUS_OK) {
            /* 传播错误。 */
            return classify_status;
        }
        /* 清空准备期候选；运行阶段不再维护动作切换滞回。 */
        engine->candidate_action = WORKOUT_ACTION_UNKNOWN;
        /* 运行期候选窗固定为零。 */
        engine->candidate_windows = 0U;
        /* 对外公开本窗真实类别；selected_action 继续保存本次会话主动作和计数器类型。 */
        engine->inferred_action = window_class;
        /* 保存更新前门状态，用于只在冻结或恢复边界清空一次未完成周期。 */
        const bool was_consistent = engine->classification_consistent;
        /* 只有实时类别与主动作一致且置信度过门，当前窗才可能允许计数。 */
        const bool same_confident_action =
            (window_class == engine->selected_action) &&
            (engine->confidence_q15 >= WORKOUT_ACTION_LOCK_CONFIDENCE_Q15);
        /* 异类或低置信窗口立即冻结；站立、静坐和切换动作均进入该分支。 */
        if (!same_confident_action) {
            /* 当前窗口不能证明用户仍在执行本轮主动作。 */
            engine->classification_consistent = false;
        } else if (quality_flags == 0U) {
            /* 只有零质量告警的同类高置信窗口能够恢复计数。 */
            engine->classification_consistent = true;
        }
        /* 同类但带质量告警的窗口保持原门状态，既不误恢复也不因边界标志重复冻结。 */
        if (engine->classification_consistent != was_consistent) {
            /* 冻结和恢复两侧都清空半周期，恢复后必须从全新完整周期开始。 */
            workout_reset_incomplete_counting(engine);
        }
        /* 诊断更新成功。 */
        return WORKOUT_STATUS_OK;
    }

    /* PREPARING 只累计零质量告警窗口；告警窗不能锁类，也不能删除先前干净证据。 */
    if (quality_flags != 0U) {
        /* 等待下一完整干净窗继续相同候选。 */
        return WORKOUT_STATUS_IGNORED;
    }

    /* 保存当前窗口加入后的 11 类动作段平均 logits，作为准备态唯一锁类证据。 */
    float averaged_logits[WORKOUT_CLASS_COUNT];
    /* 累计器从会话起点纳入当前及历史窗口，减弱动作起步瞬态误窗。 */
    const int inferred_class = workout_update_bout_accumulator(
        engine,
        logits,
        averaged_logits);
    /* 非有限输入或累计溢出时保持旧引擎状态并拒绝本帧。 */
    if (inferred_class < 0) {
        /* 返回参数错误。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* 累计 argmax 已由 Python/C 同源 BpBoutAccumulator 返回，范围固定为 0..10。 */
    const uint8_t best_class = (uint8_t)inferred_class;
    /* 对累计平均 logits 计算数值稳定 softmax 概率，与离线策略审计一致。 */
    const workout_status_t confidence_status = workout_confidence_from_averaged_logits(
        averaged_logits,
        best_class,
        &engine->confidence_q15);
    /* 计算异常直接返回。 */
    if (confidence_status != WORKOUT_STATUS_OK) {
        /* 传播错误。 */
        return confidence_status;
    }
    /* 每窗立即公开当前累计候选，UI 可在正式锁定前显示“识别中”的暂定动作。 */
    engine->inferred_action = best_class;
    /* 累计最优类保持不变时增加连续证据，否则新类从第一窗重新计数。 */
    if (engine->candidate_action == best_class) {
        /* 准备态最多四窗，uint8 不会溢出。 */
        engine->candidate_windows += 1U;
    } else {
        /* 保存新的累计候选类。 */
        engine->candidate_action = best_class;
        /* 当前窗口是该候选的第一份连续证据。 */
        engine->candidate_windows = 1U;
    }
    /* stable_and_confident 表示验证集选择的正常锁定门已满足。 */
    const bool stable_and_confident =
        (engine->candidate_windows >= WORKOUT_ACTION_LOCK_WINDOWS) &&
        (engine->confidence_q15 >= WORKOUT_ACTION_LOCK_CONFIDENCE_Q15);
    /* reached_bounded_limit 保证低置信会话最迟第四窗结束准备，不永久停留。 */
    const bool reached_bounded_limit =
        engine->bout_window_count >= WORKOUT_ACTION_MAX_PREPARE_WINDOWS;
    /* 正常门和有界兜底均未满足时继续累计下一重叠窗。 */
    if (!stable_and_confident && !reached_bounded_limit) {
        /* 当前已输出候选和置信度，但尚未提交 selected_action。 */
        return WORKOUT_STATUS_IGNORED;
    }
    /* 累计确认或第四窗兜底后初始化本次唯一动作会话并补算准备点。 */
    const workout_status_t lock_status = workout_lock_action(
        engine,
        (fitness_action_t)best_class);
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

/* 返回三个无符号累计值的中位数；比较法避免 a+b+c 在多年运行后溢出。 */
static uint64_t workout_median_three_u64(
    const uint64_t a,
    const uint64_t b,
    const uint64_t c)
{
    /* b 同时位于 a 与 c 之间时直接返回 b。 */
    if (((a <= b) && (b <= c)) || ((c <= b) && (b <= a))) {
        /* b 是中位数。 */
        return b;
    }
    /* a 同时位于 b 与 c 之间时返回 a。 */
    if (((b <= a) && (a <= c)) || ((c <= a) && (a <= b))) {
        /* a 是中位数。 */
        return a;
    }
    /* 其余有序关系下 c 必然位于 a 与 b 之间。 */
    return c;
}

/*
 * 推进开合跳三轴加速度峰谷计数。
 * 每轴用 11+5 均值检测相邻峰谷；权威次数取三个单调累计的中位数，抑制单轴噪声。
 * 时间复杂度 O(3)，空间为三组固定环形数组，无堆分配；一个峰和一个谷直接计一次，不乘二。
 */
static workout_status_t workout_push_jumping_jack_periodic(
    workout_engine_t *engine,
    const motion_phase_sample_t *sample,
    bool *rep_accepted)
{
    /* 引擎、六轴点和输出地址必须有效。 */
    if ((engine == NULL) || (sample == NULL) || (rep_accepted == NULL)) {
        /* 无法安全推进三轴状态。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* 默认当前点没有产生权威次数。 */
    *rep_accepted = false;
    /* true 表示检测到超过 120 ms 的采样间断，三个轴都必须清空未完成周期。 */
    bool gap_detected = false;
    /* 同一六轴点依次送入 ax、ay、az 三个独立流式峰谷检测器。 */
    for (uint8_t axis = 0U; axis < WORKOUT_JUMPING_JACK_AXIS_COUNT; ++axis) {
        /* axis_completed 保存当前点是否使该轴 total_pairs 增加一次。 */
        bool axis_completed = false;
        /* 加速度从六轴数组下标 3 开始，输入单位固定为 g。 */
        const motion_phase_status_t phase_status = motion_periodic_pair_push(
            &engine->jumping_jack_pair_detectors[axis],
            sample->monotonic_ms,
            sample->axis[3U + axis],
            &axis_completed);
        /* 显式读取轴完成标志；公开事件由累计中位数而非任一单轴决定。 */
        (void)axis_completed;
        /* 任一轴报告采样间断时先完成其它轴检查，随后统一重置。 */
        if (phase_status == MOTION_PHASE_GAP_RESET) {
            /* 记录间断，不使用当前轴观测。 */
            gap_detected = true;
            /* 继续让其它轴消费同一时间戳，使三个检测器时间线一致。 */
            continue;
        }
        /* 其它相位错误说明输入或内部状态无效。 */
        if (phase_status != MOTION_PHASE_OK) {
            /* 映射为训练领域错误。 */
            return WORKOUT_STATUS_ERR_DOMAIN;
        }
    }
    /* 间断优先于当前点候选，防止跨缺口拼接和部分轴状态不一致。 */
    if (gap_detected) {
        /* 三个轴同步清空未完成周期。 */
        workout_reset_jumping_jack_periodic_cycles(engine);
        /* 真实缺口是已成功消费的非致命边界；返回 OK 使协调器提交重置后的候选副本。 */
        return WORKOUT_STATUS_OK;
    }
    /* 读取三个轴的单调累计；数组顺序固定 ax、ay、az。 */
    const uint64_t fused_repetitions = workout_median_three_u64(
        engine->jumping_jack_pair_detectors[0].total_pairs,
        engine->jumping_jack_pair_detectors[1].total_pairs,
        engine->jumping_jack_pair_detectors[2].total_pairs);
    /* 中位数未超过已发布值时，当前单轴变化不足以形成权威次数。 */
    if (fused_repetitions <= engine->jumping_jack_reported_repetitions) {
        /* 正常返回，不产生 MetricEvent。 */
        return WORKOUT_STATUS_OK;
    }
    /* 单个原始点每轴最多增加一次，因此中位数不允许跨越两个以上次数。 */
    if ((fused_repetitions - engine->jumping_jack_reported_repetitions) != 1ULL) {
        /* 内部状态不一致时拒绝批量补发，避免 UI 瞬间跳数且振动次数不匹配。 */
        return WORKOUT_STATUS_ERR_DOMAIN;
    }
    /* 保存已经发布的新中位累计。 */
    engine->jumping_jack_reported_repetitions = fused_repetitions;
    /* 告知上层写入一个且仅一个 REP MetricEvent。 */
    *rep_accepted = true;
    /* 当前点处理成功。 */
    return WORKOUT_STATUS_OK;
}

/*
 * 跳过一个马达污染点但推进开合跳三轴时间线。
 * 受污染 ax/ay/az 不进入均值和峰谷；真实大于 120 ms 的间断仍同步重置三轴未完成周期。
 */
static workout_status_t workout_skip_jumping_jack_haptic_sample(
    workout_engine_t *engine,
    const uint64_t monotonic_ms)
{
    /* 引擎必须有效；调用方已经确认当前锁定动作为开合跳。 */
    if (engine == NULL) {
        /* 空对象无法维护三个轴的时间线。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* gap_detected 汇总任一轴发现的真实采样断流。 */
    bool gap_detected = false;
    /* 三轴必须消费同一污染时刻，防止中位数检测器的时间基准分叉。 */
    for (uint8_t axis = 0U; axis < WORKOUT_JUMPING_JACK_AXIS_COUNT; ++axis) {
        /* 只推进当前轴时间戳，禁止把马达振动幅值写入 11+5 均值。 */
        const motion_phase_status_t phase_status = motion_periodic_pair_skip_transient(
            &engine->jumping_jack_pair_detectors[axis],
            monotonic_ms);
        /* 真实断流先记录，循环结束后统一清空三轴未完成状态。 */
        if (phase_status == MOTION_PHASE_GAP_RESET) {
            /* 当前轴已自行保留累计并清空滤波。 */
            gap_detected = true;
            /* 继续处理剩余轴，保持时间线一致。 */
            continue;
        }
        /* 参数或时间倒退属于不可恢复的本次输入错误。 */
        if (phase_status != MOTION_PHASE_OK) {
            /* 映射为训练领域错误，协调器不会提交候选副本。 */
            return phase_status == MOTION_PHASE_ERR_TIMESTAMP
                ? WORKOUT_STATUS_ERR_TIME
                : WORKOUT_STATUS_ERR_DOMAIN;
        }
    }
    /* 任一轴发现真实断流时，三个轴必须同步清空未完成周期。 */
    if (gap_detected) {
        /* 统一重置只影响滤波和未完成峰谷，不回滚权威累计。 */
        workout_reset_jumping_jack_periodic_cycles(engine);
        /* 返回成功使协调器提交时间占位触发的重置，避免下一点永久重复发现同一缺口。 */
        return WORKOUT_STATUS_OK;
    }
    /* 短暂振动污染已占位，当前点不产生次数。 */
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
    /* PREPARING 立即保存 25 Hz 点，锁类后从点击开始时刻补算。 */
    if (engine->state == WORKOUT_STATE_PREPARING) {
        /* 保存最新质量位供准备页和 BLE 快照显示。 */
        engine->quality_flags = quality_flags;
        /* 环形保存样本和真实计数许可。 */
        workout_buffer_prelock_sample(
            engine,
            sample,
            count_input_valid,
            quality_flags);
        /* 尚未知道动作类别，当前点不能立即产生公开 MetricEvent。 */
        return WORKOUT_STATUS_IGNORED;
    }
    /* 只有已锁定运行状态继续实时接受 25 Hz 点。 */
    if (engine->state != WORKOUT_STATE_RUNNING) {
        /* 暂停、空闲和总结状态有意不累计。 */
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
    /* 检查当前无效点是否仅由马达污染导致，不包含任一真实连续性破坏位。 */
    const bool haptic_only_invalid =
        !count_input_valid &&
        ((quality_flags & WORKOUT_QUALITY_HAPTIC_CONTAMINATED) != 0U) &&
        ((quality_flags & WORKOUT_QUALITY_TIMELINE_BREAK_MASK) == 0U);
    /* 开合跳马达污染只推进时间线，避免 30 ms 反馈反复触发 120 ms 断流重置。 */
    if (haptic_only_invalid &&
        (engine->selected_action == FITNESS_ACTION_JUMPING_JACK)) {
        /* 不使用受污染加速度；当前点不会产生 MetricEvent。 */
        return workout_skip_jumping_jack_haptic_sample(
            engine,
            sample->monotonic_ms);
    }
    /* 真实采样边界必须立刻清空未完成周期；已闭合次数和会话累计保持。 */
    if (!count_input_valid &&
        ((quality_flags & WORKOUT_QUALITY_TIMELINE_BREAK_MASK) != 0U)) {
        /* 当前边界点不使用幅值，只提交重置后的计数器状态。 */
        workout_reset_incomplete_counting(engine);
        /* 该点已安全消费，不让协调器回滚候选副本。 */
        return WORKOUT_STATUS_OK;
    }
    /* 其它污染、上层冻结或实时分类不一致时，不推进相位/步峰。 */
    if (!count_input_valid || !engine->classification_consistent) {
        /* 热量已按锁定会话推进；真实时间缺口会由下一干净点按原 120 ms 合同处理。 */
        return WORKOUT_STATUS_OK;
    }
    /* 开合跳使用三轴加速度峰谷中位数；其它动作继续走通用相位路径。 */
    if (engine->selected_action == FITNESS_ACTION_JUMPING_JACK) {
        /* 保存当前点是否使三轴累计中位数增加一次。 */
        bool rep_accepted = false;
        /* 同一六轴点并行推进 ax、ay、az 三条相邻峰谷链。 */
        const workout_status_t fusion_status = workout_push_jumping_jack_periodic(
            engine,
            sample,
            &rep_accepted);
        /* 时间缺口或领域错误必须原样上报。 */
        if (fusion_status != WORKOUT_STATUS_OK) {
            /* 返回具体错误，当前点不生成事件。 */
            return fusion_status;
        }
        /* 三轴累计中位数没有增加时不产生权威事件。 */
        if (!rep_accepted) {
            /* 当前点处理成功。 */
            return WORKOUT_STATUS_OK;
        }
        /* 接受一次后写入唯一 REP MetricEvent，UI、BLE、存储和振动共享该事实。 */
        if (fitness_session_record_count(
                &engine->fitness_session,
                FITNESS_METRIC_REPETITION,
                1U,
                sample->monotonic_ms,
                stability_q15,
                quality_flags,
                event) != FITNESS_STATUS_OK) {
            /* 领域会话拒绝时不发布半完成事件。 */
            return WORKOUT_STATUS_ERR_DOMAIN;
        }
        /* 通用计数器不参与开合跳融合，镜像其权威累计仅供现有诊断字段读取。 */
        engine->rep_counter.total_repetitions = engine->fitness_session.repetitions;
        /* 保存最近权威次数时间，保持通用诊断与融合事实一致。 */
        engine->rep_counter.last_rep_ms = sample->monotonic_ms;
        /* 标记事件有效。 */
        *emitted = true;
        /* 保存振动是否成功入队；队列满不能回滚权威次数。 */
        bool request_enqueued = false;
        /* 每次有效开合跳按既有合同入队一次 30 ms 振动。 */
        (void)fitness_haptic_enqueue_for_metric(
            &engine->haptic_queue,
            event,
            &request_enqueued);
        /* 当前开合跳点处理成功。 */
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
        /* 本点未计数但重置必须提交；返回 OK 避免协调器事务回滚后永久卡在旧时间基准。 */
        return WORKOUT_STATUS_OK;
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
    /* 暂停统一清空主相位、重复候选、步峰历史和开合跳三轴未完成周期。 */
    workout_reset_incomplete_counting(engine);
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
    /* 恢复前再次统一清空未完成计数证据，防止暂停区间两侧拼接。 */
    workout_reset_incomplete_counting(engine);
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
    /* 开合跳停止时清空三个轴未完成周期，Summary 只保留权威次数。 */
    if (engine->selected_action == FITNESS_ACTION_JUMPING_JACK) {
        /* 清空三个轴相位状态。 */
        workout_reset_jumping_jack_periodic_cycles(engine);
    }
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

bool workout_engine_pop_replay_metric_event(
    workout_engine_t *engine,
    fitness_metric_event_t *event)
{
    /* 空对象、空输出或空队列都不能消费事件。 */
    if ((engine == NULL) || (event == NULL) ||
        (engine->replay_metric_event_count == 0U)) {
        /* 返回没有事件。 */
        return false;
    }
    /* 按值取出最早补算事件。 */
    *event = engine->replay_metric_events[engine->replay_metric_event_head];
    /* 清空已消费槽，避免调试快照误读旧事件。 */
    (void)memset(
        &engine->replay_metric_events[engine->replay_metric_event_head],
        0,
        sizeof(engine->replay_metric_events[engine->replay_metric_event_head]));
    /* 头指针循环推进。 */
    engine->replay_metric_event_head = (uint8_t)(
        (engine->replay_metric_event_head + 1U) % WORKOUT_REPLAY_EVENT_QUEUE_CAPACITY);
    /* 队列数量减少一。 */
    engine->replay_metric_event_count -= 1U;
    /* 返回成功。 */
    return true;
}
