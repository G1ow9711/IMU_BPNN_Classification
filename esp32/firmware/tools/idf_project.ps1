# 声明固件操作类型，避免用户把任意字符串传给 idf.py。
param(
    # Action 选择仅构建、完整清理、烧录、串口监视或烧录后立即监视。
    [Parameter(Mandatory = $true)]
    [ValidateSet('build', 'fullclean', 'flash', 'monitor', 'flash-monitor')]
    [string]$Action,
    # Port 保存 Windows 串口名，例如 COM7；仅烧录或监视操作需要。
    [Parameter(Mandatory = $false)]
    [string]$Port = ''
)

# 任一命令失败立即终止，禁止在半激活环境中继续烧录。
$ErrorActionPreference = 'Stop'
# 固件根目录是当前 tools 目录的父目录。
$firmwareRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
# 仓库工作树根位于 firmwareRoot 的上两级，用于定位项目本地缓存。
$workspaceRoot = (Resolve-Path -LiteralPath (Join-Path $firmwareRoot '..\..')).Path
# 固定 ESP-IDF 5.5.4 源码目录，避免系统其它 IDF 版本污染构建。
$idfPath = Join-Path $workspaceRoot '.codex-local\cache\esp-idf-v5.5.4'
# 固定 ESP-IDF 编译器、CMake、Ninja 和 Python 环境目录，避免写入 C 盘用户目录。
$idfToolsPath = Join-Path $workspaceRoot '.codex-local\cache\espressif-tools'
# 固定临时文件目录，长路径和构建中间文件均留在当前 G 盘工程。
$localTemp = Join-Path $workspaceRoot '.codex-local\tmp'
# 固定 ccache 对象缓存目录，避免编译缓存写入用户 LocalAppData。
$ccacheRoot = Join-Path $workspaceRoot '.codex-local\cache\ccache'
# 固定 ccache 临时响应文件目录，避免编译参数文件短暂落到系统盘。
$ccacheTemp = Join-Path $workspaceRoot '.codex-local\tmp\ccache'

# ESP-IDF 源码不存在说明尚未准备开发环境，直接给出可执行错误。
if (-not (Test-Path -LiteralPath (Join-Path $idfPath 'tools\idf.py'))) {
    # 抛出阻塞错误，防止误调用系统中版本未知的 idf.py。
    throw "缺少项目本地 ESP-IDF 5.5.4：$idfPath"
}
# 工具目录不存在说明 install.ps1 尚未成功完成。
if (-not (Test-Path -LiteralPath $idfToolsPath)) {
    # 提示先执行安装，不能静默退回 C 盘默认目录。
    throw "缺少项目本地 ESP-IDF 工具链：$idfToolsPath"
}
# 创建项目本地临时目录和 ccache 目录；Force 允许目录已存在。
New-Item -ItemType Directory -Force -Path $localTemp, $ccacheRoot, $ccacheTemp | Out-Null
# 告诉 ESP-IDF 所有下载工具和 Python 环境都从项目本地目录读取。
$env:IDF_TOOLS_PATH = $idfToolsPath
# 固定 SoC 为 ESP32-S3；没有该变量时全新 build 目录会错误回退到经典 ESP32。
$env:IDF_TARGET = 'esp32s3'
# 跳过 ESP-IDF 对全部可选 Git 子模块的递归检出；本产品不使用的 OpenThread 测试仓库会在 Windows 长路径下失败。
$env:IDF_SKIP_CHECK_SUBMODULES = '1'
# 把 PowerShell 与子进程临时目录统一到项目本地目录。
$env:TEMP = $localTemp
# 同步设置 TMP，覆盖部分工具只读取 TMP 的情况。
$env:TMP = $localTemp
# 告诉 ccache 把可复用编译对象保存到项目缓存。
$env:CCACHE_DIR = $ccacheRoot
# 告诉 ccache 把响应文件和原子写入临时文件保存到项目临时目录。
$env:CCACHE_TEMPDIR = $ccacheTemp
# exportTool 是 ESP-IDF 官方环境导出器；key-value 格式可在当前 PowerShell 内安全解析。
$exportTool = Join-Path $idfPath 'tools\idf_tools.py'
# 调用系统现有 Python 读取项目本地工具清单；此步骤不下载或修改工具链。
$exportLines = & python $exportTool export --format key-value
# 环境导出失败时立即终止，禁止使用不完整 PATH 继续编译或烧录。
if ($LASTEXITCODE -ne 0) {
    # 错误明确包含官方导出器退出码，便于检查本地 IDF 安装状态。
    throw "ESP-IDF 环境导出失败，退出码：$LASTEXITCODE"
}
# 遍历每个 `名称=值` 输出项，把编译器、Python 环境和 OpenOCD 路径写入当前进程。
foreach ($exportLine in $exportLines) {
    # 忽略官方工具可能输出的空行，避免把空名字写入环境变量表。
    if ([string]::IsNullOrWhiteSpace($exportLine)) {
        continue
    }
    # 只按第一个等号分割；PATH 或其它值内部允许继续包含等号。
    $separatorIndex = $exportLine.IndexOf('=')
    # 缺少变量名或等号表示导出格式异常，必须拒绝继续。
    if ($separatorIndex -le 0) {
        # 报错包含原始行，便于复现 ESP-IDF 版本或工具输出变化。
        throw "无法解析 ESP-IDF 环境行：$exportLine"
    }
    # environmentName 保存等号左侧稳定环境变量名。
    $environmentName = $exportLine.Substring(0, $separatorIndex)
    # environmentValue 保存等号右侧完整值，路径分号和空格均保持不变。
    $environmentValue = $exportLine.Substring($separatorIndex + 1)
    # 官方 PATH 尾部使用 `%PATH%` 占位；替换为调用前 PATH，保留系统基础命令。
    if ($environmentName -eq 'PATH') {
        # 只替换精确占位符，避免修改真实目录中偶然出现的 PATH 字样。
        $environmentValue = $environmentValue.Replace('%PATH%', $env:PATH)
    }
    # 把当前项写入子进程可继承环境；变量只在本脚本和后续 idf.py 子进程中生效。
    Set-Item -Path "Env:$environmentName" -Value $environmentValue
}
# 固定 IDF_PATH 指向当前工作树缓存，阻止系统其它 ESP-IDF 版本参与构建。
$env:IDF_PATH = $idfPath

