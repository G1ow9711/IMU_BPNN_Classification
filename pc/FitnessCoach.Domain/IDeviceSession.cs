// 设备会话接口位于领域层，WPF 不直接依赖 Mock 或 Windows BLE 实现。
namespace FitnessCoach.Domain;

/// <summary>
/// 定义健身设备控制和权威状态流；所有方法都必须具备幂等行为。
/// </summary>
public interface IDeviceSession : IAsyncDisposable
{
    /// <summary>设备发布新权威状态时触发；回调线程不保证是 WPF UI 线程。</summary>
    event EventHandler<LiveState>? StateChanged;

    /// <summary>蓝牙或 Mock 链路状态变化时触发。</summary>
    event EventHandler<DeviceConnectionChangedEventArgs>? ConnectionChanged;

    /// <summary>设备全局 ID，用于与会话序号组成唯一键。</summary>
    string DeviceId { get; }

    /// <summary>当前链路是否可用于发送控制和接收状态。</summary>
    bool IsConnected { get; }

    /// <summary>true 表示真实 BLE 实现；Mock 必须返回 false 并在界面明确标记。</summary>
    bool IsHardwareBacked { get; }

    /// <summary>建立连接；重复调用不得创建第二条连接。</summary>
    Task ConnectAsync(CancellationToken cancellationToken = default);

    /// <summary>断开 PC 链路；设备内部运行中的训练不得因此自动停止。</summary>
    Task DisconnectAsync(CancellationToken cancellationToken = default);

    /// <summary>读取当前权威状态快照，用于连接恢复。</summary>
    Task<LiveState> GetSnapshotAsync(CancellationToken cancellationToken = default);

    /// <summary>开始新会话；Running 状态重复调用必须保持原会话和累计值。</summary>
    Task StartAsync(CancellationToken cancellationToken = default);

    /// <summary>暂停当前会话；Paused 状态重复调用保持不变。</summary>
    Task PauseAsync(CancellationToken cancellationToken = default);

    /// <summary>恢复暂停会话；Running 状态重复调用保持不变。</summary>
    Task ResumeAsync(CancellationToken cancellationToken = default);

    /// <summary>结束并返回会话摘要；重复停止返回同一摘要而不重复创建记录。</summary>
    Task<TrainingSessionSummary?> StopAsync(CancellationToken cancellationToken = default);
}

/// <summary>
/// 定义可清除操作系统配对记录和应用固定设备选择的可选能力；不支持的平台无需实现。
/// </summary>
public interface IDevicePairingSession
{
    /// <summary>
    /// 断开当前链路，取消操作系统配对，并清除本会话保存的首选设备标识；下次连接必须重新选择和配对。
    /// </summary>
    Task ForgetDeviceAsync(CancellationToken cancellationToken = default);
}

/// <summary>
/// 表示一次 Windows BLE 扫描中可显示、可选择的附近设备；DeviceId 是后续连接使用的系统唯一标识。
/// </summary>
public sealed record NearbyBluetoothDevice(
    // Windows DeviceInformation.Id 用于按用户选择精确连接，禁止用可重复的广播名代替。
    string DeviceId,
    // 广播名或系统缓存名用于列表展示；空名由界面转换为“未命名蓝牙设备”。
    string DisplayName,
    // true 表示 Windows 已保存配对密钥；false 表示连接时需要系统配对。
    bool IsPaired,
    // 最近主动扫描信号强度，单位 dBm；适配器未提供时为 null。
    int? RssiDbm,
    // true 表示广播名符合产品前缀，只有此类设备允许进入健身协议连接。
    bool IsFitnessDevice);

/// <summary>
/// 定义设备页可选的附近 BLE 发现能力；Mock 或非 Windows 平台可不实现该接口。
/// </summary>
public interface IDeviceDiscoverySession
{
    /// <summary>
    /// 在指定时间窗主动扫描附近 BLE 设备；结果包含产品和其它设备，供用户像 Windows 设置一样辨认环境。
    /// </summary>
    Task<IReadOnlyList<NearbyBluetoothDevice>> ScanDevicesAsync(
        TimeSpan scanDuration,
        CancellationToken cancellationToken = default);

    /// <summary>
    /// 连接用户从最近扫描列表选中的产品手柄；其它设备必须被明确拒绝，不能误配耳机或鼠标。
    /// </summary>
    Task ConnectToDeviceAsync(
        NearbyBluetoothDevice device,
        CancellationToken cancellationToken = default);
}
