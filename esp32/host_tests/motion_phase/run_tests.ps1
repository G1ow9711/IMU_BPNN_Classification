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
# 解析工程族根目录，真数据位于工作树同级的“决赛”目录。
$projectFamilyRoot = Resolve-Path (Join-Path $repositoryRoot "..\..")
# 定义开合跳三份人工核数真数据目录。
$jumpingJackDataRoot = Join-Path $projectFamilyRoot "决赛\算法仿真代码\MATLAB\实测数据集\A类活动"
# 按 scy1、scy2、scy3 固定顺序解析绝对路径，分别对应视觉数 16、15、16。
$jumpingJackDataPaths = @(
    (Resolve-Path (Join-Path $jumpingJackDataRoot "jumping_jack_scy1_20.txt")).Path,
    (Resolve-Path (Join-Path $jumpingJackDataRoot "jumping_jack_scy2_20.txt")).Path,
    (Resolve-Path (Join-Path $jumpingJackDataRoot "jumping_jack_scy3_20.txt")).Path
)
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
# 运行刚生成的测试二进制，并把三份真数据作为只读输入传入 C 验证器。
& $outputExecutable $jumpingJackDataPaths[0] $jumpingJackDataPaths[1] $jumpingJackDataPaths[2]
# 测试非零退出时原样返回。
if ($LASTEXITCODE -ne 0) {
    # 把测试错误码返回调用方。
    exit $LASTEXITCODE
}
