// 引入领域枚举，为 UI 提供统一中文显示文本。
using FitnessCoach.Domain;

// 显示文本工具只负责格式化，不修改业务状态。
namespace FitnessCoach.App.ViewModels;

/// <summary>集中转换动作、状态和指标单位，避免各页面出现不同中文口径。</summary>
public static class DisplayText
{
    /// <summary>把会话内部设备标识转换为设备页可读文字；真实设备标识保持原样。</summary>
    public static string DeviceIdentifier(string? deviceId)
    {
        // 空值或纯空白表示会话尚未提供任何身份，使用稳定中文占位符。
        if (string.IsNullOrWhiteSpace(deviceId))
        {
            // 返回用户可理解的未选择状态，不显示 null 或空行。
            return "未选择蓝牙设备";
        }

        // 只转换应用自己定义的两个内部占位值；真实硬件标识必须原样保留。
        return deviceId switch
        {
            // Windows 真机扫描前的内部占位值转换为中文。
            "BLE-UNSELECTED" => "未选择蓝牙设备",
            // 无硬件演示会话的固定内部编号转换为中文。
            "MOCK-ESP32S3-0001" => "本机模拟设备",
            // 已连接设备的真实广播名或系统标识用于诊断，不能翻译或改写。
            _ => deviceId,
        };
    }

    /// <summary>把动作枚举转换为中文名称。</summary>
    public static string ActionName(ActionId action)
    {
        // 使用穷尽映射保持与 11 类合同一致。
        return action switch
        {
            // 模型索引 0。
            ActionId.GoodMorning => "早安式",
            // 模型索引 1。
            ActionId.JumpingJack => "开合跳",
            // 模型索引 2。
            ActionId.JumpingLunge => "跳跃弓步",
            // 模型索引 3。
            ActionId.JumpingSquat => "跳跃深蹲",
            // 模型索引 4。
            ActionId.Lunge => "弓步",
            // 模型索引 5。
            ActionId.Sit => "静坐",
            // 模型索引 6。
            ActionId.Squat => "深蹲",
            // 模型索引 7。
            ActionId.Trot => "小跑",
            // 模型索引 8。
            ActionId.TuckJump => "收腹跳",
            // 模型索引 9。
            ActionId.Walk => "行走",
            // 模型索引 10。
            ActionId.Wave => "挥手",
            // Unknown 或未来未知值显示等待识别。
            _ => "等待识别",
        };
    }

    /// <summary>把设备状态转换为用户可读中文。</summary>
    public static string DeviceStateName(FitnessDeviceState state)
    {
        // 按设备权威状态选择固定文本。
        return state switch
        {
            // 开机自检阶段。
            FitnessDeviceState.Booting => "开机自检",
            // 空闲可开始阶段。
            FitnessDeviceState.Idle => "空闲",
            // 正在采样识别阶段。
            FitnessDeviceState.Running => "训练中",
            // 会话暂停阶段。
            FitnessDeviceState.Paused => "已暂停",
            // 会话总结阶段。
            FitnessDeviceState.Summary => "训练完成",
            // 仅屏幕关闭。
            FitnessDeviceState.ScreenOff => "息屏待机",
            // 充电空闲状态。
            FitnessDeviceState.Charging => "充电中",
            // 设备阻断错误。
            FitnessDeviceState.Error => "设备错误",
            // 防御未来未知枚举。
            _ => "未知状态",
        };
    }

    /// <summary>返回指标数值后缀。</summary>
    public static string MetricUnit(MetricKind kind)
    {
        // 次数、步数和秒数必须严格区分。
        return kind switch
        {
            // 完整动作次数。
            MetricKind.Repetition => "次",
            // 行走或小跑步数。
            MetricKind.Step => "步",
            // 持续动作秒数。
            MetricKind.Second => "秒",
            // 无指标不显示单位。
            _ => string.Empty,
        };
    }

    /// <summary>把指标枚举转换为中文名称，供历史详情和导出表格共用。</summary>
    public static string MetricKindName(MetricKind kind)
    {
        // 使用穷尽映射，禁止把内部枚举英文名直接展示给用户。
        return kind switch
        {
            // 完整动作次数指标。
            MetricKind.Repetition => "动作次数",
            // 行走或小跑步数指标。
            MetricKind.Step => "步数",
            // 持续动作时间指标。
            MetricKind.Second => "持续时间",
            // 当前动作不提供累计指标。
            MetricKind.None => "无累计指标",
            // 防御未来新增枚举，使用稳定中文占位文本。
            _ => "未知指标",
        };
    }
}
