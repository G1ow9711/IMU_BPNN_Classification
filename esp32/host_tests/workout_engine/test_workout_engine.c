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

/* 质量位第 7 位表示双 M0 前向失败，不能进入准备态分类累计。 */
#define TEST_IMU_QUALITY_INFERENCE_FAILED (UINT16_C(1) << 7U)

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

/* 验证强错误首窗不能借累计惯性压过随后连续两个一致的独立窗口。 */
static int test_preparing_confirms_consecutive_independent_windows(void)
{
    /* 创建独立训练引擎，避免历史累计 logits 污染本测试。 */
    workout_engine_t engine;
    /* 初始化全部会话、分类和计数状态。 */
    workout_engine_init(&engine);
    /* 开始一轮右手腕单动作会话；动作真值不预先传给引擎。 */
    CHECK(workout_engine_start(&engine, 76U, 70000U, 0ULL) == WORKOUT_STATUS_OK);
    /* logits 保存当前一个 62 点窗口的 11 类融合分数。 */
    float logits[WORKOUT_CLASS_COUNT];
    /* locked 标记本次推理是否完成动作确认。 */
    bool locked = false;
    /* 第一窗制造极强深蹲起步噪声，目标类 +10，其余类 -10。 */
    make_scaled_logits(logits, FITNESS_ACTION_SQUAT, 10.0F, -10.0F);
    /* 单个错误窗口只能成为临时候选，不能锁定整轮。 */
    CHECK(workout_engine_push_inference(&engine, logits, 0ULL, 0U, &locked) ==
          WORKOUT_STATUS_IGNORED);
    /* 第一窗后必须继续准备且尚未选择会话动作。 */
    CHECK(!locked &&
          (engine.state == WORKOUT_STATE_PREPARING) &&
          (engine.selected_action == WORKOUT_ACTION_UNKNOWN));
    /* 第二窗制造不同的早安式起步噪声，确保错误窗口本身也不连续。 */
    make_scaled_logits(logits, FITNESS_ACTION_GOOD_MORNING, 8.0F, -8.0F);
    /* 两个互异窗口不得因第一窗累计优势而提前锁成深蹲。 */
    CHECK(workout_engine_push_inference(&engine, logits, 480ULL, 0U, &locked) ==
          WORKOUT_STATUS_IGNORED);
    /* 引擎仍应处于准备态，并公开当前独立窗口的早安式候选。 */
    CHECK(!locked &&
          (engine.state == WORKOUT_STATE_PREPARING) &&
          (engine.inferred_action == (uint8_t)FITNESS_ACTION_GOOD_MORNING));
    /* 第三窗切换为真实挥手，建立第一份可信同类证据。 */
    make_scaled_logits(logits, FITNESS_ACTION_WAVE, 8.0F, -8.0F);
    /* 第一份挥手证据仍不得锁定。 */
    CHECK(workout_engine_push_inference(&engine, logits, 960ULL, 0U, &locked) ==
          WORKOUT_STATUS_IGNORED);
    /* 当前候选必须纠正为挥手，但会话动作仍未知。 */
    CHECK(!locked &&
          (engine.inferred_action == (uint8_t)FITNESS_ACTION_WAVE) &&
          (engine.selected_action == WORKOUT_ACTION_UNKNOWN));
    /* 第四窗继续提交挥手，形成两个连续独立窗口。 */
    CHECK(workout_engine_push_inference(&engine, logits, 1440ULL, 0U, &locked) ==
          WORKOUT_STATUS_OK);
    /* 引擎必须按连续独立证据锁定挥手，而非累计惯性中的深蹲。 */
    CHECK(locked &&
          (engine.state == WORKOUT_STATE_RUNNING) &&
          (engine.selected_action == (uint8_t)FITNESS_ACTION_WAVE));
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
    /* 清空全部固定状态。 */
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

/* 验证重建后完整 62 点窗可用于锁类，而真正推理失败窗仍必须拒绝。 */
static int test_preparing_accepts_recovered_windows_only(void)
{
    /* 创建独立引擎，避免其它候选窗口污染累计。 */
    workout_engine_t engine;
    /* 初始化为空闲。 */
    workout_engine_init(&engine);
    /* 开始新会话并进入准备态。 */
    CHECK(workout_engine_start(&engine, 80U, 70000U, 0ULL) == WORKOUT_STATUS_OK);
    /* 构造高置信开合跳 logits。 */
    float logits[WORKOUT_CLASS_COUNT];
    /* 目标类别固定为开合跳。 */
    make_logits(logits, (uint8_t)FITNESS_ACTION_JUMPING_JACK);
    /* locked 保存本窗是否提交主动作。 */
    bool locked = true;
    /* 0x41 表示窗口在重采样重建后形成；62 点本身完整，第一窗应成为候选。 */
    CHECK(workout_engine_push_inference(
              &engine,
              logits,
              2440ULL,
              UINT16_C(0x0041),
              &locked) == WORKOUT_STATUS_IGNORED);
    /* 第一窗不能锁类，但必须累计为一份开合跳证据。 */
    CHECK(!locked &&
          (engine.bout_window_count == 1U) &&
          (engine.candidate_windows == 1U) &&
          (engine.inferred_action == (uint8_t)FITNESS_ACTION_JUMPING_JACK));
    /* 第二个重建后完整窗同样可信，应完成两窗确认。 */
    CHECK(workout_engine_push_inference(
              &engine,
              logits,
              2920ULL,
              UINT16_C(0x0043),
              &locked) == WORKOUT_STATUS_OK);
    /* 会话必须在正常 62+12 点时间尺度内进入运行。 */
    CHECK(locked &&
          (engine.state == WORKOUT_STATE_RUNNING) &&
          (engine.selected_action == (uint8_t)FITNESS_ACTION_JUMPING_JACK));

    /* 创建第二个引擎，单独验证模型失败质量位。 */
    workout_engine_t failed_engine;
    /* 初始化为空闲。 */
    workout_engine_init(&failed_engine);
    /* 开始独立会话。 */
    CHECK(workout_engine_start(&failed_engine, 81U, 70000U, 0ULL) ==
          WORKOUT_STATUS_OK);
    /* 推理失败窗即使 logits 看似高置信也必须拒绝。 */
    CHECK(workout_engine_push_inference(
              &failed_engine,
              logits,
              2440ULL,
              TEST_IMU_QUALITY_INFERENCE_FAILED,
              &locked) == WORKOUT_STATUS_IGNORED);
    /* 失败窗不能进入动作段累计或候选连续数。 */
    CHECK((failed_engine.bout_window_count == 0U) &&
          (failed_engine.candidate_windows == 0U) &&
          (failed_engine.state == WORKOUT_STATE_PREPARING));
    /* 测试通过。 */
    return 0;
}

/* 验证准备期补算形成的每次计数都保留原始 IMU 时刻并可按序交给 BLE/CSV。 */
static int test_prelock_replay_exposes_all_metric_events(void)
{
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
    /* 缓存两个完整手腕往返周期，代表锁类等待期已经发生的前两次开合跳。 */
    for (uint8_t repetition = 0U; repetition < 2U; ++repetition) {
        /* 每个自然周期固定 30 点，即 1.2 秒。 */
        for (uint8_t point = 0U; point < 30U; ++point) {
            /* 默认保持低幅主向运动，避免把过渡段错误识别为静止。 */
            float gyro_x_dps = 12.0F;
            /* 周期开头三点构成手臂上行主向端点。 */
            if (point < 3U) {
                /* 两次周期轻微改变幅度，验证回放不依赖完全标准动作。 */
                gyro_x_dps = 85.0F + (float)repetition * 5.0F;
            } else if ((point >= 10U) && (point < 13U)) {
                /* 中段三点构成手臂下行回向端点。 */
                gyro_x_dps = -90.0F + (float)repetition * 4.0F;
            } else if ((point >= 13U) && (point < 25U)) {
                /* 回向过渡使用低幅负值，避免重复端点。 */
                gyro_x_dps = -12.0F;
            } else if ((point >= 25U) && (point < 28U)) {
                /* 返回主向端点闭合一个完整开合跳周期。 */
                gyro_x_dps = 88.0F - (float)repetition * 3.0F;
            }
            /* 加速度保持约 1 g，证明计数来自完整手腕运动而不是落地冲击。 */
            const motion_phase_sample_t sample = make_sample(
                time_ms,
                gyro_x_dps,
                1.05F);
            /* 准备态仍只缓存，不允许直接发事件。 */
            CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
                  WORKOUT_STATUS_IGNORED);
            /* 每个缓存点都必须保持未公开。 */
            CHECK(!emitted);
            /* 推进一个 40 ms 点。 */
            time_ms += MOTION_PHASE_SAMPLE_PERIOD_MS;
        }
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

/* 验证准备锁定、深蹲完整计数和暂停时间排除。 */
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
    /* 第二个真实稳定点确认 REST，但领域层仍等待接口保持事件。 */
    sample = make_sample(1760ULL, 0.0F, 1.0F);
    /* 当前点只确认物理端点，不应提前生成次数。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 第三个输入点承载已确认 REST 的接口保持事件。 */
    sample = make_sample(1800ULL, 0.0F, 1.0F);
    /* 必须生成唯一事件。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 检查事件存在。 */
    CHECK(emitted);
    /* 检查动作、单位和总次数。 */
    CHECK((event.action == FITNESS_ACTION_SQUAT) &&
          (event.metric_kind == FITNESS_METRIC_REPETITION) &&
          (event.total_value == 1ULL));
    /* 保存暂停前热量。 */
    const uint64_t calories_before_pause = engine.fitness_session.gross_microkcal;
    /* 暂停会话。 */
    CHECK(workout_engine_pause(&engine, 1840ULL) == WORKOUT_STATUS_OK);
    /* 暂停期样本被忽略。 */
    sample = make_sample(5000ULL, 0.0F, 1.0F);
    /* 检查有意忽略。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_IGNORED);
    /* 暂停期热量不变。 */
    CHECK(engine.fitness_session.gross_microkcal == calories_before_pause);
    /* 10 秒后恢复。 */
    CHECK(workout_engine_resume(&engine, 11840ULL) == WORKOUT_STATUS_OK);
    /* 恢复后 40 ms 点正常推进，不含暂停 10 秒。 */
    sample = make_sample(11880ULL, 0.0F, 1.0F);
    /* 处理成功。 */
    CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
          WORKOUT_STATUS_OK);
    /* 停止会话。 */
    CHECK(workout_engine_stop(&engine, 11940ULL) == WORKOUT_STATUS_OK);
    /* 重复停止幂等。 */
    CHECK(workout_engine_stop(&engine, 11940ULL) == WORKOUT_STATUS_OK);
    /* 当前状态为总结。 */
    CHECK(engine.state == WORKOUT_STATE_SUMMARY);
    /* 测试通过。 */
    return 0;
}


