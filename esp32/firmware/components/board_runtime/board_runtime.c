/* 引入公开运行时合同，保证真实 BSP 与 Mock 的上层行为一致。 */
#include "board_runtime.h"
/* 引入内部后端接口，公共逻辑不直接包含 ESP-IDF 头。 */
#include "board_runtime_backend.h"

/* 引入 NULL 定义和内存清零函数。 */
#include <stddef.h>
#include <string.h>

/* 把平台返回值转换为 board_adapter 回调约定的零/非零结果。 */
static int runtime_set_display_power(void *context, bool enabled)
{
    /* context 在初始化时固定指向 board_runtime_t。 */
    board_runtime_t *runtime = (board_runtime_t *)context;
    /* 逻辑状态未变化时不重复发送 SH8601 Display On/Off 命令。 */
    if (runtime->display_on == enabled) {
        /* 返回成功；board_adapter 仍会按策略更新亮度。 */
        return 0;
    }
    /* 调用所选平台后端，并把具体错误保存在诊断字段。 */
    const int result = board_runtime_backend_set_display_power(runtime, enabled);
    /* 记录最近平台错误，供诊断页显示。 */
    runtime->diagnostics.last_platform_error = result;
    /* 平台成功时同步逻辑状态。 */
    if (result == 0) {
        runtime->display_on = enabled;
    }
    /* 返回零表示 board_adapter 可继续执行亮度步骤。 */
    return result;
}

/* 设置 AMOLED 亮度，并保留最近非零值用于屏幕恢复。 */
static int runtime_set_display_brightness(void *context, uint8_t percent)
{
    /* context 在初始化时固定指向 board_runtime_t。 */
    board_runtime_t *runtime = (board_runtime_t *)context;
    /* 调用平台亮度函数；SH8601 使用面板 0x51 命令而非背光 GPIO。 */
    const int result = board_runtime_backend_set_display_brightness(runtime, percent);
    /* 记录平台错误，避免 UI 把失败操作显示为成功。 */
    runtime->diagnostics.last_platform_error = result;
    /* 非零亮度设置成功时保存恢复亮度。 */
    if ((result == 0) && (percent > 0U)) {
        runtime->brightness_percent = percent;
    }
    /* 返回平台原始零/非零结果。 */
    return result;
}

/* 把板级触摸请求转发到真实 LVGL 输入设备或 Mock。 */
static int runtime_set_touch_active_callback(void *context, bool enabled)
{
    /* context 在初始化时固定指向 board_runtime_t。 */
    board_runtime_t *runtime = (board_runtime_t *)context;
    /* 调用平台触摸开关。 */
    const int result = board_runtime_backend_set_touch_active(runtime, enabled);
    /* 记录最近平台错误。 */
    runtime->diagnostics.last_platform_error = result;
    /* 成功时同步逻辑触摸状态。 */
    if (result == 0) {
        runtime->touch_active = enabled;
    }
    /* 返回平台结果。 */
    return result;
}

/* 把马达请求转发到 GPIO18 LEDC/定时器或 Mock。 */
static int runtime_pulse_motor_callback(
    void *context,
    uint16_t duration_ms,
    uint8_t intensity_percent)
{
    /* context 在初始化时固定指向 board_runtime_t。 */
    board_runtime_t *runtime = (board_runtime_t *)context;
    /* 平台后端负责非阻塞停止，避免调用方等待 30 ms。 */
    const int result = board_runtime_backend_pulse_motor(
        runtime,
        duration_ms,
        intensity_percent);
    /* 记录最近平台错误。 */
    runtime->diagnostics.last_platform_error = result;
    /* 返回平台结果。 */
    return result;
}

/* 控制扬声器功放；产品 v1 默认始终关闭。 */
static int runtime_set_speaker_callback(void *context, bool enabled)
{
    /* context 在初始化时固定指向 board_runtime_t。 */
    board_runtime_t *runtime = (board_runtime_t *)context;
    /* 调用平台 GPIO46 控制。 */
    const int result = board_runtime_backend_set_speaker(runtime, enabled);
    /* 保存最近平台错误。 */
    runtime->diagnostics.last_platform_error = result;
    /* 成功时同步逻辑状态。 */
    if (result == 0) {
        runtime->speaker_enabled = enabled;
    }
    /* 返回平台结果。 */
    return result;
}

