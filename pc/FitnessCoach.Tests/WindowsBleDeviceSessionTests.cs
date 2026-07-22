// 引入小端读写工具，fake transport 构造与设备 C 端相同的控制响应。
using System.Buffers.Binary;
// 引入协议和真 BLE 会话状态机。
using FitnessCoach.Bluetooth;
// 引入领域实时状态。
using FitnessCoach.Domain;
// 引入 JSON 设置仓储，验证真机/Mock 模式持久化。
using FitnessCoach.Infrastructure;
// 引入设置页 ViewModel，验证 WPF 模式入口无需热替换会话。
using FitnessCoach.App.ViewModels;
// 引入同步 UI 调度器，让设备页集合和连接事件在测试线程确定性执行。
using FitnessCoach.App.Services;

// 真 BLE 会话测试位于现有无第三方控制台测试项目中，可在没有蓝牙硬件时执行。
namespace FitnessCoach.Tests;

/// <summary>用 fake GATT transport 验证重试、重连、revision、事件和释放合同。</summary>
internal static class WindowsBleDeviceSessionTests
{
    /// <summary>顺序执行 Windows BLE 会话全部主机测试。</summary>
    public static async Task RunAllAsync()
    {
        // 先验证 132 字节黄金 Manifest 的字段、小端和未知 tag 向前兼容。
        TestManifestV1GoldenVector();
        // 再验证缺失、重复、截断、非法 UTF-8 和不兼容核心合同均被拒绝。
        TestManifestV1RejectsInvalidInputs();
        // 验证默认设备选择器只选择产品名并优先旧设备 ID。
        await TestDeviceSelectionAsync();
        // 验证设备页能显示附近蓝牙列表，并按用户选中的手柄 ID 建立唯一协议会话。
        await TestVisibleDiscoveryAndSelectedConnectAsync();
        // 验证连接发现、Manifest、订阅和首次快照恢复。
        await TestConnectAndSnapshotAsync();
        // 验证连接自动校时、配置命令、RawStream 开关和 0007 解码。
        await TestConfigurationAndRawStreamAsync();
        // 验证连接状态机在订阅前拒绝不兼容 Manifest 并释放 GATT 链路。
        await TestConnectionRejectsIncompatibleManifestAsync();
        // 验证旧 revision、重复 revision 和 Event 不修改权威状态。
        await TestAuthoritativeRevisionAndEventAsync();
        // 验证 2 秒语义超时后只使用同 request_id 重试一次。
        await TestControlRetryUsesSameRequestIdAsync();
        // 验证意外断线执行退避、重读 Manifest/快照并恢复同一设备。
        await TestAutomaticReconnectAsync();
        // 验证忘记设备会取消系统配对、清除 preferred ID，并让下次连接重新选择。
        await TestForgetDeviceClearsWindowsPairingAsync();
        // 验证设备身份、MTU、RSSI、revision、重连、CRC 和分片错误诊断。
        await TestDiagnosticsSnapshotAndProtocolCountersAsync();
        // 验证扫描取消能释放连接临界区，Dispose 最终释放 transport。
        await TestCancellationAndReleaseAsync();
        // 验证设置页可保存并重新读取真 BLE 模式。
        await TestRealBlePreferenceRoundTripAsync();
    }

    // 用 fake Windows 配对边界验证会话层忘记设备的完整状态收敛。
    private static async Task TestForgetDeviceClearsWindowsPairingAsync()
    {
        // 创建具有固定初始权威状态的 fake transport。
        FakeWindowsBleTransport transport = new(CreateState(31U, FitnessDeviceState.Idle));
        // 创建真 BLE 会话状态机，配对操作仍由 fake 记录而不访问真实 Windows 蓝牙。
        await using WindowsBleDeviceSession session = new(transport);
        // 首次连接保存 fake Windows 设备 ID 为自动重连首选项。
        await session.ConnectAsync();
        // 会话必须公开可选配对管理能力，供设备页执行忘记操作。
        IDevicePairingSession pairingSession = session;
        // 忘记设备应先断开，再请求系统取消同一设备配对。
        await pairingSession.ForgetDeviceAsync();
        // fake 必须收到精确 Windows 设备 ID，不能误用用户可见名称。
        Assert(transport.ForgottenDeviceIds.SequenceEqual([FakeWindowsBleTransport.WindowsDeviceId]), "忘记设备没有取消正确的 Windows 配对记录。");
        // 会话身份必须回到未选择，防止界面继续显示旧固定设备。
        Assert(session.DeviceId == "BLE-UNSELECTED" && !session.IsConnected, "忘记设备后会话仍保留旧身份或连接。");
        // 再次连接应完成新的设备选择。
        await session.ConnectAsync();
        // 第二次扫描不得继续传入已经忘记的 preferred ID。
        Assert(transport.PreferredDeviceIds.Count >= 2 && transport.PreferredDeviceIds[^1] is null, "忘记设备后仍优先旧 Windows 设备 ID。");
    }

    // 验证固件正式 132 字节 TLV 向量可被完整解析。
    private static void TestManifestV1GoldenVector()
    {
        // 解析与固件 tag 顺序、长度和小端格式一致的黄金字节。
        ManifestV1 manifest = ManifestV1Codec.Parse(FakeWindowsBleTransport.ManifestBytes);
        // 核对身份、协议和 297 维特征合同。
        Assert(manifest.DeviceId == "A1B2C3D4E5F6" && manifest.ProtocolMajor == 1 && manifest.ProtocolMinor == 0, "Manifest 身份或协议错误。" );
        // 核对特征和类别表合同。
        Assert(manifest.FeatureDimension == 297 && manifest.FeatureVersion == 1 && manifest.ClassCount == 11 && manifest.ClassTableCrc32 == 0xD8193927U, "Manifest 特征或类别合同错误。" );
        // 核对 32 字节原始 SHA 转为 12 字符短值。
        Assert(manifest.BaseModelSha256Short == "8f66e344bcfa" && manifest.MaskedModelSha256Short == "57f4b2bca05d", "Manifest 模型 SHA 解码错误。" );
        // 核对小端能力和 64 位 LittleFS 容量。
        Assert(manifest.Capabilities == 7U && manifest.LittleFsAvailableBytes == 0x0102030405060708UL, "Manifest 能力或 LittleFS 小端解码错误。" );
        // 在尾部追加未知 tag，解析器必须按 length 跳过而不拒绝兼容设备。
        byte[] withUnknownTag = [.. FakeWindowsBleTransport.ManifestBytes, 0x80, 0x03, 0xAA, 0xBB, 0xCC];
        // 未知 tag 不改变必填字段。
        Assert(ManifestV1Codec.Parse(withUnknownTag).FeatureDimension == 297, "Manifest 未知 tag 未向前兼容。" );
    }