# 烧录和串口监视必须明确指定端口，避免误刷其它串口设备。
if (($Action -in @('flash', 'monitor', 'flash-monitor')) -and [string]::IsNullOrWhiteSpace($Port)) {
    # 抛出端口缺失错误；示例值为 COM7。
    throw 'flash/monitor 操作必须通过 -Port 指定串口，例如 COM7。'
}

# 切换到 ESP-IDF 工程根，确保 partitions.csv 和 sdkconfig.defaults 使用正确相对路径。
Push-Location -LiteralPath $firmwareRoot
try {
    # 组装固定 idf.py 入口；使用当前激活 Python 运行，避免 Windows 文件关联差异。
    $idfPy = Join-Path $idfPath 'tools\idf.py'
    # build 只编译和链接固件，不访问串口。
    if ($Action -eq 'build') {
        # 执行完整增量构建，输出写入 firmware/build。
        python $idfPy -B build build
    }
    # fullclean 删除 IDF 构建产物，但不删除组件缓存、数据或模型工件。
    elseif ($Action -eq 'fullclean') {
        # 执行 ESP-IDF 官方完整清理命令。
        python $idfPy -B build fullclean
    }
    # flash 只烧录已构建镜像；idf.py 会在需要时先增量构建。
    elseif ($Action -eq 'flash') {
        # 使用用户明确提供的串口写入分区表、应用和启动数据。
        python $idfPy -B build -p $Port flash
    }
    # monitor 只连接串口日志，不改变 Flash 内容。
    elseif ($Action -eq 'monitor') {
        # 使用 IDF monitor 解码日志和崩溃地址；Ctrl+] 退出。
        python $idfPy -B build -p $Port monitor
    }
    # flash-monitor 连续执行烧录和监视，适合用户拿到开发板后的首次验收。
    else {
        # 同一串口完成烧录后进入日志监视，减少手工命令差异。
        python $idfPy -B build -p $Port flash monitor
    }
    # 传播 idf.py 退出码，调用方和 CI 可据此判断构建或烧录是否成功。
    exit $LASTEXITCODE
}
finally {
    # 无论成功或异常都恢复调用者原工作目录，避免后续命令误在 firmware 下执行。
    Pop-Location
}
