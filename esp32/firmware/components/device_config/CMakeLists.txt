# 注册纯 C 设备配置组件，生产固件和主机测试共用同一实现。
idf_component_register(
    # 设备命令 TLV、配置事务和 CRC32 blob 全部位于单一源码中。
    SRCS "device_config.c"
    # 公开头只暴露稳定值对象，不依赖 BLE、FreeRTOS、NVS 或文件系统。
    INCLUDE_DIRS "include")