    // 验证所有会造成歧义或模型错配的 Manifest 都抛 InvalidDataException。
    private static void TestManifestV1RejectsInvalidInputs()
    {
        // 缺少最后一个必填 LittleFS tag 必须拒绝。
        AssertManifestRejected(FakeWindowsBleTransport.ManifestBytes[..^10], "缺失必填 tag 未拒绝。" );
        // 追加重复未知 tag；严格规则要求所有 tag 唯一。
        AssertManifestRejected([.. FakeWindowsBleTransport.ManifestBytes, 0x80, 0x00, 0x80, 0x00], "重复 tag 未拒绝。" );
        // 删除最后一字节制造 value 截断。
        AssertManifestRejected(FakeWindowsBleTransport.ManifestBytes[..^1], "截断 Manifest 未拒绝。" );
        // 把协议 tag 固定长度从 2 改为 3，并插入一字节避免误判为单纯截断。
        List<byte> badKnownLength = FakeWindowsBleTransport.ManifestBytes.ToList();
        // 定位 tag 0x03 value 起点。
        int protocolValueOffset = FindManifestValueOffset(badKnownLength.ToArray(), 0x03);
        // length 位于 value 前一字节。
        badKnownLength[protocolValueOffset - 1] = 3;
        // 在原两个版本字节后插入第三个多余字节。
        badKnownLength.Insert(protocolValueOffset + 2, 0);
        // 已知 tag 必须精确匹配固定长度。
        AssertManifestRejected(badKnownLength.ToArray(), "已知 tag 错误长度未拒绝。" );
        // 复制黄金向量用于单项破坏，不修改其它测试共享数据。
        byte[] invalidUtf8 = FakeWindowsBleTransport.ManifestBytes.ToArray();
        // 设备 ID 首字符改为非法 UTF-8 起始字节。
        invalidUtf8[FindManifestValueOffset(invalidUtf8, 0x01)] = 0xFF;
        // 严格 UTF-8 解码必须拒绝替换字符。
        AssertManifestRejected(invalidUtf8, "非法 UTF-8 未拒绝。" );
        // 协议主版本改为 2。
        byte[] badProtocol = FakeWindowsBleTransport.ManifestBytes.ToArray();
        // tag 0x03 的第一个 value 字节是 major。
        badProtocol[FindManifestValueOffset(badProtocol, 0x03)] = 2;
        // PC 只接受协议 1.0。
        AssertManifestRejected(badProtocol, "不兼容协议未拒绝。" );
        // 特征维度改为 296 的小端值 0x0128。
        byte[] badFeatureDimension = FakeWindowsBleTransport.ManifestBytes.ToArray();
        // 定位 tag 0x05 value 起点。
        int featureOffset = FindManifestValueOffset(badFeatureDimension, 0x05);
        // 写入 296 低字节。
        badFeatureDimension[featureOffset] = 0x28;
        // 维度不匹配会造成网络输入错位，必须拒绝。
        AssertManifestRejected(badFeatureDimension, "错误 FEATURE_DIM 未拒绝。" );
        // 类别数量改为 10，CRC 保持原值。
        byte[] badClassCount = FakeWindowsBleTransport.ManifestBytes.ToArray();
        // tag 0x08 首字节为 class_count。
        badClassCount[FindManifestValueOffset(badClassCount, 0x08)] = 10;
        // 类别数量和 CRC 必须同时匹配。
        AssertManifestRejected(badClassCount, "错误 class_count 未拒绝。" );
        // 保持类别数 11，只破坏 CRC32 低字节。
        byte[] badClassCrc = FakeWindowsBleTransport.ManifestBytes.ToArray();
        // tag 0x08 value 后四字节是小端 CRC，偏移一跳过 class_count。
        badClassCrc[FindManifestValueOffset(badClassCrc, 0x08) + 1] ^= 0x01;
        // 类别顺序 CRC 不匹配必须拒绝。
        AssertManifestRejected(badClassCrc, "错误类别 CRC 未拒绝。" );
        // 能力位移除 LittleFS，只保留振动和历史。
        byte[] badCapabilities = FakeWindowsBleTransport.ManifestBytes.ToArray();
        // tag 0x0A 小端 uint32 低字节写为 3。
        badCapabilities[FindManifestValueOffset(badCapabilities, 0x0A)] = 3;
        // 缺少 PC 必需能力必须拒绝连接。
        AssertManifestRejected(badCapabilities, "缺少必需能力位未拒绝。" );
    }

    // 定位指定 TLV 的 value 起点；测试输入本身应包含完整 TLV。
    private static int FindManifestValueOffset(byte[] manifest, byte wantedTag)
    {
        // offset 每轮指向 tag；遍历到输入末尾为止。
        int offset = 0;
        // 按 tag、length、value 结构跳过每项。
        while (offset < manifest.Length)
        {
            // 当前 tag 位于 offset。
            byte tag = manifest[offset];
            // length 位于 tag 后一字节。
            int length = manifest[offset + 1];
            // value 起点位于 TLV 头后两字节。
            int valueOffset = offset + 2;
            // 找到目标 tag 时返回 value 起点。
            if (tag == wantedTag)
            {
                // 返回可供测试修改的数组索引。
                return valueOffset;
            }

            // 跳过当前完整 TLV。
            offset = valueOffset + length;
        }

        // 黄金向量缺少目标 tag 表示测试数据自身损坏。
        throw new InvalidOperationException($"测试 Manifest 缺少 tag 0x{wantedTag:X2}。" );
    }

    // 断言 Manifest 解析严格抛出协议数据异常。
    private static void AssertManifestRejected(byte[] manifest, string message)
    {
        try
        {
            // 尝试解析破坏后的输入。
            _ = ManifestV1Codec.Parse(manifest);
        }
        catch (InvalidDataException)
        {
            // 收到预期异常即完成本条断言。
            return;
        }

        // 未抛异常表示不兼容设备可能进入控制状态。
        Assert(false, message);
    }

    // 验证选择边界不会自动连接无关 BLE 外设。
    private static async Task TestDeviceSelectionAsync()
    {
        // 创建安全默认选择器。
        FirstFitnessDeviceSelector selector = new();
        // 构造耳机和两块产品手柄扫描结果。
        BleDiscoveredDevice[] devices =
        [
            new BleDiscoveredDevice("headset", "Audio Buds", true),
            new BleDiscoveredDevice("fit-a", "BPNN-FIT-1001", false),
            new BleDiscoveredDevice("fit-b", "BPNN-FIT-1002", true),
        ];
        // 无 preferred ID 时选择第一个产品名前缀设备。
        BleDiscoveredDevice? first = await selector.SelectAsync(devices, null, CancellationToken.None);
        // 断言没有误选耳机。
        Assert(first?.DeviceId == "fit-a", "默认选择器应选择第一个 BPNN-FIT 产品设备。" );
        // 指定旧设备时优先该 ID，即使它不是列表中第一个产品。
        BleDiscoveredDevice? preferred = await selector.SelectAsync(devices, "fit-b", CancellationToken.None);
        // 断言自动重连锁定旧设备。
        Assert(preferred?.DeviceId == "fit-b", "自动重连应优先上次 Windows 设备 ID。" );
        // 只有无关设备时必须返回 null。
        BleDiscoveredDevice? none = await selector.SelectAsync([devices[0]], null, CancellationToken.None);
        // 断言不误配其它 BLE 外设。
        Assert(none is null, "默认选择器不得自动连接非 BPNN-FIT 外设。" );
    }

    // 验证 Windows 风格设备列表、默认产品选择和按选中设备连接。
    private static async Task TestVisibleDiscoveryAndSelectedConnectAsync()
    {
        // 创建带两台附近蓝牙设备的 fake transport，第一台是无关耳机，第二台是健身手柄。
        FakeWindowsBleTransport transport = new(CreateState(revision: 4U, FitnessDeviceState.Idle));
        // 创建正式 Windows BLE 会话，设备页必须通过同一会话连接，不能复制协议状态机。
        await using WindowsBleDeviceSession session = CreateSession(transport);
        // 使用同步调度器，让连接事件和列表属性在测试线程直接提交。
        ImmediateUiDispatcher dispatcher = new();
        // 创建设备页视图模型，验证真正暴露给 WPF 的集合和命令。
        using DeviceViewModel viewModel = new(session, dispatcher);

        // 执行用户可见的“扫描设备”命令。
        await viewModel.ScanDevicesCommand.ExecuteAsync();
        // 取得自动选中的产品行；缺失时立即抛出明确测试错误并满足可空分析。
        NearbyBluetoothDeviceRow selectedRow = viewModel.SelectedNearbyDevice
            ?? throw new InvalidOperationException("扫描后没有选中任何附近蓝牙设备。");
        // 列表必须保留附近耳机供用户辨认，同时自动选中产品名前缀手柄。
        Assert(
            viewModel.NearbyDevices.Count == 2 &&
            selectedRow.DeviceId == FakeWindowsBleTransport.WindowsDeviceId &&
            selectedRow.IsFitnessDevice,
            "设备页没有显示完整附近设备列表，或没有优先选中 BPNN-FIT 手柄。");
        // 手柄行必须显示真实 RSSI 中文单位和未配对状态，禁止伪造已配对。
        Assert(
            selectedRow.SignalText == "-47 分贝毫瓦" &&
            selectedRow.PairingText == "未配对",
            "设备列表没有显示真实信号强度或配对状态。");

        // 执行“连接所选手柄”，不得回退为隐藏式随机连接。
        await viewModel.ConnectSelectedCommand.ExecuteAsync();
        // 会话必须连接，并把用户选中的 Windows DeviceInformation.Id 传给 transport。
        Assert(
            session.IsConnected &&
            transport.PreferredDeviceIds.LastOrDefault() == FakeWindowsBleTransport.WindowsDeviceId,
            "连接所选手柄没有锁定列表中的 Windows 设备 ID。");
    }

