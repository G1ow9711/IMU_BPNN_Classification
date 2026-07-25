/* 引入动作相位公共合同，保证实现阈值和状态字段与调用方一致。 */
#include "motion_phase.h"

/* 引入 isfinite、fabsf 和 sqrtf，执行有限值检查、模长和归一化。 */
#include <math.h>
/* 引入 memset，确定性清空静态状态。 */
#include <string.h>

/* 两相位内部阶段 0：等待首次明显手腕旋转并学习方向。 */
#define MOTION_TWO_WAIT_PRIMARY (0U)
/* 两相位内部阶段 1：已看到主向旋转，等待反向回摆。 */
#define MOTION_TWO_WAIT_SECONDARY (1U)
/* 两相位内部阶段 2：已看到反向峰，等待返回正峰或稳定基线闭合周期。 */
#define MOTION_TWO_WAIT_REST (2U)
/* 真实连续端点候选 0：当前没有待确认端点。 */
#define MOTION_TRANSITION_NONE (0U)
/* 真实连续端点候选 1：等待第二个 PRIMARY 原始点。 */
#define MOTION_TRANSITION_PRIMARY (1U)
/* 真实连续端点候选 2：等待第二个 SECONDARY 原始点。 */
#define MOTION_TRANSITION_SECONDARY (2U)
/* 真实连续端点候选 3：连续动作已返回主向端点。 */
#define MOTION_TRANSITION_CONTINUOUS_REST (3U)
/* 真实连续端点候选 4：慢动作已回到低运动稳定点。 */
#define MOTION_TRANSITION_STATIC_REST (4U)

/* 步峰慢基线的一阶更新系数；25 Hz 下时间常数约 2 秒。 */
#define MOTION_STEP_BASELINE_ALPHA (0.02F)
/* 相邻极值类型 0 表示尚无候选。 */
#define MOTION_PERIODIC_EXTREMUM_NONE (0U)
/* 相邻极值类型 1 表示局部波峰。 */
#define MOTION_PERIODIC_EXTREMUM_PEAK (1U)
/* 相邻极值类型 2 表示局部波谷。 */
#define MOTION_PERIODIC_EXTREMUM_VALLEY (2U)

/* 判断动作是否使用 walk/trot 局部冲击峰。 */
static bool motion_action_is_step(const fitness_action_t action)
{
    /* 只有 walk 和 trot 输出 step_peak。 */
    return (action == FITNESS_ACTION_WALK) || (action == FITNESS_ACTION_TROT);
}

/* 判断动作是否使用主向/回向两相位。 */
static bool motion_action_is_two_phase(const fitness_action_t action)
{
    /* 八类重复动作都按手腕主向、回向和闭合周期计数；腕部不可靠提供脚底支持力五阶段。 */
    return (action == FITNESS_ACTION_GOOD_MORNING) ||
           (action == FITNESS_ACTION_JUMPING_JACK) ||
           (action == FITNESS_ACTION_JUMPING_LUNGE) ||
           (action == FITNESS_ACTION_JUMPING_SQUAT) ||
           (action == FITNESS_ACTION_LUNGE) ||
           (action == FITNESS_ACTION_SQUAT) ||
           (action == FITNESS_ACTION_TUCK_JUMP) ||
           (action == FITNESS_ACTION_WAVE);
}

/* 计算三维向量欧氏模长；输入与输出单位保持不变。 */
static float motion_vector_magnitude(const float x, const float y, const float z)
{
    /* 使用 float 运算减少 ESP32 软双精度开销。 */
    return sqrtf((x * x) + (y * y) + (z * z));
}

/* 检查六轴样本是否全部为有限值，拒绝 NaN/Inf 污染状态。 */
static bool motion_sample_is_finite(const motion_phase_sample_t *sample)
{
    /* 遍历固定六轴；任一非法值使整个点失效。 */
    for (size_t axis = 0U; axis < FITNESS_IMU_AXIS_COUNT; ++axis) {
        /* isfinite 为 false 表示 NaN 或正负无穷。 */
        if (!isfinite(sample->axis[axis])) {
            /* 非有限样本不能进入基线和方向学习。 */
            return false;
        }
    }
    /* 六轴全部有限。 */
    return true;
}

/* 用当前模长填充诊断字段，并把业务输出初始化为“无新事件”。 */
static void motion_observation_init(
    motion_phase_observation_t *observation,
    const float acceleration_magnitude_g,
    const float gyro_magnitude_dps)
{
    /* 默认 REST 只是安全枚举值，phase_valid=false 表示不得推进重复计数器。 */
    observation->phase = FITNESS_PHASE_REST;
    /* 默认没有稳定相位事件。 */
    observation->phase_valid = false;
    /* 默认没有步峰。 */
    observation->step_peak = false;
    /* 保存当前加速度模长，单位 g。 */
    observation->acceleration_magnitude_g = acceleration_magnitude_g;
    /* 保存当前角速度模长，单位 deg/s。 */
    observation->gyro_magnitude_dps = gyro_magnitude_dps;
}

/* 发布一个离散相位，并让下一采样重复一次以满足连续两点合同。 */
static void motion_emit_phase(
    motion_phase_detector_t *detector,
    const fitness_motion_phase_t phase,
    motion_phase_observation_t *observation)
{
    /* 当前输出立即携带事件相位。 */
    observation->phase = phase;
    /* 标记可送入 fitness_rep_counter_update。 */
    observation->phase_valid = true;
    /* 保存需要重复输出的相位。 */
    detector->held_phase = phase;
    /* 当前点已输出一次，因此只需在下一点再输出一次。 */
    detector->hold_remaining = MOTION_PHASE_EVENT_HOLD_SAMPLES - 1U;
}

/* 判断当前点是否接近 1 g 且手腕角速度较低。 */
static bool motion_is_rest(
    const float acceleration_magnitude_g,
    const float gyro_magnitude_dps)
{
    /* 同时满足支持力接近 1 g 和角速度较小，避免在缓慢落地途中提前闭合。 */
    return (fabsf(acceleration_magnitude_g - 1.0F) <= MOTION_PHASE_REST_ACCEL_TOLERANCE_G) &&
           (gyro_magnitude_dps <= MOTION_PHASE_REST_GYRO_DPS);
}

