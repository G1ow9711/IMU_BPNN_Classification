// 引入并发字典，按 request_id 管理同时存在的控制响应等待者。
using System.Collections.Concurrent;
// 引入无锁消息通道，把 WinRT/fake 回调与协议解码后台任务隔离。
using System.Threading.Channels;
// 引入领域设备会话、实时状态和摘要合同。
using FitnessCoach.Domain;

// 真 BLE 会话状态机位于可跨平台测试的协议项目中，具体 WinRT 对象由 transport 实现持有。
namespace FitnessCoach.Bluetooth;

/// <summary>
/// 实现 Windows 真 BLE 会话：扫描连接、订阅、MTU 分片、控制重试、权威 revision 和指数退避重连。
/// </summary>
public sealed class WindowsBleDeviceSession : IDeviceSession, IDevicePairingSession, IDeviceDiscoverySession, ISessionHistorySyncSource, IDeviceDiagnosticsSource, IDeviceConfigurationSession, IRawStreamSource
{
    // 协议规定控制 indication 最长等待 2 秒；测试可注入更短值但生产默认不可漂移。
    private static readonly TimeSpan DefaultControlTimeout = TimeSpan.FromSeconds(2);
    // 协议规定重连退避为 1、2、4、8、15 秒，最后一个值循环使用。
    private static readonly TimeSpan[] DefaultReconnectDelays =
    [
        TimeSpan.FromSeconds(1),
        TimeSpan.FromSeconds(2),
        TimeSpan.FromSeconds(4),
        TimeSpan.FromSeconds(8),
        TimeSpan.FromSeconds(15),
    ];

    // 普通对象锁保护布尔状态、最新快照、设备 ID 和重连任务引用。
    private readonly object _sync = new();
    // 生命周期信号量串行化连接、断开和重连，防止创建两条 GATT 链路。
    private readonly SemaphoreSlim _lifecycleGate = new(1, 1);
    // 控制信号量让请求按顺序发送；ESP32 只缓存 16 个 request_id，串行化可降低歧义。
    private readonly SemaphoreSlim _commandGate = new(1, 1);
    // 会话摘要传输信号量保证同一连接只存在一页请求，匹配设备端单页固定队列。
    private readonly SemaphoreSlim _transferGate = new(1, 1);
    // 停止信号量覆盖“发送停止、读取最终快照、生成摘要”整个事务，保证并发 Stop 返回同一摘要。
    private readonly SemaphoreSlim _stopGate = new(1, 1);
    // transport 负责 Windows 扫描、系统配对、GATT 对象发现和原始字节收发。
    private readonly IWindowsBleTransport _transport;
    // 控制超时值，生产为 2 秒，测试可缩短以避免等待真实时间。
    private readonly TimeSpan _controlTimeout;
    // 重连延迟数组保存独立副本，防止调用者运行中修改策略。
    private readonly TimeSpan[] _reconnectDelays;
    // 延迟函数生产使用 Task.Delay，测试注入确定性时钟验证退避序列。
    private readonly Func<TimeSpan, CancellationToken, Task> _delayAsync;
    // 生命周期取消源在 Dispose 时停止通知泵和所有自动重连。
    private readonly CancellationTokenSource _lifetimeCancellation = new();
    // 通知通道固定为单读者，保持同一进程内到达顺序并避免 BLE 回调执行复杂逻辑。
    private readonly Channel<BleGattValueReceivedEventArgs> _notificationChannel = Channel.CreateUnbounded<BleGattValueReceivedEventArgs>(
        new UnboundedChannelOptions
        {
            // 只有后台通知泵读取，允许运行时使用更轻量同步。
            SingleReader = true,
            // WinRT 可能从多个特征和线程写入，必须支持多写者。
            SingleWriter = false,
            // 禁止回调内联执行消费者，避免 WinRT 线程被业务事件阻塞。
            AllowSynchronousContinuations = false,
        });
    // 每个订阅特征拥有独立严格顺序重组器，防止 sequence 相同的不同特征互相污染。
    private readonly Dictionary<Guid, BleFrameReassembler> _reassemblers = new()
    {
        // 控制 indication 使用独立 1040 字节固定重组缓冲区。
        [ProtocolConstants.ControlPointUuid] = new BleFrameReassembler(),
        // LiveState notification 使用独立重组状态。
        [ProtocolConstants.LiveStateUuid] = new BleFrameReassembler(),
        // Event notification 使用独立重组状态。
        [ProtocolConstants.EventUuid] = new BleFrameReassembler(),
        // Transfer Control indication 使用独立重组状态。
        [ProtocolConstants.TransferControlUuid] = new BleFrameReassembler(),
        // Transfer Data notification 使用独立重组状态。
        [ProtocolConstants.TransferDataUuid] = new BleFrameReassembler(),
        // RawStream notification 使用独立重组状态，避免与会话补传共享 sequence。
        [ProtocolConstants.RawStreamUuid] = new BleFrameReassembler(),
    };
    // request_id 到响应等待者的并发映射允许 BLE 后台线程完成命令任务。
    private readonly ConcurrentDictionary<uint, PendingControlRequest> _pendingControls = new();
    // request_id 到会话页等待者映射；响应和数据可能从不同 GATT 特征交错到达。
    private readonly ConcurrentDictionary<uint, PendingTransferRequest> _pendingTransfers = new();
    // 后台通知泵任务在构造时启动，Dispose 时等待正常结束。
    private readonly Task _notificationPumpTask;
    // 当前 PC 是否希望保持连接；主动 Disconnect 会置 false 并抑制自动重连。
    private bool _connectionDesired;
    // 当前会话层连接状态；只有 manifest、订阅和初始快照全部成功后才为 true。
    private bool _isConnected;
    // dispose 标志阻止释放后新建 GATT 对象。
    private volatile bool _disposed;
    // 最近成功连接的 Windows 设备 ID，用于自动重连优先选择同一设备。
    private string? _preferredDeviceId;
    // 用户可见设备名，未连接前使用稳定占位符。
    private string _deviceId = "BLE-UNSELECTED";
    // 最新设备权威状态；通知 revision 不增大时不会覆盖该对象。
    private LiveState? _latestState;
    // 最近读取的 Manifest 原始 TLV 副本；重连时必须刷新。
    private byte[] _manifest = [];
    // 最近成功连接并通过严格兼容校验的 Manifest v1；失败连接不得覆盖该对象。
    private ManifestV1? _manifestInfo;
    // 控制逻辑帧 sequence 按 16 位自然回绕。
    private ushort _controlSequence;
    // request_id 从 1 开始递增；0 保留为尚未分配的默认值。
    private uint _nextRequestId;
    // TransferRequest 使用独立逻辑帧 sequence，避免与 Control Point 缺口诊断混淆。
    private ushort _transferSequence;
    // 当前自动重连任务；null 或已完成表示可创建新循环。
    private Task? _reconnectLoopTask;
    // 自动重连独立取消源允许主动断开只停止重连而不销毁整个会话对象。
    private CancellationTokenSource? _reconnectCancellation;
    // 最近真正开始会话的 PC UTC 时间，只用于构造 UI 临时摘要，设备摘要同步仍是最终权威。
    private DateTimeOffset _startedAtUtc;
    // 最近 Stop 返回的摘要，重复停止必须返回同一对象而不重复创建本地历史。
    private TrainingSessionSummary? _lastSummary;
    // 最近扫描 RSSI，单位 dBm；Windows 未提供时为 null。
    private int? _rssiDbm;
    // 当前标准设备型号。
    private string _modelNumber = "未知";
    // 当前硬件修订号。
    private string _hardwareRevision = "未知";
    // 当前固件修订号。
    private string _firmwareRevision = "未知";
    // 自动重连尝试累计数；使用 Interlocked 保证后台循环和 UI 读取一致。
    private long _reconnectCount;
    // CRC-16 校验错误累计数。
    private long _crcErrorCount;
    // 分片包络、顺序或长度错误累计数。
    private long _fragmentErrorCount;
    // true 表示用户希望连接恢复后继续 RawStream；普通产品启动默认 false。
    private bool _rawStreamDesired;
    // true 表示当前连接已收到命令 11 成功 ACK；断线立即清零。
    private bool _rawStreamEnabled;

    /// <summary>
    /// 创建真 BLE 会话；生产只传 transport，测试可注入超时、退避和延迟函数。
    /// </summary>
    public WindowsBleDeviceSession(
        IWindowsBleTransport transport,
        TimeSpan? controlTimeout = null,
        IReadOnlyList<TimeSpan>? reconnectDelays = null,
        Func<TimeSpan, CancellationToken, Task>? delayAsync = null)
    {
        // transport 不能为空，否则无法建立 Windows GATT 链路。
        ArgumentNullException.ThrowIfNull(transport);
        // 保存传输实现；所有 WinRT 对象生命周期由该实例负责。
        _transport = transport;
        // 使用生产 2 秒或测试显式超时。
        _controlTimeout = controlTimeout ?? DefaultControlTimeout;
        // 非正超时会造成忙重试或命令永不等待。
        if (_controlTimeout <= TimeSpan.Zero)
        {
            // 拒绝非法控制等待配置。
            throw new ArgumentOutOfRangeException(nameof(controlTimeout), "控制响应超时必须大于零。");
        }

        // 复制调用者退避或协议默认退避。
        _reconnectDelays = (reconnectDelays ?? DefaultReconnectDelays).ToArray();
        // 至少一个正延迟才能避免断线忙循环耗尽 CPU 和电池。
        if ((_reconnectDelays.Length == 0) || _reconnectDelays.Any(delay => delay <= TimeSpan.Zero))
        {
            // 拒绝空数组和非正延迟。
            throw new ArgumentOutOfRangeException(nameof(reconnectDelays), "重连退避必须至少包含一个正时间间隔。");
        }

        // 使用测试延迟函数或真正可取消的 Task.Delay。
        _delayAsync = delayAsync ?? ((delay, token) => Task.Delay(delay, token));
        // transport 收包回调只复制事件到 Channel，不在 WinRT 线程解析协议。
        _transport.ValueReceived += OnTransportValueReceived;
        // transport 断线回调启动受控自动重连。
        _transport.Disconnected += OnTransportDisconnected;
        // 启动唯一后台通知泵，使用会话生命周期 token。
        _notificationPumpTask = Task.Run(() => RunNotificationPumpAsync(_lifetimeCancellation.Token));
    }

