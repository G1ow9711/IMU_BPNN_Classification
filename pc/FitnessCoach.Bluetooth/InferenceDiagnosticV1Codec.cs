// 引入显式小端读取工具，分类诊断不能依赖 PC CPU 端序。
using System.Buffers.Binary;
// 引入固定 11 类动作枚举，协议层解码后不把魔法数字暴露给界面。
using FitnessCoach.Domain;

// 双 M0 分类诊断协议位于蓝牙层，应用页只消费不可变对象。
namespace FitnessCoach.Bluetooth;

/// <summary>保存一次 62 点窗口的基础 M0、掩码 M0 和 0.85/0.15 融合诊断。</summary>
public sealed record InferenceDiagnosticV1(
    // 当前 payload 布局版本，v1 固定为一。
    byte DiagnosticVersion,
    // 融合 logits 的 Top-1 动作，Unknown 表示窗口推理失败。
    ActionId FusedAction,
    // 基础 M0 的 Top-1 动作，供现场判断两模型分歧。
    ActionId BaseAction,
    // 掩码 M0 的 Top-1 动作，供现场判断弱类补偿路径。
    ActionId MaskedAction,
    // 融合稳定 softmax Top-1 概率，取值 0～1。
    double FusedConfidence,
    // 基础模型稳定 softmax Top-1 概率，取值 0～1。
    double BaseConfidence,
    // 掩码模型稳定 softmax Top-1 概率，取值 0～1。
    double MaskedConfidence,
    // 当前推理窗口质量位低 16 位，零表示没有设备标记。
    ushort QualityFlags,
    // 自设备启动后递增的推理窗口序号，允许 uint32 自然回绕。
    uint WindowSequence,
    // 窗口末点的设备单调毫秒低 32 位。
    uint WindowEndMilliseconds,
    // 297 维特征提取和双 M0 前向总耗时，单位微秒。
    uint InferenceMicroseconds,
    // 设备启动后累计推理失败窗口数。
    uint FailureCount);

/// <summary>分类诊断事件；发送线程来自 BLE 通知泵，订阅者必须自行切换 UI 线程。</summary>
public sealed class InferenceDiagnosticReceivedEventArgs : EventArgs
{
    /// <summary>保存一次不可变分类诊断。</summary>
    public InferenceDiagnosticReceivedEventArgs(InferenceDiagnosticV1 diagnostic)
    {
        // 诊断对象不能为空，避免事件携带无效引用。
        ArgumentNullException.ThrowIfNull(diagnostic);
        // 保存调用者提供的不可变记录。
        Diagnostic = diagnostic;
    }

    /// <summary>最近一次完整且通过边界校验的 28 字节诊断。</summary>
    public InferenceDiagnosticV1 Diagnostic { get; }
}

/// <summary>严格解码 InferenceDiagnosticV1 固定 28 字节小端 payload。</summary>
public static class InferenceDiagnosticV1Codec
{
    /// <summary>v1 payload 固定为 4 个字节字段、4 个 u16 和 4 个 u32，共 28 字节。</summary>
    public const int PayloadSize = 28;
    // Q15 最大码值 65535 对应概率一。
    private const double Q15Maximum = 65535.0;

    /// <summary>解码固定布局；长度、版本或动作索引非法时不返回部分对象。</summary>
    public static bool TryDecode(
        ReadOnlySpan<byte> payload,
        out InferenceDiagnosticV1? diagnostic,
        out string? error)
    {
        // 失败前不暴露半解析对象。
        diagnostic = null;
        // 默认没有错误文本；失败分支写入具体原因。
        error = null;
        // 固定协议拒绝多余或缺失字节，防止未来版本被旧客户端错读。
        if (payload.Length != PayloadSize)
        {
            // 返回实际长度，便于定位 BLE 分片或固件版本问题。
            error = $"分类诊断 v1 必须为 {PayloadSize} 字节，实际为 {payload.Length} 字节。";
            // 长度错误时不执行任何偏移读取。
            return false;
        }

        // 偏移零是独立诊断版本；当前客户端只接受一。
        byte diagnosticVersion = payload[0];
        // 未知布局必须拒绝，不能按 v1 偏移猜测未来字段。
        if (diagnosticVersion != 1)
        {
            // 返回明确版本错误。
            error = $"不支持分类诊断版本 {diagnosticVersion}。";
            // 版本错误结束解码。
            return false;
        }

        // 严格解码融合动作；合法值为 0～10 或 255 Unknown。
        if (!TryDecodeAction(payload[1], out ActionId fusedAction))
        {
            // 标出融合字段非法原始值。
            error = $"融合动作索引 {payload[1]} 非法。";
            // 不创建错位动作对象。
            return false;
        }
        // 严格解码基础 M0 动作。
        if (!TryDecodeAction(payload[2], out ActionId baseAction))
        {
            // 标出基础模型字段非法值。
            error = $"基础模型动作索引 {payload[2]} 非法。";
            // 不创建对象。
            return false;
        }
        // 严格解码掩码 M0 动作。
        if (!TryDecodeAction(payload[3], out ActionId maskedAction))
        {
            // 标出掩码模型字段非法值。
            error = $"掩码模型动作索引 {payload[3]} 非法。";
            // 不创建对象。
            return false;
        }

        // 偏移 4 读取融合 Q15，并转换为 0～1 双精度显示值。
        double fusedConfidence = BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(4, 2)) / Q15Maximum;
        // 偏移 6 读取基础 M0 Q15。
        double baseConfidence = BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(6, 2)) / Q15Maximum;
        // 偏移 8 读取掩码 M0 Q15。
        double maskedConfidence = BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(8, 2)) / Q15Maximum;
        // 偏移 10 读取窗口质量位低 16 位。
        ushort qualityFlags = BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(10, 2));
        // 偏移 12 读取窗口序号。
        uint windowSequence = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(12, 4));
        // 偏移 16 读取窗口末点单调毫秒。
        uint windowEndMilliseconds = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(16, 4));
        // 偏移 20 读取特征提取与双 M0 前向耗时微秒。
        uint inferenceMicroseconds = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(20, 4));
        // 偏移 24 读取累计推理失败窗口数。
        uint failureCount = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(24, 4));
        // 创建完整不可变对象，字段顺序与设备端固定布局一致。
        diagnostic = new InferenceDiagnosticV1(
            diagnosticVersion,
            fusedAction,
            baseAction,
            maskedAction,
            fusedConfidence,
            baseConfidence,
            maskedConfidence,
            qualityFlags,
            windowSequence,
            windowEndMilliseconds,
            inferenceMicroseconds,
            failureCount);
        // 全部边界检查和偏移读取成功。
        return true;
    }

    // 把线上动作字节转换为固定领域枚举；未来未知类不能静默显示成当前动作。
    private static bool TryDecodeAction(byte value, out ActionId action)
    {
        // 十一个模型类别按 0～10 连续编码。
        if (value <= (byte)ActionId.Wave)
        {
            // 安全转换为固定类别枚举。
            action = (ActionId)value;
            // 返回成功。
            return true;
        }
        // 255 是协议固定 Unknown，用于失败或未形成结果的窗口。
        if (value == byte.MaxValue)
        {
            // 返回等待识别状态。
            action = ActionId.Unknown;
            // 返回成功。
            return true;
        }
        // 其余保留值先写 Unknown，防止调用方读取未赋值输出。
        action = ActionId.Unknown;
        // 返回失败。
        return false;
    }
}
