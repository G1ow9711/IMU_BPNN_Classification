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

/* 质量位第 4 位与生产 IMU_QUALITY_HAPTIC_CONTAMINATED 一致，表示马达导通污染当前六轴点。 */
#define TEST_IMU_QUALITY_HAPTIC_CONTAMINATED (UINT16_C(1) << 4U)
/* 30 ms 马达及前后保护窗在 25 Hz 重采样链中最多覆盖连续四点。 */
#define TEST_HAPTIC_CONTAMINATED_SAMPLE_COUNT (4U)

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

/* 构造三轴加速度点；用于验证开合跳峰谷中位数，单位均为 g。 */
static motion_phase_sample_t make_acceleration_sample(
    const uint64_t time_ms,
    const float acceleration_x_g,
    const float acceleration_y_g,
    const float acceleration_z_g)
{
    /* 先清零角速度和未使用字段。 */
    motion_phase_sample_t sample = make_sample(time_ms, 0.0F, acceleration_z_g);
    /* 写入 ax，单位 g。 */
    sample.axis[3] = acceleration_x_g;
    /* 写入 ay，单位 g。 */
    sample.axis[4] = acceleration_y_g;
    /* az 已由 make_sample 写入下标 5。 */
    return sample;
}

/* 提交两个高置信重叠窗口并检查累计类别稳定后锁定本次唯一动作。 */
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
    /* 首个可信窗口只建立累计候选，不能永久锁定本次会话。 */
    CHECK(workout_engine_push_inference(engine, logits, first_ms, 0U, &locked) ==
          WORKOUT_STATUS_IGNORED);
    /* 第一窗口后必须保持准备态并发布候选动作。 */
    CHECK(!locked && (engine->state == WORKOUT_STATE_PREPARING));
    /* 第二个窗口位于 12 点步长后的 480 ms，继续提交同类证据。 */
    CHECK(workout_engine_push_inference(engine, logits, first_ms + 480ULL, 0U, &locked) ==
          WORKOUT_STATUS_OK);
    /* 累计类别连续两窗且概率过门后必须报告新锁定。 */
    CHECK(locked);
    /* 引擎必须进入运行。 */
    CHECK(engine->state == WORKOUT_STATE_RUNNING);
    /* 锁定动作必须等于两个累计窗口的稳定类别。 */
    CHECK(engine->selected_action == (uint8_t)action);
    /* 返回成功。 */
    return 0;
}

/* 验证首窗瞬态误判不会锁死整场，连续两个累计正确窗口才提交唯一动作。 */
static int test_preparing_rejects_transient_first_window(void)
{
    /* 创建独立训练引擎，避免其它测试的累计证据泄漏。 */
    workout_engine_t engine;
    /* 初始化为空闲状态。 */
    workout_engine_init(&engine);
    /* 开始 70 kg 用户的新会话。 */
    CHECK(workout_engine_start(&engine, 75U, 70000U, 0ULL) == WORKOUT_STATUS_OK);
    /* logits 保存当前 [11] 类融合分数。 */
    float logits[WORKOUT_CLASS_COUNT];
    /* 首窗把 tuck_jump 写为 +5、其它类写为 -5，模拟动作起步瞬态误判。 */
    make_scaled_logits(logits, FITNESS_ACTION_TUCK_JUMP, 5.0F, -5.0F);
    /* locked 保存当前调用是否从准备态进入运行态。 */
    bool locked = false;
    /* 首个高置信误窗只能成为临时候选。 */
    CHECK(workout_engine_push_inference(&engine, logits, 0ULL, 0U, &locked) ==
          WORKOUT_STATUS_IGNORED);
    /* 错误候选不得写入 selected_action。 */
    CHECK(!locked && (engine.selected_action == WORKOUT_ACTION_UNKNOWN));
    /* 第二窗把真实 squat 写为 +20、其它类写为 -20，使累计最优类转为 squat。 */
    make_scaled_logits(logits, FITNESS_ACTION_SQUAT, 20.0F, -20.0F);
    /* 新累计类别只连续一窗，仍不得锁定。 */
    CHECK(workout_engine_push_inference(&engine, logits, 480ULL, 0U, &locked) ==
          WORKOUT_STATUS_IGNORED);
    /* 实时候选必须已经纠正为 squat，但会话仍处于准备态。 */
    CHECK(!locked &&
          (engine.inferred_action == (uint8_t)FITNESS_ACTION_SQUAT) &&
          (engine.state == WORKOUT_STATE_PREPARING));
    /* 第三窗继续提交 squat，累计类别连续两窗。 */
    CHECK(workout_engine_push_inference(&engine, logits, 960ULL, 0U, &locked) ==
          WORKOUT_STATUS_OK);
    /* 会话必须锁定真实 squat，而不是首窗 tuck_jump。 */
    CHECK(locked &&
          (engine.selected_action == (uint8_t)FITNESS_ACTION_SQUAT) &&
          (engine.state == WORKOUT_STATE_RUNNING));
    /* 测试通过。 */
    return 0;
}

