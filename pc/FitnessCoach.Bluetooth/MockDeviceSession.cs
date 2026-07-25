// 引入领域接口和状态对象，Mock 与未来真实 BLE 使用相同合同。
using FitnessCoach.Domain;

// Mock 位于蓝牙层，便于在没有硬件时替换真实设备会话。
namespace FitnessCoach.Bluetooth;

/// <summary>
/// 模拟独立运行的 ESP32：PC 断线后训练继续，重连时发布最新权威快照。
/// </summary>
public sealed class MockDeviceSession : IDeviceSession, IDevicePairingSession, IDeviceDiagnosticsSource, IDeviceConfigurationSession, IRawStreamSource, ISessionHistorySyncSource, IDeviceProtocolEventSource
{
    // 默认模拟动作按 11 类固定顺序循环，便于逐页验证动画和单位。
    private static readonly ActionId[] DefaultScript =
    [
        ActionId.GoodMorning,
        ActionId.JumpingJack,
        ActionId.JumpingLunge,
        ActionId.JumpingSquat,
        ActionId.Lunge,
        ActionId.Sit,
        ActionId.Squat,
        ActionId.Trot,
        ActionId.TuckJump,
        ActionId.Walk,
        ActionId.Wave,
    ];

    // 普通对象锁保护设备内部状态，事件始终在锁外触发。
    private readonly object _sync = new();
    // 命令信号量串行化并发开始、暂停、恢复和停止，保证幂等。
    private readonly SemaphoreSlim _commandGate = new(1, 1);
    // 保存每个模拟 tick 的真实时间间隔。
    private readonly TimeSpan _tickInterval;
    // 保存动作脚本副本，运行中不会被调用者修改。
    private readonly ActionId[] _actionScript;
    // 生命周期取消源停止后台设备时钟。
    private readonly CancellationTokenSource _lifetimeCancellation = new();
    // 后台设备任务在 PC 断线时仍继续推进训练。
    private readonly Task _deviceLoopTask;
    // 保存各动作累计指标的可变内部状态。
    private readonly Dictionary<ActionId, MutableActionMetric> _metrics = new();
    // 当前 PC 链路状态；不等同于设备是否正在训练。
    private bool _isConnected;
    // 当前设备状态，默认空闲。
    private FitnessDeviceState _deviceState = FitnessDeviceState.Idle;
    // 设备持久化会话序号；每次真正开始新会话递增一次。
    private uint _sessionSequence;
    // 权威状态修订号；每次状态或累计变化递增。
    private uint _stateRevision;
    // 会话内低延迟事件序号；每次新会话从零重新开始，模拟固件 MetricEvent 幂等合同。
    private uint _eventSequence;
    // 当前会话单调时长，单位毫秒。
    private uint _elapsedMilliseconds;
    // 当前会话总卡路里，单位千分之一千卡。
    private uint _caloriesMilliKcal;
    // 当前模拟动作。
    private ActionId _currentAction = ActionId.Unknown;
    // 当前动作对应的指标单位。
    private MetricKind _currentMetricKind = MetricKind.None;
    // 当前动作累计次数、步数或秒数。
    private uint _currentMetricValue;
    // 当前会话开始 UTC 时间。
    private DateTimeOffset _startedAtUtc;
    // 模拟 tick 序号，用于切换动作、计数和电量变化。
    private uint _tickIndex;
    // 模拟电池百分比，初始 96%。
    private byte _batteryPercent = 96;
    // 最近一次停止产生的摘要，用于重复 Stop 幂等返回。
    private TrainingSessionSummary? _lastSummary;
    // dispose 标志防止释放后继续发命令。
    private bool _disposed;
    // 是否至少成功连接过一次，用于区分首次连接和模拟重连。
    private bool _hasConnected;
    // 模拟自动/手工重连次数；首次连接不计入。
    private uint _reconnectCount;
    // 当前 Mock 用户体重，单位克；默认与本地设置 65 kg 一致。
    private uint _weightGrams = 65_000U;
    // 当前 Mock 资料修订号。
    private uint _profileRevision;
    // 当前 Mock 目标类型；默认无目标。
    private DeviceGoalKind _goalKind = DeviceGoalKind.None;
    // 当前 Mock 目标值，单位由 _goalKind 决定。
    private uint _goalValue;
    // 当前 Mock 设备偏好。
    private DevicePreferencesV1 _devicePreferences = new(35, false, 30, 0U, false);
    // 当前 Mock RawStream 是否已显式打开。
    private bool _rawStreamEnabled;
    // 最近同步的 UTC Unix 毫秒，零表示尚未同步。
    private long _syncedUtcUnixMilliseconds;
    // 最近同步的本地时区分钟。
    private short _syncedTimezoneOffsetMinutes;

    /// <summary>
    /// 创建 Mock 设备；正式 UI 默认 480 ms，测试可传入更短间隔。
    /// </summary>
    public MockDeviceSession(TimeSpan? tickInterval = null, IReadOnlyList<ActionId>? actionScript = null)
    {
        // 使用调用值或与设备推理步长一致的 480 ms。
        _tickInterval = tickInterval ?? TimeSpan.FromMilliseconds(480);
        // 非正间隔会使后台循环忙等或无法创建 PeriodicTimer。
        if (_tickInterval <= TimeSpan.Zero)
        {
            // 拒绝非法模拟时钟。
            throw new ArgumentOutOfRangeException(nameof(tickInterval), "模拟 tick 间隔必须大于零。");
        }

        // 使用调用脚本或默认 11 类脚本，并复制为私有数组。
        _actionScript = (actionScript ?? DefaultScript).ToArray();
        // 空脚本无法产生动作和计数。
        if (_actionScript.Length == 0)
        {
            // 拒绝空动作脚本。
            throw new ArgumentException("模拟动作脚本不能为空。", nameof(actionScript));
        }

        // 验证脚本不含 Unknown，避免历史指标出现未知类别。
        if (_actionScript.Any(action => action == ActionId.Unknown))
        {
            // Unknown 只用于没有可靠分类，不是可计数训练动作。
            throw new ArgumentException("模拟动作脚本不能包含 Unknown。", nameof(actionScript));
        }

        // 立即启动独立设备时钟；PC 断开只停止通知，不停止该任务。
        _deviceLoopTask = Task.Run(() => RunDeviceLoopAsync(_lifetimeCancellation.Token));
    }