/* 把 value 限制到 [minimum,maximum]，避免自适应门被极端峰值带出有效范围。 */
static float motion_clamp_float(
    const float value,
    const float minimum,
    const float maximum)
{
    /* 低于下限时返回下限。 */
    if (value < minimum) {
        /* 返回最低允许值。 */
        return minimum;
    }
    /* 高于上限时返回上限。 */
    if (value > maximum) {
        /* 返回最高允许值。 */
        return maximum;
    }
    /* 合法范围内保持原值。 */
    return value;
}

/* 清空尚未达到两个真实连续点的端点候选，不改变已确认方向和动作阶段。 */
static void motion_reset_transition_candidate(motion_phase_detector_t *detector)
{
    /* 候选类型恢复为空。 */
    detector->transition_candidate = MOTION_TRANSITION_NONE;
    /* 连续点数恢复为零。 */
    detector->transition_candidate_count = 0U;
}

/*
 * 用两个真实连续 25 Hz 点确认离散端点。
 * 首点只缓存候选；第二个同类点才发布相位，随后沿用旧接口再保持一次，供领域层稳定消费。
 */
static bool motion_confirm_transition(
    motion_phase_detector_t *detector,
    const uint8_t transition,
    const fitness_motion_phase_t phase,
    const bool condition,
    motion_phase_observation_t *observation)
{
    /* 条件中断时立即丢弃单点候选，禁止跨噪声或静止拼接。 */
    if (!condition) {
        /* 清空不完整端点。 */
        motion_reset_transition_candidate(detector);
        /* 当前没有确认相位。 */
        return false;
    }
    /* 新端点类型从连续点 1 开始，不能继承上一类型的点数。 */
    if (detector->transition_candidate != transition) {
        /* 保存当前端点类型。 */
        detector->transition_candidate = transition;
        /* 当前样本是第一个真实点。 */
        detector->transition_candidate_count = 1U;
        /* 尚未达到两个连续点。 */
        return false;
    }
    /* 同类候选未满上限时增加真实连续点数。 */
    if (detector->transition_candidate_count < MOTION_PHASE_TRANSITION_CONFIRM_SAMPLES) {
        /* 累计第二个真实点。 */
        detector->transition_candidate_count += 1U;
    }
    /* 少于两个点时继续等待。 */
    if (detector->transition_candidate_count < MOTION_PHASE_TRANSITION_CONFIRM_SAMPLES) {
        /* 当前仍未确认。 */
        return false;
    }
    /* 端点已经由真实信号确认，清空候选供下一阶段使用。 */
    motion_reset_transition_candidate(detector);
    /* 发布一次相位并保持一个接口点，让领域层的两次稳定相位防线继续生效。 */
    motion_emit_phase(detector, phase, observation);
    /* 告知调用方可以切换内部阶段。 */
    return true;
}

/* 计算六轴样本当前角速度在已确认主方向上的有符号投影，单位 deg/s。 */
static float motion_project_gyro(
    const motion_phase_detector_t *detector,
    const motion_phase_sample_t *sample)
{
    /* 三轴点积保持正负方向；单位向量保证结果仍是 deg/s。 */
    return (sample->axis[0] * detector->learned_gyro_direction[0]) +
           (sample->axis[1] * detector->learned_gyro_direction[1]) +
           (sample->axis[2] * detector->learned_gyro_direction[2]);
}

/* 根据活动段角速度包络生成幅度自适应主向/回向迟滞门，单位 deg/s。 */
static float motion_active_direction_threshold(const motion_phase_detector_t *detector)
{
    /* 未建立包络时使用最低门；正常路径会在方向候选首点写入包络。 */
    const float scaled_threshold =
        detector->motion_scale_dps * MOTION_PHASE_DIRECTION_ACTIVE_RATIO;
    /* 上下限共同避免小动作落入静止噪声，也避免猛烈峰值锁死后续动作。 */
    return motion_clamp_float(
        scaled_threshold,
        MOTION_PHASE_DIRECTION_ACTIVE_MIN_DPS,
        MOTION_PHASE_DIRECTION_ACTIVE_MAX_DPS);
}

/*
 * 用与当前轴夹角较小的运动缓慢更新方向和幅度包络。
 * 负向样本先翻转到主方向半球，因此只改变轴线、不颠倒 PRIMARY/SECONDARY 语义。
 */
