// 引入设备领域状态和会话摘要。
using FitnessCoach.Domain;
// 引入会话仓储接口。
using FitnessCoach.Infrastructure;
// 引入 MVVM 命令与通知基类。
using FitnessCoach.App.Mvvm;
// 引入 UI 调度和本地动画接口。
using FitnessCoach.App.Services;

// 实时训练页 ViewModel 位于应用视图模型命名空间。
namespace FitnessCoach.App.ViewModels;

/// <summary>显示设备权威实时状态并控制开始、暂停、恢复和停止。</summary>
public sealed class LiveTrainingViewModel : ObservableObject, IDisposable
{
    // 保存设备会话，所有计数和卡路里都以设备为准。
    private readonly IDeviceSession _deviceSession;
    // 保存幂等历史仓储。
    private readonly ISessionRepository _sessionRepository;
    // 保存 UI 调度器，后台状态通知不得直接写 WPF 属性。
    private readonly IUiDispatcher _dispatcher;
    // 保存本地动画控制器。
    private readonly IActionAnimationController _animationController;
    // 保存 Windows 辅助功能与本次运行的减少动画设置。
    private readonly IAnimationPreferences _animationPreferences;
    // 当前设备状态。
    private FitnessDeviceState _deviceState = FitnessDeviceState.Idle;
    // 当前动作类别。
    private ActionId _action = ActionId.Unknown;
    // 当前指标单位。
    private MetricKind _metricKind = MetricKind.None;
    // 当前计数、步数或秒数。
    private uint _metricValue;
    // 当前会话毫秒时长。
    private uint _elapsedMilliseconds;
    // 当前千卡。
    private double _caloriesKcal;
    // 当前电量，-1 表示未知。
    private int _batteryPercent = -1;
    // 当前置信度百分比。
    private double _confidencePercent;
    // 最新状态修订号，用于丢弃乱序通知。
    private uint _stateRevision;
    // 页面状态提示。
    private string _statusMessage = "请先在设备页连接模拟设备。";
    // 当前动作诊断回退符号；正式实时页使用 CurrentAction 绘制矢量骨架。
    private string _actionGlyph = "?";
    // 当前动画修订号。
    private uint _animationRevision;
    // true 表示实时动画可以显示当前设备动作；断线时强制等待态。
    private bool _isAnimationConnected;
    // true 表示动画必须固定为代表性姿态，不运行循环和 revision 脉冲。
    private bool _reducedMotion;
    // 是否已释放订阅。
    private bool _disposed;

    /// <summary>创建实时训练页并订阅设备状态。</summary>
    public LiveTrainingViewModel(
        IDeviceSession deviceSession,
        ISessionRepository sessionRepository,
        IUiDispatcher dispatcher,
        IActionAnimationController animationController,
        IAnimationPreferences? animationPreferences = null)
    {
        // 验证设备会话。
        ArgumentNullException.ThrowIfNull(deviceSession);
        // 验证历史仓储。
        ArgumentNullException.ThrowIfNull(sessionRepository);
        // 验证 UI 调度器。
        ArgumentNullException.ThrowIfNull(dispatcher);
        // 验证动画控制器。
        ArgumentNullException.ThrowIfNull(animationController);
        // 保存设备会话。
        _deviceSession = deviceSession;
        // 保存历史仓储。
        _sessionRepository = sessionRepository;
        // 保存调度器。
        _dispatcher = dispatcher;
        // 保存动画控制器。
        _animationController = animationController;
        // 测试或旧调用未注入时使用“系统未禁用动画”的独立默认设置。
        _animationPreferences = animationPreferences ?? new AnimationPreferences(false);
        // 读取系统与用户合并后的有效减少动画状态。
        _reducedMotion = _animationPreferences.IsReducedMotionEnabled;
        // 创建开始命令，8% 以下禁止新会话。
        StartCommand = new AsyncRelayCommand(StartAsync, CanStart);
        // 创建暂停命令。
        PauseCommand = new AsyncRelayCommand(PauseAsync, () => _deviceSession.IsConnected && DeviceState == FitnessDeviceState.Running);
        // 创建恢复命令。
        ResumeCommand = new AsyncRelayCommand(ResumeAsync, () => _deviceSession.IsConnected && DeviceState == FitnessDeviceState.Paused);
        // 创建停止命令。
        StopCommand = new AsyncRelayCommand(StopAsync, () => _deviceSession.IsConnected && DeviceState is FitnessDeviceState.Preparing or FitnessDeviceState.Running or FitnessDeviceState.Paused);
        // 订阅后台权威状态事件。
        _deviceSession.StateChanged += OnStateChanged;
        // 订阅链路事件以刷新命令和提示。
        _deviceSession.ConnectionChanged += OnConnectionChanged;
        // 订阅动画资源变化。
        _animationController.VisualChanged += OnVisualChanged;
        // 订阅减少动画设置变化。
        _animationPreferences.Changed += OnAnimationPreferencesChanged;
    }

