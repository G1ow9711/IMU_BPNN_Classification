#ifndef IMU_HANDHELD_BOARD_RUNTIME_BACKEND_H
#define IMU_HANDHELD_BOARD_RUNTIME_BACKEND_H

/* 引入公共运行时结构，平台后端只通过已声明字段保存句柄和状态。 */
#include "board_runtime.h"

/* 初始化所选平台并填充 diagnostics；返回零表示成功。 */
int board_runtime_backend_init(board_runtime_t *runtime);
/* 控制 AMOLED 逻辑开关；真实后端通过面板亮度命令实现熄屏。 */
int board_runtime_backend_set_display_power(board_runtime_t *runtime, bool enabled);
/* 设置 AMOLED 亮度百分比，范围为 0~100。 */
int board_runtime_backend_set_display_brightness(board_runtime_t *runtime, uint8_t percent);
/* 逻辑启用或禁用 LVGL 触摸设备。 */
int board_runtime_backend_set_touch_active(board_runtime_t *runtime, bool enabled);
/* 控制 GPIO46 扬声器功放使能。 */
int board_runtime_backend_set_speaker(board_runtime_t *runtime, bool enabled);
/* 挂载或卸载 TF 卡。 */
int board_runtime_backend_set_storage(board_runtime_t *runtime, bool enabled);
/* 获取平台 LVGL 互斥锁。 */
bool board_runtime_backend_lvgl_lock(board_runtime_t *runtime, uint32_t timeout_ms);
/* 释放平台 LVGL 互斥锁。 */
void board_runtime_backend_lvgl_unlock(board_runtime_t *runtime);

#endif
