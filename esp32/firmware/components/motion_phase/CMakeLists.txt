# 注册纯 C 动作相位检测器；组件只依赖 fitness_core 的动作和相位枚举。
idf_component_register(
    # motion_phase.c 实现跳跃、双向旋转和步峰三条 O(1) 检测路径。
    SRCS "motion_phase.c"
    # include 暴露静态状态、输入单位和输出合同。
    INCLUDE_DIRS "include"
    # fitness_core 提供 11 类动作与计数相位枚举。
    REQUIRES fitness_core)
