// 引入设备会话和诊断快照接口。
using FitnessCoach.Domain;
// 引入路径规范化 API，导出成功状态显示用户选择的绝对 CSV 路径。
using System.IO;
// 引入协议常量。
using FitnessCoach.Bluetooth;
// 引入 MVVM 通知基类。
using FitnessCoach.App.Mvvm;
// 引入 UI 调度接口，后台 BLE 事件不能直接更新 WPF 属性。
using FitnessCoach.App.Services;
// 引入六轴折线不可变数据点，ViewModel 不直接执行 WPF 绘图。
using FitnessCoach.App.Controls;

// 诊断页 ViewModel 位于应用命名空间。
namespace FitnessCoach.App.ViewModels;

/// <summary>汇总信号强度、最大传输单元、版本、状态修订号、重连与协议错误计数。</summary>
public sealed class DiagnosticsViewModel : ObservableObject, IDisposable
{
    // 每个可见曲线窗口固定包含 250 个 25 赫兹样本，对应十秒现场观察范围。
    private const int RawChartWindowCapacity = 250;
    // 进程内历史最多保留 15000 个同步样本，对应十分钟且不写入磁盘。
    private const int RawChartHistoryCapacity = 15000;
    // 历史满时批量移除 250 个最旧点，避免每个新点都移动整个 List。
    private const int RawChartTrimBatch = 250;
    // 曲线时间换算固定使用设备重采样频率 25 Hz，与固件 RawStream 合同一致。
    private const double RawChartSamplesPerSecond = 25.0;
    // QMI8658 诊断陀螺仪定点比例；诊断码除以 16.4 得到度每秒。
    private const double GyroscopeCodesPerDegreePerSecond = 16.4;
    // QMI8658 诊断加速度定点比例；诊断码除以 4096 得到重力加速度倍数 g。
    private const double AccelerationCodesPerG = 4096.0;
    // 十分钟 25 Hz 六轴最多对应约 1250 个 12 点步进分类窗，额外预留 50 个边界窗口。
    private const int InferenceHistoryCapacity = 1300;
    // 十分钟现场测试最多保留 2000 个权威次数或步数事件，超过时丢弃最旧事件。
    private const int MetricEventHistoryCapacity = 2000;
    // 保存设备会话用于动态读取链路状态。
    private readonly IDeviceSession _deviceSession;
    // 保存 UI 调度器。
    private readonly IUiDispatcher _dispatcher;
    // 保存可选 RawStream 能力；普通会话未实现时诊断页明确显示不可用。
    private readonly IRawStreamSource? _rawStreamSource;
    // 保存可选低延迟协议事件源；计数标记必须来自设备 EventV1，不能由 PC 累计差猜测。
    private readonly IDeviceProtocolEventSource? _protocolEventSource;
    // 保存系统与用户合并后的减少动画偏好，标准动作示范必须遵守辅助功能设置。
    private readonly IAnimationPreferences _animationPreferences;
    // 保存可选 IMU CSV 导出器；正式应用注入真实实现，旧测试可不启用文件能力。
    private readonly IImuCsvExporter? _imuCsvExporter;
    // 保存可选系统路径选择器；用户确认位置前 ViewModel 不得自行写盘。
    private readonly IImuExportDestinationPicker? _imuExportDestinationPicker;
    // 当前诊断日志。
    private string _diagnosticLog = "诊断页已初始化。";
    // 最近蓝牙信号强度文本。
    private string _rssiText = "未知";
    // 当前蓝牙最大传输单元文本。
    private string _attMtuText = "未连接";
    // 设备版本汇总。
    private string _deviceVersionText = "型号/硬件/固件：未知";
    // 最近权威状态修订号文本。
    private string _stateRevisionText = "0";
    // 自动重连次数文本。
    private string _reconnectCountText = "0";
    // 循环冗余校验和分片错误文本。
    private string _protocolErrorText = "校验错误=0；分片错误=0";
    // 最近电量文本。
    private string _batteryText = "未知";
    // 已通过兼容检查的模型、能力和内部文件系统摘要。
    private string _manifestSummaryText = "设备能力清单：未知";
    // RawStream 当前设备确认状态；默认关闭以避免隐私和带宽风险。
    private bool _rawStreamEnabled;
    // RawStream 用户可见状态，区分未连接、未授权、开启和失败。
    private string _rawStreamStatus = "开发者诊断六轴流已关闭；不会保存到磁盘。";
    // 最近一个合法诊断六轴流样本的 25 赫兹同步诊断码摘要。
    private string _latestRawSampleText = "尚未接收原始样本。";
    // 本次开启期间收到的合法样本数；关闭时保留计数供诊断查看。
    private uint _rawSampleCount;
    // 保存最近十分钟加速度物理量，点顺序固定 X、Y、Z，单位 g，仅驻留进程内存。
    private readonly List<ImuPlotPoint> _accelerationBuffer = new(RawChartHistoryCapacity);
    // 保存最近十分钟角速度物理量，点顺序固定 X、Y、Z，单位度每秒，仅驻留进程内存。
    private readonly List<ImuPlotPoint> _gyroscopeBuffer = new(RawChartHistoryCapacity);
    // 保存与两张曲线严格对齐的完整导出记录，包含原始码、物理量、设备时间、PC 时间和质量位。
    private readonly List<ImuExportSample> _imuExportBuffer = new(RawChartHistoryCapacity);
    // 保存类型九分类窗口历史；导出时按同一会话内窗口结束设备时间向前关联到 IMU 行。
    private readonly List<InferenceExportSample> _inferenceExportBuffer = new(InferenceHistoryCapacity);
    // 保存设备权威次数或步数事件；导出时只在精确 MetricEvent 设备时刻标记对应 IMU 行。
    private readonly List<MetricEventExportSample> _metricEventExportBuffer = new(MetricEventHistoryCapacity);
    // 保存最近 26 个设备单调时间，用于计算最多一秒窗口内的实际采样率。
    private readonly Queue<uint> _rawRateTimestamps = new(26);
    // 发布给 WPF 的不可变加速度快照；每个样本最多包含 250 个点。
    private IReadOnlyList<ImuPlotPoint> _accelerationPoints = Array.Empty<ImuPlotPoint>();
    // 发布给 WPF 的不可变角速度快照；每个样本最多包含 250 个点。
    private IReadOnlyList<ImuPlotPoint> _gyroscopePoints = Array.Empty<ImuPlotPoint>();
    // true 表示继续接收和计数样本，但冻结两张曲线便于现场观察。
    private bool _rawChartPaused;
    // 当前可见窗口末端距离最新样本的秒数；零表示实时，正值表示回看过去。
    private double _rawChartOffsetSeconds;
    // 当前历史能支持的最大回看秒数；不足十秒时为零。
    private double _rawChartMaximumOffsetSeconds;
    // 当前可见十秒窗口的人类可读范围，例如实时或回看 12.0 秒前。
    private string _rawChartWindowText = "实时 · 等待样本";
    // 当前进程内历史长度说明；明确最多十分钟且不落盘。
    private string _rawChartHistoryText = "已缓存 00:00 / 最长 10:00";
    // IMU 导出结果说明；默认提示实时导出全部缓存、回看导出当前十秒窗口。
    private string _imuExportStatus = "实时导出全部缓存；暂停或回看时导出当前窗口。";
    // 最近物理量文本使用 g 和度每秒，便于直接判断 QMI8658 是否有数据。
    private string _latestPhysicalValuesText = "尚未接收物理量样本。";
    // 最近一秒窗口估算的 RawStream 实际采样率。
    private string _rawSampleRateText = "采样率：等待样本";
    // 样本序号、时间戳和设备质量位的综合诊断结果。
    private string _rawQualityText = "数据质量：等待样本";
    // 最近融合模型的中文动作；分类诊断尚未到达时保持等待状态。
    private string _fusedActionText = "等待分类";
    // 最近融合模型 Top-1 概率百分比文本。
    private string _fusedConfidenceText = "--";
    // 最近基础 M0 的中文动作和置信度摘要。
    private string _baseModelText = "基础模型：等待窗口";
    // 最近掩码 M0 的中文动作和置信度摘要。
    private string _maskedModelText = "掩码模型：等待窗口";
    // 三路动作是否一致的现场解释。
    private string _modelAgreementText = "模型一致性：等待窗口";
    // 最近窗口序号、单调时间和端到端推理耗时。
    private string _inferenceTimingText = "推理性能：等待窗口";
    // 最近分类窗口质量位与累计失败数。
    private string _inferenceQualityText = "分类质量：等待窗口";
    // 保存设备权威稳定动作；候选切换尚未连续确认时保持旧动作，避免示范抖动。
    private ActionId _currentAction = ActionId.Unknown;
    // 动作或设备状态改变时递增，通知矢量示范从新动作周期起点播放。
    private uint _animationRevision;
    // true 表示当前真机或模拟会话已连接，断线时示范控件隐藏旧动作。
    private bool _isAnimationConnected;
    // 保存当前设备状态中文文本，例如准备、运行或暂停。
    private string _deviceStateText = "未连接";
    // 保存设备权威累计值，单位由 MetricUnitText 决定。
    private uint _metricValue;
    // 保存当前累计单位：次、步或秒。
    private string _metricUnitText = "次";
    // 保存格式化训练时长，使用设备单调毫秒而非 PC 墙上时间。
    private string _durationText = "00:00:00";
    // 保存设备权威累计活动热量显示文本。
    private string _caloriesText = "0.00 千卡";
    // 保存当前有效减少动画状态。
    private bool _reducedMotion;
    // 保存最近设备权威状态；新 IMU 和分类记录用其会话序号阻止跨会话关联。
    private LiveState? _latestLiveState;
    // 上一个合法样本序号，用于检测 BLE 传输或设备发布丢样。
    private uint? _lastRawSampleIndex;
    // 上一个合法设备单调毫秒，用于检测时间倒退或重复样本。
    private uint? _lastRawMonotonicMilliseconds;
    // 是否已解除事件订阅。
    private bool _disposed;

