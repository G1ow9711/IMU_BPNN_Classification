# 遇到编译或断言错误立即停止，禁止继续运行旧测试程序。
$ErrorActionPreference = "Stop"
# 获取当前测试目录，允许从仓库任意路径调用本脚本。
$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
# 解析仓库根目录，用于定位生产源码和项目本地临时目录。
$repositoryRoot = Resolve-Path (Join-Path $scriptDirectory "..\..\..")
# 把 GCC 临时文件固定到项目 G 盘目录，避免写入系统 TEMP。
$temporaryDirectory = Join-Path $repositoryRoot ".codex-local\tmp\device_config"
# 创建可重复使用的本地临时目录。
New-Item -ItemType Directory -Force -Path $temporaryDirectory | Out-Null
# 同时设置 TEMP 和 TMP，覆盖 GCC 的两种环境变量读取路径。
$env:TEMP = $temporaryDirectory
# TMP 与 TEMP 使用同一项目本地目录。
$env:TMP = $temporaryDirectory
# 获取 GCC 绝对路径；未安装时由 PowerShell 报出明确错误。
$compiler = (Get-Command gcc -ErrorAction Stop).Source
# 指定每次重新生成的测试可执行文件。
$outputExecutable = Join-Path $temporaryDirectory "device_config_tests.exe"
# 定位待实现的纯 C 设备配置源码。
$productionSource = Join-Path $repositoryRoot "esp32\firmware\components\device_config\device_config.c"
# 定位主机边界测试源码。
$testSource = Join-Path $scriptDirectory "test_device_config.c"
# 定位公开头文件目录。
$includeDirectory = Join-Path $repositoryRoot "esp32\firmware\components\device_config\include"
# 使用严格 C11 和全部常用告警编译，任何告警均视为交付失败。
& $compiler -std=c11 -Wall -Wextra -Wpedantic -Werror -Wmissing-prototypes `
    -I $includeDirectory $productionSource $testSource -o $outputExecutable
# 编译失败时透传 GCC 退出码，不运行旧文件。
if ($LASTEXITCODE -ne 0) {
    # 返回实际编译错误码。
    exit $LASTEXITCODE
}
# 运行刚编译的测试程序。
& $outputExecutable
# 透传断言失败退出码。
exit $LASTEXITCODE
