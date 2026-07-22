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

/* 跳跃内部阶段 0：等待起跳推进。 */
#define MOTION_JUMP_WAIT_TAKEOFF (0U)
/* 跳跃内部阶段 1：等待低支持力腾空。 */
#define MOTION_JUMP_WAIT_FLIGHT (1U)
/* 跳跃内部阶段 2：等待落地冲击。 */
#define MOTION_JUMP_WAIT_LANDING (2U)
/* 跳跃内部阶段 3：等待冲击恢复。 */
#define MOTION_JUMP_WAIT_RECOVERY (3U)
/* 跳跃内部阶段 4：等待稳定基线闭合周期。 */
#define MOTION_JUMP_WAIT_REST (4U)

/* 步峰慢基线的一阶更新系数；25 Hz 下时间常数约 2 秒。 */
#define MOTION_STEP_BASELINE_ALPHA (0.02F)
/* 起跳除加速度外要求的角速度门槛，单位 deg/s。 */
#define MOTION_TAKEOFF_GYRO_MIN_DPS (28.0F)
/* 相邻极值类型 0 表示尚无候选。 */
#define MOTION_PERIODIC_EXTREMUM_NONE (0U)
/* 相邻极值类型 1 表示局部波峰。 */
#define MOTION_PERIODIC_EXTREMUM_PEAK (1U)
/* 相邻极值类型 2 表示局部波谷。 */
#define MOTION_PERIODIC_EXTREMUM_VALLEY (2U)

/* 判断动作是否属于需要腕部支持力五阶段的三个跳跃类。 */
static bool motion_action_is_jump(const fitness_action_t action)
{
    /* 跳跃弓步、跳深蹲和抱膝跳共用起跳、腾空、落地、恢复检测。 */
    return (action == FITNESS_ACTION_JUMPING_LUNGE) ||
           (action == FITNESS_ACTION_JUMPING_SQUAT) ||
           (action == FITNESS_ACTION_TUCK_JUMP);
}

/* 判断动作是否使用 walk/trot 局部冲击峰。 */
static bool motion_action_is_step(const fitness_action_t action)
{
    /* 只有 walk 和 trot 输出 step_peak。 */
    return (action == FITNESS_ACTION_WALK) || (action == FITNESS_ACTION_TROT);
}