    /// <summary>创建诊断页并订阅链路/状态事件。</summary>
    public DiagnosticsViewModel(
        IDeviceSession deviceSession,
        IUiDispatcher dispatcher,
        IAnimationPreferences? animationPreferences = null,
        IImuCsvExporter? imuCsvExporter = null,
        IImuExportDestinationPicker? imuExportDestinationPicker = null)
    {
        // 设备会话不能为空。
        ArgumentNullException.ThrowIfNull(deviceSession);
        // UI 调度器不能为空。
        ArgumentNullException.ThrowIfNull(dispatcher);
        // 保存设备会话。
        _deviceSession = deviceSession;
        // 保存 UI 调度器。
        _dispatcher = dispatcher;
        // 正式应用注入共享偏好；旧测试或独立预览未注入时使用“不减少动画”的本地默认值。
        _animationPreferences = animationPreferences ?? new AnimationPreferences(false);
        // 保存可选导出器；未注入时按钮会给出明确不可用状态而不是静默失败。
        _imuCsvExporter = imuCsvExporter;
        // 保存可选路径选择器；文件写入必须经过该用户授权边界。
        _imuExportDestinationPicker = imuExportDestinationPicker;
        // 读取当前有效减少动画状态。
        _reducedMotion = _animationPreferences.IsReducedMotionEnabled;
        // 尝试取得可选 RawStream 接口；没有该接口时按钮仍给出明确错误。
        _rawStreamSource = deviceSession as IRawStreamSource;
        // 尝试取得低延迟 EventV1 接口；没有接口时 CSV 仍导出 IMU 与分类，但计数标记为空。
        _protocolEventSource = deviceSession as IDeviceProtocolEventSource;
        // 初始化示范连接态，应用启动时已连接的 Mock/真机会立即显示正确等待状态。
        _isAnimationConnected = deviceSession.IsConnected;
        // 创建串行 RawStream 开关命令，运行中自动禁用防止并发控制帧。
        ToggleRawStreamCommand = new AsyncRelayCommand(ToggleRawStreamAsync);
        // 创建暂停命令；暂停只冻结曲线，不停止 BLE 接收和质量统计。
        PauseRawChartCommand = new RelayCommand(_ => ToggleRawChartPause());
        // 创建清空命令；仅清空内存曲线和采样率窗口，不向设备发送命令。
        ClearRawChartCommand = new RelayCommand(_ => ClearRawChart());
        // 创建回到实时命令；解除暂停并把视口移动到最新十秒。
        GoLiveRawChartCommand = new RelayCommand(_ => GoLiveRawChart());
        // 创建 IMU 导出命令；执行时冻结样本快照，文件 I/O 不占用 BLE 回调线程。
        ExportImuCsvCommand = new AsyncRelayCommand(ExportImuCsvAsync);
        // 连接变化时刷新 MTU、重连和版本。
        _deviceSession.ConnectionChanged += OnConnectionChanged;
        // 状态变化时刷新电量和 revision。
        _deviceSession.StateChanged += OnStateChanged;
        // 用户或系统减少动画设置变化时立即停止或恢复标准动作循环。
        _animationPreferences.Changed += OnAnimationPreferencesChanged;
        // 仅在会话提供 RawStream 能力时订阅原始样本事件。
        if (_rawStreamSource is not null)
        {
            // 样本可能从 BLE 后台线程到达，处理函数负责切换 UI 线程。
            _rawStreamSource.RawSampleReceived += OnRawSampleReceived;
            // 分类诊断与六轴样本共享后台通知泵，处理函数同样切换 UI 线程。
            _rawStreamSource.InferenceDiagnosticReceived += OnInferenceDiagnosticReceived;
        }
        // 会话提供 EventV1 能力时订阅权威计数事件；事件处理同样切换到 UI 线程维护单线程缓存。
        if (_protocolEventSource is not null)
        {
            // 计数事件可能从 BLE 后台通知泵到达，处理函数只捕获不可变字段并调度。
            _protocolEventSource.ProtocolEventReceived += OnProtocolEventReceived;
        }
        // 应用初始快照。
        Refresh();
    }

    /// <summary>显式开启或关闭开发者 RawStream 的异步命令。</summary>
    public AsyncRelayCommand ToggleRawStreamCommand { get; }

    /// <summary>暂停或继续两张实时曲线，不改变设备 RawStream 状态。</summary>
    public RelayCommand PauseRawChartCommand { get; }

    /// <summary>清空当前十秒内存窗口，不影响 BLE 数据接收。</summary>
    public RelayCommand ClearRawChartCommand { get; }

    /// <summary>解除历史浏览并回到最新十秒窗口。</summary>
    public RelayCommand GoLiveRawChartCommand { get; }

    /// <summary>导出实时全部缓存或当前回看窗口的中文 IMU CSV。</summary>
    public AsyncRelayCommand ExportImuCsvCommand { get; }

    /// <summary>协议版本文本。</summary>
    public string ProtocolText => $"蓝牙低功耗逻辑协议版本 {ProtocolConstants.ProtocolMajor}.{ProtocolConstants.ProtocolMinor}；采用十六位循环冗余校验；支持最大传输单元分片重组。";

    /// <summary>设备实现状态。</summary>
    public string DeviceImplementationText => _deviceSession.IsHardwareBacked
        ? "当前使用真机蓝牙会话；实际射频、功耗和稳定性仍需开发板烧录验收。"
        : "当前使用模拟设备会话；信号强度、版本和错误计数均为模拟诊断值。";

    /// <summary>产品固定电源策略。</summary>
    public string PowerPolicyText => "原配电池容量 400 毫安时；15% 告警、8% 禁止新会话、5% 保存并关机。";

    /// <summary>模型和推理说明。</summary>
    public string ModelText => "设备端运行两个轻量六分支网络和 297 维特征；上位机只显示设备权威动作和累计值，不重复推理。";

    /// <summary>本地动画说明。</summary>
    public string AnimationText => "动作动画使用 11 类本地矢量骨架；断线或未知时显示等待态，并遵循系统减少动画设置。";

    /// <summary>最近扫描信号强度，单位分贝毫瓦。</summary>
    public string RssiText
    {
        // 返回 RSSI 文本。
        get => _rssiText;
        // 内部应用诊断快照。
        private set => SetProperty(ref _rssiText, value);
    }

    /// <summary>当前蓝牙最大传输单元。</summary>
    public string AttMtuText
    {
        // 返回 MTU 文本。
        get => _attMtuText;
        // 内部应用诊断快照。
        private set => SetProperty(ref _attMtuText, value);
    }

    /// <summary>设备型号、硬件和固件版本。</summary>
    public string DeviceVersionText
    {
        // 返回版本文本。
        get => _deviceVersionText;
        // 内部应用诊断快照。
        private set => SetProperty(ref _deviceVersionText, value);
    }

    /// <summary>最近权威状态修订号。</summary>
    public string StateRevisionText
    {
        // 返回 revision 文本。
        get => _stateRevisionText;
        // 内部应用诊断快照。
        private set => SetProperty(ref _stateRevisionText, value);
    }

    /// <summary>当前上位机进程内自动重连尝试次数。</summary>
    public string ReconnectCountText
    {
        // 返回重连文本。
        get => _reconnectCountText;
        // 内部应用诊断快照。
        private set => SetProperty(ref _reconnectCountText, value);
    }

    /// <summary>循环冗余校验和分片错误累计文本。</summary>
    public string ProtocolErrorText
    {
        // 返回错误文本。
        get => _protocolErrorText;
        // 内部应用诊断快照。
        private set => SetProperty(ref _protocolErrorText, value);
    }

    /// <summary>最近权威电量。</summary>
    public string BatteryText
    {
        // 返回电量文本。
        get => _batteryText;
        // 内部应用诊断快照。
        private set => SetProperty(ref _batteryText, value);
    }

    /// <summary>设备能力清单中的模型短摘要、能力位和内部文件系统可用量。</summary>
    public string ManifestSummaryText
    {
        // 返回最近成功连接的兼容摘要。
        get => _manifestSummaryText;
        // 内部应用领域诊断快照。
        private set => SetProperty(ref _manifestSummaryText, value);
    }

    /// <summary>诊断日志文本。</summary>
    public string DiagnosticLog
    {
        // 返回日志。
        get => _diagnosticLog;
        // 内部更新日志。
        private set => SetProperty(ref _diagnosticLog, value);
    }