    // 验证连接固定顺序和初始快照。
    private static async Task TestConnectAndSnapshotAsync()
    {
        // 创建初始 revision 5 的 fake transport。
        FakeWindowsBleTransport transport = new(CreateState(revision: 5, FitnessDeviceState.Idle));
        // 创建短测试超时会话，重连测试在其它用例执行。
        await using WindowsBleDeviceSession session = CreateSession(transport);
        // 收集连接事件原因。
        List<DeviceConnectionChangedEventArgs> connectionEvents = [];
        // 订阅连接事件。
        session.ConnectionChanged += (_, eventArgs) => connectionEvents.Add(eventArgs);
        // 扫描连接并恢复 Manifest/快照。
        await session.ConnectAsync();
        // 读取会话缓存快照。
        LiveState snapshot = await session.GetSnapshotAsync();

        // 断言完整会话连接成功。
        Assert(session.IsConnected, "Manifest、订阅和快照完成后会话应为已连接。" );
        // 断言真机标志供 WPF 明确显示。
        Assert(session.IsHardwareBacked, "WindowsBleDeviceSession 必须标记为真实硬件会话。" );
        // 断言设备名来自扫描结果。
        Assert(session.DeviceId == "BPNN-FIT-FAKE", "设备 ID 应使用真机显示名。" );
        // 断言 Manifest 被读取并保存。
        Assert(session.Manifest.Span.SequenceEqual(FakeWindowsBleTransport.ManifestBytes), "连接必须读取 Manifest。" );
        // 连接完成后自动校时会使 fake 权威 revision 从 5 增至 6。
        Assert(snapshot.StateRevision == 6, "连接后应自动校时并读取 revision 6 权威快照。" );
        // 读取领域诊断快照。
        DeviceDiagnosticsSnapshot diagnostics = session.GetDiagnosticsSnapshot();
        // 核对 fake transport 提供的标准设备信息、RSSI、MTU、电量和 revision。
        Assert(
            diagnostics.ModelNumber == "ESP32-S3-AMOLED-2.06" &&
            diagnostics.HardwareRevision == "2.06" &&
            diagnostics.FirmwareRevision == "0.1.0" &&
            diagnostics.RssiDbm == -47 &&
            diagnostics.AttMtu == 23 &&
            diagnostics.BatteryPercent == 88 &&
            diagnostics.StateRevision == 6 &&
            diagnostics.ManifestSummary.Contains("基础模型摘要=8f66e344bcfa", StringComparison.Ordinal) &&
            diagnostics.ManifestSummary.Contains("能力标志=0x00000007", StringComparison.Ordinal) &&
            diagnostics.ManifestSummary.Contains("内部文件系统可用量=72623859790382856 字节", StringComparison.Ordinal),
            "连接诊断快照字段错误。" );
        // 断言控制 indication、状态和事件通知均已订阅。
        Assert(transport.Subscriptions.SequenceEqual(
            [
                (ProtocolConstants.ControlPointUuid, true),
                (ProtocolConstants.LiveStateUuid, false),
                (ProtocolConstants.EventUuid, false),
                // 会话同步响应必须使用 indication。
                (ProtocolConstants.TransferControlUuid, true),
                // 会话摘要页使用 notification。
                (ProtocolConstants.TransferDataUuid, false),
                // 开发者六轴同步诊断流使用 notification，默认命令 11 保持关闭。
                (ProtocolConstants.RawStreamUuid, false),
            ]), "连接订阅顺序或 indication/notification 模式错误。" );
        // 连接提交 UI 前必须已经发送一次命令 6 自动校时。
        Assert(transport.ControlCommands.Count == 1 && transport.ControlCommands[0].CommandId == ControlCommandId.SyncTime, "连接未自动执行命令 6 校时。" );
        // 断言只发布一次成功连接事件。
        Assert(connectionEvents.Count == 1 && connectionEvents[0].IsConnected, "首次连接应发布一次已连接事件。" );
    }

    // 验证五类设备配置都走控制点 ACK，并且关闭 RawStream 后不再发布样本。
    private static async Task TestConfigurationAndRawStreamAsync()
    {
        // 创建可响应全部配置命令的 fake transport。
        FakeWindowsBleTransport transport = new(CreateState(revision: 8, FitnessDeviceState.Idle));
        // 创建短超时真 BLE 状态机。
        await using WindowsBleDeviceSession session = CreateSession(transport);
        // 建立连接；内部先执行一次命令 6 自动校时。
        await session.ConnectAsync();
        // 下发 65kg 和资料 revision 7。
        await session.SetProfileAsync(65_000U, 7U);
        // 下发 300kcal，协议使用 300000 milli-kcal。
        await session.SetGoalAsync(DeviceGoalKind.CaloriesMilliKcal, 300_000U);
        // 打开开发者模式并保存设备偏好。
        await session.SetPreferencesAsync(new DevicePreferencesV1(35, true, false, 30, 9U, true));
        // 显式打开 RawStream。
        await session.SetRawStreamEnabledAsync(true);

        // 记录收到的完整 22 字节样本。
        List<RawImuSampleV1> samples = [];
        // 记录收到的完整 28 字节双 M0 分类诊断。
        List<InferenceDiagnosticV1> diagnostics = [];
        // 订阅后台 RawStream 事件。
        session.RawSampleReceived += (_, eventArgs) => samples.Add(eventArgs.Sample);
        // 订阅同一 UUID 上的类型九分类事件。
        session.InferenceDiagnosticReceived += (_, eventArgs) => diagnostics.Add(eventArgs.Diagnostic);
        // 发布确定性 sample_index=1、monotonic_ms=2、六轴 3..8 和质量位 9。
        transport.EmitRawStream(new RawImuSampleV1(1U, 2U, 3, 4, 5, 6, 7, 8, 9));
        // 等待通知泵完成 0007 重组与解码。
        await WaitUntilAsync(() => samples.Count == 1, "RawStream 0007 样本未发布。" );
        // 核对通道顺序和质量位。
        Assert(samples[0].GxRaw == 3 && samples[0].AzRaw == 8 && samples[0].QualityFlags == 9U, "RawStream 六轴字段错位。" );
        // 发布一个融合深蹲、基础深蹲、掩码跳跃深蹲的分类窗口。
        transport.EmitInferenceDiagnostic(new InferenceDiagnosticV1(
            1,
            ActionId.Squat,
            ActionId.Squat,
            ActionId.JumpingSquat,
            0.80,
            0.75,
            0.70,
            0,
            12U,
            2480U,
            18400U,
            0U));
        // 等待通知泵按类型九解码同一 UUID 的独立逻辑帧。
        await WaitUntilAsync(() => diagnostics.Count == 1, "分类诊断 0007 窗口未发布。" );
        // 核对动作、耗时和窗口序号没有被 RawStream 分支错读。
        Assert(
            diagnostics[0].FusedAction == ActionId.Squat &&
            diagnostics[0].MaskedAction == ActionId.JumpingSquat &&
            diagnostics[0].WindowSequence == 12U &&
            diagnostics[0].InferenceMicroseconds == 18400U,
            "分类诊断类型九字段错位。" );
        // 打开命令 ACK 后属性必须为 true。
        Assert(session.IsRawStreamEnabled, "命令 11 成功后 RawStream 状态未开启。" );

        // 显式关闭 RawStream。
        await session.SetRawStreamEnabledAsync(false);
        // 关闭后即使收到迟到逻辑帧也不得发布或保存。
        transport.EmitRawStream(new RawImuSampleV1(2U, 3U, 4, 5, 6, 7, 8, 9, 10));
        // 给通知泵短暂时间消费迟到帧。
        await Task.Delay(20);
        // 样本数量必须保持一条。
        Assert(samples.Count == 1 && diagnostics.Count == 1 && !session.IsRawStreamEnabled, "关闭 RawStream 后仍发布六轴或分类诊断。" );

        // 自动校时之后的命令顺序必须固定为 7、8、9、11开、11关。
        ControlCommandId[] commandIds = transport.ControlCommands.Select(entry => entry.CommandId).ToArray();
        // 核对全链路控制命令顺序。
        Assert(commandIds.SequenceEqual([ControlCommandId.SyncTime, ControlCommandId.SetProfile, ControlCommandId.SetGoal, ControlCommandId.SetPreferences, ControlCommandId.SetRawStream, ControlCommandId.SetRawStream]), "配置命令顺序错误。" );
        // 核对关键 TLV 与纯 codec 生成值完全相等。
        Assert(transport.ControlCommands[1].Tlv.SequenceEqual(DeviceConfigurationCodec.EncodeProfile(65_000U, 7U)), "命令 7 TLV 在控制点传输中漂移。" );
        // 核对命令 11 关闭字节。
        Assert(transport.ControlCommands[^1].Tlv.SequenceEqual(DeviceConfigurationCodec.EncodeRawStreamEnabled(false)), "命令 11 关闭 TLV 漂移。" );
    }

