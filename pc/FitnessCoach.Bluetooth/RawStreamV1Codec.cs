// 引入显式小端读取工具，RawStream 不能依赖 CPU 端序。
using System.Buffers.Binary;

// 六轴同步诊断协议位于纯蓝牙层，诊断页只消费解码后的不可变对象。
namespace FitnessCoach.Bluetooth;

/// <summary>保存 RawStream v1 固定 22 字节记录；六轴值是滤波、25 Hz 重采样物理量重新量化后的 int16 诊断码，不是芯片 FIFO 原始码。</summary>
public sealed record RawImuSampleV1(
    // 自设备启动后递增的采样序号，溢出后按 uint32 自然回绕。
    uint SampleIndex,
    // 设备单调毫秒时间，不受 UTC 时间同步影响。
    uint MonotonicMilliseconds,
    // 陀螺仪 X 轴固定点诊断码；字段名 Raw 仅保持 v1 API 兼容，code/16.4 得到滤波后的 deg/s。
    short GxRaw,
    // 陀螺仪 Y 轴固定点诊断码；设备已完成抗混叠滤波和严格 25 Hz 重采样。
    short GyRaw,
    // 陀螺仪 Z 轴固定点诊断码；数值在设备端四舍五入并饱和到 int16 范围。
    short GzRaw,
    // 加速度计 X 轴固定点诊断码；code/4096 得到滤波后的 g。
    short AxRaw,
    // 加速度计 Y 轴固定点诊断码；该值不能解释为未经处理的 QMI8658 FIFO 样本。
    short AyRaw,
    // 加速度计 Z 轴固定点诊断码；PC 只解码固定小端字段，不执行第二次滤波。
    short AzRaw,
    // 质量位用于标记丢样、重采样或振动污染等设备端事实。
    ushort QualityFlags);

/// <summary>RawStream 样本事件；发送方回调线程不保证是 WPF UI 线程。</summary>
public sealed class RawImuSampleReceivedEventArgs : EventArgs
{
    /// <summary>保存不可变六轴同步诊断样本。</summary>
    public RawImuSampleReceivedEventArgs(RawImuSampleV1 sample)
    {
        // 样本不能为空。
        ArgumentNullException.ThrowIfNull(sample);
        // 保存事件样本。
        Sample = sample;
    }

    /// <summary>本次收到的固定 22 字节 v1 样本。</summary>
    public RawImuSampleV1 Sample { get; }
}

/// <summary>定义显式开发者 RawStream 能力；关闭时实现不得落盘或继续发布样本。</summary>
public interface IRawStreamSource
{
    /// <summary>收到完整且合法的 22 字节样本时触发。</summary>
    event EventHandler<RawImuSampleReceivedEventArgs>? RawSampleReceived;

    /// <summary>收到完整且合法的 28 字节双 M0 分类诊断时触发；回调线程不保证为 WPF UI 线程。</summary>
    event EventHandler<InferenceDiagnosticReceivedEventArgs>? InferenceDiagnosticReceived;

    /// <summary>表示当前会话已成功要求设备发布 RawStream。</summary>
    bool IsRawStreamEnabled { get; }

    /// <summary>显式开关 RawStream；普通产品运行默认关闭。</summary>
    Task SetRawStreamEnabledAsync(bool enabled, CancellationToken cancellationToken = default);
}

/// <summary>解码 UUID 0007 的 RawStream v1 固定记录。</summary>
public static class RawStreamV1Codec
{
    /// <summary>v1 payload 固定为 4+4+6×2+2=22 字节。</summary>
    public const int PayloadSize = 22;

    /// <summary>严格解码固定 22 字节；任何多余或缺失字节均拒绝。</summary>
    public static bool TryDecode(ReadOnlySpan<byte> payload, out RawImuSampleV1? sample, out string? error)
    {
        // 失败前不返回部分样本。
        sample = null;
        // 默认没有错误；失败分支写具体原因。
        error = null;

        // v1 不允许可选尾部，防止未来版本被旧客户端静默错读。
        if (payload.Length != PayloadSize)
        {
            // 记录实际长度便于诊断分片丢失或版本不兼容。
            error = $"RawStream v1 必须为 {PayloadSize} 字节，实际为 {payload.Length} 字节。";
            // 返回失败，禁止越界读取。
            return false;
        }

        // 按固定偏移读取 sample_index u32LE。
        uint sampleIndex = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(0, 4));
        // 按固定偏移读取 monotonic_ms u32LE。
        uint monotonicMilliseconds = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(4, 4));
        // 依次读取 gx、gy、gz、ax、ay、az 六个 int16LE 固定点诊断码；编码前设备已完成滤波和 25 Hz 重采样。
        short gxRaw = BinaryPrimitives.ReadInt16LittleEndian(payload.Slice(8, 2));
        // 读取陀螺仪 Y 轴诊断码；Raw 后缀仅维持冻结的 v1 公共 API。
        short gyRaw = BinaryPrimitives.ReadInt16LittleEndian(payload.Slice(10, 2));
        // 读取陀螺仪 Z 轴诊断码；PC 不还原芯片 FIFO 时序。
        short gzRaw = BinaryPrimitives.ReadInt16LittleEndian(payload.Slice(12, 2));
        // 读取加速度计 X 轴诊断码；按 code/4096 可换算滤波后的 g。
        short axRaw = BinaryPrimitives.ReadInt16LittleEndian(payload.Slice(14, 2));
        // 读取加速度计 Y 轴诊断码；数值已在设备端四舍五入并做 int16 饱和保护。
        short ayRaw = BinaryPrimitives.ReadInt16LittleEndian(payload.Slice(16, 2));
        // 读取加速度计 Z 轴诊断码；该解码步骤只处理小端字节序。
        short azRaw = BinaryPrimitives.ReadInt16LittleEndian(payload.Slice(18, 2));
        // 读取质量标志 u16LE。
        ushort qualityFlags = BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(20, 2));
        // 创建完整不可变样本，通道顺序固定为 gx、gy、gz、ax、ay、az。
        sample = new RawImuSampleV1(sampleIndex, monotonicMilliseconds, gxRaw, gyRaw, gzRaw, axRaw, ayRaw, azRaw, qualityFlags);
        // 返回成功。
        return true;
    }
}