    /// <inheritdoc />
    public event EventHandler<LiveState>? StateChanged;

    /// <inheritdoc />
    public event EventHandler<DeviceConnectionChangedEventArgs>? ConnectionChanged;

    /// <inheritdoc />
    public event EventHandler<RawImuSampleReceivedEventArgs>? RawSampleReceived;

    /// <inheritdoc />
    public event EventHandler<InferenceDiagnosticReceivedEventArgs>? InferenceDiagnosticReceived;

    /// <inheritdoc />
    public event EventHandler<DeviceProtocolEventEventArgs>? ProtocolEventReceived;

    /// <inheritdoc />
    public string DeviceId => "MOCK-ESP32S3-0001";

    /// <inheritdoc />
    public bool IsConnected
    {
        get
        {
            // 使用同一状态锁读取链路布尔值。
            lock (_sync)
            {
                // 返回当前 Mock 链路状态。
                return _isConnected;
            }
        }
    }

    /// <inheritdoc />
    public bool IsHardwareBacked => false;

    /// <inheritdoc />
    public bool IsRawStreamEnabled
    {
        get
        {
            // 使用状态锁读取命令 11 的 Mock 权威状态。
            lock (_sync)
            {
                // 返回当前开关。
                return _rawStreamEnabled;
            }
        }
    }

    /// <inheritdoc />
    public DeviceDiagnosticsSnapshot GetDiagnosticsSnapshot()
    {
        // 使用设备状态锁读取连接、电量和 revision 一致快照。
        lock (_sync)
        {
            // 返回明确 Mock 标识，防止用户把模拟数值当成真板测量。
            return new DeviceDiagnosticsSnapshot(
                DeviceId,
                _isConnected,
                isHardwareBacked: false,
                "ESP32-S3 Touch AMOLED 2.06（Mock）",
                "2.06-MOCK",
                "0.1.0-MOCK",
                _isConnected ? -52 : null,
                _isConnected ? (ushort)247 : null,
                _batteryPercent,
                _stateRevision,
                _reconnectCount,
                crcErrorCount: 0U,
                fragmentErrorCount: 0U,
                manifestSummary: "设备能力清单版本 1.0；基础模型摘要=8f66e344bcfa；掩码模型摘要=57f4b2bca05d；能力标志=0x00000006；内部文件系统可用量=1048576 字节（模拟）");
        }
    }

    /// <inheritdoc />
    public async Task ConnectAsync(CancellationToken cancellationToken = default)
    {
        // 检查取消和 dispose 状态。
        cancellationToken.ThrowIfCancellationRequested();
        // 串行化连接命令和训练控制。
        await _commandGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        // 记录是否需要发布连接事件，重复连接不重复发布。
        bool changed = false;
        // 保存重连后应发布的权威状态快照。
        LiveState? snapshot = null;

        try
        {
            // 检查对象未释放。
            ThrowIfDisposed();
            // 进入设备状态锁。
            lock (_sync)
            {
                // 只在原先未连接时改变状态。
                if (!_isConnected)
                {
                    // 第二次及以后从断开态连接视为重连。
                    if (_hasConnected)
                    {
                        // 饱和增加模拟重连计数，避免极端测试回绕。
                        _reconnectCount = _reconnectCount == uint.MaxValue ? uint.MaxValue : _reconnectCount + 1U;
                    }
                    else
                    {
                        // 首次连接只建立基线，不增加重连计数。
                        _hasConnected = true;
                    }
                    // 标记 Mock 链路已连接。
                    _isConnected = true;
                    // 记录需要发布连接事件。
                    changed = true;
                }

                // 无论是否重复连接，都生成当前权威快照供界面恢复。
                snapshot = CreateSnapshotLocked();
            }
        }
        finally
        {
            // 释放命令信号量。
            _commandGate.Release();
        }

        // 首次连接或重连时发布连接事件。
        if (changed)
        {
            // 在锁外通知订阅者，避免界面回调导致死锁。
            ConnectionChanged?.Invoke(this, new DeviceConnectionChangedEventArgs(true, "模拟设备已连接"));
        }

        // 发布最新权威快照，恢复断线期间累计值。
        StateChanged?.Invoke(this, snapshot);
        // 模拟真 BLE 连接完成后的自动校时，不依赖系统蓝牙 ACK。
        DateTimeOffset now = DateTimeOffset.Now;
        // 保存同一时间点的 UTC 与时区，确保夏令时边界一致。
        await SyncTimeAsync(now.ToUnixTimeMilliseconds(), checked((short)now.Offset.TotalMinutes), cancellationToken).ConfigureAwait(false);
    }