/* 验证连续低置信累计证据最迟在第四窗按当前累计类别结束准备态。 */
static int test_preparing_forces_cumulative_class_at_four_windows(void)
{
    /* 创建独立训练引擎。 */
    workout_engine_t engine;
    /* 初始化为空闲状态。 */
    workout_engine_init(&engine);
    /* 开始未知体重会话，分类锁定不依赖热量资料。 */
    CHECK(workout_engine_start(&engine, 76U, 0U, 0ULL) == WORKOUT_STATUS_OK);
    /* 构造 11 类近乎平坦 logits，walk 仅高 0.1，softmax 远低于 50%。 */
    float logits[WORKOUT_CLASS_COUNT] = {0.0F};
    /* 固定 walk 为累计 argmax，同时保持低置信。 */
    logits[FITNESS_ACTION_WALK] = 0.1F;
    /* 保存每窗锁定标志。 */
    bool locked = false;
    /* 前三窗都不得提前锁定低置信类别。 */
    for (uint8_t window_index = 0U; window_index < 3U; ++window_index) {
        /* 每个重叠窗相隔 480 ms，与 25 Hz、步长 12 点合同一致。 */
        const uint64_t now_ms = (uint64_t)window_index * 480ULL;
        /* 低置信累计只更新实时候选并继续准备。 */
        CHECK(workout_engine_push_inference(&engine, logits, now_ms, 0U, &locked) ==
              WORKOUT_STATUS_IGNORED);
        /* 前三窗不得报告锁定。 */
        CHECK(!locked && (engine.state == WORKOUT_STATE_PREPARING));
    }
    /* 第四窗达到有界等待上限，必须按累计 argmax 锁定 walk。 */
    CHECK(workout_engine_push_inference(&engine, logits, 1440ULL, 0U, &locked) ==
          WORKOUT_STATUS_OK);
    /* 最终动作和状态必须完整提交。 */
    CHECK(locked &&
          (engine.selected_action == (uint8_t)FITNESS_ACTION_WALK) &&
          (engine.state == WORKOUT_STATE_RUNNING));
    /* 测试通过。 */
    return 0;
}

/* 验证一次窗口重建与三次重叠窗等待期间的 160 个 25 Hz 点全部保留。 */
static int test_preparing_retains_full_four_window_span(void)
{
    /* 创建独立训练引擎，准备缓存从空状态开始。 */
    workout_engine_t engine;
    /* 清空全部固定状态和振动队列。 */
    workout_engine_init(&engine);
    /* 开始新会话；零时刻对应用户点击手表“开始”。 */
    CHECK(workout_engine_start(&engine, 77U, 70000U, 0ULL) == WORKOUT_STATUS_OK);
    /* event 保存准备点接口要求的输出对象；准备态不允许真正发出指标事件。 */
    fitness_metric_event_t event;
    /* emitted 保存准备点是否意外产生计数事件。 */
    bool emitted = true;
    /* 160 点覆盖 62 点首窗、一次 62 点重建和三次 12 点步进，共 6.40 秒。 */
    for (uint8_t point = 0U; point < 160U; ++point) {
        /* 构造 25 Hz 干净六轴点；幅值不影响准备缓存容量合同。 */
        const motion_phase_sample_t sample = make_acceleration_sample(
            (uint64_t)point * 40ULL,
            0.25F,
            -0.20F,
            1.00F);
        /* 准备态应保存当前点并返回安全忽略。 */
        CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
              WORKOUT_STATUS_IGNORED);
        /* 尚未锁类时不得提前公开次数。 */
        CHECK(!emitted);
    }
    /* 一次质量重建后锁类仍必须保留完整 160 点，而不是只留最后 98 点。 */
    CHECK(engine.prelock_sample_count == 160U);
    /* 最早样本必须仍位于环形缓冲有效范围，时间固定为零毫秒。 */
    CHECK(engine.prelock_samples[0].sample.monotonic_ms == 0ULL);
    /* 测试通过。 */
    return 0;
}

