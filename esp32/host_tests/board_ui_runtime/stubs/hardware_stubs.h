#ifndef IMU_HANDHELD_HARDWARE_STUBS_H
#define IMU_HANDHELD_HARDWARE_STUBS_H

/* 本文件只用于主机 `-fsyntax-only`，签名对应 ESP-IDF 5.5.4 与 Waveshare BSP v1.0.7。 */
#include <stdbool.h>
#include <stdint.h>

/* 用整数模拟 ESP-IDF 错误码。 */
typedef int esp_err_t;
/* 成功错误码。 */
#define ESP_OK (0)
/* 定时器未运行时的合法停止结果。 */
#define ESP_ERR_INVALID_STATE (0x103)
/* 模拟 ESP-IDF 片内堆能力标志，用于验证 taskLVGL 栈不会分配到 PSRAM。 */
#define MALLOC_CAP_INTERNAL (1U << 0)
/* 模拟 ESP-IDF 默认可分配内存能力标志。 */
#define MALLOC_CAP_DEFAULT (1U << 1)

/* 用不透明指针模拟 I2C 总线句柄。 */
typedef void *i2c_master_bus_handle_t;
/* 声明 ESP-IDF 5.5 I2C ACK 探测函数。 */
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout_ms);

/* 用整数模拟 GPIO 枚举。 */
typedef int gpio_num_t;
/* GPIO 输出模式常量。 */
#define GPIO_MODE_OUTPUT (1)
/* 禁用上拉。 */
#define GPIO_PULLUP_DISABLE (0)
/* 禁用下拉。 */
#define GPIO_PULLDOWN_DISABLE (0)
/* 禁用 GPIO 中断。 */
#define GPIO_INTR_DISABLE (0)
/* 保存 GPIO 配置字段。 */
typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;
/* 声明 GPIO 配置函数。 */
esp_err_t gpio_config(const gpio_config_t *config);
/* 声明 GPIO 输出函数。 */
esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level);

/* LEDC 类型使用整数模拟；仅校验字段和调用签名。 */
typedef int ledc_mode_t;
typedef int ledc_timer_t;
typedef int ledc_channel_t;
typedef int ledc_timer_bit_t;
/* 固定真实代码使用的枚举值占位。 */
#define LEDC_LOW_SPEED_MODE (0)
#define LEDC_TIMER_0 (0)
#define LEDC_CHANNEL_0 (0)
#define LEDC_TIMER_10_BIT (10)
#define LEDC_AUTO_CLK (0)
#define LEDC_INTR_DISABLE (0)
#define LEDC_SLEEP_MODE_NO_ALIVE_NO_PD (0)
/* 保存 LEDC 定时器配置字段。 */
typedef struct {
    ledc_mode_t speed_mode;
    ledc_timer_bit_t duty_resolution;
    ledc_timer_t timer_num;
    int freq_hz;
    int clk_cfg;
    bool deconfigure;
} ledc_timer_config_t;
/* 保存 LEDC 通道配置字段。 */
typedef struct {
    int gpio_num;
    ledc_mode_t speed_mode;
    ledc_channel_t channel;
    int intr_type;
    ledc_timer_t timer_sel;
    uint32_t duty;
    int hpoint;
    int sleep_mode;
    struct {
        unsigned int output_invert : 1;
    } flags;
} ledc_channel_config_t;
/* 声明 LEDC 定时器配置函数。 */
esp_err_t ledc_timer_config(const ledc_timer_config_t *config);
/* 声明 LEDC 通道配置函数。 */
esp_err_t ledc_channel_config(const ledc_channel_config_t *config);
/* 声明 LEDC 占空比写入函数。 */
esp_err_t ledc_set_duty(ledc_mode_t mode, ledc_channel_t channel, uint32_t duty);
/* 声明 LEDC 硬件更新函数。 */
esp_err_t ledc_update_duty(ledc_mode_t mode, ledc_channel_t channel);

/* 用不透明指针模拟 esp_timer 句柄。 */
typedef void *esp_timer_handle_t;
/* 定时器任务分派常量。 */
#define ESP_TIMER_TASK (0)
/* 保存一次性定时器创建字段。 */
typedef struct {
    void (*callback)(void *argument);
    void *arg;
    int dispatch_method;
    const char *name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;
/* 声明创建、停止和启动函数。 */
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);

