// 连接事件位于领域层，使 Mock、未来 WinRT BLE 和 WPF 使用同一合同。
namespace FitnessCoach.Domain;

/// <summary>
/// 描述设备连接状态变化；训练是否继续由设备状态单独决定。
/// </summary>
public sealed class DeviceConnectionChangedEventArgs : EventArgs
{
    /// <summary>
    /// 创建连接状态事件。
    /// </summary>
    public DeviceConnectionChangedEventArgs(bool isConnected, string reason)
    {
        // 保存连接布尔值，true 表示 GATT 或 Mock 链路可用。
        IsConnected = isConnected;
        // 保存用户可见原因，例如主动断开、链路丢失或重新连接。
        Reason = string.IsNullOrWhiteSpace(reason) ? "未提供原因" : reason;
    }

    /// <summary>当前链路是否已经连接。</summary>
    public bool IsConnected { get; }

    /// <summary>连接变化原因，供诊断页和日志显示。</summary>
    public string Reason { get; }
}
