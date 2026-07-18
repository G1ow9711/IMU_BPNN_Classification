/* 引入被测训练引擎。 */
#include "workout_engine.h"

/* 引入 NAN 构造异常 logits。 */
#include <math.h>
/* 引入 stdio 输出机器可读摘要。 */
#include <stdio.h>
/* 引入 string.h 清零数组。 */
#include <string.h>

/* 保存断言总数。 */
static unsigned int g_assertions = 0U;

/* 失败时打印表达式和行号并退出当前测试进程。 */
#define CHECK(expression)                                                       \
    do {                                                                        \
        g_assertions += 1U;                                                      \
        if (!(expression)) {                                                    \
            (void)fprintf(stderr, "CHECK failed line=%d: %s\n", __LINE__, #expression); \
            return 1;                                                           \
        }                                                                       \
    } while (0)

/* 构造 11 维高置信 logits；目标类为 +4，其余为 -4。 */
static void make_logits(float logits[WORKOUT_CLASS_COUNT], const uint8_t target)
{
    /* 遍历全部类别设置低分。 */
    for (uint8_t class_index = 0U; class_index < WORKOUT_CLASS_COUNT; ++class_index) {
        /* 目标类别获得高分，其余获得低分。 */
        logits[class_index] = class_index == target ? 4.0F : -4.0F;
    }
}

/* 构造可控强度的 11 维 logits；用于制造累计类别跨越而不修改模型实现。 */
static void make_scaled_logits(
    float logits[WORKOUT_CLASS_COUNT],
    const uint8_t target,
    const float target_logit,
    const float other_logit)
{
    /* 遍历固定 11 类，保证类别顺序与双 M0 输出合同一致。 */
    for (uint8_t class_index = 0U; class_index < WORKOUT_CLASS_COUNT; ++class_index) {
        /* 目标类别写入高分，其余类别写入统一低分。 */
        logits[class_index] = class_index == target ? target_logit : other_logit;
    }
}

/* 构造一维六轴点；gx 单位 deg/s，az 单位 g。 */
static motion_phase_sample_t make_sample(
    const uint64_t time_ms,
    const float gyro_x_dps,
    const float acceleration_z_g)
{
    /* 创建并清零六轴点。 */
    motion_phase_sample_t sample;
    /* 未使用轴固定为零。 */
    (void)memset(&sample, 0, sizeof(sample));
    /* 写入单调毫秒。 */
    sample.monotonic_ms = time_ms;
    /* 写入 gx。 */
    sample.axis[0] = gyro_x_dps;
    /* 写入 az。 */
    sample.axis[5] = acceleration_z_g;
    /* 返回样本。 */
    return sample;
}

/* 连续提交三个高置信窗口并检查锁定。 */
static int lock_action(
    workout_engine_t *engine,
    const fitness_action_t action,
    const uint64_t first_ms)
{
    /* 构造目标 logits。 */
    float logits[WORKOUT_CLASS_COUNT];
    /* 填充高置信目标类。 */
    make_logits(logits, (uint8_t)action);
    /* 保存本次是否锁定。 */
    bool locked = false;
    /* 第一窗口只建立候选。 */
    CHECK(workout_engine_push_inference(engine, logits, first_ms, 0U, &locked) ==
          WORKOUT_STATUS_IGNORED);
    /* 第一窗口不得锁定。 */
    CHECK(!locked);
    /* 第二窗口继续候选。 */
    CHECK(workout_engine_push_inference(engine, logits, first_ms + 480ULL, 0U, &locked) ==
          WORKOUT_STATUS_IGNORED);
    /* 第二窗口不得锁定。 */
    CHECK(!locked);
    /* 第三窗口完成锁定。 */
    CHECK(workout_engine_push_inference(engine, logits, first_ms + 960ULL, 0U, &locked) ==
          WORKOUT_STATUS_OK);
    /* 必须报告本次新锁定。 */
    CHECK(locked);
    /* 引擎必须进入运行。 */
    CHECK(engine->state == WORKOUT_STATE_RUNNING);
    /* 锁定动作必须一致。 */
    CHECK(engine->selected_action == (uint8_t)action);
    /* 返回成功。 */
    return 0;
}

/* 验证准备锁定、深蹲完整计数、每次计数 30 ms 振动和暂停时间排除。 */
static int test_repetition_flow(void)
{
    /* 创建训练引擎。 */
    workout_engine_t engine;
    /* 初始化为空闲。 */
    workout_engine_init(&engine);
    /* 以 70 kg 用户开始会话。 */
    CHECK(workout_engine_start(&engine, 7U, 70000U, 0ULL) == WORKOUT_STATUS_OK);
    /* 锁定 squat。 */
    CHECK(lock_action(&engine, FITNESS_ACTION_SQUAT, 0ULL) == 0);

    /* 锁定后队列首先包含会话开始双脉冲。 */
    fitness_haptic_request_t haptic;
    /* 取出开始反馈。 */
    CHECK(workout_engine_pop_haptic(&engine, &haptic));
    /* 开始反馈是两次 20 ms。 */
    CHECK((haptic.reason == FITNESS_HAPTIC_REASON_START) &&
          (haptic.on_ms == 20U) &&
          (haptic.repeat_count == 2U));

    /* 保存 MetricEvent 输出。 */
    fitness_metric_event_t event;
    /* 保存是否产生事件。 */
    bool emitted = false;
    /* 首个稳定点位于锁定时间之后。 */
    motion_phase_sample_t sample = make_sample(1000ULL, 0.0F, 1.0F);
    /* 稳定点只推进热量。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 尚无重复。 */
    CHECK(!emitted);
    /* 主向旋转。 */
    sample = make_sample(1040ULL, 80.0F, 1.05F);
    /* PRIMARY 第一稳定点。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 保持主向。 */
    sample = make_sample(1080ULL, 70.0F, 1.04F);
    /* PRIMARY 第二稳定点。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 等待反向，保持 30 deg/s 使其不被视为 REST。 */
    for (uint64_t time_ms = 1120ULL; time_ms < 1400ULL; time_ms += 40ULL) {
        /* 构造中间点。 */
        sample = make_sample(time_ms, 30.0F, 1.0F);
        /* 中间点不产生事件。 */
        CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
              WORKOUT_STATUS_OK);
    }
    /* 反向回摆。 */
    sample = make_sample(1400ULL, -80.0F, 1.0F);
    /* SECONDARY 第一稳定点。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 保持回摆。 */
    sample = make_sample(1440ULL, -65.0F, 1.0F);
    /* SECONDARY 第二稳定点。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 等待满足深蹲至少 600 ms 周期。 */
    for (uint64_t time_ms = 1480ULL; time_ms < 1720ULL; time_ms += 40ULL) {
        /* 保持非 REST。 */
        sample = make_sample(time_ms, 30.0F, 1.0F);
        /* 不产生事件。 */
        CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
              WORKOUT_STATUS_OK);
    }
    /* 第一个稳定点产生 REST 候选。 */
    sample = make_sample(1720ULL, 0.0F, 1.0F);
    /* 第一 REST 点不计数。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 第二稳定点闭合完整重复。 */
    sample = make_sample(1760ULL, 0.0F, 1.0F);
    /* 必须生成唯一事件。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 检查事件存在。 */
    CHECK(emitted);
    /* 检查动作、单位和总次数。 */
    CHECK((event.action == FITNESS_ACTION_SQUAT) &&
          (event.metric_kind == FITNESS_METRIC_REPETITION) &&
          (event.total_value == 1ULL));
    /* 每次有效重复必须产生一次 30 ms 请求。 */
    CHECK(workout_engine_pop_haptic(&engine, &haptic));
    /* 检查波形。 */
    CHECK((haptic.reason == FITNESS_HAPTIC_REASON_REPETITION) &&
          (haptic.on_ms == 30U) &&
          (haptic.repeat_count == 1U));
    /* 保存暂停前热量。 */
    const uint64_t calories_before_pause = engine.fitness_session.gross_microkcal;
    /* 暂停会话。 */
    CHECK(workout_engine_pause(&engine, 1800ULL) == WORKOUT_STATUS_OK);
    /* 暂停期样本被忽略。 */
    sample = make_sample(5000ULL, 0.0F, 1.0F);
    /* 检查有意忽略。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_IGNORED);
    /* 暂停期热量不变。 */
    CHECK(engine.fitness_session.gross_microkcal == calories_before_pause);
    /* 10 秒后恢复。 */
    CHECK(workout_engine_resume(&engine, 11800ULL) == WORKOUT_STATUS_OK);
    /* 恢复后 40 ms 点正常推进，不含暂停 10 秒。 */
    sample = make_sample(11840ULL, 0.0F, 1.0F);
    /* 处理成功。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 停止会话。 */
    CHECK(workout_engine_stop(&engine, 11900ULL) == WORKOUT_STATUS_OK);
    /* 重复停止幂等。 */
    CHECK(workout_engine_stop(&engine, 11900ULL) == WORKOUT_STATUS_OK);
    /* 当前状态为总结。 */
    CHECK(engine.state == WORKOUT_STATE_SUMMARY);
    /* 测试通过。 */
    return 0;
}

