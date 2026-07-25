# 注册训练协调引擎；只依赖纯 C 计数和相位组件。
idf_component_register(
# workout_engine.c 负责准备识别、热量、计数、步数、休息和暂停事实链。
    SRCS "workout_engine.c"
    # include 暴露静态引擎和快照合同。
    INCLUDE_DIRS "include"
    # 私有读取自动生成的 BpBoutAccumulator；不把 297 维特征头传播到上层组件。
    PRIV_INCLUDE_DIRS "../../../include"
    # 计数/热量与相位检测由两个独立组件提供。
    REQUIRES fitness_core motion_phase)