    /// <summary>true 表示设备已经确认 RawStream 开启。</summary>
    public bool RawStreamEnabled
    {
        // 返回当前设备确认状态，不把按钮点击意图伪装成成功。
        get => _rawStreamEnabled;
        // 内部同步设备接口事实并更新按钮文本。
        private set
        {
            // 属性未变化时不重复通知绑定。
            if (!SetProperty(ref _rawStreamEnabled, value))
            {
                // 没有状态变化，结束属性更新。
                return;
            }

            // 同一状态变化还会改变按钮动词。
            OnPropertyChanged(nameof(RawStreamButtonText));
        }
    }

    /// <summary>RawStream 按钮文字；已开时提供明确关闭动作。</summary>
    public string RawStreamButtonText => RawStreamEnabled ? "关闭六轴" : "开启六轴";

    /// <summary>RawStream 开关结果和“不落盘”边界。</summary>
    public string RawStreamStatus
    {
        // 返回用户可见状态。
        get => _rawStreamStatus;
        // 内部写入设备 ACK 或明确失败原因。
        private set => SetProperty(ref _rawStreamStatus, value);
    }

    /// <summary>最近合法诊断六轴流样本的固定 25 赫兹同步诊断码。</summary>
    public string LatestRawSampleText
    {
        // 返回最近样本摘要。
        get => _latestRawSampleText;
        // 内部只更新内存文本，禁止写入文件或历史仓储。
        private set => SetProperty(ref _latestRawSampleText, value);
    }

    /// <summary>本次进程累计收到的合法 RawStream 样本数。</summary>
    public string RawSampleCountText => _rawSampleCount.ToString(System.Globalization.CultureInfo.InvariantCulture);

    /// <summary>最近十秒加速度三轴点，单位 g。</summary>
    public IReadOnlyList<ImuPlotPoint> AccelerationPoints
    {
        // 返回 WPF 当前绑定的不可变快照。
        get => _accelerationPoints;
        // 内部替换快照并触发曲线控件重绘。
        private set => SetProperty(ref _accelerationPoints, value);
    }

    /// <summary>最近十秒角速度三轴点，单位度每秒。</summary>
    public IReadOnlyList<ImuPlotPoint> GyroscopePoints
    {
        // 返回 WPF 当前绑定的不可变快照。
        get => _gyroscopePoints;
        // 内部替换快照并触发曲线控件重绘。
        private set => SetProperty(ref _gyroscopePoints, value);
    }

    /// <summary>true 表示曲线冻结，但样本计数、最新值和质量仍继续更新。</summary>
    public bool RawChartPaused
    {
        // 返回曲线冻结状态。
        get => _rawChartPaused;
        // 内部更新状态并同步按钮文字。
        private set
        {
            // 状态未变化时不重复刷新按钮。
            if (!SetProperty(ref _rawChartPaused, value))
            {
                // 返回表示当前状态已经满足目标。
                return;
            }

            // 通知 WPF 重新读取“暂停/继续”按钮文字。
            OnPropertyChanged(nameof(RawChartPauseButtonText));
        }
    }

    /// <summary>曲线暂停按钮按当前状态显示下一步动作。</summary>
    public string RawChartPauseButtonText => RawChartPaused ? "继续曲线" : "暂停曲线";

    /// <summary>当前视口末端距离最新样本的秒数；WPF 滑块双向绑定该值。</summary>
    public double RawChartOffsetSeconds
    {
        // 返回用户当前选择的历史偏移；零表示实时。
        get => _rawChartOffsetSeconds;
        // 限制到当前历史范围，并在选择过去窗口时自动冻结视图。
        set
        {
            // NaN 或无穷不能进入滑块和索引换算，统一退回实时位置。
            double finiteValue = double.IsFinite(value) ? value : 0.0;
            // 把偏移限制在零到当前最大历史秒数，防止数组越界。
            double clampedValue = Math.Clamp(finiteValue, 0.0, RawChartMaximumOffsetSeconds);
            // 数值没有变化时不重复切片和重绘。
            if (!SetProperty(ref _rawChartOffsetSeconds, clampedValue))
            {
                // 现有视口已经对应目标偏移。
                return;
            }

            // 用户离开实时端点即进入冻结浏览，后续样本只追加历史、不推动当前视口。
            if (clampedValue > 0.001 && !RawChartPaused)
            {
                // 更新暂停状态和按钮文案。
                RawChartPaused = true;
            }

            // 按新偏移从长历史提取最多十秒同步窗口。
            PublishRawChartViewport();
        }
    }

    /// <summary>当前历史允许回看的最大秒数；不足一个完整窗口时为零。</summary>
    public double RawChartMaximumOffsetSeconds
    {
        // 返回滑块上限。
        get => _rawChartMaximumOffsetSeconds;
        // 内部随历史增长或批量裁剪更新滑块上限。
        private set => SetProperty(ref _rawChartMaximumOffsetSeconds, value);
    }

    /// <summary>当前曲线视口是实时还是历史，以及实际设备时间范围。</summary>
    public string RawChartWindowText
    {
        // 返回当前窗口说明。
        get => _rawChartWindowText;
        // 内部在视口切换或清空时更新。
        private set => SetProperty(ref _rawChartWindowText, value);
    }

    /// <summary>进程内六轴历史长度；该历史不会写入文件或训练记录。</summary>
    public string RawChartHistoryText
    {
        // 返回当前缓存时长和容量上限。
        get => _rawChartHistoryText;
        // 内部按最新设备时间更新。
        private set => SetProperty(ref _rawChartHistoryText, value);
    }

    /// <summary>最近 IMU CSV 导出范围、行数、路径或错误说明。</summary>
    public string ImuExportStatus
    {
        // 返回用户可见导出状态。
        get => _imuExportStatus;
        // 内部在取消、成功或失败时更新。
        private set => SetProperty(ref _imuExportStatus, value);
    }

    /// <summary>最近一个样本换算后的六轴物理量。</summary>
    public string LatestPhysicalValuesText
    {
        // 返回六轴物理量文本。
        get => _latestPhysicalValuesText;
        // 内部更新现场可见数值。
        private set => SetProperty(ref _latestPhysicalValuesText, value);
    }

    /// <summary>按设备单调时间估算的实际 RawStream 采样率。</summary>
    public string RawSampleRateText
    {
        // 返回采样率文本。
        get => _rawSampleRateText;
        // 内部更新最多一秒窗口的估算结果。
        private set => SetProperty(ref _rawSampleRateText, value);
    }

    /// <summary>综合序号连续性、时间单调性和设备质量位的状态文本。</summary>
    public string RawQualityText
    {
        // 返回数据质量文本。
        get => _rawQualityText;
        // 内部写入正常或明确异常原因。
        private set => SetProperty(ref _rawQualityText, value);
    }

    /// <summary>最近融合双 M0 结果的中文动作名称。</summary>
    public string FusedActionText
    {
        // 返回设备端融合 logits 的 Top-1 动作。
        get => _fusedActionText;
        // 内部只接受已解码分类诊断。
        private set => SetProperty(ref _fusedActionText, value);
    }

    /// <summary>最近融合 Top-1 稳定 softmax 概率。</summary>
    public string FusedConfidenceText
    {
        // 返回百分比文本。
        get => _fusedConfidenceText;
        // 内部更新现场诊断百分比。
        private set => SetProperty(ref _fusedConfidenceText, value);
    }

    /// <summary>基础 M0 动作和置信度摘要。</summary>
    public string BaseModelText
    {
        // 返回基础模型摘要。
        get => _baseModelText;
        // 内部更新本窗口结果。
        private set => SetProperty(ref _baseModelText, value);
    }

    /// <summary>掩码 M0 动作和置信度摘要。</summary>
    public string MaskedModelText
    {
        // 返回弱类补偿模型摘要。
        get => _maskedModelText;
        // 内部更新本窗口结果。
        private set => SetProperty(ref _maskedModelText, value);
    }

    /// <summary>三路 Top-1 是否一致。</summary>
    public string ModelAgreementText
    {
        // 返回一致或分歧解释。
        get => _modelAgreementText;
        // 内部写入三路比较结果。
        private set => SetProperty(ref _modelAgreementText, value);
    }

    /// <summary>窗口序号、设备时间和推理耗时。</summary>
    public string InferenceTimingText
    {
        // 返回性能摘要。
        get => _inferenceTimingText;
        // 内部更新最近窗口性能。
        private set => SetProperty(ref _inferenceTimingText, value);
    }

    /// <summary>分类窗口质量位和累计失败窗口数。</summary>
    public string InferenceQualityText
    {
        // 返回分类质量摘要。
        get => _inferenceQualityText;
        // 内部更新设备事实。
        private set => SetProperty(ref _inferenceQualityText, value);
    }