/* 验证 walk 步峰、权威步数和未到 10 步时不振动。 */
static int test_step_flow(void)
{
    /* 初始化新引擎。 */
    workout_engine_t engine;
    /* 清空状态。 */
    workout_engine_init(&engine);
    /* 开始新会话。 */
    CHECK(workout_engine_start(&engine, 8U, 60000U, 0ULL) == WORKOUT_STATUS_OK);
    /* 锁定 walk。 */
    CHECK(lock_action(&engine, FITNESS_ACTION_WALK, 0ULL) == 0);
    /* 丢弃开始振动。 */
    fitness_haptic_request_t haptic;
    /* 开始振动必须存在。 */
    CHECK(workout_engine_pop_haptic(&engine, &haptic));
    /* 保存事件。 */
    fitness_metric_event_t event;
    /* 保存事件标志。 */
    bool emitted = false;
    /* 两点建立步峰基线。 */
    motion_phase_sample_t sample = make_sample(1000ULL, 0.0F, 1.0F);
    /* 第一点。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 第二点。 */
    sample = make_sample(1040ULL, 0.0F, 1.0F);
    /* 处理第二点。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 峰顶。 */
    sample = make_sample(1080ULL, 0.0F, 1.35F);
    /* 峰顶等待右侧确认。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 回落点确认一步。 */
    sample = make_sample(1120ULL, 0.0F, 1.0F);
    /* 必须产生步事件。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 检查事件。 */
    CHECK(emitted && (event.metric_kind == FITNESS_METRIC_STEP) && (event.total_value == 1ULL));
    /* 不满 10 步不得有步反馈。 */
    CHECK(!workout_engine_pop_haptic(&engine, &haptic));
    /* 测试通过。 */
    return 0;
}