/* 验证开合跳按手腕主向、回向、返回的完整周期计数，不把开合冲击拆成两次。 */
static int test_jumping_jack_counts_complete_wrist_cycles(void)
{
    /* 创建独立训练引擎。 */
    workout_engine_t engine;
    /* 清空会话、活动门和计数器。 */
    workout_engine_init(&engine);
    /* 用户点击开始。 */
    CHECK(workout_engine_start(&engine, 82U, 70000U, 0ULL) == WORKOUT_STATUS_OK);
    /* 两个模型窗确认本轮主动作。 */
    CHECK(lock_action(&engine, FITNESS_ACTION_JUMPING_JACK, 0ULL) == 0);
    /* event 接收每个完整周期产生的唯一权威事实。 */
    fitness_metric_event_t event;
    /* emitted_count 统计 UI/BLE 应看到的逐次增加。 */
    uint32_t emitted_count = 0U;
    /* 十个周期使用 1.2 秒自然节奏，避免依赖最快合成动作。 */
    for (uint8_t cycle = 0U; cycle < 10U; ++cycle) {
        /* 每周期 30 个 25 Hz 点。 */
        for (uint8_t point = 0U; point < 30U; ++point) {
            /* gx 保存当前手臂方向角速度，单位 deg/s。 */
            float gyro_x_dps = 12.0F;
            /* 周期前段是手臂上行主向，连续三点越过自适应门。 */
            if (point < 3U) {
                /* 主向幅度允许每轮轻微变化，模拟人的动作差异。 */
                gyro_x_dps = 85.0F + (float)(cycle % 3U) * 5.0F;
            } else if ((point >= 10U) && (point < 13U)) {
                /* 中段手臂回落为反向，连续三点形成真实回向端点。 */
                gyro_x_dps = -90.0F + (float)(cycle % 2U) * 6.0F;
            } else if ((point >= 13U) && (point < 25U)) {
                /* 回向过渡保持低幅负值，不产生第二个回向端点。 */
                gyro_x_dps = -12.0F;
            } else if ((point >= 25U) && (point < 28U)) {
                /* 返回主向端点闭合一次完整开合跳，并成为下一周期起点。 */
                gyro_x_dps = 88.0F - (float)(cycle % 3U) * 4.0F;
            }
            /* 设备时间从 1 秒开始，所有点严格相隔 40 ms。 */
            const uint64_t time_ms =
                UINT64_C(1000) +
                ((uint64_t)cycle * UINT64_C(1200)) +
                ((uint64_t)point * UINT64_C(40));
            /* 加速度保持接近 1 g；计数必须来自完整手腕方向而非两次落地冲击。 */
            const motion_phase_sample_t sample = make_sample(
                time_ms,
                gyro_x_dps,
                1.05F);
            /* 默认当前点没有事件。 */
            bool emitted = false;
            /* 干净活动点进入生产引擎。 */
            CHECK(workout_engine_push_sample(
                      &engine,
                      &sample,
                      true,
                      0U,
                      &event,
                      &emitted) == WORKOUT_STATUS_OK);
            /* 完整周期闭合时累计一次。 */
            if (emitted) {
                /* 每条事件必须是开合跳 repetition 且累计严格递增。 */
                CHECK((event.action == FITNESS_ACTION_JUMPING_JACK) &&
                      (event.metric_kind == FITNESS_METRIC_REPETITION) &&
                      (event.total_value == (uint64_t)emitted_count + 1ULL));
                /* UI/BLE 可见次数增加一。 */
                emitted_count += 1U;
            }
        }
    }
    /* 十个完整上行回落周期必须恰好输出十次。 */
    CHECK(emitted_count == 10U);
    /* 会话权威累计与事件数一致。 */
    CHECK(engine.fitness_session.repetitions == 10ULL);
    /* 测试通过。 */
    return 0;
}

