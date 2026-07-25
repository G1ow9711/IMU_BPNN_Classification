/* 引入应用协调器公开合同。 */
#include "device_coordinator.h"

/* 引入 isfinite 检查 11 维 logits 和六轴样本。 */
#include <math.h>
/* 引入 NULL 完成指针边界检查。 */
#include <stddef.h>
/* 引入 memset 确定性清空每次按值 effect。 */
#include <string.h>

/* BLE LiveStateV1 设备状态 0..7 与协议文档保持稳定。 */
typedef enum device_ble_state {
    /* 启动与自检期间尚不允许训练。 */
    DEVICE_BLE_STATE_BOOTING = 0,
    /* 主页空闲，可接受新会话。 */
    DEVICE_BLE_STATE_IDLE = 1,
    /* 正在形成首个完整窗口并锁定本会话唯一动作。 */
    DEVICE_BLE_STATE_PREPARING = 2,
    /* 已锁定动作并接收 25 Hz 样本。 */
    DEVICE_BLE_STATE_RUNNING = 3,
    /* 会话保留，但时间、热量和计数冻结。 */
    DEVICE_BLE_STATE_PAUSED = 4,
    /* 摘要待存储任务确认。 */
    DEVICE_BLE_STATE_SUMMARY = 5,
    /* 关键自检或领域错误。 */
    DEVICE_BLE_STATE_ERROR = 6,
    /* 已进入安全关机顺序。 */
    DEVICE_BLE_STATE_SHUTDOWN = 7
} device_ble_state_t;

/* BLE power_flags 位定义；单字节可同时表示充电和低电。 */
typedef enum device_power_flag {
    /* 当前 VBUS/充电状态有效。 */
    DEVICE_POWER_FLAG_CHARGING = 1U << 0,
    /* 厂家 400 mAh 电池不高于 15%。 */
    DEVICE_POWER_FLAG_WARNING = 1U << 1,
    /* 非充电时不高于 8%，禁止新会话。 */
    DEVICE_POWER_FLAG_BLOCK_START = 1U << 2,
    /* 非充电时不高于 5%，需要保存关机。 */
    DEVICE_POWER_FLAG_CRITICAL = 1U << 3
} device_power_flag_t;