/* 验证准备阶段异常 logits 不污染累计，低电平样本不会锁定。 */
static int test_prepare_boundaries(void)
{
    /* 初始化引擎。 */
    workout_engine_t engine;
    /* 清空状态。 */
    workout_engine_init(&engine);
    /* 非法体重拒绝。 */
    CHECK(workout_engine_start(&engine, 1U, 1000U, 0ULL) == WORKOUT_STATUS_ERR_ARGUMENT);
    /* 29999 g 低于统一下界，必须拒绝。 */
    CHECK(workout_engine_start(&engine, 1U, 29999U, 0ULL) == WORKOUT_STATUS_ERR_ARGUMENT);
    /* 250001 g 高于统一上界，必须拒绝。 */
    CHECK(workout_engine_start(&engine, 1U, 250001U, 0ULL) == WORKOUT_STATUS_ERR_ARGUMENT);
    /* 30000 g 下界必须接受。 */
    CHECK(workout_engine_start(&engine, 9U, 30000U, 0ULL) == WORKOUT_STATUS_OK);
    /* 返回空闲以测试另一边界。 */
    workout_engine_return_idle(&engine);
    /* 250000 g 上界必须接受。 */
    CHECK(workout_engine_start(&engine, 9U, 250000U, 0ULL) == WORKOUT_STATUS_OK);
    /* 返回空闲以继续未知体重测试。 */
    workout_engine_return_idle(&engine);
    /* 合法未知体重允许。 */
    CHECK(workout_engine_start(&engine, 9U, 0U, 0ULL) == WORKOUT_STATUS_OK);
    /* 构造平坦 logits，softmax 约 9.09%，低于门槛。 */
    float logits[WORKOUT_CLASS_COUNT] = {0.0F};
    /* 保存锁定标志。 */
    bool locked = false;
    /* 低置信窗口被安全忽略。 */
    CHECK(workout_engine_push_inference(&engine, logits, 0ULL, 0U, &locked) ==
          WORKOUT_STATUS_IGNORED);
    /* 未锁定。 */
    CHECK(!locked);
    /* 保存污染前窗口数。 */
    const uint32_t windows_before_nan = engine.bout_window_count;
    /* 注入 NaN。 */
    logits[3] = NAN;
    /* 整帧拒绝。 */
    CHECK(workout_engine_push_inference(&engine, logits, 480ULL, 0U, &locked) ==
          WORKOUT_STATUS_ERR_ARGUMENT);
    /* 窗口数保持不变，证明修改前验证。 */
    CHECK(engine.bout_window_count == windows_before_nan);
    /* 准备阶段停止进入空总结。 */
    CHECK(workout_engine_stop(&engine, 500ULL) == WORKOUT_STATUS_OK);
    /* 返回空闲。 */
    workout_engine_return_idle(&engine);
    /* 检查空闲状态。 */
    CHECK(engine.state == WORKOUT_STATE_IDLE);
    /* 测试通过。 */
    return 0;
}

