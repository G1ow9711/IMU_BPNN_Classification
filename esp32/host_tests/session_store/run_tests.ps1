# 开启严格模式，使未定义变量立即失败。
Set-StrictMode -Version Latest
# PowerShell 错误统一转为终止错误。
$ErrorActionPreference = 'Stop'

# testDir 是当前脚本目录。
$testDir = Split-Path -Parent $MyInvocation.MyCommand.Path
# repoRoot 从 esp32/host_tests/session_store 向上三级定位仓库根。
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $testDir '..\..\..')).Path
# componentDir 指向共用纯 C 组件。
$componentDir = Join-Path $repoRoot 'esp32\firmware\components\session_store'
# buildDir 使用项目本地临时目录。
$buildDir = Join-Path $repoRoot '.codex-local\tmp\session_store_host_tests'
# binaryPath 是严格测试可执行文件。
$binaryPath = Join-Path $buildDir 'session_store_tests.exe'

# 构建目录不存在时创建。
if (-not (Test-Path -LiteralPath $buildDir)) {
    # 创建项目本地目录并抑制对象输出。
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

# 查找 MinGW GCC。
$gccCommand = Get-Command gcc -ErrorAction SilentlyContinue
# 缺少编译器时给出明确错误。
if ($null -eq $gccCommand) {
    # 不运行旧二进制。
    throw '未找到 gcc。请安装 MinGW-w64，或从 ESP-IDF 命令行运行本脚本。'
}

# includePath 是公共头目录。
$includePath = Join-Path $componentDir 'include'
# sources 包含三个生产源和一个测试源。
$sources = @(
    # 双槽快照和索引实现。
    (Join-Path $componentDir 'src\session_store.c'),
    # 故障注入内存后端。
    (Join-Path $componentDir 'src\session_memory_backend.c'),
    # 原始 IMU 块实现。
    (Join-Path $componentDir 'src\session_raw_log.c'),
    # 主机测试入口。
    (Join-Path $testDir 'test_session_store.c')
)

# 使用 C11、全部常用警告、警告即错误和静态分析器编译。
& $gccCommand.Source `
    '-std=c11' `
    '-Wall' `
    '-Wextra' `
    '-Wpedantic' `
    '-Werror' `
    '-fanalyzer' `
    '-O1' `
    "-I$includePath" `
    $sources `
    '-o' `
    $binaryPath

# 编译失败时透传 GCC 退出码。
if ($LASTEXITCODE -ne 0) {
    # 终止，禁止运行旧文件。
    exit $LASTEXITCODE
}

# 运行刚编译的测试。
& $binaryPath
# 断言失败或运行异常时透传退出码。
if ($LASTEXITCODE -ne 0) {
    # 返回测试错误。
    exit $LASTEXITCODE
}

# 全部成功。
exit 0
