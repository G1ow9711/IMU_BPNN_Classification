# 注册纯 C 会话存储组件；LittleFS/TF 驱动通过函数表注入，不形成硬件依赖。
idf_component_register(
    # 三个源文件分别负责双槽摘要、主机内存后端和原始 IMU 块。
    SRCS "src/session_store.c" "src/session_memory_backend.c" "src/session_file_backend.c" "src/session_raw_log.c"
    # 公共头目录暴露版本化格式和后端接口。
    INCLUDE_DIRS "include"
)