/* 声明 LVGL 不透明显示与输入类型。 */
typedef struct lv_display_t lv_display_t;
typedef struct lv_indev_t lv_indev_t;
/* 声明 LVGL 9 输入开关。 */
void lv_indev_enable(lv_indev_t *indev, bool enabled);

/* 模拟 esp_lvgl_port 任务唤醒事件类型。 */
typedef enum {
    /* 用户事件只唤醒 taskLVGL，不携带显示或触摸业务对象。 */
    LVGL_PORT_EVENT_USER = 0x80,
} lvgl_port_event_type_t;
/* 保存真实 esp_lvgl_port 初始化合同中的任务调度字段。 */
typedef struct {
    /* taskLVGL 的 FreeRTOS 优先级。 */
    int task_priority;
    /* taskLVGL 的任务栈字节数。 */
    int task_stack;
    /* taskLVGL 的 CPU 亲和性；-1 表示不固定核心。 */
    int task_affinity;
    /* taskLVGL 等待事件的最长毫秒数。 */
    int task_max_sleep_ms;
    /* taskLVGL 栈使用的 ESP-IDF 堆能力组合。 */
    unsigned task_stack_caps;
    /* LVGL tick 定时器周期，单位为毫秒。 */
    int timer_period_ms;
} lvgl_port_cfg_t;
/* 使用与 esp_lvgl_port 3.3.2 一致的默认任务配置，供真实后端语法检查。 */
#define ESP_LVGL_PORT_INIT_CONFIG()                                       \
    {                                                                     \
        .task_priority = 4,                                               \
        .task_stack = 7168,                                               \
        .task_affinity = -1,                                              \
        .task_max_sleep_ms = 500,                                         \
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,      \
        .timer_period_ms = 5,                                             \
    }
/* 模拟 Waveshare BSP 的显示启动配置，不替代真实面板初始化。 */
typedef struct {
    /* 保存 taskLVGL 调度配置。 */
    lvgl_port_cfg_t lvgl_port_cfg;
    /* 保存 LVGL 像素缓冲大小。 */
    uint32_t buffer_size;
    /* 保存可选传输缓冲大小；当前产品配置保持零值。 */
    uint32_t trans_size;
    /* 标记是否分配双像素缓冲。 */
    bool double_buffer;
    /* 保存像素缓冲的 DMA 与 PSRAM 能力。 */
    struct {
        /* 非零表示像素缓冲必须具备 DMA 能力。 */
        unsigned int buff_dma : 1;
        /* 非零表示像素缓冲分配到 PSRAM。 */
        unsigned int buff_spiram : 1;
    } flags;
} bsp_display_cfg_t;
/* 模拟厂家按屏幕宽度计算的绘制缓冲像素数。 */
#define BSP_LCD_DRAW_BUFF_SIZE (410U * 10U)
/* 模拟当前厂家 BSP 的单缓冲默认值。 */
#define BSP_LCD_DRAW_BUFF_DOUBLE (0)

/* 声明 Waveshare BSP v1.0.7 实际使用的公开函数。 */
lv_display_t *bsp_display_start(void);
/* 声明厂家公开的带配置显示启动入口。 */
lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *config);
lv_indev_t *bsp_display_get_input_dev(void);
bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);
esp_err_t bsp_display_brightness_set(int brightness_percent);
/* 声明板级运行时使用的 AMOLED 面板供电开关；参数 true 表示恢复显示供电。 */
esp_err_t bsp_display_panel_power_set(bool enabled);
/* 声明板级运行时使用的触摸硬件开关；参数 false 表示进入屏幕休眠路径。 */
esp_err_t bsp_touch_hardware_active_set(bool enabled);
esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);
esp_err_t bsp_sdcard_mount(void);
esp_err_t bsp_sdcard_unmount(void);
/* 声明官方 esp_lvgl_port 用户事件唤醒入口；parameter 允许为空，非空参数的生命周期仅覆盖同步调用且函数不保存该指针。 */
esp_err_t lvgl_port_task_wake(lvgl_port_event_type_t event, void *parameter);

#endif
