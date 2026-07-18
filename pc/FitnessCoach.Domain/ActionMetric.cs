// 动作指标模型位于领域层，用于设备摘要、历史仓储和导出共享。
namespace FitnessCoach.Domain;

/// <summary>
/// 保存一个动作在会话中的次数、步数或秒数，以及活动时长和卡路里。
/// </summary>
public sealed class ActionMetric
{
    /// <summary>
    /// 创建动作指标；所有时间和能量使用整数单位，避免序列化浮点误差。
    /// </summary>
    public ActionMetric(ActionId action, MetricKind metricKind, uint metricValue, uint activeMilliseconds, uint caloriesMilliKcal)
    {
        // 保存固定模型动作索引。
        Action = action;
        // 保存指标单位，决定 MetricValue 的业务含义。
        MetricKind = metricKind;
        // 保存次数、步数或秒数。
        MetricValue = metricValue;
        // 保存该动作累计活动时间，单位为毫秒。
        ActiveMilliseconds = activeMilliseconds;
        // 保存该动作估算能量，单位为千分之一千卡。
        CaloriesMilliKcal = caloriesMilliKcal;
    }

    /// <summary>动作类别。</summary>
    public ActionId Action { get; }

    /// <summary>指标单位。</summary>
    public MetricKind MetricKind { get; }

    /// <summary>次数、步数或秒数。</summary>
    public uint MetricValue { get; }

    /// <summary>活动时长，单位毫秒。</summary>
    public uint ActiveMilliseconds { get; }

    /// <summary>估算能量，单位千分之一千卡。</summary>
    public uint CaloriesMilliKcal { get; }
}
