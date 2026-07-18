# 开启高级脚本参数处理，便于 PowerShell 输出一致错误位置。
[CmdletBinding()]
param()

# 任一 PowerShell 命令失败立即终止，禁止在构建失败后继续启动旧程序。
$ErrorActionPreference = "Stop"
# 取得 pc 目录绝对路径；脚本可从任意当前目录调用。
$PcRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
# 取得仓库工作树根目录，用于保存项目本地 .NET 状态和 NuGet 缓存。
$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PcRoot "..")).Path
# 生成解决方案绝对路径，避免当前目录影响项目发现。
$SolutionPath = Join-Path $PcRoot "FitnessCoach.sln"
# 生成 WPF 项目绝对路径。
$ApplicationProject = Join-Path $PcRoot "FitnessCoach.App\FitnessCoach.App.csproj"
# 把 dotnet CLI 首次运行、证书和工具状态限制在当前项目。
$env:DOTNET_CLI_HOME = Join-Path $RepositoryRoot ".codex-local\dotnet-home"
# 把 NuGet 包缓存限制在当前项目，避免写入用户目录。
$env:NUGET_PACKAGES = Join-Path $RepositoryRoot ".codex-local\cache\nuget"
# 关闭遥测，避免离线运行产生无关网络请求。
$env:DOTNET_CLI_TELEMETRY_OPTOUT = "1"
# 跳过首次欢迎和开发证书配置，缩短首次启动时间。
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = "1"
# 未显式设置运行时滚动策略时允许本机较新桌面运行时承载 net8 测试版程序。
if ([string]::IsNullOrWhiteSpace($env:DOTNET_ROLL_FORWARD)) {
    # Major 只影响运行时选择，不改变 net8 编译目标或发布产物。
    $env:DOTNET_ROLL_FORWARD = "Major"
}

# 创建项目本地 dotnet CLI 目录。
New-Item -ItemType Directory -Force -Path $env:DOTNET_CLI_HOME | Out-Null
# 创建项目本地 NuGet 缓存目录。
New-Item -ItemType Directory -Force -Path $env:NUGET_PACKAGES | Out-Null
# 以 Release 配置编译完整解决方案，确保 BLE、仓储和 WPF 引用同时有效。
& dotnet build $SolutionPath --configuration Release --nologo --verbosity minimal
# dotnet 是外部程序，PowerShell 需要显式检查退出码。
if ($LASTEXITCODE -ne 0) {
    # 构建失败时抛出明确错误，不启动上一次残留二进制。
    throw "FitnessCoach Release 构建失败，退出码：$LASTEXITCODE"
}

# 使用刚构建的 Release 二进制启动 WPF；Mock/真 BLE 模式由设置页持久化选择。
& dotnet run --project $ApplicationProject --configuration Release --no-build --nologo
# 窗口关闭后检查应用退出码，异常退出向调用者返回失败。
if ($LASTEXITCODE -ne 0) {
    # 抛出 WPF 进程退出码，供终端和自动化日志定位。
    throw "FitnessCoach 运行失败，退出码：$LASTEXITCODE"
}
