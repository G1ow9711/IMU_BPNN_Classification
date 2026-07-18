// 引入设置与总结页面视图模型，验证新增中文交互合同。
using FitnessCoach.App.ViewModels;
// 引入同步界面调度器，让模拟设备事件立即更新设备页。
using FitnessCoach.App.Services;
// 引入模拟设备会话，验证忘记设备无需真实蓝牙硬件。
using FitnessCoach.Bluetooth;
// 引入领域单位枚举、偏好和会话摘要。
using FitnessCoach.Domain;
// 引入原子 JSON 偏好仓储，验证重启后的单位选择不会丢失。
using FitnessCoach.Infrastructure;

// PC 界面补全测试位于现有离线测试程序集，不依赖窗口或真实蓝牙。
namespace FitnessCoach.Tests;

/// <summary>验证单位换算、单位持久化、目标完成度和设备/本地保存状态。</summary>
internal static class PcUiCompletionContractTests
{
    // 千克到磅使用国际常衡磅精确换算常数。
    private const double PoundsPerKilogram = 2.2046226218487757;

    /// <summary>顺序运行设置页和总结页两组纯 PC 合同测试。</summary>
    public static async Task RunAllAsync()
    {
        // 先确认两个页面确实把新增中文合同接到可见控件，避免只改视图模型却漏改 XAML。
        TestChineseXamlBindingsExist();
        // 先验证英制只是显示层换算，设备协议仍接收千克。
        await TestImperialDisplayPersistsCanonicalKilogramsAsync();
        // 验证中文忘记设备按钮会断开链路并清除固定设备状态。
        await TestForgetMockDeviceAsync();
        // 再验证总结页明确显示目标进度和两处保存结果。
        TestSummaryShowsGoalAndPersistenceStatus();
    }

    // 使用临时偏好文件验证英制显示、公斤内存值和 JSON 重载。
    private static async Task TestImperialDisplayPersistsCanonicalKilogramsAsync()
    {
        // 定位仓库根目录，让测试运行文件始终写入工程本地运行区。
        string repositoryRoot = FindRepositoryRoot();
        // 为本测试创建唯一工程本地临时目录，避免覆盖真实用户设置或写入系统临时目录。
        string directory = Path.Combine(repositoryRoot, ".codex-local", "tmp", $"fitness-unit-{Guid.NewGuid():N}");
        // 在测试目录内保存偏好 JSON。
        string preferencesPath = Path.Combine(directory, "preferences.v1.json");

        try
        {
            // 创建工程本地临时目录，准备一个不含 unitSystem 的旧版本 JSON。
            Directory.CreateDirectory(directory);
            // 写入旧设置最小样本，验证新增字段不会破坏既有用户文件。
            await File.WriteAllTextAsync(preferencesPath, "{\"weightKilograms\":65.0}");
            // 创建真实原子 JSON 仓储。
            using JsonUserPreferencesStore store = new(preferencesPath);
            // 创建不连接设备的设置页；本测试只验证本地单位合同。
            SettingsViewModel viewModel = new(store);
            // 加载默认公制设置。
            await viewModel.LoadAsync();
            // 旧文件缺少单位字段时必须使用领域默认公制。
            Assert(viewModel.SelectedUnitOption.UnitSystem == MeasurementUnitSystem.Metric, "默认单位不是公制。" );
            // 从固定选项中取得英制项，禁止测试自行创建界面不存在的选项。
            MeasurementUnitOption imperial = viewModel.UnitOptions.Single(option => option.UnitSystem == MeasurementUnitSystem.Imperial);
            // 用户切换英制后，体重输入和显示单位应改为磅。
            viewModel.SelectedUnitOption = imperial;
            // 输入约 220.462 磅，对应协议中的 100 千克。
            viewModel.WeightDisplayValue = 100.0 * PoundsPerKilogram;
            // 确认内部规范值仍为千克，设备命令不得收到磅数。
            AssertNear(viewModel.WeightKilograms, 100.0, 1e-9, "英制输入没有换算为协议千克。" );
            // 保存设置，仓储必须同时持久化单位和规范千克值。
            await viewModel.SaveCommand.ExecuteAsync();
            // 直接重载领域偏好，验证 JSON 往返。
            UserPreferences reloaded = await store.LoadAsync();
            // 单位系统必须保持英制。
            Assert(reloaded.UnitSystem == MeasurementUnitSystem.Imperial, "英制单位选择未持久化。" );
            // 持久化体重必须仍是千克。
            AssertNear(reloaded.WeightKilograms, 100.0, 1e-9, "持久化体重不是规范千克。" );

            // 创建新视图模型模拟应用重启。
            SettingsViewModel restarted = new(store);
            // 从同一 JSON 重新加载。
            await restarted.LoadAsync();
            // 页面应恢复英制选项。
            Assert(restarted.SelectedUnitOption.UnitSystem == MeasurementUnitSystem.Imperial, "重启后没有恢复英制界面。" );
            // 页面显示值应恢复约 220.462 磅。
            AssertNear(restarted.WeightDisplayValue, 100.0 * PoundsPerKilogram, 1e-9, "重启后的磅显示值错误。" );
            // 协议规范值在重启后仍必须是 100 千克。
            AssertNear(restarted.WeightKilograms, 100.0, 1e-9, "重启后协议千克值漂移。" );
        }
        finally
        {
            // 测试完成后删除临时目录，不留下运行时文件。
            if (Directory.Exists(directory))
            {
                // 递归删除仅由本测试创建的唯一目录。
                Directory.Delete(directory, recursive: true);
            }
        }
    }

