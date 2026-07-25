# 从组件目录向上四级定位仓库根，复用 shared/protocol 的唯一 C 帧实现。
get_filename_component(BLE_SERVICE_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

# 注册设备端 BLE 服务组件；纯核心、NimBLE 适配和共享协议共同进入固件。
idf_component_register(
    # ble_service_core.c 可在主机独立测试，ble_service_nimble.c 绑定 ESP-IDF 5.5.4 ESP-NimBLE。
    SRCS
        "src/ble_service_core.c"
        "src/ble_service_manifest.c"
        # ble_service_bond_manager.c 编排“断开当前 PC+删除全部绑定”，纯 C 逻辑同时用于主机测试。
        "src/ble_service_bond_manager.c"
        "src/ble_service_nimble.c"
        "${BLE_SERVICE_REPOSITORY_ROOT}/shared/protocol/imu_ble_protocol.c"
    # 公开本组件接口和共享协议头，main 不需要复制类型声明。
    INCLUDE_DIRS
        "include"
        "${BLE_SERVICE_REPOSITORY_ROOT}/shared/protocol"
    # NimBLE、单调时钟和硬件随机数只作为组件私有实现依赖。
    PRIV_REQUIRES
        bt
        esp_timer
        esp_hw_support
        # FreeRTOS 提供有界业务队列和独立任务，确保 GATT 回调不等待应用事务。
        freertos
)
