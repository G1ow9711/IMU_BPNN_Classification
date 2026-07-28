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
/* 引入 ESP-IDF 错误码。 */
#include "esp_err.h"
/* 引入任务栈内存能力标志，确保 taskLVGL 栈位于 Flash 缓存关闭时仍可访问的片内 RAM。 */
#include "esp_heap_caps.h"
/* 引入官方 LVGL 端口任务配置和用户唤醒事件 API。 */
#include "esp_lvgl_port.h"
/* 引入 LVGL 9 输入设备开关 API。 */
#include "lvgl.h"

/* 固定 I2C 地址探测超时为 50 ms，避免坏设备阻塞启动页。 */
#define BOARD_RUNTIME_I2C_PROBE_TIMEOUT_MS (50)
/* taskLVGL 优先级高于 BLE 发布任务 6，低于应用所有者 9 和 QMI 采样任务 10。 */
#define BOARD_RUNTIME_LVGL_TASK_PRIORITY (7U)
/* 12 KiB 片内栈覆盖中文页面、触摸与异步刷新调用链，并保留运行余量。 */
#define BOARD_RUNTIME_LVGL_TASK_STACK_BYTES (12U * 1024U)
/* 最长睡眠缩短到 100 ms，使触摸和健康唤醒不会被默认 500 ms 等待放大。 */
#define BOARD_RUNTIME_LVGL_TASK_MAX_SLEEP_MS (100U)

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
    /*
     * 复制厂家 bsp_display_start() 的公开默认配置，只覆盖 taskLVGL 调度参数。
     * 面板初始化、触摸控制器、缓冲尺寸、PSRAM 像素缓冲和异步 flush 路径均保持厂家实现。
     */
    bsp_display_cfg_t display_config = {
        /* 使用 esp_lvgl_port 官方默认值建立完整配置，避免遗漏未来新增字段。 */
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        /* 保持厂家按水平分辨率和 bounce-buffer 高度计算的像素缓冲大小。 */
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        /* 保持厂家单缓冲策略，禁止本次修复同时改变 QSPI 刷新内存模型。 */
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        /* 像素缓冲继续放 PSRAM；只有任务栈固定在片内 RAM。 */
        .flags = {
            /* 当前厂家 QSPI 路径不要求 LVGL 像素缓冲具备 DMA 能力。 */
            .buff_dma = false,
            /* 大型像素缓冲保留在 PSRAM，避免挤占 BLE 和业务任务的片内内存。 */
            .buff_spiram = true,
        },
    };
    /* 提高 taskLVGL 优先级，避免 125 Hz IMU 与 BLE 发布同时活跃时长期得不到调度。 */
    display_config.lvgl_port_cfg.task_priority = BOARD_RUNTIME_LVGL_TASK_PRIORITY;
    /* 扩大 taskLVGL 栈，覆盖复杂中文 UI、触摸和异步刷新调用链。 */
    display_config.lvgl_port_cfg.task_stack = BOARD_RUNTIME_LVGL_TASK_STACK_BYTES;
    /* 栈必须位于片内默认内存，Flash 写入暂时关闭缓存时仍可安全执行。 */
    display_config.lvgl_port_cfg.task_stack_caps =
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT;
    /* 缩短事件等待上界，使触摸与界面刷新在 100 ms 内获得一次调度机会。 */
    display_config.lvgl_port_cfg.task_max_sleep_ms =
        BOARD_RUNTIME_LVGL_TASK_MAX_SLEEP_MS;
    /* 通过厂家公开配置入口启动显示，不复制或修改面板、触摸初始化实现。 */
    lv_display_t *display = bsp_display_start_with_config(&display_config);
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

/* 向官方 esp_lvgl_port 任务队列发送用户事件，只解除等待而不直接操作页面。 */
int board_runtime_backend_wake_display_task(board_runtime_t *runtime)
{
    /* 未持有显示句柄表示 taskLVGL 尚未成功创建，禁止向空队列报告恢复成功。 */
    if ((runtime == NULL) || (runtime->platform_display == NULL)) {
        /* 返回失败供上层保留诊断，不触发整机重启。 */
        return -1;
    }
    /* USER 事件无业务参数，仅用于让 taskLVGL 立即离开事件等待并运行 timer handler。 */
    const esp_err_t result = lvgl_port_task_wake(LVGL_PORT_EVENT_USER, NULL);
    /* ESP_OK 表示事件已成功写入官方端口队列。 */
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
