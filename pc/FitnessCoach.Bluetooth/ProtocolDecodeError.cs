// 解码错误类型独立定义，便于连接层映射为诊断日志或控制 NACK。
namespace FitnessCoach.Bluetooth;

/// <summary>
/// 表示逻辑帧或分片被拒绝的原因；None 只用于成功结果。
/// </summary>
public enum ProtocolDecodeError
{
    /// <summary>没有错误。</summary>
    None = 0,
    /// <summary>输入短于固定协议结构。</summary>
    TooShort = 1,
    /// <summary>逻辑帧魔数不是 0xB17E。</summary>
    BadMagic = 2,
    /// <summary>长度字段超限或与实际输入长度不一致。</summary>
    BadLength = 3,
    /// <summary>重新计算的 CRC16 与线上值不同。</summary>
    BadCrc = 4,
    /// <summary>分片总数、索引、sequence 或顺序不一致。</summary>
    BadFragment = 5,
}
