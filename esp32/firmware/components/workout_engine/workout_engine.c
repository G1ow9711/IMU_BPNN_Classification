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

/* 与 imu_pipeline.h 第 4 位保持一致；该位只兼容旧执行器污染，不表示采样时间断流。 */
#define WORKOUT_QUALITY_LEGACY_ACTUATOR_CONTAMINATED (UINT16_C(1) << 4U)
/* 与 imu_pipeline.h 第 7 位保持一致；该位表示双 M0 未产生可信 logits。 */
#define WORKOUT_QUALITY_INFERENCE_FAILED (UINT16_C(1) << 7U)
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
}

/* 清空 25 点活动统计历史；调用方决定是否保留当前计数许可。 */
static void workout_reset_activity_window(workout_engine_t *engine)
{
    /* 空对象没有可清理的活动状态。 */
    if (engine == NULL) {
        /* 防御性返回。 */
        return;
    }
    /* 清空加速度模长环，单位 g；防止旧会话或时间线另一侧进入新方差。 */
    (void)memset(
        engine->activity_acceleration_magnitude_g,
        0,
        sizeof(engine->activity_acceleration_magnitude_g));
    /* 清空角速度模长环，单位 deg/s。 */
    (void)memset(
        engine->activity_gyro_magnitude_dps,
        0,
        sizeof(engine->activity_gyro_magnitude_dps));
    /* 清空逐点活动标志环。 */
    (void)memset(
        engine->activity_point_active,
        0,
        sizeof(engine->activity_point_active));
    /* 下一干净点从槽位零写入。 */
    engine->activity_write_index = 0U;
    /* 当前窗没有有效点。 */
    engine->activity_sample_count = 0U;
    /* 当前窗没有过门活动点。 */
    engine->activity_active_point_count = 0U;
    /* 清空前一加速度向量，三个元素单位均为 g。 */
    (void)memset(
        engine->activity_previous_acceleration_g,
        0,
        sizeof(engine->activity_previous_acceleration_g));
    /* 下一点的加速度差分固定为零，与 Python instantaneous_motion 首点一致。 */
    engine->activity_previous_acceleration_valid = false;
}

/*
 * 用一个干净 25 Hz 六轴点更新训练同源活动门。
 * 公式为 M=std(|a|)+std(|g|)/200，且至少 5/25 点满足 |Δa|+|g|/200>T_active。
 * 时间复杂度 O(25)，空间固定 225 字节；25 Hz 下每秒仅约 625 次标量累加。
 */
