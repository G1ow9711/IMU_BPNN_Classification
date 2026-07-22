/* 引入纯显示模型合同。 */
#include "ui_presenter.h"

/* 引入格式化和内存清零函数。 */
#include <stdio.h>
/* 引入可变参数宏，固定缓冲格式化函数使用。 */
#include <stdarg.h>
/* 引入 memset，页面切换前清除旧字符串、按钮命令和可用状态。 */
#include <string.h>

/* 固定 11 类显示名称，顺序必须与模型输出类别完全一致。 */
static const char *const UI_ACTION_NAMES[UI_ACTION_CLASS_COUNT] = {
    /* 模型输出索引 0：早安式体前屈。 */
    "早安式体前屈",
    /* 模型输出索引 1：开合跳。 */
    "开合跳",
    /* 模型输出索引 2：跳跃弓步。 */
    "跳跃弓步",
    /* 模型输出索引 3：跳跃深蹲。 */
    "跳跃深蹲",
    /* 模型输出索引 4：普通弓步。 */
    "弓步",
    /* 模型输出索引 5：静坐。 */
    "静坐",
    /* 模型输出索引 6：普通深蹲。 */
    "深蹲",
    /* 模型输出索引 7：小跑。 */
    "小跑",
    /* 模型输出索引 8：收腹跳。 */
    "收腹跳",
    /* 模型输出索引 9：行走。 */
    "行走",
    /* 模型输出索引 10：挥手。 */
    "挥手",
};

/* 安全写入固定缓冲；format 和目标都必须由本文件提供有效值。 */
static void ui_write_text(char *target, size_t capacity, const char *format, ...)
{
    /* 创建可变参数读取器。 */
    va_list arguments;
    /* 从 format 后的第一个参数开始读取。 */
    va_start(arguments, format);
    /* 使用 vsnprintf 保证目标以 NUL 终止且不越界。 */
    (void)vsnprintf(target, capacity, format, arguments);
    /* 结束可变参数读取。 */
    va_end(arguments);
}

/* 设置一个按钮槽位。 */
static void ui_set_button(
    ui_page_model_t *page,
    size_t index,
    ui_command_t command,
    const char *label,
    bool enabled)
{
    /* 索引越界时忽略，防止未来页面配置写坏相邻字段。 */
    if ((page == NULL) || (index >= UI_PRESENTER_MAX_BUTTONS)) {
        /* 无合法按钮槽位时不修改页面模型。 */
        return;
    }
    /* 保存按钮命令。 */
    page->buttons[index].command = command;
    /* 保存按钮可用状态。 */
    page->buttons[index].enabled = enabled;
    /* 复制标签并保证结尾 NUL。 */
    ui_write_text(
        page->buttons[index].label,
        sizeof(page->buttons[index].label),
        "%s",
        label);
}

/* 返回动作显示名。 */
const char *ui_action_display_name(uint8_t action_id)
{
    /* 合法索引直接读取静态表。 */
    if (action_id < UI_ACTION_CLASS_COUNT) {
        /* 返回与模型类别索引一一对应的只读中文显示名。 */
        return UI_ACTION_NAMES[action_id];
    }
    /* 未稳定动作或损坏索引统一显示中文等待提示。 */
    return "等待识别";
}

/* 格式化通用顶栏状态。 */
static void ui_format_status(const ui_context_t *context, ui_page_model_t *page)
{
    /* 电量 255 表示 PMIC 尚未给出有效值。 */
    if (context->view.battery_percent == UINT8_MAX) {
        /* 未知电量时显示占位符，但仍显示当前 BLE 连接状态。 */
        ui_write_text(
            page->status,
            sizeof(page->status),
            "电量 --%%  蓝牙 %s",
            context->view.ble_connected ? "已连" : "未连");
        /* 未知电量格式已经完成，避免继续按 255% 显示。 */
        return;
    }
    /* 显示电量、充电符号和 BLE 状态。 */
    ui_write_text(
        page->status,
        sizeof(page->status),
        "电量 %u%%%s  蓝牙 %s",
        (unsigned int)context->view.battery_percent,
        context->view.charging ? " 充" : "",
        context->view.ble_connected ? "已连" : "未连");
}