static void motion_adapt_direction(
    motion_phase_detector_t *detector,
    const motion_phase_sample_t *sample,
    const float gyro_magnitude_dps)
{
    /* 低于方向学习下限的点只属于回程或静止，不参与方向和幅度学习。 */
    if (gyro_magnitude_dps < MOTION_PHASE_DIRECTION_LEARN_DPS) {
        /* 保持当前模型。 */
        return;
    }
    /* projected_dps 是当前方向上的有符号角速度。 */
    const float projected_dps = motion_project_gyro(detector, sample);
    /* alignment 是夹角余弦绝对值，范围理论上 [0,1]。 */
    const float alignment = fabsf(projected_dps) / gyro_magnitude_dps;
    /* 交叉轴分量过大时视为噪声或过渡，不让它旋转主轴。 */
    if (alignment < MOTION_PHASE_DIRECTION_ALIGNMENT_MIN) {
        /* 保持当前方向和包络。 */
        return;
    }
    /* 高于旧包络时快速上升，低于旧包络时缓慢下降以适应疲劳。 */
    const float scale_alpha =
        (gyro_magnitude_dps > detector->motion_scale_dps) ?
        MOTION_PHASE_SCALE_RISE_ALPHA : MOTION_PHASE_SCALE_FALL_ALPHA;
    /* 指数更新后的包络仍以 deg/s 表示。 */
    detector->motion_scale_dps +=
        scale_alpha * (gyro_magnitude_dps - detector->motion_scale_dps);
    /* sign 把负向样本翻转到当前主方向半球，防止方向更新 180 度反转。 */
    const float sign = (projected_dps >= 0.0F) ? 1.0F : -1.0F;
    /* blended_x 混合旧单位轴和当前同半球单位样本。 */
    const float blended_x =
        ((1.0F - MOTION_PHASE_DIRECTION_ADAPT_ALPHA) * detector->learned_gyro_direction[0]) +
        (MOTION_PHASE_DIRECTION_ADAPT_ALPHA * sign * sample->axis[0] / gyro_magnitude_dps);
    /* blended_y 混合 y 分量。 */
    const float blended_y =
        ((1.0F - MOTION_PHASE_DIRECTION_ADAPT_ALPHA) * detector->learned_gyro_direction[1]) +
        (MOTION_PHASE_DIRECTION_ADAPT_ALPHA * sign * sample->axis[1] / gyro_magnitude_dps);
    /* blended_z 混合 z 分量。 */
    const float blended_z =
        ((1.0F - MOTION_PHASE_DIRECTION_ADAPT_ALPHA) * detector->learned_gyro_direction[2]) +
        (MOTION_PHASE_DIRECTION_ADAPT_ALPHA * sign * sample->axis[2] / gyro_magnitude_dps);
    /* blended_norm 理论上大于零；夹角门保证不会把两个相反单位向量直接抵消。 */
    const float blended_norm = motion_vector_magnitude(blended_x, blended_y, blended_z);
    /* 仅在有限正模长下写回，防止除零或 NaN 污染会话。 */
    if (isfinite(blended_norm) && (blended_norm > 1.0e-6F)) {
        /* 写回归一化 x 分量。 */
        detector->learned_gyro_direction[0] = blended_x / blended_norm;
        /* 写回归一化 y 分量。 */
        detector->learned_gyro_direction[1] = blended_y / blended_norm;
        /* 写回归一化 z 分量。 */
        detector->learned_gyro_direction[2] = blended_z / blended_norm;
    }
}

/* 处理八类重复动作的主向、回向和稳定闭合；不要求逐点匹配标准动作模板。 */
static void motion_process_two_phase(
    motion_phase_detector_t *detector,
    const motion_phase_sample_t *sample,
    const float acceleration_magnitude_g,
    const float gyro_magnitude_dps,
    motion_phase_observation_t *observation)
{
    /* 初次活动尚无确认轴时，用首个明显旋转保存临时单位方向。 */
    if (!detector->direction_valid) {
        /* 没有候选且当前角速度足够时建立第一个 PRIMARY 原始点。 */
        if ((detector->transition_candidate == MOTION_TRANSITION_NONE) &&
            (gyro_magnitude_dps >= MOTION_PHASE_DIRECTION_LEARN_DPS)) {
            /* 临时 x 单位分量。 */
            detector->learned_gyro_direction[0] = sample->axis[0] / gyro_magnitude_dps;
            /* 临时 y 单位分量。 */
            detector->learned_gyro_direction[1] = sample->axis[1] / gyro_magnitude_dps;
            /* 临时 z 单位分量。 */
            detector->learned_gyro_direction[2] = sample->axis[2] / gyro_magnitude_dps;
            /* 首点同时建立该活动段角速度包络。 */
            detector->motion_scale_dps = gyro_magnitude_dps;
            /* 保存 PRIMARY 候选类型。 */
            detector->transition_candidate = MOTION_TRANSITION_PRIMARY;
            /* 当前只是第一个真实点，不向领域层发布。 */
            detector->transition_candidate_count = 1U;
            /* 等待下一原始点确认同方向。 */
            return;
        }
        /* 已有临时 PRIMARY 候选时，用第二个点在临时轴上的投影检查连续性。 */
        if (detector->transition_candidate == MOTION_TRANSITION_PRIMARY) {
            /* 临时投影单位为 deg/s。 */
            const float provisional_projection = motion_project_gyro(detector, sample);
            /* 临时门同样按首点包络缩放。 */
            const float provisional_threshold = motion_active_direction_threshold(detector);
            /* 第二点必须越过正门且本身达到最低学习幅值。 */
            const bool primary_confirmed =
                (gyro_magnitude_dps >= MOTION_PHASE_DIRECTION_LEARN_DPS) &&
                (provisional_projection >= provisional_threshold);
            /* 两个真实连续点成立时发布 PRIMARY。 */
            if (motion_confirm_transition(
                    detector,
                    MOTION_TRANSITION_PRIMARY,
                    FITNESS_PHASE_PRIMARY,
                    primary_confirmed,
                    observation)) {
                /* 方向从此成为活动段模型。 */
                detector->direction_valid = true;
                /* 允许第二点微调包络和方向。 */
                motion_adapt_direction(detector, sample, gyro_magnitude_dps);
                /* 下一阶段等待反向端点。 */
                detector->stage = MOTION_TWO_WAIT_SECONDARY;
                /* 当前点已确认并发布。 */
                return;
            }
            /* 连续性失败会清空候选；下一显著点必须重新学习方向。 */
            if (!primary_confirmed) {
                /* 清除临时幅度包络，防止失败尖峰影响下一候选。 */
                detector->motion_scale_dps = 0.0F;
            }
        }
        /* 未确认方向时不得检查其它阶段。 */
        return;
    }

    /* 用可信同轴样本缓慢更新方向和幅度包络。 */
    motion_adapt_direction(detector, sample, gyro_magnitude_dps);
    /* 更新后的投影单位为 deg/s；负值表示反向回摆。 */
    const float projected_dps = motion_project_gyro(detector, sample);
    /* 当前活动段的幅度自适应门。 */
    const float active_threshold_dps = motion_active_direction_threshold(detector);
    /* 等待主向阶段必须有两个真实连续正投影点。 */
    if (detector->stage == MOTION_TWO_WAIT_PRIMARY) {
        /* primary_condition 排除单点尖峰和低于自适应门的细小抖动。 */
        const bool primary_condition = projected_dps >= active_threshold_dps;
        /* 确认后进入反向等待。 */
        if (motion_confirm_transition(
                detector,
                MOTION_TRANSITION_PRIMARY,
                FITNESS_PHASE_PRIMARY,
                primary_condition,
                observation)) {
            /* 下一阶段等待 SECONDARY。 */
            detector->stage = MOTION_TWO_WAIT_SECONDARY;
        }
        /* 当前阶段只处理主向。 */
        return;
    }
    /* 等待回向阶段必须有两个真实连续负投影点。 */
    if (detector->stage == MOTION_TWO_WAIT_SECONDARY) {
        /* secondary_condition 使用与主向相同绝对幅度门，消除个人动作尺度偏差。 */
        const bool secondary_condition = projected_dps <= -active_threshold_dps;
        /* 确认后进入闭合等待。 */
        if (motion_confirm_transition(
                detector,
                MOTION_TRANSITION_SECONDARY,
                FITNESS_PHASE_SECONDARY,
                secondary_condition,
                observation)) {
            /* 下一阶段等待回到主向或稳定基线。 */
            detector->stage = MOTION_TWO_WAIT_REST;
        }
        /* 当前阶段只处理回向。 */
        return;
    }
    /* 连续动作回到主向时，优先按正投影闭合并把该端点作为下一周期起点。 */
    if (projected_dps >= active_threshold_dps) {
        /* 两个连续返回点确认完整正反周期。 */
        if (motion_confirm_transition(
                detector,
                MOTION_TRANSITION_CONTINUOUS_REST,
                FITNESS_PHASE_REST,
                true,
                observation)) {
            /* 当前主向端点同时开始下一周期，直接等待下一 SECONDARY。 */
            detector->stage = MOTION_TWO_WAIT_SECONDARY;
        }
        /* 正投影候选期间不能同时累计静止候选。 */
        return;
    }
    /* 慢动作在回程后停住时，两个真实稳定点也可闭合一次往返周期。 */
    const bool static_rest = motion_is_rest(acceleration_magnitude_g, gyro_magnitude_dps);
    /* 静止闭合与连续正峰使用不同候选类型，禁止一动一静拼成两个连续点。 */
    if (motion_confirm_transition(
            detector,
            MOTION_TRANSITION_STATIC_REST,
            FITNESS_PHASE_REST,
            static_rest,
            observation)) {
        /* 静止没有提供下一周期主端点，恢复等待显式 PRIMARY。 */
        detector->stage = MOTION_TWO_WAIT_PRIMARY;
    }
}