/* 挂载或卸载 TF 卡，并同步诊断状态。 */
static int runtime_set_storage_callback(void *context, bool enabled)
{
    /* context 在初始化时固定指向 board_runtime_t。 */
    board_runtime_t *runtime = (board_runtime_t *)context;
    /* 真实后端调用厂家 BSP 的 SDMMC 接口；Mock 只更新状态。 */
    const int result = board_runtime_backend_set_storage(runtime, enabled);
    /* 记录最近平台错误。 */
    runtime->diagnostics.last_platform_error = result;
    /* 成功时同步挂载状态。 */
    if (result == 0) {
        runtime->diagnostics.storage_mounted = enabled;
    }
    /* 返回平台结果。 */
    return result;
}

/* 通过独立 AXP2101 驱动读取电量；厂商 BSP 不提供该高层函数。 */
static int runtime_read_battery_callback(void *context, uint8_t *percent, bool *charging)
{
    /* context 在初始化时固定指向 board_runtime_t。 */
    board_runtime_t *runtime = (board_runtime_t *)context;
    /* 注册独立驱动时调用真实 PMIC 读数。 */
    if (runtime->config.external_ops.read_battery != NULL) {
        return runtime->config.external_ops.read_battery(
            runtime->config.external_ops.context,
            percent,
            charging);
    }
    /* Mock 后端没有外部驱动时返回配置快照，便于主机状态机测试。 */
    if (!runtime->diagnostics.real_backend) {
        *percent = runtime->config.mock.battery_percent;
        *charging = runtime->config.mock.charging;
        return 0;
    }
    /* 真实后端缺少 PMIC 驱动时明确失败，禁止伪造电量。 */
    return -1;
}

/* 初始化统一运行时。 */
board_runtime_result_t board_runtime_init(
    board_runtime_t *runtime,
    const board_runtime_config_t *config)
{
    /* 两个输入都必须有效，且默认亮度必须在 1~100。 */
    if ((runtime == NULL) || (config == NULL) ||
        (config->initial_brightness_percent == 0U) ||
        (config->initial_brightness_percent > 100U) ||
        (config->mock.battery_percent > 100U)) {
        return BOARD_RUNTIME_ERR_ARGUMENT;
    }
    /* 清零全部状态和不透明平台句柄，避免重复使用旧资源。 */
    (void)memset(runtime, 0, sizeof(*runtime));
    /* 复制配置，回调 context 生命周期仍由调用方保证。 */
    runtime->config = *config;
    /* 先保存目标亮度，真实显示启动后由后端设置。 */
    runtime->brightness_percent = config->initial_brightness_percent;
    /* 初始化所选平台；真实后端完成显示/I2C/GPIO，Mock 填充虚拟诊断。 */
    const int platform_result = board_runtime_backend_init(runtime);
    /* 保存启动平台错误。 */
    runtime->diagnostics.last_platform_error = platform_result;
    /* 平台失败时不建立半初始化 board_adapter。 */
    if (platform_result != 0) {
        return BOARD_RUNTIME_ERR_PLATFORM;
    }
    /* 读取并验证固定板型，引脚来自本地原理图。 */
    const board_profile_t profile = board_profile_from_build();
    /* 构造完整回调表；context 指向长期有效的 runtime。 */
    const board_adapter_ops_t ops = {
        .context = runtime,
        .set_display_power = runtime_set_display_power,
        .set_display_brightness = runtime_set_display_brightness,
        .set_touch_active = runtime_set_touch_active_callback,
        .pulse_motor = runtime_pulse_motor_callback,
        .set_speaker_power = runtime_set_speaker_callback,
        .set_storage_active = runtime_set_storage_callback,
        .read_battery = runtime_read_battery_callback,
    };
    /* 初始化 board_adapter；失败代表本地板型合同不一致。 */
    if (board_adapter_init(&runtime->adapter, &profile, &ops) != BOARD_ADAPTER_OK) {
        return BOARD_RUNTIME_ERR_PLATFORM;
    }
    /* 显示启动后按配置点亮，并设置首屏亮度。 */
    if (board_adapter_set_display(
            &runtime->adapter,
            true,
            config->initial_brightness_percent) != BOARD_ADAPTER_OK) {
        return BOARD_RUNTIME_ERR_IO;
    }
    /* 扬声器功放默认关闭，避免启动爆音和额外静态功耗。 */
    if (runtime_set_speaker_callback(runtime, false) != 0) {
        return BOARD_RUNTIME_ERR_IO;
    }
    /* 显示、适配器和默认安全状态全部完成后再标记初始化。 */
    runtime->initialized = true;
    /* 返回成功；I2C 设备是否存在需读取 diagnostics 单独判断。 */
    return BOARD_RUNTIME_OK;
}