/* 把当前页覆盖成六位 BLE 配对码页；底层状态保持不变，清码后立即恢复原页。 */
static void ui_apply_pairing_overlay(const ui_context_t *context, ui_page_model_t *page)
{
    /* 标题明确该六位码属于蓝牙配对，而不是设备解锁密码。 */
    ui_write_text(page->title, sizeof(page->title), "蓝牙配对");
    /* %06lu 保留前导零，例如数值 42 必须显示为 000042。 */
    ui_write_text(
        page->primary,
        sizeof(page->primary),
        "配对码 %06lu",
        (unsigned long)context->view.pairing_code);
    /* 次文本告知用户操作位于 PC 系统配对框。 */
    ui_write_text(page->secondary, sizeof(page->secondary), "请在电脑输入");
    /* 顶栏继续显示电量和蓝牙连接状态，不重复敏感配对码。 */
    /* 页脚告知固定 60 秒有效期，超时由应用任务分发清除事件。 */
    ui_write_text(page->footer, sizeof(page->footer), "60秒内有效");
    /* 遍历五个按钮槽位，配对期间全部隐藏以防误触训练或设置事务。 */
    for (size_t index = 0U; index < UI_PRESENTER_MAX_BUTTONS; ++index) {
        /* 清除按钮命令，渲染层会隐藏 NONE 槽位。 */
        page->buttons[index].command = UI_COMMAND_NONE;
        /* 禁用按钮，即使渲染层短暂保留旧对象也不会产生命令。 */
        page->buttons[index].enabled = false;
        /* 清空旧标签，防止快照保留“开始”或“忘记电脑”。 */
        page->buttons[index].label[0] = '\0';
    }
}

