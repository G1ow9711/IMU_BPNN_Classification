// 引入领域设备会话和连接事件。
using FitnessCoach.Domain;
// 引入可观察集合，附近蓝牙设备增删会立即刷新 WPF 列表。
using System.Collections.ObjectModel;
// 引入 MVVM 基础类型和异步命令。
using FitnessCoach.App.Mvvm;
// 引入 UI 调度抽象，保证后台断连事件安全更新界面。
using FitnessCoach.App.Services;

// 设备页 ViewModel 位于应用视图模型命名空间。
namespace FitnessCoach.App.ViewModels;

/// <summary>把领域扫描快照转换为设备列表可直接绑定的中文显示行。</summary>
public sealed class NearbyBluetoothDeviceRow
{
    // 保存不可变领域设备快照；连接命令必须原样传回会话层。
    private readonly NearbyBluetoothDevice _device;

    /// <summary>创建一个附近蓝牙设备显示行。</summary>
    public NearbyBluetoothDeviceRow(NearbyBluetoothDevice device)
    {
        // 设备快照不能为空。
        ArgumentNullException.ThrowIfNull(device);
        // 保存本次扫描结果；下一次扫描会创建新行而不修改旧快照。
        _device = device;
    }

    /// <summary>领域设备快照，供连接所选设备命令使用。</summary>
    public NearbyBluetoothDevice Device => _device;

    /// <summary>Windows 系统设备唯一标识，用于诊断和精确连接。</summary>
    public string DeviceId => _device.DeviceId;

    /// <summary>广播名或用户可辨认的未命名占位符。</summary>
    public string DisplayName => string.IsNullOrWhiteSpace(_device.DisplayName)
        ? "未命名蓝牙设备"
        : _device.DisplayName;

    /// <summary>true 表示该行符合健身手柄产品广播前缀。</summary>
    public bool IsFitnessDevice => _device.IsFitnessDevice;

    /// <summary>产品类型中文文本；其它设备只展示，不允许进入健身协议连接。</summary>
    public string DeviceKindText => _device.IsFitnessDevice ? "健身手柄" : "其它蓝牙设备";

    /// <summary>Windows 当前配对状态中文文本。</summary>
    public string PairingText => _device.IsPaired ? "已配对" : "未配对";

    /// <summary>最近扫描 RSSI，单位为分贝毫瓦；缺失时明确显示未知。</summary>
    public string SignalText => _device.RssiDbm.HasValue
        ? $"{_device.RssiDbm.Value.ToString(System.Globalization.CultureInfo.InvariantCulture)} 分贝毫瓦"
        : "信号未知";
}

/// <summary>管理模拟或真机蓝牙设备连接、断线提示和权威快照恢复。</summary>
public sealed class DeviceViewModel : ObservableObject, IDisposable
{
    // 保存设备会话接口，UI 不依赖具体 Mock 类型。
    private readonly IDeviceSession _deviceSession;
    // 保存可选配对管理能力；Windows 真机和 Mock 均实现，未来不支持的平台保持 null。
    private readonly IDevicePairingSession? _pairingSession;
    // 保存可选主动发现能力；只有 Windows 真机实现会提供附近设备列表。
    private readonly IDeviceDiscoverySession? _discoverySession;
    // 保存 UI 调度器。
    private readonly IUiDispatcher _dispatcher;
    // 当前连接状态文本。
    private string _connectionStatus = "未连接";
    // 最近一条用户可见提示。
    private string _message = "连接模拟设备即可无硬件体验完整流程。";
    // 当前设备型号。
    private string _modelNumber = "未知";
    // 当前硬件修订号。
    private string _hardwareRevision = "未知";
    // 当前固件修订号。
    private string _firmwareRevision = "未知";
    // 最近权威电量文本。
    private string _batteryText = "未知";
    // 当前蓝牙最大传输单元文本。
    private string _attMtuText = "未连接";
    // 最近权威状态修订号文本。
    private string _stateRevisionText = "0";
    // 当前列表选中的附近蓝牙设备；null 表示用户尚未选择。
    private NearbyBluetoothDeviceRow? _selectedNearbyDevice;
    // true 表示 Windows 正在主动扫描广播，按钮必须暂时禁用。
    private bool _isScanning;
    // 扫描结果、进度或错误的中文摘要。
    private string _scanStatus = "点击“扫描设备”查看附近蓝牙设备。";
    // 是否已释放事件订阅。
    private bool _disposed;

