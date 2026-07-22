#ifndef IMU_HANDHELD_UI_STATE_MACHINE_H
#define IMU_HANDHELD_UI_STATE_MACHINE_H

/* 引入布尔值、定长整数和尺寸类型，使界面合同可在主机与 ESP32 复用。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 11 类动作名称的最大 UTF-8 字节数不在状态机中保存；这里只传稳定类别索引。 */
#define UI_ACTION_CLASS_COUNT (11U)
/* BLE Display Only 配对码最大六位；0 也必须显示为 000000。 */
#define UI_PAIRING_PASSKEY_MAX (UINT32_C(999999))
/* 配对码屏幕有效期 60 秒；应用任务到期后必须发布清除事件。 */
#define UI_PAIRING_CODE_TIMEOUT_MS (UINT32_C(60000))

/* 描述配对码清除原因；仅用于日志和测试，清除后上下文不保留旧码。 */
typedef enum {
    /* 显示事件或未清除状态。 */
    UI_PAIRING_CLEAR_NONE = 0,
    /* 加密、MITM 和绑定全部成功。 */
    UI_PAIRING_CLEAR_SUCCESS,
    /* 安全握手、IO 注入或认证失败。 */
    UI_PAIRING_CLEAR_FAILED,
    /* 物理链路断开或 NimBLE 主机复位。 */
    UI_PAIRING_CLEAR_DISCONNECTED,
    /* 屏幕显示超过 60 秒。 */
    UI_PAIRING_CLEAR_TIMEOUT,
    /* 用户在设置页执行“忘记电脑”。 */
    UI_PAIRING_CLEAR_FORGOTTEN,
    /* BLE 服务停止或设备进入关机流程。 */
    UI_PAIRING_CLEAR_SERVICE_STOPPED,
    /* 清除原因数量，仅用于范围校验。 */
    UI_PAIRING_CLEAR_REASON_COUNT
} ui_pairing_clear_reason_t;

/* 描述用户可见页面；SCREEN_OFF 保存此前页面，SHUTDOWN 用于短关机动画。 */
typedef enum {
    /* 冷启动 Logo/动画页；只在上电复位后短时显示。 */
    UI_STATE_BOOT = 0,
    /* 板级资源、模型和存储自检页；结果决定进入主页或错误页。 */
    UI_STATE_SELF_TEST,
    /* 无活动会话的主页；显示开始、设置和诊断入口。 */
    UI_STATE_HOME,
    /* 开始后的准备倒计时和动作锁定页。 */
    UI_STATE_PREPARE,
    /* 训练实时页；显示动作、指标、卡路里、连接和电量。 */
    UI_STATE_RUNNING,
    /* 会话暂停页；保留指标并提供继续或停止。 */
    UI_STATE_PAUSED,
    /* 触屏停止二次确认页；保留进入前 RUNNING/PAUSED 恢复点，确认后才结束会话。 */
    UI_STATE_STOP_CONFIRM,
    /* 会话总结页；等待保存完成或用户确认返回主页。 */
    UI_STATE_SUMMARY,
    /* 设置页修改亮度、振动和连接偏好；具体值由设置组件持久化。 */
    UI_STATE_SETTINGS,
    /* 诊断页展示板级自检、数据质量和 BLE 状态，不修改算法状态。 */
    UI_STATE_DIAGNOSTICS,
    /* AMOLED 已关闭的逻辑页；唤醒时恢复 screen_off_resume_state。 */
    UI_STATE_SCREEN_OFF,
    /* 不可继续的自检或运行错误页；显示 fault_code 和恢复提示。 */
    UI_STATE_ERROR,
    /* 安全关机动画页；动画结束后交由电源管理器关闭 PMIC。 */
    UI_STATE_SHUTDOWN,
    /* 保存状态总数，仅用于固定数组边界，不是可进入页面。 */
    UI_STATE_COUNT
} ui_state_t;

