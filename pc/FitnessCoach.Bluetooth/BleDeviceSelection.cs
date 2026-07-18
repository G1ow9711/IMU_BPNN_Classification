// 设备选择对象位于无 WinRT 依赖的协议层，使扫描选择策略可在无蓝牙主机测试。
namespace FitnessCoach.Bluetooth;

/// <summary>表示 Windows 扫描到的一个 BLE Association Endpoint。</summary>
public sealed record BleDiscoveredDevice(
    // Windows DeviceInformation.Id，用于 FromIdAsync 和优先重连。
    string DeviceId,
    // 广播或系统缓存名称，空值由界面显示为未命名设备。
    string DisplayName,
    // 系统是否已有绑定记录；false 时 WinRT transport 必须进入配对边界。
    bool IsPaired,
    // 最近主动扫描 RSSI，单位 dBm；Windows 适配器不提供时为 null。
    int? RssiDbm = null);

/// <summary>把扫描列表选择为一个具体设备 ID，UI 可替换为人工选择对话框。</summary>
public interface IWindowsBleDeviceSelector
{
    /// <summary>选择一个设备；返回 null 表示用户取消，本次连接不得自动选择其它设备。</summary>
    Task<BleDiscoveredDevice?> SelectAsync(
        IReadOnlyList<BleDiscoveredDevice> devices,
        string? preferredDeviceId,
        CancellationToken cancellationToken);
}

/// <summary>生产默认选择器：优先旧设备，其次首个 BPNN-FIT 广播名，最后拒绝不相关设备。</summary>
public sealed class FirstFitnessDeviceSelector : IWindowsBleDeviceSelector
{
    /// <inheritdoc />
    public Task<BleDiscoveredDevice?> SelectAsync(
        IReadOnlyList<BleDiscoveredDevice> devices,
        string? preferredDeviceId,
        CancellationToken cancellationToken)
    {
        // 允许用户取消 WPF 连接流程。
        cancellationToken.ThrowIfCancellationRequested();
        // 扫描结果集合不能为空引用。
        ArgumentNullException.ThrowIfNull(devices);

        // 指定旧设备 ID 时优先精确匹配，保证自动重连不跳到旁边另一块手柄。
        if (!string.IsNullOrWhiteSpace(preferredDeviceId))
        {
            // 使用 Windows DeviceInformation.Id 的序号不敏感比较。
            BleDiscoveredDevice? preferred = devices.FirstOrDefault(
                device => string.Equals(device.DeviceId, preferredDeviceId, StringComparison.OrdinalIgnoreCase));
            // 找到旧设备立即返回。
            if (preferred is not null)
            {
                // 返回该设备，不再考虑其它广播名。
                return Task.FromResult<BleDiscoveredDevice?>(preferred);
            }
        }

        // 只自动选择产品约定广播名前缀，避免误配鼠标、耳机或邻居传感器。
        BleDiscoveredDevice? fitnessDevice = devices.FirstOrDefault(
            device => device.DisplayName.StartsWith("BPNN-FIT-", StringComparison.OrdinalIgnoreCase));
        // 返回找到的产品设备或 null，调用方据此提示用户靠近/开机。
        return Task.FromResult(fitnessDevice);
    }
}
