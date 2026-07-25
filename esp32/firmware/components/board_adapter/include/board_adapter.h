#ifndef IMU_HANDHELD_BOARD_ADAPTER_H
#define IMU_HANDHELD_BOARD_ADAPTER_H

/* 引入定长整数、布尔值和尺寸类型，保证主机测试与 ESP32 使用相同数据宽度。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 固定 AMOLED 可视宽度为 410 像素，与厂家 2.06 英寸面板一致。 */
#define BOARD_DISPLAY_WIDTH_PX (410U)
/* 固定 AMOLED 可视高度为 502 像素，与厂家 2.06 英寸面板一致。 */
#define BOARD_DISPLAY_HEIGHT_PX (502U)
/* 固定 FT3168 的 7 位 I2C 地址，用于启动阶段芯片探测。 */
#define BOARD_TOUCH_FT3168_ADDRESS (0x38U)
/* 固定 CST9220 的 7 位 I2C 地址，用于兼容新版厂家物料。 */
#define BOARD_TOUCH_CST9220_ADDRESS (0x5AU)

/* 描述可选 AMOLED 控制器；面板类型由 Kconfig/出厂配置确定，不能盲目试写。 */
typedef enum {
    /* 选择厂家 SH8601 AMOLED 控制器及其初始化命令序列。 */
    BOARD_PANEL_CONTROLLER_SH8601 = 0,
    /* 选择兼容物料 CO5300 AMOLED 控制器及其独立初始化序列。 */
    BOARD_PANEL_CONTROLLER_CO5300 = 1
} board_panel_controller_t;

/* 描述电容触摸控制器；两种芯片共享 I2C 总线，但寄存器协议不同。 */
typedef enum {
    /* 触摸芯片为 FT3168，使用 7 位 I2C 地址 0x38。 */
    BOARD_TOUCH_CONTROLLER_FT3168 = 0,
    /* 触摸芯片为 CST9220，使用 7 位 I2C 地址 0x5A。 */
    BOARD_TOUCH_CONTROLLER_CST9220 = 1,
    /* 探测无响应或多个地址同时响应，禁止猜测寄存器协议。 */
    BOARD_TOUCH_CONTROLLER_UNKNOWN = 2
} board_touch_controller_t;

/* 描述板级操作结果；负值表示调用未完成，零表示成功。 */
typedef enum {
    /* 操作完成且全部输出参数有效。 */
    BOARD_ADAPTER_OK = 0,
    /* 必填指针为空或数值超出公开范围。 */
    BOARD_ADAPTER_ERR_ARGUMENT = -1,
    /* 板型引脚、显示尺寸或芯片地址合同无效。 */
    BOARD_ADAPTER_ERR_PROFILE = -2,
    /* 当前 BSP/Mock 未提供请求的硬件能力。 */
    BOARD_ADAPTER_ERR_UNSUPPORTED = -3,
    /* 底层 GPIO、I2C、显示、存储或 PMIC 操作失败。 */
    BOARD_ADAPTER_ERR_IO = -4,
    /* 触摸探测出现多个应答，无法唯一选择驱动。 */
    BOARD_ADAPTER_ERR_AMBIGUOUS = -5
} board_adapter_result_t;

/* 汇总硬件引脚和板型信息；单位为 GPIO 编号、像素和 I2C 7 位地址。 */
typedef struct {
    /* 记录人类可读板型名称；指针指向静态只读字符串，生命周期覆盖整个程序。 */
    const char *profile_name;
    /* 记录编译选择的 AMOLED 控制器，供真实 BSP 初始化时分派驱动。 */
    board_panel_controller_t panel_controller;
    /* 记录触摸控制器；启动探测成功后可由实际结果覆盖编译默认值。 */
    board_touch_controller_t touch_controller;
    /* 记录触摸 7 位 I2C 地址，合法值仅为 0x38 或 0x5A。 */
    uint8_t touch_i2c_address;
    /* 记录共用 I2C SDA 引脚；当前硬件为 GPIO15。 */
    uint8_t i2c_sda_gpio;
    /* 记录共用 I2C SCL 引脚；当前硬件为 GPIO14。 */
    uint8_t i2c_scl_gpio;
    /* 记录触摸复位引脚；当前硬件为 GPIO9。 */
    uint8_t touch_reset_gpio;
    /* 记录触摸中断引脚；本地原理图为 GPIO38，仅能作为 Light-sleep GPIO 唤醒。 */
    uint8_t touch_interrupt_gpio;
    /* 记录 QMI8658 INT1 引脚；当前硬件为 GPIO21，可作为 Deep-sleep RTC GPIO 唤醒。 */
    uint8_t imu_interrupt_gpio;
    /* 记录 PCF85063 中断引脚；本地原理图为 GPIO39，仅能作为 Light-sleep GPIO 唤醒。 */
    uint8_t rtc_interrupt_gpio;
    /* 记录扬声器功放使能引脚；当前硬件为 GPIO46。 */
    uint8_t speaker_enable_gpio;
    /* 记录 TF 卡 CLK 引脚；当前硬件为 GPIO2。 */
    uint8_t sd_clk_gpio;
    /* 记录 TF 卡 CMD 引脚；当前硬件为 GPIO1。 */
    uint8_t sd_cmd_gpio;
    /* 记录 TF 卡 D0 引脚；当前硬件为 GPIO3。 */
    uint8_t sd_data0_gpio;
    /* 记录 TF 卡 CS 引脚；当前硬件为 GPIO17。 */
    uint8_t sd_cs_gpio;
    /* 标记是否允许 QMI8658 运动中断从 Deep-sleep 唤醒；产品默认启用但真机必须校准误唤醒率。 */
    bool enable_imu_deep_wake;
} board_profile_t;

