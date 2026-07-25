# 开启严格模式，拼写错误或未定义变量立即终止。
Set-StrictMode -Version Latest
# 把 PowerShell 非终止错误统一提升为终止错误。
$ErrorActionPreference = 'Stop'

# testDir 是当前主机测试目录绝对路径。
$testDir = Split-Path -Parent $MyInvocation.MyCommand.Path
# repoRoot 从 esp32/host_tests/ble_service 向上三级定位仓库根。
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $testDir '..\..\..')).Path
# componentDir 指向设备 BLE 服务生产组件。
$componentDir = Join-Path $repoRoot 'esp32\firmware\components\ble_service'
# protocolDir 指向 C/C# 共用的逻辑帧与分片参考实现。
$protocolDir = Join-Path $repoRoot 'shared\protocol'
# buildDir 使用项目本地临时目录，避免污染系统 TEMP 或 C 盘缓存。
$buildDir = Join-Path $repoRoot '.codex-local\tmp\ble_service_host_tests'
# binaryPath 是本轮严格编译产生的测试可执行文件。
$binaryPath = Join-Path $buildDir 'ble_service_tests.exe'

# 构建目录不存在时创建。
if (-not (Test-Path -LiteralPath $buildDir)) {
    # 创建项目本地目录并抑制目录对象输出。
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

# 查找 MinGW GCC，用于纯 C11 主机测试。
$gccCommand = Get-Command gcc -ErrorAction SilentlyContinue
# 缺少编译器时禁止运行可能残留的旧二进制。
if ($null -eq $gccCommand) {
    # 给出明确恢复方式。
    throw '未找到 gcc。请安装 MinGW-w64，或从 ESP-IDF 命令行运行本脚本。'
}

# sources 包含共享协议、BLE 纯 C 核心和主机测试入口。
$sources = @(
    # 共享逻辑帧、CRC 和分片实现。
    (Join-Path $protocolDir 'imu_ble_protocol.c'),
    # 设备端幂等控制、LiveState 和重组业务核心。
    (Join-Path $componentDir 'src\ble_service_core.c'),
    # 正式 Manifest TLV、SHA 解码和类别表 CRC32 纯 C 构建器。
    (Join-Path $componentDir 'src\ble_service_manifest.c'),
    # 忘记电脑纯 C 编排器；主机测试替换 NimBLE 断连与 store 回调。
    (Join-Path $componentDir 'src\ble_service_bond_manager.c'),
    # 主机测试入口。
    (Join-Path $testDir 'test_ble_service.c')
)

# 使用 C11、全部常用警告、警告即错误和 GCC 静态分析器构建。
& $gccCommand.Source `
    '-std=c11' `
    '-Wall' `
    '-Wextra' `
    '-Wpedantic' `
    '-Werror' `
    '-fanalyzer' `
    '-O1' `
    "-I$(Join-Path $componentDir 'include')" `
    "-I$(Join-Path $componentDir 'src')" `
    "-I$protocolDir" `
    $sources `
    '-o' `
    $binaryPath

# 编译失败时透传 GCC 退出码，禁止运行旧文件。
if ($LASTEXITCODE -ne 0) {
    # 结束脚本并返回编译错误。
    exit $LASTEXITCODE
}

# 运行刚生成的测试可执行文件。
& $binaryPath
# 任一断言失败时透传非零退出码。
if ($LASTEXITCODE -ne 0) {
    # 结束脚本并返回测试错误。
    exit $LASTEXITCODE
}

# 全部测试通过。
exit 0
