# 将纯 C 电源策略注册为 ESP-IDF 组件；硬件动作由主入口通过 board_adapter 执行。
idf_component_register(
    # 编译只含状态转移与功耗策略计算的纯 C 实现，便于主机替身测试。
    SRCS "power_manager.c"
    # 向主入口公开电源状态、唤醒策略和外设门控合同。
    INCLUDE_DIRS "include"
)
