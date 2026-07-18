// 引入领域会话摘要，仓储接口不暴露 JSON 或 SQLite 行结构。
using FitnessCoach.Domain;

// 仓储端口位于基础设施命名空间，App 通过接口注入具体实现。
namespace FitnessCoach.Infrastructure;

/// <summary>
/// 定义 SQLite 风格的幂等会话仓储边界；当前 JSON 实现与未来 SQLite 适配器共享该接口。
/// </summary>
public interface ISessionRepository
{
    /// <summary>创建目录、模式版本和空仓储；重复调用必须安全。</summary>
    Task InitializeAsync(CancellationToken cancellationToken = default);

    /// <summary>按 device_id + session_seq 幂等插入或替换会话。</summary>
    Task SaveAsync(TrainingSessionSummary summary, CancellationToken cancellationToken = default);

    /// <summary>按开始时间倒序返回分页结果。</summary>
    Task<IReadOnlyList<TrainingSessionSummary>> ListAsync(SessionQuery query, CancellationToken cancellationToken = default);

    /// <summary>按复合主键读取单个会话；不存在时返回 null。</summary>
    Task<TrainingSessionSummary?> GetAsync(string deviceId, uint sessionSequence, CancellationToken cancellationToken = default);
}
