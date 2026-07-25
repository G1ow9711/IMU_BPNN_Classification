/* 健身计数与热量领域核心实现；公式见 docs/算法原理、训练与实时计数.md。 */
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
    /* 生成唯一重复事件，供 UI、BLE 和日志共同消费。 */
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
    /* 手表固定佩戴在右腕，八类重复动作统一使用传感器可直接观测的正反腕部周期。 */
    counter->mode = FITNESS_REP_MODE_TWO_PHASE;
    /* 初始内部状态 0 表示等待 PRIMARY。 */
    counter->state = 0U;
    /* 候选相位设为哨兵，保证首次相位从计数 1 开始。 */
    counter->candidate_phase = FITNESS_PHASE_COUNT;
    /* 所有动作最长周期限制 10 秒，防止很久以前的不完整相位被拼接。 */
    counter->max_cycle_ms = 10000U;
    /* 通用不应期 300 ms，低于人体完成两次完整动作的合理间隔。 */
    counter->refractory_ms = 300U;

    /* 根据动作动力学设置最短完整周期。 */
    if (action == FITNESS_ACTION_JUMPING_JACK) {
        /* 开合跳完整腕部往返至少 400 ms，兼顾速度差异和抖动抑制。 */
        counter->min_cycle_ms = 400U;
    } else if (action == FITNESS_ACTION_WAVE) {
        /* 挥手周期允许更快，但仍至少 300 ms。 */
        counter->min_cycle_ms = 300U;
    } else if (action == FITNESS_ACTION_JUMPING_SQUAT) {
        /* 跳跃深蹲至少 800 ms；验证记录本身较慢，额外迟滞用于拒绝落地回弹形成的第二闭环。 */
        counter->min_cycle_ms = 800U;
    } else {
        /* 其它力量和跳跃往返至少 600 ms；验证集含更快弓步/收腹跳，不能强套深蹲节奏。 */
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
        /* 静止闭合后若检测器重新发布显式 PRIMARY，用新端点重建周期起点并覆盖休息时长。 */
        if ((counter->state == FITNESS_TWO_STATE_WAIT_SECONDARY) &&
            (phase == FITNESS_PHASE_PRIMARY)) {
            /* 当前主向端点是新周期真实起点，避免把短休息时间算入动作时长。 */
            counter->cycle_start_ms = now_ms;
            /* 仍等待反向 SECONDARY，不增加次数。 */
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
            /* REST 可能来自返回主向端点；先把它保存为下一周期起点，显式 PRIMARY 到来时可安全重建。 */
            counter->state = FITNESS_TWO_STATE_WAIT_SECONDARY;
            /* 保存闭合时刻；连续动作直接沿用，静止后新 PRIMARY 会覆盖该时间。 */
            counter->cycle_start_ms = now_ms;
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

    /* 两相位顺序不匹配时保持当前状态。 */
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
    bool *step_accepted)
{
    /* 计数器和结果指针必须有效。 */
    if ((counter == NULL) || (step_accepted == NULL)) {
        /* 缺少结果缓冲时不修改计数。 */
        return FITNESS_STATUS_INVALID_ARGUMENT;
    }
    /* 未初始化状态不能接收步峰。 */
    if (!counter->initialized) {
        /* 要求先指定 walk 或 trot。 */
        return FITNESS_STATUS_INVALID_STATE;
    }

    /* 默认拒绝当前步峰。 */
    *step_accepted = false;
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
    /* 步峰处理成功。 */
    return FITNESS_STATUS_OK;
}
