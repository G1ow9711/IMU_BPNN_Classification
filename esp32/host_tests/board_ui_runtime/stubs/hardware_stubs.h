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

/* 声明 Waveshare BSP v1.0.7 实际使用的公开函数。 */
lv_display_t *bsp_display_start(void);
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

#endif
