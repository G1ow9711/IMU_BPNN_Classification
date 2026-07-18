/* 主机端测试覆盖计数、会话、整数热量、振动队列和污染插值的正常/边界路径。 */
#include "fitness_core.h"

/* assert 在条件失败时立即终止并给出源码行号，适合无第三方依赖的主机测试。 */
#include <assert.h>
/* fabsf 用于比较线性插值的 float 结果。 */
#include <math.h>
/* printf 输出单一成功摘要，便于 CI 和开发者确认测试数量。 */
#include <stdio.h>
/* memset 用于构造确定性测试输入。 */
#include <string.h>

/* 测试断言总数由每个检查显式累加，避免只显示“程序没崩”。 */
static unsigned int g_assertion_count = 0U;

/* 对布尔条件执行 assert，并统计一个已运行检查。 */
#define CHECK(condition)            \
    do {                            \
        /* 统计当前检查。 */         \
        g_assertion_count += 1U;    \
        /* 条件为假时停止测试。 */   \
        assert(condition);          \
    } while (0)

/* 对 float 使用 1e-5 绝对容差，覆盖本测试中简单线性插值的舍入误差。 */
static void check_float_close(const float actual, const float expected)
{
    /* 绝对误差必须小于 1e-5。 */
    CHECK(fabsf(actual - expected) < 1.0e-5F);
}

/* 连续输入同一相位两点，以满足状态机 2 点稳定门槛。 */
static bool feed_stable_phase(
    fitness_rep_counter_t *counter,
    const fitness_motion_phase_t phase,
    const uint64_t first_ms)
{
    /* 保存第一次相位输入结果；首次只建立候选。 */
    bool completed = false;
    /* 第一点必须被正常接受。 */
    CHECK(fitness_rep_counter_update(counter, phase, first_ms, &completed) == FITNESS_STATUS_OK);
    /* 单点不能完成重复。 */
    CHECK(!completed);
    /* 第二点比第一点晚 40 ms，模拟 25 Hz 采样。 */
    CHECK(fitness_rep_counter_update(counter, phase, first_ms + 40ULL, &completed) == FITNESS_STATUS_OK);
    /* 返回第二点是否闭合完整周期。 */
    return completed;
}

/* 验证 8 个重复动作都能初始化到正确状态机模式。 */
static void test_repetition_action_contract(void)
{
    /* 8 个动作固定集合，防止未来枚举调整漏改状态机映射。 */
    const fitness_action_t actions[8] = {
        FITNESS_ACTION_GOOD_MORNING,
        FITNESS_ACTION_JUMPING_JACK,
        FITNESS_ACTION_JUMPING_LUNGE,
        FITNESS_ACTION_JUMPING_SQUAT,
        FITNESS_ACTION_LUNGE,
        FITNESS_ACTION_SQUAT,
        FITNESS_ACTION_TUCK_JUMP,
        FITNESS_ACTION_WAVE
    };

    /* 遍历全部 8 类，逐一验证初始化和模式。 */
    for (size_t index = 0U; index < 8U; ++index) {
        /* 每类使用独立栈状态，避免前一类残留。 */
        fitness_rep_counter_t counter;
        /* 初始化必须成功。 */
        CHECK(fitness_rep_counter_init(&counter, actions[index]) == FITNESS_STATUS_OK);
        /* initialized 必须置真。 */
        CHECK(counter.initialized);
        /* 四个 jumping/tuck 类应采用跳跃状态机。 */
        const bool expected_jump =
            (actions[index] == FITNESS_ACTION_JUMPING_JACK) ||
            (actions[index] == FITNESS_ACTION_JUMPING_LUNGE) ||
            (actions[index] == FITNESS_ACTION_JUMPING_SQUAT) ||
            (actions[index] == FITNESS_ACTION_TUCK_JUMP);
        /* 实际模式必须与动作物理类型一致。 */
        CHECK((counter.mode == FITNESS_REP_MODE_JUMP) == expected_jump);
    }

    /* sit 不允许使用重复计数器。 */
    fitness_rep_counter_t invalid_counter;
    /* 非重复动作应返回参数错误。 */
    CHECK(fitness_rep_counter_init(&invalid_counter, FITNESS_ACTION_SIT) == FITNESS_STATUS_INVALID_ARGUMENT);
}