    /// <inheritdoc />
    public async Task SyncTimeAsync(long utcUnixMilliseconds, short timezoneOffsetMinutes, CancellationToken cancellationToken = default)
    {
        // 复用正式 codec 执行 UTC 和时区范围检查。
        _ = DeviceConfigurationCodec.EncodeTimeSync(utcUnixMilliseconds, timezoneOffsetMinutes);
        // 串行化配置与训练控制。
        await _commandGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            // 检查对象生命周期。
            ThrowIfDisposed();
            // 在锁内提交 Mock 设备时间事实。
            lock (_sync)
            {
                // 未连接时模拟 GATT 控制不可用。
                EnsureConnectedLocked();
                // 保存 Unix 毫秒。
                _syncedUtcUnixMilliseconds = utcUnixMilliseconds;
                // 保存时区分钟。
                _syncedTimezoneOffsetMinutes = timezoneOffsetMinutes;
            }
        }
        finally
        {
            // 释放配置命令串行化锁。
            _commandGate.Release();
        }
    }

    /// <inheritdoc />
    public async Task SetProfileAsync(uint weightGrams, uint revision, CancellationToken cancellationToken = default)
    {
        // 复用正式 codec 检查 30～250 kg 和线上结构。
        _ = DeviceConfigurationCodec.EncodeProfile(weightGrams, revision);
        // 串行化配置写入。
        await _commandGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            // 检查对象生命周期。
            ThrowIfDisposed();
            // 修改 Mock 权威配置时持有状态锁。
            lock (_sync)
            {
                // 未连接时拒绝配置。
                EnsureConnectedLocked();
                // 旧修订号不能覆盖较新资料；相同修订号按幂等成功。
                if (revision >= _profileRevision)
                {
                    // 保存下次训练使用的克数。
                    _weightGrams = weightGrams;
                    // 保存最新修订号。
                    _profileRevision = revision;
                }
            }
        }
        finally
        {
            // 释放配置锁。
            _commandGate.Release();
        }
    }

    /// <inheritdoc />
    public async Task SetGoalAsync(DeviceGoalKind kind, uint value, CancellationToken cancellationToken = default)
    {
        // 复用正式 codec 检查枚举和零值组合。
        _ = DeviceConfigurationCodec.EncodeGoal(kind, value);
        // 串行化目标写入。
        await _commandGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            // 检查对象生命周期。
            ThrowIfDisposed();
            // 在锁内提交目标。
            lock (_sync)
            {
                // 未连接时拒绝配置。
                EnsureConnectedLocked();
                // 保存目标种类。
                _goalKind = kind;
                // 保存目标值。
                _goalValue = value;
            }
        }
        finally
        {
            // 释放配置锁。
            _commandGate.Release();
        }
    }

    /// <inheritdoc />
    public async Task SetPreferencesAsync(DevicePreferencesV1 preferences, CancellationToken cancellationToken = default)
    {
        // 复用正式 codec 检查亮度和熄屏边界。
        _ = DeviceConfigurationCodec.EncodePreferences(preferences);
        // 串行化偏好写入。
        await _commandGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            // 检查对象生命周期。
            ThrowIfDisposed();
            // 在锁内提交偏好。
            lock (_sync)
            {
                // 未连接时拒绝配置。
                EnsureConnectedLocked();
                // 旧 revision 不能覆盖新偏好；相同 revision 保持幂等。
                if (preferences.Revision >= _devicePreferences.Revision)
                {
                    // 保存不可变偏好记录。
                    _devicePreferences = preferences;
                    // 关闭开发者模式时强制关闭 RawStream，匹配真机安全边界。
                    if (!preferences.DeveloperModeEnabled)
                    {
                        // 禁止后台继续生成原始样本。
                        _rawStreamEnabled = false;
                    }
                }
            }
        }
        finally
        {
            // 释放配置锁。
            _commandGate.Release();
        }
    }

    /// <inheritdoc />
    public async Task SetRawStreamEnabledAsync(bool enabled, CancellationToken cancellationToken = default)
    {
        // 复用正式 codec 确认 bool 线上固定为 0/1。
        _ = DeviceConfigurationCodec.EncodeRawStreamEnabled(enabled);
        // 串行化 RawStream 开关。
        await _commandGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            // 检查对象生命周期。
            ThrowIfDisposed();
            // 在锁内检查开发者模式和连接。
            lock (_sync)
            {
                // 未连接时拒绝命令 11。
                EnsureConnectedLocked();
                // 打开 RawStream 前必须先在偏好中启用开发者模式。
                if (enabled && !_devicePreferences.DeveloperModeEnabled)
                {
                    // 给诊断页明确恢复方式。
                    throw new InvalidOperationException("请先在设置页启用开发者模式并同步设备。" );
                }

                // 提交 Mock RawStream 状态。
                _rawStreamEnabled = enabled;
            }
        }
        finally
        {
            // 释放配置锁。
            _commandGate.Release();
        }
    }

    /// <inheritdoc />
    public Task<SessionTransferPage> PullSessionSummariesAsync(
        uint cursorSessionSequence,
        ushort pageSize = SessionTransferCodec.MaxPageSize,
        CancellationToken cancellationToken = default)
    {
        // 传播调用方取消。
        cancellationToken.ThrowIfCancellationRequested();
        // 页大小必须匹配真实设备 1～12 条边界。
        if ((pageSize == 0) || (pageSize > SessionTransferCodec.MaxPageSize))
        {
            // 拒绝与固件不同的 Mock 行为。
            throw new ArgumentOutOfRangeException(nameof(pageSize));
        }

        // 检查对象生命周期。
        ThrowIfDisposed();
        // 在状态锁内读取最近已停止摘要。
        lock (_sync)
        {
            // 未连接时模拟真实补传 GATT 不可用。
            EnsureConnectedLocked();
            // 没有摘要或本地游标已追平时返回空终点页。
            if ((_lastSummary is null) || (_lastSummary.SessionSequence <= cursorSessionSequence))
            {
                // total_count 为当前 Mock 已保存摘要数量 0 或 1。
                ushort totalCount = _lastSummary is null ? (ushort)0 : (ushort)1;
                // 游标保持当前值，空页明确结束。
                return Task.FromResult(new SessionTransferPage(cursorSessionSequence, totalCount, true, []));
            }

            // Mock 当前只保留最近一条摘要，返回一条终点页。
            return Task.FromResult(new SessionTransferPage(_lastSummary.SessionSequence, 1, true, [_lastSummary]));
        }
    }

    /// <inheritdoc />
    public Task DisconnectAsync(CancellationToken cancellationToken = default)
    {
        // 使用统一内部方法标记用户主动断开。
        return SetDisconnectedAsync("用户主动断开模拟设备", cancellationToken);
    }

    /// <inheritdoc />
    public async Task ForgetDeviceAsync(CancellationToken cancellationToken = default)
    {
        // 先关闭模拟 PC 链路，保持设备内部训练状态与真实手柄断链合同一致。
        await SetDisconnectedAsync("模拟设备已断开并准备清除固定状态", cancellationToken).ConfigureAwait(false);
        // 串行化固定设备状态清理，避免与新的 Connect 并发修改重连基线。
        await _commandGate.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            // 检查会话尚未释放。
            ThrowIfDisposed();
            // 使用状态锁同步清除首次连接标志和重连诊断计数。
            lock (_sync)
            {
                // 下次 Connect 必须按新的首次选择处理，不沿用旧设备重连语义。
                _hasConnected = false;
                // 清除旧固定设备产生的重连计数，便于 Mock 验证本地状态确实重置。
                _reconnectCount = 0U;
                // 忘记设备同时关闭诊断原始流，防止下次连接自动继承敏感流状态。
                _rawStreamEnabled = false;
            }
        }
        finally
        {
            // 释放命令串行化锁，允许用户重新连接模拟设备。
            _commandGate.Release();
        }
    }

    /// <summary>
    /// 模拟意外链路丢失；设备训练仍继续，供断线恢复测试调用。
    /// </summary>
    public Task SimulateLinkLossAsync(CancellationToken cancellationToken = default)
    {
        // 使用与真机蓝牙超时相同的断线状态，但原因明确为模拟丢失。
        return SetDisconnectedAsync("模拟蓝牙链路丢失", cancellationToken);
    }

    /// <inheritdoc />
    public Task<LiveState> GetSnapshotAsync(CancellationToken cancellationToken = default)
    {
        // 检查取消请求。
        cancellationToken.ThrowIfCancellationRequested();
        // 检查对象未释放。
        ThrowIfDisposed();

        // 使用状态锁生成一致快照。
        lock (_sync)
        {
            // 未连接时模拟真实 GATT 无法读取特征。
            EnsureConnectedLocked();
            // 返回当前权威状态。
            return Task.FromResult(CreateSnapshotLocked());
        }
    }

    /// <inheritdoc />
    public async Task StartAsync(CancellationToken cancellationToken = default)
    {
        // 串行化并发开始命令。
        await _commandGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        // 保存需要发布的新状态；幂等重复开始仍返回当前快照。
        LiveState snapshot;

        try
        {
            // 检查对象未释放。
            ThrowIfDisposed();
            // 修改设备状态时持有锁。
            lock (_sync)
            {
                // 控制命令要求链路已连接。
                EnsureConnectedLocked();

                // Running 状态重复开始保持原会话和累计值。
                if (_deviceState == FitnessDeviceState.Running)
                {
                    // 返回当前快照，不重置任何字段。
                    snapshot = CreateSnapshotLocked();
                }
                else
                {
                    // 只允许 Idle 或 Summary 开始新会话。
                    if ((_deviceState != FitnessDeviceState.Idle) && (_deviceState != FitnessDeviceState.Summary))
                    {
                        // Paused 必须使用 Resume，防止误清零。
                        throw new InvalidOperationException("当前状态不能开始新会话，请先恢复或停止现有会话。");
                    }

                    // 为新会话递增持久化序号。
                    _sessionSequence++;
                    // 递增状态修订号。
                    _stateRevision++;
                    // 清零单调时长。
                    _elapsedMilliseconds = 0;
                    // 清零总卡路里。
                    _caloriesMilliKcal = 0;
                    // 清零模拟 tick 序号。
                    _tickIndex = 0;
                    // 新会话的低延迟事件序号从零重新开始。
                    _eventSequence = 0U;
                    // 清空各动作历史指标。
                    _metrics.Clear();
                    // 首个动作取脚本第一项。
                    _currentAction = _actionScript[0];
                    // 根据首个动作确定单位。
                    _currentMetricKind = ResolveMetricKind(_currentAction);
                    // 新会话当前指标从零开始。
                    _currentMetricValue = 0;
                    // 保存 UTC 开始时间。
                    _startedAtUtc = DateTimeOffset.UtcNow;
                    // 切换到运行状态。
                    _deviceState = FitnessDeviceState.Running;
                    // 新会话使旧停止摘要失效。
                    _lastSummary = null;
                    // 创建新会话第一帧状态。
                    snapshot = CreateSnapshotLocked();
                }
            }
        }
        finally
        {
            // 释放命令信号量。
            _commandGate.Release();
        }

        // 在锁外发布当前状态，重复开始也能让 UI 确认权威值。
        StateChanged?.Invoke(this, snapshot);
    }

    /// <inheritdoc />
    public async Task PauseAsync(CancellationToken cancellationToken = default)
    {
        // 串行化暂停命令。
        await _commandGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        // 保存暂停后快照。
        LiveState snapshot;

        try
        {
            // 检查对象未释放。
            ThrowIfDisposed();
            // 修改状态时持有锁。
            lock (_sync)
            {
                // 暂停命令要求链路已连接。
                EnsureConnectedLocked();

                // Running 状态执行一次暂停。
                if (_deviceState == FitnessDeviceState.Running)
                {
                    // 切换到暂停状态。
                    _deviceState = FitnessDeviceState.Paused;
                    // 递增状态修订号。
                    _stateRevision++;
                }
                else if (_deviceState != FitnessDeviceState.Paused)
                {
                    // 非 Running/Paused 状态没有可暂停会话。
                    throw new InvalidOperationException("当前没有可暂停的运行会话。");
                }

                // 重复暂停返回同一累计值。
                snapshot = CreateSnapshotLocked();
            }
        }
        finally
        {
            // 释放命令信号量。
            _commandGate.Release();
        }

        // 发布暂停状态。
        StateChanged?.Invoke(this, snapshot);
    }

    /// <inheritdoc />
    public async Task ResumeAsync(CancellationToken cancellationToken = default)
    {
        // 串行化恢复命令。
        await _commandGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        // 保存恢复后快照。
        LiveState snapshot;

        try
        {
            // 检查对象未释放。
            ThrowIfDisposed();
            // 修改状态时持有锁。
            lock (_sync)
            {
                // 恢复命令要求链路已连接。
                EnsureConnectedLocked();

                // Paused 状态执行一次恢复。
                if (_deviceState == FitnessDeviceState.Paused)
                {
                    // 切换到运行状态。
                    _deviceState = FitnessDeviceState.Running;
                    // 递增状态修订号。
                    _stateRevision++;
                }
                else if (_deviceState != FitnessDeviceState.Running)
                {
                    // 非 Paused/Running 状态没有可恢复会话。
                    throw new InvalidOperationException("当前没有可恢复的暂停会话。");
                }

                // 重复恢复返回同一累计值。
                snapshot = CreateSnapshotLocked();
            }
        }
        finally
        {
            // 释放命令信号量。
            _commandGate.Release();
        }

        // 发布恢复状态。
        StateChanged?.Invoke(this, snapshot);
    }

    /// <inheritdoc />
    public async Task<TrainingSessionSummary?> StopAsync(CancellationToken cancellationToken = default)
    {
        // 串行化停止命令。
        await _commandGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        // 保存停止后快照。
        LiveState? snapshot = null;
        // 保存幂等返回摘要。
        TrainingSessionSummary? summary;

        try
        {
            // 检查对象未释放。
            ThrowIfDisposed();
            // 修改状态时持有锁。
            lock (_sync)
            {
                // 停止命令要求链路已连接。
                EnsureConnectedLocked();

                // Summary 状态重复停止返回上一摘要。
                if (_deviceState == FitnessDeviceState.Summary)
                {
                    // 不重复生成时间或会话记录。
                    summary = _lastSummary;
                }
                else
                {
                    // 只允许停止运行或暂停会话。
                    if ((_deviceState != FitnessDeviceState.Running) && (_deviceState != FitnessDeviceState.Paused))
                    {
                        // Idle 没有可停止会话，返回 null 保持幂等。
                        return null;
                    }

                    // 从当前累积状态创建不可变摘要。
                    summary = CreateSummaryLocked("用户停止");
                    // 保存摘要供重复停止返回。
                    _lastSummary = summary;
                    // 切换到总结状态。
                    _deviceState = FitnessDeviceState.Summary;
                    // 递增状态修订号。
                    _stateRevision++;
                    // 创建停止后权威快照。
                    snapshot = CreateSnapshotLocked();
                }
            }
        }
        finally
        {
            // 释放命令信号量。
            _commandGate.Release();
        }

        // 首次停止时发布总结状态；重复停止不制造第二个事件。
        if (snapshot is not null)
        {
            // 在锁外通知 UI。
            StateChanged?.Invoke(this, snapshot);
        }

        // 返回首次或缓存的同一摘要。
        return summary;
    }

    /// <inheritdoc />
    public async ValueTask DisposeAsync()
    {
        // 重复释放时直接返回。
        if (_disposed)
        {
            // 没有剩余资源需要处理。
            return;
        }

        // 标记对象已释放，阻止新命令。
        _disposed = true;
        // 请求后台设备时钟停止。
        _lifetimeCancellation.Cancel();

        try
        {
            // 等待后台任务退出，避免测试进程残留线程。
            await _deviceLoopTask.ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // 生命周期取消属于正常关闭，不向上抛出。
        }

        // 释放取消源。
        _lifetimeCancellation.Dispose();
        // 释放命令信号量。
        _commandGate.Dispose();
    }

    // 标记链路断开，但不改变设备训练状态或后台时钟。
    private async Task SetDisconnectedAsync(string reason, CancellationToken cancellationToken)
    {
        // 检查取消请求。
        cancellationToken.ThrowIfCancellationRequested();
        // 串行化连接状态变化。
        await _commandGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        // 记录是否真正从连接变为断开。
        bool changed = false;

        try
        {
            // 检查对象未释放。
            ThrowIfDisposed();
            // 修改连接状态时持有锁。
            lock (_sync)
            {
                // 重复断开不发布第二个事件。
                if (_isConnected)
                {
                    // 只关闭 PC 链路。
                    _isConnected = false;
                    // 标记需要发布事件。
                    changed = true;
                    // 链路断开后 RawStream 立即关闭且不落盘。
                    _rawStreamEnabled = false;
                }
            }
        }
        finally
        {
            // 释放命令信号量。
            _commandGate.Release();
        }

        // 只有状态实际变化时通知订阅者。
        if (changed)
        {
            // 发布明确断开原因。
            ConnectionChanged?.Invoke(this, new DeviceConnectionChangedEventArgs(false, reason));
        }
    }

    // 后台设备时钟独立于 PC 连接运行。
    private async Task RunDeviceLoopAsync(CancellationToken cancellationToken)
    {
        // PeriodicTimer 按构造间隔产生稳定模拟采样周期。
        using PeriodicTimer timer = new(_tickInterval);

        try
        {
            // 持续等待下一 tick，直到生命周期取消。
            while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
            {
                // 保存本轮是否应向已连接 PC 发布状态。
                bool shouldPublish;
                // 保存锁内生成的不可变状态快照。
                LiveState? snapshot;
                // 保存可选 RawStream 样本；关闭状态保持 null。
                RawImuSampleV1? rawSample;
                // 保存可选双 M0 分类诊断；每十二个模拟点形成一个窗口事实。
                InferenceDiagnosticV1? inferenceDiagnostic;
                // 保存可选权威计数事件；只有次或步真实增加时才创建。
                DeviceProtocolEventEventArgs? metricEvent;

                // 推进设备内部状态时持有锁。
                lock (_sync)
                {
                    // 只有 Running 状态增加时长、次数和卡路里。
                    if (_deviceState != FitnessDeviceState.Running)
                    {
                        // 暂停、空闲和总结状态跳过本轮。
                        continue;
                    }

                    // 保存推进前 tick 序号，用于判断本轮是否满足步数或次数边界。
                    uint tickIndexBeforeAdvance = _tickIndex;
                    // 推进一个确定性模拟 tick。
                    AdvanceTickLocked();
                    // PC 连接时才发布通知，断线期间只保留内部状态。
                    shouldPublish = _isConnected;
                    // 创建本轮权威快照。
                    snapshot = CreateSnapshotLocked();
                    // 只有显式启用且 PC 链路连接时才创建开发者原始样本。
                    rawSample = _rawStreamEnabled && _isConnected
                        ? CreateRawSampleLocked()
                        : null;
                    // Mock 只在显式 RawStream 会话且到达固定窗口边界时创建分类诊断。
                    inferenceDiagnostic = _rawStreamEnabled && _isConnected && ((_tickIndex % 12U) == 0U)
                        ? CreateInferenceDiagnosticLocked()
                        : null;
                    // 次数或步数在本轮增加时创建精确设备时间的 EventV1；持续秒数不作为计数标记。
                    metricEvent = _rawStreamEnabled && _isConnected && DidMetricIncreaseOnTickLocked(tickIndexBeforeAdvance)
                        ? CreateMetricEventLocked()
                        : null;
                }

                // 在锁外发布状态，避免订阅者阻塞设备内部状态。
                if (shouldPublish)
                {
                    // 触发后台线程事件，WPF 必须通过 Dispatcher 更新属性。
                    StateChanged?.Invoke(this, snapshot);
                }

                // RawStream 独立发布且不写入任何仓储。
                if (rawSample is not null)
                {
                    // 发布确定性原始六轴样本供诊断页显示。
                    RawSampleReceived?.Invoke(this, new RawImuSampleReceivedEventArgs(rawSample));
                }
                // 分类诊断独立发布且不修改权威 LiveState 或训练计数。
                if (inferenceDiagnostic is not null)
                {
                    // 发布确定性三路模型结果供界面离线展示和协议测试。
                    InferenceDiagnosticReceived?.Invoke(
                        this,
                        new InferenceDiagnosticReceivedEventArgs(inferenceDiagnostic));
                }
                // 权威计数事件独立发布；上位机只把它标到 CSV，不据此修改累计值。
                if (metricEvent is not null)
                {
                    // 发布含设备单调毫秒和 EventV1 的不可变事件。
                    ProtocolEventReceived?.Invoke(this, metricEvent);
                }
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            // 生命周期正常结束，不记录为设备错误。
        }
    }

    // 推进一个运行 tick；调用者必须持有 _sync。
    private void AdvanceTickLocked()
    {
        // 当前 tick 毫秒数四舍五入并至少取 1，适配快速测试间隔。
        uint tickMilliseconds = checked((uint)Math.Max(1.0, Math.Round(_tickInterval.TotalMilliseconds)));
        // 饱和累加会话时长，避免极端长会话 uint 回绕。
        _elapsedMilliseconds = SaturatingAdd(_elapsedMilliseconds, tickMilliseconds);
        // 每 10 个 tick 切换到脚本下一动作，提供稳定动画观察时间。
        int actionIndex = checked((int)((_tickIndex / 10U) % (uint)_actionScript.Length));
        // 更新当前动作。
        _currentAction = _actionScript[actionIndex];
        // 更新当前动作指标单位。
        _currentMetricKind = ResolveMetricKind(_currentAction);

        // 获取或创建当前动作可变指标。
        if (!_metrics.TryGetValue(_currentAction, out MutableActionMetric? metric))
        {
            // 新动作第一次出现时创建零累计指标。
            metric = new MutableActionMetric(_currentMetricKind);
            // 加入按动作索引键控的字典。
            _metrics.Add(_currentAction, metric);
        }

        // 累加当前动作活动时长。
        metric.ActiveMilliseconds = SaturatingAdd(metric.ActiveMilliseconds, tickMilliseconds);
        // 根据动作强度和 tick 时长计算模拟千分之一千卡。
        uint tickCalories = CalculateMockCalories(_currentAction, tickMilliseconds);
        // 累加当前动作卡路里。
        metric.CaloriesMilliKcal = SaturatingAdd(metric.CaloriesMilliKcal, tickCalories);
        // 累加会话总卡路里。
        _caloriesMilliKcal = SaturatingAdd(_caloriesMilliKcal, tickCalories);

        // 按动作类型更新次数、步数或持续秒数。
        if (_currentMetricKind == MetricKind.Second)
        {
            // 持续型动作显示累计完整秒数。
            metric.MetricValue = metric.ActiveMilliseconds / 1000U;
        }
        else if (_currentMetricKind == MetricKind.Step)
        {
            // 行走和小跑每两个模拟 tick 产生一步。
            if ((_tickIndex % 2U) == 1U)
            {
                // 饱和增加一步。
                metric.MetricValue = SaturatingAdd(metric.MetricValue, 1U);
            }
        }
        else if (_currentMetricKind == MetricKind.Repetition)
        {
            // 其它训练动作每四个模拟 tick 完成一次完整重复。
            if ((_tickIndex % 4U) == 3U)
            {
                // 饱和增加一次。
                metric.MetricValue = SaturatingAdd(metric.MetricValue, 1U);
            }
        }

        // 把当前动作累计值复制到实时状态字段。
        _currentMetricValue = metric.MetricValue;
        // 每 300 tick 模拟电量下降 1%，最低保留 5%。
        if ((_tickIndex > 0U) && ((_tickIndex % 300U) == 0U) && (_batteryPercent > 5U))
        {
            // 电量下降一个百分点。
            _batteryPercent--;
        }

        // 增加模拟 tick 序号。
        _tickIndex++;
        // 每次累计变化都递增权威状态 revision。
        _stateRevision++;
    }

    // 创建当前状态的不可变快照；调用者必须持有 _sync。
    private LiveState CreateSnapshotLocked()
    {
        // 运行状态使用高但非满值置信度，模拟真实稳定识别。
        ushort confidence = _deviceState == FitnessDeviceState.Running ? (ushort)57_500 : (ushort)0;
        // 未设置训练目标时使用 255。
        byte goalPercent = byte.MaxValue;
        // 当前电源标志保持无充电模拟状态。
        PowerFlags powerFlags = _batteryPercent <= 15U ? PowerFlags.LowBattery : PowerFlags.None;

        // 构造完整 30 字节领域对应状态。
        return new LiveState(
            _sessionSequence,
            _stateRevision,
            _elapsedMilliseconds,
            _deviceState,
            _currentAction,
            _currentMetricKind,
            _batteryPercent,
            _currentMetricValue,
            confidence,
            _caloriesMilliKcal,
            DataQualityFlags.None,
            powerFlags,
            goalPercent);
    }

    // 创建一个确定性 Mock 原始六轴样本；调用者必须持有 _sync。
    private RawImuSampleV1 CreateRawSampleLocked()
    {
        // 把 tick 低 7 位映射为小幅变化，避免 short 溢出。
        short phase = checked((short)(_tickIndex % 128U));
        // 返回固定通道顺序 gx、gy、gz、ax、ay、az；数值是 25 赫兹同步诊断码而非物理单位。
        return new RawImuSampleV1(
            _tickIndex,
            _elapsedMilliseconds,
            checked((short)(100 + phase)),
            checked((short)(-80 - phase)),
            checked((short)(40 + phase)),
            checked((short)(4096 + phase)),
            checked((short)(-128 - phase)),
            checked((short)(2048 + phase)),
            0);
    }

    // 创建确定性 Mock 双 M0 分类诊断；调用者必须持有 _sync。
    private InferenceDiagnosticV1 CreateInferenceDiagnosticLocked()
    {
        // 复用当前脚本动作作为融合 Top-1，保证模拟页面动作随设备时钟变化。
        ActionId fusedAction = _actionScript[checked((int)((_tickIndex / 10U) % (uint)_actionScript.Length))];
        // 基础模型在 Mock 中与融合结果一致，表示当前窗口没有模型分歧。
        ActionId baseAction = fusedAction;
        // 每隔二十四个点让掩码模型显示相邻类别，用于验证分歧状态的 UI 呈现。
        ActionId maskedAction = ((_tickIndex / 12U) % 2U) == 0U
            ? fusedAction
            : (ActionId)(((byte)fusedAction + 1U) % 11U);
        // 返回固定可重复诊断；概率在 0～1，时间单位微秒，质量和失败累计均为零。
        return new InferenceDiagnosticV1(
            1,
            fusedAction,
            baseAction,
            maskedAction,
            0.86,
            0.82,
            0.74,
            0,
            _tickIndex / 12U,
            _elapsedMilliseconds,
            18350,
            0);
    }

    // 判断刚完成的 Mock tick 是否使重复次数或步数增加；调用者必须持有 _sync。
    private bool DidMetricIncreaseOnTickLocked(uint tickIndexBeforeAdvance)
    {
        // 行走与小跑在推进前 tick 为奇数时增加一步，与 AdvanceTickLocked 完全一致。
        if (_currentMetricKind == MetricKind.Step)
        {
            // 返回本轮是否命中每两个 tick 一步的确定性边界。
            return (tickIndexBeforeAdvance % 2U) == 1U;
        }
        // 其它重复动作在推进前 tick 模四等于三时增加一次。
        if (_currentMetricKind == MetricKind.Repetition)
        {
            // 返回本轮是否命中每四个 tick 一次的确定性边界。
            return (tickIndexBeforeAdvance % 4U) == 3U;
        }
        // 持续秒与无指标动作不产生计数标记。
        return false;
    }

    // 创建与最近一次累计增加对应的 EventV1；调用者必须持有 _sync。
    private DeviceProtocolEventEventArgs CreateMetricEventLocked()
    {
        // 会话内事件序号严格递增；uint32 自然回绕与固件合同一致。
        _eventSequence = unchecked(_eventSequence + 1U);
        // 组装只用于低延迟标记的协议事件；权威累计仍由相邻 LiveState 提供。
        DeviceEventV1 deviceEvent = new(
            // EventV1 payload 版本固定为一。
            1,
            // 次数和步数都复用 RepetitionCounted，MetricKind 区分单位。
            DeviceEventType.RepetitionCounted,
            // 事件发生后设备仍处于训练中。
            _deviceState,
            // 保存本轮稳定动作。
            _currentAction,
            // 保存次数或步数单位。
            _currentMetricKind,
            // 保存当前电量。
            _batteryPercent,
            // Mock 没有传感器污染或丢样质量位。
            0,
            // 保存当前持久化会话序号。
            _sessionSequence,
            // 保存会话内幂等事件序号。
            _eventSequence,
            // 保存事件对应的权威状态修订号。
            _stateRevision,
            // 每个 Mock 计数事件固定增加一。
            1U,
            // 保存事件发生后的当前权威累计值。
            _currentMetricValue,
            // 保存事件时刻累计热量，单位千分之一千卡。
            _caloriesMilliKcal,
            // Mock 运行态使用固定 Q15 分类置信度。
            57_500,
            // 普通计数没有额外原因码。
            0);
        // 逻辑帧序号使用事件序号低 16 位，设备单调毫秒与当前 RawStream 点完全一致。
        return new DeviceProtocolEventEventArgs(
            unchecked((ushort)_eventSequence),
            _elapsedMilliseconds,
            deviceEvent,
            EventV1Codec.Encode(deviceEvent));
    }

    // 从当前可变指标创建不可变摘要；调用者必须持有 _sync。
    private TrainingSessionSummary CreateSummaryLocked(string endReason)
    {
        // 按动作索引排序，确保 JSON、历史页和测试顺序稳定。
        ActionMetric[] metrics = _metrics
            .OrderBy(pair => pair.Key)
            .Select(pair => new ActionMetric(
                pair.Key,
                pair.Value.MetricKind,
                pair.Value.MetricValue,
                pair.Value.ActiveMilliseconds,
                pair.Value.CaloriesMilliKcal))
            .ToArray();
        // 使用当前 UTC 作为结束墙钟时间。
        DateTimeOffset endedAtUtc = DateTimeOffset.UtcNow;

        // 返回设备 ID 和会话序号唯一标识的摘要。
        return new TrainingSessionSummary(
            DeviceId,
            _sessionSequence,
            _startedAtUtc,
            endedAtUtc,
            _elapsedMilliseconds,
            _caloriesMilliKcal,
            endReason,
            metrics);
    }

    // 根据动作类别返回界面累计单位。
    private static MetricKind ResolveMetricKind(ActionId action)
    {
        // 行走和小跑使用步数。
        if ((action == ActionId.Walk) || (action == ActionId.Trot))
        {
            // 返回步数单位。
            return MetricKind.Step;
        }

        // 静坐使用持续秒数。
        if (action == ActionId.Sit)
        {
            // 返回秒单位。
            return MetricKind.Second;
        }

        // 其余 8 类训练动作使用完整重复次数。
        return MetricKind.Repetition;
    }

    // 根据动作强度生成确定性模拟卡路里；仅用于 UI 演示，不作为真实估算公式。
    private static uint CalculateMockCalories(ActionId action, uint tickMilliseconds)
    {
        // 跳跃和小跑使用每分钟 10 kcal 的模拟速率，其余动作使用 6 kcal/min。
        uint milliKcalPerMinute = action is ActionId.JumpingJack or ActionId.JumpingLunge or ActionId.JumpingSquat or ActionId.TuckJump or ActionId.Trot
            ? 10_000U
            : 6_000U;
        // 计算当前 tick 对应千分之一千卡，并至少返回 1 以便快速测试可见增长。
        ulong scaled = ((ulong)milliKcalPerMinute * tickMilliseconds) / 60_000UL;
        // 返回受 uint 范围保护的模拟增量。
        return checked((uint)Math.Max(1UL, scaled));
    }

    // 对 uint 执行饱和加法，极端长会话不会回绕成小值。
    private static uint SaturatingAdd(uint left, uint right)
    {
        // 先使用 64 位计算精确和。
        ulong sum = (ulong)left + right;
        // 超过 uint 上限时固定为最大值。
        return sum > uint.MaxValue ? uint.MaxValue : (uint)sum;
    }

    // 控制命令必须在链路连接时执行；调用者必须持有 _sync。
    private void EnsureConnectedLocked()
    {
        // 未连接时模拟真实 GATT 写入失败。
        if (!_isConnected)
        {
            // 抛出明确错误供 UI 显示“请先连接设备”。
            throw new InvalidOperationException("设备未连接。");
        }
    }

    // 检查 Mock 是否已释放。
    private void ThrowIfDisposed()
    {
        // dispose 后命令属于调用错误。
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    // 保存单个动作的内部可变累计值，不向仓储或 UI 暴露。
    private sealed class MutableActionMetric
    {
        // 创建指定单位的零累计指标。
        public MutableActionMetric(MetricKind metricKind)
        {
            // 保存该动作固定单位。
            MetricKind = metricKind;
        }

        // 保存指标单位。
        public MetricKind MetricKind { get; }

        // 保存次数、步数或秒数。
        public uint MetricValue { get; set; }

        // 保存活动毫秒数。
        public uint ActiveMilliseconds { get; set; }

        // 保存千分之一千卡。
        public uint CaloriesMilliKcal { get; set; }
    }
}
