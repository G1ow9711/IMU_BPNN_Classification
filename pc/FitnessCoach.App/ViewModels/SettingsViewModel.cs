// 引入领域用户偏好。
using FitnessCoach.Domain;
// 引入设置仓储接口。
using FitnessCoach.Infrastructure;
// 引入 MVVM 基础类型和异步命令。
using FitnessCoach.App.Mvvm;
// 引入应用内动画辅助功能设置。
using FitnessCoach.App.Services;
// 引入设备配置 TLV 和可选会话能力。
using FitnessCoach.Bluetooth;

// 设置页 ViewModel 位于应用命名空间。
namespace FitnessCoach.App.ViewModels;

/// <summary>编辑体重、振动、声音、亮度、熄屏时间和每日目标。</summary>
public sealed class SettingsViewModel : ObservableObject
{
    // 国际常衡磅换算常数：一千克等于 2.2046226218487757 磅。
    private const double PoundsPerKilogram = 2.2046226218487757;
    // 设置页只提供两个冻结选项，显示文本全部使用中文。
    private static readonly IReadOnlyList<MeasurementUnitOption> AvailableUnitOptions =
    [
        // 公制显示千克，卡路里继续使用千卡。
        new MeasurementUnitOption(MeasurementUnitSystem.Metric, "公制（千克/千卡）"),
        // 英制只把体重显示为磅，卡路里仍使用健身领域通用千卡。
        new MeasurementUnitOption(MeasurementUnitSystem.Imperial, "英制（磅/千卡）"),
    ];
    // 保存偏好仓储。
    private readonly IUserPreferencesStore _store;
    // 保存 Windows 系统约束与本次运行的减少动画开关。
    private readonly IAnimationPreferences _animationPreferences;
    // 保存当前设备会话，用于判断本地保存后能否立即同步。
    private readonly IDeviceSession? _deviceSession;
    // 保存可选设备配置能力；旧设计器或不支持实现保持 null。
    private readonly IDeviceConfigurationSession? _configurationSession;
    // 是否在下次启动使用 Windows WinRT 真 BLE，会话切换需要重建 GATT 和页面订阅。
    private bool _useRealBleDevice;
    // 用户体重，单位千克。
    private double _weightKilograms = 65.0;
    // 当前界面单位选项；内部体重和 BLE 协议始终保持千克。
    private MeasurementUnitOption _selectedUnitOption = AvailableUnitOptions[0];
    // 是否启用有效计次振动。
    private bool _hapticEnabled = true;
    // 是否启用提示音。
    private bool _soundEnabled;
    // AMOLED 亮度百分比。
    private int _brightnessPercent = 35;
    // 自动熄屏秒数。
    private int _screenTimeoutSeconds = 30;
    // 每日千卡目标。
    private double _dailyCalorieGoal = 300.0;
    // 是否允许诊断页显式开启诊断六轴流。
    private bool _developerModeEnabled;
    // 当前已持久化用户资料修订号。
    private uint _profileRevision;
    // 当前已持久化设备偏好修订号。
    private uint _preferencesRevision;
    // 页面提示。
    private string _statusMessage = "设置尚未保存。";

    /// <summary>创建设置页。</summary>
    public SettingsViewModel(
        IUserPreferencesStore store,
        IAnimationPreferences? animationPreferences = null,
        IDeviceSession? deviceSession = null)
    {
        // 仓储不能为空。
        ArgumentNullException.ThrowIfNull(store);
        // 保存仓储。
        _store = store;
        // 未注入时创建应用内默认设置，保持旧测试和设计器构造路径可用。
        _animationPreferences = animationPreferences ?? new AnimationPreferences(false);
        // 保存可选设备会话；设计器和旧测试可不注入。
        _deviceSession = deviceSession;
        // 仅支持配置扩展接口的会话才参与设备同步。
        _configurationSession = deviceSession as IDeviceConfigurationSession;
        // 创建保存命令。
        SaveCommand = new AsyncRelayCommand(SaveAsync);
    }

    /// <summary>保存设置命令。</summary>
    public AsyncRelayCommand SaveCommand { get; }

    /// <summary>true 表示下次启动扫描 BPNN-FIT 真机；false 表示使用 Mock。</summary>
    public bool UseRealBleDevice
    {
        // 返回下次启动模式。
        get => _useRealBleDevice;
        // 保存用户选择并通知设置页。
        set => SetProperty(ref _useRealBleDevice, value);
    }