/* 验证两相位动作只有完整顺序和合理时长才计一次。 */
static void test_two_phase_counter(void)
{
    /* 使用 squat 代表两相位力量动作。 */
    fitness_rep_counter_t counter;
    /* 初始化 squat 状态机。 */
    CHECK(fitness_rep_counter_init(&counter, FITNESS_ACTION_SQUAT) == FITNESS_STATUS_OK);
    /* PRIMARY 两点开启周期但不计数。 */
    CHECK(!feed_stable_phase(&counter, FITNESS_PHASE_PRIMARY, 100ULL));
    /* SECONDARY 两点确认到达最低点但不计数。 */
    CHECK(!feed_stable_phase(&counter, FITNESS_PHASE_SECONDARY, 400ULL));
    /* REST 在 800/840 ms 闭合约 700 ms 周期，应计一次。 */
    CHECK(feed_stable_phase(&counter, FITNESS_PHASE_REST, 800ULL));
    /* 累计次数必须为 1。 */
    CHECK(counter.total_repetitions == 1ULL);

    /* 只输入 PRIMARY 后直接 REST，缺少 SECONDARY，不能计数。 */
    CHECK(!feed_stable_phase(&counter, FITNESS_PHASE_PRIMARY, 1200ULL));
    /* 错序 REST 不闭合周期。 */
    CHECK(!feed_stable_phase(&counter, FITNESS_PHASE_REST, 2000ULL));
    /* 总次数保持 1。 */
    CHECK(counter.total_repetitions == 1ULL);

    /* 显式重置不完整周期。 */
    CHECK(fitness_rep_counter_reset_cycle(&counter) == FITNESS_STATUS_OK);
    /* 重置不能清除已累计次数。 */
    CHECK(counter.total_repetitions == 1ULL);
}

/* 验证跳跃动作必须经过起跳、腾空、落地、恢复和基线。 */
static void test_jump_counter(void)
{
    /* 使用 jumping_squat 代表五阶段跳跃。 */
    fitness_rep_counter_t counter;
    /* 初始化跳跃状态机。 */
    CHECK(fitness_rep_counter_init(&counter, FITNESS_ACTION_JUMPING_SQUAT) == FITNESS_STATUS_OK);
    /* 起跳阶段开启周期。 */
    CHECK(!feed_stable_phase(&counter, FITNESS_PHASE_TAKEOFF, 100ULL));
    /* 腾空阶段推进状态。 */
    CHECK(!feed_stable_phase(&counter, FITNESS_PHASE_FLIGHT, 220ULL));
    /* 落地阶段推进状态。 */
    CHECK(!feed_stable_phase(&counter, FITNESS_PHASE_LANDING, 380ULL));
    /* 恢复阶段推进状态。 */
    CHECK(!feed_stable_phase(&counter, FITNESS_PHASE_RECOVERY, 500ULL));
    /* 回到基线闭合完整跳跃。 */
    CHECK(feed_stable_phase(&counter, FITNESS_PHASE_REST, 680ULL));
    /* 累计次数必须为 1。 */
    CHECK(counter.total_repetitions == 1ULL);

    /* 时间倒退必须被明确拒绝。 */
    bool completed = false;
    /* 600 ms 早于上一输入 720 ms。 */
    CHECK(fitness_rep_counter_update(&counter, FITNESS_PHASE_REST, 600ULL, &completed) ==
          FITNESS_STATUS_INVALID_TIME);
}

/* 验证 walk/trot 步峰去重和每 10 步反馈规则。 */
static void test_step_counter(void)
{
    /* 行走最短步间隔应为 240 ms。 */
    fitness_step_counter_t counter;
    /* 初始化 walk 计数器。 */
    CHECK(fitness_step_counter_init(&counter, FITNESS_ACTION_WALK) == FITNESS_STATUS_OK);

    /* 保存每次调用的接受与反馈结果。 */
    bool accepted = false;
    /* 保存是否到达 10 步反馈点。 */
    bool haptic_due = false;
    /* 首步无历史间隔，应被接受。 */
    CHECK(fitness_step_counter_accept(&counter, 1000ULL, &accepted, &haptic_due) == FITNESS_STATUS_OK);
    /* 首步接受且不振动。 */
    CHECK(accepted && !haptic_due);
    /* 100 ms 后的重复峰过密，应静默拒绝。 */
    CHECK(fitness_step_counter_accept(&counter, 1100ULL, &accepted, &haptic_due) == FITNESS_STATUS_OK);
    /* 过密峰不计步。 */
    CHECK(!accepted && !haptic_due);

    /* 再输入 9 个合法步峰，使总步数达到 10。 */
    for (uint64_t step = 2ULL; step <= 10ULL; ++step) {
        /* 每步相隔 300 ms，大于 walk 最短间隔。 */
        const uint64_t timestamp_ms = 1000ULL + ((step - 1ULL) * 300ULL);
        /* 合法步峰必须成功。 */
        CHECK(fitness_step_counter_accept(&counter, timestamp_ms, &accepted, &haptic_due) == FITNESS_STATUS_OK);
        /* 每个峰均被接受。 */
        CHECK(accepted);
        /* 只有第 10 步应触发反馈。 */
        CHECK(haptic_due == (step == 10ULL));
    }
    /* 总步数必须恰好为 10。 */
    CHECK(counter.total_steps == 10ULL);

    /* squat 不能初始化为步计数器。 */
    CHECK(fitness_step_counter_init(&counter, FITNESS_ACTION_SQUAT) == FITNESS_STATUS_INVALID_ARGUMENT);
}