/* 返回内部 board_adapter。 */
board_adapter_t *board_runtime_adapter(board_runtime_t *runtime)
{
    /* 未初始化对象不能交给上层使用。 */
    if ((runtime == NULL) || !runtime->initialized) {
        return NULL;
    }
    /* 返回 runtime 内部对象地址，生命周期与 runtime 相同。 */
    return &runtime->adapter;
}

/* 返回共用 I2C 主总线句柄。 */
void *board_runtime_i2c_handle(board_runtime_t *runtime)
{
    /* 未初始化或总线未就绪时返回 NULL，独立驱动必须停止初始化。 */
    if ((runtime == NULL) || !runtime->initialized ||
        !runtime->diagnostics.i2c_ready) {
        return NULL;
    }
    /* 返回平台不透明句柄；ESP-IDF 端可转换为 i2c_master_bus_handle_t。 */
    return runtime->platform_i2c;
}

/* 复制诊断快照。 */
board_runtime_result_t board_runtime_get_diagnostics(
    const board_runtime_t *runtime,
    board_runtime_diagnostics_t *diagnostics)
{
    /* 运行时、输出和初始化状态都必须有效。 */
    if ((runtime == NULL) || (diagnostics == NULL)) {
        return BOARD_RUNTIME_ERR_ARGUMENT;
    }
    /* 未完成初始化时不输出可能误导的部分诊断。 */
    if (!runtime->initialized) {
        return BOARD_RUNTIME_ERR_NOT_READY;
    }
    /* 按值复制固定大小快照，调用方可跨任务保存副本。 */
    *diagnostics = runtime->diagnostics;
    /* 返回成功。 */
    return BOARD_RUNTIME_OK;
}

/* 设置触摸逻辑活动状态。 */
board_runtime_result_t board_runtime_set_touch_active(board_runtime_t *runtime, bool active)
{
    /* 必须完成初始化。 */
    if ((runtime == NULL) || !runtime->initialized) {
        return BOARD_RUNTIME_ERR_NOT_READY;
    }
    /* 同一逻辑状态不重复复位或休眠 FT3168，避免每次电源策略刷新产生触摸中断空窗。 */
    if (runtime->touch_active == active) {
        /* 当前硬件状态已经满足请求。 */
        return BOARD_RUNTIME_OK;
    }
    /* 调用与 adapter 相同的平台路径。 */
    if (runtime_set_touch_active_callback(runtime, active) != 0) {
        return BOARD_RUNTIME_ERR_IO;
    }
    /* 返回成功。 */
    return BOARD_RUNTIME_OK;
}

/* 设置 TF 挂载状态。 */
board_runtime_result_t board_runtime_set_storage_active(board_runtime_t *runtime, bool active)
{
    /* 必须完成初始化。 */
    if ((runtime == NULL) || !runtime->initialized) {
        return BOARD_RUNTIME_ERR_NOT_READY;
    }
    /* 调用厂家 BSP 或 Mock 路径。 */
    if (runtime_set_storage_callback(runtime, active) != 0) {
        return BOARD_RUNTIME_ERR_IO;
    }
    /* 返回成功。 */
    return BOARD_RUNTIME_OK;
}