/* 处理 walk/trot 三点局部冲击峰；高门触发、低门重武装，避免慢速宽峰重复输出。 */
static void motion_process_step(
    motion_phase_detector_t *detector,
    const float acceleration_magnitude_g,
    motion_phase_observation_t *observation)
{
    /* 首点直接建立慢基线，避免把设备刚启动的 1 g 当成一步。 */
    if (detector->step_history_count == 0U) {
        /* 保存当前支持力作为初始基线。 */
        detector->acceleration_baseline_g = acceleration_magnitude_g;
    } else {
        /* 用一阶指数平均缓慢跟踪佩戴静态偏差，不跟随单次脚步冲击。 */
        detector->acceleration_baseline_g +=
            MOTION_STEP_BASELINE_ALPHA *
            (acceleration_magnitude_g - detector->acceleration_baseline_g);
    }
    /* 去除慢基线后得到脚步动态冲击，单位 g。 */
    const float dynamic_acceleration_g =
        acceleration_magnitude_g - detector->acceleration_baseline_g;
    /* 当前动态量已经回落到低门时重新武装；负值同样表示冲击已结束。 */
    if (dynamic_acceleration_g <= MOTION_PHASE_STEP_REARM_DELTA_G) {
        /* 下一次独立高门局部峰可以公开一个新步事件。 */
        detector->step_peak_armed = true;
    }
    /* 已有前两个点时，检查中间点是否高于两侧且超过 0.16 g。 */
    if (detector->step_history_ready) {
        /* 中间点严格高于左侧且不低于右侧，避免平台峰重复输出。 */
        const bool local_peak =
            (detector->previous_dynamic_acceleration_g >
             detector->previous_previous_dynamic_acceleration_g) &&
            (detector->previous_dynamic_acceleration_g >= dynamic_acceleration_g);
        /* 只有低门回落后已重新武装，且局部峰达到高门时才发布一个步峰。 */
        observation->step_peak =
            detector->step_peak_armed &&
            local_peak &&
            (detector->previous_dynamic_acceleration_g >= MOTION_PHASE_STEP_PEAK_DELTA_G);
        /* 接受高门峰后立即解除武装；宽峰内后续波瓣必须等真实回落后才能再触发。 */
        if (observation->step_peak) {
            /* 锁住当前物理步，防止单靠固定毫秒不应期依赖人的步速。 */
            detector->step_peak_armed = false;
        }
    }
    /* 左移三点历史：上一个点成为上上点。 */
    detector->previous_previous_dynamic_acceleration_g =
        detector->previous_dynamic_acceleration_g;
    /* 当前点成为下一轮的上一个点。 */
    detector->previous_dynamic_acceleration_g = dynamic_acceleration_g;
    /* 计数未满 2 时增加历史点数。 */
    if (detector->step_history_count < 2U) {
        /* 递增到 2 后即可检测中间点。 */
        detector->step_history_count += 1U;
    }
    /* 两个历史点齐备后保持 true。 */
    detector->step_history_ready = detector->step_history_count >= 2U;
}

/* 初始化指定长度的流式均值滤波器；长度只允许 5 或 11。 */
static void motion_periodic_mean_init(
    motion_periodic_mean_filter_t *filter,
    const uint8_t length)
{
    /* 调用方只传静态对象；空指针由上层 API 拦截。 */
    (void)memset(filter, 0, sizeof(*filter));
    /* 保存本级窗口长度，后续循环写入范围固定。 */
    filter->length = length;
}

/*
 * 推进一级均值滤波。
 * 启动未满时复现 StepCounter：第 1、3、5...个点输出当前奇数窗均值；满窗后每点输出。
 */
