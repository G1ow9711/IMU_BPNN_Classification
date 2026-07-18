param(
    # FontPath 指向 SIL OFL 1.1 授权的 Noto Sans SC OpenType 字体；文件必须覆盖简体中文。
    [string]$FontPath = 'C:\Windows\Fonts\Noto Sans SC (TrueType).otf'
)

# 开启严格模式，避免未定义变量静默生成不完整字库。
Set-StrictMode -Version Latest
# 任一外部命令或文件错误都终止生成，禁止留下部分更新的字体集合。
$ErrorActionPreference = 'Stop'

# repositoryRoot 是当前脚本所在 tools 目录的父目录，即 Git 工作树根目录。
$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
# presenterPath 是设备所有用户可见中文字符串的权威来源。
$presenterPath = Join-Path $repositoryRoot 'esp32\firmware\components\ui\ui_presenter.c'
# outputDirectory 保存可直接参与 ESP-IDF 编译的字体 C 文件、许可证和哈希清单。
$outputDirectory = Join-Path $repositoryRoot 'esp32\firmware\components\ui\fonts'
# npmCache 把 npx 下载缓存限制在当前工作树，避免污染用户全局缓存目录。
$npmCache = Join-Path $repositoryRoot '.codex-local\cache\npm'

# 源字体不存在时立即失败；不得回退到授权不明或缺字的系统字体。
if (-not (Test-Path -LiteralPath $FontPath -PathType Leaf)) {
    # 报错同时给出调用方传入的绝对或相对路径，便于安装 Noto Sans SC 后重试。
    throw "找不到 Noto Sans SC 字体：$FontPath"
}
# presenter 不存在表示脚本不在预期项目中运行，继续会生成错误字符集合。
if (-not (Test-Path -LiteralPath $presenterPath -PathType Leaf)) {
    # 阻止在错误目录生成空字库。
    throw "找不到设备 UI 文案源：$presenterPath"
}

# 创建项目内 npm 缓存目录；目录已存在时保持其内容，便于离线重跑。
New-Item -ItemType Directory -Force -Path $npmCache | Out-Null
# 创建字体输出目录；生成文件使用固定名字覆盖，确保 CMake 不引用旧版本。
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
# 把 npm 缓存环境变量设为项目路径；只影响当前 PowerShell 进程及其 npx 子进程。
$env:npm_config_cache = $npmCache

# 以 UTF-8 读取 presenter，保留所有简体中文用户文案。
$presenterText = [System.IO.File]::ReadAllText($presenterPath, [System.Text.Encoding]::UTF8)
# stringLiteralMatches 只提取 C 字符串字面量，排除中文代码注释，避免无意义扩大 Flash 字库。
$stringLiteralMatches = [regex]::Matches($presenterText, '"(?:[^"\\]|\\.)*"')
# glyphSet 使用有序字典语义去重；键表示 Unicode 字符，值不参与生成。
$glyphSet = [System.Collections.Generic.SortedSet[string]]::new([System.StringComparer]::Ordinal)
# 遍历全部 C 字符串字面量，收集 U+3400～U+9FFF 中日韩统一表意字符。
foreach ($literalMatch in $stringLiteralMatches) {
    # chineseMatches 只保留汉字；ASCII 数字、符号由固定 0x20～0x7E 范围提供。
    $chineseMatches = [regex]::Matches($literalMatch.Value, '[\u3400-\u9FFF]')
    # 逐个加入有序集合；重复字符不会增加生成字体体积。
    foreach ($chineseMatch in $chineseMatches) {
        # 忽略 Add 返回的是否新增标志；最终集合本身就是权威去重结果。
        [void]$glyphSet.Add($chineseMatch.Value)
    }
}
# 加入中文全角标点，允许未来提示文案使用常见符号而无需立即重生成字体。
foreach ($punctuation in @('，', '。', '！', '？', '：', '；', '（', '）', '、')) {
    # 把每个标点加入同一集合，保证 manifest 可审计完整字符范围。
    [void]$glyphSet.Add($punctuation)
}
# 没有任何中文字符表示提取规则或源文件损坏；空字库不允许进入构建。
if ($glyphSet.Count -eq 0) {
    # 抛出明确错误，要求先恢复 presenter 中文文案。
    throw '未从 ui_presenter.c 提取到任何中文用户字符。'
}
# symbols 按 Unicode 顺序连接全部中文字符，作为 lv_font_conv 的精确 --symbols 参数。
$symbols = [string]::Concat([string[]]$glyphSet)
# npxPath 固定使用 Node.js 官方 Windows 命令入口，避免 PowerShell 执行策略阻止 npx.ps1。
$npxPath = (Get-Command npx.cmd -ErrorAction Stop).Source
# fontSizes 覆盖兼容小字号、状态/按钮、标题/副信息和主动作/倒计时四种产品层级，单位为像素。
$fontSizes = @(16, 20, 28, 36)
# generatedFonts 收集四个生成物的名字、字号、字节数和 SHA-256，最终写入清单。
$generatedFonts = [System.Collections.Generic.List[object]]::new()