/* 构建页面纯模型。 */
bool ui_presenter_build(const ui_context_t *context, ui_page_model_t *page)
{
    /* 输入和输出不能为空，状态必须位于固定数组范围内。 */
    if ((context == NULL) || (page == NULL) || (context->state >= UI_STATE_COUNT)) {
        /* 返回 false，调用方保持上一帧并记录非法页面状态。 */
        return false;
    }
    /* 清零全部字符串和按钮，未使用按钮自然隐藏。 */
    (void)memset(page, 0, sizeof(*page));
    /* 先写入所有页面共享的电池/BLE 状态。 */
    ui_format_status(context, page);
    /* 按页面生成标题、指标和按钮。 */
    switch (context->state) {
        case UI_STATE_BOOT:
            /* 开机页标题显示产品名。 */
            ui_write_text(page->title, sizeof(page->title), "训练助手");
            /* 开机页主区域使用产品状态，不向用户暴露内部联调日期或阶段编号。 */
            ui_write_text(page->primary, sizeof(page->primary), "正在启动");
            /* 开机页副区域说明设备正在建立传感器、显示和蓝牙能力。 */
            ui_write_text(page->secondary, sizeof(page->secondary), "正在准备设备");
            /* 开机页脚保持简短，避免重复主状态。 */
            ui_write_text(page->footer, sizeof(page->footer), "请稍候");
            /* BOOT 页面不提供按钮，等待状态机自动进入自检。 */
            break;
        case UI_STATE_SELF_TEST:
            /* 自检页标题。 */
            ui_write_text(page->title, sizeof(page->title), "设备自检");
            /* 自检页主区域说明当前阶段。 */
            ui_write_text(page->primary, sizeof(page->primary), "正在检查硬件");
            /* 列出需要通过的显示、触摸、IMU 和存储项目。 */
            ui_write_text(page->secondary, sizeof(page->secondary), "屏幕  触摸  惯导  存储");
            /* 自检期间禁止用户启动训练。 */
            ui_write_text(page->footer, sizeof(page->footer), "请稍候");
            /* SELF_TEST 页面等待硬件结果，不提供按钮。 */
            break;
        case UI_STATE_HOME:
            /* 主页显示产品功能名称。 */
            ui_write_text(page->title, sizeof(page->title), "训练助手");
            /* 主页主区域明确唯一首要操作，避免工程版本号占据视觉中心。 */
            ui_write_text(page->primary, sizeof(page->primary), "准备训练");
            /* 副区域固定当前模型的右手腕佩戴合同和可识别动作数量。 */
            ui_write_text(page->secondary, sizeof(page->secondary), "右手腕 / 11种动作");
            /* 页脚只保留下一步操作，不显示内部常亮联调状态。 */
            ui_write_text(page->footer, sizeof(page->footer), "点击开始");
            /* 第一个按钮启动准备倒计时。 */
            ui_set_button(page, 0U, UI_COMMAND_START, "开始", true);
            /* 第二个按钮进入设置页。 */
            ui_set_button(page, 1U, UI_COMMAND_OPEN_SETTINGS, "设置", true);
            /* 第三个按钮请求保存并安全关机。 */
            ui_set_button(page, 2U, UI_COMMAND_SHUTDOWN, "关机", true);
            /* 主页模型构建完成。 */
            break;
        case UI_STATE_PREPARE:
            /* 准备页直接进入动作识别，不再插入会误导用户等待的倒计时。 */
            ui_write_text(page->title, sizeof(page->title), "正在识别");
            /* 主文案要求用户点击开始后立即做本会话唯一动作。 */
            ui_write_text(page->primary, sizeof(page->primary), "开始运动");
            /* 次文案再次提醒固定右手腕，避免佩戴域变化造成分类偏差。 */
            ui_write_text(page->secondary, sizeof(page->secondary), "保持右手佩戴");
            /* 页脚说明识别完成后的自动行为；准备期已经持续采样并缓存。 */
            ui_write_text(page->footer, sizeof(page->footer), "识别后自动记录");
            /* 动作尚未锁定时只允许取消本次会话。 */
            ui_set_button(page, 0U, UI_COMMAND_CANCEL, "取消", true);
            /* 准备页模型构建完成。 */
            break;
        case UI_STATE_RUNNING: {
            /* sit 使用秒，walk/trot 使用步，其它动作使用次数。 */
            const char *unit = context->view.action_id == 5U
                ? "秒"
                : ((context->view.action_id == 7U) || (context->view.action_id == 9U)
                    ? "步"
                    : "次");
            /* 实时类别有效时显示真实识别动作；未知时使用已有“正在识别”文案。 */
            const char *live_action_name = context->view.inferred_action_id < UI_ACTION_CLASS_COUNT
                ? ui_action_display_name(context->view.inferred_action_id)
                : "正在识别";
            /* 计数门关闭时明确显示暂停，避免把站立或静坐伪装为持续主动作。 */
            ui_write_text(
                page->title,
                sizeof(page->title),
                "%s",
                context->view.counting_enabled ? "训练中" : "计数已暂停");
            /* 计数允许时显示主动作；冻结时显示最近实时类别，二者角色不再混淆。 */
            ui_write_text(
                page->primary,
                sizeof(page->primary),
                "%s",
                context->view.counting_enabled
                    ? ui_action_display_name(context->view.action_id)
                    : live_action_name);
            /* 副区域显示动作对应指标和设备累计卡路里。 */
            ui_write_text(
                page->secondary,
                sizeof(page->secondary),
                "%lu %s   %.3f 千卡",
                (unsigned long)(context->view.action_id == 5U
                    ? context->view.elapsed_seconds
                    : context->view.count),
                unit,
                (double)context->view.calories_milli_kcal / 1000.0);
            /* 计数状态决定页脚：正常时显示时间/置信度，冻结时说明恢复条件。 */
            if (context->view.counting_enabled) {
                /* 正常计数页脚显示训练时长和百分之一百分比置信度。 */
                ui_write_text(
                    page->footer,
                    sizeof(page->footer),
                    "时间 %02lu:%02lu  置信度 %u.%02u%%",
                    (unsigned long)(context->view.elapsed_seconds / 60U),
                    (unsigned long)(context->view.elapsed_seconds % 60U),
                    (unsigned int)(context->view.confidence_centipercent / 100U),
                    (unsigned int)(context->view.confidence_centipercent % 100U));
            } else {
                /* 使用已有字体字符说明同类恢复后自动继续，不改变本轮计数器类型。 */
                ui_write_text(
                    page->footer,
                    sizeof(page->footer),
                    "继续%s后自动记录",
                    ui_action_display_name(context->view.action_id));
            }
            /* 第一个按钮暂停计数和热量时钟。 */
            ui_set_button(page, 0U, UI_COMMAND_PAUSE, "暂停", true);
            /* 第二个按钮结束会话并进入总结。 */
            ui_set_button(page, 1U, UI_COMMAND_STOP, "停止", true);
            /* 训练页模型构建完成。 */
            break;
        }
        case UI_STATE_PAUSED:
            /* 暂停页标题明确当前训练被冻结。 */
            ui_write_text(page->title, sizeof(page->title), "已暂停");
            /* 保留锁定动作名，恢复时不重新猜动作。 */
            ui_write_text(page->primary, sizeof(page->primary), "%s", ui_action_display_name(context->view.action_id));
            /* 显示暂停前权威累计次数和卡路里。 */
            ui_write_text(page->secondary, sizeof(page->secondary), "%lu 次   %.3f 千卡", (unsigned long)context->view.count, (double)context->view.calories_milli_kcal / 1000.0);
            /* 页脚说明暂停期间不计数。 */
            ui_write_text(page->footer, sizeof(page->footer), "计数已暂停");
            /* 第一个按钮恢复原会话。 */
            ui_set_button(page, 0U, UI_COMMAND_RESUME, "继续", true);
            /* 第二个按钮结束并保存当前会话。 */
            ui_set_button(page, 1U, UI_COMMAND_STOP, "停止", true);
            /* 暂停页模型构建完成。 */
            break;
        case UI_STATE_STOP_CONFIRM:
            /* 标题明确这是停止二次确认，不是会话已结束。 */
            ui_write_text(page->title, sizeof(page->title), "停止训练？");
            /* 显示当前已锁定动作，帮助用户判断是否误触。 */
            ui_write_text(
                page->primary,
                sizeof(page->primary),
                "%s",
                ui_action_display_name(context->view.action_id));
            /* 保留停止前权威指标，确认前不清零。 */
            ui_write_text(
                page->secondary,
                sizeof(page->secondary),
                "%lu 次合计   %.3f 千卡",
                (unsigned long)context->view.count,
                (double)context->view.calories_milli_kcal / 1000.0);
            /* 说明确认会保存摘要。 */
            ui_write_text(page->footer, sizeof(page->footer), "确认后保存训练记录");
            /* 第一个按钮执行真正停止。 */
            ui_set_button(page, 0U, UI_COMMAND_CONFIRM_STOP, "停止", true);
            /* 第二个按钮取消并恢复 RUNNING 或 PAUSED。 */
            ui_set_button(page, 1U, UI_COMMAND_BACK, "取消", true);
            /* 确认页模型构建完成。 */
            break;
        case UI_STATE_SUMMARY:
            /* 总结页标题。 */
            ui_write_text(page->title, sizeof(page->title), "训练总结");
            /* 主区域显示本次权威累计指标。 */
            ui_write_text(page->primary, sizeof(page->primary), "%lu 次合计", (unsigned long)context->view.count);
            /* 副区域显示卡路里和总时长。 */
            ui_write_text(page->secondary, sizeof(page->secondary), "%.3f 千卡   %02lu:%02lu", (double)context->view.calories_milli_kcal / 1000.0, (unsigned long)(context->view.elapsed_seconds / 60U), (unsigned long)(context->view.elapsed_seconds % 60U));
            /* 页脚确认摘要已进入本地保存链。 */
            ui_write_text(page->footer, sizeof(page->footer), "训练记录已本地保存");
            /* DONE 返回主页，不重复提交同一摘要。 */
            ui_set_button(page, 0U, UI_COMMAND_DONE, "完成", true);
            /* 总结页模型构建完成。 */
            break;
        case UI_STATE_SETTINGS:
            /* 设置页标题。 */
            ui_write_text(page->title, sizeof(page->title), "设置");
            /* 显示 NVS 恢复或 PC/触屏刚保存的真实亮度。 */
            ui_write_text(
                page->primary,
                sizeof(page->primary),
                "屏幕亮度 %u%%",
                (unsigned int)context->view.brightness_percent);
            /* 同时显示真实振动开关和熄屏秒数。 */
            ui_write_text(
                page->secondary,
                sizeof(page->secondary),
                "振动 %s   熄屏 %u秒",
                context->view.haptic_enabled ? "开" : "关",
                (unsigned int)context->view.screen_timeout_seconds);
            /* 显示厂家原配电池和本地保存事实。 */
            ui_write_text(page->footer, sizeof(page->footer), "已本地保存 / 原配400毫安时");
            /* 第一个按钮循环 AMOLED 用户亮度。 */
            ui_set_button(page, 0U, UI_COMMAND_CYCLE_BRIGHTNESS, "亮度", true);
            /* 第二个按钮切换振动偏好。 */
            ui_set_button(page, 1U, UI_COMMAND_TOGGLE_HAPTIC, "振动", true);
            /* 第三个按钮进入设备诊断。 */
            ui_set_button(page, 2U, UI_COMMAND_OPEN_DIAGNOSTICS, "诊断", true);
            /* 第四个按钮请求删除 NimBLE 保存的全部 PC 绑定。 */
            ui_set_button(page, 3U, UI_COMMAND_FORGET_COMPUTER, "忘记电脑", true);
            /* 第五个按钮返回主页；安全关机仍保留在主页和实体 PWR 键。 */
            ui_set_button(page, 4U, UI_COMMAND_BACK, "返回", true);
            /* 设置页模型构建完成。 */
            break;
        case UI_STATE_DIAGNOSTICS:
            /* 诊断页标题。 */
            ui_write_text(page->title, sizeof(page->title), "设备诊断");
            /* 能进入本页表示启动时显示、触摸、IMU、RTC、PMIC、存储和模型合同均已通过。 */
            ui_write_text(page->primary, sizeof(page->primary), "开机检查通过");
            /* 显示运行期质量位、BLE 事实和当前熄屏门槛。 */
            ui_write_text(
                page->secondary,
                sizeof(page->secondary),
                "质量码 %04X  蓝牙 %s  %u秒",
                (unsigned int)context->view.data_quality_flags,
                context->view.ble_connected ? "已连" : "未连",
                (unsigned int)context->view.screen_timeout_seconds);
            /* 显示当前偏好修订号，便于与 PC 诊断页核对。 */
            ui_write_text(
                page->footer,
                sizeof(page->footer),
                "设置版本 %lu",
                (unsigned long)context->view.preferences_revision);
            /* 第一个按钮发送一次 30 ms 马达测试脉冲。 */
            ui_set_button(page, 0U, UI_COMMAND_TEST_HAPTIC, "测试振动", true);
            /* 第二个按钮循环自动熄屏秒数。 */
            ui_set_button(page, 1U, UI_COMMAND_CYCLE_TIMEOUT, "熄屏", true);
            /* 第三个按钮返回设置页。 */
            ui_set_button(page, 2U, UI_COMMAND_BACK, "返回", true);
            /* 诊断页模型构建完成。 */
            break;
        case UI_STATE_SCREEN_OFF:
            /* 熄屏模型清空标题，避免隐藏页面保留旧像素。 */
            ui_write_text(page->title, sizeof(page->title), "");
            /* 熄屏模型清空主文本。 */
            ui_write_text(page->primary, sizeof(page->primary), "");
            /* 熄屏模型清空顶栏；硬件层随后关闭 AMOLED。 */
            ui_write_text(page->status, sizeof(page->status), "");
            /* 熄屏页保留一个不可见的全屏唤醒命令；AMOLED 已关闭，所以不会产生额外发光。 */
            ui_set_button(page, 0U, UI_COMMAND_WAKE, "", true);
            /* 熄屏页模型构建完成。 */
            break;
        case UI_STATE_ERROR:
            /* 错误页标题使用高可见文字。 */
            ui_write_text(page->title, sizeof(page->title), "设备故障");
            /* 主区域显示稳定故障码，便于串口与上位机对照。 */
            ui_write_text(page->primary, sizeof(page->primary), "故障码 %ld", (long)context->fault_code);
            /* 提示重启后再进入诊断。 */
            ui_write_text(page->secondary, sizeof(page->secondary), "重启后打开设备诊断");
            /* 错误状态禁止开始训练。 */
            ui_write_text(page->footer, sizeof(page->footer), "训练已禁用");
            /* 错误页只允许安全关机。 */
            ui_set_button(page, 0U, UI_COMMAND_SHUTDOWN, "关机", true);
            /* 错误页模型构建完成。 */
            break;
        case UI_STATE_SHUTDOWN:
            /* 关机动画保留产品名。 */
            ui_write_text(page->title, sizeof(page->title), "健身助手");
            /* 主区域确认会话已保存。 */
            ui_write_text(page->primary, sizeof(page->primary), "训练记录已保存");
            /* 副区域说明 PMIC 即将断电。 */
            ui_write_text(page->secondary, sizeof(page->secondary), "正在关机...");
            /* 页脚显示简短结束语。 */
            ui_write_text(page->footer, sizeof(page->footer), "下次训练见");
            /* 关机动画期间不再接受按钮。 */
            break;
        case UI_STATE_COUNT:
        default:
            /* 哨兵值或未知状态没有合法页面模型。 */
            return false;
    }
    /* 活动配对码优先覆盖当前业务页，但不修改 context->state 或会话指标。 */
    if (context->view.pairing_active) {
        /* 显示六位码并隐藏全部按钮。 */
        ui_apply_pairing_overlay(context, page);
    }
    /* 页面模型生成成功。 */
    return true;
}