    /// <summary>创建设备页并注册链路事件。</summary>
    public DeviceViewModel(IDeviceSession deviceSession, IUiDispatcher dispatcher)
    {
        // 设备会话不能为空。
        ArgumentNullException.ThrowIfNull(deviceSession);
        // 调度器不能为空。
        ArgumentNullException.ThrowIfNull(dispatcher);
        // 保存设备接口。
        _deviceSession = deviceSession;
        // 只对明确实现忘记设备合同的会话开放操作。
        _pairingSession = deviceSession as IDevicePairingSession;
        // 只对明确实现主动发现合同的会话开放 Windows 风格设备列表。
        _discoverySession = deviceSession as IDeviceDiscoverySession;
        // 保存调度器。
        _dispatcher = dispatcher;
        // 初始化空的附近设备集合；扫描完成后在 UI 线程逐项替换。
        NearbyDevices = new ObservableCollection<NearbyBluetoothDeviceRow>();
        // 创建扫描命令；Mock、扫描中或已连接时禁用。
        ScanDevicesCommand = new AsyncRelayCommand(
            ScanDevicesAsync,
            () => (_discoverySession is not null) && !IsScanning && !_deviceSession.IsConnected);
        // 创建连接所选手柄命令；只有产品行且当前未连接时允许执行。
        ConnectSelectedCommand = new AsyncRelayCommand(
            ConnectSelectedAsync,
            () => (_discoverySession is not null) && (SelectedNearbyDevice?.IsFitnessDevice == true) && !_deviceSession.IsConnected);
        // 创建连接命令；已连接时禁用。
        ConnectCommand = new AsyncRelayCommand(ConnectAsync, () => !_deviceSession.IsConnected);
        // 创建断开命令；未连接时禁用。
        DisconnectCommand = new AsyncRelayCommand(DisconnectAsync, () => _deviceSession.IsConnected);
        // 创建忘记设备命令；能力缺失的平台按钮保持禁用，不能伪装已取消配对。
        ForgetDeviceCommand = new AsyncRelayCommand(ForgetDeviceAsync, () => _pairingSession is not null);
        // 订阅设备链路事件；回调线程可能是后台线程。
        _deviceSession.ConnectionChanged += OnConnectionChanged;
        // 订阅权威状态，设备页实时更新电量和 revision；回调可能来自后台线程。
        _deviceSession.StateChanged += OnStateChanged;
        // 无主动扫描能力时显示模拟模式说明，避免用户等待不存在的 Windows 列表。
        if (_discoverySession is null)
        {
            // Mock 会话继续使用快速连接，不显示伪造附近设备。
            ScanStatus = "当前为模拟设备模式，不使用 Windows 蓝牙扫描。";
        }
        // 根据初始链路同步文本。
        UpdateConnectionState(_deviceSession.IsConnected, "初始状态");
    }

    /// <summary>主动扫描附近 BLE 设备的命令。</summary>
    public AsyncRelayCommand ScanDevicesCommand { get; }

    /// <summary>连接列表中明确选中的健身手柄命令。</summary>
    public AsyncRelayCommand ConnectSelectedCommand { get; }

    /// <summary>连接命令。</summary>
    public AsyncRelayCommand ConnectCommand { get; }

    /// <summary>断开命令。</summary>
    public AsyncRelayCommand DisconnectCommand { get; }

    /// <summary>取消 Windows 配对并清除应用固定设备状态的命令。</summary>
    public AsyncRelayCommand ForgetDeviceCommand { get; }

    /// <summary>当前扫描显示的附近蓝牙设备行。</summary>
    public ObservableCollection<NearbyBluetoothDeviceRow> NearbyDevices { get; }