    // 验证解析失败不能留下半连接、通知订阅或可控制状态。
    private static async Task TestConnectionRejectsIncompatibleManifestAsync()
    {
        // 创建初始空闲 fake transport。
        FakeWindowsBleTransport transport = new(CreateState(revision: 6, FitnessDeviceState.Idle));
        // 复制黄金向量并把协议主版本改为 2。
        byte[] incompatibleManifest = FakeWindowsBleTransport.ManifestBytes.ToArray();
        // tag 0x03 value 首字节为协议主版本。
        incompatibleManifest[FindManifestValueOffset(incompatibleManifest, 0x03)] = 2;
        // 让本次连接读取不兼容 Manifest。
        transport.ManifestValue = incompatibleManifest;
        // 创建真实会话状态机。
        await using WindowsBleDeviceSession session = CreateSession(transport);
        // 记录是否收到预期 InvalidDataException。
        bool rejected = false;

        try
        {
            // 发起完整连接；解析应在任何 SubscribeAsync 之前失败。
            await session.ConnectAsync();
        }
        catch (InvalidDataException)
        {
            // 标记协议不兼容已被连接层拒绝。
            rejected = true;
        }

        // 必须向调用方返回明确拒绝异常。
        Assert(rejected, "连接未拒绝不兼容 Manifest。" );
        // 会话和 transport 均必须恢复断开状态。
        Assert(!session.IsConnected && !transport.IsConnected, "Manifest 拒绝后链路未释放。" );
        // 解析发生在订阅前，任何通知订阅都不应建立。
        Assert(transport.Subscriptions.Count == 0, "Manifest 拒绝后仍建立了通知订阅。" );
    }

    // 验证设备 revision 是 PC 唯一状态覆盖依据。
    private static async Task TestAuthoritativeRevisionAndEventAsync()
    {
        // 创建初始 revision 10 transport。
        FakeWindowsBleTransport transport = new(CreateState(revision: 10, FitnessDeviceState.Running));
        // 创建会话。
        await using WindowsBleDeviceSession session = CreateSession(transport);
        // 记录连接后新增状态 revision。
        List<uint> revisions = [];
        // 记录协议 Event 数量。
        int protocolEventCount = 0;
        // 订阅状态事件。
        session.StateChanged += (_, state) => revisions.Add(state.StateRevision);
        // 订阅低延迟事件。
        session.ProtocolEventReceived += (_, _) => protocolEventCount++;
        // 建立连接，初始基线会产生 revision 10。
        await session.ConnectAsync();
        // 清除初始基线事件，只观察通知更新。
        revisions.Clear();
        // 推送重复 revision 10，必须忽略。
        transport.EmitLiveState(CreateState(revision: 10, FitnessDeviceState.Paused));
        // 推送更旧 revision 9，必须忽略。
        transport.EmitLiveState(CreateState(revision: 9, FitnessDeviceState.Paused));
        // 推送 revision 11，必须接受。
        transport.EmitLiveState(CreateState(revision: 11, FitnessDeviceState.Paused));
        // 构造合法 EventV1，事件只触发动画，不能自行增加次数或产生 StateChanged。
        DeviceEventV1 deviceEvent = new(
            1,
            DeviceEventType.RepetitionCounted,
            FitnessDeviceState.Paused,
            ActionId.JumpingSquat,
            MetricKind.Repetition,
            88,
            0,
            1,
            7,
            11,
            1,
            12,
            2345,
            0x7FFF,
            0);
        // 按固定 36 字节合同发布 Event notification。
        transport.EmitProtocolEvent(EventV1Codec.Encode(deviceEvent));
        // 等待后台 Channel 消费四条通知。
        await WaitUntilAsync(() => revisions.Count == 1 && protocolEventCount == 1, "BLE 通知泵未按时处理 revision/Event。" );
        // 断言只接受严格更大的 revision。
        Assert(revisions.SequenceEqual([11u]), "重复或旧 revision 不得覆盖设备权威状态。" );
        // 读取快照；fake 当前 revision 11。
        LiveState snapshot = await session.GetSnapshotAsync();
        // 断言设备状态为 Paused。
        Assert(snapshot.DeviceState == FitnessDeviceState.Paused, "最新权威状态应来自 revision 11。" );
    }

    // 验证控制超时只重试一次且 request_id 不变。
    private static async Task TestControlRetryUsesSameRequestIdAsync()
    {
        // 创建空闲 fake transport 并配置丢弃首次完整控制请求响应。
        FakeWindowsBleTransport transport = new(CreateState(revision: 20, FitnessDeviceState.Idle));
        // 创建 15ms 控制超时会话。
        await using WindowsBleDeviceSession session = CreateSession(transport);
        // 建立连接。
        await session.ConnectAsync();
        // 自动校时已成功；现在才丢弃 Start 的首次 indication。
        transport.DropFirstControlResponse = true;
        // 记录 Start 前控制帧和 request_id 数量。
        int framesBeforeStart = transport.CompletedControlFrames;
        // 记录 Start 前已存在的自动校时 request_id。
        int idsBeforeStart = transport.ControlRequestIds.Count;
        // 开始会话；内部必须超时后仅重试一次。
        await session.StartAsync();

        // 断言 fake 收到两次完整逻辑控制请求。
        Assert(transport.ControlRequestIds.Count == idsBeforeStart + 2, "丢失首次 indication 时应额外发送两次控制请求。" );
        // 断言两次 request_id 完全相同，设备缓存可保证幂等。
        Assert(transport.ControlRequestIds[^2] == transport.ControlRequestIds[^1], "控制重试必须复用同一 request_id。" );
        // 断言命令没有发送第三次。
        Assert(transport.CompletedControlFrames == framesBeforeStart + 2, "控制命令最多允许一次重试。" );
        // 断言设备最终状态由快照恢复为 Running。
        LiveState snapshot = await session.GetSnapshotAsync();
        // 检查状态而非 PC 本地猜测。
        Assert(snapshot.DeviceState == FitnessDeviceState.Running, "Start ACK 后应读取 Running 权威快照。" );

        // 记录并发停止前完整控制帧数量。
        int framesBeforeStop = transport.CompletedControlFrames;
        // 并发发起两个 Stop，状态机必须把整个停止事务串行化。
        Task<TrainingSessionSummary?> firstStop = session.StopAsync();
        // 第二个调用应等待首个摘要生成后直接复用。
        Task<TrainingSessionSummary?> secondStop = session.StopAsync();
        // 等待两个停止调用完成。
        TrainingSessionSummary?[] summaries = await Task.WhenAll(firstStop, secondStop);
        // 断言只发送一条 Stop 控制请求。
        Assert(transport.CompletedControlFrames == framesBeforeStop + 1, "并发 Stop 只能发送一条控制请求。" );
        // 断言两个调用返回同一摘要实例。
        Assert(summaries[0] is not null && ReferenceEquals(summaries[0], summaries[1]), "并发 Stop 应返回同一幂等摘要。" );
    }

    // 验证意外断线自动使用退避并重新读取 manifest 和 snapshot。
    private static async Task TestAutomaticReconnectAsync()
    {
        // 创建初始 revision 30 transport。
        FakeWindowsBleTransport transport = new(CreateState(revision: 30, FitnessDeviceState.Running));
        // 记录自动重连使用的短测试退避。
        List<TimeSpan> observedReconnectDelays = [];
        // 创建自定义延迟：控制超时保持真实等待，1/2ms 重连立即完成并记录。
        Task DelayAsync(TimeSpan delay, CancellationToken cancellationToken)
        {
            // 小于 10ms 的值属于本测试重连退避。
            if (delay < TimeSpan.FromMilliseconds(10))
            {
                // 记录实际退避值。
                observedReconnectDelays.Add(delay);
                // 检查取消后立即完成，避免测试墙钟等待。
                cancellationToken.ThrowIfCancellationRequested();
                // 返回完成任务。
                return Task.CompletedTask;
            }

            // 控制超时仍使用真正 Task.Delay。
            return Task.Delay(delay, cancellationToken);
        }

        // 创建带 1/2ms 测试退避的会话。
        await using WindowsBleDeviceSession session = new(
            transport,
            TimeSpan.FromMilliseconds(20),
            [TimeSpan.FromMilliseconds(1), TimeSpan.FromMilliseconds(2)],
            DelayAsync);
        // 记录连接/断开事件。
        List<bool> connectionStates = [];
        // 订阅事件状态。
        session.ConnectionChanged += (_, eventArgs) => connectionStates.Add(eventArgs.IsConnected);
        // 建立首次连接。
        await session.ConnectAsync();
        // 断线前让设备内部权威 revision 变为 4，模拟 ESP32 重启后 revision 从较小值重新开始。
        transport.SetCurrentState(CreateState(revision: 4, FitnessDeviceState.Running));
        // 触发 Windows 意外链路丢失。
        transport.TriggerLinkLoss();
        // 等待自动重连、第二次 Manifest 和快照读取完成。
        await WaitUntilAsync(() => transport.ConnectCount >= 2 && session.IsConnected, "自动重连未在测试退避后完成。" );
        // 读取重连强制基线快照；较小 revision 4 必须被接受。
        LiveState snapshot = await session.GetSnapshotAsync();

        // 断言至少使用第一档 1ms 退避。
        Assert(observedReconnectDelays.Count >= 1 && observedReconnectDelays[0] == TimeSpan.FromMilliseconds(1), "重连第一档退避应为配置的 1ms。" );
        // 断言重连固定同一 preferred Windows 设备 ID。
        Assert(transport.PreferredDeviceIds.Count >= 2 && transport.PreferredDeviceIds[1] == FakeWindowsBleTransport.WindowsDeviceId, "自动重连应优先同一 Windows 设备 ID。" );
        // 断言 Manifest 每次连接都刷新。
        Assert(transport.ManifestReadCount >= 2, "自动重连必须重新读取 Manifest。" );
        // 断言较小 revision 作为新连接/boot 基线被接受。
        Assert(snapshot.StateRevision == 5, "重连初始快照应重建 revision 4 基线并自动校时到 5。" );
        // 自动重连至少真正发起一次，诊断计数必须增加。
        Assert(session.GetDiagnosticsSnapshot().ReconnectCount >= 1U, "自动重连诊断计数未增加。" );
        // 断言事件顺序至少包含连接、断开、重连。
        Assert(connectionStates.Count >= 3 && connectionStates[0] && !connectionStates[1] && connectionStates[^1], "连接事件应包含 true、false、true 恢复序列。" );

        // 显式释放会话，后续断言 transport 生命周期。
        await session.DisposeAsync();
        // 断言 transport 已释放且没有残留连接。
        Assert(transport.Disposed && !transport.IsConnected, "Dispose 应释放 fake transport 和连接。" );
    }