static workout_status_t workout_update_activity_gate(
    workout_engine_t *engine,
    const motion_phase_sample_t *sample)
{
    /* 引擎、样本和已锁定动作必须有效。 */
    if ((engine == NULL) || (sample == NULL) ||
        (engine->selected_action >= (uint8_t)FITNESS_ACTION_COUNT)) {
        /* 无法安全更新活动窗。 */
        return WORKOUT_STATUS_ERR_ARGUMENT;
    }
    /* 六轴任一 NaN/Inf 都会污染模长、方差和长期门状态，因此整点拒绝。 */
    for (uint8_t axis = 0U; axis < FITNESS_IMU_AXIS_COUNT; ++axis) {
        /* isfinite=false 表示当前物理量非法。 */
        if (!isfinite(sample->axis[axis])) {
            /* 原活动窗保持不变。 */
            return WORKOUT_STATUS_ERR_ARGUMENT;
        }
    }
    /* 角速度模长输入 gx、gy、gz，单位 deg/s。 */
    const float gyro_magnitude_dps = sqrtf(
        (sample->axis[0] * sample->axis[0]) +
        (sample->axis[1] * sample->axis[1]) +
        (sample->axis[2] * sample->axis[2]));
    /* 加速度模长输入 ax、ay、az，单位 g。 */
    const float acceleration_magnitude_g = sqrtf(
        (sample->axis[3] * sample->axis[3]) +
        (sample->axis[4] * sample->axis[4]) +
        (sample->axis[5] * sample->axis[5]));
    /* 首个干净点没有前项，|Δa| 按训练端约定为零。 */
    float acceleration_delta_g = 0.0F;
    /* 有前一干净加速度时计算三轴向量差模，单位 g。 */
    if (engine->activity_previous_acceleration_valid) {
        /* 保存 ax 差值。 */
        const float delta_x_g =
            sample->axis[3] - engine->activity_previous_acceleration_g[0];
        /* 保存 ay 差值。 */
        const float delta_y_g =
            sample->axis[4] - engine->activity_previous_acceleration_g[1];
        /* 保存 az 差值。 */
        const float delta_z_g =
            sample->axis[5] - engine->activity_previous_acceleration_g[2];
        /* 计算 |Δa|，与 Python np.linalg.norm(diff(acc)) 一致。 */
        acceleration_delta_g = sqrtf(
            (delta_x_g * delta_x_g) +
            (delta_y_g * delta_y_g) +
            (delta_z_g * delta_z_g));
    }
    /* 保存当前加速度为下一干净点的差分基准，顺序固定 ax、ay、az。 */
    engine->activity_previous_acceleration_g[0] = sample->axis[3];
    /* 保存 ay，单位 g。 */
    engine->activity_previous_acceleration_g[1] = sample->axis[4];
    /* 保存 az，单位 g。 */
    engine->activity_previous_acceleration_g[2] = sample->axis[5];
    /* 后续点可以计算加速度向量差。 */
    engine->activity_previous_acceleration_valid = true;
    /* 逐点活动分数无量纲；200 deg/s 归一化与训练端完全相同。 */
    const float point_motion_score =
        acceleration_delta_g + (gyro_magnitude_dps / 200.0F);
    /* 严格大于训练阈值时记为活动点，平局按非活动处理。 */
    const uint8_t point_active =
        point_motion_score > ACTIVE_POINT_THRESHOLD ? 1U : 0U;
    /* 当前写槽在满窗时保存即将被移出的最旧活动标志。 */
    const uint8_t write_index = engine->activity_write_index;
    /* 满窗覆盖前先从 O(1) 活动计数中删除旧标志。 */
    if (engine->activity_sample_count == WORKOUT_ACTIVITY_WINDOW_SAMPLES) {
        /* 旧值仅为 0/1，不会使无符号计数下溢。 */
        engine->activity_active_point_count -=
            engine->activity_point_active[write_index];
    } else {
        /* 窗未满时新增一个有效槽。 */
        engine->activity_sample_count += 1U;
    }
    /* 写入当前加速度模长，单位 g。 */
    engine->activity_acceleration_magnitude_g[write_index] =
        acceleration_magnitude_g;
    /* 写入当前角速度模长，单位 deg/s。 */
    engine->activity_gyro_magnitude_dps[write_index] = gyro_magnitude_dps;
    /* 写入当前逐点活动标志。 */
    engine->activity_point_active[write_index] = point_active;
    /* O(1) 累加当前标志。 */
    engine->activity_active_point_count += point_active;
    /* 写指针循环前进，范围保持 0..24。 */
    engine->activity_write_index = (uint8_t)(
        (write_index + 1U) % WORKOUT_ACTIVITY_WINDOW_SAMPLES);
    /* 未满一秒时保留锁类或上次完整窗的门状态，避免启动期人为漏掉首个慢动作。 */
    if (engine->activity_sample_count < WORKOUT_ACTIVITY_WINDOW_SAMPLES) {
        /* 当前样本已安全写入。 */
        return WORKOUT_STATUS_OK;
    }
    /* 静坐是目标动作本身，不使用动态活动门冻结其持续时间。 */
    if (engine->selected_action == (uint8_t)FITNESS_ACTION_SIT) {
        /* 静坐会话始终允许 fitness_session_tick 产生整秒事件。 */
        engine->classification_consistent = true;
        /* 当前窗处理完成。 */
        return WORKOUT_STATUS_OK;
    }
    /* 分别累计 |a|、|a|²、|g|、|g|²，按总体方差复现 NumPy std(ddof=0)。 */
    float acceleration_sum_g = 0.0F;
    /* 加速度平方和单位 g²。 */
    float acceleration_square_sum_g2 = 0.0F;
    /* 角速度和单位 deg/s。 */
    float gyro_sum_dps = 0.0F;
    /* 角速度平方和单位 (deg/s)²。 */
    float gyro_square_sum_dps2 = 0.0F;
    /* 固定遍历完整 25 点窗，时间复杂度 O(25)。 */
    for (uint8_t index = 0U; index < WORKOUT_ACTIVITY_WINDOW_SAMPLES; ++index) {
        /* 读取一个加速度模长。 */
        const float acc_g = engine->activity_acceleration_magnitude_g[index];
        /* 读取一个角速度模长。 */
        const float gyro_dps = engine->activity_gyro_magnitude_dps[index];
        /* 累加加速度模长。 */
        acceleration_sum_g += acc_g;
        /* 累加加速度模长平方。 */
        acceleration_square_sum_g2 += acc_g * acc_g;
        /* 累加角速度模长。 */
        gyro_sum_dps += gyro_dps;
        /* 累加角速度模长平方。 */
        gyro_square_sum_dps2 += gyro_dps * gyro_dps;
    }
    /* 点数倒数固定为 1/25，避免运行时整数除法语义歧义。 */
    const float inverse_count = 1.0F / (float)WORKOUT_ACTIVITY_WINDOW_SAMPLES;
    /* 计算加速度模长总体均值。 */
    const float acceleration_mean_g = acceleration_sum_g * inverse_count;
    /* 计算角速度模长总体均值。 */
    const float gyro_mean_dps = gyro_sum_dps * inverse_count;
    /* 方差理论非负；浮点消减误差导致负小量时夹紧为零。 */
    float acceleration_variance_g2 =
        (acceleration_square_sum_g2 * inverse_count) -
        (acceleration_mean_g * acceleration_mean_g);
    /* 同样计算角速度总体方差。 */
    float gyro_variance_dps2 =
        (gyro_square_sum_dps2 * inverse_count) -
        (gyro_mean_dps * gyro_mean_dps);
    /* 夹紧加速度负零附近误差，避免 sqrtf 产生 NaN。 */
    if (acceleration_variance_g2 < 0.0F) {
        /* 常量窗方差定义为零。 */
        acceleration_variance_g2 = 0.0F;
    }
    /* 夹紧角速度负零附近误差。 */
    if (gyro_variance_dps2 < 0.0F) {
        /* 常量窗方差定义为零。 */
        gyro_variance_dps2 = 0.0F;
    }
    /* 组合活动分数与 Python motion_score 完全同式，无量纲。 */
    const float window_motion_score =
        sqrtf(acceleration_variance_g2) +
        (sqrtf(gyro_variance_dps2) / 200.0F);
    /* 双门同时满足才判为运动，抑制单个尖峰和纯静止姿态漂移。 */
    const bool motion_detected =
        (window_motion_score >= REST_MOTION_THRESHOLD) &&
        (engine->activity_active_point_count >=
         WORKOUT_ACTIVITY_MIN_ACTIVE_POINTS);
    /* 首次检测到运动后，后续完整静止窗才有权关闭计数。 */
    if (motion_detected) {
        /* 记录本轮已越过准备上下文并进入真实运动。 */
        engine->activity_gate_has_seen_motion = true;
    }
    /* 完整一秒窗没有任何逐点活动且整体低于静止门，才确认用户已经休息。 */
    const bool rest_detected =
        (window_motion_score < REST_MOTION_THRESHOLD) &&
        (engine->activity_active_point_count == 0U);
    /* 默认保持旧门状态，给 1～4 个边界活动点形成迟滞区，避免步峰附近反复开关。 */
    bool next_counting_enabled = engine->classification_consistent;
    /* 满足双活动门时立即打开或保持打开。 */
    if (motion_detected) {
        /* 真实运动恢复后允许推进当前主动作。 */
        next_counting_enabled = true;
    } else if (engine->activity_gate_has_seen_motion && rest_detected) {
        /* 只有已见过运动后的完整静止窗才能关闭，准备上下文不截断首周期。 */
        next_counting_enabled = false;
    }
    /* 只有门状态变化时清空一次半周期，禁止休息前后拼接成伪次数。 */
    if (next_counting_enabled != engine->classification_consistent) {
        /* 提交新的活动门状态。 */
        engine->classification_consistent = next_counting_enabled;
        /* 冻结和恢复两侧都从完整新周期开始。 */
        workout_reset_incomplete_counting(engine);
    }
    /* 当前活动窗更新成功。 */
    return WORKOUT_STATUS_OK;
}