    /// <summary>会话成功停止和保存后发布摘要。</summary>
    public event EventHandler<TrainingSessionSummary>? SessionCompleted;

    /// <summary>开始新会话命令。</summary>
    public AsyncRelayCommand StartCommand { get; }

    /// <summary>暂停命令。</summary>
    public AsyncRelayCommand PauseCommand { get; }

    /// <summary>恢复命令。</summary>
    public AsyncRelayCommand ResumeCommand { get; }

    /// <summary>停止并保存命令。</summary>
    public AsyncRelayCommand StopCommand { get; }

    /// <summary>设备权威状态。</summary>
    public FitnessDeviceState DeviceState
    {
        // 返回当前状态。
        get => _deviceState;
        // 内部设置并通知状态文字和命令。
        private set
        {
            // 相同状态无需重复刷新。
            if (SetProperty(ref _deviceState, value))
            {
                // 状态中文文本属于计算属性。
                OnPropertyChanged(nameof(DeviceStateText));
                // 状态改变会影响四个控制命令。
                RefreshCommands();
            }
        }
    }

    /// <summary>设备状态中文文本。</summary>
    public string DeviceStateText => DisplayText.DeviceStateName(DeviceState);

    /// <summary>当前动作中文文本。</summary>
    public string ActionName => DisplayText.ActionName(_action);

    /// <summary>当前动作的一条标准姿态提示，帮助用户理解教练人偶。</summary>
    public string ActionCueText => DisplayText.ActionCue(_action);

    /// <summary>当前权威动作 ID；矢量控件直接使用该枚举选择姿态。</summary>
    public ActionId CurrentAction => _action;

    /// <summary>当前诊断回退符号；不再作为实时页主视觉。</summary>
    public string ActionGlyph
    {
        // 返回当前符号。
        get => _actionGlyph;
        // 动画服务内部更新。
        private set => SetProperty(ref _actionGlyph, value);
    }

    /// <summary>动画修订号，可用于后续本地逐帧控件触发一次脉冲。</summary>
    public uint AnimationRevision
    {
        // 返回当前修订号。
        get => _animationRevision;
        // 动画服务内部更新。
        private set => SetProperty(ref _animationRevision, value);
    }

    /// <summary>true 表示设备已连接且动画可以显示权威动作。</summary>
    public bool IsAnimationConnected
    {
        // 返回实时链路状态。
        get => _isAnimationConnected;
        // 链路事件和权威状态在 UI 调度器内更新该值。
        private set => SetProperty(ref _isAnimationConnected, value);
    }

    /// <summary>true 表示遵循系统或用户选择，禁用循环和修订脉冲。</summary>
    public bool ReducedMotion
    {
        // 返回当前有效设置。
        get => _reducedMotion;
        // 设置变化由共享动画偏好服务驱动。
        private set => SetProperty(ref _reducedMotion, value);
    }

    /// <summary>当前权威累计值。</summary>
    public uint MetricValue
    {
        // 返回数值。
        get => _metricValue;
        // 状态帧内部更新。
        private set => SetProperty(ref _metricValue, value);
    }

    /// <summary>当前累计单位。</summary>
    public string MetricUnit => DisplayText.MetricUnit(_metricKind);

    /// <summary>格式化会话时长。</summary>
    public string DurationText => TimeSpan.FromMilliseconds(_elapsedMilliseconds).ToString(@"hh\:mm\:ss");

    /// <summary>当前千卡，保留两位小数。</summary>
    public string CaloriesText => $"{_caloriesKcal:F2} 千卡";

    /// <summary>当前电量；未知时显示 --。</summary>
    public string BatteryText => _batteryPercent < 0 ? "--" : $"{_batteryPercent}%";

    /// <summary>当前置信度百分比。</summary>
    public string ConfidenceText => $"{_confidencePercent:F1}%";

    /// <summary>产品低电策略提示。</summary>
    public string PowerHint => _batteryPercent switch
    {
        // 未知电量提示等待 PMIC。
        < 0 => "电量未知",
        // 5% 及以下设备应保存并关机。
        >= 0 and <= 5 => "电量≤5%：设备将保存会话并关机",
        // 8% 及以下禁止开始新会话。
        <= 8 => "电量≤8%：禁止开始新会话",
        // 15% 及以下显示告警。
        <= 15 => "电量≤15%：低电量告警",
        // 正常电量无需告警。
        _ => "电量正常",
    };

