# 指定 ESP-IDF 5.5.4 支持的最低 CMake 版本，保证 Windows 与 CI 使用同一工程入口。
cmake_minimum_required(VERSION 3.16)

# 引入 ESP-IDF 官方工程函数；IDF_PATH 由 export.ps1 或 ESP-IDF 终端设置。
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 启用 ESP-IDF 官方最小构建，只解析 main 显式依赖及其传递依赖，避免 Windows 把全部示例组件路径塞进同一编译命令而超过 CreateProcess 参数上限。
idf_build_set_property(MINIMAL_BUILD ON)

# 定义固件工程名；该名称同时用于生成 ELF、BIN 和构建日志标签。
project(imu_fitness_handheld)
