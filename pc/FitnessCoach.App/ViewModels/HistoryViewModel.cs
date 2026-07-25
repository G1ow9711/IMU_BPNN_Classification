// 引入可观察集合用于 WPF 列表和动作筛选绑定。
using System.Collections.ObjectModel;
// 引入协议数据异常，设备补传游标或主键错误不得污染本地历史。
using System.IO;
// 引入领域会话、动作和指标类型。
using FitnessCoach.Domain;
// 引入仓储查询接口。
using FitnessCoach.Infrastructure;
// 引入 MVVM 基础类型和异步命令。
using FitnessCoach.App.Mvvm;
// 引入 CSV 导出和保存位置服务。
using FitnessCoach.App.Services;
// 引入设备会话和会话摘要补传扩展接口。
using FitnessCoach.Bluetooth;

// 历史页 ViewModel 位于应用命名空间。
namespace FitnessCoach.App.ViewModels;

/// <summary>提供本地历史日期/动作筛选、选中详情和 CSV 导出。</summary>
public sealed class HistoryViewModel : ObservableObject, IDisposable
{
    // 保存会话仓储接口。
    private readonly ISessionRepository _repository;
    // 保存 CSV 编码器。
    private readonly IHistoryCsvExporter _csvExporter;
    // 保存 WPF 或测试用路径选择器。
    private readonly IHistoryExportDestinationPicker _destinationPicker;
    // 保存可选当前设备会话；未注入时历史页只读取本地 JSON。
    private readonly IDeviceSession? _deviceSession;
    // 保存可选设备历史补传能力。
    private readonly ISessionHistorySyncSource? _historySyncSource;
    // 保存可选 UI 调度器，用于 BLE 重连事件切回 WPF 线程。
    private readonly IUiDispatcher? _dispatcher;
    // 标记事件订阅是否已经解除。
    private bool _disposed;
    // 保存最近一次筛选得到的领域摘要，CSV 必须导出原始数值而非格式化文本。
    private IReadOnlyList<TrainingSessionSummary> _filteredSessions = [];
    // 当前用户提示。
    private string _statusMessage = "尚未加载历史。";
    // 可选本地开始日期；null 表示不限制下界。
    private DateTime? _dateFrom;
    // 可选本地结束日期；null 表示不限制上界。
    private DateTime? _dateTo;
    // 当前动作筛选，默认全部动作。
    private HistoryActionFilterOption _selectedActionFilter;
    // 当前表格选中行；null 表示不显示详情。
    private HistorySessionRow? _selectedSession;

    /// <summary>创建历史页并注入仓储、导出器和保存位置选择器。</summary>
    public HistoryViewModel(
        ISessionRepository repository,
        IHistoryCsvExporter csvExporter,
        IHistoryExportDestinationPicker destinationPicker,
        IDeviceSession? deviceSession = null,
        IUiDispatcher? dispatcher = null)
    {
        // 仓储不能为空。
        ArgumentNullException.ThrowIfNull(repository);
        // CSV 导出器不能为空。
        ArgumentNullException.ThrowIfNull(csvExporter);
        // 保存位置选择器不能为空。
        ArgumentNullException.ThrowIfNull(destinationPicker);
        // 保存仓储。
        _repository = repository;
        // 保存 CSV 导出器。
        _csvExporter = csvExporter;
        // 保存路径选择器。
        _destinationPicker = destinationPicker;
        // 保存可选设备会话。
        _deviceSession = deviceSession;
        // 只对实现补传接口的设备启用拉取。
        _historySyncSource = deviceSession as ISessionHistorySyncSource;
        // 保存 UI 调度器；设备为空时允许为空。
        _dispatcher = dispatcher;
        // 有设备但没有调度器会导致后台 BLE 事件跨线程更新集合，因此拒绝。
        if ((deviceSession is not null) && (dispatcher is null))
        {
            // 报告依赖配置错误。
            throw new ArgumentNullException(nameof(dispatcher), "注入设备会话时必须同时注入 UI 调度器。");
        }

        // 连接恢复时自动拉取设备摘要并刷新本地列表。
        if (_deviceSession is not null)
        {
            // 订阅连接事实；断开事件不发起补传。
            _deviceSession.ConnectionChanged += OnConnectionChanged;
        }
        // 初始化可观察历史集合。
        Sessions = new ObservableCollection<HistorySessionRow>();
        // 创建“全部动作”选项作为默认值。
        HistoryActionFilterOption allActions = new(null, "全部动作");
        // 初始化动作筛选集合并把全部选项放在首位。
        ActionFilters = new ObservableCollection<HistoryActionFilterOption> { allActions };

        // 遍历 11 个正式动作，不把 Unknown 加入可保存历史筛选。
        foreach (ActionId action in Enum.GetValues<ActionId>().Where(value => value != ActionId.Unknown))
        {
            // 追加领域枚举与中文名称组合。
            ActionFilters.Add(new HistoryActionFilterOption(action, DisplayText.ActionName(action)));
        }

        // 默认选择全部动作。
        _selectedActionFilter = allActions;
        // 创建刷新/筛选命令。
        RefreshCommand = new AsyncRelayCommand(RefreshAsync);
        // 创建 CSV 导出命令；没有筛选结果时禁用。
        ExportCsvCommand = new AsyncRelayCommand(ExportCsvAsync, () => _filteredSessions.Count > 0);
        // 创建清空筛选命令。
        ClearFiltersCommand = new AsyncRelayCommand(ClearFiltersAsync);
    }

