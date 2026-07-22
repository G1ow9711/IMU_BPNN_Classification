// 引入小端读写工具，实时状态字段不依赖运行主机端序。
using System.Buffers.Binary;
// 引入纯领域对象，解码后不向界面暴露字节偏移。
using FitnessCoach.Domain;

// 实时状态 codec 位于蓝牙层，负责 30 字节结构与领域对象之间转换。
namespace FitnessCoach.Bluetooth;

/// <summary>
/// 编解码 LiveStateV1 固定 30 字节 payload。
/// </summary>
public static class LiveStateCodec
{
    /// <summary>
    /// 把领域状态编码为 30 字节小端 payload，供模拟设备和黄金测试使用。
    /// </summary>
    public static byte[] Encode(LiveState state)
    {
        // 空状态对象没有可编码字段。
        ArgumentNullException.ThrowIfNull(state);
        // 按协议固定长度分配 payload。
        byte[] output = new byte[ProtocolConstants.LiveStatePayloadSize];

        // 偏移 0 写入持久化会话序号。
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(0, 4), state.SessionSequence);
        // 偏移 4 写入状态修订号。
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(4, 4), state.StateRevision);
        // 偏移 8 写入单调会话时长，单位毫秒。
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(8, 4), state.ElapsedMilliseconds);
        // 偏移 12 写入设备状态枚举。
        output[12] = (byte)state.DeviceState;
        // 偏移 13 写入固定模型动作索引。
        output[13] = (byte)state.Action;
        // 偏移 14 写入指标单位。
        output[14] = (byte)state.MetricKind;
        // 偏移 15 写入电量百分比或 255 未知值。
        output[15] = state.BatteryPercent;
        // 偏移 16 写入次数、步数或秒数。
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(16, 4), state.MetricValue);
        // 偏移 20 写入 Q15 置信度。
        BinaryPrimitives.WriteUInt16LittleEndian(output.AsSpan(20, 2), state.ConfidenceQ15);
        // 偏移 22 写入千分之一千卡。
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(22, 4), state.CaloriesMilliKcal);
        // 偏移 26 写入数据质量位集合。
        BinaryPrimitives.WriteUInt16LittleEndian(output.AsSpan(26, 2), (ushort)state.QualityFlags);
        // 偏移 28 写入电源状态位集合。
        output[28] = (byte)state.PowerFlags;
        // 偏移 29 写入目标百分比或 255 未设置值。
        output[29] = state.GoalPercent;

        // 返回完整固定长度 payload。
        return output;
    }

    /// <summary>
    /// 尝试把 30 字节 payload 解码为领域对象；失败时不产生部分状态。
    /// </summary>
    public static bool TryDecode(
        ReadOnlySpan<byte> payload,
        out LiveState? state,
        out string? error)
    {
        // 默认不返回领域对象。
        state = null;
        // 默认没有错误文本，失败分支会写入中文原因。
        error = null;

        // v1 payload 必须恰好为 30 字节，禁止尾随字段被旧代码误解。
        if (payload.Length != ProtocolConstants.LiveStatePayloadSize)
        {
            // 返回实际长度，便于定位协议版本或重组错误。
            error = $"LiveStateV1 长度应为 {ProtocolConstants.LiveStatePayloadSize} 字节，实际为 {payload.Length} 字节。";
            // 返回失败，禁止继续读取固定偏移。
            return false;
        }

        // 读取设备状态原始字节。
        byte rawDeviceState = payload[12];
        // v1 只接受 Booting 到 Error 的连续枚举值。
        if (rawDeviceState > (byte)FitnessDeviceState.Shutdown)
        {
            // 未知设备状态可能属于更高协议版本，不应映射成错误旧状态。
            error = $"未知设备状态值：{rawDeviceState}。";
            // 返回失败，连接协调器应检查协议版本。
            return false;
        }

        // 读取动作索引原始字节。
        byte rawAction = payload[13];
        // 动作只允许 0 到 10，或 255 表示未知。
        if ((rawAction > (byte)ActionId.Wave) && (rawAction != (byte)ActionId.Unknown))
        {
            // 类别表不一致时禁止播放错误动作动画。
            error = $"未知动作索引：{rawAction}。";
            // 返回失败，等待 Manifest 类别 CRC 检查。
            return false;
        }

        // 读取指标单位原始字节。
        byte rawMetricKind = payload[14];
        // v1 只定义 None、Repetition、Step 和 Second。
        if (rawMetricKind > (byte)MetricKind.Second)
        {
            // 未知单位不能安全显示为“次”。
            error = $"未知指标单位：{rawMetricKind}。";
            // 返回失败，防止业务含义错误。
            return false;
        }

        // 从偏移 0 读取会话序号。
        uint sessionSequence = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(0, 4));
        // 从偏移 4 读取状态修订号。
        uint stateRevision = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(4, 4));
        // 从偏移 8 读取单调会话时长，单位毫秒。
        uint elapsedMilliseconds = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(8, 4));
        // 从偏移 15 读取电量百分比或 255 未知值。
        byte batteryPercent = payload[15];
        // 从偏移 16 读取次数、步数或秒数。
        uint metricValue = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(16, 4));
        // 从偏移 20 读取 Q15 置信度。
        ushort confidenceQ15 = BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(20, 2));
        // 从偏移 22 读取千分之一千卡。
        uint caloriesMilliKcal = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(22, 4));
        // 从偏移 26 读取数据质量标志集合。
        DataQualityFlags qualityFlags = (DataQualityFlags)BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(26, 2));
        // 从偏移 28 读取电源标志集合。
        PowerFlags powerFlags = (PowerFlags)payload[28];
        // 从偏移 29 读取目标百分比或 255 未设置值。
        byte goalPercent = payload[29];

        // 领域构造函数继续校验电量和目标百分比范围。
        try
        {
            // 创建一次完整、不可变的权威设备状态。
            state = new LiveState(
                sessionSequence,
                stateRevision,
                elapsedMilliseconds,
                (FitnessDeviceState)rawDeviceState,
                (ActionId)rawAction,
                (MetricKind)rawMetricKind,
                batteryPercent,
                metricValue,
                confidenceQ15,
                caloriesMilliKcal,
                qualityFlags,
                powerFlags,
                goalPercent);
        }
        catch (ArgumentOutOfRangeException exception)
        {
            // 把领域范围错误转换为无异常的协议解码失败结果。
            error = exception.Message;
            // 保持输出状态为空。
            state = null;
            // 返回失败，通知处理器只记录错误而不更新界面。
            return false;
        }

        // 返回成功，调用者可按 StateRevision 更新界面状态库。
        return true;
    }
}
