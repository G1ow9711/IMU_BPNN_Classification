# 向 ESP-IDF 注册纯 C 健身领域核心；该组件不依赖任何硬件驱动组件。
idf_component_register(
# fitness_core.c 包含计数、会话和整数热量实现。
    SRCS "src/fitness_core.c"
    # include 作为公共头文件目录，供 main、BLE、UI 和主机镜像共同引用。
    INCLUDE_DIRS "include"
)