    /// <summary>历史刷新和筛选命令。</summary>
    public AsyncRelayCommand RefreshCommand { get; }

    /// <summary>把当前筛选结果导出 CSV 的命令。</summary>
    public AsyncRelayCommand ExportCsvCommand { get; }

    /// <summary>清空日期和动作筛选命令。</summary>
    public AsyncRelayCommand ClearFiltersCommand { get; }

    /// <summary>最多 500 条筛选历史行。</summary>
    public ObservableCollection<HistorySessionRow> Sessions { get; }

    /// <summary>“全部动作”加 11 个正式动作选项。</summary>
    public ObservableCollection<HistoryActionFilterOption> ActionFilters { get; }

    /// <summary>本地开始日期，包含当天；null 表示不限。</summary>
    public DateTime? DateFrom
    {
        // 返回开始日期。
        get => _dateFrom;
        // 保存 DatePicker 值并通知绑定。
        set => SetProperty(ref _dateFrom, value?.Date);
    }

    /// <summary>本地结束日期，包含当天；null 表示不限。</summary>
    public DateTime? DateTo
    {
        // 返回结束日期。
        get => _dateTo;
        // 保存 DatePicker 值并通知绑定。
        set => SetProperty(ref _dateTo, value?.Date);
    }

    /// <summary>当前动作筛选选项。</summary>
    public HistoryActionFilterOption SelectedActionFilter
    {
        // 返回当前选项。
        get => _selectedActionFilter;
        // null 绑定回写时回退首项，避免查询状态不确定。
        set => SetProperty(ref _selectedActionFilter, value ?? ActionFilters[0]);
    }

    /// <summary>当前表格选中会话；详情面板直接绑定其只读字段。</summary>
    public HistorySessionRow? SelectedSession
    {
        // 返回选中行。
        get => _selectedSession;
        // 保存选中行并更新详情面板。
        set => SetProperty(ref _selectedSession, value);
    }

    /// <summary>加载、筛选或导出状态提示。</summary>
    public string StatusMessage
    {
        // 返回提示。
        get => _statusMessage;
        // 内部更新提示。
        private set => SetProperty(ref _statusMessage, value);
    }

