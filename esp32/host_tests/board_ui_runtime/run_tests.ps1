# 任何命令失败立即停止，避免执行旧二进制。
$ErrorActionPreference = "Stop"
# 解析当前测试目录。
$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
# 从 host_tests/board_ui_runtime 向上两级得到 esp32 根目录。
$esp32Root = Resolve-Path (Join-Path $testRoot "..\..")
# 从 esp32 根向上一级得到项目根目录。
$projectRoot = Resolve-Path (Join-Path $esp32Root "..")
# 使用项目本地忽略目录保存测试产物。
$buildRoot = Join-Path $projectRoot ".codex-local\tmp\board_ui_runtime_tests"
# 创建或复用测试目录。
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
# 设置测试可执行文件路径。
$binaryPath = Join-Path $buildRoot "board_ui_runtime_tests.exe"
# 定义板级适配头目录。
$boardInclude = Join-Path $esp32Root "firmware\components\board_adapter\include"
# 定义板级运行时公开头目录。
$runtimeInclude = Join-Path $esp32Root "firmware\components\board_runtime\include"
# 定义板级运行时内部头目录。
$runtimePrivate = Join-Path $esp32Root "firmware\components\board_runtime"
# 定义 UI 公开头目录。
$uiInclude = Join-Path $esp32Root "firmware\components\ui\include"
# 定义生产源文件集合；主机只链接 Mock 后端，不链接 LVGL。
$sources = @(
    (Join-Path $esp32Root "firmware\components\board_adapter\board_adapter.c"),
    (Join-Path $esp32Root "firmware\components\board_runtime\board_runtime.c"),
    (Join-Path $esp32Root "firmware\components\board_runtime\board_runtime_mock.c"),
    (Join-Path $esp32Root "firmware\components\ui\ui_state_machine.c"),
    (Join-Path $esp32Root "firmware\components\ui\ui_presenter.c"),
    # 配对码原子邮箱负责 NimBLE 任务到应用任务的无阻塞事件交接。
    (Join-Path $esp32Root "firmware\components\ui\ui_app_pairing.c"),
    (Join-Path $testRoot "test_board_ui_runtime.c")
)
# 使用 C11 和 Werror 编译生产合同及测试。
& gcc -std=c11 -Wall -Wextra -Werror "-I$boardInclude" "-I$runtimeInclude" "-I$runtimePrivate" "-I$uiInclude" $sources -o $binaryPath
# 编译失败时抛出明确错误。
if ($LASTEXITCODE -ne 0) {
    throw "board/ui runtime host compile failed with exit code $LASTEXITCODE"
}
# 定义真实 BSP API 编译桩目录。
$stubInclude = Join-Path $testRoot "stubs"
# 对真实后端执行仅语法编译，校验 Waveshare/ESP-IDF API 名称和字段边界。
& gcc -std=c11 -Wall -Wextra -Werror -fsyntax-only "-I$stubInclude" "-I$boardInclude" "-I$runtimeInclude" "-I$runtimePrivate" (Join-Path $esp32Root "firmware\components\board_runtime\board_runtime_esp.c")
# 真实后端边界不通过时抛出错误；该测试不代表硬件功能实测。
if ($LASTEXITCODE -ne 0) {
    throw "board runtime real BSP boundary compile failed with exit code $LASTEXITCODE"
}
# 使用厂家仓库内 LVGL 9 头文件对真实 renderer 执行语法编译。
$vendorLvgl = Resolve-Path (Join-Path $projectRoot ".codex-local\cache\vendor\ESP32-S3-Touch-AMOLED-2.06\examples\arduino\libraries\lvgl")
# 校验所有页面、动画和触摸事件 API 属于 LVGL 9。
& gcc -std=c11 -Wall -Wextra -Werror -fsyntax-only "-I$uiInclude" "-I$vendorLvgl" "-I$(Join-Path $vendorLvgl 'src')" (Join-Path $esp32Root "firmware\components\ui\ui_lvgl_renderer.c")
# LVGL API 边界不通过时阻止交付。
if ($LASTEXITCODE -ne 0) {
    throw "ui LVGL 9 boundary compile failed with exit code $LASTEXITCODE"
}
# 运行刚生成的二进制。
& $binaryPath
# 用例失败时阻止交付。
if ($LASTEXITCODE -ne 0) {
    throw "board/ui runtime host tests failed with exit code $LASTEXITCODE"
}