    // 验证坏 CRC 和坏分片分别进入独立计数，且通知泵继续运行。
    private static async Task TestDiagnosticsSnapshotAndProtocolCountersAsync()
    {
        // 创建带合法初始快照的 fake transport。
        FakeWindowsBleTransport transport = new(CreateState(revision: 50, FitnessDeviceState.Running));
        // 创建待测真 BLE 会话状态机。
        await using WindowsBleDeviceSession session = CreateSession(transport);
        // 建立连接。
        await session.ConnectAsync();
        // 编码一个完整合法 LiveState 帧。
        byte[] corruptedFrame = transport.CreateLiveStateFrame(CreateState(revision: 51, FitnessDeviceState.Running));
        // 翻转 payload 字节但保留旧 CRC，制造确定性 BadCrc。
        corruptedFrame[ProtocolConstants.LogicalHeaderSize] ^= 0x01;
        // 发布损坏完整帧。
        transport.EmitRaw(ProtocolConstants.LiveStateUuid, corruptedFrame);
        // 发布 1 字节非法分片，重组器应返回 TooShort 并计入分片错误。
        transport.EmitRaw(ProtocolConstants.LiveStateUuid, [0x00]);
        // 等待后台通知泵处理两个值。
        await WaitUntilAsync(
            () =>
            {
                // 每次轮询读取线程安全诊断快照。
                DeviceDiagnosticsSnapshot snapshot = session.GetDiagnosticsSnapshot();
                // 两类计数均达到 1 即完成。
                return snapshot.CrcErrorCount >= 1U && snapshot.FragmentErrorCount >= 1U;
            },
            "CRC/分片错误未进入诊断计数。" );
        // 发布后续合法 revision，证明通知泵没有因错误永久退出。
        transport.EmitLiveState(CreateState(revision: 52, FitnessDeviceState.Paused));
        // 等待合法状态被接受。
        await WaitUntilAsync(() => session.GetDiagnosticsSnapshot().StateRevision == 52U, "协议错误后通知泵未继续接收合法状态。" );
    }

    // 验证 Windows 扫描等待可由调用方取消，随后对象仍能完整释放。
    private static async Task TestCancellationAndReleaseAsync()
    {
        // 创建会在扫描阶段等待取消的 fake transport。
        FakeWindowsBleTransport transport = new(CreateState(revision: 1, FitnessDeviceState.Idle))
        {
            // 模拟用户关闭选择窗口或应用退出时尚未完成系统扫描。
            WaitForConnectCancellation = true,
        };
        // 创建真 BLE 会话状态机。
        WindowsBleDeviceSession session = CreateSession(transport);
        // 创建短取消源，终止阻塞扫描。
        using CancellationTokenSource cancellation = new(TimeSpan.FromMilliseconds(20));
        // 记录是否收到预期取消异常。
        bool cancelled = false;

        try
        {
            // 启动可取消连接。
            await session.ConnectAsync(cancellation.Token);
        }
        catch (OperationCanceledException)
        {
            // 标记取消已从 transport 传播到会话调用者。
            cancelled = true;
        }

        // 断言没有把取消错误转换为普通连接失败或吞掉。
        Assert(cancelled, "扫描/选择取消应传播 OperationCanceledException。" );
        // 断言取消后没有提交连接状态。
        Assert(!session.IsConnected, "取消扫描不得把会话标记为已连接。" );
        // 释放会话，验证取消后生命周期锁已经释放。
        await session.DisposeAsync();
        // 断言 transport Dispose 完成。
        Assert(transport.Disposed, "取消连接后 Dispose 仍必须释放 transport。" );
    }

    // 验证用户在设置页选择真 BLE 后，下次启动可以从 JSON 恢复该模式。
    private static async Task TestRealBlePreferenceRoundTripAsync()
    {
        // 使用项目本地临时目录，避免测试在用户 AppData 或系统 TEMP 留文件。
        string directory = Path.Combine(Directory.GetCurrentDirectory(), ".codex-local", "tmp", "windows_ble_preference_test");
        // 创建测试目录。
        Directory.CreateDirectory(directory);
        // 使用固定文件名，测试开始先删除旧失败残留。
        string filePath = Path.Combine(directory, "preferences.json");
        // 删除此前中断测试的旧设置。
        File.Delete(filePath);

        try
        {
            // 创建原子 JSON 设置仓储。
            using JsonUserPreferencesStore store = new(filePath);
            // 创建可验证设备配置同步的 Mock 会话。
            await using MockDeviceSession device = new(TimeSpan.FromMilliseconds(20), [ActionId.Squat]);
            // 先建立设备连接，设置页保存后应立即同步四类配置。
            await device.ConnectAsync();
            // 创建设置页 ViewModel 并注入当前设备会话。
            SettingsViewModel viewModel = new(store, animationPreferences: null, deviceSession: device);
            // 选择下次启动使用真 BLE。
            viewModel.UseRealBleDevice = true;
            // 显式允许诊断页 RawStream。
            viewModel.DeveloperModeEnabled = true;
            // 执行与 WPF 保存按钮相同的异步命令。
            await viewModel.SaveCommand.ExecuteAsync();
            // 从磁盘重新加载，模拟应用重启。
            UserPreferences reloaded = await store.LoadAsync();
            // 断言模式选择已持久化。
            Assert(reloaded.UseRealBleDevice, "设置页应持久化下次启动使用真实 BLE。" );
            // 断言开发者开关和两个 revision 均已持久化。
            Assert(reloaded.DeveloperModeEnabled && reloaded.ProfileRevision == 1U && reloaded.PreferencesRevision == 1U, "设备配置开关或 revision 未持久化。" );
            // 断言用户提示明确模式在下次启动生效。
        Assert(viewModel.StatusMessage.Contains("下次启动使用真机蓝牙", StringComparison.Ordinal), "设置保存提示应明确下次启动模式。" );
            // 设备四条配置均成功 ACK 后才允许显示同步成功。
            Assert(viewModel.StatusMessage.Contains("设备时间、体重、目标和偏好已同步成功", StringComparison.Ordinal), "设置页未区分本地保存与设备同步成功。" );
            // 设置页已同步开发者模式，命令 11 现在应可打开。
            await device.SetRawStreamEnabledAsync(true);
            // 核对 Mock 设备接受开发者流。
            Assert(device.IsRawStreamEnabled, "设置页开发者模式未同步到设备。" );
        }
        finally
        {
            // 删除临时设置文件，测试不在项目留下用户数据。
            File.Delete(filePath);
            // 目录存在且为空时删除；异常文件存在时保留供诊断。
            if (Directory.Exists(directory) && !Directory.EnumerateFileSystemEntries(directory).Any())
            {
                // 删除空测试目录。
                Directory.Delete(directory);
            }
        }
    }

    // 创建生产状态机但使用短测试超时和退避。
    private static WindowsBleDeviceSession CreateSession(FakeWindowsBleTransport transport)
    {
        // 使用 15ms 控制超时，重连退避使用短值但本函数不触发重连。
        return new WindowsBleDeviceSession(
            transport,
            TimeSpan.FromMilliseconds(15),
            [TimeSpan.FromMilliseconds(1), TimeSpan.FromMilliseconds(2)]);
    }

    // 创建确定性 LiveState。
    private static LiveState CreateState(uint revision, FitnessDeviceState deviceState)
    {
        // 使用会话 7、jumping_squat、12 次和 4.2 kcal，便于跨快照断言。
        return new LiveState(
            7,
            revision,
            12_000,
            deviceState,
            ActionId.JumpingSquat,
            MetricKind.Repetition,
            88,
            12,
            60_000,
            4_200,
            DataQualityFlags.None,
            PowerFlags.None,
            40);
    }

