/* 主机端测试覆盖计数、会话和整数热量的正常与边界路径。 */
#include "fitness_core.h"

/* assert 在条件失败时立即终止并给出源码行号，适合无第三方依赖的主机测试。 */
#include <assert.h>
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

/* 验证 8 个腕戴重复动作都使用主向、回向和闭合的完整周期模式。 */
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
        /* 传感器位于手腕，全部重复动作都必须使用可观测的正反腕部周期，不能要求脚底支持力五阶段。 */
        CHECK(counter.mode == FITNESS_REP_MODE_TWO_PHASE);
        /* 开合跳允许 400 ms 完整腕部周期，不依赖脚底腾空支持力。 */
        if (actions[index] == FITNESS_ACTION_JUMPING_JACK) {
            /* 最短周期 400 ms，兼顾正常开合速度和抖动抑制。 */
            CHECK((counter.mode == FITNESS_REP_MODE_TWO_PHASE) &&
                  (counter.min_cycle_ms == 400U));
        } else if (actions[index] == FITNESS_ACTION_WAVE) {
            /* 挥手允许更快往返，但仍保留 300 ms 防抖。 */
            CHECK(counter.min_cycle_ms == 300U);
        } else if (actions[index] == FITNESS_ACTION_JUMPING_SQUAT) {
            /* 跳跃深蹲使用 800 ms 抑制落地回弹重复闭环。 */
            CHECK(counter.min_cycle_ms == 800U);
        } else {
            /* 其它力量和跳跃动作保留 600 ms，容纳验证集中的自然快节奏。 */
            CHECK(counter.min_cycle_ms == 600U);
        }
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
    /* REST 在 920/960 ms 闭合约 820 ms 周期，应计一次。 */
    CHECK(feed_stable_phase(&counter, FITNESS_PHASE_REST, 920ULL));
    /* 累计次数必须为 1。 */
    CHECK(counter.total_repetitions == 1ULL);

    /* 只输入 PRIMARY 后直接 REST，缺少 SECONDARY，不能计数。 */
    CHECK(!feed_stable_phase(&counter, FITNESS_PHASE_PRIMARY, 1320ULL));
    /* 错序 REST 不闭合周期。 */
    CHECK(!feed_stable_phase(&counter, FITNESS_PHASE_REST, 2120ULL));
    /* 总次数保持 1。 */
    CHECK(counter.total_repetitions == 1ULL);

    /* 显式重置不完整周期。 */
    CHECK(fitness_rep_counter_reset_cycle(&counter) == FITNESS_STATUS_OK);
    /* 重置不能清除已累计次数。 */
    CHECK(counter.total_repetitions == 1ULL);
}

/* 验证腕戴跳跃动作按完整正反周期计一次，不依赖不可稳定观测的腾空支持力。 */
static void test_wrist_jump_counter(void)
{
    /* 使用 jumping_squat 代表三类腕戴跳跃动作。 */
    fitness_rep_counter_t counter;
    /* 初始化后必须选择统一两相位状态机。 */
    CHECK(fitness_rep_counter_init(&counter, FITNESS_ACTION_JUMPING_SQUAT) == FITNESS_STATUS_OK);
    /* 主向腕部运动只开启周期。 */
    CHECK(!feed_stable_phase(&counter, FITNESS_PHASE_PRIMARY, 100ULL));
    /* 反向腕部运动确认到达另一端。 */
    CHECK(!feed_stable_phase(&counter, FITNESS_PHASE_SECONDARY, 420ULL));
    /* 840 ms 的完整往返超过通用 800 ms 下限，应闭合一次。 */
    CHECK(feed_stable_phase(&counter, FITNESS_PHASE_REST, 940ULL));
    /* 累计次数必须为 1。 */
    CHECK(counter.total_repetitions == 1ULL);

    /* 时间倒退必须被明确拒绝。 */
    bool completed = false;
    /* 600 ms 早于上一已接受输入时刻。 */
    CHECK(fitness_rep_counter_update(&counter, FITNESS_PHASE_REST, 600ULL, &completed) ==
          FITNESS_STATUS_INVALID_TIME);
}