/* 描述输入事件；事件可来自触摸、PWR 键、算法、BLE 或电源管理器。 */
typedef enum {
    /* 板级和 UI 基础资源就绪，BOOT 可进入自检页。 */
    UI_EVENT_BOOT_READY = 0,
    /* 所有必需自检项通过，自检页进入主页。 */
    UI_EVENT_SELF_TEST_OK,
    /* 任一必需自检项失败；故障码由上层预先写入 context->fault_code。 */
    UI_EVENT_SELF_TEST_FAILED,
    /* 用户点击开始；主页进入准备倒计时。 */
    UI_EVENT_START_REQUESTED,
    /* 倒计时和动作锁定完成；准备页进入训练页。 */
    UI_EVENT_PREPARE_COMPLETED,
    /* 用户请求暂停当前会话。 */
    UI_EVENT_PAUSE_REQUESTED,
    /* 用户请求恢复已暂停会话。 */
    UI_EVENT_RESUME_REQUESTED,
    /* 触屏首次点击停止；只打开确认页，不修改 workout 或持久化状态。 */
    UI_EVENT_STOP_CONFIRM_REQUESTED,
    /* 用户确认停止；训练/暂停页进入总结保存流程。 */
    UI_EVENT_STOP_REQUESTED,
    /* 会话摘要持久化成功；总结页允许返回主页。 */
    UI_EVENT_SUMMARY_SAVED,
    /* 用户从主页进入设置。 */
    UI_EVENT_OPEN_SETTINGS,
    /* 用户从设置进入诊断。 */
    UI_EVENT_OPEN_DIAGNOSTICS,
    /* 用户从设置/诊断返回上一层。 */
    UI_EVENT_BACK_REQUESTED,
    /* 无交互计时达到配置门槛；当前页面进入逻辑熄屏。 */
    UI_EVENT_SCREEN_TIMEOUT,
    /* 触摸、按键或允许的唤醒源请求恢复显示。 */
    UI_EVENT_USER_WAKE,
    /* PWR 键长按达到关机门槛，进入安全关机动画。 */
    UI_EVENT_POWER_LONG_PRESS,
    /* PMIC 报告临界低电量；跳过非必要动画并保存关机。 */
    UI_EVENT_CRITICAL_BATTERY,
    /* 权威 MetricEvent 更新动作、计数、卡路里或时长。 */
    UI_EVENT_METRIC_UPDATED,
    /* BLE 连接状态变化，刷新主页和训练页连接图标。 */
    UI_EVENT_BLE_CHANGED,
    /* NimBLE Display Only 生成六位码；事件负载 pairing_code 和 monotonic_ms。 */
    UI_EVENT_PAIRING_CODE_SHOWN,
    /* 成功、失败、断线、超时、忘记或停止时清除配对码。 */
    UI_EVENT_PAIRING_CODE_CLEARED,
    /* 电量百分比或充电状态变化，刷新顶部状态。 */
    UI_EVENT_BATTERY_UPDATED
} ui_event_type_t;

/* 描述事件处理结果；IGNORED 表示事件在当前页面无效但不是程序错误。 */
typedef enum {
    /* 事件已处理，状态或视图模型可能发生变化。 */
    UI_DISPATCH_OK = 0,
    /* 当前页面不接受该事件；安全忽略且不视为程序错误。 */
    UI_DISPATCH_IGNORED = 1,
    /* 状态机、事件或输出指针为空。 */
    UI_DISPATCH_ERR_ARGUMENT = -1,
    /* 页面、动作、电量或置信度字段超出允许范围。 */
    UI_DISPATCH_ERR_RANGE = -2
} ui_dispatch_result_t;

/* 保存训练页展示数据；卡路里使用毫卡避免状态机依赖浮点格式化。 */
typedef struct {
    /* 保存本轮主动作索引，取值 0~10；255 表示尚未选择计数器类型。 */
    uint8_t action_id;
    /* 保存最近推理窗口实时动作索引，取值 0~10；255 表示尚无可靠实时类别。 */
    uint8_t inferred_action_id;
    /* true 表示实时动作与本轮主动作一致且数据干净，当前点允许进入计数器。 */
    bool counting_enabled;
    /* 保存当前动作计数或步数；sit 类由 elapsed_seconds 表示持续时间。 */
    uint32_t count;
    /* 保存会话累计热量，单位为 0.001 kcal。 */
    uint32_t calories_milli_kcal;
    /* 保存会话已运行秒数，用于顶部计时和 sit 持续时间。 */
    uint32_t elapsed_seconds;
    /* 保存当前 AMOLED 用户亮度百分比，范围 5~100；由设备配置投影。 */
    uint8_t brightness_percent;
    /* 保存当前自动熄屏时间，单位秒，范围 10~600；由设备配置投影。 */
    uint16_t screen_timeout_seconds;
    /* 保存偏好修订号，设备触屏或 PC 每次成功持久化后递增。 */
    uint32_t preferences_revision;
    /* 标记有效计数、开始/暂停和提示是否允许驱动振动马达。 */
    bool haptic_enabled;
    /* 保存模型置信度，单位为 0.01%，取值 0~10000。 */
    uint16_t confidence_centipercent;
    /* 保存电池百分比，取值 0~100；255 表示 PMIC 数据未知。 */
    uint8_t battery_percent;
    /* 标记 BLE 是否已连接 PC。 */
    bool ble_connected;
    /* 标记当前是否显示六位配对码；false 时 pairing_code 必须为 0。 */
    bool pairing_active;
    /* 保存 0～999999 配对码；清除时立即归零，不跨事件保留。 */
    uint32_t pairing_code;
    /* 保存显示截止单调毫秒；使用 uint32 回绕安全比较。 */
    uint32_t pairing_expires_ms;
    /* 标记 USB/电池是否处于充电状态。 */
    bool charging;
    /* 保存数据质量位图；具体位定义由算法/协议层统一管理。 */
    uint16_t data_quality_flags;
} ui_view_model_t;

