// 引入显式小端写入工具，黄金 payload 不依赖测试机端序。
using System.Buffers.Binary;
// 引入分类诊断解码器。
using FitnessCoach.Bluetooth;
// 引入固定 11 类动作枚举。
using FitnessCoach.Domain;

// 分类诊断测试位于上位机综合测试命名空间。
namespace FitnessCoach.Tests;

/// <summary>验证设备与 PC 共用的 InferenceDiagnosticV1 固定 28 字节合同。</summary>
internal static class InferenceDiagnosticContractTests
{
    /// <summary>顺序执行黄金向量、长度、版本和动作边界测试。</summary>
    public static void RunAll()
    {
        // 验证全部字段偏移和 Q15 换算。
        TestGoldenVector();
        // 验证严格长度和版本拒绝。
        TestLengthAndVersionRejection();
        // 验证动作只允许 0～10 或 255。
        TestActionBoundaryRejection();
    }

    // 构造一个固定 28 字节黄金 payload 并逐字段核对。
    private static void TestGoldenVector()
    {
        // 分配固定 payload，所有未显式写入字节保持零。
        byte[] payload = new byte[InferenceDiagnosticV1Codec.PayloadSize];
        // 版本固定为一。
        payload[0] = 1;
        // 融合动作使用深蹲索引六。
        payload[1] = (byte)ActionId.Squat;
        // 基础模型动作使用深蹲。
        payload[2] = (byte)ActionId.Squat;
        // 掩码模型动作使用跳跃深蹲索引三，验证分歧字段。
        payload[3] = (byte)ActionId.JumpingSquat;
        // 融合概率使用 32768，对应约 50.0008%。
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(4, 2), 32768);
        // 基础模型概率使用最大值一。
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(6, 2), ushort.MaxValue);
        // 掩码模型概率使用零。
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(8, 2), 0);
        // 质量位使用固定十六进制向量。
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(10, 2), 0x1234);
        // 窗口序号使用固定 u32。
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(12, 4), 0x11223344U);
        // 窗口末点单调毫秒使用固定值。
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(16, 4), 80200U);
        // 推理耗时固定为 18.35 毫秒。
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(20, 4), 18350U);
        // 累计失败固定为七。
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(24, 4), 7U);
        // 解码完整黄金向量。
        bool decoded = InferenceDiagnosticV1Codec.TryDecode(
            payload,
            out InferenceDiagnosticV1? diagnostic,
            out string? error);
        // 核对解码成功且没有错误文本。
        Assert(decoded && diagnostic is not null && error is null, "分类诊断黄金向量解码失败。" );
        // 核对动作、质量、序号、时间、耗时和失败累计。
        Assert(
            diagnostic!.FusedAction == ActionId.Squat &&
            diagnostic.BaseAction == ActionId.Squat &&
            diagnostic.MaskedAction == ActionId.JumpingSquat &&
            diagnostic.QualityFlags == 0x1234 &&
            diagnostic.WindowSequence == 0x11223344U &&
            diagnostic.WindowEndMilliseconds == 80200U &&
            diagnostic.InferenceMicroseconds == 18350U &&
            diagnostic.FailureCount == 7U,
            "分类诊断固定字段偏移或小端解码错误。" );
        // 核对三组 Q15 到 0～1 的换算容差。
        Assert(
            Math.Abs(diagnostic.FusedConfidence - (32768.0 / 65535.0)) < 1e-12 &&
            Math.Abs(diagnostic.BaseConfidence - 1.0) < 1e-12 &&
            Math.Abs(diagnostic.MaskedConfidence) < 1e-12,
            "分类诊断 Q15 概率换算错误。" );
    }

    // 验证非固定长度和未知版本不能进入界面。
    private static void TestLengthAndVersionRejection()
    {
        // 构造少一字节 payload。
        byte[] shortPayload = new byte[InferenceDiagnosticV1Codec.PayloadSize - 1];
        // 长度错误必须失败并返回原因。
        Assert(
            !InferenceDiagnosticV1Codec.TryDecode(shortPayload, out _, out string? shortError) &&
            !string.IsNullOrWhiteSpace(shortError),
            "分类诊断错误长度未被拒绝。" );
        // 构造正确长度但未知版本二。
        byte[] unknownVersion = new byte[InferenceDiagnosticV1Codec.PayloadSize];
        // 写入未知版本。
        unknownVersion[0] = 2;
        // 三个动作写 Unknown，确保失败只来自版本。
        unknownVersion[1] = byte.MaxValue;
        // 基础动作写 Unknown。
        unknownVersion[2] = byte.MaxValue;
        // 掩码动作写 Unknown。
        unknownVersion[3] = byte.MaxValue;
        // 未知版本必须失败。
        Assert(
            !InferenceDiagnosticV1Codec.TryDecode(unknownVersion, out _, out string? versionError) &&
            versionError?.Contains("版本", StringComparison.Ordinal) == true,
            "分类诊断未知版本未被拒绝。" );
    }

    // 验证动作保留区不能被静默转换为现有类别。
    private static void TestActionBoundaryRejection()
    {
        // 构造正确版本的基础 payload。
        byte[] payload = new byte[InferenceDiagnosticV1Codec.PayloadSize];
        // 写入 v1。
        payload[0] = 1;
        // 融合动作写非法保留值十一。
        payload[1] = 11;
        // 基础动作写合法零。
        payload[2] = 0;
        // 掩码动作写合法 Unknown。
        payload[3] = byte.MaxValue;
        // 非法融合动作必须失败。
        Assert(
            !InferenceDiagnosticV1Codec.TryDecode(payload, out _, out string? actionError) &&
            actionError?.Contains("融合动作", StringComparison.Ordinal) == true,
            "分类诊断非法动作索引未被拒绝。" );
    }

    // 统一断言失败格式，便于 PowerShell 和 CI 输出具体合同原因。
    private static void Assert(bool condition, string message)
    {
        // 条件为假时抛出稳定异常并终止测试进程。
        if (!condition)
        {
            // 使用 InvalidOperationException 表示合同不满足。
            throw new InvalidOperationException(message);
        }
    }
}
