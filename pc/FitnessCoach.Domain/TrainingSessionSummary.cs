// 会话摘要位于领域层，保证 Mock、BLE、仓储和 UI 使用相同结构。
namespace FitnessCoach.Domain;

/// <summary>
/// 表示设备停止会话后生成的不可变摘要。
/// </summary>
public sealed class TrainingSessionSummary
{
    /// <summary>
    /// 创建完整会话摘要；设备 ID 与会话序号共同构成幂等主键。
    /// </summary>
    /// <param name="deviceId">设备稳定 ID；不能为空或纯空白。</param>
    /// <param name="sessionSequence">设备持久化会话序号。</param>
    /// <param name="startedAtUtc">会话 UTC 开始时间。</param>
    /// <param name="endedAtUtc">会话 UTC 结束时间；不得早于开始时间。</param>
    /// <param name="elapsedMilliseconds">设备单调会话时长，单位毫秒。</param>
    /// <param name="caloriesMilliKcal">会话累计热量，单位 0.001 kcal。</param>
    /// <param name="endReason">用户停止、低电量或错误等结束原因。</param>
    /// <param name="actionMetrics">各动作最终指标集合；引用不得为空。</param>
    public TrainingSessionSummary(
        string deviceId,
        uint sessionSequence,
        DateTimeOffset startedAtUtc,
        DateTimeOffset endedAtUtc,
        uint elapsedMilliseconds,
        uint caloriesMilliKcal,
        string endReason,
        IReadOnlyList<ActionMetric> actionMetrics)
    {
        // 设备 ID 不能为空，否则无法与其它设备会话去重。
        if (string.IsNullOrWhiteSpace(deviceId))
        {
            // 拒绝缺少持久化主键的摘要。
            throw new ArgumentException("设备 ID 不能为空。", nameof(deviceId));
        }

        // 结束时间不得早于开始时间。
        if (endedAtUtc < startedAtUtc)
        {
            // 拒绝墙钟时间顺序错误的摘要。
            throw new ArgumentOutOfRangeException(nameof(endedAtUtc), "结束时间不能早于开始时间。");
        }

        // 动作指标集合不能为空引用；空列表仍表示合法空会话。
        ArgumentNullException.ThrowIfNull(actionMetrics);
        // 保存设备全局 ID。
        DeviceId = deviceId;
        // 保存设备持久化会话序号。
        SessionSequence = sessionSequence;
        // 统一保存 UTC 开始时间。
        StartedAtUtc = startedAtUtc.ToUniversalTime();
        // 统一保存 UTC 结束时间。
        EndedAtUtc = endedAtUtc.ToUniversalTime();
        // 保存设备单调时长，单位毫秒。
        ElapsedMilliseconds = elapsedMilliseconds;
        // 保存总估算能量，单位千分之一千卡。
        CaloriesMilliKcal = caloriesMilliKcal;
        // 保存结束原因，例如用户停止、低电量或错误。
        EndReason = string.IsNullOrWhiteSpace(endReason) ? "未知" : endReason;
        // 复制列表，避免仓储写入期间外部修改指标。
        ActionMetrics = actionMetrics.ToArray();
    }

    /// <summary>设备全局 ID。</summary>
    public string DeviceId { get; }

    /// <summary>设备持久化会话序号。</summary>
    public uint SessionSequence { get; }

    /// <summary>UTC 开始时间。</summary>
    public DateTimeOffset StartedAtUtc { get; }

    /// <summary>UTC 结束时间。</summary>
    public DateTimeOffset EndedAtUtc { get; }

    /// <summary>设备单调会话时长，单位毫秒。</summary>
    public uint ElapsedMilliseconds { get; }

    /// <summary>总估算能量，单位千分之一千卡。</summary>
    public uint CaloriesMilliKcal { get; }

    /// <summary>会话结束原因。</summary>
    public string EndReason { get; }

    /// <summary>各动作的最终累计指标。</summary>
    public IReadOnlyList<ActionMetric> ActionMetrics { get; }

    /// <summary>用于 SQLite 或 JSON 幂等保存的可读复合键。</summary>
    public string StorageKey => $"{DeviceId}:{SessionSequence}";

    /// <summary>用于界面显示的总千卡。</summary>
    public double CaloriesKcal => CaloriesMilliKcal / 1000.0;
}
