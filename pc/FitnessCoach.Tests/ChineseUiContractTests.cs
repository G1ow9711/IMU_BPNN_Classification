// 引入正则表达式，用于只检查 XAML 中真正显示给用户的静态属性。
using System.Text.RegularExpressions;
// 引入应用视图模型，验证运行时生成的动态状态文案。
using FitnessCoach.App.ViewModels;
// 引入同步测试调度器，让后台设备事件立即更新视图模型。
using FitnessCoach.App.Services;
// 引入模拟设备与配置接口，测试无需真实开发板即可稳定复现。
using FitnessCoach.Bluetooth;
// 引入领域枚举，构造模拟训练动作。
using FitnessCoach.Domain;

// 中文界面合同测试位于独立测试命名空间，避免污染正式应用程序集。
namespace FitnessCoach.Tests;

/// <summary>验证全部用户可见静态界面和关键动态文案均使用自然中文。</summary>
internal static partial class ChineseUiContractTests
{
    // 匹配 XAML 中会直接显示给用户的标题、正文、按钮、表头和提示属性。
    [GeneratedRegex("(?:Title|Text|Content|Header|ToolTip)\\s*=\\s*\"([^\"]*)\"", RegexOptions.CultureInvariant)]
    private static partial Regex VisibleXamlAttributeRegex();

    // 匹配任意英文字母；绑定表达式和设备实际返回值由调用处排除。
    [GeneratedRegex("[A-Za-z]", RegexOptions.CultureInvariant)]
    private static partial Regex LatinLetterRegex();

    /// <summary>顺序执行静态 XAML 与动态视图模型中文化检查。</summary>
    public static async Task RunAllAsync()
    {
        // 先检查编译前 XAML，阻止新英文标题、标签或按钮进入界面。
        TestStaticXamlUsesChineseText();
        // 检查阶段二训练监测页保持单屏动作、指标和双六轴曲线。
        TestTrainingDashboardUsesSinglePageLayout();
        // 再检查运行时动态文案，覆盖设备、诊断和原始诊断流状态。
        await TestDynamicViewModelTextAsync();
    }

    // 检查训练监测页的单屏信息架构，防止后续又退回长说明和纵向滚动。
    private static void TestTrainingDashboardUsesSinglePageLayout()
    {
        // 定位仓库根，读取真实人工维护的诊断页 XAML。
        string repositoryRoot = FindRepositoryRoot();
        // diagnosticsPath 指向阶段二单页训练监测视图。
        string diagnosticsPath = Path.Combine(repositoryRoot, "pc", "FitnessCoach.App", "Views", "DiagnosticsView.xaml");
        // 读取 UTF-8 XAML，静态合同无需启动 WPF 窗口。
        string xaml = File.ReadAllText(diagnosticsPath);
        // 训练页不得再使用纵向滚动容器，否则 1440×900 不能一眼看到全部关键事实。
        Assert(!xaml.Contains("<ScrollViewer", StringComparison.Ordinal), "训练监测页仍依赖纵向滚动。 ");
        // 标准动作必须使用本地矢量控件并绑定设备稳定动作。
        Assert(
            xaml.Contains("<controls:ActionStickFigure", StringComparison.Ordinal) &&
            xaml.Contains("Action=\"{Binding CurrentAction}\"", StringComparison.Ordinal) &&
            xaml.Contains("Text=\"{Binding StableActionCueText}\"", StringComparison.Ordinal) &&
            xaml.Contains("Text=\"右手腕佩戴\"", StringComparison.Ordinal),
            "训练监测页缺少随稳定分类更新的标准动作示范。 ");
        // 两张曲线必须同屏存在，且分别绑定加速度和角速度点集。
        Assert(
            xaml.Contains("Points=\"{Binding AccelerationPoints}\"", StringComparison.Ordinal) &&
            xaml.Contains("Points=\"{Binding GyroscopePoints}\"", StringComparison.Ordinal),
            "训练监测页没有同屏显示加速度与角速度曲线。 ");
        // 曲线必须提供过去窗口滑块和回到实时命令，不能只能看最新十秒。
        Assert(
            xaml.Contains("Maximum=\"{Binding RawChartMaximumOffsetSeconds}\"", StringComparison.Ordinal) &&
            xaml.Contains("Value=\"{Binding RawChartOffsetSeconds", StringComparison.Ordinal) &&
            xaml.Contains("Command=\"{Binding GoLiveRawChartCommand}\"", StringComparison.Ordinal),
            "训练监测页缺少曲线历史滑块或回到实时入口。 ");
        // 次数、时长和热量必须直接绑定设备权威状态，不能用长说明替代。
        Assert(
            xaml.Contains("Text=\"{Binding MetricValue}\"", StringComparison.Ordinal) &&
            xaml.Contains("Text=\"{Binding DurationText}\"", StringComparison.Ordinal) &&
            xaml.Contains("Text=\"{Binding CaloriesText}\"", StringComparison.Ordinal),
            "训练监测页缺少次数、时长或热量关键指标。 ");
    }

