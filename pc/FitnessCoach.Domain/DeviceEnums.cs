// 领域枚举统一放在该命名空间，确保蓝牙层和未来 WPF 层共享相同含义。
namespace FitnessCoach.Domain;

/// <summary>
/// 表示设备会话状态；PC 必须以设备状态为准，不能自行推导开始或暂停。
/// </summary>
public enum FitnessDeviceState : byte
{
    /// <summary>设备正在开机和自检。</summary>
    Booting = 0,
    /// <summary>设备空闲，允许开始新会话。</summary>
    Idle = 1,
    /// <summary>设备已开始采样，正在累计首个动作的分类证据。</summary>
    Preparing = 2,
    /// <summary>设备已经锁定本轮唯一动作，正在采样、识别和计数。</summary>
    Running = 3,
    /// <summary>会话暂停，累计值保留但不增加。</summary>
    Paused = 4,
    /// <summary>会话已经结束，正在展示或保存总结。</summary>
    Summary = 5,
    /// <summary>传感器、存储或模型出现阻断性错误。</summary>
    Error = 6,
    /// <summary>设备已经进入安全关机流程，不再接受训练命令。</summary>
    Shutdown = 7,
}

/// <summary>
/// 表示当前主要指标的单位，避免 PC 把步数、次数和持续时间混为一谈。
/// </summary>
public enum MetricKind : byte
{
    /// <summary>当前动作没有可显示的累计指标。</summary>
    None = 0,
    /// <summary>力量或跳跃动作的完整重复次数。</summary>
    Repetition = 1,
    /// <summary>行走或小跑的步数。</summary>
    Step = 2,
    /// <summary>静坐等持续型动作的累计秒数。</summary>
    Second = 3,
}

/// <summary>
/// 表示实时数据质量；每一位都可独立组合并显示诊断提示。
/// </summary>
[Flags]
public enum DataQualityFlags : ushort
{
    /// <summary>当前窗口没有发现质量异常。</summary>
    None = 0,
    /// <summary>动作段刚开始，累计证据仍在预热。</summary>
    Warmup = 1 << 0,
    /// <summary>模型置信度低于设备设定阈值。</summary>
    LowConfidence = 1 << 1,
    /// <summary>IMU 读取失败或样本时间不连续。</summary>
    SensorFault = 1 << 2,
    /// <summary>振动反馈期间使用了替代样本以避免马达污染。</summary>
    HapticSampleSubstituted = 1 << 3,
    /// <summary>设备还没有获得可信 UTC 时间。</summary>
    TimeUnsynchronized = 1 << 4,
    /// <summary>microSD 不存在，因此没有原始日志。</summary>
    StorageUnavailable = 1 << 5,
    /// <summary>原始流缓冲区溢出，开发者日志存在缺口。</summary>
    RawStreamOverrun = 1 << 6,
}

/// <summary>
/// 表示电源状态位；电量百分比单独通过实时状态字段传输。
/// </summary>
[Flags]
public enum PowerFlags : byte
{
    /// <summary>当前没有充电、USB 或低电量标志。</summary>
    None = 0,
    /// <summary>AXP2101 报告电池正在充电。</summary>
    Charging = 1 << 0,
    /// <summary>USB 电源当前存在。</summary>
    UsbPresent = 1 << 1,
    /// <summary>电量低于设备低电量阈值。</summary>
    LowBattery = 1 << 2,
}
