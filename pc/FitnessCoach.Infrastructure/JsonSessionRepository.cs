// 引入 UTF-8 编码控制，确保中文动作和结束原因不依赖系统代码页。
using System.Text;
// 引入 System.Text.Json，当前实现无需外部数据库包即可离线构建。
using System.Text.Json;
// 引入领域会话摘要。
using FitnessCoach.Domain;

// JSON 仓储实现位于基础设施命名空间，可被未来 SQLite 实现替换。
namespace FitnessCoach.Infrastructure;

/// <summary>
/// 使用单个 JSON 文件实现线程安全、幂等和原子替换的会话仓储。
/// </summary>
public sealed class JsonSessionRepository : ISessionRepository, IDisposable
{
    // 当前文件模式版本为 1，未来迁移必须显式处理旧版本。
    private const int CurrentSchemaVersion = 1;
    // 仓储文件绝对路径。
    private readonly string _filePath;
    // 信号量串行化同进程读写，模拟 SQLite 单事务边界。
    private readonly SemaphoreSlim _gate = new(1, 1);
    // JSON 选项固定驼峰命名、缩进和大小写兼容读取。
    private readonly JsonSerializerOptions _jsonOptions = new()
    {
        // 写入易审计的缩进 JSON。
        WriteIndented = true,
        // 线上字段使用驼峰命名，便于未来导出和其它语言读取。
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        // 读取时允许旧文件使用不同属性大小写。
        PropertyNameCaseInsensitive = true,
    };
    // dispose 后禁止继续访问已释放信号量。
    private bool _disposed;

    /// <summary>
    /// 创建 JSON 会话仓储。
    /// </summary>
    public JsonSessionRepository(string filePath)
    {
        // 文件路径不能为空。
        if (string.IsNullOrWhiteSpace(filePath))
        {
            // 拒绝不确定存储位置。
            throw new ArgumentException("会话仓储路径不能为空。", nameof(filePath));
        }

        // 保存规范化绝对路径，避免工作目录变化影响数据位置。
        _filePath = Path.GetFullPath(filePath);
    }

    /// <inheritdoc />
    public async Task InitializeAsync(CancellationToken cancellationToken = default)
    {
        // 检查对象还可使用。
        ThrowIfDisposed();
        // 等待当前仓储事务完成。
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            // 获取仓储文件所在目录；绝对路径正常情况下始终有目录。
            string directory = Path.GetDirectoryName(_filePath) ?? throw new InvalidOperationException("无法解析会话仓储目录。");
            // 创建目录，重复调用安全。
            Directory.CreateDirectory(directory);

            // 已有文件只执行模式读取验证，不覆盖用户历史。
            if (File.Exists(_filePath))
            {
                // 读取并验证模式版本和 JSON 结构。
                _ = await LoadEnvelopeUnlockedAsync(cancellationToken).ConfigureAwait(false);
                // 初始化完成。
                return;
            }

            // 创建空的 v1 仓储信封。
            RepositoryEnvelope envelope = new();
            // 原子写入首个仓储文件。
            await WriteEnvelopeUnlockedAsync(envelope, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            // 无论成功、取消或异常都释放事务锁。
            _gate.Release();
        }
    }

    /// <inheritdoc />
    public async Task SaveAsync(TrainingSessionSummary summary, CancellationToken cancellationToken = default)
    {
        // 摘要不能为空。
        ArgumentNullException.ThrowIfNull(summary);
        // 检查对象还可使用。
        ThrowIfDisposed();
        // 串行化读取、替换和写回，防止并发丢失更新。
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            // 读取当前完整仓储。
            RepositoryEnvelope envelope = await LoadEnvelopeUnlockedAsync(cancellationToken).ConfigureAwait(false);
            // 按复合主键查找已有记录。
            int existingIndex = envelope.Sessions.FindIndex(item => string.Equals(item.StorageKey, summary.StorageKey, StringComparison.Ordinal));

            // 已有同主键记录时执行幂等替换。
            if (existingIndex >= 0)
            {
                // 用设备最新摘要替换旧值，不增加列表数量。
                envelope.Sessions[existingIndex] = summary;
            }
            else
            {
                // 新主键追加一条会话。
                envelope.Sessions.Add(summary);
            }

            // 按开始时间倒序整理文件，便于人工审计和历史页读取。
            envelope.Sessions = envelope.Sessions.OrderByDescending(item => item.StartedAtUtc).ToList();
            // 原子写回整个小型仓储；SQLite 适配器未来会替换该步骤为事务 UPSERT。
            await WriteEnvelopeUnlockedAsync(envelope, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            // 释放事务锁。
            _gate.Release();
        }
    }

