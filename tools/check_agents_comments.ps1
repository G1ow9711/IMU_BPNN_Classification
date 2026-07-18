# 声明可选 Python 解释器；留空时只在当前工程和原 Project 工程的本地 .venv 中查找。
param(
    # PythonExe 必须指向项目本地解释器，避免系统 Python 版本和依赖漂移。
    [string]$PythonExe = ""
)

# 严格模式让未定义变量和缺失属性立即中止审计。
Set-StrictMode -Version Latest
# 任一 PowerShell 非终止错误升级为终止错误，避免漏跑后仍输出通过。
$ErrorActionPreference = "Stop"

# scriptRoot 是当前 tools 目录的绝对路径，可从任意工作目录调用本脚本。
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
# projectRoot 是包含 Agents.md、ESP32、PC 和 Python 的仓库根目录。
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $scriptRoot "..")).Path

# 调用者未指定解释器时按当前工作树、原 Project 工作区顺序查找本地 .venv。
if ([string]::IsNullOrWhiteSpace($PythonExe)) {
    # candidates 不包含系统 Python，保证审计结果可复现。
    $candidates = @(
        (Join-Path $projectRoot ".venv\Scripts\python.exe"),
        (Join-Path $projectRoot "..\..\Project\.venv\Scripts\python.exe")
    )
    # 逐个检查候选路径；第一个存在的解释器成为本次唯一运行时。
    foreach ($candidate in $candidates) {
        # 只接受真实文件，目录或失效链接不能执行。
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            # 规范化绝对路径，避免后续 Push-Location 改变相对路径含义。
            $PythonExe = (Resolve-Path -LiteralPath $candidate).Path
            # 找到最高优先级解释器后停止搜索。
            break
        }
    }
}

# 最终仍为空或文件不存在时拒绝回退系统环境，并给出恢复方式。
if ([string]::IsNullOrWhiteSpace($PythonExe) -or -not (Test-Path -LiteralPath $PythonExe -PathType Leaf)) {
    # 抛错使 CI 和总验收获得非零退出码。
    throw "未找到项目 Python 环境；请在仓库根创建 .venv，或用 -PythonExe 指定项目本地解释器。"
}

# 进入仓库根，保证 unittest 发现路径和 Git 变更范围均稳定。
Push-Location -LiteralPath $projectRoot
try {
    # 先运行审计器自身红绿边界测试，防止规则函数被改坏后产生假通过。
    & $PythonExe -m unittest discover -s tools -p "test_*.py"
    # 单元测试非零时立即中止，不运行可能失真的生产审计。
    if ($LASTEXITCODE -ne 0) {
        # 错误保留测试退出码，便于总验收定位。
        throw "AGENTS 注释审计器单元测试失败，exit=$LASTEXITCODE"
    }
    # 运行实际变更源码审计，覆盖中文密度、语义块、枚举、参数及 ABI 数组/指针合同。
    & $PythonExe tools\check_agents_compliance.py
    # 任一源码或生成工件合同失败时返回非零。
    if ($LASTEXITCODE -ne 0) {
        # 抛错阻止未满足 Agents.md 的代码进入完成结论。
        throw "AGENTS 中文注释与公开 ABI 合同审计失败，exit=$LASTEXITCODE"
    }
    # 输出唯一稳定成功标记，供人工日志和 CI 检索。
    Write-Host "AGENTS_COMMENT_CONTRACT_OK"
}
finally {
    # 无论成功失败都恢复调用者原目录，避免污染后续命令。
    Pop-Location
}