/* 验证 walk 步峰和权威步数。 */
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

/* 验证运行期模型只保留诊断，不能再以单窗 Top-1 冻结已选动作计数器。 */
static int test_running_inference_is_diagnostic_only(void)
{
    /* 创建独立引擎，避免其它会话的活动窗和相位状态污染本测试。 */
    workout_engine_t engine;
    /* 初始化全部静态状态。 */
    workout_engine_init(&engine);
    /* 开始一个固定右腕、单动作训练会话。 */
    CHECK(workout_engine_start(&engine, 81U, 70000U, 0ULL) == WORKOUT_STATUS_OK);
    /* 用两个干净高置信窗口确认本轮主动作为深蹲。 */
    CHECK(lock_action(&engine, FITNESS_ACTION_SQUAT, 0ULL) == 0);
    /* 锁定完成时计数门应允许已经缓存的动作继续推进。 */
    CHECK(engine.classification_consistent);

    /* logits 保存一个与主动作完全不同的高置信静坐诊断窗。 */
    float logits[WORKOUT_CLASS_COUNT];
    /* 静坐写入 +20，其余类别写入 -20，确保旧 Top-1 门必然触发冻结。 */
    make_scaled_logits(logits, FITNESS_ACTION_SIT, 20.0F, -20.0F);
    /* RUNNING 不得再次产生动作锁定事件。 */
    bool locked = true;
    /* 提交异类诊断窗；它不能重建计数器或修改会话主动作。 */
    CHECK(workout_engine_push_inference(&engine, logits, 960ULL, 0U, &locked) ==
          WORKOUT_STATUS_OK);
    /* 运行期推理只更新诊断，因此不会产生第二次锁定。 */
    CHECK(!locked);
    /* 最近模型诊断应如实保留静坐，供 CSV 分析而不参与业务计数。 */
    CHECK(engine.inferred_action == (uint8_t)FITNESS_ACTION_SIT);
    /* 本轮主动作必须始终保持深蹲。 */
    CHECK(engine.selected_action == (uint8_t)FITNESS_ACTION_SQUAT);
    /* 异类 Top-1 不能关闭计数；后续只由训练域活动门和质量边界控制。 */
    CHECK(engine.classification_consistent);
    /* 测试通过。 */
    return 0;
}

