# 开启严格模式，使拼写错误或未定义变量立即终止测试。
Set-StrictMode -Version Latest
# 任一外部命令或 PowerShell 错误均由脚本显式转为失败退出码。
$ErrorActionPreference = 'Stop'

# testDir 是本脚本所在目录，避免调用者当前目录影响相对路径。
$testDir = Split-Path -Parent $MyInvocation.MyCommand.Path
# repoRoot 从 host_tests/fitness_core 向上三级定位到仓库根目录。
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $testDir '..\..\..')).Path
# componentDir 指向 ESP-IDF 与主机测试共用的 fitness_core 实现。
$componentDir = Join-Path $repoRoot 'esp32\firmware\components\fitness_core'
# buildDir 使用项目本地临时目录，避免污染用户 TEMP 或 C 盘缓存。
$buildDir = Join-Path $repoRoot '.codex-local\tmp\fitness_core_host_tests'
# binaryPath 是本轮主机测试可执行文件的完整路径。
$binaryPath = Join-Path $buildDir 'fitness_core_tests.exe'

# 构建目录不存在时在项目本地创建；已有目录直接复用。
if (-not (Test-Path -LiteralPath $buildDir)) {
    # 创建目录并抑制无用对象输出。
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

# gccCommand 查找 MinGW GCC；缺失时给出明确工具要求。
$gccCommand = Get-Command gcc -ErrorAction SilentlyContinue
# 当前主机没有 GCC 时不能执行 C 测试。
if ($null -eq $gccCommand) {
    # 抛出带恢复方式的错误，不伪造测试通过。
    throw '未找到 gcc。请安装 MinGW-w64，或从 ESP-IDF 命令行运行本脚本。'
}

# sourcePath 是被测纯 C 实现。
$sourcePath = Join-Path $componentDir 'src\fitness_core.c'
# includePath 是公共 API 头文件目录。
$includePath = Join-Path $componentDir 'include'
# testSourcePath 是主机断言测试源码。
$testSourcePath = Join-Path $testDir 'test_fitness_core.c'

# 用 C11、最高常用警告和警告即错误编译，提前发现移植风险。
& $gccCommand.Source `
    '-std=c11' `
    '-Wall' `
    '-Wextra' `
    '-Wpedantic' `
    '-Werror' `
    '-O2' `
    "-I$includePath" `
    $sourcePath `
    $testSourcePath `
    '-lm' `
    '-o' `
    $binaryPath

# GCC 非零退出时停止，不运行旧二进制。
if ($LASTEXITCODE -ne 0) {
    # 使用 GCC 原退出码说明编译失败。
    exit $LASTEXITCODE
}

# 执行刚编译的测试程序，所有 assert 必须通过。
& $binaryPath
# 测试程序非零退出时透传失败码给 CI。
if ($LASTEXITCODE -ne 0) {
    # 退出码来自断言失败或运行时异常。
    exit $LASTEXITCODE
}

# 显式返回成功，便于调用方组合其它测试。
exit 0
