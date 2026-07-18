# 把纯 C 核心与 ESP-IDF 新 I2C 适配器注册为独立组件。
idf_component_register(
    # 列出生产源；主机测试只单独编译 board_sensors.c。
    SRCS "board_sensors.c" "board_sensors_esp_idf.c"
    # 暴露统一板载传感器公共头。
    INCLUDE_DIRS "include"
    # 新 I2C 驱动来自 IDF 5.x，延时依赖 FreeRTOS。
    REQUIRES esp_driver_i2c freertos
)
