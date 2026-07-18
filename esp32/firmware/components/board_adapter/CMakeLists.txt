# 将板级适配实现注册为 ESP-IDF 组件，供主入口、界面和电源管理层调用。
idf_component_register(
    # 编译板型合同、GPIO 映射、触摸探测和外设回调适配实现。
    SRCS "board_adapter.c"
    # 向同级组件公开 board_adapter.h；头文件不依赖具体厂家 BSP。
    INCLUDE_DIRS "include"
)