    /// <summary>true 表示本次运行主动固定动作代表姿态并关闭循环。</summary>
    public bool ReducedMotionEnabled
    {
        // 返回用户本次运行的显式开关，不把系统强制状态伪装成用户选择。
        get => _animationPreferences.UserRequestedReducedMotion;
        // 更新共享服务，实时训练页会立即停止或恢复循环。
        set
        {
            // 相同选择不重复触发服务事件。
            if (_animationPreferences.UserRequestedReducedMotion == value)
            {
                // 保持当前设置。
                return;
            }

            // 保存本次运行的用户选择。
            _animationPreferences.UserRequestedReducedMotion = value;
            // 通知设置页复选框刷新。
            OnPropertyChanged();
            // 有效摘要可能同时受 Windows 系统设置影响。
            OnPropertyChanged(nameof(ReducedMotionSummary));
        }
    }

    /// <summary>说明减少动画的有效来源；Windows 已关闭动画时应用不能强制开启。</summary>
    public string ReducedMotionSummary => _animationPreferences.IsSystemReducedMotion
        ? "系统已关闭客户端动画：动作图固定为代表姿态。"
        : (_animationPreferences.IsReducedMotionEnabled
            ? "本次运行已减少动画：动作图固定为代表姿态。"
            : "动作图使用本地矢量循环，不访问网络。");

    /// <summary>体重规范值，单位千克，合法范围 30～250；设备命令 7 只读取该值。</summary>
    public double WeightKilograms
    {
        // 返回不受界面单位影响的规范千克值。
        get => _weightKilograms;
        // 更新规范千克值并同步刷新当前单位下的显示值。
        set
        {
            // 相同千克值不产生重复属性通知。
            if (!SetProperty(ref _weightKilograms, value))
            {
                // 数值未变化，无需刷新换算显示。
                return;
            }

            // 通知体重输入框按当前单位重新显示。
            OnPropertyChanged(nameof(WeightDisplayValue));
        }
    }

    /// <summary>设置页可选单位列表；集合在进程生命周期内只读。</summary>
    public IReadOnlyList<MeasurementUnitOption> UnitOptions => AvailableUnitOptions;

    /// <summary>当前界面单位；改变单位不会改变用户实际体重。</summary>
    public MeasurementUnitOption SelectedUnitOption
    {
        // 返回当前选中的中文单位项。
        get => _selectedUnitOption;
        // 切换显示单位并通知体重值和标签重新换算。
        set
        {
            // ComboBox 不应提供 null；仍拒绝外部错误赋值。
            ArgumentNullException.ThrowIfNull(value);
            // 只允许静态列表中的两个枚举值，避免未知单位进入协议换算。
            MeasurementUnitOption normalized = AvailableUnitOptions.Single(option => option.UnitSystem == value.UnitSystem);
            // 相同单位不重复刷新输入框。
            if (!SetProperty(ref _selectedUnitOption, normalized))
            {
                // 单位未变化，保留当前显示。
                return;
            }

            // 同一千克值需要按新单位重新显示。
            OnPropertyChanged(nameof(WeightDisplayValue));
            // 标签范围和单位文本同时变化。
            OnPropertyChanged(nameof(WeightInputLabel));
        }
    }

    /// <summary>体重输入框显示值；公制为千克，英制为磅，写入时立即换算回千克。</summary>
    public double WeightDisplayValue
    {
        // 根据当前单位返回千克或磅，内部规范值不变。
        get => SelectedUnitOption.UnitSystem == MeasurementUnitSystem.Imperial
            ? WeightKilograms * PoundsPerKilogram
            : WeightKilograms;
        // 把用户输入按当前单位换算为千克，后续验证和 BLE 下发共用规范值。
        set => WeightKilograms = SelectedUnitOption.UnitSystem == MeasurementUnitSystem.Imperial
            ? value / PoundsPerKilogram
            : value;
    }

    /// <summary>体重输入框中文标签，范围与设备协议 30～250 千克一致。</summary>
    public string WeightInputLabel => SelectedUnitOption.UnitSystem == MeasurementUnitSystem.Imperial
        ? $"体重（磅，{30.0 * PoundsPerKilogram:F1}～{250.0 * PoundsPerKilogram:F1}）"
        : "体重（千克，30～250）";

    /// <summary>是否启用每次有效 repetition 的 30ms 振动。</summary>
    public bool HapticEnabled
    {
        // 返回开关值。
        get => _hapticEnabled;
        // 更新开关值。
        set => SetProperty(ref _hapticEnabled, value);
    }

    /// <summary>是否启用提示音。</summary>
    public bool SoundEnabled
    {
        // 返回开关值。
        get => _soundEnabled;
        // 更新开关值。
        set => SetProperty(ref _soundEnabled, value);
    }