static bool motion_periodic_mean_push(
    motion_periodic_mean_filter_t *filter,
    const float input_g,
    float *output_g)
{
    /* 窗口未满时把新点顺序追加到静态数组。 */
    if (filter->count < filter->length) {
        /* 当前 count 同时是尚未写入的数组下标。 */
        filter->buffer[filter->count] = input_g;
        /* 加入当前加速度，单位保持 g。 */
        filter->sum_g += input_g;
        /* 有效点数增加一。 */
        filter->count += 1U;
        /* 满窗后下一点从最旧的下标 0 开始覆盖。 */
        if (filter->count == filter->length) {
            /* 写指针归零，形成定长环。 */
            filter->write_index = 0U;
        }
        /* 奇数个启动点才输出居中均值，与原 MeanFilter 行为一致。 */
        if ((filter->count & 1U) == 0U) {
            /* 偶数启动点只填充历史，不产生级联输出。 */
            return false;
        }
        /* 用 float 除法输出当前奇数窗口均值，输入和输出单位均为 g。 */
        *output_g = filter->sum_g / (float)filter->count;
        /* 告知下一级当前均值有效。 */
        return true;
    }
    /* 保存即将被覆盖的最旧点，单位 g。 */
    const float removed_g = filter->buffer[filter->write_index];
    /* 用新点覆盖环形数组当前位置。 */
    filter->buffer[filter->write_index] = input_g;
    /* 常数时间更新窗口总和，避免每点遍历 11 个元素。 */
    filter->sum_g += input_g - removed_g;
    /* 写指针循环移动到下一最旧点。 */
    filter->write_index = (uint8_t)((filter->write_index + 1U) % filter->length);
    /* 满窗后每个输入都输出固定长度均值。 */
    *output_g = filter->sum_g / (float)filter->length;
    /* 输出有效。 */
    return true;
}

/* 保存或合并一个局部极值，并在异类极值满足幅值和时间约束时形成一对。 */
static void motion_periodic_consume_extremum(
    motion_periodic_pair_detector_t *detector,
    const uint8_t extremum_kind,
    const uint32_t extremum_sequence,
    const float extremum_value_g,
    bool *pair_accepted)
{
    /* 没有待配端点时，当前峰或谷成为本组第一个端点。 */
    if (!detector->pending_valid) {
        /* 标记待配端点有效。 */
        detector->pending_valid = true;
        /* 保存峰/谷类型。 */
        detector->pending_kind = extremum_kind;
        /* 保存端点序号，单位滤波点。 */
        detector->pending_sequence = extremum_sequence;
        /* 保存端点加速度，单位 g。 */
        detector->pending_value_g = extremum_value_g;
        /* 首个端点不能单独构成动作。 */
        return;
    }
    /* 连续两个同类极值不能配对，只保留更高峰或更低谷。 */
    if (detector->pending_kind == extremum_kind) {
        /* 峰取较高值，谷取较低值，压缩局部平台和次级波纹。 */
        const bool current_is_stronger =
            ((extremum_kind == MOTION_PERIODIC_EXTREMUM_PEAK) &&
             (extremum_value_g > detector->pending_value_g)) ||
            ((extremum_kind == MOTION_PERIODIC_EXTREMUM_VALLEY) &&
             (extremum_value_g < detector->pending_value_g));
        /* 当前极值更显著时替换旧端点。 */
        if (current_is_stronger) {
            /* 更新端点序号。 */
            detector->pending_sequence = extremum_sequence;
            /* 更新端点值。 */
            detector->pending_value_g = extremum_value_g;
        }
        /* 同类事件处理结束。 */
        return;
    }
    /* 当前序号严格晚于待配端点，差值就是半周期点数。 */
    const uint32_t half_cycle_samples =
        extremum_sequence - detector->pending_sequence;
    /* peak_g 按事件类型选择更高端点，单位 g。 */
    const float peak_g =
        (detector->pending_kind == MOTION_PERIODIC_EXTREMUM_PEAK) ?
        detector->pending_value_g : extremum_value_g;
    /* valley_g 选择更低端点，单位 g。 */
    const float valley_g =
        (detector->pending_kind == MOTION_PERIODIC_EXTREMUM_VALLEY) ?
        detector->pending_value_g : extremum_value_g;
    /* amplitude_g 是峰减谷的正幅值，单位 g。 */
    const float amplitude_g = peak_g - valley_g;
    /* 太短或幅值不足属于毛刺；当前反向端点成为下一组起点。 */
    if ((half_cycle_samples < MOTION_PERIODIC_MIN_HALF_CYCLE_SAMPLES) ||
        (amplitude_g <= MOTION_PERIODIC_MIN_AMPLITUDE_G)) {
        /* 保存当前端点类型。 */
        detector->pending_kind = extremum_kind;
        /* 保存当前端点序号。 */
        detector->pending_sequence = extremum_sequence;
        /* 保存当前端点值。 */
        detector->pending_value_g = extremum_value_g;
        /* 当前毛刺不计数。 */
        return;
    }
    /* 间隔超过 1.60 秒属于慢姿态变化；丢弃旧端点并从当前端点重启。 */
    if (half_cycle_samples > MOTION_PERIODIC_MAX_HALF_CYCLE_SAMPLES) {
        /* 保存当前端点类型。 */
        detector->pending_kind = extremum_kind;
        /* 保存当前端点序号。 */
        detector->pending_sequence = extremum_sequence;
        /* 保存当前端点值。 */
        detector->pending_value_g = extremum_value_g;
        /* 当前慢漂移不计数。 */
        return;
    }
    /* 一峰一谷已经消费，无论是否落入不应期都不能复用其中任一端点。 */
    detector->pending_valid = false;
    /* 13 点内第二组峰谷通常是同一开合跳内部振铃，不重复累计。 */
    if (detector->has_last_pair &&
        ((extremum_sequence - detector->last_pair_sequence) <
         MOTION_PERIODIC_AXIS_REFRACTORY_SAMPLES)) {
        /* 当前峰谷对被单轴不应期拒绝。 */
        return;
    }
    /* 记录本次峰谷完成序号，供下一候选计算间隔。 */
    detector->last_pair_sequence = extremum_sequence;
    /* 标记已经存在可比较的上一配对。 */
    detector->has_last_pair = true;
    /* 单轴累计增加一次；峰谷一对就是一次动作，不再乘二。 */
    detector->total_pairs += 1ULL;
    /* 告知调用方当前点产生一个新配对。 */
    *pair_accepted = true;
}

