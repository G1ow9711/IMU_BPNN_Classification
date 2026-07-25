/* 引入内部后端合同；Mock 与真实后端导出相同函数集合。 */
#include "board_runtime_backend.h"

/* 初始化主机/固件 Mock。 */
int board_runtime_backend_init(board_runtime_t *runtime)
{
    /* Mock 不访问真实硬件，所有状态来自测试配置。 */
    runtime->diagnostics.real_backend = false;
    /* Mock 屏幕始终可创建，便于纯状态测试。 */
    runtime->diagnostics.display_ready = true;
    /* Mock 触摸始终可用。 */
    runtime->diagnostics.touch_ready = true;
    /* Mock I2C 总线视为可用。 */
    runtime->diagnostics.i2c_ready = true;
    /* Mock 使用 runtime 自身地址作为非空不透明总线句柄。 */
    runtime->platform_i2c = (void *)runtime;
    /* 按配置模拟三个外设是否存在。 */
    runtime->diagnostics.qmi_present = runtime->config.mock.sensor_devices_present;
    /* 存在时固定模拟低地址 0x6A。 */
    runtime->diagnostics.qmi_i2c_address = runtime->config.mock.sensor_devices_present
        ? BOARD_RUNTIME_QMI8658_LOW_ADDRESS
        : 0U;
    /* 按配置模拟 AXP2101。 */
    runtime->diagnostics.axp2101_present = runtime->config.mock.sensor_devices_present;
    /* 按配置模拟 PCF85063。 */
    runtime->diagnostics.rtc_present = runtime->config.mock.sensor_devices_present;
    /* Mock 初始不挂载 TF；存在性在挂载调用中检查。 */
    runtime->diagnostics.storage_mounted = false;
    /* Mock 扬声器门控可用。 */
    runtime->diagnostics.speaker_gate_ready = true;
    /* Mock 默认触摸活动。 */
    runtime->touch_active = true;
    /* 返回成功。 */
    return 0;
}

/* Mock 保存屏幕逻辑状态。 */
int board_runtime_backend_set_display_power(board_runtime_t *runtime, bool enabled)
{
    /* 保存逻辑开关。 */
    runtime->display_on = enabled;
    /* 返回成功。 */
    return 0;
}

/* Mock 保存亮度；范围已由上层 board_adapter 验证。 */
int board_runtime_backend_set_display_brightness(board_runtime_t *runtime, uint8_t percent)
{
    /* 非零亮度保存为恢复值，零值只代表当前熄屏。 */
    if (percent > 0U) {
        runtime->brightness_percent = percent;
    }
    /* 返回成功。 */
    return 0;
}

/* Mock 保存触摸活动状态。 */
int board_runtime_backend_set_touch_active(board_runtime_t *runtime, bool enabled)
{
    /* 更新逻辑触摸状态。 */
    runtime->touch_active = enabled;
    /* 返回成功。 */
    return 0;
}

/* Mock 保存扬声器门控。 */
int board_runtime_backend_set_speaker(board_runtime_t *runtime, bool enabled)
{
    /* 更新逻辑门控状态。 */
    runtime->speaker_enabled = enabled;
    /* 返回成功。 */
    return 0;
}

/* Mock 模拟 TF 挂载。 */
int board_runtime_backend_set_storage(board_runtime_t *runtime, bool enabled)
{
    /* 请求挂载但配置为无卡时返回失败。 */
    if (enabled && !runtime->config.mock.storage_present) {
        return -1;
    }
    /* 保存挂载状态。 */
    runtime->diagnostics.storage_mounted = enabled;
    /* 返回成功。 */
    return 0;
}

/* Mock LVGL 锁始终成功。 */
bool board_runtime_backend_lvgl_lock(board_runtime_t *runtime, uint32_t timeout_ms)
{
    /* Mock 不创建线程互斥量；读取参数仅为保持合同。 */
    (void)runtime;
    /* timeout_ms 在 Mock 中不影响结果。 */
    (void)timeout_ms;
    /* 返回成功。 */
    return true;
}

/* Mock LVGL 解锁为空操作。 */
void board_runtime_backend_lvgl_unlock(board_runtime_t *runtime)
{
    /* Mock 没有需要释放的资源。 */
    (void)runtime;
}