/* 设置扬声器功放使能。 */
board_runtime_result_t board_runtime_set_speaker_enabled(board_runtime_t *runtime, bool enabled)
{
    /* 必须完成初始化。 */
    if ((runtime == NULL) || !runtime->initialized) {
        return BOARD_RUNTIME_ERR_NOT_READY;
    }
    /* GPIO46 平台调用失败时报告 I/O 错误。 */
    if (runtime_set_speaker_callback(runtime, enabled) != 0) {
        return BOARD_RUNTIME_ERR_IO;
    }
    /* 返回成功。 */
    return BOARD_RUNTIME_OK;
}

/* 通过独立驱动切换 QMI8658 OFF、ACTIVE 或 WOM 模式。 */
board_runtime_result_t board_runtime_set_qmi_mode(
    board_runtime_t *runtime,
    board_runtime_qmi_mode_t mode)
{
    /* 必须完成初始化。 */
    if ((runtime == NULL) || !runtime->initialized) {
        /* 未初始化不能访问外部驱动。 */
        return BOARD_RUNTIME_ERR_NOT_READY;
    }
    /* 枚举只允许三种公开值，避免任意整数进入寄存器配置回调。 */
    if ((mode != BOARD_RUNTIME_QMI_OFF) && (mode != BOARD_RUNTIME_QMI_ACTIVE) &&
        (mode != BOARD_RUNTIME_QMI_WAKE_ON_MOTION)) {
        /* 返回参数错误，外部回调不会执行。 */
        return BOARD_RUNTIME_ERR_ARGUMENT;
    }
    /* 未注册独立 QMI 驱动时明确返回不支持。 */
    if (runtime->config.external_ops.set_qmi_mode == NULL) {
        /* 返回不支持。 */
        return BOARD_RUNTIME_ERR_UNSUPPORTED;
    }
    /* 启动探测没有发现 QMI 时禁止发寄存器命令。 */
    if (!runtime->diagnostics.qmi_present) {
        /* 返回 I/O 错误，表明硬件未应答。 */
        return BOARD_RUNTIME_ERR_IO;
    }
    /* 调用独立驱动；非零表示总线或寄存器设置失败。 */
    if (runtime->config.external_ops.set_qmi_mode(
            runtime->config.external_ops.context,
            mode) != 0) {
        /* 返回 I/O 错误，禁止上层把半配置模式视为成功。 */
        return BOARD_RUNTIME_ERR_IO;
    }
    /* 返回成功。 */
    return BOARD_RUNTIME_OK;
}

/* 判断 QMI GPIO21 是否已经构成唯一可信的 Deep-sleep 唤醒链。 */
bool board_runtime_qmi_deep_wake_ready(
    const board_runtime_t *runtime,
    bool policy_enabled,
    bool wom_configured)
{
    /* 运行时必须存在且已初始化，避免读取无效板型或诊断字段。 */
    if ((runtime == NULL) || !runtime->initialized) {
        /* 无运行时不能进入 Deep-sleep。 */
        return false;
    }
    /* 产品策略和 QMI CTRL9 WOM 握手必须同时成功。 */
    if (!policy_enabled || !wom_configured) {
        /* 缺任一条件即没有可信运动唤醒。 */
        return false;
    }
    /* 编译板型必须显式允许运动唤醒，防止未知板型复用 GPIO21。 */
    if (!runtime->adapter.profile.enable_imu_deep_wake) {
        /* 板型禁用时拒绝。 */
        return false;
    }
    /* 启动探测必须确认 0x6A 或 0x6B 上存在 QMI8658。 */
    if (!runtime->diagnostics.qmi_present) {
        /* 无芯片应答时 GPIO21 不可信。 */
        return false;
    }
    /* ESP32-S3 可用于 RTC EXT1 的 GPIO 范围为 0~21；本板固定使用上界 GPIO21。 */
    if (runtime->adapter.profile.imu_interrupt_gpio > 21U) {
        /* GPIO38/39 等数字 GPIO 只能作 Light-sleep wake，不能注册 EXT1。 */
        return false;
    }
    /* 全链路条件满足，允许主入口注册 EXT1 ANY_HIGH 后进入 Deep-sleep。 */
    return true;
}

