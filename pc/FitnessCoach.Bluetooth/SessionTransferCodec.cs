// 引入确定性小端读写，禁止依赖 Windows CPU 端序。
using System.Buffers.Binary;
// 引入领域动作、指标和本地历史摘要。
using FitnessCoach.Domain;

// 会话传输编解码位于纯协议项目，不依赖 WinRT、WPF 或磁盘。
namespace FitnessCoach.Bluetooth;

/// <summary>定义 TransferRequestV1 操作码。</summary>
public enum SessionTransferOperation : byte
{
    /// <summary>列出 cursor_session_seq 之后的摘要，按旧到新返回。</summary>
    List = 1,
    /// <summary>按精确 session_seq 读取一条摘要。</summary>
    Get = 2,
}

/// <summary>定义 TransferResponseV1 稳定线上状态。</summary>
public enum SessionTransferStatus : byte
{
    /// <summary>请求成功，允许返回空页。</summary>
    Ok = 0,
    /// <summary>载荷版本不受支持。</summary>
    UnsupportedVersion = 1,
    /// <summary>操作码非法。</summary>
    InvalidOperation = 2,
    /// <summary>请求号、页大小或游标非法。</summary>
    InvalidRequest = 3,
    /// <summary>精确读取的会话不存在。</summary>
    NotFound = 4,
    /// <summary>设备上一页尚未发送完成。</summary>
    Busy = 5,
    /// <summary>设备摘要仓储读取失败。</summary>
    StorageError = 6,
    /// <summary>同一 request_id 被复用于不同字节。</summary>
    RequestConflict = 7,
}

/// <summary>描述固定 16 字节传输响应。</summary>
/// <param name="Version">响应 payload 版本；当前固定为 1。</param>
/// <param name="Operation">回显 List 或 Get 操作码。</param>
/// <param name="Status">设备处理结果或稳定协议错误码。</param>
/// <param name="Flags">HasData 与 End 响应标志位集合。</param>
/// <param name="RequestId">回显 PC 非零幂等请求号。</param>
/// <param name="NextCursorSessionSequence">本页最后会话序号；空页保持输入游标。</param>
/// <param name="TotalCount">响应生成时设备持有的摘要总数，范围 0～200。</param>
/// <param name="ItemCount">本页后续数据帧数量，范围 0～12。</param>
public sealed record SessionTransferResponseV1(
    byte Version,
    SessionTransferOperation Operation,
    SessionTransferStatus Status,
    byte Flags,
    uint RequestId,
    uint NextCursorSessionSequence,
    ushort TotalCount,
    ushort ItemCount)
{
    /// <summary>当前页是否声明至少一条数据。</summary>
    public bool HasData => (Flags & SessionTransferCodec.ResponseFlagHasData) != 0;

    /// <summary>当前页是否已经追平设备摘要。</summary>
    public bool IsEnd => (Flags & SessionTransferCodec.ResponseFlagEnd) != 0;
}