/* 验证会话事件序号、计数总值和动作匹配规则。 */
static void test_session_metric_event(void)
{
    /* 建立 70 kg squat 会话。 */
    fitness_session_t session;
    /* 会话序号 42，起始时刻 1000 ms。 */
    CHECK(fitness_session_start(&session, 42U, FITNESS_ACTION_SQUAT, 70000U, 1000ULL) ==
          FITNESS_STATUS_OK);
    /* 先推进 1 秒以累计热量。 */
    fitness_metric_event_t unused_sit_event;
    /* 非 sit tick 不应发时长事件。 */
    bool emitted = true;
    /* 推进成功。 */
    CHECK(fitness_session_tick(&session, 2000ULL, 30000U, 0U, &unused_sit_event, &emitted) ==
          FITNESS_STATUS_OK);
    /* squat 不产生 duration 事件。 */
    CHECK(!emitted);
    /* 3.8 MET、70 kg、1 s 的毛热量向下取整为 77583 microkcal。 */
    CHECK(session.gross_microkcal == 77583ULL);
    /* 扣除 1 MET 后活动热量为 57166 microkcal。 */
    CHECK(session.active_microkcal == 57166ULL);

    /* 记录一次重复。 */
    fitness_metric_event_t event;
    /* 事件时间不早于最近 tick。 */
    CHECK(fitness_session_record_count(
              &session,
              FITNESS_METRIC_REPETITION,
              1U,
              2040ULL,
              31000U,
              0U,
              &event) == FITNESS_STATUS_OK);
    /* 事件身份和序号正确。 */
    CHECK((event.session_seq == 42U) && (event.event_seq == 1U));
    /* 重复增量和总值正确。 */
    CHECK((event.delta_value == 1U) && (event.total_value == 1ULL));
    /* 事件携带当前毛热量快照。 */
    CHECK(event.gross_microkcal == session.gross_microkcal);
    /* squat 会话不能写 STEP。 */
    CHECK(fitness_session_record_count(
              &session,
              FITNESS_METRIC_STEP,
              1U,
              2080ULL,
              31000U,
              0U,
              &event) == FITNESS_STATUS_INVALID_STATE);
    /* 停止后禁止继续计数。 */
    CHECK(fitness_session_stop(&session) == FITNESS_STATUS_OK);
    /* 已停止会话写事件必须失败。 */
    CHECK(fitness_session_record_count(
              &session,
              FITNESS_METRIC_REPETITION,
              1U,
              2100ULL,
              31000U,
              0U,
              &event) == FITNESS_STATUS_INVALID_STATE);
}

