// 引入设备会话和诊断快照接口。
using FitnessCoach.Domain;
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
    // 曲线固定保留最近 250 个 25 赫兹样本，对应十秒现场观察窗口。
    private const int RawChartCapacity = 250;
    // QMI8658 诊断陀螺仪定点比例；诊断码除以 16.4 得到度每秒。
    private const double GyroscopeCodesPerDegreePerSecond = 16.4;
    // QMI8658 诊断加速度定点比例；诊断码除以 4096 得到重力加速度倍数 g。
    private const double AccelerationCodesPerG = 4096.0;
    // 保存设备会话用于动态读取链路状态。
    private readonly IDeviceSession _deviceSession;
    // 保存 UI 调度器。
    private readonly IUiDispatcher _dispatcher;
    // 保存可选 RawStream 能力；普通会话未实现时诊断页明确显示不可用。
    private readonly IRawStreamSource? _rawStreamSource;
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
    // 保存最近十秒加速度物理量，点顺序固定 X、Y、Z，单位 g。
    private readonly List<ImuPlotPoint> _accelerationBuffer = new(RawChartCapacity);
    // 保存最近十秒角速度物理量，点顺序固定 X、Y、Z，单位度每秒。
    private readonly List<ImuPlotPoint> _gyroscopeBuffer = new(RawChartCapacity);
    // 保存最近 26 个设备单调时间，用于计算最多一秒窗口内的实际采样率。
    private readonly Queue<uint> _rawRateTimestamps = new(26);
    // 发布给 WPF 的不可变加速度快照；每个样本最多包含 250 个点。
    private IReadOnlyList<ImuPlotPoint> _accelerationPoints = Array.Empty<ImuPlotPoint>();
    // 发布给 WPF 的不可变角速度快照；每个样本最多包含 250 个点。
    private IReadOnlyList<ImuPlotPoint> _gyroscopePoints = Array.Empty<ImuPlotPoint>();
    // true 表示继续接收和计数样本，但冻结两张曲线便于现场观察。
    private bool _rawChartPaused;
    // 最近物理量文本使用 g 和度每秒，便于直接判断 QMI8658 是否有数据。
    private string _latestPhysicalValuesText = "尚未接收物理量样本。";
    // 最近一秒窗口估算的 RawStream 实际采样率。
    private string _rawSampleRateText = "采样率：等待样本";
    // 样本序号、时间戳和设备质量位的综合诊断结果。
    private string _rawQualityText = "数据质量：等待样本";
    // 上一个合法样本序号，用于检测 BLE 传输或设备发布丢样。
    private uint? _lastRawSampleIndex;
    // 上一个合法设备单调毫秒，用于检测时间倒退或重复样本。
    private uint? _lastRawMonotonicMilliseconds;
    // 是否已解除事件订阅。
    private bool _disposed;

    /// <summary>创建诊断页并订阅链路/状态事件。</summary>
    public DiagnosticsViewModel(IDeviceSession deviceSession, IUiDispatcher dispatcher)
    {
        // 设备会话不能为空。
        ArgumentNullException.ThrowIfNull(deviceSession);
        // UI 调度器不能为空。
        ArgumentNullException.ThrowIfNull(dispatcher);
        // 保存设备会话。
        _deviceSession = deviceSession;
        // 保存 UI 调度器。
        _dispatcher = dispatcher;
        // 尝试取得可选 RawStream 接口；没有该接口时按钮仍给出明确错误。
        _rawStreamSource = deviceSession as IRawStreamSource;
        // 创建串行 RawStream 开关命令，运行中自动禁用防止并发控制帧。
        ToggleRawStreamCommand = new AsyncRelayCommand(ToggleRawStreamAsync);
        // 创建暂停命令；暂停只冻结曲线，不停止 BLE 接收和质量统计。
        PauseRawChartCommand = new RelayCommand(_ => ToggleRawChartPause());
        // 创建清空命令；仅清空内存曲线和采样率窗口，不向设备发送命令。
        ClearRawChartCommand = new RelayCommand(_ => ClearRawChart());
        // 连接变化时刷新 MTU、重连和版本。
        _deviceSession.ConnectionChanged += OnConnectionChanged;
        // 状态变化时刷新电量和 revision。
        _deviceSession.StateChanged += OnStateChanged;
        // 仅在会话提供 RawStream 能力时订阅原始样本事件。
        if (_rawStreamSource is not null)
        {
            // 样本可能从 BLE 后台线程到达，处理函数负责切换 UI 线程。
            _rawStreamSource.RawSampleReceived += OnRawSampleReceived;
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

    /// <summary>协议版本文本。</summary>
    public string ProtocolText => $"蓝牙低功耗逻辑协议版本 {ProtocolConstants.ProtocolMajor}.{ProtocolConstants.ProtocolMinor}；采用十六位循环冗余校验；支持最大传输单元分片重组。";

    /// <summary>设备实现状态。</summary>
    public string DeviceImplementationText => _deviceSession.IsHardwareBacked
        ? "当前使用真机蓝牙会话；实际射频、功耗和稳定性仍需开发板烧录验收。"
        : "当前使用模拟设备会话；信号强度、版本和错误计数均为模拟诊断值。";

    /// <summary>产品固定电源策略。</summary>
    public string PowerPolicyText => "原配电池容量 400 毫安时；15% 告警、8% 禁止新会话、5% 保存并关机。";

    /// <summary>产品固定振动策略。</summary>
    public string HapticPolicyText => "每次有效计次后振动 30 毫秒；非计次状态不应连续振动。";

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
    public string RawStreamButtonText => RawStreamEnabled ? "关闭诊断六轴流" : "开启诊断六轴流";

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
        // 取反当前冻结状态，按钮文本由属性通知同步更新。
        RawChartPaused = !RawChartPaused;
    }

    // 清空两张曲线及其采样率/连续性窗口，所有数据只存在进程内存。
    private void ClearRawChart()
    {
        // 清空加速度可变缓冲。
        _accelerationBuffer.Clear();
        // 清空角速度可变缓冲。
        _gyroscopeBuffer.Clear();
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
        // 事件参数由诊断快照覆盖；只需要安全调度刷新。
        _ = _dispatcher.InvokeAsync(Refresh);
    }

    // 权威状态事件切回 UI 线程后刷新电量和 revision。
    private void OnStateChanged(object? sender, LiveState state)
    {
        // 状态对象已被会话保存，统一从诊断快照刷新全部字段。
        _ = _dispatcher.InvokeAsync(Refresh);
    }

    // RawStream 后台事件切回 UI 线程，只展示最近值和内存计数。
    private void OnRawSampleReceived(object? sender, RawImuSampleReceivedEventArgs eventArgs)
    {
        // 捕获不可变样本，避免闭包依赖可变事件对象。
        RawImuSampleV1 sample = eventArgs.Sample;
        // 调度一次 UI 更新；此路径严禁调用 JSON 仓储或文件 API。
        _ = _dispatcher.InvokeAsync(() => ApplyRawSample(sample));
    }

    // 把一个合法 22 字节样本映射为物理量、质量状态和两张十秒曲线；六轴顺序固定 gx、gy、gz、ax、ay、az。
    private void ApplyRawSample(RawImuSampleV1 sample)
    {
        // 页面已释放或底层已关闭时丢弃迟到回调，不污染关闭后的计数。
        if (_disposed || _rawStreamSource is null || !_rawStreamSource.IsRawStreamEnabled)
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
        // 暂停时仍更新计数、物理值、频率和质量，但不改变曲线快照。
        if (RawChartPaused)
        {
            // 返回表示当前样本只参与诊断统计。
            return;
        }

        // 用设备单调时间转换为秒，避免 PC 调度抖动改变横轴顺序。
        double seconds = sample.MonotonicMilliseconds / 1000.0;
        // 把加速度 X/Y/Z 物理量加入十秒窗口。
        AppendRollingPoint(_accelerationBuffer, new ImuPlotPoint(seconds, axG, ayG, azG));
        // 把角速度 X/Y/Z 物理量加入十秒窗口。
        AppendRollingPoint(_gyroscopeBuffer, new ImuPlotPoint(seconds, gxDegreesPerSecond, gyDegreesPerSecond, gzDegreesPerSecond));
        // 发布加速度数组快照；WPF 依赖属性收到新引用后重绘。
        AccelerationPoints = _accelerationBuffer.ToArray();
        // 发布角速度数组快照。
        GyroscopePoints = _gyroscopeBuffer.ToArray();
    }

    // 向固定容量窗口追加一个物理量点，超过十秒容量时丢弃最旧点。
    private static void AppendRollingPoint(List<ImuPlotPoint> buffer, ImuPlotPoint point)
    {
        // 缓冲达到容量时先移除索引零的最旧样本。
        if (buffer.Count >= RawChartCapacity)
        {
            // 只移除一个点，因为每次调用只追加一个新样本。
            buffer.RemoveAt(0);
        }

        // 把当前样本追加到时间顺序末尾。
        buffer.Add(point);
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
        // 会话提供 RawStream 能力时解除样本事件，防止页面关闭后继续更新。
        if (_rawStreamSource is not null)
        {
            // 解除 RawStream 样本事件。
            _rawStreamSource.RawSampleReceived -= OnRawSampleReceived;
        }
    }
}