/* 验证准备期连续性告警只切断未完成周期，不清除已缓存动作和干净分类候选。 */
static int test_preparing_timeline_break_preserves_buffer_and_candidate(void)
{
    /* 创建独立训练引擎。 */
    workout_engine_t engine;
    /* 清空全部状态。 */
    workout_engine_init(&engine);
    /* 用户点击开始后立即进入准备态。 */
    CHECK(workout_engine_start(&engine, 79U, 70000U, 0ULL) == WORKOUT_STATUS_OK);
    /* 保存准备态样本接口要求的事件槽。 */
    fitness_metric_event_t event;
    /* 准备态不得公开事件。 */
    bool emitted = true;
    /* 缓存二十个严格 25 Hz 干净点，代表连续性告警前已经发生的动作片段。 */
    for (uint8_t point = 0U; point < 20U; ++point) {
        /* 三轴幅值不影响本测试的缓存所有权。 */
        const motion_phase_sample_t sample = make_acceleration_sample(
            (uint64_t)point * MOTION_PHASE_SAMPLE_PERIOD_MS,
            0.5F,
            -0.4F,
            0.2F);
        /* 准备态必须保存当前点。 */
        CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
              WORKOUT_STATUS_IGNORED);
        /* 锁类前没有公开计数。 */
        CHECK(!emitted);
    }
    /* 构造一个高置信开合跳干净窗口。 */
    float logits[WORKOUT_CLASS_COUNT];
    /* 目标类别固定为开合跳。 */
    make_logits(logits, (uint8_t)FITNESS_ACTION_JUMPING_JACK);
    /* locked 保存本窗是否完成动作选择。 */
    bool locked = true;
    /* 第一窗只建立候选。 */
    CHECK(workout_engine_push_inference(&engine, logits, 800ULL, 0U, &locked) ==
          WORKOUT_STATUS_IGNORED);
    /* 第一窗不能提前锁类。 */
    CHECK(!locked);
    /* 保存断流前准备样本数和分类窗口数。 */
    const uint8_t samples_before_break = engine.prelock_sample_count;
    /* 模拟 QMI/重采样连续性告警。 */
    workout_engine_reset_bout_evidence(&engine);
    /* 已缓存动作不得被一次数据边界整段删除。 */
    CHECK(engine.prelock_sample_count == samples_before_break);
    /* 已完成的干净模型候选不得因下一窗口重新填充而丢失。 */
    CHECK(engine.bout_window_count == 1U);
    /* 候选动作和连续窗数必须保持，下一干净窗可完成确认。 */
    CHECK((engine.candidate_action == (uint8_t)FITNESS_ACTION_JUMPING_JACK) &&
          (engine.candidate_windows == 1U));
    /* 把真实连续性边界点写入准备缓存；位 0 对应加速度时间缺口。 */
    const motion_phase_sample_t boundary_sample = make_acceleration_sample(
        840ULL,
        8.0F,
        -8.0F,
        4.0F);
    /* 边界幅值必须被标为无效，但该点仍保留原始时间供重放切断半周期。 */
    CHECK(workout_engine_push_sample(
              &engine,
              &boundary_sample,
              false,
              UINT16_C(0x0001),
              &event,
              &emitted) == WORKOUT_STATUS_IGNORED);
    /* 边界点只增加一条缓存，不删除断流前已经闭合的动作数据。 */
    CHECK(engine.prelock_sample_count == (uint8_t)(samples_before_break + 1U));
    /* 第二个干净同类窗口完成本轮主动作选择。 */
    CHECK(workout_engine_push_inference(&engine, logits, 3280ULL, 0U, &locked) ==
          WORKOUT_STATUS_OK);
    /* 会话进入运行态且主动作正确。 */
    CHECK(locked && (engine.selected_action == (uint8_t)FITNESS_ACTION_JUMPING_JACK));
    /* 测试通过。 */
    return 0;
}

/* 验证准备期补算形成的每次计数都保留原始 IMU 时刻并可按序交给 BLE/CSV。 */
static int test_prelock_replay_exposes_all_metric_events(void)
{
    /* 一个开合跳周期固定 20 点、0.8 秒，峰谷幅值足以越过产品动态门。 */
    static const float cycle[20] = {
        0.0F, 0.31F, 0.59F, 0.81F, 0.95F,
        1.0F, 0.95F, 0.81F, 0.59F, 0.31F,
        0.0F, -0.31F, -0.59F, -0.81F, -0.95F,
        -1.0F, -0.95F, -0.81F, -0.59F, -0.31F
    };
    /* 创建独立训练引擎，准备缓存、回放 FIFO 和权威累计均从零开始。 */
    workout_engine_t engine;
    /* 清空全部固定状态。 */
    workout_engine_init(&engine);
    /* 会话从用户点击开始的零毫秒进入 PREPARING。 */
    CHECK(workout_engine_start(&engine, 78U, 70000U, 0ULL) == WORKOUT_STATUS_OK);
    /* event 是准备态接口要求的输出槽；锁类前不得直接公开事件。 */
    fitness_metric_event_t event;
    /* emitted 验证准备态只缓存，不提前发布。 */
    bool emitted = true;
    /* time_ms 以 25 Hz 的 40 ms 周期严格递增。 */
    uint64_t time_ms = 0ULL;
    /* 先缓存 20 个平坦点，填满 11+5 双均值预热。 */
    for (uint8_t point = 0U; point < 20U; ++point) {
        /* 三轴动态量为零，只建立滤波基线。 */
        const motion_phase_sample_t sample = make_acceleration_sample(
            time_ms,
            0.0F,
            0.0F,
            0.0F);
        /* PREPARING 必须缓存当前点并返回有意忽略。 */
        CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
              WORKOUT_STATUS_IGNORED);
        /* 锁类前不能直接产生 UI/BLE 事件。 */
        CHECK(!emitted);
        /* 推进一个采样周期。 */
        time_ms += MOTION_PHASE_SAMPLE_PERIOD_MS;
    }
    /* 缓存两个完整开合跳周期，代表锁类等待期已经发生的前两次动作。 */
    for (uint8_t repetition = 0U; repetition < 2U; ++repetition) {
        /* 当前周期逐点写入三轴，其中 X/Y 是一致可信轴。 */
        for (uint8_t point = 0U; point < 20U; ++point) {
            /* 构造三轴加速度；Z 使用小幅反相噪声验证中位融合。 */
            const motion_phase_sample_t sample = make_acceleration_sample(
                time_ms,
                cycle[point],
                cycle[point] * 0.9F,
                -cycle[point] * 0.15F);
            /* 准备态仍只缓存，不允许直接发事件。 */
            CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
                  WORKOUT_STATUS_IGNORED);
            /* 每个缓存点都必须保持未公开。 */
            CHECK(!emitted);
            /* 推进一个 40 ms 点。 */
            time_ms += MOTION_PHASE_SAMPLE_PERIOD_MS;
        }
    }
    /* 追加十个上升点，使双均值延迟可以确认第二个周期的末尾极值。 */
    for (uint8_t point = 0U; point < 10U; ++point) {
        /* 使用下一周期前半段作为有限尾部确认信号。 */
        const motion_phase_sample_t sample = make_acceleration_sample(
            time_ms,
            cycle[point],
            cycle[point] * 0.9F,
            -cycle[point] * 0.15F);
        /* 尾部仍属于准备缓存。 */
        CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
              WORKOUT_STATUS_IGNORED);
        /* 尾部点不得提前公开。 */
        CHECK(!emitted);
        /* 推进到下一采样时刻。 */
        time_ms += MOTION_PHASE_SAMPLE_PERIOD_MS;
    }

    /* 构造两个一致高置信窗口，第二窗触发本轮唯一动作锁定与历史样本回放。 */
    float logits[WORKOUT_CLASS_COUNT];
    /* 目标类别固定为开合跳。 */
    make_logits(logits, (uint8_t)FITNESS_ACTION_JUMPING_JACK);
    /* locked 保存每次推理是否完成锁定。 */
    bool locked = false;
    /* 第一窗只建立累计候选。 */
    CHECK(workout_engine_push_inference(&engine, logits, time_ms, 0U, &locked) ==
          WORKOUT_STATUS_IGNORED);
    /* 第一窗后仍未锁定。 */
    CHECK(!locked);
    /* 第二窗位于 480 ms 后，必须锁定并回放全部准备点。 */
    CHECK(workout_engine_push_inference(&engine, logits, time_ms + 480ULL, 0U, &locked) ==
          WORKOUT_STATUS_OK);
    /* 锁定完成且会话权威累计已经补算为两次。 */
    CHECK(locked && (engine.fitness_session.repetitions == 2ULL));
    /* 固定回放 FIFO 必须保留两条事件，而不是只保留最终总数。 */
    CHECK(engine.replay_metric_event_count == 2U);

    /* first_event 保存第一次历史计数，必须携带 event_seq=1 和 total=1。 */
    fitness_metric_event_t first_event;
    /* 从 FIFO 取最早事件。 */
    CHECK(workout_engine_pop_replay_metric_event(&engine, &first_event));
    /* 第一次事件必须属于当前会话和开合跳。 */
    CHECK((first_event.session_seq == 78U) &&
          (first_event.action == FITNESS_ACTION_JUMPING_JACK) &&
          (first_event.event_seq == 1U) &&
          (first_event.total_value == 1ULL));
    /* second_event 保存第二次历史计数。 */
    fitness_metric_event_t second_event;
    /* FIFO 第二条必须存在。 */
    CHECK(workout_engine_pop_replay_metric_event(&engine, &second_event));
    /* 第二次事件序号和累计必须严格递增。 */
    CHECK((second_event.event_seq == 2U) && (second_event.total_value == 2ULL));
    /* 两条事件必须保留各自原始 IMU 时间，且都早于第二个锁定窗口。 */
    CHECK((first_event.monotonic_ms < second_event.monotonic_ms) &&
          (second_event.monotonic_ms < time_ms + 480ULL));
    /* 两条事件取完后不得凭空出现第三条。 */
    CHECK(!workout_engine_pop_replay_metric_event(&engine, &event));
    /* 测试通过。 */
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

    /* 保存后续次数振动输出。 */
    fitness_haptic_request_t haptic;
    /* 累计锁定不振动，避免马达污染紧接着的第一个实时动作。 */
    CHECK(!workout_engine_pop_haptic(&engine, &haptic));

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


