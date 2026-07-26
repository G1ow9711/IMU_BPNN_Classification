// 引入路径、目录、文件和流 API；截图器只写调用者指定的正式资产目录。
using System.IO;
// 引入 SHA-256，清单记录每张教程图片的可复核哈希。
using System.Security.Cryptography;
// 引入 UTF-8 编码和字符串构建器，计算排序源码集合的稳定内容哈希。
using System.Text;
// 引入宽松 JSON 编码器，使中文说明直接以 UTF-8 写入清单。
using System.Text.Encodings.Web;
// 引入 JSON 序列化，输出稳定机器可读的图片清单。
using System.Text.Json;
// 引入 WPF 布局、窗口和离屏渲染 API。
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
// 引入正式 WPF 窗口、服务、ViewModel 和页面绑定。
using FitnessCoach.App;
using FitnessCoach.App.Services;
using FitnessCoach.App.ViewModels;
// 引入分类诊断和六轴协议对象。
using FitnessCoach.Bluetooth;
// 引入领域动作、状态和会话摘要。
using FitnessCoach.Domain;

// 文档截图工具使用独立命名空间，避免正式应用误引用教程数据。
namespace FitnessCoach.UiCapture;

/// <summary>使用生产 WPF 视图和确定性 Mock 数据生成无 BLE 副作用的教程截图。</summary>
internal static class Program
{
    // 上位机产品窗口逻辑宽度固定为 1440，输出一逻辑像素对应一 PNG 像素。
    private const int CaptureWidth = 1440;
    // 上位机产品窗口逻辑高度固定为 900。
    private const int CaptureHeight = 900;
    // PNG 采用 96 DPI，避免显示器缩放改变文档资产尺寸。
    private const double CaptureDpi = 96.0;

    /// <summary>创建 STA WPF 环境、生成六页截图并验证文件。</summary>
    [STAThread]
    private static int Main(string[] arguments)
    {
        try
        {
            // 解析唯一可选输出目录；缺省值适合从仓库根执行。
            string outputDirectory = ParseOutputDirectory(arguments);
            // 在同一 STA 线程执行全部异步初始化，避免 WPF 视觉对象跨线程。
            GenerateAsync(outputDirectory).GetAwaiter().GetResult();
            // 输出稳定成功标记，供 CI 和教程维护者核对。
            Console.WriteLine(
                $"PC_UI_CAPTURE_OK pages=6 size={CaptureWidth}x{CaptureHeight} mode=deterministic-mock output={outputDirectory}");
            // 返回零表示全部 PNG 和清单已生成并通过边界检查。
            return 0;
        }
        catch (Exception exception)
        {
            // 把完整异常写到标准错误，调用者不得把半套截图当成成功。
            Console.Error.WriteLine($"PC_UI_CAPTURE_FAILED {exception}");
            // 返回非零退出码阻断文档提交。
            return 1;
        }
    }

    // 解析 --output <directory>，拒绝未知参数和空路径。
    private static string ParseOutputDirectory(string[] arguments)
    {
        // 无参数时使用正式文档资产目录。
        if (arguments.Length == 0)
        {
            // 规范化为绝对路径，后续日志可直接定位。
            return Path.GetFullPath(Path.Combine("docs", "assets", "ui", "pc"));
        }
        // 只接受两个参数，避免拼写错误被静默忽略。
        if ((arguments.Length != 2) ||
            !string.Equals(arguments[0], "--output", StringComparison.Ordinal) ||
            string.IsNullOrWhiteSpace(arguments[1]))
        {
            // 明确给出稳定调用格式。
            throw new ArgumentException("用法：FitnessCoach.UiCapture --output <目录>");
        }
        // 返回调用者指定目录的绝对路径。
        return Path.GetFullPath(arguments[1]);
    }

