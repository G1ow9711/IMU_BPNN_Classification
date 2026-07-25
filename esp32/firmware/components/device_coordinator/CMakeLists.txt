# 注册纯 C 产品应用协调器。
idf_component_register(
    # device_coordinator.c 统一会话控制、事实投影、低电和幂等摘要。
    SRCS "device_coordinator.c"
    # include 暴露静态状态、命令和按值 effect 合同。
    INCLUDE_DIRS "include"
    # 协调器只组合现有领域组件，不重复实现任何算法。
    REQUIRES workout_engine ui power_manager session_store ble_service device_config)
