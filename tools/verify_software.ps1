# 声明无硬件软件验收参数；默认执行包含 ESP-IDF 链接在内的全部检查。
param(
    # SkipIdfBuild 仅用于快速本地回归；正式交付验收不得启用。
    [switch]$SkipIdfBuild,
    # BaseArtifactDir 可覆盖基础 M0 工件目录；留空时使用相邻训练工作树中已验证的基础 M0。
    [string]$BaseArtifactDir = "",
    # MaskedArtifactDir 可覆盖相位掩码 M0 工件目录；留空时使用相邻训练工作树中已验证的相位掩码 M0。
    [string]$MaskedArtifactDir = ""
)

# 严格模式让未定义变量、缺失属性和错误索引立即中止，避免漏跑后仍输出通过。
Set-StrictMode -Version Latest
# 任一 PowerShell 非终止错误升级为终止错误，由顶层调用者获得非零退出码。
$ErrorActionPreference = "Stop"

# scriptRoot 是当前 tools 目录的绝对路径，脚本可从任意工作目录启动。
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
# projectRoot 是包含 Python、ESP32、PC 和共享协议的仓库根目录。
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $scriptRoot "..")).Path
# localRoot 保存本次验证临时文件、dotnet 状态和包缓存，禁止写入用户 C 盘目录。
$localRoot = Join-Path $projectRoot ".codex-local"
# localTemp 保存 GCC、Python、ESP-IDF 和 PowerShell 临时文件。
$localTemp = Join-Path $localRoot "tmp\software-verification"
# dotnetHome 保存首次运行标志和 SDK 本地状态。
$dotnetHome = Join-Path $localRoot "dotnet-home"
# nugetPackages 保存可复用 NuGet 包；当前工程无额外包时目录仍保持产品合同。
$nugetPackages = Join-Path $localRoot "cache\nuget"
# 创建所有项目本地目录；Force 允许重复验收而不报目录已存在。
New-Item -ItemType Directory -Force -Path $localTemp, $dotnetHome, $nugetPackages | Out-Null
# 把临时目录传给 GCC、Python 和 ESP-IDF 子进程。
$env:TEMP = $localTemp
# TMP 与 TEMP 保持一致，兼容不同 Windows 工具读取习惯。
$env:TMP = $localTemp
# 把 dotnet CLI 状态固定在当前 G 盘工作树。
$env:DOTNET_CLI_HOME = $dotnetHome
# 把 NuGet 缓存固定在项目本地缓存目录。
$env:NUGET_PACKAGES = $nugetPackages
# 本机只有更高主版本桌面运行时时允许测试宿主向前滚动；不改变目标框架 net8。
$env:DOTNET_ROLL_FORWARD = "Major"

# pythonCandidates 按“当前工作树 venv、原工程共享 venv”顺序列出可接受解释器。
$pythonCandidates = @(
    (Join-Path $projectRoot ".venv\Scripts\python.exe"),
    (Join-Path $projectRoot "..\..\Project\.venv\Scripts\python.exe")
)
# pythonExe 初始为空；循环会选择第一个真实存在的项目本地解释器。
$pythonExe = $null
# 逐个检查候选，不调用系统 Python，避免包版本漂移。
foreach ($candidate in $pythonCandidates) {
    # 只有可执行文件存在时才锁定该解释器。
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        # Resolve-Path 规范化相对父目录，便于日志显示准确绝对路径。
        $pythonExe = (Resolve-Path -LiteralPath $candidate).Path
        # 找到最高优先级候选后结束搜索。
        break
    }
}
# 没有项目 venv 时拒绝回退系统环境，并给出明确恢复方法。
if ($null -eq $pythonExe) {
    # 抛错说明需要在仓库根创建 .venv 或保留原工程共享 venv。
    throw "未找到项目 Python 环境；请先在仓库根创建 .venv 并安装 python/requirements.txt。"
}

# 未显式传入基础工件时定位相邻 finals-jumping-squat 的已验证输出。
if ([string]::IsNullOrWhiteSpace($BaseArtifactDir)) {
    # 基础 M0 使用完整 297 维输入，作为固定融合的主要分类证据。
    $BaseArtifactDir = Join-Path $projectRoot "..\finals-jumping-squat\outputs\round29_clean297_m0_validation_20260712"
}
# 未显式传入掩码工件时定位相邻 finals-jumping-squat 的已验证输出。
if ([string]::IsNullOrWhiteSpace($MaskedArtifactDir)) {
    # 相位掩码 M0 在标准化后屏蔽索引 184:232，用于补充基础模型的分类证据。
    $MaskedArtifactDir = Join-Path $projectRoot "..\finals-jumping-squat\outputs\round37_suppress_normalized_phase_validation_20260712"
}
# 基础工件必须存在，否则无法重建 Python 参考 logits。
if (-not (Test-Path -LiteralPath $BaseArtifactDir -PathType Container)) {
    # 错误包含实际路径，便于用户通过参数指向冻结工件。
    throw "基础 M0 工件目录不存在：$BaseArtifactDir"
}
# 掩码工件必须存在，否则无法核对第二模型和固定融合。
if (-not (Test-Path -LiteralPath $MaskedArtifactDir -PathType Container)) {
    # 错误包含实际路径，避免把单模型验证误报成最终双模型通过。
    throw "掩码 M0 工件目录不存在：$MaskedArtifactDir"
}

