/* 健身计数、热量、振动领域核心实现；公式见 docs/计数卡路里与振动算法.md。 */
#include "fitness_core.h"

/* UINT64_MAX 用于时间区间加法的饱和保护。 */
#include <stdint.h>
/* memset 用于把领域状态清零为可重复的初始值。 */
#include <string.h>

/* 两相位状态 0：等待 PRIMARY。 */
#define FITNESS_TWO_STATE_WAIT_PRIMARY 0U
/* 两相位状态 1：已看到 PRIMARY，等待 SECONDARY。 */
#define FITNESS_TWO_STATE_WAIT_SECONDARY 1U
/* 两相位状态 2：已看到 SECONDARY，等待 REST 完成周期。 */
#define FITNESS_TWO_STATE_WAIT_RETURN 2U
/* 跳跃状态 0：等待 TAKEOFF。 */
#define FITNESS_JUMP_STATE_WAIT_TAKEOFF 0U
/* 跳跃状态 1：已起跳，等待 FLIGHT。 */
#define FITNESS_JUMP_STATE_WAIT_FLIGHT 1U
/* 跳跃状态 2：已腾空，等待 LANDING。 */
#define FITNESS_JUMP_STATE_WAIT_LANDING 2U
/* 跳跃状态 3：已落地，等待 RECOVERY。 */
#define FITNESS_JUMP_STATE_WAIT_RECOVERY 3U
/* 跳跃状态 4：已恢复，等待 REST 完成周期。 */
#define FITNESS_JUMP_STATE_WAIT_REST 4U
/* 一个 tick 最大允许 60 秒；更长间隔通常意味着暂停/休眠，不能计入运动热量。 */
#define FITNESS_MAX_TICK_INTERVAL_MS 60000ULL
/* 静坐时长按整秒发布事件，减少 BLE/UI 消息频率。 */
#define FITNESS_SIT_EVENT_QUANTUM_MS 1000ULL
/* Q15 稳定度最大合法值。 */
#define FITNESS_STABILITY_Q15_MAX 32767U

/* 判断 action 是否属于模型的 11 个合法动作。 */
static bool fitness_action_is_valid(const fitness_action_t action)
{
    /* 枚举底层为有符号实现时也拒绝负值，并拒绝 ACTION_COUNT 哨兵。 */
    return ((int)action >= 0) && (action < FITNESS_ACTION_COUNT);
}

/* 判断 action 是否属于 8 个“完整周期计一次”的动作。 */
static bool fitness_action_is_repetition(const fitness_action_t action)
{
    /* 用显式集合避免把 sit、walk、trot 意外纳入重复计数。 */
    return (action == FITNESS_ACTION_GOOD_MORNING) ||
           (action == FITNESS_ACTION_JUMPING_JACK) ||
           (action == FITNESS_ACTION_JUMPING_LUNGE) ||
           (action == FITNESS_ACTION_JUMPING_SQUAT) ||
           (action == FITNESS_ACTION_LUNGE) ||
           (action == FITNESS_ACTION_SQUAT) ||
           (action == FITNESS_ACTION_TUCK_JUMP) ||
           (action == FITNESS_ACTION_WAVE);
}

/* 判断 action 是否使用腕部支持力五阶段跳跃状态机。 */
static bool fitness_action_is_jump(const fitness_action_t action)
{
    /* 开合跳按手臂开合两相位计数；其余三个跳跃类要求起跳、腾空、落地和恢复。 */
    return (action == FITNESS_ACTION_JUMPING_LUNGE) ||
           (action == FITNESS_ACTION_JUMPING_SQUAT) ||
           (action == FITNESS_ACTION_TUCK_JUMP);
}

/* 判断 action 是否使用步峰计数。 */
static bool fitness_action_is_step(const fitness_action_t action)
{
    /* v1 仅 walk 和 trot 以步为指标。 */
    return (action == FITNESS_ACTION_WALK) || (action == FITNESS_ACTION_TROT);
}

/* 判断稳定度和质量字段是否满足公共事件合同。 */
static bool fitness_event_fields_are_valid(const uint16_t stability_q15)
{
    /* Q15 只使用非负 15 位范围 0..32767。 */
    return stability_q15 <= FITNESS_STABILITY_Q15_MAX;
}

/* 把当前会话快照填入 MetricEvent，调用方负责先更新 total。 */
static void fitness_fill_event(
    fitness_session_t *session,
    const fitness_metric_kind_t metric_kind,
    const uint32_t delta_value,
    const uint64_t total_value,
    const uint64_t now_ms,
    const uint16_t stability_q15,
    const uint16_t quality_flags,
    fitness_metric_event_t *event)
{
    /* 复制会话序号，供 BLE 去重和持久化关联。 */
    event->session_seq = session->session_seq;
    /* 分配当前事件序号后递增，保证首次为 1。 */
    event->event_seq = session->next_event_seq;
    /* 下一个事件使用连续序号；uint32 回绕由上层按新会话处理。 */
    session->next_event_seq += 1U;
    /* 保存设备单调时间，不依赖 RTC 是否校时。 */
    event->monotonic_ms = now_ms;
    /* 一个会话只跟踪一个动作，因此事件动作直接来自会话。 */
    event->action = session->action;
    /* 写入指标语义，决定 delta/total 的单位。 */
    event->metric_kind = metric_kind;
    /* 写入本事件增量。 */
    event->delta_value = delta_value;
    /* 写入会话当前累计值。 */
    event->total_value = total_value;
    /* 写入 Q15 时间稳定度。 */
    event->stability_q15 = stability_q15;
    /* 写入可组合质量位。 */
    event->quality_flags = quality_flags;
    /* 同步毛热量，UI/BLE 不需要另读可变会话状态。 */
    event->gross_microkcal = session->gross_microkcal;
    /* 同步活动热量，供 PC 端显示或导出。 */
    event->active_microkcal = session->active_microkcal;
}

