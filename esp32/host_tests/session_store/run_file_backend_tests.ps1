# 遇到未处理错误立即停止。
$ErrorActionPreference = "Stop"
# 解析脚本目录。
$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
# 解析仓库根目录。
$repositoryRoot = Resolve-Path (Join-Path $scriptDirectory "..\..\..")
# 使用项目本地临时目录。
$temporaryDirectory = Join-Path $repositoryRoot ".codex-local\tmp\session_store"
# 创建临时目录。
New-Item -ItemType Directory -Force -Path $temporaryDirectory | Out-Null
# 定义测试数据文件。
$testDataPath = Join-Path $temporaryDirectory "file_backend_test.bin"
# 删除上次测试文件，确保首次打开路径覆盖新建分支。
Remove-Item -LiteralPath $testDataPath -Force -ErrorAction SilentlyContinue
# 获取 GCC。
$compiler = (Get-Command gcc -ErrorAction Stop).Source
# 定义输出程序。
$outputExecutable = Join-Path $temporaryDirectory "session_file_backend_tests.exe"
# 定义生产和测试源文件。
$storeSource = Join-Path $repositoryRoot "esp32\firmware\components\session_store\src\session_store.c"
$fileSource = Join-Path $repositoryRoot "esp32\firmware\components\session_store\src\session_file_backend.c"
$testSource = Join-Path $scriptDirectory "test_session_file_backend.c"
# 定义公共头目录。
$includeDirectory = Join-Path $repositoryRoot "esp32\firmware\components\session_store\include"
# 严格编译 C11。
& $compiler -std=c11 -Wall -Wextra -Wpedantic -Werror -Wmissing-prototypes `
    -I $includeDirectory $storeSource $fileSource $testSource -o $outputExecutable
# 编译失败原样退出。
if ($LASTEXITCODE -ne 0) {
    # 返回 GCC 错误码。
    exit $LASTEXITCODE
}
# 运行并传入项目本地测试文件路径。
& $outputExecutable $testDataPath
# 保存测试退出码。
$testExitCode = $LASTEXITCODE
# 无论测试成功与否都清理临时介质文件；可执行文件保留用于复查。
Remove-Item -LiteralPath $testDataPath -Force -ErrorAction SilentlyContinue
# 测试失败时原样退出。
if ($testExitCode -ne 0) {
    # 返回测试错误码。
    exit $testExitCode
}