    // 检查应用源目录下所有 XAML 静态可见属性。
    private static void TestStaticXamlUsesChineseText()
    {
        // 从测试输出目录向上寻找仓库根，避免依赖调用者当前工作目录。
        string repositoryRoot = FindRepositoryRoot();
        // 应用 XAML 根目录包含主窗口和全部页面。
        string appDirectory = Path.Combine(repositoryRoot, "pc", "FitnessCoach.App");
        // 枚举真实源文件；bin 和 obj 中的生成副本不属于人工交付合同。
        IEnumerable<string> xamlFiles = Directory.EnumerateFiles(appDirectory, "*.xaml", SearchOption.AllDirectories)
            .Where(path => !path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}", StringComparison.OrdinalIgnoreCase))
            .Where(path => !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}", StringComparison.OrdinalIgnoreCase));

        // 逐个文件检查标题、正文、按钮和表头的静态文本。
        foreach (string xamlFile in xamlFiles)
        {
            // 使用 UTF-8 读取完整 XAML，保留中文原文用于错误定位。
            string xaml = File.ReadAllText(xamlFile);
            // 遍历每个可见属性值；绑定表达式运行时由下一项测试覆盖。
            foreach (Match match in VisibleXamlAttributeRegex().Matches(xaml))
            {
                // 第一个捕获组是属性的实际显示值。
                string visibleText = match.Groups[1].Value;
                // 绑定、静态资源和模板表达式以左花括号开头，不是静态用户文案。
                if (visibleText.StartsWith('{'))
                {
                    // 跳过运行时表达式，继续检查下一个静态值。
                    continue;
                }

                // 静态可见文案不得夹带英文缩写；技术值应放在绑定数据区而非标签中。
                Assert(
                    !LatinLetterRegex().IsMatch(visibleText),
                    $"用户可见 XAML 文案仍含英文：{Path.GetRelativePath(repositoryRoot, xamlFile)} -> {visibleText}");
            }
        }
    }

