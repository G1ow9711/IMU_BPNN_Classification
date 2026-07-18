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
/* 两相位内部阶段 2：已看到回摆，等待稳定基线闭合周期。 */
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

/* 判断动作是否属于四个跳跃类。 */
static bool motion_action_is_jump(const fitness_action_t action)
{
    /* 四个包含离地和落地的类别共用五阶段检测。 */
    return (action == FITNESS_ACTION_JUMPING_JACK) ||
           (action == FITNESS_ACTION_JUMPING_LUNGE) ||
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
    /* 早安式、普通弓步、普通深蹲和挥手属于往返动作。 */
    return (action == FITNESS_ACTION_GOOD_MORNING) ||
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

/* 处理早安式/弓步/深蹲/挥手的主向、回向和稳定闭合。 */
static void motion_process_two_phase(
    motion_phase_detector_t *detector,
    const motion_phase_sample_t *sample,
    const float acceleration_magnitude_g,
    const float gyro_magnitude_dps,
    motion_phase_observation_t *observation)
{
    /* 等待主向时，用第一次显著旋转的单位向量定义正方向。 */
    if ((detector->stage == MOTION_TWO_WAIT_PRIMARY) &&
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
    }

    /* 主向和回向均出现后，稳定点闭合一次往返周期。 */
    if ((detector->stage == MOTION_TWO_WAIT_REST) &&
        motion_is_rest(acceleration_magnitude_g, gyro_magnitude_dps)) {
        /* 回到等待下一次主向。 */
        detector->stage = MOTION_TWO_WAIT_PRIMARY;
        /* 下一周期重新学习方向，适应手腕姿态缓慢变化。 */
        detector->direction_valid = false;
        /* 发布 REST 两点稳定事件。 */
        motion_emit_phase(detector, FITNESS_PHASE_REST, observation);
    }
}

/* 处理四个跳跃类的起跳、腾空、落地、恢复和稳定闭合。 */
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

void motion_phase_reset(motion_phase_detector_t *detector)
{
    /* 空对象或未初始化对象没有可保留合同。 */
    if ((detector == NULL) || !detector->initialized) {
        /* 安全无操作。 */
        return;
    }
    /* 暂存锁定动作，清空其它状态后恢复。 */
    const fitness_action_t action = detector->action;
    /* 清空不完整周期、方向、步峰和时间历史。 */
    memset(detector, 0, sizeof(*detector));
    /* 恢复锁定动作。 */
    detector->action = action;
    /* 恢复初始化标志。 */
    detector->initialized = true;
    /* 恢复安全相位哨兵。 */
    detector->held_phase = FITNESS_PHASE_REST;
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

    /* 跳跃类执行五阶段检测。 */
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
    /* 四个往返动作执行主旋转方向检测。 */
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
