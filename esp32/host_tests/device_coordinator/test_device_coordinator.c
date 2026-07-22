/* 引入被测产品应用协调器。 */
#include "device_coordinator.h"

/* 引入 NAN 构造必须无状态突变的错误输入。 */
#include <math.h>
/* 引入 printf/fprintf 输出机器可读测试摘要。 */
#include <stdio.h>
/* 引入 memcmp/memset 验证错误事务没有状态突变。 */
#include <string.h>

/* 保存已执行断言数量。 */
static unsigned int g_assertions = 0U;

/* 断言失败时输出行号和表达式并结束当前测试。 */
#define CHECK(expression)                                                       \
    do {                                                                        \
        g_assertions += 1U;                                                      \
        if (!(expression)) {                                                    \
            (void)fprintf(stderr, "CHECK failed line=%d: %s\n", __LINE__, #expression); \
            return 1;                                                           \
        }                                                                       \
    } while (0)

/* 构造 11 维高置信 logits；目标类 +4，其它类 -4。 */
static void make_logits(float logits[WORKOUT_CLASS_COUNT], const uint8_t target)
{
    /* 遍历全部动作类。 */
    for (uint8_t class_index = 0U; class_index < WORKOUT_CLASS_COUNT; ++class_index) {
        /* 写入目标高分或非目标低分。 */
        logits[class_index] = class_index == target ? 4.0F : -4.0F;
    }
}

/* 构造单点六轴样本；gx 单位 deg/s，az 单位 g。 */
static motion_phase_sample_t make_sample(
    const uint64_t time_ms,
    const float gyro_x_dps,
    const float acceleration_z_g)
{
    /* 创建六轴点。 */
    motion_phase_sample_t sample;
    /* 未使用轴固定为 0。 */
    (void)memset(&sample, 0, sizeof(sample));
    /* 写入单调毫秒。 */
    sample.monotonic_ms = time_ms;
    /* 写入 gx 角速度。 */
    sample.axis[0] = gyro_x_dps;
    /* 写入 az 加速度。 */
    sample.axis[5] = acceleration_z_g;
    /* 返回按值样本。 */
    return sample;
}

/* 初始化就绪设备并写入 100% 厂家原配电池状态。 */
static int initialize_ready(
    device_coordinator_t *coordinator,
    const uint32_t next_session_seq,
    const uint64_t base_ms)
{
    /* 配置 70 kg 用户和新会话序号。 */
    const device_coordinator_config_t config = {
        .next_session_seq = next_session_seq,
        .weight_g = 70000U,
        .allow_imu_deep_wake = false,
    };
    /* 自检成功后初始化到 Home。 */
    CHECK(device_coordinator_init(coordinator, &config, base_ms) ==
          DEVICE_COORDINATOR_OK);
    /* 检查 UI 已就绪。 */
    CHECK(coordinator->ui.state == UI_STATE_HOME);
    /* 检查电源已就绪。 */
    CHECK(coordinator->power.state == POWER_STATE_HOME);
    /* 保存电池更新 effect。 */
    device_effects_t effects;
    /* 写入满电且未充电。 */
    CHECK(device_coordinator_update_battery(
              coordinator,
              100U,
              false,
              base_ms,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 电量更新必须刷新 UI、BLE 和电源策略。 */
    CHECK((effects.flags & (DEVICE_EFFECT_UI_RENDER |
                            DEVICE_EFFECT_BLE_LIVE_STATE |
                            DEVICE_EFFECT_POWER_POLICY)) ==
          (DEVICE_EFFECT_UI_RENDER |
           DEVICE_EFFECT_BLE_LIVE_STATE |
           DEVICE_EFFECT_POWER_POLICY));
    /* 就绪成功。 */
    return 0;
}

/* 开始会话并用两个累计高置信重叠窗口锁定指定动作。 */
static int start_and_lock(
    device_coordinator_t *coordinator,
    const fitness_action_t action,
    const uint64_t start_ms,
    device_effects_t *lock_effects)
{
    /* 保存开始 effect。 */
    device_effects_t effects;
    /* 开始进入 Prepare。 */
    CHECK(device_coordinator_handle_control(
              coordinator,
              DEVICE_CONTROL_START,
              start_ms,
              &effects) == DEVICE_COORDINATOR_OK);
    /* UI 必须显示准备页。 */
    CHECK(effects.ui.state == UI_STATE_PREPARE);
    /* 电源必须开启运行策略。 */
    CHECK(effects.power_policy.imu_mode == POWER_IMU_ACTIVE_25HZ);
    /* 构造高置信 logits。 */
    float logits[WORKOUT_CLASS_COUNT];
    /* 填充目标类。 */
    make_logits(logits, (uint8_t)action);
    /* 首个可信窗口只提交累计候选，UI 继续保持 Prepare。 */
    CHECK(device_coordinator_push_inference(
              coordinator,
              logits,
              start_ms,
              0U,
              lock_effects) == DEVICE_COORDINATOR_OK);
    /* 首窗不得永久锁定动作。 */
    CHECK(coordinator->workout.state == WORKOUT_STATE_PREPARING);
    /* 第二个同类窗口位于 12 点步长后的 480 ms，完成有界累计确认。 */
    CHECK(device_coordinator_push_inference(
              coordinator,
              logits,
              start_ms + 480ULL,
              0U,
              lock_effects) == DEVICE_COORDINATOR_OK);
    /* workout 进入 Running。 */
    CHECK(coordinator->workout.state == WORKOUT_STATE_RUNNING);
    /* UI 进入 Running。 */
    CHECK(lock_effects->ui.state == UI_STATE_RUNNING);
    /* LiveState 复制目标动作。 */
    CHECK(lock_effects->live_state.action_id == (uint8_t)action);
    /* 累计锁类不产生开始振动，避免污染紧随其后的第一个计数周期。 */
    CHECK(lock_effects->haptic_count == 0U);
    /* 锁定成功。 */
    return 0;
}

/* 提交一个样本并返回 effect，方便构造相位序列。 */
static int push_point(
    device_coordinator_t *coordinator,
    const uint64_t time_ms,
    const float gyro_x_dps,
    const float acceleration_z_g,
    device_effects_t *effects)
{
    /* 构造 gx/az 样本。 */
    const motion_phase_sample_t sample = make_sample(
        time_ms,
        gyro_x_dps,
        acceleration_z_g);
    /* 提交有效计数输入。 */
    CHECK(device_coordinator_push_sample(
              coordinator,
              &sample,
              true,
              0U,
              effects) == DEVICE_COORDINATOR_OK);
    /* 样本处理成功。 */
    return 0;
}

/* 构造一次 squat 完整两相周期并返回唯一 REP effect。 */
static int emit_one_squat_rep(
    device_coordinator_t *coordinator,
    const uint64_t base_ms,
    device_effects_t *metric_effects)
{
    /* 保存中间无事件 effect。 */
    device_effects_t effects;
    /* 稳定起点。 */
    CHECK(push_point(coordinator, base_ms, 0.0F, 1.0F, &effects) == 0);
    /* 主向第一点。 */
    CHECK(push_point(coordinator, base_ms + 40ULL, 80.0F, 1.05F, &effects) == 0);
    /* 主向第二点确认 PRIMARY。 */
    CHECK(push_point(coordinator, base_ms + 80ULL, 70.0F, 1.04F, &effects) == 0);
    /* 保持非 REST 直到反向时刻。 */
    for (uint64_t time_ms = base_ms + 120ULL;
         time_ms < base_ms + 400ULL;
         time_ms += 40ULL) {
        /* 提交中间点。 */
        CHECK(push_point(coordinator, time_ms, 30.0F, 1.0F, &effects) == 0);
    }
    /* 反向第一点。 */
    CHECK(push_point(coordinator, base_ms + 400ULL, -80.0F, 1.0F, &effects) == 0);
    /* 反向第二点确认 SECONDARY。 */
    CHECK(push_point(coordinator, base_ms + 440ULL, -65.0F, 1.0F, &effects) == 0);
    /* 保持非 REST，满足最短 600 ms 深蹲周期。 */
    for (uint64_t time_ms = base_ms + 480ULL;
         time_ms < base_ms + 720ULL;
         time_ms += 40ULL) {
        /* 提交中间点。 */
        CHECK(push_point(coordinator, time_ms, 30.0F, 1.0F, &effects) == 0);
    }
    /* 第一 REST 点只建立候选。 */
    CHECK(push_point(coordinator, base_ms + 720ULL, 0.0F, 1.0F, &effects) == 0);
    /* 第二 REST 点闭合完整周期。 */
    CHECK(push_point(coordinator, base_ms + 760ULL, 0.0F, 1.0F, metric_effects) == 0);
    /* 必须产生唯一 BLE Event。 */
    CHECK((metric_effects->flags & DEVICE_EFFECT_BLE_EVENT) != 0U);
    /* 必须产生幂等摘要写入。 */
    CHECK((metric_effects->flags & DEVICE_EFFECT_SUMMARY_WRITE) != 0U);
    /* 一次深蹲事件成功。 */
    return 0;
}

/* 验证主动作只固定计数器；静坐休息动态冻结，恢复后必须从新完整周期计数。 */
static int test_dynamic_rest_freezes_primary_counter(void)
{
    /* 创建独立协调器，覆盖领域、UI、BLE LiveState 和 MetricEvent 联合事务。 */
    device_coordinator_t coordinator;
    /* 使用独立会话 40，避免与其它测试序号重叠。 */
    CHECK(initialize_ready(&coordinator, 40U, 0ULL) == 0);
    /* effects 保存每次协调器调用产生的 UI、BLE、存储和振动输出。 */
    device_effects_t effects;
    /* 两个干净高置信窗口选择深蹲作为本轮主动作和计数器类型。 */
    CHECK(start_and_lock(&coordinator, FITNESS_ACTION_SQUAT, 10ULL, &effects) == 0);
    /* 本轮主动作必须为深蹲。 */
    CHECK(coordinator.workout.selected_action == (uint8_t)FITNESS_ACTION_SQUAT);
    /* 最近实时分类与主动作一致，计数门初始打开。 */
    CHECK((coordinator.workout.inferred_action == (uint8_t)FITNESS_ACTION_SQUAT) &&
          coordinator.workout.classification_consistent);

    /* 两个正向点建立一个尚未闭合的深蹲半周期。 */
    CHECK(push_point(&coordinator, 1010ULL, 80.0F, 1.0F, &effects) == 0);
    /* 第二点确认 PRIMARY 相位，但尚未返回站立。 */
    CHECK(push_point(&coordinator, 1050ULL, 65.0F, 1.0F, &effects) == 0);
    /* 当前没有完整周期，不应产生 MetricEvent。 */
    CHECK((effects.flags & DEVICE_EFFECT_BLE_EVENT) == 0U);

    /* 构造高置信静坐窗口，代表用户在本轮深蹲过程中站定或坐下休息。 */
    float logits[WORKOUT_CLASS_COUNT];
    /* 静坐类别写高分，其它类写低分。 */
    make_logits(logits, FITNESS_ACTION_SIT);
    /* 动态分类必须被领域层吸收，而不是强制改写成深蹲。 */
    CHECK(device_coordinator_push_inference(
              &coordinator,
              logits,
              1090ULL,
              0U,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 实时类别必须公开静坐。 */
    CHECK(coordinator.workout.inferred_action == (uint8_t)FITNESS_ACTION_SIT);
    /* 休息期间必须关闭深蹲计数门。 */
    CHECK(!coordinator.workout.classification_consistent);
    /* 主动作仍为深蹲，禁止自动换成静坐时长计数器。 */
    CHECK(coordinator.workout.selected_action == (uint8_t)FITNESS_ACTION_SQUAT);
    /* 手表训练页仍保存本轮深蹲，但实时动作显示静坐。 */
    CHECK((effects.ui.view.action_id == (uint8_t)FITNESS_ACTION_SQUAT) &&
          (effects.ui.view.inferred_action_id == (uint8_t)FITNESS_ACTION_SIT));
    /* 手表必须明确关闭计数显示门。 */
    CHECK(!effects.ui.view.counting_enabled);
    /* 冻结 BLE LiveState 的 action_id 仍表示本轮指标类型，不能变成静坐秒数。 */
    CHECK(effects.live_state.action_id == (uint8_t)FITNESS_ACTION_SQUAT);

    /* 休息期间即使腕部形成反向点，也不能与休息前 PRIMARY 半周期拼接。 */
    CHECK(push_point(&coordinator, 1130ULL, -80.0F, 1.0F, &effects) == 0);
    /* 第二个反向点本来会确认 SECONDARY，但计数门已冻结。 */
    CHECK(push_point(&coordinator, 1170ULL, -65.0F, 1.0F, &effects) == 0);
    /* 两个静止点本来会返回 REST，但仍不得闭合旧半周期。 */
    CHECK(push_point(&coordinator, 1210ULL, 0.0F, 1.0F, &effects) == 0);
    /* 第二个静止点结束休息测试段。 */
    CHECK(push_point(&coordinator, 1250ULL, 0.0F, 1.0F, &effects) == 0);
    /* 休息段不得产生次数。 */
    CHECK(coordinator.workout.fitness_session.repetitions == 0ULL);
    /* 休息段不得产生 BLE MetricEvent 或振动。 */
    CHECK((effects.flags & (DEVICE_EFFECT_BLE_EVENT | DEVICE_EFFECT_HAPTIC)) == 0U);

    /* 构造新的干净高置信深蹲窗口，表示用户结束休息并继续本轮动作。 */
    make_logits(logits, FITNESS_ACTION_SQUAT);
    /* 同类干净窗口恢复原深蹲计数器，不重新选择主动作。 */
    CHECK(device_coordinator_push_inference(
              &coordinator,
              logits,
              1290ULL,
              0U,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 主动作仍为深蹲，实时动作重新一致。 */
    CHECK((coordinator.workout.selected_action == (uint8_t)FITNESS_ACTION_SQUAT) &&
          (coordinator.workout.inferred_action == (uint8_t)FITNESS_ACTION_SQUAT));
    /* 恢复窗口重新打开计数门。 */
    CHECK(coordinator.workout.classification_consistent);
    /* 手表恢复“训练中”状态。 */
    CHECK(effects.ui.view.counting_enabled);

    /* 只送休息前半周期的后半段，验证恢复边界确实清空旧相位。 */
    CHECK(push_point(&coordinator, 1330ULL, -80.0F, 1.0F, &effects) == 0);
    /* 第二个反向点仍缺少新的 PRIMARY，不能计数。 */
    CHECK(push_point(&coordinator, 1370ULL, -65.0F, 1.0F, &effects) == 0);
    /* 第一静止点不能闭合。 */
    CHECK(push_point(&coordinator, 1410ULL, 0.0F, 1.0F, &effects) == 0);
    /* 第二静止点仍不能闭合。 */
    CHECK(push_point(&coordinator, 1450ULL, 0.0F, 1.0F, &effects) == 0);
    /* 半周期拼接被拒绝，累计保持零。 */
    CHECK(coordinator.workout.fitness_session.repetitions == 0ULL);

    /* 再次进入静坐，明确结束刚才用于验证的无效后半周期。 */
    make_logits(logits, FITNESS_ACTION_SIT);
    /* 第二次休息窗口再次关闭门并清空全部未完成相位。 */
    CHECK(device_coordinator_push_inference(
              &coordinator,
              logits,
              1490ULL,
              0U,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 480 ms 后用新的深蹲窗口恢复，模拟真实重叠推理步长。 */
    make_logits(logits, FITNESS_ACTION_SQUAT);
    /* 干净同类窗口重新建立计数许可。 */
    CHECK(device_coordinator_push_inference(
              &coordinator,
              logits,
              1970ULL,
              0U,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 从下一相邻 25 Hz 点开始生成一个全新的完整深蹲周期。 */
    CHECK(emit_one_squat_rep(&coordinator, 2010ULL, &effects) == 0);
    /* 恢复后完整周期必须且只能增加一次。 */
    CHECK(coordinator.workout.fitness_session.repetitions == 1ULL);
    /* 对外 MetricEvent 与领域累计一致。 */
    CHECK(effects.metric_event.total_value == 1ULL);
    /* 测试通过。 */
    return 0;
}

/* 验证完整会话、MetricEvent 事实同值投影和摘要幂等。 */
static int test_complete_session_and_fact_chain(void)
{
    /* 创建协调器。 */
    device_coordinator_t coordinator;
    /* 以会话 41 初始化。 */
    CHECK(initialize_ready(&coordinator, 41U, 0ULL) == 0);
    /* 保存锁类 effect。 */
    device_effects_t effects;
    /* 开始并锁定 squat。 */
    CHECK(start_and_lock(&coordinator, FITNESS_ACTION_SQUAT, 10ULL, &effects) == 0);
    /* 生成第一次深蹲。 */
    CHECK(emit_one_squat_rep(&coordinator, 1010ULL, &effects) == 0);
    /* MetricEvent 是第 1 个事件且总次数为 1。 */
    CHECK((effects.metric_event.event_seq == 1U) &&
          (effects.metric_event.metric_kind == FITNESS_METRIC_REPETITION) &&
          (effects.metric_event.total_value == 1ULL));
    /* UI 只显示权威总值 1，不再根据 delta 加一次。 */
    CHECK(effects.ui.view.count == 1U);
    /* BLE LiveState 同样显示权威总值 1。 */
    CHECK(effects.live_state.metric_value == 1U);
    /* 存储摘要的权威总值同样为 1。 */
    CHECK(effects.summary.metric_total == 1ULL);
    /* Event 和摘要共用同一 event_seq 幂等键。 */
    CHECK(effects.summary.last_event_seq == effects.metric_event.event_seq);
    /* 每次 REP 必须仅有一次 30 ms 振动。 */
    CHECK((effects.haptic_count == 1U) &&
          (effects.haptics[0].reason == FITNESS_HAPTIC_REASON_REPETITION) &&
          (effects.haptics[0].on_ms == 30U) &&
          (effects.haptics[0].repeat_count == 1U));

    /* 创建固定容量内存介质，完整覆盖双槽摘要。 */
    static uint8_t storage_bytes[SESSION_STORE_REQUIRED_BACKEND_SIZE];
    /* 创建内存后端上下文。 */
    session_memory_backend_t memory_backend;
    /* 清空为 0xFF 介质。 */
    CHECK(session_memory_backend_init(
              &memory_backend,
              storage_bytes,
              sizeof(storage_bytes),
              true) == SESSION_STORE_STATUS_OK);
    /* 生成函数表。 */
    const session_store_backend_t backend = session_memory_backend_interface(
        &memory_backend);
    /* 创建会话存储。 */
    session_store_t store;
    /* 从空双槽恢复。 */
    CHECK(session_store_init(&store, &backend) == SESSION_STORE_STATUS_OK);
    /* 保存是否发生写入。 */
    bool changed = false;
    /* 第一次上送必须写入。 */
    CHECK(session_store_upsert(&store, &effects.summary, &changed) ==
          SESSION_STORE_STATUS_OK);
    /* 确认介质已变化。 */
    CHECK(changed);
    /* 记录首次写调用数。 */
    const uint32_t writes_after_first = memory_backend.successful_write_calls;
    /* 相同 session_seq+last_event_seq 重试必须幂等。 */
    CHECK(session_store_upsert(&store, &effects.summary, &changed) ==
          SESSION_STORE_STATUS_OK);
    /* 重试不应报 changed。 */
    CHECK(!changed);
    /* 重试不应再写介质。 */
    CHECK(memory_backend.successful_write_calls == writes_after_first);

    /* 停止会话并生成最终摘要。 */
    CHECK(device_coordinator_handle_control(
              &coordinator,
              DEVICE_CONTROL_STOP,
              1810ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* UI 进入总结页。 */
    CHECK(effects.ui.state == UI_STATE_SUMMARY);
    /* 最终摘要标记正常完成。 */
    CHECK((effects.summary.flags & DEVICE_SUMMARY_FLAG_COMPLETED) != 0U);
    /* 最终摘要仍保持一次。 */
    CHECK(effects.summary.metric_total == 1ULL);
    /* 结束反馈是一次 40 ms。 */
    CHECK((effects.haptic_count == 1U) &&
          (effects.haptics[0].reason == FITNESS_HAPTIC_REASON_PAUSE_OR_END) &&
          (effects.haptics[0].on_ms == 40U));
    /* 模拟存储成功确认。 */
    CHECK(device_coordinator_handle_control(
              &coordinator,
              DEVICE_CONTROL_DONE,
              1820ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* workout 回到 Idle。 */
    CHECK(coordinator.workout.state == WORKOUT_STATE_IDLE);
    /* UI 回到 Home。 */
    CHECK(coordinator.ui.state == UI_STATE_HOME);
    /* 测试通过。 */
    return 0;
}

/* 验证真实采样缺口提交重置状态，后续干净点不会永久卡在 ERR_TIME。 */
static int test_jumping_jack_gap_reset_commits_and_recovers(void)
{
    /* 创建独立协调器，避免其它会话时间和统计污染。 */
    device_coordinator_t coordinator;
    /* 使用会话 42 从满电 Home 初始化。 */
    CHECK(initialize_ready(&coordinator, 42U, 0ULL) == 0);
    /* 保存开始、锁类和采样效果。 */
    device_effects_t effects;
    /* 用两个高置信窗口锁定开合跳。 */
    CHECK(start_and_lock(&coordinator, FITNESS_ACTION_JUMPING_JACK, 10ULL, &effects) == 0);
    /* 构造首个干净六轴点，三个加速度轴均建立时间线。 */
    motion_phase_sample_t sample = make_sample(1000ULL, 0.0F, 1.0F);
    /* ax、ay 也写入有限 g 值，三轴峰谷链必须同时有效。 */
    sample.axis[3] = 0.2F;
    /* ay 使用不同幅值，防止测试依赖三个轴完全相同。 */
    sample.axis[4] = -0.1F;
    /* 首点正常提交。 */
    CHECK(device_coordinator_push_sample(
              &coordinator,
              &sample,
              true,
              0U,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 连续三个 40 ms 点模拟加速度缺口和重采样重置；无效幅值不得进入计数器。 */
    for (uint64_t time_ms = 1040ULL; time_ms <= 1120ULL; time_ms += 40ULL) {
        /* 只更新时间；幅值保持有限但 count_input_valid=false。 */
        sample.monotonic_ms = time_ms;
        /* 位 0 表示生产 IMU_QUALITY_ACCEL_GAP。 */
        CHECK(device_coordinator_push_sample(
                  &coordinator,
                  &sample,
                  false,
                  UINT16_C(1),
                  &effects) == DEVICE_COORDINATOR_OK);
    }
    /* 下一干净点距检测器上次有效点 160 ms，必须提交一次非致命周期重置。 */
    sample.monotonic_ms = 1160ULL;
    /* 缺口点不产生事件，但调用整体必须成功提交重置后的候选状态。 */
    CHECK(device_coordinator_push_sample(
              &coordinator,
              &sample,
              true,
              0U,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 重置点不增加权威次数。 */
    CHECK(coordinator.workout.fitness_session.repetitions == 0ULL);
    /* 再下一点必须建立新连续段，不能因上次重置回滚而再次 ERR_TIME。 */
    sample.monotonic_ms = 1200ULL;
    /* 正常点成功提交。 */
    CHECK(device_coordinator_push_sample(
              &coordinator,
              &sample,
              true,
              0U,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 三个轴都必须以当前点建立新的时间基准。 */
    for (uint8_t axis = 0U; axis < WORKOUT_JUMPING_JACK_AXIS_COUNT; ++axis) {
        /* 防止任一轴仍停留在缺口前 1000 ms。 */
        CHECK(coordinator.workout.jumping_jack_pair_detectors[axis].last_timestamp_ms == 1200ULL);
    }
    /* 测试通过。 */
    return 0;
}

/* 验证锁类补算的每条 MetricEvent 都进入同一次 effect，并保留原始 IMU 时刻。 */
static int test_prelock_replay_events_reach_effects(void)
{
    /* 一个开合跳周期固定 20 点、0.8 秒；X/Y 作为两个一致可信轴。 */
    static const float cycle[20] = {
        0.0F, 0.31F, 0.59F, 0.81F, 0.95F,
        1.0F, 0.95F, 0.81F, 0.59F, 0.31F,
        0.0F, -0.31F, -0.59F, -0.81F, -0.95F,
        -1.0F, -0.95F, -0.81F, -0.59F, -0.31F
    };
    /* 创建独立协调器，使用新会话 43 避免其它事件序号影响。 */
    device_coordinator_t coordinator;
    /* 从满电 Home 初始化。 */
    CHECK(initialize_ready(&coordinator, 43U, 0ULL) == 0);
    /* effects 接收开始、准备点、推理锁类及全部回放事件。 */
    device_effects_t effects;
    /* 用户点击开始后进入 PREPARING。 */
    CHECK(device_coordinator_handle_control(
              &coordinator,
              DEVICE_CONTROL_START,
              10ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* time_ms 从首个合法 40 ms 样本开始，严格晚于开始时刻。 */
    uint64_t time_ms = 40ULL;
    /* 前 20 点提供 11+5 双均值预热基线。 */
    for (uint8_t point = 0U; point < 20U; ++point) {
        /* 三轴动态量为零，只填充准备缓存。 */
        motion_phase_sample_t sample = make_sample(time_ms, 0.0F, 0.0F);
        /* ax 和 ay 显式保持零。 */
        sample.axis[3] = 0.0F;
        /* 提交 PREPARING 点，协调器必须原子保存且不发业务事件。 */
        CHECK(device_coordinator_push_sample(
                  &coordinator,
                  &sample,
                  true,
                  0U,
                  &effects) == DEVICE_COORDINATOR_OK);
        /* 准备点没有 BLE Event。 */
        CHECK((effects.flags & DEVICE_EFFECT_BLE_EVENT) == 0U);
        /* 推进一个 25 Hz 周期。 */
        time_ms += MOTION_PHASE_SAMPLE_PERIOD_MS;
    }
    /* 在锁类前缓存两个完整动作周期。 */
    for (uint8_t repetition = 0U; repetition < 2U; ++repetition) {
        /* 当前周期逐点写入三个加速度轴。 */
        for (uint8_t point = 0U; point < 20U; ++point) {
            /* 先构造零角速度、Z 小幅反相的六轴点。 */
            motion_phase_sample_t sample = make_sample(
                time_ms,
                0.0F,
                -cycle[point] * 0.15F);
            /* X 轴保存主周期，单位 g。 */
            sample.axis[3] = cycle[point];
            /* Y 轴保存 0.9 倍同相周期，单位 g。 */
            sample.axis[4] = cycle[point] * 0.9F;
            /* 准备态只缓存，不允许提前产生 Event effect。 */
            CHECK(device_coordinator_push_sample(
                      &coordinator,
                      &sample,
                      true,
                      0U,
                      &effects) == DEVICE_COORDINATOR_OK);
            /* 当前点仍没有 BLE Event。 */
            CHECK((effects.flags & DEVICE_EFFECT_BLE_EVENT) == 0U);
            /* 推进一个采样周期。 */
            time_ms += MOTION_PHASE_SAMPLE_PERIOD_MS;
        }
    }
    /* 追加十个上升点，使滤波延迟确认第二个波谷。 */
    for (uint8_t point = 0U; point < 10U; ++point) {
        /* 构造下一周期前半段。 */
        motion_phase_sample_t sample = make_sample(
            time_ms,
            0.0F,
            -cycle[point] * 0.15F);
        /* 写入 X 轴主周期。 */
        sample.axis[3] = cycle[point];
        /* 写入 Y 轴同相周期。 */
        sample.axis[4] = cycle[point] * 0.9F;
        /* 尾部点进入准备缓存。 */
        CHECK(device_coordinator_push_sample(
                  &coordinator,
                  &sample,
                  true,
                  0U,
                  &effects) == DEVICE_COORDINATOR_OK);
        /* 锁类前仍不发布。 */
        CHECK((effects.flags & DEVICE_EFFECT_BLE_EVENT) == 0U);
        /* 推进时间。 */
        time_ms += MOTION_PHASE_SAMPLE_PERIOD_MS;
    }

    /* 构造开合跳高置信 logits。 */
    float logits[WORKOUT_CLASS_COUNT];
    /* 目标类固定为开合跳。 */
    make_logits(logits, (uint8_t)FITNESS_ACTION_JUMPING_JACK);
    /* 第一窗只累计候选，不触发锁定。 */
    CHECK(device_coordinator_push_inference(
              &coordinator,
              logits,
              time_ms,
              0U,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 第一窗后仍处于准备态。 */
    CHECK(coordinator.workout.state == WORKOUT_STATE_PREPARING);
    /* 第二窗完成锁定并回放准备缓存。 */
    CHECK(device_coordinator_push_inference(
              &coordinator,
              logits,
              time_ms + 480ULL,
              0U,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 锁类 effect 必须同时含两条回放 Event、LiveState 和摘要写入。 */
    CHECK((effects.flags & DEVICE_EFFECT_BLE_EVENT) != 0U);
    /* 回放事件数量必须精确为二。 */
    CHECK(effects.replay_metric_event_count == 2U);
    /* 第一条事件保留序号一和累计一。 */
    CHECK((effects.replay_metric_events[0].event_seq == 1U) &&
          (effects.replay_metric_events[0].total_value == 1ULL));
    /* 第二条事件保留序号二和累计二。 */
    CHECK((effects.replay_metric_events[1].event_seq == 2U) &&
          (effects.replay_metric_events[1].total_value == 2ULL));
    /* 两个历史时刻严格递增且早于锁类完成时刻。 */
    CHECK((effects.replay_metric_events[0].monotonic_ms <
           effects.replay_metric_events[1].monotonic_ms) &&
          (effects.replay_metric_events[1].monotonic_ms < time_ms + 480ULL));
    /* 协调器摘要和 LiveState 必须与最后一条事件一致。 */
    CHECK((coordinator.last_event_seq == 2U) &&
          (effects.live_state.metric_value == 2U) &&
          (effects.summary.metric_total == 2ULL) &&
          (effects.summary.last_event_seq == 2U));
    /* 测试通过。 */
    return 0;
}

/* 验证 8% 禁止启动，充电时可放行，失败不改状态。 */
static int test_low_battery_start_gate(void)
{
    /* 初始化就绪设备。 */
    device_coordinator_t coordinator;
    /* 使用会话 50。 */
    CHECK(initialize_ready(&coordinator, 50U, 0ULL) == 0);
    /* 保存 effect。 */
    device_effects_t effects;
    /* 先下降到 15% 验证低电提醒。 */
    CHECK(device_coordinator_update_battery(
              &coordinator,
              15U,
              false,
              5ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 15% 第一次进入产生两次 40 ms 低电振动。 */
    CHECK((effects.haptic_count == 1U) &&
          (effects.haptics[0].reason == FITNESS_HAPTIC_REASON_LOW_BATTERY) &&
          (effects.haptics[0].on_ms == 40U) &&
          (effects.haptics[0].repeat_count == 2U));
    /* 继续下降到 14% 不得重复提醒。 */
    CHECK(device_coordinator_update_battery(
              &coordinator,
              14U,
              false,
              6ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 同一低电区间只提醒一次。 */
    CHECK((effects.flags & DEVICE_EFFECT_HAPTIC) == 0U);
    /* 把电量更新到 8% 且未充电。 */
    CHECK(device_coordinator_update_battery(
              &coordinator,
              8U,
              false,
              10ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 保存失败前完整状态。 */
    const device_coordinator_t before = coordinator;
    /* 尝试开始必须返回低电错误。 */
    CHECK(device_coordinator_handle_control(
              &coordinator,
              DEVICE_CONTROL_START,
              20ULL,
              &effects) == DEVICE_COORDINATOR_ERR_LOW_BATTERY);
    /* 失败后状态逐字节不变。 */
    CHECK(memcmp(&before, &coordinator, sizeof(coordinator)) == 0);
    /* 失败后不产生副作用。 */
    CHECK(effects.flags == 0U);
    /* 同为 8% 但接入充电。 */
    CHECK(device_coordinator_update_battery(
              &coordinator,
              8U,
              true,
              30ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 充电时允许新会话。 */
    CHECK(device_coordinator_handle_control(
              &coordinator,
              DEVICE_CONTROL_START,
              40ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 已进入准备页。 */
    CHECK(coordinator.workout.state == WORKOUT_STATE_PREPARING);
    /* 测试通过。 */
    return 0;
}

/* 验证 5% 会先生成幂等摘要，再请求安全关机。 */
static int test_critical_battery_persist_then_shutdown(void)
{
    /* 初始化就绪设备。 */
    device_coordinator_t coordinator;
    /* 使用会话 60。 */
    CHECK(initialize_ready(&coordinator, 60U, 0ULL) == 0);
    /* 保存 effect。 */
    device_effects_t effects;
    /* 开始并锁定 squat。 */
    CHECK(start_and_lock(&coordinator, FITNESS_ACTION_SQUAT, 10ULL, &effects) == 0);
    /* 生成一次事件，使临界摘要含幂等键。 */
    CHECK(emit_one_squat_rep(&coordinator, 1010ULL, &effects) == 0);
    /* 更新到 5% 且未充电。 */
    CHECK(device_coordinator_update_battery(
              &coordinator,
              5U,
              false,
              1800ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 必须同时产生摘要和“落盘后关机”指令。 */
    CHECK((effects.flags & (DEVICE_EFFECT_SUMMARY_WRITE |
                            DEVICE_EFFECT_SHUTDOWN_AFTER_PERSIST)) ==
          (DEVICE_EFFECT_SUMMARY_WRITE |
           DEVICE_EFFECT_SHUTDOWN_AFTER_PERSIST));
    /* 摘要必须标记中断和临界低电。 */
    CHECK((effects.summary.flags &
           (DEVICE_SUMMARY_FLAG_INTERRUPTED |
            DEVICE_SUMMARY_FLAG_CRITICAL_BATTERY)) ==
          (DEVICE_SUMMARY_FLAG_INTERRUPTED |
           DEVICE_SUMMARY_FLAG_CRITICAL_BATTERY));
    /* 摘要保留最大 MetricEvent 序号。 */
    CHECK(effects.summary.last_event_seq == 1U);
    /* UI 已进入关机页。 */
    CHECK(effects.ui.state == UI_STATE_SHUTDOWN);
    /* 电源策略已请求 PMIC 断电。 */
    CHECK(effects.power_policy.request_pmic_shutdown);
    /* 协调器已进入安全关机状态。 */
    CHECK(coordinator.power.state == POWER_STATE_SAFE_SHUTDOWN);
    /* 测试通过。 */
    return 0;
}

/* 验证暂停不计热量，且相同 40 ms 恢复区间与短暂停结果相同。 */
static int test_pause_excludes_calories(void)
{
    /* 初始化就绪设备。 */
    device_coordinator_t coordinator;
    /* 使用会话 70。 */
    CHECK(initialize_ready(&coordinator, 70U, 0ULL) == 0);
    /* 保存 effect。 */
    device_effects_t effects;
    /* 开始并锁定 squat。 */
    CHECK(start_and_lock(&coordinator, FITNESS_ACTION_SQUAT, 10ULL, &effects) == 0);
    /* 提交首个运行样本建立热量基准。 */
    CHECK(push_point(&coordinator, 1010ULL, 0.0F, 1.0F, &effects) == 0);
    /* 再提交 40 ms 样本积累小量热量。 */
    CHECK(push_point(&coordinator, 1050ULL, 0.0F, 1.0F, &effects) == 0);
    /* 1090 ms 暂停。 */
    CHECK(device_coordinator_handle_control(
              &coordinator,
              DEVICE_CONTROL_PAUSE,
              1090ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 记录暂停前毛热量。 */
    const uint64_t calories_before_pause =
        coordinator.workout.fitness_session.gross_microkcal;
    /* 复制一份用于短暂停对照。 */
    device_coordinator_t short_pause = coordinator;
    /* 暂停期样本应被安全忽略。 */
    const motion_phase_sample_t paused_sample = make_sample(5000ULL, 0.0F, 1.0F);
    /* 保存忽略前状态。 */
    const device_coordinator_t paused_before = coordinator;
    /* 提交暂停样本。 */
    CHECK(device_coordinator_push_sample(
              &coordinator,
              &paused_sample,
              true,
              0U,
              &effects) == DEVICE_COORDINATOR_IGNORED);
    /* 忽略样本不能修改任何状态。 */
    CHECK(memcmp(&paused_before, &coordinator, sizeof(coordinator)) == 0);
    /* 长暂停到 10090 ms 恢复。 */
    CHECK(device_coordinator_handle_control(
              &coordinator,
              DEVICE_CONTROL_RESUME,
              10090ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 长暂停后只处理 40 ms。 */
    CHECK(push_point(&coordinator, 10130ULL, 0.0F, 1.0F, &effects) == 0);
    /* 对照立即恢复。 */
    CHECK(device_coordinator_handle_control(
              &short_pause,
              DEVICE_CONTROL_RESUME,
              1090ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 对照同样只处理 40 ms。 */
    CHECK(push_point(&short_pause, 1130ULL, 0.0F, 1.0F, &effects) == 0);
    /* 两者热量必须逐整数相同，证明 9 s 暂停未积分。 */
    CHECK(coordinator.workout.fitness_session.gross_microkcal ==
          short_pause.workout.fitness_session.gross_microkcal);
    /* 两者活动时长也相同。 */
    CHECK(coordinator.active_elapsed_ms == short_pause.active_elapsed_ms);
    /* 暂停前热量不能倒退。 */
    CHECK(coordinator.workout.fitness_session.gross_microkcal >= calories_before_pause);
    /* 测试通过。 */
    return 0;
}

/* 生成一个局部加速度峰并检查是否产生步事件。 */
static int emit_walk_peak(
    device_coordinator_t *coordinator,
    const uint64_t base_ms,
    device_effects_t *peak_effects)
{
    /* 保存中间 effect。 */
    device_effects_t effects;
    /* 稳定左侧点。 */
    CHECK(push_point(coordinator, base_ms, 0.0F, 1.0F, &effects) == 0);
    /* 第二基线点。 */
    CHECK(push_point(coordinator, base_ms + 40ULL, 0.0F, 1.0F, &effects) == 0);
    /* 峰顶点 1.35 g。 */
    CHECK(push_point(coordinator, base_ms + 80ULL, 0.0F, 1.35F, &effects) == 0);
    /* 右侧回落点确认局部峰。 */
    CHECK(push_point(coordinator, base_ms + 120ULL, 0.0F, 1.0F, peak_effects) == 0);
    /* 必须生成步事件。 */
    CHECK((peak_effects->flags & DEVICE_EFFECT_BLE_EVENT) != 0U);
    /* 保存峰事件，后续填充点不得覆盖调用方输出。 */
    const device_effects_t accepted_peak = *peak_effects;
    /* 以 40 ms 间隔填充到下一步，避免超过 120 ms 间断门槛。 */
    for (uint64_t time_ms = base_ms + 160ULL;
         time_ms < base_ms + 400ULL;
         time_ms += 40ULL) {
        /* 提交稳定填充点。 */
        CHECK(push_point(coordinator, time_ms, 0.0F, 1.0F, &effects) == 0);
    }
    /* 恢复该步的权威 event effect。 */
    *peak_effects = accepted_peak;
    /* 峰生成成功。 */
    return 0;
}

/* 验证 walk/trot 仅每 10 步一次 30 ms 振动，sit 整秒事件不振动。 */
static int test_step_batch_and_sit_haptics(void)
{
    /* 初始化 walk 设备。 */
    device_coordinator_t walk;
    /* 使用会话 80。 */
    CHECK(initialize_ready(&walk, 80U, 0ULL) == 0);
    /* 保存 effect。 */
    device_effects_t effects;
    /* 开始并锁定 walk。 */
    CHECK(start_and_lock(&walk, FITNESS_ACTION_WALK, 10ULL, &effects) == 0);
    /* 生成 10 个间隔 400 ms 的合法步峰。 */
    for (uint32_t step_index = 0U; step_index < 10U; ++step_index) {
        /* 每步基准时间比生理不应期更长。 */
        const uint64_t step_base_ms = 1010ULL + ((uint64_t)step_index * 400ULL);
        /* 生成局部峰。 */
        CHECK(emit_walk_peak(&walk, step_base_ms, &effects) == 0);
        /* 检查权威总步数。 */
        CHECK(effects.metric_event.total_value == (uint64_t)(step_index + 1U));
        /* 前 9 步不振动。 */
        if (step_index < 9U) {
            /* 检查振动 effect 缺失。 */
            CHECK((effects.flags & DEVICE_EFFECT_HAPTIC) == 0U);
        }
    }
    /* 第 10 步产生一次 30 ms 振动。 */
    CHECK((effects.haptic_count == 1U) &&
          (effects.haptics[0].reason == FITNESS_HAPTIC_REASON_STEP_BATCH) &&
          (effects.haptics[0].on_ms == 30U) &&
          (effects.haptics[0].repeat_count == 1U));

    /* 初始化 sit 设备。 */
    device_coordinator_t sit;
    /* 使用会话 81。 */
    CHECK(initialize_ready(&sit, 81U, 0ULL) == 0);
    /* 开始并锁定 sit。 */
    CHECK(start_and_lock(&sit, FITNESS_ACTION_SIT, 10ULL, &effects) == 0);
    /* 记录是否看到至少一个整秒事件。 */
    bool saw_sit_event = false;
    /* 提交 1.6 s 静坐样本。 */
    for (uint64_t time_ms = 1010ULL; time_ms <= 2610ULL; time_ms += 40ULL) {
        /* 提交静态 1 g 样本。 */
        CHECK(push_point(&sit, time_ms, 0.0F, 1.0F, &effects) == 0);
        /* 仅 MetricEvent 时检查 sit 振动规则。 */
        if ((effects.flags & DEVICE_EFFECT_BLE_EVENT) != 0U) {
            /* 标记已看到持续时间事件。 */
            saw_sit_event = true;
            /* 必须是毫秒持续指标。 */
            CHECK(effects.metric_event.metric_kind == FITNESS_METRIC_DURATION_MS);
            /* sit 事件不允许振动。 */
            CHECK((effects.flags & DEVICE_EFFECT_HAPTIC) == 0U);
        }
    }
    /* 至少 1 s 后必须产生事件。 */
    CHECK(saw_sit_event);
    /* 测试通过。 */
    return 0;
}

/* 验证 UI/BLE 命令映射和 BLE 连接属性事实链。 */
static int test_control_mapping_and_ble_state(void)
{
    /* 保存映射结果。 */
    device_control_t control = DEVICE_CONTROL_COUNT;
    /* UI 开始映射为 START。 */
    CHECK(device_coordinator_control_from_ui(UI_EVENT_START_REQUESTED, &control));
    /* 检查映射值。 */
    CHECK(control == DEVICE_CONTROL_START);
    /* UI 总结存储确认映射为 DONE。 */
    CHECK(device_coordinator_control_from_ui(UI_EVENT_SUMMARY_SAVED, &control));
    /* 检查映射值。 */
    CHECK(control == DEVICE_CONTROL_DONE);
    /* 非控制 UI 电池事件不映射。 */
    CHECK(!device_coordinator_control_from_ui(UI_EVENT_BATTERY_UPDATED, &control));
    /* BLE command 1..5/10 逐一映射。 */
    CHECK(device_coordinator_control_from_ble(1U, &control) &&
          (control == DEVICE_CONTROL_START));
    /* 检查暂停命令。 */
    CHECK(device_coordinator_control_from_ble(2U, &control) &&
          (control == DEVICE_CONTROL_PAUSE));
    /* 检查继续命令。 */
    CHECK(device_coordinator_control_from_ble(3U, &control) &&
          (control == DEVICE_CONTROL_RESUME));
    /* 检查停止命令。 */
    CHECK(device_coordinator_control_from_ble(4U, &control) &&
          (control == DEVICE_CONTROL_STOP));
    /* 检查重置命令。 */
    CHECK(device_coordinator_control_from_ble(5U, &control) &&
          (control == DEVICE_CONTROL_RESET));
    /* 检查快照命令。 */
    CHECK(device_coordinator_control_from_ble(10U, &control) &&
          (control == DEVICE_CONTROL_SNAPSHOT));
    /* 设置命令 9 留给设置组件。 */
    CHECK(!device_coordinator_control_from_ble(9U, &control));

    /* 初始化就绪设备。 */
    device_coordinator_t coordinator;
    /* 使用会话 90。 */
    CHECK(initialize_ready(&coordinator, 90U, 0ULL) == 0);
    /* 保存连接 effect。 */
    device_effects_t effects;
    /* 建立 PC BLE 连接。 */
    CHECK(device_coordinator_set_ble_connected(
              &coordinator,
              true,
              10ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* UI 图标已连接。 */
    CHECK(effects.ui.view.ble_connected);
    /* 电源策略使用已连接模式。 */
    CHECK(effects.power_policy.ble_mode == POWER_BLE_CONNECTED_ACTIVE);
    /* 重复连接不产生修订。 */
    CHECK(device_coordinator_set_ble_connected(
              &coordinator,
              true,
              20ULL,
              &effects) == DEVICE_COORDINATOR_IGNORED);
    /* 重复连接 effect 为空。 */
    CHECK(effects.flags == 0U);
    /* 测试通过。 */
    return 0;
}

/* 验证 NaN logits、NaN 样本和倒退时间均无状态突变。 */
static int test_error_inputs_are_transactional(void)
{
    /* 初始化就绪设备。 */
    device_coordinator_t coordinator;
    /* 使用会话 100。 */
    CHECK(initialize_ready(&coordinator, 100U, 0ULL) == 0);
    /* 保存 effect。 */
    device_effects_t effects;
    /* 开始进入 Prepare。 */
    CHECK(device_coordinator_handle_control(
              &coordinator,
              DEVICE_CONTROL_START,
              10ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 构造目标 logits。 */
    float logits[WORKOUT_CLASS_COUNT];
    /* 填充 squat 高分。 */
    make_logits(logits, FITNESS_ACTION_SQUAT);
    /* 注入 NaN。 */
    logits[4] = NAN;
    /* 保存错误前状态。 */
    device_coordinator_t before = coordinator;
    /* 尝试提交非有限 logits。 */
    CHECK(device_coordinator_push_inference(
              &coordinator,
              logits,
              20ULL,
              0U,
              &effects) == DEVICE_COORDINATOR_ERR_ARGUMENT);
    /* 状态逐字节不变。 */
    CHECK(memcmp(&before, &coordinator, sizeof(coordinator)) == 0);
    /* effect 必须为空。 */
    CHECK(effects.flags == 0U);

    /* 重新初始化，避免已开始状态重复 start。 */
    CHECK(initialize_ready(&coordinator, 101U, 100ULL) == 0);
    /* 开始并锁定 squat。 */
    CHECK(start_and_lock(&coordinator, FITNESS_ACTION_SQUAT, 110ULL, &effects) == 0);
    /* 构造含 NaN 的六轴样本。 */
    motion_phase_sample_t sample = make_sample(1110ULL, 0.0F, 1.0F);
    /* 在 ay 注入 NaN。 */
    sample.axis[4] = NAN;
    /* 保存错误前状态。 */
    before = coordinator;
    /* 提交非有限样本必须失败。 */
    CHECK(device_coordinator_push_sample(
              &coordinator,
              &sample,
              true,
              0U,
              &effects) == DEVICE_COORDINATOR_ERR_ARGUMENT);
    /* 状态逐字节不变。 */
    CHECK(memcmp(&before, &coordinator, sizeof(coordinator)) == 0);
    /* effect 必须为空。 */
    CHECK(effects.flags == 0U);
    /* 用早于已提交时间的控制命令。 */
    CHECK(device_coordinator_handle_control(
              &coordinator,
              DEVICE_CONTROL_PAUSE,
              100ULL,
              &effects) == DEVICE_COORDINATOR_ERR_TIME);
    /* 倒退时间后状态仍不变。 */
    CHECK(memcmp(&before, &coordinator, sizeof(coordinator)) == 0);
    /* 测试通过。 */
    return 0;
}

/* 验证运行中更新体重只影响下一会话，并统一拒绝 30～250 kg 外数值。 */
static int test_next_session_weight_update(void)
{
    /* 初始化 70 kg 设备。 */
    device_coordinator_t coordinator;
    /* 使用会话 110。 */
    CHECK(initialize_ready(&coordinator, 110U, 0ULL) == 0);
    /* 保存效果。 */
    device_effects_t effects;
    /* 启动并锁定当前深蹲会话。 */
    CHECK(start_and_lock(&coordinator, FITNESS_ACTION_SQUAT, 10ULL, &effects) == 0);
    /* 当前训练引擎必须冻结开始时的 70 kg。 */
    CHECK(coordinator.workout.weight_g == 70000U);
    /* 运行中更新下次会话体重为 80 kg。 */
    CHECK(device_coordinator_set_next_session_weight(
              &coordinator,
              80000U,
              1000ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 配置值已经更新。 */
    CHECK(coordinator.weight_g == 80000U);
    /* 当前会话仍保持 70 kg，不追溯修改已累计热量。 */
    CHECK(coordinator.workout.weight_g == 70000U);
    /* 更新必须产生权威 LiveState 修订。 */
    CHECK((effects.flags & DEVICE_EFFECT_BLE_LIVE_STATE) != 0U);
    /* 29999 g 低于统一下界。 */
    const device_coordinator_t before = coordinator;
    /* 非法体重必须事务失败。 */
    CHECK(device_coordinator_set_next_session_weight(
              &coordinator,
              29999U,
              1001ULL,
              &effects) == DEVICE_COORDINATOR_ERR_ARGUMENT);
    /* 失败后状态逐字节不变。 */
    CHECK(memcmp(&coordinator, &before, sizeof(coordinator)) == 0);
    /* 停止当前会话。 */
    CHECK(device_coordinator_handle_control(
              &coordinator,
              DEVICE_CONTROL_STOP,
              1100ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 确认摘要已保存并返回主页。 */
    CHECK(device_coordinator_handle_control(
              &coordinator,
              DEVICE_CONTROL_DONE,
              1101ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 开始下一会话。 */
    CHECK(device_coordinator_handle_control(
              &coordinator,
              DEVICE_CONTROL_START,
              1102ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 新会话必须使用 80 kg。 */
    CHECK(coordinator.workout.weight_g == 80000U);
    /* 体重更新测试完成。 */
    return 0;
}

/* 验证无目标 255、次数/秒/mcal 进度和 100% 饱和。 */
static int test_goal_progress_projection(void)
{
    /* 初始化无目标设备。 */
    device_coordinator_t coordinator;
    /* 使用会话 120。 */
    CHECK(initialize_ready(&coordinator, 120U, 0ULL) == 0);
    /* 保存 effect。 */
    device_effects_t effects;
    /* 无目标快照必须使用 255。 */
    CHECK(device_coordinator_handle_control(
              &coordinator,
              DEVICE_CONTROL_SNAPSHOT,
              1ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 校验无目标哨兵。 */
    CHECK(effects.live_state.goal_percent == BLE_SERVICE_GOAL_NOT_SET);
    /* 设置 2 次目标。 */
    CHECK(device_coordinator_set_goal(
              &coordinator,
              DEVICE_GOAL_REPETITIONS,
              2U,
              2ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 尚未训练时进度为 0%。 */
    CHECK(effects.live_state.goal_percent == 0U);
    /* 开始并锁定 squat。 */
    CHECK(start_and_lock(&coordinator, FITNESS_ACTION_SQUAT, 10ULL, &effects) == 0);
    /* 生成第一次数。 */
    CHECK(emit_one_squat_rep(&coordinator, 1010ULL, &effects) == 0);
    /* 一次除以两次等于 50%。 */
    CHECK(effects.live_state.goal_percent == 50U);
    /* 生成第二次数；起点承接上一周期末点后 40 ms。 */
    CHECK(emit_one_squat_rep(&coordinator, 1810ULL, &effects) == 0);
    /* 达到目标后饱和 100%。 */
    CHECK(effects.live_state.goal_percent == 100U);
    /* 继续第三次也不能超过 100；仍保持严格 25 Hz 连续样本。 */
    CHECK(emit_one_squat_rep(&coordinator, 2610ULL, &effects) == 0);
    /* 校验饱和值。 */
    CHECK(effects.live_state.goal_percent == 100U);
    /* 设置 10 秒目标。 */
    CHECK(device_coordinator_set_goal(
              &coordinator,
              DEVICE_GOAL_SECONDS,
              10U,
              6000ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 活动时间约 6 秒，百分比必须位于 50～60。 */
    CHECK((effects.live_state.goal_percent >= 50U) &&
          (effects.live_state.goal_percent <= 60U));
    /* 注入 500 mcal 权威累计，验证热量目标投影公式。 */
    coordinator.workout.fitness_session.gross_microkcal = UINT64_C(500000);
    /* 设置 1000 mcal 目标。 */
    CHECK(device_coordinator_set_goal(
              &coordinator,
              DEVICE_GOAL_MCAL,
              1000U,
              6001ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 500/1000 等于 50%。 */
    CHECK(effects.live_state.goal_percent == 50U);
    /* 无目标必须要求 value=0。 */
    const device_coordinator_t before = coordinator;
    /* 错误组合必须拒绝。 */
    CHECK(device_coordinator_set_goal(
              &coordinator,
              DEVICE_GOAL_NONE,
              1U,
              6002ULL,
              &effects) == DEVICE_COORDINATOR_ERR_ARGUMENT);
    /* 失败后状态不变。 */
    CHECK(memcmp(&coordinator, &before, sizeof(coordinator)) == 0);
    /* 清除目标。 */
    CHECK(device_coordinator_set_goal(
              &coordinator,
              DEVICE_GOAL_NONE,
              0U,
              6002ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* LiveState 恢复 255。 */
    CHECK(effects.live_state.goal_percent == BLE_SERVICE_GOAL_NOT_SET);
    /* 目标测试完成。 */
    return 0;
}

/* 验证协调器把 UI 与功耗的熄屏、唤醒和长空闲作为一个事务提交。 */
static int test_idle_power_event_transaction(void)
{
    /* 初始化主页设备。 */
    device_coordinator_t coordinator;
    /* 使用独立会话序号 130。 */
    CHECK(initialize_ready(&coordinator, 130U, 0ULL) == 0);
    /* 保存按值效果。 */
    device_effects_t effects;
    /* 主页空闲超时。 */
    CHECK(device_coordinator_handle_idle_power_event(
              &coordinator,
              POWER_EVENT_SCREEN_TIMEOUT,
              30000ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* UI 必须进入统一熄屏页。 */
    CHECK(coordinator.ui.state == UI_STATE_SCREEN_OFF);
    /* 无 BLE 连接时电源进入慢广播待机。 */
    CHECK(coordinator.power.state == POWER_STATE_ADVERTISING_STANDBY);
    /* 效果必须同时包含 UI 与 Power。 */
    CHECK((effects.flags & DEVICE_EFFECT_UI_RENDER) != 0U);
    /* 检查电源效果位。 */
    CHECK((effects.flags & DEVICE_EFFECT_POWER_POLICY) != 0U);
    /* 验证慢广播策略。 */
    CHECK(effects.power_policy.ble_mode == POWER_BLE_SLOW_ADVERTISING);
    /* 用户触摸唤醒。 */
    CHECK(device_coordinator_handle_idle_power_event(
              &coordinator,
              POWER_EVENT_USER_WAKE,
              30100ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* UI 恢复主页。 */
    CHECK(coordinator.ui.state == UI_STATE_HOME);
    /* 电源恢复主页。 */
    CHECK(coordinator.power.state == POWER_STATE_HOME);
    /* Home 使用快速广播。 */
    CHECK(effects.power_policy.ble_mode == POWER_BLE_FAST_ADVERTISING);
    /* 再次熄屏，形成合法长空闲前置状态。 */
    CHECK(device_coordinator_handle_idle_power_event(
              &coordinator,
              POWER_EVENT_SCREEN_TIMEOUT,
              60100ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 长空闲进入 Deep-sleep。 */
    CHECK(device_coordinator_handle_idle_power_event(
              &coordinator,
              POWER_EVENT_LONG_IDLE,
              630100ULL,
              &effects) == DEVICE_COORDINATOR_OK);
    /* 电源状态必须是 Deep Standby。 */
    CHECK(coordinator.power.state == POWER_STATE_DEEP_STANDBY);
    /* 策略必须请求 Deep-sleep。 */
    CHECK(effects.power_policy.request_deep_sleep);
    /* 已进入 Deep-sleep 时重复 LONG_IDLE 必须事务拒绝。 */
    const device_coordinator_t before = coordinator;
    /* 重复事件返回状态错误。 */
    CHECK(device_coordinator_handle_idle_power_event(
              &coordinator,
              POWER_EVENT_LONG_IDLE,
              630101ULL,
              &effects) == DEVICE_COORDINATOR_ERR_STATE);
    /* 失败不得修改状态。 */
    CHECK(memcmp(&coordinator, &before, sizeof(coordinator)) == 0);
    /* 测试完成。 */
    return 0;
}

/* 按顺序执行全部主机测试。 */
int main(void)
{
    /* 验证休息动态冻结且主动作计数器不被切换。 */
    CHECK(test_dynamic_rest_freezes_primary_counter() == 0);
    /* 验证完整会话和事实链。 */
    CHECK(test_complete_session_and_fact_chain() == 0);
    /* 验证真实 IMU 缺口只重置未完成周期，后续计数链可恢复。 */
    CHECK(test_jumping_jack_gap_reset_commits_and_recovers() == 0);
    /* 验证准备期补算事件全部进入 BLE/CSV effect。 */
    CHECK(test_prelock_replay_events_reach_effects() == 0);
    /* 验证低电启动门槛。 */
    CHECK(test_low_battery_start_gate() == 0);
    /* 验证临界低电保存关机。 */
    CHECK(test_critical_battery_persist_then_shutdown() == 0);
    /* 验证暂停排除热量。 */
    CHECK(test_pause_excludes_calories() == 0);
    /* 验证步批振动和 sit 无振动。 */
    CHECK(test_step_batch_and_sit_haptics() == 0);
    /* 验证 UI/BLE 映射和连接状态。 */
    CHECK(test_control_mapping_and_ble_state() == 0);
    /* 验证错误输入事务性。 */
    CHECK(test_error_inputs_are_transactional() == 0);
    /* 验证下次会话体重更新。 */
    CHECK(test_next_session_weight_update() == 0);
    /* 验证目标进度投影。 */
    CHECK(test_goal_progress_projection() == 0);
    /* 验证生产空闲事件不会拆分 UI 与功耗事实。 */
    CHECK(test_idle_power_event_transaction() == 0);
    /* 输出机器可读通过摘要和主机结构大小。 */
    (void)printf(
        "device_coordinator_tests passed assertions=%u coordinator_bytes=%zu effects_bytes=%zu\n",
        g_assertions,
        sizeof(device_coordinator_t),
        sizeof(device_effects_t));
    /* 返回成功。 */
    return 0;
}