/* 验证 sit 每整秒事件、热量余数和体重边界。 */
static void test_sit_duration_and_calories(void)
{
    /* 建立 70 kg sit 会话。 */
    fitness_session_t session;
    /* 起始时刻为 0。 */
    CHECK(fitness_session_start(&session, 7U, FITNESS_ACTION_SIT, 70000U, 0ULL) == FITNESS_STATUS_OK);
    /* 准备 duration 事件缓冲。 */
    fitness_metric_event_t event;
    /* 保存发出标记。 */
    bool emitted = false;
    /* 500 ms 不满整秒，只累计不发事件。 */
    CHECK(fitness_session_tick(&session, 500ULL, 32767U, 0U, &event, &emitted) == FITNESS_STATUS_OK);
    /* 半秒不发事件。 */
    CHECK(!emitted);
    /* 到 1000 ms 时应发布 1 秒。 */
    CHECK(fitness_session_tick(&session, 1000ULL, 32767U, 0U, &event, &emitted) == FITNESS_STATUS_OK);
    /* 发出 duration 事件。 */
    CHECK(emitted);
    /* delta 和 total 均为 1000 ms。 */
    CHECK((event.delta_value == 1000U) && (event.total_value == 1000ULL));
    /* 1 MET、70 kg、1 s 毛热量为 20416 microkcal。 */
    CHECK(session.gross_microkcal == 20416ULL);
    /* sit 扣除静息 1 MET 后活动热量为 0。 */
    CHECK(session.active_microkcal == 0ULL);

    /* 未设置体重允许会话运行，但热量保持 0。 */
    CHECK(fitness_session_start(&session, 8U, FITNESS_ACTION_WALK, 0U, 0ULL) == FITNESS_STATUS_OK);
    /* 推进 1 秒。 */
    CHECK(fitness_session_tick(&session, 1000ULL, 10000U, 0U, &event, &emitted) == FITNESS_STATUS_OK);
    /* 体重未知时不虚构热量。 */
    CHECK(session.gross_microkcal == 0ULL);
    /* 29.999 kg 低于合同下限，应拒绝。 */
    CHECK(fitness_session_start(&session, 9U, FITNESS_ACTION_WALK, 29999U, 0ULL) ==
          FITNESS_STATUS_INVALID_ARGUMENT);
    /* 250.001 kg 高于合同上限，应拒绝。 */
    CHECK(fitness_session_start(&session, 9U, FITNESS_ACTION_WALK, 250001U, 0ULL) ==
          FITNESS_STATUS_INVALID_ARGUMENT);
}

/* 验证整数余数使 25 个 40 ms tick 与单个 1000 ms tick 完全一致。 */
static void test_calorie_tick_invariance(void)
{
    /* one_tick 使用一次 1000 ms 推进。 */
    fitness_session_t one_tick;
    /* many_ticks 使用 25 次 40 ms 推进。 */
    fitness_session_t many_ticks;
    /* 两个会话使用相同动作和 70 kg 体重。 */
    CHECK(fitness_session_start(&one_tick, 100U, FITNESS_ACTION_SQUAT, 70000U, 0ULL) ==
          FITNESS_STATUS_OK);
    /* 第二个会话参数与第一个完全一致，仅 session_seq 不同。 */
    CHECK(fitness_session_start(&many_ticks, 101U, FITNESS_ACTION_SQUAT, 70000U, 0ULL) ==
          FITNESS_STATUS_OK);

    /* tick API 需要事件缓冲；squat 不会实际填充 duration 事件。 */
    fitness_metric_event_t event;
    /* 保存事件发出标记。 */
    bool emitted = false;
    /* 一次推进整秒。 */
    CHECK(fitness_session_tick(&one_tick, 1000ULL, 30000U, 0U, &event, &emitted) ==
          FITNESS_STATUS_OK);
    /* squat 不应产生 sit 时长事件。 */
    CHECK(!emitted);

    /* 以 25 Hz 节奏推进相同总时长。 */
    for (uint64_t sample = 1ULL; sample <= 25ULL; ++sample) {
        /* 每个 tick 的右端点为 sample*40 ms。 */
        const uint64_t now_ms = sample * 40ULL;
        /* 每个短 tick 都必须成功且不发 sit 事件。 */
        CHECK(fitness_session_tick(&many_ticks, now_ms, 30000U, 0U, &event, &emitted) ==
              FITNESS_STATUS_OK);
        /* squat 始终不产生 duration 事件。 */
        CHECK(!emitted);
    }

    /* 毛热量整数部分必须完全一致。 */
    CHECK(many_ticks.gross_microkcal == one_tick.gross_microkcal);
    /* 毛热量余数也必须一致，保证未来 tick 仍不分叉。 */
    CHECK(many_ticks.gross_calorie_remainder == one_tick.gross_calorie_remainder);
    /* 活动热量整数部分必须完全一致。 */
    CHECK(many_ticks.active_microkcal == one_tick.active_microkcal);
    /* 活动热量余数也必须一致。 */
    CHECK(many_ticks.active_calorie_remainder == one_tick.active_calorie_remainder);

    /* 单次 60001 ms 超过断层上限，应拒绝而不累计。 */
    fitness_session_t gap_session;
    /* 建立独立会话。 */
    CHECK(fitness_session_start(&gap_session, 102U, FITNESS_ACTION_WALK, 70000U, 0ULL) ==
          FITNESS_STATUS_OK);
    /* 超长 tick 返回时间错误。 */
    CHECK(fitness_session_tick(&gap_session, 60001ULL, 30000U, 0U, &event, &emitted) ==
          FITNESS_STATUS_INVALID_TIME);
    /* 被拒绝的断层不能产生热量。 */
    CHECK(gap_session.gross_microkcal == 0ULL);
}