/* 把一个双均值输出送入三点局部极值检测。 */
static void motion_periodic_push_filtered(
    motion_periodic_pair_detector_t *detector,
    const float filtered_g,
    bool *pair_accepted)
{
    /* 首个滤波点只保存为上上点。 */
    if (detector->filtered_history_count == 0U) {
        /* 保存首点，单位 g。 */
        detector->previous_previous_g = filtered_g;
        /* 历史点数变为 1。 */
        detector->filtered_history_count = 1U;
        /* 输出序号推进一。 */
        detector->filtered_sequence += 1U;
        /* 一个点不能判定极值。 */
        return;
    }
    /* 第二个滤波点只保存为上一点。 */
    if (detector->filtered_history_count == 1U) {
        /* 保存第二点，单位 g。 */
        detector->previous_g = filtered_g;
        /* 两点历史已经齐备。 */
        detector->filtered_history_count = 2U;
        /* 输出序号推进一。 */
        detector->filtered_sequence += 1U;
        /* 仍缺右邻点，不能判定中间极值。 */
        return;
    }
    /* 中间点不低于左邻且严格高于右邻时确认平台末端波峰。 */
    const bool local_peak =
        (detector->previous_g >= detector->previous_previous_g) &&
        (detector->previous_g > filtered_g);
    /* 中间点不高于左邻且严格低于右邻时确认平台末端波谷。 */
    const bool local_valley =
        (detector->previous_g <= detector->previous_previous_g) &&
        (detector->previous_g < filtered_g);
    /* 中间点序号恒为当前输出序号减一。 */
    const uint32_t extremum_sequence = detector->filtered_sequence - 1U;
    /* 局部峰进入无方向相邻峰谷配对。 */
    if (local_peak) {
        /* 峰值单位 g。 */
        motion_periodic_consume_extremum(
            detector,
            MOTION_PERIODIC_EXTREMUM_PEAK,
            extremum_sequence,
            detector->previous_g,
            pair_accepted);
    }
    /* 局部谷进入同一配对器；平坦点不会同时满足峰和谷。 */
    if (local_valley) {
        /* 谷值单位 g。 */
        motion_periodic_consume_extremum(
            detector,
            MOTION_PERIODIC_EXTREMUM_VALLEY,
            extremum_sequence,
            detector->previous_g,
            pair_accepted);
    }
    /* 左移历史：上一点成为上上点。 */
    detector->previous_previous_g = detector->previous_g;
    /* 当前滤波值成为下一轮中间点。 */
    detector->previous_g = filtered_g;
    /* 输出序号推进一。 */
    detector->filtered_sequence += 1U;
}

motion_phase_status_t motion_periodic_pair_init(
    motion_periodic_pair_detector_t *detector)
{
    /* 检测器地址必须有效。 */
    if (detector == NULL) {
        /* 空地址无法保存静态状态。 */
        return MOTION_PHASE_ERR_ARGUMENT;
    }
    /* 清空旧滤波、极值、累计和时间线。 */
    (void)memset(detector, 0, sizeof(*detector));
    /* 建立 11 点第一级均值。 */
    motion_periodic_mean_init(
        &detector->long_filter,
        MOTION_PERIODIC_MEAN_LONG_LENGTH);
    /* 建立 5 点第二级均值。 */
    motion_periodic_mean_init(
        &detector->short_filter,
        MOTION_PERIODIC_MEAN_SHORT_LENGTH);
    /* 标记初始化完成。 */
    detector->initialized = true;
    /* 返回成功。 */
    return MOTION_PHASE_OK;
}

void motion_periodic_pair_reset_cycle(
    motion_periodic_pair_detector_t *detector)
{
    /* 空对象或未初始化对象不处理。 */
    if ((detector == NULL) || !detector->initialized) {
        /* 安全返回。 */
        return;
    }
    /* 暂存已完成峰谷总数，重置不能回滚权威轴累计。 */
    const uint64_t total_pairs = detector->total_pairs;
    /* 清空全部滤波、极值、不应期和时间线。 */
    (void)motion_periodic_pair_init(detector);
    /* 恢复已完成累计。 */
    detector->total_pairs = total_pairs;
}