    /// <summary>列表当前选中行；选中变化会立即刷新“连接所选手柄”按钮。</summary>
    public NearbyBluetoothDeviceRow? SelectedNearbyDevice
    {
        // 返回当前选中设备行。
        get => _selectedNearbyDevice;
        // 保存选中行并刷新连接命令。
        set
        {
            // 相同对象无需重复通知 WPF。
            if (!SetProperty(ref _selectedNearbyDevice, value))
            {
                // 选中项未变化，命令状态也不变。
                return;
            }

            // 新选中项可能从其它设备变为健身手柄，重新查询命令可执行性。
            ConnectSelectedCommand.RaiseCanExecuteChanged();
        }
    }

    /// <summary>true 表示主动广播扫描尚未结束。</summary>
    public bool IsScanning
    {
        // 返回扫描状态。
        get => _isScanning;
        // 内部更新扫描状态和按钮可执行性。
        private set
        {
            // 状态未变化时不重复刷新命令。
            if (!SetProperty(ref _isScanning, value))
            {
                // 保持当前按钮状态。
                return;
            }

            // 扫描开始或结束都需要重查扫描按钮。
            ScanDevicesCommand.RaiseCanExecuteChanged();
        }
    }

    /// <summary>用户可见扫描进度与结果摘要。</summary>
    public string ScanStatus
    {
        // 返回扫描摘要。
        get => _scanStatus;
        // 内部写入中文扫描状态。
        private set => SetProperty(ref _scanStatus, value);
    }

    /// <summary>设备唯一标识。</summary>
    public string DeviceId => _deviceSession.DeviceId;

    /// <summary>设备页使用的中文标识；仅本地化内部占位值，真实硬件标识保持原样。</summary>
    public string DeviceIdText => DisplayText.DeviceIdentifier(_deviceSession.DeviceId);

    /// <summary>当前实现模式；真机与模拟设备必须在界面明确区分。</summary>
    public string DeviceMode => _deviceSession.IsHardwareBacked ? "真机蓝牙设备" : "模拟设备模式";

    /// <summary>标准 Device Information 型号。</summary>
    public string ModelNumber
    {
        // 返回设备型号。
        get => _modelNumber;
        // 内部应用诊断快照。
        private set => SetProperty(ref _modelNumber, value);
    }

    /// <summary>板卡硬件修订号。</summary>
    public string HardwareRevision
    {
        // 返回硬件修订号。
        get => _hardwareRevision;
        // 内部应用诊断快照。
        private set => SetProperty(ref _hardwareRevision, value);
    }

    /// <summary>设备固件语义版本。</summary>
    public string FirmwareRevision
    {
        // 返回固件修订号。
        get => _firmwareRevision;
        // 内部应用诊断快照。
        private set => SetProperty(ref _firmwareRevision, value);
    }

    /// <summary>最近设备电量，例如 96% 或未知。</summary>
    public string BatteryText
    {
        // 返回电量文本。
        get => _batteryText;
        // 内部应用 LiveState 或诊断快照。
        private set => SetProperty(ref _batteryText, value);
    }

    /// <summary>当前蓝牙最大传输单元，单位字节。</summary>
    public string AttMtuText
    {
        // 返回 MTU 文本。
        get => _attMtuText;
        // 内部应用诊断快照。
        private set => SetProperty(ref _attMtuText, value);
    }

    /// <summary>最近接受的权威状态修订号。</summary>
    public string StateRevisionText
    {
        // 返回 revision 文本。
        get => _stateRevisionText;
        // 内部应用权威状态。
        private set => SetProperty(ref _stateRevisionText, value);
    }

    /// <summary>当前连接状态文本。</summary>
    public string ConnectionStatus
    {
        // 返回绑定字段。
        get => _connectionStatus;
        // 内部更新并通知 WPF。
        private set => SetProperty(ref _connectionStatus, value);
    }