    // 组合生产页面、填入教程数据并逐页离屏渲染。
    private static async Task GenerateAsync(string outputDirectory)
    {
        // 创建正式资产目录；程序不会在其它位置创建文件。
        Directory.CreateDirectory(outputDirectory);
        // 创建 WPF Application 以提供正式 App.xaml 资源，但不调用 Run 或 OnStartup。
        FitnessCoach.App.App application = new();
        // 加载正式颜色、按钮、DataTemplate 和中文样式。
        application.InitializeComponent();
        // 禁止隐式关闭逻辑介入离屏流程。
        application.ShutdownMode = ShutdownMode.OnExplicitShutdown;

        // 创建完全进程内的确定性 Mock；该类型不引用 Windows BLE transport。
        await using DocumentationMockDeviceSession deviceSession = new();
        // 创建三条固定教程历史。
        IReadOnlyList<TrainingSessionSummary> summaries =
            DocumentationMockDeviceSession.CreateDocumentationSummaries();
        // 创建只驻留内存的会话仓储。
        DocumentationSessionRepository sessionRepository = new(summaries);
        // 创建只驻留内存的用户偏好仓储。
        DocumentationPreferencesStore preferencesStore = new();
        // 初始化内存仓储；不会创建目录或 JSON。
        await sessionRepository.InitializeAsync().ConfigureAwait(true);
        // 使用同步调度器，因为全部事件都在当前 STA 线程发布。
        IUiDispatcher dispatcher = new ImmediateUiDispatcher();
        // 截图固定代表姿态，避免帧定时器造成像素漂移。
        IAnimationPreferences animationPreferences = new AnimationPreferences(systemReducedMotion: true);
        // 使用生产本地矢量动作控制器。
        IActionAnimationController animationController = new LocalActionAnimationController();
        // 创建不会弹窗的保存位置替身。
        DocumentationDestinationPicker destinationPicker = new();
        // 创建设备页；初始状态明确为已连接 Mock。
        DeviceViewModel device = new(deviceSession, dispatcher);
        // 创建实时训练页；计数和动作仍由设备状态事件提供。
        LiveTrainingViewModel live = new(
            deviceSession,
            sessionRepository,
            dispatcher,
            animationController,
            animationPreferences);
        // 创建总结页。
        SummaryViewModel summary = new();
        // 创建历史页并注入真实 CSV 编码器；截图不会执行导出命令。
        HistoryViewModel history = new(
            sessionRepository,
            new HistoryCsvExporter(),
            destinationPicker);
        // 创建设置页并只注入内存偏好；不注入设备以阻止任何配置发送。
        SettingsViewModel settings = new(preferencesStore, animationPreferences);
        // 创建训练监测页；导出器存在但路径选择器始终返回取消。
        DiagnosticsViewModel diagnostics = new(
            deviceSession,
            dispatcher,
            animationPreferences,
            new ImuCsvExporter(),
            destinationPicker);
        // 组合与生产应用完全相同的六页主 ViewModel。
        using MainViewModel main = new(device, live, summary, history, settings, diagnostics);
        // 从内存仓储加载设置和历史。
        await main.InitializeAsync().ConfigureAwait(true);
        // 在全部页面完成事件订阅后连接确定性 Mock，确保设备页展示真实的文档模式连接原因。
        await deviceSession.ConnectAsync().ConfigureAwait(true);
        // 通过正式命令开启诊断流，使 ViewModel 的本地门控与 Mock 会话状态完全同步。
        await diagnostics.ToggleRawStreamCommand.ExecuteAsync().ConfigureAwait(true);
        // 把最新运行态发布给实时页、侧栏和诊断页。
        deviceSession.PublishState(await deviceSession.GetSnapshotAsync().ConfigureAwait(true));
        // 发布十秒 25 Hz 确定性波形和分类事件。
        PublishDeterministicDiagnostics(deviceSession);
        // 给总结页设置最近一次完整会话。
        summary.SetSummary(summaries[0], dailyCalorieGoal: 300.0, devicePersisted: true, localPersisted: true);
        // 历史页选择最近一行，使右侧详情卡包含可读内容。
        await history.RefreshAsync().ConfigureAwait(true);
        // 历史存在时选择第一条；固定数据保证该分支始终执行。
        if (history.Sessions.Count > 0)
        {
            // 选择最近会话以展示详情。
            history.SelectedSession = history.Sessions[0];
        }

        // 创建无标题栏、无任务栏图标的正式主窗口，让 Loaded 事件和 DataGrid 星号列完成生产布局。
        MainWindow window = new()
        {
            // 绑定与正式应用相同的聚合 ViewModel。
            DataContext = main,
            // 窗口外框宽度等于目标 PNG 宽度。
            Width = CaptureWidth,
            // 窗口外框高度等于目标 PNG 高度。
            Height = CaptureHeight,
            // 去掉系统标题栏，内容区与目标尺寸严格一致。
            WindowStyle = WindowStyle.None,
            // 禁止用户尺寸交互，避免系统最小尺寸影响离屏布局。
            ResizeMode = ResizeMode.NoResize,
            // 文档工具不在任务栏留下可点击窗口。
            ShowInTaskbar = false,
            // 离屏加载不抢占当前前台应用焦点。
            ShowActivated = false,
            // 使用手动坐标，不受系统窗口居中策略影响。
            WindowStartupLocation = WindowStartupLocation.Manual,
            // 把加载窗口放到虚拟桌面可见范围之外；像素仍由 RenderTargetBitmap 读取视觉树。
            Left = -32000.0,
            // 纵向坐标同样放到虚拟桌面外。
            Top = -32000.0,
        };
        // 短暂加载离屏窗口，触发生产控件 Loaded、模板和星号列宽计算；不执行正式 App 启动逻辑。
        window.Show();
        // 主窗口内容必须是可布局视觉元素，否则 XAML 结构已经破坏。
        FrameworkElement content = window.Content as FrameworkElement
            ?? throw new InvalidOperationException("主窗口内容不是可渲染的 FrameworkElement。");
        // 保存已生成图片的清单项。
        List<CaptureManifestEntry> entries = new();

        // 设备页是 MainViewModel 初始页，展示明确 Mock 链路和设备摘要。
        entries.Add(CapturePage(content, outputDirectory, "pc-device.png", "设备", "确定性 Mock 设备与链路摘要"));
        // 导航到实时训练页并展示八次深蹲的权威状态。
        main.ShowLiveTrainingCommand.Execute(null);
        // 捕获实时训练页。
        entries.Add(CapturePage(content, outputDirectory, "pc-live-training.png", "实时训练", "深蹲八次与本地矢量动作示范"));
        // 导航到训练监测页，显示十秒六轴波形与双模型诊断。
        main.ShowDiagnosticsCommand.Execute(null);
        // 捕获训练监测页。
        entries.Add(CapturePage(content, outputDirectory, "pc-training-monitor.png", "训练监测", "25 Hz 六轴曲线与分类诊断"));
        // 导航到历史页，展示固定三条教程会话。
        main.ShowHistoryCommand.Execute(null);
        // 再次同步刷新，保证异步命令触发前后的集合一致。
        await history.RefreshAsync().ConfigureAwait(true);
        // 刷新后恢复最近会话选择。
        if (history.Sessions.Count > 0)
        {
            // 设置详情区域绑定对象。
            history.SelectedSession = history.Sessions[0];
        }
        // 捕获历史页。
        entries.Add(CapturePage(content, outputDirectory, "pc-history.png", "历史记录", "三条固定教程会话及详情"));
        // 导航到设置页，展示安全 Mock 配置。
        main.ShowSettingsCommand.Execute(null);
        // 捕获设置页。
        entries.Add(CapturePage(content, outputDirectory, "pc-settings.png", "设置", "公制、亮度、熄屏与减少动画"));
        // 导航到训练总结页；此前已设置固定摘要。
        main.ShowSummaryCommand.Execute(null);
        // 捕获总结页。
        entries.Add(CapturePage(content, outputDirectory, "pc-summary.png", "训练总结", "深蹲会话的双端保存结果"));

        // 关闭未显示的窗口对象，释放生产控件计时器和资源。
        window.Close();
        // 写出稳定机器清单并包含图片 SHA-256。
        WriteManifest(outputDirectory, entries);
        // 显式关闭 WPF Application；未调用 Run 时该操作不触发正式 App.OnStartup。
        application.Shutdown();
    }