/* 把指定数量的三轴周期信号送入已锁定开合跳会话，并返回公开次数事件数。 */
static int push_jumping_jack_acceleration_cycles(
    workout_engine_t *engine,
    const uint32_t repetition_count,
    const bool simulate_haptic_feedback,
    uint32_t *emitted_count)
{
    /* 一个周期 20 点、0.8 秒，峰谷幅值 2 g，明显超过 1/14 g。 */
    static const float cycle[20] = {
        0.0F, 0.31F, 0.59F, 0.81F, 0.95F,
        1.0F, 0.95F, 0.81F, 0.59F, 0.31F,
        0.0F, -0.31F, -0.59F, -0.81F, -0.95F,
        -1.0F, -0.95F, -0.81F, -0.59F, -0.31F
    };
    /* 公开次数从零累计。 */
    *emitted_count = 0U;
    /* time_ms 按 25 Hz 单调推进。 */
    uint64_t time_ms = 0ULL;
    /* event 接收唯一 MetricEvent。 */
    fitness_metric_event_t event;
    /* emitted 保存当前点是否使三轴中位数增加一次。 */
    bool emitted = false;
    /* contaminated_points_remaining 保存计次振动后仍需按污染点提交的 25 Hz 点数。 */
    uint32_t contaminated_points_remaining = 0U;
    /* 前 20 点填满 11+5 均值窗口，三轴动态分量均为零。 */
    for (uint32_t point = 0U; point < 20U; ++point) {
        /* 构造静止三轴加速度点。 */
        motion_phase_sample_t sample = make_acceleration_sample(time_ms, 0.0F, 0.0F, 0.0F);
        /* 静止点只能推进时间和滤波。 */
        CHECK(workout_engine_push_sample(engine, &sample, true, 0U, &event, &emitted) ==
              WORKOUT_STATUS_OK);
        /* 平坦输入不得公开次数。 */
        CHECK(!emitted);
        /* 推进 40 ms。 */
        time_ms += MOTION_PHASE_SAMPLE_PERIOD_MS;
    }
    /* 连续输入指定数量动作。 */
    for (uint32_t repetition = 0U; repetition < repetition_count; ++repetition) {
        /* 一个动作周期逐点输入。 */
        for (uint32_t point = 0U; point < 20U; ++point) {
            /* X/Y 保持同一权威周期，Z 注入较小反相量模拟单轴差异。 */
            motion_phase_sample_t sample = make_acceleration_sample(
                time_ms,
                cycle[point],
                cycle[point] * 0.9F,
                -cycle[point] * 0.15F);
            /* 当前点只有在马达保护窗外才允许进入峰谷滤波器。 */
            const bool count_input_valid = contaminated_points_remaining == 0U;
            /* 污染点携带与生产 IMU 链一致的质量位，干净点为零。 */
            const uint16_t quality_flags = count_input_valid
                ? UINT16_C(0)
                : TEST_IMU_QUALITY_HAPTIC_CONTAMINATED;
            /* 当前六轴点必须被在线状态机接受；污染点仍推进会话时间但不写峰谷幅值。 */
            CHECK(workout_engine_push_sample(
                      engine,
                      &sample,
                      count_input_valid,
                      quality_flags,
                      &event,
                      &emitted) ==
                  WORKOUT_STATUS_OK);
            /* 一个污染点已经按 40 ms 时间线提交，递减剩余保护点数。 */
            if (contaminated_points_remaining > 0U) {
                /* 下一点继续按剩余值决定是否受马达污染。 */
                contaminated_points_remaining -= 1U;
            }
            /* 中位累计增加时只允许一个事件。 */
            if (emitted) {
                /* 事件必须是一次重复动作。 */
                CHECK((event.metric_kind == FITNESS_METRIC_REPETITION) &&
                      (event.total_value == (uint64_t)(*emitted_count + 1U)));
                /* 累计公开次数。 */
                *emitted_count += 1U;
                /* 反馈闭环测试必须真实消费训练引擎排入的 30 ms 振动请求。 */
                if (simulate_haptic_feedback) {
                    /* haptic 保存刚才次数对应的唯一马达请求。 */
                    fitness_haptic_request_t haptic;
                    /* 每次权威重复都必须能够取出一个振动请求。 */
                    CHECK(workout_engine_pop_haptic(engine, &haptic));
                    /* 请求参数必须保持产品合同的 30 ms 单脉冲。 */
                    CHECK((haptic.reason == FITNESS_HAPTIC_REASON_REPETITION) &&
                          (haptic.on_ms == 30U) &&
                          (haptic.repeat_count == 1U));
                    /* 从下一采样点开始模拟马达导通及保护窗污染。 */
                    contaminated_points_remaining = TEST_HAPTIC_CONTAMINATED_SAMPLE_COUNT;
                }
            }
            /* 推进一个采样周期。 */
            time_ms += MOTION_PHASE_SAMPLE_PERIOD_MS;
        }
    }
    /* 追加十点上升段，确认双均值延迟中的末尾波谷。 */
    for (uint32_t point = 0U; point < 10U; ++point) {
        /* 构造尾部确认点。 */
        motion_phase_sample_t sample = make_acceleration_sample(
            time_ms,
            cycle[point],
            cycle[point] * 0.9F,
            -cycle[point] * 0.15F);
        /* 尾部同样服从振动保护窗，避免测试在周期尾部绕过真实反馈链。 */
        const bool count_input_valid = contaminated_points_remaining == 0U;
        /* 按当前保护状态写入生产兼容质量位。 */
        const uint16_t quality_flags = count_input_valid
            ? UINT16_C(0)
            : TEST_IMU_QUALITY_HAPTIC_CONTAMINATED;
        /* 尾部点正常推进；污染点仍不进入峰谷幅值链。 */
        CHECK(workout_engine_push_sample(
                  engine,
                  &sample,
                  count_input_valid,
                  quality_flags,
                  &event,
                  &emitted) ==
              WORKOUT_STATUS_OK);
        /* 消费一个已经提交的污染点。 */
        if (contaminated_points_remaining > 0U) {
            /* 递减后为零表示下一点重新干净。 */
            contaminated_points_remaining -= 1U;
        }
        /* 捕获可能因滤波延迟在尾部完成的最后一次。 */
        if (emitted) {
            /* 尾部仍必须是单次重复事件。 */
            CHECK((event.metric_kind == FITNESS_METRIC_REPETITION) &&
                  (event.total_value == (uint64_t)(*emitted_count + 1U)));
            /* 累加该次。 */
            *emitted_count += 1U;
            /* 尾部若完成次数，同样验证并启动真实振动污染窗。 */
            if (simulate_haptic_feedback) {
                /* haptic 保存尾部次数对应的请求。 */
                fitness_haptic_request_t haptic;
                /* 尾部事件也必须有一次马达请求。 */
                CHECK(workout_engine_pop_haptic(engine, &haptic));
                /* 尾部请求仍是 30 ms 单脉冲。 */
                CHECK((haptic.reason == FITNESS_HAPTIC_REASON_REPETITION) &&
                      (haptic.on_ms == 30U) &&
                      (haptic.repeat_count == 1U));
                /* 记录后续污染窗；当前测试结束前无需再消费完毕。 */
                contaminated_points_remaining = TEST_HAPTIC_CONTAMINATED_SAMPLE_COUNT;
            }
        }
        /* 推进时间。 */
        time_ms += MOTION_PHASE_SAMPLE_PERIOD_MS;
    }
    /* 数据送入完成。 */
    return 0;
}

