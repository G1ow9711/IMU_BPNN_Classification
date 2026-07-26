// 引入领域会话摘要和用户偏好，文档数据与正式页面使用同一类型。
using FitnessCoach.Domain;
// 引入仓储端口和查询对象，截图工具不访问 JSON 或用户目录。
using FitnessCoach.Infrastructure;
// 引入保存位置选择端口；文档模式永不打开系统对话框。
using FitnessCoach.App.Services;

// 文档截图辅助类型位于独立命名空间，防止进入正式应用依赖图。
namespace FitnessCoach.UiCapture;

/// <summary>仅驻留进程内存的确定性会话仓储；不会读取或写入任何本地文件。</summary>
internal sealed class DocumentationSessionRepository : ISessionRepository
{
    // 保存按复合键去重的教程会话；数据量固定且仅在截图进程内存在。
    private readonly Dictionary<string, TrainingSessionSummary> _sessions = new(StringComparer.Ordinal);

    /// <summary>使用固定教程摘要创建仓储。</summary>
    public DocumentationSessionRepository(IEnumerable<TrainingSessionSummary> sessions)
    {
        // 输入集合不能为空，否则历史页无法展示教程样例。
        ArgumentNullException.ThrowIfNull(sessions);
        // 遍历调用者提供的确定性摘要。
        foreach (TrainingSessionSummary session in sessions)
        {
            // 按设备标识和会话序号保存；重复键以后一个完整摘要为准。
            _sessions[session.StorageKey] = session;
        }
    }

    /// <inheritdoc />
    public Task InitializeAsync(CancellationToken cancellationToken = default)
    {
        // 在返回前响应取消，保持与正式异步仓储一致的调用语义。
        cancellationToken.ThrowIfCancellationRequested();
        // 内存字典构造时已经就绪，不创建目录或模式文件。
        return Task.CompletedTask;
    }

    /// <inheritdoc />
    public Task SaveAsync(TrainingSessionSummary summary, CancellationToken cancellationToken = default)
    {
        // 摘要不能为空，避免历史页出现无法展示的空项。
        ArgumentNullException.ThrowIfNull(summary);
        // 在修改内存前响应取消。
        cancellationToken.ThrowIfCancellationRequested();
        // 使用正式幂等主键覆盖旧值；不会产生磁盘副作用。
        _sessions[summary.StorageKey] = summary;
        // 操作同步完成，返回已完成任务。
        return Task.CompletedTask;
    }

    /// <inheritdoc />
    public Task<IReadOnlyList<TrainingSessionSummary>> ListAsync(
        SessionQuery query,
        CancellationToken cancellationToken = default)
    {
        // 查询对象不能为空。
        ArgumentNullException.ThrowIfNull(query);
        // 使用正式范围规则校验分页和动作条件。
        query.Validate();
        // 在枚举前响应取消。
        cancellationToken.ThrowIfCancellationRequested();
        // 从全部教程会话开始构造只读查询。
        IEnumerable<TrainingSessionSummary> selected = _sessions.Values;
        // 指定设备时只保留完全相同的设备标识。
        if (!string.IsNullOrWhiteSpace(query.DeviceId))
        {
            // 使用序号比较，设备 ID 不执行区域化转换。
            selected = selected.Where(session =>
                string.Equals(session.DeviceId, query.DeviceId, StringComparison.Ordinal));
        }
        // 指定开始时间下界时保留不早于下界的会话。
        if (query.StartedOnOrAfterUtc.HasValue)
        {
            // UTC 时间由领域对象统一保存，直接比较时间点。
            selected = selected.Where(session => session.StartedAtUtc >= query.StartedOnOrAfterUtc.Value);
        }
        // 指定开始时间上界时保留严格早于上界的会话。
        if (query.StartedBeforeUtc.HasValue)
        {
            // 严格上界与正式仓储合同一致。
            selected = selected.Where(session => session.StartedAtUtc < query.StartedBeforeUtc.Value);
        }
        // 指定动作时要求会话至少包含一条该动作指标。
        if (query.Action.HasValue)
        {
            // 只检查领域枚举，不从中文文本反解析类别。
            selected = selected.Where(session =>
                session.ActionMetrics.Any(metric => metric.Action == query.Action.Value));
        }
        // 固定按开始时间倒序，再应用分页，保证每次截图行顺序相同。
        TrainingSessionSummary[] result = selected
            .OrderByDescending(session => session.StartedAtUtc)
            .Skip(query.Offset)
            .Take(query.Limit)
            .ToArray();
        // 以只读接口返回确定性结果。
        return Task.FromResult<IReadOnlyList<TrainingSessionSummary>>(result);
    }