    /// <summary>设备权威稳定动作；标准动作示范只跟随三窗确认后的该值。</summary>
    public ActionId CurrentAction
    {
        // 返回当前稳定动作枚举。
        get => _currentAction;
        // 内部应用 LiveState，不接受单窗诊断直接覆盖。
        private set
        {
            // 动作未变化时不重复刷新标准动作名称。
            if (!SetProperty(ref _currentAction, value))
            {
                // 当前动作已是目标值。
                return;
            }
            // 同一枚举变化还会改变稳定动作中文名称。
            OnPropertyChanged(nameof(StableActionText));
            // 同一枚举变化还会改变标准姿态短提示。
            OnPropertyChanged(nameof(StableActionCueText));
        }
    }

    /// <summary>设备三窗确认后的稳定动作中文名称。</summary>
    public string StableActionText => DisplayText.ActionName(CurrentAction);

    /// <summary>当前稳定动作的一条标准姿态提示，用于辅助理解人偶动作。</summary>
    public string StableActionCueText => DisplayText.ActionCue(CurrentAction);

    /// <summary>标准动作示范周期修订号；动作切换时递增一次。</summary>
    public uint AnimationRevision
    {
        // 返回当前示范修订号。
        get => _animationRevision;
        // 内部更新并通知矢量控件重置相位时钟。
        private set => SetProperty(ref _animationRevision, value);
    }

    /// <summary>true 表示链路在线，断线时标准动作区显示等待设备。</summary>
    public bool IsAnimationConnected
    {
        // 返回当前连接事实。
        get => _isAnimationConnected;
        // 内部同步设备会话连接变化。
        private set => SetProperty(ref _isAnimationConnected, value);
    }

    /// <summary>true 表示标准动作示范固定在代表姿态，不循环播放。</summary>
    public bool ReducedMotion
    {
        // 返回系统与用户合并后的有效值。
        get => _reducedMotion;
        // 动画偏好事件内部更新。
        private set => SetProperty(ref _reducedMotion, value);
    }

    /// <summary>设备当前状态中文文本。</summary>
    public string DeviceStateText
    {
        // 返回准备、运行、暂停或总结文本。
        get => _deviceStateText;
        // 内部应用权威 LiveState。
        private set => SetProperty(ref _deviceStateText, value);
    }

    /// <summary>设备权威累计次数、步数或秒数。</summary>
    public uint MetricValue
    {
        // 返回累计整数值。
        get => _metricValue;
        // 内部应用权威 LiveState。
        private set => SetProperty(ref _metricValue, value);
    }

    /// <summary>当前累计单位。</summary>
    public string MetricUnitText
    {
        // 返回次、步或秒。
        get => _metricUnitText;
        // 内部根据 MetricKind 更新。
        private set => SetProperty(ref _metricUnitText, value);
    }

    /// <summary>设备会话单调时长。</summary>
    public string DurationText
    {
        // 返回 hh:mm:ss 文本。
        get => _durationText;
        // 内部应用权威 LiveState。
        private set => SetProperty(ref _durationText, value);
    }

    /// <summary>设备权威累计活动热量。</summary>
    public string CaloriesText
    {
        // 返回保留两位小数的千卡文本。
        get => _caloriesText;
        // 内部应用权威 LiveState。
        private set => SetProperty(ref _caloriesText, value);
    }

    // 导出实时全部缓存或当前冻结视口；导出只读取内存快照，不停止 BLE 接收。
    private async Task ExportImuCsvAsync()
    {
        // 正式应用必须同时注入写入器和用户路径选择器，缺任一依赖都禁止隐式写盘。
        if (_imuCsvExporter is null || _imuExportDestinationPicker is null)
        {
            // 明确说明当前启动方式没有文件导出能力。
            ImuExportStatus = "当前上位机未启用 IMU 文件导出服务。";
            // 不创建路径或文件。
            return;
        }

        // 没有样本时导出空表没有诊断价值，提示用户先开启六轴采集。
        if (_imuExportBuffer.Count == 0)
        {
            // 提供可恢复操作，不弹出空保存对话框。
            ImuExportStatus = "暂无 IMU 样本；请先连接设备并开启六轴。";
            // 当前命令结束。
            return;
        }

        // 暂停或离开实时端点表示用户正在分析某个历史窗口，此时只导出当前可见范围。
        bool exportVisibleWindow = RawChartPaused || RawChartOffsetSeconds > 0.001;
        // 声明不可变导出快照；后续 BLE 样本只进入可变历史，不改变本次文件内容。
        IReadOnlyList<ImuExportSample> snapshot;
        // 回看模式按当前曲线首尾设备时间截取同一批同步样本。
        if (exportVisibleWindow && AccelerationPoints.Count > 0)
        {
            // 读取可见窗口起点，单位设备单调秒。
            double startSeconds = AccelerationPoints[0].Seconds;
            // 读取可见窗口终点，单位设备单调秒。
            double endSeconds = AccelerationPoints[^1].Seconds;
            // 选取闭区间内记录；0.5 毫秒容差只吸收 double 换算误差，不跨越 25 Hz 相邻点。
            snapshot = _imuExportBuffer
                .Where(sample =>
                {
                    // 把设备 uint32 毫秒转换为与图表相同的秒坐标。
                    double sampleSeconds = sample.DeviceMonotonicMilliseconds / 1000.0;
                    // 返回当前点是否处于可见十秒闭区间。
                    return sampleSeconds >= startSeconds - 0.0005 && sampleSeconds <= endSeconds + 0.0005;
                })
                .ToArray();
        }
        else
        {
            // 实时模式复制最近十分钟全部缓存，便于离线定位计数前后完整波形。
            snapshot = _imuExportBuffer.ToArray();
        }

        // 极端时间回绕或刚清空视口时可能得到空窗口，禁止生成误导文件。
        if (snapshot.Count == 0)
        {
            // 提示回到实时或重新选择有数据的窗口。
            ImuExportStatus = "当前可见窗口没有可导出的 IMU 样本。";
            // 结束本次命令。
            return;
        }

        // 中文范围标签进入默认文件名，用户能直接区分完整缓存与当前窗口。
        string rangeName = exportVisibleWindow ? "当前窗口" : "全部缓存";
        // 使用本地时间生成唯一且可排序的建议文件名。
        string suggestedFileName = $"IMU_{rangeName}_{DateTimeOffset.Now:yyyyMMdd_HHmmss}.csv";
        // 打开系统保存对话框；用户取消时不得触发写盘。
        string? selectedPath = await _imuExportDestinationPicker.PickCsvPathAsync(suggestedFileName).ConfigureAwait(true);
        // 空路径表示用户主动取消。
        if (string.IsNullOrWhiteSpace(selectedPath))
        {
            // 取消不视为错误，也不改变缓存。
            ImuExportStatus = "已取消 IMU 数据导出。";
            // 命令正常结束。
            return;
        }

        try
        {
            // 冻结类型九分类历史，导出器按设备窗口结束时间而非 PC 通知顺序关联到 IMU 行。
            IReadOnlyList<InferenceExportSample> inferenceSnapshot = _inferenceExportBuffer.ToArray();
            // 冻结权威计数事件历史，导出器按原始 MetricEvent 设备时刻标出准确计数点。
            IReadOnlyList<MetricEventExportSample> metricEventSnapshot = _metricEventExportBuffer.ToArray();
            // 异步写出三条冻结时间轴；CSV 包含模型类别、窗口末点、推理耗时和计数标记。
            await _imuCsvExporter.ExportAsync(
                    snapshot,
                    inferenceSnapshot,
                    metricEventSnapshot,
                    selectedPath)
                .ConfigureAwait(true);
            // 成功状态包含范围、行数和绝对路径，便于用户把准确文件交给算法分析。
            ImuExportStatus = $"已导出{rangeName} {snapshot.Count} 条 IMU 样本：{Path.GetFullPath(selectedPath)}";
        }
        catch (Exception exception)
        {
            // 文件占用、权限或磁盘错误转成中文可恢复提示，不影响继续接收 BLE 样本。
            ImuExportStatus = $"IMU 数据导出失败：{exception.Message}";
        }
    }

