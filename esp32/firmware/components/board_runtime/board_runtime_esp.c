/* 引入内部后端合同，真实实现只在 ESP-IDF 组件构建中编译。 */
#include "board_runtime_backend.h"

/* 引入厂家 v1.0.7 BSP 的公开显示、I2C、触摸和 TF 接口。 */
#include "bsp/esp-bsp.h"
/* 引入 AMOLED 亮度控制函数；SH8601 通过 0x51 命令调光。 */
#include "bsp/display.h"
/* 引入 ESP-IDF GPIO 配置，GPIO46 用于扬声器功放使能。 */
#include "driver/gpio.h"
/* 引入 ESP-IDF I2C 主机探测 API，只做地址 ACK 检查。 */
#include "driver/i2c_master.h"
/* 引入 ESP-IDF LEDC API，GPIO18 使用 5 kHz PWM 驱动马达 NMOS。 */
#include "driver/ledc.h"
/* 引入 ESP-IDF 错误码。 */
#include "esp_err.h"
/* 引入一次性软件定时器，脉冲结束后自动关闭马达。 */
#include "esp_timer.h"
/* 引入 LVGL 9 输入设备开关 API。 */
#include "lvgl.h"

/* 固定马达 LEDC 低速模式，ESP32-S3 Light-sleep 前由电源层停止脉冲。 */
#define BOARD_RUNTIME_MOTOR_SPEED_MODE (LEDC_LOW_SPEED_MODE)
/* 固定马达 LEDC 定时器 0；集成时不得与其它组件重复占用。 */
#define BOARD_RUNTIME_MOTOR_TIMER (LEDC_TIMER_0)
/* 固定马达 LEDC 通道 0。 */
#define BOARD_RUNTIME_MOTOR_CHANNEL (LEDC_CHANNEL_0)
/* 固定 10 位 PWM 分辨率，最大占空比为 1023。 */
#define BOARD_RUNTIME_MOTOR_DUTY_RESOLUTION (LEDC_TIMER_10_BIT)
/* 固定 10 位最大占空比，用整数比例避免浮点。 */
#define BOARD_RUNTIME_MOTOR_MAX_DUTY (1023U)
/* 固定 I2C 地址探测超时为 50 ms，避免坏设备阻塞启动页。 */
#define BOARD_RUNTIME_I2C_PROBE_TIMEOUT_MS (50)

/* 马达脉冲结束回调运行在 esp_timer 任务，不在硬中断上下文。 */
static void board_runtime_motor_stop_callback(void *argument)
{
    /* argument 在创建定时器时固定指向长期有效的 board_runtime_t。 */
    board_runtime_t *runtime = (board_runtime_t *)argument;
    /* 空指针表示初始化异常，不能访问 LEDC。 */
    if (runtime == NULL) {
        return;
    }
    /* 把 GPIO18 占空比降为零，停止马达。 */
    (void)ledc_set_duty(
        BOARD_RUNTIME_MOTOR_SPEED_MODE,
        BOARD_RUNTIME_MOTOR_CHANNEL,
        0U);
    /* 把零占空比写入硬件。 */
    (void)ledc_update_duty(
        BOARD_RUNTIME_MOTOR_SPEED_MODE,
        BOARD_RUNTIME_MOTOR_CHANNEL);
}

/* 探测单个 7 位 I2C 地址；只判断 ACK，不读写芯片寄存器。 */
static bool board_runtime_probe_address(
    i2c_master_bus_handle_t bus,
    uint8_t address)
{
    /* 未创建总线时不能探测。 */
    if (bus == NULL) {
        return false;
    }
    /* ESP_OK 表示目标地址在 50 ms 内应答。 */
    return i2c_master_probe(
               bus,
               address,
               BOARD_RUNTIME_I2C_PROBE_TIMEOUT_MS) == ESP_OK;
}