/* 通过独立 PCF85063 驱动读取 Unix 秒。 */
board_runtime_result_t board_runtime_read_rtc_unix_seconds(
    board_runtime_t *runtime,
    uint64_t *unix_seconds)
{
    /* 运行时、输出和初始化状态必须有效。 */
    if ((runtime == NULL) || (unix_seconds == NULL)) {
        return BOARD_RUNTIME_ERR_ARGUMENT;
    }
    /* 未初始化对象不能读取硬件。 */
    if (!runtime->initialized) {
        return BOARD_RUNTIME_ERR_NOT_READY;
    }
    /* 未注册独立 RTC 驱动时明确返回不支持。 */
    if (runtime->config.external_ops.read_rtc_unix_seconds == NULL) {
        return BOARD_RUNTIME_ERR_UNSUPPORTED;
    }
    /* 启动探测没有发现 RTC 时禁止调用独立驱动。 */
    if (!runtime->diagnostics.rtc_present) {
        return BOARD_RUNTIME_ERR_IO;
    }
    /* 调用独立驱动并检查结果。 */
    if (runtime->config.external_ops.read_rtc_unix_seconds(
            runtime->config.external_ops.context,
            unix_seconds) != 0) {
        return BOARD_RUNTIME_ERR_IO;
    }
    /* 返回成功。 */
    return BOARD_RUNTIME_OK;
}

/* 请求 AXP2101 安全断电。 */
board_runtime_result_t board_runtime_request_pmic_shutdown(board_runtime_t *runtime)
{
    /* 必须完成初始化。 */
    if ((runtime == NULL) || !runtime->initialized) {
        return BOARD_RUNTIME_ERR_NOT_READY;
    }
    /* 未注册独立 PMIC 驱动时禁止假装关机成功。 */
    if (runtime->config.external_ops.request_pmic_shutdown == NULL) {
        return BOARD_RUNTIME_ERR_UNSUPPORTED;
    }
    /* 启动探测没有发现 AXP2101 时禁止报告关机请求成功。 */
    if (!runtime->diagnostics.axp2101_present) {
        return BOARD_RUNTIME_ERR_IO;
    }
    /* 调用独立驱动；上层应在调用前完成文件同步。 */
    if (runtime->config.external_ops.request_pmic_shutdown(
            runtime->config.external_ops.context) != 0) {
        return BOARD_RUNTIME_ERR_IO;
    }
    /* 返回成功；真实电源切断需用户后续烧录实测。 */
    return BOARD_RUNTIME_OK;
}

/* 获取 LVGL 锁。 */
bool board_runtime_lvgl_lock(void *runtime_context, uint32_t timeout_ms)
{
    /* renderer 把 board_runtime_t 作为 port context 传入。 */
    board_runtime_t *runtime = (board_runtime_t *)runtime_context;
    /* 未初始化时不能访问 LVGL。 */
    if ((runtime == NULL) || !runtime->initialized) {
        return false;
    }
    /* 把超时毫秒透传给 BSP/Mock。 */
    return board_runtime_backend_lvgl_lock(runtime, timeout_ms);
}

/* 释放 LVGL 锁。 */
void board_runtime_lvgl_unlock(void *runtime_context)
{
    /* renderer 把 board_runtime_t 作为 port context 传入。 */
    board_runtime_t *runtime = (board_runtime_t *)runtime_context;
    /* 仅初始化对象允许释放锁。 */
    if ((runtime == NULL) || !runtime->initialized) {
        return;
    }
    /* 与成功的 lock 成对释放。 */
    board_runtime_backend_lvgl_unlock(runtime);
}