    /// <summary>用户可见状态提示。</summary>
    public string StatusMessage
    {
        // 返回提示。
        get => _statusMessage;
        // 内部更新提示。
        private set => SetProperty(ref _statusMessage, value);
    }

    /// <summary>最近应用的设备修订号，供诊断和线程测试使用。</summary>
    public uint StateRevision
    {
        // 返回修订号。
        get => _stateRevision;
        // 仅允许内部更新。
        private set => SetProperty(ref _stateRevision, value);
    }

    // 判断是否允许开始新会话。
    private bool CanStart()
    {
        // 必须已连接、设备空闲或总结、且电量未知或高于 8%。
        return _deviceSession.IsConnected
            && DeviceState is FitnessDeviceState.Idle or FitnessDeviceState.Summary
            && ((_batteryPercent < 0) || (_batteryPercent > 8));
    }

    // 发送开始命令。
    private async Task StartAsync()
    {
        // 使用统一包装把异常显示在页面。
        await ExecuteDeviceCommandAsync(_deviceSession.StartAsync, "训练已开始；有效计次时设备振动 30 毫秒。 ").ConfigureAwait(true);
    }

    // 发送暂停命令。
    private async Task PauseAsync()
    {
        // 暂停时设备保留累计值。
        await ExecuteDeviceCommandAsync(_deviceSession.PauseAsync, "训练已暂停，累计值保留。 ").ConfigureAwait(true);
    }

    // 发送恢复命令。
    private async Task ResumeAsync()
    {
        // 恢复同一设备会话。
        await ExecuteDeviceCommandAsync(_deviceSession.ResumeAsync, "训练已恢复。 ").ConfigureAwait(true);
    }

    // 停止设备会话并幂等保存摘要。
    private async Task StopAsync()
    {
        try
        {
            // 请求设备返回权威摘要；重复停止返回同一摘要。
            TrainingSessionSummary? summary = await _deviceSession.StopAsync().ConfigureAwait(true);
            // Idle 没有会话时保持提示，不写空记录。
            if (summary is null)
            {
                // 告知用户没有可保存会话。
                StatusMessage = "当前没有可停止的训练会话。";
                // 结束空摘要路径。
                return;
            }

            // 按 device_id + session_seq 幂等保存历史。
            await _sessionRepository.SaveAsync(summary).ConfigureAwait(true);
            // 显示保存成功提示。
            StatusMessage = "训练已停止并保存到本地历史。";
            // 通知主页面切换总结页。
            SessionCompleted?.Invoke(this, summary);
        }
        catch (Exception exception)
        {
            // 保存或设备失败时显示原因，摘要不会被默默丢弃。
            StatusMessage = $"停止失败：{exception.Message}";
        }
        finally
        {
            // 状态改变后刷新按钮。
            RefreshCommands();
        }
    }

    // 执行单个设备命令并统一处理异常。
    private async Task ExecuteDeviceCommandAsync(Func<CancellationToken, Task> command, string successMessage)
    {
        try
        {
            // 调用设备异步命令，使用默认取消令牌。
            await command(CancellationToken.None).ConfigureAwait(true);
            // 显示成功提示。
            StatusMessage = successMessage;
        }
        catch (Exception exception)
        {
            // 显示明确设备失败原因。
            StatusMessage = $"设备命令失败：{exception.Message}";
        }
        finally
        {
            // 刷新命令状态。
            RefreshCommands();
        }
    }

    // 接收后台状态事件并调度到 UI 线程。
    private void OnStateChanged(object? sender, LiveState state)
    {
        // UI 调度器保证属性更新串行。
        _ = _dispatcher.InvokeAsync(() => ApplyState(state));
    }

    // 接收链路变化并刷新提示和命令。
    private void OnConnectionChanged(object? sender, DeviceConnectionChangedEventArgs eventArgs)
    {
        // 切到 UI 线程更新状态。
        _ = _dispatcher.InvokeAsync(() =>
        {
            // 更新矢量控件连接态；断线时控件立即停止循环。
            IsAnimationConnected = eventArgs.IsConnected;
            // 断线后清除旧动作，禁止把最后一次识别继续显示为实时状态。
            if (!eventArgs.IsConnected)
            {
                // 本地动作切回 Unknown。
                _action = ActionId.Unknown;
                // 通知动作名称改为等待识别。
                OnPropertyChanged(nameof(ActionName));
                // 通知姿态提示切回单动作训练说明。
                OnPropertyChanged(nameof(ActionCueText));
                // 通知矢量控件切换到等待态。
                OnPropertyChanged(nameof(CurrentAction));
                // 驱动动画控制器递增修订并清除旧视觉描述。
                _animationController.SetAction(ActionId.Unknown);
            }

            // 显示连接或断开提示。
            StatusMessage = eventArgs.IsConnected ? "设备已连接，状态已同步。" : $"设备已断开：{eventArgs.Reason}";
            // 刷新控制命令。
            RefreshCommands();
        });
    }

