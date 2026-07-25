// MTU 容量、分片数公式和边界处理见 docs/BLE通信、设备配置与会话存储.md 第 6 节。
// 引入固定端序工具，分片包络不依赖 Windows 本机端序。
using System.Buffers.Binary;

// 分片器与逻辑帧编解码器位于同一蓝牙协议命名空间。
namespace FitnessCoach.Bluetooth;

/// <summary>
/// 按实际 ATT MTU 把完整逻辑帧拆成带 8 字节包络的 GATT Value。
/// </summary>
public static class BleFragmentCodec
{
    /// <summary>
    /// 把完整帧拆成顺序分片；ATT Value 容量固定为 mtu-3 字节。
    /// </summary>
    public static IReadOnlyList<byte[]> Fragment(
        ReadOnlySpan<byte> frame,
        ushort attMtu,
        ushort logicalSequence)
    {
        // 完整逻辑帧至少包含 14 字节头和 2 字节 CRC。
        if (frame.Length < ProtocolConstants.LogicalHeaderSize + ProtocolConstants.CrcSize)
        {
            // 太短的数据不是可发送逻辑帧。
            throw new ArgumentOutOfRangeException(nameof(frame), "完整逻辑帧长度不足 16 字节。");
        }

        // 帧长度不得超过协议固定 1040 字节缓冲区。
        if (frame.Length > ProtocolConstants.MaxFrameSize)
        {
            // 上层应把大对象拆为多个会话传输块。
            throw new ArgumentOutOfRangeException(nameof(frame), $"完整逻辑帧不能超过 {ProtocolConstants.MaxFrameSize} 字节。");
        }

        // ATT 操作头占 3 字节，分片包络占 8 字节，至少还需一个数据字节。
        if (attMtu <= 3 + ProtocolConstants.FragmentHeaderSize)
        {
            // 无有效数据容量的 MTU 无法使用本协议分片。
            throw new ArgumentOutOfRangeException(nameof(attMtu), "ATT MTU 太小，无法容纳分片包络和数据。");
        }

        // 计算单个 GATT Value 最多承载的逻辑帧字节数。
        int fragmentDataCapacity = attMtu - 3 - ProtocolConstants.FragmentHeaderSize;
        // 使用向上取整公式计算总分片数。
        int fragmentCount = (frame.Length + fragmentDataCapacity - 1) / fragmentDataCapacity;
        // 分片总数字段只有 16 位，超限时必须拒绝。
        if (fragmentCount > ushort.MaxValue)
        {
            // 当前最大帧不会触发该分支，但保留完整边界检查。
            throw new ArgumentOutOfRangeException(nameof(frame), "分片总数超过 16 位字段范围。");
        }

        // 按精确总数创建结果列表，避免扩容改变延迟。
        List<byte[]> fragments = new(fragmentCount);

        // 顺序遍历每个分片索引，保证 BLE 通知保持编码顺序。
        for (int fragmentIndex = 0; fragmentIndex < fragmentCount; fragmentIndex++)
        {
            // 计算当前片在完整帧中的起始偏移。
            int sourceOffset = fragmentIndex * fragmentDataCapacity;
            // 计算尚未编码的剩余字节数。
            int remaining = frame.Length - sourceOffset;
            // 最后一片取剩余长度，其余片填满容量。
            int fragmentDataLength = Math.Min(fragmentDataCapacity, remaining);
            // 每个数组只分配包络和实际数据所需字节。
            byte[] fragment = new byte[ProtocolConstants.FragmentHeaderSize + fragmentDataLength];

            // 包络偏移 0 写入逻辑帧 sequence。
            BinaryPrimitives.WriteUInt16LittleEndian(fragment.AsSpan(0, 2), logicalSequence);
            // 包络偏移 2 写入当前分片索引。
            BinaryPrimitives.WriteUInt16LittleEndian(fragment.AsSpan(2, 2), checked((ushort)fragmentIndex));
            // 包络偏移 4 写入总分片数。
            BinaryPrimitives.WriteUInt16LittleEndian(fragment.AsSpan(4, 2), checked((ushort)fragmentCount));
            // 包络偏移 6 写入当前片数据长度。
            BinaryPrimitives.WriteUInt16LittleEndian(fragment.AsSpan(6, 2), checked((ushort)fragmentDataLength));
            // 把完整帧对应区间复制到包络之后。
            frame.Slice(sourceOffset, fragmentDataLength).CopyTo(fragment.AsSpan(ProtocolConstants.FragmentHeaderSize));
            // 把完成的 GATT Value 加入顺序结果。
            fragments.Add(fragment);
        }

        // 返回只读接口，调用者按列表顺序写入同一 GATT 特征。
        return fragments;
    }
}