    /// <summary>AMOLED 亮度百分比，范围 5～100。</summary>
    public int BrightnessPercent
    {
        // 返回亮度。
        get => _brightnessPercent;
        // 更新亮度。
        set => SetProperty(ref _brightnessPercent, value);
    }

    /// <summary>无操作自动熄屏秒数，范围 10～300。</summary>
    public int ScreenTimeoutSeconds
    {
        // 返回熄屏秒数。
        get => _screenTimeoutSeconds;
        // 更新熄屏秒数。
        set => SetProperty(ref _screenTimeoutSeconds, value);
    }

    /// <summary>每日卡路里目标，零表示关闭目标。</summary>
    public double DailyCalorieGoal
    {
        // 返回目标。
        get => _dailyCalorieGoal;
        // 更新目标。
        set => SetProperty(ref _dailyCalorieGoal, value);
    }

    /// <summary>true 表示允许用户在诊断页手工开启 RawStream；默认关闭。</summary>
    public bool DeveloperModeEnabled
    {
        // 返回开发者模式开关。
        get => _developerModeEnabled;
        // 保存用户选择并刷新绑定。
        set => SetProperty(ref _developerModeEnabled, value);
    }

    /// <summary>设置页状态提示。</summary>
    public string StatusMessage
    {
        // 返回提示。
        get => _statusMessage;
        // 内部更新提示。
        private set => SetProperty(ref _statusMessage, value);
    }

    /// <summary>从仓储读取设置。</summary>
    public async Task LoadAsync()
    {
        try
        {
            // 读取或创建默认设置。
            UserPreferences preferences = await _store.LoadAsync().ConfigureAwait(true);
            // 复制体重。
            WeightKilograms = preferences.WeightKilograms;
            // 根据持久化枚举恢复中文单位选项；旧 JSON 缺字段时领域默认公制。
            SelectedUnitOption = AvailableUnitOptions.Single(option => option.UnitSystem == preferences.UnitSystem);
            // 复制下次启动真 BLE/Mock 模式。
            UseRealBleDevice = preferences.UseRealBleDevice;
            // 复制振动开关。
            HapticEnabled = preferences.HapticEnabled;
            // 复制声音开关。
            SoundEnabled = preferences.SoundEnabled;
            // 复制亮度。
            BrightnessPercent = preferences.BrightnessPercent;
            // 复制熄屏时间。
            ScreenTimeoutSeconds = preferences.ScreenTimeoutSeconds;
            // 复制每日目标。
            DailyCalorieGoal = preferences.DailyCalorieGoal;
            // 复制开发者模式开关。
            DeveloperModeEnabled = preferences.DeveloperModeEnabled;
            // 保存资料 revision，下一次本地保存在此基础上单调增加。
            _profileRevision = preferences.ProfileRevision;
            // 保存偏好 revision。
            _preferencesRevision = preferences.PreferencesRevision;
            // 显示加载成功提示。
            StatusMessage = "设置已从本地加载。";
            // 动画开关是本次运行设置；加载设备设置后刷新其说明。
            OnPropertyChanged(nameof(ReducedMotionEnabled));
            // 刷新系统辅助功能来源说明。
            OnPropertyChanged(nameof(ReducedMotionSummary));
        }
        catch (Exception exception)
        {
            // 显示读取或解析错误。
            StatusMessage = $"设置加载失败：{exception.Message}";
        }
    }