    /// <summary>从仓储读取符合日期和动作条件的最多 500 条会话。</summary>
    public async Task RefreshAsync()
    {
        try
        {
            // 保存本轮设备补传状态；即使补传失败仍继续显示本地历史。
            string? synchronizationStatus = null;
            // 仅在连接且设备实现补传接口时尝试拉取。
            if ((_deviceSession?.IsConnected == true) && (_historySyncSource is not null))
            {
                try
                {
                    // 按本地最大序号游标分页拉取并幂等写 JSON。
                    int synchronizedCount = await SynchronizeDeviceHistoryAsync().ConfigureAwait(true);
                    // 记录本轮新增/替换条数。
                    synchronizationStatus = $"设备补传 {synchronizedCount} 条";
                }
                catch (Exception exception)
                {
                    // 补传失败不能清空既有本地历史。
                    synchronizationStatus = $"设备补传失败：{exception.Message}";
                }
            }

            // 验证本地日期顺序；结束日期包含整天，因此同日范围合法。
            if (DateFrom.HasValue && DateTo.HasValue && (DateFrom.Value.Date > DateTo.Value.Date))
            {
                // 使用用户可读异常进入统一状态消息。
                throw new ArgumentOutOfRangeException(nameof(DateTo), "结束日期不能早于开始日期。");
            }

            // 把本地开始日期午夜转换为 UTC 下界。
            DateTimeOffset? lowerUtc = DateFrom.HasValue
                ? new DateTimeOffset(DateFrom.Value.Date, TimeZoneInfo.Local.GetUtcOffset(DateFrom.Value.Date)).ToUniversalTime()
                : null;
            // 把结束日期下一天午夜转换为 UTC 排他上界，完整包含结束当天。
            DateTimeOffset? upperUtc = DateTo.HasValue
                ? new DateTimeOffset(DateTo.Value.Date.AddDays(1), TimeZoneInfo.Local.GetUtcOffset(DateTo.Value.Date.AddDays(1))).ToUniversalTime()
                : null;
            // 构造最大 500 条查询，避免长期历史一次占用无界 UI 内存。
            SessionQuery query = new()
            {
                // 当前页面不限制设备，允许查看多手柄历史。
                DeviceId = null,
                // 应用 UTC 下界。
                StartedOnOrAfterUtc = lowerUtc,
                // 应用 UTC 排他上界。
                StartedBeforeUtc = upperUtc,
                // null 表示全部动作。
                Action = SelectedActionFilter.Action,
                // 从最新记录开始。
                Offset = 0,
                // 历史页上限与仓储合同一致。
                Limit = 500,
            };
            // 异步读取按开始时间倒序的会话。
            IReadOnlyList<TrainingSessionSummary> sessions = await _repository.ListAsync(query).ConfigureAwait(true);
            // 保存独立数组，CSV 导出使用同一筛选快照。
            _filteredSessions = sessions.ToArray();
            // 清除旧列表。
            Sessions.Clear();

            // 把领域摘要转换为只读显示行。
            foreach (TrainingSessionSummary session in _filteredSessions)
            {
                // 追加含摘要详情的一条历史记录。
                Sessions.Add(HistorySessionRow.FromSummary(session));
            }

            // 默认选择第一条，空结果则清除详情。
            SelectedSession = Sessions.FirstOrDefault();
            // 刷新导出按钮状态。
            ExportCsvCommand.RaiseCanExecuteChanged();
            // 显示实际记录数量和筛选口径。
            StatusMessage = synchronizationStatus is null
                ? $"已加载 {Sessions.Count} 条本地历史；日期和动作条件已应用。"
                : $"{synchronizationStatus}；已加载 {Sessions.Count} 条本地历史；日期和动作条件已应用。";
        }
        catch (Exception exception)
        {
            // 查询失败时清除旧结果，避免用户误认为旧列表符合新筛选。
            _filteredSessions = [];
            // 清空可观察列表。
            Sessions.Clear();
            // 清除旧详情。
            SelectedSession = null;
            // 禁用导出。
            ExportCsvCommand.RaiseCanExecuteChanged();
            // 把文件、解析或范围错误显示给用户。
            StatusMessage = $"历史加载失败：{exception.Message}";
        }
    }

    // 从设备按 cursor 拉取全部未同步摘要，并由仓储复合键幂等写入。
    private async Task<int> SynchronizeDeviceHistoryAsync()
    {
        // 依赖检查在调用点已经完成；这里保留明确异常防止未来误用。
        IDeviceSession deviceSession = _deviceSession ?? throw new InvalidOperationException("未配置设备会话。" );
        // 获取补传来源。
        ISessionHistorySyncSource syncSource = _historySyncSource ?? throw new InvalidOperationException("设备不支持会话补传。" );
        // 查询当前设备最多 500 条本地摘要，确定已持久化最大序号。
        IReadOnlyList<TrainingSessionSummary> localSessions = await _repository.ListAsync(new SessionQuery
        {
            // 只查询当前物理设备，不能用另一设备游标。
            DeviceId = deviceSession.DeviceId,
            // 从最新记录开始即可求最大序号。
            Offset = 0,
            // 仓储 v1 上限为 500 条。
            Limit = 500,
        }).ConfigureAwait(true);
        // 空库从游标零开始，否则使用同设备最大 session_seq。
        uint cursor = localSessions.Count == 0 ? 0U : localSessions.Max(session => session.SessionSequence);
        // 统计本轮设备返回条数；幂等替换也计入同步事实。
        int synchronizedCount = 0;

        // 设备当前最多保存 200 条；32 页×12 足够且能阻止错误设备无限分页。
        for (int pageIndex = 0; pageIndex < 32; pageIndex++)
        {
            // 拉取 cursor 之后一页摘要。
            SessionTransferPage page = await syncSource.PullSessionSummariesAsync(cursor).ConfigureAwait(true);
            // 逐条验证设备 ID 并幂等保存。
            foreach (TrainingSessionSummary summary in page.Summaries)
            {
                // 当前页摘要必须属于连接设备，防止错误主键污染本地 JSON。
                if (!string.Equals(summary.DeviceId, deviceSession.DeviceId, StringComparison.Ordinal))
                {
                    // 拒绝来源不一致摘要。
            throw new InvalidDataException($"设备补传摘要标识 {summary.DeviceId} 与当前设备 {deviceSession.DeviceId} 不一致。" );
                }

                // 按 device_id + session_seq 幂等插入或替换。
                await _repository.SaveAsync(summary).ConfigureAwait(true);
                // 增加本轮处理条数。
                synchronizedCount++;
            }

            // 设备声明追平后结束分页。
            if (page.IsEnd)
            {
                // 返回本轮同步条数。
                return synchronizedCount;
            }

            // 非终点页必须严格推进游标，否则会无限重复同一页。
            if (page.NextCursorSessionSequence <= cursor)
            {
                // 报告设备协议错误且不再请求。
            throw new InvalidDataException("设备补传非终点页没有推进会话游标。" );
            }

            // 保存下一页游标。
            cursor = page.NextCursorSessionSequence;
        }

        // 超过 32 页说明设备违反最多 200 条合同或一直不结束。
        throw new InvalidDataException("设备补传超过 32 页仍未结束。" );
    }

