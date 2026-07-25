/* 引入 UI 公开合同；状态机不依赖 LVGL 和 ESP-IDF，便于主机单测。 */
#include "ui_state_machine.h"

/* 使用标准 NULL 定义完成参数检查。 */
#include <stddef.h>

/* 统一切换页面并记录进入时间，避免各分支遗漏 screen_on 更新。 */
static void ui_transition(ui_context_t *context, ui_state_t next_state, uint32_t monotonic_ms)
{
    /* 写入目标页面，渲染任务下一轮据此创建/切换 LVGL 页面。 */
    context->state = next_state;
    /* 根据页面属性更新逻辑屏幕开关；物理面板由 power_manager 执行。 */
    context->screen_on = ui_state_requires_display(next_state);
    /* 记录状态进入时刻，供倒计时、动画和超时诊断使用。 */
    context->state_entered_ms = monotonic_ms;
}

/* 初始化 UI 上下文；调用方必须提供静态或长期有效存储。 */
void ui_context_init(ui_context_t *context, uint32_t monotonic_ms)
{
    /* 空指针没有可写目标，直接返回；公开分派函数仍会报告参数错误。 */
    if (context == NULL) {
        return;
    }
    /* 冷启动固定从 BOOT 页面开始。 */
    context->state = UI_STATE_BOOT;
    /* 默认恢复页设为 HOME，避免异常熄屏唤醒进入未定义状态。 */
    context->screen_off_resume_state = UI_STATE_HOME;
    /* 停止确认取消默认回 RUNNING；真实进入确认时会覆盖为 RUNNING 或 PAUSED。 */
    context->stop_confirm_resume_state = UI_STATE_RUNNING;
    /* 冷启动时没有活动训练会话。 */
    context->session_active = false;
    /* BOOT 动画需要 AMOLED 点亮。 */
    context->screen_on = true;
    /* 保存冷启动单调时刻。 */
    context->state_entered_ms = monotonic_ms;
    /* 255 表示本轮主动作尚未选择，不与 0~10 类别冲突。 */
    context->view.action_id = UINT8_MAX;
    /* 冷启动尚无实时推理类别。 */
    context->view.inferred_action_id = UINT8_MAX;
    /* 冷启动没有运行中的计数许可。 */
    context->view.counting_enabled = false;
    /* 新会话计数从零开始。 */
    context->view.count = 0U;
    /* 新会话热量从零毫卡开始。 */
    context->view.calories_milli_kcal = 0U;
    /* 新会话运行时间从零秒开始。 */
    context->view.elapsed_seconds = 0U;
    /* 纯状态机使用冻结默认 35%，生产入口会用 NVS 配置覆盖。 */
    context->view.brightness_percent = 35U;
    /* 纯状态机默认 30 秒熄屏，生产入口会用 NVS 配置覆盖。 */
    context->view.screen_timeout_seconds = 30U;
    /* 冻结默认偏好修订从 1 开始。 */
    context->view.preferences_revision = 1U;
    /* 没有模型输出时置信度为零。 */
    context->view.confidence_centipercent = 0U;
    /* 255 表示 PMIC 尚未完成读取。 */
    context->view.battery_percent = UINT8_MAX;
    /* 冷启动时尚未建立 BLE 连接。 */
    context->view.ble_connected = false;
    /* 冷启动不显示配对码。 */
    context->view.pairing_active = false;
    /* 清零敏感配对码。 */
    context->view.pairing_code = UINT32_C(0);
    /* 冷启动没有配对截止时间。 */
    context->view.pairing_expires_ms = UINT32_C(0);
    /* 冷启动默认未知充电状态，以 false 展示。 */
    context->view.charging = false;
    /* 冷启动没有传感器质量告警。 */
    context->view.data_quality_flags = 0U;
    /* 零错误码表示等待自检而不是自检失败。 */
    context->fault_code = 0;
}

