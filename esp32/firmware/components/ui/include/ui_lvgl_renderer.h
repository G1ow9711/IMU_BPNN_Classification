#ifndef IMU_HANDHELD_UI_LVGL_RENDERER_H
#define IMU_HANDHELD_UI_LVGL_RENDERER_H

/* 引入纯页面模型和按钮命令；渲染器不直接修改业务状态机。 */
#include "ui_presenter.h"

/* 引入布尔值和定长整数，定义锁与超时合同。 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 描述渲染结果；零为成功，负值按失败阶段区分。 */
typedef enum {
    /* 渲染器操作成功，LVGL 对象和显示内容有效。 */
    UI_LVGL_OK = 0,
    /* 必填渲染器、端口、上下文或视图模型指针为空。 */
    UI_LVGL_ERR_ARGUMENT = -1,
    /* 获取 BSP LVGL 互斥锁失败，未安全访问对象树。 */
    UI_LVGL_ERR_LOCK = -2,
    /* LVGL 对象或样式创建失败，通常表示内部堆不足。 */
    UI_LVGL_ERR_MEMORY = -3,
    /* 未初始化、重复初始化或页面状态值非法。 */
    UI_LVGL_ERR_STATE = -4
} ui_lvgl_result_t;

/* 获取 LVGL 互斥锁；context 指向 board_runtime 或测试端口。 */
typedef bool (*ui_lvgl_lock_fn)(void *context, uint32_t timeout_ms);
/* 释放 LVGL 互斥锁；只在成功获取后调用。 */
typedef void (*ui_lvgl_unlock_fn)(void *context);
/* 上报按钮命令；回调应只投递队列，不得在 LVGL 任务中阻塞或改硬件。 */
typedef void (*ui_lvgl_command_fn)(void *context, ui_command_t command);

/* 保存 LVGL 线程边界；所有 lv_ API 均必须位于 lock/unlock 之间。 */
typedef struct {
    /* 保存锁实现私有上下文。 */
    void *context;
    /* 保存加锁函数。 */
    ui_lvgl_lock_fn lock;
    /* 保存解锁函数。 */
    ui_lvgl_unlock_fn unlock;
} ui_lvgl_port_t;

/* 保存渲染器公开句柄；implementation 指向内部固定页面对象集合。 */
typedef struct {
    /* 保存 LVGL 锁端口副本。 */
    ui_lvgl_port_t port;
    /* 保存按钮命令回调。 */
    ui_lvgl_command_fn command_callback;
    /* 保存按钮命令回调上下文。 */
    void *command_context;
    /* 保存内部实现指针；由 init 分配、deinit 释放。 */
    void *implementation;
    /* true 允许页面和文字淡入；false 使用立即切页并禁止所有文字透明度动画。 */
    bool animations_enabled;
    /* 标记初始化完成。 */
    bool initialized;
} ui_lvgl_renderer_t;

/*
 * 在 BSP 已启动显示后创建全部页面；函数内部持有 LVGL 锁。
 * command_context 可为空，由 command_callback 自行解释；渲染器只在同步按钮回调期间借用，
 * 不释放该指针，且其生命周期必须覆盖 renderer 的完整使用期。
 */
ui_lvgl_result_t ui_lvgl_renderer_init(
    /* 非空输出对象；初始化成功后由调用者持有并覆盖全部渲染调用。 */
    ui_lvgl_renderer_t *renderer,
    /* 非空 LVGL 加锁端口；回调及 context 生命周期必须覆盖 renderer。 */
    const ui_lvgl_port_t *port,
    /* 非空用户命令回调；按钮事件在持锁渲染路径外同步分派。 */
    ui_lvgl_command_fn command_callback,
    /* 可为空命令上下文；生命周期覆盖 renderer，所有权始终归调用者。 */
    void *command_context);
/* 渲染最新状态快照并切换页面；函数内部持有 LVGL 锁。 */
ui_lvgl_result_t ui_lvgl_renderer_render(
    ui_lvgl_renderer_t *renderer,
    const ui_context_t *context);
/* 启用或禁用页面/文字动画；显示-only 诊断在首帧前关闭，避免分块 QSPI 淡入残影。 */
ui_lvgl_result_t ui_lvgl_renderer_set_animations_enabled(
    /* 非空且已初始化的渲染器；函数不保存额外指针。 */
    ui_lvgl_renderer_t *renderer,
    /* true 恢复产品动画，false 使用静态立即刷新。 */
    bool enabled);
/* 删除所有 LVGL 页面并释放内部内存；函数内部持有 LVGL 锁。 */
ui_lvgl_result_t ui_lvgl_renderer_deinit(ui_lvgl_renderer_t *renderer);

#ifdef __cplusplus
}
#endif

#endif
