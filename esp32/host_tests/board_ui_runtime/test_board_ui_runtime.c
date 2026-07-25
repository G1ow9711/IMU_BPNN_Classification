/* 引入真实/Mock 共用板级运行时合同。 */
#include "board_runtime.h"
/* 引入 UI presenter 和状态机合同。 */
#include "ui_presenter.h"
/* 引入配对码原子邮箱；测试 NimBLE 任务到应用任务的无阻塞事件交接。 */
#include "ui_app_pairing.h"

/* 引入标准 I/O 和字符串比较。 */
#include <stdio.h>
#include <string.h>

/* 保存失败数量；main 最终根据该值返回。 */
static int g_failures = 0;

/* 检查布尔表达式并打印行号。 */
#define CHECK_TRUE(expression)                                                         \
    do {                                                                               \
        /* 表达式为假时记录失败，但继续运行后续边界用例。 */                          \
        if (!(expression)) {                                                           \
            (void)fprintf(stderr, "FAIL line=%d expression=%s\n", __LINE__, #expression); \
            g_failures += 1;                                                           \
        }                                                                              \
    } while (0)

/* 检查整数相等并输出期望/实际值。 */
#define CHECK_EQ(expected, actual)                                                     \
    do {                                                                               \
        /* 保存值，避免含副作用表达式重复执行。 */                                    \
        const long long expected_value = (long long)(expected);                        \
        const long long actual_value = (long long)(actual);                            \
        /* 不相等时记录失败。 */                                                       \
        if (expected_value != actual_value) {                                          \
            (void)fprintf(stderr, "FAIL line=%d expected=%lld actual=%lld\n",       \
                __LINE__, expected_value, actual_value);                               \
            g_failures += 1;                                                           \
        }                                                                              \
    } while (0)

/* 保存独立 QMI/RTC/PMIC 驱动 Mock 调用状态。 */
typedef struct {
    /* 保存 QMI 最近三态模式，验证 ACTIVE、WOM、OFF 不被压缩为布尔值。 */
    board_runtime_qmi_mode_t qmi_mode;
    /* 保存 RTC 模拟 Unix 秒。 */
    uint64_t rtc_seconds;
    /* 保存 PMIC 关机调用次数。 */
    uint32_t shutdown_calls;
} external_mock_t;

/* 模拟切换 QMI 模式。 */
static int external_set_qmi(void *context, board_runtime_qmi_mode_t mode)
{
    /* 把上下文转换为测试结构。 */
    external_mock_t *mock = (external_mock_t *)context;
    /* 保存三态模式。 */
    mock->qmi_mode = mode;
    /* 返回成功。 */
    return 0;
}

/* 模拟读取 RTC。 */
static int external_read_rtc(void *context, uint64_t *unix_seconds)
{
    /* 把上下文转换为只读测试结构。 */
    const external_mock_t *mock = (const external_mock_t *)context;
    /* 输出固定 UTC 秒。 */
    *unix_seconds = mock->rtc_seconds;
    /* 返回成功。 */
    return 0;
}

/* 模拟 PMIC 安全断电请求。 */
static int external_shutdown(void *context)
{
    /* 把上下文转换为测试结构。 */
    external_mock_t *mock = (external_mock_t *)context;
    /* 记录一次关机请求。 */
    mock->shutdown_calls += 1U;
    /* 返回成功。 */
    return 0;
}

/* 验证 Mock 板级运行时及独立驱动边界。 */
static void test_board_runtime_mock(void)
{
    /* 准备独立驱动 Mock。 */
    external_mock_t external = {
        .rtc_seconds = UINT64_C(1783987200),
    };
    /* 配置 400 mAh 原厂电池对应的运行快照；容量策略位于 power_manager。 */
    const board_runtime_config_t config = {
        .external_ops = {
            .context = &external,
            .set_qmi_mode = external_set_qmi,
            .read_rtc_unix_seconds = external_read_rtc,
            .request_pmic_shutdown = external_shutdown,
        },
        .mock = {
            .battery_percent = 76U,
            .charging = true,
            .storage_present = true,
            .sensor_devices_present = true,
        },
        .initial_brightness_percent = 35U,
    };
    /* 创建零初始化运行时。 */
    board_runtime_t runtime = {0};
    /* Mock 初始化必须成功。 */
    CHECK_EQ(BOARD_RUNTIME_OK, board_runtime_init(&runtime, &config));
    /* 读取诊断快照。 */
    board_runtime_diagnostics_t diagnostics;
    /* 检查诊断复制。 */
    CHECK_EQ(BOARD_RUNTIME_OK, board_runtime_get_diagnostics(&runtime, &diagnostics));
    /* Mock 不是实际硬件。 */
    CHECK_TRUE(!diagnostics.real_backend);
    /* Mock 模拟显示、触摸和三个 I2C 外设就绪。 */
    CHECK_TRUE(diagnostics.display_ready && diagnostics.touch_ready && diagnostics.i2c_ready);
    /* 检查 QMI 地址。 */
    CHECK_EQ(BOARD_RUNTIME_QMI8658_LOW_ADDRESS, diagnostics.qmi_i2c_address);
    /* 获取上层适配器。 */
    board_adapter_t *adapter = board_runtime_adapter(&runtime);
    /* 适配器必须存在。 */
    CHECK_TRUE(adapter != NULL);
    /* Mock 共用 I2C 句柄必须非空，独立驱动可据此完成接口联调。 */
    CHECK_TRUE(board_runtime_i2c_handle(&runtime) != NULL);
    /* 读取 Mock 电池。 */
    uint8_t percent = 0U;
    /* 保存充电输出。 */
    bool charging = false;
    /* 电池回调必须返回配置值。 */
    CHECK_EQ(BOARD_ADAPTER_OK, board_adapter_read_battery(adapter, &percent, &charging));
    /* 检查百分比和充电状态。 */
    CHECK_EQ(76U, percent);
    CHECK_TRUE(charging);
    /* 挂载 Mock TF。 */
    CHECK_EQ(BOARD_RUNTIME_OK, board_runtime_set_storage_active(&runtime, true));
    /* 再读诊断确认挂载。 */
    CHECK_EQ(BOARD_RUNTIME_OK, board_runtime_get_diagnostics(&runtime, &diagnostics));
    CHECK_TRUE(diagnostics.storage_mounted);
    /* 关闭触摸逻辑输入。 */
    CHECK_EQ(BOARD_RUNTIME_OK, board_runtime_set_touch_active(&runtime, false));
    CHECK_TRUE(!runtime.touch_active);
    /* 扬声器默认关闭，可显式打开再关闭。 */
    CHECK_EQ(BOARD_RUNTIME_OK, board_runtime_set_speaker_enabled(&runtime, true));
    CHECK_TRUE(runtime.speaker_enabled);
    CHECK_EQ(BOARD_RUNTIME_OK, board_runtime_set_speaker_enabled(&runtime, false));
    /* 切换 QMI 活动模式。 */
    CHECK_EQ(BOARD_RUNTIME_OK, board_runtime_set_qmi_mode(&runtime, BOARD_RUNTIME_QMI_ACTIVE));
    /* 外部驱动必须收到 ACTIVE，而非模糊的 true。 */
    CHECK_EQ(BOARD_RUNTIME_QMI_ACTIVE, external.qmi_mode);
    /* 切换 QMI 低功耗 WOM 模式。 */
    CHECK_EQ(BOARD_RUNTIME_OK, board_runtime_set_qmi_mode(&runtime, BOARD_RUNTIME_QMI_WAKE_ON_MOTION));
    /* 外部驱动必须收到 WOM。 */
    CHECK_EQ(BOARD_RUNTIME_QMI_WAKE_ON_MOTION, external.qmi_mode);
    /* 切换 QMI 完全关闭模式。 */
    CHECK_EQ(BOARD_RUNTIME_OK, board_runtime_set_qmi_mode(&runtime, BOARD_RUNTIME_QMI_OFF));
    /* 外部驱动必须收到 OFF。 */
    CHECK_EQ(BOARD_RUNTIME_QMI_OFF, external.qmi_mode);
    /* 越界枚举必须在调用回调前被拒绝。 */
    CHECK_EQ(BOARD_RUNTIME_ERR_ARGUMENT, board_runtime_set_qmi_mode(&runtime, (board_runtime_qmi_mode_t)99));
    /* 未启用板型深睡选项时，即使 WOM 驱动成功也不能进入 Deep-sleep。 */
    CHECK_TRUE(!board_runtime_qmi_deep_wake_ready(&runtime, true, true));
    /* 测试显式模拟 Kconfig 已启用 GPIO21 深睡唤醒。 */
    runtime.adapter.profile.enable_imu_deep_wake = true;
    /* 策略未请求 QMI 唤醒时必须拒绝，防止没有任何源的睡死。 */
    CHECK_TRUE(!board_runtime_qmi_deep_wake_ready(&runtime, false, true));
    /* WOM 寄存器配置失败时必须拒绝。 */
    CHECK_TRUE(!board_runtime_qmi_deep_wake_ready(&runtime, true, false));
    /* 板型启用、QMI 存在、GPIO21 合法、策略请求且 WOM 成功时才允许 Deep-sleep。 */
    CHECK_TRUE(board_runtime_qmi_deep_wake_ready(&runtime, true, true));
    /* 非 RTC GPIO 会使 ext1 配置无效；纯逻辑门必须提前拒绝。 */
    runtime.adapter.profile.imu_interrupt_gpio = 38U;
    /* GPIO38 只支持 Light-sleep，不能作为本产品 Deep-sleep 唤醒源。 */
    CHECK_TRUE(!board_runtime_qmi_deep_wake_ready(&runtime, true, true));
    /* 读取 RTC。 */
    uint64_t unix_seconds = 0U;
    CHECK_EQ(BOARD_RUNTIME_OK, board_runtime_read_rtc_unix_seconds(&runtime, &unix_seconds));
    CHECK_TRUE(unix_seconds == external.rtc_seconds);
    /* 请求 PMIC 关机。 */
    CHECK_EQ(BOARD_RUNTIME_OK, board_runtime_request_pmic_shutdown(&runtime));
    CHECK_EQ(1U, external.shutdown_calls);
    /* Mock LVGL 锁可用于 presenter/renderer 联调。 */
    CHECK_TRUE(board_runtime_lvgl_lock(&runtime, 10U));
    /* 释放 Mock 锁。 */
    board_runtime_lvgl_unlock(&runtime);
}

/* 验证全部页面都有稳定 presenter 映射。 */
static void test_ui_presenter_pages(void)
{
    /* 创建上下文并填充典型训练指标。 */
    ui_context_t context;
    /* 初始化冷启动状态。 */
    ui_context_init(&context, 0U);
    /* 填充展示数据。 */
    context.view.action_id = 3U;
    /* 实时分类与本轮跳跃深蹲一致，正常显示训练中。 */
    context.view.inferred_action_id = 3U;
    /* 打开计数门，使既有训练页合同走正常分支。 */
    context.view.counting_enabled = true;
    context.view.count = 12U;
    context.view.calories_milli_kcal = 3456U;
    context.view.elapsed_seconds = 125U;
    context.view.confidence_centipercent = 9234U;
    context.view.battery_percent = 64U;
    context.view.ble_connected = true;
    /* 写入真实设置快照，presenter 不得继续硬编码默认值。 */
    context.view.brightness_percent = 60U;
    context.view.screen_timeout_seconds = 60U;
    context.view.preferences_revision = 7U;
    /* 保存 presenter 输出。 */
    ui_page_model_t page;
    /* 遍历全部真实页面，确保没有遗漏。 */
    for (ui_state_t state = UI_STATE_BOOT; state < UI_STATE_COUNT; state = (ui_state_t)(state + 1)) {
        /* 设置当前页。 */
        context.state = state;
        /* 每页都必须生成成功。 */
        CHECK_TRUE(ui_presenter_build(&context, &page));
    }
    /* 检查训练页动作名和按钮。 */
    context.state = UI_STATE_RUNNING;
    CHECK_TRUE(ui_presenter_build(&context, &page));
    CHECK_TRUE(strcmp(page.primary, "跳跃深蹲") == 0);
    CHECK_EQ(UI_COMMAND_PAUSE, page.buttons[0].command);
    CHECK_EQ(UI_COMMAND_STOP, page.buttons[1].command);
    CHECK_TRUE(strcmp(page.buttons[0].label, "暂停") == 0);
    CHECK_TRUE(strcmp(page.buttons[1].label, "停止") == 0);
    CHECK_TRUE(strstr(page.secondary, "12 次") != NULL);
    CHECK_TRUE(strstr(page.footer, "3.456千卡") != NULL);
    CHECK_TRUE(strstr(page.status, "电量 64%") != NULL);
    CHECK_TRUE(strstr(page.status, "蓝牙 已连") != NULL);
    /* 模拟训练中休息：主动作仍为跳跃深蹲，诊断类别可变成静坐但不参与界面主动作。 */
    context.view.inferred_action_id = 5U;
    /* 模拟活动门关闭，禁止把休息期间腕部噪声送入主动作计数器。 */
    context.view.counting_enabled = false;
    /* 重新构建同一训练页，不切换会话或计数器。 */
    CHECK_TRUE(ui_presenter_build(&context, &page));
    /* 标题必须明确当前不计数。 */
    CHECK_TRUE(strcmp(page.title, "休息  计数暂停") == 0);
    /* 主区域必须保持本轮跳跃深蹲，禁止噪声类别把页面改成“静坐若干次”。 */
    CHECK_TRUE(strcmp(page.primary, "跳跃深蹲") == 0);
    /* 权威累计保持 12 次，不因休息清零。 */
    CHECK_TRUE(strstr(page.secondary, "12 次") != NULL);
    /* 页脚说明恢复本轮主动作后自动继续。 */
    CHECK_TRUE(strcmp(page.footer, "累计保持  恢复完整动作后继续") == 0);
    /* 恢复正常状态，后续页面遍历继续使用一致快照。 */
    context.view.inferred_action_id = 3U;
    /* 同类干净窗口重新允许计数。 */
    context.view.counting_enabled = true;
    /* PREPARE 页面必须立即要求用户做动作，不能再显示会延后动作的 2 秒倒计时。 */
    context.state = UI_STATE_PREPARE;
    CHECK_TRUE(ui_presenter_build(&context, &page));
    CHECK_TRUE(strcmp(page.title, "动作识别") == 0);
    /* 主文案明确点击开始后立即开始动作。 */
    CHECK_TRUE(strcmp(page.primary, "开始做动作") == 0);
    /* 次文案说明设备正在建立本轮单一动作模型。 */
    CHECK_TRUE(strcmp(page.secondary, "正在建立动作模型") == 0);
    /* 页脚必须说明锁类后的自动记录行为，不能暴露内部窗口术语。 */
    CHECK_TRUE(strcmp(page.footer, "右手佩戴  识别后自动记录") == 0);
    /* 停止确认页必须同时提供确认和取消，防止训练中误触直接结束。 */
    context.state = UI_STATE_STOP_CONFIRM;
    CHECK_TRUE(ui_presenter_build(&context, &page));
    CHECK_EQ(UI_COMMAND_CONFIRM_STOP, page.buttons[0].command);
    CHECK_EQ(UI_COMMAND_BACK, page.buttons[1].command);
    /* 检查设置页显示真实快照并提供四个可操作入口。 */
    context.state = UI_STATE_SETTINGS;
    CHECK_TRUE(ui_presenter_build(&context, &page));
    CHECK_TRUE(strstr(page.primary, "60%") != NULL);
    CHECK_TRUE(strstr(page.secondary, "60秒") != NULL);
    CHECK_EQ(UI_COMMAND_CYCLE_BRIGHTNESS, page.buttons[0].command);
    CHECK_EQ(UI_COMMAND_OPEN_DIAGNOSTICS, page.buttons[1].command);
    CHECK_EQ(UI_COMMAND_FORGET_COMPUTER, page.buttons[2].command);
    CHECK_EQ(UI_COMMAND_BACK, page.buttons[3].command);
    CHECK_EQ(UI_COMMAND_NONE, page.buttons[4].command);
    CHECK_TRUE(strcmp(page.buttons[0].label, "亮度") == 0);
    CHECK_TRUE(strcmp(page.buttons[1].label, "诊断") == 0);
    CHECK_TRUE(strcmp(page.buttons[2].label, "忘记电脑") == 0);
    CHECK_TRUE(strcmp(page.buttons[3].label, "返回") == 0);
    CHECK_TRUE(strstr(page.footer, "400毫安时") != NULL);
    /* 检查诊断页质量位图、配置修订号和熄屏门槛入口。 */
    context.state = UI_STATE_DIAGNOSTICS;
    context.view.data_quality_flags = 0x0021U;
    CHECK_TRUE(ui_presenter_build(&context, &page));
    CHECK_TRUE(strstr(page.secondary, "0021") != NULL);
    CHECK_TRUE(strstr(page.footer, "7") != NULL);
    CHECK_EQ(UI_COMMAND_CYCLE_TIMEOUT, page.buttons[0].command);
    CHECK_EQ(UI_COMMAND_BACK, page.buttons[1].command);
    CHECK_EQ(UI_COMMAND_NONE, page.buttons[2].command);
    /* 检查熄屏页保留唯一全屏唤醒命令。 */
    context.state = UI_STATE_SCREEN_OFF;
    CHECK_TRUE(ui_presenter_build(&context, &page));
    CHECK_EQ(UI_COMMAND_WAKE, page.buttons[0].command);
    CHECK_EQ(UI_COMMAND_NONE, page.buttons[1].command);
    /* 故障页必须显示中文故障码、训练阻断说明和唯一安全关机入口。 */
    context.state = UI_STATE_ERROR;
    context.fault_code = -42;
    CHECK_TRUE(ui_presenter_build(&context, &page));
    CHECK_TRUE(strcmp(page.title, "设备故障") == 0);
    CHECK_TRUE(strstr(page.primary, "-42") != NULL);
    CHECK_TRUE(strcmp(page.footer, "训练已禁用") == 0);
    CHECK_EQ(UI_COMMAND_SHUTDOWN, page.buttons[0].command);
    CHECK_EQ(UI_COMMAND_NONE, page.buttons[1].command);
    CHECK_TRUE(strcmp(page.buttons[0].label, "关机") == 0);
    /* 越界动作显示中文等待提示。 */
    CHECK_TRUE(strcmp(ui_action_display_name(UINT8_MAX), "等待识别") == 0);
}

/* 验证配对码跨任务邮箱、纯状态投影、六位格式和全部清除路径。 */
static void test_ui_pairing_mailbox_and_lifecycle(void)
{
    /* 邮箱由应用静态持有；初始化后没有待处理事件。 */
    ui_app_pairing_mailbox_t mailbox;
    /* 清零全部 C11 原子字段并建立初始偶数序列。 */
    ui_app_pairing_mailbox_init(&mailbox);
    /* 保存从邮箱取出的 UI 事件。 */
    ui_event_t event;
    /* 空邮箱不得伪造清除或显示事件。 */
    CHECK_TRUE(!ui_app_pairing_try_take(&mailbox, &event));
    /* 七位值不属于 BLE 六位配对码，发布端必须拒绝。 */
    CHECK_EQ(
        UI_APP_PAIRING_ERR_RANGE,
        ui_app_pairing_publish_code(&mailbox, UINT32_C(1000000), UINT32_C(900)));
    /* 合法值 42 必须保留数值并由 presenter 补齐为 000042。 */
    CHECK_EQ(
        UI_APP_PAIRING_OK,
        ui_app_pairing_publish_code(&mailbox, UINT32_C(42), UINT32_C(1000)));
    /* 单消费者取得一次显示事件。 */
    CHECK_TRUE(ui_app_pairing_try_take(&mailbox, &event));
    /* 事件类型必须为显示配对码。 */
    CHECK_EQ(UI_EVENT_PAIRING_CODE_SHOWN, event.type);
    /* 原始六位数值保持 42，格式化属于 presenter 职责。 */
    CHECK_EQ(UINT32_C(42), event.pairing_code);
    /* 同一事件不得被重复消费。 */
    CHECK_TRUE(!ui_app_pairing_try_take(&mailbox, &event));

    /* 创建主页上下文，验证配对覆盖层不改变底层页面状态。 */
    ui_context_t context;
    /* 从冷启动初始化全部视图字段。 */
    ui_context_init(&context, UINT32_C(0));
    /* 直接设置主页，仅用于纯 presenter 合同测试。 */
    context.state = UI_STATE_HOME;
    /* 分派显示事件，开始 60 秒有效期。 */
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    /* 配对覆盖层必须处于活动状态。 */
    CHECK_TRUE(context.view.pairing_active);
    /* 上下文保存配对码供应用任务向 LVGL 快照投影。 */
    CHECK_EQ(UINT32_C(42), context.view.pairing_code);
    /* 60 秒截止点使用同一 uint32 单调毫秒时基。 */
    CHECK_EQ(UINT32_C(61000), context.view.pairing_expires_ms);
    /* 保存纯页面模型。 */
    ui_page_model_t page;
    /* 配对活动时构建中文覆盖层。 */
    CHECK_TRUE(ui_presenter_build(&context, &page));
    /* 覆盖层标题必须明确属于蓝牙配对。 */
    CHECK_TRUE(strcmp(page.title, "蓝牙配对") == 0);
    /* 主文本必须严格显示六位码，包括前导零。 */
    CHECK_TRUE(strcmp(page.primary, "配对码 000042") == 0);
    /* 次文本明确要求在电脑输入。 */
    CHECK_TRUE(strcmp(page.secondary, "请在电脑输入") == 0);
    /* 页脚告知固定有效期。 */
    CHECK_TRUE(strcmp(page.footer, "60秒内有效") == 0);
    /* 覆盖层期间隐藏全部按钮，防止误触训练或设置命令。 */
    for (size_t button_index = 0U; button_index < UI_PRESENTER_MAX_BUTTONS; ++button_index) {
        /* 每个槽位都必须为空命令。 */
        CHECK_EQ(UI_COMMAND_NONE, page.buttons[button_index].command);
    }
    /* 截止点前一毫秒不得生成超时清除事件。 */
    CHECK_TRUE(!ui_app_pairing_build_timeout_event(&context, UINT32_C(60999), &event));
    /* 到达截止点必须生成带 TIMEOUT 原因的清除事件。 */
    CHECK_TRUE(ui_app_pairing_build_timeout_event(&context, UINT32_C(61000), &event));
    /* 超时清除事件类型必须稳定。 */
    CHECK_EQ(UI_EVENT_PAIRING_CODE_CLEARED, event.type);
    /* 清除原因用于日志，不在屏幕保留旧配对码。 */
    CHECK_EQ(UI_PAIRING_CLEAR_TIMEOUT, event.pairing_clear_reason);
    /* 分派超时事件。 */
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    /* 清除后活动位关闭。 */
    CHECK_TRUE(!context.view.pairing_active);
    /* 清除后配对码归零，禁止敏感码残留在后续快照。 */
    CHECK_EQ(UINT32_C(0), context.view.pairing_code);
    /* 截止时间也必须归零。 */
    CHECK_EQ(UINT32_C(0), context.view.pairing_expires_ms);

    /* 再次显示 654321，验证安全连接成功会通过 BLE_CHANGED 自动清码。 */
    CHECK_EQ(
        UI_APP_PAIRING_OK,
        ui_app_pairing_publish_code(&mailbox, UINT32_C(654321), UINT32_C(70000)));
    /* 取得第二次显示事件。 */
    CHECK_TRUE(ui_app_pairing_try_take(&mailbox, &event));
    /* 写入上下文。 */
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    /* 构造安全连接成功事件。 */
    (void)memset(&event, 0, sizeof(event));
    /* BLE true 表示加密、MITM 与绑定均已完成。 */
    event.type = UI_EVENT_BLE_CHANGED;
    /* 保存连接成功单调时刻。 */
    event.monotonic_ms = UINT32_C(70500);
    /* 写入已连接事实。 */
    event.flag = true;
    /* 分派连接成功事件。 */
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    /* 成功后配对码必须立即清除。 */
    CHECK_TRUE(!context.view.pairing_active && (context.view.pairing_code == UINT32_C(0)));

    /* 发布第三个码并验证显式失败清除事件。 */
    CHECK_EQ(
        UI_APP_PAIRING_OK,
        ui_app_pairing_publish_code(&mailbox, UINT32_C(7), UINT32_C(80000)));
    /* 消费第三个显示事件。 */
    CHECK_TRUE(ui_app_pairing_try_take(&mailbox, &event));
    /* 分派显示事件。 */
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    /* NimBLE 失败路径发布明确清除原因。 */
    CHECK_EQ(
        UI_APP_PAIRING_OK,
        ui_app_pairing_publish_clear(
            &mailbox,
            UI_PAIRING_CLEAR_FAILED,
            UINT32_C(80100)));
    /* 消费失败清除。 */
    CHECK_TRUE(ui_app_pairing_try_take(&mailbox, &event));
    /* 分派失败清除。 */
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    /* 失败后不得保留码 000007。 */
    CHECK_TRUE(!context.view.pairing_active && (context.view.pairing_code == UINT32_C(0)));

    /* 再次显示后，断线事件即使连接尚未安全上报，也必须兜底清码。 */
    event.type = UI_EVENT_PAIRING_CODE_SHOWN;
    /* 保存最后一个测试码。 */
    event.pairing_code = UINT32_C(123456);
    /* 保存显示时刻。 */
    event.monotonic_ms = UINT32_C(90000);
    /* 分派显示。 */
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    /* 构造断线。 */
    event.type = UI_EVENT_BLE_CHANGED;
    /* 断线时连接标志为 false。 */
    event.flag = false;
    /* 保存断线时刻。 */
    event.monotonic_ms = UINT32_C(90100);
    /* 分派断线。 */
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    /* 断线后清码。 */
    CHECK_TRUE(!context.view.pairing_active && (context.view.pairing_code == UINT32_C(0)));
}

/* 验证设置、诊断和按钮命令映射。 */
static void test_ui_navigation_commands(void)
{
    /* 创建并推进到主页。 */
    ui_context_t context;
    ui_context_init(&context, 0U);
    /* 构造启动完成事件。 */
    ui_event_t event = {.type = UI_EVENT_BOOT_READY, .monotonic_ms = 800U};
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    /* 自检通过。 */
    event.type = UI_EVENT_SELF_TEST_OK;
    event.monotonic_ms = 900U;
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    /* SETTINGS 命令映射为 OPEN_SETTINGS。 */
    CHECK_TRUE(ui_command_to_event(UI_COMMAND_OPEN_SETTINGS, 1000U, &event));
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    CHECK_EQ(UI_STATE_SETTINGS, context.state);
    /* DIAGNOSTICS 命令进入诊断页。 */
    CHECK_TRUE(ui_command_to_event(UI_COMMAND_OPEN_DIAGNOSTICS, 1100U, &event));
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    CHECK_EQ(UI_STATE_DIAGNOSTICS, context.state);
    /* BACK 返回设置，再次 BACK 返回主页。 */
    CHECK_TRUE(ui_command_to_event(UI_COMMAND_BACK, 1200U, &event));
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    CHECK_EQ(UI_STATE_SETTINGS, context.state);
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    CHECK_EQ(UI_STATE_HOME, context.state);
    /* START 进入 PREPARE。 */
    CHECK_TRUE(ui_command_to_event(UI_COMMAND_START, 1210U, &event));
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    /* 模拟动作锁定进入 RUNNING。 */
    event.type = UI_EVENT_PREPARE_COMPLETED;
    event.monotonic_ms = 4210U;
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    /* 屏幕 STOP 只打开确认页，不直接结束。 */
    CHECK_TRUE(ui_command_to_event(UI_COMMAND_STOP, 4220U, &event));
    CHECK_EQ(UI_EVENT_STOP_CONFIRM_REQUESTED, event.type);
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    CHECK_EQ(UI_STATE_STOP_CONFIRM, context.state);
    /* BACK 取消确认并恢复原 RUNNING 页面。 */
    CHECK_TRUE(ui_command_to_event(UI_COMMAND_BACK, 4230U, &event));
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    CHECK_EQ(UI_STATE_RUNNING, context.state);
    /* 第二次打开后由 CONFIRM_STOP 产生真正的停止事件。 */
    CHECK_TRUE(ui_command_to_event(UI_COMMAND_STOP, 4240U, &event));
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    CHECK_TRUE(ui_command_to_event(UI_COMMAND_CONFIRM_STOP, 4250U, &event));
    CHECK_EQ(UI_EVENT_STOP_REQUESTED, event.type);
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    CHECK_EQ(UI_STATE_SUMMARY, context.state);
    /* 返回主页，为后续熄屏导航用例建立无会话状态。 */
    event.type = UI_EVENT_SUMMARY_SAVED;
    event.monotonic_ms = 4260U;
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    CHECK_EQ(UI_STATE_HOME, context.state);
    /* 先让主页进入熄屏状态。 */
    event.type = UI_EVENT_SCREEN_TIMEOUT;
    event.monotonic_ms = 1250U;
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    CHECK_EQ(UI_STATE_SCREEN_OFF, context.state);
    /* WAKE 命令恢复熄屏前主页。 */
    CHECK_TRUE(ui_command_to_event(UI_COMMAND_WAKE, 1260U, &event));
    CHECK_EQ(UI_DISPATCH_OK, ui_dispatch_event(&context, &event));
    CHECK_EQ(UI_STATE_HOME, context.state);
    /* NONE 不生成事件。 */
    CHECK_TRUE(!ui_command_to_event(UI_COMMAND_NONE, 1300U, &event));
    /* 忘记电脑属于 main 应用事务，不直接映射为页面状态事件。 */
    CHECK_TRUE(!ui_command_to_event(UI_COMMAND_FORGET_COMPUTER, 1310U, &event));
}

/* 运行全部主机测试。 */
int main(void)
{
    /* 验证板级运行时 Mock。 */
    test_board_runtime_mock();
    /* 验证全部页面模型。 */
    test_ui_presenter_pages();
    /* 验证设置/诊断导航。 */
    test_ui_navigation_commands();
    /* 验证配对码跨任务显示、超时和清除生命周期。 */
    test_ui_pairing_mailbox_and_lifecycle();
    /* 失败时打印数量并返回非零。 */
    if (g_failures != 0) {
        (void)fprintf(stderr, "board_ui_runtime_tests failed=%d\n", g_failures);
        return 1;
    }
    /* 输出稳定通过标记。 */
    (void)printf("board_ui_runtime_tests passed\n");
    /* 返回成功。 */
    return 0;
}
