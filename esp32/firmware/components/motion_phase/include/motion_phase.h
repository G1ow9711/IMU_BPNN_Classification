#ifndef IMU_HANDHELD_MOTION_PHASE_H
#define IMU_HANDHELD_MOTION_PHASE_H

/*
 * 把严格 25 Hz 六轴样本转换为计数状态机需要的动作相位或步峰。
 *
 * 输入轴顺序固定 gx、gy、gz、ax、ay、az；前三轴单位 deg/s，后三轴单位 g。
 * 公式、阈值、边界和复杂度见 docs/算法原理、训练与实时计数.md。
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
/* 两相位动作开始学习主旋转方向的角速度下限，单位 deg/s；活动门已先排除整段静止。 */
#define MOTION_PHASE_DIRECTION_LEARN_DPS (26.0F)
/* 自适应主向/回向门的最低值，单位 deg/s；必须高于 22 deg/s 静止门以形成迟滞。 */
#define MOTION_PHASE_DIRECTION_ACTIVE_MIN_DPS (27.0F)
/* 自适应主向/回向门的最高值，单位 deg/s；防止一次猛烈冲击让后续正常动作永久失效。 */
#define MOTION_PHASE_DIRECTION_ACTIVE_MAX_DPS (72.0F)
/* 主向/回向门取活动角速度包络的 45%，使不同人的动作幅度按比例归一并拒绝次级波纹。 */
#define MOTION_PHASE_DIRECTION_ACTIVE_RATIO (0.45F)
/* 新高峰按 25% 更新角速度包络，使大幅动作的判定门在一到两次峰内同步抬升。 */
#define MOTION_PHASE_SCALE_RISE_ALPHA (0.25F)
/* 低于包络的有效运动按 1% 缓慢回落，兼顾同一轮中的疲劳和幅度变小。 */
#define MOTION_PHASE_SCALE_FALL_ALPHA (0.01F)
/* 新方向与当前方向夹角余弦绝对值至少为 0.60 才参与更新，拒绝交叉轴噪声。 */
#define MOTION_PHASE_DIRECTION_ALIGNMENT_MIN (0.60F)
/* 合法同轴运动每点只用 8% 更新方向，允许自然姿态漂移但不跟随瞬时摆腕。 */
#define MOTION_PHASE_DIRECTION_ADAPT_ALPHA (0.08F)
/* 离散端点必须由两个真实连续 25 Hz 点确认，禁止把单点峰值人工复制成稳定相位。 */
#define MOTION_PHASE_TRANSITION_CONFIRM_SAMPLES (2U)
/* 静止判定角速度模长上限，单位 deg/s。 */
#define MOTION_PHASE_REST_GYRO_DPS (22.0F)
/* 静止判定加速度模长相对 1 g 的允许偏差，单位 g。 */
#define MOTION_PHASE_REST_ACCEL_TOLERANCE_G (0.24F)
/* walk/trot 局部冲击峰相对慢基线的最小增量，单位 g。 */
#define MOTION_PHASE_STEP_PEAK_DELTA_G (0.16F)
/* 步峰接受后必须真正回落到慢基线才重新武装，防止拉长动作把正向次级波瓣拆成第二步。 */
#define MOTION_PHASE_STEP_REARM_DELTA_G (0.0F)
/* 每个离散事件至少保持两个 25 Hz 点，满足下游计数器稳定相位合同。 */
#define MOTION_PHASE_EVENT_HOLD_SAMPLES (2U)
/* 开合跳参考算法第一级均值窗口为 11 点，25 Hz 下覆盖 0.44 秒。 */
#define MOTION_PERIODIC_MEAN_LONG_LENGTH (11U)
/* 开合跳参考算法第二级均值窗口为 5 点，25 Hz 下覆盖 0.20 秒。 */
#define MOTION_PERIODIC_MEAN_SHORT_LENGTH (5U)
/* 峰谷间至少相隔 4 个滤波输出点，即至少 0.16 秒，过近变化视为毛刺。 */
#define MOTION_PERIODIC_MIN_HALF_CYCLE_SAMPLES (4U)
/* 峰谷间至多相隔 40 个滤波输出点，即至多 1.60 秒，过慢变化视为姿态漂移。 */
#define MOTION_PERIODIC_MAX_HALF_CYCLE_SAMPLES (40U)
/* 相邻峰谷幅值至少为 1/14 g，与用户 StepCounter 的 4096/14 raw 阈值一致。 */
#define MOTION_PERIODIC_MIN_AMPLITUDE_G (1.0F / 14.0F)
/* 同一轴两次有效峰谷至少相隔 13 点，即 0.52 秒，抑制一个动作内部的次级波纹。 */
#define MOTION_PERIODIC_AXIS_REFRACTORY_SAMPLES (13U)
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
    /* true 表示 phase 来自方向迟滞端点、跳跃阶段或稳定静止，可送入重复计数器。 */
    bool phase_valid;
    /* true 表示 walk/trot 出现一个局部冲击峰，可送入步峰去重器。 */
    bool step_peak;
    /* 当前加速度模长，单位 g，供诊断页显示。 */
    float acceleration_magnitude_g;
    /* 当前角速度模长，单位 deg/s，供诊断页显示。 */
    float gyro_magnitude_dps;
} motion_phase_observation_t;