    // 读取设置页和总结页 XAML，验证新增状态真正出现在中文界面。
    private static void TestChineseXamlBindingsExist()
    {
        // 定位包含 pc 子工程的仓库根目录。
        string repositoryRoot = FindRepositoryRoot();
        // 拼出设置页 XAML 的绝对路径。
        string settingsViewPath = Path.Combine(repositoryRoot, "pc", "FitnessCoach.App", "Views", "SettingsView.xaml");
        // 拼出总结页 XAML 的绝对路径。
        string summaryViewPath = Path.Combine(repositoryRoot, "pc", "FitnessCoach.App", "Views", "SummaryView.xaml");
        // 读取设置页 UTF-8 文本，检查单位选择和规范显示值绑定。
        string settingsXaml = File.ReadAllText(settingsViewPath);
        // 读取总结页 UTF-8 文本，检查目标进度和持久化状态绑定。
        string summaryXaml = File.ReadAllText(summaryViewPath);
        // 单位下拉框必须绑定固定中文单位选项。
        Assert(settingsXaml.Contains("SelectedUnitOption", StringComparison.Ordinal)
            && settingsXaml.Contains("DisplayMemberPath=\"DisplayName\"", StringComparison.Ordinal), "设置页缺少中文计量单位选择绑定。");
        // 体重输入必须使用随单位变化的显示值，不能继续直接编辑协议千克字段。
        Assert(settingsXaml.Contains("WeightDisplayValue", StringComparison.Ordinal)
            && settingsXaml.Contains("WeightInputLabel", StringComparison.Ordinal), "设置页缺少体重单位换算绑定。");
        // 总结页必须显示目标进度以及设备、本地两处保存状态。
        Assert(summaryXaml.Contains("GoalProgressText", StringComparison.Ordinal)
            && summaryXaml.Contains("DeviceSaveStatus", StringComparison.Ordinal)
            && summaryXaml.Contains("LocalSaveStatus", StringComparison.Ordinal), "总结页缺少目标或保存状态绑定。");
        // 设备页必须提供中文忘记按钮，并绑定独立异步命令。
        string deviceViewPath = Path.Combine(repositoryRoot, "pc", "FitnessCoach.App", "Views", "DeviceView.xaml");
        // 读取设备页 UTF-8 文本，验证按钮没有停留在视图模型内部。
        string deviceXaml = File.ReadAllText(deviceViewPath);
        // 按钮文案和命令必须同时存在。
        Assert(deviceXaml.Contains("Content=\"忘记设备\"", StringComparison.Ordinal)
            && deviceXaml.Contains("ForgetDeviceCommand", StringComparison.Ordinal), "设备页缺少中文忘记设备按钮。");
        // 设备页必须提供 Windows 风格的可见扫描列表，不能继续隐藏候选设备。
        Assert(deviceXaml.Contains("附近蓝牙设备", StringComparison.Ordinal)
            && deviceXaml.Contains("NearbyDevices", StringComparison.Ordinal)
            && deviceXaml.Contains("SelectedNearbyDevice", StringComparison.Ordinal), "设备页缺少附近蓝牙设备列表或选中项绑定。");
        // 用户必须能刷新候选列表并连接明确选中的手柄。
        Assert(deviceXaml.Contains("ScanDevicesCommand", StringComparison.Ordinal)
            && deviceXaml.Contains("ConnectSelectedCommand", StringComparison.Ordinal)
            && deviceXaml.Contains("连接所选手柄", StringComparison.Ordinal), "设备页缺少扫描或连接所选手柄命令。");
    }