/// <summary>保存设备 64 字节摘要全部字段，不在协议层截断 uint64。</summary>
/// <param name="SessionSequence">设备持久化会话序号。</param>
/// <param name="LastEventSequence">会话内最后持久化事件序号。</param>
/// <param name="ActionId">锁定动作索引，合法范围 0～10。</param>
/// <param name="MetricKind">设备指标类型：0 次、1 步、2 持续毫秒。</param>
/// <param name="Flags">摘要质量、结束原因和时间有效性位图。</param>
/// <param name="StartUnixMilliseconds">UTC 开始时间，单位毫秒；0 表示未校时。</param>
/// <param name="DurationMilliseconds">会话单调持续时间，单位毫秒。</param>
/// <param name="MetricTotal">最终次数、步数或持续毫秒值。</param>
/// <param name="GrossMicroKcal">毛热量，单位百万分之一千卡。</param>
/// <param name="ActiveMicroKcal">扣除静息代谢后的活动热量，单位百万分之一千卡。</param>
/// <param name="AverageStabilityQ15">全会话平均稳定度，0～65535 映射到 0～1。</param>
/// <param name="MinimumStabilityQ15">全会话最低稳定度，0～65535 映射到 0～1。</param>
/// <param name="EventCount">会话内持久化指标事件数量。</param>
public sealed record DeviceSessionSummaryV1(
    uint SessionSequence,
    uint LastEventSequence,
    byte ActionId,
    byte MetricKind,
    ushort Flags,
    ulong StartUnixMilliseconds,
    ulong DurationMilliseconds,
    ulong MetricTotal,
    ulong GrossMicroKcal,
    ulong ActiveMicroKcal,
    ushort AverageStabilityQ15,
    ushort MinimumStabilityQ15,
    uint EventCount)
{
    /// <summary>
    /// 转为 PC 领域摘要；deviceId 与 SessionSequence 保持当前原子 JSON 仓储的唯一键。
    /// 超过 PC v1 uint 范围的异常超长会话明确拒绝，不能静默截断。
    /// </summary>
    public TrainingSessionSummary ToTrainingSessionSummary(string deviceId)
    {
        // 设备 ID 不能为空，否则无法建立 (device_id,session_seq) 唯一键。
        if (string.IsNullOrWhiteSpace(deviceId))
        {
            // 拒绝无主键历史。
            throw new ArgumentException("设备 ID 不能为空。", nameof(deviceId));
        }

        // 当前领域层使用 uint 毫秒，超过约 49.7 天的单会话必须由未来模式升级处理。
        if (DurationMilliseconds > uint.MaxValue)
        {
            // 禁止持续时间回绕。
            throw new InvalidDataException("设备会话持续时间超过 PC v1 可表示范围。");
        }

        // 根据 session_store 指标枚举把持续毫秒转换为领域秒，其它指标保持整数值。
        ulong normalizedMetric = MetricKind == 2 ? MetricTotal / 1000UL : MetricTotal;
        // PC v1 动作指标使用 uint，异常超大累计必须拒绝。
        if (normalizedMetric > uint.MaxValue)
        {
            // 禁止指标截断。
            throw new InvalidDataException("设备会话指标超过 PC v1 可表示范围。");
        }

        // 毛热量从 microkcal 四舍五入到 PC 的 mcal。
        ulong caloriesMilliKcal64 = (GrossMicroKcal + 500UL) / 1000UL;
        // PC v1 热量使用 uint。
        if (caloriesMilliKcal64 > uint.MaxValue)
        {
            // 禁止热量回绕。
            throw new InvalidDataException("设备会话热量超过 PC v1 可表示范围。");
        }

        // 映射 session_store 的 0次、1步、2持续毫秒到领域 1次、2步、3秒。
        FitnessCoach.Domain.MetricKind domainMetricKind = MetricKind switch
        {
            // 重复次数。
            0 => FitnessCoach.Domain.MetricKind.Repetition,
            // 步数。
            1 => FitnessCoach.Domain.MetricKind.Step,
            // 持续秒数。
            2 => FitnessCoach.Domain.MetricKind.Second,
            // codec 已经拒绝其它值；该分支保护未来误用。
            _ => throw new InvalidDataException($"未知设备摘要指标类型 {MetricKind}。"),
        };
        // UTC 为 0 表示设备未校时，使用 UnixEpoch 作为明确占位而非 PC 当前时间猜测。
        DateTimeOffset startedAt = StartUnixMilliseconds == 0UL
            ? DateTimeOffset.UnixEpoch
            : DateTimeOffset.FromUnixTimeMilliseconds(checked((long)StartUnixMilliseconds));
        // 按设备单调持续时间计算结束 UTC；未校时会话仍保持自洽持续时间。
        DateTimeOffset endedAt = startedAt.AddMilliseconds((double)DurationMilliseconds);
        // 会话摘要只含单一已锁定动作，因此构造一项动作指标。
        ActionMetric metric = new(
            (ActionId)ActionId,
            domainMetricKind,
            checked((uint)normalizedMetric),
            checked((uint)DurationMilliseconds),
            checked((uint)caloriesMilliKcal64));
        // 返回由当前原子 JSON 仓储按相同复合键幂等替换的领域对象。
        return new TrainingSessionSummary(
            deviceId,
            SessionSequence,
            startedAt,
            endedAt,
            checked((uint)DurationMilliseconds),
            checked((uint)caloriesMilliKcal64),
            $"设备摘要 flags=0x{Flags:X4}",
            [metric]);
    }
}

/// <summary>描述一条固定 80 字节 TransferDataV1。</summary>
/// <param name="Version">数据 payload 版本；当前固定为 1。</param>
/// <param name="DataKind">数据类型；当前 1 表示 64 字节会话摘要。</param>
/// <param name="Flags">页尾和同步终点标志位集合。</param>
/// <param name="RequestId">对应 TransferRequest 的非零幂等请求号。</param>
/// <param name="ItemIndex">当前页零基条目索引。</param>
/// <param name="ItemCount">当前页声明的条目总数。</param>
/// <param name="TotalCount">设备当前持有的全部摘要数量。</param>
/// <param name="Summary">当前条目的完整设备会话摘要。</param>
public sealed record SessionTransferDataV1(
    byte Version,
    byte DataKind,
    byte Flags,
    uint RequestId,
    ushort ItemIndex,
    ushort ItemCount,
    ushort TotalCount,
    DeviceSessionSummaryV1 Summary)
{
    /// <summary>当前数据是否为页尾。</summary>
    public bool IsLastInPage => (Flags & SessionTransferCodec.DataFlagLastInPage) != 0;

    /// <summary>当前页是否为同步终点。</summary>
    public bool IsEnd => (Flags & SessionTransferCodec.DataFlagEnd) != 0;
}