/* 保存一级流式均值滤波器；最大 11 点静态数组避免 ESP32 堆分配。 */
typedef struct {
    /* buffer 按时间循环保存最多 11 个加速度样本，单位 g，内存 44 字节。 */
    float buffer[MOTION_PERIODIC_MEAN_LONG_LENGTH];
    /* sum_g 保存当前有效窗口总和，单位 g，用于 O(1) 更新均值。 */
    float sum_g;
    /* length 保存本级真实窗口长度，只允许 5 或 11。 */
    uint8_t length;
    /* write_index 指向下一次覆盖位置，范围 0..length-1。 */
    uint8_t write_index;
    /* count 保存已写入有效点数，达到 length 后保持不变。 */
    uint8_t count;
} motion_periodic_mean_filter_t;

/* 保存一个加速度轴的在线相邻峰谷计数状态；无动态内存。 */
typedef struct {
    /* initialized 表示两个均值滤波器和计数状态已经建立。 */
    bool initialized;
    /* has_timestamp 表示 last_timestamp_ms 可用于检查单调性和采样间断。 */
    bool has_timestamp;
    /* last_timestamp_ms 保存最近输入时间，单位 ms，必须严格递增。 */
    uint64_t last_timestamp_ms;
    /* long_filter 先执行 11 点均值，压制腕部高频抖动。 */
    motion_periodic_mean_filter_t long_filter;
    /* short_filter 再执行 5 点均值，复现 StepCounter 的 11+5 双均值链。 */
    motion_periodic_mean_filter_t short_filter;
    /* previous_previous_g 保存上上个有效滤波点，单位 g，用于三点极值判定。 */
    float previous_previous_g;
    /* previous_g 保存上个有效滤波点，单位 g；当前点到来时判断它是否为峰或谷。 */
    float previous_g;
    /* filtered_history_count 保存已有历史点数，达到 2 后才检测局部极值。 */
    uint8_t filtered_history_count;
    /* filtered_sequence 保存有效滤波输出序号，单位点，宽度 32 位足够连续运行约 5.4 年。 */
    uint32_t filtered_sequence;
    /* pending_valid 表示已经保存一个尚未配对的峰或谷。 */
    bool pending_valid;
    /* pending_kind 保存 1=峰、2=谷；0 仅为空状态哨兵。 */
    uint8_t pending_kind;
    /* pending_sequence 保存待配极值在滤波输出中的序号。 */
    uint32_t pending_sequence;
    /* pending_value_g 保存待配极值，单位 g。 */
    float pending_value_g;
    /* has_last_pair 表示 last_pair_sequence 可用于单轴 13 点不应期。 */
    bool has_last_pair;
    /* last_pair_sequence 保存上一有效峰谷完成时的输出序号。 */
    uint32_t last_pair_sequence;
    /* total_pairs 保存本会话该轴累计峰谷对；每对直接代表一次动作，不乘二。 */
    uint64_t total_pairs;
} motion_periodic_pair_detector_t;

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
    /* 两相位动作学习到的三轴单位旋转方向；活动段内只按同轴证据缓慢自适应。 */
    float learned_gyro_direction[3];
    /* true 表示 learned_gyro_direction 已由两个真实连续运动点确认并可用于投影。 */
    bool direction_valid;
    /* motion_scale_dps 保存活动段角速度包络，单位 deg/s，用于构造幅度自适应迟滞门。 */
    float motion_scale_dps;
    /* transition_candidate 保存待确认端点类型：0=空、1=PRIMARY、2=SECONDARY、3/4=两种闭合。 */
    uint8_t transition_candidate;
    /* transition_candidate_count 保存当前端点连续真实点数，范围 0..2。 */
    uint8_t transition_candidate_count;
    /* 开合跳首窗选出的主周期角速度轴，0/1/2 分别表示 gx/gy/gz；其它动作不读取。 */
    uint8_t fixed_gyro_axis;
    /* true 表示开合跳改用 fixed_gyro_axis 的有符号单轴值，禁止三轴点积互相抵消。 */
    bool fixed_gyro_axis_valid;
    /* 内部阶段；两相位重复动作使用等待主向、回向或闭合三个编号，外部不得改写。 */
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
    /* true 表示上一物理步已经回落到低门以下，允许下一个高门局部峰产生一次步事件。 */
    bool step_peak_armed;
    /* 已收集的步峰历史点数，达到 2 后 step_history_ready=true。 */
    uint8_t step_history_count;
} motion_phase_detector_t;

