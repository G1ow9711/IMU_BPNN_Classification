# 根据 Kconfig 选择真实 Waveshare BSP 或主机/固件 Mock 后端，避免同名符号重复。
if(CONFIG_BOARD_RUNTIME_REAL)
    # 真实后端接厂家 BSP、ESP-IDF GPIO/LEDC/I2C 和 esp_timer。
    set(BOARD_RUNTIME_BACKEND_SOURCE "board_runtime_esp.c")
else()
    # Mock 后端不访问硬件，适合无板调试和界面联调。
    set(BOARD_RUNTIME_BACKEND_SOURCE "board_runtime_mock.c")
endif()

# 注册板级运行时公共逻辑和所选后端。
idf_component_register(
    SRCS "board_runtime.c" "${BOARD_RUNTIME_BACKEND_SOURCE}"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES board_adapter
    PRIV_REQUIRES waveshare__esp32_s3_touch_amoled_2_06 lvgl esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_timer
)