    /// <inheritdoc />
    public async Task<IReadOnlyList<TrainingSessionSummary>> ListAsync(SessionQuery query, CancellationToken cancellationToken = default)
    {
        // 查询对象不能为空。
        ArgumentNullException.ThrowIfNull(query);
        // 验证分页范围。
        query.Validate();
        // 检查对象还可使用。
        ThrowIfDisposed();
        // 读取期间也持有锁，避免读到原子替换前后的不同文件状态。
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            // 读取当前仓储快照。
            RepositoryEnvelope envelope = await LoadEnvelopeUnlockedAsync(cancellationToken).ConfigureAwait(false);
            // 从全部记录开始构造只读查询。
            IEnumerable<TrainingSessionSummary> queryable = envelope.Sessions;

            // 指定设备 ID 时只返回该设备记录。
            if (!string.IsNullOrWhiteSpace(query.DeviceId))
            {
                // 使用序号比较，设备 ID 大小写由协议固定。
                queryable = queryable.Where(item => string.Equals(item.DeviceId, query.DeviceId, StringComparison.Ordinal));
            }

            // 指定开始时间下界时，保留同一时刻和更晚开始的会话。
            if (query.StartedOnOrAfterUtc.HasValue)
            {
                // 转为 UTC 后比较，避免本地夏令时或时区影响结果。
                DateTimeOffset lowerBoundUtc = query.StartedOnOrAfterUtc.Value.ToUniversalTime();
                // 延迟执行下界过滤，后续仍可叠加动作条件。
                queryable = queryable.Where(item => item.StartedAtUtc >= lowerBoundUtc);
            }

            // 指定开始时间上界时，使用严格小于实现自然日结束的半开区间。
            if (query.StartedBeforeUtc.HasValue)
            {
                // 转为 UTC 后比较，保证仓储内 UTC 字段口径一致。
                DateTimeOffset upperBoundUtc = query.StartedBeforeUtc.Value.ToUniversalTime();
                // 排除上界时刻及更晚会话。
                queryable = queryable.Where(item => item.StartedAtUtc < upperBoundUtc);
            }

            // 指定动作时，会话任一指标命中即可保留。
            if (query.Action.HasValue)
            {
                // 保存不可空动作值，避免闭包每行重复读取 nullable。
                FitnessCoach.Domain.ActionId action = query.Action.Value;
                // 使用领域指标而非显示字符串，防止中文翻译变化破坏查询。
                queryable = queryable.Where(item => item.ActionMetrics.Any(metric => metric.Action == action));
            }

            // 按 UTC 开始时间倒序，再执行 OFFSET/LIMIT。
            TrainingSessionSummary[] result = queryable
                .OrderByDescending(item => item.StartedAtUtc)
                .Skip(query.Offset)
                .Take(query.Limit)
                .ToArray();
            // 返回独立数组，释放锁后调用者不能修改仓储内部列表。
            return result;
        }
        finally
        {
            // 释放读取锁。
            _gate.Release();
        }
    }

    /// <inheritdoc />
    public async Task<TrainingSessionSummary?> GetAsync(string deviceId, uint sessionSequence, CancellationToken cancellationToken = default)
    {
        // 设备 ID 不能为空。
        if (string.IsNullOrWhiteSpace(deviceId))
        {
            // 拒绝无法组成复合键的查询。
            throw new ArgumentException("设备 ID 不能为空。", nameof(deviceId));
        }

        // 检查对象还可使用。
        ThrowIfDisposed();
        // 等待仓储事务锁。
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            // 读取当前仓储快照。
            RepositoryEnvelope envelope = await LoadEnvelopeUnlockedAsync(cancellationToken).ConfigureAwait(false);
            // 查找设备 ID 和会话序号同时匹配的唯一记录。
            return envelope.Sessions.FirstOrDefault(item =>
                string.Equals(item.DeviceId, deviceId, StringComparison.Ordinal) &&
                item.SessionSequence == sessionSequence);
        }
        finally
        {
            // 释放读取锁。
            _gate.Release();
        }
    }

    /// <summary>
    /// 释放并发锁；仓储文件本身不保持打开句柄。
    /// </summary>
    public void Dispose()
    {
        // 重复释放时不再次操作信号量。
        if (_disposed)
        {
            // 已释放对象直接返回。
            return;
        }

        // 标记对象已释放。
        _disposed = true;
        // 释放信号量持有的系统资源。
        _gate.Dispose();
    }

    // 在已经持有 _gate 时读取仓储信封，禁止外部直接调用。
    private async Task<RepositoryEnvelope> LoadEnvelopeUnlockedAsync(CancellationToken cancellationToken)
    {
        // 文件不存在表示尚未初始化，返回空 v1 信封。
        if (!File.Exists(_filePath))
        {
            // 创建内存空仓储，不在读取函数中产生磁盘副作用。
            return new RepositoryEnvelope();
        }

        // 以 UTF-8 异步读取完整小型 JSON 文件。
        string json = await File.ReadAllTextAsync(_filePath, Encoding.UTF8, cancellationToken).ConfigureAwait(false);
        // 空文件视为损坏，不能静默覆盖用户历史。
        if (string.IsNullOrWhiteSpace(json))
        {
            // 抛出明确数据错误，界面应提示导出或恢复备份。
            throw new InvalidDataException("会话仓储文件为空。");
        }

        // 反序列化模式信封和会话列表。
        RepositoryEnvelope? envelope = JsonSerializer.Deserialize<RepositoryEnvelope>(json, _jsonOptions);
        // null 表示 JSON 根结构不符合合同。
        if (envelope is null)
        {
            // 拒绝覆盖无法解析的文件。
            throw new InvalidDataException("无法解析会话仓储文件。");
        }

        // 当前实现只支持模式 v1。
        if (envelope.SchemaVersion != CurrentSchemaVersion)
        {
            // 后续 SQLite 或 JSON 迁移器必须显式升级旧模式。
            throw new InvalidDataException($"不支持的会话仓储模式版本：{envelope.SchemaVersion}。");
        }

        // 防御旧文件或手工编辑把 Sessions 写成 null。
        if (envelope.Sessions is null)
        {
            // 恢复为空列表，避免后续查询空引用。
            envelope.Sessions = new List<TrainingSessionSummary>();
        }
        // 返回经过模式检查的信封。
        return envelope;
    }

    // 在已经持有 _gate 时通过临时文件原子替换正式文件。
    private async Task WriteEnvelopeUnlockedAsync(RepositoryEnvelope envelope, CancellationToken cancellationToken)
    {
        // 把内存信封序列化为可审计 UTF-8 JSON。
        string json = JsonSerializer.Serialize(envelope, _jsonOptions);
        // 临时文件放在同一目录，保证 File.Move 原子替换不跨卷。
        string temporaryPath = _filePath + ".tmp";
        // 使用无 BOM UTF-8 写入临时文件。
        await File.WriteAllTextAsync(temporaryPath, json, new UTF8Encoding(false), cancellationToken).ConfigureAwait(false);
        // 原子替换正式文件；首次写入时同样适用。
        File.Move(temporaryPath, _filePath, true);
    }

    // 检查仓储是否已经释放。
    private void ThrowIfDisposed()
    {
        // dispose 后任何访问都属于调用错误。
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    // JSON 根信封保存模式版本和会话列表，未来 SQLite schema_version 使用同一版本语义。
    private sealed class RepositoryEnvelope
    {
        // 保存 JSON 模式版本。
        public int SchemaVersion { get; set; } = CurrentSchemaVersion;

        // 保存全部小型会话摘要；原始 IMU 数据不进入该列表。
        public List<TrainingSessionSummary> Sessions { get; set; } = new();
    }
}