/* 验证用户做十次开合跳时，三轴累计中位数输出十次而不是二次或二十次。 */
static int test_jumping_jack_peak_valley_median_counts_ten(void)
{
    /* 创建训练引擎。 */
    workout_engine_t engine;
    /* 初始化为空闲。 */
    workout_engine_init(&engine);
    /* 开始新会话。 */
    CHECK(workout_engine_start(&engine, 72U, 70000U, 0ULL) == WORKOUT_STATUS_OK);
    /* 两个累计高置信窗口锁定本次唯一动作为开合跳。 */
    CHECK(lock_action(&engine, FITNESS_ACTION_JUMPING_JACK, 0ULL) == 0);
    /* 三个峰谷检测器都必须初始化，轴累计从零开始。 */
    for (uint8_t axis = 0U; axis < WORKOUT_JUMPING_JACK_AXIS_COUNT; ++axis) {
        /* 检查静态状态有效。 */
        CHECK(engine.jumping_jack_pair_detectors[axis].initialized);
        /* 检查当前轴尚无配对。 */
        CHECK(engine.jumping_jack_pair_detectors[axis].total_pairs == 0ULL);
    }
    /* emitted_count 保存 UI/BLE 可见 MetricEvent 次数。 */
    uint32_t emitted_count = 0U;
    /* 输入十个周期。 */
    CHECK(push_jumping_jack_acceleration_cycles(&engine, 10U, false, &emitted_count) == 0);
    /* X/Y 两轴一致形成十次，中位数必须为十。 */
    CHECK(emitted_count == 10U);
    /* 权威会话累计必须同为十。 */
    CHECK(engine.fitness_session.repetitions == 10ULL);
    /* 已发布中位累计必须同为十，防止恢复后重复事件。 */
    CHECK(engine.jumping_jack_reported_repetitions == 10ULL);
    /* 测试通过。 */
    return 0;
}