    // 按当前设备确认状态切换 RawStream；设备失败不会伪装为已开启。
    private async Task ToggleRawStreamAsync()
    {
        // 会话不支持 RawStream 时给出明确能力错误。
        if (_rawStreamSource is null)
        {
            // 页面保持关闭，说明当前会话缺少能力。
            RawStreamStatus = "当前设备会话不支持诊断六轴流。";
            // 不发送任何控制命令。
            return;
        }

        // 未连接时不得缓存一个虚假开启状态。
        if (!_deviceSession.IsConnected)
        {
            // 明确要求先连接设备。
            RawStreamStatus = "设备未连接；请先连接，再开启开发者诊断六轴流。";
            // 保持设备事实为关闭。
            RawStreamEnabled = false;
            // 不调用底层控制命令。
            return;
        }

        // 目标状态取当前确认状态的反值。
        bool targetEnabled = !RawStreamEnabled;

        // 关闭命令发出前先关闭本地接收门，丢弃等待设备 ACK 期间已经在途的旧样本。
        if (!targetEnabled)
        {
            // 本地状态立即变为关闭；底层命令失败时由 catch 按设备事实恢复。
            RawStreamEnabled = false;
        }

        try
        {
            // 等待设备 ACK；失败异常由下方转换为可见状态。
            await _rawStreamSource.SetRawStreamEnabledAsync(targetEnabled).ConfigureAwait(true);
            // 重新读取底层确认状态，禁止只相信调用参数。
            RawStreamEnabled = _rawStreamSource.IsRawStreamEnabled;
            // 开启时重新从零计数，便于观察本次诊断流量。
            if (RawStreamEnabled)
            {
                // 清空本次开启前的样本计数。
                _rawSampleCount = 0U;
                // 通知计数文本已归零。
                OnPropertyChanged(nameof(RawSampleCountText));
                // 新一轮真机诊断从空曲线和空质量窗口开始。
                ClearRawChart();
                // 新一轮真机诊断从等待首个 62 点分类窗口开始。
                ResetInferenceDiagnostic();
                // 开启新数据流时默认继续绘图，避免旧暂停状态让用户误判无数据。
                RawChartPaused = false;
                // 明确诊断六轴流只在内存显示且不保存。
                RawStreamStatus = "开发者诊断六轴流已开启；仅内存显示，不写入历史或文件。";
            }
            else
            {
                // 关闭成功后明确设备停止发布且上位机不保存。
                RawStreamStatus = "开发者诊断六轴流已关闭；后续样本不接收、不保存。";
            }
        }
        catch (Exception exception)
        {
            // 失败后读取底层事实；例如设备可能因开发者模式未开启而拒绝。
            RawStreamEnabled = _rawStreamSource.IsRawStreamEnabled;
            // 显示完整业务原因，同时说明没有改变本地持久化事实。
            RawStreamStatus = $"诊断六轴流切换失败：{exception.Message}；未保存任何诊断数据。";
        }
    }

    // 切换曲线冻结状态；RawStream 和样本质量统计保持运行。
    private void ToggleRawChartPause()
    {
        // 已暂停时按“继续”直接返回实时端点，避免恢复后仍停留在旧偏移。
        if (RawChartPaused)
        {
            // 解除暂停、清零偏移并发布最新十秒。
            GoLiveRawChart();
            // 恢复操作完成。
            return;
        }

        // 实时状态按“暂停”只冻结当前不可变快照，后台仍继续追加十分钟历史。
        RawChartPaused = true;
        // 当前视口仍为零偏移，但文案明确显示已暂停。
        UpdateRawChartWindowText();
    }

    // 解除暂停并把两张同步曲线移动到最新十秒。
    private void GoLiveRawChart()
    {
        // 先解除暂停，使后续样本继续推动视口。
        RawChartPaused = false;
        // 直接更新字段，避免属性 setter 在相同 UI 操作中重复切片。
        bool offsetChanged = _rawChartOffsetSeconds != 0.0;
        // 零偏移固定表示实时端点。
        _rawChartOffsetSeconds = 0.0;
        // 偏移实际改变时通知滑块回到实时端。
        if (offsetChanged)
        {
            // 通知 WPF 双向绑定同步滑块位置。
            OnPropertyChanged(nameof(RawChartOffsetSeconds));
        }

        // 从长历史重新提取最新十秒并更新窗口说明。
        PublishRawChartViewport();
    }

    // 清空两张曲线及其采样率/连续性窗口，所有数据只存在进程内存。
    private void ClearRawChart()
    {
        // 清空加速度可变缓冲。
        _accelerationBuffer.Clear();
        // 清空角速度可变缓冲。
        _gyroscopeBuffer.Clear();
        // 清空与曲线同步的完整导出记录，防止导出用户已经明确清除的数据。
        _imuExportBuffer.Clear();
        // 清空分类窗口历史，防止新一轮六轴导出关联到已清除的旧类别。
        _inferenceExportBuffer.Clear();
        // 清空权威计数点历史，防止新一轮导出保留已清除的旧事件标记。
        _metricEventExportBuffer.Clear();
        // 清除历史后最大回看范围归零。
        RawChartMaximumOffsetSeconds = 0.0;
        // 清除用户选择的历史偏移。
        _rawChartOffsetSeconds = 0.0;
        // 通知滑块回到实时端点。
        OnPropertyChanged(nameof(RawChartOffsetSeconds));
        // 清空只删除内存样本，不改变用户暂停选择；开启新 RawStream 的调用方会显式恢复实时模式。
        // 发布空加速度快照，立即清除图形。
        AccelerationPoints = Array.Empty<ImuPlotPoint>();
        // 发布空角速度快照，立即清除图形。
        GyroscopePoints = Array.Empty<ImuPlotPoint>();
        // 清空采样率时间窗口。
        _rawRateTimestamps.Clear();
        // 清除上一个序号，使下一点成为新连续性起点。
        _lastRawSampleIndex = null;
        // 清除上一个设备时间，使下一点成为新时间起点。
        _lastRawMonotonicMilliseconds = null;
        // 恢复等待采样率状态。
        RawSampleRateText = "采样率：等待样本";
        // 恢复等待质量状态。
        RawQualityText = "数据质量：等待样本";
        // 恢复等待物理量状态。
        LatestPhysicalValuesText = "尚未接收物理量样本。";
        // 曲线窗口恢复为等待首个实时样本。
        RawChartWindowText = "实时 · 等待样本";
        // 内存历史时长归零，并保留十分钟容量说明。
        RawChartHistoryText = "已缓存 00:00 / 最长 10:00";
        // 恢复导出范围提示；清空后下一次导出会先要求新样本。
        ImuExportStatus = "实时导出全部缓存；暂停或回看时导出当前窗口。";
    }

    // 重置进程内分类显示；不向设备发命令，也不清除设备统计。
    private void ResetInferenceDiagnostic()
    {
        // 融合动作等待首个类型九 payload。
        FusedActionText = "等待分类";
        // 置信度没有窗口时显示占位符。
        FusedConfidenceText = "--";
        // 基础模型等待首个窗口。
        BaseModelText = "基础模型：等待窗口";
        // 掩码模型等待首个窗口。
        MaskedModelText = "掩码模型：等待窗口";
        // 一致性需要三路动作后才能判断。
        ModelAgreementText = "模型一致性：等待窗口";
        // 性能需要设备端测量后才能显示。
        InferenceTimingText = "推理性能：等待窗口";
        // 质量需要设备窗口事实后才能显示。
        InferenceQualityText = "分类质量：等待窗口";
    }

    /// <summary>刷新不发起额外 GATT 读的安全诊断快照。</summary>
    public void Refresh()
    {
        // 未实现诊断接口时只显示基础会话状态。
        if (_deviceSession is not IDeviceDiagnosticsSource diagnosticsSource)
        {
            // 记录接口缺失和当前连接态。
            DiagnosticLog = $"{DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss} | Device={_deviceSession.DeviceId} | Connected={_deviceSession.IsConnected} | Diagnostics=Unavailable";
            // 结束刷新。
            return;
        }

        // 同步获取线程安全快照；该调用不得产生 BLE I/O。
        DeviceDiagnosticsSnapshot snapshot = diagnosticsSource.GetDiagnosticsSnapshot();
        // 信号强度缺失时明确显示系统未提供。
        RssiText = snapshot.RssiDbm.HasValue ? $"{snapshot.RssiDbm.Value} 分贝毫瓦" : "未知";
        // 断开时蓝牙最大传输单元无当前意义。
        AttMtuText = snapshot.AttMtu.HasValue ? $"{snapshot.AttMtu.Value} 字节" : "未连接";
        // 拼接标准设备信息。
        DeviceVersionText = $"型号 {snapshot.ModelNumber}；硬件 {snapshot.HardwareRevision}；固件 {snapshot.FirmwareRevision}";
        // 格式化状态修订号。
        StateRevisionText = snapshot.StateRevision.ToString(System.Globalization.CultureInfo.InvariantCulture);
        // 格式化重连次数。
        ReconnectCountText = snapshot.ReconnectCount.ToString(System.Globalization.CultureInfo.InvariantCulture);
        // 分开显示循环冗余校验与分片错误。
        ProtocolErrorText = $"校验错误={snapshot.CrcErrorCount}；分片错误={snapshot.FragmentErrorCount}";
        // 格式化电量。
        BatteryText = snapshot.BatteryPercent.HasValue ? $"{snapshot.BatteryPercent.Value}%" : "未知";
        // 显示连接阶段已经严格验证过的设备能力清单摘要。
        ManifestSummaryText = snapshot.ManifestSummary;
        // 记录一次完整可复制诊断行。
        DiagnosticLog = $"{DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss} | Device={snapshot.DeviceId} | Connected={snapshot.IsConnected} | Hardware={snapshot.IsHardwareBacked} | RSSI={RssiText} | MTU={AttMtuText} | Revision={StateRevisionText} | Reconnect={ReconnectCountText} | {ProtocolErrorText} | {ManifestSummaryText}";
    }

    // 链路事件切回 UI 线程后刷新诊断。
    private void OnConnectionChanged(object? sender, DeviceConnectionChangedEventArgs eventArgs)
    {
        // 事件参数不可变，捕获连接事实后再切换 UI 线程。
        bool isConnected = eventArgs.IsConnected;
        // 在 UI 线程同步示范连接态并刷新链路卡片。
        _ = _dispatcher.InvokeAsync(() =>
        {
            // 断线时矢量控件立即隐藏最后动作，避免把旧结果误认为实时识别。
            IsAnimationConnected = isConnected;
            // 刷新 RSSI、MTU、重连次数和版本摘要。
            Refresh();
        });
    }

