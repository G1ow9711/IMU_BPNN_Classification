// 引入小端定宽整数编解码，禁止依赖 Windows 主机端序。
using System.Buffers.Binary;
// 引入领域枚举，解码后直接构造稳定业务类型。
using FitnessCoach.Domain;

// EventV1 codec 位于蓝牙层，与设备端 ble_service_encode_event_v1 逐偏移一致。
namespace FitnessCoach.Bluetooth;

/// <summary>编解码固定 36 字节 EventV1 payload。</summary>
public static class EventV1Codec
{
    /// <summary>把已校验事件编码为固定小端 payload。</summary>
    public static byte[] Encode(DeviceEventV1 value)
    {
        // 空事件无法编码，立即抛出参数异常。
        ArgumentNullException.ThrowIfNull(value);
        // 分配精确 36 字节，避免尾部未定义扩展数据。
        byte[] output = new byte[ProtocolConstants.EventPayloadSize];
        // 偏移 0 写入事件结构版本。
        output[0] = value.EventVersion;
        // 偏移 1 写入事件类型。
        output[1] = (byte)value.EventType;
        // 偏移 2 写入设备状态。
        output[2] = (byte)value.DeviceState;
        // 偏移 3 写入动作索引或 255 未知。
        output[3] = (byte)value.Action;
        // 偏移 4 写入指标单位。
        output[4] = (byte)value.MetricKind;
        // 偏移 5 写入电量百分比或 255 未知。
        output[5] = value.BatteryPercent;
        // 偏移 6 写入小端质量位集合。
        BinaryPrimitives.WriteUInt16LittleEndian(output.AsSpan(6, 2), value.QualityFlags);
        // 偏移 8 写入会话序号。
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(8, 4), value.SessionSequence);
        // 偏移 12 写入会话内事件序号。
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(12, 4), value.EventSequence);
        // 偏移 16 写入状态修订号。
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(16, 4), value.StateRevision);
        // 偏移 20 写入本次指标增量。
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(20, 4), value.MetricDelta);
        // 偏移 24 写入指标累计值。
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(24, 4), value.MetricTotal);
        // 偏移 28 写入千分之一千卡累计值。
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(28, 4), value.CaloriesMilliKcal);
        // 偏移 32 写入 Q15 稳定度或置信度。
        BinaryPrimitives.WriteUInt16LittleEndian(output.AsSpan(32, 2), value.ConfidenceQ15);
        // 偏移 34 写入事件专用原因子码。
        BinaryPrimitives.WriteUInt16LittleEndian(output.AsSpan(34, 2), value.DetailCode);
        // 返回独立 payload，调用者可安全交给异步 BLE 写入。
        return output;
    }

    /// <summary>从固定小端 payload 解码 EventV1；失败时返回中文原因且不产生部分对象。</summary>
    public static bool TryDecode(ReadOnlySpan<byte> payload, out DeviceEventV1? value, out string? error)
    {
        // 默认没有可用事件对象。
        value = null;
        // 默认没有错误文本，成功路径保持 null。
        error = null;
        // EventV1 必须严格为 36 字节，扩展版本应使用新 event_version 和新合同。
        if (payload.Length != ProtocolConstants.EventPayloadSize)
        {
            // 返回实际长度，便于诊断分片或版本错误。
            error = $"EventV1 长度应为 {ProtocolConstants.EventPayloadSize} 字节，实际为 {payload.Length} 字节。";
            // 拒绝不完整或多余字节。
            return false;
        }

        try
        {
            // 按设备端固定偏移构造经过范围校验的不可变事件。
            value = new DeviceEventV1(
                payload[0],
                (DeviceEventType)payload[1],
                (FitnessDeviceState)payload[2],
                (ActionId)payload[3],
                (MetricKind)payload[4],
                payload[5],
                BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(6, 2)),
                BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(8, 4)),
                BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(12, 4)),
                BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(16, 4)),
                BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(20, 4)),
                BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(24, 4)),
                BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(28, 4)),
                BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(32, 2)),
                BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(34, 2)));
            // 所有字段合法，返回成功。
            return true;
        }
        catch (ArgumentOutOfRangeException exception)
        {
            // 把构造器范围错误转成协议层可记录文本。
            error = exception.Message;
            // 确保失败时不泄漏部分对象。
            value = null;
            // 拒绝非法事件。
            return false;
        }
    }
}
