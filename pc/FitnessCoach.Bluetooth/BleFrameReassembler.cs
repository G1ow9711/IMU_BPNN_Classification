// 引入固定端序工具，按协议小端格式读取分片包络。
using System.Buffers.Binary;

// 重组器位于蓝牙命名空间，每个订阅特征应拥有独立实例。
namespace FitnessCoach.Bluetooth;

/// <summary>
/// 顺序重组同一 GATT 特征上的分片；缺片、乱序或跨帧混入都会清除状态。
/// </summary>
public sealed class BleFrameReassembler
{
    // 固定 1040 字节缓冲区与 ESP32 C 实现相同，拒绝远端诱导的大内存分配。
    private readonly byte[] _buffer = new byte[ProtocolConstants.MaxFrameSize];
    // 保存当前已写入固定缓冲区的有效字节数。
    private int _length;
    // 保存当前逻辑帧 sequence。
    private ushort _logicalSequence;
    // 保存发送端在第一片声明的总分片数。
    private ushort _fragmentCount;
    // 保存下一次必须收到的分片索引。
    private ushort _nextFragmentIndex;
    // 标记是否已经收到当前逻辑帧的索引 0 分片。
    private bool _active;

    /// <summary>
    /// 清空重组状态；BLE 断连、超时和用户切换设备时必须调用。
    /// </summary>
    public void Reset()
    {
        // 清零当前有效长度，旧缓冲区不再代表完整帧。
        _length = 0;
        // 清零逻辑 sequence，等待下一片索引 0 覆盖。
        _logicalSequence = 0;
        // 清零总分片数。
        _fragmentCount = 0;
        // 清零下一期望索引。
        _nextFragmentIndex = 0;
        // 退出激活状态，禁止接收非零起始索引。
        _active = false;
        // 清空旧帧字节，避免诊断转储暴露上一会话数据。
        Array.Clear(_buffer);
    }

    /// <summary>
    /// 推入一个 GATT 分片；完成时 completeFrame 为经过 CRC 校验的独立数组。
    /// </summary>
    public FragmentPushStatus Push(
        ReadOnlySpan<byte> fragment,
        out byte[]? completeFrame,
        out ProtocolDecodeError error)
    {
        // 默认没有完整帧，只有最后一片通过后才写入数组。
        completeFrame = null;
        // 默认没有错误，拒绝分支会覆盖具体原因。
        error = ProtocolDecodeError.None;

        // 分片必须至少包含 8 字节包络。
        if (fragment.Length < ProtocolConstants.FragmentHeaderSize)
        {
            // 截断包络不能读取所有长度和序号字段。
            error = ProtocolDecodeError.TooShort;
            // 清空当前重组，避免下一片被拼到错误前缀。
            Reset();
            // 返回拒绝状态。
            return FragmentPushStatus.Rejected;
        }

        // 从包络偏移 0 读取逻辑帧 sequence。
        ushort logicalSequence = BinaryPrimitives.ReadUInt16LittleEndian(fragment.Slice(0, 2));
        // 从包络偏移 2 读取当前分片索引。
        ushort fragmentIndex = BinaryPrimitives.ReadUInt16LittleEndian(fragment.Slice(2, 2));
        // 从包络偏移 4 读取总分片数。
        ushort fragmentCount = BinaryPrimitives.ReadUInt16LittleEndian(fragment.Slice(4, 2));
        // 从包络偏移 6 读取当前数据区长度。
        ushort fragmentDataLength = BinaryPrimitives.ReadUInt16LittleEndian(fragment.Slice(6, 2));

        // 总数必须非零，且当前索引必须小于总数。
        if ((fragmentCount == 0) || (fragmentIndex >= fragmentCount))
        {
            // 标记分片结构错误。
            error = ProtocolDecodeError.BadFragment;
            // 清空状态，等待下一帧索引 0。
            Reset();
            // 返回拒绝状态。
            return FragmentPushStatus.Rejected;
        }

        // 包络长度必须与实际剩余字节数完全一致。
        if (fragmentDataLength != fragment.Length - ProtocolConstants.FragmentHeaderSize)
        {
            // 标记尾随垃圾或截断数据错误。
            error = ProtocolDecodeError.BadFragment;
            // 清空状态，防止错误长度污染后续帧。
            Reset();
            // 返回拒绝状态。
            return FragmentPushStatus.Rejected;
        }

        // 索引 0 明确开始新帧，并覆盖可能超时残留的旧状态。
        if (fragmentIndex == 0)
        {
            // 新帧从固定缓冲区偏移 0 开始。
            _length = 0;
            // 保存本帧 sequence。
            _logicalSequence = logicalSequence;
            // 保存本帧总片数。
            _fragmentCount = fragmentCount;
            // 写入前期望索引为 0。
            _nextFragmentIndex = 0;
            // 标记重组激活。
            _active = true;
        }

        // 没有索引 0 时拒绝非零分片，表示通知已经丢包。
        if (!_active)
        {
            // 标记分片顺序错误。
            error = ProtocolDecodeError.BadFragment;
            // 返回拒绝；状态本身已经处于空闲。
            return FragmentPushStatus.Rejected;
        }

        // sequence、总数和期望索引必须全部匹配当前帧。
        if ((logicalSequence != _logicalSequence) ||
            (fragmentCount != _fragmentCount) ||
            (fragmentIndex != _nextFragmentIndex))
        {
            // 标记缺片、乱序或跨帧混入。
            error = ProtocolDecodeError.BadFragment;
            // 清空状态，下一片必须重新从索引 0 开始。
            Reset();
            // 返回拒绝状态。
            return FragmentPushStatus.Rejected;
        }

        // 追加当前数据后不得超过固定 1040 字节缓冲区。
        if (_length + fragmentDataLength > _buffer.Length)
        {
            // 标记逻辑帧长度超限。
            error = ProtocolDecodeError.BadLength;
            // 清空状态，阻止越界复制。
            Reset();
            // 返回拒绝状态。
            return FragmentPushStatus.Rejected;
        }

        // 把当前数据区追加到固定重组缓冲区。
        fragment.Slice(ProtocolConstants.FragmentHeaderSize, fragmentDataLength).CopyTo(_buffer.AsSpan(_length));
        // 累加已经接收的完整帧字节数。
        _length += fragmentDataLength;
        // 下一次只接受当前索引加一。
        _nextFragmentIndex++;

        // 尚未到最后一片时保持激活。
        if (_nextFragmentIndex < _fragmentCount)
        {
            // 返回正常未完成状态，不分配临时完整帧。
            return FragmentPushStatus.AcceptedIncomplete;
        }

        // 复制完整帧供业务层持有，避免下一次 Reset 覆盖数据。
        byte[] candidate = _buffer.AsSpan(0, _length).ToArray();
        // 先用统一解码器核对魔数、长度和 CRC。
        bool frameValid = BleFrameCodec.TryDecode(candidate, out _, out ProtocolDecodeError frameError);
        // 校验失败时整帧必须丢弃。
        if (!frameValid)
        {
            // 把逻辑帧校验错误返回调用者。
            error = frameError;
            // 清空当前重组状态。
            Reset();
            // 返回拒绝状态。
            return FragmentPushStatus.Rejected;
        }

        // 返回经过校验的独立完整帧数组。
        completeFrame = candidate;
        // 当前帧完成后退出激活状态，但不需要再次清空返回数组。
        _active = false;
        // 清零当前长度，下一帧索引 0 会重新写入缓冲区。
        _length = 0;
        // 返回完成状态。
        return FragmentPushStatus.Completed;
    }
}