/* 验证暂停恢复只移动积分基准，不把暂停墙钟时间计入热量或时长。 */
static void test_session_rebase_time(void)
{
    /* 创建普通深蹲会话，体重 70 kg，起点为 1000 ms。 */
    fitness_session_t session;
    /* 启动会话。 */
    CHECK(fitness_session_start(&session, 103U, FITNESS_ACTION_SQUAT, 70000U, 1000ULL) ==
          FITNESS_STATUS_OK);
    /* 准备 tick 的可选 sit 事件缓冲；深蹲不会写入该事件。 */
    fitness_metric_event_t event;
    /* 保存是否产生 sit 事件。 */
    bool emitted = false;
    /* 先运行 1 秒，建立非零热量。 */
    CHECK(fitness_session_tick(&session, 2000ULL, 30000U, 0U, &event, &emitted) ==
          FITNESS_STATUS_OK);
    /* 保存暂停前毛热量。 */
    const uint64_t gross_before_pause = session.gross_microkcal;
    /* 模拟暂停 10 秒后恢复，只移动时间基准。 */
    CHECK(fitness_session_rebase_time(&session, 12000ULL) == FITNESS_STATUS_OK);
    /* 暂停区间不得增加热量。 */
    CHECK(session.gross_microkcal == gross_before_pause);
    /* 恢复后再运行 1 秒。 */
    CHECK(fitness_session_tick(&session, 13000ULL, 30000U, 0U, &event, &emitted) ==
          FITNESS_STATUS_OK);
    /* 总热量应大于暂停前，但只增加一秒对应值。 */
    CHECK(session.gross_microkcal > gross_before_pause);
    /* 倒退时间必须拒绝。 */
    CHECK(fitness_session_rebase_time(&session, 12999ULL) == FITNESS_STATUS_INVALID_TIME);
    /* 停止会话。 */
    CHECK(fitness_session_stop(&session) == FITNESS_STATUS_OK);
    /* 已停止会话不得恢复。 */
    CHECK(fitness_session_rebase_time(&session, 14000ULL) == FITNESS_STATUS_INVALID_STATE);
    /* 空指针必须拒绝。 */
    CHECK(fitness_session_rebase_time(NULL, 14000ULL) == FITNESS_STATUS_INVALID_ARGUMENT);
}

/* 验证振动 FIFO、固定波形和 MetricEvent 反馈规则。 */
static void test_haptic_queue(void)
{
    /* 初始化固定容量队列。 */
    fitness_haptic_queue_t queue;
    /* 清空索引和槽位。 */
    fitness_haptic_queue_init(&queue);
    /* 空队列元素数必须为 0。 */
    CHECK(queue.count == 0U);
    /* 入队开始双脉冲。 */
    CHECK(fitness_haptic_enqueue_reason(&queue, FITNESS_HAPTIC_REASON_START) == FITNESS_STATUS_OK);
    /* 入队一次重复脉冲。 */
    CHECK(fitness_haptic_enqueue_reason(&queue, FITNESS_HAPTIC_REASON_REPETITION) == FITNESS_STATUS_OK);

    /* 按 FIFO 取出开始波形。 */
    fitness_haptic_request_t request;
    /* 第一个请求存在。 */
    CHECK(fitness_haptic_queue_pop(&queue, &request));
    /* 开始波形为 2 次 20 ms，间隔 40 ms。 */
    CHECK((request.reason == FITNESS_HAPTIC_REASON_START) &&
          (request.on_ms == 20U) &&
          (request.off_ms == 40U) &&
          (request.repeat_count == 2U));
    /* 第二个请求存在。 */
    CHECK(fitness_haptic_queue_pop(&queue, &request));
    /* 重复波形为 1 次 30 ms。 */
    CHECK((request.reason == FITNESS_HAPTIC_REASON_REPETITION) &&
          (request.on_ms == 30U) &&
          (request.repeat_count == 1U));
    /* 队列已空。 */
    CHECK(!fitness_haptic_queue_pop(&queue, &request));

    /* 构造第 10 步事件验证每十步反馈。 */
    fitness_metric_event_t event;
    /* 清除未使用字段，避免测试依赖栈垃圾。 */
    memset(&event, 0, sizeof(event));
    /* 指标类型设为步数。 */
    event.metric_kind = FITNESS_METRIC_STEP;
    /* 总步数 10 应触发。 */
    event.total_value = 10ULL;
    /* 保存是否入队。 */
    bool enqueued = false;
    /* 根据事件入队。 */
    CHECK(fitness_haptic_enqueue_for_metric(&queue, &event, &enqueued) == FITNESS_STATUS_OK);
    /* 必须生成请求。 */
    CHECK(enqueued);
    /* 取出每十步波形。 */
    CHECK(fitness_haptic_queue_pop(&queue, &request));
    /* 原因为 STEP_BATCH。 */
    CHECK(request.reason == FITNESS_HAPTIC_REASON_STEP_BATCH);

    /* 总步数 11 不应振动。 */
    event.total_value = 11ULL;
    /* 正常处理但不入队。 */
    CHECK(fitness_haptic_enqueue_for_metric(&queue, &event, &enqueued) == FITNESS_STATUS_OK);
    /* 明确未入队。 */
    CHECK(!enqueued);

    /* 填满 16 个槽位验证容量边界。 */
    for (uint8_t index = 0U; index < FITNESS_HAPTIC_QUEUE_CAPACITY; ++index) {
        /* 每个低电量请求都应成功进入尚未满的槽位。 */
        CHECK(fitness_haptic_enqueue_reason(&queue, FITNESS_HAPTIC_REASON_LOW_BATTERY) ==
              FITNESS_STATUS_OK);
    }
    /* 第 17 个请求必须返回队列满且不覆盖旧请求。 */
    CHECK(fitness_haptic_enqueue_reason(&queue, FITNESS_HAPTIC_REASON_GOAL) ==
          FITNESS_STATUS_QUEUE_FULL);
}

