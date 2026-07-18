// 引入小端读写工具，控制请求和响应不能依赖 Windows CPU 本机端序。
using System.Buffers.Binary;

// 控制点编解码位于 BLE 协议层，供真机会话和 fake transport 测试共用。
namespace FitnessCoach.Bluetooth;

/// <summary>定义协议 v1 的 11 个控制命令 ID。</summary>
public enum ControlCommandId : byte
{
    /// <summary>开始新训练会话。</summary>
    Start = 1,
    /// <summary>暂停运行中的会话。</summary>
    Pause = 2,
    /// <summary>恢复暂停会话。</summary>
    Resume = 3,
    /// <summary>停止并保存当前会话。</summary>
    Stop = 4,
    /// <summary>清零暂停会话，设备端仍需二次确认配置。</summary>
    Reset = 5,
    /// <summary>同步 UTC 和时区，不改变单调计时。</summary>
    SyncTime = 6,
    /// <summary>设置体重等用户资料。</summary>
    SetProfile = 7,
    /// <summary>设置次数、时长或卡路里目标。</summary>
    SetGoal = 8,
    /// <summary>设置亮度、振动、声音和熄屏时间。</summary>
    SetPreferences = 9,
    /// <summary>要求设备发布权威 LiveState 快照。</summary>
    GetSnapshot = 10,
    /// <summary>开关开发者诊断六轴流；字段和值保持冻结协议不变。</summary>
    SetRawStream = 11,
}

/// <summary>保存设备 indication 返回的控制结果。</summary>
public sealed record ControlPointResponse(
    // request_id 必须与请求一致，重试时保持不变。
    uint RequestId,
    // command_id 用于防止另一并发命令的响应错误唤醒等待者。
    ControlCommandId CommandId,
    // status 为 0 表示成功，其它值必须作为 NACK 处理。
    byte Status,
    // error_code 由设备定义，0 表示没有错误。
    ushort ErrorCode,
    // state_revision 表示执行命令后设备权威状态版本。
    uint StateRevision,
    // 可选 TLV 由高层按命令解释，未知 TLV 可被忽略。
    ReadOnlyMemory<byte> OptionalTlv);

/// <summary>编码控制请求并解码固定 12 字节响应头。</summary>
public static class ControlPointCodec
{
    /// <summary>控制请求固定头为 request_id 4 字节、command_id 1 字节和 command_version 1 字节。</summary>
    public const int RequestHeaderSize = 6;

    /// <summary>控制响应固定头为 request_id 4、command_id 1、status 1、error 2、revision 4，共 12 字节。</summary>
    public const int ResponseHeaderSize = 12;

    /// <summary>编码一个 v1 控制请求，tlvData 可为空但不能超过逻辑帧 payload 上限。</summary>
    public static byte[] EncodeRequest(uint requestId, ControlCommandId commandId, ReadOnlySpan<byte> tlvData)
    {
        // 请求头与 TLV 总长不能突破逻辑帧 1024 字节 payload 上限。
        if (tlvData.Length > ProtocolConstants.MaxPayloadSize - RequestHeaderSize)
        {
            // 拒绝会导致 ESP32 固定接收缓冲区溢出的请求。
            throw new ArgumentOutOfRangeException(nameof(tlvData), "控制请求 TLV 超过协议 payload 上限。");
        }

        // 按精确长度分配请求 payload，避免尾部未定义字节进入 CRC。
        byte[] payload = new byte[RequestHeaderSize + tlvData.Length];
        // 偏移 0 写入小端 request_id；同一逻辑命令的重试必须复用该值。
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0, 4), requestId);
        // 偏移 4 写入命令 ID。
        payload[4] = (byte)commandId;
        // 偏移 5 写入命令结构版本 v1。
        payload[5] = 1;
        // 可选 TLV 从偏移 6 开始原样复制。
        tlvData.CopyTo(payload.AsSpan(RequestHeaderSize));
        // 返回可交给逻辑帧编码器的 payload。
        return payload;
    }

    /// <summary>尝试解析控制响应；未知 command_id 或短响应均拒绝进入命令状态机。</summary>
    public static bool TryDecodeResponse(ReadOnlySpan<byte> payload, out ControlPointResponse? response, out string? error)
    {
        // 默认不返回部分响应。
        response = null;
        // 默认无错误，失败分支写入具体原因。
        error = null;

        // 响应至少包含固定 12 字节头。
        if (payload.Length < ResponseHeaderSize)
        {
            // 返回实际长度，便于判断固件协议版本或分片缺失。
            error = $"控制响应至少需要 {ResponseHeaderSize} 字节，实际为 {payload.Length} 字节。";
            // 返回失败，禁止读取越界字段。
            return false;
        }

        // 读取命令原始值，v1 只允许 1～11。
        byte rawCommandId = payload[4];
        // 未知命令不能错误映射为已有控制操作。
        if ((rawCommandId < (byte)ControlCommandId.Start) || (rawCommandId > (byte)ControlCommandId.SetRawStream))
        {
            // 记录不兼容命令 ID。
            error = $"控制响应包含未知命令 ID：{rawCommandId}。";
            // 返回失败，等待 manifest 版本检查。
            return false;
        }

        // 从偏移 0 读取小端请求 ID。
        uint requestId = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(0, 4));
        // 从偏移 6 读取设备错误码。
        ushort errorCode = BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(6, 2));
        // 从偏移 8 读取命令执行后的权威状态修订号。
        uint stateRevision = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(8, 4));
        // 复制可选 TLV，确保输入帧释放后响应仍有效。
        byte[] optionalTlv = payload.Slice(ResponseHeaderSize).ToArray();
        // 创建完整不可变响应对象。
        response = new ControlPointResponse(requestId, (ControlCommandId)rawCommandId, payload[5], errorCode, stateRevision, optionalTlv);
        // 返回成功，调用者可按 request_id 完成对应等待任务。
        return true;
    }
}