/* 用整数余数累计一段时间的 microkcal，避免每 40 ms 截断为 0。 */
static void fitness_accumulate_calories(
    const uint32_t met_milli,
    const uint32_t weight_g,
    const uint64_t delta_ms,
    uint64_t *microkcal,
    uint64_t *remainder)
{
    /* 体重未设置或 MET 为 0 时，不产生热量，也不改变余数。 */
    if ((weight_g == 0U) || (met_milli == 0U) || (delta_ms == 0ULL)) {
        /* 无有效输入时直接返回，避免无意义乘法。 */
        return;
    }

    /*
     * 分子 = milliMET * g * ms * 7；在既定上限 20 MET、250 kg、60 s 下
     * 最大约 2.1e15，小于 uint64_t 上限。
     */
    const uint64_t numerator = ((uint64_t)met_milli) *
                               ((uint64_t)weight_g) *
                               delta_ms *
                               7ULL;
    /* 合并上一次除法余数，使任意 tick 划分得到相同累计结果。 */
    const uint64_t combined = numerator + *remainder;
    /* 整数商是本次新增 microkcal。 */
    const uint64_t increment = combined / FITNESS_CALORIE_DENOMINATOR;
    /* 模保留不足 1 microkcal 的分数部分。 */
    *remainder = combined % FITNESS_CALORIE_DENOMINATOR;
    /* 饱和累加避免极端长会话回绕为小值。 */
    if (UINT64_MAX - *microkcal < increment) {
        /* 溢出时固定到最大值，维持单调不减。 */
        *microkcal = UINT64_MAX;
    } else {
        /* 正常范围内加入本 tick 热量。 */
        *microkcal += increment;
    }
}

uint32_t fitness_action_met_milli(const fitness_action_t action)
{
    /* 根据方案固定值返回 milliMET，避免运行时浮点和版本漂移。 */
    switch (action) {
        /* 静坐采用 1.0 MET。 */
        case FITNESS_ACTION_SIT:
            return 1000U;
        /* 挥手采用 1.5 MET。 */
        case FITNESS_ACTION_WAVE:
            return 1500U;
        /* 早安式采用 3.5 MET。 */
        case FITNESS_ACTION_GOOD_MORNING:
            return 3500U;
        /* 普通弓步采用 3.8 MET。 */
        case FITNESS_ACTION_LUNGE:
            return 3800U;
        /* 普通深蹲采用 3.8 MET。 */
        case FITNESS_ACTION_SQUAT:
            return 3800U;
        /* 行走采用 3.8 MET。 */
        case FITNESS_ACTION_WALK:
            return 3800U;
        /* 小跑采用 4.8 MET。 */
        case FITNESS_ACTION_TROT:
            return 4800U;
        /* 四种跳跃动作统一采用 7.5 MET。 */
        case FITNESS_ACTION_JUMPING_JACK:
        case FITNESS_ACTION_JUMPING_LUNGE:
        case FITNESS_ACTION_JUMPING_SQUAT:
        case FITNESS_ACTION_TUCK_JUMP:
            return 7500U;
        /* 非法动作不估算热量，返回 0 让调用方安全降级。 */
        default:
            return 0U;
    }
}