/* 判断动作是否使用主向/回向两相位。 */
static bool motion_action_is_two_phase(const fitness_action_t action)
{
    /* 开合跳按手臂张开/合拢腕部回摆计数，其余四类同样属于往返动作。 */
    return (action == FITNESS_ACTION_GOOD_MORNING) ||
           (action == FITNESS_ACTION_JUMPING_JACK) ||
           (action == FITNESS_ACTION_LUNGE) ||
           (action == FITNESS_ACTION_SQUAT) ||
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

/* 处理开合跳及普通往返动作的主向、回向和稳定闭合。 */
static void motion_process_two_phase(
    motion_phase_detector_t *detector,
    const motion_phase_sample_t *sample,
    const float acceleration_magnitude_g,
    const float gyro_magnitude_dps,
    motion_phase_observation_t *observation)
{
    /* 尚未建立投影轴时，用第一次显著旋转的单位向量定义正方向。 */
    if ((detector->stage == MOTION_TWO_WAIT_PRIMARY) &&
        !detector->direction_valid &&
        (gyro_magnitude_dps >= MOTION_PHASE_DIRECTION_LEARN_DPS)) {
        /* 三个分量除以非零模长，得到与手表佩戴坐标一致的单位方向。 */
        detector->learned_gyro_direction[0] = sample->axis[0] / gyro_magnitude_dps;
        /* 保存 y 方向分量。 */
        detector->learned_gyro_direction[1] = sample->axis[1] / gyro_magnitude_dps;
        /* 保存 z 方向分量。 */
        detector->learned_gyro_direction[2] = sample->axis[2] / gyro_magnitude_dps;
        /* 标记投影方向可用。 */
        detector->direction_valid = true;
        /* 后续等待沿相反方向的回摆。 */
        detector->stage = MOTION_TWO_WAIT_SECONDARY;
        /* 发布 PRIMARY 两点稳定事件。 */
        motion_emit_phase(detector, FITNESS_PHASE_PRIMARY, observation);
        /* 当前样本已处理。 */
        return;
    }

    /* 已学习方向时计算当前角速度在主旋转方向上的有符号投影。 */
    if (detector->direction_valid) {
        /* 点积单位为 deg/s；负值表示与首次动作方向相反。 */
        const float projected_dps =
            (sample->axis[0] * detector->learned_gyro_direction[0]) +
            (sample->axis[1] * detector->learned_gyro_direction[1]) +
            (sample->axis[2] * detector->learned_gyro_direction[2]);
        /* 开合跳完成首周期后保留会话投影轴；再次越过正门槛即可开启下一周期。 */
        if ((detector->stage == MOTION_TWO_WAIT_PRIMARY) &&
            (projected_dps >= MOTION_PHASE_DIRECTION_ACTIVE_DPS)) {
            /* 下一阶段等待沿同一固定投影轴的反向手臂运动。 */
            detector->stage = MOTION_TWO_WAIT_SECONDARY;
            /* 发布 PRIMARY 两点稳定事件，交叉轴能量不改变会话主方向。 */
            motion_emit_phase(detector, FITNESS_PHASE_PRIMARY, observation);
            /* 当前样本已处理。 */
            return;
        }
        /* 等待回向时，足够强的负投影确认 SECONDARY。 */
        if ((detector->stage == MOTION_TWO_WAIT_SECONDARY) &&
            (projected_dps <= -MOTION_PHASE_DIRECTION_ACTIVE_DPS)) {
            /* 下一阶段等待低运动稳定点。 */
            detector->stage = MOTION_TWO_WAIT_REST;
            /* 发布 SECONDARY 两点稳定事件。 */
            motion_emit_phase(detector, FITNESS_PHASE_SECONDARY, observation);
            /* 当前样本已处理。 */
            return;
        }
        /* 连续动作不会整机静止；回到主方向说明手臂已完成回程并开始下一周期。 */
        if ((detector->stage == MOTION_TWO_WAIT_REST) &&
            (projected_dps >= MOTION_PHASE_DIRECTION_ACTIVE_DPS)) {
            /* 开合跳的返回正峰同时是下一周期起点，直接等待下一负峰；其它动作重新等待 PRIMARY。 */
            detector->stage = (detector->action == FITNESS_ACTION_JUMPING_JACK) ?
                              MOTION_TWO_WAIT_SECONDARY :
                              MOTION_TWO_WAIT_PRIMARY;
            /* 开合跳整场保留首次主投影轴；慢动作仍逐周期重学以适应缓慢姿态变化。 */
            detector->direction_valid = detector->action == FITNESS_ACTION_JUMPING_JACK;
            /* 发布 REST 两点稳定事件；下游仍检查动作最短周期和不应期。 */
            motion_emit_phase(detector, FITNESS_PHASE_REST, observation);
            /* 开合跳把当前返回正峰保存在内部阶段中作为下一周期起点；事件层只发布一次 REST，避免同点重复事件。 */
            return;
        }
    }

    /* 慢动作在回程后停住时，低运动稳定点仍可闭合一次往返周期。 */
    if ((detector->stage == MOTION_TWO_WAIT_REST) &&
        motion_is_rest(acceleration_magnitude_g, gyro_magnitude_dps)) {
        /* 开合跳静止端点也作为下一周期主端；其它慢动作回到等待新 PRIMARY。 */
        detector->stage = (detector->action == FITNESS_ACTION_JUMPING_JACK) ?
                          MOTION_TWO_WAIT_SECONDARY :
                          MOTION_TWO_WAIT_PRIMARY;
        /* 开合跳整场保留首次主投影轴；其它慢动作在静止端点重新学习方向。 */
        detector->direction_valid = detector->action == FITNESS_ACTION_JUMPING_JACK;
        /* 发布 REST 两点稳定事件。 */
        motion_emit_phase(detector, FITNESS_PHASE_REST, observation);
    }
}

/* 处理三个高动态跳跃类的起跳、腾空、落地、恢复和稳定闭合。 */
static void motion_process_jump(
    motion_phase_detector_t *detector,
    const float acceleration_magnitude_g,
    const float gyro_magnitude_dps,
    motion_phase_observation_t *observation)
{
    /* 起跳要求支持力上升，且有手腕运动或更强加速度冲击。 */
    const bool takeoff =
        (acceleration_magnitude_g >= MOTION_PHASE_TAKEOFF_MIN_G) &&
        ((gyro_magnitude_dps >= MOTION_TAKEOFF_GYRO_MIN_DPS) ||
         (acceleration_magnitude_g >= MOTION_PHASE_LANDING_MIN_G));
    /* 等待起跳时，满足推进条件后进入腾空等待。 */
    if ((detector->stage == MOTION_JUMP_WAIT_TAKEOFF) && takeoff) {
        /* 更新内部阶段。 */
        detector->stage = MOTION_JUMP_WAIT_FLIGHT;
        /* 发布 TAKEOFF 两点稳定事件。 */
        motion_emit_phase(detector, FITNESS_PHASE_TAKEOFF, observation);
        /* 当前样本已处理。 */
        return;
    }
    /* 低支持力加速度确认腾空。 */
    if ((detector->stage == MOTION_JUMP_WAIT_FLIGHT) &&
        (acceleration_magnitude_g <= MOTION_PHASE_FLIGHT_MAX_G)) {
        /* 更新为等待落地。 */
        detector->stage = MOTION_JUMP_WAIT_LANDING;
        /* 发布 FLIGHT 两点稳定事件。 */
        motion_emit_phase(detector, FITNESS_PHASE_FLIGHT, observation);
        /* 当前样本已处理。 */
        return;
    }
    /* 腾空后出现大于 1.30 g 的冲击，确认落地。 */
    if ((detector->stage == MOTION_JUMP_WAIT_LANDING) &&
        (acceleration_magnitude_g >= MOTION_PHASE_LANDING_MIN_G)) {
        /* 更新为等待冲击恢复。 */
        detector->stage = MOTION_JUMP_WAIT_RECOVERY;
        /* 发布 LANDING 两点稳定事件。 */
        motion_emit_phase(detector, FITNESS_PHASE_LANDING, observation);
        /* 当前样本已处理。 */
        return;
    }
    /* 落地后首次恢复到稳定范围，标记 RECOVERY。 */
    if ((detector->stage == MOTION_JUMP_WAIT_RECOVERY) &&
        motion_is_rest(acceleration_magnitude_g, gyro_magnitude_dps)) {
        /* 下一阶段再要求一个稳定事件，避免同一冲击边沿直接计数。 */
        detector->stage = MOTION_JUMP_WAIT_REST;
        /* 发布 RECOVERY 两点稳定事件。 */
        motion_emit_phase(detector, FITNESS_PHASE_RECOVERY, observation);
        /* 当前样本已处理。 */
        return;
    }
    /* 恢复事件结束后仍保持稳定，闭合完整跳跃周期。 */
    if ((detector->stage == MOTION_JUMP_WAIT_REST) &&
        motion_is_rest(acceleration_magnitude_g, gyro_magnitude_dps)) {
        /* 回到等待下一次起跳。 */
        detector->stage = MOTION_JUMP_WAIT_TAKEOFF;
        /* 发布 REST 两点稳定事件。 */
        motion_emit_phase(detector, FITNESS_PHASE_REST, observation);
    }
}

/* 处理 walk/trot 三点局部冲击峰；下游仍负责生理不应期去重。 */
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
    /* 已有前两个点时，检查中间点是否高于两侧且超过 0.16 g。 */
    if (detector->step_history_ready) {
        /* 中间点严格高于左侧且不低于右侧，避免平台峰重复输出。 */
        const bool local_peak =
            (detector->previous_dynamic_acceleration_g >
             detector->previous_previous_dynamic_acceleration_g) &&
            (detector->previous_dynamic_acceleration_g >= dynamic_acceleration_g);
        /* 同时满足幅值门槛才发布步峰。 */
        observation->step_peak =
            local_peak &&
            (detector->previous_dynamic_acceleration_g >= MOTION_PHASE_STEP_PEAK_DELTA_G);
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

motion_phase_status_t motion_periodic_pair_skip_transient(
    motion_periodic_pair_detector_t *detector,
    const uint64_t monotonic_ms)
{
    /* 检测器必须有效且已经建立 11+5 均值与峰谷静态状态。 */
    if ((detector == NULL) || !detector->initialized) {
        /* 空对象或未初始化对象不能维护时间线。 */
        return MOTION_PHASE_ERR_ARGUMENT;
    }
    /* 已有时间基准时必须继续执行严格递增与真实断流检查。 */
    if (detector->has_timestamp) {
        /* 重复或倒退时间不能伪装成马达污染点。 */
        if (monotonic_ms <= detector->last_timestamp_ms) {
            /* 拒绝该时刻且不修改已有检测状态。 */
            return MOTION_PHASE_ERR_TIMESTAMP;
        }
        /* gap_ms 表示当前污染点与上一原始 25 Hz 点的间隔，单位 ms。 */
        const uint64_t gap_ms = monotonic_ms - detector->last_timestamp_ms;
        /* 超过 120 ms 仍属于真实采样断流，不能仅靠时间占位跨过。 */
        if (gap_ms > MOTION_PHASE_MAX_GAP_MS) {
            /* 清空滤波与未完成峰谷，但保留该轴已经发布的累计次数。 */
            motion_periodic_pair_reset_cycle(detector);
            /* 当前污染点作为新连续段的时间起点，幅值仍不进入滤波器。 */
            detector->has_timestamp = true;
            /* 保存新时间起点，下一点继续按真实相邻间隔检查。 */
            detector->last_timestamp_ms = monotonic_ms;
            /* 通知上层同步清空其它轴未完成周期。 */
            return MOTION_PHASE_GAP_RESET;
        }
    }
    /* 第五个连续污染点已超过单次振动保护上限，禁止跨长污染拼接峰谷。 */
    if (detector->transient_skip_count >=
        MOTION_PERIODIC_MAX_TRANSIENT_SKIP_SAMPLES) {
        /* 清空滤波、未完成峰谷和连续污染计数，但保留已完成累计。 */
        motion_periodic_pair_reset_cycle(detector);
        /* 当前污染点作为新连续段时间起点，幅值仍不进入滤波器。 */
        detector->has_timestamp = true;
        /* 保存当前时刻供下一点检查。 */
        detector->last_timestamp_ms = monotonic_ms;
        /* 超长污染按断流处理。 */
        return MOTION_PHASE_GAP_RESET;
    }
    /* 当前点只成为下一点的时间基准，不改变任何滤波样本或极值端点。 */
    detector->has_timestamp = true;
    /* 保存马达污染点的单调毫秒，保持 25 Hz 时间线连续。 */
    detector->last_timestamp_ms = monotonic_ms;
    /* 连续污染计数增加一，用于限制跨污染拼接的最长时间。 */
    detector->transient_skip_count += 1U;
    /* 短暂污染已安全跳过。 */
    return MOTION_PHASE_OK;
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
    /* 干净幅值终止连续污染段，下一次振动从零重新计数。 */
    detector->transient_skip_count = 0U;
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

    /* 高动态跳跃类执行五阶段检测；开合跳走腕部两相位分支。 */
    if (motion_action_is_jump(detector->action)) {
        /* 用支持力和角速度检测起跳、腾空、落地与恢复。 */
        motion_process_jump(
            detector,
            acceleration_magnitude_g,
            gyro_magnitude_dps,
            observation);
        /* 返回成功。 */
        return MOTION_PHASE_OK;
    }
    /* 开合跳和四个普通往返动作执行主旋转方向检测。 */
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
