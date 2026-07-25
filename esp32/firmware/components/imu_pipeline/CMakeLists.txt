# 注册实时 IMU 流水线和双 M0 适配器；模型权重仍只存在于 esp32/include 自动生成头。
idf_component_register(
    # imu_pipeline.c 实现可主机测试的数据链，dual_m0_adapter 只做现有模型函数边界转换。
    SRCS "imu_pipeline.c" "imu_pipeline_dual_m0_adapter.c"
    # 对外暴露纯 C 流水线和模型适配器合同。
    INCLUDE_DIRS "include"
    # 私有引入自动生成的特征/模型头，禁止上层复制权重数组。
    PRIV_INCLUDE_DIRS "../../../include")