/* 以当前稳定动作初始化检测器；动作必须位于 0..10。 */
motion_phase_status_t motion_phase_init(
    motion_phase_detector_t *detector,
    fitness_action_t action);
/*
 * 在开合跳检测器首个样本前固定其角速度轴；gyro_axis 仅允许 0..2。
 * 训练引擎创建三个检测器并分别配置 gx、gy、gz，使主周期迁移时任一轴都能提出候选。
 */
motion_phase_status_t motion_phase_configure_jumping_jack_axis(
    motion_phase_detector_t *detector,
    uint8_t gyro_axis);
/* 保留锁定动作并清空不完整周期、方向、步峰和时间历史。 */
void motion_phase_reset(motion_phase_detector_t *detector);
/* 输入一个严格 25 Hz 六轴点，输出相位或步峰；时间复杂度 O(1)。 */
motion_phase_status_t motion_phase_push(
    motion_phase_detector_t *detector,
    const motion_phase_sample_t *sample,
    motion_phase_observation_t *observation);
/* 初始化单轴 11+5 均值和相邻峰谷检测器；累计从零开始。 */
motion_phase_status_t motion_periodic_pair_init(
    motion_periodic_pair_detector_t *detector);
/* 清空滤波、未完成峰谷和时间线，但保留 total_pairs 权威轴累计。 */
void motion_periodic_pair_reset_cycle(
    motion_periodic_pair_detector_t *detector);
/* 输入一个 25 Hz 单轴加速度点，输出当前点是否完成一个不复用的相邻峰谷对。 */
motion_phase_status_t motion_periodic_pair_push(
    motion_periodic_pair_detector_t *detector,
    uint64_t monotonic_ms,
    float acceleration_g,
    bool *pair_accepted);

#ifdef __cplusplus
/* 结束 C ABI 声明区。 */
}
#endif

#endif /* IMU_HANDHELD_MOTION_PHASE_H */