/// <summary>描述 Windows 一次分页拉取结果。</summary>
public sealed record SessionTransferPage(
    uint NextCursorSessionSequence,
    ushort TotalCount,
    bool IsEnd,
    IReadOnlyList<TrainingSessionSummary> Summaries);

/// <summary>定义具备设备历史拉取能力的会话；应用可按需类型检查，不扩大通用 IDeviceSession。</summary>
public interface ISessionHistorySyncSource
{
    /// <summary>从设备拉取 cursor 之后一页摘要；结果按旧到新排列。</summary>
    Task<SessionTransferPage> PullSessionSummariesAsync(
        uint cursorSessionSequence,
        ushort pageSize = SessionTransferCodec.MaxPageSize,
        CancellationToken cancellationToken = default);
}

/// <summary>实现 Request12、Response16、Data80 的严格小端编解码。</summary>
public static class SessionTransferCodec
{
    // payload 自身版本固定为 1。
    public const byte Version = 1;
    // Request 固定 12 字节。
    public const int RequestSize = 12;
    // Response 固定 16 字节。
    public const int ResponseSize = 16;
    // Data 固定 80 字节。
    public const int DataSize = 80;
    // 单页最大 12 条，与设备 960 字节固定队列一致。
    public const ushort MaxPageSize = 12;
    // Response 有数据位。
    public const byte ResponseFlagHasData = 1 << 0;
    // Response 已结束位。
    public const byte ResponseFlagEnd = 1 << 1;
    // Data 页尾位。
    public const byte DataFlagLastInPage = 1 << 0;
    // Data 同步终点位。
    public const byte DataFlagEnd = 1 << 1;

