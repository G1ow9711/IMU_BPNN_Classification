# 注册 ESP-IDF 生产入口，并显式声明全部跨任务组件，避免链接阶段依赖隐式传递。
idf_component_register(
    # main.c 负责真实 BSP、传感器、双 M0、业务、UI、BLE、存储和低功耗总编排。
    SRCS "main.c"
    # 当前目录没有额外公共头，只暴露本目录给私有辅助声明。
    INCLUDE_DIRS "."
    # 公开依赖覆盖真实生产数据链和 ESP-IDF 平台服务。
    REQUIRES
        # 板级运行时负责厂家 BSP、LVGL 锁、AMOLED、触摸、TF 与马达端口。
        board_runtime
        # 板载传感器组件负责 QMI8658、AXP2101、PCF85063 及共享 I2C 适配。
        board_sensors
        # IMU 流水线负责异步 ODR、抗混叠、25 Hz 重采样、297 特征和双 M0。
        imu_pipeline
        # 应用协调器是训练状态、指标、UI/BLE/存储/振动效果的唯一写者。
        device_coordinator
        # 设备配置提供命令 TLV、范围验证和带 CRC32 的 NVS blob。
        device_config
        # 会话存储提供 CRC32 双槽摘要和最近 200 条恢复。
        session_store
        # 会话传输提供 BLE LIST/GET 分页与分块下载。
        session_transfer
        # BLE 服务提供 NimBLE GATT、幂等命令、实时状态和事件通知。
        ble_service
        # UI 组件提供页面状态机、纯 presenter 和 LVGL 9 渲染器。
        ui
        # 电源管理组件提供 400 mAh 预算、低电门槛和功耗状态机。
        power_manager
        # NVS 保存 NimBLE 绑定、设备偏好和 ESP-IDF 系统状态。
        nvs_flash
        # LittleFS 承载双槽会话摘要文件。
        littlefs
        # GPIO 驱动用于 GPIO38/39 Light-sleep 低电平唤醒配置。
        esp_driver_gpio
        # 高精度单调时钟用于 IMU 时间戳、振动污染区和任务节拍。
        esp_timer
        # ESP PM 提供动态调频、自动 Light-sleep 和推理频率锁。
        esp_pm
        # FreeRTOS 提供九任务、六队列、互斥锁和事件组。
        freertos)