    // 把权威状态复制到绑定属性。
    private void ApplyState(LiveState state)
    {
        // 同一会话中丢弃比当前更旧的乱序状态。
        if ((state.SessionSequence > 0U) && (state.StateRevision < StateRevision))
        {
            // 旧通知不覆盖新状态。
            return;
        }

        // 更新状态修订号。
        StateRevision = state.StateRevision;
        // 更新设备状态。
        DeviceState = state.DeviceState;
        // 收到权威状态意味着当前会话链路可用于动画显示。
        IsAnimationConnected = _deviceSession.IsConnected;
        // 保存动作枚举。
        _action = state.Action;
        // 通知动作中文文本。
        OnPropertyChanged(nameof(ActionName));
        // 通知标准姿态提示随权威动作更新。
        OnPropertyChanged(nameof(ActionCueText));
        // 通知矢量控件使用新的权威动作 ID。
        OnPropertyChanged(nameof(CurrentAction));
        // 保存指标单位。
        _metricKind = state.MetricKind;
        // 通知单位变化。
        OnPropertyChanged(nameof(MetricUnit));
        // 更新权威指标值。
        MetricValue = state.MetricValue;
        // 保存会话时长。
        _elapsedMilliseconds = state.ElapsedMilliseconds;
        // 通知格式化时长。
        OnPropertyChanged(nameof(DurationText));
        // 保存卡路里。
        _caloriesKcal = state.CaloriesKcal;
        // 通知卡路里文本。
        OnPropertyChanged(nameof(CaloriesText));
        // 把 255 未知电量映射为 -1。
        _batteryPercent = state.BatteryPercent == byte.MaxValue ? -1 : state.BatteryPercent;
        // 通知电量文本和低电提示。
        OnPropertyChanged(nameof(BatteryText));
        // 通知低电策略文本。
        OnPropertyChanged(nameof(PowerHint));
        // 保存百分比置信度。
        _confidencePercent = state.Confidence * 100.0;
        // 通知置信度文本。
        OnPropertyChanged(nameof(ConfidenceText));
        // 驱动本地动作视觉。
        _animationController.SetAction(state.Action);
        // 正常状态到达后显示权威状态说明。
        StatusMessage = $"状态已同步：{DisplayText.DeviceStateName(state.DeviceState)}，修订 {state.StateRevision}。";
        // 电量与状态更新后刷新开始条件。
        RefreshCommands();
    }

    // 接收动画控制器变化并同步绑定属性。
    private void OnVisualChanged(object? sender, EventArgs eventArgs)
    {
        // 当前调用已位于 ApplyState 的 UI 调度路径。
        ActionGlyph = _animationController.Current.Glyph;
        // 复制动画修订号。
        AnimationRevision = _animationController.Revision;
    }

    // 接收系统或用户减少动画设置变化。
    private void OnAnimationPreferencesChanged(object? sender, EventArgs eventArgs)
    {
        // 设置页通常位于 UI 线程，但仍统一经调度器保证线程安全。
        _ = _dispatcher.InvokeAsync(() =>
        {
            // 复制系统与用户合并后的有效状态。
            ReducedMotion = _animationPreferences.IsReducedMotionEnabled;
        });
    }

    // 刷新全部控制命令。
    private void RefreshCommands()
    {
        // 刷新开始按钮。
        StartCommand.RaiseCanExecuteChanged();
        // 刷新暂停按钮。
        PauseCommand.RaiseCanExecuteChanged();
        // 刷新恢复按钮。
        ResumeCommand.RaiseCanExecuteChanged();
        // 刷新停止按钮。
        StopCommand.RaiseCanExecuteChanged();
    }

    /// <inheritdoc />
    public void Dispose()
    {
        // 重复释放直接返回。
        if (_disposed)
        {
            // 已经解除全部事件订阅。
            return;
        }

        // 标记已释放。
        _disposed = true;
        // 解除状态事件。
        _deviceSession.StateChanged -= OnStateChanged;
        // 解除连接事件。
        _deviceSession.ConnectionChanged -= OnConnectionChanged;
        // 解除动画事件。
        _animationController.VisualChanged -= OnVisualChanged;
        // 解除动画设置事件，防止页面释放后仍被设置页引用。
        _animationPreferences.Changed -= OnAnimationPreferencesChanged;
    }
}
