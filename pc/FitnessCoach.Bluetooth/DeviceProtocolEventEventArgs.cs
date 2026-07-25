// 协议事件对象位于蓝牙层，领域层仍以设备权威 LiveState 作为累计值唯一来源。
namespace FitnessCoach.Bluetooth;

/// <summary>定义设备低延迟事件源；累计值仍以 LiveState 为准，事件仅提供精确发生时刻。</summary>
public interface IDeviceProtocolEventSource
{
    /// <summary>收到完整且通过 EventV1 边界校验的设备事件时触发。</summary>
    event EventHandler<DeviceProtocolEventEventArgs>? ProtocolEventReceived;
}

/// <summary>
/// 描述一个经过逻辑帧 CRC 校验的 Event 消息；事件只触发动画和诊断，不在 PC 端自行增加次数。
/// </summary>
public sealed class DeviceProtocolEventEventArgs : EventArgs
{
    /// <summary>创建已复制的事件 payload。</summary>
    public DeviceProtocolEventEventArgs(
        ushort sequence,
        uint monotonicMilliseconds,
        DeviceEventV1 deviceEvent,
        ReadOnlyMemory<byte> payload)
    {
        // 解码事件为必填对象，避免订阅者再次猜测原始偏移。
        ArgumentNullException.ThrowIfNull(deviceEvent);
        // 保存事件特征独立逻辑序号，用于诊断通知缺口。
        Sequence = sequence;
        // 保存设备开机后的单调毫秒时间，不把它解释为 UTC。
        MonotonicMilliseconds = monotonicMilliseconds;
        // 保存已通过版本和范围检查的事件字段。
        Event = deviceEvent;
        // 复制 payload，保证 BLE 回调和重组缓冲释放后仍可安全读取。
        Payload = payload.ToArray();
    }

    /// <summary>事件特征上的逻辑帧序号。</summary>
    public ushort Sequence { get; }

    /// <summary>设备开机单调毫秒时间。</summary>
    public uint MonotonicMilliseconds { get; }

    /// <summary>经过 EventV1Codec 校验的结构化事件。</summary>
    public DeviceEventV1 Event { get; }

    /// <summary>事件业务 payload；PC 不据此修改权威次数。</summary>
    public ReadOnlyMemory<byte> Payload { get; }
}
