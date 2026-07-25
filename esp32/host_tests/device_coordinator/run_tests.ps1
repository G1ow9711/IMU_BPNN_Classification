# 遇到未处理错误立即停止，避免运行旧二进制。
$ErrorActionPreference = "Stop"
# 解析脚本目录，保证从任意工作目录调用都稳定。
$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
# 解析仓库根目录。
$repositoryRoot = Resolve-Path (Join-Path $scriptDirectory "..\..\..")
# 使用项目本地临时目录，不写入系统 TEMP。
$temporaryDirectory = Join-Path $repositoryRoot ".codex-local\tmp\device_coordinator"
# 创建项目本地临时目录。
New-Item -ItemType Directory -Force -Path $temporaryDirectory | Out-Null
# 获取 GCC 路径，未安装时抛出明确错误。
$compiler = (Get-Command gcc -ErrorAction Stop).Source
# 定义输出主机测试程序。
$outputExecutable = Join-Path $temporaryDirectory "device_coordinator_tests.exe"
# 定义协调器生产源文件。
$coordinatorSource = Join-Path $repositoryRoot "esp32\firmware\components\device_coordinator\device_coordinator.c"
# 定义设备配置生产源文件，协调器目标与体重 API 复用同一范围合同。
$deviceConfigSource = Join-Path $repositoryRoot "esp32\firmware\components\device_config\device_config.c"
# 定义训练引擎源文件。
$workoutSource = Join-Path $repositoryRoot "esp32\firmware\components\workout_engine\workout_engine.c"
# 定义动作相位源文件。
$phaseSource = Join-Path $repositoryRoot "esp32\firmware\components\motion_phase\motion_phase.c"
# 定义计数、热量和训练事件源文件；真表没有振动马达。
$fitnessSource = Join-Path $repositoryRoot "esp32\firmware\components\fitness_core\src\fitness_core.c"
# 定义 UI 纯状态机源文件。
$uiSource = Join-Path $repositoryRoot "esp32\firmware\components\ui\ui_state_machine.c"
# 定义电源纯状态机源文件。
$powerSource = Join-Path $repositoryRoot "esp32\firmware\components\power_manager\power_manager.c"
# 定义会话双槽编码与幂等更新源文件。
$sessionSource = Join-Path $repositoryRoot "esp32\firmware\components\session_store\src\session_store.c"
# 定义会话内存后端源文件。
$memoryBackendSource = Join-Path $repositoryRoot "esp32\firmware\components\session_store\src\session_memory_backend.c"
# 定义主机测试源文件。
$testSource = Join-Path $scriptDirectory "test_device_coordinator.c"
# 定义协调器头文件目录。
$coordinatorInclude = Join-Path $repositoryRoot "esp32\firmware\components\device_coordinator\include"
# 定义设备配置头文件目录。
$deviceConfigInclude = Join-Path $repositoryRoot "esp32\firmware\components\device_config\include"
# 定义训练引擎头文件目录。
$workoutInclude = Join-Path $repositoryRoot "esp32\firmware\components\workout_engine\include"
# 定义动作相位头文件目录。
$phaseInclude = Join-Path $repositoryRoot "esp32\firmware\components\motion_phase\include"
# 定义计数领域头文件目录。
$fitnessInclude = Join-Path $repositoryRoot "esp32\firmware\components\fitness_core\include"
# 定义 UI 头文件目录。
$uiInclude = Join-Path $repositoryRoot "esp32\firmware\components\ui\include"
# 定义电源头文件目录。
$powerInclude = Join-Path $repositoryRoot "esp32\firmware\components\power_manager\include"
# 定义会话存储头文件目录。
$sessionInclude = Join-Path $repositoryRoot "esp32\firmware\components\session_store\include"
# 定义 BLE 核心合同头文件目录。
$bleInclude = Join-Path $repositoryRoot "esp32\firmware\components\ble_service\include"
# 定义共享 BLE 帧协议头文件目录。
$protocolInclude = Join-Path $repositoryRoot "shared\protocol"
# 定义自动生成模型公共头目录，供 workout_engine 私有复用 BpBoutAccumulator。
$modelInclude = Join-Path $repositoryRoot "esp32\include"
# 以最严格 C11 警告编译全部生产链和测试。
& $compiler -std=c11 -Wall -Wextra -Wpedantic -Werror -Wmissing-prototypes `
    -I $coordinatorInclude -I $deviceConfigInclude -I $workoutInclude -I $phaseInclude -I $fitnessInclude `
    -I $uiInclude -I $powerInclude -I $sessionInclude -I $bleInclude -I $protocolInclude -I $modelInclude `
    $coordinatorSource $deviceConfigSource $workoutSource $phaseSource $fitnessSource $uiSource $powerSource `
    $sessionSource $memoryBackendSource $testSource -lm -o $outputExecutable
# 编译失败时原样返回 GCC 错误码。
if ($LASTEXITCODE -ne 0) {
    # 终止脚本。
    exit $LASTEXITCODE
}
# 运行新编译的主机测试。
& $outputExecutable
# 测试失败时原样返回。
if ($LASTEXITCODE -ne 0) {
    # 终止脚本。
    exit $LASTEXITCODE
}
