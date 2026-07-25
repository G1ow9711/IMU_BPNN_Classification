// 引入领域命名空间，定义不含 BLE 字节操作的权威实时状态对象。
namespace FitnessCoach.Domain;

/// <summary>
/// 保存 ESP32 发布的一次完整实时状态；所有累计值均由设备计算，PC 不重复计数。
/// </summary>
public sealed class LiveState
{
    /// <summary>
    /// 创建经过基本范围校验的实时状态。
    /// </summary>
    /// <param name="sessionSequence">设备持久化会话序号，与设备 ID 共同构成历史键。</param>
    /// <param name="stateRevision">单调递增状态修订号，用于丢弃乱序旧通知。</param>
    /// <param name="elapsedMilliseconds">当前会话单调时长，单位毫秒。</param>
    /// <param name="deviceState">设备权威运行状态。</param>
    /// <param name="action">稳定动作索引，或 Unknown 表示尚无可靠分类。</param>
    /// <param name="metricKind">当前累计值的业务单位。</param>
    /// <param name="batteryPercent">电池百分比 0～100，或 255 表示未知。</param>
    /// <param name="metricValue">设备权威次数、步数或秒数。</param>
    /// <param name="confidenceQ15">模型置信度，0～65535 映射到 0～1。</param>
    /// <param name="caloriesMilliKcal">累计热量，单位 0.001 kcal。</param>
    /// <param name="qualityFlags">传感器、旧固件替代样本和存储质量标志。</param>
    /// <param name="powerFlags">充电、USB 和低电量状态位。</param>
    /// <param name="goalPercent">目标完成度 0～100，或 255 表示未配置。</param>
    public LiveState(
        uint sessionSequence,
        uint stateRevision,
        uint elapsedMilliseconds,
        FitnessDeviceState deviceState,
        ActionId action,
        MetricKind metricKind,
        byte batteryPercent,
        uint metricValue,
        ushort confidenceQ15,
        uint caloriesMilliKcal,
        DataQualityFlags qualityFlags,
        PowerFlags powerFlags,
        byte goalPercent)
    {
        // 电量只能是 0 到 100，或 255 表示暂不可用。
        if ((batteryPercent > 100) && (batteryPercent != byte.MaxValue))
        {
            // 非法电量意味着协议字段损坏，拒绝构造业务对象。
            throw new ArgumentOutOfRangeException(nameof(batteryPercent), "电量必须位于 0..100，或使用 255 表示未知。");
        }

        // 目标完成度只能是 0 到 100，或 255 表示没有目标。
        if ((goalPercent > 100) && (goalPercent != byte.MaxValue))
        {
            // 非法目标值不得进入 WPF 进度条。
            throw new ArgumentOutOfRangeException(nameof(goalPercent), "目标完成度必须位于 0..100，或使用 255 表示未设置。");
        }

        // 保存设备持久化会话序号，用于与历史记录去重。
        SessionSequence = sessionSequence;
        // 保存单调递增状态修订号，PC 用它丢弃乱序旧通知。
        StateRevision = stateRevision;
        // 保存会话单调时长，单位为毫秒，不受 UTC 校时影响。
        ElapsedMilliseconds = elapsedMilliseconds;
        // 保存设备权威状态，例如运行、暂停或总结。
        DeviceState = deviceState;
        // 保存当前稳定动作；Unknown 表示没有可靠分类。
        Action = action;
        // 保存当前指标单位，决定界面显示“次”“步”或“秒”。
        MetricKind = metricKind;
        // 保存电量百分比；255 表示 PMIC 暂无有效数据。
        BatteryPercent = batteryPercent;
        // 保存当前动作的次数、步数或秒数，具体单位由 MetricKind 指定。
        MetricValue = metricValue;
        // 保存 Q15 置信度，0 到 65535 对应 0% 到 100%。
        ConfidenceQ15 = confidenceQ15;
        // 保存千分之一千卡，避免 BLE 浮点格式和跨语言舍入差异。
        CaloriesMilliKcal = caloriesMilliKcal;
        // 保存传感器、预热、旧固件替代样本和存储质量标志。
        QualityFlags = qualityFlags;
        // 保存充电、USB 和低电量状态位。
        PowerFlags = powerFlags;
        // 保存目标百分比；255 表示没有配置目标。
        GoalPercent = goalPercent;
    }

    /// <summary>设备持久化会话序号，与设备 ID 共同构成全局会话键。</summary>
    public uint SessionSequence { get; }

    /// <summary>设备状态修订号，新状态必须大于旧状态。</summary>
    public uint StateRevision { get; }

    /// <summary>当前会话单调时长，单位为毫秒。</summary>
    public uint ElapsedMilliseconds { get; }

    /// <summary>设备权威运行状态。</summary>
    public FitnessDeviceState DeviceState { get; }

    /// <summary>当前稳定动作类别。</summary>
    public ActionId Action { get; }

    /// <summary>当前主要累计值的单位。</summary>
    public MetricKind MetricKind { get; }

    /// <summary>电池百分比，255 表示未知。</summary>
    public byte BatteryPercent { get; }

    /// <summary>次数、步数或秒数。</summary>
    public uint MetricValue { get; }

    /// <summary>Q15 置信度，0 到 65535 对应 0 到 1。</summary>
    public ushort ConfidenceQ15 { get; }

    /// <summary>累计卡路里，单位为千分之一千卡。</summary>
    public uint CaloriesMilliKcal { get; }

    /// <summary>当前数据质量标志集合。</summary>
    public DataQualityFlags QualityFlags { get; }

    /// <summary>当前电源状态标志集合。</summary>
    public PowerFlags PowerFlags { get; }

    /// <summary>目标完成度百分比，255 表示未设置目标。</summary>
    public byte GoalPercent { get; }

    /// <summary>把 Q15 置信度转换为适合界面绑定的 0 到 1 双精度值。</summary>
    public double Confidence => ConfidenceQ15 / 65535.0;

    /// <summary>把千分之一千卡转换为界面使用的千卡数。</summary>
    public double CaloriesKcal => CaloriesMilliKcal / 1000.0;
}
