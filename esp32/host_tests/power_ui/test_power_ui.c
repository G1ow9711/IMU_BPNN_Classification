/* 引入板级适配合同，主机测试直接链接纯 C 实现。 */
#include "board_adapter.h"
/* 引入电源策略合同，验证续航预算和状态转移。 */
#include "power_manager.h"
/* 引入 UI 状态机合同，验证页面、指标和熄屏恢复。 */
#include "ui_state_machine.h"

/* 引入标准 I/O，测试失败时打印具体表达式和行号。 */
#include <stdio.h>
/* 引入定长整数，确保 mock 计数与生产结构宽度一致。 */
#include <stdint.h>

/* 保存当前累计失败数；零表示全部检查通过。 */
static int g_failure_count = 0;

/* 检查布尔表达式；失败时记录源文件行号，但继续执行后续用例。 */
#define CHECK_TRUE(expression)                                                        \
    do {                                                                              \
        /* 仅在表达式为假时增加失败数，便于一次运行发现多处问题。 */                 \
        if (!(expression)) {                                                          \
            /* 输出原始表达式和行号，定位失败合同。 */                                \
            (void)fprintf(stderr, "FAIL line=%d expression=%s\n", __LINE__, #expression); \
            /* 累加失败项，main 最终以非零退出。 */                                   \
            g_failure_count += 1;                                                     \
        }                                                                             \
    } while (0)

/* 检查整数相等；强制转为 long long，兼容枚举、布尔和不同宽度整数。 */
#define CHECK_EQ_INT(expected, actual)                                                \
    do {                                                                              \
        /* 保存期望值，避免带副作用表达式被重复求值。 */                             \
        const long long expected_value = (long long)(expected);                       \
        /* 保存实际值，避免带副作用表达式被重复求值。 */                             \
        const long long actual_value = (long long)(actual);                           \
        /* 比较数值而不是字符串表现。 */                                               \
        if (expected_value != actual_value) {                                         \
            /* 输出期望值、实际值和行号。 */                                           \
            (void)fprintf(                                                            \
                stderr,                                                               \
                "FAIL line=%d expected=%lld actual=%lld\n",                         \
                __LINE__,                                                             \
                expected_value,                                                       \
                actual_value);                                                        \
            /* 累加失败项。 */                                                         \
            g_failure_count += 1;                                                     \
        }                                                                             \
    } while (0)

/* 检查浮点近似相等；预算公式只需 0.01 mA/小时量级容差。 */
static void check_close(float expected, float actual, float tolerance, int line)
{
    /* 计算绝对差，避免依赖 libm 的 fabsf。 */
    const float difference = expected >= actual ? expected - actual : actual - expected;
    /* 超过容差时输出完整数值。 */
    if (difference > tolerance) {
        /* 打印浮点结果和对应调用行。 */
        (void)fprintf(
            stderr,
            "FAIL line=%d expected=%.6f actual=%.6f tolerance=%.6f\n",
            line,
            (double)expected,
            (double)actual,
            (double)tolerance);
        /* 累加失败项。 */
        g_failure_count += 1;
    }
}

/* 把调用点行号传入浮点比较函数。 */
#define CHECK_CLOSE(expected, actual, tolerance) \
    check_close((expected), (actual), (tolerance), __LINE__)

/* 保存触摸 I2C 探测模拟结果；位 0 表示 0x38，位 1 表示 0x5A。 */
typedef struct {
    /* 保存应答位图，探测函数只读取不修改。 */
    uint8_t ack_mask;
} probe_mock_t;

/* 模拟 I2C ACK；仅支持两个触摸地址。 */
static bool mock_probe(void *context, uint8_t address)
{
    /* 测试始终提供有效上下文。 */
    const probe_mock_t *probe = (const probe_mock_t *)context;
    /* 0x38 使用位 0。 */
    if (address == BOARD_TOUCH_FT3168_ADDRESS) {
        return (probe->ack_mask & 0x01U) != 0U;
    }
    /* 0x5A 使用位 1。 */
    if (address == BOARD_TOUCH_CST9220_ADDRESS) {
        return (probe->ack_mask & 0x02U) != 0U;
    }
    /* 其它地址不应答。 */
    return false;
}

/* 保存板级 mock 的调用结果和电池快照。 */
typedef struct {
    /* 保存屏幕供电状态。 */
    bool display_power;
    /* 保存最近亮度百分比。 */
    uint8_t brightness;
    /* 保存最近马达脉冲时长，单位毫秒。 */
    uint16_t haptic_duration_ms;
    /* 保存最近马达强度百分比。 */
    uint8_t haptic_intensity_percent;
    /* 保存马达调用次数，验证一次计数只产生一次脉冲。 */
    uint32_t haptic_call_count;
    /* 保存 mock 电量百分比。 */
    uint8_t battery_percent;
    /* 保存 mock 充电状态。 */
    bool charging;
} board_mock_t;

/* mock 设置屏幕供电。 */
static int board_mock_set_power(void *context, bool enabled)
{
    /* 转换并保存状态。 */
    board_mock_t *mock = (board_mock_t *)context;
    /* 记录开关。 */
    mock->display_power = enabled;
    /* 返回成功。 */
    return 0;
}

/* mock 设置屏幕亮度。 */
static int board_mock_set_brightness(void *context, uint8_t percent)
{
    /* 转换并保存 0~100 亮度。 */
    board_mock_t *mock = (board_mock_t *)context;
    /* 记录亮度。 */
    mock->brightness = percent;
    /* 返回成功。 */
    return 0;
}

/* mock 输出振动脉冲。 */
static int board_mock_pulse(void *context, uint16_t duration_ms, uint8_t intensity_percent)
{
    /* 转换 mock 上下文。 */
    board_mock_t *mock = (board_mock_t *)context;
    /* 记录脉冲时长。 */
    mock->haptic_duration_ms = duration_ms;
    /* 记录 PWM 强度。 */
    mock->haptic_intensity_percent = intensity_percent;
    /* 记录调用次数。 */
    mock->haptic_call_count += 1U;
    /* 返回成功。 */
    return 0;
}

/* mock 读取电池状态。 */
static int board_mock_read_battery(void *context, uint8_t *percent, bool *charging)
{
    /* 转换只读 mock 上下文。 */
    const board_mock_t *mock = (const board_mock_t *)context;
    /* 输出电量百分比。 */
    *percent = mock->battery_percent;
    /* 输出充电状态。 */
    *charging = mock->charging;
    /* 返回成功。 */
    return 0;
}

/* 验证板型引脚、面板默认值和触摸双地址探测。 */
static void test_board_profile_and_probe(void)
{
    /* 主机默认配置必须匹配本地原理图的 SH8601+FT3168。 */
    board_profile_t profile = board_profile_from_build();
    /* 验证板型合同有效。 */
    CHECK_EQ_INT(BOARD_ADAPTER_OK, board_profile_validate(&profile));
    /* 检查关键尺寸。 */
    CHECK_EQ_INT(410U, BOARD_DISPLAY_WIDTH_PX);
    /* 检查关键尺寸。 */
    CHECK_EQ_INT(502U, BOARD_DISPLAY_HEIGHT_PX);
    /* 检查 I2C SDA。 */
    CHECK_EQ_INT(15U, profile.i2c_sda_gpio);
    /* 检查 I2C SCL。 */
    CHECK_EQ_INT(14U, profile.i2c_scl_gpio);
    /* 检查马达 GPIO。 */
    CHECK_EQ_INT(18U, profile.motor_gpio);
    /* 检查 QMI 深睡可唤醒 GPIO。 */
    CHECK_EQ_INT(21U, profile.imu_interrupt_gpio);
    /* 检查本地原理图的触摸 Light-sleep 唤醒 GPIO。 */
    CHECK_EQ_INT(38U, profile.touch_interrupt_gpio);
    /* 检查本地原理图的 RTC Light-sleep 唤醒 GPIO。 */
    CHECK_EQ_INT(39U, profile.rtc_interrupt_gpio);
    /* 保存探测输出。 */
    board_touch_controller_t detected = BOARD_TOUCH_CONTROLLER_UNKNOWN;
    /* 仅 0x38 应答。 */
    probe_mock_t probe = {.ack_mask = 0x01U};
    /* 应识别 FT3168。 */
    CHECK_EQ_INT(BOARD_ADAPTER_OK, board_detect_touch_controller(mock_probe, &probe, &detected));
    /* 检查探测类型。 */
    CHECK_EQ_INT(BOARD_TOUCH_CONTROLLER_FT3168, detected);
    /* 改为仅 0x5A 应答。 */
    probe.ack_mask = 0x02U;
    /* 应识别 CST9220。 */
    CHECK_EQ_INT(BOARD_ADAPTER_OK, board_detect_touch_controller(mock_probe, &probe, &detected));
    /* 检查探测类型。 */
    CHECK_EQ_INT(BOARD_TOUCH_CONTROLLER_CST9220, detected);
    /* 两地址同时应答必须拒绝猜测。 */
    probe.ack_mask = 0x03U;
    /* 检查歧义错误。 */
    CHECK_EQ_INT(BOARD_ADAPTER_ERR_AMBIGUOUS, board_detect_touch_controller(mock_probe, &probe, &detected));
    /* 检查输出标记未知。 */
    CHECK_EQ_INT(BOARD_TOUCH_CONTROLLER_UNKNOWN, detected);
    /* 两地址都不应答必须报告 I/O 错误。 */
    probe.ack_mask = 0x00U;
    /* 检查无响应错误。 */
    CHECK_EQ_INT(BOARD_ADAPTER_ERR_IO, board_detect_touch_controller(mock_probe, &probe, &detected));
}

/* 验证板级屏幕、马达和电池抽象，不依赖真实 GPIO。 */
static void test_board_adapter_operations(void)
{
    /* 创建有效板型。 */
    const board_profile_t profile = board_profile_from_build();
    /* 创建 73% 且充电中的 mock 硬件。 */
    board_mock_t mock = {.battery_percent = 73U, .charging = true};
    /* 只提供本用例需要的回调；其它回调允许为空。 */
    const board_adapter_ops_t ops = {
        .context = &mock,
        .set_display_power = board_mock_set_power,
        .set_display_brightness = board_mock_set_brightness,
        .pulse_motor = board_mock_pulse,
        .read_battery = board_mock_read_battery,
    };
    /* 创建零初始化适配器。 */
    board_adapter_t adapter = {0};
    /* 初始化纯软件合同。 */
    CHECK_EQ_INT(BOARD_ADAPTER_OK, board_adapter_init(&adapter, &profile, &ops));
    /* 点亮屏幕并设置 35%。 */
    CHECK_EQ_INT(BOARD_ADAPTER_OK, board_adapter_set_display(&adapter, true, 35U));
    /* 检查供电已打开。 */
    CHECK_TRUE(mock.display_power);
    /* 检查亮度已设置。 */
    CHECK_EQ_INT(35U, mock.brightness);
    /* 关闭屏幕应把亮度归零。 */
    CHECK_EQ_INT(BOARD_ADAPTER_OK, board_adapter_set_display(&adapter, false, 35U));
    /* 检查供电关闭。 */
    CHECK_TRUE(!mock.display_power);
    /* 检查亮度归零。 */
    CHECK_EQ_INT(0U, mock.brightness);
    /* 请求一次 30 ms、75% 计数反馈，与领域层有效计数波形一致。 */
    CHECK_EQ_INT(BOARD_ADAPTER_OK, board_adapter_pulse_haptic(&adapter, 30U, 75U));
    /* 检查仅一次回调。 */
    CHECK_EQ_INT(1U, mock.haptic_call_count);
    /* 检查时长。 */
    CHECK_EQ_INT(30U, mock.haptic_duration_ms);
    /* 检查强度。 */
    CHECK_EQ_INT(75U, mock.haptic_intensity_percent);
    /* 零时长必须拒绝，防止非法 PWM 请求。 */
    CHECK_EQ_INT(BOARD_ADAPTER_ERR_ARGUMENT, board_adapter_pulse_haptic(&adapter, 0U, 75U));
    /* 准备电池输出。 */
    uint8_t percent = 0U;
    /* 准备充电输出。 */
    bool charging = false;
    /* 读取 mock 电池。 */
    CHECK_EQ_INT(BOARD_ADAPTER_OK, board_adapter_read_battery(&adapter, &percent, &charging));
    /* 检查百分比。 */
    CHECK_EQ_INT(73U, percent);
    /* 检查充电标记。 */
    CHECK_TRUE(charging);
}

/* 构造无负载 UI 事件，减少状态转移用例中的重复字段。 */
static ui_event_t make_ui_event(ui_event_type_t type, uint32_t monotonic_ms)
{
    /* 创建零初始化事件并填充类型和时刻。 */
    ui_event_t event = {.type = type, .monotonic_ms = monotonic_ms};
    /* 返回按值事件。 */
    return event;
}

/* 验证 UI 冷启动、开始、计数、熄屏、暂停、总结和安全关机。 */
static void test_ui_state_machine(void)
{
    /* 创建 UI 上下文。 */
    ui_context_t ui;
    /* 从 100 ms 冷启动。 */
    ui_context_init(&ui, 100U);
    /* 检查 BOOT。 */
    CHECK_EQ_INT(UI_STATE_BOOT, ui.state);
    /* 开机动画完成。 */
    ui_event_t event = make_ui_event(UI_EVENT_BOOT_READY, 900U);
    /* 进入自检。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 检查自检页。 */
    CHECK_EQ_INT(UI_STATE_SELF_TEST, ui.state);
    /* 自检通过。 */
    event = make_ui_event(UI_EVENT_SELF_TEST_OK, 1000U);
    /* 进入主页。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 检查主页。 */
    CHECK_EQ_INT(UI_STATE_HOME, ui.state);
    /* 用户开始会话。 */
    event = make_ui_event(UI_EVENT_START_REQUESTED, 1100U);
    /* 进入准备页。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 检查准备页。 */
    CHECK_EQ_INT(UI_STATE_PREPARE, ui.state);
    /* IMU 预热和倒计时完成。 */
    event = make_ui_event(UI_EVENT_PREPARE_COMPLETED, 4100U);
    /* 进入训练页。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 构造第 7 类第 12 次计数快照。 */
    event = make_ui_event(UI_EVENT_METRIC_UPDATED, 5000U);
    /* 设置动作索引。 */
    event.metrics.action_id = 6U;
    /* 设置计数。 */
    event.metrics.count = 12U;
    /* 设置 3.250 kcal。 */
    event.metrics.calories_milli_kcal = 3250U;
    /* 设置 65 秒。 */
    event.metrics.elapsed_seconds = 65U;
    /* 设置 92.50% 置信度。 */
    event.metrics.confidence_centipercent = 9250U;
    /* 设置 68% 电量。 */
    event.metrics.battery_percent = 68U;
    /* 训练页应接受快照。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 检查计数更新。 */
    CHECK_EQ_INT(12U, ui.view.count);
    /* 熄屏不停止训练。 */
    event = make_ui_event(UI_EVENT_SCREEN_TIMEOUT, 6000U);
    /* 进入 SCREEN_OFF。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 检查恢复点为 RUNNING。 */
    CHECK_EQ_INT(UI_STATE_RUNNING, ui.screen_off_resume_state);
    /* 熄屏训练仍接受新快照。 */
    event = make_ui_event(UI_EVENT_METRIC_UPDATED, 6500U);
    /* 设置有效动作。 */
    event.metrics.action_id = 6U;
    /* 计数增至 13。 */
    event.metrics.count = 13U;
    /* 保持合法置信度。 */
    event.metrics.confidence_centipercent = 9000U;
    /* 保持合法电量。 */
    event.metrics.battery_percent = 67U;
    /* 接受训练熄屏指标。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 检查计数。 */
    CHECK_EQ_INT(13U, ui.view.count);
    /* 用户唤醒恢复训练页。 */
    event = make_ui_event(UI_EVENT_USER_WAKE, 7000U);
    /* 分派唤醒。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 检查恢复。 */
    CHECK_EQ_INT(UI_STATE_RUNNING, ui.state);
    /* 暂停会话。 */
    event = make_ui_event(UI_EVENT_PAUSE_REQUESTED, 7100U);
    /* 进入暂停。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 暂停后残留指标必须忽略。 */
    event = make_ui_event(UI_EVENT_METRIC_UPDATED, 7200U);
    /* 设置合法动作。 */
    event.metrics.action_id = 6U;
    /* 试图错误增至 14。 */
    event.metrics.count = 14U;
    /* 设置合法置信度。 */
    event.metrics.confidence_centipercent = 9000U;
    /* 设置合法电量。 */
    event.metrics.battery_percent = 67U;
    /* 检查事件被忽略。 */
    CHECK_EQ_INT(UI_DISPATCH_IGNORED, ui_dispatch_event(&ui, &event));
    /* 检查原计数保持。 */
    CHECK_EQ_INT(13U, ui.view.count);
    /* 暂停状态的屏幕停止先打开确认页。 */
    event = make_ui_event(UI_EVENT_STOP_CONFIRM_REQUESTED, 7900U);
    /* 进入停止确认。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 检查确认页。 */
    CHECK_EQ_INT(UI_STATE_STOP_CONFIRM, ui.state);
    /* 用户确认后才产生真正停止事件。 */
    event = make_ui_event(UI_EVENT_STOP_REQUESTED, 8000U);
    /* 进入总结。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 检查总结页。 */
    CHECK_EQ_INT(UI_STATE_SUMMARY, ui.state);
    /* 摘要保存完成。 */
    event = make_ui_event(UI_EVENT_SUMMARY_SAVED, 8100U);
    /* 回主页。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 检查会话已清除。 */
    CHECK_TRUE(!ui.session_active);
    /* 任意页面长按电源键进入安全关机。 */
    event = make_ui_event(UI_EVENT_POWER_LONG_PRESS, 9000U);
    /* 分派关机。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 检查关机动画页。 */
    CHECK_EQ_INT(UI_STATE_SHUTDOWN, ui.state);
    /* 正常关机动画期间屏幕仍点亮，动画完成后由电源层关闭。 */
    CHECK_TRUE(ui.screen_on);
}

/* 验证设置、诊断和总结等非训练页面同样可熄屏并精确恢复。 */
static void test_ui_secondary_page_screen_timeout(void)
{
    /* 创建独立 UI 上下文，避免依赖上一用例的关机状态。 */
    ui_context_t ui;
    /* 从 Boot 初始化。 */
    ui_context_init(&ui, 0U);
    /* 完成开机动画。 */
    ui_event_t event = make_ui_event(UI_EVENT_BOOT_READY, 100U);
    /* 进入 SelfTest。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 完成自检。 */
    event = make_ui_event(UI_EVENT_SELF_TEST_OK, 200U);
    /* 进入 Home。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 打开设置页。 */
    event = make_ui_event(UI_EVENT_OPEN_SETTINGS, 300U);
    /* 分派导航。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 设置页超时熄屏。 */
    event = make_ui_event(UI_EVENT_SCREEN_TIMEOUT, 30300U);
    /* 分派熄屏。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 恢复点必须是 Settings。 */
    CHECK_EQ_INT(UI_STATE_SETTINGS, ui.screen_off_resume_state);
    /* 触摸唤醒。 */
    event = make_ui_event(UI_EVENT_USER_WAKE, 30400U);
    /* 分派唤醒。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 恢复 Settings。 */
    CHECK_EQ_INT(UI_STATE_SETTINGS, ui.state);
    /* 从设置进入诊断。 */
    event = make_ui_event(UI_EVENT_OPEN_DIAGNOSTICS, 30500U);
    /* 分派导航。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 诊断页超时熄屏。 */
    event = make_ui_event(UI_EVENT_SCREEN_TIMEOUT, 60500U);
    /* 分派熄屏。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 恢复点必须是 Diagnostics。 */
    CHECK_EQ_INT(UI_STATE_DIAGNOSTICS, ui.screen_off_resume_state);
    /* 唤醒诊断页。 */
    event = make_ui_event(UI_EVENT_USER_WAKE, 60600U);
    /* 分派唤醒。 */
    CHECK_EQ_INT(UI_DISPATCH_OK, ui_dispatch_event(&ui, &event));
    /* 验证恢复诊断页。 */
    CHECK_EQ_INT(UI_STATE_DIAGNOSTICS, ui.state);
}

/* 构造无附加负载的电源事件。 */
static power_event_t make_power_event(power_event_type_t type, uint32_t monotonic_ms)
{
    /* 创建零初始化事件。 */
    power_event_t event = {.type = type, .monotonic_ms = monotonic_ms, .charging = false};
    /* 返回按值事件。 */
    return event;
}

/* 验证 400 mAh 电池预算、低电量门控和主要功耗状态。 */
static void test_power_manager(void)
{
    /* 6 小时活动允许约 53.333 mA。 */
    CHECK_CLOSE(53.333333F, power_budget_max_average_current_ma(6.0F), 0.001F);
    /* 7 天待机允许约 1.905 mA。 */
    CHECK_CLOSE(1.904762F, power_budget_max_average_current_ma(168.0F), 0.001F);
    /* 53.333 mA 应反推约 6 小时。 */
    CHECK_CLOSE(6.0F, power_budget_estimated_runtime_hours(53.333333F), 0.001F);
    /* 创建允许 QMI GPIO21 深睡唤醒的状态机。 */
    power_manager_t manager;
    /* 初始化 BOOT。 */
    power_manager_init(&manager, 0U, true);
    /* 未知电量禁止启动。 */
    CHECK_TRUE(!power_manager_can_start_session(&manager));
    /* 80% 电量为正常。 */
    CHECK_EQ_INT(POWER_BATTERY_NORMAL, power_manager_update_battery(&manager, 80U, false));
    /* 正常电量允许启动。 */
    CHECK_TRUE(power_manager_can_start_session(&manager));
    /* 启动完成进入 HOME。 */
    power_event_t event = make_power_event(POWER_EVENT_BOOT_COMPLETED, 100U);
    /* 分派启动完成。 */
    CHECK_TRUE(power_manager_dispatch(&manager, &event));
    /* 检查主页状态。 */
    CHECK_EQ_INT(POWER_STATE_HOME, manager.state);
    /* 主页策略点亮 35% 屏幕并开启 WOM。 */
    power_policy_t policy = power_manager_policy(&manager);
    /* 检查屏幕。 */
    CHECK_TRUE(policy.display_on);
    /* 检查亮度。 */
    CHECK_EQ_INT(35U, policy.display_brightness_percent);
    /* 检查 IMU 模式。 */
    CHECK_EQ_INT(POWER_IMU_WAKE_ON_MOTION, policy.imu_mode);
    /* 建立 BLE 连接。 */
    event = make_power_event(POWER_EVENT_BLE_CONNECTED, 200U);
    /* 更新连接属性。 */
    CHECK_TRUE(power_manager_dispatch(&manager, &event));
    /* 开始会话。 */
    event = make_power_event(POWER_EVENT_SESSION_STARTED, 300U);
    /* 进入训练。 */
    CHECK_TRUE(power_manager_dispatch(&manager, &event));
    /* 检查训练状态。 */
    CHECK_EQ_INT(POWER_STATE_RUNNING, manager.state);
    /* 训练策略使用 25 Hz IMU。 */
    policy = power_manager_policy(&manager);
    /* 检查 IMU。 */
    CHECK_EQ_INT(POWER_IMU_ACTIVE_25HZ, policy.imu_mode);
    /* 检查 BLE 活动连接。 */
    CHECK_EQ_INT(POWER_BLE_CONNECTED_ACTIVE, policy.ble_mode);
    /* 训练超时熄屏。 */
    event = make_power_event(POWER_EVENT_SCREEN_TIMEOUT, 400U);
    /* 进入训练熄屏。 */
    CHECK_TRUE(power_manager_dispatch(&manager, &event));
    /* 检查状态。 */
    CHECK_EQ_INT(POWER_STATE_RUNNING_SCREEN_OFF, manager.state);
    /* 熄屏仍保持 25 Hz 和 BLE，但不自动 Light-sleep。 */
    policy = power_manager_policy(&manager);
    /* 检查屏幕关闭。 */
    CHECK_TRUE(!policy.display_on);
    /* 检查 IMU 仍活动。 */
    CHECK_EQ_INT(POWER_IMU_ACTIVE_25HZ, policy.imu_mode);
    /* 检查未请求自动 Light-sleep。 */
    CHECK_TRUE(!policy.automatic_light_sleep);
    /* 唤醒恢复 RUNNING。 */
    event = make_power_event(POWER_EVENT_USER_WAKE, 500U);
    /* 分派唤醒。 */
    CHECK_TRUE(power_manager_dispatch(&manager, &event));
    /* 检查恢复。 */
    CHECK_EQ_INT(POWER_STATE_RUNNING, manager.state);
    /* 暂停。 */
    event = make_power_event(POWER_EVENT_SESSION_PAUSED, 600U);
    /* 分派暂停。 */
    CHECK_TRUE(power_manager_dispatch(&manager, &event));
    /* 暂停熄屏进入连接待机。 */
    event = make_power_event(POWER_EVENT_SCREEN_TIMEOUT, 700U);
    /* 分派熄屏。 */
    CHECK_TRUE(power_manager_dispatch(&manager, &event));
    /* 检查连接待机。 */
    CHECK_EQ_INT(POWER_STATE_CONNECTED_STANDBY, manager.state);
    /* 连接待机必须使用 BLE modem sleep 和自动 Light-sleep。 */
    policy = power_manager_policy(&manager);
    /* 检查自动 Light-sleep。 */
    CHECK_TRUE(policy.automatic_light_sleep);
    /* 检查 BLE modem sleep。 */
    CHECK_EQ_INT(POWER_BLE_CONNECTED_MODEM_SLEEP, policy.ble_mode);
    /* 检查触摸 Light-sleep 唤醒。 */
    CHECK_TRUE(policy.enable_touch_light_wake);
    /* 当前 BSP 不支持触摸芯片硬休眠，待机必须保留 LVGL 输入以接收第一下触摸。 */
    CHECK_TRUE(policy.touch_active);
    /* 模拟 USB/外部 VBUS 可用；产品把 charging 属性作为外部供电保护位。 */
    event = make_power_event(POWER_EVENT_CHARGING_CHANGED, 750U);
    /* 设置外部供电有效。 */
    event.charging = true;
    /* 外部供电属性应更新，但不改变连接待机页面状态。 */
    CHECK_TRUE(power_manager_dispatch(&manager, &event));
    /* 检查仍为连接待机。 */
    CHECK_EQ_INT(POWER_STATE_CONNECTED_STANDBY, manager.state);
    /* 外部供电时长空闲不得进入 Deep-sleep，否则 USB 调试和 BLE 联调会失联。 */
    event = make_power_event(POWER_EVENT_LONG_IDLE, 800U);
    /* 无状态变化应返回 false。 */
    CHECK_TRUE(!power_manager_dispatch(&manager, &event));
    /* 检查仍保持连接待机。 */
    CHECK_EQ_INT(POWER_STATE_CONNECTED_STANDBY, manager.state);
    /* 模拟拔掉 USB 后恢复纯电池供电。 */
    event = make_power_event(POWER_EVENT_CHARGING_CHANGED, 850U);
    /* 清除外部供电保护位。 */
    event.charging = false;
    /* 供电属性变化应被接受。 */
    CHECK_TRUE(power_manager_dispatch(&manager, &event));
    /* 纯电池长空闲才允许进入 Deep-sleep。 */
    event = make_power_event(POWER_EVENT_LONG_IDLE, 900U);
    /* 分派长空闲。 */
    CHECK_TRUE(power_manager_dispatch(&manager, &event));
    /* 检查 Deep-sleep。 */
    CHECK_EQ_INT(POWER_STATE_DEEP_STANDBY, manager.state);
    /* Deep-sleep 只允许可选 GPIO21 QMI 唤醒。 */
    policy = power_manager_policy(&manager);
    /* 检查深睡请求。 */
    CHECK_TRUE(policy.request_deep_sleep);
    /* 检查 QMI 深睡唤醒已启用。 */
    CHECK_TRUE(policy.enable_imu_deep_wake);
    /* GPIO38 触摸不允许 Deep-sleep 唤醒。 */
    CHECK_TRUE(!policy.enable_touch_light_wake);
    /* GPIO39 RTC 不允许 Deep-sleep 唤醒。 */
    CHECK_TRUE(!policy.enable_rtc_light_wake);
    /* 验证低电量级别。 */
    CHECK_EQ_INT(POWER_BATTERY_WARNING, power_manager_update_battery(&manager, 15U, false));
    /* 8% 禁止新会话。 */
    CHECK_EQ_INT(POWER_BATTERY_BLOCK_START, power_manager_update_battery(&manager, 8U, false));
    /* 检查门控。 */
    CHECK_TRUE(!power_manager_can_start_session(&manager));
    /* 充电时 8% 可启动。 */
    CHECK_EQ_INT(POWER_BATTERY_BLOCK_START, power_manager_update_battery(&manager, 8U, true));
    /* 检查充电旁路。 */
    CHECK_TRUE(power_manager_can_start_session(&manager));
    /* 5% 返回临界。 */
    CHECK_EQ_INT(POWER_BATTERY_CRITICAL, power_manager_update_battery(&manager, 5U, false));
    /* 临界事件进入安全关机。 */
    event = make_power_event(POWER_EVENT_CRITICAL_BATTERY, 1000U);
    /* 分派临界电量。 */
    CHECK_TRUE(power_manager_dispatch(&manager, &event));
    /* 检查安全关机。 */
    CHECK_EQ_INT(POWER_STATE_SAFE_SHUTDOWN, manager.state);
    /* 安全关机请求 PMIC 断电。 */
    policy = power_manager_policy(&manager);
    /* 检查 PMIC 请求。 */
    CHECK_TRUE(policy.request_pmic_shutdown);
}

/* 验证生产空闲计时器的一次性熄屏、会话保护、长空闲和时间倒退语义。 */
static void test_power_idle_timer(void)
{
    /* 创建纯值计时器，主机测试不依赖 FreeRTOS 定时器。 */
    power_idle_timer_t timer;
    /* 从 1000 ms 建立首次活动基准。 */
    power_idle_timer_init(&timer, UINT64_C(1000));
    /* 保存轮询输出事件。 */
    power_event_type_t event_type = POWER_EVENT_BOOT_COMPLETED;
    /* 30 秒前不能熄屏。 */
    CHECK_TRUE(!power_idle_timer_poll(
        &timer,
        UINT64_C(30999),
        UINT32_C(30000),
        UINT32_C(600000),
        false,
        &event_type));
    /* 精确到 30 秒门槛时发出一次熄屏。 */
    CHECK_TRUE(power_idle_timer_poll(
        &timer,
        UINT64_C(31000),
        UINT32_C(30000),
        UINT32_C(600000),
        false,
        &event_type));
    /* 验证输出类型。 */
    CHECK_EQ_INT(POWER_EVENT_SCREEN_TIMEOUT, event_type);
    /* 同一活动周期下一秒不能重复发熄屏。 */
    CHECK_TRUE(!power_idle_timer_poll(
        &timer,
        UINT64_C(32000),
        UINT32_C(30000),
        UINT32_C(600000),
        false,
        &event_type));
    /* 精确到 10 分钟空闲时发出长空闲。 */
    CHECK_TRUE(power_idle_timer_poll(
        &timer,
        UINT64_C(601000),
        UINT32_C(30000),
        UINT32_C(600000),
        false,
        &event_type));
    /* 验证长空闲类型。 */
    CHECK_EQ_INT(POWER_EVENT_LONG_IDLE, event_type);
    /* 长空闲同样只发一次。 */
    CHECK_TRUE(!power_idle_timer_poll(
        &timer,
        UINT64_C(602000),
        UINT32_C(30000),
        UINT32_C(600000),
        false,
        &event_type));
    /* 新用户活动重新打开两个门槛。 */
    power_idle_timer_note_activity(&timer, UINT64_C(700000));
    /* 有会话时 10 分钟只能先发熄屏，不能发 Deep-sleep。 */
    CHECK_TRUE(power_idle_timer_poll(
        &timer,
        UINT64_C(1300000),
        UINT32_C(30000),
        UINT32_C(600000),
        true,
        &event_type));
    /* 验证会话保护下输出仍是熄屏。 */
    CHECK_EQ_INT(POWER_EVENT_SCREEN_TIMEOUT, event_type);
    /* 活动会话持续更久也不得发 LONG_IDLE。 */
    CHECK_TRUE(!power_idle_timer_poll(
        &timer,
        UINT64_C(1900000),
        UINT32_C(30000),
        UINT32_C(600000),
        true,
        &event_type));
    /* 单调时间倒退时重新建立基准且不发事件。 */
    CHECK_TRUE(!power_idle_timer_poll(
        &timer,
        UINT64_C(10),
        UINT32_C(30000),
        UINT32_C(600000),
        false,
        &event_type));
    /* 新基准后 30 秒再次允许熄屏。 */
    CHECK_TRUE(power_idle_timer_poll(
        &timer,
        UINT64_C(30010),
        UINT32_C(30000),
        UINT32_C(600000),
        false,
        &event_type));
    /* 验证倒退恢复后的事件类型。 */
    CHECK_EQ_INT(POWER_EVENT_SCREEN_TIMEOUT, event_type);
    /* 非法相同门槛必须拒绝，避免同轮事件优先级不确定。 */
    CHECK_TRUE(!power_idle_timer_poll(
        &timer,
        UINT64_C(900000),
        UINT32_C(30000),
        UINT32_C(30000),
        false,
        &event_type));
}

/* 运行全部纯 C 测试；退出码供 PowerShell/CI 判定。 */
int main(void)
{
    /* 验证板型和触摸探测。 */
    test_board_profile_and_probe();
    /* 验证板级回调。 */
    test_board_adapter_operations();
    /* 验证 UI 状态机。 */
    test_ui_state_machine();
    /* 验证非训练页面的 AMOLED 超时也可达且可恢复。 */
    test_ui_secondary_page_screen_timeout();
    /* 验证电源状态机和预算。 */
    test_power_manager();
    /* 验证生产空闲时钟能真实触发熄屏与 Deep-sleep。 */
    test_power_idle_timer();
    /* 有失败时输出数量并返回 1。 */
    if (g_failure_count != 0) {
        /* 打印失败总数。 */
        (void)fprintf(stderr, "power_ui_tests failed=%d\n", g_failure_count);
        /* 非零退出让自动化阻止交付。 */
        return 1;
    }
    /* 全部合同通过时输出稳定标记。 */
    (void)printf("power_ui_tests passed\n");
    /* 返回零表示成功。 */
    return 0;
}