/* 验证运行阶段继续使用动作段因果累计，且类别不一致时冻结计数状态。 */
static int test_running_bout_accumulation_and_count_freeze(void)
{
    /* 创建独立训练引擎，避免复用其它测试的累计状态。 */
    workout_engine_t engine;
    /* 初始化为空闲状态。 */
    workout_engine_init(&engine);
    /* 开始 70 kg 用户的单动作会话。 */
    CHECK(workout_engine_start(&engine, 10U, 70000U, 0ULL) == WORKOUT_STATUS_OK);
    /* 三个高置信窗口锁定普通深蹲。 */
    CHECK(lock_action(&engine, FITNESS_ACTION_SQUAT, 0ULL) == 0);
    /* 锁定类别来自前三个累计窗口。 */
    CHECK(engine.inferred_action == (uint8_t)FITNESS_ACTION_SQUAT);
    /* 锁定后累计类别与计数动作一致。 */
    CHECK(engine.classification_consistent);
    /* 保存唯一开始振动并清空队列，后续不得再次入队开始反馈。 */
    fitness_haptic_request_t haptic;
    /* 第一次锁定必须存在开始振动。 */
    CHECK(workout_engine_pop_haptic(&engine, &haptic));

    /* 构造足以让四窗口累计均值改判为 tuck_jump 的强反例。 */
    float logits[WORKOUT_CLASS_COUNT];
    /* tuck_jump 写 +20，其余写 -20，使累计和越过前三个深蹲窗口。 */
    make_scaled_logits(logits, FITNESS_ACTION_TUCK_JUMP, 20.0F, -20.0F);
    /* RUNNING 更新不得再次报告动作锁定。 */
    bool locked = true;
    /* 第四个窗口必须被动作段累计器吸收。 */
    CHECK(workout_engine_push_inference(&engine, logits, 1440ULL, 0U, &locked) ==
          WORKOUT_STATUS_OK);
    /* 运行阶段不会产生第二次锁定事件。 */
    CHECK(!locked);
    /* 累计窗口数必须从三增加到四，证明没有退化为单窗口诊断。 */
    CHECK(engine.bout_window_count == 4U);
    /* 当前因果累计分类应改为 tuck_jump。 */
    CHECK(engine.inferred_action == (uint8_t)FITNESS_ACTION_TUCK_JUMP);
    /* 分类与本会话锁定动作不一致时必须冻结计数输入。 */
    CHECK(!engine.classification_consistent);
    /* 会话计数动作必须保持普通深蹲，禁止运行中重建计数器。 */
    CHECK(engine.selected_action == (uint8_t)FITNESS_ACTION_SQUAT);
    /* fitness_core 的权威会话动作同样必须保持普通深蹲。 */
    CHECK(engine.fitness_session.action == FITNESS_ACTION_SQUAT);
    /* 重复计数器动作不得被新分类覆盖。 */
    CHECK(engine.rep_counter.action == FITNESS_ACTION_SQUAT);
    /* 运行改判不得重复产生开始振动。 */
    CHECK(!workout_engine_pop_haptic(&engine, &haptic));

    /* 构造一个本来会进入相位检测器的有效六轴点。 */
    motion_phase_sample_t sample = make_sample(1480ULL, 80.0F, 1.05F);
    /* 保存事件输出。 */
    fitness_metric_event_t event;
    /* 保存是否生成次数事件。 */
    bool emitted = true;
    /* 上层质量有效，但分类不一致仍必须只推进时间/热量而冻结相位。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 冻结点不得产生次数。 */
    CHECK(!emitted);
    /* 相位检测器没有接受时间戳，证明计数路径确实被冻结。 */
    CHECK(!engine.phase_detector.has_timestamp);

    /* 构造强普通深蹲窗口，使五窗口累计均值重新回到锁定动作。 */
    make_scaled_logits(logits, FITNESS_ACTION_SQUAT, 20.0F, -20.0F);
    /* 第五个窗口继续进入同一累计器。 */
    CHECK(workout_engine_push_inference(&engine, logits, 1920ULL, 0U, &locked) ==
          WORKOUT_STATUS_OK);
    /* 累计分类恢复普通深蹲。 */
    CHECK(engine.inferred_action == (uint8_t)FITNESS_ACTION_SQUAT);
    /* 分类重新一致后允许后续原始点进入计数器。 */
    CHECK(engine.classification_consistent);
    /* 再送一个有效点建立相位时间基线。 */
    sample = make_sample(1960ULL, 0.0F, 1.0F);
    /* 一致状态应正常处理。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 相位检测器已接受时间戳，证明冻结解除。 */
    CHECK(engine.phase_detector.has_timestamp);
    /* 测试通过。 */
    return 0;
}

