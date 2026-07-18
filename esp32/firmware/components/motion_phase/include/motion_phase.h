#ifndef IMU_HANDHELD_MOTION_PHASE_H
#define IMU_HANDHELD_MOTION_PHASE_H

/*
 * 把严格 25 Hz 六轴样本转换为计数状态机需要的动作相位或步峰。
 *
 * 输入轴顺序固定 gx、gy、gz、ax、ay、az；前三轴单位 deg/s，后三轴单位 g。
 * 公式、阈值、边界和复杂度见 docs/动作相位与步峰检测.md。
 */

/* 引入 fitness_motion_phase_t 与 11 类动作枚举，保证相位输出直接接计数器。 */
#include "fitness_core.h"

/* 引入 bool 表达相位是否可信、步峰是否成立和方向是否已学习。 */
#include <stdbool.h>
/* 引入定长整数，保证单调时间和内部计数在主机与 ESP32 上宽度一致。 */
#include <stdint.h>

#ifdef __cplusplus
/* C++ 固件按 C ABI 调用，避免函数名改编。 */
extern "C" {
#endif

/* 相位检测器只接受算法固定 25 Hz，即相邻目标点 40 ms。 */
#define MOTION_PHASE_SAMPLE_PERIOD_MS (40U)
/* 超过 120 ms 表示至少丢失两个 25 Hz 点，当前周期必须重置。 */
#define MOTION_PHASE_MAX_GAP_MS (120U)
/* 两相位动作开始学习主旋转方向的角速度门槛，单位 deg/s。 */
#define MOTION_PHASE_DIRECTION_LEARN_DPS (45.0F)
/* 学习方向后的主向和回向判定门槛，单位 deg/s。 */
#define MOTION_PHASE_DIRECTION_ACTIVE_DPS (32.0F)
/* 静止判定角速度模长上限，单位 deg/s。 */
#define MOTION_PHASE_REST_GYRO_DPS (22.0F)
/* 静止判定加速度模长相对 1 g 的允许偏差，单位 g。 */
#define MOTION_PHASE_REST_ACCEL_TOLERANCE_G (0.24F)
/* 跳跃腾空判定加速度模长上限，单位 g。 */
#define MOTION_PHASE_FLIGHT_MAX_G (0.78F)
/* 起跳推进判定加速度模长门槛，单位 g。 */
#define MOTION_PHASE_TAKEOFF_MIN_G (1.18F)
/* 落地冲击判定加速度模长门槛，单位 g。 */
#define MOTION_PHASE_LANDING_MIN_G (1.30F)
/* walk/trot 局部冲击峰相对慢基线的最小增量，单位 g。 */
#define MOTION_PHASE_STEP_PEAK_DELTA_G (0.16F)
/* 每个离散事件至少保持两个 25 Hz 点，满足下游计数器稳定相位合同。 */
#define MOTION_PHASE_EVENT_HOLD_SAMPLES (2U)

/* 统一结果码；时间间断会重置内部周期并显式返回 GAP。 */
typedef enum {
    /* 当前样本已处理，输出结构有效。 */
    MOTION_PHASE_OK = 0,
    /* 输入、输出或检测器为空，或动作枚举越界。 */
    MOTION_PHASE_ERR_ARGUMENT = -1,
    /* 单调时间倒退或重复，当前样本被拒绝。 */
    MOTION_PHASE_ERR_TIMESTAMP = -2,
    /* 相邻样本间隔超过 120 ms，内部周期已重置。 */
    MOTION_PHASE_GAP_RESET = 1
} motion_phase_status_t;

/* 保存一次 25 Hz 输入；axis 顺序和单位与模型完全一致。 */
typedef struct {
    /* 设备启动后的单调毫秒时间，不允许使用可跳变 RTC。 */
    uint64_t monotonic_ms;
    /* 保存 [gx,gy,gz,ax,ay,az]；前三项 deg/s，后三项 g。 */
    float axis[FITNESS_IMU_AXIS_COUNT];
} motion_phase_sample_t;

/* 保存一次相位/步峰观测；调用方按 action 选择对应字段。 */
typedef struct {
    /* 当前相位；sit、walk、trot 没有重复相位时为 REST。 */
    fitness_motion_phase_t phase;
    /* true 表示 phase 来自完整阈值事件或稳定静止，可送入重复计数器。 */
    bool phase_valid;
    /* true 表示 walk/trot 出现一个局部冲击峰，可送入步峰去重器。 */
    bool step_peak;
    /* 当前加速度模长，单位 g，供诊断页显示。 */
    float acceleration_magnitude_g;
    /* 当前角速度模长，单位 deg/s，供诊断页显示。 */
    float gyro_magnitude_dps;
} motion_phase_observation_t;

/* 保存检测器静态状态；无动态内存，单会话约百字节。 */
typedef struct {
    /* 当前锁定动作；动作变化必须调用 reset。 */
    fitness_action_t action;
    /* true 表示 init 已成功。 */
    bool initialized;
    /* true 表示已经接受过一个时间戳。 */
    bool has_timestamp;
    /* 上一个已接受样本的单调毫秒。 */
    uint64_t last_timestamp_ms;
    /* 两相位动作学习到的三轴单位旋转方向。 */
    float learned_gyro_direction[3];
    /* true 表示 learned_gyro_direction 已归一化且可用于投影。 */
    bool direction_valid;
    /* 内部阶段；两相位和跳跃模式使用不同编号，外部不得改写。 */
    uint8_t stage;
    /* 当前需要继续输出的离散事件相位。 */
    fitness_motion_phase_t held_phase;
    /* held_phase 还需输出的样本数，范围 0..2。 */
    uint8_t hold_remaining;
    /* 加速度模长慢基线，walk/trot 用于适应佩戴静态偏差。 */
    float acceleration_baseline_g;
    /* 上一个去基线加速度，用于三点局部峰检测。 */
    float previous_dynamic_acceleration_g;
    /* 上上个去基线加速度，用于确认中间点为局部峰。 */
    float previous_previous_dynamic_acceleration_g;
    /* true 表示局部峰检测已有两个历史点。 */
    bool step_history_ready;
    /* 已收集的步峰历史点数，达到 2 后 step_history_ready=true。 */
    uint8_t step_history_count;
} motion_phase_detector_t;

/* 以当前稳定动作初始化检测器；动作必须位于 0..10。 */
motion_phase_status_t motion_phase_init(
    motion_phase_detector_t *detector,
    fitness_action_t action);
/* 保留锁定动作并清空不完整周期、方向、步峰和时间历史。 */
void motion_phase_reset(motion_phase_detector_t *detector);
/* 输入一个严格 25 Hz 六轴点，输出相位或步峰；时间复杂度 O(1)。 */
motion_phase_status_t motion_phase_push(
    motion_phase_detector_t *detector,
    const motion_phase_sample_t *sample,
    motion_phase_observation_t *observation);

#ifdef __cplusplus
/* 结束 C ABI 声明区。 */
}
#endif

#endif /* IMU_HANDHELD_MOTION_PHASE_H */
