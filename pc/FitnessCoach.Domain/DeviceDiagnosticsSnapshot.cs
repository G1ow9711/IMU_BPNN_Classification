// 诊断快照只使用领域类型，避免设备页和诊断页直接依赖 WinRT 或 Mock 实现。
namespace FitnessCoach.Domain;

/// <summary>
/// 保存某一时刻的设备身份、链路质量和协议错误计数；所有计数从当前 PC 进程启动后累计。
/// </summary>
public sealed class DeviceDiagnosticsSnapshot
{
    /// <summary>创建不可变诊断快照；未知数值使用 null，未知文本使用“未知”。</summary>
    public DeviceDiagnosticsSnapshot(
        // 设备稳定 ID 或广播名称；空白值会转换为“未知设备”。
        string deviceId,
        // 是否已完成 Manifest、订阅和权威状态恢复。
        bool isConnected,
        // 是否来自真实 Windows BLE；false 表示 Mock 会话。
        bool isHardwareBacked,
        // 标准 Device Information 型号文本；空白值显示“未知”。
        string modelNumber,
        // 厂家板卡硬件修订文本；空白值显示“未知”。
        string hardwareRevision,
        // 固件语义版本文本；空白值显示“未知”。
        string firmwareRevision,
        // 最近接收信号强度，单位 dBm；null 或超出 -127～20 表示未知。
        int? rssiDbm,
        // 当前 ATT MTU，单位字节；null 或小于 23 表示未知。
        ushort? attMtu,
        // 最近权威电量百分比，范围 0～100；null 表示设备未提供。
        byte? batteryPercent,
        // 最近接受的设备状态修订号；0 表示尚未收到权威状态。
        uint stateRevision,
        // 当前 PC 进程内自动重连尝试累计次数。
        uint reconnectCount,
        // 当前 PC 进程内 CRC-16 校验失败累计次数。
        uint crcErrorCount,
        // 当前 PC 进程内分片包络、顺序或长度错误累计次数。
        uint fragmentErrorCount,
        // 最近通过兼容校验的设备能力清单摘要；缺失时显示默认未知文本。
        string manifestSummary = "设备能力清单：未知")
    {
        // 设备 ID 为空时使用稳定占位符，防止界面绑定出现空文本。
        DeviceId = string.IsNullOrWhiteSpace(deviceId) ? "未知设备" : deviceId;
        // 保存完整连接态；发现广播但尚未恢复快照时必须为 false。
        IsConnected = isConnected;
        // 保存实现模式，界面据此区分真 BLE 与 Mock 数据。
        IsHardwareBacked = isHardwareBacked;
        // 保存标准 Device Information 的型号文本；空值统一显示未知。
        ModelNumber = NormalizeText(modelNumber);
        // 保存板卡硬件修订号；空值统一显示未知。
        HardwareRevision = NormalizeText(hardwareRevision);
        // 保存固件语义版本；空值统一显示未知。
        FirmwareRevision = NormalizeText(firmwareRevision);
        // RSSI 合法工程范围约为 -127～20 dBm；超界值视为系统不可用。
        RssiDbm = rssiDbm is >= -127 and <= 20 ? rssiDbm : null;
        // ATT MTU 最小合法值为 23；更小系统值视为未知。
        AttMtu = attMtu is >= 23 ? attMtu : null;
        // 电量只允许 0～100；协议中的 255 未知值在会话层转换为 null。
        BatteryPercent = batteryPercent is <= 100 ? batteryPercent : null;
        // 保存最近接受的权威状态修订号；零表示尚未收到状态。
        StateRevision = stateRevision;
        // 保存自动重连尝试次数；主动首次连接不计入该值。
        ReconnectCount = reconnectCount;
        // 保存 CRC-16 校验失败数量，用于判断射频噪声或协议字节损坏。
        CrcErrorCount = crcErrorCount;
        // 保存分片包络、顺序或长度错误数量，用于判断 MTU/丢包问题。
        FragmentErrorCount = fragmentErrorCount;
        // 保存已通过兼容校验的 Manifest 摘要；内容包含双模型短 SHA、能力位和 LittleFS 可用量。
        ManifestSummary = NormalizeText(manifestSummary);
    }

    /// <summary>设备用户可见 ID 或名称。</summary>
    public string DeviceId { get; }

    /// <summary>true 表示 Manifest、订阅和权威快照均已恢复。</summary>
    public bool IsConnected { get; }

    /// <summary>true 表示真实 Windows BLE，false 表示本地模拟设备。</summary>
    public bool IsHardwareBacked { get; }

    /// <summary>标准 GATT Device Information 型号。</summary>
    public string ModelNumber { get; }

    /// <summary>标准 GATT 硬件修订号。</summary>
    public string HardwareRevision { get; }

    /// <summary>标准 GATT 固件修订号。</summary>
    public string FirmwareRevision { get; }

    /// <summary>最近扫描 RSSI，单位 dBm；null 表示 Windows 未提供该属性。</summary>
    public int? RssiDbm { get; }

    /// <summary>当前 ATT MTU，单位字节；null 表示尚未连接。</summary>
    public ushort? AttMtu { get; }

    /// <summary>最近权威 LiveState 电量百分比；null 表示设备尚未提供有效值。</summary>
    public byte? BatteryPercent { get; }

    /// <summary>最近接受的权威 state_revision。</summary>
    public uint StateRevision { get; }

    /// <summary>当前 PC 进程内自动重连尝试次数。</summary>
    public uint ReconnectCount { get; }

    /// <summary>当前 PC 进程内 CRC 错误累计数。</summary>
    public uint CrcErrorCount { get; }

    /// <summary>当前 PC 进程内分片错误累计数。</summary>
    public uint FragmentErrorCount { get; }

    /// <summary>最近成功连接 Manifest 的兼容摘要；失败连接不会提交新值。</summary>
    public string ManifestSummary { get; }

    // 把空设备信息转换为稳定界面文本。
    private static string NormalizeText(string value)
    {
        // 返回非空原值，或统一未知占位符。
        return string.IsNullOrWhiteSpace(value) ? "未知" : value.Trim();
    }
}

/// <summary>
/// 由设备会话可选实现的只读诊断接口；诊断读取不得产生 GATT I/O 或改变训练状态。
/// </summary>
public interface IDeviceDiagnosticsSource
{
    /// <summary>返回线程安全快照；调用者可在 UI 刷新时同步读取。</summary>
    DeviceDiagnosticsSnapshot GetDiagnosticsSnapshot();
}
