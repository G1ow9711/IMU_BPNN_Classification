# 任何命令失败立即终止，避免继续运行旧测试二进制。
$ErrorActionPreference = "Stop"
# 解析当前测试目录，避免依赖调用者工作目录。
$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
# 从 esp32/host_tests/imu_pipeline 向上两级得到 esp32 目录。
$esp32Root = Resolve-Path (Join-Path $testRoot "..\..")
# 从测试目录向上三级得到仓库根目录。
$projectRoot = Resolve-Path (Join-Path $testRoot "..\..\..")
# 把运行产物放进项目本地忽略目录，不污染 Git 工作树。
$buildRoot = Join-Path $projectRoot ".codex-local\tmp\imu_pipeline_host_tests"
# 重复执行时复用目录。
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
# 生成主机测试二进制路径。
$binaryPath = Join-Path $buildRoot "imu_pipeline_tests.exe"
# 定义组件公开头路径。
$pipelineInclude = Join-Path $esp32Root "firmware\components\imu_pipeline\include"
# 定义自动生成双 M0 头路径，仅用于适配器语法检查。
$modelInclude = Join-Path $esp32Root "include"
# 定义纯 C 生产源。
$pipelineSource = Join-Path $esp32Root "firmware\components\imu_pipeline\imu_pipeline.c"
# 定义双 M0 适配器源；主机测试不链接它，避免复制大权重。
$adapterSource = Join-Path $esp32Root "firmware\components\imu_pipeline\imu_pipeline_dual_m0_adapter.c"
# 定义测试源。
$testSource = Join-Path $testRoot "test_imu_pipeline.c"
# 严格语法检查生产双 M0 边界，证明现有自动生成头可直接调用。
& gcc -std=c11 -Wall -Wextra -Werror -Wmissing-prototypes "-I$pipelineInclude" "-I$modelInclude" -fsyntax-only $adapterSource
# 适配器检查失败时终止。
if ($LASTEXITCODE -ne 0) {
    # 抛出带退出码的错误。
    throw "dual M0 adapter syntax check failed with exit code $LASTEXITCODE"
}
# 只链接纯流水线和 mock 测试，不把双 M0 权重复制进主机二进制。
& gcc -std=c11 -Wall -Wextra -Werror -Wmissing-prototypes "-I$pipelineInclude" $pipelineSource $testSource -o $binaryPath
# 编译失败时终止。
if ($LASTEXITCODE -ne 0) {
    # 抛出带退出码的错误。
    throw "IMU pipeline host compile failed with exit code $LASTEXITCODE"
}
# 运行刚生成的测试二进制。
& $binaryPath
# 任一断言失败时终止。
if ($LASTEXITCODE -ne 0) {
    # 抛出测试失败错误。
    throw "IMU pipeline host tests failed with exit code $LASTEXITCODE"
}
