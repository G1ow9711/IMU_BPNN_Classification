/* 引入板级公开合同，确保实现与主机测试、ESP-IDF 使用同一结构。 */
#include "board_adapter.h"

/* 在 ESP-IDF 构建中读取 Kconfig；主机测试不依赖 sdkconfig.h。 */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* 返回当前编译板型；所有 GPIO 均来自用户提供的硬件原理图。 */
board_profile_t board_profile_from_build(void)
{
    /* 创建完整板型副本；字段逐项赋值便于审计引脚来源。 */
    board_profile_t profile = {
        /* 板型名称用于启动日志和上位机诊断，不参与 GPIO 计算。 */
        .profile_name = "waveshare-esp32-s3-touch-amoled-2.06",
#if defined(ESP_PLATFORM) && defined(CONFIG_BOARD_PANEL_CO5300)
        /* 新版物料选择 CO5300 AMOLED 控制器，具体 SPI/QSPI 时序由厂家 BSP 提供。 */
        .panel_controller = BOARD_PANEL_CONTROLLER_CO5300,
#else
        /* 厂家标准例程默认使用 SH8601 AMOLED 控制器。 */
        .panel_controller = BOARD_PANEL_CONTROLLER_SH8601,
#endif
#if defined(ESP_PLATFORM) && defined(CONFIG_BOARD_TOUCH_CST9220)
        /* 新版物料选择 CST9220 触摸控制器。 */
        .touch_controller = BOARD_TOUCH_CONTROLLER_CST9220,
        /* CST9220 在共用 I2C 总线上的 7 位地址固定为 0x5A。 */
        .touch_i2c_address = BOARD_TOUCH_CST9220_ADDRESS,
#else
        /* 厂家标准例程默认使用 FT3168/FT5x06 兼容触摸控制器。 */
        .touch_controller = BOARD_TOUCH_CONTROLLER_FT3168,
        /* FT3168 在共用 I2C 总线上的 7 位地址固定为 0x38。 */
        .touch_i2c_address = BOARD_TOUCH_FT3168_ADDRESS,
#endif
        /* GPIO15 承载板载 I2C SDA，连接触摸、IMU、RTC 和电源管理芯片。 */
        .i2c_sda_gpio = 15U,
        /* GPIO14 承载板载 I2C SCL，频率由各驱动协商但不得改变物理引脚。 */
        .i2c_scl_gpio = 14U,
        /* GPIO9 控制触摸控制器复位，低电平脉冲宽度由厂家 BSP 保证。 */
        .touch_reset_gpio = 9U,
        /* GPIO38 接触摸低有效中断；仅支持 Light-sleep GPIO 唤醒，不属于 RTC IO。 */
        .touch_interrupt_gpio = 38U,
        /* GPIO21 接 QMI8658 INT1；该 RTC IO 可用于高电平 EXT1 Deep-sleep 唤醒。 */
        .imu_interrupt_gpio = 21U,
        /* GPIO39 接 PCF85063 低有效中断；仅支持 Light-sleep GPIO 唤醒。 */
        .rtc_interrupt_gpio = 39U,
        /* GPIO46 控制扬声器功放使能；静音和低功耗状态必须拉到关闭电平。 */
        .speaker_enable_gpio = 46U,
        /* GPIO2 是 microSD 时钟线，休眠前需停止时钟以降低动态功耗。 */
        .sd_clk_gpio = 2U,
        /* GPIO1 是 microSD 命令线，工作电压由板载电源域决定。 */
        .sd_cmd_gpio = 1U,
        /* GPIO3 是 microSD 单线数据通道 DAT0，当前固件使用一位 SDMMC。 */
        .sd_data0_gpio = 3U,
        /* GPIO17 是兼容 SPI 模式片选脚；SDMMC 一位模式下仍保留板级合同。 */
        .sd_cs_gpio = 17U,
#if defined(ESP_PLATFORM) && defined(CONFIG_BOARD_ENABLE_QMI_DEEP_WAKE)
        /* 构建配置允许 QMI8658 WOM 通过 GPIO21 唤醒 Deep-sleep。 */
        .enable_imu_deep_wake = true,
#else
        /* 未启用时禁止依赖 IMU 唤醒，避免无有效唤醒源时进入 Deep-sleep。 */
        .enable_imu_deep_wake = false,
#endif
    };
    /* 返回值是独立结构副本，调用方可安全修改探测后的触摸类型。 */
    return profile;
}