    // 最多等待两秒让后台 Channel 或重连任务完成。
    private static async Task WaitUntilAsync(Func<bool> condition, string failureMessage)
    {
        // 记录单调截止时间，系统 UTC 变化不影响测试超时。
        long deadline = Environment.TickCount64 + 2_000;

        // 条件未满足且未到截止时间时短暂让出线程。
        while (!condition() && (Environment.TickCount64 < deadline))
        {
            // 5ms 足够后台任务调度且不会拖慢测试。
            await Task.Delay(5);
        }

        // 最终检查条件并给出调用方错误文本。
        Assert(condition(), failureMessage);
    }

    // 无第三方测试框架断言。
    private static void Assert(bool condition, string message)
    {
        // 条件失败时抛出明确异常，Program.Main 转换为非零退出码。
        if (!condition)
        {
            // 抛出测试失败原因。
            throw new InvalidOperationException(message);
        }
    }

    // fake transport 模拟实际 MTU23 分片、控制 indication、通知和断线，不依赖蓝牙硬件。
    private sealed class FakeWindowsBleTransport : IWindowsBleTransport, IWindowsBlePairingManager, IWindowsBleDiscoveryTransport
    {
        // fake Windows 设备 ID 用于验证 preferred 重连。
        public const string WindowsDeviceId = "BluetoothLE#FAKE-0001";
        // 固定 132 字节黄金 Manifest 与固件正式 TLV tag、长度、小端和模型摘要合同一致。
        public static readonly byte[] ManifestBytes =
        [
            0x01, 0x0C, 0x41, 0x31, 0x42, 0x32, 0x43, 0x33, 0x44, 0x34, 0x45, 0x35, 0x46, 0x36,
            0x02, 0x04, 0x32, 0x2E, 0x30, 0x36, 0x03, 0x02, 0x01, 0x00, 0x04, 0x05, 0x30, 0x2E, 0x31, 0x2E, 0x30,
            0x05, 0x04, 0x29, 0x01, 0x01, 0x00, 0x06, 0x20, 0x8F, 0x66, 0xE3, 0x44, 0xBC, 0xFA, 0xAF, 0xB8,
            0x29, 0x9A, 0x1C, 0xCD, 0xBE, 0x18, 0x96, 0x51, 0xB4, 0x57, 0xE2, 0x6B, 0xE7, 0xE7, 0x06, 0x04,
            0xC5, 0x27, 0xE9, 0xDE, 0x2D, 0xCA, 0x8F, 0x27, 0x07, 0x20, 0x57, 0xF4, 0xB2, 0xBC, 0xA0, 0x5D,
            0xB7, 0x50, 0xCA, 0x73, 0x4E, 0x67, 0x4E, 0x20, 0x38, 0x12, 0xBE, 0xC1, 0xEA, 0x88, 0x4F, 0x16,
            0xA2, 0x01, 0x8D, 0xDF, 0x2E, 0x2C, 0xC7, 0xED, 0x5B, 0x3B, 0x08, 0x05, 0x0B, 0x27, 0x39, 0x19,
            0xD8, 0x09, 0x02, 0x01, 0x00, 0x0A, 0x04, 0x07, 0x00, 0x00, 0x00, 0x0B, 0x08, 0x08, 0x07, 0x06,
            0x05, 0x04, 0x03, 0x02, 0x01,
        ];
        // 当前 fake 返回的 Manifest；异常连接测试可替换为破坏后的独立副本。
        public byte[] ManifestValue { get; set; } = ManifestBytes.ToArray();
        // 控制点写入分片重组器按 MTU23 还原完整逻辑帧。
        private readonly BleFrameReassembler _controlRequestReassembler = new();
        // 当前设备权威状态；PC 断线不清空。
        private LiveState _currentState;
        // 逻辑通知 sequence。
        private ushort _notificationSequence;
        // transport 连接状态。
        private bool _isConnected;
        // dispose 标志。
        private bool _disposed;
        // 是否已经执行首次控制响应丢弃。
        private bool _firstResponseDropped;
        // 最近已经执行的 request_id，用于模拟设备端缓存幂等响应。
        private uint _lastAppliedRequestId;

        /// <summary>创建带初始状态的 fake。</summary>
        public FakeWindowsBleTransport(LiveState initialState)
        {
            // 保存不可变初始状态对象。
            _currentState = initialState;
        }

        /// <inheritdoc />
        public event EventHandler<BleGattValueReceivedEventArgs>? ValueReceived;

        /// <inheritdoc />
        public event EventHandler<BleTransportDisconnectedEventArgs>? Disconnected;

        /// <inheritdoc />
        public bool IsConnected => _isConnected;

        /// <summary>true 时只丢弃第一次完整控制请求的 indication。</summary>
        public bool DropFirstControlResponse { get; set; }

        /// <summary>true 时扫描一直等待调用方取消，用于验证连接取消和释放。</summary>
        public bool WaitForConnectCancellation { get; set; }

        /// <summary>收到的完整控制请求数量，不按分片计数。</summary>
        public int CompletedControlFrames { get; private set; }

        /// <summary>完整控制请求中的 request_id 序列。</summary>
        public List<uint> ControlRequestIds { get; } = [];

        /// <summary>按接收顺序保存命令 ID 和独立 TLV 副本，供配置黄金链路断言。</summary>
        public List<(ControlCommandId CommandId, byte[] Tlv)> ControlCommands { get; } = [];

        /// <summary>每次扫描收到的 preferredDeviceId。</summary>
        public List<string?> PreferredDeviceIds { get; } = [];

        /// <summary>按调用顺序保存会话请求取消配对的 Windows 设备 ID。</summary>
        public List<string> ForgottenDeviceIds { get; } = [];

        /// <summary>成功扫描连接次数。</summary>
        public int ConnectCount { get; private set; }

        /// <summary>Manifest 读取次数。</summary>
        public int ManifestReadCount { get; private set; }

        /// <summary>订阅 UUID 和 indication 布尔值顺序。</summary>
        public List<(Guid Uuid, bool Indicate)> Subscriptions { get; } = [];

        /// <summary>Dispose 是否完成。</summary>
        public bool Disposed => _disposed;

        /// <inheritdoc />
        public Task<IReadOnlyList<BleDiscoveredDevice>> ScanDevicesAsync(TimeSpan scanDuration, CancellationToken cancellationToken)
        {
            // 检查测试取消信号，保持与 WinRT 主动扫描的可取消合同一致。
            cancellationToken.ThrowIfCancellationRequested();
            // 扫描时间必须为正值，避免生产实现形成无限忙循环。
            if (scanDuration <= TimeSpan.Zero)
            {
                // 非正扫描窗口属于调用方配置错误。
                throw new ArgumentOutOfRangeException(nameof(scanDuration));
            }

            // 返回无关耳机和产品手柄，验证界面显示全部候选但只连接产品设备。
            IReadOnlyList<BleDiscoveredDevice> devices =
            [
                // 已配对耳机信号较强，但不得自动选为健身设备。
                new BleDiscoveredDevice("BluetoothLE#HEADSET", "蓝牙耳机", true, -38),
                // 未配对手柄使用 fake 正式连接 ID，RSSI 为 -47 分贝毫瓦。
                new BleDiscoveredDevice(WindowsDeviceId, "BPNN-FIT-FAKE", false, -47),
            ];
            // 返回只读列表；调用方必须复制到自己的 UI 集合。
            return Task.FromResult(devices);
        }

        /// <inheritdoc />
        public async Task<BleConnectedDevice> ScanAndConnectAsync(string? preferredDeviceId, CancellationToken cancellationToken)
        {
            // 检查取消和生命周期。
            cancellationToken.ThrowIfCancellationRequested();
            // 释放后拒绝连接。
            ObjectDisposedException.ThrowIf(_disposed, this);
            // 记录会话层是否锁定旧设备。
            PreferredDeviceIds.Add(preferredDeviceId);
            // 测试配置要求扫描保持等待，直到用户取消或应用释放。
            if (WaitForConnectCancellation)
            {
                // 使用无限可取消延迟模拟 WinRT DeviceInformation 扫描/选择等待。
                await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            }

            // 增加成功连接次数。
            ConnectCount++;
            // 标记 fake 链路连接。
            _isConnected = true;
            // 每次连接清除旧控制半帧。
            _controlRequestReassembler.Reset();
            // 返回 MTU23，强制测试所有控制请求经过多分片路径。
            return new BleConnectedDevice(
                WindowsDeviceId,
                "BPNN-FIT-FAKE",
                23,
                -47,
                "ESP32-S3-AMOLED-2.06",
                "2.06",
                "0.1.0");
        }