motion_phase_status_t motion_periodic_pair_push(
    motion_periodic_pair_detector_t *detector,
    const uint64_t monotonic_ms,
    const float acceleration_g,
    bool *pair_accepted)
{
    /* 检测器、输出地址必须有效，输入加速度必须为有限 g 值。 */
    if ((detector == NULL) || (pair_accepted == NULL) ||
        !detector->initialized || !isfinite(acceleration_g)) {
        /* 非法输入不修改内部状态。 */
        return MOTION_PHASE_ERR_ARGUMENT;
    }
    /* 默认当前点没有完成峰谷配对。 */
    *pair_accepted = false;
    /* 已有时间线时先检查重复、倒退和长间断。 */
    if (detector->has_timestamp) {
        /* 时间必须严格递增。 */
        if (monotonic_ms <= detector->last_timestamp_ms) {
            /* 拒绝重复或倒退时间。 */
            return MOTION_PHASE_ERR_TIMESTAMP;
        }
        /* gap_ms 是相邻原始 IMU 点间隔，单位 ms。 */
        const uint64_t gap_ms = monotonic_ms - detector->last_timestamp_ms;
        /* 超过 120 ms 时不能跨缺口配峰谷。 */
        if (gap_ms > MOTION_PHASE_MAX_GAP_MS) {
            /* 清空未完成状态但保留轴累计。 */
            motion_periodic_pair_reset_cycle(detector);
            /* 当前点作为新时间线首点。 */
            detector->has_timestamp = true;
            /* 保存当前时间。 */
            detector->last_timestamp_ms = monotonic_ms;
            /* 第一级启动窗口接收当前加速度。 */
            float long_mean_g = 0.0F;
            /* 首点必然产生一个奇数窗口均值。 */
            if (motion_periodic_mean_push(
                    &detector->long_filter,
                    acceleration_g,
                    &long_mean_g)) {
                /* 保存第二级输出占位。 */
                float short_mean_g = 0.0F;
                /* 第二级同样接收首个有效长均值。 */
                if (motion_periodic_mean_push(
                        &detector->short_filter,
                        long_mean_g,
                        &short_mean_g)) {
                    /* 建立新的首个滤波历史点，不可能在本点计数。 */
                    motion_periodic_push_filtered(
                        detector,
                        short_mean_g,
                        pair_accepted);
                }
            }
            /* 显式通知上层同步清空跨缺口周期。 */
            return MOTION_PHASE_GAP_RESET;
        }
    }
    /* 保存当前点为下一次时间检查基准。 */
    detector->has_timestamp = true;
    /* 保存当前单调时间，单位 ms。 */
    detector->last_timestamp_ms = monotonic_ms;
    /* long_mean_g 接收 11 点级输出，单位 g。 */
    float long_mean_g = 0.0F;
    /* 偶数启动点没有输出时正常等待。 */
    if (!motion_periodic_mean_push(
            &detector->long_filter,
            acceleration_g,
            &long_mean_g)) {
        /* 当前点仅填充窗口。 */
        return MOTION_PHASE_OK;
    }
    /* short_mean_g 接收 5 点级输出，单位 g。 */
    float short_mean_g = 0.0F;
    /* 第二级偶数启动点同样只填充历史。 */
    if (!motion_periodic_mean_push(
            &detector->short_filter,
            long_mean_g,
            &short_mean_g)) {
        /* 当前长均值已保存，但尚无最终滤波点。 */
        return MOTION_PHASE_OK;
    }
    /* 32 位滤波序号耗尽前主动清空未完成状态，避免减法回绕。 */
    if (detector->filtered_sequence == UINT32_MAX) {
        /* 保留累计并重新建立窗口。 */
        motion_periodic_pair_reset_cycle(detector);
        /* 当前时间仍作为新时间线基准。 */
        detector->has_timestamp = true;
        /* 保存当前时间。 */
        detector->last_timestamp_ms = monotonic_ms;
        /* 报告间断式重置，实际需连续多年才可能到达。 */
        return MOTION_PHASE_GAP_RESET;
    }
    /* 送入三点局部极值和一对一峰谷状态机。 */
    motion_periodic_push_filtered(detector, short_mean_g, pair_accepted);
    /* 当前点处理成功。 */
    return MOTION_PHASE_OK;
}

motion_phase_status_t motion_phase_init(
    motion_phase_detector_t *detector,
    const fitness_action_t action)
{
    /* 检测器必须存在，动作必须位于模型 11 类范围。 */
    if ((detector == NULL) || (action < FITNESS_ACTION_GOOD_MORNING) ||
        (action >= FITNESS_ACTION_COUNT)) {
        /* 非法输入不修改未知内存。 */
        return MOTION_PHASE_ERR_ARGUMENT;
    }
    /* 清空旧会话方向、基线、时间和事件保持状态。 */
    memset(detector, 0, sizeof(*detector));
    /* 保存锁定动作。 */
    detector->action = action;
    /* 标记初始化成功。 */
    detector->initialized = true;
    /* 初始输出事件设为 REST。 */
    detector->held_phase = FITNESS_PHASE_REST;
    /* walk/trot 首个合法高门峰允许触发；其它动作不读取该字段。 */
    detector->step_peak_armed = true;
    /* 返回成功。 */
    return MOTION_PHASE_OK;
}

motion_phase_status_t motion_phase_configure_jumping_jack_axis(
    motion_phase_detector_t *detector,
    const uint8_t gyro_axis)
{
    /* 仅允许在已初始化的开合跳检测器接收首点前配置 gx、gy 或 gz。 */
    if ((detector == NULL) || !detector->initialized || detector->has_timestamp ||
        (detector->action != FITNESS_ACTION_JUMPING_JACK) || (gyro_axis >= 3U)) {
        /* 非法动作、轴号或中途改轴均拒绝，防止把两个物理周期拼接。 */
        return MOTION_PHASE_ERR_ARGUMENT;
    }
    /* 清除可能残留的三维单位向量，后续只允许一个轴参与有符号投影。 */
    detector->learned_gyro_direction[0] = 0.0F;
    /* 清除 gy 权重。 */
    detector->learned_gyro_direction[1] = 0.0F;
    /* 清除 gz 权重。 */
    detector->learned_gyro_direction[2] = 0.0F;
    /* 当前检测器所绑定轴的权重固定为 +1；轴坐标正负只决定从哪个半周期开始。 */
    detector->learned_gyro_direction[gyro_axis] = 1.0F;
    /* 保存当前检测器轴编号，供暂停或间断重置后恢复同一通道。 */
    detector->fixed_gyro_axis = gyro_axis;
    /* 标记固定轴合同生效。 */
    detector->fixed_gyro_axis_valid = true;
    /* 单位轴已经可直接投影，不再等待首次三维向量学习。 */
    detector->direction_valid = true;
    /* 从正轴门槛开始等待完整正→负→正周期。 */
    detector->stage = MOTION_TWO_WAIT_PRIMARY;
    /* 清除未消费离散事件，配置后首点必须由真实波形产生。 */
    detector->hold_remaining = 0U;
    /* 返回成功。 */
    return MOTION_PHASE_OK;
}