    // 生成十秒训练波形、分类诊断和八个权威计数标记。
    private static void PublishDeterministicDiagnostics(DocumentationMockDeviceSession deviceSession)
    {
        // 每个样本间隔 40 ms，对应固件严格 25 Hz 重采样合同。
        const uint intervalMilliseconds = 40;
        // 生成 250 点，即十秒可见窗口。
        for (uint index = 0; index < 250U; index++)
        {
            // 把样本索引转换为秒，后续正弦函数使用 SI 时间轴。
            double seconds = index / 25.0;
            // 深蹲主频设为 0.8 Hz，代表约 1.25 秒一个完整周期。
            double phase = 2.0 * Math.PI * 0.8 * seconds;
            // 横向角速度使用主频正弦，单位度每秒。
            double gxDegreesPerSecond = 38.0 * Math.Sin(phase + 0.35);
            // 纵向角速度表示屈髋主运动，幅度高于其它轴。
            double gyDegreesPerSecond = 118.0 * Math.Sin(phase);
            // 垂直轴角速度加入二次谐波，帮助曲线区别于单轴模板。
            double gzDegreesPerSecond = 24.0 * Math.Sin((2.0 * phase) + 0.6);
            // 横向加速度围绕零小幅变化，单位 g。
            double axG = 0.14 * Math.Sin(phase + 1.1);
            // 纵向加速度反映下蹲和起身，单位 g。
            double ayG = 0.42 * Math.Sin(phase);
            // 垂直加速度围绕重力一变化，并含轻微二次谐波。
            double azG = 1.0 + (0.22 * Math.Cos(phase)) + (0.04 * Math.Sin(2.0 * phase));
            // 量化成正式 RawStream 陀螺仪诊断码，每度每秒对应 16.4 码。
            short gxCode = QuantizeToInt16(gxDegreesPerSecond * 16.4);
            // 量化纵向角速度。
            short gyCode = QuantizeToInt16(gyDegreesPerSecond * 16.4);
            // 量化垂直轴角速度。
            short gzCode = QuantizeToInt16(gzDegreesPerSecond * 16.4);
            // 量化横向加速度，每 g 对应 4096 码。
            short axCode = QuantizeToInt16(axG * 4096.0);
            // 量化纵向加速度。
            short ayCode = QuantizeToInt16(ayG * 4096.0);
            // 量化含重力的垂直加速度。
            short azCode = QuantizeToInt16(azG * 4096.0);
            // 构造固定小端协议已经解码后的领域记录。
            RawImuSampleV1 sample = new(
                SampleIndex: index,
                MonotonicMilliseconds: index * intervalMilliseconds,
                GxRaw: gxCode,
                GyRaw: gyCode,
                GzRaw: gzCode,
                AxRaw: axCode,
                AyRaw: ayCode,
                AzRaw: azCode,
                QualityFlags: 0);
            // 发布到训练监测页内存缓冲。
            deviceSession.PublishRawSample(sample);

            // 每 31 点发布一次与 62 点窗半窗步进一致的分类诊断。
            if ((index >= 61U) && (((index - 61U) % 31U) == 0U))
            {
                // 窗口序号从一开始，保证界面显示自然。
                uint windowSequence = ((index - 61U) / 31U) + 1U;
                // 三路模型都判为深蹲，置信度略有差异。
                InferenceDiagnosticV1 diagnostic = new(
                    DiagnosticVersion: 1,
                    FusedAction: ActionId.Squat,
                    BaseAction: ActionId.Squat,
                    MaskedAction: ActionId.Squat,
                    FusedConfidence: 0.94,
                    BaseConfidence: 0.92,
                    MaskedConfidence: 0.96,
                    QualityFlags: 0,
                    WindowSequence: windowSequence,
                    WindowEndMilliseconds: index * intervalMilliseconds,
                    InferenceMicroseconds: 118000,
                    FailureCount: 0);
                // 发布确定性双模型诊断。
                deviceSession.PublishInference(diagnostic);
            }
        }

        // 八个计数点按完整周期间隔分布，仅用于图表标记。
        for (uint count = 1; count <= 8U; count++)
        {
            // 第一完整动作在 1.25 秒附近结束，后续间隔相同。
            uint eventMilliseconds = count * 1250U;
            // 事件序号和累计值均使用当前次数。
            deviceSession.PublishMetricEvent(eventMilliseconds, count, count);
        }
    }