    // 校验并保存当前设置。
    private async Task SaveAsync()
    {
        try
        {
            // 每次成功保存形成新的资料 revision；达到 uint 上限后保持饱和。
            uint nextProfileRevision = _profileRevision == uint.MaxValue ? uint.MaxValue : _profileRevision + 1U;
            // 每次成功保存形成新的偏好 revision；达到 uint 上限后保持饱和。
            uint nextPreferencesRevision = _preferencesRevision == uint.MaxValue ? uint.MaxValue : _preferencesRevision + 1U;
            // 组装领域偏好对象；仓储会执行最终范围校验。
            UserPreferences preferences = new()
            {
                // 保存下次启动使用真 BLE 或 Mock；当前会话不热切换，避免丢失训练订阅。
                UseRealBleDevice = UseRealBleDevice,
                // 保存体重。
                WeightKilograms = WeightKilograms,
                // 保存界面单位；设备端仍只接收上面的规范千克值。
                UnitSystem = SelectedUnitOption.UnitSystem,
                // 保存振动开关。
                HapticEnabled = HapticEnabled,
                // 保存声音开关。
                SoundEnabled = SoundEnabled,
                // 保存亮度。
                BrightnessPercent = BrightnessPercent,
                // 保存熄屏秒数。
                ScreenTimeoutSeconds = ScreenTimeoutSeconds,
                // 保存每日目标。
                DailyCalorieGoal = DailyCalorieGoal,
                // 保存开发者 RawStream 总开关。
                DeveloperModeEnabled = DeveloperModeEnabled,
                // 保存本次资料修订号。
                ProfileRevision = nextProfileRevision,
                // 保存本次偏好修订号。
                PreferencesRevision = nextPreferencesRevision,
            };
            // 原子写入本地设置文件。
            await _store.SaveAsync(preferences).ConfigureAwait(true);
            // 本地文件成功后提交内存 revision，设备失败不能回退已落盘事实。
            _profileRevision = nextProfileRevision;
            // 保存偏好 revision 内存基线。
            _preferencesRevision = nextPreferencesRevision;
            // 先显示本地成功，后续设备同步失败时保留该事实。
            StatusMessage = $"本地设置已保存；下次启动使用{(UseRealBleDevice ? "真机蓝牙" : "模拟设备")}。";

            // 未注入设备或会话不支持配置时明确说明只有本地成功。
            if ((_deviceSession is null) || (_configurationSession is null))
            {
                // 不伪装设备已同步。
                StatusMessage += " 当前会话不支持设备配置同步。";
                // 本地保存流程结束。
                return;
            }

            // 未连接时配置保留到本地，用户下次连接后可再次点击保存同步。
            if (!_deviceSession.IsConnected)
            {
                // 明确设备尚未成功。
                StatusMessage += " 设备未连接，尚未同步到手柄。";
                // 不发送会失败的控制命令。
                return;
            }

            try
            {
                // 使用同一时刻生成 UTC 与时区，避免夏令时切换边界不一致。
                DateTimeOffset now = DateTimeOffset.Now;
                // 先同步设备 RTC。
                await _configurationSession.SyncTimeAsync(now.ToUnixTimeMilliseconds(), checked((short)now.Offset.TotalMinutes)).ConfigureAwait(true);
                // 把 kg 四舍五入为协议克数并进行 checked 边界保护。
                uint weightGrams = checked((uint)Math.Round(WeightKilograms * 1000.0, MidpointRounding.AwayFromZero));
                // 同步体重和资料 revision。
                await _configurationSession.SetProfileAsync(weightGrams, nextProfileRevision).ConfigureAwait(true);
                // 零卡路里表示关闭目标，否则转换为 milli-kcal。
                DeviceGoalKind goalKind = DailyCalorieGoal <= 0.0 ? DeviceGoalKind.None : DeviceGoalKind.CaloriesMilliKcal;
                // 关闭目标使用零；启用目标按千倍转换。
                uint goalValue = goalKind == DeviceGoalKind.None
                    ? 0U
                    : checked((uint)Math.Round(DailyCalorieGoal * 1000.0, MidpointRounding.AwayFromZero));
                // 同步训练目标。
                await _configurationSession.SetGoalAsync(goalKind, goalValue).ConfigureAwait(true);
                // 构造固件命令 9 偏好记录。
                DevicePreferencesV1 devicePreferences = new(
                    checked((byte)BrightnessPercent),
                    HapticEnabled,
                    SoundEnabled,
                    checked((ushort)ScreenTimeoutSeconds),
                    nextPreferencesRevision,
                    DeveloperModeEnabled);
                // 同步设备偏好并等待 ACK。
                await _configurationSession.SetPreferencesAsync(devicePreferences).ConfigureAwait(true);
                // 四条设备命令均 ACK 后才显示设备同步成功。
                StatusMessage += " 设备时间、体重、目标和偏好已同步成功。";
            }
            catch (Exception exception)
            {
                // 保留本地保存成功事实，并明确设备侧失败及恢复方式。
                StatusMessage += $" 设备同步失败：{exception.Message}；本地设置仍已保存，可连接后重试。";
            }
        }
        catch (Exception exception)
        {
            // 显示具体范围或文件错误。
            StatusMessage = $"设置保存失败：{exception.Message}";
        }
    }
}

/// <summary>表示设置页一个中文单位选项及其稳定领域枚举。</summary>
public sealed record MeasurementUnitOption(
    // UnitSystem 决定体重显示换算和 JSON 持久化值。
    MeasurementUnitSystem UnitSystem,
    // DisplayName 是 ComboBox 中显示的完整中文文本。
    string DisplayName);
