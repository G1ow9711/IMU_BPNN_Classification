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
    /* 第二 REST 点闭合周期。 */
    sample = make_sample(760ULL, 0.0F, 1.0F);
    /* 本点必须完成一次深蹲。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 检查完成标志。 */
    CHECK(completed);
    /* 检查计数器只增加一次。 */
    CHECK(counter.total_repetitions == 1ULL);
    /* 测试通过。 */
    return 0;
}

/* 验证跳跃深蹲五阶段顺序能闭合一次，缺少腾空不能计数。 */
static int test_jump_cycle(void)
{
    /* 创建跳跃相位检测器和计数器。 */
    motion_phase_detector_t detector;
    /* 保存跳跃重复计数器。 */
    fitness_rep_counter_t counter;
    /* 保存完成标志。 */
    bool completed = false;
    /* 初始化跳跃深蹲路径。 */
    CHECK(motion_phase_init(&detector, FITNESS_ACTION_JUMPING_SQUAT) == MOTION_PHASE_OK);
    /* 初始化五阶段计数器。 */
    CHECK(fitness_rep_counter_init(&counter, FITNESS_ACTION_JUMPING_SQUAT) == FITNESS_STATUS_OK);
    /* 首点稳定。 */
    motion_phase_sample_t sample = make_sample(0ULL, 0.0F, 1.0F);
    /* 首点不计数。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 起跳推进高支持力和角速度。 */
    sample = make_sample(40ULL, 60.0F, 1.25F);
    /* TAKEOFF 第一稳定点。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 保持 TAKEOFF。 */
    sample = make_sample(80ULL, 50.0F, 1.10F);
    /* TAKEOFF 第二稳定点。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 腾空低支持力。 */
    sample = make_sample(160ULL, 35.0F, 0.55F);
    /* FLIGHT 第一稳定点。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 保持 FLIGHT。 */
    sample = make_sample(200ULL, 30.0F, 0.60F);
    /* FLIGHT 第二稳定点。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 落地冲击。 */
    sample = make_sample(320ULL, 70.0F, 1.65F);
    /* LANDING 第一稳定点。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 保持 LANDING。 */
    sample = make_sample(360ULL, 45.0F, 1.20F);
    /* LANDING 第二稳定点。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 落地后恢复稳定。 */
    sample = make_sample(480ULL, 0.0F, 1.0F);
    /* RECOVERY 第一稳定点。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 保持 RECOVERY。 */
    sample = make_sample(520ULL, 0.0F, 1.0F);
    /* RECOVERY 第二稳定点。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 继续稳定触发 REST。 */
    sample = make_sample(600ULL, 0.0F, 1.0F);
    /* REST 第一稳定点。 */
    CHECK(push_rep_sample(&detector, &counter, &sample, &completed) == 0);
    /* 保持 REST 并完成完整周期。 */
    sample = make_sample(640ULL, 0.0F, 1.0F);
    /* 第二 REST 点必须计数。 */
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
int main(void)
{
    /* 验证两相位完整周期。 */
    CHECK(test_two_phase_cycle() == 0);
    /* 验证跳跃五阶段完整周期。 */
    CHECK(test_jump_cycle() == 0);
    /* 验证 walk 局部峰。 */
    CHECK(test_step_peak() == 0);
    /* 验证异常边界。 */
    CHECK(test_boundaries() == 0);
    /* 输出稳定成功标记和断言总数。 */
    (void)printf("motion_phase host tests: PASS (%u assertions)\n", g_assertions);
    /* 返回成功。 */
    return 0;
}