    // 权威状态事件切回 UI 线程后刷新电量和 revision。
    private void OnStateChanged(object? sender, LiveState state)
    {
        // LiveState 是不可变领域对象，可安全捕获后交给 UI 线程。
        _ = _dispatcher.InvokeAsync(() =>
        {
            // 应用稳定动作、计数、时长和热量，标准动作不跟随单窗候选抖动。
            ApplyLiveState(state);
            // 同步刷新电量、修订号和链路诊断。
            Refresh();
        });
    }

    // 把设备权威 LiveState 映射到单页训练仪表盘，不在 PC 重算次数或热量。
    private void ApplyLiveState(LiveState state)
    {
        // 保存最新权威状态，使后续 IMU、分类窗和计数事件都能绑定到同一设备会话。
        _latestLiveState = state;
        // 动作变化时递增修订号，让标准动作从第一关键帧开始播放。
        if (CurrentAction != state.Action)
        {
            // 保存三窗确认后的稳定动作。
            CurrentAction = state.Action;
            // 无符号自然回绕不影响“值发生变化”的动画触发语义。
            AnimationRevision = unchecked(AnimationRevision + 1U);
        }
        // 链路事实来自当前设备会话，状态通知到达即表示本帧来源仍在线。
        IsAnimationConnected = _deviceSession.IsConnected;
        // 使用统一中文映射显示设备状态。
        DeviceStateText = DisplayText.DeviceStateName(state.DeviceState);
        // 直接显示设备权威累计值。
        MetricValue = state.MetricValue;
        // 根据设备指标类型显示次、步或秒。
        MetricUnitText = DisplayText.MetricUnit(state.MetricKind);
        // 使用设备单调毫秒格式化训练时长，不受系统时间调整影响。
        DurationText = TimeSpan.FromMilliseconds(state.ElapsedMilliseconds).ToString(@"hh\:mm\:ss");
        // 设备发送千分之一千卡，界面换算为千卡并保留两位小数。
        CaloriesText = $"{state.CaloriesKcal:F2} 千卡";
    }

    // 系统或用户减少动画设置变化时，在 UI 线程更新标准动作示范。
    private void OnAnimationPreferencesChanged(object? sender, EventArgs eventArgs)
    {
        // 共享偏好可能由设置页 UI 线程触发，仍统一经过调度器保持线程合同。
        _ = _dispatcher.InvokeAsync(() =>
        {
            // 应用系统和用户合并后的最终设置。
            ReducedMotion = _animationPreferences.IsReducedMotionEnabled;
        });
    }

    // RawStream 后台事件切回 UI 线程，只展示最近值和内存计数。
    private void OnRawSampleReceived(object? sender, RawImuSampleReceivedEventArgs eventArgs)
    {
        // 捕获不可变样本，避免闭包依赖可变事件对象。
        RawImuSampleV1 sample = eventArgs.Sample;
        // 在 BLE 事件到达时立即记录 PC UTC，避免 UI 调度排队时间混入无线接收时间。
        DateTimeOffset receivedAtUtc = DateTimeOffset.UtcNow;
        // 调度一次 UI 更新；此路径严禁调用 JSON 仓储或文件 API。
        _ = _dispatcher.InvokeAsync(() => ApplyRawSample(sample, receivedAtUtc));
    }

    // 类型九分类诊断从 BLE 后台线程切回 UI 线程，禁止跨线程写绑定属性。
    private void OnInferenceDiagnosticReceived(
        object? sender,
        InferenceDiagnosticReceivedEventArgs eventArgs)
    {
        // 捕获不可变诊断对象，避免调度后依赖事件参数生命周期。
        InferenceDiagnosticV1 diagnostic = eventArgs.Diagnostic;
        // 在 BLE 事件到达时记录 PC UTC；设备窗口结束时间仍是分类关联的主时间轴。
        DateTimeOffset receivedAtUtc = DateTimeOffset.UtcNow;
        // 调度一次轻量属性和内存历史更新；不修改训练状态或会话仓储。
        _ = _dispatcher.InvokeAsync(() => ApplyInferenceDiagnostic(diagnostic, receivedAtUtc));
    }

    // 把设备端三路模型结果映射为中文诊断；不在 PC 重跑模型或修改权威动作。
    private void ApplyInferenceDiagnostic(InferenceDiagnosticV1 diagnostic, DateTimeOffset receivedAtUtc)
    {
        // 页面已释放或 RawStream 已关闭时丢弃迟到通知。
        if (_disposed || !RawStreamEnabled || _rawStreamSource is null || !_rawStreamSource.IsRawStreamEnabled)
        {
            // 不更新关闭后的界面事实。
            return;
        }
        // 分类窗口到达时写入有界内存历史；会话序号来自最近 LiveState，零表示尚未同步状态。
        AppendInferenceExportSample(
            _inferenceExportBuffer,
            new InferenceExportSample(
                _latestLiveState?.SessionSequence ?? 0U,
                receivedAtUtc,
                diagnostic));
        // 11 类动作使用统一中文映射，Unknown 显示等待识别。
        FusedActionText = DisplayText.ActionName(diagnostic.FusedAction);
        // 概率保留一位百分比，避免给现场测试虚假的过高精度。
        FusedConfidenceText = $"{diagnostic.FusedConfidence * 100.0:F1}%";
        // 展示基础 M0 的中文 Top-1 和概率。
        BaseModelText = $"基础模型：{DisplayText.ActionName(diagnostic.BaseAction)} · {diagnostic.BaseConfidence * 100.0:F1}%";
        // 展示掩码 M0 的中文 Top-1 和概率。
        MaskedModelText = $"掩码模型：{DisplayText.ActionName(diagnostic.MaskedAction)} · {diagnostic.MaskedConfidence * 100.0:F1}%";
        // 三路一致仅表示同一窗口 Top-1 相同，不表示动作段已经锁定或计数成立。
        bool allModelsAgree = (diagnostic.FusedAction == diagnostic.BaseAction) &&
            (diagnostic.FusedAction == diagnostic.MaskedAction);
        // 用明确中文区分一致和分歧，便于现场观察弱类模型影响。
        ModelAgreementText = allModelsAgree ? "模型一致性：三路最高类别一致" : "模型一致性：三路存在分歧";
        // 微秒换算毫秒仅用于显示；窗口序号和设备单调时间保持原始整数事实。
        double inferenceMilliseconds = diagnostic.InferenceMicroseconds / 1000.0;
        // 拼接窗口序号、结束时间和端到端耗时。
        InferenceTimingText = $"推理性能：窗口 {diagnostic.WindowSequence} · 设备 {diagnostic.WindowEndMilliseconds} ms · {inferenceMilliseconds:F2} ms";
        // 零质量位且失败累计不增长时显示正常，否则保留十六进制事实和累计数。
        InferenceQualityText = diagnostic.QualityFlags == 0U
            ? $"分类质量：正常 · 累计失败 {diagnostic.FailureCount}"
            : $"分类质量：设备标志 0x{diagnostic.QualityFlags:X4} · 累计失败 {diagnostic.FailureCount}";
    }