    /// <inheritdoc />
    public Task<TrainingSessionSummary?> GetAsync(
        string deviceId,
        uint sessionSequence,
        CancellationToken cancellationToken = default)
    {
        // 设备标识不能为空，否则无法构造复合键。
        if (string.IsNullOrWhiteSpace(deviceId))
        {
            // 与正式仓储一样拒绝无业务意义的空标识。
            throw new ArgumentException("设备标识不能为空。", nameof(deviceId));
        }
        // 在查找前响应取消。
        cancellationToken.ThrowIfCancellationRequested();
        // 按领域 StorageKey 格式构造键并读取可空结果。
        _sessions.TryGetValue($"{deviceId}:{sessionSequence}", out TrainingSessionSummary? summary);
        // 不复制不可变摘要，直接返回同一只读对象。
        return Task.FromResult(summary);
    }
}

/// <summary>固定返回教程偏好的内存设置仓储；不会触碰 LocalApplicationData。</summary>
internal sealed class DocumentationPreferencesStore : IUserPreferencesStore
{
    // 保存当前教程偏好；每次读取和保存都通过显式复制隔离调用者修改。
    private UserPreferences _preferences = CreateDefaultPreferences();

    /// <inheritdoc />
    public Task<UserPreferences> LoadAsync(CancellationToken cancellationToken = default)
    {
        // 在复制前响应取消。
        cancellationToken.ThrowIfCancellationRequested();
        // 返回新对象，防止设置页未保存输入直接改变仓储事实。
        return Task.FromResult(Clone(_preferences));
    }

    /// <inheritdoc />
    public Task SaveAsync(UserPreferences preferences, CancellationToken cancellationToken = default)
    {
        // 调用者必须提供完整偏好对象。
        ArgumentNullException.ThrowIfNull(preferences);
        // 在提交前响应取消。
        cancellationToken.ThrowIfCancellationRequested();
        // 复制全部教程相关字段，保证后续读取不受外部引用影响。
        _preferences = Clone(preferences);
        // 内存提交同步完成。
        return Task.CompletedTask;
    }

    // 创建用于设置页截图的安全默认值。
    private static UserPreferences CreateDefaultPreferences()
    {
        // 返回明确关闭真 BLE 的教程配置，避免任何未来调用误建硬件会话。
        return new UserPreferences
        {
            // 文档截图永远使用进程内 Mock。
            UseRealBleDevice = false,
            // 体重仅用于界面示例，单位千克。
            WeightKilograms = 65.0,
            // 教程主界面采用公制。
            UnitSystem = MeasurementUnitSystem.Metric,
            // 截图不播放声音。
            SoundEnabled = false,
            // AMOLED 演示亮度使用产品默认 35%。
            BrightnessPercent = 35,
            // 无操作熄屏演示值为 30 秒。
            ScreenTimeoutSeconds = 30,
            // 每日目标用于总结进度展示，单位千卡。
            DailyCalorieGoal = 300.0,
            // 文档模式展示训练监测页，因此启用开发者诊断入口。
            DeveloperModeEnabled = true,
            // 固定资料修订号，保证界面文本稳定。
            ProfileRevision = 3,
            // 固定偏好修订号，保证界面文本稳定。
            PreferencesRevision = 5,
        };
    }

    // 复制可变偏好对象的全部正式字段。
    private static UserPreferences Clone(UserPreferences source)
    {
        // 返回字段级副本；不访问序列化器或磁盘。
        return new UserPreferences
        {
            // 保留 Mock/真机选择；教程默认始终为 false。
            UseRealBleDevice = source.UseRealBleDevice,
            // 复制体重千克值。
            WeightKilograms = source.WeightKilograms,
            // 复制界面计量单位。
            UnitSystem = source.UnitSystem,
            // 复制提示音设置。
            SoundEnabled = source.SoundEnabled,
            // 复制 AMOLED 亮度。
            BrightnessPercent = source.BrightnessPercent,
            // 复制熄屏秒数。
            ScreenTimeoutSeconds = source.ScreenTimeoutSeconds,
            // 复制每日千卡目标。
            DailyCalorieGoal = source.DailyCalorieGoal,
            // 复制开发者诊断开关。
            DeveloperModeEnabled = source.DeveloperModeEnabled,
            // 复制资料修订号。
            ProfileRevision = source.ProfileRevision,
            // 复制偏好修订号。
            PreferencesRevision = source.PreferencesRevision,
        };
    }
}

/// <summary>文档模式取消全部文件选择；截图过程不会弹窗或写出 CSV。</summary>
internal sealed class DocumentationDestinationPicker :
    IHistoryExportDestinationPicker,
    IImuExportDestinationPicker
{
    /// <inheritdoc />
    public Task<string?> PickCsvPathAsync(string suggestedFileName)
    {
        // 建议文件名只用于界面命令，文档模式不创建目标文件。
        _ = suggestedFileName;
        // 返回 null 表示用户取消，保证任何误触导出命令都无文件副作用。
        return Task.FromResult<string?>(null);
    }
}