    /// <inheritdoc />
    public event EventHandler<LiveState>? StateChanged;

    /// <inheritdoc />
    public event EventHandler<DeviceConnectionChangedEventArgs>? ConnectionChanged;

    /// <inheritdoc />
    public event EventHandler<RawImuSampleReceivedEventArgs>? RawSampleReceived;

    /// <summary>收到低延迟 Event 时触发；累计次数仍必须来自后续 LiveState。</summary>
    public event EventHandler<DeviceProtocolEventEventArgs>? ProtocolEventReceived;

    /// <inheritdoc />
    public string DeviceId
    {
        get
        {
            // 在锁内读取设备 ID，避免重连同时替换字符串。
            lock (_sync)
            {
                // 返回最近成功连接设备的显示名或未选择占位符。
                return _deviceId;
            }
        }
    }

    /// <inheritdoc />
    public bool IsConnected
    {
        get
        {
            // 在锁内读取会话层完整连接状态。
            lock (_sync)
            {
                // 只有服务发现、订阅和快照恢复完成才返回 true。
                return _isConnected;
            }
        }
    }

    /// <inheritdoc />
    public bool IsHardwareBacked => true;

    /// <inheritdoc />
    public bool IsRawStreamEnabled
    {
        get
        {
            // 使用状态锁读取 ACK 后的真实 RawStream 状态。
            lock (_sync)
            {
                // 只有当前连接命令 11 成功后才返回 true。
                return _rawStreamEnabled;
            }
        }
    }

    /// <summary>最近重连读取的 Manifest 原始副本，供诊断页显示协议和模型信息。</summary>
    public ReadOnlyMemory<byte> Manifest
    {
        get
        {
            // 在锁内复制，防止重连刷新数组时调用者观察到变化。
            lock (_sync)
            {
                // 返回独立副本，禁止界面修改内部兼容性基线。
                return _manifest.ToArray();
            }
        }
    }

    /// <inheritdoc />
    public DeviceDiagnosticsSnapshot GetDiagnosticsSnapshot()
    {
        // 在锁内复制身份、连接、MTU 和最近状态，避免重连中观察到混合值。
        lock (_sync)
        {
            // 协议 255 表示未知电量，领域诊断使用 null。
            byte? batteryPercent = (_latestState is not null) && (_latestState.BatteryPercent <= 100)
                ? _latestState.BatteryPercent
                : null;
            // 把模型短摘要、能力位和内部文件系统可用量压成一条稳定诊断摘要，避免界面依赖蓝牙层类型。
            string manifestSummary = _manifestInfo is null
                ? "设备能力清单：未知"
                : $"设备能力清单版本 {_manifestInfo.ProtocolMajor}.{_manifestInfo.ProtocolMinor}；基础模型摘要={_manifestInfo.BaseModelSha256Short}；掩码模型摘要={_manifestInfo.MaskedModelSha256Short}；能力标志=0x{_manifestInfo.Capabilities:X8}（振动、会话历史、内部文件系统）；内部文件系统可用量={_manifestInfo.LittleFsAvailableBytes} 字节";
            // 创建不可变快照；Interlocked 读取的计数夹紧到 uint 范围。
            return new DeviceDiagnosticsSnapshot(
                _deviceId,
                _isConnected,
                isHardwareBacked: true,
                _modelNumber,
                _hardwareRevision,
                _firmwareRevision,
                _rssiDbm,
                _isConnected ? _attMtu : null,
                batteryPercent,
                _latestState?.StateRevision ?? 0U,
                ClampCounter(Interlocked.Read(ref _reconnectCount)),
                ClampCounter(Interlocked.Read(ref _crcErrorCount)),
                ClampCounter(Interlocked.Read(ref _fragmentErrorCount)),
                manifestSummary);
        }
    }