    // 把一个合法 22 字节样本映射为物理量、质量状态和两张十秒曲线；六轴顺序固定 gx、gy、gz、ax、ay、az。
    private void ApplyRawSample(RawImuSampleV1 sample, DateTimeOffset receivedAtUtc)
    {
        // 页面已释放或底层已关闭时丢弃迟到回调，不污染关闭后的计数。
        if (_disposed || !RawStreamEnabled || _rawStreamSource is null || !_rawStreamSource.IsRawStreamEnabled)
        {
            // 不更新任何 UI 状态。
            return;
        }

        // 饱和递增计数，避免极长调试会话发生 uint 回绕。
        if (_rawSampleCount < uint.MaxValue)
        {
            // 当前合法样本计入本次内存诊断计数。
            _rawSampleCount++;
        }

        // 通知绑定重新读取格式化计数。
        OnPropertyChanged(nameof(RawSampleCountText));
        // 格式化序号、单调毫秒和六轴 25 赫兹同步诊断码；不进行隐式量程换算。
        LatestRawSampleText = $"第 {sample.SampleIndex} 个样本；设备单调时间={sample.MonotonicMilliseconds} 毫秒；陀螺仪诊断码=[{sample.GxRaw},{sample.GyRaw},{sample.GzRaw}]；加速度诊断码=[{sample.AxRaw},{sample.AyRaw},{sample.AzRaw}]；质量标志=0x{sample.QualityFlags:X4}";

        // 把三轴角速度定点码转换为度每秒；转换只用于显示，不回写模型输入。
        double gxDegreesPerSecond = sample.GxRaw / GyroscopeCodesPerDegreePerSecond;
        // 转换 Y 轴角速度。
        double gyDegreesPerSecond = sample.GyRaw / GyroscopeCodesPerDegreePerSecond;
        // 转换 Z 轴角速度。
        double gzDegreesPerSecond = sample.GzRaw / GyroscopeCodesPerDegreePerSecond;
        // 把三轴加速度定点码转换为重力加速度倍数 g。
        double axG = sample.AxRaw / AccelerationCodesPerG;
        // 转换 Y 轴加速度。
        double ayG = sample.AyRaw / AccelerationCodesPerG;
        // 转换 Z 轴加速度。
        double azG = sample.AzRaw / AccelerationCodesPerG;
        // 用固定两位或三位小数显示现场物理量，足以观察静止重力和手腕转动。
        LatestPhysicalValuesText = $"角速度：X={gxDegreesPerSecond:F2}、Y={gyDegreesPerSecond:F2}、Z={gzDegreesPerSecond:F2} °/s；加速度：X={axG:F3}、Y={ayG:F3}、Z={azG:F3} g";

        // 创建本样本质量异常列表；空列表表示序号、时间和设备质量位均正常。
        List<string> qualityIssues = new(3);
        // 已有上一个样本时检查 uint32 自然回绕后的序号差是否恰好为一。
        if (_lastRawSampleIndex.HasValue && unchecked(sample.SampleIndex - _lastRawSampleIndex.Value) != 1U)
        {
            // 序号不连续通常表示 BLE 通知丢失或设备端主动丢样。
            qualityIssues.Add("样本序号不连续");
        }

        // 已有上一个时间时检查单调毫秒差；大于半个 uint32 周期视为时间倒退。
        if (_lastRawMonotonicMilliseconds.HasValue)
        {
            // 以无符号减法兼容设备运行约 49.7 天后的自然回绕。
            uint monotonicDelta = unchecked(sample.MonotonicMilliseconds - _lastRawMonotonicMilliseconds.Value);
            // 零间隔或超过半周期表示重复/倒退时间戳。
            if (monotonicDelta == 0U || monotonicDelta >= 0x80000000U)
            {
                // 记录时间异常，并清空采样率窗口避免显示错误频率。
                qualityIssues.Add("设备时间不单调");
                // 异常时间不能继续参与速率估算。
                _rawRateTimestamps.Clear();
            }
        }

        // 设备质量位非零时原样显示十六进制，具体位定义由固件协议负责。
        if (sample.QualityFlags != 0U)
        {
            // 把设备端污染、丢样或重采样标记加入综合状态。
            qualityIssues.Add($"设备标志 0x{sample.QualityFlags:X4}");
        }

        // 保存本样本序号作为下一次连续性基准。
        _lastRawSampleIndex = sample.SampleIndex;
        // 保存本样本单调毫秒作为下一次时间基准。
        _lastRawMonotonicMilliseconds = sample.MonotonicMilliseconds;
        // 把当前时间加入最多 26 点窗口，25 赫兹时覆盖约一秒。
        _rawRateTimestamps.Enqueue(sample.MonotonicMilliseconds);
        // 窗口超过 26 点时移除最旧时间。
        while (_rawRateTimestamps.Count > 26)
        {
            // 每轮只移除一个最旧时间，直到容量满足要求。
            _rawRateTimestamps.Dequeue();
        }

        // 至少两个时间点才能估算频率。
        if (_rawRateTimestamps.Count >= 2)
        {
            // 读取窗口起点。
            uint firstTimestamp = _rawRateTimestamps.Peek();
            // 读取窗口终点；队列最多 26 项，Last 的线性开销可忽略。
            uint lastTimestamp = _rawRateTimestamps.Last();
            // 以无符号差兼容 uint32 自然回绕。
            uint elapsedMilliseconds = unchecked(lastTimestamp - firstTimestamp);
            // 有效正时间窗口才执行除法。
            if (elapsedMilliseconds > 0U && elapsedMilliseconds < 0x80000000U)
            {
                // 频率等于间隔数除以持续秒数，单位赫兹。
                double sampleRateHertz = (_rawRateTimestamps.Count - 1) * 1000.0 / elapsedMilliseconds;
                // 保留一位小数，25 赫兹目标偏差可直接观察。
                RawSampleRateText = $"采样率：{sampleRateHertz:F1} Hz（目标 25 Hz）";
            }
        }

        // 综合质量为空时显示正常，否则用中文分号列出全部原因。
        RawQualityText = qualityIssues.Count == 0 ? "数据质量：正常" : $"数据质量：{string.Join("；", qualityIssues)}";
        // 用设备单调时间转换为秒，避免 PC 调度抖动改变横轴顺序。
        double seconds = sample.MonotonicMilliseconds / 1000.0;
        // 把加速度 X/Y/Z 物理量加入十分钟进程内历史；暂停只冻结视口、不停止追加。
        AppendHistoryPoint(_accelerationBuffer, new ImuPlotPoint(seconds, axG, ayG, azG));
        // 把角速度 X/Y/Z 物理量加入相同时间索引的十分钟进程内历史。
        AppendHistoryPoint(_gyroscopeBuffer, new ImuPlotPoint(seconds, gxDegreesPerSecond, gyDegreesPerSecond, gzDegreesPerSecond));
        // 把同一条样本的原始码、物理量、设备时间、PC 接收时间和质量位加入对齐导出历史。
        AppendExportSample(
            _imuExportBuffer,
            new ImuExportSample(
                // 保存原始 uint32 样本序号。
                sample.SampleIndex,
                // 保存原始设备单调毫秒。
                sample.MonotonicMilliseconds,
                // 保存 BLE 事件到达时捕获的 UTC。
                receivedAtUtc,
                // 保存六轴 int16 诊断码，顺序固定 gx、gy、gz、ax、ay、az。
                sample.GxRaw,
                sample.GyRaw,
                sample.GzRaw,
                sample.AxRaw,
                sample.AyRaw,
                sample.AzRaw,
                // 保存相同样本换算后的三轴角速度，单位度每秒。
                gxDegreesPerSecond,
                gyDegreesPerSecond,
                gzDegreesPerSecond,
                // 保存相同样本换算后的三轴加速度，单位 g。
                axG,
                ayG,
                azG,
                // 保存设备质量位原值，离线计数器可跳过或单独标记污染点。
                sample.QualityFlags,
                // 保存当前样本所属权威会话序号；未收到 LiveState 时使用零。
                _latestLiveState?.SessionSequence ?? 0U,
                // 保存当前设备状态；未收到 LiveState 时保持空值而不是伪造准备态。
                _latestLiveState?.DeviceState,
                // 保存设备已经锁定的稳定动作；分类候选仍由类型九独立记录。
                _latestLiveState?.Action ?? ActionId.Unknown,
                // 保存设备稳定动作 Q15 置信度换算值；未知状态使用零。
                _latestLiveState?.Confidence ?? 0.0,
                // 保存当前权威累计的业务单位；未知状态使用 None。
                _latestLiveState?.MetricKind ?? MetricKind.None,
                // 保存当前样本到达时的权威累计值；精确增量点仍由 EventV1 标记。
                _latestLiveState?.MetricValue ?? 0U));
        // 三份历史必须保持同样长度；任一实现偏差时取共同长度计算安全上限。
        int synchronizedCount = Math.Min(
            Math.Min(_accelerationBuffer.Count, _gyroscopeBuffer.Count),
            _imuExportBuffer.Count);
        // 可回看范围等于历史中完整十秒窗口之前的样本时长。
        RawChartMaximumOffsetSeconds = Math.Max(
            0.0,
            (synchronizedCount - RawChartWindowCapacity) / RawChartSamplesPerSecond);
        // 每 25 点约一秒更新一次缓存时长文字，避免 25 Hz 文本布局开销。
        if (synchronizedCount == 1 || (sample.SampleIndex % 25U) == 0U)
        {
            // 历史实际持续时间使用首尾设备单调时间，避免 PC 调度误差。
            double historySeconds = synchronizedCount > 1 ?
                Math.Max(0.0, _accelerationBuffer[^1].Seconds - _accelerationBuffer[0].Seconds) :
                0.0;
            // 转换为分钟秒，十分钟满时固定显示 10:00。
            TimeSpan historyDuration = TimeSpan.FromSeconds(Math.Min(historySeconds, 600.0));
            // 明确历史只在内存保留，容量为十分钟。
            RawChartHistoryText = $"已缓存 {historyDuration:mm\\:ss} / 最长 10:00";
        }

        // 实时且未暂停时发布最新十秒；历史浏览时只保存新点，不推动用户正在看的窗口。
        if (!RawChartPaused && RawChartOffsetSeconds <= 0.001)
        {
            // 提取同步窗口并触发两张图重绘。
            PublishRawChartViewport();
        }
    }

    // 向十分钟进程内历史追加一个物理量点，满容量时批量丢弃最旧十秒。
    private static void AppendHistoryPoint(List<ImuPlotPoint> buffer, ImuPlotPoint point)
    {
        // 缓冲达到十分钟容量时批量释放最旧十秒，摊薄 List 左移成本。
        if (buffer.Count >= RawChartHistoryCapacity)
        {
            // 实际移除量不超过当前点数，防止未来容量调整造成参数越界。
            int removeCount = Math.Min(RawChartTrimBatch, buffer.Count);
            // 删除最旧连续区间；两张同步历史对同一采样执行相同操作。
            buffer.RemoveRange(0, removeCount);
        }

        // 把当前样本追加到严格时间顺序末尾。
        buffer.Add(point);
    }