fitness_status_t fitness_session_start(
    fitness_session_t *session,
    const uint32_t session_seq,
    const fitness_action_t action,
    const uint32_t weight_g,
    const uint64_t start_ms)
{
    /* 会话输出指针和动作必须有效。 */
    if ((session == NULL) || !fitness_action_is_valid(action)) {
        /* 空指针或越界动作不能建立会话。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }
    /* 体重 0 表示未设置；非零值必须位于 30..250 kg。 */
    if ((weight_g != 0U) && ((weight_g < 30000U) || (weight_g > 250000U))) {
        /* 拒绝明显不合理体重，避免热量误导和乘法上限失效。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }

    /* 清除旧会话全部计数、热量和除法余数。 */
    memset(session, 0, sizeof(*session));
    /* 标记会话已激活。 */
    session->active = true;
    /* 保存单动作会话类别。 */
    session->action = action;
    /* 保存持久化层提供的会话序号。 */
    session->session_seq = session_seq;
    /* 首个 MetricEvent 序号固定为 1。 */
    session->next_event_seq = 1U;
    /* 保存热量估算使用的体重，单位 g。 */
    session->weight_g = weight_g;
    /* 起始时间同时作为第一次 tick 的左端点。 */
    session->last_tick_ms = start_ms;
    /* 会话建立完成。 */
    return FITNESS_STATUS_OK;
}

fitness_status_t fitness_session_stop(fitness_session_t *session)
{
    /* 停止操作必须指向已存在会话。 */
    if (session == NULL) {
        /* 空指针无法修改状态。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }
    /* 重复停止表示调用顺序错误。 */
    if (!session->active) {
        /* 保持原状态并报告错误。 */
        return FITNESS_STATUS_INVALID_STATE;
    }

    /* 只改变 active；汇总字段继续保留给 UI/存储读取。 */
    session->active = false;
    /* 停止成功。 */
    return FITNESS_STATUS_OK;
}

fitness_status_t fitness_session_tick(
    fitness_session_t *session,
    const uint64_t now_ms,
    const uint16_t stability_q15,
    const uint16_t quality_flags,
    fitness_metric_event_t *optional_sit_event,
    bool *event_emitted)
{
    /* session、事件缓冲和发出标记均由调用方提供。 */
    if ((session == NULL) || (optional_sit_event == NULL) || (event_emitted == NULL) ||
        !fitness_event_fields_are_valid(stability_q15)) {
        /* 缺少输出或 Q15 越界时不推进时间。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }
    /* 未启动或已停止会话不能累计时间。 */
    if (!session->active) {
        /* 返回状态错误，避免后台 tick 修改已完成汇总。 */
        return FITNESS_STATUS_INVALID_STATE;
    }
    /* 单调时间不能倒退。 */
    if (now_ms < session->last_tick_ms) {
        /* 时间倒退通常来自计时源错误或跨重启状态复用。 */
        return FITNESS_STATUS_INVALID_TIME;
    }

    /* 计算本 tick 时长，单位 ms。 */
    const uint64_t delta_ms = now_ms - session->last_tick_ms;
    /* 超过 60 秒视为暂停/休眠断层，必须由上层重启或显式恢复会话。 */
    if (delta_ms > FITNESS_MAX_TICK_INTERVAL_MS) {
        /* 不改变 last_tick，防止静默丢失断层诊断。 */
        return FITNESS_STATUS_INVALID_TIME;
    }

    /* 默认本 tick 不产生 sit 时长事件。 */
    *event_emitted = false;
    /* 合法 tick 更新左端点；delta=0 也允许以支持重复状态读取。 */
    session->last_tick_ms = now_ms;

    /* 查出当前动作固定 milliMET。 */
    const uint32_t gross_met_milli = fitness_action_met_milli(session->action);
    /* 活动 MET 扣除 1.0 MET 静息部分，并对低于 1 MET 的异常值截到 0。 */
    const uint32_t active_met_milli = (gross_met_milli > 1000U) ?
                                      (gross_met_milli - 1000U) :
                                      0U;
    /* 使用整数余数累计毛热量。 */
    fitness_accumulate_calories(
        gross_met_milli,
        session->weight_g,
        delta_ms,
        &session->gross_microkcal,
        &session->gross_calorie_remainder);
    /* 使用相同公式累计扣除静息部分后的活动热量。 */
    fitness_accumulate_calories(
        active_met_milli,
        session->weight_g,
        delta_ms,
        &session->active_microkcal,
        &session->active_calorie_remainder);

    /* 非 sit 会话只更新热量，不发布时长事件。 */
    if (session->action != FITNESS_ACTION_SIT) {
        /* 正常完成无事件 tick。 */
        return FITNESS_STATUS_OK;
    }

    /* sit 会话把全部合法 tick 计入持续时间。 */
    if (UINT64_MAX - session->sit_duration_ms < delta_ms) {
        /* 极端溢出时饱和，保持持续时间单调。 */
        session->sit_duration_ms = UINT64_MAX;
    } else {
        /* 正常累计静坐毫秒。 */
        session->sit_duration_ms += delta_ms;
    }

    /* 计算尚未通过 MetricEvent 发布的静坐毫秒数。 */
    const uint64_t unpublished_ms = session->sit_duration_ms - session->published_sit_duration_ms;
    /* 不满 1 秒时保留到后续 tick，减少消息数量。 */
    if (unpublished_ms < FITNESS_SIT_EVENT_QUANTUM_MS) {
        /* 时长已累计但尚无整秒事件。 */
        return FITNESS_STATUS_OK;
    }

    /* 只发布完整秒，余下不足 1 秒继续保留。 */
    const uint64_t publish_ms = (unpublished_ms / FITNESS_SIT_EVENT_QUANTUM_MS) *
                                FITNESS_SIT_EVENT_QUANTUM_MS;
    /* 单次 tick 最大 60 秒，因此 delta 能安全放入 uint32_t。 */
    const uint32_t event_delta_ms = (uint32_t)publish_ms;
    /* 推进已发布水位，防止同一毫秒重复通知。 */
    session->published_sit_duration_ms += publish_ms;
    /* 生成 sit 时长事件，total 保留真实累计值而非仅整秒水位。 */
    fitness_fill_event(
        session,
        FITNESS_METRIC_DURATION_MS,
        event_delta_ms,
        session->sit_duration_ms,
        now_ms,
        stability_q15,
        quality_flags,
        optional_sit_event);
    /* 告知调用方事件缓冲已填充。 */
    *event_emitted = true;
    /* tick 和事件生成均成功。 */
    return FITNESS_STATUS_OK;
}

fitness_status_t fitness_session_rebase_time(
    fitness_session_t *session,
    const uint64_t now_ms)
{
    /* 会话对象必须有效且仍处于 active，已停止会话不能恢复。 */
    if (session == NULL) {
        /* 空指针没有可更新时间基准。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }
    /* 只有已启动且未停止的会话允许重设时间基准。 */
    if (!session->active) {
        /* 返回状态错误，避免把未开始对象伪装成已恢复。 */
        return FITNESS_STATUS_INVALID_STATE;
    }
    /* 单调时钟不得倒退，否则后续热量积分会产生无符号下溢。 */
    if (now_ms < session->last_tick_ms) {
        /* 保留原时间基准并报告错误。 */
        return FITNESS_STATUS_INVALID_TIME;
    }
    /* 只移动积分起点；不修改热量、次数、步数或静坐累计。 */
    session->last_tick_ms = now_ms;
    /* 返回成功。 */
    return FITNESS_STATUS_OK;
}

fitness_status_t fitness_session_switch_action(
    fitness_session_t *session,
    const fitness_action_t action,
    const uint64_t now_ms)
{
    /* 会话指针和新动作必须有效，非法输入不得改写任何领域累计。 */
    if ((session == NULL) || !fitness_action_is_valid(action)) {
        /* 返回参数错误，调用方继续保持旧动作。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }
    /* 已停止或尚未开始的会话不能在原地切换动作。 */
    if (!session->active) {
        /* 返回状态错误，避免复活已完成会话。 */
        return FITNESS_STATUS_INVALID_STATE;
    }
    /* 切换时刻不能早于最近一次 25 Hz tick，否则热量时间轴会倒退。 */
    if (now_ms < session->last_tick_ms) {
        /* 保留旧动作和旧积分基准。 */
        return FITNESS_STATUS_INVALID_TIME;
    }
    /* 先移动热量积分左端点，动作切换本身不虚构一段未知动作热量。 */
    session->last_tick_ms = now_ms;
    /* 最后提交新动作；次数、步数、时长、热量、余数和事件序号全部保持原值。 */
    session->action = action;
    /* 切换成功。 */
    return FITNESS_STATUS_OK;
}

fitness_status_t fitness_session_record_count(
    fitness_session_t *session,
    const fitness_metric_kind_t metric_kind,
    const uint32_t delta_value,
    const uint64_t now_ms,
    const uint16_t stability_q15,
    const uint16_t quality_flags,
    fitness_metric_event_t *event)
{
    /* 指针、正增量和 Q15 范围是生成事件的最低条件。 */
    if ((session == NULL) || (event == NULL) || (delta_value == 0U) ||
        !fitness_event_fields_are_valid(stability_q15)) {
        /* 不完整输入不能修改会话计数。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }
    /* 只有活跃会话允许写指标。 */
    if (!session->active) {
        /* 已停止会话保持不可变。 */
        return FITNESS_STATUS_INVALID_STATE;
    }
    /* 事件时间不得早于最近一次会话 tick。 */
    if (now_ms < session->last_tick_ms) {
        /* 拒绝乱序事件，避免 BLE/存储时间线倒退。 */
        return FITNESS_STATUS_INVALID_TIME;
    }

    /* REPETITION 只适用于 8 个周期动作。 */
    if ((metric_kind == FITNESS_METRIC_REPETITION) && fitness_action_is_repetition(session->action)) {
        /* 用饱和加法维护总次数。 */
        if (UINT64_MAX - session->repetitions < delta_value) {
            /* 极端溢出固定到最大值。 */
            session->repetitions = UINT64_MAX;
        } else {
            /* 正常加入本次重复增量。 */
            session->repetitions += delta_value;
        }
        /* 生成唯一重复事件，供振动/UI/BLE共同消费。 */
        fitness_fill_event(
            session,
            metric_kind,
            delta_value,
            session->repetitions,
            now_ms,
            stability_q15,
            quality_flags,
            event);
        /* 重复指标写入成功。 */
        return FITNESS_STATUS_OK;
    }

    /* STEP 只适用于 walk/trot。 */
    if ((metric_kind == FITNESS_METRIC_STEP) && fitness_action_is_step(session->action)) {
        /* 用饱和加法维护总步数。 */
        if (UINT64_MAX - session->steps < delta_value) {
            /* 极端溢出固定到最大值。 */
            session->steps = UINT64_MAX;
        } else {
            /* 正常加入本次步数增量。 */
            session->steps += delta_value;
        }
        /* 生成唯一步数事件。 */
        fitness_fill_event(
            session,
            metric_kind,
            delta_value,
            session->steps,
            now_ms,
            stability_q15,
            quality_flags,
            event);
        /* 步数指标写入成功。 */
        return FITNESS_STATUS_OK;
    }

    /* 指标类型和会话动作不匹配；持续时长只能通过 tick 产生。 */
    return FITNESS_STATUS_INVALID_STATE;
}

fitness_status_t fitness_rep_counter_init(
    fitness_rep_counter_t *counter,
    const fitness_action_t action)
{
    /* 计数器指针必须有效，动作必须属于 8 个周期动作。 */
    if ((counter == NULL) || !fitness_action_is_repetition(action)) {
        /* sit/walk/trot 不使用此状态机。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }

    /* 清除旧周期、候选相位和累计次数。 */
    memset(counter, 0, sizeof(*counter));
    /* 标记初始化成功。 */
    counter->initialized = true;
    /* 保存动作类别。 */
    counter->action = action;
    /* 三个高动态跳跃类使用五阶段；开合跳及其余四类使用腕部两相位模式。 */
    counter->mode = fitness_action_is_jump(action) ?
                    FITNESS_REP_MODE_JUMP :
                    FITNESS_REP_MODE_TWO_PHASE;
    /* 初始内部状态均为 0，分别表示等待 PRIMARY 或 TAKEOFF。 */
    counter->state = 0U;
    /* 候选相位设为哨兵，保证首次相位从计数 1 开始。 */
    counter->candidate_phase = FITNESS_PHASE_COUNT;
    /* 所有动作最长周期限制 10 秒，防止很久以前的不完整相位被拼接。 */
    counter->max_cycle_ms = 10000U;
    /* 通用不应期 300 ms，低于人体完成两次完整动作的合理间隔。 */
    counter->refractory_ms = 300U;

    /* 根据动作动力学设置最短完整周期。 */
    if (counter->mode == FITNESS_REP_MODE_JUMP) {
        /* 跳跃起飞到恢复至少 400 ms。 */
        counter->min_cycle_ms = 400U;
    } else if (action == FITNESS_ACTION_JUMPING_JACK) {
        /* 开合跳手臂张开到合拢至少 400 ms，兼顾快速动作和抖动抑制。 */
        counter->min_cycle_ms = 400U;
    } else if (action == FITNESS_ACTION_WAVE) {
        /* 挥手周期允许更快，但仍至少 300 ms。 */
        counter->min_cycle_ms = 300U;
    } else {
        /* 早安式、弓步、深蹲至少 600 ms，抑制半程抖动。 */
        counter->min_cycle_ms = 600U;
    }

    /* 初始化成功。 */
    return FITNESS_STATUS_OK;
}

fitness_status_t fitness_rep_counter_reset_cycle(fitness_rep_counter_t *counter)
{
    /* 只有初始化过的重复计数器可重置。 */
    if (counter == NULL) {
        /* 空指针无状态可改。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }
    if (!counter->initialized) {
        /* 未初始化状态不含有效动作配置。 */
        return FITNESS_STATUS_INVALID_STATE;
    }

    /* 回到等待首相位，不清除 total_repetitions。 */
    counter->state = 0U;
    /* 清除连续相位候选，要求重置后重新稳定两点。 */
    counter->candidate_phase = FITNESS_PHASE_COUNT;
    /* 候选连续点数归零。 */
    counter->candidate_samples = 0U;
    /* 当前周期起点归零，表示没有未完成周期。 */
    counter->cycle_start_ms = 0ULL;
    /* 重置成功。 */
    return FITNESS_STATUS_OK;
}

/* 处理已连续出现至少两点的稳定相位，并在完成周期时返回 true。 */
static bool fitness_rep_process_stable_phase(
    fitness_rep_counter_t *counter,
    const fitness_motion_phase_t phase,
    const uint64_t now_ms)
{
    /* 两相位状态机按 REST->PRIMARY->SECONDARY->REST 顺序推进。 */
    if (counter->mode == FITNESS_REP_MODE_TWO_PHASE) {
        /* 等待第一方向时，稳定 PRIMARY 开启新周期。 */
        if ((counter->state == FITNESS_TWO_STATE_WAIT_PRIMARY) &&
            (phase == FITNESS_PHASE_PRIMARY)) {
            /* 记录周期起点用于时长检查。 */
            counter->cycle_start_ms = now_ms;
            /* 下一状态等待 SECONDARY。 */
            counter->state = FITNESS_TWO_STATE_WAIT_SECONDARY;
            /* 尚未完成一次。 */
            return false;
        }
        /* 已看到 PRIMARY 后，稳定 SECONDARY 确认到达另一端。 */
        if ((counter->state == FITNESS_TWO_STATE_WAIT_SECONDARY) &&
            (phase == FITNESS_PHASE_SECONDARY)) {
            /* 下一状态等待回到 REST。 */
            counter->state = FITNESS_TWO_STATE_WAIT_RETURN;
            /* 尚未完成一次。 */
            return false;
        }
        /* 已到另一端后，稳定 REST 尝试闭合周期。 */
        if ((counter->state == FITNESS_TWO_STATE_WAIT_RETURN) &&
            (phase == FITNESS_PHASE_REST)) {
            /* 计算完整周期时长，单位 ms。 */
            const uint64_t cycle_ms = now_ms - counter->cycle_start_ms;
            /* 检查最短/最长周期和上一次计数后的不应期。 */
            const bool duration_valid = (cycle_ms >= counter->min_cycle_ms) &&
                                        (cycle_ms <= counter->max_cycle_ms);
            /* 首次计数不需要 last_rep；后续计数必须跨过不应期。 */
            const bool refractory_valid = (counter->total_repetitions == 0ULL) ||
                                          ((now_ms - counter->last_rep_ms) >= counter->refractory_ms);
            /* 开合跳的返回正峰同时是下一周期主峰，直接等待负峰；其它动作重新等待 PRIMARY。 */
            counter->state = (counter->action == FITNESS_ACTION_JUMPING_JACK) ?
                             FITNESS_TWO_STATE_WAIT_SECONDARY :
                             FITNESS_TWO_STATE_WAIT_PRIMARY;
            /* 开合跳从当前返回正峰开始下一周期；其它动作清除周期起点。 */
            counter->cycle_start_ms = (counter->action == FITNESS_ACTION_JUMPING_JACK) ? now_ms : 0ULL;
            /* 任一约束失败时丢弃该周期。 */
            if (!duration_valid || !refractory_valid) {
                /* 周期不计数。 */
                return false;
            }
            /* 累计一次有效重复。 */
            counter->total_repetitions += 1ULL;
            /* 保存去重时间。 */
            counter->last_rep_ms = now_ms;
            /* 通知调用方生成 MetricEvent。 */
            return true;
        }
        /* 其它相位不会推进或倒退状态，等待后续正确序列或超时重置。 */
        return false;
    }

    /* 跳跃状态机依次确认 TAKEOFF、FLIGHT、LANDING、RECOVERY、REST。 */
    if ((counter->state == FITNESS_JUMP_STATE_WAIT_TAKEOFF) &&
        (phase == FITNESS_PHASE_TAKEOFF)) {
        /* 起跳作为周期起点。 */
        counter->cycle_start_ms = now_ms;
        /* 下一状态等待腾空。 */
        counter->state = FITNESS_JUMP_STATE_WAIT_FLIGHT;
        /* 尚未完成一次。 */
        return false;
    }
    if ((counter->state == FITNESS_JUMP_STATE_WAIT_FLIGHT) &&
        (phase == FITNESS_PHASE_FLIGHT)) {
        /* 确认腾空后等待落地冲击。 */
        counter->state = FITNESS_JUMP_STATE_WAIT_LANDING;
        /* 尚未完成一次。 */
        return false;
    }
    if ((counter->state == FITNESS_JUMP_STATE_WAIT_LANDING) &&
        (phase == FITNESS_PHASE_LANDING)) {
        /* 确认落地后等待冲击恢复。 */
        counter->state = FITNESS_JUMP_STATE_WAIT_RECOVERY;
        /* 尚未完成一次。 */
        return false;
    }
    if ((counter->state == FITNESS_JUMP_STATE_WAIT_RECOVERY) &&
        (phase == FITNESS_PHASE_RECOVERY)) {
        /* 恢复阶段后等待回到稳定基线。 */
        counter->state = FITNESS_JUMP_STATE_WAIT_REST;
        /* 尚未完成一次。 */
        return false;
    }
    if ((counter->state == FITNESS_JUMP_STATE_WAIT_REST) &&
        (phase == FITNESS_PHASE_REST)) {
        /* 计算起跳到稳定恢复的完整周期。 */
        const uint64_t cycle_ms = now_ms - counter->cycle_start_ms;
        /* 验证生理合理周期区间。 */
        const bool duration_valid = (cycle_ms >= counter->min_cycle_ms) &&
                                    (cycle_ms <= counter->max_cycle_ms);
        /* 验证两次计数之间的不应期。 */
        const bool refractory_valid = (counter->total_repetitions == 0ULL) ||
                                      ((now_ms - counter->last_rep_ms) >= counter->refractory_ms);
        /* 周期结束后立即回到等待起跳，防止 REST 连续点重复计数。 */
        counter->state = FITNESS_JUMP_STATE_WAIT_TAKEOFF;
        /* 清除当前周期起点。 */
        counter->cycle_start_ms = 0ULL;
        /* 不合法周期被丢弃。 */
        if (!duration_valid || !refractory_valid) {
            /* 不产生重复事件。 */
            return false;
        }
        /* 累计有效跳跃次数。 */
        counter->total_repetitions += 1ULL;
        /* 保存最近计数时刻。 */
        counter->last_rep_ms = now_ms;
        /* 通知调用方一次完整跳跃已完成。 */
        return true;
    }

    /* 顺序不匹配时保持当前状态。 */
    return false;
}

fitness_status_t fitness_rep_counter_update(
    fitness_rep_counter_t *counter,
    const fitness_motion_phase_t phase,
    const uint64_t now_ms,
    bool *rep_completed)
{
    /* 输出指针、计数器和相位枚举必须有效。 */
    if ((counter == NULL) || (rep_completed == NULL) ||
        ((int)phase < 0) || (phase >= FITNESS_PHASE_COUNT)) {
        /* 不修改任何计数状态。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }
    /* 未初始化计数器没有动作模式和时长参数。 */
    if (!counter->initialized) {
        /* 要求先调用 init。 */
        return FITNESS_STATUS_INVALID_STATE;
    }
    /* 输入时间必须单调不减。 */
    if ((counter->last_update_ms != 0ULL) && (now_ms < counter->last_update_ms)) {
        /* 乱序样本不能拼入状态机。 */
        return FITNESS_STATUS_INVALID_TIME;
    }

    /* 默认本采样不完成重复。 */
    *rep_completed = false;
    /* 保存最新合法输入时刻。 */
    counter->last_update_ms = now_ms;

    /* 未完成周期超过最长时长时自动丢弃，避免跨休息段拼接。 */
    if ((counter->state != 0U) &&
        (counter->cycle_start_ms != 0ULL) &&
        ((now_ms - counter->cycle_start_ms) > counter->max_cycle_ms)) {
        /* 回到等待首相位，累计次数保持不变。 */
        (void)fitness_rep_counter_reset_cycle(counter);
    }

    /* 相位变化时开始新的稳定候选计数。 */
    if (counter->candidate_phase != phase) {
        /* 保存新候选相位。 */
        counter->candidate_phase = phase;
        /* 当前仅连续出现 1 点，尚不能触发转移。 */
        counter->candidate_samples = 1U;
        /* 正常接受采样但不计数。 */
        return FITNESS_STATUS_OK;
    }

    /* 相同候选连续出现时计数，255 后饱和避免 uint8 回绕。 */
    if (counter->candidate_samples < UINT8_MAX) {
        /* 累计连续稳定采样点数。 */
        counter->candidate_samples += 1U;
    }
    /* 少于 2 个连续点时不允许状态转移。 */
    if (counter->candidate_samples < 2U) {
        /* 当前相位证据不足。 */
        return FITNESS_STATUS_OK;
    }

    /* 交给对应有限状态机处理稳定相位。 */
    *rep_completed = fitness_rep_process_stable_phase(counter, phase, now_ms);
    /* 更新成功。 */
    return FITNESS_STATUS_OK;
}

fitness_status_t fitness_step_counter_init(
    fitness_step_counter_t *counter,
    const fitness_action_t action)
{
    /* 步计数器只接受 walk/trot。 */
    if ((counter == NULL) || !fitness_action_is_step(action)) {
        /* 其它动作必须使用重复状态机或 sit tick。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }

    /* 清除旧步数和时间。 */
    memset(counter, 0, sizeof(*counter));
    /* 标记初始化完成。 */
    counter->initialized = true;
    /* 保存步态类别。 */
    counter->action = action;
    /* 行走最短步间隔 240 ms，小跑允许更快的 150 ms。 */
    counter->min_step_interval_ms = (action == FITNESS_ACTION_WALK) ? 240U : 150U;
    /* 初始化成功。 */
    return FITNESS_STATUS_OK;
}

fitness_status_t fitness_step_counter_accept(
    fitness_step_counter_t *counter,
    const uint64_t now_ms,
    bool *step_accepted,
    bool *haptic_due)
{
    /* 计数器和两个结果指针必须有效。 */
    if ((counter == NULL) || (step_accepted == NULL) || (haptic_due == NULL)) {
        /* 缺少结果缓冲时不修改计数。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }
    /* 未初始化状态不能接收步峰。 */
    if (!counter->initialized) {
        /* 要求先指定 walk 或 trot。 */
        return FITNESS_STATUS_INVALID_STATE;
    }

    /* 默认拒绝当前步峰且不触发反馈。 */
    *step_accepted = false;
    /* 默认不振动。 */
    *haptic_due = false;
    /* 已有步峰时检查时间不能倒退。 */
    if ((counter->total_steps != 0ULL) && (now_ms < counter->last_step_ms)) {
        /* 乱序步峰返回明确错误。 */
        return FITNESS_STATUS_INVALID_TIME;
    }
    /* 非首次步峰必须满足最短间隔。 */
    if ((counter->total_steps != 0ULL) &&
        ((now_ms - counter->last_step_ms) < counter->min_step_interval_ms)) {
        /* 过密峰被视为同一步冲击，不报系统错误。 */
        return FITNESS_STATUS_OK;
    }

    /* 接受新步并累计。 */
    counter->total_steps += 1ULL;
    /* 保存去重时间。 */
    counter->last_step_ms = now_ms;
    /* 告知调用方应生成 STEP MetricEvent。 */
    *step_accepted = true;
    /* 每满 10 步触发一次短振动，避免每一步持续扰动 IMU。 */
    *haptic_due = ((counter->total_steps % 10ULL) == 0ULL);
    /* 步峰处理成功。 */
    return FITNESS_STATUS_OK;
}

void fitness_haptic_queue_init(fitness_haptic_queue_t *queue)
{
    /* void 接口遇到空指针只能安全忽略；调用方应在静态初始化时传有效地址。 */
    if (queue == NULL) {
        /* 无状态可清除。 */
        return;
    }
    /* 清除全部槽位和环形索引。 */
    memset(queue, 0, sizeof(*queue));
}

/*
 * 根据业务原因构造导通时长、关闭间隔和重复次数；本领域层不直接驱动 GPIO。
 * 板级运行时实际使用 GPIO18、5 kHz、10 位 LEDC，正式有效计数以 75% 占空比异步输出一次 30 ms 脉冲。
 * 一次性 esp_timer 负责硬件关断，haptic FreeRTOS 任务只阻塞等待队列/脉冲间隔，不执行占用 CPU 的忙等待。
 */
static bool fitness_haptic_pattern_for_reason(
    const fitness_haptic_reason_t reason,
    fitness_haptic_request_t *request)
{
    /* 输出指针为空或原因越界时无法生成波形。 */
    if ((request == NULL) || ((int)reason < 0) || (reason >= FITNESS_HAPTIC_REASON_COUNT)) {
        /* 返回 false 表示没有合法请求。 */
        return false;
    }

    /* 先写入业务原因，后续分支补充脉冲参数。 */
    request->reason = reason;
    /* 各原因使用方案固定波形。 */
    switch (reason) {
        /* 会话开始使用双短脉冲。 */
        case FITNESS_HAPTIC_REASON_START:
            request->on_ms = 20U;
            request->off_ms = 40U;
            request->repeat_count = 2U;
            break;
        /* 每次重复使用单个 30 ms 脉冲。 */
        case FITNESS_HAPTIC_REASON_REPETITION:
            request->on_ms = 30U;
            request->off_ms = 0U;
            request->repeat_count = 1U;
            break;
        /* 每 10 步使用单个 30 ms 脉冲。 */
        case FITNESS_HAPTIC_REASON_STEP_BATCH:
            request->on_ms = 30U;
            request->off_ms = 0U;
            request->repeat_count = 1U;
            break;
        /* 暂停或结束使用单个 40 ms 脉冲。 */
        case FITNESS_HAPTIC_REASON_PAUSE_OR_END:
            request->on_ms = 40U;
            request->off_ms = 0U;
            request->repeat_count = 1U;
            break;
        /* 目标完成使用三个 25 ms 脉冲。 */
        case FITNESS_HAPTIC_REASON_GOAL:
            request->on_ms = 25U;
            request->off_ms = 35U;
            request->repeat_count = 3U;
            break;
        /* 低电量使用两个 40 ms 脉冲。 */
        case FITNESS_HAPTIC_REASON_LOW_BATTERY:
            request->on_ms = 40U;
            request->off_ms = 60U;
            request->repeat_count = 2U;
            break;
        /* 枚举已在入口验证，default 仍作为编译器防御。 */
        default:
            return false;
    }

    /* 波形生成成功。 */
    return true;
}

fitness_status_t fitness_haptic_enqueue_reason(
    fitness_haptic_queue_t *queue,
    const fitness_haptic_reason_t reason)
{
    /* 队列必须有效。 */
    if (queue == NULL) {
        /* 空队列指针无法写入。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }
    /* 固定容量已满时保持 FIFO 内容不变。 */
    if (queue->count >= FITNESS_HAPTIC_QUEUE_CAPACITY) {
        /* 调用方可记录丢弃或稍后重试。 */
        return FITNESS_STATUS_QUEUE_FULL;
    }

    /* 在栈上构造一个小型请求，避免先写槽位后发现原因非法。 */
    fitness_haptic_request_t request;
    /* 非法原因不进入队列。 */
    if (!fitness_haptic_pattern_for_reason(reason, &request)) {
        /* 返回参数错误。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }

    /* 把完整请求写入 tail 指向的空槽。 */
    queue->items[queue->tail] = request;
    /* tail 环形前进一格。 */
    queue->tail = (uint8_t)((queue->tail + 1U) % FITNESS_HAPTIC_QUEUE_CAPACITY);
    /* 增加有效元素数量。 */
    queue->count += 1U;
    /* 入队成功。 */
    return FITNESS_STATUS_OK;
}

fitness_status_t fitness_haptic_enqueue_for_metric(
    fitness_haptic_queue_t *queue,
    const fitness_metric_event_t *event,
    bool *request_enqueued)
{
    /* 队列、事件和结果标记必须有效。 */
    if ((queue == NULL) || (event == NULL) || (request_enqueued == NULL)) {
        /* 缺少任一对象均无法确定反馈。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }

    /* 默认当前指标不需要振动。 */
    *request_enqueued = false;
    /* 每次有效重复都生成一个 30 ms 请求。 */
    if (event->metric_kind == FITNESS_METRIC_REPETITION) {
        /* 只有正增量事件才允许反馈。 */
        if (event->delta_value == 0U) {
            /* 零增量事件违反 MetricEvent 合同。 */
            return FITNESS_STATUS_INVALID_ARGUMENT;
        }
        /* 入队固定重复波形。 */
        const fitness_status_t status = fitness_haptic_enqueue_reason(
            queue,
            FITNESS_HAPTIC_REASON_REPETITION);
        /* 入队成功才置 true。 */
        *request_enqueued = (status == FITNESS_STATUS_OK);
        /* 透传成功或队列满错误。 */
        return status;
    }

    /* walk/trot 仅在总步数恰好跨到 10 的倍数时反馈。 */
    if (event->metric_kind == FITNESS_METRIC_STEP) {
        /* total=0 或非 10 的倍数无需振动。 */
        if ((event->total_value == 0ULL) || ((event->total_value % 10ULL) != 0ULL)) {
            /* 正常无反馈。 */
            return FITNESS_STATUS_OK;
        }
        /* 入队固定每十步波形。 */
        const fitness_status_t status = fitness_haptic_enqueue_reason(
            queue,
            FITNESS_HAPTIC_REASON_STEP_BATCH);
        /* 入队成功才置 true。 */
        *request_enqueued = (status == FITNESS_STATUS_OK);
        /* 透传队列结果。 */
        return status;
    }

    /* sit 时长事件不产生周期振动，避免持续干扰佩戴。 */
    if (event->metric_kind == FITNESS_METRIC_DURATION_MS) {
        /* 正常无反馈。 */
        return FITNESS_STATUS_OK;
    }

    /* 未知指标类型属于参数错误。 */
    return FITNESS_STATUS_INVALID_ARGUMENT;
}

bool fitness_haptic_queue_pop(
    fitness_haptic_queue_t *queue,
    fitness_haptic_request_t *request)
{
    /* 队列或输出为空时不能取出。 */
    if ((queue == NULL) || (request == NULL)) {
        /* 返回 false 表示没有有效输出。 */
        return false;
    }
    /* 空队列保持索引不变。 */
    if (queue->count == 0U) {
        /* 没有请求。 */
        return false;
    }

    /* 复制最早请求给消费者。 */
    *request = queue->items[queue->head];
    /* head 环形前进一格。 */
    queue->head = (uint8_t)((queue->head + 1U) % FITNESS_HAPTIC_QUEUE_CAPACITY);
    /* 减少有效元素数量。 */
    queue->count -= 1U;
    /* 成功取出一个请求。 */
    return true;
}

void fitness_haptic_guard_init(fitness_haptic_guard_t *guard)
{
    /* void 初始化接口对空指针安全忽略。 */
    if (guard == NULL) {
        /* 无状态可清除。 */
        return;
    }
    /* 清除端点、待插值时间、冻结位和污染区间。 */
    memset(guard, 0, sizeof(*guard));
}

fitness_status_t fitness_haptic_guard_mark_pulse(
    fitness_haptic_guard_t *guard,
    const uint64_t pulse_start_ms,
    const uint32_t pulse_on_ms)
{
    /* guard 必须有效，通电脉冲必须至少 1 ms。 */
    if ((guard == NULL) || (pulse_on_ms == 0U)) {
        /* 空对象或零脉冲不建立污染区。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }

    /* 需要增加的保护长度包含通电和固定 80 ms 余振。 */
    const uint64_t guard_length_ms = ((uint64_t)pulse_on_ms) + FITNESS_HAPTIC_TAIL_GUARD_MS;
    /* 时间加法接近 uint64 上限时使用饱和终点。 */
    const uint64_t new_until_ms = (UINT64_MAX - pulse_start_ms < guard_length_ms) ?
                                  UINT64_MAX :
                                  (pulse_start_ms + guard_length_ms);

    /* 新脉冲与现有区间不相交时，用新脉冲替换已结束区间。 */
    if ((guard->contaminated_until_ms == 0ULL) ||
        (pulse_start_ms > guard->contaminated_until_ms)) {
        /* 保存新污染起点。 */
        guard->contaminated_from_ms = pulse_start_ms;
        /* 保存新污染终点。 */
        guard->contaminated_until_ms = new_until_ms;
        /* 登记成功。 */
        return FITNESS_STATUS_OK;
    }

    /* 重叠脉冲可能更早开始，区间起点取较小值。 */
    if (pulse_start_ms < guard->contaminated_from_ms) {
        /* 向前扩展污染起点。 */
        guard->contaminated_from_ms = pulse_start_ms;
    }
    /* 重叠脉冲可能延长尾部，终点取较大值。 */
    if (new_until_ms > guard->contaminated_until_ms) {
        /* 向后扩展污染终点。 */
        guard->contaminated_until_ms = new_until_ms;
    }
    /* 合并完成。 */
    return FITNESS_STATUS_OK;
}

bool fitness_haptic_guard_is_contaminated(
    const fitness_haptic_guard_t *guard,
    const uint64_t sample_ms)
{
    /* 空 guard 表示没有已知振动，不把采样误判为污染。 */
    if (guard == NULL) {
        /* 安全返回未污染。 */
        return false;
    }
    /* 只有闭区间 [from, until] 内的时间才标记污染。 */
    return (guard->contaminated_until_ms != 0ULL) &&
           (sample_ms >= guard->contaminated_from_ms) &&
           (sample_ms <= guard->contaminated_until_ms);
}

fitness_status_t fitness_haptic_guard_push_sample(
    fitness_haptic_guard_t *guard,
    const fitness_imu_sample_t *input_sample,
    fitness_imu_sample_t output_samples[FITNESS_HAPTIC_MAX_OUTPUT_SAMPLES],
    size_t *output_count,
    bool *counter_frozen)
{
    /* 所有输入输出缓冲都必须有效。 */
    if ((guard == NULL) || (input_sample == NULL) || (output_samples == NULL) ||
        (output_count == NULL) || (counter_frozen == NULL)) {
        /* 缺少缓冲时不改变 guard。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }

    /* 默认本次尚无可交付采样。 */
    *output_count = 0U;
    /* 先报告进入本次调用前的硬冻结状态。 */
    *counter_frozen = guard->hard_freeze;

    /* 判断当前时间是否受电机通电或余振污染。 */
    if (fitness_haptic_guard_is_contaminated(guard, input_sample->monotonic_ms)) {
        /* 没有左侧干净端点时无法插值，直接进入硬冻结。 */
        if (!guard->has_previous_clean) {
            /* 冻结计数直到下一干净采样。 */
            guard->hard_freeze = true;
            /* 清除无法使用的待插值点。 */
            guard->pending_count = 0U;
            /* 返回最新冻结状态。 */
            *counter_frozen = true;
            /* 污染采样被有意丢弃。 */
            return FITNESS_STATUS_OK;
        }

        /* 尚未硬冻结且污染长度不超过 3 点时，只保存时间戳等待右端点。 */
        if (!guard->hard_freeze &&
            (guard->pending_count < FITNESS_HAPTIC_MAX_INTERPOLATED_SAMPLES)) {
            /* 保存待修复采样时刻，不保存被污染的六轴值。 */
            guard->pending_timestamps[guard->pending_count] = input_sample->monotonic_ms;
            /* 增加待插值点数。 */
            guard->pending_count += 1U;
            /* 3 点以内暂不硬冻结；下一个干净点到来后会补发。 */
            *counter_frozen = false;
            /* 当前没有可交付采样。 */
            return FITNESS_STATUS_OK;
        }

        /* 第 4 个连续污染点触发硬冻结，不再尝试构造长段虚假波形。 */
        guard->hard_freeze = true;
        /* 丢弃此前 3 个待插值时间，状态机应整体重置当前周期。 */
        guard->pending_count = 0U;
        /* 通知调用方冻结计数。 */
        *counter_frozen = true;
        /* 污染采样被丢弃。 */
        return FITNESS_STATUS_OK;
    }

    /* 时间必须晚于已保存的干净端点，才能执行因果流重建。 */
    if (guard->has_previous_clean &&
        (input_sample->monotonic_ms < guard->previous_clean.monotonic_ms)) {
        /* 乱序采样不会覆盖端点。 */
        return FITNESS_STATUS_INVALID_TIME;
    }

    /* 硬冻结后的首个干净点只用于恢复，不回填超过 3 点的污染区。 */
    if (guard->hard_freeze) {
        /* 复制当前干净点作为唯一输出。 */
        output_samples[0] = *input_sample;
        /* 该点之后计数器可以重新建立新周期。 */
        output_samples[0].quality_flags |= FITNESS_QUALITY_COUNTER_FROZEN;
        /* 保存新的干净左端点。 */
        guard->previous_clean = *input_sample;
        /* 标记已有可用端点。 */
        guard->has_previous_clean = true;
        /* 清除冻结状态。 */
        guard->hard_freeze = false;
        /* 清除任何残留待插值点。 */
        guard->pending_count = 0U;
        /* 本次输出一个恢复采样。 */
        *output_count = 1U;
        /* 调用结束后不再冻结，但调用方看到输出质量位后应重置动作周期。 */
        *counter_frozen = false;
        /* 恢复成功。 */
        return FITNESS_STATUS_OK;
    }

    /* 有 1..3 个待修复点时，用前后干净点做逐轴线性插值。 */
    if ((guard->pending_count > 0U) && guard->has_previous_clean) {
        /* 两端时间跨度必须为正，避免除零。 */
        const uint64_t span_ms = input_sample->monotonic_ms - guard->previous_clean.monotonic_ms;
        /* 同一时间戳无法定义插值比例。 */
        if (span_ms == 0ULL) {
            /* 保留待处理状态，要求调用方修正时间源。 */
            return FITNESS_STATUS_INVALID_TIME;
        }

        /* 依次重建每个污染点，时间顺序与原采样一致。 */
        for (uint8_t sample_index = 0U; sample_index < guard->pending_count; ++sample_index) {
            /* 当前待修复点时间必须严格位于两个干净端点之间。 */
            const uint64_t pending_ms = guard->pending_timestamps[sample_index];
            /* 越界时间表示上游乱序或重复时间戳。 */
            if ((pending_ms <= guard->previous_clean.monotonic_ms) ||
                (pending_ms >= input_sample->monotonic_ms)) {
                /* 不输出部分结果，保持状态供诊断。 */
                return FITNESS_STATUS_INVALID_TIME;
            }
            /* 线性比例 r 位于 (0,1)，float 精度足够覆盖 25 Hz 时间间隔。 */
            const float ratio = (float)(pending_ms - guard->previous_clean.monotonic_ms) /
                                (float)span_ms;
            /* 写入原污染点的时间戳。 */
            output_samples[sample_index].monotonic_ms = pending_ms;
            /* 标记该点原受振动污染且已经插值修复。 */
            output_samples[sample_index].quality_flags =
                FITNESS_QUALITY_HAPTIC_CONTAMINATED | FITNESS_QUALITY_INTERPOLATED;
            /* 对固定六轴逐轴插值，保持 gx..az 顺序和原单位。 */
            for (uint8_t axis_index = 0U; axis_index < FITNESS_IMU_AXIS_COUNT; ++axis_index) {
                /* 读取左端点对应轴物理值。 */
                const float left_value = guard->previous_clean.axis[axis_index];
                /* 读取右端点对应轴物理值。 */
                const float right_value = input_sample->axis[axis_index];
                /* y=(1-r)*left+r*right，避免使用污染中心值。 */
                output_samples[sample_index].axis[axis_index] =
                    left_value + ratio * (right_value - left_value);
            }
        }

        /* 当前干净点紧跟在全部插值点之后输出。 */
        output_samples[guard->pending_count] = *input_sample;
        /* 输出数量为待插值点数加当前点，最大 4。 */
        *output_count = (size_t)guard->pending_count + 1U;
        /* 当前干净点成为下一段插值左端点。 */
        guard->previous_clean = *input_sample;
        /* 清空待插值队列。 */
        guard->pending_count = 0U;
        /* 本次完成短污染区修复。 */
        return FITNESS_STATUS_OK;
    }

    /* 普通干净点直接输出，不引入延迟。 */
    output_samples[0] = *input_sample;
    /* 保存为未来可能插值的左端点。 */
    guard->previous_clean = *input_sample;
    /* 标记端点有效。 */
    guard->has_previous_clean = true;
    /* 输出一个采样。 */
    *output_count = 1U;
    /* 正常输入处理成功。 */
    return FITNESS_STATUS_OK;
}