    /// <summary>当前提示或错误消息。</summary>
    public string Message
    {
        // 返回绑定字段。
        get => _message;
        // 内部更新并通知 WPF。
        private set => SetProperty(ref _message, value);
    }

    /// <summary>原配 400 毫安时电池与低电阈值固定产品说明。</summary>
    public string PowerContract => "原配电池容量 400 毫安时；15% 告警，8% 禁止新会话，5% 保存并关机。";

    // 主动扫描附近设备并替换当前可见列表。
    private async Task ScanDevicesAsync()
    {
        // 没有发现能力时给出明确说明；命令通常已禁用，此分支防御代码直接调用。
        if (_discoverySession is null)
        {
            // 不创建伪造设备列表。
            ScanStatus = "当前设备会话不支持 Windows 蓝牙扫描。";
            // 结束本次操作。
            return;
        }

        // 标记扫描进行中，防止用户重复启动多个 watcher。
        IsScanning = true;
        // 显示固定六秒主动扫描提示。
        ScanStatus = "正在扫描附近蓝牙设备，请保持手柄开机并靠近电脑……";
        // 清除旧选中项，避免用户连接已经离开范围的旧快照。
        SelectedNearbyDevice = null;
        // 清空旧列表，界面立即反映本轮扫描已开始。
        NearbyDevices.Clear();

        try
        {
            // 请求真机会话执行六秒主动广播与系统缓存合并。
            IReadOnlyList<NearbyBluetoothDevice> devices = await _discoverySession.ScanDevicesAsync(
                TimeSpan.FromSeconds(6)).ConfigureAwait(true);
            // 把每个领域快照转换为中文显示行并加入 UI 集合。
            foreach (NearbyBluetoothDevice device in devices)
            {
                // 新建独立行，后续重扫不会修改当前对象。
                NearbyDevices.Add(new NearbyBluetoothDeviceRow(device));
            }

            // 优先选中第一台健身手柄；没有产品时选中首台其它设备供用户查看信息。
            SelectedNearbyDevice = NearbyDevices.FirstOrDefault(row => row.IsFitnessDevice)
                ?? NearbyDevices.FirstOrDefault();
            // 统计符合产品前缀的手柄数量。
            int fitnessDeviceCount = NearbyDevices.Count(row => row.IsFitnessDevice);
            // 列表为空时给出靠近、开机和重新扫描建议。
            ScanStatus = NearbyDevices.Count == 0
                ? "未发现附近蓝牙设备；请检查 Windows 蓝牙开关和手柄电源后重试。"
                : $"发现 {NearbyDevices.Count} 台蓝牙设备，其中健身手柄 {fitnessDeviceCount} 台。";
        }
        catch (Exception exception)
        {
            // 把 WinRT 权限、适配器关闭或取消错误转换为页面消息。
            ScanStatus = $"扫描失败：{exception.Message}";
        }
        finally
        {
            // 无论成功或失败都退出扫描状态。
            IsScanning = false;
            // 刷新所有设备页命令。
            RefreshCommands();
        }
    }

    // 建立默认或上次设备链路并读取权威快照。
    private Task ConnectAsync()
    {
        // 复用统一连接完成逻辑；设备会话负责自动选择产品或上次设备。
        return CompleteConnectionAsync(
            () => _deviceSession.ConnectAsync(),
            "自动选择的健身手柄");
    }

    // 连接列表中用户明确选中的产品设备。
    private Task ConnectSelectedAsync()
    {
        // 没有发现能力或选中项时防御性拒绝；命令正常情况下已禁用。
        if ((_discoverySession is null) || (SelectedNearbyDevice is null))
        {
            // 显示恢复动作。
            Message = "请先扫描并选择一台健身手柄。";
            // 返回已完成任务，不启动隐藏扫描。
            return Task.CompletedTask;
        }

        // 保存本次点击时的不可变扫描快照，避免等待期间列表选择变化。
        NearbyBluetoothDevice selectedDevice = SelectedNearbyDevice.Device;
        // 复用统一连接完成逻辑，并让会话锁定精确 Windows 设备 ID。
        return CompleteConnectionAsync(
            () => _discoverySession.ConnectToDeviceAsync(selectedDevice),
            SelectedNearbyDevice.DisplayName);
    }