    /// <summary>编码 LIST 请求；时间/空间复杂度均为 O(1)。</summary>
    public static byte[] EncodeListRequest(uint requestId, uint cursorSessionSequence, ushort pageSize)
    {
        // request_id 0 保留为未分配。
        if (requestId == 0U)
        {
            // 拒绝无法匹配响应的请求。
            throw new ArgumentOutOfRangeException(nameof(requestId));
        }

        // 页大小必须匹配设备固定队列上限。
        if ((pageSize == 0) || (pageSize > MaxPageSize))
        {
            // 拒绝空页和过大页。
            throw new ArgumentOutOfRangeException(nameof(pageSize));
        }

        // 分配精确 12 字节。
        byte[] output = new byte[RequestSize];
        // 写版本和 LIST 操作。
        output[0] = Version;
        output[1] = (byte)SessionTransferOperation.List;
        // 写页大小、请求号和已持久化最大序号。
        BinaryPrimitives.WriteUInt16LittleEndian(output.AsSpan(2, 2), pageSize);
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(4, 4), requestId);
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(8, 4), cursorSessionSequence);
        // 返回独立载荷。
        return output;
    }

    /// <summary>严格解码固定 16 字节响应。</summary>
    public static bool TryDecodeResponse(ReadOnlySpan<byte> input, out SessionTransferResponseV1? response, out string? error)
    {
        // 默认输出为空。
        response = null;
        error = null;
        // 长度必须精确匹配。
        if (input.Length != ResponseSize)
        {
            // 报告实际长度。
            error = $"TransferResponse 长度 {input.Length}，预期 {ResponseSize}。";
            return false;
        }

        // 校验版本、操作、状态和 flags。
        if ((input[0] != Version) || (input[1] is < 1 or > 2) || (input[2] > 7) || ((input[3] & 0xFC) != 0))
        {
            // 禁止猜测未知字段。
            error = "TransferResponse 版本、操作、状态或 flags 非法。";
            return false;
        }

        // 解码固定字段。
        uint requestId = BinaryPrimitives.ReadUInt32LittleEndian(input.Slice(4, 4));
        uint nextCursor = BinaryPrimitives.ReadUInt32LittleEndian(input.Slice(8, 4));
        ushort totalCount = BinaryPrimitives.ReadUInt16LittleEndian(input.Slice(12, 2));
        ushort itemCount = BinaryPrimitives.ReadUInt16LittleEndian(input.Slice(14, 2));
        // 数量不得超过设备合同。
        if ((totalCount > 200) || (itemCount > MaxPageSize))
        {
            // 拒绝可能造成过量等待的响应。
            error = "TransferResponse 数量字段超出范围。";
            return false;
        }

        // HAS_DATA 与 item_count 必须一致。
        bool hasData = (input[3] & ResponseFlagHasData) != 0;
        if (hasData != (itemCount > 0))
        {
            // 防止 PC 等待不存在的数据或忽略声明数据。
            error = "TransferResponse HAS_DATA 与 item_count 不一致。";
            return false;
        }

        // 构造不可变响应。
        response = new SessionTransferResponseV1(
            input[0],
            (SessionTransferOperation)input[1],
            (SessionTransferStatus)input[2],
            input[3],
            requestId,
            nextCursor,
            totalCount,
            itemCount);
        // 解码成功。
        return true;
    }

    /// <summary>严格解码固定 80 字节数据和内部 64 字节摘要。</summary>
    public static bool TryDecodeData(ReadOnlySpan<byte> input, out SessionTransferDataV1? data, out string? error)
    {
        // 默认输出为空。
        data = null;
        error = null;
        // 长度必须精确匹配。
        if (input.Length != DataSize)
        {
            // 报告长度错误。
            error = $"TransferData 长度 {input.Length}，预期 {DataSize}。";
            return false;
        }

        // 校验数据头固定字段和保留位。
        if ((input[0] != Version) || (input[1] != 1) || ((input[2] & 0xFC) != 0) || (input[3] != 0) ||
            (BinaryPrimitives.ReadUInt16LittleEndian(input.Slice(14, 2)) != 0))
        {
            // 未知版本/类型/保留字段不得进入历史库。
            error = "TransferData 版本、类型、flags 或保留字段非法。";
            return false;
        }

        // 读取页关联字段。
        uint requestId = BinaryPrimitives.ReadUInt32LittleEndian(input.Slice(4, 4));
        ushort itemIndex = BinaryPrimitives.ReadUInt16LittleEndian(input.Slice(8, 2));
        ushort itemCount = BinaryPrimitives.ReadUInt16LittleEndian(input.Slice(10, 2));
        ushort totalCount = BinaryPrimitives.ReadUInt16LittleEndian(input.Slice(12, 2));
        // 请求号和数组范围必须有效。
        if ((requestId == 0U) || (itemCount == 0) || (itemCount > MaxPageSize) || (itemIndex >= itemCount) || (totalCount > 200))
        {
            // 拒绝无法安全关联的数据。
            error = "TransferData 请求号或数量字段非法。";
            return false;
        }

        // 摘要位于偏移 16，先校验内部版本和固定长度 64。
        ReadOnlySpan<byte> summary = input.Slice(16, 64);
        if ((BinaryPrimitives.ReadUInt16LittleEndian(summary.Slice(0, 2)) != 1) ||
            (BinaryPrimitives.ReadUInt16LittleEndian(summary.Slice(2, 2)) != 64))
        {
            // 禁止把未知摘要格式按 v1 读取。
            error = "SessionSummary 版本或长度非法。";
            return false;
        }

        // 读取摘要枚举和 Q15。
        uint sessionSequence = BinaryPrimitives.ReadUInt32LittleEndian(summary.Slice(4, 4));
        byte actionId = summary[12];
        byte metricKind = summary[13];
        ushort averageStability = BinaryPrimitives.ReadUInt16LittleEndian(summary.Slice(56, 2));
        ushort minimumStability = BinaryPrimitives.ReadUInt16LittleEndian(summary.Slice(58, 2));
        // 主键、动作、指标和稳定度必须合法。
        if ((sessionSequence == 0U) || (actionId > 10) || (metricKind > 2) ||
            (averageStability > 32767) || (minimumStability > 32767))
        {
            // 拒绝异常摘要。
            error = "SessionSummary 主键、枚举或稳定度非法。";
            return false;
        }

        // 构造完整 64 字节摘要对象。
        DeviceSessionSummaryV1 decodedSummary = new(
            sessionSequence,
            BinaryPrimitives.ReadUInt32LittleEndian(summary.Slice(8, 4)),
            actionId,
            metricKind,
            BinaryPrimitives.ReadUInt16LittleEndian(summary.Slice(14, 2)),
            BinaryPrimitives.ReadUInt64LittleEndian(summary.Slice(16, 8)),
            BinaryPrimitives.ReadUInt64LittleEndian(summary.Slice(24, 8)),
            BinaryPrimitives.ReadUInt64LittleEndian(summary.Slice(32, 8)),
            BinaryPrimitives.ReadUInt64LittleEndian(summary.Slice(40, 8)),
            BinaryPrimitives.ReadUInt64LittleEndian(summary.Slice(48, 8)),
            averageStability,
            minimumStability,
            BinaryPrimitives.ReadUInt32LittleEndian(summary.Slice(60, 4)));
        // 构造页数据对象。
        data = new SessionTransferDataV1(
            input[0],
            input[1],
            input[2],
            requestId,
            itemIndex,
            itemCount,
            totalCount,
            decodedSummary);
        // 解码成功。
        return true;
    }
}
