# 遇到任何未处理错误立即停止，避免编译失败后继续运行旧可执行文件。
$ErrorActionPreference = "Stop"
# 解析当前脚本目录，保证从任意工作目录启动时路径稳定。
$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
# 解析仓库根目录：motion_phase -> host_tests -> esp32 -> 仓库根。
$repositoryRoot = Resolve-Path (Join-Path $scriptDirectory "..\..\..")
# 使用项目本地临时目录保存可执行文件，禁止写系统 TEMP。
$temporaryDirectory = Join-Path $repositoryRoot ".codex-local\tmp\motion_phase"
# 创建临时目录；已存在时不报错。
New-Item -ItemType Directory -Force -Path $temporaryDirectory | Out-Null
# 定位 MinGW/GCC；缺少编译器时明确失败。
$compiler = (Get-Command gcc -ErrorAction Stop).Source
# 定义测试可执行文件路径。
$outputExecutable = Join-Path $temporaryDirectory "motion_phase_tests.exe"
# 定义被测相位源文件。
$phaseSource = Join-Path $repositoryRoot "esp32\firmware\components\motion_phase\motion_phase.c"
# 定义下游计数状态机源文件，验证真实相位合同。
$fitnessSource = Join-Path $repositoryRoot "esp32\firmware\components\fitness_core\src\fitness_core.c"
# 定义测试源文件。
$testSource = Join-Path $scriptDirectory "test_motion_phase.c"
# 优先读取维护者显式配置的外部真板数据目录；公开仓库不携带原始受试者数据。
$jumpingJackDataRoot = $env:IMU_MOTION_PHASE_DATA_ROOT
# 未配置环境变量时兼容原开发机的历史目录，避免破坏维护者现有工作流。
if ([string]::IsNullOrWhiteSpace($jumpingJackDataRoot)) {
    # 用路径归一化计算工程族根目录，不要求目标仓库具有固定盘符或目录名。
    $projectFamilyRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot "..\.."))
    # 拼出历史开合跳人工核数数据目录；目录不存在时只跳过可选外部回放。
    $jumpingJackDataRoot = Join-Path $projectFamilyRoot "决赛\算法仿真代码\MATLAB\实测数据集\A类活动"
}
# 按 scy1、scy2、scy3 固定顺序声明候选文件，人工视觉核数分别为 16、15、16。
$jumpingJackDataCandidates = @(
    (Join-Path $jumpingJackDataRoot "jumping_jack_scy1_20.txt"),
    (Join-Path $jumpingJackDataRoot "jumping_jack_scy2_20.txt"),
    (Join-Path $jumpingJackDataRoot "jumping_jack_scy3_20.txt")
)
# 统计真实存在的候选文件；必须三份齐全才允许执行外部回放，防止只验证部分受试者。
$availableExternalDataCount = @(
    $jumpingJackDataCandidates | Where-Object {
        # 只接受普通文件，拒绝同名目录。
        Test-Path -LiteralPath $_ -PathType Leaf
    }
).Count
# 三份候选文件全部存在时才启用维护者外部数据门。
$hasCompleteExternalDataset = ($availableExternalDataCount -eq $jumpingJackDataCandidates.Count)
# 定义相位公共头目录。
$phaseInclude = Join-Path $repositoryRoot "esp32\firmware\components\motion_phase\include"
# 定义计数公共头目录。
$fitnessInclude = Join-Path $repositoryRoot "esp32\firmware\components\fitness_core\include"
# 严格编译 C11；所有警告都视为失败，并链接数学库。
& $compiler -std=c11 -Wall -Wextra -Wpedantic -Werror -Wmissing-prototypes `
    -I $phaseInclude -I $fitnessInclude `
    $phaseSource $fitnessSource $testSource -lm -o $outputExecutable
# 编译器非零退出时终止脚本。
if ($LASTEXITCODE -ne 0) {
    # 把 GCC 错误码返回调用方。
    exit $LASTEXITCODE
}
# 根据外部数据是否齐全选择公开合成门或维护者追加真板门。
if ($hasCompleteExternalDataset) {
    # 把三份候选路径解析为绝对路径，避免 C 测试受调用目录影响。
    $jumpingJackDataPaths = @(
        (Resolve-Path -LiteralPath $jumpingJackDataCandidates[0]).Path,
        (Resolve-Path -LiteralPath $jumpingJackDataCandidates[1]).Path,
        (Resolve-Path -LiteralPath $jumpingJackDataCandidates[2]).Path
    )
    # 运行全部合成测试，并追加三份人工核数真板数据回放。
    & $outputExecutable $jumpingJackDataPaths[0] $jumpingJackDataPaths[1] $jumpingJackDataPaths[2]
}
else {
    # 公开克隆只运行仓库内自包含测试；缺少私有数据不伪装成算法失败。
    & $outputExecutable
    # 输出机器可检索标记，明确跳过项及配置外部数据的入口。
    Write-Host "MOTION_PHASE_EXTERNAL_REPLAY_SKIPPED reason=dataset_not_available env=IMU_MOTION_PHASE_DATA_ROOT"
}
# 测试非零退出时原样返回。
if ($LASTEXITCODE -ne 0) {
    # 把测试错误码返回调用方。
    exit $LASTEXITCODE
}
