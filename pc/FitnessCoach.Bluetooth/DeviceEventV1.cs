// 引入领域动作、设备状态和指标单位，避免蓝牙层复制第二套枚举。
using FitnessCoach.Domain;

// EventV1 属于蓝牙协议对象，应用层仍以 LiveState 作为累计值唯一事实源。
namespace FitnessCoach.Bluetooth;

/// <summary>定义 EventV1 的 11 种稳定线上类型；数值必须与设备端 ble_service_event_type_t 一致。</summary>
public enum DeviceEventType : byte
{
    /// <summary>新会话已创建。</summary>
    SessionStarted = 1,
    /// <summary>运行会话已暂停。</summary>
    SessionPaused = 2,
    /// <summary>暂停会话已恢复。</summary>
    SessionResumed = 3,
    /// <summary>会话已停止并保存摘要。</summary>
    SessionStopped = 4,
    /// <summary>设备锁定了新的动作，界面可立即切换动画。</summary>
    ActionChanged = 5,
    /// <summary>完成一次重复动作，或产生一个步数反馈事件。</summary>
    RepetitionCounted = 6,
    /// <summary>用户训练目标已完成。</summary>
    GoalReached = 7,
    /// <summary>原配 400mAh 电池进入低电量门槛。</summary>
    LowBattery = 8,
    /// <summary>QMI8658 采样或时间连续性故障。</summary>
    SensorFault = 9,
    /// <summary>LittleFS 或 TF 存储故障。</summary>
    StorageFault = 10,
    /// <summary>设备即将安全关机。</summary>
    PowerOffPending = 11,
}