/* 验证六种业务原因的完整固定振动目录。 */
static void test_haptic_pattern_catalog(void)
{
    /* 原因按枚举 0..5 排列。 */
    const fitness_haptic_reason_t reasons[FITNESS_HAPTIC_REASON_COUNT] = {
        FITNESS_HAPTIC_REASON_START,
        FITNESS_HAPTIC_REASON_REPETITION,
        FITNESS_HAPTIC_REASON_STEP_BATCH,
        FITNESS_HAPTIC_REASON_PAUSE_OR_END,
        FITNESS_HAPTIC_REASON_GOAL,
        FITNESS_HAPTIC_REASON_LOW_BATTERY
    };
    /* 每种原因的通电毫秒。 */
    const uint16_t expected_on_ms[FITNESS_HAPTIC_REASON_COUNT] = {20U, 30U, 30U, 40U, 25U, 40U};
    /* 每种原因的脉冲间隔毫秒。 */
    const uint16_t expected_off_ms[FITNESS_HAPTIC_REASON_COUNT] = {40U, 0U, 0U, 0U, 35U, 60U};
    /* 每种原因的脉冲次数。 */
    const uint8_t expected_repeats[FITNESS_HAPTIC_REASON_COUNT] = {2U, 1U, 1U, 1U, 3U, 2U};

    /* 使用空队列依次入队全部 6 种波形。 */
    fitness_haptic_queue_t queue;
    /* 清空队列。 */
    fitness_haptic_queue_init(&queue);
    /* 遍历固定目录。 */
    for (uint8_t index = 0U; index < FITNESS_HAPTIC_REASON_COUNT; ++index) {
        /* 当前合法原因应成功入队。 */
        CHECK(fitness_haptic_enqueue_reason(&queue, reasons[index]) == FITNESS_STATUS_OK);
    }
    /* 逐项按 FIFO 顺序取出并验证全部时序字段。 */
    for (uint8_t index = 0U; index < FITNESS_HAPTIC_REASON_COUNT; ++index) {
        /* 保存当前出队请求。 */
        fitness_haptic_request_t request;
        /* 队列中必须存在对应项。 */
        CHECK(fitness_haptic_queue_pop(&queue, &request));
        /* 原因顺序必须保持。 */
        CHECK(request.reason == reasons[index]);
        /* 通电时长必须匹配产品方案。 */
        CHECK(request.on_ms == expected_on_ms[index]);
        /* 脉冲间隔必须匹配产品方案。 */
        CHECK(request.off_ms == expected_off_ms[index]);
        /* 重复次数必须匹配产品方案。 */
        CHECK(request.repeat_count == expected_repeats[index]);
    }
}