    // 检查设备页和诊断页由代码动态生成的关键文本。
    private static async Task TestDynamicViewModelTextAsync()
    {
        // 使用快速模拟设备产生确定性的电量、链路和诊断数据。
        await using MockDeviceSession device = new(TimeSpan.FromMilliseconds(5), [ActionId.Squat]);
        // 同步调度器让后台事件在测试线程立即落入视图模型。
        ImmediateUiDispatcher dispatcher = new();
        // 创建设备页动态文本来源。
        using DeviceViewModel deviceViewModel = new(device, dispatcher);
        // 创建诊断页动态文本来源。
        using DiagnosticsViewModel diagnosticsViewModel = new(device, dispatcher);

        // 核对尚未连接时的说明已使用完整中文术语。
        AssertNoLatin(deviceViewModel.DeviceMode, "设备模式");
        // 真机尚未选择时，界面不得直接暴露 BLE-UNSELECTED 内部占位值。
        Assert(
            DisplayText.DeviceIdentifier("BLE-UNSELECTED") == "未选择蓝牙设备",
            "未选择真机的设备标识没有转换为中文占位文本。");
        // Mock 内部设备号也应转换为自然中文，避免普通体验页出现英文调试标识。
        Assert(
            DisplayText.DeviceIdentifier("MOCK-ESP32S3-0001") == "本机模拟设备",
            "模拟设备标识没有转换为中文占位文本。");
        // 已连接真机返回的真实产品标识必须原样保留，便于售后和诊断定位。
        Assert(
            DisplayText.DeviceIdentifier("BPNN-FIT-A1B2") == "BPNN-FIT-A1B2",
            "真实设备标识被错误本地化或改写。");
        // 原配电池容量和低电策略必须使用中文单位。
        AssertNoLatin(deviceViewModel.PowerContract, "电源合同");
        // 协议说明用中文名称表达，协议数值可以保留阿拉伯数字。
        AssertNoLatin(diagnosticsViewModel.ProtocolText, "协议说明");
        // 模拟或真机实现说明不得把内部类名暴露给普通用户。
        AssertNoLatin(diagnosticsViewModel.DeviceImplementationText, "设备实现说明");
        // 模型职责使用用户可懂的中文名称，不展示内部模型代号。
        AssertNoLatin(diagnosticsViewModel.ModelText, "模型说明");
        // 动画说明不得暴露桌面框架缩写。
        AssertNoLatin(diagnosticsViewModel.AnimationText, "动画说明");

        // 建立模拟链路以刷新信号强度、协议错误和设备能力摘要。
        await deviceViewModel.ConnectCommand.ExecuteAsync();
        // 主动刷新不发起额外设备读的诊断快照。
        diagnosticsViewModel.Refresh();
        // 信号强度单位应为中文“分贝毫瓦”。
        Assert(!LatinLetterRegex().IsMatch(diagnosticsViewModel.RssiText), "信号强度仍含英文单位。");
        // 协议错误摘要应使用中文错误名称。
        AssertNoLatin(diagnosticsViewModel.ProtocolErrorText, "协议错误摘要");
        // 能力清单前缀和文件系统名称必须使用中文。
        Assert(
            diagnosticsViewModel.ManifestSummaryText.StartsWith("设备能力清单", StringComparison.Ordinal) &&
            !diagnosticsViewModel.ManifestSummaryText.Contains("Manifest", StringComparison.Ordinal) &&
            !diagnosticsViewModel.ManifestSummaryText.Contains("LittleFS", StringComparison.Ordinal),
            "设备能力摘要未完成中文化。");
    }

    // 从测试输出目录向父级定位包含 pc/FitnessCoach.App 的仓库根。
    private static string FindRepositoryRoot()
    {
        // 测试程序集目录在 bin/Release 下，从这里向上搜索最稳定。
        DirectoryInfo? current = new(AppContext.BaseDirectory);
        // 最多遍历到磁盘根；每轮移动一个父目录。
        while (current is not null)
        {
            // 主窗口文件存在说明当前目录就是仓库根。
            string marker = Path.Combine(current.FullName, "pc", "FitnessCoach.App", "MainWindow.xaml");
            // 文件存在时返回已解析的绝对路径。
            if (File.Exists(marker))
            {
                // 返回仓库根供后续相对路径和源文件扫描使用。
                return current.FullName;
            }

            // 未命中时继续向父目录移动。
            current = current.Parent;
        }

        // 找不到源目录说明测试运行包不完整，不能跳过中文界面合同。
        throw new DirectoryNotFoundException("无法定位包含上位机 XAML 的仓库根目录。");
    }

    // 断言指定用户文本不含英文字母。
    private static void AssertNoLatin(string text, string fieldName)
    {
        // 字段必须有实际内容，空白同样不能作为合格中文化结果。
        Assert(!string.IsNullOrWhiteSpace(text), $"{fieldName}为空。");
        // 用户说明不得夹带英文缩写、类名或单位。
        Assert(!LatinLetterRegex().IsMatch(text), $"{fieldName}仍含英文：{text}");
    }

    // 统一断言失败行为，保持控制台测试器无第三方依赖。
    private static void Assert(bool condition, string message)
    {
        // 条件成立时测试通过并继续。
        if (condition)
        {
            // 不创建异常，降低成功路径噪声。
            return;
        }

        // 条件不成立时抛出明确业务错误，由主测试入口转换为非零退出码。
        throw new InvalidOperationException(message);
    }
}