    // 执行一种连接入口，并统一恢复快照、诊断字段、消息和按钮。
    private async Task CompleteConnectionAsync(Func<Task> connectOperation, string targetName)
    {
        try
        {
            // 执行默认连接或按选中 ID 连接；底层仍只有一套 GATT/Manifest 状态机。
            await connectOperation().ConfigureAwait(true);
            // 读取快照验证状态恢复和 LiveState 特征可用。
            LiveState snapshot = await _deviceSession.GetSnapshotAsync().ConfigureAwait(true);
            // 应用快照电量和状态修订号。
            ApplyLiveState(snapshot);
            // 刷新设备标准信息和最大传输单元。
            RefreshDeviceInformation();
            // 显示目标名称和恢复后的设备状态修订号。
            Message = $"已连接 {targetName}，状态修订 {snapshot.StateRevision}。";
        }
        catch (Exception exception)
        {
            // 把设备异常转换为用户可读提示，不让 UI 崩溃。
            Message = $"连接失败：{exception.Message}";
        }
        finally
        {
            // 刷新连接、扫描和断开命令的可执行状态。
            RefreshCommands();
        }
    }

    // 主动断开 PC 链路；设备内部会话按合同继续运行。
    private async Task DisconnectAsync()
    {
        try
        {
            // 请求设备会话断开链路。
            await _deviceSession.DisconnectAsync().ConfigureAwait(true);
            // 明确说明断线不自动停止设备训练。
            Message = "上位机链路已断开；设备上的进行中会话不会自动停止。";
        }
        catch (Exception exception)
        {
            // 显示断开失败原因。
            Message = $"断开失败：{exception.Message}";
        }
        finally
        {
            // 刷新按钮状态。
            RefreshCommands();
        }
    }

    // 忘记设备会同时断开链路、取消系统配对并清除会话首选设备 ID。
    private async Task ForgetDeviceAsync()
    {
        // 没有配对管理能力时给出明确说明，避免按钮调用静默无效。
        if (_pairingSession is null)
        {
            // 用户可切换到受支持的 Windows 真机或 Mock 会话。
            Message = "当前设备会话不支持忘记设备。";
            // 无能力时不执行断开或清理。
            return;
        }

        try
        {
            // 会话层负责先断开、取消系统配对，再清除固定设备缓存。
            await _pairingSession.ForgetDeviceAsync().ConfigureAwait(true);
            // 真机提示明确包含 Windows 配对记录；Mock 只声明模拟和本地状态。
            Message = _deviceSession.IsHardwareBacked
                ? "已忘记设备；Windows 配对记录和本地固定设备状态已清除。下次连接将重新选择并配对。"
                : "已忘记模拟设备；本地固定设备状态已清除。下次连接将按首次连接处理。";
            // 会话设备标识已经回到未选择或模拟默认值，通知界面刷新。
            OnPropertyChanged(nameof(DeviceId));
            // 同步刷新用户可见中文设备标识。
            OnPropertyChanged(nameof(DeviceIdText));
            // 清除旧设备型号、电量和最大传输单元显示。
            RefreshDeviceInformation();
        }
        catch (Exception exception)
        {
            // 取消配对失败时保留具体系统状态和可重试事实。
            Message = $"忘记设备失败：{exception.Message}；请检查 Windows 蓝牙权限后重试。";
        }
        finally
        {
            // 刷新连接、断开和忘记按钮状态。
            RefreshCommands();
        }
    }

    // 接收后台连接变化并切回 UI 线程。
    private void OnConnectionChanged(object? sender, DeviceConnectionChangedEventArgs eventArgs)
    {
        // 丢弃任务返回值；调度器内部负责串行化，更新逻辑不抛异常。
        _ = _dispatcher.InvokeAsync(() => UpdateConnectionState(eventArgs.IsConnected, eventArgs.Reason));
    }