void motion_phase_reset(motion_phase_detector_t *detector)
{
    /* 空对象或未初始化对象没有可保留合同。 */
    if ((detector == NULL) || !detector->initialized) {
        /* 安全无操作。 */
        return;
    }
    /* 暂存锁定动作，清空其它状态后恢复。 */
    const fitness_action_t action = detector->action;
    /* 暂存当前开合跳检测器固定轴；暂停或 120 ms 间断不能改变其物理通道。 */
    const uint8_t fixed_gyro_axis = detector->fixed_gyro_axis;
    /* 只有有效开合跳固定轴才允许跨重置恢复。 */
    const bool fixed_gyro_axis_valid =
        (action == FITNESS_ACTION_JUMPING_JACK) && detector->fixed_gyro_axis_valid &&
        (fixed_gyro_axis < 3U);
    /* 清空不完整周期、方向、步峰和时间历史。 */
    memset(detector, 0, sizeof(*detector));
    /* 恢复锁定动作。 */
    detector->action = action;
    /* 恢复初始化标志。 */
    detector->initialized = true;
    /* 恢复安全相位哨兵。 */
    detector->held_phase = FITNESS_PHASE_REST;
    /* 时间线或休息边界后只接受一个全新高门峰。 */
    detector->step_peak_armed = true;
    /* 恢复当前检测器固定轴有效标志。 */
    detector->fixed_gyro_axis_valid = fixed_gyro_axis_valid;
    /* 恢复轴编号；无固定轴时零值仅为确定性哨兵。 */
    detector->fixed_gyro_axis = fixed_gyro_axis_valid ? fixed_gyro_axis : 0U;
    /* 固定轴有效时重建单位向量，避免三轴点积在恢复后重新出现。 */
    if (fixed_gyro_axis_valid) {
        /* 当前固定轴权重恢复为 +1，其余分量已由 memset 清零。 */
        detector->learned_gyro_direction[fixed_gyro_axis] = 1.0F;
        /* 单轴方向可立即使用。 */
        detector->direction_valid = true;
    }
}

motion_phase_status_t motion_phase_push(
    motion_phase_detector_t *detector,
    const motion_phase_sample_t *sample,
    motion_phase_observation_t *observation)
{
    /* 三个对象必须有效，检测器必须先初始化，六轴必须为有限值。 */
    if ((detector == NULL) || (sample == NULL) || (observation == NULL) ||
        !detector->initialized || !motion_sample_is_finite(sample)) {
        /* 非法输入不得污染时间和滤波状态。 */
        return MOTION_PHASE_ERR_ARGUMENT;
    }
    /* 计算当前角速度模长，单位 deg/s。 */
    const float gyro_magnitude_dps = motion_vector_magnitude(
        sample->axis[0], sample->axis[1], sample->axis[2]);
    /* 计算当前加速度模长，单位 g。 */
    const float acceleration_magnitude_g = motion_vector_magnitude(
        sample->axis[3], sample->axis[4], sample->axis[5]);
    /* 初始化输出，确保任何分支都不留下未定义字段。 */
    motion_observation_init(
        observation,
        acceleration_magnitude_g,
        gyro_magnitude_dps);

    /* 已有时间历史时，先检查重复、倒退和长间断。 */
    if (detector->has_timestamp) {
        /* 时间必须严格递增。 */
        if (sample->monotonic_ms <= detector->last_timestamp_ms) {
            /* 拒绝重复或倒退点，保留此前周期。 */
            return MOTION_PHASE_ERR_TIMESTAMP;
        }
        /* 计算无符号安全时间间隔。 */
        const uint64_t gap_ms = sample->monotonic_ms - detector->last_timestamp_ms;
        /* 超过 120 ms 时，旧相位不能与新数据拼接。 */
        if (gap_ms > MOTION_PHASE_MAX_GAP_MS) {
            /* 清空周期并保留动作。 */
            motion_phase_reset(detector);
            /* 把当前点作为新时间线首点。 */
            detector->has_timestamp = true;
            /* 保存当前单调时间。 */
            detector->last_timestamp_ms = sample->monotonic_ms;
            /* walk/trot 首点同时建立基线。 */
            if (motion_action_is_step(detector->action)) {
                /* 初始化步峰基线和历史。 */
                motion_process_step(detector, acceleration_magnitude_g, observation);
            }
            /* 显式报告已重置，调用方同步重置未完成计数周期。 */
            return MOTION_PHASE_GAP_RESET;
        }
    }
    /* 记录当前点作为下一次时间检查基准。 */
    detector->has_timestamp = true;
    /* 保存当前单调时间。 */
    detector->last_timestamp_ms = sample->monotonic_ms;

    /* 离散事件需要连续两点时，优先重复 held_phase。 */
    if (detector->hold_remaining > 0U) {
        /* 输出保存的事件相位。 */
        observation->phase = detector->held_phase;
        /* 标记相位有效。 */
        observation->phase_valid = true;
        /* 消耗一个保持点。 */
        detector->hold_remaining -= 1U;
        /* 保持阶段不同时寻找下一事件，避免同一点承担两个生理阶段。 */
        return MOTION_PHASE_OK;
    }

    /* 八类腕戴重复动作执行主旋转方向检测。 */
    if (motion_action_is_two_phase(detector->action)) {
        /* 用三轴投影检测主向、回向与稳定闭合。 */
        motion_process_two_phase(
            detector,
            sample,
            acceleration_magnitude_g,
            gyro_magnitude_dps,
            observation);
        /* 返回成功。 */
        return MOTION_PHASE_OK;
    }
    /* walk/trot 执行三点局部冲击峰检测。 */
    if (motion_action_is_step(detector->action)) {
        /* 更新慢基线和局部峰历史。 */
        motion_process_step(detector, acceleration_magnitude_g, observation);
        /* 返回成功。 */
        return MOTION_PHASE_OK;
    }
    /* sit 没有重复相位，稳定点只作为诊断 REST，不触发计数。 */
    if ((detector->action == FITNESS_ACTION_SIT) &&
        motion_is_rest(acceleration_magnitude_g, gyro_magnitude_dps)) {
        /* 输出 REST 供活动门控参考。 */
        observation->phase = FITNESS_PHASE_REST;
        /* sit 不调用重复计数器，因此仍可标记稳定相位。 */
        observation->phase_valid = true;
    }
    /* 所有合法动作均处理完成。 */
    return MOTION_PHASE_OK;
}
