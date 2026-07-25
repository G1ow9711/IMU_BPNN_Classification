# 遇到未处理错误立即停止，避免运行旧二进制。
$ErrorActionPreference = "Stop"
# 解析脚本目录，保证任意工作目录调用均稳定。
$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
# 解析仓库根目录。
$repositoryRoot = Resolve-Path (Join-Path $scriptDirectory "..\..\..")
# 使用项目本地临时目录。
$temporaryDirectory = Join-Path $repositoryRoot ".codex-local\tmp\workout_engine"
# 创建临时目录。
New-Item -ItemType Directory -Force -Path $temporaryDirectory | Out-Null
# 获取 GCC 路径。
$compiler = (Get-Command gcc -ErrorAction Stop).Source
# 定义输出程序。
$outputExecutable = Join-Path $temporaryDirectory "workout_engine_tests.exe"
# 定义三个生产源文件和测试源文件。
$engineSource = Join-Path $repositoryRoot "esp32\firmware\components\workout_engine\workout_engine.c"
$phaseSource = Join-Path $repositoryRoot "esp32\firmware\components\motion_phase\motion_phase.c"
$fitnessSource = Join-Path $repositoryRoot "esp32\firmware\components\fitness_core\src\fitness_core.c"
$testSource = Join-Path $scriptDirectory "test_workout_engine.c"
# 定义三个公共头目录。
$engineInclude = Join-Path $repositoryRoot "esp32\firmware\components\workout_engine\include"
$phaseInclude = Join-Path $repositoryRoot "esp32\firmware\components\motion_phase\include"
$fitnessInclude = Join-Path $repositoryRoot "esp32\firmware\components\fitness_core\include"
# 定义自动生成模型公共头目录，供 workout_engine 私有复用 BpBoutAccumulator。
$modelInclude = Join-Path $repositoryRoot "esp32\include"
# 严格编译 C11 并链接数学库。
& $compiler -std=c11 -Wall -Wextra -Wpedantic -Werror -Wmissing-prototypes `
    -I $engineInclude -I $phaseInclude -I $fitnessInclude -I $modelInclude `
    $engineSource $phaseSource $fitnessSource $testSource -lm -o $outputExecutable
# 编译失败原样退出。
if ($LASTEXITCODE -ne 0) {
    # 返回 GCC 错误码。
    exit $LASTEXITCODE
}
# 运行测试程序。
& $outputExecutable
# 测试失败原样退出。
if ($LASTEXITCODE -ne 0) {
    # 返回测试错误码。
    exit $LASTEXITCODE
}