    // 接收后台权威状态并切回 UI 线程。
    private void OnStateChanged(object? sender, LiveState state)
    {
        // 调度到 WPF UI 线程，避免跨线程属性通知异常。
        _ = _dispatcher.InvokeAsync(() => ApplyLiveState(state));
    }

    // 更新连接状态、提示和命令。
    private void UpdateConnectionState(bool connected, string reason)
    {
        // 连接时显示绿色语义文本，断开时显示离线。
        ConnectionStatus = connected ? "已连接" : "未连接";
        // 保存设备提供的状态原因。
        Message = reason;
        // 设备名称可能在首次连接后从占位符变为真实名称。
        OnPropertyChanged(nameof(DeviceId));
        // 设备页绑定中文显示属性，必须和内部标识同一时刻刷新。
        OnPropertyChanged(nameof(DeviceIdText));
        // 连接或断开都刷新设备信息；断开保留最后版本但最大传输单元显示未连接。
        RefreshDeviceInformation();
        // 通知 WPF 重新查询按钮状态。
        RefreshCommands();
    }

    // 应用权威 LiveState 中的电量和 revision。
    private void ApplyLiveState(LiveState state)
    {
        // 255 表示设备暂未提供有效电量。
        BatteryText = state.BatteryPercent <= 100 ? $"{state.BatteryPercent}%" : "未知";
        // 使用固定十进制格式显示状态修订号。
        StateRevisionText = state.StateRevision.ToString(System.Globalization.CultureInfo.InvariantCulture);
    }

    // 从可选诊断接口读取设备身份和链路信息。
    private void RefreshDeviceInformation()
    {
        // 未实现诊断接口的未来会话仍可使用基本连接功能。
        if (_deviceSession is not IDeviceDiagnosticsSource diagnosticsSource)
        {
            // 保持未知占位符。
            return;
        }

        // 获取同步线程安全快照，不产生额外 GATT I/O。
        DeviceDiagnosticsSnapshot snapshot = diagnosticsSource.GetDiagnosticsSnapshot();
        // 更新型号。
        ModelNumber = snapshot.ModelNumber;
        // 更新硬件修订号。
        HardwareRevision = snapshot.HardwareRevision;
        // 更新固件修订号。
        FirmwareRevision = snapshot.FirmwareRevision;
        // 连接时显示协商最大传输单元，断开时显示未连接。
        AttMtuText = snapshot.AttMtu.HasValue ? $"{snapshot.AttMtu.Value} 字节" : "未连接";
        // 快照存在有效电量时同步设备页。
        if (snapshot.BatteryPercent.HasValue)
        {
            // 格式化百分比。
            BatteryText = $"{snapshot.BatteryPercent.Value}%";
        }

        // 同步最近状态修订号。
        StateRevisionText = snapshot.StateRevision.ToString(System.Globalization.CultureInfo.InvariantCulture);
    }

    // 统一刷新连接/断开按钮。
    private void RefreshCommands()
    {
        // 重新查询扫描命令。
        ScanDevicesCommand.RaiseCanExecuteChanged();
        // 重新查询连接所选设备命令。
        ConnectSelectedCommand.RaiseCanExecuteChanged();
        // 重新查询连接命令。
        ConnectCommand.RaiseCanExecuteChanged();
        // 重新查询断开命令。
        DisconnectCommand.RaiseCanExecuteChanged();
        // 重新查询忘记命令；异步执行期间命令自身也会阻止重复点击。
        ForgetDeviceCommand.RaiseCanExecuteChanged();
    }

    /// <inheritdoc />
    public void Dispose()
    {
        // 重复释放不重复退订。
        if (_disposed)
        {
            // 已完成释放。
            return;
        }

        // 标记已释放。
        _disposed = true;
        // 移除连接事件，防止页面生命周期结束后继续更新。
        _deviceSession.ConnectionChanged -= OnConnectionChanged;
        // 移除状态事件，防止页面释放后后台通知更新属性。
        _deviceSession.StateChanged -= OnStateChanged;
    }
}