/* 验证板型中的硬件不变量，防止错误配置在真实板上驱动冲突 GPIO。 */
board_adapter_result_t board_profile_validate(const board_profile_t *profile)
{
    /* 空指针没有可验证内容，返回参数错误。 */
    if (profile == NULL) {
        return BOARD_ADAPTER_ERR_ARGUMENT;
    }
    /* 当前板只允许原理图确定的共用 I2C 引脚 15/14。 */
    if ((profile->i2c_sda_gpio != 15U) || (profile->i2c_scl_gpio != 14U)) {
        return BOARD_ADAPTER_ERR_PROFILE;
    }
    /* 传感器中断脚属于产品合同，错配会破坏 Deep-sleep 唤醒。 */
    if (profile->imu_interrupt_gpio != 21U) {
        return BOARD_ADAPTER_ERR_PROFILE;
    }
    /* GPIO38/39 仅是 Light-sleep GPIO 唤醒脚，配置必须与本地原理图一致。 */
    if ((profile->touch_interrupt_gpio != 38U) || (profile->rtc_interrupt_gpio != 39U)) {
        return BOARD_ADAPTER_ERR_PROFILE;
    }
    /* FT3168 驱动只能配 0x38；地址错配会把其它 I2C 芯片误判为触摸。 */
    if ((profile->touch_controller == BOARD_TOUCH_CONTROLLER_FT3168) &&
        (profile->touch_i2c_address != BOARD_TOUCH_FT3168_ADDRESS)) {
        return BOARD_ADAPTER_ERR_PROFILE;
    }
    /* CST9220 驱动只能配 0x5A；地址错配时必须拒绝启动 UI。 */
    if ((profile->touch_controller == BOARD_TOUCH_CONTROLLER_CST9220) &&
        (profile->touch_i2c_address != BOARD_TOUCH_CST9220_ADDRESS)) {
        return BOARD_ADAPTER_ERR_PROFILE;
    }
    /* UNKNOWN 只能作为探测中间态，正式初始化不接受未知触摸。 */
    if (profile->touch_controller == BOARD_TOUCH_CONTROLLER_UNKNOWN) {
        return BOARD_ADAPTER_ERR_PROFILE;
    }
    /* 所有板级不变量均满足，允许后续初始化真实 BSP 或 mock。 */
    return BOARD_ADAPTER_OK;
}

/* 探测厂家文档中冲突的两种触摸芯片，避免盲目写错寄存器。 */
board_adapter_result_t board_detect_touch_controller(
    board_i2c_probe_fn probe,
    void *probe_context,
    board_touch_controller_t *detected_controller)
{
    /* 探测回调和输出都不可为空，否则无法获得可靠结果。 */
    if ((probe == NULL) || (detected_controller == NULL)) {
        return BOARD_ADAPTER_ERR_ARGUMENT;
    }
    /* 记录 FT3168 地址是否应答；该调用只做 I2C ACK 探测，不改寄存器。 */
    const bool ft3168_present = probe(probe_context, BOARD_TOUCH_FT3168_ADDRESS);
    /* 记录 CST9220 地址是否应答；与 FT3168 探测相互独立。 */
    const bool cst9220_present = probe(probe_context, BOARD_TOUCH_CST9220_ADDRESS);
    /* 两个地址同时应答代表总线上还有未知设备或板型配置不明确，禁止猜测。 */
    if (ft3168_present && cst9220_present) {
        *detected_controller = BOARD_TOUCH_CONTROLLER_UNKNOWN;
        return BOARD_ADAPTER_ERR_AMBIGUOUS;
    }
    /* 仅 0x38 应答时选择 FT3168/FT5x06 兼容驱动。 */
    if (ft3168_present) {
        *detected_controller = BOARD_TOUCH_CONTROLLER_FT3168;
        return BOARD_ADAPTER_OK;
    }
    /* 仅 0x5A 应答时选择 CST9220 驱动。 */
    if (cst9220_present) {
        *detected_controller = BOARD_TOUCH_CONTROLLER_CST9220;
        return BOARD_ADAPTER_OK;
    }
    /* 两个地址都不应答时标记未知，交由启动自检显示硬件故障。 */
    *detected_controller = BOARD_TOUCH_CONTROLLER_UNKNOWN;
    return BOARD_ADAPTER_ERR_IO;
}

