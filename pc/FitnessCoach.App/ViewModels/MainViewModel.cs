// 引入设备和摘要领域对象。
using FitnessCoach.Domain;
// 引入 MVVM 通知和同步命令。
using FitnessCoach.App.Mvvm;

// 主窗口 ViewModel 位于应用命名空间。
namespace FitnessCoach.App.ViewModels;

/// <summary>组合六个页面并处理导航、总结跳转和应用初始化。</summary>
public sealed class MainViewModel : ObservableObject, IDisposable
{
    // 当前显示页面对象，由 App.xaml DataTemplate 选择具体 UserControl。
    private object _currentPage;
    // 当前页面标题。
    private string _pageTitle = "设备";
    // 是否已释放子页面订阅。
    private bool _disposed;

    /// <summary>创建主窗口页面组合。</summary>
    public MainViewModel(
        DeviceViewModel device,
        LiveTrainingViewModel liveTraining,
        SummaryViewModel summary,
        HistoryViewModel history,
        SettingsViewModel settings,
        DiagnosticsViewModel diagnostics)
    {
        // 验证并保存设备页。
        Device = device ?? throw new ArgumentNullException(nameof(device));
        // 验证并保存实时训练页。
        LiveTraining = liveTraining ?? throw new ArgumentNullException(nameof(liveTraining));
        // 验证并保存总结页。
        Summary = summary ?? throw new ArgumentNullException(nameof(summary));
        // 验证并保存历史页。
        History = history ?? throw new ArgumentNullException(nameof(history));
        // 验证并保存设置页。
        Settings = settings ?? throw new ArgumentNullException(nameof(settings));
        // 验证并保存诊断页。
        Diagnostics = diagnostics ?? throw new ArgumentNullException(nameof(diagnostics));
        // 初始显示设备页。
        _currentPage = Device;
        // 创建六个导航命令。
        ShowDeviceCommand = new RelayCommand(_ => Navigate(Device, "设备"));
        // 导航到实时训练。
        ShowLiveTrainingCommand = new RelayCommand(_ => Navigate(LiveTraining, "实时训练"));
        // 导航到总结。
        ShowSummaryCommand = new RelayCommand(_ => Navigate(Summary, "训练总结"));
        // 导航到历史并异步刷新。
        ShowHistoryCommand = new RelayCommand(_ =>
        {
            // 立即切换页面。
            Navigate(History, "历史记录");
            // 后台刷新本地历史；页面状态会显示异常。
            _ = History.RefreshAsync();
        });
        // 导航到设置。
        ShowSettingsCommand = new RelayCommand(_ => Navigate(Settings, "设置"));
        // 导航到诊断并刷新安全信息。
        ShowDiagnosticsCommand = new RelayCommand(_ =>
        {
            // 切换诊断页。
            Navigate(Diagnostics, "诊断" );
            // 刷新当前连接摘要。
            Diagnostics.Refresh();
        });
        // 停止会话后自动展示总结。
        LiveTraining.SessionCompleted += OnSessionCompleted;
    }

    /// <summary>设备页。</summary>
    public DeviceViewModel Device { get; }

    /// <summary>实时训练页。</summary>
    public LiveTrainingViewModel LiveTraining { get; }

    /// <summary>训练总结页。</summary>
    public SummaryViewModel Summary { get; }

    /// <summary>历史页。</summary>
    public HistoryViewModel History { get; }

    /// <summary>设置页。</summary>
    public SettingsViewModel Settings { get; }

    /// <summary>诊断页。</summary>
    public DiagnosticsViewModel Diagnostics { get; }

    /// <summary>设备页导航命令。</summary>
    public RelayCommand ShowDeviceCommand { get; }

    /// <summary>实时训练页导航命令。</summary>
    public RelayCommand ShowLiveTrainingCommand { get; }

    /// <summary>总结页导航命令。</summary>
    public RelayCommand ShowSummaryCommand { get; }

    /// <summary>历史页导航命令。</summary>
    public RelayCommand ShowHistoryCommand { get; }

    /// <summary>设置页导航命令。</summary>
    public RelayCommand ShowSettingsCommand { get; }

    /// <summary>诊断页导航命令。</summary>
    public RelayCommand ShowDiagnosticsCommand { get; }

    /// <summary>当前页面对象。</summary>
    public object CurrentPage
    {
        // 返回页面对象。
        get => _currentPage;
        // 私有更新页面对象。
        private set => SetProperty(ref _currentPage, value);
    }

    /// <summary>窗口顶部页面标题。</summary>
    public string PageTitle
    {
        // 返回标题。
        get => _pageTitle;
        // 私有更新标题。
        private set => SetProperty(ref _pageTitle, value);
    }

    /// <summary>初始化仓储页面数据。</summary>
    public async Task InitializeAsync()
    {
        // 并行读取设置和历史，减少窗口就绪时间。
        await Task.WhenAll(Settings.LoadAsync(), History.RefreshAsync()).ConfigureAwait(true);
    }

    // 切换页面和标题。
    private void Navigate(object page, string title)
    {
        // 更新当前页面对象。
        CurrentPage = page;
        // 更新顶部标题。
        PageTitle = title;
    }

    // 接收已保存摘要并自动进入总结页。
    private void OnSessionCompleted(object? sender, TrainingSessionSummary summary)
    {
        // 把摘要、当前每日千卡目标和已完成的双端保存事实送给总结页。
        Summary.SetSummary(
            summary,
            Settings.DailyCalorieGoal,
            devicePersisted: true,
            localPersisted: true);
        // 切换到总结页面。
        Navigate(Summary, "训练总结");
        // 刷新历史，幂等保存后可立即看到新记录。
        _ = History.RefreshAsync();
    }

    /// <inheritdoc />
    public void Dispose()
    {
        // 重复释放直接返回。
        if (_disposed)
        {
            // 子页面已释放。
            return;
        }

        // 标记主 ViewModel 已释放。
        _disposed = true;
        // 解除总结事件。
        LiveTraining.SessionCompleted -= OnSessionCompleted;
        // 释放设备页事件订阅。
        Device.Dispose();
        // 释放实时页事件订阅。
        LiveTraining.Dispose();
        // 释放历史页自动补传连接事件订阅。
        History.Dispose();
        // 释放诊断页后台设备事件订阅。
        Diagnostics.Dispose();
    }
}