/* 构造一个六轴所有通道取相同值的确定性样本。 */
static fitness_imu_sample_t make_uniform_sample(const uint64_t timestamp_ms, const float value)
{
    /* 清零质量位和可能的结构填充。 */
    fitness_imu_sample_t sample;
    /* 结构先归零。 */
    memset(&sample, 0, sizeof(sample));
    /* 保存单调采样时间。 */
    sample.monotonic_ms = timestamp_ms;
    /* 六轴均写入测试值，便于逐轴验证插值。 */
    for (uint8_t axis = 0U; axis < FITNESS_IMU_AXIS_COUNT; ++axis) {
        /* 写入当前轴。 */
        sample.axis[axis] = value;
    }
    /* 返回完整样本副本。 */
    return sample;
}

/* 验证 3 点线性插值和超过 3 点后的计数冻结策略。 */
static void test_haptic_contamination_guard(void)
{
    /* 初始化污染保护状态。 */
    fitness_haptic_guard_t guard;
    /* 清空端点和污染区间。 */
    fitness_haptic_guard_init(&guard);
    /* 输出最多 4 点。 */
    fitness_imu_sample_t outputs[FITNESS_HAPTIC_MAX_OUTPUT_SAMPLES];
    /* 保存实际输出数。 */
    size_t output_count = 0U;
    /* 保存硬冻结状态。 */
    bool frozen = false;

    /* 先输入 t=0、值 0 的干净左端点。 */
    fitness_imu_sample_t sample = make_uniform_sample(0ULL, 0.0F);
    /* 干净点应直接输出。 */
    CHECK(fitness_haptic_guard_push_sample(&guard, &sample, outputs, &output_count, &frozen) ==
          FITNESS_STATUS_OK);
    /* 输出 1 点且未冻结。 */
    CHECK((output_count == 1U) && !frozen);

    /* 电机从 40 ms 通电 30 ms，污染保护到 150 ms。 */
    CHECK(fitness_haptic_guard_mark_pulse(&guard, 40ULL, 30U) == FITNESS_STATUS_OK);
    /* 保护区边界内应标记污染。 */
    CHECK(fitness_haptic_guard_is_contaminated(&guard, 150ULL));
    /* 151 ms 已离开保护区。 */
    CHECK(!fitness_haptic_guard_is_contaminated(&guard, 151ULL));

    /* 输入 40/80/120 ms 三个污染点，均不立即输出。 */
    for (uint64_t timestamp_ms = 40ULL; timestamp_ms <= 120ULL; timestamp_ms += 40ULL) {
        /* 污染值故意设为 999，确保实现没有使用它插值。 */
        sample = make_uniform_sample(timestamp_ms, 999.0F);
        /* 输入成功。 */
        CHECK(fitness_haptic_guard_push_sample(&guard, &sample, outputs, &output_count, &frozen) ==
              FITNESS_STATUS_OK);
        /* 等待右端点时不输出，也未进入超过 3 点的硬冻结。 */
        CHECK((output_count == 0U) && !frozen);
    }

    /* 160 ms 干净右端点值为 16。 */
    sample = make_uniform_sample(160ULL, 16.0F);
    /* 应一次输出 3 个修复点和当前点。 */
    CHECK(fitness_haptic_guard_push_sample(&guard, &sample, outputs, &output_count, &frozen) ==
          FITNESS_STATUS_OK);
    /* 输出数量为 4。 */
    CHECK((output_count == 4U) && !frozen);
    /* 插值结果应为 4、8、12，当前干净点为 16。 */
    for (size_t index = 0U; index < output_count; ++index) {
        /* 每个样本六轴值相同，只需逐轴检查第一轴即可代表公式，再检查质量位。 */
        check_float_close(outputs[index].axis[0], 4.0F * (float)(index + 1U));
        /* 前三个点必须带污染和插值质量位。 */
        if (index < 3U) {
            /* 两个质量位都必须存在。 */
            CHECK((outputs[index].quality_flags &
                   (FITNESS_QUALITY_HAPTIC_CONTAMINATED | FITNESS_QUALITY_INTERPOLATED)) ==
                  (FITNESS_QUALITY_HAPTIC_CONTAMINATED | FITNESS_QUALITY_INTERPOLATED));
        }
    }

    /* 建立覆盖 200..480 ms 的长污染区。 */
    CHECK(fitness_haptic_guard_mark_pulse(&guard, 200ULL, 200U) == FITNESS_STATUS_OK);
    /* 连续输入 4 个污染点；第 4 点触发硬冻结。 */
    for (uint64_t timestamp_ms = 200ULL; timestamp_ms <= 320ULL; timestamp_ms += 40ULL) {
        /* 污染值不应进入算法。 */
        sample = make_uniform_sample(timestamp_ms, 500.0F);
        /* 输入成功。 */
        CHECK(fitness_haptic_guard_push_sample(&guard, &sample, outputs, &output_count, &frozen) ==
              FITNESS_STATUS_OK);
    }
    /* 第 4 点后必须冻结。 */
    CHECK(frozen);

    /* 520 ms 是保护区后的首个干净点。 */
    sample = make_uniform_sample(520ULL, 52.0F);
    /* 首个干净点恢复输出但不回填长污染段。 */
    CHECK(fitness_haptic_guard_push_sample(&guard, &sample, outputs, &output_count, &frozen) ==
          FITNESS_STATUS_OK);
    /* 只输出当前点，调用后解除冻结。 */
    CHECK((output_count == 1U) && !frozen);
    /* 恢复点带 COUNTER_FROZEN，提示上游重置不完整周期。 */
    CHECK((outputs[0].quality_flags & FITNESS_QUALITY_COUNTER_FROZEN) != 0U);
}

