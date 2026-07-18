# 失败即终止，避免编译错误后继续运行旧可执行文件。
$ErrorActionPreference = "Stop"
# 从脚本所在目录解析独立工作树根，避免依赖调用者当前目录。
$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
# 从 host_tests/power_ui 向上两级得到 esp32 目录。
$esp32Root = Resolve-Path (Join-Path $testRoot "..\..")
# 从 esp32/host_tests/power_ui 向上三级得到仓库根目录。
$projectRoot = Resolve-Path (Join-Path $testRoot "..\..\..")
# 在被 .gitignore 忽略的项目本地临时目录保存主机测试产物。
$buildRoot = Join-Path $projectRoot ".codex-local\tmp\power_ui_host_tests"
# 目录不存在时创建，Force 允许重复运行。
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
# 生成测试可执行文件绝对路径。
$binaryPath = Join-Path $buildRoot "power_ui_tests.exe"
# 定义三个生产组件头文件目录，保证测试不复制合同。
$boardInclude = Join-Path $esp32Root "firmware\components\board_adapter\include"
# 定义 UI 组件头文件目录。
$uiInclude = Join-Path $esp32Root "firmware\components\ui\include"
# 定义电源组件头文件目录。
$powerInclude = Join-Path $esp32Root "firmware\components\power_manager\include"
# 定义板级生产源文件路径。
$boardSource = Join-Path $esp32Root "firmware\components\board_adapter\board_adapter.c"
# 定义 UI 生产源文件路径。
$uiSource = Join-Path $esp32Root "firmware\components\ui\ui_state_machine.c"
# 定义电源生产源文件路径。
$powerSource = Join-Path $esp32Root "firmware\components\power_manager\power_manager.c"
# 定义主机测试源文件路径。
$testSource = Join-Path $testRoot "test_power_ui.c"
# 使用 C11、全部常见警告和 Werror 编译，确保纯 C 合同没有隐式转换或未使用代码。
& gcc -std=c11 -Wall -Wextra -Werror "-I$boardInclude" "-I$uiInclude" "-I$powerInclude" $boardSource $uiSource $powerSource $testSource -o $binaryPath
# gcc 非零退出时抛出明确错误，避免运行旧二进制。
if ($LASTEXITCODE -ne 0) {
    # 抛出编译失败异常。
    throw "power_ui host test compile failed with exit code $LASTEXITCODE"
}
# 运行刚编译的测试可执行文件。
& $binaryPath
# 测试非零退出时抛出异常，供 CI 或 Codex 明确判定失败。
if ($LASTEXITCODE -ne 0) {
    # 抛出测试失败异常。
    throw "power_ui host tests failed with exit code $LASTEXITCODE"
}