/* 验证显式证据重置不会清除锁定动作、已计次数或领域会话。 */
static int test_bout_evidence_reset_preserves_session(void)
{
    /* 创建独立引擎。 */
    workout_engine_t engine;
    /* 初始化为空闲。 */
    workout_engine_init(&engine);
    /* 开始新会话。 */
    CHECK(workout_engine_start(&engine, 11U, 65000U, 0ULL) == WORKOUT_STATUS_OK);
    /* 锁定普通深蹲。 */
    CHECK(lock_action(&engine, FITNESS_ACTION_SQUAT, 0ULL) == 0);
    /* 模拟已经确认的两个次数，验证证据重置不修改业务累计。 */
    engine.fitness_session.repetitions = 2ULL;
    /* 清空动作段分类证据，模拟暂停、IMU 间断或设备重连。 */
    workout_engine_reset_bout_evidence(&engine);
    /* 因果累计窗口数必须归零。 */
    CHECK(engine.bout_window_count == 0U);
    /* 尚无新窗口时累计分类无效。 */
    CHECK(engine.inferred_action == WORKOUT_ACTION_UNKNOWN);
    /* 重新取得同动作证据前计数必须冻结。 */
    CHECK(!engine.classification_consistent);
    /* 锁定动作仍是普通深蹲。 */
    CHECK(engine.selected_action == (uint8_t)FITNESS_ACTION_SQUAT);
    /* 权威次数保持为二。 */
    CHECK(engine.fitness_session.repetitions == 2ULL);
    /* 领域会话仍保持活动，避免重新开始导致事件序号或热量丢失。 */
    CHECK(engine.fitness_session.active);
    /* 测试通过。 */
    return 0;
}

/* 入口按产品链路顺序运行全部测试。 */
int main(void)
{
    /* 验证重复计数、振动和暂停。 */
    CHECK(test_repetition_flow() == 0);
    /* 验证步数链路。 */
    CHECK(test_step_flow() == 0);
    /* 验证准备异常边界。 */
    CHECK(test_prepare_boundaries() == 0);
    /* 验证运行阶段因果累计和动作不一致冻结。 */
    CHECK(test_running_bout_accumulation_and_count_freeze() == 0);
    /* 验证证据重置不破坏已锁定会话。 */
    CHECK(test_bout_evidence_reset_preserves_session() == 0);
    /* 输出稳定成功摘要。 */
    (void)printf("workout_engine host tests: PASS (%u assertions)\n", g_assertions);
    /* 返回成功。 */
    return 0;
}
