// 引入 WPF 应用和启动参数。
using System.Windows;
// 引入 Mock 和可测试真 BLE 会话状态机。
using FitnessCoach.Bluetooth;
// 引入 Windows.Devices.Bluetooth WinRT transport。
using FitnessCoach.Bluetooth.Windows;
// 引入设备会话接口。
using FitnessCoach.Domain;
// 引入本地仓储实现。
using FitnessCoach.Infrastructure;
// 引入应用服务。
using FitnessCoach.App.Services;
// 引入页面 ViewModel。
using FitnessCoach.App.ViewModels;

// 应用入口位于根命名空间。
namespace FitnessCoach.App;

/// <summary>创建本地依赖、启动主窗口，并在退出时释放后台 Mock 时钟。</summary>
public partial class App : Application
{
    // 保存设备会话，退出时停止后台任务。
    private IDeviceSession? _deviceSession;
    // 保存会话仓储，退出时释放锁。
    private JsonSessionRepository? _sessionRepository;
    // 保存设置仓储，退出时释放锁。
    private JsonUserPreferencesStore? _preferencesStore;
    // 保存主 ViewModel，退出时解除事件订阅。
    private MainViewModel? _mainViewModel;

    /// <inheritdoc />
    protected override async void OnStartup(StartupEventArgs eventArgs)
    {
        // 先调用 WPF 基类启动逻辑。
        base.OnStartup(eventArgs);

        try
        {
            // 生成当前用户隔离的 LocalApplicationData 路径。
            LocalAppDataPaths paths = LocalAppDataPaths.CreateDefault();
            // 创建历史、日志和设置目录。
            paths.EnsureDirectories();
            // 创建线程安全 JSON 会话仓储；接口可换 SQLite 适配器。
            _sessionRepository = new JsonSessionRepository(paths.SessionsFile);
            // 初始化模式 v1 文件。
            await _sessionRepository.InitializeAsync().ConfigureAwait(true);
            // 创建用户设置仓储。
            _preferencesStore = new JsonUserPreferencesStore(paths.PreferencesFile);
            // 启动前读取设备模式；真 BLE/Mock 切换需要重建整套 GATT 和页面订阅，因此下次启动生效。
            UserPreferences startupPreferences = await _preferencesStore.LoadAsync().ConfigureAwait(true);
            // 真机模式创建 WinRT transport 和完整会话状态机；默认 Mock 保证无硬件仍可体验。
            _deviceSession = startupPreferences.UseRealBleDevice
                ? new WindowsBleDeviceSession(new WinRtBleTransport(new FirstFitnessDeviceSelector()))
                : new MockDeviceSession();
            // 创建生产 WPF UI 调度器。
            IUiDispatcher dispatcher = new WpfUiDispatcher();
            // 读取 Windows“在 Windows 中显示动画”辅助功能；关闭时应用不得自行恢复循环。
            IAnimationPreferences animationPreferences = new AnimationPreferences(!SystemParameters.ClientAreaAnimation);
            // 创建本地动作视觉控制器。
            IActionAnimationController animationController = new LocalActionAnimationController();
            // 创建设备页。
            DeviceViewModel device = new(_deviceSession, dispatcher);
            // 创建实时训练页。
            LiveTrainingViewModel live = new(_deviceSession, _sessionRepository, dispatcher, animationController, animationPreferences);
            // 创建总结页。
            SummaryViewModel summary = new();
            // 创建 CSV 导出器，使用带 BOM UTF-8 和 RFC4180 转义。
            IHistoryCsvExporter historyCsvExporter = new HistoryCsvExporter();
            // 创建 WPF 保存位置选择器，用户明确确认后才写文件。
            IHistoryExportDestinationPicker historyDestinationPicker = new WpfHistoryExportDestinationPicker();
            // 创建含设备补传、断线重连同步、日期/动作筛选、详情和 CSV 导出的历史页。
            HistoryViewModel history = new(_sessionRepository, historyCsvExporter, historyDestinationPicker, _deviceSession, dispatcher);
            // 创建设置页并注入同一设备会话；本地保存成功后才按连接状态同步时间、资料、目标和偏好。
            SettingsViewModel settings = new(_preferencesStore, animationPreferences, _deviceSession);
            // 创建诊断页。
            DiagnosticsViewModel diagnostics = new(_deviceSession, dispatcher);
            // 组合主窗口 ViewModel。
            _mainViewModel = new MainViewModel(device, live, summary, history, settings, diagnostics);
            // 读取设置和历史。
            await _mainViewModel.InitializeAsync().ConfigureAwait(true);
            // 创建主窗口并设置绑定上下文。
            MainWindow window = new() { DataContext = _mainViewModel };
            // 保存 WPF 主窗口引用。
            MainWindow = window;
            // 显示主窗口。
            window.Show();
        }
        catch (Exception exception)
        {
            // 启动失败时展示明确错误，避免黑屏退出。
            MessageBox.Show($"上位机启动失败：{exception.Message}", "手腕健身动作助手", MessageBoxButton.OK, MessageBoxImage.Error);
            // 使用非零退出码结束应用。
            Shutdown(1);
        }
    }

    /// <inheritdoc />
    protected override void OnExit(ExitEventArgs eventArgs)
    {
        // 解除全部页面事件订阅。
        _mainViewModel?.Dispose();
        // 异步设备释放在同步退出钩子中安全等待，确保后台 Mock 任务结束。
        _deviceSession?.DisposeAsync().AsTask().GetAwaiter().GetResult();
        // 释放会话仓储信号量。
        _sessionRepository?.Dispose();
        // 释放设置仓储信号量。
        _preferencesStore?.Dispose();
        // 调用 WPF 基类退出逻辑。
        base.OnExit(eventArgs);
    }
}