/* 处理与页面无关的快照更新；返回 true 表示事件已完全消费。 */
static ui_dispatch_result_t ui_apply_global_event(ui_context_t *context, const ui_event_t *event)
{
    /* 指标仅允许有效动作索引或 255 哨兵，且百分比量纲不得超过 100%。 */
    if (event->type == UI_EVENT_METRIC_UPDATED) {
        if (((event->metrics.action_id >= UI_ACTION_CLASS_COUNT) &&
             (event->metrics.action_id != UINT8_MAX)) ||
            ((event->metrics.inferred_action_id >= UI_ACTION_CLASS_COUNT) &&
             (event->metrics.inferred_action_id != UINT8_MAX)) ||
            (event->metrics.confidence_centipercent > 10000U) ||
            ((event->metrics.battery_percent > 100U) &&
             (event->metrics.battery_percent != UINT8_MAX))) {
            return UI_DISPATCH_ERR_RANGE;
        }
        /* 只有训练页或训练熄屏恢复态接收算法指标，暂停后不得继续增长次数。 */
        const bool running_visible = context->state == UI_STATE_RUNNING;
        /* SCREEN_OFF 时检查恢复页，区分训练熄屏与主页熄屏。 */
        const bool running_hidden = (context->state == UI_STATE_SCREEN_OFF) &&
                                    (context->screen_off_resume_state == UI_STATE_RUNNING);
        /* 非训练状态忽略指标，防止停止后队列残留事件污染总结。 */
        if (!running_visible && !running_hidden) {
            return UI_DISPATCH_IGNORED;
        }
        /* 复制完整快照，保证同一帧中的动作、次数、热量和质量相互一致。 */
        context->view = event->metrics;
        return UI_DISPATCH_OK;
    }
    /* 配对显示事件更新覆盖层数据，不改变当前业务页面。 */
    if (event->type == UI_EVENT_PAIRING_CODE_SHOWN) {
        /* 六位上限外的数据必须拒绝，防止格式化截断。 */
        if (event->pairing_code > UI_PAIRING_PASSKEY_MAX) {
            /* 返回范围错误且保留当前码。 */
            return UI_DISPATCH_ERR_RANGE;
        }
        /* 标记覆盖层活动。 */
        context->view.pairing_active = true;
        /* 保存六位数值，presenter 使用 %06u 补前导零。 */
        context->view.pairing_code = event->pairing_code;
        /* 60 秒有效期远小于 uint32 半周期，单次回绕仍可比较。 */
        context->view.pairing_expires_ms =
            event->monotonic_ms + UI_PAIRING_CODE_TIMEOUT_MS;
        /* 返回已处理。 */
        return UI_DISPATCH_OK;
    }
    /* 显式清除事件验证原因后抹除全部配对字段。 */
    if (event->type == UI_EVENT_PAIRING_CODE_CLEARED) {
        /* NONE 或越界原因表示生产者合同损坏。 */
        if ((event->pairing_clear_reason <= UI_PAIRING_CLEAR_NONE) ||
            (event->pairing_clear_reason >= UI_PAIRING_CLEAR_REASON_COUNT)) {
            /* 返回范围错误。 */
            return UI_DISPATCH_ERR_RANGE;
        }
        /* 关闭覆盖层。 */
        context->view.pairing_active = false;
        /* 立即清零敏感码，后续 UI 快照不得残留。 */
        context->view.pairing_code = UINT32_C(0);
        /* 清零截止时刻。 */
        context->view.pairing_expires_ms = UINT32_C(0);
        /* 返回已处理。 */
        return UI_DISPATCH_OK;
    }
    /* BLE 事件更新连接图标，不改变当前页面。 */
    if (event->type == UI_EVENT_BLE_CHANGED) {
        context->view.ble_connected = event->flag;
        /* 成功连接或断线都结束当前配对码生命周期。 */
        context->view.pairing_active = false;
        /* 两条路径都抹除敏感码。 */
        context->view.pairing_code = UINT32_C(0);
        /* 清除旧截止时间。 */
        context->view.pairing_expires_ms = UINT32_C(0);
        return UI_DISPATCH_OK;
    }
    /* 电池事件检查百分比后更新顶栏；充电标记使用 event.flag。 */
    if (event->type == UI_EVENT_BATTERY_UPDATED) {
        if (event->percent > 100U) {
            return UI_DISPATCH_ERR_RANGE;
        }
        context->view.battery_percent = event->percent;
        context->view.charging = event->flag;
        return UI_DISPATCH_OK;
    }
    /* 返回 IGNORED 表示该事件仍需交给页面状态转移逻辑。 */
    return UI_DISPATCH_IGNORED;
}