/* 配置 GPIO18 LEDC 和一次性停止定时器。 */
static int board_runtime_motor_init(board_runtime_t *runtime)
{
    /* 创建 5 kHz、10 位低速 LEDC 定时器。 */
    const ledc_timer_config_t timer_config = {
        .speed_mode = BOARD_RUNTIME_MOTOR_SPEED_MODE,
        .duty_resolution = BOARD_RUNTIME_MOTOR_DUTY_RESOLUTION,
        .timer_num = BOARD_RUNTIME_MOTOR_TIMER,
        .freq_hz = (int)BOARD_RUNTIME_MOTOR_PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    /* 定时器配置失败时不能继续驱动马达。 */
    if (ledc_timer_config(&timer_config) != ESP_OK) {
        return -1;
    }
    /* 把 LEDC 通道 0 路由到 GPIO18，初始占空比为零。 */
    const ledc_channel_config_t channel_config = {
        .gpio_num = (int)runtime->adapter.profile.motor_gpio,
        .speed_mode = BOARD_RUNTIME_MOTOR_SPEED_MODE,
        .channel = BOARD_RUNTIME_MOTOR_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BOARD_RUNTIME_MOTOR_TIMER,
        .duty = 0U,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags = {
            .output_invert = 0U,
        },
    };
    /* 通道配置失败时返回错误。 */
    if (ledc_channel_config(&channel_config) != ESP_OK) {
        return -1;
    }
    /* 创建一次性停止定时器；回调参数使用 runtime 长期存储。 */
    const esp_timer_create_args_t timer_args = {
        .callback = board_runtime_motor_stop_callback,
        .arg = runtime,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "motor_stop",
        .skip_unhandled_events = true,
    };
    /* 创建定时器并把句柄写入不透明字段。 */
    esp_timer_handle_t timer_handle = NULL;
    /* 创建失败时不能保证马达自动停止。 */
    if (esp_timer_create(&timer_args, &timer_handle) != ESP_OK) {
        return -1;
    }
    /* 保存句柄，后续脉冲可停止旧定时并重新启动。 */
    runtime->platform_motor_timer = (void *)timer_handle;
    /* 返回成功。 */
    return 0;
}

/* 配置 GPIO46 扬声器功放使能并默认拉低。 */
static int board_runtime_speaker_gate_init(board_runtime_t *runtime)
{
    /* GPIO46 仅作数字输出，不启用上下拉和中断。 */
    const gpio_config_t speaker_gpio_config = {
        .pin_bit_mask = UINT64_C(1) << runtime->adapter.profile.speaker_enable_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    /* 应用配置失败时返回错误。 */
    if (gpio_config(&speaker_gpio_config) != ESP_OK) {
        return -1;
    }
    /* 启动默认拉低，避免功放爆音和静态耗电。 */
    if (gpio_set_level(runtime->adapter.profile.speaker_enable_gpio, 0U) != ESP_OK) {
        return -1;
    }
    /* 返回成功。 */
    return 0;
}

/* 初始化真实 Waveshare BSP。 */
int board_runtime_backend_init(board_runtime_t *runtime)
{
    /* 先写入固定板型，使 GPIO 初始化可在 board_adapter 建立前读取。 */
    runtime->adapter.profile = board_profile_from_build();
    /* 当前受管 BSP 只暴露 SH8601/FT5x06 兼容路径；原生 CO5300/CST9220 路径尚未接入。 */
    if ((runtime->adapter.profile.panel_controller != BOARD_PANEL_CONTROLLER_SH8601) ||
        (runtime->adapter.profile.touch_controller != BOARD_TOUCH_CONTROLLER_FT3168) ||
        (board_profile_validate(&runtime->adapter.profile) != BOARD_ADAPTER_OK)) {
        return -1;
    }
    /* 标记真实后端，诊断页据此显示 HARDWARE。 */
    runtime->diagnostics.real_backend = true;
    /* 厂家 BSP 启动其兼容面板、触摸驱动和 LVGL 任务；具体芯片物料由厂家组件封装。 */
    lv_display_t *display = bsp_display_start();
    /* NULL 表示显示或 LVGL 初始化失败。 */
    if (display == NULL) {
        return -1;
    }
    /* 保存显示句柄，供诊断和后续旋转扩展使用。 */
    runtime->platform_display = (void *)display;
    /* 获取 BSP 在显示启动时创建的触摸输入设备。 */
    lv_indev_t *touch = bsp_display_get_input_dev();
    /* 保存触摸句柄；NULL 会在诊断中显示故障。 */
    runtime->platform_touch = (void *)touch;
    /* 初始化共用 I2C 总线；重复调用由 BSP 幂等处理。 */
    if (bsp_i2c_init() != ESP_OK) {
        return -1;
    }
    /* 保存 BSP I2C 主总线句柄，QMI/PMIC/RTC 独立驱动可复用。 */
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    /* NULL 表示总线创建失败。 */
    if (bus == NULL) {
        return -1;
    }
    /* 保存不透明总线句柄。 */
    runtime->platform_i2c = (void *)bus;
    /* 配置马达 PWM 与自动停止定时器。 */
    if (board_runtime_motor_init(runtime) != 0) {
        return -1;
    }
    /* 配置扬声器功放门控并默认关闭。 */
    if (board_runtime_speaker_gate_init(runtime) != 0) {
        return -1;
    }
    /* 探测 QMI8658 低地址。 */
    const bool qmi_low_present = board_runtime_probe_address(
        bus,
        BOARD_RUNTIME_QMI8658_LOW_ADDRESS);
    /* 仅在低地址未应答时探测高地址，避免多余启动延迟。 */
    const bool qmi_high_present = !qmi_low_present && board_runtime_probe_address(
        bus,
        BOARD_RUNTIME_QMI8658_HIGH_ADDRESS);
    /* 填充显示诊断。 */
    runtime->diagnostics.display_ready = true;
    /* 触摸句柄非空才视为就绪。 */
    runtime->diagnostics.touch_ready = touch != NULL;
    /* I2C 句柄已验证非空。 */
    runtime->diagnostics.i2c_ready = true;
    /* 任一 QMI 地址应答即可。 */
    runtime->diagnostics.qmi_present = qmi_low_present || qmi_high_present;
    /* 保存实际 QMI 地址；无设备时保存零。 */
    runtime->diagnostics.qmi_i2c_address = qmi_low_present
        ? BOARD_RUNTIME_QMI8658_LOW_ADDRESS
        : (qmi_high_present ? BOARD_RUNTIME_QMI8658_HIGH_ADDRESS : 0U);
    /* 探测 AXP2101 地址，只判断 ACK。 */
    runtime->diagnostics.axp2101_present = board_runtime_probe_address(
        bus,
        BOARD_RUNTIME_AXP2101_ADDRESS);
    /* 探测 PCF85063 地址，只判断 ACK。 */
    runtime->diagnostics.rtc_present = board_runtime_probe_address(
        bus,
        BOARD_RUNTIME_PCF85063_ADDRESS);
    /* TF 默认按需挂载，启动时不阻塞主页。 */
    runtime->diagnostics.storage_mounted = false;
    /* 马达初始化已经成功。 */
    runtime->diagnostics.motor_ready = true;
    /* 扬声器门控初始化已经成功且保持关闭。 */
    runtime->diagnostics.speaker_gate_ready = true;
    /* 触摸默认启用。 */
    runtime->touch_active = touch != NULL;
    /* 返回成功；缺失外设通过 diagnostics 交给自检页判定。 */
    return 0;
}

/* 保留上层显示电源合同；当前真板联调版禁用自动低功耗并优先保持官方 BSP 2.0.0 原路径。 */
int board_runtime_backend_set_display_power(board_runtime_t *runtime, bool enabled)
{
    /* 显示句柄只在官方 bsp_display_start 成功后有效，空句柄不能接受状态请求。 */
    if (runtime->platform_display == NULL) {
        /* 没有面板时返回失败，避免上层把黑屏误记为成功切换。 */
        return -1;
    }
    /* BSP 2.0.0 未公开安全的面板句柄；联调阶段只记录逻辑状态，亮度由独立公开接口执行。 */
    (void)enabled;
    /* 返回成功使初始化可继续；自动熄屏、Light-sleep 和 Deep-sleep 已由 APP_BENCH_ALWAYS_ON 拦截。 */
    return 0;
}

/* 设置 AMOLED 亮度。 */
int board_runtime_backend_set_display_brightness(board_runtime_t *runtime, uint8_t percent)
{
    /* runtime 仅用于保持平台函数签名一致。 */
    (void)runtime;
    /* 把 0~100 百分比传给厂家 BSP。 */
    return bsp_display_brightness_set((int)percent) == ESP_OK ? 0 : -1;
}

/* 启停 LVGL 触摸输入分派；不修改官方 BSP 2.0.0 的 FT3168 初始化和硬件电源路径。 */
int board_runtime_backend_set_touch_active(board_runtime_t *runtime, bool enabled)
{
    /* 没有触摸句柄时报告失败。 */
    if (runtime->platform_touch == NULL) {
        return -1;
    }
    /* 获取 BSP LVGL 锁，避免与 LVGL 任务并发修改输入设备。 */
    if (!bsp_display_lock(100U)) {
        return -1;
    }
    /* 只切换官方 LVGL 输入设备是否分派事件；当前常亮联调不会自动请求禁用。 */
    lv_indev_enable((lv_indev_t *)runtime->platform_touch, enabled);
    /* 释放 BSP LVGL 锁。 */
    bsp_display_unlock();
    /* 返回成功。 */
    return 0;
}

/* 输出非阻塞马达脉冲。 */
int board_runtime_backend_pulse_motor(
    board_runtime_t *runtime,
    uint16_t duration_ms,
    uint8_t intensity_percent)
{
    /* 定时器未创建时不能保证自动停止。 */
    if (runtime->platform_motor_timer == NULL) {
        return -1;
    }
    /* 把 1~100% 映射到 10 位占空比，使用整数四舍五入。 */
    const uint32_t duty =
        ((uint32_t)intensity_percent * BOARD_RUNTIME_MOTOR_MAX_DUTY + 50U) / 100U;
    /* 先停止可能仍在运行的旧定时器；未运行返回 INVALID_STATE，可忽略。 */
    const esp_err_t stop_result = esp_timer_stop(
        (esp_timer_handle_t)runtime->platform_motor_timer);
    /* 其它停止错误说明句柄失效。 */
    if ((stop_result != ESP_OK) && (stop_result != ESP_ERR_INVALID_STATE)) {
        return -1;
    }
    /* 写入目标占空比。 */
    if (ledc_set_duty(
            BOARD_RUNTIME_MOTOR_SPEED_MODE,
            BOARD_RUNTIME_MOTOR_CHANNEL,
            duty) != ESP_OK) {
        return -1;
    }
    /* 更新硬件输出。 */
    if (ledc_update_duty(
            BOARD_RUNTIME_MOTOR_SPEED_MODE,
            BOARD_RUNTIME_MOTOR_CHANNEL) != ESP_OK) {
        return -1;
    }
    /* 以微秒启动一次性定时器；uint16 毫秒转换不会溢出 uint64。 */
    if (esp_timer_start_once(
            (esp_timer_handle_t)runtime->platform_motor_timer,
            (uint64_t)duration_ms * UINT64_C(1000)) != ESP_OK) {
        /* 启动失败时立即关闭马达，避免持续导通。 */
        board_runtime_motor_stop_callback(runtime);
        return -1;
    }
    /* 返回成功；马达会由定时器异步关闭。 */
    return 0;
}

/* 控制扬声器功放 GPIO46。 */
int board_runtime_backend_set_speaker(board_runtime_t *runtime, bool enabled)
{
    /* GPIO46 高电平使能功放，产品默认低电平。 */
    return gpio_set_level(
               runtime->adapter.profile.speaker_enable_gpio,
               enabled ? 1U : 0U) == ESP_OK
        ? 0
        : -1;
}

/* 挂载或卸载 TF 卡。 */
int board_runtime_backend_set_storage(board_runtime_t *runtime, bool enabled)
{
    /* runtime 仅用于保持后端签名一致。 */
    (void)runtime;
    /* 厂家 BSP 使用 GPIO2/1/3 的 1-bit SDMMC 挂载。 */
    const esp_err_t result = enabled ? bsp_sdcard_mount() : bsp_sdcard_unmount();
    /* ESP_OK 表示文件系统状态已切换。 */
    return result == ESP_OK ? 0 : -1;
}

/* 获取厂家 BSP LVGL 锁。 */
bool board_runtime_backend_lvgl_lock(board_runtime_t *runtime, uint32_t timeout_ms)
{
    /* runtime 仅用于保持后端签名一致。 */
    (void)runtime;
    /* timeout_ms=0 时 BSP 无限等待。 */
    return bsp_display_lock(timeout_ms);
}

/* 释放厂家 BSP LVGL 锁。 */
void board_runtime_backend_lvgl_unlock(board_runtime_t *runtime)
{
    /* runtime 仅用于保持后端签名一致。 */
    (void)runtime;
    /* 释放与 lock 成对的 BSP 互斥量。 */
    bsp_display_unlock();
}