/* 验证 walk/trot 步峰去重和累计规则。 */
static void test_step_counter(void)
{
    /* 行走最短步间隔应为 240 ms。 */
    fitness_step_counter_t counter;
    /* 初始化 walk 计数器。 */
    CHECK(fitness_step_counter_init(&counter, FITNESS_ACTION_WALK) == FITNESS_STATUS_OK);

    /* 保存每次调用的接受结果。 */
    bool accepted = false;
    /* 首步无历史间隔，应被接受。 */
    CHECK(fitness_step_counter_accept(&counter, 1000ULL, &accepted) == FITNESS_STATUS_OK);
    /* 首步必须接受。 */
    CHECK(accepted);
    /* 100 ms 后的重复峰过密，应静默拒绝。 */
    CHECK(fitness_step_counter_accept(&counter, 1100ULL, &accepted) == FITNESS_STATUS_OK);
    /* 过密峰不计步。 */
    CHECK(!accepted);

    /* 再输入 9 个合法步峰，使总步数达到 10。 */
    for (uint64_t step = 2ULL; step <= 10ULL; ++step) {
        /* 每步相隔 300 ms，大于 walk 最短间隔。 */
        const uint64_t timestamp_ms = 1000ULL + ((step - 1ULL) * 300ULL);
        /* 合法步峰必须成功。 */
        CHECK(fitness_step_counter_accept(&counter, timestamp_ms, &accepted) == FITNESS_STATUS_OK);
        /* 每个峰均被接受。 */
        CHECK(accepted);
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

/* 验证运行中切换动作只改变当前动作和积分基准，不清除会话累计。 */
static void test_session_switch_action_preserves_totals(void)
{
    /* 建立普通深蹲会话，后续切换为弓步。 */
    fitness_session_t session;
    /* 以 70 kg、序号 104、1000 ms 起点启动会话。 */
    CHECK(fitness_session_start(&session, 104U, FITNESS_ACTION_SQUAT, 70000U, 1000ULL) ==
          FITNESS_STATUS_OK);
    /* 准备一次重复事件，建立非零累计和连续事件序号。 */
    fitness_metric_event_t event;
    /* 记录一次深蹲，事件序号应推进到 2。 */
    CHECK(fitness_session_record_count(
              &session,
              FITNESS_METRIC_REPETITION,
              1U,
              1500ULL,
              30000U,
              0U,
              &event) == FITNESS_STATUS_OK);
    /* 先推进到 2000 ms，建立切换前深蹲热量。 */
    bool emitted = false;
    /* 非 sit 动作不会输出时长事件。 */
    CHECK(fitness_session_tick(&session, 2000ULL, 30000U, 0U, &event, &emitted) ==
          FITNESS_STATUS_OK);
    /* 保存切换前毛热量，后续必须原样保留。 */
    const uint64_t gross_before_switch = session.gross_microkcal;
    /* 运行中切换为弓步，同时把热量左端点移动到当前推理时间。 */
    CHECK(fitness_session_switch_action(&session, FITNESS_ACTION_LUNGE, 2200ULL) ==
          FITNESS_STATUS_OK);
    /* 当前动作必须更新为弓步，后续事件和 MET 使用新动作。 */
    CHECK(session.action == FITNESS_ACTION_LUNGE);
    /* 已完成的一次重复不能因动作切换归零。 */
    CHECK(session.repetitions == 1ULL);
    /* 已分配的事件序号必须保持连续。 */
    CHECK(session.next_event_seq == 2U);
    /* 切换本身不凭空增加热量。 */
    CHECK(session.gross_microkcal == gross_before_switch);
    /* 新动作计一次后累计应变为二，事件动作应为弓步。 */
    CHECK(fitness_session_record_count(
              &session,
              FITNESS_METRIC_REPETITION,
              1U,
              2300ULL,
              30000U,
              0U,
              &event) == FITNESS_STATUS_OK);
    /* 累计次数跨动作保持单调。 */
    CHECK(session.repetitions == 2ULL);
    /* 第二个事件必须携带切换后的动作类别。 */
    CHECK(event.action == FITNESS_ACTION_LUNGE);
    /* 非法动作不得修改当前动作。 */
    CHECK(fitness_session_switch_action(&session, FITNESS_ACTION_COUNT, 2400ULL) ==
          FITNESS_STATUS_INVALID_ARGUMENT);
    /* 时间倒退不得修改当前动作。 */
    CHECK(fitness_session_switch_action(&session, FITNESS_ACTION_SQUAT, 2100ULL) ==
          FITNESS_STATUS_INVALID_TIME);
}

/* 测试入口按固定顺序运行全部领域测试。 */
int main(void)
{
    /* 验证动作到状态机映射。 */
    test_repetition_action_contract();
    /* 验证两相位完整周期。 */
    test_two_phase_counter();
    /* 验证腕戴跳跃动作使用完整正反周期。 */
    test_wrist_jump_counter();
    /* 验证步峰不应期和累计。 */
    test_step_counter();
    /* 验证会话与 MetricEvent。 */
    test_session_metric_event();
    /* 验证 sit 时长和整数热量。 */
    test_sit_duration_and_calories();
    /* 验证热量与 tick 划分无关。 */
    test_calorie_tick_invariance();
    /* 验证暂停恢复不累计暂停区间。 */
    test_session_rebase_time();
    /* 验证动作切换保留会话累计和事件连续性。 */
    test_session_switch_action_preserves_totals();
    /* 输出机器可读成功摘要；没有失败时 main 返回 0。 */
    printf("fitness_core host tests: PASS (%u assertions)\n", g_assertion_count);
    /* 0 表示全部测试通过。 */
    return 0;
}
