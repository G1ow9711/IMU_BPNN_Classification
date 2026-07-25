// 引入固定端序读写工具，禁止依赖 Windows CPU 的本机端序。
using System.Buffers.Binary;

// 编解码器位于蓝牙命名空间，供 GATT 收发服务和测试共同调用。
namespace FitnessCoach.Bluetooth;

/// <summary>
/// 编码和解码完整 BLE 逻辑帧；CRC 范围排除魔数和 CRC 自身。
/// </summary>
public static class BleFrameCodec
{
    /// <summary>
    /// 把逻辑字段编码为连续字节，返回值可继续交给分片器。
    /// </summary>
    public static byte[] Encode(BleLogicalFrame frame)
    {
        // 空帧对象无法读取字段，使用标准异常提醒调用者修复逻辑。
        ArgumentNullException.ThrowIfNull(frame);

        // payload 长度已经由 BleLogicalFrame 构造函数限制在 1024 字节以内。
        int payloadLength = frame.Payload.Length;
        // 计算完整帧长度：14 字节固定头、payload 和 2 字节 CRC。
        int frameLength = ProtocolConstants.LogicalHeaderSize + payloadLength + ProtocolConstants.CrcSize;
        // 按精确长度分配输出，不保留未初始化尾部空间。
        byte[] output = new byte[frameLength];

        // 偏移 0 写入小端魔数 0xB17E。
        BinaryPrimitives.WriteUInt16LittleEndian(output.AsSpan(0, 2), ProtocolConstants.Magic);
        // 偏移 2 写入协议主版本。
        output[2] = frame.ProtocolMajor;
        // 偏移 3 写入协议次版本。
        output[3] = frame.ProtocolMinor;
        // 偏移 4 写入消息类型。
        output[4] = frame.MessageType;
        // 偏移 5 写入消息标志。
        output[5] = frame.Flags;
        // 偏移 6 写入小端逻辑帧序号。
        BinaryPrimitives.WriteUInt16LittleEndian(output.AsSpan(6, 2), frame.Sequence);
        // 偏移 8 写入小端单调毫秒时间。
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(8, 4), frame.MonotonicMilliseconds);
        // 偏移 12 写入小端 payload 长度。
        BinaryPrimitives.WriteUInt16LittleEndian(output.AsSpan(12, 2), checked((ushort)payloadLength));
        // 把 payload 副本写入固定头之后。
        frame.Payload.Span.CopyTo(output.AsSpan(ProtocolConstants.LogicalHeaderSize, payloadLength));

        // CRC 输入从协议主版本开始，因此排除前两个魔数字节。
        ReadOnlySpan<byte> crcInput = output.AsSpan(2, ProtocolConstants.LogicalHeaderSize - 2 + payloadLength);
        // 使用 CCITT-FALSE 参数计算应用层校验值。
        ushort crc = Crc16CcittFalse.Compute(crcInput);
        // CRC 紧跟 payload，按小端写入两个字节。
        BinaryPrimitives.WriteUInt16LittleEndian(output.AsSpan(ProtocolConstants.LogicalHeaderSize + payloadLength, 2), crc);

        // 返回完整、可独立校验的逻辑帧。
        return output;
    }

    /// <summary>
    /// 尝试解析并校验完整逻辑帧；失败时 frame 为 null 且 error 给出原因。
    /// </summary>
    public static bool TryDecode(
        ReadOnlySpan<byte> input,
        out BleLogicalFrame? frame,
        out ProtocolDecodeError error)
    {
        // 默认输出为空，只有所有检查通过后才创建业务对象。
        frame = null;
        // 默认错误为 None，具体失败分支会覆盖。
        error = ProtocolDecodeError.None;

        // 最短帧必须包含 14 字节头和 2 字节 CRC。
        if (input.Length < ProtocolConstants.LogicalHeaderSize + ProtocolConstants.CrcSize)
        {
            // 标记输入被截断。
            error = ProtocolDecodeError.TooShort;
            // 返回失败，禁止读取尚不存在的固定字段。
            return false;
        }

        // 从偏移 0 按小端读取魔数。
        ushort magic = BinaryPrimitives.ReadUInt16LittleEndian(input.Slice(0, 2));
        // 非本协议魔数必须立即拒绝。
        if (magic != ProtocolConstants.Magic)
        {
            // 标记魔数错误，便于发现订阅错特征或协议版本混用。
            error = ProtocolDecodeError.BadMagic;
            // 返回失败，不解释后续长度字段。
            return false;
        }

        // 从固定偏移 12 读取 payload 长度。
        ushort payloadLength = BinaryPrimitives.ReadUInt16LittleEndian(input.Slice(12, 2));
        // payload 超过 1024 字节会突破设备固定缓冲区。
        if (payloadLength > ProtocolConstants.MaxPayloadSize)
        {
            // 标记长度超限。
            error = ProtocolDecodeError.BadLength;
            // 返回失败，不分配远端声明的大块内存。
            return false;
        }

        // 根据长度字段计算唯一合法完整帧长度。
        int expectedLength = ProtocolConstants.LogicalHeaderSize + payloadLength + ProtocolConstants.CrcSize;
        // 输入必须恰好是一帧，不接受尾随垃圾或拼接帧。
        if (input.Length != expectedLength)
        {
            // 标记长度字段与实际字节数不一致。
            error = ProtocolDecodeError.BadLength;
            // 返回失败，避免 CRC 范围歧义。
            return false;
        }

        // 定位 payload 末尾的线上小端 CRC。
        ushort receivedCrc = BinaryPrimitives.ReadUInt16LittleEndian(input.Slice(ProtocolConstants.LogicalHeaderSize + payloadLength, 2));
        // CRC 输入排除魔数和 CRC 自身。
        ReadOnlySpan<byte> crcInput = input.Slice(2, ProtocolConstants.LogicalHeaderSize - 2 + payloadLength);
        // 重新计算应用层 CRC。
        ushort calculatedCrc = Crc16CcittFalse.Compute(crcInput);
        // 校验值不同表示重组缺片、数据损坏或跨语言实现不一致。
        if (receivedCrc != calculatedCrc)
        {
            // 标记 CRC 错误，调用者应丢弃整帧。
            error = ProtocolDecodeError.BadCrc;
            // 返回失败，不创建任何实时状态。
            return false;
        }

        // 从固定偏移读取协议主版本。
        byte protocolMajor = input[2];
        // 从固定偏移读取协议次版本。
        byte protocolMinor = input[3];
        // 从固定偏移读取消息类型。
        byte messageType = input[4];
        // 从固定偏移读取标志位。
        byte flags = input[5];
        // 从固定偏移读取小端逻辑序号。
        ushort sequence = BinaryPrimitives.ReadUInt16LittleEndian(input.Slice(6, 2));
        // 从固定偏移读取小端单调毫秒时间。
        uint monotonicMilliseconds = BinaryPrimitives.ReadUInt32LittleEndian(input.Slice(8, 4));
        // 创建逻辑帧并复制 payload，确保输入缓冲区释放后对象仍有效。
        frame = new BleLogicalFrame(
            protocolMajor,
            protocolMinor,
            messageType,
            flags,
            sequence,
            monotonicMilliseconds,
            input.Slice(ProtocolConstants.LogicalHeaderSize, payloadLength));
        // 显式保持无错误结果。
        error = ProtocolDecodeError.None;
        // 返回成功，调用者可以继续按 messageType 解码 payload。
        return true;
    }
}