    // 连接或自动重连成功后，在 UI 线程启动补传和列表刷新。
    private async void OnConnectionChanged(object? sender, DeviceConnectionChangedEventArgs eventArgs)
    {
        // 断开事件不发起 GATT 请求。
        if (!eventArgs.IsConnected || (_dispatcher is null))
        {
            // 等待下一次连接成功。
            return;
        }

        // 保存将在 UI 线程启动的异步刷新任务。
        Task refreshTask = Task.CompletedTask;
        // 切到 UI 线程创建 RefreshAsync，使其 ObservableCollection 续体保持 UI 上下文。
        await _dispatcher.InvokeAsync(() => refreshTask = RefreshAsync()).ConfigureAwait(false);
        // 等待刷新结束；RefreshAsync 内部已经把异常转为状态文本。
        await refreshTask.ConfigureAwait(false);
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

        // 标记释放。
        _disposed = true;
        // 注入设备时解除连接事件，避免关闭窗口后刷新集合。
        if (_deviceSession is not null)
        {
            // 解除事件订阅。
            _deviceSession.ConnectionChanged -= OnConnectionChanged;
        }
    }

    /// <summary>请求保存路径并导出当前筛选快照。</summary>
    public async Task ExportCsvAsync()
    {
        try
        {
            // 没有筛选结果时保持幂等并提示先刷新。
            if (_filteredSessions.Count == 0)
            {
                // 给出明确恢复方法。
                StatusMessage = "没有可导出的记录，请先刷新历史。";
                // 不打开空保存对话框。
                return;
            }

            // 使用本地日期时间生成稳定默认文件名。
            string suggestedName = $"健身训练历史-{DateTimeOffset.Now:yyyyMMdd-HHmmss}.csv";
            // 打开保存位置选择器。
            string? path = await _destinationPicker.PickCsvPathAsync(suggestedName).ConfigureAwait(true);
            // 用户取消不算错误。
            if (string.IsNullOrWhiteSpace(path))
            {
                // 显示取消状态，不改变当前筛选结果。
                StatusMessage = "已取消表格导出。";
                // 结束导出。
                return;
            }

            // 按原始领域数值导出，不使用界面格式化字符串。
            await _csvExporter.ExportAsync(_filteredSessions, path).ConfigureAwait(true);
            // 显示记录数量和目标路径。
            StatusMessage = $"已导出 {_filteredSessions.Count} 条会话：{path}";
        }
        catch (Exception exception)
        {
            // 文件权限、磁盘或编码异常转换为用户提示。
            StatusMessage = $"表格导出失败：{exception.Message}";
        }
    }

    // 清除筛选并重新读取全部最近历史。
    private async Task ClearFiltersAsync()
    {
        // 清空开始日期。
        DateFrom = null;
        // 清空结束日期。
        DateTo = null;
        // 选择全部动作。
        SelectedActionFilter = ActionFilters[0];
        // 等待刷新完成，防止快速点击与前一筛选结果交错覆盖。
        await RefreshAsync().ConfigureAwait(true);
    }
}

/// <summary>动作筛选选项；Action 为 null 表示全部动作。</summary>
public sealed record HistoryActionFilterOption(ActionId? Action, string DisplayName);