/* 验证训练集冻结的 25 点活动门独立控制休息和恢复，不等待下一模型窗口。 */
static int test_activity_gate_freezes_rest_and_recovers_from_samples(void)
{
    /* 创建独立引擎，验证活动窗从空状态因果建立。 */
    workout_engine_t engine;
    /* 初始化全部静态字段。 */
    workout_engine_init(&engine);
    /* 开始固定单动作会话。 */
    CHECK(workout_engine_start(&engine, 82U, 70000U, 0ULL) == WORKOUT_STATUS_OK);
    /* 选择普通深蹲计数器。 */
    CHECK(lock_action(&engine, FITNESS_ACTION_SQUAT, 0ULL) == 0);
    /* event 接收可能产生的唯一指标事实。 */
    fitness_metric_event_t event;
    /* emitted 保存当前点是否闭合完整动作；纯静止阶段必须始终为 false。 */
    bool emitted = false;
    /* 先输入完整 25 点明显运动窗，表示用户已经开始本轮动作并武装休息检测。 */
    for (uint32_t point = 0U; point < 25U; ++point) {
        /* 每点严格间隔 40 ms；80 deg/s 使整体和逐点活动门均可靠过阈值。 */
        motion_phase_sample_t sample = make_sample(
            520ULL + ((uint64_t)point * 40ULL),
            (point & 1U) == 0U ? 80.0F : -80.0F,
            1.0F);
        /* 运动点质量正常并进入活动统计。 */
        CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
              WORKOUT_STATUS_OK);
    }
    /* 首个完整活动窗后计数门保持打开。 */
    CHECK(engine.classification_consistent);

    /* 再输入完整 25 点 1 g、零角速度静止窗，模拟动作中途休息。 */
    for (uint32_t point = 0U; point < 25U; ++point) {
        /* 时间从 1520 ms 连续推进，六轴物理量表示腕表稳定静止。 */
        motion_phase_sample_t sample = make_sample(
            1520ULL + ((uint64_t)point * 40ULL),
            0.0F,
            1.0F);
        /* 静止点质量正常，但不得产生动作次数。 */
        CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
              WORKOUT_STATUS_OK);
        /* 静止窗不能产生深蹲次数。 */
        CHECK(!emitted);
    }
    /* 满 1 秒静止证据后活动门必须关闭，显示与累计进入休息状态。 */
    CHECK(!engine.classification_consistent);

    /* 输入五个明显腕部运动点；它们满足训练端 5/25 活动点合同。 */
    for (uint32_t point = 0U; point < 5U; ++point) {
        /* 80 deg/s 角速度使逐点活动分数超过冻结训练阈值，时间继续严格单调。 */
        motion_phase_sample_t sample = make_sample(
            2520ULL + ((uint64_t)point * 40ULL),
            80.0F,
            1.0F);
        /* 活动样本应被接受；相位尚未完整闭合，不要求产生次数。 */
        CHECK(workout_engine_push_sample(&engine, &sample, true, 0U, &event, &emitted) ==
              WORKOUT_STATUS_OK);
    }
    /* 滚动窗达到训练域活动条件后必须自动恢复，无需等待 2.48 秒推理窗。 */
    CHECK(engine.classification_consistent);
    /* 测试通过。 */
    return 0;
}