/* 把 uint64 饱和到 UI/BLE 的 uint32，禁止截断回绕。 */
static uint32_t device_saturate_u64_to_u32(const uint64_t value)
{
    /* 超过 32 位上限时固定为 UINT32_MAX。 */
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

/* 饱和累加 uint64，保护长时运行诊断值不回绕。 */
static uint64_t device_saturating_add_u64(const uint64_t left, const uint64_t right)
{
    /* 右值大于剩余空间时固定为最大值。 */
    return right > (UINT64_MAX - left) ? UINT64_MAX : left + right;
}

/* 把 0..65535 置信度转为 UI 0..10000，使用整数四舍五入。 */
static uint16_t device_confidence_to_centipercent(const uint16_t confidence_q15)
{
    /* 先用 32 位乘法避免 16 位溢出，32767 实现四舍五入。 */
    return (uint16_t)(((uint32_t)confidence_q15 * 10000U + 32767U) / 65535U);
}

/* 将 fitness 指标枚举转为 BLE 0=无、1=次、2=步、3=秒。 */
static uint8_t device_ble_metric_kind(
    const uint8_t action_id,
    const fitness_metric_kind_t metric_kind)
{
    /* 未锁类时固定输出 0，不将占位 REP 冒充真指标。 */
    if (action_id == WORKOUT_ACTION_UNKNOWN) {
        /* 返回无指标。 */
        return 0U;
    }
    /* fitness 0..2 与 BLE 1..3 只相差 1。 */
    return (uint8_t)metric_kind + 1U;
}

/* 把 workout 状态和全局 UI 安全状态投影到 BLE 设备状态。 */
static uint8_t device_ble_state(const device_coordinator_t *coordinator)
{
    /* 关机页优先于 workout 状态，避免 PC 继续发控制命令。 */
    if ((coordinator->ui.state == UI_STATE_SHUTDOWN) ||
        (coordinator->power.state == POWER_STATE_SAFE_SHUTDOWN)) {
        /* 返回关机。 */
        return DEVICE_BLE_STATE_SHUTDOWN;
    }
    /* UI 错误页必须告知 PC。 */
    if (coordinator->ui.state == UI_STATE_ERROR) {
        /* 返回错误。 */
        return DEVICE_BLE_STATE_ERROR;
    }
    /* 按 workout 领域状态一对一映射。 */
    switch (coordinator->workout.state) {
        /* 空闲对应 Idle。 */
        case WORKOUT_STATE_IDLE: return DEVICE_BLE_STATE_IDLE;
        /* 准备对应 Preparing。 */
        case WORKOUT_STATE_PREPARING: return DEVICE_BLE_STATE_PREPARING;
        /* 运行对应 Running。 */
        case WORKOUT_STATE_RUNNING: return DEVICE_BLE_STATE_RUNNING;
        /* 暂停对应 Paused。 */
        case WORKOUT_STATE_PAUSED: return DEVICE_BLE_STATE_PAUSED;
        /* 总结对应 Summary。 */
        case WORKOUT_STATE_SUMMARY: return DEVICE_BLE_STATE_SUMMARY;
        /* 未知内存值按 Error 安全处理。 */
        default: return DEVICE_BLE_STATE_ERROR;
    }
}

/* 返回当前电源位图，不依赖未知电量的数值比较。 */
static uint8_t device_power_flags(const power_manager_t *power)
{
    /* 从空位图开始。 */
    uint8_t flags = 0U;
    /* 充电时设置充电位。 */
    if (power->charging) {
        /* 合并充电位。 */
        flags |= DEVICE_POWER_FLAG_CHARGING;
    }
    /* 255 表示未知，不设任何阈值位。 */
    if (power->battery_percent == UINT8_MAX) {
        /* 返回当前充电位。 */
        return flags;
    }
    /* 15% 及以下设置警告位。 */
    if (power->battery_percent <= POWER_BATTERY_WARNING_PERCENT) {
        /* 合并警告位。 */
        flags |= DEVICE_POWER_FLAG_WARNING;
    }
    /* 8% 及以下设置启动阻止位。 */
    if (power->battery_percent <= POWER_BATTERY_BLOCK_START_PERCENT) {
        /* 合并阻止位。 */
        flags |= DEVICE_POWER_FLAG_BLOCK_START;
    }
    /* 5% 及以下设置临界位。 */
    if (power->battery_percent <= POWER_BATTERY_CRITICAL_PERCENT) {
        /* 合并临界位。 */
        flags |= DEVICE_POWER_FLAG_CRITICAL;
    }
    /* 返回完整位图。 */
    return flags;
}

/* 把 workout 领域返回码转换为产品返回码。 */
static device_coordinator_status_t device_map_workout_status(const workout_status_t status)
{
    /* 按稳定语义显式转换。 */
    switch (status) {
        /* 正常成功。 */
        case WORKOUT_STATUS_OK: return DEVICE_COORDINATOR_OK;
        /* 有意忽略。 */
        case WORKOUT_STATUS_IGNORED: return DEVICE_COORDINATOR_IGNORED;
        /* 参数错误。 */
        case WORKOUT_STATUS_ERR_ARGUMENT: return DEVICE_COORDINATOR_ERR_ARGUMENT;
        /* 状态错误。 */
        case WORKOUT_STATUS_ERR_STATE: return DEVICE_COORDINATOR_ERR_STATE;
        /* 时间错误。 */
        case WORKOUT_STATUS_ERR_TIME: return DEVICE_COORDINATOR_ERR_TIME;
        /* 其它下游错误。 */
        default: return DEVICE_COORDINATOR_ERR_DOMAIN;
    }
}

/* 清空一次 effect，保证错误返回不泄漏上次调用数据。 */
static void device_effects_clear(device_effects_t *effects)
{
    /* 全字节置零，flags=0 使其它字段无效。 */
    (void)memset(effects, 0, sizeof(*effects));
}

/* 检查新时间不早于已提交时间。 */
static bool device_time_is_valid(
    const device_coordinator_t *coordinator,
    const uint64_t monotonic_ms)
{
    /* 相同毫秒可包含样本和紧随的推理结果。 */
    return monotonic_ms >= coordinator->last_monotonic_ms;
}

/* 在 PREPARING/RUNNING 期间累加排除暂停的会话时长。 */
static void device_advance_active_elapsed(
    device_coordinator_t *coordinator,
    const uint64_t monotonic_ms)
{
    /* 只有准备和运行时间计入可见会话时长。 */
    if ((coordinator->workout.state == WORKOUT_STATE_PREPARING) ||
        (coordinator->workout.state == WORKOUT_STATE_RUNNING)) {
        /* 计算自上次记账点的正向间隔。 */
        const uint64_t delta_ms = monotonic_ms - coordinator->last_active_accounted_ms;
        /* 饱和累加活动毫秒。 */
        coordinator->active_elapsed_ms = device_saturating_add_u64(
            coordinator->active_elapsed_ms,
            delta_ms);
    }
    /* 无论当前状态如何，新活动起点都从本时刻开始。 */
    coordinator->last_active_accounted_ms = monotonic_ms;
    /* 保存最近已提交全局时间。 */
    coordinator->last_monotonic_ms = monotonic_ms;
}

/* 修订号自然回绕；单设备每次快照都使用同一函数。 */
static void device_increment_revision(device_coordinator_t *coordinator)
{
    /* uint32 回绕属协议允许行为，PC 按模序比较。 */
    coordinator->state_revision += 1U;
}

/* 根据当前权威指标计算目标完成度；无目标返回 BLE 哨兵 255。 */
static uint8_t device_calculate_goal_percent(
    const device_coordinator_t *coordinator,
    const workout_snapshot_t *snapshot)
{
    /* 无目标时不得显示 0%，否则 PC 会误认为已设置但尚未开始。 */
    if (coordinator->goal_kind == (uint8_t)DEVICE_GOAL_NONE) {
        /* 返回协议定义的未设置哨兵。 */
        return BLE_SERVICE_GOAL_NOT_SET;
    }
    /* current_value 统一提升为 uint64，避免毫秒和热量累计截断。 */
    uint64_t current_value = UINT64_C(0);
    /* 次数目标只读取重复动作次数，不把 walk/trot 步数混作次数。 */
    if (coordinator->goal_kind == (uint8_t)DEVICE_GOAL_REPETITIONS) {
        /* 当前动作指标确为 REP 时才使用权威总次数。 */
        if (snapshot->metric_kind == FITNESS_METRIC_REPETITION) {
            /* 复制 workout 的权威次数。 */
            current_value = snapshot->metric_value;
        }
    } else if (coordinator->goal_kind == (uint8_t)DEVICE_GOAL_SECONDS) {
        /* 时长目标使用排除暂停后的活动毫秒向下取整为整秒。 */
        current_value = coordinator->active_elapsed_ms / UINT64_C(1000);
    } else if (coordinator->goal_kind == (uint8_t)DEVICE_GOAL_MCAL) {
        /* gross_microkcal 除以 1000 转换为协议 mcal。 */
        current_value = snapshot->gross_microkcal / UINT64_C(1000);
    } else {
        /* 初始化和设置 API 已拦截未知 kind，此处只作内存损坏防御。 */
        return BLE_SERVICE_GOAL_NOT_SET;
    }
    /* 目标值由共享配置合同保证大于 0。 */
    const uint64_t target_value = (uint64_t)coordinator->goal_value;
    /* 达到或超过目标时固定显示 100%，禁止回绕。 */
    if (current_value >= target_value) {
        /* 返回饱和值。 */
        return UINT8_C(100);
    }
    /* 此分支 current<target<=UINT32_MAX，所以 current*100 不会溢出 uint64。 */
    const uint64_t percent = (current_value * UINT64_C(100)) / target_value;
    /* 数学上结果为 0～99，可安全收窄为 u8。 */
    return (uint8_t)percent;
}

/* 按 workout 权威快照生成 UI 展示值，不增加次数或热量。 */
static device_coordinator_status_t device_build_view_model(
    const device_coordinator_t *coordinator,
    ui_view_model_t *view,
    workout_snapshot_t *snapshot)
{
    /* 输出指针必须有效。 */
    if ((view == NULL) || (snapshot == NULL)) {
        /* 返回参数错误。 */
        return DEVICE_COORDINATOR_ERR_ARGUMENT;
    }
    /* 读取 workout 权威快照。 */
    if (workout_engine_snapshot(&coordinator->workout, snapshot) != WORKOUT_STATUS_OK) {
        /* 下游快照异常。 */
        return DEVICE_COORDINATOR_ERR_DOMAIN;
    }
    /* 从当前 UI 顶栏开始，保留 BLE/电池/充电属性。 */
    *view = coordinator->ui.view;
    /* 复制本轮主动作或 255 未知哨兵；该字段只决定计数器和指标单位。 */
    view->action_id = snapshot->action_id;
    /* 复制最近推理窗口真实类别，使手表休息时不伪装为仍在执行主动作。 */
    view->inferred_action_id = snapshot->inferred_action_id;
    /* 复制领域计数门；false 时 UI 显示暂不计数但保留本轮累计。 */
    view->counting_enabled = snapshot->classification_consistent;
    /* sit 快照已转为秒，其它动作是次或步。 */
    view->count = device_saturate_u64_to_u32(snapshot->metric_value);
    /* microkcal 除以 1000 变为 0.001 kcal，只做单位投影。 */
    view->calories_milli_kcal = device_saturate_u64_to_u32(
        snapshot->gross_microkcal / 1000ULL);
    /* 活动毫秒除以 1000 变为完整秒。 */
    view->elapsed_seconds = device_saturate_u64_to_u32(
        coordinator->active_elapsed_ms / 1000ULL);
    /* 把 BLE Q15 置信度转为 UI 0.01% 单位。 */
    view->confidence_centipercent = device_confidence_to_centipercent(
        snapshot->confidence_q15);
    /* 电量直接使用 PMIC 百分比或 255 未知。 */
    view->battery_percent = coordinator->power.battery_percent;
    /* BLE 图标使用 power_manager 已提交连接属性。 */
    view->ble_connected = coordinator->power.ble_connected;
    /* 充电图标使用 PMIC/VBUS 属性。 */
    view->charging = coordinator->power.charging;
    /* 质量位原样投影，不重新解释。 */
    view->data_quality_flags = snapshot->quality_flags;
    /* 生成成功。 */
    return DEVICE_COORDINATOR_OK;
}

/* 根据当前权威状态构造 BLE LiveStateV1。 */
static device_coordinator_status_t device_build_live_state(
    const device_coordinator_t *coordinator,
    ble_service_live_state_v1_t *live_state)
{
    /* 输出必须有效。 */
    if (live_state == NULL) {
        /* 返回参数错误。 */
        return DEVICE_COORDINATOR_ERR_ARGUMENT;
    }
    /* 读取 workout 权威快照。 */
    workout_snapshot_t snapshot;
    /* 快照异常时不生成半帧。 */
    if (workout_engine_snapshot(&coordinator->workout, &snapshot) != WORKOUT_STATUS_OK) {
        /* 返回领域错误。 */
        return DEVICE_COORDINATOR_ERR_DOMAIN;
    }
    /* 先清零保留字段。 */
    (void)memset(live_state, 0, sizeof(*live_state));
    /* 复制当前会话序号；Idle 为 0。 */
    live_state->session_sequence = snapshot.session_seq;
    /* 复制当前权威修订号。 */
    live_state->state_revision = coordinator->state_revision;
    /* 排除暂停后的会话时长饱和到 uint32 毫秒。 */
    live_state->elapsed_ms = device_saturate_u64_to_u32(
        coordinator->active_elapsed_ms);
    /* 投影产品状态 0..7。 */
    live_state->device_state = device_ble_state(coordinator);
    /* 复制动作索引或 255。 */
    live_state->action_id = snapshot.action_id;
    /* 把 fitness 指标类型转为 BLE 0..3。 */
    live_state->metric_kind = device_ble_metric_kind(
        snapshot.action_id,
        snapshot.metric_kind);
    /* 复制电池百分比或 255。 */
    live_state->battery_percent = coordinator->power.battery_percent;
    /* 次/步/秒饱和到 32 位线上字段。 */
    live_state->metric_value = device_saturate_u64_to_u32(snapshot.metric_value);
    /* 复制模型 Q15 置信度。 */
    live_state->confidence_q15 = snapshot.confidence_q15;
    /* microkcal 除以 1000 变为 mcal。 */
    live_state->calories_mcal = device_saturate_u64_to_u32(
        snapshot.gross_microkcal / 1000ULL);
    /* 复制数据质量位。 */
    live_state->quality_flags = snapshot.quality_flags;
    /* 生成充电和低电位图。 */
    live_state->power_flags = device_power_flags(&coordinator->power);
    /* 按次数、活动秒或 mcal 计算 0～100；无目标使用 255。 */
    live_state->goal_percent = device_calculate_goal_percent(coordinator, &snapshot);
    /* 生成成功。 */
    return DEVICE_COORDINATOR_OK;
}

/* 把当前 view 通过 UI 状态机合法事件提交。 */
static device_coordinator_status_t device_sync_ui_metrics(
    device_coordinator_t *coordinator,
    const uint64_t monotonic_ms)
{
    /* 生成可见指标和 workout 快照。 */
    ui_view_model_t view;
    /* 快照仅用于组装 view。 */
    workout_snapshot_t snapshot;
    /* 生成失败时不修改 UI。 */
    const device_coordinator_status_t build_status = device_build_view_model(
        coordinator,
        &view,
        &snapshot);
    /* 错误原样返回。 */
    if (build_status != DEVICE_COORDINATOR_OK) {
        /* 传播错误。 */
        return build_status;
    }
    /* 仅 Running 或训练熄屏状态允许 METRIC_UPDATED。 */
    const bool running_visible = coordinator->ui.state == UI_STATE_RUNNING;
    /* 检查熄屏恢复点是否为 Running。 */
    const bool running_hidden = (coordinator->ui.state == UI_STATE_SCREEN_OFF) &&
                                (coordinator->ui.screen_off_resume_state == UI_STATE_RUNNING);
    /* 非运行页只更新内部快照，总结页仍能看到最终值。 */
    if (!running_visible && !running_hidden) {
        /* 直接写入已验证范围的值拷贝。 */
        coordinator->ui.view = view;
        /* 同步成功。 */
        return DEVICE_COORDINATOR_OK;
    }
    /* 构造 UI 指标事件。 */
    ui_event_t event;
    /* 清零不使用载荷。 */
    (void)memset(&event, 0, sizeof(event));
    /* 指定指标更新。 */
    event.type = UI_EVENT_METRIC_UPDATED;
    /* UI 仅保留低 32 位状态进入时间。 */
    event.monotonic_ms = (uint32_t)monotonic_ms;
    /* 按值复制完整快照。 */
    event.metrics = view;
    /* 分派必须成功，否则表示状态链不一致。 */
    if (ui_dispatch_event(&coordinator->ui, &event) != UI_DISPATCH_OK) {
        /* 返回领域错误。 */
        return DEVICE_COORDINATOR_ERR_DOMAIN;
    }
    /* 同步成功。 */
    return DEVICE_COORDINATOR_OK;
}

/* 生成 UI/LiveState/Power 快照 effect，不访问硬件。 */
static device_coordinator_status_t device_emit_snapshots(
    device_coordinator_t *coordinator,
    device_effects_t *effects,
    const bool include_power)
{
    /* 按值复制 UI 完整上下文。 */
    effects->ui = coordinator->ui;
    /* 标记 UI effect 有效。 */
    effects->flags |= DEVICE_EFFECT_UI_RENDER;
    /* 生成 BLE LiveState。 */
    const device_coordinator_status_t live_status = device_build_live_state(
        coordinator,
        &effects->live_state);
    /* 生成失败时返回错误，外层事务将回滚。 */
    if (live_status != DEVICE_COORDINATOR_OK) {
        /* 传播错误。 */
        return live_status;
    }
    /* 标记 LiveState effect 有效。 */
    effects->flags |= DEVICE_EFFECT_BLE_LIVE_STATE;
    /* 仅状态/电源事件需重新应用完整功耗策略。 */
    if (include_power) {
        /* 读取纯函数电源策略。 */
        effects->power_policy = power_manager_policy(&coordinator->power);
        /* 标记电源 effect 有效。 */
        effects->flags |= DEVICE_EFFECT_POWER_POLICY;
    }
    /* 生成成功。 */
    return DEVICE_COORDINATOR_OK;
}

/* 从 workout 权威字段构造会话摘要，不重算指标。 */
static device_coordinator_status_t device_build_summary(
    const device_coordinator_t *coordinator,
    const uint16_t flags,
    session_summary_t *summary)
{
    /* 输出指针必须有效。 */
    if (summary == NULL) {
        /* 返回参数错误。 */
        return DEVICE_COORDINATOR_ERR_ARGUMENT;
    }
    /* 未锁定动作的准备会话不能伪造动作摘要。 */
    if (coordinator->workout.selected_action == WORKOUT_ACTION_UNKNOWN) {
        /* 返回状态错误。 */
        return DEVICE_COORDINATOR_ERR_STATE;
    }
    /* 清零线性格式的所有字段。 */
    (void)memset(summary, 0, sizeof(*summary));
    /* 会话序号同时是存储主键。 */
    summary->session_seq = coordinator->workout.session_seq;
    /* 最大事件序号是同会话幂等比较键。 */
    summary->last_event_seq = coordinator->last_event_seq;
    /* 复制锁定动作 0..10。 */
    summary->action_id = coordinator->workout.selected_action;
    /* sit 持续毫秒、walk/trot 步数、其它次数分别使用 2/1/0。 */
    if (coordinator->workout.selected_action == FITNESS_ACTION_SIT) {
        /* 设置持续时间指标。 */
        summary->metric_kind = (uint8_t)FITNESS_METRIC_DURATION_MS;
        /* 直接复制 fitness_session 权威毫秒。 */
        summary->metric_total = coordinator->workout.fitness_session.sit_duration_ms;
    } else if ((coordinator->workout.selected_action == FITNESS_ACTION_WALK) ||
               (coordinator->workout.selected_action == FITNESS_ACTION_TROT)) {
        /* 设置步数指标。 */
        summary->metric_kind = (uint8_t)FITNESS_METRIC_STEP;
        /* 直接复制 fitness_session 权威步数。 */
        summary->metric_total = coordinator->workout.fitness_session.steps;
    } else {
        /* 其余 8 类使用次数。 */
        summary->metric_kind = (uint8_t)FITNESS_METRIC_REPETITION;
        /* 直接复制 fitness_session 权威次数。 */
        summary->metric_total = coordinator->workout.fitness_session.repetitions;
    }
    /* 复制完成/中断/临界低电标志。 */
    summary->flags = flags;
    /* 未同步 UTC，固定为 0。 */
    summary->start_unix_ms = DEVICE_COORDINATOR_UNIX_TIME_UNKNOWN;
    /* 复制排除暂停的活动毫秒。 */
    summary->duration_ms = coordinator->active_elapsed_ms;
    /* 直接复制 workout 权威毛热量。 */
    summary->gross_microkcal = coordinator->workout.fitness_session.gross_microkcal;
    /* 直接复制 workout 权威活动热量。 */
    summary->active_microkcal = coordinator->workout.fitness_session.active_microkcal;
    /* 空会话没有稳定度样本，平均值为 0。 */
    if (!coordinator->has_stability || (coordinator->event_count == 0U)) {
        /* 写入空平均。 */
        summary->average_stability_q15 = 0U;
        /* 写入空最低值。 */
        summary->minimum_stability_q15 = 0U;
    } else {
        /* 整数除法得到事件等权 Q15 平均。 */
        summary->average_stability_q15 = (uint16_t)(
            coordinator->stability_sum_q15 / coordinator->event_count);
        /* 复制会话最低稳定度。 */
        summary->minimum_stability_q15 = coordinator->minimum_stability_q15;
    }
    /* 复制已吸收事件数，用于诊断而不参与幂等判定。 */
    summary->event_count = coordinator->event_count;
    /* 生成成功。 */
    return DEVICE_COORDINATOR_OK;
}

/* 把一个 MetricEvent 吸收到摘要统计，事件序号必须严格递增。 */
static device_coordinator_status_t device_absorb_metric_event(
    device_coordinator_t *coordinator,
    const fitness_metric_event_t *event)
{
    /* 会话和动作必须与当前锁定领域状态一致。 */
    if ((event->session_seq != coordinator->workout.session_seq) ||
        ((uint8_t)event->action != coordinator->workout.selected_action) ||
        (event->event_seq <= coordinator->last_event_seq)) {
        /* 事实链冲突不可提交。 */
        return DEVICE_COORDINATOR_ERR_DOMAIN;
    }
    /* 保存最大序号作幂等键。 */
    coordinator->last_event_seq = event->event_seq;
    /* 饱和增加事件数。 */
    if (coordinator->event_count < UINT32_MAX) {
        /* 记录一个新事件。 */
        coordinator->event_count += 1U;
    }
    /* 饱和累加稳定度，防止长会话回绕。 */
    coordinator->stability_sum_q15 = device_saturating_add_u64(
        coordinator->stability_sum_q15,
        event->stability_q15);
    /* 首个事件或更低值更新会话最低稳定度。 */
    if (!coordinator->has_stability ||
        (event->stability_q15 < coordinator->minimum_stability_q15)) {
        /* 保存新最低值。 */
        coordinator->minimum_stability_q15 = event->stability_q15;
    }
    /* 标记已有稳定度样本。 */
    coordinator->has_stability = true;
    /* 吸收成功。 */
    return DEVICE_COORDINATOR_OK;
}

/* 从训练引擎取出全部准备期补算事件，按序吸收摘要统计并写入本次 effect。 */
static device_coordinator_status_t device_drain_replay_metric_events(
    device_coordinator_t *coordinator,
    device_effects_t *effects)
{
    /* 协调器和效果对象必须有效。 */
    if ((coordinator == NULL) || (effects == NULL)) {
        /* 返回参数错误。 */
        return DEVICE_COORDINATOR_ERR_ARGUMENT;
    }
    /* 在固定 effect 容量内逐条取最早事件。 */
    while (effects->replay_metric_event_count <
           DEVICE_COORDINATOR_MAX_REPLAY_METRIC_EFFECTS) {
        /* 当前写槽同时是尚未交付的事件索引。 */
        fitness_metric_event_t *event =
            &effects->replay_metric_events[effects->replay_metric_event_count];
        /* 空队列表示全部补算事件已取完。 */
        if (!workout_engine_pop_replay_metric_event(&coordinator->workout, event)) {
            /* 正常结束循环。 */
            break;
        }
        /* 每条事件必须按严格递增序号进入协调器摘要统计。 */
        const device_coordinator_status_t absorb_status = device_absorb_metric_event(
            coordinator,
            event);
        /* 事件链冲突时拒绝整次锁类候选事务。 */
        if (absorb_status != DEVICE_COORDINATOR_OK) {
            /* 返回真实错误。 */
            return absorb_status;
        }
        /* 有效事件数增加一。 */
        effects->replay_metric_event_count += 1U;
    }
    /* 训练引擎仍有事件表示容量推导漂移，禁止静默截断。 */
    if (coordinator->workout.replay_metric_event_count != 0U) {
        /* 返回领域错误。 */
        return DEVICE_COORDINATOR_ERR_DOMAIN;
    }
    /* 至少一条事件时启用 BLE Event effect。 */
    if (effects->replay_metric_event_count > 0U) {
        /* 主任务将逐条排入 BLE 输出队列。 */
        effects->flags |= DEVICE_EFFECT_BLE_EVENT;
    }
    /* 全部事件已按序吸收。 */
    return DEVICE_COORDINATOR_OK;
}

/* 重置当前会话派生统计，不修改持久化序号。 */
static void device_reset_session_projection(
    device_coordinator_t *coordinator,
    const uint64_t monotonic_ms)
{
    /* 保存新会话单调开始。 */
    coordinator->session_started_ms = monotonic_ms;
    /* 活动时长从零开始。 */
    coordinator->active_elapsed_ms = 0ULL;
    /* 记账点从开始时刻起算。 */
    coordinator->last_active_accounted_ms = monotonic_ms;
    /* 事件幂等键从 0 开始。 */
    coordinator->last_event_seq = 0U;
    /* 事件数从 0 开始。 */
    coordinator->event_count = 0U;
    /* 清空稳定度总和。 */
    coordinator->stability_sum_q15 = 0ULL;
    /* 清空最低稳定度。 */
    coordinator->minimum_stability_q15 = 0U;
    /* 标记尚无稳定度样本。 */
    coordinator->has_stability = false;
}

/* 分派一个无载荷 UI 事件并要求真正转移。 */
static device_coordinator_status_t device_dispatch_ui(
    device_coordinator_t *coordinator,
    const ui_event_type_t type,
    const uint64_t monotonic_ms)
{
    /* 构造纯值 UI 事件。 */
    ui_event_t event;
    /* 清零未使用载荷。 */
    (void)memset(&event, 0, sizeof(event));
    /* 写入事件类型。 */
    event.type = type;
    /* 写入低 32 位单调时间供动画和诊断。 */
    event.monotonic_ms = (uint32_t)monotonic_ms;
    /* 该 helper 仅用于必须转移的事件。 */
    if (ui_dispatch_event(&coordinator->ui, &event) != UI_DISPATCH_OK) {
        /* 页面与产品状态不一致。 */
        return DEVICE_COORDINATOR_ERR_STATE;
    }
    /* 分派成功。 */
    return DEVICE_COORDINATOR_OK;
}

/* 分派一个电源事件；要求状态实际发生变化。 */
static device_coordinator_status_t device_dispatch_power(
    device_coordinator_t *coordinator,
    const power_event_type_t type,
    const uint64_t monotonic_ms,
    const bool charging,
    const bool require_change)
{
    /* 构造纯值电源事件。 */
    power_event_t event;
    /* 写入事件类型。 */
    event.type = type;
    /* 写入低 32 位单调时间。 */
    event.monotonic_ms = (uint32_t)monotonic_ms;
    /* 充电属性仅 CHARGING_CHANGED 使用。 */
    event.charging = charging;
    /* 调用纯电源状态机。 */
    const bool changed = power_manager_dispatch(&coordinator->power, &event);
    /* 必须变化的业务事件未转移时报状态错误。 */
    if (require_change && !changed) {
        /* 返回状态错误。 */
        return DEVICE_COORDINATOR_ERR_STATE;
    }
    /* 分派成功或幂等无变化。 */
    return DEVICE_COORDINATOR_OK;
}

/* 处理正常停止，产生总结 UI、电源和最终摘要。 */
static device_coordinator_status_t device_stop_session(
    device_coordinator_t *coordinator,
    const uint64_t monotonic_ms,
    const uint16_t summary_flags,
    device_effects_t *effects)
{
    /* 停止前先把最终热量和时长投影到 UI。 */
    const device_coordinator_status_t sync_status = device_sync_ui_metrics(
        coordinator,
        monotonic_ms);
    /* 投影失败时不停止候选副本。 */
    if (sync_status != DEVICE_COORDINATOR_OK) {
        /* 传播错误。 */
        return sync_status;
    }
    /* 保存停止前是否已锁定动作。 */
    const bool has_action = coordinator->workout.selected_action != WORKOUT_ACTION_UNKNOWN;
    /* 停止 workout 领域会话。 */
    const workout_status_t stop_status = workout_engine_stop(
        &coordinator->workout,
        monotonic_ms);
    /* 任何停止错误中止事务。 */
    if (stop_status != WORKOUT_STATUS_OK) {
        /* 映射错误。 */
        return device_map_workout_status(stop_status);
    }
    /* UI 按当前 PREPARE/RUNNING/PAUSED 规则进入 HOME 或 SUMMARY。 */
    const device_coordinator_status_t ui_status = device_dispatch_ui(
        coordinator,
        UI_EVENT_STOP_REQUESTED,
        monotonic_ms);
    /* UI 不一致时回滚外层副本。 */
    if (ui_status != DEVICE_COORDINATOR_OK) {
        /* 传播错误。 */
        return ui_status;
    }
    /* 电源从 Running/Paused 回 HOME。 */
    const device_coordinator_status_t power_status = device_dispatch_power(
        coordinator,
        POWER_EVENT_SESSION_STOPPED,
        monotonic_ms,
        false,
        true);
    /* 电源不一致时回滚。 */
    if (power_status != DEVICE_COORDINATOR_OK) {
        /* 传播错误。 */
        return power_status;
    }
    /* 已锁定动作时生成最终幂等摘要。 */
    if (has_action) {
        /* 生成持久化值。 */
        const device_coordinator_status_t summary_status = device_build_summary(
            coordinator,
            summary_flags,
            &effects->summary);
        /* 摘要生成失败时回滚。 */
        if (summary_status != DEVICE_COORDINATOR_OK) {
            /* 传播错误。 */
            return summary_status;
        }
        /* 标记摘要写入 effect。 */
        effects->flags |= DEVICE_EFFECT_SUMMARY_WRITE;
    } else {
        /* PREPARING 取消没有动作摘要，直接回到 Idle 与 HOME 对齐。 */
        workout_engine_return_idle(&coordinator->workout);
    }
    /* 停止成功。 */
    return DEVICE_COORDINATOR_OK;
}

device_coordinator_status_t device_coordinator_init(
    device_coordinator_t *coordinator,
    const device_coordinator_config_t *config,
    const uint64_t monotonic_ms)
{
    /* 必填指针、非零序号、统一体重范围和目标组合必须有效。 */
    if ((coordinator == NULL) || (config == NULL) ||
        (config->next_session_seq == 0U) ||
        (config->weight_g < DEVICE_CONFIG_WEIGHT_MIN_G) ||
        (config->weight_g > DEVICE_CONFIG_WEIGHT_MAX_G) ||
        (device_config_validate_goal(config->goal_kind, config->goal_value) != DEVICE_CONFIG_OK)) {
        /* 返回参数错误。 */
        return DEVICE_COORDINATOR_ERR_ARGUMENT;
    }
    /* 清空全部静态状态。 */
    (void)memset(coordinator, 0, sizeof(*coordinator));
    /* 初始化训练引擎到 Idle。 */
    workout_engine_init(&coordinator->workout);
    /* 初始化 UI 到 Boot。 */
    ui_context_init(&coordinator->ui, (uint32_t)monotonic_ms);
    /* 产品协调器在板级自检成功后初始化，因此连续转到 Home。 */
    ui_event_t ui_event;
    /* 清零事件载荷。 */
    (void)memset(&ui_event, 0, sizeof(ui_event));
    /* 通知 Boot 动画结束。 */
    ui_event.type = UI_EVENT_BOOT_READY;
    /* 保存低 32 位时间。 */
    ui_event.monotonic_ms = (uint32_t)monotonic_ms;
    /* Boot 必须进入 SelfTest。 */
    if (ui_dispatch_event(&coordinator->ui, &ui_event) != UI_DISPATCH_OK) {
        /* 理论上只能表示编程错误。 */
        return DEVICE_COORDINATOR_ERR_DOMAIN;
    }
    /* 通知板级自检已成功。 */
    ui_event.type = UI_EVENT_SELF_TEST_OK;
    /* SelfTest 必须进入 Home。 */
    if (ui_dispatch_event(&coordinator->ui, &ui_event) != UI_DISPATCH_OK) {
        /* 返回领域错误。 */
        return DEVICE_COORDINATOR_ERR_DOMAIN;
    }
    /* 初始化电源状态到 Boot。 */
    power_manager_init(
        &coordinator->power,
        (uint32_t)monotonic_ms,
        config->allow_imu_deep_wake);
    /* 构造启动完成事件。 */
    power_event_t power_event;
    /* 写入启动完成类型。 */
    power_event.type = POWER_EVENT_BOOT_COMPLETED;
    /* 写入低 32 位时间。 */
    power_event.monotonic_ms = (uint32_t)monotonic_ms;
    /* 本事件不读充电字段，固定 false。 */
    power_event.charging = false;
    /* Power Boot 必须进入 Home。 */
    if (!power_manager_dispatch(&coordinator->power, &power_event)) {
        /* 返回领域错误。 */
        return DEVICE_COORDINATOR_ERR_DOMAIN;
    }
    /* 复制下一持久化会话序号。 */
    coordinator->next_session_seq = config->next_session_seq;
    /* 复制体重 g。 */
    coordinator->weight_g = config->weight_g;
    /* 复制初始训练目标种类。 */
    coordinator->goal_kind = config->goal_kind;
    /* 复制初始训练目标值。 */
    coordinator->goal_value = config->goal_value;
    /* 首个可见状态修订号为 1。 */
    coordinator->state_revision = 1U;
    /* 记录初始单调时间。 */
    coordinator->last_monotonic_ms = monotonic_ms;
    /* 活动记账点从初始时刻开始。 */
    coordinator->last_active_accounted_ms = monotonic_ms;
    /* 初始化成功。 */
    return DEVICE_COORDINATOR_OK;
}

device_coordinator_status_t device_coordinator_handle_control(
    device_coordinator_t *coordinator,
    const device_control_t control,
    const uint64_t monotonic_ms,
    device_effects_t *effects)
{
    /* 所有指针和枚举必须有效。 */
    if ((coordinator == NULL) || (effects == NULL) ||
        (control >= DEVICE_CONTROL_COUNT)) {
        /* 可写 effect 存在时先清空。 */
        if (effects != NULL) {
            /* 清空输出。 */
            device_effects_clear(effects);
        }
        /* 返回参数错误。 */
        return DEVICE_COORDINATOR_ERR_ARGUMENT;
    }
    /* 每次调用先清空输出。 */
    device_effects_clear(effects);
    /* 单调时间倒退时状态不变。 */
    if (!device_time_is_valid(coordinator, monotonic_ms)) {
        /* 返回时间错误。 */
        return DEVICE_COORDINATOR_ERR_TIME;
    }
    /* 在局部副本上执行，任一下游错误都不污染原状态。 */
    device_coordinator_t candidate = *coordinator;
    /* 累加本次前的活动时间并保存全局时间。 */
    device_advance_active_elapsed(&candidate, monotonic_ms);
    /* 保存本次处理状态。 */
    device_coordinator_status_t status = DEVICE_COORDINATOR_OK;

    /* 按统一控制语义处理。 */
    switch (control) {
        case DEVICE_CONTROL_START: {
            /* 只允许 Idle/Home 新建会话。 */
            if ((candidate.workout.state != WORKOUT_STATE_IDLE) ||
                (candidate.ui.state != UI_STATE_HOME)) {
                /* 拒绝覆盖当前会话。 */
                return DEVICE_COORDINATOR_ERR_STATE;
            }
            /* 未知或非充电且不高于 8% 时禁止启动。 */
            if (!power_manager_can_start_session(&candidate.power)) {
                /* 返回低电错误且不修改状态。 */
                return DEVICE_COORDINATOR_ERR_LOW_BATTERY;
            }
            /* 使用持久化下一序号开始准备。 */
            const workout_status_t workout_status = workout_engine_start(
                &candidate.workout,
                candidate.next_session_seq,
                candidate.weight_g,
                monotonic_ms);
            /* 开始失败时不提交副本。 */
            if (workout_status != WORKOUT_STATUS_OK) {
                /* 转换错误。 */
                return device_map_workout_status(workout_status);
            }
            /* 重置本会话投影统计。 */
            device_reset_session_projection(&candidate, monotonic_ms);
            /* 会话序号递增，UINT32_MAX 后跳过 0。 */
            candidate.next_session_seq += 1U;
            /* 回绕到 0 时改为 1。 */
            if (candidate.next_session_seq == 0U) {
                /* 跳过无效哨兵序号。 */
                candidate.next_session_seq = 1U;
            }
            /* UI Home 进入 Prepare。 */
            status = device_dispatch_ui(
                &candidate,
                UI_EVENT_START_REQUESTED,
                monotonic_ms);
            /* UI 失败时回滚。 */
            if (status != DEVICE_COORDINATOR_OK) {
                /* 返回错误。 */
                return status;
            }
            /* 电源 Home 进入 Running，打开 25 Hz IMU。 */
            status = device_dispatch_power(
                &candidate,
                POWER_EVENT_SESSION_STARTED,
                monotonic_ms,
                false,
                true);
            /* 电源失败时回滚。 */
            if (status != DEVICE_COORDINATOR_OK) {
                /* 返回错误。 */
                return status;
            }
            /* 完成开始分支。 */
            break;
        }
        case DEVICE_CONTROL_PAUSE: {
            /* 重复暂停不产生第二次状态修订。 */
            if (candidate.workout.state == WORKOUT_STATE_PAUSED) {
                /* 返回安全忽略。 */
                return DEVICE_COORDINATOR_IGNORED;
            }
            /* 只允许已锁定运行会话暂停。 */
            if (candidate.workout.state != WORKOUT_STATE_RUNNING) {
                /* 返回状态错误。 */
                return DEVICE_COORDINATOR_ERR_STATE;
            }
            /* 暂停前同步最终可见热量和时长。 */
            status = device_sync_ui_metrics(&candidate, monotonic_ms);
            /* 同步失败时回滚。 */
            if (status != DEVICE_COORDINATOR_OK) {
                /* 返回错误。 */
                return status;
            }
            /* workout 冻结热量时间基准和不完整相位。 */
            const workout_status_t workout_status = workout_engine_pause(
                &candidate.workout,
                monotonic_ms);
            /* 下游错误时回滚。 */
            if (workout_status != WORKOUT_STATUS_OK) {
                /* 转换错误。 */
                return device_map_workout_status(workout_status);
            }
            /* UI Running 进入 Paused。 */
            status = device_dispatch_ui(
                &candidate,
                UI_EVENT_PAUSE_REQUESTED,
                monotonic_ms);
            /* UI 失败时回滚。 */
            if (status != DEVICE_COORDINATOR_OK) {
                /* 返回错误。 */
                return status;
            }
            /* 电源 Running 进入 Paused，QMI 降为 WOM。 */
            status = device_dispatch_power(
                &candidate,
                POWER_EVENT_SESSION_PAUSED,
                monotonic_ms,
                false,
                true);
            /* 电源失败时回滚。 */
            if (status != DEVICE_COORDINATOR_OK) {
                /* 返回错误。 */
                return status;
            }
            /* 完成暂停分支。 */
            break;
        }
        case DEVICE_CONTROL_RESUME: {
            /* 重复继续不修改状态。 */
            if (candidate.workout.state == WORKOUT_STATE_RUNNING) {
                /* 返回安全忽略。 */
                return DEVICE_COORDINATOR_IGNORED;
            }
            /* 只允许 Paused 恢复。 */
            if (candidate.workout.state != WORKOUT_STATE_PAUSED) {
                /* 返回状态错误。 */
                return DEVICE_COORDINATOR_ERR_STATE;
            }
            /* workout 把热量时间基准跳到当前时刻。 */
            const workout_status_t workout_status = workout_engine_resume(
                &candidate.workout,
                monotonic_ms);
            /* 恢复失败时回滚。 */
            if (workout_status != WORKOUT_STATUS_OK) {
                /* 转换错误。 */
                return device_map_workout_status(workout_status);
            }
            /* 恢复后活动记账从 now_ms 开始，不补算暂停。 */
            candidate.last_active_accounted_ms = monotonic_ms;
            /* UI Paused 进入 Running。 */
            status = device_dispatch_ui(
                &candidate,
                UI_EVENT_RESUME_REQUESTED,
                monotonic_ms);
            /* UI 失败时回滚。 */
            if (status != DEVICE_COORDINATOR_OK) {
                /* 返回错误。 */
                return status;
            }
            /* 电源 Paused 回 Running。 */
            status = device_dispatch_power(
                &candidate,
                POWER_EVENT_SESSION_RESUMED,
                monotonic_ms,
                false,
                true);
            /* 电源失败时回滚。 */
            if (status != DEVICE_COORDINATOR_OK) {
                /* 返回错误。 */
                return status;
            }
            /* 完成继续分支。 */
            break;
        }
        case DEVICE_CONTROL_STOP: {
            /* Idle/Summary 没有可再停止的会话。 */
            if ((candidate.workout.state == WORKOUT_STATE_IDLE) ||
                (candidate.workout.state == WORKOUT_STATE_SUMMARY)) {
                /* 返回状态错误。 */
                return DEVICE_COORDINATOR_ERR_STATE;
            }
            /* 停止并生成完成摘要。 */
            status = device_stop_session(
                &candidate,
                monotonic_ms,
                (uint16_t)DEVICE_SUMMARY_FLAG_COMPLETED,
                effects);
            /* 停止失败时回滚。 */
            if (status != DEVICE_COORDINATOR_OK) {
                /* 返回错误。 */
                return status;
            }
            /* 完成停止分支。 */
            break;
        }
        case DEVICE_CONTROL_RESET: {
            /* BLE command 5 只允许暂停时清零且不持久化。 */
            if (candidate.workout.state != WORKOUT_STATE_PAUSED) {
                /* 返回状态错误。 */
                return DEVICE_COORDINATOR_ERR_STATE;
            }
            /* 停止 workout 以清理领域会话。 */
            const workout_status_t stop_status = workout_engine_stop(
                &candidate.workout,
                monotonic_ms);
            /* 停止失败时回滚。 */
            if (stop_status != WORKOUT_STATUS_OK) {
                /* 转换错误。 */
                return device_map_workout_status(stop_status);
            }
            /* UI Paused 经 Summary 回 Home。 */
            status = device_dispatch_ui(
                &candidate,
                UI_EVENT_STOP_REQUESTED,
                monotonic_ms);
            /* 检查第一次转移。 */
            if (status != DEVICE_COORDINATOR_OK) {
                /* 返回错误。 */
                return status;
            }
            /* 模拟摘要已处理，实际不生成写入 effect。 */
            status = device_dispatch_ui(
                &candidate,
                UI_EVENT_SUMMARY_SAVED,
                monotonic_ms);
            /* 检查回主页。 */
            if (status != DEVICE_COORDINATOR_OK) {
                /* 返回错误。 */
                return status;
            }
            /* 电源回主页。 */
            status = device_dispatch_power(
                &candidate,
                POWER_EVENT_SESSION_STOPPED,
                monotonic_ms,
                false,
                true);
            /* 电源失败时回滚。 */
            if (status != DEVICE_COORDINATOR_OK) {
                /* 返回错误。 */
                return status;
            }
            /* 清空 workout 到 Idle。 */
            workout_engine_return_idle(&candidate.workout);
            /* 完成重置分支。 */
            break;
        }
        case DEVICE_CONTROL_DONE: {
            /* 只允许摘要页在存储成功后回空闲。 */
            if ((candidate.workout.state != WORKOUT_STATE_SUMMARY) ||
                (candidate.ui.state != UI_STATE_SUMMARY)) {
                /* 返回状态错误。 */
                return DEVICE_COORDINATOR_ERR_STATE;
            }
            /* UI Summary 回 Home。 */
            status = device_dispatch_ui(
                &candidate,
                UI_EVENT_SUMMARY_SAVED,
                monotonic_ms);
            /* UI 失败时回滚。 */
            if (status != DEVICE_COORDINATOR_OK) {
                /* 返回错误。 */
                return status;
            }
            /* workout Summary 回 Idle。 */
            workout_engine_return_idle(&candidate.workout);
            /* 完成确认分支。 */
            break;
        }
        case DEVICE_CONTROL_SNAPSHOT: {
            /* 快照不改变业务状态，但会更新可见时长。 */
            status = device_sync_ui_metrics(&candidate, monotonic_ms);
            /* 快照生成失败时回滚。 */
            if (status != DEVICE_COORDINATOR_OK) {
                /* 返回错误。 */
                return status;
            }
            /* 完成快照分支。 */
            break;
        }
        case DEVICE_CONTROL_SHUTDOWN: {
            /* 有准备/运行/暂停会话时先中断停止。 */
            if ((candidate.workout.state == WORKOUT_STATE_PREPARING) ||
                (candidate.workout.state == WORKOUT_STATE_RUNNING) ||
                (candidate.workout.state == WORKOUT_STATE_PAUSED)) {
                /* 生成中断摘要；准备阶段没动作时不写入。 */
                status = device_stop_session(
                    &candidate,
                    monotonic_ms,
                    (uint16_t)DEVICE_SUMMARY_FLAG_INTERRUPTED,
                    effects);
                /* 会话停止失败时不关机，保留可恢复状态。 */
                if (status != DEVICE_COORDINATOR_OK) {
                    /* 返回错误。 */
                    return status;
                }
            }
            /* UI 从任意非关机页进入 Shutdown。 */
            status = device_dispatch_ui(
                &candidate,
                UI_EVENT_POWER_LONG_PRESS,
                monotonic_ms);
            /* UI 失败时回滚。 */
            if (status != DEVICE_COORDINATOR_OK) {
                /* 返回错误。 */
                return status;
            }
            /* 电源进入 SAFE_SHUTDOWN。 */
            status = device_dispatch_power(
                &candidate,
                POWER_EVENT_SHUTDOWN_REQUESTED,
                monotonic_ms,
                false,
                true);
            /* 电源失败时回滚。 */
            if (status != DEVICE_COORDINATOR_OK) {
                /* 返回错误。 */
                return status;
            }
            /* 标记存储同步完成后才断电。 */
            effects->flags |= DEVICE_EFFECT_SHUTDOWN_AFTER_PERSIST;
            /* 完成关机分支。 */
            break;
        }
        default:
            /* 上方范围检查已拦截，此处只作防御。 */
            return DEVICE_COORDINATOR_ERR_ARGUMENT;
    }

    /* 本次权威输出修订号递增。 */
    device_increment_revision(&candidate);
    /* 保证 UI 最终值与 workout 快照对齐。 */
    status = device_sync_ui_metrics(&candidate, monotonic_ms);
    /* 投影失败时回滚。 */
    if (status != DEVICE_COORDINATOR_OK) {
        /* 返回错误。 */
        return status;
    }
    /* 生成 UI、LiveState 和电源策略。 */
    status = device_emit_snapshots(&candidate, effects, true);
    /* effect 生成失败时回滚。 */
    if (status != DEVICE_COORDINATOR_OK) {
        /* 清空半成品 effect。 */
        device_effects_clear(effects);
        /* 返回错误。 */
        return status;
    }
    /* 全部子状态与 effect 成功后一次性提交。 */
    *coordinator = candidate;
    /* 返回成功。 */
    return DEVICE_COORDINATOR_OK;
}

/* 原子处理空闲相关 UI 与电源事件，避免只熄屏但功耗仍为 Home 或相反。 */
device_coordinator_status_t device_coordinator_handle_idle_power_event(
    device_coordinator_t *coordinator,
    const power_event_type_t event_type,
    const uint64_t monotonic_ms,
    device_effects_t *effects)
{
    /* 只接受三个冻结空闲事件，并要求状态与效果对象存在。 */
    if ((coordinator == NULL) || (effects == NULL) ||
        ((event_type != POWER_EVENT_SCREEN_TIMEOUT) &&
         (event_type != POWER_EVENT_USER_WAKE) &&
         (event_type != POWER_EVENT_LONG_IDLE))) {
        /* 可写效果存在时清零，防止调用方误用旧 flags。 */
        if (effects != NULL) {
            /* 清空全部按值效果。 */
            device_effects_clear(effects);
        }
        /* 返回参数错误。 */
        return DEVICE_COORDINATOR_ERR_ARGUMENT;
    }
    /* 每次调用先清空输出。 */
    device_effects_clear(effects);
    /* 单调时间倒退不能改变 UI、功耗或会话活动时长。 */
    if (!device_time_is_valid(coordinator, monotonic_ms)) {
        /* 返回时间错误。 */
        return DEVICE_COORDINATOR_ERR_TIME;
    }
    /* 在完整副本执行 UI 与电源两条状态链，任一失败都不提交。 */
    device_coordinator_t candidate = *coordinator;
    /* 运行或准备会话在熄屏前仍需累计到本事件时刻，暂停不会补算。 */
    device_advance_active_elapsed(&candidate, monotonic_ms);
    /* SCREEN_TIMEOUT 与 USER_WAKE 有对应 UI 事件；LONG_IDLE 只影响硬件功耗。 */
    if (event_type != POWER_EVENT_LONG_IDLE) {
        /* 构造无载荷 UI 事件。 */
        ui_event_t ui_event;
        /* 清零未使用的指标和标志。 */
        (void)memset(&ui_event, 0, sizeof(ui_event));
        /* 熄屏和唤醒枚举按明确分支映射，禁止依赖数值相等。 */
        ui_event.type = event_type == POWER_EVENT_SCREEN_TIMEOUT
            ? UI_EVENT_SCREEN_TIMEOUT
            : UI_EVENT_USER_WAKE;
        /* UI 状态机使用低 32 位单调时间记录页面进入点。 */
        ui_event.monotonic_ms = (uint32_t)monotonic_ms;
        /* 当前页面必须实际接受本事件，否则保持原 UI/power 一致状态。 */
        if (ui_dispatch_event(&candidate.ui, &ui_event) != UI_DISPATCH_OK) {
            /* 返回状态错误。 */
            return DEVICE_COORDINATOR_ERR_STATE;
        }
    }
    /* 构造同一时刻的电源事件。 */
    power_event_t power_event;
    /* 写入已验证事件类型。 */
    power_event.type = event_type;
    /* 电源状态机同样保存低 32 位单调毫秒。 */
    power_event.monotonic_ms = (uint32_t)monotonic_ms;
    /* 三个事件都不读取充电字段，固定 false 保证字节确定。 */
    power_event.charging = false;
    /* 电源状态必须真正改变，否则不提交已经改变的 UI 副本。 */
    if (!power_manager_dispatch(&candidate.power, &power_event)) {
        /* 返回状态错误。 */
        return DEVICE_COORDINATOR_ERR_STATE;
    }
    /* UI/Power 联合变化形成新的权威修订。 */
    device_increment_revision(&candidate);
    /* 生成 UI、LiveState 和完整功耗策略。 */
    const device_coordinator_status_t emit_status = device_emit_snapshots(
        &candidate,
        effects,
        true);
    /* 快照生成失败时不提交候选。 */
    if (emit_status != DEVICE_COORDINATOR_OK) {
        /* 清空半成品效果。 */
        device_effects_clear(effects);
        /* 传播错误。 */
        return emit_status;
    }
    /* 所有领域合同成功后一次性提交候选。 */
    *coordinator = candidate;
    /* 返回成功。 */
    return DEVICE_COORDINATOR_OK;
}

device_coordinator_status_t device_coordinator_push_inference(
    device_coordinator_t *coordinator,
    const float logits[WORKOUT_CLASS_COUNT],
    const uint64_t monotonic_ms,
    const uint16_t quality_flags,
    device_effects_t *effects)
{
    /* 必填指针必须有效。 */
    if ((coordinator == NULL) || (logits == NULL) || (effects == NULL)) {
        /* 可写 effect 存在时清空。 */
        if (effects != NULL) {
            /* 清空输出。 */
            device_effects_clear(effects);
        }
        /* 返回参数错误。 */
        return DEVICE_COORDINATOR_ERR_ARGUMENT;
    }
    /* 先清空 effect。 */
    device_effects_clear(effects);
    /* 只允许准备或运行状态接收推理。 */
    if ((coordinator->workout.state != WORKOUT_STATE_PREPARING) &&
        (coordinator->workout.state != WORKOUT_STATE_RUNNING)) {
        /* 返回状态错误。 */
        return DEVICE_COORDINATOR_ERR_STATE;
    }
    /* 时间必须单调不倒退。 */
    if (!device_time_is_valid(coordinator, monotonic_ms)) {
        /* 返回时间错误。 */
        return DEVICE_COORDINATOR_ERR_TIME;
    }
    /* 修改前检查全部 11 维为有限值。 */
    for (uint8_t class_index = 0U; class_index < WORKOUT_CLASS_COUNT; ++class_index) {
        /* NaN/Inf 不允许进入累计器。 */
        if (!isfinite(logits[class_index])) {
            /* 返回参数错误且不修改状态。 */
            return DEVICE_COORDINATOR_ERR_ARGUMENT;
        }
    }
    /* 使用局部副本实现错误回滚。 */
    device_coordinator_t candidate = *coordinator;
    /* 推进排除暂停的活动时长。 */
    device_advance_active_elapsed(&candidate, monotonic_ms);
    /* 保存本次是否完成动作锁定。 */
    bool action_locked = false;
    /* 调用唯一推理锁类状态机。 */
    const workout_status_t workout_status = workout_engine_push_inference(
        &candidate.workout,
        logits,
        monotonic_ms,
        quality_flags,
        &action_locked);
    /* 错误时不提交副本。 */
    if ((workout_status != WORKOUT_STATUS_OK) &&
        (workout_status != WORKOUT_STATUS_IGNORED)) {
        /* 转换错误。 */
        return device_map_workout_status(workout_status);
    }
    /* 准备证据即使尚未锁定也必须提交，但不发 2 Hz 之外的 UI/BLE。 */
    if (!action_locked && (candidate.workout.state == WORKOUT_STATE_PREPARING)) {
        /* 提交候选证据和时间。 */
        *coordinator = candidate;
        /* 返回成功，表示该窗口已被吸收。 */
        return DEVICE_COORDINATOR_OK;
    }
    /* 本次锁定时 UI Prepare 进入 Running。 */
    if (action_locked) {
        /* 分派准备完成事件。 */
        const device_coordinator_status_t ui_status = device_dispatch_ui(
            &candidate,
            UI_EVENT_PREPARE_COMPLETED,
            monotonic_ms);
        /* UI 不一致时回滚。 */
        if (ui_status != DEVICE_COORDINATOR_OK) {
            /* 返回错误。 */
            return ui_status;
        }
    }
    /* 锁类补算可能一次形成多条权威事件；必须在生成 LiveState 前逐条吸收并保留原时刻。 */
    const device_coordinator_status_t replay_event_status =
        device_drain_replay_metric_events(&candidate, effects);
    /* 事件序号、会话或容量异常时回滚整次候选。 */
    if (replay_event_status != DEVICE_COORDINATOR_OK) {
        /* 清空半成品输出。 */
        device_effects_clear(effects);
        /* 返回真实错误。 */
        return replay_event_status;
    }
    /* 已锁定或运行置信度变化时递增修订号。 */
    device_increment_revision(&candidate);
    /* 把新动作、置信度和当前指标投影到 UI。 */
    const device_coordinator_status_t ui_status = device_sync_ui_metrics(
        &candidate,
        monotonic_ms);
    /* 投影失败时回滚。 */
    if (ui_status != DEVICE_COORDINATOR_OK) {
        /* 返回错误。 */
        return ui_status;
    }
    /* 生成 UI 和 LiveState；锁定不改变电源状态。 */
    const device_coordinator_status_t effect_status = device_emit_snapshots(
        &candidate,
        effects,
        false);
    /* effect 生成失败时回滚。 */
    if (effect_status != DEVICE_COORDINATOR_OK) {
        /* 清空半成品。 */
        device_effects_clear(effects);
        /* 返回错误。 */
        return effect_status;
    }
    /* 补算事件已改变权威次数和 last_event_seq，必须同时交付幂等摘要写入。 */
    if (effects->replay_metric_event_count > 0U) {
        /* 从已吸收全部事件的候选构造最新摘要。 */
        const device_coordinator_status_t summary_status = device_build_summary(
            &candidate,
            0U,
            &effects->summary);
        /* 摘要构造失败时回滚整次锁类。 */
        if (summary_status != DEVICE_COORDINATOR_OK) {
            /* 清空半成品。 */
            device_effects_clear(effects);
            /* 返回错误。 */
            return summary_status;
        }
        /* 请求存储任务幂等写入最新累计。 */
        effects->flags |= DEVICE_EFFECT_SUMMARY_WRITE;
    }
    /* 一次性提交。 */
    *coordinator = candidate;
    /* 返回成功。 */
    return DEVICE_COORDINATOR_OK;
}

device_coordinator_status_t device_coordinator_push_sample(
    device_coordinator_t *coordinator,
    const motion_phase_sample_t *sample,
    const bool count_input_valid,
    const uint16_t quality_flags,
    device_effects_t *effects)
{
    /* 必填指针必须有效。 */
    if ((coordinator == NULL) || (sample == NULL) || (effects == NULL)) {
        /* 可写 effect 存在时清空。 */
        if (effects != NULL) {
            /* 清空输出。 */
            device_effects_clear(effects);
        }
        /* 返回参数错误。 */
        return DEVICE_COORDINATOR_ERR_ARGUMENT;
    }
    /* 先清空 effect。 */
    device_effects_clear(effects);
    /* PREPARING 需要缓存首窗样本；只有暂停、空闲和总结才不处理 25 Hz 点。 */
    if ((coordinator->workout.state != WORKOUT_STATE_PREPARING) &&
        (coordinator->workout.state != WORKOUT_STATE_RUNNING)) {
        /* 返回安全忽略。 */
        return DEVICE_COORDINATOR_IGNORED;
    }
    /* 样本时间必须单调不倒退。 */
    if (!device_time_is_valid(coordinator, sample->monotonic_ms)) {
        /* 返回时间错误。 */
        return DEVICE_COORDINATOR_ERR_TIME;
    }
    /* 修改前检查 gx,gy,gz,ax,ay,az 全部为有限物理值。 */
    for (uint8_t axis_index = 0U; axis_index < FITNESS_IMU_AXIS_COUNT; ++axis_index) {
        /* NaN/Inf 不能进入热量或相位状态。 */
        if (!isfinite(sample->axis[axis_index])) {
            /* 返回参数错误且不修改状态。 */
            return DEVICE_COORDINATOR_ERR_ARGUMENT;
        }
    }
    /* 保存调用前活动门；无 MetricEvent 的休息/恢复边界仍需立即刷新 UI 和 BLE。 */
    const bool previous_counting_enabled =
        coordinator->workout.classification_consistent;
    /* 使用局部副本实现事务提交。 */
    device_coordinator_t candidate = *coordinator;
    /* 推进会话活动时长。 */
    device_advance_active_elapsed(&candidate, sample->monotonic_ms);
    /* 为唯一 MetricEvent 预留按值输出。 */
    fitness_metric_event_t event;
    /* 清零未产生时的输出。 */
    (void)memset(&event, 0, sizeof(event));
    /* 保存本点是否产生新指标事实。 */
    bool emitted = false;
    /* 调用唯一计数、步数和热量领域引擎。 */
    const workout_status_t workout_status = workout_engine_push_sample(
        &candidate.workout,
        sample,
        count_input_valid,
        quality_flags,
        &event,
        &emitted);
    /* PREPARING 返回 IGNORED 表示点已进入补算缓存，不是错误。 */
    if ((workout_status != WORKOUT_STATUS_OK) &&
        (workout_status != WORKOUT_STATUS_IGNORED)) {
        /* 转换错误。 */
        return device_map_workout_status(workout_status);
    }
    /* 准备期样本只提交环形缓存和活动时间，不在 25 Hz 频率刷新 UI/BLE。 */
    if ((workout_status == WORKOUT_STATUS_IGNORED) &&
        (candidate.workout.state == WORKOUT_STATE_PREPARING)) {
        /* 原子提交已验证的准备点。 */
        *coordinator = candidate;
        /* 返回成功，主入口可继续分发空 effect。 */
        return DEVICE_COORDINATOR_OK;
    }
    /* 判断本点是否使训练同源活动门开关发生变化。 */
    const bool counting_state_changed =
        candidate.workout.classification_consistent != previous_counting_enabled;
    /* 没有 MetricEvent 且活动门未变化时只提交领域时间/热量，不以 25 Hz 刷 UI/BLE。 */
    if (!emitted) {
        /* 休息进入或恢复是可见状态变化，必须立即修订 UI/LiveState，但不能伪造次数事件。 */
        if (counting_state_changed) {
            /* 为活动门边界分配新的单调状态修订号。 */
            device_increment_revision(&candidate);
            /* 从同一 workout 快照同步主动作、累计值和 counting_enabled。 */
            const device_coordinator_status_t ui_status = device_sync_ui_metrics(
                &candidate,
                sample->monotonic_ms);
            /* UI 状态机拒绝时回滚整点，避免 UI/BLE 状态分叉。 */
            if (ui_status != DEVICE_COORDINATOR_OK) {
                /* 清空半成品 effect。 */
                device_effects_clear(effects);
                /* 返回精确错误。 */
                return ui_status;
            }
            /* 生成同一修订的 UI 和 BLE LiveState；活动门不改变功耗策略。 */
            const device_coordinator_status_t snapshot_status = device_emit_snapshots(
                &candidate,
                effects,
                false);
            /* 任一快照失败都不提交候选状态。 */
            if (snapshot_status != DEVICE_COORDINATOR_OK) {
                /* 清空半成品，调用方不会看到只有一侧的状态。 */
                device_effects_clear(effects);
                /* 返回精确错误。 */
                return snapshot_status;
            }
        }
        /* 提交内部状态。 */
        *coordinator = candidate;
        /* 返回成功。 */
        return DEVICE_COORDINATOR_OK;
    }
    /* 吸收事件幂等键和稳定度摘要。 */
    const device_coordinator_status_t absorb_status = device_absorb_metric_event(
        &candidate,
        &event);
    /* 事实链不一致时回滚。 */
    if (absorb_status != DEVICE_COORDINATOR_OK) {
        /* 返回错误。 */
        return absorb_status;
    }
    /* 新 MetricEvent 是权威状态修订。 */
    device_increment_revision(&candidate);
    /* 从领域快照投影 UI，不使用 delta 再加一次。 */
    const device_coordinator_status_t ui_status = device_sync_ui_metrics(
        &candidate,
        sample->monotonic_ms);
    /* UI 投影失败时回滚。 */
    if (ui_status != DEVICE_COORDINATOR_OK) {
        /* 返回错误。 */
        return ui_status;
    }
    /* Event 按值拷贝原始 MetricEvent，PC 只用于动画。 */
    effects->metric_event = event;
    /* 标记 BLE Event 有效。 */
    effects->flags |= DEVICE_EFFECT_BLE_EVENT;
    /* 生成同一事件修订对应的幂等摘要。 */
    const device_coordinator_status_t summary_status = device_build_summary(
        &candidate,
        0U,
        &effects->summary);
    /* 摘要失败时回滚。 */
    if (summary_status != DEVICE_COORDINATOR_OK) {
        /* 清空半成品 effect。 */
        device_effects_clear(effects);
        /* 返回错误。 */
        return summary_status;
    }
    /* 标记存储更新 effect 有效。 */
    effects->flags |= DEVICE_EFFECT_SUMMARY_WRITE;
    /* 生成 UI 和 LiveState，本点不改电源策略。 */
    const device_coordinator_status_t effect_status = device_emit_snapshots(
        &candidate,
        effects,
        false);
    /* effect 生成失败时回滚。 */
    if (effect_status != DEVICE_COORDINATOR_OK) {
        /* 清空半成品。 */
        device_effects_clear(effects);
        /* 返回错误。 */
        return effect_status;
    }
    /* 全部投影与摘要完成后一次性提交状态。 */
    *coordinator = candidate;
    /* 返回成功。 */
    return DEVICE_COORDINATOR_OK;
}

device_coordinator_status_t device_coordinator_update_battery(
    device_coordinator_t *coordinator,
    const uint8_t battery_percent,
    const bool charging,
    const uint64_t monotonic_ms,
    device_effects_t *effects)
{
    /* 指针和百分比必须有效。 */
    if ((coordinator == NULL) || (effects == NULL) || (battery_percent > 100U)) {
        /* 可写 effect 存在时清空。 */
        if (effects != NULL) {
            /* 清空输出。 */
            device_effects_clear(effects);
        }
        /* 返回参数错误。 */
        return DEVICE_COORDINATOR_ERR_ARGUMENT;
    }
    /* 先清空 effect。 */
    device_effects_clear(effects);
    /* 检查单调时间。 */
    if (!device_time_is_valid(coordinator, monotonic_ms)) {
        /* 返回时间错误。 */
        return DEVICE_COORDINATOR_ERR_TIME;
    }
    /* 使用副本实现临界关机失败回滚。 */
    device_coordinator_t candidate = *coordinator;
    /* 更新会话活动时长。 */
    device_advance_active_elapsed(&candidate, monotonic_ms);
    /* 用电源组件统一分类 15/8/5% 门槛。 */
    const power_battery_level_t level = power_manager_update_battery(
        &candidate.power,
        battery_percent,
        charging);
    /* 构造 UI 电池更新事件。 */
    ui_event_t ui_battery_event;
    /* 清零未使用载荷。 */
    (void)memset(&ui_battery_event, 0, sizeof(ui_battery_event));
    /* 写入电池更新类型。 */
    ui_battery_event.type = UI_EVENT_BATTERY_UPDATED;
    /* 写入低 32 位时间。 */
    ui_battery_event.monotonic_ms = (uint32_t)monotonic_ms;
    /* 写入 0..100 百分比。 */
    ui_battery_event.percent = battery_percent;
    /* 写入充电状态。 */
    ui_battery_event.flag = charging;
    /* 电池更新是全局 UI 事件，必须成功。 */
    if (ui_dispatch_event(&candidate.ui, &ui_battery_event) != UI_DISPATCH_OK) {
        /* 返回领域错误。 */
        return DEVICE_COORDINATOR_ERR_DOMAIN;
    }
    /* 低电量只通过常驻电池图标和页面状态提示；真表没有振动马达。 */
    /* 非充电且 5% 及以下必须保存后关机。 */
    if ((level == POWER_BATTERY_CRITICAL) && !charging) {
        /* 活动会话先生成临界中断摘要。 */
        if ((candidate.workout.state == WORKOUT_STATE_PREPARING) ||
            (candidate.workout.state == WORKOUT_STATE_RUNNING) ||
            (candidate.workout.state == WORKOUT_STATE_PAUSED)) {
            /* 中断并标记临界低电。 */
            const device_coordinator_status_t stop_status = device_stop_session(
                &candidate,
                monotonic_ms,
                (uint16_t)(DEVICE_SUMMARY_FLAG_INTERRUPTED |
                           DEVICE_SUMMARY_FLAG_CRITICAL_BATTERY),
                effects);
            /* 存储摘要不能生成时拒绝关机事务。 */
            if (stop_status != DEVICE_COORDINATOR_OK) {
                /* 返回错误。 */
                return stop_status;
            }
        }
        /* UI 进入临界低电关机页。 */
        const device_coordinator_status_t ui_status = device_dispatch_ui(
            &candidate,
            UI_EVENT_CRITICAL_BATTERY,
            monotonic_ms);
        /* UI 不一致时回滚。 */
        if (ui_status != DEVICE_COORDINATOR_OK) {
            /* 返回错误。 */
            return ui_status;
        }
        /* 电源进入 SAFE_SHUTDOWN。 */
        const device_coordinator_status_t power_status = device_dispatch_power(
            &candidate,
            POWER_EVENT_CRITICAL_BATTERY,
            monotonic_ms,
            false,
            true);
        /* 电源不一致时回滚。 */
        if (power_status != DEVICE_COORDINATOR_OK) {
            /* 返回错误。 */
            return power_status;
        }
        /* 主程序必须先落盘，再执行 PMIC 关机。 */
        effects->flags |= DEVICE_EFFECT_SHUTDOWN_AFTER_PERSIST;
    }
    /* 本次电量更新产生新权威快照。 */
    device_increment_revision(&candidate);
    /* 把电量/充电和最终会话值同步到 UI。 */
    const device_coordinator_status_t sync_status = device_sync_ui_metrics(
        &candidate,
        monotonic_ms);
    /* 同步失败时回滚。 */
    if (sync_status != DEVICE_COORDINATOR_OK) {
        /* 返回错误。 */
        return sync_status;
    }
    /* 生成 UI、LiveState 和最新功耗策略。 */
    const device_coordinator_status_t effect_status = device_emit_snapshots(
        &candidate,
        effects,
        true);
    /* effect 生成失败时回滚。 */
    if (effect_status != DEVICE_COORDINATOR_OK) {
        /* 清空半成品。 */
        device_effects_clear(effects);
        /* 返回错误。 */
        return effect_status;
    }
    /* 一次性提交。 */
    *coordinator = candidate;
    /* 返回成功。 */
    return DEVICE_COORDINATOR_OK;
}

device_coordinator_status_t device_coordinator_set_ble_connected(
    device_coordinator_t *coordinator,
    const bool connected,
    const uint64_t monotonic_ms,
    device_effects_t *effects)
{
    /* 必填指针必须有效。 */
    if ((coordinator == NULL) || (effects == NULL)) {
        /* 可写 effect 存在时清空。 */
        if (effects != NULL) {
            /* 清空输出。 */
            device_effects_clear(effects);
        }
        /* 返回参数错误。 */
        return DEVICE_COORDINATOR_ERR_ARGUMENT;
    }
    /* 先清空 effect。 */
    device_effects_clear(effects);
    /* 检查单调时间。 */
    if (!device_time_is_valid(coordinator, monotonic_ms)) {
        /* 返回时间错误。 */
        return DEVICE_COORDINATOR_ERR_TIME;
    }
    /* 重复连接状态不产生新修订。 */
    if (coordinator->power.ble_connected == connected) {
        /* 返回安全忽略。 */
        return DEVICE_COORDINATOR_IGNORED;
    }
    /* 使用副本实现事务提交。 */
    device_coordinator_t candidate = *coordinator;
    /* 更新会话活动时长。 */
    device_advance_active_elapsed(&candidate, monotonic_ms);
    /* 分派电源 BLE 连接/断开事件。 */
    const device_coordinator_status_t power_status = device_dispatch_power(
        &candidate,
        connected ? POWER_EVENT_BLE_CONNECTED : POWER_EVENT_BLE_DISCONNECTED,
        monotonic_ms,
        false,
        true);
    /* 电源属性更新失败时回滚。 */
    if (power_status != DEVICE_COORDINATOR_OK) {
        /* 返回错误。 */
        return power_status;
    }
    /* 构造 UI BLE 图标事件。 */
    ui_event_t ui_event;
    /* 清零不使用载荷。 */
    (void)memset(&ui_event, 0, sizeof(ui_event));
    /* 指定 BLE 更新类型。 */
    ui_event.type = UI_EVENT_BLE_CHANGED;
    /* 写入低 32 位时间。 */
    ui_event.monotonic_ms = (uint32_t)monotonic_ms;
    /* 写入新连接状态。 */
    ui_event.flag = connected;
    /* 全局 UI 图标更新必须成功。 */
    if (ui_dispatch_event(&candidate.ui, &ui_event) != UI_DISPATCH_OK) {
        /* 返回领域错误。 */
        return DEVICE_COORDINATOR_ERR_DOMAIN;
    }
    /* 连接属性产生新权威修订。 */
    device_increment_revision(&candidate);
    /* 生成 UI、LiveState 和按连接状态选择的电源策略。 */
    const device_coordinator_status_t effect_status = device_emit_snapshots(
        &candidate,
        effects,
        true);
    /* effect 生成失败时回滚。 */
    if (effect_status != DEVICE_COORDINATOR_OK) {
        /* 清空半成品。 */
        device_effects_clear(effects);
        /* 返回错误。 */
        return effect_status;
    }
    /* 一次性提交。 */
    *coordinator = candidate;
    /* 返回成功。 */
    return DEVICE_COORDINATOR_OK;
}

device_coordinator_status_t device_coordinator_set_next_session_weight(
    device_coordinator_t *coordinator,
    const uint32_t weight_g,
    const uint64_t monotonic_ms,
    device_effects_t *effects)
{
    /* 必填状态和 effect 必须存在。 */
    if ((coordinator == NULL) || (effects == NULL)) {
        /* 可写 effect 存在时先清空，避免调用者误用旧标志。 */
        if (effects != NULL) {
            /* 清空全部输出。 */
            device_effects_clear(effects);
        }
        /* 返回参数错误。 */
        return DEVICE_COORDINATOR_ERR_ARGUMENT;
    }
    /* 每次调用先清空输出。 */
    device_effects_clear(effects);
    /* BLE、设备页和训练核心统一只允许 30～250 kg。 */
    if ((weight_g < DEVICE_CONFIG_WEIGHT_MIN_G) ||
        (weight_g > DEVICE_CONFIG_WEIGHT_MAX_G)) {
        /* 越界值不修改状态。 */
        return DEVICE_COORDINATOR_ERR_ARGUMENT;
    }
    /* 配置更新仍必须遵守全局单调时间。 */
    if (!device_time_is_valid(coordinator, monotonic_ms)) {
        /* 倒退时间不修改状态。 */
        return DEVICE_COORDINATOR_ERR_TIME;
    }
    /* 在局部副本执行，任一投影错误都可完整回滚。 */
    device_coordinator_t candidate = *coordinator;
    /* 先把当前运行区间记入活动时长。 */
    device_advance_active_elapsed(&candidate, monotonic_ms);
    /* 只更新协调器的下次会话值；workout.weight_g 保持本会话启动值。 */
    candidate.weight_g = weight_g;
    /* 配置改变产生新的权威修订号。 */
    device_increment_revision(&candidate);
    /* 同步 UI 快照，但不重新计算任何次数或热量。 */
    const device_coordinator_status_t ui_status = device_sync_ui_metrics(
        &candidate,
        monotonic_ms);
    /* UI 领域失败时不提交体重。 */
    if (ui_status != DEVICE_COORDINATOR_OK) {
        /* 返回具体错误。 */
        return ui_status;
    }
    /* 生成 UI 与 LiveState；体重变化不要求重配电源。 */
    const device_coordinator_status_t effect_status = device_emit_snapshots(
        &candidate,
        effects,
        false);
    /* effect 失败时清空输出并回滚。 */
    if (effect_status != DEVICE_COORDINATOR_OK) {
        /* 清空半成品。 */
        device_effects_clear(effects);
        /* 返回错误。 */
        return effect_status;
    }
    /* 全部步骤成功后一次性提交。 */
    *coordinator = candidate;
    /* 返回成功。 */
    return DEVICE_COORDINATOR_OK;
}

device_coordinator_status_t device_coordinator_set_goal(
    device_coordinator_t *coordinator,
    const uint8_t goal_kind,
    const uint32_t goal_value,
    const uint64_t monotonic_ms,
    device_effects_t *effects)
{
    /* 必填状态和 effect 必须存在。 */
    if ((coordinator == NULL) || (effects == NULL)) {
        /* 可写 effect 存在时清空旧输出。 */
        if (effects != NULL) {
            /* 清空全部输出字段。 */
            device_effects_clear(effects);
        }
        /* 返回参数错误。 */
        return DEVICE_COORDINATOR_ERR_ARGUMENT;
    }
    /* 每次调用先清空输出。 */
    device_effects_clear(effects);
    /* 复用配置组件唯一的 kind/value 组合规则。 */
    if (device_config_validate_goal(goal_kind, goal_value) != DEVICE_CONFIG_OK) {
        /* 非法目标不修改状态。 */
        return DEVICE_COORDINATOR_ERR_ARGUMENT;
    }
    /* 目标更新必须遵守单调时间。 */
    if (!device_time_is_valid(coordinator, monotonic_ms)) {
        /* 倒退时间不修改状态。 */
        return DEVICE_COORDINATOR_ERR_TIME;
    }
    /* 在局部副本计算当前活动时长和进度。 */
    device_coordinator_t candidate = *coordinator;
    /* 记入本次命令前的运行区间，秒目标立即反映最新时间。 */
    device_advance_active_elapsed(&candidate, monotonic_ms);
    /* 保存跨会话目标种类。 */
    candidate.goal_kind = goal_kind;
    /* 保存目标值及其次数/秒/mcal 单位。 */
    candidate.goal_value = goal_value;
    /* 目标改变产生新的权威修订号。 */
    device_increment_revision(&candidate);
    /* 同步 UI 权威快照。 */
    const device_coordinator_status_t ui_status = device_sync_ui_metrics(
        &candidate,
        monotonic_ms);
    /* UI 失败时回滚目标。 */
    if (ui_status != DEVICE_COORDINATOR_OK) {
        /* 返回错误。 */
        return ui_status;
    }
    /* 生成包含最新 goal_percent 的 UI 和 LiveState。 */
    const device_coordinator_status_t effect_status = device_emit_snapshots(
        &candidate,
        effects,
        false);
    /* effect 失败时清空输出并回滚。 */
    if (effect_status != DEVICE_COORDINATOR_OK) {
        /* 清空半成品。 */
        device_effects_clear(effects);
        /* 返回错误。 */
        return effect_status;
    }
    /* 成功后提交目标和时间。 */
    *coordinator = candidate;
    /* 返回成功。 */
    return DEVICE_COORDINATOR_OK;
}

bool device_coordinator_control_from_ui(
    const ui_event_type_t event,
    device_control_t *control)
{
    /* 输出指针必须有效。 */
    if (control == NULL) {
        /* 无法返回映射。 */
        return false;
    }
    /* 只映射真正修改会话的 UI 事件。 */
    switch (event) {
        /* 开始按钮。 */
        case UI_EVENT_START_REQUESTED: *control = DEVICE_CONTROL_START; return true;
        /* 暂停按钮。 */
        case UI_EVENT_PAUSE_REQUESTED: *control = DEVICE_CONTROL_PAUSE; return true;
        /* 继续按钮。 */
        case UI_EVENT_RESUME_REQUESTED: *control = DEVICE_CONTROL_RESUME; return true;
        /* 停止按钮。 */
        case UI_EVENT_STOP_REQUESTED: *control = DEVICE_CONTROL_STOP; return true;
        /* 存储确认。 */
        case UI_EVENT_SUMMARY_SAVED: *control = DEVICE_CONTROL_DONE; return true;
        /* 长按电源键。 */
        case UI_EVENT_POWER_LONG_PRESS: *control = DEVICE_CONTROL_SHUTDOWN; return true;
        /* 其它 UI 事件不是协调器会话命令。 */
        default: return false;
    }
}

bool device_coordinator_control_from_ble(
    const uint8_t command_id,
    device_control_t *control)
{
    /* 输出指针必须有效。 */
    if (control == NULL) {
        /* 无法返回映射。 */
        return false;
    }
    /* 映射 BLE command 1..5 和 10，其它命令留给设置/传输组件。 */
    switch (command_id) {
        /* command 1 开始会话。 */
        case BLE_SERVICE_COMMAND_START_SESSION: *control = DEVICE_CONTROL_START; return true;
        /* command 2 暂停会话。 */
        case BLE_SERVICE_COMMAND_PAUSE_SESSION: *control = DEVICE_CONTROL_PAUSE; return true;
        /* command 3 继续会话。 */
        case BLE_SERVICE_COMMAND_RESUME_SESSION: *control = DEVICE_CONTROL_RESUME; return true;
        /* command 4 停止并保存。 */
        case BLE_SERVICE_COMMAND_STOP_SESSION: *control = DEVICE_CONTROL_STOP; return true;
        /* command 5 仅暂停时丢弃会话。 */
        case BLE_SERVICE_COMMAND_RESET_SESSION: *control = DEVICE_CONTROL_RESET; return true;
        /* command 10 立即快照。 */
        case BLE_SERVICE_COMMAND_GET_SNAPSHOT: *control = DEVICE_CONTROL_SNAPSHOT; return true;
        /* 其它命令不属于会话控制。 */
        default: return false;
    }
}