/* 抽象实际 BSP 或主机 mock；所有回调都由调用方持有且必须覆盖适配器生命周期。 */
typedef struct {
    /* 保存驱动私有上下文；适配层只透传，不解引用。 */
    void *context;
    /* 控制 AMOLED 供电/睡眠；enabled=true 表示点亮面板。 */
    int (*set_display_power)(void *context, bool enabled);
    /* 设置 AMOLED 亮度百分比，输入范围为 0~100。 */
    int (*set_display_brightness)(void *context, uint8_t percent);
    /* 控制触摸工作/监视模式；enabled=false 表示进入低功耗监视或休眠。 */
    int (*set_touch_active)(void *context, bool enabled);
    /* 控制扬声器功放；enabled=false 时 GPIO46 必须保持关闭。 */
    int (*set_speaker_power)(void *context, bool enabled);
    /* 挂载或卸载 TF；enabled=false 时应结束写入并释放 SDMMC。 */
    int (*set_storage_active)(void *context, bool enabled);
    /* 读取 AXP2101 电池百分比与充电状态；百分比有效范围为 0~100。 */
    int (*read_battery)(void *context, uint8_t *percent, bool *charging);
} board_adapter_ops_t;

/* 保存已验证板型与驱动回调；结构可静态分配，不使用动态内存。 */
typedef struct {
    /* 保存当前板型副本，避免调用方临时对象失效。 */
    board_profile_t profile;
    /* 保存 BSP/mock 回调副本，供后续所有板级操作使用。 */
    board_adapter_ops_t ops;
    /* 标记初始化是否完成；未初始化时所有硬件操作都会拒绝。 */
    bool initialized;
} board_adapter_t;

/* 定义 I2C 地址探测函数；返回 true 表示指定 7 位地址收到 ACK。 */
typedef bool (*board_i2c_probe_fn)(void *context, uint8_t address);

/* 根据 ESP-IDF Kconfig 生成当前硬件默认板型；主机测试固定使用 SH8601+FT3168。 */
board_profile_t board_profile_from_build(void);
/* 检查引脚、尺寸和芯片地址合同；返回 BOARD_ADAPTER_OK 表示可安全初始化。 */
board_adapter_result_t board_profile_validate(const board_profile_t *profile);
/*
 * 同时探测 0x38 与 0x5A；恰好一个响应时返回对应触摸控制器。
 * probe 必须非空；probe_context 可为空，由 probe 自行解释，本函数只在同步探测调用期间借用，
 * 不保存、不释放且不延长其生命周期。detected_controller 必须非空并由调用者持有。
 */
board_adapter_result_t board_detect_touch_controller(
    /* 非空 I2C 探测回调；每个候选 7 位地址同步调用一次。 */
    board_i2c_probe_fn probe,
    /* 可为空回调上下文；生命周期覆盖本次函数调用，所有权始终归调用者。 */
    void *probe_context,
    /* 非空输出；成功时写入唯一探测到的触摸控制器枚举。 */
    board_touch_controller_t *detected_controller);
/* 用已验证板型和驱动回调初始化适配器；函数不访问真实硬件。 */
board_adapter_result_t board_adapter_init(
    board_adapter_t *adapter,
    const board_profile_t *profile,
    const board_adapter_ops_t *ops);
/* 设置屏幕开关和亮度；关闭时先把亮度降为零，再关闭面板。 */
board_adapter_result_t board_adapter_set_display(
    board_adapter_t *adapter,
    bool enabled,
    uint8_t brightness_percent);
/* 读取电池状态；输出指针不能为空。 */
board_adapter_result_t board_adapter_read_battery(
    board_adapter_t *adapter,
    uint8_t *percent,
    bool *charging);

#ifdef __cplusplus
}
#endif

#endif