/* 初始化板级适配器；不在这里访问硬件，便于主机测试注入 mock。 */
board_adapter_result_t board_adapter_init(
    board_adapter_t *adapter,
    const board_profile_t *profile,
    const board_adapter_ops_t *ops)
{
    /* 三个输入都必须存在；适配器不拥有外部分配器或默认隐式驱动。 */
    if ((adapter == NULL) || (profile == NULL) || (ops == NULL)) {
        return BOARD_ADAPTER_ERR_ARGUMENT;
    }
    /* 先验证引脚和控制器合同，避免保存非法板型。 */
    const board_adapter_result_t validation = board_profile_validate(profile);
    /* 验证失败时原样返回具体错误，调用方可在自检页展示。 */
    if (validation != BOARD_ADAPTER_OK) {
        return validation;
    }
    /* 复制板型，保证调用方临时结构离开作用域后仍然有效。 */
    adapter->profile = *profile;
    /* 复制回调表；回调指向的 context 生命周期由调用方保证。 */
    adapter->ops = *ops;
    /* 最后写入初始化标志，避免部分初始化对象被误用。 */
    adapter->initialized = true;
    /* 返回成功，表示纯软件合同已建立；不代表真实硬件已通过自检。 */
    return BOARD_ADAPTER_OK;
}

/* 统一控制屏幕电源和亮度，保证关闭顺序不会产生亮屏闪烁。 */
board_adapter_result_t board_adapter_set_display(
    board_adapter_t *adapter,
    bool enabled,
    uint8_t brightness_percent)
{
    /* 适配器必须完成初始化，亮度范围不得超过百分比上限。 */
    if ((adapter == NULL) || !adapter->initialized || (brightness_percent > 100U)) {
        return BOARD_ADAPTER_ERR_ARGUMENT;
    }
    /* 两个屏幕回调都必须提供，否则无法保证开关和亮度状态一致。 */
    if ((adapter->ops.set_display_power == NULL) ||
        (adapter->ops.set_display_brightness == NULL)) {
        return BOARD_ADAPTER_ERR_UNSUPPORTED;
    }
    /* 关闭时先把亮度降为零，避免面板休眠命令前闪出旧帧。 */
    if (!enabled) {
        if (adapter->ops.set_display_brightness(adapter->ops.context, 0U) != 0) {
            return BOARD_ADAPTER_ERR_IO;
        }
        if (adapter->ops.set_display_power(adapter->ops.context, false) != 0) {
            return BOARD_ADAPTER_ERR_IO;
        }
        return BOARD_ADAPTER_OK;
    }
    /* 开启时先唤醒面板，再设置目标亮度，避免驱动在睡眠态忽略亮度命令。 */
    if (adapter->ops.set_display_power(adapter->ops.context, true) != 0) {
        return BOARD_ADAPTER_ERR_IO;
    }
    /* 把用户/电源策略给出的百分比传给具体面板驱动。 */
    if (adapter->ops.set_display_brightness(adapter->ops.context, brightness_percent) != 0) {
        return BOARD_ADAPTER_ERR_IO;
    }
    /* 屏幕电源和亮度均设置完成。 */
    return BOARD_ADAPTER_OK;
}

/* 读取 AXP2101 状态；该层只验证范围，不推断电芯电压。 */
board_adapter_result_t board_adapter_read_battery(
    board_adapter_t *adapter,
    uint8_t *percent,
    bool *charging)
{
    /* 适配器和两个输出指针都必须有效。 */
    if ((adapter == NULL) || !adapter->initialized || (percent == NULL) || (charging == NULL)) {
        return BOARD_ADAPTER_ERR_ARGUMENT;
    }
    /* 缺少 PMIC 回调时返回不支持，UI 应显示未知电量而非 0%。 */
    if (adapter->ops.read_battery == NULL) {
        return BOARD_ADAPTER_ERR_UNSUPPORTED;
    }
    /* 调用 PMIC/mock 读取函数，输出值由调用方提供的存储接收。 */
    if (adapter->ops.read_battery(adapter->ops.context, percent, charging) != 0) {
        return BOARD_ADAPTER_ERR_IO;
    }
    /* 百分比大于 100 说明驱动或寄存器解析错误，拒绝把坏值送入低电量策略。 */
    if (*percent > 100U) {
        return BOARD_ADAPTER_ERR_IO;
    }
    /* 电量和充电状态均通过基本范围检查。 */
    return BOARD_ADAPTER_OK;
}
