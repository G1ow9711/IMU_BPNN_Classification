// 引入 UTF-8 解码，读取标准 Device Information 字符串。
using System.Text;
// 引入线程安全字典，汇总 WinRT 广播回调中的地址、名称和 RSSI。
using System.Collections.Concurrent;
// 引入 WinRT BluetoothLEDevice、连接状态和 GATT 会话类型。
using Windows.Devices.Bluetooth;
// 引入主动 BLE 广播监听器，设备页据此显示未配对的附近设备。
using Windows.Devices.Bluetooth.Advertisement;
// 引入 GATT 服务、特征、缓存模式和通信状态枚举。
using Windows.Devices.Bluetooth.GenericAttributeProfile;
// 引入系统 DeviceInformation 扫描和配对 API。
using Windows.Devices.Enumeration;
// 引入 WinRT IBuffer 的安全读写工具。
using Windows.Storage.Streams;

// Windows transport 位于独立命名空间，WPF 启动层可显式选择 Mock 或真机。
namespace FitnessCoach.Bluetooth.Windows;

/// <summary>
/// 使用 Windows.Devices.Bluetooth 实现真实 GATT：扫描、系统配对、服务/0001～0007 发现、订阅和释放。
/// </summary>
public sealed class WinRtBleTransport : IWindowsBleTransport, IWindowsBlePairingManager, IWindowsBleDiscoveryTransport
{
    // Windows AEP 扫描 RSSI 属性键；值通常为 Int32，单位 dBm。
    private const string SignalStrengthProperty = "System.Devices.Aep.SignalStrength";
    // 选择器决定优先旧设备、自动产品名或未来 WPF 人工选择对话框。
    private readonly IWindowsBleDeviceSelector _deviceSelector;
    // 生命周期信号量防止扫描连接和主动断开并发释放同一 WinRT 对象。
    private readonly SemaphoreSlim _lifecycleGate = new(1, 1);
    // 已发现的七个自定义 GATT 特征按 UUID 索引。
    private readonly Dictionary<Guid, GattCharacteristic> _characteristics = new();
    // 当前 Windows BLE 设备对象；Disconnect/Dispose 时必须释放。
    private BluetoothLEDevice? _device;
    // 自定义健身 GATT 服务对象；持有期间保持特征有效。
    private GattDeviceService? _fitnessService;
    // GATT 会话用于读取 MaxPduSize 并请求维持连接。
    private GattSession? _gattSession;
    // 当前完整连接状态；只有七个特征全部发现后才为 true。
    private volatile bool _isConnected;
    // 主动断开标志抑制 ConnectionStatusChanged 产生重复“意外断线”。
    private volatile bool _disconnectRequested;
    // dispose 标志阻止释放后再次扫描。
    private volatile bool _disposed;
    // 保存最近扫描 RSSI，重连短路返回时仍可显示最后已知值。
    private int? _lastRssiDbm;
    // 保存标准设备型号，断线后诊断页仍可显示最后已知设备身份。
    private string _modelNumber = "未知";
    // 保存标准硬件修订号。
    private string _hardwareRevision = "未知";
    // 保存标准固件修订号。
    private string _firmwareRevision = "未知";

    /// <summary>创建 WinRT transport；未提供选择器时只自动选择 BPNN-FIT-* 产品设备。</summary>
    public WinRtBleTransport(IWindowsBleDeviceSelector? deviceSelector = null)
    {
        // 使用调用者选择器或安全默认选择器。
        _deviceSelector = deviceSelector ?? new FirstFitnessDeviceSelector();
    }

    /// <inheritdoc />
    public event EventHandler<BleGattValueReceivedEventArgs>? ValueReceived;

    /// <inheritdoc />
    public event EventHandler<BleTransportDisconnectedEventArgs>? Disconnected;

    /// <inheritdoc />
    public bool IsConnected => _isConnected;

    /// <inheritdoc />
    public async Task<IReadOnlyList<BleDiscoveredDevice>> ScanDevicesAsync(
        TimeSpan scanDuration,
        CancellationToken cancellationToken)
    {
        // 释放后不得访问 Windows 蓝牙适配器。
        ThrowIfDisposed();
        // 交互扫描必须是有限正时间窗，防止 watcher 永久占用系统无线资源。
        if ((scanDuration <= TimeSpan.Zero) || (scanDuration > TimeSpan.FromSeconds(30)))
        {
            // 报告调用方配置错误。
            throw new ArgumentOutOfRangeException(nameof(scanDuration), "蓝牙扫描时间必须大于零且不超过 30 秒。");
        }

        // 扫描与连接/断开共用生命周期锁，避免枚举期间释放同一 WinRT 设备对象。
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            // 执行主动广播与系统缓存合并，返回不含 WinRT 对象的稳定快照。
            return await ScanDevicesCoreAsync(scanDuration, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            // 无论取消或失败都释放生命周期锁。
            _lifecycleGate.Release();
        }
    }

