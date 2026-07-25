# 声明必填中文 CSV、会话序号和人工真值动作枚举。
param(
    # InputCsv 指向上位机导出的 43/44 列中文 CSV，只读。
    [Parameter(Mandatory = $true)][string]$InputCsv,
    # SessionSequence 指定要重放的独立会话序号。
    [Parameter(Mandatory = $true)][uint32]$SessionSequence,
    # ActionId 指定人工真值动作 0..10，只用于选择生产计数器。
    [Parameter(Mandatory = $true)][ValidateRange(0, 10)][uint32]$ActionId
)
# 遇到解析、编译或回放错误立即停止，禁止沿用旧可执行文件。
$ErrorActionPreference = "Stop"
# 解析脚本目录，保证从任意工作目录调用均稳定。
$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
# 解析仓库根目录。
$repositoryRoot = Resolve-Path (Join-Path $scriptDirectory "..\..\..")
# 输入文件必须真实存在，禁止静默搜索另一份日志。
$resolvedInput = Resolve-Path -LiteralPath $InputCsv
# 使用项目本地临时目录，避免污染系统 TEMP。
$temporaryDirectory = Join-Path $repositoryRoot ".codex-local\tmp\session_replay"
# 创建固定临时目录；已存在时复用。
New-Item -ItemType Directory -Force -Path $temporaryDirectory | Out-Null
# 获取当前受控 GCC 路径。
$compiler = (Get-Command gcc -ErrorAction Stop).Source
# 定义新回放可执行文件，编译失败时不会运行旧副本。
$outputExecutable = Join-Path $temporaryDirectory "session_replay.exe"
# 定义三个生产算法源文件。
$engineSource = Join-Path $repositoryRoot "esp32\firmware\components\workout_engine\workout_engine.c"
$phaseSource = Join-Path $repositoryRoot "esp32\firmware\components\motion_phase\motion_phase.c"
$fitnessSource = Join-Path $repositoryRoot "esp32\firmware\components\fitness_core\src\fitness_core.c"
# 定义只负责标准输入编排的回放入口。
$replaySource = Join-Path $scriptDirectory "session_replay.c"
# 定义生产公共头目录。
$engineInclude = Join-Path $repositoryRoot "esp32\firmware\components\workout_engine\include"
# 定义动作相位公共头目录。
$phaseInclude = Join-Path $repositoryRoot "esp32\firmware\components\motion_phase\include"
# 定义健身领域公共头目录。
$fitnessInclude = Join-Path $repositoryRoot "esp32\firmware\components\fitness_core\include"
# 定义自动生成双 M0 特征头目录。
$modelInclude = Join-Path $repositoryRoot "esp32\include"
# 严格编译生产实现和回放入口；任何警告都视为失败。
& $compiler -std=c11 -Wall -Wextra -Wpedantic -Werror -Wmissing-prototypes `
    -I $engineInclude -I $phaseInclude -I $fitnessInclude -I $modelInclude `
    $engineSource $phaseSource $fitnessSource $replaySource -lm -o $outputExecutable
# 编译失败原样退出，禁止执行残留二进制。
if ($LASTEXITCODE -ne 0) {
    # 返回 GCC 错误码。
    exit $LASTEXITCODE
}
# 使用 UTF-8 读取全部日志行，避免中文表头被本机代码页破坏。
$rows = Import-Csv -LiteralPath $resolvedInput -Encoding UTF8
# 只选目标会话的准备和训练行；训练完成后的静态尾巴不属于计数输入。
$sessionRows = @(
    $rows | Where-Object {
        # 会话序号必须匹配，且设备状态不能是训练完成。
        ([uint32]$_.'会话序号' -eq $SessionSequence) -and
        ($_.'设备状态' -ne '训练完成')
    }
)
# 目标会话必须至少包含两个模型窗所需的 74 点。
if ($sessionRows.Count -lt 74) {
    # 抛出精确会话和点数。
    throw "会话 $SessionSequence 仅有 $($sessionRows.Count) 点，不能完成两窗生产重放。"
}
# 使用不受系统小数区域影响的固定文化格式。
$culture = [System.Globalization.CultureInfo]::InvariantCulture
# 把中文 CSV 转成 C 回放器固定八列 ASCII；不落盘、不修改原文件。
$numericLines = foreach ($row in $sessionRows) {
    # 把 0x 前缀质量位解析为 16 位无符号整数。
    $qualityText = [string]$row.'质量标志（十六进制）'
    # 空质量位按零处理；当前导出正常应始终有值。
    $qualityValue = if ([string]::IsNullOrWhiteSpace($qualityText)) {
        # 返回零质量位。
        [uint16]0
    } else {
        # 去掉 0x 后按十六进制转换。
        [Convert]::ToUInt16($qualityText.Replace('0x', ''), 16)
    }
    # 按 ms,gx,gy,gz,ax,ay,az,quality 顺序输出，浮点使用往返安全 R 格式。
    [string]::Format(
        $culture,
        '{0},{1:R},{2:R},{3:R},{4:R},{5:R},{6:R},0x{7:X4}',
        [uint64]$row.'设备单调时间（毫秒）',
        [double]::Parse($row.'角速度横轴（度每秒）', $culture),
        [double]::Parse($row.'角速度纵轴（度每秒）', $culture),
        [double]::Parse($row.'角速度垂直轴（度每秒）', $culture),
        [double]::Parse($row.'加速度横轴（重力倍数）', $culture),
        [double]::Parse($row.'加速度纵轴（重力倍数）', $culture),
        [double]::Parse($row.'加速度垂直轴（重力倍数）', $culture),
        [uint16]$qualityValue)
}
# 把数值流直接送入新编译的生产 C 回放器，不创建中间数据副本。
$numericLines | & $outputExecutable $SessionSequence $ActionId
# 回放失败原样退出。
if ($LASTEXITCODE -ne 0) {
    # 返回回放器错误码。
    exit $LASTEXITCODE
}