/* 验证每次计数后的真实振动污染不能把后续八个周期漏成仅二次。 */
static int test_jumping_jack_haptic_feedback_preserves_ten_cycles(void)
{
    /* 创建独立训练引擎，避免普通十周期测试残留振动队列。 */
    workout_engine_t engine;
    /* 初始化全部会话、检测器和 FIFO。 */
    workout_engine_init(&engine);
    /* 开始本次 70 kg 测试会话。 */
    CHECK(workout_engine_start(&engine, 74U, 70000U, 0ULL) == WORKOUT_STATUS_OK);
    /* 首个分类窗口把本次唯一动作锁定为开合跳。 */
    CHECK(lock_action(&engine, FITNESS_ACTION_JUMPING_JACK, 0ULL) == 0);
    /* 保存 UI、BLE 和振动共用的权威事件数。 */
    uint32_t emitted_count = 0U;
    /* 输入十周期，并在每个事件后模拟四个马达污染点。 */
    CHECK(push_jumping_jack_acceleration_cycles(&engine, 10U, true, &emitted_count) == 0);
    /* 短暂马达污染只能跳过受污染幅值，不能反复清空 11+5 滤波历史。 */
    CHECK(emitted_count == 10U);
    /* 会话权威累计必须与事件数完全一致。 */
    CHECK(engine.fitness_session.repetitions == 10ULL);
    /* 三轴中位数发布游标同样必须达到十，防止恢复后补发旧次数。 */
    CHECK(engine.jumping_jack_reported_repetitions == 10ULL);
    /* 测试通过。 */
    return 0;
}