/* 把按钮命令转为状态机事件。 */
bool ui_command_to_event(ui_command_t command, uint32_t monotonic_ms, ui_event_t *event)
{
    /* 输出不能为空。 */
    if (event == NULL) {
        /* 无法写出事件时返回失败。 */
        return false;
    }
    /* 清零未使用负载，避免旧栈数据进入状态机。 */
    (void)memset(event, 0, sizeof(*event));
    /* 保存单调毫秒时间。 */
    event->monotonic_ms = monotonic_ms;
    /* 按按钮意图映射状态机事件。 */
    switch (command) {
        /* START 请求进入准备阶段。 */
        case UI_COMMAND_START:
            /* 写入开始训练事件。 */
            event->type = UI_EVENT_START_REQUESTED;
            /* 当前命令映射完成。 */
            break;
        /* CANCEL 复用停止事件退出准备阶段。 */
        case UI_COMMAND_CANCEL:
            /* 写入停止事件。 */
            event->type = UI_EVENT_STOP_REQUESTED;
            /* 当前命令映射完成。 */
            break;
        /* PAUSE 冻结当前会话。 */
        case UI_COMMAND_PAUSE:
            /* 写入暂停事件。 */
            event->type = UI_EVENT_PAUSE_REQUESTED;
            /* 当前命令映射完成。 */
            break;
        /* RESUME 恢复原会话。 */
        case UI_COMMAND_RESUME:
            /* 写入恢复事件。 */
            event->type = UI_EVENT_RESUME_REQUESTED;
            /* 当前命令映射完成。 */
            break;
        /* STOP 首次点击只打开确认页，不直接修改 workout。 */
        case UI_COMMAND_STOP:
            /* 写入停止确认请求。 */
            event->type = UI_EVENT_STOP_CONFIRM_REQUESTED;
            /* 当前命令映射完成。 */
            break;
        /* CONFIRM_STOP 才执行真正停止并进入摘要保存链。 */
        case UI_COMMAND_CONFIRM_STOP:
            /* 写入领域协调器识别的停止事件。 */
            event->type = UI_EVENT_STOP_REQUESTED;
            /* 当前命令映射完成。 */
            break;
        /* DONE 表示用户已看完摘要。 */
        case UI_COMMAND_DONE:
            /* 写入摘要完成事件。 */
            event->type = UI_EVENT_SUMMARY_SAVED;
            /* 当前命令映射完成。 */
            break;
        /* OPEN_SETTINGS 打开设置页。 */
        case UI_COMMAND_OPEN_SETTINGS:
            /* 写入设置页事件。 */
            event->type = UI_EVENT_OPEN_SETTINGS;
            /* 当前命令映射完成。 */
            break;
        /* OPEN_DIAGNOSTICS 打开诊断页。 */
        case UI_COMMAND_OPEN_DIAGNOSTICS:
            /* 写入诊断页事件。 */
            event->type = UI_EVENT_OPEN_DIAGNOSTICS;
            /* 当前命令映射完成。 */
            break;
        /* BACK 返回上一级页面。 */
        case UI_COMMAND_BACK:
            /* 写入返回事件。 */
            event->type = UI_EVENT_BACK_REQUESTED;
            /* 当前命令映射完成。 */
            break;
        /* WAKE 请求恢复进入熄屏前保存的页面。 */
        case UI_COMMAND_WAKE:
            /* 写入用户唤醒事件。 */
            event->type = UI_EVENT_USER_WAKE;
            /* 当前命令映射完成。 */
            break;
        /* SHUTDOWN 复用长按电源语义进入安全关机。 */
        case UI_COMMAND_SHUTDOWN:
            /* 写入电源长按事件。 */
            event->type = UI_EVENT_POWER_LONG_PRESS;
            /* 当前命令映射完成。 */
            break;
        /* 五个设备设置/诊断命令由 main 应用任务直接处理，不映射为纯页面事件。 */
        case UI_COMMAND_CYCLE_BRIGHTNESS:
        case UI_COMMAND_TOGGLE_HAPTIC:
        case UI_COMMAND_CYCLE_TIMEOUT:
        case UI_COMMAND_TEST_HAPTIC:
        case UI_COMMAND_FORGET_COMPUTER:
            /* 返回 false 让上层保留命令原值并进入设备配置事务。 */
            return false;
        case UI_COMMAND_NONE:
        default:
            /* 无命令和未知命令不能生成状态事件。 */
            return false;
    }
    /* 映射成功。 */
    return true;
}