/* 锁类后按时间顺序补算准备期样本；MetricEvent 进入会话累计和回放事件 FIFO。 */
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
        /* 复用正式运行计数链，保证相位、热量和事件规则完全一致。 */
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
    /* 有准备缓存时锁类阶段已经观察过原始时间线；后续完整静止窗必须能够关闭计数。 */
    if (engine->prelock_sample_count > 0U) {
        /* 缓存内若尚未形成完整活动窗，模型锁类仍作为已开始运动的补充证据。 */
        engine->activity_gate_has_seen_motion = true;
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
    /* 新动作从空活动窗开始，准备期补算将按真实样本重建一秒统计。 */
    workout_reset_activity_window(engine);
    /* 首次完整活动窗出现前只做滤波预热，禁止首动作前沿触发一次假冻结。 */
    engine->activity_gate_has_seen_motion = false;
    /* 锁类本身证明动作已开始；活动窗未满前先放行，避免慢动作首周期被人为截断。 */
    engine->classification_consistent = true;
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
    /* 锁类只建立会话和计数器，不产生任何硬件反馈。 */
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
    /* RUNNING 连续性边界只切断跨缺口周期，不再等待新的模型窗口。 */
    if (engine->state == WORKOUT_STATE_RUNNING) {
        /* 清空跨时间线的 25 点统计和加速度差分；当前门状态暂时保留。 */
        workout_reset_activity_window(engine);
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

    /* RUNNING 保持会话主动作和计数许可不变，模型窗口只进入诊断。 */
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
        /* 后续 Top-1、置信度和质量位只供 CSV/调试；活动门在逐样本入口独立更新。 */
        /* 诊断更新成功，selected_action、相位和计数许可均保持。 */
        return WORKOUT_STATUS_OK;
    }

    /*
     * 62 点窗口只有在双 M0 前向失败或旧版执行器污染时拒绝。
     * 时间线重置位属于窗口开始前的来源事实：imu_pipeline 已先清空环形区，再积满 62 个
     * 新连续点才推理，因此该位不能让一个完整模型窗再等待 2.48 秒。
     */
    if ((quality_flags &
         (WORKOUT_QUALITY_INFERENCE_FAILED |
          WORKOUT_QUALITY_LEGACY_ACTUATOR_CONTAMINATED)) != 0U) {
        /* 等待下一次成功且无旧执行器污染的完整窗口。 */
        return WORKOUT_STATUS_IGNORED;
    }

    /* 保存当前独立窗口最优类别；连续门不能被更早的强错误窗口拖住。 */
    uint8_t window_class = WORKOUT_ACTION_UNKNOWN;
    /* 保存当前独立窗口 0..65535 的 softmax 概率。 */
    uint16_t window_confidence_q15 = 0U;
    /* 对本窗执行有限值校验、Top-1 和数值稳定 softmax。 */
    const workout_status_t window_classify_status = workout_classify_single_window(
        logits,
        &window_class,
        &window_confidence_q15);
    /* 非有限 logits 或概率异常不能进入累计器或候选状态。 */
    if (window_classify_status != WORKOUT_STATUS_OK) {
        /* 传播精确错误。 */
        return window_classify_status;
    }
    /* 保存当前窗口加入后的 11 类动作段平均 logits，供第四窗有界兜底。 */
    float averaged_logits[WORKOUT_CLASS_COUNT];
    /* 累计器从会话起点纳入当前及历史窗口，只承担不稳定会话的最终兜底。 */
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
    const uint8_t cumulative_best_class = (uint8_t)inferred_class;
    /* 保存累计平均 logits 的 Q15 概率，只在第四窗兜底时公开。 */
    uint16_t cumulative_confidence_q15 = 0U;
    /* 对累计平均 logits 计算数值稳定 softmax，与冻结验证器兜底完全一致。 */
    const workout_status_t cumulative_confidence_status =
        workout_confidence_from_averaged_logits(
        averaged_logits,
        cumulative_best_class,
        &cumulative_confidence_q15);
    /* 计算异常直接返回。 */
    if (cumulative_confidence_status != WORKOUT_STATUS_OK) {
        /* 传播错误。 */
        return cumulative_confidence_status;
    }
    /* 每窗立即公开独立窗口候选，UI 可准确反映起手类别如何变化。 */
    engine->inferred_action = window_class;
    /* 正常确认阶段公开当前独立窗口概率。 */
    engine->confidence_q15 = window_confidence_q15;
    /* 低置信窗口切断连续证据，后续必须重新形成两个完整可信窗口。 */
    if (window_confidence_q15 < WORKOUT_ACTION_LOCK_CONFIDENCE_Q15) {
        /* 未知候选表示本窗不参与连续确认。 */
        engine->candidate_action = WORKOUT_ACTION_UNKNOWN;
        /* 清零连续可信窗口数。 */
        engine->candidate_windows = 0U;
    } else if (engine->candidate_action == window_class) {
        /* 独立窗口同类且仍过概率门时增加连续证据。 */
        /* 准备态最多四窗，uint8 不会溢出。 */
        engine->candidate_windows += 1U;
    } else {
        /* 保存新的高置信独立窗口候选类。 */
        engine->candidate_action = window_class;
        /* 当前窗口是该候选的第一份连续证据。 */
        engine->candidate_windows = 1U;
    }
    /* stable_and_confident 表示同一独立类别已连续两窗通过 50% 概率门。 */
    const bool stable_and_confident =
        engine->candidate_windows >= WORKOUT_ACTION_LOCK_WINDOWS;
    /* reached_bounded_limit 保证低置信会话最迟第四窗结束准备，不永久停留。 */
    const bool reached_bounded_limit =
        engine->bout_window_count >= WORKOUT_ACTION_MAX_PREPARE_WINDOWS;
    /* 正常门和有界兜底均未满足时继续累计下一重叠窗。 */
    if (!stable_and_confident && !reached_bounded_limit) {
        /* 当前已输出候选和置信度，但尚未提交 selected_action。 */
        return WORKOUT_STATUS_IGNORED;
    }
    /* 正常门锁定连续独立候选；不稳定第四窗才锁定累计平均 Top-1。 */
    const uint8_t lock_class =
        stable_and_confident ? engine->candidate_action : cumulative_best_class;
    /* 第四窗兜底时把公开诊断切换到真正提交的累计类别和概率。 */
    if (!stable_and_confident) {
        /* 公开最终累计兜底类别。 */
        engine->inferred_action = cumulative_best_class;
        /* 公开累计兜底概率，避免 UI 显示最后一个无效单窗的概率。 */
        engine->confidence_q15 = cumulative_confidence_q15;
    }
    /* 连续独立确认或第四窗累计兜底后初始化本次唯一动作并补算准备点。 */
    const workout_status_t lock_status = workout_lock_action(
        engine,
        (fitness_action_t)lock_class);
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
    /* sit 整秒事件已经产生，直接发布唯一指标事实。 */
    if (*emitted) {
        /* 返回成功。 */
        return WORKOUT_STATUS_OK;
    }
    /* 真实采样边界必须立刻清空未完成周期；已闭合次数和会话累计保持。 */
    if (!count_input_valid &&
        ((quality_flags & WORKOUT_QUALITY_TIMELINE_BREAK_MASK) != 0U)) {
        /* 缺口两侧不能共用 25 点方差或加速度差分，但当前活动/休息状态暂时保留。 */
        workout_reset_activity_window(engine);
        /* 当前边界点不使用幅值，只提交重置后的计数器状态。 */
        workout_reset_incomplete_counting(engine);
        /* 该点已安全消费，不让协调器回滚候选副本。 */
        return WORKOUT_STATUS_OK;
    }
    /* 其它污染点不进入活动统计或相位/步峰。 */
    if (!count_input_valid) {
        /* 热量已按锁定会话推进；真实时间缺口会由下一干净点按原 120 ms 合同处理。 */
        return WORKOUT_STATUS_OK;
    }
    /* 用当前干净点更新训练同源活动门；非法六轴值不允许污染长期状态。 */
    const workout_status_t activity_status = workout_update_activity_gate(
        engine,
        sample);
    /* 活动统计错误直接返回，当前点不得进入动作计数器。 */
    if (activity_status != WORKOUT_STATUS_OK) {
        /* 传播精确参数错误。 */
        return activity_status;
    }
    /* 完整 1 秒窗判为休息时只推进时间和热量，不推进相位/步峰。 */
    if (!engine->classification_consistent) {
        /* 主动作、累计值和诊断类别保持。 */
        return WORKOUT_STATUS_OK;
    }
    /* 所有重复类动作统一使用主腕角速度的“主向—回向—返回”完整周期，避免把一次开合跳的开、合冲击拆成两次。 */
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
        /* 推进统一主向、回向、闭合计数器。 */
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
        /* 推进步峰去重器。 */
        const fitness_status_t step_status = fitness_step_counter_accept(
            &engine->step_counter,
            sample->monotonic_ms,
            &step_accepted);
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
        /* 不重复修改时间。 */
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
    /* 暂停统一清空主相位、重复候选和步峰历史。 */
    workout_reset_incomplete_counting(engine);
    /* 暂停结束当前活动统计；恢复后由新的 25 Hz 样本重建，不依赖模型窗口。 */
    workout_reset_bout(engine);
    /* 清空暂停前活动窗，防止暂停两侧样本进入同一个一秒方差。 */
    workout_reset_activity_window(engine);
    /* 切换到暂停状态。 */
    engine->state = WORKOUT_STATE_PAUSED;
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
        /* 不重复保存。 */
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
    /* 重新初始化为空闲并清空会话状态。 */
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
    /* 复制训练同源活动门状态，供上层显示训练或休息提示。 */
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