# 进入仓库根，确保所有相对路径和 Python 本地导入保持一致。
Push-Location -LiteralPath $projectRoot
try {
    # 输出稳定阶段标记，长日志可按名称定位当前验收项。
    Write-Host "VERIFY_BEGIN stage=python_unittest"
    # 执行 Python 全量标准库测试，不依赖额外 pytest 插件。
    & $pythonExe -m unittest discover -s python -p "test_*.py"
    # 非零退出表示算法、导出或合同测试失败，立即停止后续阶段。
    if ($LASTEXITCODE -ne 0) {
        # 抛错保留 Python 原始退出码。
        throw "Python 单元测试失败，exit=$LASTEXITCODE"
    }
    # 输出 Python 阶段通过标记。
    Write-Host "VERIFY_PASS stage=python_unittest"

    # 输出十三页中文预览合同阶段标记，防止设备画布外开发导航重新出现英文状态码。
    Write-Host "VERIFY_BEGIN stage=chinese_ui_preview"
    # previewDirectory 指向预览器源码和标准库单元测试所在目录。
    $previewDirectory = Join-Path $projectRoot "tools\watch_ui_preview"
    # 临时进入预览器目录，使其本地模块导入不依赖用户 PYTHONPATH。
    Push-Location -LiteralPath $previewDirectory
    try {
        # 执行十三页名称、控制按钮和边界说明的纯中文合同。
        & $pythonExe -m unittest -v test_watch_ui_preview.py
        # 中文合同失败时立即停止，不能只靠人工截图放行。
        if ($LASTEXITCODE -ne 0) {
            # 抛错保留标准库测试退出码。
            throw "中文界面预览合同失败，exit=$LASTEXITCODE"
        }
        # 无窗口冒烟覆盖十三页 presenter 分支和关键交互状态机。
        & $pythonExe watch_ui_preview.py --smoke-test
        # 页面漏分支或交互合同失败时阻止后续交付阶段。
        if ($LASTEXITCODE -ne 0) {
            # 抛错保留预览器冒烟退出码。
            throw "十三页界面预览冒烟失败，exit=$LASTEXITCODE"
        }
    } finally {
        # 无论测试成功或失败都恢复仓库根，保证后续相对路径稳定。
        Pop-Location
    }
    # 输出中文预览阶段通过标记。
    Write-Host "VERIFY_PASS stage=chinese_ui_preview"

    # 输出 C/Python 数值对照阶段标记。
    Write-Host "VERIFY_BEGIN stage=dual_m0_c"
    # dualScratch 为当前 PowerShell 进程分配独立 C harness 目录，避免并行审计争用同名 exe。
    $dualScratch = Join-Path $localTemp ("dual-m0-c-{0}" -f $PID)
    # 创建当前验证器独占目录；Force 允许同一进程失败后安全重试。
    New-Item -ItemType Directory -Force -Path $dualScratch | Out-Null
    # reportPath 保存当前进程独占的机器可读误差和分类一致性，不写入 Git 跟踪目录。
    $reportPath = Join-Path $dualScratch "dual_m0_c_report.json"
    # 运行正式验证器：297 特征、两个 M0、0.85/0.15 融合和动作段累计均需过门。
    & $pythonExe python\verify_dual_m0_c.py `
        --base-artifact-dir $BaseArtifactDir `
        --masked-artifact-dir $MaskedArtifactDir `
        --header-dir esp32\include `
        --work-dir $dualScratch `
        --report $reportPath
    # 任一误差门或类别断言失败都会返回非零。
    if ($LASTEXITCODE -ne 0) {
        # 抛错阻止后续构建掩盖数值回归。
        throw "双 M0 C/Python 数值验证失败，exit=$LASTEXITCODE"
    }
    # 输出双模型数值阶段通过标记。
    Write-Host "VERIFY_PASS stage=dual_m0_c"

    # 输出 ESP32 纯 C 主机测试阶段标记。
    Write-Host "VERIFY_BEGIN stage=esp32_host_tests"
    # 使用独立 PowerShell 5.1 运行 11 个组件入口，隔离子脚本 exit 和变量。
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File esp32\host_tests\run_all.ps1 -StopOnFailure
    # 总入口非零表示至少一个组件编译或断言失败。
    if ($LASTEXITCODE -ne 0) {
        # 透传失败事实并停止上位机构建。
        throw "ESP32 主机测试失败，exit=$LASTEXITCODE"
    }
    # 输出 ESP32 主机测试通过标记。
    Write-Host "VERIFY_PASS stage=esp32_host_tests"

    # 输出上位机 Release 构建阶段标记。
    Write-Host "VERIFY_BEGIN stage=pc_release"
    # 构建 solution 中六个项目；TreatWarningsAsErrors 由各项目合同保证。
    & dotnet build pc\FitnessCoach.sln --configuration Release --nologo --verbosity minimal
    # 编译失败或警告升级错误时立即停止。
    if ($LASTEXITCODE -ne 0) {
        # 错误保留 dotnet 原始退出码。
        throw "PC solution Release 构建失败，exit=$LASTEXITCODE"
    }
    # 独立会话传输测试不在 solution 内，必须单独构建，避免漏验第七个工程目录。
    & dotnet build pc\FitnessCoach.SessionTransfer.Tests\FitnessCoach.SessionTransfer.Tests.csproj --configuration Release --nologo --verbosity minimal
    # 独立测试项目编译失败时终止。
    if ($LASTEXITCODE -ne 0) {
        # 明确指出独立项目而非 solution 失败。
        throw "PC SessionTransfer 独立测试构建失败，exit=$LASTEXITCODE"
    }
    # 运行综合协议、WinRT fake 会话、仓储、ViewModel 和动画测试。
    & dotnet run --project pc\FitnessCoach.Tests\FitnessCoach.Tests.csproj --configuration Release --no-build --nologo
    # 综合测试非零表示上位机业务合同失败。
    if ($LASTEXITCODE -ne 0) {
        # 抛错阻止把只编译成功写成上位机测试通过。
        throw "PC 综合测试失败，exit=$LASTEXITCODE"
    }
    # 运行独立 LIST/GET 分块补传和幂等仓储测试。
    & dotnet run --project pc\FitnessCoach.SessionTransfer.Tests\FitnessCoach.SessionTransfer.Tests.csproj --configuration Release --no-build --nologo
    # 会话补传测试失败时返回非零。
    if ($LASTEXITCODE -ne 0) {
        # 错误区分独立会话测试，方便定位协议回归。
        throw "PC 会话传输测试失败，exit=$LASTEXITCODE"
    }
    # 输出上位机构建与测试通过标记。
    Write-Host "VERIFY_PASS stage=pc_release"

    # 输出 AGENTS.md 中文注释和生成工件哈希审计阶段标记。
    Write-Host "VERIFY_BEGIN stage=agents_compliance"
    # 通过统一入口先验证审计器测试，再审计源码、公开 ABI 和生成工件哈希。
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\check_agents_comments.ps1 -PythonExe $pythonExe
    # 非零表示逐行中文说明或生成物一致性未满足交付规范。
    if ($LASTEXITCODE -ne 0) {
        # 抛错阻止未合规源码进入最终交付结论。
        throw "AGENTS.md 合规审计失败，exit=$LASTEXITCODE"
    }
    # 输出注释与生成物审计通过标记。
    Write-Host "VERIFY_PASS stage=agents_compliance"

    # 正式验收默认执行完整 ESP-IDF 增量编译和链接。
    if (-not $SkipIdfBuild) {
        # 输出固件构建阶段标记。
        Write-Host "VERIFY_BEGIN stage=esp_idf_build"
        # 固定使用项目本地 ESP-IDF 5.5.4、ESP32-S3 目标和 G 盘工具缓存。
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File esp32\firmware\tools\idf_project.ps1 -Action build
        # 编译或链接非零时终止，不把主机测试替代真实固件构建。
        if ($LASTEXITCODE -ne 0) {
            # 抛错保留 IDF 原始退出码。
            throw "ESP-IDF 固件构建失败，exit=$LASTEXITCODE"
        }
        # 输出固件构建通过标记。
        Write-Host "VERIFY_PASS stage=esp_idf_build"
    }
    else {
        # 快速回归明确显示跳过，不允许日志误读为已经构建。
        Write-Host "VERIFY_SKIP stage=esp_idf_build reason=SkipIdfBuild"
    }

    # 输出 Git 空白与补丁结构检查阶段标记。
    Write-Host "VERIFY_BEGIN stage=git_diff_check"
    # git diff --check 检测尾随空格、冲突标记和补丁空白错误，不修改工作树。
    & git diff --check
    # 非零表示代码或文档存在提交级格式错误。
    if ($LASTEXITCODE -ne 0) {
        # 抛错保留 Git 原始退出码。
        throw "Git 差异检查失败，exit=$LASTEXITCODE"
    }
    # 输出 Git 差异检查通过标记。
    Write-Host "VERIFY_PASS stage=git_diff_check"

    # 所有未跳过阶段均通过后输出唯一总标记；该标记不代表真板实测。
    Write-Host "SOFTWARE_VERIFICATION_OK hardware_validation=not_run_no_hardware"
}
finally {
    # 无论成功或失败都恢复调用者目录，避免后续命令误在仓库根执行。
    Pop-Location
}
