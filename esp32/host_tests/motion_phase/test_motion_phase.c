/* 引入被测相位检测器和下游计数器，验证端到端相位合同。 */
#include "motion_phase.h"

/* 引入 NAN 构造异常输入。 */
#include <math.h>
/* 引入 stdio 输出稳定测试摘要。 */
#include <stdio.h>
/* 引入 string.h 清零测试样本。 */
#include <string.h>

/* 保存断言总数，便于确认测试没有空跑。 */
static unsigned int g_assertions = 0U;

/* 失败时打印表达式和行号并立即返回非零。 */
#define CHECK(expression)                                                       \
    do {                                                                        \
        g_assertions += 1U;                                                      \
        if (!(expression)) {                                                    \
            (void)fprintf(stderr, "CHECK failed line=%d: %s\n", __LINE__, #expression); \
            return 1;                                                           \
        }                                                                       \
    } while (0)

/* 构造六轴样本；角速度只用 x 轴，加速度只用 z 轴，单位与生产合同一致。 */
static motion_phase_sample_t make_sample(
    const uint64_t monotonic_ms,
    const float gyro_x_dps,
    const float acceleration_z_g)
{
    /* 清零未使用轴，形成可解释的一维合成波形。 */
    motion_phase_sample_t sample;
    /* 全字段归零，避免测试未初始化内存。 */
    (void)memset(&sample, 0, sizeof(sample));
    /* 写入严格单调毫秒时间。 */
    sample.monotonic_ms = monotonic_ms;
    /* 写入 gx，单位 deg/s。 */
    sample.axis[0] = gyro_x_dps;
    /* 写入 az，单位 g。 */
    sample.axis[5] = acceleration_z_g;
    /* 返回按值样本。 */
    return sample;
}

/* 构造三轴角速度样本；用于模拟真板佩戴姿态变化引入的交叉轴分量。 */
static motion_phase_sample_t make_vector_sample(
    const uint64_t monotonic_ms,
    const float gyro_x_dps,
    const float gyro_y_dps,
    const float gyro_z_dps,
    const float acceleration_z_g)
{
    /* 先复用一维构造器写入时间、gx 和 az，保持六轴顺序与生产合同一致。 */
    motion_phase_sample_t sample = make_sample(monotonic_ms, gyro_x_dps, acceleration_z_g);
    /* 写入 gy，单位 deg/s；该分量模拟手表佩戴角度变化。 */
    sample.axis[1] = gyro_y_dps;
    /* 写入 gz，单位 deg/s；零值或小值用于控制第三轴干扰。 */
    sample.axis[2] = gyro_z_dps;
    /* 返回完整六轴样本。 */
    return sample;
}

/* 把一个样本送入相位检测器，并在有效相位时同步推进重复计数器。 */
static int push_rep_sample(
    motion_phase_detector_t *detector,
    fitness_rep_counter_t *counter,
    const motion_phase_sample_t *sample,
    bool *rep_completed)
{
    /* 保存检测输出。 */
    motion_phase_observation_t observation;
    /* 默认本点没有完成重复。 */
    *rep_completed = false;
    /* 相位检测必须成功。 */
    if (motion_phase_push(detector, sample, &observation) != MOTION_PHASE_OK) {
        /* 返回失败供调用测试定位。 */
        return 1;
    }
    /* 没有稳定相位时不调用下游计数器。 */
    if (!observation.phase_valid) {
        /* 当前点处理成功。 */
        return 0;
    }
    /* 有效相位必须被计数器接受。 */
    if (fitness_rep_counter_update(
            counter,
            observation.phase,
            sample->monotonic_ms,
            rep_completed) != FITNESS_STATUS_OK) {
        /* 返回失败。 */
        return 1;
    }
    /* 相位和计数器均成功。 */
    return 0;
}

/* 验证普通深蹲的主向、回向和稳定闭合恰好计数一次。 */
static int test_two_phase_cycle(void)
{
    /* 创建深蹲相位检测器。 */
    motion_phase_detector_t detector;
    /* 创建下游重复计数器。 */
    fitness_rep_counter_t counter;
    /* 保存单点是否完成计数。 */
    bool completed = false;
    /* 初始化两组件。 */
    CHECK(motion_phase_init(&detector, FITNESS_ACTION_SQUAT) == MOTION_PHASE_OK);
    /* 深蹲必须使用两相位计数模式。 */
    CHECK(fitness_rep_counter_init(&counter, FITNESS_ACTION_SQUAT) == FITNESS_STATUS_OK);

    /* 首个 1 g 静止点只建立时间线。 */
    motion_phase_sample_t sample = make_sample(0ULL, 0.0F, 1.0F);
    /* 静止点不完成计数。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 主向旋转触发 PRIMARY 第一稳定点。 */
    sample = make_sample(40ULL, 80.0F, 1.05F);
    /* 第一主向点不完成周期。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 下一点由检测器保持 PRIMARY。 */
    sample = make_sample(80ULL, 70.0F, 1.04F);
    /* 第二主向点只开启周期。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 中间运动不满足反向门槛。 */
    for (uint64_t time_ms = 120ULL; time_ms < 400ULL; time_ms += 40ULL) {
        /* 使用低于静止角速度门槛的点等待回摆。 */
        sample = make_sample(time_ms, 10.0F, 1.0F);
        /* 这些点不完成周期。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    }
    /* 反向旋转触发 SECONDARY。 */
    sample = make_sample(400ULL, -75.0F, 1.02F);
    /* 第一回向点不完成周期。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 保持 SECONDARY 第二点。 */
    sample = make_sample(440ULL, -60.0F, 1.01F);
    /* 第二回向点仍未回到基线。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 等到完整周期超过深蹲最短 600 ms。 */
    for (uint64_t time_ms = 480ULL; time_ms < 720ULL; time_ms += 40ULL) {
        /* 轻微运动保持非静止，防止提前闭合。 */
        sample = make_sample(time_ms, 30.0F, 1.0F);
        /* 周期仍未完成。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    }
    /* 稳定点触发 REST 第一稳定点。 */
    sample = make_sample(720ULL, 0.0F, 1.0F);
    /* 第一 REST 点只进入候选。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 第二个真实 REST 点确认物理端点并发布第一条领域相位。 */
    sample = make_sample(760ULL, 0.0F, 1.0F);
    /* 领域层仍等待同一已确认相位的接口保持点。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 第三个输入点只承载已由两个原始点确认的 REST 保持事件。 */
    sample = make_sample(800ULL, 0.0F, 1.0F);
    /* 保持事件必须让领域层完成一次深蹲。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 检查完成标志。 */
    CHECK(completed);
    /* 检查计数器只增加一次。 */
    CHECK(counter.total_repetitions == 1ULL);
    /* 测试通过。 */
    return 0;
}

/* 验证连续开合跳不需要整机静止：回到主方向即闭合上一周期，并可继续下一次。 */
static int test_continuous_jumping_jack_cycles(void)
{
    /* 创建开合跳腕部方向检测器。 */
    motion_phase_detector_t detector;
    /* 创建开合跳两相位重复计数器。 */
    fitness_rep_counter_t counter;
    /* 保存当前点是否完成一次动作。 */
    bool completed = false;
    /* 初始化开合跳相位检测器。 */
    CHECK(motion_phase_init(&detector, FITNESS_ACTION_JUMPING_JACK) == MOTION_PHASE_OK);
    /* 初始化开合跳重复计数器，最短完整周期固定 400 ms。 */
    CHECK(fitness_rep_counter_init(&counter, FITNESS_ACTION_JUMPING_JACK) == FITNESS_STATUS_OK);

    /* 首个 1 g 静止点只建立严格 25 Hz 时间线。 */
    motion_phase_sample_t sample = make_sample(0ULL, 0.0F, 1.0F);
    /* 首点不得完成动作。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 第一次手臂张开沿正方向旋转，发布 PRIMARY 第一证据点。 */
    sample = make_sample(40ULL, 80.0F, 1.25F);
    /* 第一证据点不得提前计数。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 保持正方向，检测器发布 PRIMARY 第二稳定点。 */
    sample = make_sample(80ULL, 70.0F, 1.20F);
    /* PRIMARY 只开启周期。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 保持 25 Hz 连续时间线，等待手臂到达张开端点。 */
    for (uint64_t time_ms = 120ULL; time_ms < 240ULL; time_ms += 40ULL) {
        /* 低于方向门槛的同向运动不产生新相位，1.30 g 防止旧实现误判 REST。 */
        sample = make_sample(time_ms, 12.0F, 1.30F);
        /* 过渡点只维持连续性。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    }
    /* 手臂合拢沿相反方向旋转，发布 SECONDARY 第一证据点。 */
    sample = make_sample(240ULL, -82.0F, 1.35F);
    /* 到达另一端仍未闭合完整周期。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 保持反方向，发布 SECONDARY 第二稳定点。 */
    sample = make_sample(280ULL, -72.0F, 1.30F);
    /* SECONDARY 只把计数器推进到等待返回。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 保持连续采样并等待下一次正向启动，过程不提供整机静止条件。 */
    for (uint64_t time_ms = 320ULL; time_ms < 480ULL; time_ms += 40ULL) {
        /* 低幅反向运动和 1.30 g 支持力均不满足旧 REST 条件。 */
        sample = make_sample(time_ms, -12.0F, 1.30F);
        /* 过渡点不得计数。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    }
    /* 不停顿地开始下一次张开；正向迟滞越界应发布上一周期 REST。 */
    sample = make_sample(480ULL, 78.0F, 1.28F);
    /* REST 第一稳定点不立即计数。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 连续正向点是第二个真实端点，只确认并发布第一条 REST。 */
    sample = make_sample(520ULL, 68.0F, 1.24F);
    /* 领域层尚未收到保持点。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 下一输入只承载已确认 REST 的接口保持事件。 */
    sample = make_sample(560ULL, 75.0F, 1.22F);
    /* 保持事件必须完成第一周期且周期时长不短于 400 ms。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 检查第一周期完成标志。 */
    CHECK(completed);
    /* 检查累计恰好为一。 */
    CHECK(counter.total_repetitions == 1ULL);

    /* 连续闭合端点已经作为第二周期起点；后续正向点只维持回程前运动。 */
    sample = make_sample(600ULL, 65.0F, 1.18F);
    /* 第二周期已开启但未到达反向端点。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 保持第二周期 25 Hz 连续时间线。 */
    for (uint64_t time_ms = 640ULL; time_ms < 800ULL; time_ms += 40ULL) {
        /* 同向低幅运动不跨越回向门槛。 */
        sample = make_sample(time_ms, 12.0F, 1.28F);
        /* 过渡点不完成动作。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    }
    /* 第二次合拢进入反方向。 */
    sample = make_sample(800ULL, -80.0F, 1.32F);
    /* SECONDARY 第一证据点不计数。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 保持 SECONDARY 第二稳定点。 */
    sample = make_sample(840ULL, -70.0F, 1.27F);
    /* 状态推进成功但仍未完成。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 等待下一次正向时继续输入连续反向低幅点。 */
    for (uint64_t time_ms = 880ULL; time_ms < 1040ULL; time_ms += 40ULL) {
        /* 保持非静止支持力，确保测试不借助旧 REST 条件。 */
        sample = make_sample(time_ms, -12.0F, 1.28F);
        /* 过渡点不计数。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    }
    /* 再次直接进入正向，闭合第二周期。 */
    sample = make_sample(1040ULL, 76.0F, 1.26F);
    /* REST 第一证据点不计数。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 第二个真实正向点确认 REST，但领域层仍等待接口保持事件。 */
    sample = make_sample(1080ULL, 66.0F, 1.20F);
    /* 当前点只发布第一条已确认 REST。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 下一点承载 REST 保持事件。 */
    sample = make_sample(1120ULL, 60.0F, 1.18F);
    /* 本点必须完成第二次。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 检查第二周期完成标志。 */
    CHECK(completed);
    /* 连续两次只计两次，没有重复。 */
    CHECK(counter.total_repetitions == 2ULL);
    /* 测试通过。 */
    return 0;
}

/* 验证一次会话固定主投影轴：十次开合跳即使交叉轴变化也不能反复重学后漏计。 */
static int test_jumping_jack_keeps_session_projection_axis(void)
{
    /* 创建开合跳相位检测器。 */
    motion_phase_detector_t detector;
    /* 创建开合跳重复计数器。 */
    fitness_rep_counter_t counter;
    /* 保存当前样本是否闭合一次完整开合跳。 */
    bool completed = false;
    /* 初始化相位检测器；首次明显运动将建立本会话唯一主方向。 */
    CHECK(motion_phase_init(&detector, FITNESS_ACTION_JUMPING_JACK) == MOTION_PHASE_OK);
    /* 初始化开合跳计数器。 */
    CHECK(fitness_rep_counter_init(&counter, FITNESS_ACTION_JUMPING_JACK) == FITNESS_STATUS_OK);
    /* 首个静止点只建立严格单调时间线。 */
    motion_phase_sample_t sample = make_vector_sample(0ULL, 0.0F, 0.0F, 0.0F, 1.0F);
    /* 静止点不得计数。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);

    /* 连续生成十个 600 ms 周期；25 Hz 下每点间隔 40 ms。 */
    for (uint64_t cycle = 0ULL; cycle < 10ULL; ++cycle) {
        /* 每个周期起点按 600 ms 平移，保证计数不应期和最短周期均满足。 */
        const uint64_t base_ms = cycle * 600ULL;
        /* 首周期只沿 x 轴建立主方向；后九周期加入强 y 轴干扰但保留正 x 主向。 */
        const float primary_x_dps = (cycle == 0ULL) ? 80.0F : 35.0F;
        /* 交叉轴正负交替，模拟腕表姿态和手臂轨迹变化，禁止把它误学成新主轴。 */
        const float cross_y_dps = (cycle == 0ULL) ? 0.0F : ((cycle % 2ULL) == 0ULL ? 100.0F : -100.0F);
        /* 输入 PRIMARY 第一证据点。 */
        sample = make_vector_sample(base_ms + 40ULL, primary_x_dps, cross_y_dps, 0.0F, 1.25F);
        /* 第一证据点只建立或越过主向门槛。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        /* 输入连续 PRIMARY 第二点，满足两点稳定合同。 */
        sample = make_vector_sample(base_ms + 80ULL, primary_x_dps + 5.0F, cross_y_dps, 0.0F, 1.22F);
        /* PRIMARY 只开启周期，不立即增加次数。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        /* 主向与回向之间输入三个低幅过渡点，防止测试依赖相邻尖峰。 */
        for (uint64_t offset_ms = 120ULL; offset_ms < 240ULL; offset_ms += 40ULL) {
            /* 低幅 x 方向和动态支持力均不形成离散相位。 */
            sample = make_vector_sample(base_ms + offset_ms, 10.0F, 0.0F, 0.0F, 1.30F);
            /* 过渡点不得计数。 */
            CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        }
        /* 沿最初 x 主轴输入反向 SECONDARY；y 轴仅保留小扰动。 */
        sample = make_vector_sample(base_ms + 240ULL, -82.0F, 8.0F, 0.0F, 1.34F);
        /* 第一回向点不完成周期。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        /* 第二回向点满足稳定合同。 */
        sample = make_vector_sample(base_ms + 280ULL, -72.0F, 6.0F, 0.0F, 1.30F);
        /* SECONDARY 只推进到等待返回。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        /* 输入四个低幅反向过渡点，不提供整机静止条件。 */
        for (uint64_t offset_ms = 320ULL; offset_ms < 480ULL; offset_ms += 40ULL) {
            /* 加速度保持 1.28 g，确保旧静止闭合分支不能帮助测试。 */
            sample = make_vector_sample(base_ms + offset_ms, -10.0F, 0.0F, 0.0F, 1.28F);
            /* 过渡点不得提前计数。 */
            CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        }
        /* 返回最初正 x 方向，关闭当前完整周期。 */
        sample = make_vector_sample(base_ms + 480ULL, 74.0F, 0.0F, 0.0F, 1.26F);
        /* REST 第一稳定点不立即增加次数。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        /* 第二个真实返回点确认端点并发布第一条 REST。 */
        sample = make_vector_sample(base_ms + 520ULL, 66.0F, 0.0F, 0.0F, 1.22F);
        /* 当前只确认物理端点，领域层仍等待保持事件。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        /* 第三个输入点承载已确认 REST 的接口保持事件，不参与端点判定。 */
        sample = make_vector_sample(base_ms + 560ULL, 20.0F, 0.0F, 0.0F, 1.18F);
        /* 保持事件必须完成本周期。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        /* 当前周期必须恰好产生一次完成事件。 */
        CHECK(completed);
        /* 累计次数必须和已生成周期数一致，禁止中间漏计。 */
        CHECK(counter.total_repetitions == cycle + 1ULL);
    }

    /* 十个周期全部完成且没有重复计数。 */
    CHECK(counter.total_repetitions == 10ULL);
    /* 测试通过。 */
    return 0;
}

/* 验证固定主单轴能避开三轴点积抵消：首姿态与后续姿态正交时仍完整计十次。 */
static int test_jumping_jack_fixed_axis_avoids_projection_cancellation(void)
{
    /* 创建开合跳相位检测器。 */
    motion_phase_detector_t detector;
    /* 创建开合跳重复计数器。 */
    fitness_rep_counter_t counter;
    /* 保存当前样本是否闭合一次完整开合跳。 */
    bool completed = false;
    /* 初始化开合跳检测器。 */
    CHECK(motion_phase_init(&detector, FITNESS_ACTION_JUMPING_JACK) == MOTION_PHASE_OK);
    /* 固定 gy 为首窗峰峰值最大的主周期轴，禁止后续 gx/gz 抵消 gy。 */
    CHECK(motion_phase_configure_jumping_jack_axis(&detector, 1U) == MOTION_PHASE_OK);
    /* 初始化开合跳两相位重复计数器。 */
    CHECK(fitness_rep_counter_init(&counter, FITNESS_ACTION_JUMPING_JACK) == FITNESS_STATUS_OK);
    /* 首个静止点建立严格单调时间线。 */
    motion_phase_sample_t sample = make_vector_sample(0ULL, 0.0F, 0.0F, 0.0F, 1.0F);
    /* 静止点不得产生次数。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);

    /* 连续生成十个 600 ms 周期，gy 保持真实周期，gx/gz 专门构造三维点积抵消。 */
    for (uint64_t cycle = 0ULL; cycle < 10ULL; ++cycle) {
        /* 每个周期基准时间保证最短周期和 300 ms 不应期均满足。 */
        const uint64_t base_ms = cycle * 600ULL;
        /* 正向两点：首点向量 [60,100,80]，后续向量 [-60,100,-80] 与首向量点积为零。 */
        sample = make_vector_sample(base_ms + 40ULL, cycle == 0ULL ? 60.0F : -60.0F, 100.0F, cycle == 0ULL ? 80.0F : -80.0F, 1.24F);
        /* 第一正峰点只开启或维持周期。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        /* 第二正峰点满足两点相位确认。 */
        sample = make_vector_sample(base_ms + 80ULL, cycle == 0ULL ? 58.0F : -58.0F, 92.0F, cycle == 0ULL ? 76.0F : -76.0F, 1.20F);
        /* 正峰不得提前完成次数。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        /* 三个低幅过渡点分开正负峰，避免测试依赖相邻尖峰。 */
        for (uint64_t offset_ms = 120ULL; offset_ms < 240ULL; offset_ms += 40ULL) {
            /* 过渡点三个轴均低于相位门槛。 */
            sample = make_vector_sample(base_ms + offset_ms, 8.0F, 10.0F, -8.0F, 1.28F);
            /* 过渡点不得产生次数。 */
            CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        }
        /* 负向两点：向量 [60,-100,80] 同样与首姿态点积为零，但 gy 明确越过负门槛。 */
        sample = make_vector_sample(base_ms + 240ULL, 60.0F, -100.0F, 80.0F, 1.32F);
        /* 第一负峰点只推进回向候选。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        /* 第二负峰点满足两点相位确认。 */
        sample = make_vector_sample(base_ms + 280ULL, 58.0F, -92.0F, 76.0F, 1.28F);
        /* 负峰不得直接完成周期。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        /* 四个低幅回程点保持连续运动，不提供整机静止捷径。 */
        for (uint64_t offset_ms = 320ULL; offset_ms < 480ULL; offset_ms += 40ULL) {
            /* gy 低幅负值仍低于相位门槛。 */
            sample = make_vector_sample(base_ms + offset_ms, -8.0F, -10.0F, 8.0F, 1.26F);
            /* 回程点不得提前计数。 */
            CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        }
        /* 返回正 gy 第一证据点，闭合本周期并同时开启下一周期。 */
        sample = make_vector_sample(base_ms + 480ULL, -60.0F, 100.0F, -80.0F, 1.24F);
        /* REST 第一证据点尚不满足下游两点确认。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        /* 返回正 gy 第二个真实点确认端点。 */
        sample = make_vector_sample(base_ms + 520ULL, -58.0F, 92.0F, -76.0F, 1.20F);
        /* 当前只发布第一条已确认 REST。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        /* 下一点承载已确认端点的接口保持事件。 */
        sample = make_vector_sample(base_ms + 560ULL, 0.0F, 20.0F, 0.0F, 1.16F);
        /* 本点必须完成当前周期。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
        /* 每个合成周期必须恰好累计一次。 */
        CHECK(completed && (counter.total_repetitions == cycle + 1ULL));
    }

    /* 十个三轴抵消周期必须全部计入。 */
    CHECK(counter.total_repetitions == 10ULL);
    /* 测试通过。 */
    return 0;
}

/* 验证腕戴跳跃深蹲在加速度始终高于腾空门时，完整正反腕部周期仍能计一次。 */
static int test_wrist_jump_cycle_without_flight(void)
{
    /* 创建跳跃深蹲相位检测器和计数器。 */
    motion_phase_detector_t detector;
    /* 保存统一腕部周期计数器。 */
    fitness_rep_counter_t counter;
    /* 保存完成标志。 */
    bool completed = false;
    /* 初始化跳跃深蹲腕部路径。 */
    CHECK(motion_phase_init(&detector, FITNESS_ACTION_JUMPING_SQUAT) == MOTION_PHASE_OK);
    /* 初始化统一两相位计数器。 */
    CHECK(fitness_rep_counter_init(&counter, FITNESS_ACTION_JUMPING_SQUAT) == FITNESS_STATUS_OK);
    /* 首点稳定。 */
    motion_phase_sample_t sample = make_sample(0ULL, 0.0F, 1.0F);
    /* 首点不计数。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 主向腕部转动；加速度保持 1.05 g，明确没有低于 0.78 g 的腾空点。 */
    sample = make_sample(40ULL, 80.0F, 1.05F);
    /* PRIMARY 第一稳定点。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 保持主向运动，让离散相位满足连续两点合同。 */
    sample = make_sample(80ULL, 70.0F, 1.02F);
    /* PRIMARY 第二稳定点只开启周期。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 120 ms 内插入低于方向门但高于静止角速度的连续点，避免合成数据触发真实断流保护。 */
    sample = make_sample(160ULL, 25.0F, 1.00F);
    /* 中间点不能推进相位。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 继续保持合法时间线，尚未到达反向端点。 */
    sample = make_sample(240ULL, 25.0F, 1.00F);
    /* 中间点仍不得计数。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 反向腕部转动确认动作另一端；支持力仍约 1 g。 */
    sample = make_sample(320ULL, -80.0F, 0.98F);
    /* SECONDARY 第一稳定点。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 保持反向转动。 */
    sample = make_sample(360ULL, -70.0F, 1.01F);
    /* SECONDARY 第二稳定点仍不计数。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 用 25 deg/s 连续点保持时间线且不满足 22 deg/s 静止门，防止尚未回到主向时提前闭合。 */
    sample = make_sample(440ULL, 25.0F, 1.00F);
    /* 回程中间点不形成 REST。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 第二个回程中间点保持相同合同。 */
    sample = make_sample(520ULL, 25.0F, 1.00F);
    /* 中间点不计数。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 第三个回程中间点继续维持小于方向阈值的正向速度。 */
    sample = make_sample(600ULL, 25.0F, 1.00F);
    /* 尚未到达主向端点。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 第四个中间点使慢速回程保持连续，且尚未满足 800 ms 跳跃深蹲周期。 */
    sample = make_sample(680ULL, 25.0F, 1.00F);
    /* 当前点仍不闭合周期。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 继续输入三个 80 ms 间隔的低幅回程点，使合成周期覆盖当前 800 ms 最短门。 */
    for (uint64_t time_ms = 760ULL; time_ms <= 920ULL; time_ms += 80ULL) {
        /* 25 deg/s 高于静止门但低于自适应方向门，不会提前闭合。 */
        sample = make_sample(time_ms, 25.0F, 1.00F);
        /* 回程点只延续时间线，不发布相位。 */
        CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    }
    /* 回到主向起点闭合完整周期；从已确认 PRIMARY 到最终保持点超过 800 ms。 */
    sample = make_sample(960ULL, 80.0F, 1.06F);
    /* REST 第一稳定点。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 第二个真实返回点确认 REST。 */
    sample = make_sample(1000ULL, 70.0F, 1.03F);
    /* 当前只发布第一条已确认相位。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 下一输入承载 REST 接口保持事件。 */
    sample = make_sample(1040ULL, 20.0F, 1.01F);
    /* 保持事件必须计数。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 检查完成标志。 */
    CHECK(completed);
    /* 检查只计一次。 */
    CHECK(counter.total_repetitions == 1ULL);
    /* 测试通过。 */
    return 0;
}

/* 验证 walk 三点局部峰只输出一次，平坦信号不误报。 */
static int test_step_peak(void)
{
    /* 创建 walk 检测器。 */
    motion_phase_detector_t detector;
    /* 保存观测。 */
    motion_phase_observation_t observation;
    /* 初始化步峰路径。 */
    CHECK(motion_phase_init(&detector, FITNESS_ACTION_WALK) == MOTION_PHASE_OK);
    /* 两个 1 g 点建立基线和历史。 */
    motion_phase_sample_t sample = make_sample(0ULL, 0.0F, 1.0F);
    /* 首点不得报步峰。 */
    CHECK(motion_phase_push(&detector, &sample, &observation) == MOTION_PHASE_OK);
    /* 检查首点无步峰。 */
    CHECK(!observation.step_peak);
    /* 第二点继续建立历史。 */
    sample = make_sample(40ULL, 0.0F, 1.0F);
    /* 第二点处理成功。 */
    CHECK(motion_phase_push(&detector, &sample, &observation) == MOTION_PHASE_OK);
    /* 第二点无步峰。 */
    CHECK(!observation.step_peak);
    /* 第三点制造 1.35 g 脚步冲击。 */
    sample = make_sample(80ULL, 0.0F, 1.35F);
    /* 峰顶尚需右侧点确认。 */
    CHECK(motion_phase_push(&detector, &sample, &observation) == MOTION_PHASE_OK);
    /* 峰顶本点不提前报。 */
    CHECK(!observation.step_peak);
    /* 右侧回落点确认上一点为局部峰。 */
    sample = make_sample(120ULL, 0.0F, 1.0F);
    /* 回落点处理成功。 */
    CHECK(motion_phase_push(&detector, &sample, &observation) == MOTION_PHASE_OK);
    /* 必须恰好报一个步峰。 */
    CHECK(observation.step_peak);
    /* 后续平坦点不重复报峰。 */
    sample = make_sample(160ULL, 0.0F, 1.0F);
    /* 平坦点处理成功。 */
    CHECK(motion_phase_push(&detector, &sample, &observation) == MOTION_PHASE_OK);
    /* 不得重复。 */
    CHECK(!observation.step_peak);
    /* 测试通过。 */
    return 0;
}

/* 验证慢速单步形成宽峰和次级波瓣时，必须等信号真正回落后才能重新武装。 */
static int test_step_wide_peak_requires_low_rearm(void)
{
    /* detector 使用 walk 步峰路径；本测试只审计上游峰事件，不依赖下游固定不应期。 */
    motion_phase_detector_t detector;
    /* observation 接收每个 25 Hz 点是否产生步峰。 */
    motion_phase_observation_t observation;
    /* 初始化 walk 检测器并清空慢基线、历史和重新武装状态。 */
    CHECK(motion_phase_init(&detector, FITNESS_ACTION_WALK) == MOTION_PHASE_OK);
    /* time_ms 严格按 40 ms 推进，单位为毫秒。 */
    uint64_t time_ms = 0ULL;
    /* peak_count 累计上游报告的步峰数；两个物理宽峰应恰好得到两次。 */
    uint32_t peak_count = 0U;
    /* 前 20 点保持 1 g，建立稳定慢基线且不得产生步峰。 */
    for (uint32_t point = 0U; point < 20U; ++point) {
        /* sample 只在 az 写入 1 g，模拟佩戴者静止站立。 */
        motion_phase_sample_t sample = make_sample(time_ms, 0.0F, 1.0F);
        /* 平坦点必须被正常消费。 */
        CHECK(motion_phase_push(&detector, &sample, &observation) == MOTION_PHASE_OK);
        /* 静止基线不能触发步峰。 */
        CHECK(!observation.step_peak);
        /* 推进一个采样周期。 */
        time_ms += MOTION_PHASE_SAMPLE_PERIOD_MS;
    }
    /* wide_step_g 模拟一个慢步的宽冲击；中间次级波瓣仍高于低回落门，不是第二步。 */
    static const float wide_step_g[] = {
        1.10F, 1.24F, 1.40F, 1.28F, 1.23F, 1.24F, 1.25F,
        1.27F, 1.29F, 1.35F, 1.26F, 1.18F, 1.08F, 1.01F, 1.00F
    };
    /* 连续输入两个物理步；每一步内部有两个局部峰，但两步之间完整回到基线。 */
    for (uint32_t physical_step = 0U; physical_step < 2U; ++physical_step) {
        /* 遍历当前宽峰全部点，保持同一人的慢动作形状。 */
        for (uint32_t point = 0U;
             point < (uint32_t)(sizeof(wide_step_g) / sizeof(wide_step_g[0]));
             ++point) {
            /* sample 的加速度单位为 g；角速度保持零以隔离步峰算法。 */
            motion_phase_sample_t sample = make_sample(time_ms, 0.0F, wide_step_g[point]);
            /* 当前点必须正常推进慢基线和峰值迟滞。 */
            CHECK(motion_phase_push(&detector, &sample, &observation) == MOTION_PHASE_OK);
            /* 仅在检测器公开真实步峰时累计。 */
            if (observation.step_peak) {
                /* 记录一个上游步事件。 */
                peak_count += 1U;
            }
            /* 推进一个采样周期。 */
            time_ms += MOTION_PHASE_SAMPLE_PERIOD_MS;
        }
        /* 两个物理步之间追加 8 个基线点，明确完成低阈值回落和重新武装。 */
        for (uint32_t rest_point = 0U; rest_point < 8U; ++rest_point) {
            /* 1 g 点表示脚步冲击已经结束。 */
            motion_phase_sample_t sample = make_sample(time_ms, 0.0F, 1.0F);
            /* 回落点必须正常消费。 */
            CHECK(motion_phase_push(&detector, &sample, &observation) == MOTION_PHASE_OK);
            /* 回落过程不得额外产生局部峰。 */
            CHECK(!observation.step_peak);
            /* 推进一个采样周期。 */
            time_ms += MOTION_PHASE_SAMPLE_PERIOD_MS;
        }
    }
    /* 两个慢速宽峰必须恰好对应两步，内部波瓣不能重复计步。 */
    CHECK(peak_count == 2U);
    /* 测试通过。 */
    return 0;
}

/* 验证开合跳流式峰谷检测：一个相邻峰谷只计一次，十个周期不能再乘二。 */
static int test_periodic_pair_counter_counts_ten_acceleration_cycles(void)
{
    /* detector 保存单轴 11+5 均值滤波、局部极值和一对一峰谷状态。 */
    motion_periodic_pair_detector_t detector;
    /* 初始化后累计必须从零开始。 */
    CHECK(motion_periodic_pair_init(&detector) == MOTION_PHASE_OK);
    /* 一个 20 点周期覆盖 +1 g 到 -1 g，相邻周期间隔 0.8 秒。 */
    static const float cycle[20] = {
        0.0F, 0.31F, 0.59F, 0.81F, 0.95F,
        1.0F, 0.95F, 0.81F, 0.59F, 0.31F,
        0.0F, -0.31F, -0.59F, -0.81F, -0.95F,
        -1.0F, -0.95F, -0.81F, -0.59F, -0.31F
    };
    /* time_ms 按 25 Hz 单调推进，单位毫秒。 */
    uint64_t time_ms = 0ULL;
    /* 前 20 点静止用于填满 11+5 两级滤波，不应产生动作。 */
    for (uint32_t point = 0U; point < 20U; ++point) {
        /* accepted 表示当前点是否使单轴累计增加一次。 */
        bool accepted = true;
        /* 静止 0 g 动态分量只建立滤波历史。 */
        CHECK(motion_periodic_pair_push(&detector, time_ms, 0.0F, &accepted) ==
              MOTION_PHASE_OK);
        /* 平坦输入不能产生峰谷配对。 */
        CHECK(!accepted);
        /* 严格推进一个 40 ms 采样周期。 */
        time_ms += MOTION_PHASE_SAMPLE_PERIOD_MS;
    }
    /* 连续输入十个完整周期，模拟用户做十次开合跳。 */
    for (uint32_t repetition = 0U; repetition < 10U; ++repetition) {
        /* 每个周期逐点输入，滤波后的峰谷仍保持一一对应。 */
        for (uint32_t point = 0U; point < 20U; ++point) {
            /* 保存当前点是否完成一组相邻峰谷。 */
            bool accepted = false;
            /* 输入单位为 g；该单轴信号代表腕部加速度的周期分量。 */
            CHECK(motion_periodic_pair_push(
                      &detector,
                      time_ms,
                      cycle[point],
                      &accepted) == MOTION_PHASE_OK);
            /* 当前点无论是否闭合周期都只推进一次时间线。 */
            time_ms += MOTION_PHASE_SAMPLE_PERIOD_MS;
        }
    }
    /* 追加半个上升段确认最后一个谷后的回升趋势，不构成额外峰谷对。 */
    for (uint32_t point = 0U; point < 10U; ++point) {
        /* accepted 保存尾部确认点结果。 */
        bool accepted = false;
        /* 继续输入前半周期，让滤波延迟中的第十个谷被右邻点确认。 */
        CHECK(motion_periodic_pair_push(
                  &detector,
                  time_ms,
                  cycle[point],
                  &accepted) == MOTION_PHASE_OK);
        /* 推进下一时间戳。 */
        time_ms += MOTION_PHASE_SAMPLE_PERIOD_MS;
    }
    /* 十个峰谷对必须得到十次，禁止旧逻辑只计半数或最终乘二。 */
    CHECK(detector.total_pairs == 10ULL);
    /* 测试通过。 */
    return 0;
}

/* 验证采样间断只清空未完成峰谷，已经确认的累计次数必须保留。 */
static int test_periodic_pair_gap_preserves_completed_total(void)
{
    /* detector 保存独立单轴在线状态。 */
    motion_periodic_pair_detector_t detector;
    /* 初始化检测器。 */
    CHECK(motion_periodic_pair_init(&detector) == MOTION_PHASE_OK);
    /* accepted 保存当前点是否闭合峰谷。 */
    bool accepted = false;
    /* 首点建立时间线和均值滤波。 */
    CHECK(motion_periodic_pair_push(&detector, 0ULL, 0.0F, &accepted) ==
          MOTION_PHASE_OK);
    /* 人工保存三个已确认动作，模拟暂停前的权威轴累计。 */
    detector.total_pairs = 3ULL;
    /* 200 ms 间断超过 120 ms，必须返回显式重置。 */
    CHECK(motion_periodic_pair_push(&detector, 200ULL, 0.5F, &accepted) ==
          MOTION_PHASE_GAP_RESET);
    /* 间断点本身不计数。 */
    CHECK(!accepted);
    /* 已确认的三个配对必须保留，避免恢复后中位数回退并重复发布旧次数。 */
    CHECK(detector.total_pairs == 3ULL);
    /* 测试通过。 */
    return 0;
}

/* 返回三个单轴累计的中位数，复现训练引擎的鲁棒融合口径。 */
static uint64_t median_three_u64(
    const uint64_t a,
    const uint64_t b,
    const uint64_t c)
{
    /* b 位于 a 与 c 之间时返回 b。 */
    if (((a <= b) && (b <= c)) || ((c <= b) && (b <= a))) {
        /* b 是中位数。 */
        return b;
    }
    /* a 位于 b 与 c 之间时返回 a。 */
    if (((b <= a) && (a <= c)) || ((c <= a) && (a <= b))) {
        /* a 是中位数。 */
        return a;
    }
    /* 剩余关系下 c 是中位数。 */
    return c;
}

/* 读取一份七列真数据的第 26..433 行，并用生产 C 峰谷状态机核对人工次数。 */
static int test_periodic_pair_real_dataset(
    const char *path,
    const uint64_t expected_repetitions)
{
    /* path 必须由 PowerShell 运行器传入绝对路径。 */
    CHECK(path != NULL);
    /* 以只读文本方式打开原数据，测试不得修改用户数据集。 */
    FILE *file = fopen(path, "r");
    /* 文件必须存在且可读。 */
    CHECK(file != NULL);
    /* detectors 按 ax、ay、az 保存三个生产检测器。 */
    motion_periodic_pair_detector_t detectors[3];
    /* 三个轴必须从零独立初始化。 */
    for (uint8_t axis = 0U; axis < 3U; ++axis) {
        /* 建立本轴 11+5 均值和峰谷状态。 */
        CHECK(motion_periodic_pair_init(&detectors[axis]) == MOTION_PHASE_OK);
    }
    /* row_fields 保存一行七个十进制整数，顺序为三轴角速度、三轴加速度和标签。 */
    long row_fields[7];
    /* active_index 保存权威 408 点动作段内的零基序号。 */
    uint32_t active_index = 0U;
    /* 只需读取到原 MATLAB 切片右边界第 433 行。 */
    for (uint32_t row_index = 0U; row_index < 433U; ++row_index) {
        /* 严格读取逗号分隔七列；任一缺列都说明数据格式漂移。 */
        const int parsed = fscanf(
            file,
            " %ld , %ld , %ld , %ld , %ld , %ld , %ld ,",
            &row_fields[0],
            &row_fields[1],
            &row_fields[2],
            &row_fields[3],
            &row_fields[4],
            &row_fields[5],
            &row_fields[6]);
        /* 每行必须恰好七列。 */
        CHECK(parsed == 7);
        /* 前 25 行属于训练脚本明确排除的边界，只读但不送入计数器。 */
        if (row_index < 25U) {
            /* 继续读取下一行。 */
            continue;
        }
        /* 当前动作段时间按 25 Hz 生成，单位 ms。 */
        const uint64_t time_ms =
            (uint64_t)active_index * (uint64_t)MOTION_PHASE_SAMPLE_PERIOD_MS;
        /* 三个加速度 raw 通道分别除以 4096 raw/g，转换为生产 API 的 g。 */
        for (uint8_t axis = 0U; axis < 3U; ++axis) {
            /* accepted 只用于确认单轴接口输出地址有效；此历史样本没有跨轴事件时间人工标注。 */
            bool accepted = false;
            /* 第 4..6 列对应 ax、ay、az。 */
            const float acceleration_g =
                (float)row_fields[3U + axis] / 4096.0F;
            /* 真数据必须逐点被生产 C 状态机接受。 */
            CHECK(motion_periodic_pair_push(
                      &detectors[axis],
                      time_ms,
                      acceleration_g,
                      &accepted) == MOTION_PHASE_OK);
        }
        /* 动作段有效点数增加一。 */
        active_index += 1U;
    }
    /* 关闭只读文件句柄。 */
    CHECK(fclose(file) == 0);
    /* 权威动作段必须精确为 408 点。 */
    CHECK(active_index == 408U);
    /* 历史离线标注用三轴累计中位数核对底层峰谷不漂移；生产权威融合另由训练引擎测试。 */
    const uint64_t fused_repetitions = median_three_u64(
        detectors[0].total_pairs,
        detectors[1].total_pairs,
        detectors[2].total_pairs);
    /* 底层三轴峰谷累计必须匹配旧人工视觉核数，防止滤波/配对器回归。 */
    CHECK(fused_repetitions == expected_repetitions);
    /* 测试通过。 */
    return 0;
}

/* 验证时间倒退、长间断和非有限输入不会拼接旧周期。 */
static int test_boundaries(void)
{
    /* 创建挥手检测器。 */
    motion_phase_detector_t detector;
    /* 保存输出。 */
    motion_phase_observation_t observation;
    /* 初始化合法动作。 */
    CHECK(motion_phase_init(&detector, FITNESS_ACTION_WAVE) == MOTION_PHASE_OK);
    /* 首点建立时间。 */
    motion_phase_sample_t sample = make_sample(100ULL, 0.0F, 1.0F);
    /* 首点成功。 */
    CHECK(motion_phase_push(&detector, &sample, &observation) == MOTION_PHASE_OK);
    /* 重复时间戳必须拒绝。 */
    CHECK(motion_phase_push(&detector, &sample, &observation) == MOTION_PHASE_ERR_TIMESTAMP);
    /* 300 ms 间断超过 120 ms，必须重置。 */
    sample = make_sample(400ULL, 0.0F, 1.0F);
    /* 返回显式重置状态。 */
    CHECK(motion_phase_push(&detector, &sample, &observation) == MOTION_PHASE_GAP_RESET);
    /* NaN 轴必须拒绝且不修改时间。 */
    sample = make_sample(440ULL, 0.0F, 1.0F);
    /* 注入非法 gx。 */
    sample.axis[0] = NAN;
    /* 检查参数错误。 */
    CHECK(motion_phase_push(&detector, &sample, &observation) == MOTION_PHASE_ERR_ARGUMENT);
    /* 空指针初始化必须拒绝。 */
    CHECK(motion_phase_init(NULL, FITNESS_ACTION_SQUAT) == MOTION_PHASE_ERR_ARGUMENT);
    /* 越界动作必须拒绝。 */
    CHECK(motion_phase_init(&detector, FITNESS_ACTION_COUNT) == MOTION_PHASE_ERR_ARGUMENT);
    /* 测试通过。 */
    return 0;
}

/* 主函数串行运行全部测试，任一失败立即返回非零。 */
int main(int argc, char **argv)
{
    /* 公开克隆可不传外部数据；维护者真板门必须一次传齐 scy1、scy2、scy3 三份绝对路径。 */
    CHECK((argc == 1) || (argc == 4));
    /* 验证两相位完整周期。 */
    CHECK(test_two_phase_cycle() == 0);
    /* 验证开合跳连续周期不依赖整机静止。 */
    CHECK(test_continuous_jumping_jack_cycles() == 0);
    /* 验证开合跳会话固定主投影轴，交叉轴变化不造成十次中的漏计。 */
    CHECK(test_jumping_jack_keeps_session_projection_axis() == 0);
    /* 验证首窗固定主单轴可消除三轴点积抵消漏计。 */
    CHECK(test_jumping_jack_fixed_axis_avoids_projection_cancellation() == 0);
    /* 验证腕戴跳跃无需腾空支持力也能按完整正反周期计数。 */
    CHECK(test_wrist_jump_cycle_without_flight() == 0);
    /* 验证 walk 局部峰。 */
    CHECK(test_step_peak() == 0);
    /* 验证慢速宽峰内部波瓣不会重复计步。 */
    CHECK(test_step_wide_peak_requires_low_rearm() == 0);
    /* 验证相邻峰谷在线计数十个周期。 */
    CHECK(test_periodic_pair_counter_counts_ten_acceleration_cycles() == 0);
    /* 验证采样间断不清除已完成峰谷累计。 */
    CHECK(test_periodic_pair_gap_preserves_completed_total() == 0);
    /* 仅当三份外部数据齐全时执行人工视觉核数回放，公开自包含门不依赖仓库外文件。 */
    if (argc == 4) {
        /* scy1 用户视觉核数为 16。 */
        CHECK(test_periodic_pair_real_dataset(argv[1], 16ULL) == 0);
        /* scy2 用户视觉核数为 15。 */
        CHECK(test_periodic_pair_real_dataset(argv[2], 15ULL) == 0);
        /* scy3 用户视觉核数为 16。 */
        CHECK(test_periodic_pair_real_dataset(argv[3], 16ULL) == 0);
    }
    else {
        /* 输出稳定跳过标记，说明合成回归已执行而外部数据门未提供。 */
        (void)printf("motion_phase external replay: SKIP (dataset not supplied)\n");
    }
    /* 验证异常边界。 */
    CHECK(test_boundaries() == 0);
    /* 输出稳定成功标记和断言总数。 */
    (void)printf("motion_phase host tests: PASS (%u assertions)\n", g_assertions);
    /* 返回成功。 */
    return 0;
}
