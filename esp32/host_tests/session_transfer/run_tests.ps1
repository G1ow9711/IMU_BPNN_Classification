# 开启严格模式，未定义变量立即终止测试。
Set-StrictMode -Version Latest
# 把非终止 PowerShell 错误提升为终止错误。
$ErrorActionPreference = 'Stop'

# testDir 指向本主机测试目录。
$testDir = Split-Path -Parent $MyInvocation.MyCommand.Path
# repoRoot 从 esp32/host_tests/session_transfer 向上三级定位仓库根。
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $testDir '..\..\..')).Path
# transferDir 指向待测纯 C 会话同步组件。
$transferDir = Join-Path $repoRoot 'esp32\firmware\components\session_transfer'
# storeDir 指向真实会话仓储实现。
$storeDir = Join-Path $repoRoot 'esp32\firmware\components\session_store'
# bleInclude 只提供稳定回调返回码类型，不链接 NimBLE。
$bleInclude = Join-Path $repoRoot 'esp32\firmware\components\ble_service\include'
# protocolInclude 提供 ble_service_core.h 间接引用的共享协议类型。
$protocolInclude = Join-Path $repoRoot 'shared\protocol'
# buildDir 使用项目本地临时目录，避免系统 TEMP 和 C 盘缓存。
$buildDir = Join-Path $repoRoot '.codex-local\tmp\session_transfer_host_tests'
# binaryPath 保存本轮新编译可执行文件。
$binaryPath = Join-Path $buildDir 'session_transfer_tests.exe'

# 首次运行时创建本地构建目录。
if (-not (Test-Path -LiteralPath $buildDir)) {
    # 创建目录并抑制对象输出。
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

# 查找 MinGW GCC。
$gccCommand = Get-Command gcc -ErrorAction SilentlyContinue
# 没有编译器时禁止运行旧测试文件。
if ($null -eq $gccCommand) {
    # 给出明确依赖错误。
    throw '未找到 gcc。请从项目 ESP-IDF/MinGW 环境运行。'
}

# sources 只包含纯 C 仓储、同步组件和测试入口。
$sources = @(
    # 真实双槽、环形索引和内存后端实现。
    (Join-Path $storeDir 'src\session_store.c'),
    # 主机内存后端提供双槽随机读写函数表。
    (Join-Path $storeDir 'src\session_memory_backend.c'),
    # 真实会话同步编解码与回调适配。
    (Join-Path $transferDir 'session_transfer.c'),
    # 主机断言入口。
    (Join-Path $testDir 'test_session_transfer.c')
)

# 严格 C11 编译并启用 GCC 静态分析器。
& $gccCommand.Source `
    '-std=c11' `
    '-Wall' `
    '-Wextra' `
    '-Wpedantic' `
    '-Werror' `
    '-fanalyzer' `
    '-O1' `
    "-I$(Join-Path $transferDir 'include')" `
    "-I$(Join-Path $storeDir 'include')" `
    "-I$bleInclude" `
    "-I$protocolInclude" `
    $sources `
    '-o' `
    $binaryPath

# 编译失败时透传退出码，禁止运行旧二进制。
if ($LASTEXITCODE -ne 0) {
    # 返回编译错误。
    exit $LASTEXITCODE
}

# 运行刚生成测试程序。
& $binaryPath
# 任一断言失败时透传退出码。
if ($LASTEXITCODE -ne 0) {
    # 返回测试错误。
    exit $LASTEXITCODE
}

# 全部断言通过。
exit 0