/* 验证单轴噪声不能越过三轴中位数，两个可信轴仍可持续计数。 */
static int test_jumping_jack_median_rejects_one_noisy_axis(void)
{
    /* 创建独立引擎。 */
    workout_engine_t engine;
    /* 初始化为空闲。 */
    workout_engine_init(&engine);
    /* 开始新会话。 */
    CHECK(workout_engine_start(&engine, 73U, 70000U, 0ULL) == WORKOUT_STATUS_OK);
    /* 锁定开合跳。 */
    CHECK(lock_action(&engine, FITNESS_ACTION_JUMPING_JACK, 0ULL) == 0);
    /* 直接制造单轴 Z 噪声累计，X/Y 仍为零。 */
    engine.jumping_jack_pair_detectors[2].total_pairs = 25ULL;
    /* reported 仍为零。 */
    CHECK(engine.jumping_jack_reported_repetitions == 0ULL);
    /* 输入一个平坦点，让融合层读取 0、0、25。 */
    motion_phase_sample_t sample = make_acceleration_sample(0ULL, 0.0F, 0.0F, 0.0F);
    /* 保存事件输出。 */
    fitness_metric_event_t event;
    /* 默认设为 true，验证函数明确清零。 */
    bool emitted = true;
    /* 平坦点处理成功。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 中位数为零，单轴 25 次噪声不能公开。 */
    CHECK(!emitted);
    /* 权威累计保持零。 */
    CHECK(engine.fitness_session.repetitions == 0ULL);
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
    /* 保存后续步数振动输出。 */
    fitness_haptic_request_t haptic;
    /* 累计锁定不产生开始振动。 */
    CHECK(!workout_engine_pop_haptic(&engine, &haptic));
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

/* 验证主动作不换计数器，但实时异类/休息会冻结计数并在恢复后重建周期。 */
static int test_running_action_remains_locked(void)
{
    /* 创建独立训练引擎，验证会话内分类抖动不能改变计数动作。 */
    workout_engine_t engine;
    /* 初始化为空闲状态。 */
    workout_engine_init(&engine);
    /* 开始 70 kg 用户的单动作会话。 */
    CHECK(workout_engine_start(&engine, 10U, 70000U, 0ULL) == WORKOUT_STATUS_OK);
    /* 两个累计高置信窗口锁定普通深蹲。 */
    CHECK(lock_action(&engine, FITNESS_ACTION_SQUAT, 0ULL) == 0);
    /* 稳定动作固定为普通深蹲。 */
    CHECK(engine.inferred_action == (uint8_t)FITNESS_ACTION_SQUAT);
    /* 锁定后累计类别与计数动作一致。 */
    CHECK(engine.classification_consistent);
    /* 保存振动读取对象。 */
    fitness_haptic_request_t haptic;
    /* 累计锁定不产生开始振动。 */
    CHECK(!workout_engine_pop_haptic(&engine, &haptic));

    /* 预置两个已完成重复，验证诊断抖动不改累计。 */
    engine.fitness_session.repetitions = 2ULL;
    /* 构造高置信 tuck_jump 单窗噪声。 */
    float logits[WORKOUT_CLASS_COUNT];
    /* tuck_jump 写 +20，其余写 -20，形成极强但不应生效的异类诊断窗。 */
    make_scaled_logits(logits, FITNESS_ACTION_TUCK_JUMP, 20.0F, -20.0F);
    /* RUNNING 更新不得再次报告动作锁定。 */
    bool locked = true;
    /* 首个异类窗口携带真板常见的 0x0041 边界位，仍必须参与休息/异类冻结判断。 */
    CHECK(workout_engine_push_inference(
              &engine,
              logits,
              1440ULL,
              UINT16_C(0x0041),
              &locked) ==
          WORKOUT_STATUS_OK);
    /* 运行阶段不会产生第二次锁定事件。 */
    CHECK(!locked);
    /* 实时类别必须公开当前窗口的收腹跳，不能伪装成仍在深蹲。 */
    CHECK(engine.inferred_action == (uint8_t)FITNESS_ACTION_TUCK_JUMP);
    /* 异类窗口必须立即冻结深蹲计数。 */
    CHECK(!engine.classification_consistent);
    /* 会话计数动作必须保持普通深蹲，禁止运行中重建计数器。 */
    CHECK(engine.selected_action == (uint8_t)FITNESS_ACTION_SQUAT);
    /* fitness_core 的权威会话动作同样必须保持普通深蹲。 */
    CHECK(engine.fitness_session.action == FITNESS_ACTION_SQUAT);
    /* 重复计数器动作不得被新分类覆盖。 */
    CHECK(engine.rep_counter.action == FITNESS_ACTION_SQUAT);
    /* 诊断异类不得产生任何振动。 */
    CHECK(!workout_engine_pop_haptic(&engine, &haptic));

    /* 构造一个本来会进入相位检测器的有效六轴点。 */
    motion_phase_sample_t sample = make_sample(1480ULL, 80.0F, 1.05F);
    /* 保存事件输出。 */
    fitness_metric_event_t event;
    /* 保存是否生成次数事件。 */
    bool emitted = true;
    /* 上层质量有效但实时类别不一致时，固定动作相位必须冻结。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 单个 PRIMARY 点尚未闭合周期，不产生次数。 */
    CHECK(!emitted);
    /* 相位检测器不得接受异类窗口后的腕部动作。 */
    CHECK(!engine.phase_detector.has_timestamp);

    /* 真实设备在推理窗口之间仍持续提交 25 Hz 点，补齐 1520~1880 ms 时间线。 */
    for (uint64_t time_ms = 1520ULL; time_ms < 1920ULL; time_ms += 40ULL) {
        /* 使用非静止同向运动，保持当前未闭合深蹲周期。 */
        sample = make_sample(time_ms, 28.0F, 1.0F);
        /* 异类诊断期间样本只推进会话时间和热量，不推进相位。 */
        CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
              WORKOUT_STATUS_OK);
    }
    /* 第二个连续 tuck_jump 诊断窗仍不能切换。 */
    CHECK(workout_engine_push_inference(&engine, logits, 1920ULL, 0U, &locked) ==
          WORKOUT_STATUS_OK);
    /* 两窗后主动作仍保持深蹲，但实时计数许可保持冻结。 */
    CHECK(engine.selected_action == (uint8_t)FITNESS_ACTION_SQUAT);
    CHECK(!engine.classification_consistent);
    /* 继续补齐 1960~2360 ms 真实 25 Hz 点，避免人为制造大于 120 ms 的断点。 */
    for (uint64_t time_ms = 1960ULL; time_ms < 2400ULL; time_ms += 40ULL) {
        /* 保持普通运动点，不闭合完整重复。 */
        sample = make_sample(time_ms, 28.0F, 1.0F);
        /* 冻结期间固定动作计数链不得接收。 */
        CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
              WORKOUT_STATUS_OK);
    }
    /* 第三个连续异类窗同样只能作为诊断噪声。 */
    CHECK(workout_engine_push_inference(&engine, logits, 2400ULL, 0U, &locked) ==
          WORKOUT_STATUS_OK);
    /* 三窗后会话主动作仍为深蹲，实时类别仍为收腹跳。 */
    CHECK(engine.inferred_action == (uint8_t)FITNESS_ACTION_TUCK_JUMP);
    CHECK(engine.selected_action == (uint8_t)FITNESS_ACTION_SQUAT);
    CHECK(engine.fitness_session.action == FITNESS_ACTION_SQUAT);
    CHECK(engine.rep_counter.action == FITNESS_ACTION_SQUAT);
    /* 计数许可保持关闭，避免站立/静坐阶段继续闭合旧深蹲周期。 */
    CHECK(!engine.classification_consistent);
    /* 会话累计次数必须保留。 */
    CHECK(engine.fitness_session.repetitions == 2ULL);
    /* 切换不重复产生开始振动。 */
    CHECK(!workout_engine_pop_haptic(&engine, &haptic));
    /* 构造恢复后的高置信深蹲窗口。 */
    make_scaled_logits(logits, FITNESS_ACTION_SQUAT, 20.0F, -20.0F);
    /* 同类但带边界质量位的窗口不能把冻结状态误恢复为可计数。 */
    CHECK(workout_engine_push_inference(
              &engine,
              logits,
              2880ULL,
              UINT16_C(0x0041),
              &locked) == WORKOUT_STATUS_OK);
    /* 实时类别可显示深蹲，但计数门仍等待干净窗。 */
    CHECK((engine.inferred_action == (uint8_t)FITNESS_ACTION_SQUAT) &&
          !engine.classification_consistent);
    /* 单个干净同类窗口即可恢复本轮主动作计数，不重新选择计数器。 */
    CHECK(workout_engine_push_inference(&engine, logits, 3360ULL, 0U, &locked) ==
          WORKOUT_STATUS_OK);
    /* 实时类别恢复为深蹲且计数许可重新打开。 */
    CHECK((engine.inferred_action == (uint8_t)FITNESS_ACTION_SQUAT) &&
          engine.classification_consistent);
    /* 送一个恢复后的有效点建立全新深蹲相位时间线。 */
    sample = make_sample(3400ULL, 0.0F, 1.0F);
    /* 主动作状态应正常处理。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 恢复后由当前点建立新时间线，不能接续冻结前半周期。 */
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
    /* 证据重置后稳定显示仍保持本次唯一动作。 */
    CHECK(engine.inferred_action == (uint8_t)FITNESS_ACTION_SQUAT);
    /* 连续性重置后必须冻结，直到新的干净同类窗口确认动作已经恢复。 */
    CHECK(!engine.classification_consistent);
    /* 锁定动作仍是普通深蹲。 */
    CHECK(engine.selected_action == (uint8_t)FITNESS_ACTION_SQUAT);
    /* 权威次数保持为二。 */
    CHECK(engine.fitness_session.repetitions == 2ULL);
    /* 领域会话仍保持活动，避免重新开始导致事件序号或热量丢失。 */
    CHECK(engine.fitness_session.active);
    /* 构造新的高置信深蹲窗口。 */
    float logits[WORKOUT_CLASS_COUNT];
    /* 当前主动作仍为深蹲。 */
    make_scaled_logits(logits, FITNESS_ACTION_SQUAT, 20.0F, -20.0F);
    /* locked 在运行态固定为 false。 */
    bool locked = true;
    /* 新干净同类窗口恢复计数许可。 */
    CHECK(workout_engine_push_inference(&engine, logits, 960ULL, 0U, &locked) ==
          WORKOUT_STATUS_OK);
    /* 不重新锁类，但实时一致性恢复。 */
    CHECK(!locked && engine.classification_consistent);
    /* 测试通过。 */
    return 0;
}

