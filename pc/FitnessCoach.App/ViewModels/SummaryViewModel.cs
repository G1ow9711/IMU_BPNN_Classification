// 引入领域会话摘要和动作指标。
using FitnessCoach.Domain;
// 引入可观察集合，列表绑定可自动刷新。
using System.Collections.ObjectModel;
// 引入 MVVM 通知基类。
using FitnessCoach.App.Mvvm;

// 总结页 ViewModel 位于应用视图模型命名空间。
namespace FitnessCoach.App.ViewModels;

/// <summary>展示刚结束会话的总时长、千卡和各动作指标。</summary>
public sealed class SummaryViewModel : ObservableObject
{
    // 当前会话设备标识。
    private string _deviceId = "--";
    // 当前会话序号。
    private uint _sessionSequence;
    // 当前格式化时长。
    private string _durationText = "00:00:00";
    // 当前格式化卡路里。
    private string _caloriesText = "0.00 千卡";
    // 当前结束原因。
    private string _endReason = "暂无训练总结";
    // 当前每日卡路里目标完成百分比；未设置目标时为零。
    private double _goalProgressPercent;
    // 当前目标完成度中文说明。
    private string _goalProgressText = "未设置每日卡路里目标";
    // 设备端会话持久化状态。
    private string _deviceSaveStatus = "设备保存状态未知";
    // PC 本地历史持久化状态。
    private string _localSaveStatus = "本地保存状态未知";

    /// <summary>创建空总结页。</summary>
    public SummaryViewModel()
    {
        // 初始化可观察动作指标集合。
        Metrics = new ObservableCollection<ActionMetricRow>();
    }

    /// <summary>设备标识。</summary>
    public string DeviceId
    {
        // 返回设备 ID。
        get => _deviceId;
        // 内部更新设备 ID。
        private set => SetProperty(ref _deviceId, value);
    }

    /// <summary>设备会话序号。</summary>
    public uint SessionSequence
    {
        // 返回会话序号。
        get => _sessionSequence;
        // 内部更新会话序号。
        private set => SetProperty(ref _sessionSequence, value);
    }

    /// <summary>格式化会话时长。</summary>
    public string DurationText
    {
        // 返回时长文本。
        get => _durationText;
        // 内部更新时长文本。
        private set => SetProperty(ref _durationText, value);
    }

    /// <summary>格式化总卡路里。</summary>
    public string CaloriesText
    {
        // 返回卡路里文本。
        get => _caloriesText;
        // 内部更新卡路里文本。
        private set => SetProperty(ref _caloriesText, value);
    }

    /// <summary>会话结束原因。</summary>
    public string EndReason
    {
        // 返回结束原因。
        get => _endReason;
        // 内部更新结束原因。
        private set => SetProperty(ref _endReason, value);
    }

    /// <summary>每日卡路里目标完成百分比；允许超过 100 表示超额完成。</summary>
    public double GoalProgressPercent
    {
        // 返回按会话千卡除以当前每日目标得到的百分比。
        get => _goalProgressPercent;
        // 仅由应用会话完成路径更新。
        private set => SetProperty(ref _goalProgressPercent, value);
    }

    /// <summary>包含当前千卡、目标千卡和百分比的中文进度文本。</summary>
    public string GoalProgressText
    {
        // 返回用户可见目标说明。
        get => _goalProgressText;
        // 仅由摘要应用过程更新。
        private set => SetProperty(ref _goalProgressText, value);
    }

    /// <summary>设备是否已经在停止 ACK 前保存会话。</summary>
    public string DeviceSaveStatus
    {
        // 返回设备侧保存状态。
        get => _deviceSaveStatus;
        // 仅由会话完成事件更新。
        private set => SetProperty(ref _deviceSaveStatus, value);
    }

    /// <summary>PC 是否已经把同一摘要写入本地历史。</summary>
    public string LocalSaveStatus
    {
        // 返回本地保存状态。
        get => _localSaveStatus;
        // 仅由会话完成事件更新。
        private set => SetProperty(ref _localSaveStatus, value);
    }

    /// <summary>按动作展示的指标行。</summary>
    public ObservableCollection<ActionMetricRow> Metrics { get; }

    /// <summary>兼容旧调用路径；没有目标和保存上下文时明确显示未知状态。</summary>
    public void SetSummary(TrainingSessionSummary summary)
    {
        // 旧调用没有目标或持久化事实，不能伪装为成功。
        SetSummary(summary, 0.0, devicePersisted: false, localPersisted: false);
    }

    /// <summary>应用设备权威摘要、当前每日千卡目标和双端持久化事实。</summary>
    public void SetSummary(
        TrainingSessionSummary summary,
        double dailyCalorieGoal,
        bool devicePersisted,
        bool localPersisted)
    {
        // 摘要不能为空。
        ArgumentNullException.ThrowIfNull(summary);
        // 复制设备标识。
        DeviceId = summary.DeviceId;
        // 复制会话序号。
        SessionSequence = summary.SessionSequence;
        // 使用设备单调时长，避免墙钟跳变。
        DurationText = TimeSpan.FromMilliseconds(summary.ElapsedMilliseconds).ToString(@"hh\:mm\:ss");
        // 显示两位小数千卡。
        CaloriesText = $"{summary.CaloriesKcal:F2} 千卡";
        // 显示设备结束原因。
        EndReason = summary.EndReason;
        // 只有有限正目标才能计算百分比；零表示用户关闭每日目标。
        if (double.IsFinite(dailyCalorieGoal) && (dailyCalorieGoal > 0.0))
        {
            // 使用设备摘要千卡除以本地当前目标，结果允许超过 100%。
            GoalProgressPercent = (summary.CaloriesKcal / dailyCalorieGoal) * 100.0;
            // 同时显示分子、分母和一位小数百分比，便于用户核对估算来源。
            GoalProgressText = $"每日目标：{summary.CaloriesKcal:F2} / {dailyCalorieGoal:F2} 千卡（{GoalProgressPercent:F1}%）";
        }
        else
        {
            // 未设置目标时归零，避免旧会话百分比残留。
            GoalProgressPercent = 0.0;
            // 给出明确关闭状态，不执行除零。
            GoalProgressText = "未设置每日卡路里目标";
        }

        // 设备 Stop ACK 合同保证返回摘要前已经完成设备侧持久化。
        DeviceSaveStatus = devicePersisted ? "设备会话已保存" : "设备保存状态未知";
        // SessionCompleted 只在 JSON SaveAsync 成功后触发，因此可显示本地成功。
        LocalSaveStatus = localPersisted ? "本地历史已保存" : "本地保存状态未知";
        // 清除上一会话的动作行。
        Metrics.Clear();

        // 按模型动作索引稳定排序摘要指标。
        foreach (ActionMetric metric in summary.ActionMetrics.OrderBy(item => item.Action))
        {
            // 构造不再变化的绑定行。
            Metrics.Add(new ActionMetricRow(
                DisplayText.ActionName(metric.Action),
                $"{metric.MetricValue} {DisplayText.MetricUnit(metric.MetricKind)}",
                TimeSpan.FromMilliseconds(metric.ActiveMilliseconds).ToString(@"hh\:mm\:ss"),
                $"{metric.CaloriesMilliKcal / 1000.0:F2} 千卡"));
        }
    }
}

/// <summary>总结页单个动作的只读显示行。</summary>
public sealed record ActionMetricRow(string ActionName, string MetricText, string DurationText, string CaloriesText);
