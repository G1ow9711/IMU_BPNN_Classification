// 消息类型枚举集中定义，避免控制、状态和传输模块使用冲突数值。
namespace FitnessCoach.Bluetooth;

/// <summary>
/// 定义逻辑帧 message_type 的 v1 数值。
/// </summary>
public enum ProtocolMessageType : byte
{
    /// <summary>客户端发送的控制请求。</summary>
    ControlRequest = 1,
    /// <summary>设备通过 indication 返回的控制 ACK 或 NACK。</summary>
    ControlResponse = 2,
    /// <summary>设备发布的 30 字节实时状态快照。</summary>
    LiveState = 3,
    /// <summary>计数、动作变化、目标或故障即时事件。</summary>
    Event = 4,
    /// <summary>会话列表、下载或删除请求。</summary>
    TransferRequest = 5,
    /// <summary>会话传输控制响应。</summary>
    TransferResponse = 6,
    /// <summary>会话摘要或原始文件数据块。</summary>
    TransferData = 7,
    /// <summary>开发者模式实时六轴原始样本批次。</summary>
    RawStream = 8,
}