/// <summary>
/// 保存经过范围校验的 EventV1；事件只触发低延迟动画/提示，PC 不据此自行增加权威次数或卡路里。
/// </summary>
public sealed class DeviceEventV1
{
    /// <summary>创建一个固定字段事件并执行全部 v1 范围检查。</summary>
    /// <param name="eventVersion">事件 payload 版本；当前必须为 1。</param>
    /// <param name="eventType">线上事件类型，合法范围为 1～11。</param>
    /// <param name="deviceState">事件发生后的设备权威状态，合法范围为 0～7。</param>
    /// <param name="action">模型动作索引 0～10，或 255 表示未知。</param>
    /// <param name="metricKind">指标单位：无、次、步或秒。</param>
    /// <param name="batteryPercent">电池百分比 0～100，或 255 表示未知。</param>
    /// <param name="qualityFlags">传感器、振动替代和存储质量位图。</param>
    /// <param name="sessionSequence">设备持久化会话序号。</param>
    /// <param name="eventSequence">当前会话内严格递增的事件序号。</param>
    /// <param name="stateRevision">事件对应的权威状态修订号。</param>
    /// <param name="metricDelta">本次事件新增次数、步数或秒数。</param>
    /// <param name="metricTotal">事件发生后的设备权威累计指标。</param>
    /// <param name="caloriesMilliKcal">累计热量，单位 0.001 kcal。</param>
    /// <param name="confidenceQ15">模型置信度，0～65535 映射到 0～1。</param>
    /// <param name="detailCode">事件类型专用诊断码；无附加信息时为 0。</param>
    public DeviceEventV1(
        byte eventVersion,
        DeviceEventType eventType,
        FitnessDeviceState deviceState,
        ActionId action,
        MetricKind metricKind,
        byte batteryPercent,
        ushort qualityFlags,
        uint sessionSequence,
        uint eventSequence,
        uint stateRevision,
        uint metricDelta,
        uint metricTotal,
        uint caloriesMilliKcal,
        ushort confidenceQ15,
        ushort detailCode)
    {
        // 当前只支持 payload 版本 1，未知版本不能套用固定偏移。
        if (eventVersion != 1)
        {
            // 抛出明确版本异常，调用方应等待协议升级而不是猜字段。
            throw new ArgumentOutOfRangeException(nameof(eventVersion), "Event payload 版本必须为 1。");
        }

        // 事件类型只允许线上稳定值 1～11。
        if ((eventType < DeviceEventType.SessionStarted) || (eventType > DeviceEventType.PowerOffPending))
        {
            // 未知事件不得错误播放动画或故障提示。
            throw new ArgumentOutOfRangeException(nameof(eventType), "Event 类型必须位于 1..11。");
        }

        // 设备状态只允许 Booting～Error 的 0～7。
        if (deviceState > FitnessDeviceState.Error)
        {
            // 拒绝未来状态被旧客户端误解释。
            throw new ArgumentOutOfRangeException(nameof(deviceState), "设备状态必须位于 0..7。");
        }

        // 动作只允许模型索引 0～10 或 255 未知。
        if ((action > ActionId.Wave) && (action != ActionId.Unknown))
        {
            // 非法动作不能驱动错误的健身动画。
            throw new ArgumentOutOfRangeException(nameof(action), "动作必须位于 0..10，或使用 255 表示未知。");
        }

        // 指标单位只允许无、次、步、秒四种稳定值。
        if (metricKind > MetricKind.Second)
        {
            // 未知单位不能安全格式化数值。
            throw new ArgumentOutOfRangeException(nameof(metricKind), "指标单位必须位于 0..3。");
        }

        // 电量只能是 0～100，或 255 表示暂不可用。
        if ((batteryPercent > 100) && (batteryPercent != byte.MaxValue))
        {
            // 非法电量不得进入低电量提示。
            throw new ArgumentOutOfRangeException(nameof(batteryPercent), "电量必须位于 0..100，或使用 255 表示未知。");
        }

        // 保存固定事件结构版本。
        EventVersion = eventVersion;
        // 保存低延迟事件语义。
        EventType = eventType;
        // 保存事件提交后的设备状态。
        DeviceState = deviceState;
        // 保存关联动作或 Unknown。
        Action = action;
        // 保存主指标单位。
        MetricKind = metricKind;
        // 保存电池百分比或 Unknown。
        BatteryPercent = batteryPercent;
        // 保存质量位原始集合；具体位定义与 LiveState 一致。
        QualityFlags = qualityFlags;
        // 保存持久化会话序号。
        SessionSequence = sessionSequence;
        // 保存会话内事件序号。
        EventSequence = eventSequence;
        // 保存事件提交后的权威状态修订号。
        StateRevision = stateRevision;
        // 保存本次指标增量。
        MetricDelta = metricDelta;
        // 保存事件时刻指标累计值，仅用于即时标签。
        MetricTotal = metricTotal;
        // 保存千分之一千卡累计值，仅用于即时标签。
        CaloriesMilliKcal = caloriesMilliKcal;
        // 保存 Q15 稳定度或置信度。
        ConfidenceQ15 = confidenceQ15;
        // 保存低电量门槛、故障或关机原因子码。
        DetailCode = detailCode;
    }

    /// <summary>事件 payload 版本，当前固定为 1。</summary>
    public byte EventVersion { get; }
    /// <summary>事件类型。</summary>
    public DeviceEventType EventType { get; }
    /// <summary>事件提交后的设备状态。</summary>
    public FitnessDeviceState DeviceState { get; }
    /// <summary>关联动作，或 Unknown。</summary>
    public ActionId Action { get; }
    /// <summary>指标单位。</summary>
    public MetricKind MetricKind { get; }
    /// <summary>电池百分比，255 表示未知。</summary>
    public byte BatteryPercent { get; }
    /// <summary>质量位集合。</summary>
    public ushort QualityFlags { get; }
    /// <summary>持久化会话序号。</summary>
    public uint SessionSequence { get; }
    /// <summary>会话内事件序号。</summary>
    public uint EventSequence { get; }
    /// <summary>权威状态修订号。</summary>
    public uint StateRevision { get; }
    /// <summary>本次指标增量。</summary>
    public uint MetricDelta { get; }
    /// <summary>事件时刻指标累计值；掉包后由 LiveState 恢复。</summary>
    public uint MetricTotal { get; }
    /// <summary>事件时刻累计热量，单位为千分之一千卡。</summary>
    public uint CaloriesMilliKcal { get; }
    /// <summary>Q15 稳定度或置信度。</summary>
    public ushort ConfidenceQ15 { get; }
    /// <summary>事件专用原因子码。</summary>
    public ushort DetailCode { get; }
}
