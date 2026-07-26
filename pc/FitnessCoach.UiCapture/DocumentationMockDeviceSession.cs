// 引入设备领域状态、摘要和诊断接口。
using FitnessCoach.Domain;
// 引入 RawStream、分类诊断和低延迟事件接口。
using FitnessCoach.Bluetooth;

// 文档截图专用 Mock 位于独立命名空间，不进入正式运行时。
namespace FitnessCoach.UiCapture;

/// <summary>
/// 提供完全确定性的进程内设备状态；不创建 WinRT 对象、不扫描广播、不访问蓝牙适配器。
/// </summary>
internal sealed class DocumentationMockDeviceSession :
    IDeviceSession,
    IDeviceDiagnosticsSource,
    IRawStreamSource,
    IDeviceProtocolEventSource
{
    // 固定设备标识同时进入侧栏、历史和总结，便于教程读者对照数据流。
    private const string DocumentationDeviceId = "DOCS-MOCK-01";
    // 保存当前权威状态；所有命令只在内存中替换该不可变对象。
    private LiveState _currentState = CreateRunningState();
    // 保存连接事实；默认断开，待全部页面订阅事件后再显式连接，确保连接提示与生产链一致。
    private bool _isConnected = false;
    // 保存诊断流开关；仅影响是否接受 PublishRawSample 调用。
    private bool _isRawStreamEnabled = true;
    // 保存释放标记，重复 DisposeAsync 保持幂等。
    private bool _disposed;

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
    public string DeviceId => DocumentationDeviceId;

    /// <inheritdoc />
    public bool IsConnected => _isConnected;

    /// <inheritdoc />
    public bool IsHardwareBacked => false;

    /// <inheritdoc />
    public bool IsRawStreamEnabled => _isRawStreamEnabled;

    /// <inheritdoc />
    public Task ConnectAsync(CancellationToken cancellationToken = default)
    {
        // 在改变状态前响应取消。
        cancellationToken.ThrowIfCancellationRequested();
        // 重复连接保持幂等。
        if (_isConnected)
        {
            // 已连接时无需再次发布事件。
            return Task.CompletedTask;
        }
        // 提交进程内连接事实。
        _isConnected = true;
        // 发布明确 Mock 原因，页面不会误标为真机。
        ConnectionChanged?.Invoke(this, new DeviceConnectionChangedEventArgs(true, "文档模式：已连接确定性模拟设备。"));
        // 发布当前权威状态帮助页面恢复。
        StateChanged?.Invoke(this, _currentState);
        // 操作同步完成。
        return Task.CompletedTask;
    }

    /// <inheritdoc />
    public Task DisconnectAsync(CancellationToken cancellationToken = default)
    {
        // 在改变状态前响应取消。
        cancellationToken.ThrowIfCancellationRequested();
        // 重复断开保持幂等。
        if (!_isConnected)
        {
            // 已断开时直接返回。
            return Task.CompletedTask;
        }
        // 提交进程内断开事实。
        _isConnected = false;
        // 发布断开事件；不修改训练摘要或磁盘。
        ConnectionChanged?.Invoke(this, new DeviceConnectionChangedEventArgs(false, "文档模式：模拟链路已断开。"));
        // 操作同步完成。
        return Task.CompletedTask;
    }

    /// <inheritdoc />
    public Task<LiveState> GetSnapshotAsync(CancellationToken cancellationToken = default)
    {
        // 读取前响应取消。
        cancellationToken.ThrowIfCancellationRequested();
        // 返回当前不可变权威状态。
        return Task.FromResult(_currentState);
    }

    /// <inheritdoc />
    public Task StartAsync(CancellationToken cancellationToken = default)
    {
        // 在创建新状态前响应取消。
        cancellationToken.ThrowIfCancellationRequested();
        // 文档截图固定展示已识别的深蹲运行态，不使用真实时钟。
        PublishState(CreateRunningState());
        // 操作同步完成。
        return Task.CompletedTask;
    }

    /// <inheritdoc />
    public Task PauseAsync(CancellationToken cancellationToken = default)
    {
        // 在创建暂停快照前响应取消。
        cancellationToken.ThrowIfCancellationRequested();
        // 保留动作和累计，只把设备状态改为暂停并递增修订号。
        PublishState(new LiveState(
            _currentState.SessionSequence,
            _currentState.StateRevision + 1U,
            _currentState.ElapsedMilliseconds,
            FitnessDeviceState.Paused,
            _currentState.Action,
            _currentState.MetricKind,
            _currentState.BatteryPercent,
            _currentState.MetricValue,
            _currentState.ConfidenceQ15,
            _currentState.CaloriesMilliKcal,
            _currentState.QualityFlags,
            _currentState.PowerFlags,
            _currentState.GoalPercent));
        // 操作同步完成。
        return Task.CompletedTask;
    }

    /// <inheritdoc />
    public Task ResumeAsync(CancellationToken cancellationToken = default)
    {
        // 在恢复前响应取消。
        cancellationToken.ThrowIfCancellationRequested();
        // 恢复为固定运行快照，累计值不回退。
        PublishState(CreateRunningState(_currentState.StateRevision + 1U));
        // 操作同步完成。
        return Task.CompletedTask;
    }

    /// <inheritdoc />
    public Task<TrainingSessionSummary?> StopAsync(CancellationToken cancellationToken = default)
    {
        // 在返回摘要前响应取消。
        cancellationToken.ThrowIfCancellationRequested();
        // 使用固定时间点创建可重复的教程摘要。
        TrainingSessionSummary summary = CreateDocumentationSummaries()[0];
        // 返回同一业务内容，不写仓储或设备。
        return Task.FromResult<TrainingSessionSummary?>(summary);
    }

    /// <inheritdoc />
    public DeviceDiagnosticsSnapshot GetDiagnosticsSnapshot()
    {
        // 返回固定且明确标记 Mock 的诊断快照。
        return new DeviceDiagnosticsSnapshot(
            DocumentationDeviceId,
            _isConnected,
            isHardwareBacked: false,
            modelNumber: "ESP32-S3 Touch AMOLED 2.06（教程 Mock）",
            hardwareRevision: "Waveshare 2.06",
            firmwareRevision: "tutorial-preview",
            rssiDbm: -42,
            attMtu: 247,
            batteryPercent: _currentState.BatteryPercent,
            stateRevision: _currentState.StateRevision,
            reconnectCount: 0,
            crcErrorCount: 0,
            fragmentErrorCount: 0,
            manifestSummary: "教程 Mock；双模型与 297 维特征合同已加载；不使用真实 BLE");
    }

    /// <inheritdoc />
    public Task SetRawStreamEnabledAsync(bool enabled, CancellationToken cancellationToken = default)
    {
        // 在提交开关前响应取消。
        cancellationToken.ThrowIfCancellationRequested();
        // 只保存进程内布尔值，不发送任何 GATT 命令。
        _isRawStreamEnabled = enabled;
        // 操作同步完成。
        return Task.CompletedTask;
    }

    /// <summary>向已订阅页面发布一条固定权威状态。</summary>
    public void PublishState(LiveState state)
    {
        // 状态不能为空。
        ArgumentNullException.ThrowIfNull(state);
        // 保存最近快照供连接恢复和诊断页读取。
        _currentState = state;
        // 仅连接时发布，语义与真实会话一致。
        if (_isConnected)
        {
            // 事件在当前 STA 线程同步发送，页面调度器保持确定性。
            StateChanged?.Invoke(this, state);
        }
    }

    /// <summary>发布一条 25 Hz 六轴样本；关闭诊断流时拒绝发布。</summary>
    public void PublishRawSample(RawImuSampleV1 sample)
    {
        // 样本不能为空。
        ArgumentNullException.ThrowIfNull(sample);
        // 只有连接且显式开启诊断流时才允许事件进入页面。
        if (_isConnected && _isRawStreamEnabled)
        {
            // 使用正式事件对象传递不可变六轴记录。
            RawSampleReceived?.Invoke(this, new RawImuSampleReceivedEventArgs(sample));
        }
    }

    /// <summary>发布一条双模型分类诊断，供训练监测页展示一致性与耗时。</summary>
    public void PublishInference(InferenceDiagnosticV1 diagnostic)
    {
        // 诊断不能为空。
        ArgumentNullException.ThrowIfNull(diagnostic);
        // 只有连接且诊断流开启时发布。
        if (_isConnected && _isRawStreamEnabled)
        {
            // 使用正式事件包装，页面无需知道截图器实现。
            InferenceDiagnosticReceived?.Invoke(this, new InferenceDiagnosticReceivedEventArgs(diagnostic));
        }
    }

    /// <summary>发布一次权威计数位置，页面只标记事件而不自行增加累计。</summary>
    public void PublishMetricEvent(uint monotonicMilliseconds, uint eventSequence, uint metricTotal)
    {
        // 构造经过正式范围校验的 EventV1。
        DeviceEventV1 deviceEvent = new(
            eventVersion: 1,
            eventType: DeviceEventType.RepetitionCounted,
            deviceState: FitnessDeviceState.Running,
            action: ActionId.Squat,
            metricKind: MetricKind.Repetition,
            batteryPercent: 87,
            qualityFlags: 0,
            sessionSequence: 42,
            eventSequence: eventSequence,
            stateRevision: _currentState.StateRevision,
            metricDelta: 1,
            metricTotal: metricTotal,
            caloriesMilliKcal: _currentState.CaloriesMilliKcal,
            confidenceQ15: _currentState.ConfidenceQ15,
            detailCode: 0);
        // 空 payload 足以满足页面结构化事件消费；截图器不测试字节编解码。
        ProtocolEventReceived?.Invoke(
            this,
            new DeviceProtocolEventEventArgs(
                sequence: checked((ushort)eventSequence),
                monotonicMilliseconds,
                deviceEvent,
                ReadOnlyMemory<byte>.Empty));
    }

    /// <summary>返回固定教程历史；调用者可直接填充内存仓储和总结页。</summary>
    public static IReadOnlyList<TrainingSessionSummary> CreateDocumentationSummaries()
    {
        // 固定 UTC 基准避免不同日期运行时截图文本变化。
        DateTimeOffset baseTime = new(2026, 7, 23, 14, 30, 0, TimeSpan.Zero);
        // 返回三类代表性会话，覆盖次数和步数单位。
        return
        [
            // 最近一次深蹲会话展示总结页的当前产品语义。
            new TrainingSessionSummary(
                DocumentationDeviceId,
                sessionSequence: 42,
                startedAtUtc: baseTime,
                endedAtUtc: baseTime.AddSeconds(45),
                elapsedMilliseconds: 45000,
                caloriesMilliKcal: 1210,
                endReason: "用户停止",
                actionMetrics:
                [
                    new ActionMetric(ActionId.Squat, MetricKind.Repetition, 8, 39200, 1210),
                ]),
            // 开合跳会话展示历史列表中的另一种动作。
            new TrainingSessionSummary(
                DocumentationDeviceId,
                sessionSequence: 41,
                startedAtUtc: baseTime.AddMinutes(-8),
                endedAtUtc: baseTime.AddMinutes(-8).AddSeconds(38),
                elapsedMilliseconds: 38000,
                caloriesMilliKcal: 1680,
                endReason: "用户停止",
                actionMetrics:
                [
                    new ActionMetric(ActionId.JumpingJack, MetricKind.Repetition, 10, 32700, 1680),
                ]),
            // 行走会话覆盖步数单位，帮助教程解释 MetricKind。
            new TrainingSessionSummary(
                DocumentationDeviceId,
                sessionSequence: 40,
                startedAtUtc: baseTime.AddMinutes(-18),
                endedAtUtc: baseTime.AddMinutes(-18).AddSeconds(52),
                elapsedMilliseconds: 52000,
                caloriesMilliKcal: 920,
                endReason: "用户停止",
                actionMetrics:
                [
                    new ActionMetric(ActionId.Walk, MetricKind.Step, 24, 46800, 920),
                ]),
        ];
    }

    /// <inheritdoc />
    public ValueTask DisposeAsync()
    {
        // 重复释放保持幂等。
        if (_disposed)
        {
            // 已释放时返回已完成 ValueTask。
            return ValueTask.CompletedTask;
        }
        // 标记释放并停止后续连接态发布。
        _disposed = true;
        // 清除连接事实，不发布 UI 事件，截图流程已经结束。
        _isConnected = false;
        // 返回已完成 ValueTask。
        return ValueTask.CompletedTask;
    }

    // 创建主截图使用的运行态快照。
    private static LiveState CreateRunningState(uint stateRevision = 19)
    {
        // 返回固定深蹲会话：八次、45 秒、87% 电量、94% 置信度。
        return new LiveState(
            sessionSequence: 42,
            stateRevision,
            elapsedMilliseconds: 45000,
            deviceState: FitnessDeviceState.Running,
            action: ActionId.Squat,
            metricKind: MetricKind.Repetition,
            batteryPercent: 87,
            metricValue: 8,
            confidenceQ15: 61603,
            caloriesMilliKcal: 1210,
            qualityFlags: DataQualityFlags.None,
            powerFlags: PowerFlags.UsbPresent,
            goalPercent: 37);
    }
}