    // 四舍五入并饱和到 int16，保持与设备诊断量化边界一致。
    private static short QuantizeToInt16(double value)
    {
        // 先按 MidpointAwayFromZero 四舍五入，减少正负半码不对称。
        double rounded = Math.Round(value, MidpointRounding.AwayFromZero);
        // 小于 int16 下限时饱和，避免转换回绕。
        if (rounded < short.MinValue)
        {
            // 返回最小诊断码。
            return short.MinValue;
        }
        // 大于 int16 上限时饱和。
        if (rounded > short.MaxValue)
        {
            // 返回最大诊断码。
            return short.MaxValue;
        }
        // 已通过范围检查，可安全转换为 int16。
        return (short)rounded;
    }

    // 测量生产视觉树并写出一张固定 1440×900 PNG。
    private static CaptureManifestEntry CapturePage(
        FrameworkElement content,
        string outputDirectory,
        string fileName,
        string pageName,
        string description)
    {
        // 排空页面导航、数据绑定和 Loaded 回调，使虚拟化表格获得真实行与列模板。
        content.Dispatcher.Invoke(
            // 空回调仅作为队列栅栏，不修改任何页面状态。
            static () => { },
            // ApplicationIdle 之前会先处理 DataBind、Loaded、Render 等更高优先级工作。
            DispatcherPriority.ApplicationIdle);
        // 清除上一页布局缓存，保证 DataTemplate 按当前 ViewModel 重新选择。
        content.InvalidateMeasure();
        // 按产品窗口逻辑尺寸测量完整视觉树。
        content.Measure(new Size(CaptureWidth, CaptureHeight));
        // 把内容安排到像素原点，不包含 Windows 标题栏和桌面背景。
        content.Arrange(new Rect(0.0, 0.0, CaptureWidth, CaptureHeight));
        // 强制绑定和布局在渲染前完成。
        content.UpdateLayout();
        // 再次排空布局触发的列宽和虚拟化回调，防止 DataGrid 只截到初始窄列。
        content.Dispatcher.Invoke(
            // 空回调形成第二道渲染队列栅栏。
            static () => { },
            // Render 优先级保证当前视觉树已生成绘制数据。
            DispatcherPriority.Render);
        // 最后一遍同步布局，把队列中完成的列宽写入可渲染视觉树。
        content.UpdateLayout();
        // 创建 96 DPI 离屏位图，一逻辑像素对应一个输出像素。
        RenderTargetBitmap bitmap = new(
            CaptureWidth,
            CaptureHeight,
            CaptureDpi,
            CaptureDpi,
            PixelFormats.Pbgra32);
        // 渲染生产 MainWindow 内容，不调用 CopyFromScreen。
        bitmap.Render(content);
        // PNG 编码器无损保留中文、曲线和矢量动作示范。
        PngBitmapEncoder encoder = new();
        // 单页截图只包含一帧。
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        // 组合正式资产绝对路径。
        string outputPath = Path.Combine(outputDirectory, fileName);
        // 覆盖同名旧截图，使脚本具有幂等行为。
        using (FileStream stream = File.Create(outputPath))
        {
            // 写出完整 PNG。
            encoder.Save(stream);
        }
        // 校验 PNG 文件头、尺寸和非空长度。
        ValidatePng(outputPath);
        // 计算文件 SHA-256 进入清单。
        string sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(outputPath))).ToLowerInvariant();
        // 返回稳定清单条目。
        return new CaptureManifestEntry(fileName, pageName, description, CaptureWidth, CaptureHeight, sha256);
    }

    // 校验输出确实是目标尺寸 PNG，防止空白或错误格式进入教程。
    private static void ValidatePng(string filePath)
    {
        // 文件必须存在且至少包含 PNG 头和 IHDR。
        FileInfo file = new(filePath);
        // 过小文件不可能包含 1440×900 完整界面。
        if (!file.Exists || file.Length < 1024)
        {
            // 抛出明确路径，阻断清单生成。
            throw new InvalidDataException($"截图文件缺失或过小：{filePath}");
        }
        // 使用 WPF 解码器读取元数据，不依赖第三方图片库。
        using FileStream stream = File.OpenRead(filePath);
        // 保留像素格式并在加载时关闭文件依赖。
        PngBitmapDecoder decoder = new(stream, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
        // PNG 必须只有至少一帧。
        if (decoder.Frames.Count == 0)
        {
            // 无帧说明编码结果损坏。
            throw new InvalidDataException($"截图没有 PNG 帧：{filePath}");
        }
        // 读取首帧像素尺寸。
        BitmapFrame frame = decoder.Frames[0];
        // 尺寸必须与产品截图合同完全一致。
        if ((frame.PixelWidth != CaptureWidth) || (frame.PixelHeight != CaptureHeight))
        {
            // 报告实际尺寸便于定位 DPI 或布局漂移。
            throw new InvalidDataException(
                $"截图尺寸错误：{filePath}，实际 {frame.PixelWidth}x{frame.PixelHeight}。");
        }
    }

    // 写出包含生成模式、稳定源码内容哈希和图片哈希的 JSON。
    private static void WriteManifest(string outputDirectory, IReadOnlyList<CaptureManifestEntry> entries)
    {
        // 从构建产物路径向上定位仓库根，避免依赖调用者当前工作目录。
        string repositoryRoot = FindRepositoryRoot();
        // 对截图器和正式 WPF 视图源码做排序内容哈希，不读取 Git HEAD 或时间戳。
        string sourceFilesSha256 = CalculateSourceFilesSha256(repositoryRoot);
        // 构造机器可读清单；不写当前时间以保持重复生成稳定。
        CaptureManifest manifest = new(
            SchemaVersion: 1,
            Generator: "pc/FitnessCoach.UiCapture",
            GeneratorVersion: "1.0",
            SourceFilesSha256: sourceFilesSha256,
            CaptureMode: "deterministic-mock-render-target-bitmap",
            ReadsLocalApplicationData: false,
            UsesBluetooth: false,
            Images: entries);
        // 使用缩进和中文直出 JSON，方便教程维护者直接审阅。
        JsonSerializerOptions options = new()
        {
            // 每级使用两个空格，清单差异保持紧凑稳定。
            WriteIndented = true,
            // 中文保持 UTF-8 原文，不转成难读的 \u 转义。
            Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        };
        // 序列化成 UTF-8 文本。
        string json = JsonSerializer.Serialize(manifest, options);
        // 写入正式清单并固定 LF 结尾。
        File.WriteAllText(Path.Combine(outputDirectory, "manifest.json"), json + "\n");
    }

    // 从已构建截图器所在目录向上查找产品仓库根。
    private static string FindRepositoryRoot()
    {
        // 起点使用应用基目录，发布或测试从任意当前目录启动都能工作。
        DirectoryInfo? directory = new(AppContext.BaseDirectory);
        // 每轮检查当前目录，直到磁盘根目录终止。
        while (directory is not null)
        {
            // 正式上位机项目文件必须存在。
            string appProject = Path.Combine(
                directory.FullName,
                "pc",
                "FitnessCoach.App",
                "FitnessCoach.App.csproj");
            // 截图器项目文件也必须存在，防止命中同名父目录。
            string captureProject = Path.Combine(
                directory.FullName,
                "pc",
                "FitnessCoach.UiCapture",
                "FitnessCoach.UiCapture.csproj");
            // 两个项目同时存在才把当前目录视为仓库根。
            if (File.Exists(appProject) && File.Exists(captureProject))
            {
                // 返回规范绝对路径供源码枚举使用。
                return directory.FullName;
            }
            // 向父目录继续查找；磁盘根的 Parent 为 null。
            directory = directory.Parent;
        }
        // 无仓库源码时拒绝生成不可溯源清单。
        throw new DirectoryNotFoundException("无法从截图器构建目录定位仓库根。");
    }

    // 对截图器与正式 WPF 应用源码生成与 Git 状态无关的稳定集合哈希。
    private static string CalculateSourceFilesSha256(string repositoryRoot)
    {
        // 两个目录分别覆盖截图数据/流程和实际生产视图、样式、ViewModel。
        string[] sourceDirectories =
        {
            // 截图器自身源码决定确定性 Mock、页面状态和输出合同。
            Path.Combine(repositoryRoot, "pc", "FitnessCoach.UiCapture"),
            // 正式应用源码决定 XAML、控件、主题和页面绑定。
            Path.Combine(repositoryRoot, "pc", "FitnessCoach.App"),
        };
        // 收集参与哈希的源码绝对路径。
        List<string> sourceFiles = new();
        // 逐个目录递归枚举，目录必须真实存在。
        foreach (string sourceDirectory in sourceDirectories)
        {
            // 构建前缺少任一源目录都应阻断清单。
            if (!Directory.Exists(sourceDirectory))
            {
                // 报告精确目录，便于修复发布布局。
                throw new DirectoryNotFoundException($"源码目录不存在：{sourceDirectory}");
            }
            // 遍历目录下全部文件，再按扩展名和构建目录过滤。
            foreach (string filePath in Directory.EnumerateFiles(
                sourceDirectory,
                "*",
                SearchOption.AllDirectories))
            {
                // 转换为仓库相对路径，统一后续排序和排除判断。
                string relativePath = Path.GetRelativePath(repositoryRoot, filePath);
                // bin/obj 属于派生构建物，不能进入源码内容哈希。
                string[] pathSegments = relativePath.Split(
                    Path.DirectorySeparatorChar,
                    Path.AltDirectorySeparatorChar);
                // 任一目录段命中 bin 或 obj 时跳过当前文件。
                if (pathSegments.Any(segment =>
                        string.Equals(segment, "bin", StringComparison.OrdinalIgnoreCase) ||
                        string.Equals(segment, "obj", StringComparison.OrdinalIgnoreCase)))
                {
                    // 继续检查下一个文件。
                    continue;
                }
                // 读取小写扩展名，平台差异不会改变筛选结果。
                string extension = Path.GetExtension(filePath).ToLowerInvariant();
                // 只纳入 C#、XAML 和项目合同文件。
                if ((extension == ".cs") || (extension == ".xaml") || (extension == ".csproj"))
                {
                    // 记录源码绝对路径，稍后按规范相对路径排序。
                    sourceFiles.Add(filePath);
                }
            }
        }
        // 至少应包含两个项目文件和生产视图；空集合说明路径规则失效。
        if (sourceFiles.Count < 3)
        {
            // 阻断发布无法代表真实界面的截图。
            throw new InvalidDataException("参与截图溯源的源码文件数量异常。");
        }
        // 按统一斜杠的仓库相对路径排序，消除文件系统枚举顺序差异。
        sourceFiles.Sort((left, right) =>
            string.CompareOrdinal(
                NormalizeRelativePath(repositoryRoot, left),
                NormalizeRelativePath(repositoryRoot, right)));
        // 保存“相对路径 + 单文件摘要”的规范文本。
        StringBuilder canonicalSourceList = new();
        // 逐文件计算摘要并追加固定 LF 分隔。
        foreach (string filePath in sourceFiles)
        {
            // 相对路径纳入哈希，重命名也会改变集合指纹。
            string relativePath = NormalizeRelativePath(repositoryRoot, filePath);
            // 文件内容使用 SHA-256，避免把大段源码拼进内存。
            string fileSha256 = Convert.ToHexString(
                SHA256.HashData(File.ReadAllBytes(filePath))).ToLowerInvariant();
            // 写入路径、制表符、摘要和 LF，格式跨平台稳定。
            canonicalSourceList
                .Append(relativePath)
                .Append('\t')
                .Append(fileSha256)
                .Append('\n');
        }
        // 对规范清单再次计算 SHA-256，得到单一源码集合指纹。
        return Convert.ToHexString(
            SHA256.HashData(Encoding.UTF8.GetBytes(canonicalSourceList.ToString())))
            .ToLowerInvariant();
    }

    // 把绝对源码路径转换为使用正斜杠的仓库相对路径。
    private static string NormalizeRelativePath(string repositoryRoot, string filePath)
    {
        // Path.GetRelativePath 处理盘符和目录分隔，Replace 固定 manifest 语义。
        return Path.GetRelativePath(repositoryRoot, filePath).Replace('\\', '/');
    }

    // 描述一张截图的稳定字段。
    private sealed record CaptureManifestEntry(
        string File,
        string Page,
        string Description,
        int Width,
        int Height,
        string Sha256);

    // 描述 PC 截图集及其无硬件副作用边界。
    private sealed record CaptureManifest(
        int SchemaVersion,
        string Generator,
        string GeneratorVersion,
        string SourceFilesSha256,
        string CaptureMode,
        bool ReadsLocalApplicationData,
        bool UsesBluetooth,
        IReadOnlyList<CaptureManifestEntry> Images);
}