    // 向十分钟 IMU 导出历史追加完整记录，裁剪节奏与两张曲线严格一致。
    private static void AppendExportSample(List<ImuExportSample> buffer, ImuExportSample sample)
    {
        // 缓冲达到十分钟容量时批量释放最旧十秒，避免逐样本移动 15000 项列表。
        if (buffer.Count >= RawChartHistoryCapacity)
        {
            // 实际移除量不超过当前点数，防止未来容量调整造成范围越界。
            int removeCount = Math.Min(RawChartTrimBatch, buffer.Count);
            // 删除最旧连续区间；本函数与两张曲线在同一样本调用中同步执行。
            buffer.RemoveRange(0, removeCount);
        }

        // 把本条完整记录追加到设备时间顺序末尾。
        buffer.Add(sample);
    }

    // 向分类窗口历史追加一个类型九诊断，超过十分钟推导容量时删除最旧窗口。
    private static void AppendInferenceExportSample(
        List<InferenceExportSample> buffer,
        InferenceExportSample sample)
    {
        // 分类窗口数量达到上限时只丢弃一个最旧窗口；每 12 个 IMU 点最多产生一个新窗。
        if (buffer.Count >= InferenceHistoryCapacity)
        {
            // 删除首项后仍保持严格接收顺序，导出器会按窗口序号再次排序。
            buffer.RemoveAt(0);
        }

        // 把当前三路模型结果追加到历史末尾。
        buffer.Add(sample);
    }

    // 向权威计数事件历史追加一个次数或步数标记，超过容量时删除最旧事件。
    private static void AppendMetricEventExportSample(
        List<MetricEventExportSample> buffer,
        MetricEventExportSample sample)
    {
        // 事件数量达到上限时只保留最近现场范围，避免长时间调试无限增长内存。
        if (buffer.Count >= MetricEventHistoryCapacity)
        {
            // 删除最旧事件；设备事件序号仍能让导出端发现中间缺口。
            buffer.RemoveAt(0);
        }

        // 把当前权威次数或步数事件追加到历史末尾。
        buffer.Add(sample);
    }

    // EventV1 从 BLE 后台线程到达时捕获不可变事件和 PC UTC，再切换 UI 线程。
    private void OnProtocolEventReceived(
        object? sender,
        DeviceProtocolEventEventArgs eventArgs)
    {
        // 捕获已经通过 CRC 与字段范围校验的不可变事件对象。
        DeviceEventV1 deviceEvent = eventArgs.Event;
        // 捕获逻辑帧头的设备单调毫秒；固件修复后该值等于原始 MetricEvent 时刻。
        uint deviceMonotonicMilliseconds = eventArgs.MonotonicMilliseconds;
        // 捕获 PC 收到完整通知的 UTC，只用于诊断传输延迟。
        DateTimeOffset receivedAtUtc = DateTimeOffset.UtcNow;
        // 调度到 UI 线程，与三份导出缓存保持单线程写入合同。
        _ = _dispatcher.InvokeAsync(() => ApplyProtocolEvent(
            deviceEvent,
            deviceMonotonicMilliseconds,
            receivedAtUtc));
    }

    // 只把真实增加次数或步数的设备事件写入导出缓存，其他 EventV1 不污染计数标记。
    private void ApplyProtocolEvent(
        DeviceEventV1 deviceEvent,
        uint deviceMonotonicMilliseconds,
        DateTimeOffset receivedAtUtc)
    {
        // 页面已释放、诊断六轴流已关闭或底层已停止时丢弃迟到事件。
        if (_disposed || !RawStreamEnabled || _rawStreamSource is null || !_rawStreamSource.IsRawStreamEnabled)
        {
            // 关闭后的事件不得进入下一次导出。
            return;
        }

        // 只有 RepetitionCounted 且指标为次或步、增量大于零时才是用户要求的计数标记点。
        if (deviceEvent.EventType != DeviceEventType.RepetitionCounted ||
            (deviceEvent.MetricKind != MetricKind.Repetition && deviceEvent.MetricKind != MetricKind.Step) ||
            deviceEvent.MetricDelta == 0U)
        {
            // 会话、故障、电量等事件不写入计数点列。
            return;
        }

        // 保存事件与精确设备时刻；导出器按会话序号和毫秒匹配 IMU 行并去重。
        AppendMetricEventExportSample(
            _metricEventExportBuffer,
            new MetricEventExportSample(
                receivedAtUtc,
                deviceMonotonicMilliseconds,
                deviceEvent));
    }

    // 从十分钟同步历史中提取用户选择的最多十秒窗口，并发布不可变数组快照。
    private void PublishRawChartViewport()
    {
        // 两张历史任一为空时发布空图，避免不同步索引访问。
        if (_accelerationBuffer.Count == 0 || _gyroscopeBuffer.Count == 0)
        {
            // 清空加速度快照。
            AccelerationPoints = Array.Empty<ImuPlotPoint>();
            // 清空角速度快照。
            GyroscopePoints = Array.Empty<ImuPlotPoint>();
            // 同步等待文案。
            RawChartWindowText = "实时 · 等待样本";
            // 空历史不再计算索引。
            return;
        }

        // 使用共同长度保证两张图的样本索引和时间完全一致。
        int synchronizedCount = Math.Min(_accelerationBuffer.Count, _gyroscopeBuffer.Count);
        // 把用户选择的秒偏移换算为最接近的 25 Hz 样本数。
        int offsetSamples = (int)Math.Round(RawChartOffsetSeconds * RawChartSamplesPerSecond);
        // 视口末点不能早于历史首点，也不能晚于最新共同点。
        int endIndex = Math.Clamp(synchronizedCount - 1 - offsetSamples, 0, synchronizedCount - 1);
        // 视口最多包含 250 点；历史不足十秒时从零开始。
        int startIndex = Math.Max(0, endIndex - RawChartWindowCapacity + 1);
        // 计算闭区间点数，保证至少包含当前末点。
        int windowCount = endIndex - startIndex + 1;
        // 发布加速度不可变数组快照；后续历史追加不会改变当前视图。
        AccelerationPoints = _accelerationBuffer.GetRange(startIndex, windowCount).ToArray();
        // 发布同索引角速度快照，保证两图横轴严格同步。
        GyroscopePoints = _gyroscopeBuffer.GetRange(startIndex, windowCount).ToArray();
        // 按实际首尾设备时间生成实时/回看说明。
        UpdateRawChartWindowText();
    }

    // 根据当前不可变快照更新实时、暂停或回看窗口范围文字。
    private void UpdateRawChartWindowText()
    {
        // 没有可见点时保持等待状态。
        if (AccelerationPoints.Count == 0 || _accelerationBuffer.Count == 0)
        {
            // 空视口没有时间范围。
            RawChartWindowText = "实时 · 等待样本";
            // 结束文字更新。
            return;
        }

        // 读取当前可见窗口首点设备秒数。
        double startSeconds = AccelerationPoints[0].Seconds;
        // 读取当前可见窗口末点设备秒数。
        double endSeconds = AccelerationPoints[^1].Seconds;
        // 读取历史最新设备秒数，用于计算实际回看距离。
        double latestSeconds = _accelerationBuffer[^1].Seconds;
        // 回看距离限制为非负，兼容 uint32 时间自然回绕前的正常会话。
        double behindLiveSeconds = Math.Max(0.0, latestSeconds - endSeconds);
        // 实时状态展示当前十秒设备时间范围。
        if (!RawChartPaused && behindLiveSeconds <= 0.05)
        {
            // 短文案适配单页仪表盘。
            RawChartWindowText = $"实时 · {startSeconds:F1}–{endSeconds:F1} 秒";
            // 实时文字更新完成。
            return;
        }

        // 暂停或历史浏览显示实际窗口和距实时秒数。
        RawChartWindowText = $"回看 · {startSeconds:F1}–{endSeconds:F1} 秒 · 距实时 {behindLiveSeconds:F1} 秒";
    }

    /// <inheritdoc />
    public void Dispose()
    {
        // 重复释放不重复退订。
        if (_disposed)
        {
            // 已释放直接返回。
            return;
        }

        // 标记已释放。
        _disposed = true;
        // 解除连接事件。
        _deviceSession.ConnectionChanged -= OnConnectionChanged;
        // 解除状态事件。
        _deviceSession.StateChanged -= OnStateChanged;
        // 解除动画偏好事件，避免页面释放后仍被设置页持有。
        _animationPreferences.Changed -= OnAnimationPreferencesChanged;
        // 会话提供 RawStream 能力时解除样本事件，防止页面关闭后继续更新。
        if (_rawStreamSource is not null)
        {
            // 解除 RawStream 样本事件。
            _rawStreamSource.RawSampleReceived -= OnRawSampleReceived;
            // 解除分类诊断事件，防止页面关闭后继续更新属性。
            _rawStreamSource.InferenceDiagnosticReceived -= OnInferenceDiagnosticReceived;
        }
        // 会话提供 EventV1 能力时解除计数事件，防止页面释放后继续持有 ViewModel。
        if (_protocolEventSource is not null)
        {
            // 解除低延迟协议事件订阅。
            _protocolEventSource.ProtocolEventReceived -= OnProtocolEventReceived;
        }
    }
}