    // 使用 Mock 验证忘记设备命令可测试、可断开，并清除固定设备重连痕迹。
    private static async Task TestForgetMockDeviceAsync()
    {
        // 创建短周期模拟设备，测试不依赖 Windows 蓝牙和真实开发板。
        await using MockDeviceSession device = new(TimeSpan.FromMilliseconds(5), [ActionId.Squat]);
        // 同步调度器让连接变化事件直接写入视图模型。
        ImmediateUiDispatcher dispatcher = new();
        // 创建设备页并注册链路事件。
        using DeviceViewModel viewModel = new(device, dispatcher);
        // 先连接一次，形成需要清除的固定设备状态。
        await viewModel.ConnectCommand.ExecuteAsync();
        // 前置条件必须真正连接成功。
        Assert(device.IsConnected, "模拟设备未建立忘记操作的前置连接。");
        // 从中文设备页执行忘记命令，而不是绕过界面直接调用会话。
        await viewModel.ForgetDeviceCommand.ExecuteAsync();
        // 忘记后链路必须断开，防止仍使用旧 GATT 会话。
        Assert(!device.IsConnected, "忘记设备后模拟链路仍处于连接状态。");
        // 中文提示必须说明本地固定状态已清除和下次行为。
        Assert(viewModel.Message.Contains("已忘记", StringComparison.Ordinal)
            && viewModel.Message.Contains("本地固定设备状态已清除", StringComparison.Ordinal), "忘记设备中文结果不完整。");
        // 再次连接应视为新的首次选择，而不是旧设备自动重连。
        await viewModel.ConnectCommand.ExecuteAsync();
        // 读取 Mock 诊断快照，确认重连计数没有沿用被忘记设备的历史。
        DeviceDiagnosticsSnapshot snapshot = device.GetDiagnosticsSnapshot();
        // 清除固定状态后首次重连计数必须为零。
        Assert(snapshot.ReconnectCount == 0U, "忘记设备后仍保留旧设备重连状态。");
    }

    // 从测试可执行文件目录逐级向上查找包含 PC 工程的仓库根目录。
    private static string FindRepositoryRoot()
    {
        // 从当前测试程序集所在目录开始，兼容默认 bin 和项目本地独立输出目录。
        DirectoryInfo? current = new(AppContext.BaseDirectory);
        // 每轮检查当前目录，找到后返回；到磁盘根目录时停止。
        while (current is not null)
        {
            // 以设置页文件作为仓库结构标志，避免依赖当前工作目录。
            string marker = Path.Combine(current.FullName, "pc", "FitnessCoach.App", "Views", "SettingsView.xaml");
            // 标志文件存在表示当前目录就是仓库根目录。
            if (File.Exists(marker))
            {
                // 返回已确认的绝对仓库根目录。
                return current.FullName;
            }

            // 继续检查父目录。
            current = current.Parent;
        }

        // 找不到仓库根目录时提供明确错误，避免后续报模糊的文件不存在。
        throw new DirectoryNotFoundException("无法从测试输出目录定位包含 PC 界面的仓库根目录。");
    }

    // 构造固定 125 千卡会话，验证 250 千卡目标显示 50%。
    private static void TestSummaryShowsGoalAndPersistenceStatus()
    {
        // 使用 UTC 固定时间，避免本地时区影响摘要构造。
        DateTimeOffset startedAt = new(2026, 7, 15, 1, 0, 0, TimeSpan.Zero);
        // 创建 10 分钟、125 千卡的设备权威摘要。
        TrainingSessionSummary summary = new(
            "DEVICE-UI",
            9U,
            startedAt,
            startedAt.AddMinutes(10),
            600_000U,
            125_000U,
            "用户停止",
            []);
        // 创建空总结页。
        SummaryViewModel viewModel = new();
        // 事件只在设备停止 ACK 和本地 JSON 保存后触发，因此两项状态都传 true。
        viewModel.SetSummary(summary, 250.0, devicePersisted: true, localPersisted: true);
        // 125/250 必须显示为 50%。
        AssertNear(viewModel.GoalProgressPercent, 50.0, 1e-9, "目标完成百分比错误。" );
        // 中文进度文本必须同时包含当前消耗、目标和百分比。
        Assert(viewModel.GoalProgressText.Contains("125.00 / 250.00 千卡", StringComparison.Ordinal)
            && viewModel.GoalProgressText.Contains("50.0%", StringComparison.Ordinal), "目标完成度中文文本不完整。" );
        // 设备侧持久化成功必须明确显示。
        Assert(viewModel.DeviceSaveStatus == "设备会话已保存", "设备保存状态错误。" );
        // 本地历史持久化成功必须明确显示。
        Assert(viewModel.LocalSaveStatus == "本地历史已保存", "本地保存状态错误。" );

        // 零目标表示用户关闭目标，不允许显示除零或伪百分比。
        viewModel.SetSummary(summary, 0.0, devicePersisted: true, localPersisted: true);
        // 无目标时百分比回到零，文本给出明确恢复方式。
        Assert(viewModel.GoalProgressPercent == 0.0
            && viewModel.GoalProgressText == "未设置每日卡路里目标", "关闭目标时的总结文本错误。" );
    }

    // 断言两个双精度值在给定绝对误差内相等。
    private static void AssertNear(double actual, double expected, double tolerance, string message)
    {
        // 非有限值或超差均表示换算失败。
        if (!double.IsFinite(actual) || (Math.Abs(actual - expected) > tolerance))
        {
            // 抛出包含实际值的错误，便于定位换算或序列化漂移。
            throw new InvalidOperationException($"{message} 实际={actual:R}，期望={expected:R}。" );
        }
    }

    // 统一布尔断言，失败时抛出稳定中文异常。
    private static void Assert(bool condition, string message)
    {
        // 条件成立时测试继续。
        if (condition)
        {
            // 成功路径不创建异常。
            return;
        }

        // 条件不成立时让测试进程返回非零。
        throw new InvalidOperationException(message);
    }
}