    /// <inheritdoc />
    public async Task ConnectAsync(CancellationToken cancellationToken = default)
    {
        // 释放后不得重新连接系统蓝牙对象。
        ThrowIfDisposed();
        // 标记用户希望持续连接；后续意外断线将自动重连。
        lock (_sync)
        {
            // 保存期望状态，主动 Disconnect 才会清除。
            _connectionDesired = true;
        }

        // 停止可能仍在等待退避的旧重连循环，改由本次显式连接负责。
        CancelReconnectLoop();
        // 把调用方取消和整个会话生命周期关联；Dispose 可终止仍在等待的 Windows 扫描/选择。
        using CancellationTokenSource linkedCancellation = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _lifetimeCancellation.Token);
        // 串行执行扫描、发现、订阅和快照恢复。
        await ConnectCoreAsync(isReconnect: false, linkedCancellation.Token).ConfigureAwait(false);
    }

    /// <inheritdoc />
    public async Task<IReadOnlyList<NearbyBluetoothDevice>> ScanDevicesAsync(
        TimeSpan scanDuration,
        CancellationToken cancellationToken = default)
    {
        // 释放后不得启动新的 Windows 广播扫描。
        ThrowIfDisposed();
        // 扫描必须有有限正时间窗，避免设备页形成忙循环或永久占用蓝牙适配器。
        if ((scanDuration <= TimeSpan.Zero) || (scanDuration > TimeSpan.FromSeconds(30)))
        {
            // 拒绝非正值和超过 30 秒的交互扫描。
            throw new ArgumentOutOfRangeException(nameof(scanDuration), "蓝牙扫描时间必须大于零且不超过 30 秒。");
        }

        // transport 未实现主动发现时明确报告能力缺失，禁止返回伪造空列表。
        if (_transport is not IWindowsBleDiscoveryTransport discoveryTransport)
        {
            // 告知设备页当前平台不能显示附近设备。
            throw new NotSupportedException("当前蓝牙适配器不支持可见设备扫描。");
        }

        // 把调用方取消与会话释放信号合并，关闭应用可立即停止扫描。
        using CancellationTokenSource linkedCancellation = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _lifetimeCancellation.Token);
        // 获取无 WinRT 对象的 transport 扫描快照。
        IReadOnlyList<BleDiscoveredDevice> devices = await discoveryTransport.ScanDevicesAsync(
            scanDuration,
            linkedCancellation.Token).ConfigureAwait(false);
        // 转换为领域 DTO，并按产品、配对、信号和名称形成稳定列表顺序。
        return devices
            // 每个 transport DTO 映射为不依赖 Windows API 的领域设备对象。
            .Select(device => new NearbyBluetoothDevice(
                device.DeviceId,
                device.DisplayName,
                device.IsPaired,
                device.RssiDbm,
                device.DisplayName.StartsWith("BPNN-FIT-", StringComparison.OrdinalIgnoreCase)))
            // 产品手柄排在其它蓝牙设备之前。
            .OrderByDescending(device => device.IsFitnessDevice)
            // 同类设备中已配对项优先，方便快速重连。
            .ThenByDescending(device => device.IsPaired)
            // 信号未知按最弱处理，数值越接近零表示设备越近。
            .ThenByDescending(device => device.RssiDbm ?? int.MinValue)
            // 最后按名称稳定排序，避免列表每次扫描无原因跳动。
            .ThenBy(device => device.DisplayName, StringComparer.CurrentCultureIgnoreCase)
            // 物化独立数组，transport 后续扫描不会修改当前 UI 列表。
            .ToArray();
    }

    /// <inheritdoc />
    public async Task ConnectToDeviceAsync(
        NearbyBluetoothDevice device,
        CancellationToken cancellationToken = default)
    {
        // 设备参数不能为空，避免空引用绕过产品前缀检查。
        ArgumentNullException.ThrowIfNull(device);
        // 已连接时必须先断开，禁止一次用户操作隐式切换两条 GATT 链路。
        if (IsConnected)
        {
            // 给设备页明确恢复动作。
            throw new InvalidOperationException("当前已有连接；请先断开，再选择另一台手柄。");
        }

        // 只允许扫描时识别为产品且名称仍匹配产品前缀的设备进入健身协议。
        if (!device.IsFitnessDevice || !device.DisplayName.StartsWith("BPNN-FIT-", StringComparison.OrdinalIgnoreCase))
        {
            // 拒绝误连耳机、鼠标和邻居传感器。
            throw new InvalidOperationException("所选蓝牙设备不是 BPNN 健身手柄，不能连接。");
        }

        // Windows DeviceInformation.Id 不能为空，否则 transport 无法精确打开用户所选设备。
        if (string.IsNullOrWhiteSpace(device.DeviceId))
        {
            // 拒绝空系统标识。
            throw new ArgumentException("所选蓝牙设备缺少 Windows 设备标识。", nameof(device));
        }

        // 在状态锁内保存用户选择，ConnectCore 只允许传入这一个 preferred ID。
        lock (_sync)
        {
            // 覆盖旧首选项；本次连接不得回退为随机第一台产品设备。
            _preferredDeviceId = device.DeviceId;
        }

        // 复用正式连接、Manifest 校验、订阅和快照恢复链。
        await ConnectAsync(cancellationToken).ConfigureAwait(false);
    }

    /// <inheritdoc />
    public async Task SyncTimeAsync(long utcUnixMilliseconds, short timezoneOffsetMinutes, CancellationToken cancellationToken = default)
    {
        // 编码经过范围检查的命令 6 TLV。
        byte[] tlv = DeviceConfigurationCodec.EncodeTimeSync(utcUnixMilliseconds, timezoneOffsetMinutes);
        // 等待设备 ACK；失败时调用方不得显示同步成功。
        _ = await ExecuteControlAsync(ControlCommandId.SyncTime, tlv, cancellationToken).ConfigureAwait(false);
    }

    /// <inheritdoc />
    public async Task SetProfileAsync(uint weightGrams, uint revision, CancellationToken cancellationToken = default)
    {
        // 编码克数和资料修订号。
        byte[] tlv = DeviceConfigurationCodec.EncodeProfile(weightGrams, revision);
        // 等待设备持久化或接受配置后的 ACK。
        _ = await ExecuteControlAsync(ControlCommandId.SetProfile, tlv, cancellationToken).ConfigureAwait(false);
    }

    /// <inheritdoc />
    public async Task SetGoalAsync(DeviceGoalKind kind, uint value, CancellationToken cancellationToken = default)
    {
        // 编码冻结的目标种类和值。
        byte[] tlv = DeviceConfigurationCodec.EncodeGoal(kind, value);
        // 等待设备 ACK，避免 PC 本地目标被误写成设备目标事实。
        _ = await ExecuteControlAsync(ControlCommandId.SetGoal, tlv, cancellationToken).ConfigureAwait(false);
    }

    /// <inheritdoc />
    public async Task SetPreferencesAsync(DevicePreferencesV1 preferences, CancellationToken cancellationToken = default)
    {
        // 编码亮度、反馈、熄屏、修订号和开发者模式。
        byte[] tlv = DeviceConfigurationCodec.EncodePreferences(preferences);
        // 等待设备 ACK 后才算设备同步完成。
        _ = await ExecuteControlAsync(ControlCommandId.SetPreferences, tlv, cancellationToken).ConfigureAwait(false);
    }

    /// <inheritdoc />
    public async Task SetRawStreamEnabledAsync(bool enabled, CancellationToken cancellationToken = default)
    {
        // 编码命令 11 的严格 0/1 TLV。
        byte[] tlv = DeviceConfigurationCodec.EncodeRawStreamEnabled(enabled);
        // 等待设备真正打开或关闭 RawStream。
        _ = await ExecuteControlAsync(ControlCommandId.SetRawStream, tlv, cancellationToken).ConfigureAwait(false);
        // ACK 成功后提交用户期望和当前连接状态。
        lock (_sync)
        {
            // 记录重连后是否恢复开发者流。
            _rawStreamDesired = enabled;
            // 当前连接设备已确认该状态。
            _rawStreamEnabled = enabled;
        }
    }

    /// <inheritdoc />
    public async Task DisconnectAsync(CancellationToken cancellationToken = default)
    {
        // 释放前或释放过程中都允许调用断开，保证资源可收敛。
        lock (_sync)
        {
            // 清除保持连接意图，transport 断线事件不得再次启动重连。
            _connectionDesired = false;
        }

        // 取消正在等待的指数退避循环。
        CancelReconnectLoop();
        // 等待其它连接或重连步骤退出临界区。
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            // 请求 transport 释放 GATT 特征、服务和设备。
            await _transport.DisconnectAsync(cancellationToken).ConfigureAwait(false);
            // 标记会话断开并通知 UI；重复断开不会重复发布。
            PublishDisconnected("用户主动断开真机蓝牙设备");
            // 清除全部重组状态，下一连接必须从分片 0 开始。
            ResetReassemblers();
            // 主动断开表示用户结束本次 RawStream 诊断，不在下次连接自动恢复。
            lock (_sync)
            {
                // 清除用户期望。
                _rawStreamDesired = false;
                // 链路断开后当前状态必为关闭。
                _rawStreamEnabled = false;
            }
            // 让所有等待控制响应的任务立即失败，不再等待 2 秒。
            FailPendingControls(new IOException("BLE 链路已主动断开。"));
            // 让所有等待摘要页的任务立即失败，不推进同步游标。
            FailPendingTransfers(new IOException("BLE 链路已主动断开。"));
        }
        finally
        {
            // 释放生命周期锁，允许对象后续再次 Connect。
            _lifecycleGate.Release();
        }
    }

    /// <inheritdoc />
    public async Task ForgetDeviceAsync(CancellationToken cancellationToken = default)
    {
        // 释放后不能访问 transport 或修改配对状态。
        ThrowIfDisposed();
        // 在断开前复制 Windows 设备 ID；用户可见名称不能用于系统取消配对。
        string? windowsDeviceId;
        // 使用状态锁与连接完成时写入 preferred ID 的过程同步。
        lock (_sync)
        {
            // 保存本次要取消配对的精确系统设备标识。
            windowsDeviceId = _preferredDeviceId;
        }

        // 先停止自动重连并释放全部 GATT 对象，Windows 才能稳定取消配对。
        await DisconnectAsync(cancellationToken).ConfigureAwait(false);

        // 曾经选中过设备时必须调用操作系统配对边界；从未连接时只清理本地占位状态。
        if (!string.IsNullOrWhiteSpace(windowsDeviceId))
        {
            // 正式 WinRT transport 实现该能力；缺失表示当前平台不能安全声称已取消系统配对。
            if (_transport is not IWindowsBlePairingManager pairingManager)
            {
                // 保留 preferred ID 供用户修复 transport 后重试，不能只清本地却伪装系统已忘记。
                throw new NotSupportedException("当前 Windows 蓝牙传输不支持取消系统配对。");
            }

            // 请求 Windows 使用精确设备 ID 取消配对；已取消配对由实现按幂等成功处理。
            await pairingManager.ForgetDeviceAsync(windowsDeviceId, cancellationToken).ConfigureAwait(false);
        }

        // 系统取消配对成功后清除全部设备特定缓存，下次连接必须重新扫描和验证能力清单。
        lock (_sync)
        {
            // 清除自动重连首选设备 ID。
            _preferredDeviceId = null;
            // 恢复未选择设备占位符，设备页不得继续显示旧名称。
            _deviceId = "BLE-UNSELECTED";
            // 清除旧设备权威状态，避免新设备连接前显示旧电量和 revision。
            _latestState = null;
            // 清除旧能力清单原始字节。
            _manifest = [];
            // 清除解析后的设备模型和能力摘要。
            _manifestInfo = null;
            // 清除信号强度诊断值。
            _rssiDbm = null;
            // 恢复 BLE 默认最大传输单元，未连接时诊断仍显示空值。
            _attMtu = 23;
            // 清除标准设备型号。
            _modelNumber = "未知";
            // 清除硬件修订号。
            _hardwareRevision = "未知";
            // 清除固件修订号。
            _firmwareRevision = "未知";
            // 忘记设备不允许下次连接恢复旧诊断原始流。
            _rawStreamDesired = false;
            // 当前链路已断开，原始流状态必为关闭。
            _rawStreamEnabled = false;
        }

        // 新设备诊断从零开始，旧设备重连次数不应污染新设备页面。
        Interlocked.Exchange(ref _reconnectCount, 0L);
        // 清除旧设备 CRC 错误累计。
        Interlocked.Exchange(ref _crcErrorCount, 0L);
        // 清除旧设备分片错误累计。
        Interlocked.Exchange(ref _fragmentErrorCount, 0L);
        // 清除所有特征半帧，防止旧设备数据与新设备第一帧拼接。
        ResetReassemblers();
    }

    /// <inheritdoc />
    public async Task<LiveState> GetSnapshotAsync(CancellationToken cancellationToken = default)
    {
        // 快照读取需要完整连接。
        EnsureConnected();
        // 使用 Uncached GATT 读取最新 LiveState 值。
        byte[] value = await _transport.ReadAsync(ProtocolConstants.LiveStateUuid, cancellationToken).ConfigureAwait(false);
        // 解码读取值；非连接恢复场景不得用旧 revision 覆盖新通知。
        ProcessGattValue(ProtocolConstants.LiveStateUuid, value, forceStateBaseline: false);

        // 在锁内返回最新权威状态。
        lock (_sync)
        {
            // 读取失败或通知尚未产生状态时给出明确错误。
            if (_latestState is null)
            {
                // 禁止 UI 使用本地猜测状态。
                throw new InvalidDataException("设备 LiveState 快照没有产生可用权威状态。");
            }

            // 返回不可变 LiveState 对象，调用者不能修改内部状态。
            return _latestState;
        }
    }

    /// <inheritdoc />
    public async Task StartAsync(CancellationToken cancellationToken = default)
    {
        // 发送幂等开始命令；重试复用同一 request_id。
        await ExecuteControlAsync(ControlCommandId.Start, ReadOnlyMemory<byte>.Empty, cancellationToken).ConfigureAwait(false);
        // 记录 PC 侧临时摘要的 UTC 起点；设备同步摘要仍是持久化权威。
        _startedAtUtc = DateTimeOffset.UtcNow;
        // 新会话清除旧 Stop 摘要，防止重复停止返回上一会话。
        _lastSummary = null;
        // 主动读取快照，减少开始 indication 与第一条周期通知之间的 UI 延迟。
        _ = await GetSnapshotAsync(cancellationToken).ConfigureAwait(false);
    }

    /// <inheritdoc />
    public async Task PauseAsync(CancellationToken cancellationToken = default)
    {
        // 发送幂等暂停命令。
        await ExecuteControlAsync(ControlCommandId.Pause, ReadOnlyMemory<byte>.Empty, cancellationToken).ConfigureAwait(false);
        // 读取暂停后的设备权威状态。
        _ = await GetSnapshotAsync(cancellationToken).ConfigureAwait(false);
    }

    /// <inheritdoc />
    public async Task ResumeAsync(CancellationToken cancellationToken = default)
    {
        // 发送幂等恢复命令。
        await ExecuteControlAsync(ControlCommandId.Resume, ReadOnlyMemory<byte>.Empty, cancellationToken).ConfigureAwait(false);
        // 读取恢复后的设备权威状态。
        _ = await GetSnapshotAsync(cancellationToken).ConfigureAwait(false);
    }

    /// <inheritdoc />
    public async Task<TrainingSessionSummary?> StopAsync(CancellationToken cancellationToken = default)
    {
        // 串行化完整停止事务，防止两个按钮事件在摘要生成前各发送不同 request_id。
        await _stopGate.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            // 已保存摘要时重复 Stop 直接返回同一对象，不再发送第二条结束命令。
            if (_lastSummary is not null)
            {
                // 返回同一摘要实例保证本地仓储幂等。
                return _lastSummary;
            }

            // 发送幂等停止命令；设备必须先持久化摘要再 ACK。
            await ExecuteControlAsync(ControlCommandId.Stop, ReadOnlyMemory<byte>.Empty, cancellationToken).ConfigureAwait(false);
            // 读取最终状态快照，恢复断线期间最后累计值。
            LiveState snapshot = await GetSnapshotAsync(cancellationToken).ConfigureAwait(false);
            // 未记录开始 UTC 时用当前时间减去设备单调时长，避免生成结束早于开始的摘要。
            DateTimeOffset startedAt = _startedAtUtc == default
                ? DateTimeOffset.UtcNow - TimeSpan.FromMilliseconds(snapshot.ElapsedMilliseconds)
                : _startedAtUtc;
            // 使用当前 UTC 作为 PC 临时摘要结束时间；正式历史同步应以设备摘要 UTC 覆盖。
            DateTimeOffset endedAt = DateTimeOffset.UtcNow;
            // 构造不重复计数的临时摘要；动作明细留空，后续 TransferData 同步提供完整 11 类指标。
            _lastSummary = new TrainingSessionSummary(
                DeviceId,
                snapshot.SessionSequence,
                startedAt,
                endedAt,
                snapshot.ElapsedMilliseconds,
                snapshot.CaloriesMilliKcal,
                "用户通过 Windows BLE 停止",
                []);
            // 返回本次停止产生的稳定摘要。
            return _lastSummary;
        }
        finally
        {
            // 允许后续重复停止读取同一摘要。
            _stopGate.Release();
        }
    }

    /// <inheritdoc />
    public async Task<SessionTransferPage> PullSessionSummariesAsync(
        uint cursorSessionSequence,
        ushort pageSize = SessionTransferCodec.MaxPageSize,
        CancellationToken cancellationToken = default)
    {
        // 只有完成 Manifest、订阅和权威快照恢复的连接才允许同步历史。
        EnsureConnected();
        // codec 会再次校验页大小；这里先串行化完整请求事务。
        await _transferGate.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            // 分配与控制命令共享的全局非零 request_id，避免诊断日志出现重复编号。
            uint requestId = unchecked(++_nextRequestId);
            // 0 是未分配哨兵，回绕时跳到 1。
            if (requestId == 0U)
            {
                // 保存回绕后的实际请求号。
                requestId = ++_nextRequestId;
            }

            // 编码固定 12 字节 LIST 请求。
            byte[] requestPayload = SessionTransferCodec.EncodeListRequest(requestId, cursorSessionSequence, pageSize);
            // 分配 Transfer Control 独立 sequence；重试复用同一完整帧。
            ushort sequence = unchecked(++_transferSequence);
            // 构造类型 5 逻辑帧，单调时间取进程开机毫秒低 32 位。
            BleLogicalFrame logicalFrame = new(
                ProtocolConstants.ProtocolMajor,
                ProtocolConstants.ProtocolMinor,
                (byte)ProtocolMessageType.TransferRequest,
                0,
                sequence,
                unchecked((uint)Environment.TickCount64),
                requestPayload);
            // 编码一次 CRC16；两次可靠重试必须保持字节完全相同。
            byte[] encodedFrame = BleFrameCodec.Encode(logicalFrame);
            // 创建响应完成源和单读者数据通道。
            PendingTransferRequest pending = new(
                new TaskCompletionSource<SessionTransferResponseV1>(TaskCreationOptions.RunContinuationsAsynchronously),
                Channel.CreateUnbounded<SessionTransferDataV1>(new UnboundedChannelOptions
                {
                    // 本方法是唯一读者。
                    SingleReader = true,
                    // BLE 通知泵是唯一写者。
                    SingleWriter = true,
                    // 禁止在 BLE 回调路径同步执行拉取后续代码。
                    AllowSynchronousContinuations = false,
                }));
            // request_id 冲突表示内部回绕或未完成请求泄漏。
            if (!_pendingTransfers.TryAdd(requestId, pending))
            {
                // 拒绝无法安全匹配的数据页。
                throw new InvalidOperationException($"会话同步 request_id {requestId} 已存在。" );
            }

            try
            {
                // 最多发送首次和一次同 ID 重试；设备会重放相同冻结页。
                for (int attempt = 0; attempt < 2; attempt++)
                {
                    // 按协商 MTU 写入 Transfer Control 0005。
                    await WriteLogicalFrameAsync(
                        ProtocolConstants.TransferControlUuid,
                        encodedFrame,
                        sequence,
                        cancellationToken).ConfigureAwait(false);

                    try
                    {
                        // 等待类型 6 indication；生产超时沿用控制点 2 秒合同。
                        SessionTransferResponseV1 response = await pending.Response.Task
                            .WaitAsync(_controlTimeout, cancellationToken).ConfigureAwait(false);
                        // 设备业务状态非 OK 时给出稳定错误，不猜测摘要。
                        if (response.Status != SessionTransferStatus.Ok)
                        {
                            // BUSY、NOT_FOUND 和存储错误都由调用方明确处理。
                            throw new InvalidOperationException($"设备拒绝会话同步：status={response.Status}。" );
                        }

                        // 空页必须声明没有数据，直接返回当前游标。
                        if (response.ItemCount == 0)
                        {
                            // 返回空的只读数组。
                            return new SessionTransferPage(
                                response.NextCursorSessionSequence,
                                response.TotalCount,
                                response.IsEnd,
                                []);
                        }

                        // 用页内 index 去重；重试重放的重复 notification 不产生重复摘要。
                        Dictionary<ushort, SessionTransferDataV1> items = new();
                        // 收齐 response.item_count 条不同索引。
                        while (items.Count < response.ItemCount)
                        {
                            // 每条数据最多等待同一协议超时，超时进入一次同 ID 重试。
                            SessionTransferDataV1 item = await pending.Data.Reader.ReadAsync(cancellationToken).AsTask()
                                .WaitAsync(_controlTimeout, cancellationToken).ConfigureAwait(false);
                            // 只接受匹配响应页大小和设备总数的数据。
                            if ((item.ItemCount != response.ItemCount) || (item.TotalCount != response.TotalCount))
                            {
                                // 拒绝跨页或损坏数据。
                                throw new InvalidDataException("TransferData 页大小或总数与 TransferResponse 不一致。" );
                            }

                            // 相同索引重放只保留一份；session_seq 最终还由仓储复合键幂等。
                            items.TryAdd(item.ItemIndex, item);
                        }

                        // 按 item_index 还原设备旧到新顺序并转为领域摘要。
                        TrainingSessionSummary[] summaries = items
                            .OrderBy(entry => entry.Key)
                            .Select(entry => entry.Value.Summary.ToTrainingSessionSummary(DeviceId))
                            .ToArray();
                        // 页尾数据必须与响应 item_count 一致。
                        SessionTransferDataV1 lastItem = items[checked((ushort)(response.ItemCount - 1))];
                        if (!lastItem.IsLastInPage || (lastItem.IsEnd != response.IsEnd))
                        {
                            // 防止缺页尾或响应/数据终点矛盾。
                            throw new InvalidDataException("TransferData 页尾标志与 TransferResponse 不一致。" );
                        }

                        // 返回可由当前原子 JSON 仓储逐条按复合键幂等写入的一页。
                        return new SessionTransferPage(
                            response.NextCursorSessionSequence,
                            response.TotalCount,
                            response.IsEnd,
                            summaries);
                    }
                    catch (TimeoutException) when (attempt == 0)
                    {
                        // 首次超时后循环，用完全相同 request_id、sequence 和帧字节重试一次。
                    }
                }

                // 两次均未收齐响应和数据，结果未知；不得推进本地游标。
                throw new TimeoutException("会话摘要同步两次发送后仍未收齐响应和数据。" );
            }
            finally
            {
                // 移除等待者；迟到 notification 将按未知 request_id 安全忽略。
                _pendingTransfers.TryRemove(requestId, out _);
            }
        }
        finally
        {
            // 允许下一页请求。
            _transferGate.Release();
        }
    }

    /// <summary>释放通知泵、重连、GATT 对象和同步原语。</summary>
    public async ValueTask DisposeAsync()
    {
        // 锁内实现幂等释放。
        lock (_sync)
        {
            // 重复释放直接返回。
            if (_disposed)
            {
                // 不重复释放 transport 和信号量。
                return;
            }

            // 标记释放并清除保持连接意图。
            _disposed = true;
            // 释放期间禁止自动重连。
            _connectionDesired = false;
        }

        // 停止重连退避。
        CancelReconnectLoop();
        // 停止后台通知泵。
        _lifetimeCancellation.Cancel();
        // 解除 transport 回调，避免释放后继续写 Channel。
        _transport.ValueReceived -= OnTransportValueReceived;
        // 解除断线回调，避免主动释放触发新任务。
        _transport.Disconnected -= OnTransportDisconnected;
        // 完成 Channel 写端，使通知泵在已排队消息处理完后退出。
        _notificationChannel.Writer.TryComplete();

        try
        {
            // 主动释放 transport 当前 GATT 连接。
            await _transport.DisconnectAsync(CancellationToken.None).ConfigureAwait(false);
        }
        catch
        {
            // Dispose 必须尽力收敛；系统蓝牙关闭导致的断开异常不阻止后续资源释放。
        }

        try
        {
            // 等待通知泵观察取消或 Channel 完成。
            await _notificationPumpTask.ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // 生命周期取消是正常退出路径。
        }

        // 让尚未完成的控制调用得到释放异常。
        FailPendingControls(new ObjectDisposedException(nameof(WindowsBleDeviceSession)));
        // 让尚未完成的会话同步立即结束。
        FailPendingTransfers(new ObjectDisposedException(nameof(WindowsBleDeviceSession)));
        // 释放具体 WinRT 或 fake transport 持有资源。
        await _transport.DisposeAsync().ConfigureAwait(false);
        // 释放生命周期取消源。
        _lifetimeCancellation.Dispose();
        // 释放连接串行化信号量。
        _lifecycleGate.Dispose();
        // 释放控制串行化信号量。
        _commandGate.Dispose();
        // 释放会话同步串行化信号量。
        _transferGate.Dispose();
        // 释放完整停止事务信号量。
        _stopGate.Dispose();
        // 释放重连取消源残留对象。
        _reconnectCancellation?.Dispose();
    }

    // 执行扫描、GATT 发现、订阅、manifest 和强制基线快照恢复。
    private async Task ConnectCoreAsync(bool isReconnect, CancellationToken cancellationToken)
    {
        // 串行化显式连接与后台重连。
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            // 若另一路已完成连接，重复 Connect 只刷新快照而不创建第二条链路。
            if (IsConnected)
            {
                // 重复连接仍读取权威快照，恢复 UI 状态。
                _ = await GetSnapshotAsync(cancellationToken).ConfigureAwait(false);
                // 连接已经可用，无需继续扫描。
                return;
            }

            // 在状态锁内复制用户选择或上次连接的 Windows 设备 ID。
            string? preferredDeviceId;
            // 与设备页选择和忘记设备操作同步读取。
            lock (_sync)
            {
                // 保存当前连接周期的固定目标，连接期间不再受列表选择变化影响。
                preferredDeviceId = _preferredDeviceId;
            }

            // 请求 transport 按固定目标扫描/连接、系统配对并发现完整 0001～0007 GATT 表。
            BleConnectedDevice connected = await _transport.ScanAndConnectAsync(preferredDeviceId, cancellationToken).ConfigureAwait(false);
            // 在状态锁内保存系统设备 ID，后续重连仍锁定同一块手柄。
            lock (_sync)
            {
                // 使用 transport 返回的规范 Windows ID 覆盖扫描快照值。
                _preferredDeviceId = connected.DeviceId;
            }
            // ATT MTU 必须能放下 8 字节分片包络和至少 1 字节数据。
            if (connected.AttMtu <= 3 + ProtocolConstants.FragmentHeaderSize)
            {
                // 断开不兼容链路，防止分片容量为零或负数。
                await _transport.DisconnectAsync(CancellationToken.None).ConfigureAwait(false);
                // 抛出协议资源错误供 UI 诊断。
                throw new InvalidOperationException($"设备 ATT MTU {connected.AttMtu} 太小，无法承载协议分片。");
            }

            // Uncached 读取 Manifest；每次重连都刷新固件、模型、类别表和 boot 信息。
            byte[] manifest = await _transport.ReadAsync(ProtocolConstants.ManifestUuid, cancellationToken).ConfigureAwait(false);
            // 空 manifest 无法验证协议兼容性。
            if (manifest.Length == 0)
            {
                // 拒绝进入可控制状态。
                throw new InvalidDataException("设备 Manifest 为空，禁止执行训练控制。");
            }

            // 在订阅任何通知前严格解析并校验协议 1.0、297 维特征、11 类 CRC 和必需能力位。
            ManifestV1 manifestInfo = ManifestV1Codec.Parse(manifest);

            // 先订阅控制 indication，确保后续快照请求和控制 ACK 不丢失。
            await _transport.SubscribeAsync(ProtocolConstants.ControlPointUuid, useIndication: true, cancellationToken).ConfigureAwait(false);
            // 订阅权威 LiveState notification。
            await _transport.SubscribeAsync(ProtocolConstants.LiveStateUuid, useIndication: false, cancellationToken).ConfigureAwait(false);
            // 订阅低延迟 Event notification；事件不直接修改次数。
            await _transport.SubscribeAsync(ProtocolConstants.EventUuid, useIndication: false, cancellationToken).ConfigureAwait(false);
            // 订阅会话同步可靠响应 indication。
            await _transport.SubscribeAsync(ProtocolConstants.TransferControlUuid, useIndication: true, cancellationToken).ConfigureAwait(false);
            // 订阅摘要页数据 notification；原始日志仍按用户操作另行请求。
            await _transport.SubscribeAsync(ProtocolConstants.TransferDataUuid, useIndication: false, cancellationToken).ConfigureAwait(false);
            // 订阅开发者 RawStream notification；固件只在命令 11 开启后发布，默认没有带宽开销。
            await _transport.SubscribeAsync(ProtocolConstants.RawStreamUuid, useIndication: false, cancellationToken).ConfigureAwait(false);
            // 清空旧连接的分片状态，避免把断线前半帧拼到新连接。
            ResetReassemblers();
            // 清除旧连接快照；新连接必须真正解码当前 LiveState 后才能提交已连接状态。
            lock (_sync)
            {
                // ESP32 重启后 revision 可能归零，旧状态不能掩盖读取失败。
                _latestState = null;
            }
            // 读取完整权威快照；连接恢复允许新 boot 的 revision 从较小值重新建立基线。
            byte[] snapshotValue = await _transport.ReadAsync(ProtocolConstants.LiveStateUuid, cancellationToken).ConfigureAwait(false);
            // 解码并强制设置当前连接基线。
            ProcessGattValue(ProtocolConstants.LiveStateUuid, snapshotValue, forceStateBaseline: true);

            // 初始读取必须产生完整 LiveState；单片不完整或坏帧时禁止假连接。
            lock (_sync)
            {
                // 没有状态表示 Read 返回的数据未构成有效完整帧。
                if (_latestState is null)
                {
                    // 报告快照恢复失败。
                    throw new InvalidDataException("连接读取未产生完整 LiveState 快照。" );
                }
            }

            // 锁内提交完整连接状态，避免 UI 在订阅前误以为链路可用。
            lock (_sync)
            {
                // 保存用户可见设备名；空名回退 Windows 设备 ID。
                _deviceId = string.IsNullOrWhiteSpace(connected.DisplayName) ? connected.DeviceId : connected.DisplayName;
                // 保存本连接实际 ATT MTU，后续分片容量严格使用 mtu-3-8。
                _attMtu = connected.AttMtu;
                // 保存独立 manifest 副本。
                _manifest = manifest.ToArray();
                // 只在完整连接提交阶段保存兼容 Manifest，失败连接不会污染诊断摘要。
                _manifestInfo = manifestInfo;
                // 保存最近扫描 RSSI。
                _rssiDbm = connected.RssiDbm;
                // 保存标准设备型号。
                _modelNumber = connected.ModelNumber;
                // 保存硬件修订号。
                _hardwareRevision = connected.HardwareRevision;
                // 保存固件修订号。
                _firmwareRevision = connected.FirmwareRevision;
                // 标记会话层连接完成。
                _isConnected = true;
                // 新连接尚未恢复命令 11，当前 RawStream 先标记关闭。
                _rawStreamEnabled = false;
            }

            // 每次首次连接或自动重连都同步当前 PC UTC 和时区，RTC 不使用旧开机时间。
            await SynchronizeCurrentClockAsync(cancellationToken).ConfigureAwait(false);
            // 读取用户在断线前的 RawStream 期望，决定是否恢复开发者诊断流。
            bool restoreRawStream;
            // 使用状态锁读取期望值。
            lock (_sync)
            {
                // 仅显式打开过的诊断流才自动恢复。
                restoreRawStream = _rawStreamDesired;
            }
            // 断线前打开时发送同一命令 11 恢复；默认 false 不发送额外命令。
            if (restoreRawStream)
            {
                // 命令 ACK 成功后方法会恢复 enabled 标志。
                await SetRawStreamEnabledAsync(true, cancellationToken).ConfigureAwait(false);
            }

            // 在锁外通知 WPF；回调线程不保证为 UI 线程。
            ConnectionChanged?.Invoke(
                this,
                new DeviceConnectionChangedEventArgs(true, isReconnect ? "真机蓝牙已自动重连并恢复设备能力清单与快照" : "真机蓝牙已连接并恢复设备能力清单与快照"));
        }
        catch
        {
            // 连接任一步失败都释放半初始化 GATT 对象。
            await SafeTransportDisconnectAsync().ConfigureAwait(false);
            // 清除会话层连接标志但保留 preferred ID 用于下次重试。
            PublishDisconnected(isReconnect ? "真机蓝牙自动重连失败" : "真机蓝牙连接失败");
            // 把原始异常交给显式连接或重连循环记录。
            throw;
        }
        finally
        {
            // 释放生命周期锁，允许断开或下一次退避重连。
            _lifecycleGate.Release();
        }
    }

    // 编码并发送控制命令；2 秒无 indication 时只重发一次完全相同的 request_id 和逻辑帧。
    private async Task<ControlPointResponse> ExecuteControlAsync(
        ControlCommandId commandId,
        ReadOnlyMemory<byte> tlvData,
        CancellationToken cancellationToken)
    {
        // 只有完整连接状态允许发送训练控制。
        EnsureConnected();
        // 串行化命令，避免 UI 快速点击造成状态竞争。
        await _commandGate.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            // 原子分配非零 request_id；自然溢出到 0 时再跳到 1。
            uint requestId = unchecked(++_nextRequestId);
            // 0 保留为无请求，因此回绕后立即分配 1。
            if (requestId == 0)
            {
                // 更新内部计数和本次请求 ID。
                requestId = ++_nextRequestId;
            }

            // 编码固定 request_id、命令版本和可选 TLV。
            byte[] requestPayload = ControlPointCodec.EncodeRequest(requestId, commandId, tlvData.Span);
            // 分配 16 位逻辑帧 sequence；同一次重试复用编码结果和 sequence。
            ushort sequence = unchecked(++_controlSequence);
            // 使用进程单调毫秒低 32 位，不把系统 UTC 写入单调字段。
            uint monotonicMilliseconds = unchecked((uint)Environment.TickCount64);
            // 构造协议 v1 控制请求帧。
            BleLogicalFrame logicalFrame = new(
                ProtocolConstants.ProtocolMajor,
                ProtocolConstants.ProtocolMinor,
                (byte)ProtocolMessageType.ControlRequest,
                0,
                sequence,
                monotonicMilliseconds,
                requestPayload);
            // 编码含 CRC16 的完整逻辑帧；两次发送必须复用同一字节数组。
            byte[] encodedFrame = BleFrameCodec.Encode(logicalFrame);
            // 创建异步响应完成源；RunContinuationsAsynchronously 防止 BLE 回调内联执行 UI 后续逻辑。
            TaskCompletionSource<ControlPointResponse> completion = new(TaskCreationOptions.RunContinuationsAsynchronously);
            // 保存命令 ID，响应必须同时匹配 request_id 和 command_id。
            PendingControlRequest pending = new(commandId, completion);
            // request_id 理论上不会与当前串行命令冲突，仍检查防止回绕覆盖未完成项。
            if (!_pendingControls.TryAdd(requestId, pending))
            {
                // 拒绝无法安全关联响应的命令。
                throw new InvalidOperationException($"控制 request_id {requestId} 已存在未完成请求。");
            }

            try
            {
                // 最多发送两次：首次和一次协议允许的同 ID 重试。
                for (int attempt = 0; attempt < 2; attempt++)
                {
                    // 按当前协商 MTU 分片并顺序写入 Control Point。
                    await WriteLogicalFrameAsync(ProtocolConstants.ControlPointUuid, encodedFrame, sequence, cancellationToken).ConfigureAwait(false);
                    // 创建本轮 2 秒可取消超时任务。
                    Task timeoutTask = _delayAsync(_controlTimeout, cancellationToken);
                    // 等待 indication 或超时，二者任一先完成。
                    Task completed = await Task.WhenAny(completion.Task, timeoutTask).ConfigureAwait(false);
                    // indication 先到时读取结果并结束重试。
                    if (completed == completion.Task)
                    {
                        // 等待并返回已匹配命令的响应。
                        ControlPointResponse response = await completion.Task.ConfigureAwait(false);
                        // 非零 status 或 error_code 均表示设备拒绝命令。
                        if ((response.Status != 0) || (response.ErrorCode != 0))
                        {
                            // 抛出包含设备错误码的业务异常，不在 PC 猜测执行结果。
                            throw new InvalidOperationException($"设备拒绝命令 {commandId}：status={response.Status}，error={response.ErrorCode}。" );
                        }

                        // 返回成功响应，state_revision 可用于诊断命令后的设备版本。
                        return response;
                    }

                    // 超时任务完成后先传播外部取消，而不是错误地执行重试。
                    cancellationToken.ThrowIfCancellationRequested();
                    // attempt=0 时继续并复用同 request_id；attempt=1 后循环结束并抛超时。
                }

                // 两次发送均未收到 indication，命令执行结果未知，UI 不得自行改变状态。
                throw new TimeoutException($"命令 {commandId} 两次发送均未在 {_controlTimeout.TotalSeconds:0.###} 秒内收到 indication。" );
            }
            finally
            {
                // 无论成功、拒绝、取消或超时都移除等待者，防止字典增长。
                _pendingControls.TryRemove(requestId, out _);
            }
        }
        finally
        {
            // 允许下一条控制命令发送。
            _commandGate.Release();
        }
    }

    // 按 transport 协商 MTU 把完整逻辑帧分片并使用有响应写入顺序发送。
    private async Task WriteLogicalFrameAsync(
        Guid characteristicUuid,
        byte[] encodedFrame,
        ushort sequence,
        CancellationToken cancellationToken)
    {
        // 通过 transport 不暴露连接描述属性，因此使用最近连接 MTU 的安全默认 23 并由 WinRT transport Write 保持实际限制。
        // 该字段由 GetCurrentAttMtu 读取会话保存值，避免在此依赖 WinRT 类型。
        ushort attMtu = GetCurrentAttMtu();
        // 计算分片列表；每片结构为 8 字节包络加最多 mtu-11 字节帧数据。
        IReadOnlyList<byte[]> fragments = BleFragmentCodec.Fragment(encodedFrame, attMtu, sequence);

        // 按索引顺序写入同一 GATT 特征，Windows 有响应写完成后才发送下一片。
        foreach (byte[] fragment in fragments)
        {
            // Control Point 使用 WriteWithResponse，保证 ATT 层接收每个分片。
            await _transport.WriteAsync(characteristicUuid, fragment, withResponse: true, cancellationToken).ConfigureAwait(false);
        }

    }

    // 当前连接 ATT MTU；字段在 ConnectCore 完成 transport 连接后写入。
    private ushort _attMtu = 23;

    // 锁内读取 ATT MTU，返回值单位为字节且至少为 12。
    private ushort GetCurrentAttMtu()
    {
        // 使用状态锁防止重连同时更新 MTU。
        lock (_sync)
        {
            // 返回最近连接协商值。
            return _attMtu;
        }
    }

    // transport 收到 GATT 值时只入队，避免阻塞系统 BLE 回调线程。
    private void OnTransportValueReceived(object? sender, BleGattValueReceivedEventArgs eventArgs)
    {
        // 尝试无阻塞写入无界 Channel；Dispose 后 writer 已完成则安全丢弃。
        _notificationChannel.Writer.TryWrite(eventArgs);
    }

    // transport 意外断开时启动异步处理；事件签名不能直接 await。
    private void OnTransportDisconnected(object? sender, BleTransportDisconnectedEventArgs eventArgs)
    {
        // 后台处理断线、失败等待命令并启动指数退避；异常由内部收敛。
        _ = HandleUnexpectedDisconnectAsync(eventArgs.Reason);
    }

    // 顺序消费所有特征通知，单个坏帧不会终止后续状态恢复。
    private async Task RunNotificationPumpAsync(CancellationToken cancellationToken)
    {
        // 持续读取直到 Dispose 完成 Channel 或生命周期取消。
        await foreach (BleGattValueReceivedEventArgs eventArgs in _notificationChannel.Reader.ReadAllAsync(cancellationToken).ConfigureAwait(false))
        {
            try
            {
                // 按来源 UUID 重组和解释逻辑消息；普通通知不得强制降低 revision 基线。
                ProcessGattValue(eventArgs.CharacteristicUuid, eventArgs.Value.Span, forceStateBaseline: false);
            }
            catch (Exception exception) when (exception is InvalidDataException or InvalidOperationException)
            {
                // 坏帧只作为链路诊断丢弃，不能让后台通知泵永久退出。
                System.Diagnostics.Debug.WriteLine($"丢弃 BLE 通知：{exception.Message}");
            }
        }
    }

    // 接受完整逻辑帧或单/多片 GATT Value，并在完成后分发业务消息。
    private void ProcessGattValue(Guid characteristicUuid, ReadOnlySpan<byte> value, bool forceStateBaseline)
    {
        // 有些 WinRT Read 返回完整长值，先直接尝试逻辑帧解码。
        if (BleFrameCodec.TryDecode(value, out BleLogicalFrame? directFrame, out ProtocolDecodeError directError))
        {
            // 直接帧已经通过 CRC，立即分发。
            DispatchLogicalFrame(directFrame!, forceStateBaseline);
            // 完整帧处理结束。
            return;
        }

        // 看起来是完整逻辑帧但 CRC 损坏时直接计数并拒绝，不能误送分片重组器二次计数。
        if (directError == ProtocolDecodeError.BadCrc)
        {
            // 原子增加 CRC 错误计数。
            _ = Interlocked.Increment(ref _crcErrorCount);
            // 抛出可诊断错误，后台通知泵会丢弃当前值并继续。
            throw new InvalidDataException("BLE 完整逻辑帧 CRC 校验失败。" );
        }

        // 未订阅特征没有重组器，禁止把 Manifest 或未知特征误当分片。
        if (!_reassemblers.TryGetValue(characteristicUuid, out BleFrameReassembler? reassembler))
        {
            // 报告来源 UUID，便于发现 GATT 映射错误。
            throw new InvalidDataException($"特征 {characteristicUuid} 的值既不是完整逻辑帧，也没有配置分片重组器。" );
        }

        // 把当前片推入该特征独立重组器。
        FragmentPushStatus status = reassembler.Push(value, out byte[]? completeFrame, out ProtocolDecodeError error);
        // 未完成表示等待同特征下一片，当前不产生业务事件。
        if (status == FragmentPushStatus.AcceptedIncomplete)
        {
            // 正常返回，不能误报数据错误。
            return;
        }

        // 拒绝状态表示包络、顺序、长度或 CRC 错误。
        if ((status == FragmentPushStatus.Rejected) || (completeFrame is null))
        {
            // CRC 错误与分片包络/顺序错误使用不同计数，便于判断根因。
            RecordProtocolError(error);
            // 抛出可记录错误，通知泵会丢弃并继续。
            throw new InvalidDataException($"BLE 分片重组失败：{error}。" );
        }

        // 重组器已经校验 CRC，再解码字段对象。
        if (!BleFrameCodec.TryDecode(completeFrame, out BleLogicalFrame? frame, out ProtocolDecodeError frameError))
        {
            // 理论上的二次解码错误也必须进入统计。
            RecordProtocolError(frameError);
            // 理论上不会发生，仍保持双重边界检查。
            throw new InvalidDataException($"完整 BLE 逻辑帧解码失败：{frameError}。" );
        }

        // 分发经过协议校验的帧。
        DispatchLogicalFrame(frame!, forceStateBaseline);
    }

    // 按 message_type 解码控制响应、权威状态或低延迟事件。
    private void DispatchLogicalFrame(BleLogicalFrame frame, bool forceStateBaseline)
    {
        // 主版本不一致时禁止业务控制和错误状态映射。
        if (frame.ProtocolMajor != ProtocolConstants.ProtocolMajor)
        {
            // 抛出兼容性错误，连接层应只保留 manifest/导出能力。
            throw new InvalidDataException($"设备协议主版本 {frame.ProtocolMajor} 与 PC {ProtocolConstants.ProtocolMajor} 不兼容。" );
        }

        // 按 v1 消息类型处理。
        switch ((ProtocolMessageType)frame.MessageType)
        {
            // 控制 indication 完成对应 request_id 等待者。
            case ProtocolMessageType.ControlResponse:
                // 解码固定响应头和可选 TLV。
                if (!ControlPointCodec.TryDecodeResponse(frame.Payload.Span, out ControlPointResponse? response, out string? responseError))
                {
                    // 坏响应不能错误唤醒命令。
                    throw new InvalidDataException(responseError ?? "控制响应解码失败。" );
                }

                // 只处理当前仍在等待的 request_id；迟到或重复 indication 安全忽略。
                if (_pendingControls.TryGetValue(response!.RequestId, out PendingControlRequest? pending) &&
                    (pending.CommandId == response.CommandId))
                {
                    // 完成等待任务；重复 indication 的 TrySetResult 返回 false 且无副作用。
                    pending.Completion.TrySetResult(response);
                }

                // 控制响应不直接伪造 LiveState，等待通知或显式快照读取。
                break;

            // 权威实时状态更新 UI 和本地最新快照。
            case ProtocolMessageType.LiveState:
                // 解码固定 30 字节 payload。
                if (!LiveStateCodec.TryDecode(frame.Payload.Span, out LiveState? state, out string? stateError))
                {
                    // 非法状态不得进入领域对象。
                    throw new InvalidDataException(stateError ?? "LiveState 解码失败。" );
                }

                // 按 revision 规则接受新状态；重连初始读取可强制建立新 boot 基线。
                AcceptAuthoritativeState(state!, forceStateBaseline);
                // 状态处理完成。
                break;

            // Event 只触发动画和诊断，绝不在 PC 自行增加次数或卡路里。
            case ProtocolMessageType.Event:
                // 按固定 36 字节布局解码，禁止 UI 自行猜测字段偏移。
                if (!EventV1Codec.TryDecode(frame.Payload.Span, out DeviceEventV1? deviceEvent, out string? eventError))
                {
                    // 坏事件只由通知泵记录并丢弃，权威 LiveState 不受影响。
                    throw new InvalidDataException(eventError ?? "EventV1 解码失败。" );
                }

                // 发布结构化事件和原始字节；应用只把它用于动画或提示。
                ProtocolEventReceived?.Invoke(
                    this,
                    new DeviceProtocolEventEventArgs(
                        frame.Sequence,
                        frame.MonotonicMilliseconds,
                        deviceEvent!,
                        frame.Payload));
                // 事件处理完成。
                break;

            // TransferResponse 完成对应 request_id 的页响应等待者。
            case ProtocolMessageType.TransferResponse:
                // 严格解码固定 16 字节响应。
                if (!SessionTransferCodec.TryDecodeResponse(frame.Payload.Span, out SessionTransferResponseV1? transferResponse, out string? transferResponseError))
                {
                    // 坏响应不能推进本地游标。
                    throw new InvalidDataException(transferResponseError ?? "TransferResponse 解码失败。" );
                }

                // 只完成当前存在的等待者；迟到或重复响应安全忽略。
                if (_pendingTransfers.TryGetValue(transferResponse!.RequestId, out PendingTransferRequest? responsePending))
                {
                    // 重复 indication 的 TrySetResult 无副作用。
                    responsePending.Response.TrySetResult(transferResponse);
                }

                // 响应处理完成。
                break;

            // TransferData 放入对应请求的单读者通道。
            case ProtocolMessageType.TransferData:
                // 严格解码固定 80 字节数据及 64 字节摘要。
                if (!SessionTransferCodec.TryDecodeData(frame.Payload.Span, out SessionTransferDataV1? transferData, out string? transferDataError))
                {
                    // 坏数据不得写入历史库。
                    throw new InvalidDataException(transferDataError ?? "TransferData 解码失败。" );
                }

                // 只接收当前页 request_id；迟到页安全丢弃。
                if (_pendingTransfers.TryGetValue(transferData!.RequestId, out PendingTransferRequest? dataPending))
                {
                    // 无界小页通道写入不会阻塞 BLE 通知泵。
                    dataPending.Data.Writer.TryWrite(transferData);
                }

                // 数据处理完成。
                break;

            // RawStream 只在显式开发者开关成功后解码并发布，不进入历史仓储。
            case ProtocolMessageType.RawStream:
                // 关闭状态下迟到或设备错误发布的原始数据必须丢弃。
                if (!IsRawStreamEnabled)
                {
                    // 不保存、不转发关闭后的 RawStream。
                    break;
                }

                // 严格解码固定 22 字节 sample_index、单调时间、六轴和质量位。
                if (!RawStreamV1Codec.TryDecode(frame.Payload.Span, out RawImuSampleV1? rawSample, out string? rawError))
                {
                    // 坏原始样本不得进入诊断图或文件。
                    throw new InvalidDataException(rawError ?? "RawStream v1 解码失败。" );
                }

                // 在通知泵线程发布不可变样本；ViewModel 必须切回 WPF Dispatcher。
                RawSampleReceived?.Invoke(this, new RawImuSampleReceivedEventArgs(rawSample!));
                // 原始样本处理结束。
                break;

            // 其它消息由会话同步或开发者原始流模块处理，本设备会话安全忽略。
            default:
                // 不把未知/其它类型误解释为 LiveState。
                break;
        }
    }

    // 仅接受设备权威 revision 增长状态；重连首帧可重建较小 revision 基线。
    private void AcceptAuthoritativeState(LiveState state, bool forceStateBaseline)
    {
        // 记录是否需要在锁外发布状态事件。
        bool accepted;

        lock (_sync)
        {
            // 强制基线、首次状态或严格更大 revision 才能覆盖当前状态。
            accepted = forceStateBaseline || (_latestState is null) || (state.StateRevision > _latestState.StateRevision);
            // 重复或乱序旧状态不得使 UI 回退。
            if (!accepted)
            {
                // 保持当前权威状态不变。
                return;
            }

            // 保存新的不可变状态对象。
            _latestState = state;
        }

        // 在锁外通知订阅者，避免 UI 回调重入造成死锁。
        StateChanged?.Invoke(this, state);
    }

    // 处理 transport 意外断开，清理当前帧和命令并启动唯一重连循环。
    private Task HandleUnexpectedDisconnectAsync(string reason)
    {
        // 标记断开并通知 UI；重复系统事件只发布一次。
        PublishDisconnected(reason);
        // 清除半帧，重连后必须从 fragment_index 0 开始。
        ResetReassemblers();
        // 链路丢失后未完成命令结果未知，立即失败而不是错误重发到旧连接。
        FailPendingControls(new IOException(reason));
        // 链路断开后会话页不得继续等待旧 GATT 对象。
        FailPendingTransfers(new IOException(reason));

        lock (_sync)
        {
            // 主动断开或 Dispose 后禁止自动重连。
            if (!_connectionDesired || _disposed)
            {
                // 返回已完成任务，事件处理结束。
                return Task.CompletedTask;
            }

            // 已有运行中的重连循环时复用，避免多次断线事件创建并发扫描。
            if ((_reconnectLoopTask is not null) && !_reconnectLoopTask.IsCompleted)
            {
                // 返回现有任务供诊断使用。
                return _reconnectLoopTask;
            }

            // 释放旧取消源并创建本轮独立取消源。
            _reconnectCancellation?.Dispose();
            // 与整个会话生命周期链接，Dispose 会立即停止退避。
            _reconnectCancellation = CancellationTokenSource.CreateLinkedTokenSource(_lifetimeCancellation.Token);
            // 后台启动指数退避循环，不阻塞 WinRT 断线回调。
            _reconnectLoopTask = Task.Run(() => RunReconnectLoopAsync(_reconnectCancellation.Token));
            // 返回新任务引用。
            return _reconnectLoopTask;
        }
    }

    // 按 1、2、4、8、15 秒指数退避；超过数组后一直使用最后 15 秒。
    private async Task RunReconnectLoopAsync(CancellationToken cancellationToken)
    {
        // 尝试序号从 0 开始，用于选择退避数组元素。
        int attempt = 0;

        // 只要用户仍希望连接且未取消，就持续重试。
        while (!cancellationToken.IsCancellationRequested)
        {
            // 超过数组长度后固定使用最后一个 15 秒值，避免无限增长。
            TimeSpan delay = _reconnectDelays[Math.Min(attempt, _reconnectDelays.Length - 1)];
            // 可取消等待退避，禁止断线忙循环。
            await _delayAsync(delay, cancellationToken).ConfigureAwait(false);

            try
            {
                // 每次真正发起自动重连前累计一次；首次显式连接不计入。
                _ = Interlocked.Increment(ref _reconnectCount);
                // 使用同一 preferred Windows 设备 ID 重连并恢复 Manifest/快照。
                await ConnectCoreAsync(isReconnect: true, cancellationToken).ConfigureAwait(false);
                // 连接成功后结束本轮循环。
                return;
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                // 主动断开或 Dispose 属于正常退出。
                return;
            }
            catch (Exception exception)
            {
                // 记录本次失败，不向 UI 抛后台未观察异常。
                System.Diagnostics.Debug.WriteLine($"BLE 自动重连第 {attempt + 1} 次失败：{exception.Message}");
                // 下一轮使用更长或封顶退避。
                attempt++;
            }
        }
    }

    // 发布断开事件并保持设备内部会话独立运行。
    private void PublishDisconnected(string reason)
    {
        // 记录是否从连接态真正发生变化。
        bool changed;

        lock (_sync)
        {
            // 保存原状态以抑制重复断线事件。
            changed = _isConnected;
            // 清除会话层连接标志。
            _isConnected = false;
            // 断线后设备通知状态未知，必须清除当前 RawStream 已启用事实。
            _rawStreamEnabled = false;
        }

        // 只有连接态变化时通知 UI。
        if (changed)
        {
            // 明确 PC 断线不代表设备训练停止。
            ConnectionChanged?.Invoke(this, new DeviceConnectionChangedEventArgs(false, reason));
        }
    }

    // 清空三个订阅特征独立重组器。
    private void ResetReassemblers()
    {
        // 遍历固定三个重组器，终止所有半帧。
        foreach (BleFrameReassembler reassembler in _reassemblers.Values)
        {
            // 清除 sequence、索引、长度和旧缓冲内容。
            reassembler.Reset();
        }
    }

    // 按协议错误类型更新 CRC 或分片诊断计数。
    private void RecordProtocolError(ProtocolDecodeError error)
    {
        // CRC 错误单独累计，便于识别字节损坏。
        if (error == ProtocolDecodeError.BadCrc)
        {
            // 原子增加 CRC 错误数。
            _ = Interlocked.Increment(ref _crcErrorCount);
            // 已完成分类。
            return;
        }

        // 其它重组拒绝表示包络、索引、长度或顺序不符合分片合同。
        _ = Interlocked.Increment(ref _fragmentErrorCount);
    }

    // 把非负 long 计数安全转换为公开 uint，避免极端长运行溢出。
    private static uint ClampCounter(long value)
    {
        // 负值理论上不会出现，仍夹到零；超上限时保持饱和。
        return checked((uint)Math.Clamp(value, 0L, uint.MaxValue));
    }

    // 让全部等待命令收到同一链路异常，并从字典移除。
    private void FailPendingControls(Exception exception)
    {
        // 遍历并发字典当前快照；TryRemove 处理与正常响应竞争。
        foreach (KeyValuePair<uint, PendingControlRequest> entry in _pendingControls)
        {
            // 只有成功移除的调用负责完成异常。
            if (_pendingControls.TryRemove(entry.Key, out PendingControlRequest? pending))
            {
                // 异步完成等待任务，调用者得到明确链路失败。
                pending.Completion.TrySetException(exception);
            }
        }
    }

    // 让全部等待会话页收到链路异常，并关闭数据通道。
    private void FailPendingTransfers(Exception exception)
    {
        // 遍历并发字典快照，TryRemove 负责与正常完成竞争。
        foreach (KeyValuePair<uint, PendingTransferRequest> entry in _pendingTransfers)
        {
            // 只有成功移除者负责完成等待者。
            if (_pendingTransfers.TryRemove(entry.Key, out PendingTransferRequest? pending))
            {
                // indication 等待收到明确异常。
                pending.Response.TrySetException(exception);
                // 数据读取收到同一链路异常。
                pending.Data.Writer.TryComplete(exception);
            }
        }
    }

    // 取消当前重连退避，不等待任务，任务会在下一取消点正常退出。
    private void CancelReconnectLoop()
    {
        lock (_sync)
        {
            // 请求现有重连循环停止；null 时无动作。
            _reconnectCancellation?.Cancel();
        }
    }

    // transport 断开异常不应覆盖原始连接错误。
    private async Task SafeTransportDisconnectAsync()
    {
        try
        {
            // 尽力释放半初始化 GATT 对象。
            await _transport.DisconnectAsync(CancellationToken.None).ConfigureAwait(false);
        }
        catch
        {
            // 保留调用方原始扫描、发现、订阅或读取异常。
        }
    }

    // 检查会话已完成连接。
    private void EnsureConnected()
    {
        // 释放后优先报告对象生命周期错误。
        ThrowIfDisposed();
        // 未完成连接时禁止 GATT 读写。
        if (!IsConnected)
        {
            // 提醒 UI 先连接或等待自动重连。
            throw new InvalidOperationException("真机蓝牙设备尚未连接或正在自动重连。");
        }
    }

    // 使用当前 UTC 和系统本地时区执行命令 6；连接若不同步成功则不提交给 UI。
    private Task SynchronizeCurrentClockAsync(CancellationToken cancellationToken)
    {
        // 读取单一当前时间点，避免 UTC 和时区跨越夏令时切换边界。
        DateTimeOffset now = DateTimeOffset.Now;
        // DateTimeOffset.Offset 已表示该时刻本地有效偏移，单位换算后位于 short 范围。
        short offsetMinutes = checked((short)now.Offset.TotalMinutes);
        // 接口传入当前 UTC Unix 毫秒；配置 codec 会截断为固件命令 6 要求的 Unix 秒，并附带时区分钟。
        return SyncTimeAsync(now.ToUnixTimeMilliseconds(), offsetMinutes, cancellationToken);
    }

    // 检查对象是否已释放。
    private void ThrowIfDisposed()
    {
        // 使用运行时标准异常，调用者可区分连接失败和生命周期错误。
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    // 保存一个 request_id 对应的命令和完成源。
    private sealed record PendingControlRequest(
        // 响应 command_id 必须匹配该值。
        ControlCommandId CommandId,
        // indication 到达时完成该异步等待者。
        TaskCompletionSource<ControlPointResponse> Completion);

    // 保存一个会话同步请求的可靠响应和数据通知通道。
    private sealed record PendingTransferRequest(
        // 类型 6 indication 完成源。
        TaskCompletionSource<SessionTransferResponseV1> Response,
        // 类型 7 数据通道；重试重复项由页内 index 去重。
        Channel<SessionTransferDataV1> Data);
}