# 逐个字号生成独立 C 字体；任一字号失败都会终止，不更新 manifest。
foreach ($fontSize in $fontSizes) {
    # fontName 是 C 全局 lv_font_t 符号，renderer 与生成文件必须逐字符一致。
    $fontName = "ui_font_noto_sans_sc_$fontSize"
    # outputPath 是当前字号自动生成 C 文件绝对路径。
    $outputPath = Join-Path $outputDirectory "$fontName.c"
    # arguments 使用 2 bpp 抗锯齿和非压缩位图；厂家稳定中文字体同样使用 bitmap_format=0，避免 ESP32 真板压缩字形解码重影。
    $arguments = @(
        '--yes',
        'lv_font_conv',
        '--size', [string]$fontSize,
        '--bpp', '2',
        '--no-compress',
        '--format', 'lvgl',
        '--font', $FontPath,
        '--range', '0x20-0x7E',
        '--symbols', $symbols,
        '--no-kerning',
        '--lv-include', 'lvgl.h',
        '--lv-font-name', $fontName,
        '--output', $outputPath
    )
    # 同步运行字体转换器；标准输出保留给开发者定位缺字或字体解析失败。
    & $npxPath @arguments
    # npx 非零退出表示当前字号未可靠生成，立即失败并阻止写 manifest。
    if ($LASTEXITCODE -ne 0) {
        # 错误包含字号，便于单独复现同一转换命令。
        throw "lv_font_conv 生成 ${fontSize}px 字体失败，退出码：$LASTEXITCODE"
    }
    # 生成文件必须存在且非空，否则视为转换器异常退出。
    if (-not (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
        # 阻止后续 CMake 引用不存在文件。
        throw "字体生成器未产生文件：$outputPath"
    }
    # generatedText 读取转换器输出，用于追加中文来源、授权和禁止手改说明。
    $generatedText = [System.IO.File]::ReadAllText($outputPath, [System.Text.Encoding]::UTF8)
    # 把转换器注释中的本机绝对字体路径替换为稳定文件名，避免泄露机器目录且保证工作树移动后差异可复现。
    $generatedText = $generatedText.Replace($FontPath, 'NotoSansSC-Regular.otf')
    # portableOutputPath 使用仓库相对路径描述生成物，避免转换器把当前工作树绝对路径写进 C 文件头。
    $portableOutputPath = "esp32/firmware/components/ui/fonts/$fontName.c"
    # 把转换器记录的绝对输出路径替换为仓库相对路径，使不同开发机重生成时只产生真实字形差异。
    $generatedText = $generatedText.Replace($outputPath, $portableOutputPath)
    # 删除转换器末尾多余空行并保留一个换行，确保生成 C 文件通过 Git 空白检查且跨平台稳定。
    $generatedText = $generatedText.TrimEnd("`r", "`n") + "`n"
    # chineseHeader 说明该大数组为何允许机器生成，并给出重新生成与许可证入口。
    $chineseHeader = @"
/*
 * 自动生成中文界面字体：Noto Sans SC ${fontSize}px、2 bpp 非压缩位图、ASCII 0x20～0x7E 与设备可见中文子集。
 * 输入文案：esp32/firmware/components/ui/ui_presenter.c；生成脚本：tools/generate_lvgl_ui_fonts.ps1。
 * 字体许可：SIL Open Font License 1.1，完整文本见同目录 OFL-1.1.txt。
 * 位图、字形描述和 Unicode 映射均由 lv_font_conv 生成，禁止手工调整数组；修改文案后必须重跑脚本。
 */

"@
    # 以无 BOM UTF-8 重写生成文件；ESP-IDF/GCC 和哈希清单跨平台读取一致。
    [System.IO.File]::WriteAllText(
        $outputPath,
        $chineseHeader + $generatedText,
        [System.Text.UTF8Encoding]::new($false))
    # outputItem 提供最终文件长度，单位字节。
    $outputItem = Get-Item -LiteralPath $outputPath
    # outputHash 计算最终含中文头的 SHA-256，用于审计生成物未被手工修改。
    $outputHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $outputPath).Hash.ToLowerInvariant()
    # 向 manifest 列表加入稳定字段；相对路径便于工作树移动后验证。
    $generatedFonts.Add([ordered]@{
        name = $fontName
        size_px = $fontSize
        bpp = 2
        file = "esp32/firmware/components/ui/fonts/$fontName.c"
        bytes = [int64]$outputItem.Length
        sha256 = $outputHash
    })
}

# sourceHash 记录实际输入 OTF 摘要；不同 Noto 版本重生成时 manifest 会明确变化。
$sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $FontPath).Hash.ToLowerInvariant()
# manifest 按固定字段顺序描述输入、字符集合、授权和四个生成物。
$manifest = [ordered]@{
    schema_version = 1
    generator = 'lv_font_conv via tools/generate_lvgl_ui_fonts.ps1'
    source_font = (Split-Path -Leaf $FontPath)
    source_font_sha256 = $sourceHash
    license = 'SIL Open Font License 1.1'
    license_file = 'esp32/firmware/components/ui/fonts/OFL-1.1.txt'
    ascii_range = '0x20-0x7E'
    chinese_glyph_count = $glyphSet.Count
    chinese_symbols = $symbols
    generated_fonts = $generatedFonts
}
# manifestPath 是注释审计和交付复现共同读取的权威生成清单。
$manifestPath = Join-Path $outputDirectory 'ui_font_manifest.json'
# manifestJson 使用足够深度展开字体对象，并在末尾添加换行便于 Git 文本工具检查。
$manifestJson = ($manifest | ConvertTo-Json -Depth 6) + [Environment]::NewLine
# 以无 BOM UTF-8 写入清单，避免 PowerShell 5.1 默认 UTF-16 破坏跨平台读取。
[System.IO.File]::WriteAllText(
    $manifestPath,
    $manifestJson,
    [System.Text.UTF8Encoding]::new($false))
# 输出机器可解析成功标志、中文字符数和四个字号，供总验证脚本汇总。
Write-Output "LVGL_UI_FONTS_GENERATED glyphs=$($glyphSet.Count) sizes=16,20,28,36"
