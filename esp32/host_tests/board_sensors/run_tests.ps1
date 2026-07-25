# 任一外部命令失败时，由下方退出码检查转成终止错误。
$ErrorActionPreference = "Stop"
# 获取当前测试脚本目录，避免依赖调用者工作目录。
$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
# 从 host_tests/board_sensors 向上两级得到 esp32 根目录。
$esp32Root = Resolve-Path (Join-Path $testRoot "..\..")
# 从 esp32 根目录向上一级得到仓库根目录。
$projectRoot = Resolve-Path (Join-Path $esp32Root "..")
# 把二进制放到项目本地忽略目录，避免污染 Git 状态。
$buildRoot = Join-Path $projectRoot ".codex-local\tmp\board_sensors_host_tests"
# 创建或复用本地构建目录。
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
# 生成测试二进制绝对路径。
$binaryPath = Join-Path $buildRoot "board_sensors_tests.exe"
# 生成 ESP-IDF 适配器主机 stub 测试二进制绝对路径。
$adapterBinaryPath = Join-Path $buildRoot "board_sensors_esp_idf_tests.exe"
# 定义组件公开头目录。
$includePath = Join-Path $esp32Root "firmware\components\board_sensors\include"
# 定义纯 C 生产源路径。
$sourcePath = Join-Path $esp32Root "firmware\components\board_sensors\board_sensors.c"
# 定义 ESP-IDF 新 I2C 适配器源路径。
$idfSourcePath = Join-Path $esp32Root "firmware\components\board_sensors\board_sensors_esp_idf.c"
# 定义只用于无 IDF 环境语法检查的最小头文件 stub。
$stubInclude = Join-Path $testRoot "stubs"
# 定义主机 fake 测试源路径。
$testPath = Join-Path $testRoot "test_board_sensors.c"
# 定义 ESP-IDF 适配器运行级 stub 测试源路径。
$adapterTestPath = Join-Path $testRoot "test_board_sensors_esp_idf.c"
# 用 ESP_PLATFORM 和最小 stub 严格检查实际 ESP-IDF 适配源；不伪装成真 IDF 链接。
& gcc -std=c11 -Wall -Wextra -Werror -Wmissing-prototypes -DESP_PLATFORM "-I$includePath" "-I$stubInclude" -fsyntax-only $idfSourcePath
# 适配器语法失败时立即停止。
if ($LASTEXITCODE -ne 0) {
    # 抛出带退出码的异常。
    throw "board sensors ESP-IDF adapter syntax check failed with exit code $LASTEXITCODE"
}
# 使用严格告警编译生产源和测试；任何缺失原型或隐式转换都视为失败。
& gcc -std=c11 -Wall -Wextra -Werror -Wmissing-prototypes "-I$includePath" $sourcePath $testPath -lm -o $binaryPath
# 编译失败时给出稳定错误。
if ($LASTEXITCODE -ne 0) {
    # 抛出带退出码的异常。
    throw "board sensors host compile failed with exit code $LASTEXITCODE"
}
# 运行刚生成的测试二进制。
& $binaryPath
# 断言失败时终止脚本。
if ($LASTEXITCODE -ne 0) {
    # 抛出带退出码的异常。
    throw "board sensors host tests failed with exit code $LASTEXITCODE"
}
# 使用 ESP_PLATFORM 和本地 stub 编译适配器实现及动态地址运行测试。
& gcc -std=c11 -Wall -Wextra -Werror -Wmissing-prototypes -DESP_PLATFORM "-I$includePath" "-I$stubInclude" $idfSourcePath $adapterTestPath -o $adapterBinaryPath
# 适配器运行测试编译失败时立即停止。
if ($LASTEXITCODE -ne 0) {
    # 抛出带退出码的异常。
    throw "board sensors ESP-IDF adapter host compile failed with exit code $LASTEXITCODE"
}
# 运行适配器动态地址测试二进制。
& $adapterBinaryPath
# 适配器地址路由断言失败时终止脚本。
if ($LASTEXITCODE -ne 0) {
    # 抛出带退出码的异常。
    throw "board sensors ESP-IDF adapter host tests failed with exit code $LASTEXITCODE"
}
