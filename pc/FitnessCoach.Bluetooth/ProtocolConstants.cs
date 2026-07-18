// 协议常量集中在单一类型中，避免 UUID、长度和版本在不同模块漂移。
namespace FitnessCoach.Bluetooth;

/// <summary>
/// 定义 BLE 应用协议 v1 的固定常量；对应 C 头文件为 shared/protocol/imu_ble_protocol.h。
/// </summary>
public static class ProtocolConstants
{
    /// <summary>逻辑帧魔数 0xB17E，线上小端字节为 7E B1。</summary>
    public const ushort Magic = 0xB17E;

    /// <summary>协议主版本；不同主版本禁止执行训练控制。</summary>
    public const byte ProtocolMajor = 1;

    /// <summary>协议次版本；高次版本中的未知 TLV 可被忽略。</summary>
    public const byte ProtocolMinor = 0;

    /// <summary>固定逻辑帧头长度，单位为字节，不包含末尾 CRC16。</summary>
    public const int LogicalHeaderSize = 14;

    /// <summary>CRC-16/CCITT-FALSE 在线上占用两个小端字节。</summary>
    public const int CrcSize = 2;

    /// <summary>每个 GATT Value 的分片包络长度，单位为字节。</summary>
    public const int FragmentHeaderSize = 8;

    /// <summary>单个逻辑 payload 最大长度，限制异常远端造成的内存占用。</summary>
    public const int MaxPayloadSize = 1024;

    /// <summary>完整逻辑帧最大长度，等于 14 字节头、1024 字节 payload 和 2 字节 CRC。</summary>
    public const int MaxFrameSize = LogicalHeaderSize + MaxPayloadSize + CrcSize;

    /// <summary>实时状态 payload v1 固定为 30 字节。</summary>
    public const int LiveStatePayloadSize = 30;

    /// <summary>低延迟事件 payload v1 固定为 36 字节。</summary>
    public const int EventPayloadSize = 36;

    /// <summary>自定义 IMU 健身服务 UUID。</summary>
    public static readonly Guid FitnessServiceUuid = new("7B2E0000-6D57-4A51-9E43-494D5542504E");

    /// <summary>控制请求与 ACK/NACK 指示特征 UUID。</summary>
    public static readonly Guid ControlPointUuid = new("7B2E0001-6D57-4A51-9E43-494D5542504E");

    /// <summary>固件、模型、类别表和能力清单只读特征 UUID。</summary>
    public static readonly Guid ManifestUuid = new("7B2E0002-6D57-4A51-9E43-494D5542504E");

    /// <summary>实时状态读取与通知特征 UUID。</summary>
    public static readonly Guid LiveStateUuid = new("7B2E0003-6D57-4A51-9E43-494D5542504E");

    /// <summary>计数、动作变化、目标和故障事件通知特征 UUID。</summary>
    public static readonly Guid EventUuid = new("7B2E0004-6D57-4A51-9E43-494D5542504E");

    /// <summary>会话列表和断点续传控制特征 UUID。</summary>
    public static readonly Guid TransferControlUuid = new("7B2E0005-6D57-4A51-9E43-494D5542504E");

    /// <summary>会话摘要与原始日志数据通知特征 UUID。</summary>
    public static readonly Guid TransferDataUuid = new("7B2E0006-6D57-4A51-9E43-494D5542504E");

    /// <summary>开发者模式实时六轴原始流通知特征 UUID。</summary>
    public static readonly Guid RawStreamUuid = new("7B2E0007-6D57-4A51-9E43-494D5542504E");
}
