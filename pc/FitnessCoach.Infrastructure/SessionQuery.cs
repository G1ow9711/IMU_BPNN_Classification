// 查询对象位于基础设施命名空间，接口形态可直接映射未来 SQLite LIMIT/OFFSET。
namespace FitnessCoach.Infrastructure;

/// <summary>
/// 定义历史会话分页、设备、日期和动作过滤条件。
/// </summary>
public sealed class SessionQuery
{
    /// <summary>可选设备 ID；null 或空字符串表示全部设备。</summary>
    public string? DeviceId { get; init; }

    /// <summary>可选 UTC 开始时间下界，满足 started_at_utc 大于等于该值。</summary>
    public DateTimeOffset? StartedOnOrAfterUtc { get; init; }

    /// <summary>可选 UTC 开始时间上界，满足 started_at_utc 严格小于该值。</summary>
    public DateTimeOffset? StartedBeforeUtc { get; init; }

    /// <summary>可选动作过滤；会话至少有一条该动作指标时命中。</summary>
    public FitnessCoach.Domain.ActionId? Action { get; init; }

    /// <summary>按开始时间倒序跳过的记录数，必须非负。</summary>
    public int Offset { get; init; }

    /// <summary>最多返回的记录数，范围 1～500。</summary>
    public int Limit { get; init; } = 100;

    /// <summary>
    /// 验证查询范围，JSON 与未来 SQLite 实现必须使用同一规则。
    /// </summary>
    public void Validate()
    {
        // 负偏移没有数据库语义，必须拒绝。
        if (Offset < 0)
        {
            // 抛出范围异常，调用者应修正分页状态。
            throw new ArgumentOutOfRangeException(nameof(Offset), "分页偏移不能为负数。");
        }

        // 限制单页记录数，避免界面误请求全部历史导致内存峰值。
        if ((Limit < 1) || (Limit > 500))
        {
            // 抛出范围异常并说明合法区间。
            throw new ArgumentOutOfRangeException(nameof(Limit), "分页数量必须位于 1..500。");
        }

        // 同时设置日期上下界时，下界必须早于上界，避免产生含义相反的空范围。
        if (StartedOnOrAfterUtc.HasValue && StartedBeforeUtc.HasValue &&
            (StartedOnOrAfterUtc.Value >= StartedBeforeUtc.Value))
        {
            // 抛出明确范围异常，调用者应修正日期控件。
            throw new ArgumentOutOfRangeException(nameof(StartedBeforeUtc), "历史结束日期必须晚于开始日期。");
        }

        // Unknown 只表示实时识别尚未稳定，不会作为已保存动作指标。
        if (Action == FitnessCoach.Domain.ActionId.Unknown)
        {
            // 拒绝永远无法命中的动作过滤值。
            throw new ArgumentOutOfRangeException(nameof(Action), "历史动作过滤不能使用 Unknown。");
        }
    }
}