/* 验证开机立即振动、没有左侧干净端点时必须冻结而不能伪造插值。 */
static void test_haptic_guard_without_left_endpoint(void)
{
    /* 新 guard 没有 previous_clean。 */
    fitness_haptic_guard_t guard;
    /* 初始化空状态。 */
    fitness_haptic_guard_init(&guard);
    /* 电机从 0 ms 通电 30 ms，保护区到 110 ms。 */
    CHECK(fitness_haptic_guard_mark_pulse(&guard, 0ULL, 30U) == FITNESS_STATUS_OK);

    /* 40 ms 污染点无法插值。 */
    fitness_imu_sample_t sample = make_uniform_sample(40ULL, 99.0F);
    /* 准备最多 4 点输出缓冲。 */
    fitness_imu_sample_t outputs[FITNESS_HAPTIC_MAX_OUTPUT_SAMPLES];
    /* 保存输出数。 */
    size_t output_count = 0U;
    /* 保存冻结状态。 */
    bool frozen = false;
    /* 污染点被接受但不输出。 */
    CHECK(fitness_haptic_guard_push_sample(&guard, &sample, outputs, &output_count, &frozen) ==
          FITNESS_STATUS_OK);
    /* 无左端点时必须硬冻结。 */
    CHECK((output_count == 0U) && frozen);

    /* 120 ms 已离开保护区，是首个干净点。 */
    sample = make_uniform_sample(120ULL, 12.0F);
    /* 首个干净点恢复输出。 */
    CHECK(fitness_haptic_guard_push_sample(&guard, &sample, outputs, &output_count, &frozen) ==
          FITNESS_STATUS_OK);
    /* 只输出当前点并解除冻结。 */
    CHECK((output_count == 1U) && !frozen);
    /* 质量位要求上游重置不完整动作周期。 */
    CHECK((outputs[0].quality_flags & FITNESS_QUALITY_COUNTER_FROZEN) != 0U);
}

/* 测试入口按固定顺序运行全部领域测试。 */
int main(void)
{
    /* 验证动作到状态机映射。 */
    test_repetition_action_contract();
    /* 验证两相位完整周期。 */
    test_two_phase_counter();
    /* 验证跳跃五阶段完整周期。 */
    test_jump_counter();
    /* 验证步峰和每十步反馈点。 */
    test_step_counter();
    /* 验证会话与 MetricEvent。 */
    test_session_metric_event();
    /* 验证 sit 时长和整数热量。 */
    test_sit_duration_and_calories();
    /* 验证热量与 tick 划分无关。 */
    test_calorie_tick_invariance();
    /* 验证暂停恢复不累计暂停区间。 */
    test_session_rebase_time();
    /* 验证振动队列和波形。 */
    test_haptic_queue();
    /* 验证全部固定振动波形目录。 */
    test_haptic_pattern_catalog();
    /* 验证振动污染插值与冻结。 */
    test_haptic_contamination_guard();
    /* 验证开机振动缺少插值左端点的冻结路径。 */
    test_haptic_guard_without_left_endpoint();

    /* 输出机器可读成功摘要；没有失败时 main 返回 0。 */
    printf("fitness_core host tests: PASS (%u assertions)\n", g_assertion_count);
    /* 0 表示全部测试通过。 */
    return 0;
}