/* 入口按产品链路顺序运行全部测试。 */
int main(void)
{
    /* 验证首窗误判只作候选，累计稳定后锁定真实动作。 */
    CHECK(test_preparing_rejects_transient_first_window() == 0);
    /* 验证低置信会话最多等待四个重叠窗口。 */
    CHECK(test_preparing_forces_cumulative_class_at_four_windows() == 0);
    /* 验证一次窗口重建后第四窗锁类前完整 160 点动作都可补算。 */
    CHECK(test_preparing_retains_full_four_window_span() == 0);
    /* 验证准备期连续性告警保留缓存和干净分类候选。 */
    CHECK(test_preparing_timeline_break_preserves_buffer_and_candidate() == 0);
    /* 验证锁类补算的每条计数都能按原始时间进入 BLE/CSV。 */
    CHECK(test_prelock_replay_exposes_all_metric_events() == 0);
    /* 验证重复计数、振动和暂停。 */
    CHECK(test_repetition_flow() == 0);
    /* 验证十个相邻峰谷周期公开十次。 */
    CHECK(test_jumping_jack_peak_valley_median_counts_ten() == 0);
    /* 验证每次计数振动后的污染窗不会把十次漏成二次。 */
    CHECK(test_jumping_jack_haptic_feedback_preserves_ten_cycles() == 0);
    /* 验证三轴中位数拒绝单轴噪声。 */
    CHECK(test_jumping_jack_median_rejects_one_noisy_axis() == 0);
    /* 验证步数链路。 */
    CHECK(test_step_flow() == 0);
    /* 验证准备异常边界。 */
    CHECK(test_prepare_boundaries() == 0);
    /* 验证运行阶段保持准备态累计锁定动作且不因异类诊断冻结。 */
    CHECK(test_running_action_remains_locked() == 0);
    /* 验证证据重置不破坏已锁定会话。 */
    CHECK(test_bout_evidence_reset_preserves_session() == 0);
    /* 输出稳定成功摘要。 */
    (void)printf("workout_engine host tests: PASS (%u assertions)\n", g_assertions);
    /* 返回成功。 */
    return 0;
}