        /// <inheritdoc />
        public Task<byte[]> ReadAsync(Guid characteristicUuid, CancellationToken cancellationToken)
        {
            // 检查连接和取消。
            EnsureConnected(cancellationToken);
            // Manifest 返回原始 TLV 副本。
            if (characteristicUuid == ProtocolConstants.ManifestUuid)
            {
                // 统计每次连接均刷新 Manifest。
                ManifestReadCount++;
                // 返回独立副本。
                return Task.FromResult(ManifestValue.ToArray());
            }

            // LiveState 读取返回完整逻辑帧，模拟 WinRT long read。
            if (characteristicUuid == ProtocolConstants.LiveStateUuid)
            {
                // 编码当前权威状态。
                return Task.FromResult(EncodeLiveStateFrame(_currentState));
            }

            // 其它特征当前测试不支持读取。
            throw new KeyNotFoundException($"fake 不支持读取特征 {characteristicUuid}。" );
        }

        /// <inheritdoc />
        public Task SubscribeAsync(Guid characteristicUuid, bool useIndication, CancellationToken cancellationToken)
        {
            // 检查连接和取消。
            EnsureConnected(cancellationToken);
            // 记录订阅顺序和模式。
            Subscriptions.Add((characteristicUuid, useIndication));
            // fake 无 CCCD 写延迟。
            return Task.CompletedTask;
        }

        /// <inheritdoc />
        public Task WriteAsync(Guid characteristicUuid, ReadOnlyMemory<byte> value, bool withResponse, CancellationToken cancellationToken)
        {
            // 检查连接和取消。
            EnsureConnected(cancellationToken);
            // 当前只允许向控制点写入。
            if (characteristicUuid != ProtocolConstants.ControlPointUuid)
            {
                // 报告测试调用错误。
                throw new KeyNotFoundException($"fake 不支持写入特征 {characteristicUuid}。" );
            }

            // 控制分片必须使用 ATT WriteWithResponse。
            Assert(withResponse, "控制点分片必须使用 WriteWithResponse。" );
            // 推入当前分片并尝试完成逻辑帧。
            FragmentPushStatus status = _controlRequestReassembler.Push(value.Span, out byte[]? completeFrame, out ProtocolDecodeError error);
            // 分片拒绝表示会话 MTU 或顺序实现错误。
            if (status == FragmentPushStatus.Rejected)
            {
                // 抛出重组错误。
                throw new InvalidDataException($"fake 控制请求重组失败：{error}。" );
            }

            // 尚未完成时等待下一片。
            if (status != FragmentPushStatus.Completed)
            {
                // 当前写成功。
                return Task.CompletedTask;
            }

            // 完整帧必须非空且通过 CRC。
            Assert(completeFrame is not null, "完成状态必须返回完整控制帧。" );
            // 解码控制逻辑帧。
            bool decoded = BleFrameCodec.TryDecode(completeFrame!, out BleLogicalFrame? frame, out ProtocolDecodeError frameError);
            // 断言请求帧有效。
            Assert(decoded && frame is not null && frame.MessageType == (byte)ProtocolMessageType.ControlRequest, $"控制逻辑帧无效：{frameError}。" );
            // 控制 payload 至少包含 6 字节固定请求头。
            Assert(frame!.Payload.Length >= ControlPointCodec.RequestHeaderSize, "控制请求 payload 太短。" );
            // 读取小端 request_id。
            uint requestId = BinaryPrimitives.ReadUInt32LittleEndian(frame.Payload.Span.Slice(0, 4));
            // 读取命令 ID。
            ControlCommandId commandId = (ControlCommandId)frame.Payload.Span[4];
            // 复制偏移 6 之后的命令专用 TLV，逻辑帧释放后测试仍可读取。
            byte[] commandTlv = frame.Payload.Span.Slice(ControlPointCodec.RequestHeaderSize).ToArray();
            // 记录一次完整请求和 ID。
            CompletedControlFrames++;
            // 保存 ID，用于断言重试幂等。
            ControlRequestIds.Add(requestId);
            // 保存命令和 TLV；同 request_id 重试仍保留两次线上事实。
            ControlCommands.Add((commandId, commandTlv));

            // 配置丢弃首次响应时只模拟 indication 丢失，设备仍执行命令并缓存结果。
            if (DropFirstControlResponse && !_firstResponseDropped)
            {
                // 第一次执行命令并增加 revision。
                ApplyCommand(commandId);
                // 记录已执行 ID，下一次同 ID 重试不得重复执行。
                _lastAppliedRequestId = requestId;
                // 标记响应已经丢弃，第二次同 ID 不重复执行。
                _firstResponseDropped = true;
                // 不发布 indication，迫使会话超时重试。
                return Task.CompletedTask;
            }

            // 非重试首次请求或新的 request_id 执行命令；同 ID 重试沿用缓存状态。
            if (!DropFirstControlResponse || (requestId != _lastAppliedRequestId))
            {
                // 执行当前命令。
                ApplyCommand(commandId);
                // 记录已执行 request_id。
                _lastAppliedRequestId = requestId;
            }

            // 构造固定 12 字节成功响应。
            byte[] responsePayload = new byte[ControlPointCodec.ResponseHeaderSize];
            // 写入相同 request_id。
            BinaryPrimitives.WriteUInt32LittleEndian(responsePayload.AsSpan(0, 4), requestId);
            // 写入相同命令 ID。
            responsePayload[4] = (byte)commandId;
            // status=0 表示成功。
            responsePayload[5] = 0;
            // error_code=0 保持数组默认值。
            BinaryPrimitives.WriteUInt32LittleEndian(responsePayload.AsSpan(8, 4), _currentState.StateRevision);
            // 构造控制响应逻辑帧。
            BleLogicalFrame responseFrame = new(
                ProtocolConstants.ProtocolMajor,
                ProtocolConstants.ProtocolMinor,
                (byte)ProtocolMessageType.ControlResponse,
                0,
                unchecked(++_notificationSequence),
                1234,
                responsePayload);
            // 直接发布完整逻辑帧，验证会话同时支持 WinRT long value 和通知分片入口。
            ValueReceived?.Invoke(this, new BleGattValueReceivedEventArgs(ProtocolConstants.ControlPointUuid, BleFrameCodec.Encode(responseFrame)));
            // fake 写入完成。
            return Task.CompletedTask;
        }

        /// <inheritdoc />
        public Task DisconnectAsync(CancellationToken cancellationToken)
        {
            // 允许取消主动断开。
            cancellationToken.ThrowIfCancellationRequested();
            // 标记链路断开；主动路径不发布意外断线事件。
            _isConnected = false;
            // 清除控制半帧。
            _controlRequestReassembler.Reset();
            // 返回完成任务。
            return Task.CompletedTask;
        }

        /// <inheritdoc />
        public Task ForgetDeviceAsync(string deviceId, CancellationToken cancellationToken)
        {
            // 允许测试取消忘记操作。
            cancellationToken.ThrowIfCancellationRequested();
            // 空 ID 表示会话错误地丢失了 Windows 设备身份。
            if (string.IsNullOrWhiteSpace(deviceId))
            {
                // 与正式 WinRT 实现保持相同输入合同。
                throw new ArgumentException("Windows 设备 ID 不能为空。", nameof(deviceId));
            }

            // 记录精确系统设备 ID，供测试断言取消配对目标。
            ForgottenDeviceIds.Add(deviceId);
            // fake 不访问操作系统，记录完成即视为取消配对成功。
            return Task.CompletedTask;
        }

        /// <inheritdoc />
        public ValueTask DisposeAsync()
        {
            // 标记释放和断开。
            _disposed = true;
            // 清除连接状态。
            _isConnected = false;
            // 返回完成 ValueTask。
            return ValueTask.CompletedTask;
        }

        /// <summary>替换设备内部权威状态，模拟 ESP32 重启或断线期间继续运行。</summary>
        public void SetCurrentState(LiveState state)
        {
            // 保存新不可变状态。
            _currentState = state;
        }

        /// <summary>发布一个 LiveState notification。</summary>
        public void EmitLiveState(LiveState state)
        {
            // 同时更新 fake 读快照值。
            _currentState = state;
            // 发布完整逻辑帧通知。
            ValueReceived?.Invoke(this, new BleGattValueReceivedEventArgs(ProtocolConstants.LiveStateUuid, EncodeLiveStateFrame(state)));
        }

        /// <summary>发布一个低延迟 Event 逻辑帧。</summary>
        public void EmitProtocolEvent(ReadOnlySpan<byte> payload)
        {
            // 构造协议 Event 帧。
            BleLogicalFrame frame = new(
                ProtocolConstants.ProtocolMajor,
                ProtocolConstants.ProtocolMinor,
                (byte)ProtocolMessageType.Event,
                0,
                unchecked(++_notificationSequence),
                2345,
                payload);
            // 发布完整事件值。
            ValueReceived?.Invoke(this, new BleGattValueReceivedEventArgs(ProtocolConstants.EventUuid, BleFrameCodec.Encode(frame)));
        }

