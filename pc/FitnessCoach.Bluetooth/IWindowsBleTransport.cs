// 传输抽象只描述 Windows BLE 会话需要的 GATT 能力，不把 WinRT 类型泄漏到协议和状态机测试。
namespace FitnessCoach.Bluetooth;

/// <summary>
/// 表示扫描、选择并连接成功后的 BLE 设备信息。
/// </summary>
public sealed record BleConnectedDevice(
    // Windows 设备 ID 在本次系统配对记录中唯一，用于后续优先重连。
    string DeviceId,
    // 用户可见名称通常为 BPNN-FIT-XXXX，空名称由实现回退为设备 ID。
    string DisplayName,
    // ATT MTU 包含 3 字节 ATT 操作头；应用分片容量由 mtu-3-8 计算。
    ushort AttMtu,
    // 最近扫描 RSSI，单位 dBm；Windows 未提供属性时为 null。
    int? RssiDbm = null,
    // 标准 Device Information 型号；缺少服务时为“未知”。
    string ModelNumber = "未知",
    // 标准 Device Information 硬件修订号；缺少服务时为“未知”。
    string HardwareRevision = "未知",
    // 标准 Device Information 固件修订号；缺少服务时为“未知”。
    string FirmwareRevision = "未知");

/// <summary>
/// 描述一个 GATT notification 或 indication；Value 是已经脱离 WinRT 缓冲区生命周期的副本。
/// </summary>
public sealed class BleGattValueReceivedEventArgs : EventArgs
{
    /// <summary>创建一次已复制的 GATT 值事件。</summary>
    public BleGattValueReceivedEventArgs(Guid characteristicUuid, ReadOnlyMemory<byte> value)
    {
        // 保存来源特征 UUID，状态机据此选择独立重组器和消息类型。
        CharacteristicUuid = characteristicUuid;
        // 复制数据，保证 WinRT 回调返回后后台解码仍可安全读取。
        Value = value.ToArray();
    }

    /// <summary>产生通知或指示的特征 UUID。</summary>
    public Guid CharacteristicUuid { get; }

    /// <summary>完整 GATT Value 副本，可能是单个逻辑帧或带 8 字节包络的分片。</summary>
    public ReadOnlyMemory<byte> Value { get; }
}

/// <summary>
/// 描述 Windows BLE 链路意外断开；主动断开由会话层抑制自动重连。
/// </summary>
public sealed class BleTransportDisconnectedEventArgs : EventArgs
{
    /// <summary>创建断线原因对象。</summary>
    public BleTransportDisconnectedEventArgs(string reason)
    {
        // 空原因无法用于诊断，统一替换为稳定中文文本。
        Reason = string.IsNullOrWhiteSpace(reason) ? "Windows BLE 链路已断开。" : reason;
    }

    /// <summary>用户可见且可记录日志的断线原因。</summary>
    public string Reason { get; }
}

/// <summary>
/// 定义 Windows BLE 扫描、配对、GATT 发现和字节收发边界；测试使用 fake，正式程序使用 WinRT。
/// </summary>
public interface IWindowsBleTransport : IAsyncDisposable
{
    /// <summary>收到 indication 或 notification 时触发；回调线程不保证为 UI 线程。</summary>
    event EventHandler<BleGattValueReceivedEventArgs>? ValueReceived;

    /// <summary>远端、系统蓝牙或无线环境造成链路丢失时触发。</summary>
    event EventHandler<BleTransportDisconnectedEventArgs>? Disconnected;

    /// <summary>当前 GATT 链路是否可读写。</summary>
    bool IsConnected { get; }

    /// <summary>
    /// 扫描并连接设备；preferredDeviceId 非空时优先该系统设备 ID，否则交给选择器决定。
    /// 实现必须在返回前完成配对边界、服务发现和 0001～0007 特征发现。
    /// </summary>
    Task<BleConnectedDevice> ScanAndConnectAsync(string? preferredDeviceId, CancellationToken cancellationToken);

    /// <summary>读取指定特征；返回数据必须是独立副本。</summary>
    Task<byte[]> ReadAsync(Guid characteristicUuid, CancellationToken cancellationToken);

    /// <summary>按实际特征能力订阅 notification 或 indication。</summary>
    Task SubscribeAsync(Guid characteristicUuid, bool useIndication, CancellationToken cancellationToken);

    /// <summary>写入一个 GATT Value；控制点必须使用有响应写入。</summary>
    Task WriteAsync(Guid characteristicUuid, ReadOnlyMemory<byte> value, bool withResponse, CancellationToken cancellationToken);

    /// <summary>主动释放当前设备、服务、特征和 GATT 会话对象。</summary>
    Task DisconnectAsync(CancellationToken cancellationToken);
}

/// <summary>
/// 定义 Windows transport 的可选主动广播发现能力；协议 fake 只有在验证设备列表时才需要实现。
/// </summary>
public interface IWindowsBleDiscoveryTransport
{
    /// <summary>
    /// 主动扫描指定时间并返回已脱离 WinRT 生命周期的设备快照；调用方可安全绑定到 WPF 集合。
    /// </summary>
    Task<IReadOnlyList<BleDiscoveredDevice>> ScanDevicesAsync(
        TimeSpan scanDuration,
        CancellationToken cancellationToken);
}

/// <summary>
/// 定义 Windows 系统 BLE 配对记录管理边界；会话层只传 Windows 设备 ID，不接触 WinRT 类型。
/// </summary>
public interface IWindowsBlePairingManager
{
    /// <summary>
    /// 取消指定 Windows BLE 设备的系统配对；已取消配对视为幂等成功，其它系统状态必须抛出异常。
    /// </summary>
    Task ForgetDeviceAsync(string deviceId, CancellationToken cancellationToken);
}
