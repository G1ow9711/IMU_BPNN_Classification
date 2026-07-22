#ifndef IMU_HANDHELD_UI_PRESENTER_H
#define IMU_HANDHELD_UI_PRESENTER_H

/* 引入 UI 状态和指标快照；presenter 只做纯文本/按钮映射。 */
#include "ui_state_machine.h"

/* 引入布尔、尺寸和定长整数类型。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 固定页面文本缓冲上限；全部数组内嵌，渲染过程不分配字符串。 */
#define UI_PRESENTER_TEXT_CAPACITY (96U)
/* 每页最多五个触摸按钮；设置页使用两行排列容纳忘记电脑入口。 */
#define UI_PRESENTER_MAX_BUTTONS (5U)

/* 描述用户按钮意图；上层把命令写入 UI 事件队列，不在 LVGL 回调内直接改状态。 */
typedef enum {
    /* 当前按钮槽无命令，渲染层应隐藏或禁用。 */
    UI_COMMAND_NONE = 0,
    /* 从主页启动新训练并进入准备流程。 */
    UI_COMMAND_START,
    /* 取消准备或停止确认，不修改已保存会话。 */
    UI_COMMAND_CANCEL,
    /* 暂停正在运行的训练会话。 */
    UI_COMMAND_PAUSE,
    /* 恢复已暂停的训练会话。 */
    UI_COMMAND_RESUME,
    /* 请求停止当前训练并进入总结保存。 */
    UI_COMMAND_STOP,
    /* 在停止确认页执行真正停止；与首次 STOP 分离，防止误触。 */
    UI_COMMAND_CONFIRM_STOP,
    /* 确认总结已查看并返回主页。 */
    UI_COMMAND_DONE,
    /* 从主页打开设置页。 */
    UI_COMMAND_OPEN_SETTINGS,
    /* 从设置页打开诊断页。 */
    UI_COMMAND_OPEN_DIAGNOSTICS,
    /* 从设置或诊断页返回上一层。 */
    UI_COMMAND_BACK,
    /* 设置页按 15/35/60/100% 循环亮度并持久化。 */
    UI_COMMAND_CYCLE_BRIGHTNESS,
    /* 设置页切换计次振动开关并持久化。 */
    UI_COMMAND_TOGGLE_HAPTIC,
    /* 诊断页按 15/30/60/120 秒循环熄屏门槛并持久化。 */
    UI_COMMAND_CYCLE_TIMEOUT,
    /* 诊断页触发一次 30 ms 马达自检；不改变次数、热量或会话。 */
    UI_COMMAND_TEST_HAPTIC,
    /* 设置页删除全部 BLE 绑定并断开当前电脑；由 main 应用任务执行本地事务。 */
    UI_COMMAND_FORGET_COMPUTER,
    /* 熄屏页收到任意有效触摸后请求恢复进入熄屏前页面。 */
    UI_COMMAND_WAKE,
    /* 用户确认安全关机。 */
    UI_COMMAND_SHUTDOWN
} ui_command_t;

/* 保存单个按钮的纯显示模型。 */
typedef struct {
    /* 保存按钮命令；NONE 表示该槽位隐藏。 */
    ui_command_t command;
    /* 保存 UTF-8 中文标签；渲染器必须使用覆盖本项目完整文案的项目中文字库。 */
    char label[UI_PRESENTER_TEXT_CAPACITY];
    /* true 表示允许点击；false 表示显示但禁用。 */
    bool enabled;
} ui_button_model_t;

/* 保存一页完整纯显示模型；LVGL 层只消费该结构，不重新解释业务状态。 */
typedef struct {
    /* 保存页面标题。 */
    char title[UI_PRESENTER_TEXT_CAPACITY];
    /* 保存最醒目的动作名、倒计时或总结值。 */
    char primary[UI_PRESENTER_TEXT_CAPACITY];
    /* 保存次数/步数/秒和 kcal 等次要指标。 */
    char secondary[UI_PRESENTER_TEXT_CAPACITY];
    /* 保存电池、BLE、质量位图和错误等状态。 */
    char status[UI_PRESENTER_TEXT_CAPACITY];
    /* 保存底部操作提示或约束说明。 */
    char footer[UI_PRESENTER_TEXT_CAPACITY];
    /* 保存最多五个按钮；未使用槽位的 command 为 NONE。 */
    ui_button_model_t buttons[UI_PRESENTER_MAX_BUTTONS];
} ui_page_model_t;

/* 把 0~10 动作索引映射为稳定中文显示名；255 或越界返回“等待识别”。 */
const char *ui_action_display_name(uint8_t action_id);
/* 把状态机上下文格式化为页面模型；时间复杂度 O(1)，不分配动态内存。 */
bool ui_presenter_build(const ui_context_t *context, ui_page_model_t *page);
/* 把触摸命令映射为状态机事件；无对应事件时返回 false。 */
bool ui_command_to_event(ui_command_t command, uint32_t monotonic_ms, ui_event_t *event);

#ifdef __cplusplus
}
#endif

#endif
