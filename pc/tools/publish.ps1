# 开启高级参数；OutputDirectory 可覆盖默认项目本地发布目录。
[CmdletBinding()]
param(
    # 自定义发布目录；留空时使用带时间戳的 .codex-local\publish 子目录。
    [string]$OutputDirectory = ""
)

# 任一 PowerShell 命令失败立即终止，禁止把半成品误报为发布成功。
$ErrorActionPreference = "Stop"
# 取得 pc 目录绝对路径。
$PcRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
# 取得仓库根目录，所有缓存和默认发布物均保存在该目录内。
$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PcRoot "..")).Path
# 生成 WPF 项目绝对路径。
$ApplicationProject = Join-Path $PcRoot "FitnessCoach.App\FitnessCoach.App.csproj"
# 把 dotnet CLI 状态限制在当前项目。
$env:DOTNET_CLI_HOME = Join-Path $RepositoryRoot ".codex-local\dotnet-home"
# 把 NuGet 和 win-x64 runtime pack 缓存限制在当前项目。
$env:NUGET_PACKAGES = Join-Path $RepositoryRoot ".codex-local\cache\nuget"
# 关闭遥测，发布过程只访问必要的软件包源。
$env:DOTNET_CLI_TELEMETRY_OPTOUT = "1"
# 跳过首次欢迎和开发证书配置。
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = "1"

# 创建项目本地 dotnet CLI 目录。
New-Item -ItemType Directory -Force -Path $env:DOTNET_CLI_HOME | Out-Null
# 创建项目本地 NuGet 缓存目录。
New-Item -ItemType Directory -Force -Path $env:NUGET_PACKAGES | Out-Null

# 未指定目录时使用时间戳创建全新目录，避免旧发布文件混入新版本。
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    # 时间戳使用本机时间，只用于目录名，不进入协议或训练记录。
    $Timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    # 默认产物保存在被 Git 忽略的项目本地运行目录。
    $OutputDirectory = Join-Path $RepositoryRoot ".codex-local\publish\FitnessCoach-win-x64-$Timestamp"
}

# 把相对自定义目录转换为绝对路径，输出日志保持清晰。
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
# 创建目标目录；默认时间戳目录不会覆盖旧发布物。
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
# 发布 win-x64 自包含 Release：目标电脑无需预装 .NET 8 Desktop Runtime。
& dotnet publish $ApplicationProject `
    --configuration Release `
    --runtime win-x64 `
    --self-contained true `
    --output $OutputDirectory `
    --nologo `
    --verbosity minimal `
    -p:PublishSingleFile=false `
    -p:PublishReadyToRun=false
# 显式检查外部 dotnet 退出码。
if ($LASTEXITCODE -ne 0) {
    # 发布失败时给出目标目录，便于人工删除未完成内容。
    throw "FitnessCoach win-x64 自包含发布失败，退出码：$LASTEXITCODE，目录：$OutputDirectory"
}

# 输出可直接复制到 Windows 10/11 x64 电脑的目录。
Write-Host "发布完成：$OutputDirectory"
# 输出主程序路径，减少用户查找步骤。
Write-Host "启动文件：$(Join-Path $OutputDirectory 'FitnessCoach.App.exe')"