/* 分派 UI 事件；全局安全关机优先于局部页面事件。 */
ui_dispatch_result_t ui_dispatch_event(ui_context_t *context, const ui_event_t *event)
{
    /* 上下文和事件均不能为空。 */
    if ((context == NULL) || (event == NULL)) {
        return UI_DISPATCH_ERR_ARGUMENT;
    }
    /* 长按电源键或临界低电量在任意非关机页面都进入安全关机。 */
    if ((event->type == UI_EVENT_POWER_LONG_PRESS) ||
        (event->type == UI_EVENT_CRITICAL_BATTERY)) {
        if (context->state == UI_STATE_SHUTDOWN) {
            return UI_DISPATCH_IGNORED;
        }
        ui_transition(context, UI_STATE_SHUTDOWN, event->monotonic_ms);
        return UI_DISPATCH_OK;
    }
    /* 优先处理不改变页面的快照更新。 */
    const ui_dispatch_result_t global_result = ui_apply_global_event(context, event);
    /* OK 或错误表示事件已经处理完成；只有 IGNORED 继续走页面逻辑。 */
    if (global_result != UI_DISPATCH_IGNORED) {
        return global_result;
    }
    /* 按当前页面限定合法转移，所有未列事件都返回 IGNORED。 */
    switch (context->state) {
        case UI_STATE_BOOT:
            /* BOOT 动画结束后进入硬件与模型自检页。 */
            if (event->type == UI_EVENT_BOOT_READY) {
                ui_transition(context, UI_STATE_SELF_TEST, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            break;
        case UI_STATE_SELF_TEST:
            /* 全部自检通过后进入主页。 */
            if (event->type == UI_EVENT_SELF_TEST_OK) {
                context->fault_code = 0;
                ui_transition(context, UI_STATE_HOME, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 任一关键自检失败进入错误页；错误码由上层先写入 context。 */
            if (event->type == UI_EVENT_SELF_TEST_FAILED) {
                ui_transition(context, UI_STATE_ERROR, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            break;
        case UI_STATE_HOME:
            /* 用户点击设置时进入设置页，不创建训练会话。 */
            if (event->type == UI_EVENT_OPEN_SETTINGS) {
                ui_transition(context, UI_STATE_SETTINGS, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 用户点击开始后立即进入采样和识别；首个 62 点窗口形成期间不再显示倒计时。 */
            if (event->type == UI_EVENT_START_REQUESTED) {
                context->session_active = true;
                context->view.count = 0U;
                context->view.calories_milli_kcal = 0U;
                context->view.elapsed_seconds = 0U;
                context->view.action_id = UINT8_MAX;
                /* 新会话尚无实时类别。 */
                context->view.inferred_action_id = UINT8_MAX;
                /* 准备阶段只采样和锁类，不运行任何计数器。 */
                context->view.counting_enabled = false;
                ui_transition(context, UI_STATE_PREPARE, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 主页超时后保存恢复点并熄屏。 */
            if (event->type == UI_EVENT_SCREEN_TIMEOUT) {
                context->screen_off_resume_state = UI_STATE_HOME;
                ui_transition(context, UI_STATE_SCREEN_OFF, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            break;
        case UI_STATE_PREPARE:
            /* 首个完整窗口锁定本会话唯一动作后进入训练页。 */
            if (event->type == UI_EVENT_PREPARE_COMPLETED) {
                ui_transition(context, UI_STATE_RUNNING, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 准备阶段停止视为取消，不生成空总结。 */
            if (event->type == UI_EVENT_STOP_REQUESTED) {
                context->session_active = false;
                ui_transition(context, UI_STATE_HOME, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 准备页异常停留超过门槛时仍允许熄屏；会话和预热状态由领域层继续保存。 */
            if (event->type == UI_EVENT_SCREEN_TIMEOUT) {
                /* 保存准确恢复页，唤醒后继续显示准备进度而不是跳到训练页。 */
                context->screen_off_resume_state = UI_STATE_PREPARE;
                /* 切换逻辑熄屏页，物理 AMOLED 由电源任务关闭。 */
                ui_transition(context, UI_STATE_SCREEN_OFF, event->monotonic_ms);
                /* 返回成功表示页面状态已改变。 */
                return UI_DISPATCH_OK;
            }
            break;
        case UI_STATE_RUNNING:
            /* 触屏首次 STOP 只打开确认页，底层 workout 继续保持原状态。 */
            if (event->type == UI_EVENT_STOP_CONFIRM_REQUESTED) {
                context->stop_confirm_resume_state = UI_STATE_RUNNING;
                ui_transition(context, UI_STATE_STOP_CONFIRM, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 暂停冻结计数和热量，但保留会话。 */
            if (event->type == UI_EVENT_PAUSE_REQUESTED) {
                ui_transition(context, UI_STATE_PAUSED, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 停止进入总结页，由存储层异步保存。 */
            if (event->type == UI_EVENT_STOP_REQUESTED) {
                ui_transition(context, UI_STATE_SUMMARY, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 训练熄屏不停止算法；恢复点必须保留 RUNNING。 */
            if (event->type == UI_EVENT_SCREEN_TIMEOUT) {
                context->screen_off_resume_state = UI_STATE_RUNNING;
                ui_transition(context, UI_STATE_SCREEN_OFF, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            break;
        case UI_STATE_PAUSED:
            /* 暂停页首次 STOP 同样只打开确认页，底层 workout 继续保持暂停。 */
            if (event->type == UI_EVENT_STOP_CONFIRM_REQUESTED) {
                context->stop_confirm_resume_state = UI_STATE_PAUSED;
                ui_transition(context, UI_STATE_STOP_CONFIRM, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 继续训练时恢复 RUNNING。 */
            if (event->type == UI_EVENT_RESUME_REQUESTED) {
                ui_transition(context, UI_STATE_RUNNING, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 暂停状态结束会话时进入总结。 */
            if (event->type == UI_EVENT_STOP_REQUESTED) {
                ui_transition(context, UI_STATE_SUMMARY, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 暂停超时保存 PAUSED 恢复点。 */
            if (event->type == UI_EVENT_SCREEN_TIMEOUT) {
                context->screen_off_resume_state = UI_STATE_PAUSED;
                ui_transition(context, UI_STATE_SCREEN_OFF, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            break;
        case UI_STATE_STOP_CONFIRM:
            /* 确认停止后进入总结；领域协调器随后停止 workout 并保存摘要。 */
            if (event->type == UI_EVENT_STOP_REQUESTED) {
                ui_transition(context, UI_STATE_SUMMARY, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 取消确认恢复首次点击 STOP 前的训练或暂停页。 */
            if (event->type == UI_EVENT_BACK_REQUESTED) {
                if ((context->stop_confirm_resume_state != UI_STATE_RUNNING) &&
                    (context->stop_confirm_resume_state != UI_STATE_PAUSED)) {
                    return UI_DISPATCH_ERR_RANGE;
                }
                ui_transition(context, context->stop_confirm_resume_state, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 确认页超时仍允许熄屏；唤醒后继续等待用户选择。 */
            if (event->type == UI_EVENT_SCREEN_TIMEOUT) {
                context->screen_off_resume_state = UI_STATE_STOP_CONFIRM;
                ui_transition(context, UI_STATE_SCREEN_OFF, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            break;
        case UI_STATE_SUMMARY:
            /* 会话摘要持久化完成后结束会话并回主页。 */
            if (event->type == UI_EVENT_SUMMARY_SAVED) {
                context->session_active = false;
                ui_transition(context, UI_STATE_HOME, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 总结页长时间无人操作时允许熄屏，但不删除尚待用户查看的摘要。 */
            if (event->type == UI_EVENT_SCREEN_TIMEOUT) {
                /* 保存 SUMMARY 恢复点。 */
                context->screen_off_resume_state = UI_STATE_SUMMARY;
                /* 进入统一熄屏页。 */
                ui_transition(context, UI_STATE_SCREEN_OFF, event->monotonic_ms);
                /* 返回成功。 */
                return UI_DISPATCH_OK;
            }
            break;
        case UI_STATE_SETTINGS:
            /* 设置页进入诊断页，继续保留当前会话为空的状态。 */
            if (event->type == UI_EVENT_OPEN_DIAGNOSTICS) {
                ui_transition(context, UI_STATE_DIAGNOSTICS, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 设置页返回主页。 */
            if (event->type == UI_EVENT_BACK_REQUESTED) {
                ui_transition(context, UI_STATE_HOME, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 设置页同样受用户配置的屏幕超时约束，防止 AMOLED 长时间常亮。 */
            if (event->type == UI_EVENT_SCREEN_TIMEOUT) {
                /* 保存 SETTINGS 恢复点，未提交的控件值由渲染层模型继续持有。 */
                context->screen_off_resume_state = UI_STATE_SETTINGS;
                /* 进入逻辑熄屏页。 */
                ui_transition(context, UI_STATE_SCREEN_OFF, event->monotonic_ms);
                /* 返回成功。 */
                return UI_DISPATCH_OK;
            }
            break;
        case UI_STATE_DIAGNOSTICS:
            /* 诊断页返回设置页，避免直接跳过设置导航层级。 */
            if (event->type == UI_EVENT_BACK_REQUESTED) {
                ui_transition(context, UI_STATE_SETTINGS, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            /* 诊断页没有训练会话，也必须在超时后关闭 AMOLED。 */
            if (event->type == UI_EVENT_SCREEN_TIMEOUT) {
                /* 保存 DIAGNOSTICS 恢复点。 */
                context->screen_off_resume_state = UI_STATE_DIAGNOSTICS;
                /* 进入逻辑熄屏页。 */
                ui_transition(context, UI_STATE_SCREEN_OFF, event->monotonic_ms);
                /* 返回成功。 */
                return UI_DISPATCH_OK;
            }
            break;
        case UI_STATE_SCREEN_OFF:
            /* 触摸、PWR 短按或合法 BLE 命令唤醒此前页面。 */
            if (event->type == UI_EVENT_USER_WAKE) {
                ui_transition(context, context->screen_off_resume_state, event->monotonic_ms);
                return UI_DISPATCH_OK;
            }
            break;
        case UI_STATE_ERROR:
            /* 错误页仅允许全局关机事件；恢复必须重启并重新自检。 */
            break;
        case UI_STATE_SHUTDOWN:
            /* 关机动画页不接受普通事件，等待动画/日志完成后由电源层断电。 */
            break;
        default:
            /* 未知枚举值表示内存损坏或版本不匹配，报告范围错误。 */
            return UI_DISPATCH_ERR_RANGE;
    }
    /* 当前页面不接受该事件；忽略而不改变任何状态。 */
    return UI_DISPATCH_IGNORED;
}

/* 返回页面是否需要 AMOLED；只有 SCREEN_OFF 是逻辑熄屏页。 */
bool ui_state_requires_display(ui_state_t state)
{
    /* SHUTDOWN 仍显示最多 500 ms 动画，随后 power_manager 关闭面板并让 PMIC 断电。 */
    return state != UI_STATE_SCREEN_OFF;
}

/* 返回均衡模式的轻量动画配置，避免高亮全屏动画增加功耗和烧屏风险。 */
ui_animation_profile_t ui_default_animation_profile(void)
{
    /* 所有时长均使用固定整数毫秒，渲染层无需浮点计算。 */
    const ui_animation_profile_t profile = {
        .boot_ms = 800U,
        .shutdown_ms = 500U,
        .count_feedback_ms = 180U,
        .action_crossfade_ms = 150U,
    };
    /* 返回独立副本，渲染层可在临界低电量时把动画时长改为零。 */
    return profile;
}

/* 将状态枚举映射为诊断字符串；未知状态返回 UNKNOWN。 */
const char *ui_state_name(ui_state_t state)
{
    /* 使用 switch 保证新增状态时编译器可提示未覆盖分支。 */
    switch (state) {
        case UI_STATE_BOOT: return "BOOT";
        case UI_STATE_SELF_TEST: return "SELF_TEST";
        case UI_STATE_HOME: return "HOME";
        case UI_STATE_PREPARE: return "PREPARE";
        case UI_STATE_RUNNING: return "RUNNING";
        case UI_STATE_PAUSED: return "PAUSED";
        case UI_STATE_STOP_CONFIRM: return "STOP_CONFIRM";
        case UI_STATE_SUMMARY: return "SUMMARY";
        case UI_STATE_SETTINGS: return "SETTINGS";
        case UI_STATE_DIAGNOSTICS: return "DIAGNOSTICS";
        case UI_STATE_SCREEN_OFF: return "SCREEN_OFF";
        case UI_STATE_ERROR: return "ERROR";
        case UI_STATE_SHUTDOWN: return "SHUTDOWN";
        default: return "UNKNOWN";
    }
}