/* 保存一次 UI 事件及其负载；不含动态内存，适合 FreeRTOS 队列按值传递。 */
typedef struct {
    /* 指明本次事件类型。 */
    ui_event_type_t type;
    /* 保存单调时钟毫秒值，用于动画和超时，不使用可能跳变的 RTC。 */
    uint32_t monotonic_ms;
    /* 保存指标更新负载；仅 UI_EVENT_METRIC_UPDATED 使用。 */
    ui_view_model_t metrics;
    /* 保存布尔负载；BLE/充电类简单事件可复用。 */
    bool flag;
    /* 保存 0~100 百分比负载；电池更新事件使用。 */
    uint8_t percent;
    /* 保存 0～999999 六位码；仅 UI_EVENT_PAIRING_CODE_SHOWN 使用。 */
    uint32_t pairing_code;
    /* 保存配对码清除原因；仅 UI_EVENT_PAIRING_CODE_CLEARED 使用。 */
    ui_pairing_clear_reason_t pairing_clear_reason;
} ui_event_t;

/* 保存轻量动画时长；单位均为毫秒，渲染层可按低功耗策略跳过。 */
typedef struct {
    /* 冷启动 Logo 动画时长；Light-sleep 唤醒不播放。 */
    uint16_t boot_ms;
    /* 安全关机动画时长；临界低电量可跳过。 */
    uint16_t shutdown_ms;
    /* 有效计数后数字缩放/圆环动画时长。 */
    uint16_t count_feedback_ms;
    /* 稳定动作变化后的图标淡入时长。 */
    uint16_t action_crossfade_ms;
} ui_animation_profile_t;

/* 保存界面状态和恢复点；渲染层只读该结构，不直接修改。 */
typedef struct {
    /* 保存当前页面。 */
    ui_state_t state;
    /* 保存进入 SCREEN_OFF 前的页面，唤醒时精确恢复。 */
    ui_state_t screen_off_resume_state;
    /* 保存进入停止确认前的 RUNNING 或 PAUSED 页面，取消时精确恢复。 */
    ui_state_t stop_confirm_resume_state;
    /* 保存当前训练会话是否存在；暂停和训练熄屏时仍为 true。 */
    bool session_active;
    /* 保存面板逻辑开关；不等同于物理驱动调用是否已完成。 */
    bool screen_on;
    /* 保存最近一次状态切换的单调毫秒值。 */
    uint32_t state_entered_ms;
    /* 保存当前用户可见数据快照。 */
    ui_view_model_t view;
    /* 保存自检失败错误码；零表示没有错误。 */
    int32_t fault_code;
} ui_context_t;

/* 将 UI 上下文初始化到冷启动页，并设置未知动作与未知电量哨兵。 */
void ui_context_init(ui_context_t *context, uint32_t monotonic_ms);
/* 分派一个输入事件；函数只更新纯状态，不调用 LVGL 或硬件。 */
ui_dispatch_result_t ui_dispatch_event(ui_context_t *context, const ui_event_t *event);
/* 返回当前页面是否需要 AMOLED 点亮；SHUTDOWN 动画页在 PMIC 断电前仍需点亮。 */
bool ui_state_requires_display(ui_state_t state);
/* 返回均衡模式动画时长；所有动画均避免大面积白色刷新。 */
ui_animation_profile_t ui_default_animation_profile(void);
/* 返回稳定状态名称，供日志和诊断页显示；返回字符串为静态只读内存。 */
const char *ui_state_name(ui_state_t state);

#ifdef __cplusplus
}
#endif

#endif