    /// <inheritdoc />
    public async Task<BleConnectedDevice> ScanAndConnectAsync(string? preferredDeviceId, CancellationToken cancellationToken)
    {
        // 释放后不得重新创建 WinRT 蓝牙对象。
        ThrowIfDisposed();
        // 串行化扫描连接与 Disconnect。
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            // 若连接已经完整，直接返回当前描述，不重复发现 GATT。
            if (IsConnected && (_device is not null))
            {
                // 从当前会话读取 ATT MTU 并返回稳定设备 ID。
                return new BleConnectedDevice(
                    _device.DeviceInformation.Id,
                    SafeDeviceName(_device),
                    GetAttMtu(),
                    _lastRssiDbm,
                    _modelNumber,
                    _hardwareRevision,
                    _firmwareRevision);
            }

            // 清理此前失败连接留下的服务、特征和设备对象。
            await DisconnectCoreAsync().ConfigureAwait(false);
            // 本次连接不是用户主动断开。
            _disconnectRequested = false;

            // 保存最终选择 DTO；显式设备列表传入 preferred ID 时无需再次等待广播扫描。
            BleDiscoveredDevice? selected = null;
            // 保存用于系统配对的 WinRT 设备信息；其生命周期只覆盖当前连接步骤。
            DeviceInformation? selectedInformation = null;

            // 用户已从可见列表选中设备或自动重连已有固定 ID 时，直接打开该系统记录。
            if (!string.IsNullOrWhiteSpace(preferredDeviceId))
            {
                // 精确读取 Windows 设备记录，不回退为同名的另一台手柄。
                selectedInformation = await DeviceInformation.CreateFromIdAsync(preferredDeviceId)
                    .AsTask(cancellationToken)
                    .ConfigureAwait(false);
                // 系统记录存在时构造选择 DTO，RSSI 使用记录中的最近值。
                if (selectedInformation is not null)
                {
                    // 空系统名称回退为 ID；后续 BluetoothLEDevice 仍会尝试读取广播名。
                    string preferredDisplayName = string.IsNullOrWhiteSpace(selectedInformation.Name)
                        ? preferredDeviceId
                        : selectedInformation.Name;
                    // 保存精确设备及其配对、RSSI 状态。
                    selected = new BleDiscoveredDevice(
                        selectedInformation.Id,
                        preferredDisplayName,
                        selectedInformation.Pairing.IsPaired,
                        ReadRssiDbm(selectedInformation));
                }
            }

            // 没有固定设备或旧系统记录失效时，主动扫描并由安全选择器挑产品手柄。
            if ((selected is null) || (selectedInformation is null))
            {
                // 合并 6 秒主动广播和 Windows 缓存，未配对手柄也必须可发现。
                IReadOnlyList<BleDiscoveredDevice> discoveredDevices = await ScanDevicesCoreAsync(
                    TimeSpan.FromSeconds(6),
                    cancellationToken).ConfigureAwait(false);
                // 默认只选择产品前缀；未来其它调用者仍可注入人工选择策略。
                selected = await _deviceSelector.SelectAsync(
                    discoveredDevices,
                    preferredDeviceId,
                    cancellationToken).ConfigureAwait(false);
                // null 表示没有产品设备或用户取消。
                if (selected is null)
                {
                    // 提供可恢复提示，不自动连接其它 BLE 外设。
                    throw new InvalidOperationException("没有发现可连接的 BPNN-FIT 手柄，或用户取消了设备选择。" );
                }

                // 用主动扫描返回的精确 DeviceInformation.Id 重新打开系统配对对象。
                selectedInformation = await DeviceInformation.CreateFromIdAsync(selected.DeviceId)
                    .AsTask(cancellationToken)
                    .ConfigureAwait(false);
                // 广播在扫描结束后消失或权限被撤销时明确失败。
                if (selectedInformation is null)
                {
                    // 提示用户保持手柄开机并重新扫描。
                    throw new IOException("Windows 无法打开所选蓝牙设备记录；请保持手柄开机后重新扫描。" );
                }
            }

            // 优先使用主动广播 RSSI，缺失时回退系统 AEP 属性。
            _lastRssiDbm = selected.RssiDbm ?? ReadRssiDbm(selectedInformation);
            // 未配对时进入 Windows 系统配对边界；弹窗和 PIN/数字确认由系统 UI 负责。
            if (!selectedInformation.Pairing.IsPaired)
            {
                // 请求加密并认证的 LE Secure Connections 配对级别。
                DevicePairingResult pairingResult = await selectedInformation.Pairing.PairAsync(
                    DevicePairingProtectionLevel.EncryptionAndAuthentication).AsTask(cancellationToken).ConfigureAwait(false);
                // Paired 和 AlreadyPaired 都表示后续允许读取受保护控制特征。
                if ((pairingResult.Status != DevicePairingResultStatus.Paired) &&
                    (pairingResult.Status != DevicePairingResultStatus.AlreadyPaired))
                {
                    // 报告 Windows 系统配对状态，用户可在系统蓝牙设置中清除后重试。
                    throw new UnauthorizedAccessException($"Windows BLE 配对失败：{pairingResult.Status}。" );
                }
            }

            // 通过 Windows 设备 ID 建立 BluetoothLEDevice 对象。
            _device = await BluetoothLEDevice.FromIdAsync(selected.DeviceId).AsTask(cancellationToken).ConfigureAwait(false);
            // 设备离线、权限被拒绝或广播消失时 FromIdAsync 可能返回 null。
            if (_device is null)
            {
                // 明确提示重新打开手柄并靠近电脑。
                throw new IOException("Windows 无法打开所选 BLE 设备；请确认手柄已开机、在范围内且未被其它 PC 占用。" );
            }

            // 订阅系统连接状态；异常断开时通知会话层执行指数退避。
            _device.ConnectionStatusChanged += OnDeviceConnectionStatusChanged;
            // Uncached 查询自定义服务，避免 Windows 使用固件升级前的旧 GATT 数据库。
            GattDeviceServicesResult servicesResult = await _device.GetGattServicesForUuidAsync(
                ProtocolConstants.FitnessServiceUuid,
                BluetoothCacheMode.Uncached).AsTask(cancellationToken).ConfigureAwait(false);
            // 非成功状态可能来自未配对、设备离线或系统蓝牙权限。
            EnsureGattSuccess(servicesResult.Status, "发现健身 GATT 服务");
            // 产品合同只允许一个同 UUID 服务；没有服务表示刷入了错误固件。
            _fitnessService = servicesResult.Services.FirstOrDefault()
                ?? throw new InvalidDataException("设备没有提供 7B2E0000 健身 GATT 服务。" );

            // 依次发现协议规定的 0001～0007 七个特征。
            Guid[] requiredCharacteristics =
            [
                ProtocolConstants.ControlPointUuid,
                ProtocolConstants.ManifestUuid,
                ProtocolConstants.LiveStateUuid,
                ProtocolConstants.EventUuid,
                ProtocolConstants.TransferControlUuid,
                ProtocolConstants.TransferDataUuid,
                ProtocolConstants.RawStreamUuid,
            ];

            // 每个 UUID 使用 Uncached 查询并要求唯一首项。
            foreach (Guid characteristicUuid in requiredCharacteristics)
            {
                // 查询当前服务下指定 UUID 特征。
                GattCharacteristicsResult characteristicsResult = await _fitnessService.GetCharacteristicsForUuidAsync(
                    characteristicUuid,
                    BluetoothCacheMode.Uncached).AsTask(cancellationToken).ConfigureAwait(false);
                // 验证系统 GATT 操作成功。
                EnsureGattSuccess(characteristicsResult.Status, $"发现特征 {characteristicUuid}");
                // 缺失任一特征表示固件协议数据库不完整，禁止进入控制状态。
                GattCharacteristic characteristic = characteristicsResult.Characteristics.FirstOrDefault()
                    ?? throw new InvalidDataException($"设备缺少必需 GATT 特征 {characteristicUuid}。" );
                // 保存 UUID 到 WinRT 特征映射。
                _characteristics.Add(characteristicUuid, characteristic);
            }

            // 创建 GATT 会话以获取 MaxPduSize 和保持连接能力。
            _gattSession = await GattSession.FromDeviceIdAsync(_device.BluetoothDeviceId).AsTask(cancellationToken).ConfigureAwait(false);
            // 系统无法创建会话时仍可按 MTU23 工作，但产品需要自动重连和 MTU 读取，故明确拒绝。
            if (_gattSession is null)
            {
                // 提示系统蓝牙栈或设备权限异常。
                throw new IOException("Windows 无法创建 GATT 会话。" );
            }

            // 请求 Windows 在空闲期间维持连接；真正断线仍由 ConnectionStatusChanged 报告。
            _gattSession.MaintainConnection = true;
            // 读取标准 0x180A Device Information；缺少可选字符串时保留“未知”而不阻断训练连接。
            (_modelNumber, _hardwareRevision, _firmwareRevision) = await ReadStandardDeviceInformationAsync(cancellationToken)
                .ConfigureAwait(false);
            // 所有服务和特征准备完毕后提交连接状态。
            _isConnected = true;
            // 返回系统设备 ID、名称、实际 ATT MTU、RSSI 和标准设备信息。
            return new BleConnectedDevice(
                selected.DeviceId,
                SafeDeviceName(_device),
                GetAttMtu(),
                _lastRssiDbm,
                _modelNumber,
                _hardwareRevision,
                _firmwareRevision);
        }
        catch
        {
            // 发现或配对任一步失败都释放半初始化 WinRT 对象。
            await DisconnectCoreAsync().ConfigureAwait(false);
            // 把原始错误交给会话层和 WPF 显示。
            throw;
        }
        finally
        {
            // 释放生命周期锁，允许断开或下一次扫描。
            _lifecycleGate.Release();
        }
    }

    /// <inheritdoc />
    public async Task<byte[]> ReadAsync(Guid characteristicUuid, CancellationToken cancellationToken)
    {
        // 查找已发现特征并检查完整连接。
        GattCharacteristic characteristic = GetConnectedCharacteristic(characteristicUuid);
        // 使用 Uncached 读取，快照和 Manifest 不接受 Windows 旧缓存。
        GattReadResult result = await characteristic.ReadValueAsync(BluetoothCacheMode.Uncached).AsTask(cancellationToken).ConfigureAwait(false);
        // 验证 GATT 通信状态。
        EnsureGattSuccess(result.Status, $"读取特征 {characteristicUuid}");
        // 把 WinRT IBuffer 复制为托管数组。
        return CopyBuffer(result.Value);
    }

    /// <inheritdoc />
    public async Task SubscribeAsync(Guid characteristicUuid, bool useIndication, CancellationToken cancellationToken)
    {
        // 查找已发现特征并检查连接。
        GattCharacteristic characteristic = GetConnectedCharacteristic(characteristicUuid);
        // 按调用合同选择 indication 或 notification CCCD 值。
        GattClientCharacteristicConfigurationDescriptorValue descriptorValue = useIndication
            ? GattClientCharacteristicConfigurationDescriptorValue.Indicate
            : GattClientCharacteristicConfigurationDescriptorValue.Notify;
        // 先注册 ValueChanged，避免 CCCD 写成功后第一帧到达却没有处理器。
        characteristic.ValueChanged -= OnCharacteristicValueChanged;
        // 注册统一复制回调。
        characteristic.ValueChanged += OnCharacteristicValueChanged;
        // 写入 CCCD 并等待 Windows 确认。
        GattCommunicationStatus status = await characteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
            descriptorValue).AsTask(cancellationToken).ConfigureAwait(false);
        // 订阅失败时不能假装连接恢复完整。
        EnsureGattSuccess(status, $"订阅特征 {characteristicUuid}");
    }

    /// <inheritdoc />
    public async Task WriteAsync(
        Guid characteristicUuid,
        ReadOnlyMemory<byte> value,
        bool withResponse,
        CancellationToken cancellationToken)
    {
        // 查找控制或传输特征。
        GattCharacteristic characteristic = GetConnectedCharacteristic(characteristicUuid);
        // 创建精确长度 WinRT 缓冲区并写入当前分片副本。
        IBuffer buffer = CreateBuffer(value.Span);
        // 控制点必须 WriteWithResponse；原始流控制等未来路径可显式选无响应。
        GattWriteOption writeOption = withResponse ? GattWriteOption.WriteWithResponse : GattWriteOption.WriteWithoutResponse;
        // 执行 GATT 写并获得协议错误状态。
        GattWriteResult result = await characteristic.WriteValueWithResultAsync(buffer, writeOption).AsTask(cancellationToken).ConfigureAwait(false);
        // 非成功状态立即交给会话层重试/重连。
        EnsureGattSuccess(result.Status, $"写入特征 {characteristicUuid}");
    }

    /// <inheritdoc />
    public async Task DisconnectAsync(CancellationToken cancellationToken)
    {
        // 串行化主动断开和扫描连接。
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            // 标记主动断开，ConnectionStatusChanged 不触发自动重连。
            _disconnectRequested = true;
            // 释放当前所有 WinRT 对象。
            await DisconnectCoreAsync().ConfigureAwait(false);
        }
        finally
        {
            // 释放生命周期锁。
            _lifecycleGate.Release();
        }
    }

    /// <inheritdoc />
    public async Task ForgetDeviceAsync(string deviceId, CancellationToken cancellationToken)
    {
        // 空 Windows 设备 ID 无法定位系统配对记录。
        if (string.IsNullOrWhiteSpace(deviceId))
        {
            // 拒绝把用户可见名称或空值误当系统设备 ID。
            throw new ArgumentException("Windows 设备 ID 不能为空。", nameof(deviceId));
        }

        // 释放后不得再次调用 WinRT 配对 API。
        ThrowIfDisposed();
        // 串行化断开、取消配对和扫描，防止新连接在取消配对过程中建立。
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            // 标记主动操作，释放旧 BluetoothLEDevice 时不发布意外断线。
            _disconnectRequested = true;
            // 取消配对前释放 GATT 会话、服务和特征句柄。
            await DisconnectCoreAsync().ConfigureAwait(false);
            // 用扫描保存的精确 Windows ID 重建设备信息，仅访问系统配对元数据。
            DeviceInformation? information = await DeviceInformation.CreateFromIdAsync(deviceId)
                .AsTask(cancellationToken)
                .ConfigureAwait(false);
            // 设备记录已经被系统删除时视为忘记完成，不再制造不可恢复错误。
            if (information is null)
            {
                // 没有系统记录即无需继续取消配对。
                return;
            }

            // 已经未配对时按幂等成功返回。
            if (!information.Pairing.IsPaired)
            {
                // 系统状态已满足忘记设备目标。
                return;
            }

            // 请求 Windows 删除该设备的配对密钥和配对记录。
            DeviceUnpairingResult result = await information.Pairing.UnpairAsync()
                .AsTask(cancellationToken)
                .ConfigureAwait(false);
            // Unpaired 和 AlreadyUnpaired 都表示下次连接必须重新配对。
            if ((result.Status != DeviceUnpairingResultStatus.Unpaired) &&
                (result.Status != DeviceUnpairingResultStatus.AlreadyUnpaired))
            {
                // 把系统状态交给中文设备页，用户可检查 Windows 蓝牙权限后重试。
                throw new UnauthorizedAccessException($"Windows 取消 BLE 配对失败：{result.Status}。");
            }
        }
        finally
        {
            // 释放生命周期锁，允许忘记成功后重新扫描和配对。
            _lifecycleGate.Release();
        }
    }

    /// <inheritdoc />
    public async ValueTask DisposeAsync()
    {
        // 幂等检查避免重复释放信号量。
        if (_disposed)
        {
            // 已释放无后续动作。
            return;
        }

        // 标记释放，阻止新扫描。
        _disposed = true;
        // 主动断开并抑制重连事件。
        _disconnectRequested = true;
        // 等待生命周期锁，避免在扫描中释放对象。
        await _lifecycleGate.WaitAsync().ConfigureAwait(false);

        try
        {
            // 释放当前 GATT 会话。
            await DisconnectCoreAsync().ConfigureAwait(false);
        }
        finally
        {
            // 释放锁后销毁信号量。
            _lifecycleGate.Release();
            // 销毁生命周期信号量。
            _lifecycleGate.Dispose();
        }
    }

    // WinRT 特征回调只复制 Value 并发布无 WinRT 类型事件。
    private void OnCharacteristicValueChanged(GattCharacteristic sender, GattValueChangedEventArgs eventArgs)
    {
        // 把 IBuffer 内容复制为托管数组，脱离回调生命周期。
        byte[] value = CopyBuffer(eventArgs.CharacteristicValue);
        // 发布特征 UUID 和独立字节副本。
        ValueReceived?.Invoke(this, new BleGattValueReceivedEventArgs(sender.Uuid, value));
    }

    // Windows 连接状态变为 Disconnected 时通知会话层。
    private void OnDeviceConnectionStatusChanged(BluetoothLEDevice sender, object eventArgs)
    {
        // 仍连接时不发布任何事件。
        if (sender.ConnectionStatus == BluetoothConnectionStatus.Connected)
        {
            // 返回等待后续变化。
            return;
        }

        // 清除 transport 完整连接标志。
        _isConnected = false;
        // 主动 Disconnect/Dispose 不属于意外断线。
        if (_disconnectRequested || _disposed)
        {
            // 会话层主动路径会自行发布断线状态。
            return;
        }

        // 发布系统蓝牙或远端造成的链路丢失。
        Disconnected?.Invoke(this, new BleTransportDisconnectedEventArgs("系统报告蓝牙设备连接已丢失。"));
    }

    // 释放所有特征事件、GATT 会话、服务和设备对象；调用方已持有生命周期锁。
    private Task DisconnectCoreAsync()
    {
        // transport 从此刻不再允许读写。
        _isConnected = false;

        // 遍历七个特征，解除统一 ValueChanged 回调。
        foreach (GattCharacteristic characteristic in _characteristics.Values)
        {
            // 解除通知处理，防止释放后继续回调。
            characteristic.ValueChanged -= OnCharacteristicValueChanged;
        }

        // 清空 UUID 映射，旧特征对象不得跨重连复用。
        _characteristics.Clear();

        // GattSession 存在时先关闭 MaintainConnection。
        if (_gattSession is not null)
        {
            // 允许 Windows 蓝牙栈释放空闲连接。
            _gattSession.MaintainConnection = false;
            // 释放会话 COM 对象。
            _gattSession.Dispose();
            // 清除字段防止重复 Dispose。
            _gattSession = null;
        }

        // 释放自定义服务及其底层句柄。
        _fitnessService?.Dispose();
        // 清除服务字段。
        _fitnessService = null;

        // 设备存在时解除连接状态事件并释放对象。
        if (_device is not null)
        {
            // 解除系统回调。
            _device.ConnectionStatusChanged -= OnDeviceConnectionStatusChanged;
            // 释放 BluetoothLEDevice COM 对象。
            _device.Dispose();
            // 清除字段。
            _device = null;
        }

        // 当前没有需要异步等待的 WinRT 释放操作。
        return Task.CompletedTask;
    }

    // 查找已发现特征并验证链路连接。
    private GattCharacteristic GetConnectedCharacteristic(Guid characteristicUuid)
    {
        // transport 必须处于完整连接态。
        if (!IsConnected)
        {
            // 禁止对已释放 WinRT 特征执行操作。
            throw new InvalidOperationException("Windows BLE transport 尚未连接。" );
        }

        // 查找当前重连周期发现的特征对象。
        if (!_characteristics.TryGetValue(characteristicUuid, out GattCharacteristic? characteristic))
        {
            // 报告协议或调用方 UUID 错误。
            throw new KeyNotFoundException($"当前 GATT 数据库没有特征 {characteristicUuid}。" );
        }

        // 返回有效特征对象。
        return characteristic;
    }

    // 从当前 GattSession 获取 ATT MTU；WinRT MaxPduSize 超过 ushort 时安全夹紧。
    private ushort GetAttMtu()
    {
        // GattSession 尚未创建时使用 BLE 最小 MTU23。
        if (_gattSession is null)
        {
            // 返回协议必须支持的最小值。
            return 23;
        }

        // MaxPduSize 使用 uint，夹紧到 ushort 字段范围。
        uint maxPduSize = _gattSession.MaxPduSize;
        // 小于 23 的异常系统值回退 23，大于 65535 则夹到上限。
        return checked((ushort)Math.Clamp(maxPduSize, 23u, ushort.MaxValue));
    }

    // 合并 Windows 缓存设备与主动广播设备；调用方已持有生命周期锁。
    private async Task<IReadOnlyList<BleDiscoveredDevice>> ScanDevicesCoreAsync(
        TimeSpan scanDuration,
        CancellationToken cancellationToken)
    {
        // 使用系统设备 ID 去重；相同物理设备可能同时来自缓存枚举和主动广播。
        Dictionary<string, BleDiscoveredDevice> devicesById = new(StringComparer.OrdinalIgnoreCase);
        // 请求 AEP RSSI 扩展属性；部分适配器不提供时保留 null。
        string[] additionalProperties = [SignalStrengthProperty];
        // 枚举系统已缓存和已配对 BLE 记录，保证暂未广播的已配对手柄仍可见。
        DeviceInformationCollection cachedDevices = await DeviceInformation.FindAllAsync(
            BluetoothLEDevice.GetDeviceSelector(),
            additionalProperties).AsTask(cancellationToken).ConfigureAwait(false);

        // 复制每个有名称的系统记录；空名称未配对地址对用户没有辨识价值。
        foreach (DeviceInformation information in cachedDevices)
        {
            // 系统名称为空且未配对时跳过，避免列表充满不可辨认的 AEP 路径。
            if (string.IsNullOrWhiteSpace(information.Name) && !information.Pairing.IsPaired)
            {
                // 继续处理下一条系统记录。
                continue;
            }

            // 已配对空名称设备回退为系统 ID，仍允许用户查看和忘记旧记录。
            string displayName = string.IsNullOrWhiteSpace(information.Name) ? information.Id : information.Name;
            // 保存缓存设备的配对和最近 RSSI 快照。
            devicesById[information.Id] = new BleDiscoveredDevice(
                information.Id,
                displayName,
                information.Pairing.IsPaired,
                ReadRssiDbm(information));
        }

        // 广播回调可能来自多个系统线程，按 48 位蓝牙地址汇总最新名称和最强 RSSI。
        ConcurrentDictionary<ulong, AdvertisementSnapshot> advertisements = new();
        // 创建主动扫描 watcher，使未配对 BPNN-FIT 广播可出现在设备页。
        BluetoothLEAdvertisementWatcher watcher = new()
        {
            // Active 模式会请求扫描响应，通常能取得完整本地名称。
            ScanningMode = BluetoothLEScanningMode.Active,
        };

        // 收到一次广播时只复制值类型和名称，不在系统回调线程执行 WinRT 设备打开。
        void OnAdvertisementReceived(
            BluetoothLEAdvertisementWatcher sender,
            BluetoothLEAdvertisementReceivedEventArgs eventArgs)
        {
            // sender 由事件签名要求；扫描配置已由外层 watcher 固定，无需在回调中读取。
            _ = sender;
            // 复制本次扫描响应中的本地名称；普通广播可能为空。
            string localName = eventArgs.Advertisement.LocalName ?? string.Empty;
            // 构造本次地址、名称和 RSSI 快照，RSSI 单位为 dBm。
            AdvertisementSnapshot incoming = new(
                eventArgs.BluetoothAddress,
                localName,
                eventArgs.RawSignalStrengthInDBm);
            // 同一地址保留已取得的非空名称和扫描窗口内最强 RSSI。
            advertisements.AddOrUpdate(
                eventArgs.BluetoothAddress,
                incoming,
                (_, current) => new AdvertisementSnapshot(
                    current.BluetoothAddress,
                    string.IsNullOrWhiteSpace(incoming.LocalName) ? current.LocalName : incoming.LocalName,
                    Math.Max(current.RssiDbm, incoming.RssiDbm)));
        }

        // 注册轻量广播回调。
        watcher.Received += OnAdvertisementReceived;

        try
        {
            // 启动 Windows 主动 BLE 扫描。
            watcher.Start();
            // 在用户可见时间窗内收集附近设备；取消会立即进入 finally 停止 watcher。
            await Task.Delay(scanDuration, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            // 先解除回调，停止后到达的尾部事件不会再修改集合。
            watcher.Received -= OnAdvertisementReceived;
            // 释放系统扫描资源；重复 Stop 对已停止 watcher 是安全操作。
            watcher.Stop();
        }

        // 最多解析信号最强的 64 个地址，防止拥挤环境中无界打开 WinRT 对象。
        IEnumerable<AdvertisementSnapshot> strongestAdvertisements = advertisements.Values
            // RSSI 越接近零表示信号越强，优先解析近距离设备。
            .OrderByDescending(snapshot => snapshot.RssiDbm)
            // 固定上限约束扫描结束后的 WinRT I/O 和内存占用。
            .Take(64);

        // 逐个把 48 位地址转换为 Windows DeviceInformation.Id，供后续精确连接和配对。
        foreach (AdvertisementSnapshot advertisement in strongestAdvertisements)
        {
            // 检查用户取消，避免解析大量地址时界面无法退出。
            cancellationToken.ThrowIfCancellationRequested();
            // 当前地址对应的 WinRT BLE 对象只用于读取系统 ID、名称和配对状态。
            BluetoothLEDevice? candidate = null;

            try
            {
                // 从广播地址打开设备对象；设备离开范围时可能返回 null。
                candidate = await BluetoothLEDevice.FromBluetoothAddressAsync(advertisement.BluetoothAddress)
                    .AsTask(cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception) when (!cancellationToken.IsCancellationRequested)
            {
                // 单台无权限或瞬时离线设备不应让整个列表扫描失败。
                continue;
            }

            // 无法解析的广播地址没有可供后续连接的 Windows ID。
            if (candidate is null)
            {
                // 继续处理下一台附近设备。
                continue;
            }

            try
            {
                // 保存系统设备信息，包含稳定 ID 与当前配对状态。
                DeviceInformation information = candidate.DeviceInformation;
                // 广播本地名称优先，其次使用 BluetoothLEDevice/System 缓存名称。
                string displayName = !string.IsNullOrWhiteSpace(advertisement.LocalName)
                    ? advertisement.LocalName
                    : SafeDeviceName(candidate);
                // 未命名且未配对设备无法辨认，跳过而不显示裸地址。
                if (string.IsNullOrWhiteSpace(displayName) && !information.Pairing.IsPaired)
                {
                    // 继续下一台设备。
                    continue;
                }

                // 已配对空名称设备回退系统 ID，保证用户能看见旧配对记录。
                displayName = string.IsNullOrWhiteSpace(displayName) ? information.Id : displayName;
                // 构造主动广播快照，RSSI 使用当前扫描窗口的最强值。
                BleDiscoveredDevice activeDevice = new(
                    information.Id,
                    displayName,
                    information.Pairing.IsPaired,
                    advertisement.RssiDbm);
                // 主动广播覆盖同一系统 ID 的旧 RSSI和名称，同时保留任一路径的已配对事实。
                if (devicesById.TryGetValue(information.Id, out BleDiscoveredDevice? cachedDevice))
                {
                    // 非空主动广播名优先；配对状态取逻辑或，避免瞬态系统投影丢失绑定事实。
                    activeDevice = activeDevice with
                    {
                        DisplayName = string.IsNullOrWhiteSpace(activeDevice.DisplayName)
                            ? cachedDevice.DisplayName
                            : activeDevice.DisplayName,
                        IsPaired = activeDevice.IsPaired || cachedDevice.IsPaired,
                    };
                }

                // 写入最终去重字典。
                devicesById[information.Id] = activeDevice;
            }
            finally
            {
                // 释放临时 BluetoothLEDevice COM 对象，列表只保存纯托管 DTO。
                candidate.Dispose();
            }
        }

        // 返回 Windows 风格稳定顺序：产品、已配对、强信号、名称。
        return devicesById.Values
            // 产品广播名前缀优先显示在列表顶部。
            .OrderByDescending(device => device.DisplayName.StartsWith("BPNN-FIT-", StringComparison.OrdinalIgnoreCase))
            // 同类设备中已配对记录优先。
            .ThenByDescending(device => device.IsPaired)
            // 信号未知按最弱处理。
            .ThenByDescending(device => device.RssiDbm ?? int.MinValue)
            // 最后按名称稳定排序。
            .ThenBy(device => device.DisplayName, StringComparer.CurrentCultureIgnoreCase)
            // 复制为独立数组，后续扫描不会改变当前结果。
            .ToArray();
    }

    // 从 DeviceInformation AEP 属性读取 RSSI，单位 dBm。
    private static int? ReadRssiDbm(DeviceInformation information)
    {
        // 属性不存在表示 Windows 蓝牙栈未提供扫描信号强度。
        if (!information.Properties.TryGetValue(SignalStrengthProperty, out object? rawValue) || (rawValue is null))
        {
            // 返回 null，界面显示未知而不是伪造 0 dBm。
            return null;
        }

        try
        {
            // WinRT 可能投影为 Int16 或 Int32，统一转换为有符号整数。
            int rssiDbm = Convert.ToInt32(rawValue, System.Globalization.CultureInfo.InvariantCulture);
            // BLE RSSI 工程范围约 -127～20 dBm，超界值视为无效。
            return rssiDbm is >= -127 and <= 20 ? rssiDbm : null;
        }
        catch (Exception exception) when (exception is FormatException or InvalidCastException or OverflowException)
        {
            // 系统属性类型异常不应阻断设备连接。
            return null;
        }
    }

    // 保存主动广播回调复制出的值类型快照，不让 WinRT 事件参数越过回调生命周期。
    private sealed record AdvertisementSnapshot(
        // 48 位 BLE 公共或随机地址；仅在当前扫描周期内用于 FromBluetoothAddressAsync。
        ulong BluetoothAddress,
        // 广播或扫描响应本地名称，可能为空字符串。
        string LocalName,
        // 扫描窗口内最强接收信号，单位 dBm。
        int RssiDbm);

    // 读取标准 Device Information 0x180A 的型号、硬件和固件修订字符串。
    private async Task<(string ModelNumber, string HardwareRevision, string FirmwareRevision)> ReadStandardDeviceInformationAsync(
        CancellationToken cancellationToken)
    {
        // 设备对象为空时无法查询标准服务。
        if (_device is null)
        {
            // 返回三个未知占位符。
            return ("未知", "未知", "未知");
        }

        try
        {
            // 使用 Uncached 发现标准 Device Information，避免固件升级后显示旧版本。
            GattDeviceServicesResult servicesResult = await _device.GetGattServicesForUuidAsync(
                GattServiceUuids.DeviceInformation,
                BluetoothCacheMode.Uncached).AsTask(cancellationToken).ConfigureAwait(false);
            // 缺少标准服务不影响自定义协议连接，只返回未知字段。
            if ((servicesResult.Status != GattCommunicationStatus.Success) || (servicesResult.Services.Count == 0))
            {
                // 返回未知设备信息。
                return ("未知", "未知", "未知");
            }

            // 只使用唯一标准服务首项，并在读取完成后释放 WinRT 对象。
            using GattDeviceService informationService = servicesResult.Services[0];
            // 读取型号字符串 0x2A24。
            string modelNumber = await ReadStandardStringAsync(
                informationService,
                GattCharacteristicUuids.ModelNumberString,
                cancellationToken).ConfigureAwait(false);
            // 读取硬件修订字符串 0x2A27。
            string hardwareRevision = await ReadStandardStringAsync(
                informationService,
                GattCharacteristicUuids.HardwareRevisionString,
                cancellationToken).ConfigureAwait(false);
            // 读取固件修订字符串 0x2A26。
            string firmwareRevision = await ReadStandardStringAsync(
                informationService,
                GattCharacteristicUuids.FirmwareRevisionString,
                cancellationToken).ConfigureAwait(false);
            // 返回三个独立字符串。
            return (modelNumber, hardwareRevision, firmwareRevision);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            // 调用方取消必须继续传播，不能误认为设备信息缺失。
            throw;
        }
        catch (Exception exception)
        {
            // 标准信息是诊断增强项；WinRT 可能抛 COMException 等平台异常，均不应拆除已完成的自定义服务连接。
            System.Diagnostics.Debug.WriteLine($"读取标准 Device Information 失败：{exception.Message}");
            // 返回稳定未知文本。
            return ("未知", "未知", "未知");
        }
    }

    // 从标准服务读取一个 UTF-8/ASCII 字符串特征。
    private static async Task<string> ReadStandardStringAsync(
        GattDeviceService service,
        Guid characteristicUuid,
        CancellationToken cancellationToken)
    {
        // 使用 Uncached 查找当前字符串特征。
        GattCharacteristicsResult characteristicsResult = await service.GetCharacteristicsForUuidAsync(
            characteristicUuid,
            BluetoothCacheMode.Uncached).AsTask(cancellationToken).ConfigureAwait(false);
        // 特征缺失或系统状态失败时返回未知。
        if ((characteristicsResult.Status != GattCommunicationStatus.Success) || (characteristicsResult.Characteristics.Count == 0))
        {
            // 不把可选标准字段缺失升级为连接错误。
            return "未知";
        }

        // 读取首个标准字符串特征。
        GattReadResult readResult = await characteristicsResult.Characteristics[0].ReadValueAsync(BluetoothCacheMode.Uncached)
            .AsTask(cancellationToken)
            .ConfigureAwait(false);
        // GATT 读取失败或空值时返回未知。
        if ((readResult.Status != GattCommunicationStatus.Success) || (readResult.Value is null) || (readResult.Value.Length == 0U))
        {
            // 保持诊断页可用。
            return "未知";
        }

        // 复制 WinRT 缓冲并按 UTF-8 解码；厂家 ASCII 是 UTF-8 子集。
        string value = Encoding.UTF8.GetString(CopyBuffer(readResult.Value)).Trim('\0', ' ', '\r', '\n', '\t');
        // 空白结果统一为未知。
        return string.IsNullOrWhiteSpace(value) ? "未知" : value;
    }

    // 返回非空设备名称。
    private static string SafeDeviceName(BluetoothLEDevice device)
    {
        // 优先广播/系统名称，空名称回退系统设备 ID。
        return string.IsNullOrWhiteSpace(device.Name) ? device.DeviceInformation.Id : device.Name;
    }

    // 把托管字节写入 WinRT IBuffer。
    private static IBuffer CreateBuffer(ReadOnlySpan<byte> value)
    {
        // DataWriter 持有独立内存流，写完后 DetachBuffer 转移所有权。
        using DataWriter writer = new();
        // 写入当前 GATT Value 的全部字节。
        writer.WriteBytes(value.ToArray());
        // 分离缓冲区，使 writer Dispose 不销毁返回对象。
        return writer.DetachBuffer();
    }

    // 把 WinRT IBuffer 复制为托管数组。
    private static byte[] CopyBuffer(IBuffer buffer)
    {
        // 创建只读 DataReader；using 在复制后释放 COM 包装。
        using DataReader reader = DataReader.FromBuffer(buffer);
        // 分配精确未读长度数组。
        byte[] output = new byte[reader.UnconsumedBufferLength];
        // 读取全部字节并推进 reader 游标。
        reader.ReadBytes(output);
        // 返回独立数组。
        return output;
    }

    // 验证 GATT 操作状态。
    private static void EnsureGattSuccess(GattCommunicationStatus status, string operation)
    {
        // Success 以外均不允许业务层继续。
        if (status != GattCommunicationStatus.Success)
        {
            // 抛出包含操作和系统状态的 I/O 异常。
            throw new IOException($"{operation}失败：{status}。" );
        }
    }

    // 检查 transport 生命周期。
    private void ThrowIfDisposed()
    {
        // 释放后访问属于调用错误。
        ObjectDisposedException.ThrowIf(_disposed, this);
    }
}
