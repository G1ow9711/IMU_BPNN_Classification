// 逻辑帧对象位于蓝牙命名空间，只表达已经组装完成的应用层消息。
namespace FitnessCoach.Bluetooth;

/// <summary>
/// 表示通过 CRC 校验前或编码前的完整逻辑帧字段。
/// </summary>
public sealed class BleLogicalFrame
{
    /// <summary>
    /// 创建逻辑帧并复制 payload，避免调用者后续修改数组破坏待发送数据。
    /// </summary>
    public BleLogicalFrame(
        byte protocolMajor,
        byte protocolMinor,
        byte messageType,
        byte flags,
        ushort sequence,
        uint monotonicMilliseconds,
        ReadOnlySpan<byte> payload)
    {
        // payload 超过 1024 字节会突破 ESP32 固定重组缓冲区，因此拒绝。
        if (payload.Length > ProtocolConstants.MaxPayloadSize)
        {
            // 抛出明确范围异常，使调用者拆成会话传输块后重试。
            throw new ArgumentOutOfRangeException(nameof(payload), $"payload 不能超过 {ProtocolConstants.MaxPayloadSize} 字节。");
        }

        // 保存协议主版本，兼容性判断由连接协调器完成。
        ProtocolMajor = protocolMajor;
        // 保存协议次版本，允许未来追加可忽略字段。
        ProtocolMinor = protocolMinor;
        // 保存上层消息类型，例如实时状态或控制响应。
        MessageType = messageType;
        // 保存响应、错误、快照和结束块标志。
        Flags = flags;
        // 保存 16 位逻辑序号，用于发现通知缺口。
        Sequence = sequence;
        // 保存设备开机后的单调毫秒时间。
        MonotonicMilliseconds = monotonicMilliseconds;
        // 复制 payload，确保对象创建后内容保持不可变。
        Payload = payload.ToArray();
    }

    /// <summary>协议主版本。</summary>
    public byte ProtocolMajor { get; }

    /// <summary>协议次版本。</summary>
    public byte ProtocolMinor { get; }

    /// <summary>上层消息类型。</summary>
    public byte MessageType { get; }

    /// <summary>消息标志位。</summary>
    public byte Flags { get; }

    /// <summary>逻辑帧序号。</summary>
    public ushort Sequence { get; }

    /// <summary>设备单调毫秒时间。</summary>
    public uint MonotonicMilliseconds { get; }

    /// <summary>逻辑帧 payload 的私有副本。</summary>
    public ReadOnlyMemory<byte> Payload { get; }
}