/// <summary>历史列表和详情面板共用的只读显示行。</summary>
public sealed class HistorySessionRow
{
    // 构造函数只由 FromSummary 调用，保证全部显示字段来自同一摘要。
    private HistorySessionRow(
        string storageKey,
        string deviceId,
        string startedAtText,
        string endedAtText,
        string durationText,
        string caloriesText,
        string endReason,
        string actionsText,
        string detailsText)
    {
        // 保存复合会话键。
        StorageKey = storageKey;
        // 保存设备标识。
        DeviceId = deviceId;
        // 保存本地开始时间文本。
        StartedAtText = startedAtText;
        // 保存本地结束时间文本。
        EndedAtText = endedAtText;
        // 保存时长文本。
        DurationText = durationText;
        // 保存总卡路里文本。
        CaloriesText = caloriesText;
        // 保存结束原因。
        EndReason = endReason;
        // 保存动作摘要文本。
        ActionsText = actionsText;
        // 保存多行详情。
        DetailsText = detailsText;
    }

    /// <summary>设备标识与会话序号组成的唯一键。</summary>
    public string StorageKey { get; }

    /// <summary>会话来源设备标识。</summary>
    public string DeviceId { get; }

    /// <summary>本地开始时间。</summary>
    public string StartedAtText { get; }

    /// <summary>本地结束时间。</summary>
    public string EndedAtText { get; }

    /// <summary>设备单调会话时长。</summary>
    public string DurationText { get; }

    /// <summary>总估算能量。</summary>
    public string CaloriesText { get; }

    /// <summary>设备会话结束原因。</summary>
    public string EndReason { get; }

    /// <summary>会话内有指标的动作中文名。</summary>
    public string ActionsText { get; }

    /// <summary>选中详情面板使用的多行文本。</summary>
    public string DetailsText { get; }

    /// <summary>从领域摘要构造列表行和完整动作明细。</summary>
    public static HistorySessionRow FromSummary(TrainingSessionSummary session)
    {
        // 摘要不能为空。
        ArgumentNullException.ThrowIfNull(session);
        // 转换本地开始时间。
        string started = session.StartedAtUtc.ToLocalTime().ToString("yyyy-MM-dd HH:mm:ss zzz");
        // 转换本地结束时间。
        string ended = session.EndedAtUtc.ToLocalTime().ToString("yyyy-MM-dd HH:mm:ss zzz");
        // 超过 24 小时的会话仍按总小时显示，而不是 TimeSpan.hh 回绕。
        string duration = $"{Math.Floor(TimeSpan.FromMilliseconds(session.ElapsedMilliseconds).TotalHours):00}:{TimeSpan.FromMilliseconds(session.ElapsedMilliseconds):mm\\:ss}";
        // 拼接所有动作中文名；空会话显示无动作。
        string actions = session.ActionMetrics.Count == 0
            ? "无动作指标"
            : string.Join("、", session.ActionMetrics.Select(metric => DisplayText.ActionName(metric.Action)));
        // 构建动作明细行集合。
        string[] metricLines = session.ActionMetrics.Select(metric =>
            $"  {DisplayText.ActionName(metric.Action)}：{metric.MetricValue}{DisplayText.MetricUnit(metric.MetricKind)}，活动 {metric.ActiveMilliseconds / 1000.0:F1} 秒，{metric.CaloriesMilliKcal / 1000.0:F3} 千卡")
            .ToArray();
        // 没有指标时使用稳定占位行。
        string metricDetails = metricLines.Length == 0 ? "  无动作指标" : string.Join(Environment.NewLine, metricLines);
        // 拼接不依赖 XAML 转换器的完整详情。
        string details =
            $"设备：{session.DeviceId}{Environment.NewLine}" +
            $"会话序号：{session.SessionSequence}{Environment.NewLine}" +
            $"开始：{started}{Environment.NewLine}" +
            $"结束：{ended}{Environment.NewLine}" +
            $"时长：{duration}{Environment.NewLine}" +
            $"总卡路里：{session.CaloriesKcal:F3} 千卡{Environment.NewLine}" +
            $"结束原因：{session.EndReason}{Environment.NewLine}" +
            $"动作明细：{Environment.NewLine}{metricDetails}";
        // 返回同一摘要生成的不可变行。
        return new HistorySessionRow(
            session.StorageKey,
            session.DeviceId,
            started,
            ended,
            duration,
            $"{session.CaloriesKcal:F2} 千卡",
            session.EndReason,
            actions,
            details);
    }
}