/* 验证时间线质量边界只清空未完成周期，不把活动门绑回下一次模型推理。 */
static int test_timeline_break_preserves_activity_gate(void)
{
    /* 创建独立引擎。 */
    workout_engine_t engine;
    /* 初始化为空闲。 */
    workout_engine_init(&engine);
    /* 开始并锁定普通深蹲。 */
    CHECK(workout_engine_start(&engine, 83U, 70000U, 0ULL) == WORKOUT_STATUS_OK);
    /* 两窗确认主动作。 */
    CHECK(lock_action(&engine, FITNESS_ACTION_SQUAT, 0ULL) == 0);
    /* 锁定后活动门处于可计数状态。 */
    CHECK(engine.classification_consistent);
    /* 标记一次真实连续性边界；权威累计与主动作均应保留。 */
    workout_engine_reset_bout_evidence(&engine);
    /* 边界只切断半周期，不能等待新的同类模型窗才能恢复。 */
    CHECK(engine.classification_consistent);
    /* 主动作保持普通深蹲。 */
    CHECK(engine.selected_action == (uint8_t)FITNESS_ACTION_SQUAT);
    /* 测试通过。 */
    return 0;
}

/* 验证多个连续异类诊断窗也不能更换主动作或回滚累计。 */
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
    /* 锁定后活动门先允许准备期动作继续。 */
    CHECK(engine.classification_consistent);
    /* 预置两个已完成重复，验证诊断抖动不改累计。 */
    engine.fitness_session.repetitions = 2ULL;
    /* 构造高置信 tuck_jump 单窗噪声。 */
    float logits[WORKOUT_CLASS_COUNT];
    /* tuck_jump 写 +20，其余写 -20，形成极强但不应生效的异类诊断窗。 */
    make_scaled_logits(logits, FITNESS_ACTION_TUCK_JUMP, 20.0F, -20.0F);
    /* RUNNING 更新不得再次报告动作锁定。 */
    bool locked = true;
    /* 连续提交三个异类窗口，覆盖单窗毛刺和持续模型域偏差。 */
    for (uint8_t window = 0U; window < 3U; ++window) {
        /* 第二窗携带真板常见质量位，验证质量只进入诊断而不冻结到下一推理。 */
        const uint16_t quality_flags = window == 1U ? UINT16_C(0x0041) : 0U;
        /* 每个窗口相隔 480 ms，时间与生产重叠推理一致。 */
        CHECK(workout_engine_push_inference(
                  &engine,
                  logits,
                  1440ULL + ((uint64_t)window * 480ULL),
                  quality_flags,
                  &locked) ==
              WORKOUT_STATUS_OK);
        /* 运行阶段不会产生第二次锁定事件。 */
        CHECK(!locked);
        /* 实时诊断如实保存收腹跳。 */
        CHECK(engine.inferred_action == (uint8_t)FITNESS_ACTION_TUCK_JUMP);
        /* 会话、重复计数器和页面主动作均保持深蹲。 */
        CHECK(engine.selected_action == (uint8_t)FITNESS_ACTION_SQUAT);
        /* fitness_core 的权威会话动作不得改变。 */
        CHECK(engine.fitness_session.action == FITNESS_ACTION_SQUAT);
        /* 重复计数器动作不得被诊断覆盖。 */
        CHECK(engine.rep_counter.action == FITNESS_ACTION_SQUAT);
        /* 模型窗口不控制活动门。 */
        CHECK(engine.classification_consistent);
    }
    /* 三个异类窗后权威累计仍保持原来的两次。 */
    CHECK(engine.fitness_session.repetitions == 2ULL);
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
    /* 保存质量边界前诊断累计窗口数；RUNNING 不应清除此只读诊断。 */
    const uint32_t windows_before_reset = engine.bout_window_count;
    /* 标记 IMU 间断或设备重连。 */
    workout_engine_reset_bout_evidence(&engine);
    /* RUNNING 诊断累计不参与计数，边界无需清零或等待新模型窗。 */
    CHECK(engine.bout_window_count == windows_before_reset);
    /* 证据重置后稳定显示仍保持本次唯一动作。 */
    CHECK(engine.inferred_action == (uint8_t)FITNESS_ACTION_SQUAT);
    /* 连续性重置只清半周期，当前活动门状态保持。 */
    CHECK(engine.classification_consistent);
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
    /* 验证首窗误判只作候选，累计稳定后锁定真实动作。 */
    CHECK(test_preparing_rejects_transient_first_window() == 0);
    /* 验证独立窗口连续性可压过强错误首窗的累计惯性。 */
    CHECK(test_preparing_confirms_consecutive_independent_windows() == 0);
    /* 验证低置信会话最多等待四个重叠窗口。 */
    CHECK(test_preparing_forces_cumulative_class_at_four_windows() == 0);
    /* 验证一次窗口重建后第四窗锁类前完整 160 点动作都可补算。 */
    CHECK(test_preparing_retains_full_four_window_span() == 0);
    /* 验证准备期连续性告警保留缓存和干净分类候选。 */
    CHECK(test_preparing_timeline_break_preserves_buffer_and_candidate() == 0);
    /* 验证锁类补算的每条计数都能按原始时间进入 BLE/CSV。 */
    CHECK(test_prelock_replay_exposes_all_metric_events() == 0);
    /* 验证重复计数和暂停。 */
    CHECK(test_repetition_flow() == 0);
    /* 验证十个完整手腕正反周期逐次公开十次。 */
    CHECK(test_jumping_jack_counts_complete_wrist_cycles() == 0);
    /* 验证重建后完整窗口可锁类，模型失败窗口仍拒绝。 */
    CHECK(test_preparing_accepts_recovered_windows_only() == 0);
    /* 验证步数链路。 */
    CHECK(test_step_flow() == 0);
    /* 验证准备异常边界。 */
    CHECK(test_prepare_boundaries() == 0);
    /* 验证运行期异类推理只进入诊断，不能冻结主动作计数。 */
    CHECK(test_running_inference_is_diagnostic_only() == 0);
    /* 验证样本级活动门在静止后冻结并由真实运动自动恢复。 */
    CHECK(test_activity_gate_freezes_rest_and_recovers_from_samples() == 0);
    /* 验证质量边界不再把计数恢复绑定到下一模型窗口。 */
    CHECK(test_timeline_break_preserves_activity_gate() == 0);
    /* 验证运行阶段保持准备态累计锁定动作且不因异类诊断冻结。 */
    CHECK(test_running_action_remains_locked() == 0);
    /* 验证证据重置不破坏已锁定会话。 */
    CHECK(test_bout_evidence_reset_preserves_session() == 0);
    /* 输出稳定成功摘要。 */
    (void)printf("workout_engine host tests: PASS (%u assertions)\n", g_assertions);
    /* 返回成功。 */
    return 0;
}
