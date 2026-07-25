// 引入小端字段构造，测试不依赖设备或 WinRT。
using System.Buffers.Binary;
// 引入待测会话传输 codec。
using FitnessCoach.Bluetooth;

// 复用现有无框架测试命名空间；本文件不修改 Program.cs。
namespace FitnessCoach.Tests;

/// <summary>提供可由现有 Program 显式接入的会话传输合同测试。</summary>
internal static class SessionTransferContractTests
{
    /// <summary>运行 Request12、Response16 和 Data80 最小黄金往返。</summary>
    public static Task RunAllAsync()
    {
        // 编码固定 LIST 请求。
        byte[] request = SessionTransferCodec.EncodeListRequest(9U, 3U, 12);
        // 核对固定长度和游标。
        Assert(request.Length == 12, "TransferRequest 长度错误。" );
        Assert(BinaryPrimitives.ReadUInt32LittleEndian(request.AsSpan(8, 4)) == 3U, "TransferRequest 游标错误。" );

        // 构造固定成功响应。
        byte[] response = new byte[16];
        // 写版本、LIST、OK、HAS_DATA|END。
        response[0] = 1;
        response[1] = 1;
        response[2] = 0;
        response[3] = 3;
        // 写请求号、下一游标、总数和本页数。
        BinaryPrimitives.WriteUInt32LittleEndian(response.AsSpan(4, 4), 9U);
        BinaryPrimitives.WriteUInt32LittleEndian(response.AsSpan(8, 4), 4U);
        BinaryPrimitives.WriteUInt16LittleEndian(response.AsSpan(12, 2), 4);
        BinaryPrimitives.WriteUInt16LittleEndian(response.AsSpan(14, 2), 1);
        // 解码并核对响应。
        Assert(SessionTransferCodec.TryDecodeResponse(response, out SessionTransferResponseV1? decodedResponse, out _), "TransferResponse 解码失败。" );
        Assert(decodedResponse!.NextCursorSessionSequence == 4U, "TransferResponse 下一游标错误。" );
        Assert(decodedResponse.IsEnd && decodedResponse.ItemCount == 1, "TransferResponse flags 或数量错误。" );

        // 构造固定 80 字节数据。
        byte[] data = new byte[80];
        // 写数据头和页尾标志。
        data[0] = 1;
        data[1] = 1;
        data[2] = 3;
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(4, 4), 9U);
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(8, 2), 0);
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(10, 2), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(12, 2), 4);
        // 写 64 字节摘要版本、长度和主键。
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(16, 2), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(18, 2), 64);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(20, 4), 4U);
        // 写动作 squat=6、指标重复=0、UTC、时长、热量和稳定度。
        data[28] = 6;
        data[29] = 0;
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(32, 8), 1_700_000_000_000UL);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(40, 8), 60_000UL);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(48, 8), 20UL);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(56, 8), 123_000UL);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(64, 8), 100_000UL);
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(72, 2), 28_000);
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(74, 2), 24_000);
        // 解码并核对主键。
        Assert(SessionTransferCodec.TryDecodeData(data, out SessionTransferDataV1? decodedData, out _), "TransferData 解码失败。" );
        Assert(decodedData!.Summary.SessionSequence == 4U, "SessionSummary 主键错误。" );
        Assert(decodedData.IsLastInPage && decodedData.IsEnd, "TransferData 页尾标志错误。" );
        // 返回已完成任务，保持与其它异步测试入口一致。
        return Task.CompletedTask;
    }

    // 条件失败时抛出可读异常，让现有 Program 返回非零。
    private static void Assert(bool condition, string message)
    {
        // 条件为真时继续。
        if (condition)
        {
            // 无后续动作。
            return;
        }

        // 抛出测试失败原因。
        throw new InvalidOperationException(message);
    }
}