        /// <summary>按 UUID 0007 发布一个固定 RawStream v1 逻辑帧。</summary>
        public void EmitRawStream(RawImuSampleV1 sample)
        {
            // 样本不能为空。
            ArgumentNullException.ThrowIfNull(sample);
            // 精确分配 22 字节 payload。
            byte[] payload = new byte[RawStreamV1Codec.PayloadSize];
            // 写入 sample_index u32LE。
            BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0, 4), sample.SampleIndex);
            // 写入 monotonic_ms u32LE。
            BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), sample.MonotonicMilliseconds);
            // 写入 gx、gy、gz、ax、ay、az 六个 int16LE 固定点诊断码；测试传输布局，不声称它们是未处理 FIFO 原始码。
            BinaryPrimitives.WriteInt16LittleEndian(payload.AsSpan(8, 2), sample.GxRaw);
            // 写入陀螺仪 Y 轴。
            BinaryPrimitives.WriteInt16LittleEndian(payload.AsSpan(10, 2), sample.GyRaw);
            // 写入陀螺仪 Z 轴。
            BinaryPrimitives.WriteInt16LittleEndian(payload.AsSpan(12, 2), sample.GzRaw);
            // 写入加速度计 X 轴。
            BinaryPrimitives.WriteInt16LittleEndian(payload.AsSpan(14, 2), sample.AxRaw);
            // 写入加速度计 Y 轴。
            BinaryPrimitives.WriteInt16LittleEndian(payload.AsSpan(16, 2), sample.AyRaw);
            // 写入加速度计 Z 轴。
            BinaryPrimitives.WriteInt16LittleEndian(payload.AsSpan(18, 2), sample.AzRaw);
            // 写入质量位 u16LE。
            BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(20, 2), sample.QualityFlags);
            // 构造消息类型 8 的完整逻辑帧。
            BleLogicalFrame frame = new(
                ProtocolConstants.ProtocolMajor,
                ProtocolConstants.ProtocolMinor,
                (byte)ProtocolMessageType.RawStream,
                0,
                unchecked(++_notificationSequence),
                sample.MonotonicMilliseconds,
                payload);
            // 通过 RawStream UUID 发布，状态机必须使用其独立重组器。
            ValueReceived?.Invoke(this, new BleGattValueReceivedEventArgs(ProtocolConstants.RawStreamUuid, BleFrameCodec.Encode(frame)));
        }

        /// <summary>按 UUID 0007 发布一个固定 InferenceDiagnosticV1 逻辑帧。</summary>
        public void EmitInferenceDiagnostic(InferenceDiagnosticV1 diagnostic)
        {
            // 诊断对象不能为空。
            ArgumentNullException.ThrowIfNull(diagnostic);
            // 精确分配 28 字节 payload。
            byte[] payload = new byte[InferenceDiagnosticV1Codec.PayloadSize];
            // 写入诊断版本一。
            payload[0] = diagnostic.DiagnosticVersion;
            // 写入融合动作索引。
            payload[1] = (byte)diagnostic.FusedAction;
            // 写入基础模型动作索引。
            payload[2] = (byte)diagnostic.BaseAction;
            // 写入掩码模型动作索引。
            payload[3] = (byte)diagnostic.MaskedAction;
            // 把融合概率限制到 0～1 后四舍五入为 Q15。
            BinaryPrimitives.WriteUInt16LittleEndian(
                payload.AsSpan(4, 2),
                checked((ushort)Math.Round(Math.Clamp(diagnostic.FusedConfidence, 0.0, 1.0) * ushort.MaxValue)));
            // 编码基础模型概率。
            BinaryPrimitives.WriteUInt16LittleEndian(
                payload.AsSpan(6, 2),
                checked((ushort)Math.Round(Math.Clamp(diagnostic.BaseConfidence, 0.0, 1.0) * ushort.MaxValue)));
            // 编码掩码模型概率。
            BinaryPrimitives.WriteUInt16LittleEndian(
                payload.AsSpan(8, 2),
                checked((ushort)Math.Round(Math.Clamp(diagnostic.MaskedConfidence, 0.0, 1.0) * ushort.MaxValue)));
            // 写入质量位。
            BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(10, 2), diagnostic.QualityFlags);
            // 写入窗口序号。
            BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(12, 4), diagnostic.WindowSequence);
            // 写入窗口末点单调毫秒。
            BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(16, 4), diagnostic.WindowEndMilliseconds);
            // 写入推理耗时微秒。
            BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(20, 4), diagnostic.InferenceMicroseconds);
            // 写入累计推理失败数。
            BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(24, 4), diagnostic.FailureCount);
            // 构造消息类型九的完整逻辑帧。
            BleLogicalFrame frame = new(
                ProtocolConstants.ProtocolMajor,
                ProtocolConstants.ProtocolMinor,
                (byte)ProtocolMessageType.InferenceDiagnostic,
                0,
                unchecked(++_notificationSequence),
                diagnostic.WindowEndMilliseconds,
                payload);
            // 通过同一 RawStream UUID 发布，状态机必须按 message_type 路由。
            ValueReceived?.Invoke(this, new BleGattValueReceivedEventArgs(ProtocolConstants.RawStreamUuid, BleFrameCodec.Encode(frame)));
        }

        /// <summary>发布任意 GATT Value，用于 CRC 和分片错误边界测试。</summary>
        public void EmitRaw(Guid characteristicUuid, ReadOnlySpan<byte> value)
        {
            // 复制调用数据并发布到会话后台通道。
            ValueReceived?.Invoke(this, new BleGattValueReceivedEventArgs(characteristicUuid, value.ToArray()));
        }

        /// <summary>生成一个完整 LiveState 逻辑帧副本，测试可在发布前损坏。</summary>
        public byte[] CreateLiveStateFrame(LiveState state)
        {
            // 复用 fake 与生产相同的编码入口。
            return EncodeLiveStateFrame(state);
        }

        /// <summary>模拟 Windows 系统报告意外链路断开。</summary>
        public void TriggerLinkLoss()
        {
            // 清除 transport 连接状态。
            _isConnected = false;
            // 发布意外断线事件，会话层应自动重连。
            Disconnected?.Invoke(this, new BleTransportDisconnectedEventArgs("fake 链路丢失"));
        }

        // 按命令更新 fake 权威状态并递增 revision。
        private void ApplyCommand(ControlCommandId commandId)
        {
            // 根据命令选择设备状态，其它字段保持稳定。
            FitnessDeviceState nextState = commandId switch
            {
                // 开始和恢复进入 Running。
                ControlCommandId.Start or ControlCommandId.Resume => FitnessDeviceState.Running,
                // 暂停进入 Paused。
                ControlCommandId.Pause => FitnessDeviceState.Paused,
                // 停止进入 Summary。
                ControlCommandId.Stop => FitnessDeviceState.Summary,
                // 获取快照和其它配置不改变当前状态。
                _ => _currentState.DeviceState,
            };
            // 创建 revision 加一的完整权威状态。
            _currentState = new LiveState(
                _currentState.SessionSequence,
                _currentState.StateRevision + 1,
                _currentState.ElapsedMilliseconds,
                nextState,
                _currentState.Action,
                _currentState.MetricKind,
                _currentState.BatteryPercent,
                _currentState.MetricValue,
                _currentState.ConfidenceQ15,
                _currentState.CaloriesMilliKcal,
                _currentState.QualityFlags,
                _currentState.PowerFlags,
                _currentState.GoalPercent);
        }

        // 编码完整 LiveState 逻辑帧。
        private byte[] EncodeLiveStateFrame(LiveState state)
        {
            // 创建协议 v1 LiveState 帧。
            BleLogicalFrame frame = new(
                ProtocolConstants.ProtocolMajor,
                ProtocolConstants.ProtocolMinor,
                (byte)ProtocolMessageType.LiveState,
                0,
                unchecked(++_notificationSequence),
                state.ElapsedMilliseconds,
                LiveStateCodec.Encode(state));
            // 编码固定头、payload 和 CRC。
            return BleFrameCodec.Encode(frame);
        }

        // 检查 fake 连接和取消状态。
        private void EnsureConnected(CancellationToken cancellationToken)
        {
            // 传播测试取消。
            cancellationToken.ThrowIfCancellationRequested();
            // 释放后拒绝访问。
            ObjectDisposedException.ThrowIf(_disposed, this);
            // 未连接时模拟 GATT I/O 失败。
            if (!_isConnected)
            {
                // 抛出链路错误。
                throw new IOException("fake BLE 未连接。" );
            }
        }
    }
}
