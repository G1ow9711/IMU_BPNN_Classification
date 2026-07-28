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
# 定义产品主入口；源码合同用于阻止 UI 任务栈再次迁回 PSRAM。
$mainSourcePath = Join-Path $esp32Root "firmware\main\main.c"
# 读取完整主入口文本；正则需跨行核对任务创建参数。
$mainSourceText = Get-Content -LiteralPath $mainSourcePath -Raw -Encoding UTF8
# UI 任务必须使用 FreeRTOS 默认片内栈；Flash 写入期间 PSRAM 不可访问。
if ($mainSourceText -notmatch '(?s)xTaskCreate\s*\(\s*app_ui_task\s*,') {
    throw "ui_render task must use xTaskCreate with an internal stack"
}
# 禁止把 UI 任务重新交给带 PSRAM capability 的创建入口。
if ($mainSourceText -match '(?s)xTaskCreateWithCaps\s*\(\s*app_ui_task.*?MALLOC_CAP_SPIRAM') {
    throw "ui_render task stack must not be allocated from PSRAM"
}
# UI 任务必须周期醒来更新自身心跳，禁止永久等待掩盖局部任务失活。
if ($mainSourceText -notmatch 'APP_UI_HEALTH_POLL_MS') {
    throw "ui_render task must expose a bounded health polling interval"
}
# 独立 BLE 任务必须监控 UI 与 LVGL 双心跳，保证局部故障不会永久冻结手表。
if ($mainSourceText -notmatch 'app_check_ui_health') {
    throw "firmware must supervise both UI and LVGL progress"
}
# 截取完整 UI 健康监督函数，防止仅在其它位置删除重启字符串而保留主动复位行为。
$healthFunctionMatch = [regex]::Match(
    $mainSourceText,
    '(?s)static void app_check_ui_health\(.*?\n}\r?\n\r?\n/\* BLE 任务')
# 未匹配到完整函数说明生产源码结构漂移，测试不能静默跳过安全合同。
if (-not $healthFunctionMatch.Success) {
    throw "unable to isolate app_check_ui_health for restart contract"
}
# UI 局部停滞不得升级为整机软复位，否则用户会看到屏幕重新启动且训练会话中断。
if ($healthFunctionMatch.Value -match 'esp_restart\s*\(') {
    throw "UI health supervision must not restart the whole device"
}
# 停滞恢复必须通过板级公开接口唤醒官方 taskLVGL，禁止后台任务直接修改 LVGL 对象。
if ($healthFunctionMatch.Value -notmatch 'board_runtime_wake_display_task') {
    throw "UI health supervision must wake the official taskLVGL"
}
# 定义真实板级后端；源码合同验证 taskLVGL 的产品调度和片内栈配置。
$runtimeEspSourcePath = Join-Path $esp32Root "firmware\components\board_runtime\board_runtime_esp.c"
# 读取真实后端完整源码，防止只修改主机 Mock 后让测试误通过。
$runtimeEspSourceText = Get-Content -LiteralPath $runtimeEspSourcePath -Raw -Encoding UTF8
# 产品必须使用公开可配置启动接口，不能继续依赖优先级 4、7168 字节栈的厂家默认值。
if ($runtimeEspSourceText -notmatch 'bsp_display_start_with_config\s*\(') {
    throw "real BSP must start taskLVGL with an explicit product configuration"
}
# taskLVGL 优先级 7 高于 BLE 发布任务 6，低于领域所有者 9 与 QMI 10。
if ($runtimeEspSourceText -notmatch 'BOARD_RUNTIME_LVGL_TASK_PRIORITY\s+\(7U\)') {
    throw "taskLVGL product priority must be 7"
}
# 12 KiB 片内栈覆盖 LVGL 9 动画、触摸、刷新和厂家 QSPI 调用链余量。
if ($runtimeEspSourceText -notmatch 'BOARD_RUNTIME_LVGL_TASK_STACK_BYTES\s+\(12U\s*\*\s*1024U\)') {
    throw "taskLVGL product stack must be 12 KiB"
}
# 最大休眠 100 ms 限制触摸和健康恢复的最坏唤醒延迟。
if ($runtimeEspSourceText -notmatch 'BOARD_RUNTIME_LVGL_TASK_MAX_SLEEP_MS\s+\(100U\)') {
    throw "taskLVGL maximum sleep must be 100 ms"
}
# 真实任务栈必须显式要求片内 RAM，Flash cache 关闭时仍可执行。
if ($runtimeEspSourceText -notmatch 'MALLOC_CAP_INTERNAL\s*\|\s*MALLOC_CAP_DEFAULT') {
    throw "taskLVGL stack must be allocated from internal RAM"
}
# 定义真实 LVGL renderer 源码，后续同时做 API 与生命周期合同检查。
$rendererSourcePath = Join-Path $esp32Root "firmware\components\ui\ui_lvgl_renderer.c"
# 读取 renderer 完整源码，确认心跳由 LVGL timer handler 推进。
$rendererSourceText = Get-Content -LiteralPath $rendererSourcePath -Raw -Encoding UTF8
# 只有 LVGL 定时器回调能证明官方 taskLVGL 仍在执行输入和刷新循环。
if (($rendererSourceText -notmatch 'lv_timer_create') -or
    ($rendererSourceText -notmatch 'ui_lvgl_renderer_get_heartbeat')) {
    throw "renderer must publish a taskLVGL heartbeat"
}
# 官方异步 QSPI flush 路径禁止在持锁更新中强制同步刷新。
if ($rendererSourceText -match 'lv_refr_now\s*\(') {
    throw "renderer must not call lv_refr_now under the BSP LVGL lock"
}
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
& gcc -std=c11 -Wall -Wextra -Werror -fsyntax-only "-I$stubInclude" "-I$boardInclude" "-I$runtimeInclude" "-I$runtimePrivate" $runtimeEspSourcePath
# 真实后端边界不通过时抛出错误；该测试不代表硬件功能实测。
if ($LASTEXITCODE -ne 0) {
    throw "board runtime real BSP boundary compile failed with exit code $LASTEXITCODE"
}
# 使用厂家仓库内 LVGL 9 头文件对真实 renderer 执行语法编译。
$vendorLvgl = Resolve-Path (Join-Path $projectRoot ".codex-local\cache\vendor\ESP32-S3-Touch-AMOLED-2.06\examples\arduino\libraries\lvgl")
# 校验所有页面、动画和触摸事件 API 属于 LVGL 9。
& gcc -std=c11 -Wall -Wextra -Werror -fsyntax-only "-I$uiInclude" "-I$vendorLvgl" "-I$(Join-Path $vendorLvgl 'src')" $rendererSourcePath
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
