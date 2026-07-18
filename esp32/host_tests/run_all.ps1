param(
    # StopOnFailure=true 时首个失败立即结束；默认继续执行以一次列出全部失败组件。
    [switch]$StopOnFailure
)

# 严格模式会把未定义变量和错误属性访问立即报告，避免脚本静默漏测组件。
Set-StrictMode -Version Latest
# 任一 PowerShell 非终止错误升级为终止错误，由统一异常分支记录组件名称。
$ErrorActionPreference = "Stop"

# scriptRoot 是 host_tests 绝对目录，脚本从任意当前目录启动都能定位子组件。
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
# projectRoot 是仓库根目录，用于把 TEMP/TMP 固定到 G 盘项目本地缓存。
$projectRoot = Resolve-Path (Join-Path $scriptRoot "..\..")
# localTemp 保存 GCC 临时文件，避免 Codex 创建的构建数据写入系统 TEMP 或 C 盘。
$localTemp = Join-Path $projectRoot ".codex-local\tmp\host-tests"
# 创建项目本地临时目录；Force 允许重复执行而不报目录已存在。
New-Item -ItemType Directory -Force -Path $localTemp | Out-Null
# 把当前进程及子测试的临时目录指向项目本地路径。
$env:TEMP = $localTemp
# TMP 与 TEMP 保持一致，兼容 GCC、Clang 和 PowerShell 的不同环境变量读取方式。
$env:TMP = $localTemp

# testNames 固定组件回归顺序：先底层协议/硬件，再算法/存储，最后总协调器。
$testNames = @(
    "ble_service",
    "board_sensors",
    "board_ui_runtime",
    "device_config",
    "fitness_core",
    "imu_pipeline",
    "motion_phase",
    "power_ui",
    "session_transfer",
    "session_store",
    "workout_engine",
    "device_coordinator"
)
# failures 收集失败组件和退出码，默认模式在全部测试后统一返回非零。
$failures = New-Object System.Collections.Generic.List[string]

# 逐项运行每个组件自己的权威测试脚本；子脚本仍负责具体编译器参数和断言。
foreach ($testName in $testNames) {
    # testScript 是当前组件的绝对测试入口。
    $testScript = Join-Path $scriptRoot "$testName\run_tests.ps1"
    # 缺失入口属于交付错误，不能把未执行误报成通过。
    if (-not (Test-Path -LiteralPath $testScript -PathType Leaf)) {
        # 记录缺失脚本，便于最终汇总准确定位。
        $failures.Add("${testName}: missing run_tests.ps1")
        # StopOnFailure 模式立即返回失败，适合本地快速开发。
        if ($StopOnFailure) {
            # exit 1 表示测试基础设施不完整。
            exit 1
        }
        # 默认模式继续检查其它组件，避免一次只发现一个缺口。
        continue
    }

    # 输出稳定的开始标记，CI/人工日志都能定位当前组件。
    Write-Host "HOST_TEST_BEGIN name=$testName"
    try {
        # 使用独立 PowerShell 5.1 进程执行，隔离子脚本的变量、目录和 exit 行为。
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $testScript
        # 保存原生进程退出码；PowerShell 本身不会自动因非零退出而抛异常。
        $componentExitCode = $LASTEXITCODE
    }
    catch {
        # 启动或执行层异常统一映射为 1，并保留异常消息。
        $componentExitCode = 1
        # 输出异常详情但不吞掉其它组件的测试机会。
        Write-Host "HOST_TEST_EXCEPTION name=$testName message=$($_.Exception.Message)"
    }

    # 非零退出码表示编译或断言失败，必须进入失败清单。
    if ($componentExitCode -ne 0) {
        # 记录组件和原始退出码，方便复现。
        $failures.Add("${testName}: exit=$componentExitCode")
        # 输出稳定失败标记，避免从长日志猜测结果。
        Write-Host "HOST_TEST_FAIL name=$testName exit=$componentExitCode"
        # 请求快速失败时立即透传该组件退出码。
        if ($StopOnFailure) {
            # 返回实际非零码；异常映射时为 1。
            exit $componentExitCode
        }
    }
    else {
        # 输出稳定通过标记，证明该组件脚本已实际执行且退出码为零。
        Write-Host "HOST_TEST_PASS name=$testName"
    }
}

# 任一失败都让总脚本返回 1，并打印可机器读取的逗号分隔清单。
if ($failures.Count -gt 0) {
    # 失败汇总保留组件顺序，便于与执行日志对应。
    Write-Host "HOST_TESTS_FAILED count=$($failures.Count) items=$($failures -join ', ')"
    # exit 1 表示至少一项回归未通过。
    exit 1
}

# 全部入口存在且退出码为零时输出唯一总通过标记。
Write-Host "HOST_TESTS_OK count=$($testNames.Count)"
# 显式返回零，供 CI、Codex 和用户脚本可靠判定成功。
exit 0
