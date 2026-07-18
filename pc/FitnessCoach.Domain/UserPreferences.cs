// 用户偏好属于可离线持久化的领域配置，不依赖具体 JSON 或 SQLite 实现。
namespace FitnessCoach.Domain;

/// <summary>定义 PC 界面体重显示单位；设备协议始终使用千克，不受该选择影响。</summary>
public enum MeasurementUnitSystem : byte
{
    /// <summary>公制界面使用千克和千卡。</summary>
    Metric = 0,
    /// <summary>英制界面使用磅和千卡；保存及设备同步前换算为千克。</summary>
    Imperial = 1,
}

/// <summary>
/// 保存训练估算和设备界面所需用户设置。
/// </summary>
public sealed class UserPreferences
{
    /// <summary>true 表示下次启动使用 Windows 真 BLE；false 表示使用无硬件 Mock。</summary>
    public bool UseRealBleDevice { get; set; }

    /// <summary>用户体重，单位千克，用于设备端卡路里估算。</summary>
    public double WeightKilograms { get; set; } = 65.0;

    /// <summary>PC 界面体重显示单位；旧 JSON 缺少该字段时默认公制。</summary>
    public MeasurementUnitSystem UnitSystem { get; set; } = MeasurementUnitSystem.Metric;

    /// <summary>是否启用每次有效计数的振动反馈。</summary>
    public bool HapticEnabled { get; set; } = true;

    /// <summary>是否启用开始、目标和错误提示音。</summary>
    public bool SoundEnabled { get; set; }

    /// <summary>AMOLED 亮度百分比，允许范围 5～100。</summary>
    public int BrightnessPercent { get; set; } = 35;

    /// <summary>无操作自动熄屏秒数，允许范围 10～300。</summary>
    public int ScreenTimeoutSeconds { get; set; } = 30;

    /// <summary>每天训练目标，单位千卡；零表示未设置。</summary>
    public double DailyCalorieGoal { get; set; } = 300.0;

    /// <summary>true 表示允许诊断页显式开启 RawStream；普通用户默认关闭。</summary>
    public bool DeveloperModeEnabled { get; set; }

    /// <summary>用户资料单调修订号；本地保存成功后递增并发送给设备。</summary>
    public uint ProfileRevision { get; set; }

    /// <summary>设备偏好单调修订号；本地保存成功后递增并发送给设备。</summary>
    public uint PreferencesRevision { get; set; }
}
