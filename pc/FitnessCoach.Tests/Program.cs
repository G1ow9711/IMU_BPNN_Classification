// 引入协议实现，测试 CRC、逻辑帧、分片和重组。
using FitnessCoach.Bluetooth;
// 引入领域模型，测试实时状态固定字段映射。
using FitnessCoach.Domain;
// 引入本地仓储，验证幂等和并发读写。
using FitnessCoach.Infrastructure;
// 引入应用服务，验证本地动画和线程调度。
using FitnessCoach.App.Services;
// 引入实时训练 ViewModel，验证 UI 状态机。
using FitnessCoach.App.ViewModels;

// 测试程序使用独立命名空间，避免与未来正式 WPF 启动入口冲突。
namespace FitnessCoach.Tests;

/// <summary>
/// 提供无第三方测试框架的离线黄金向量测试；任一断言失败均返回非零退出码。
/// </summary>
internal static class Program
{
    /// <summary>
    /// 顺序执行全部测试并打印唯一通过标志。
    /// </summary>
    private static async Task<int> Main()
    {
        // 捕获断言异常并转换为稳定进程退出码，便于 PowerShell 和 CI 判定。
        try
        {
            // 验证公认 CRC-16/CCITT-FALSE 标准向量。
            TestStandardCrcVector();
            // 验证 C 与 C# 共用的完整逻辑帧黄金字节。
            TestLogicalFrameGoldenVector();
            // 验证 MTU23 分片字节和顺序重组。
            TestFragmentGoldenVectorsAndReassembly();
            // 验证 MTU23、185、247 都能重组较大逻辑帧。
            TestMtuVariants();
            // 验证 CRC 损坏和乱序分片会被拒绝。
            TestCorruptionAndOutOfOrderRejection();
            // 验证 30 字节实时状态可无损往返。
            TestLiveStateRoundTrip();
            // 验证固件状态字节 2/3/5/7 与 PC 准备、训练、总结和关机语义完全一致。
            TestFirmwareDeviceStateNumericContract();
            // 验证非法实时状态字段不会进入领域层。
            TestInvalidLiveStateRejection();
            // 验证 EventV1 固定 36 字节载荷可跨 C/C# 无损往返。
            TestEventV1RoundTrip();
            // 验证 JSON 会话仓储的幂等和并发合同。
            await TestJsonRepositoryIdempotencyAsync();
            // 验证历史日期/动作筛选、选中详情和 CSV 导出合同。
            await TestHistoryFilteringAndCsvExportAsync();
            // 验证 IMU 实时全部缓存、回看窗口和中文 CSV 导出合同。
            await TestImuCsvExportAsync();
            // 验证连接设备摘要按 cursor 自动补传并幂等写入 JSON。
            await TestHistoryDeviceSynchronizationAsync();
            // 验证 Mock 开始、停止和重复命令幂等。
            await TestMockDeviceIdempotencyAsync();
            // 验证 PC 断线时设备继续运行，重连恢复权威快照。
            await TestMockDisconnectRecoveryAsync();
            // 验证设备页和诊断页显示电量、版本、MTU、RSSI 与重连次数。
            await TestDeviceAndDiagnosticsViewModelsAsync();
            // 验证后台状态通知经调度器安全更新实时 ViewModel。
            await TestLiveViewModelStateFlowAsync();
            // 验证 11 类本地动画资源均存在。
            TestLocalActionAnimationMapping();
            // 验证 11 类离线矢量教练人偶姿态、帧推进和减少动态效果。
            ActionAnimationTests.RunAll();
            // 验证 Windows 真 BLE 会话的选择、重试、revision、事件、重连和释放状态机。
            await WindowsBleDeviceSessionTests.RunAllAsync();
            // 验证命令 6/7/8/9/11 的冻结 TLV 字节、边界和 RawStream 固定 22 字节布局。
            DeviceConfigurationContractTests.RunAll();
            // 验证分类诊断固定 28 字节布局、动作边界和 Q15 换算。
            InferenceDiagnosticContractTests.RunAll();
            // 验证会话摘要 LIST/GET 编解码、分页重放、超时同 ID 重试和幂等拉取合同。
            await SessionTransferContractTests.RunAllAsync();
            // 验证窗口、六个页面和动态状态文案全部使用自然中文。
            await ChineseUiContractTests.RunAllAsync();
            // 验证设置页公制/英制换算与持久化，以及总结页目标和双端保存状态。
            await PcUiCompletionContractTests.RunAllAsync();
            // 打印真 BLE 会话 fake transport 测试通过标志。
            Console.WriteLine("CSHARP_WINDOWS_BLE_SESSION_TESTS_OK");
            // 打印唯一成功标志，便于日志过滤。
            Console.WriteLine("CSHARP_PROTOCOL_TESTS_OK");
            // 打印包含仓储、Mock 和 ViewModel 的上位机综合通过标志。
            Console.WriteLine("CSHARP_PC_TESTS_OK");
            // 返回零表示全部测试通过。
            return 0;
        }
        catch (Exception exception)
        {
            // 把失败类型和堆栈写入标准错误，保留定位信息。
            Console.Error.WriteLine(exception);
            // 返回一表示至少一项测试失败。
            return 1;
        }
    }

    /// <summary>
    /// 验证三种目标 MTU 对 700 字节 payload 都能保持完整帧一致。
    /// </summary>
    private static void TestMtuVariants()
    {
        // 分配 700 字节 payload，确保 MTU185 和 MTU247 也会产生多个分片。
        byte[] payload = new byte[700];

        // 逐字节写入确定性模式，重组时可以发现任何偏移错误。
        for (int index = 0; index < payload.Length; index++)
        {
            // 使用模 251 模式避免长区域重复为全零。
            payload[index] = checked((byte)(index % 251));
        }

        // 构造大 payload 逻辑帧。
        BleLogicalFrame source = new(
            ProtocolConstants.ProtocolMajor,
            ProtocolConstants.ProtocolMinor,
            (byte)ProtocolMessageType.TransferData,
            0,
            0xA55A,
            987_654,
            payload);
        // 编码一次完整帧，后续所有 MTU 都必须还原同一字节序列。
        byte[] frame = BleFrameCodec.Encode(source);
        // 固定覆盖最小 MTU、常用 Windows MTU 和 ESP32 目标 MTU。
        ushort[] mtuValues = [23, 185, 247];

        // 对每个 MTU 独立分片和重组，防止状态跨参数污染。
        foreach (ushort mtu in mtuValues)
        {
            // 按当前 MTU 生成全部顺序分片。
            IReadOnlyList<byte[]> fragments = BleFragmentCodec.Fragment(frame, mtu, 0xA55A);
            // 每种 MTU 都必须至少产生一个分片。
            Assert(fragments.Count > 0, $"MTU{mtu} 没有生成分片。");
            // 创建当前 MTU 专用重组状态。
            BleFrameReassembler reassembler = new();
            // 完成帧初始为空，最后一片后才赋值。
            byte[]? completedFrame = null;

            // 按 BLE 保序语义逐片推入。
            for (int index = 0; index < fragments.Count; index++)
            {
                // 每个 GATT Value 必须不超过 mtu-3 字节 ATT Value 上限。
                Assert(fragments[index].Length <= mtu - 3, $"MTU{mtu} 第 {index} 片超过 ATT Value 上限。");
                // 推入当前片并取得状态。
                FragmentPushStatus status = reassembler.Push(fragments[index], out byte[]? candidate, out ProtocolDecodeError error);
                // 所有分片都必须通过包络和顺序检查。
                Assert(error == ProtocolDecodeError.None, $"MTU{mtu} 第 {index} 片被拒绝：{error}");

                // 最后一片必须返回 Completed，其余片必须返回 AcceptedIncomplete。
                if (index == fragments.Count - 1)
                {
                    // 核对最后一片完成状态。
                    Assert(status == FragmentPushStatus.Completed, $"MTU{mtu} 最后一片没有完成逻辑帧。");
                    // 保存完整帧供循环结束后比较。
                    completedFrame = candidate;
                }
                else
                {
                    // 核对中间片不会过早产生完整帧。
                    Assert(status == FragmentPushStatus.AcceptedIncomplete && candidate is null, $"MTU{mtu} 第 {index} 片状态错误。");
                }
            }

            // 最后一片必须产生非空完整帧。
            byte[] reassembled = completedFrame ?? throw new InvalidOperationException($"MTU{mtu} 重组完成却没有返回数据。");
            // 重组字节必须与原逻辑帧完全一致。
            Assert(reassembled.SequenceEqual(frame), $"MTU{mtu} 重组结果不一致。");
        }
    }

    /// <summary>
    /// 验证字符串“123456789”的 CCITT-FALSE 结果固定为 0x29B1。
    /// </summary>
    private static void TestStandardCrcVector()
    {
        // 使用 ASCII 固定字节，避免区域设置影响输入编码。
        byte[] input = "123456789"u8.ToArray();
        // 计算协议 CRC。
        ushort actual = Crc16CcittFalse.Compute(input);
        // 与国际通用检查值比较。
        Assert(actual == 0x29B1, $"CRC 标准向量错误：0x{actual:X4}");
    }

    /// <summary>验证 EventV1 固定字段、字节布局和非法动作拒绝。</summary>
    private static void TestEventV1RoundTrip()
    {
        // 构造一次跳跃深蹲完成计数事件，数值与设备端 C 黄金测试一致。
        DeviceEventV1 source = new(
            1,
            DeviceEventType.RepetitionCounted,
            FitnessDeviceState.Running,
            ActionId.JumpingSquat,
            MetricKind.Repetition,
            88,
            0x1234,
            0x01020304,
            7,
            9,
            1,
            12,
            2345,
            0x7FFF,
            0);
        // 编码固定 payload，供 BLE Event 逻辑帧承载。
        byte[] payload = EventV1Codec.Encode(source);
        // 长度必须与协议常量严格一致。
        Assert(payload.Length == ProtocolConstants.EventPayloadSize, "EventV1 长度错误。");
        // 与 C 测试确定的完整小端字节逐字节比较。
        Assert(
            Convert.ToHexString(payload) == "0106030301583412040302010700000009000000010000000C00000029090000FF7F0000",
            $"EventV1 黄金字节错误：{Convert.ToHexString(payload)}");
        // 解码合法 payload。
        bool decoded = EventV1Codec.TryDecode(payload, out DeviceEventV1? actual, out string? error);
        // 解码必须成功且没有错误文本。
        Assert(decoded && actual is not null && error is null, $"EventV1 解码失败：{error}");
        // 核对 PC 动画、指标和诊断依赖的关键字段。
        Assert(
            actual!.EventType == DeviceEventType.RepetitionCounted &&
            actual.Action == ActionId.JumpingSquat &&
            actual.MetricTotal == 12U &&
            actual.CaloriesMilliKcal == 2345U,
            "EventV1 解码字段错误。");
        // 损坏动作索引为 11，当前模型只允许 0～10 或 255 未知。
        payload[3] = 11;
        // 非法 payload 必须拒绝，不能错误播放动作动画。
        Assert(
            !EventV1Codec.TryDecode(payload, out _, out string? invalidError) && !string.IsNullOrWhiteSpace(invalidError),
            "EventV1 非法动作未被拒绝。");
    }

    /// <summary>
    /// 验证逻辑帧编码结果与 shared/protocol/golden_vectors.json 完全一致。
    /// </summary>
    private static void TestLogicalFrameGoldenVector()
    {
        // 构造固定 5 字节 payload。
        byte[] payload = Convert.FromHexString("1020304050");
        // 构造与 C 黄金测试相同的字段视图。
        BleLogicalFrame frame = new(
            ProtocolConstants.ProtocolMajor,
            ProtocolConstants.ProtocolMinor,
            (byte)ProtocolMessageType.LiveState,
            1,
            0x1234,
            0x01020304,
            payload);
        // 编码完整逻辑帧。
        byte[] encoded = BleFrameCodec.Encode(frame);
        // 读取人工固定的跨语言黄金字节。
        byte[] expected = Convert.FromHexString("7EB1010003013412040302010500102030405092F9");
        // 逐字节比较，任何差异都表示端序、长度或 CRC 漂移。
        Assert(encoded.SequenceEqual(expected), $"逻辑帧黄金向量不一致：{Convert.ToHexString(encoded)}");

        // 解码刚生成的完整帧。
        bool decodedOk = BleFrameCodec.TryDecode(encoded, out BleLogicalFrame? decoded, out ProtocolDecodeError error);
        // 解码必须成功且没有错误码。
        Assert(decodedOk && error == ProtocolDecodeError.None, $"逻辑帧解码失败：{error}");
        // 取得非空解码对象，失败时立即终止当前测试。
        BleLogicalFrame decodedFrame = decoded ?? throw new InvalidOperationException("解码成功却没有返回逻辑帧。");
        // 核对消息类型。
        Assert(decodedFrame.MessageType == (byte)ProtocolMessageType.LiveState, "消息类型解码错误。");
        // 核对 sequence。
        Assert(decodedFrame.Sequence == 0x1234, "逻辑 sequence 解码错误。");
        // 核对四字节单调时间。
        Assert(decodedFrame.MonotonicMilliseconds == 0x01020304, "单调时间解码错误。");
        // 核对 payload 内容。
        Assert(decodedFrame.Payload.Span.SequenceEqual(payload), "payload 解码错误。");
    }

    /// <summary>
    /// 验证 MTU23 会产生两片黄金字节，并能还原完整帧。
    /// </summary>
    private static void TestFragmentGoldenVectorsAndReassembly()
    {
        // 读取完整逻辑帧黄金字节。
        byte[] frame = Convert.FromHexString("7EB1010003013412040302010500102030405092F9");
        // 按最小常见 ATT MTU23 执行分片。
        IReadOnlyList<byte[]> fragments = BleFragmentCodec.Fragment(frame, 23, 0x1234);
        // 21 字节逻辑帧在每片 12 字节数据容量下必须得到两片。
        Assert(fragments.Count == 2, $"MTU23 分片数错误：{fragments.Count}");
        // 第一片黄金字节包括 8 字节包络和前 12 字节逻辑帧。
        byte[] expected0 = Convert.FromHexString("3412000002000C007EB101000301341204030201");
        // 第二片黄金字节包括剩余 9 字节。
        byte[] expected1 = Convert.FromHexString("34120100020009000500102030405092F9");
        // 核对第一片逐字节一致。
        Assert(fragments[0].SequenceEqual(expected0), $"第一片黄金向量错误：{Convert.ToHexString(fragments[0])}");
        // 核对第二片逐字节一致。
        Assert(fragments[1].SequenceEqual(expected1), $"第二片黄金向量错误：{Convert.ToHexString(fragments[1])}");

        // 为该 GATT 特征创建独立重组器。
        BleFrameReassembler reassembler = new();
        // 推入第一片，预期正常但尚未完成。
        FragmentPushStatus firstStatus = reassembler.Push(fragments[0], out byte[]? firstFrame, out ProtocolDecodeError firstError);
        // 核对第一片状态、错误码和空结果。
        Assert(firstStatus == FragmentPushStatus.AcceptedIncomplete && firstError == ProtocolDecodeError.None && firstFrame is null, "第一片重组状态错误。");
        // 推入最后一片，预期完成。
        FragmentPushStatus secondStatus = reassembler.Push(fragments[1], out byte[]? completeFrame, out ProtocolDecodeError secondError);
        // 核对完成状态和无错误码。
        Assert(secondStatus == FragmentPushStatus.Completed && secondError == ProtocolDecodeError.None, $"第二片重组失败：{secondError}");
        // 取得非空完整帧。
        byte[] reassembled = completeFrame ?? throw new InvalidOperationException("重组完成却没有返回逻辑帧。");
        // 完整帧必须与分片前输入逐字节一致。
        Assert(reassembled.SequenceEqual(frame), "重组结果与原逻辑帧不同。");
    }

    /// <summary>
    /// 验证损坏 payload 和缺少索引 0 的分片不会被静默接受。
    /// </summary>
    private static void TestCorruptionAndOutOfOrderRejection()
    {
        // 读取正确完整帧作为损坏测试起点。
        byte[] corrupted = Convert.FromHexString("7EB1010003013412040302010500102030405092F9");
        // 翻转 payload 首字节最低位，但保留原 CRC。
        corrupted[ProtocolConstants.LogicalHeaderSize] ^= 0x01;
        // 解码损坏帧。
        bool decoded = BleFrameCodec.TryDecode(corrupted, out _, out ProtocolDecodeError crcError);
        // 必须返回 BadCrc，不能误报为长度错误。
        Assert(!decoded && crcError == ProtocolDecodeError.BadCrc, $"损坏帧错误码不正确：{crcError}");

        // 获取黄金逻辑帧的两片数据。
        byte[] frame = Convert.FromHexString("7EB1010003013412040302010500102030405092F9");
        // 按 MTU23 重新生成分片。
        IReadOnlyList<byte[]> fragments = BleFragmentCodec.Fragment(frame, 23, 0x1234);
        // 新重组器还没有收到索引 0。
        BleFrameReassembler reassembler = new();
        // 先推入索引 1，模拟第一片通知丢失。
        FragmentPushStatus status = reassembler.Push(fragments[1], out byte[]? result, out ProtocolDecodeError fragmentError);
        // 必须拒绝、返回 BadFragment 且不产生完整帧。
        Assert(status == FragmentPushStatus.Rejected && fragmentError == ProtocolDecodeError.BadFragment && result is null, "乱序分片未被正确拒绝。");
    }

    /// <summary>
    /// 验证 LiveStateV1 的全部字段可以无损编码和解码。
    /// </summary>
    private static void TestLiveStateRoundTrip()
    {
        // 构造同时覆盖枚举、位标志、Q15、电量和目标的领域状态。
        LiveState source = new(
            42,
            77,
            123_456,
            FitnessDeviceState.Running,
            ActionId.JumpingSquat,
            MetricKind.Repetition,
            88,
            12,
            49_151,
            12_345,
            DataQualityFlags.Warmup | DataQualityFlags.HapticSampleSubstituted,
            PowerFlags.Charging | PowerFlags.UsbPresent,
            60);
        // 编码为固定 30 字节 payload。
        byte[] payload = LiveStateCodec.Encode(source);
        // 长度必须严格等于 v1 合同。
        Assert(payload.Length == ProtocolConstants.LiveStatePayloadSize, "实时状态 payload 长度错误。");
        // 解码回领域对象。
        bool decodedOk = LiveStateCodec.TryDecode(payload, out LiveState? decoded, out string? error);
        // 解码必须成功且没有错误文本。
        Assert(decodedOk && error is null, $"实时状态解码失败：{error}");
        // 取得非空状态对象。
        LiveState state = decoded ?? throw new InvalidOperationException("实时状态解码成功却返回 null。");
        // 核对会话序号。
        Assert(state.SessionSequence == source.SessionSequence, "会话序号往返错误。");
        // 核对状态修订号。
        Assert(state.StateRevision == source.StateRevision, "状态修订号往返错误。");
        // 核对动作索引。
        Assert(state.Action == ActionId.JumpingSquat, "动作索引往返错误。");
        // 核对指标值。
        Assert(state.MetricValue == 12 && state.MetricKind == MetricKind.Repetition, "次数或单位往返错误。");
        // 核对卡路里定点整数。
        Assert(state.CaloriesMilliKcal == 12_345, "卡路里往返错误。");
        // 核对组合质量标志。
        Assert(state.QualityFlags == source.QualityFlags, "质量标志往返错误。");
        // 核对组合电源标志。
        Assert(state.PowerFlags == source.PowerFlags, "电源标志往返错误。");
    }

    /// <summary>验证 PC 直接按固件冻结数值解码设备状态，防止运行态误显示为暂停。</summary>
    private static void TestFirmwareDeviceStateNumericContract()
    {
        // 构造其余字段均合法的实时状态，用于取得固定 30 字节载荷。
        LiveState source = new(
            1,
            1,
            0,
            FitnessDeviceState.Idle,
            ActionId.Unknown,
            MetricKind.None,
            100,
            0,
            0,
            0,
            DataQualityFlags.None,
            PowerFlags.None,
            0);
        // 编码为可修改状态字节的载荷。
        byte[] payload = LiveStateCodec.Encode(source);
        // 固件冻结状态值及 PC 期望枚举逐项配对。
        (byte RawValue, FitnessDeviceState Expected)[] cases =
        [
            // 2 表示累计分类证据准备态。
            (2, FitnessDeviceState.Preparing),
            // 3 表示真正训练和计数运行态。
            (3, FitnessDeviceState.Running),
            // 5 表示停止后的会话总结态。
            (5, FitnessDeviceState.Summary),
            // 7 表示设备正在执行安全关机。
            (7, FitnessDeviceState.Shutdown),
        ];
        // 遍历四个历史错位值。
        foreach ((byte rawValue, FitnessDeviceState expected) in cases)
        {
            // LiveStateV1 第 13 字节偏移 12 固定为 device_state。
            payload[12] = rawValue;
            // 解码固件原始载荷。
            bool decodedOk = LiveStateCodec.TryDecode(payload, out LiveState? decoded, out string? error);
            // 解码必须成功且返回预期语义。
            Assert(decodedOk && error is null && decoded?.DeviceState == expected, $"固件状态值 {rawValue} 未映射为 {expected}。" );
        }
    }

    /// <summary>
    /// 验证非法动作索引和电量值不会进入领域对象。
    /// </summary>
    private static void TestInvalidLiveStateRejection()
    {
        // 创建 30 字节零状态，零值字段本身均合法。
        byte[] payload = new byte[ProtocolConstants.LiveStatePayloadSize];
        // 把动作索引改为未定义的 11。
        payload[13] = 11;
        // 解码非法动作索引。
        bool actionOk = LiveStateCodec.TryDecode(payload, out LiveState? actionState, out string? actionError);
        // 必须拒绝并给出错误文本。
        Assert(!actionOk && actionState is null && actionError is not null, "非法动作索引未被拒绝。");

        // 恢复合法 Unknown 动作值。
        payload[13] = byte.MaxValue;
        // 设置非法电量 101。
        payload[15] = 101;
        // 解码非法电量值。
        bool batteryOk = LiveStateCodec.TryDecode(payload, out LiveState? batteryState, out string? batteryError);
        // 必须拒绝并保留中文错误文本。
        Assert(!batteryOk && batteryState is null && batteryError is not null, "非法电量未被拒绝。");
    }

    /// <summary>验证相同复合键只保存一条记录，并发写入不会损坏 JSON。</summary>
    private static async Task TestJsonRepositoryIdempotencyAsync()
    {
        // 在项目定向 TEMP 下创建唯一测试目录。
        string directory = Path.Combine(Path.GetTempPath(), $"fitness-repository-{Guid.NewGuid():N}");
        // 构造会话仓储文件路径。
        string filePath = Path.Combine(directory, "sessions.json");

        try
        {
            // 创建可释放 JSON 仓储。
            using JsonSessionRepository repository = new(filePath);
            // 初始化模式文件。
            await repository.InitializeAsync();
            // 创建固定复合键摘要。
            TrainingSessionSummary first = CreateTestSummary("DEVICE-A", 7, 1_000, 1_500);
            // 创建同键更新摘要。
            TrainingSessionSummary replacement = CreateTestSummary("DEVICE-A", 7, 2_000, 2_500);
            // 并发保存同一复合键，仓储必须串行化。
            await Task.WhenAll(repository.SaveAsync(first), repository.SaveAsync(replacement));
            // 查询全部记录。
            IReadOnlyList<TrainingSessionSummary> sessions = await repository.ListAsync(new SessionQuery { Limit = 10 });
            // 相同 device_id + session_seq 只能保留一条。
            Assert(sessions.Count == 1, $"幂等仓储出现重复记录：{sessions.Count}");
            // 读取复合键记录。
            TrainingSessionSummary? loaded = await repository.GetAsync("DEVICE-A", 7);
            // 记录必须存在且是两个合法写入之一，不能出现半写 JSON。
            Assert(loaded is not null && (loaded.ElapsedMilliseconds is 1_000U or 2_000U), "幂等仓储读取结果非法。");
        }
        finally
        {
            // 测试结束删除独立临时目录。
            if (Directory.Exists(directory))
            {
                // 递归删除当前测试创建的文件。
                Directory.Delete(directory, true);
            }
        }
    }

    /// <summary>验证历史半开日期范围、动作筛选、详情和 RFC4180 CSV。</summary>
    private static async Task TestHistoryFilteringAndCsvExportAsync()
    {
        // 在项目定向 TEMP 下创建唯一测试目录。
        string directory = Path.Combine(Path.GetTempPath(), $"fitness-history-{Guid.NewGuid():N}");
        // 构造会话仓储路径。
        string repositoryPath = Path.Combine(directory, "sessions.json");
        // 构造 CSV 输出路径。
        string csvPath = Path.Combine(directory, "filtered-history.csv");

        try
        {
            // 创建可释放 JSON 仓储。
            using JsonSessionRepository repository = new(repositoryPath);
            // 初始化仓储。
            await repository.InitializeAsync();
            // 固定目标 UTC 开始时间。
            DateTimeOffset targetStart = new(2026, 7, 14, 2, 0, 0, TimeSpan.Zero);
            // 保存前一天深蹲会话，日期过滤应排除。
            await repository.SaveAsync(CreateHistorySummary("DEVICE-H", 1U, targetStart.AddDays(-1), ActionId.Squat, "前一天"));
            // 保存目标日期收腹跳会话，动作和日期均命中；结束原因含逗号和引号测试 CSV 转义。
            await repository.SaveAsync(CreateHistorySummary("DEVICE-H", 2U, targetStart, ActionId.TuckJump, "用户,\"停止\""));
            // 保存目标日期开合跳会话，动作过滤应排除。
            await repository.SaveAsync(CreateHistorySummary("DEVICE-H", 3U, targetStart.AddHours(1), ActionId.JumpingJack, "其它动作"));

            // 直接验证仓储 UTC 半开区间和动作条件。
            IReadOnlyList<TrainingSessionSummary> filtered = await repository.ListAsync(new SessionQuery
            {
                // 包含目标日 UTC 零点。
                StartedOnOrAfterUtc = new DateTimeOffset(2026, 7, 14, 0, 0, 0, TimeSpan.Zero),
                // 排除次日 UTC 零点。
                StartedBeforeUtc = new DateTimeOffset(2026, 7, 15, 0, 0, 0, TimeSpan.Zero),
                // 只保留收腹跳指标。
                Action = ActionId.TuckJump,
                // 测试数据上限。
                Limit = 10,
            });
            // 只能命中会话序号 2。
            Assert(filtered.Count == 1 && filtered[0].SessionSequence == 2U, "历史日期/动作组合筛选结果错误。");

            // 创建返回固定 CSV 路径的测试选择器。
            FixedHistoryDestinationPicker picker = new(csvPath);
            // 创建包含真实导出器的历史 ViewModel。
            HistoryViewModel viewModel = new(repository, new HistoryCsvExporter(), picker);
            // 使用目标会话的本地自然日，验证 ViewModel 时区转换。
            viewModel.DateFrom = targetStart.ToLocalTime().Date;
            // 结束日期同日，应完整包含当天。
            viewModel.DateTo = targetStart.ToLocalTime().Date;
            // 选择收腹跳动作选项。
            viewModel.SelectedActionFilter = viewModel.ActionFilters.Single(option => option.Action == ActionId.TuckJump);
            // 应用筛选。
            await viewModel.RefreshAsync();
            // 列表、默认选中和详情必须一致。
            Assert(
                viewModel.Sessions.Count == 1 &&
                viewModel.SelectedSession?.StorageKey == "DEVICE-H:2" &&
                viewModel.SelectedSession.DetailsText.Contains("收腹跳", StringComparison.Ordinal),
                "历史 ViewModel 未显示筛选结果或选中详情。");
            // 导出当前筛选快照。
            await viewModel.ExportCsvAsync();
            // CSV 文件必须生成。
            Assert(File.Exists(csvPath), "历史 CSV 文件未生成。");
            // 读取原始字节验证 UTF-8 BOM。
            byte[] csvBytes = await File.ReadAllBytesAsync(csvPath);
            // BOM 必须为 EF BB BF，保证中文 Excel 兼容。
            Assert(csvBytes.Length >= 3 && csvBytes[0] == 0xEF && csvBytes[1] == 0xBB && csvBytes[2] == 0xBF, "历史 CSV 未使用 UTF-8 BOM。");
            // 读取文本验证字段和 RFC4180 双引号转义。
            string csvText = await File.ReadAllTextAsync(csvPath);
            // 只导出收腹跳且结束原因必须转义为双引号包裹。
            Assert(
                csvText.Contains("收腹跳", StringComparison.Ordinal) &&
                !csvText.Contains("开合跳", StringComparison.Ordinal) &&
                csvText.Contains("动作次数", StringComparison.Ordinal) &&
                csvText.Contains("\"用户,\"\"停止\"\"\"", StringComparison.Ordinal),
                "历史 CSV 动作筛选或字段转义错误。");
        }
        finally
        {
            // 测试结束删除独立临时目录。
            if (Directory.Exists(directory))
            {
                // 递归删除当前测试创建的 JSON 和 CSV。
                Directory.Delete(directory, true);
            }
        }
    }

    /// <summary>验证历史页把设备摘要接入应用仓储，而不是只保留协议层测试。</summary>
    private static async Task TestHistoryDeviceSynchronizationAsync()
    {
        // 在项目定向 TEMP 下创建独立仓储目录。
        string directory = Path.Combine(Path.GetTempPath(), $"fitness-history-sync-{Guid.NewGuid():N}");
        // 构造 JSON 仓储路径。
        string repositoryPath = Path.Combine(directory, "sessions.json");

        try
        {
            // 创建并初始化幂等 JSON 仓储。
            using JsonSessionRepository repository = new(repositoryPath);
            // 创建空模式文件。
            await repository.InitializeAsync();
            // 创建实现补传接口的 Mock 设备。
            await using MockDeviceSession device = new(TimeSpan.FromMilliseconds(5), [ActionId.Squat]);
            // 建立 Mock 链路。
            await device.ConnectAsync();
            // 开始一个设备会话。
            await device.StartAsync();
            // 等待至少一个 tick，确保摘要含设备权威累计值。
            await Task.Delay(12);
            // 停止并在设备侧形成可补传摘要。
            TrainingSessionSummary? deviceSummary = await device.StopAsync();
            // 测试前置条件必须产生摘要。
            Assert(deviceSummary is not null, "Mock 未生成设备摘要。" );
            // 创建不写文件的取消选择器；本测试不执行 CSV。
            FixedHistoryDestinationPicker picker = new(Path.Combine(directory, "unused.csv"));
            // 注入设备、补传接口和同步 UI 调度器。
            using HistoryViewModel viewModel = new(repository, new HistoryCsvExporter(), picker, device, new ImmediateUiDispatcher());
            // 刷新会先按本地 cursor 拉设备，再查询 JSON。
            await viewModel.RefreshAsync();
            // 核对补传状态与列表都包含设备摘要。
            Assert(viewModel.StatusMessage.Contains("设备补传 1 条", StringComparison.Ordinal) && viewModel.Sessions.Any(row => row.StorageKey == deviceSummary!.StorageKey), "历史页未把设备摘要补传到产品列表。" );
            // 再刷新一次，cursor 已追平，设备返回空终点页。
            await viewModel.RefreshAsync();
            // 复合键仓储不得出现重复摘要。
            IReadOnlyList<TrainingSessionSummary> stored = await repository.ListAsync(new SessionQuery { DeviceId = device.DeviceId, Limit = 10 });
            // 只保留同一条摘要。
            Assert(stored.Count == 1 && stored[0].SessionSequence == deviceSummary!.SessionSequence, "重复补传破坏 JSON 幂等主键。" );
        }
        finally
        {
            // 测试结束删除独立临时目录。
            if (Directory.Exists(directory))
            {
                // 递归删除本测试创建的 JSON。
                Directory.Delete(directory, true);
            }
        }
    }

    /// <summary>验证 IMU 全缓存与回看窗口均能导出带中文表头的可复算 CSV。</summary>
    private static async Task TestImuCsvExportAsync()
    {
        // 使用当前系统临时根创建唯一目录，测试结束后只删除本方法创建的文件。
        string directory = Path.Combine(Path.GetTempPath(), $"fitness-coach-imu-export-{Guid.NewGuid():N}");
        // 创建隔离目录，避免和并行测试或真实用户导出冲突。
        Directory.CreateDirectory(directory);
        // 定义实时完整缓存的输出路径。
        string fullPath = Path.Combine(directory, "imu-full.csv");
        // 定义回看十秒窗口的输出路径。
        string viewportPath = Path.Combine(directory, "imu-window.csv");

        try
        {
            // 创建短周期 Mock，以有限等待生成超过十秒的 25 Hz 逻辑样本。
            await using MockDeviceSession device = new(TimeSpan.FromMilliseconds(5), [ActionId.JumpingJack]);
            // 同步调度器让 BLE 事件在测试线程立即进入 ViewModel。
            ImmediateUiDispatcher dispatcher = new();
            // 路径选择器按两次命令顺序返回完整缓存和回看窗口文件。
            QueuedImuDestinationPicker picker = new(fullPath, viewportPath);
            // 注入真实 CSV 导出器和测试路径选择器，覆盖生产文件边界。
            using DiagnosticsViewModel viewModel = new(
                device,
                dispatcher,
                animationPreferences: null,
                imuCsvExporter: new ImuCsvExporter(),
                imuExportDestinationPicker: picker);
            // 建立 Mock 链路后才能同步开发者模式偏好。
            await device.ConnectAsync();
            // 开启开发者模式，满足 RawStream 命令权限合同。
            await ((IDeviceConfigurationSession)device).SetPreferencesAsync(new DevicePreferencesV1(75, true, false, 30, 1U, true));
            // 通过诊断页开启 RawStream，使 ViewModel 建立三份严格对齐的十分钟缓冲。
            await viewModel.ToggleRawStreamCommand.ExecuteAsync();
            // 启动训练，Mock 在活动态持续发布六轴记录。
            await device.StartAsync();

            // 最多等待五秒，直到历史超过一个 250 点可见窗口。
            for (int waitAttempt = 0; waitAttempt < 100 && viewModel.RawChartMaximumOffsetSeconds <= 0.0; waitAttempt++)
            {
                // 每轮让后台定时器发布更多样本，避免依赖固定调度精度。
                await Task.Delay(50);
            }

            // 测试前置条件必须已有超过十秒的同步历史。
            Assert(viewModel.RawChartMaximumOffsetSeconds > 0.0, "IMU 导出测试没有形成超过十秒的样本历史。");
            // 实时模式导出全部缓存。
            await viewModel.ExportImuCsvCommand.ExecuteAsync();
            // 完整文件必须存在并包含至少 251 行数据加一行表头。
            Assert(File.Exists(fullPath), "实时 IMU 全缓存 CSV 未生成。");
            // 读取完整文件字节验证带 BOM 的 UTF-8 中文兼容性。
            byte[] fullBytes = await File.ReadAllBytesAsync(fullPath);
            // BOM 固定为 EF BB BF。
            Assert(fullBytes.Length >= 3 && fullBytes[0] == 0xEF && fullBytes[1] == 0xBB && fullBytes[2] == 0xBF, "IMU CSV 未使用 UTF-8 BOM。");
            // 读取文本验证中文列名、原始码、物理量和质量位列均存在。
            string[] fullLines = await File.ReadAllLinesAsync(fullPath);
            // 第一行必须是用户要求的中文内容，并标明关键单位。
            Assert(
                fullLines[0].Contains("样本序号", StringComparison.Ordinal) &&
                fullLines[0].Contains("设备单调时间（毫秒）", StringComparison.Ordinal) &&
                fullLines[0].Contains("角速度横轴（度每秒）", StringComparison.Ordinal) &&
                fullLines[0].Contains("加速度垂直轴（重力倍数）", StringComparison.Ordinal) &&
                fullLines[0].Contains("质量标志（十六进制）", StringComparison.Ordinal) &&
                fullLines[0].Contains("佩戴手侧", StringComparison.Ordinal) &&
                fullLines[0].Contains("设备稳定动作", StringComparison.Ordinal) &&
                fullLines[0].Contains("分类窗口序号", StringComparison.Ordinal) &&
                fullLines[0].Contains("分类窗口结束时间（毫秒）", StringComparison.Ordinal) &&
                fullLines[0].Contains("基础模型类别", StringComparison.Ordinal) &&
                fullLines[0].Contains("掩码模型类别", StringComparison.Ordinal) &&
                fullLines[0].Contains("融合模型类别", StringComparison.Ordinal) &&
                fullLines[0].Contains("模型是否一致", StringComparison.Ordinal) &&
                fullLines[0].Contains("分类窗口是否在本行结束", StringComparison.Ordinal) &&
                fullLines[0].Contains("是否计数标记点", StringComparison.Ordinal) &&
                fullLines[0].Contains("计数事件序号", StringComparison.Ordinal) &&
                fullLines[0].Contains("计数动作", StringComparison.Ordinal) &&
                fullLines[0].Contains("计数后累计值", StringComparison.Ordinal),
                "IMU CSV 中文表头或单位不完整。");
            // 按中文表头建立列索引，后续断言不依赖列在文件尾部或字符串偶然包含相同词。
            string[] headerFields = fullLines[0].Split(',');
            // 新诊断合同固定为 44 列，防止以后新增字段时无意删除佩戴域、六轴、模型或计数证据。
            Assert(headerFields.Length == 44, $"IMU CSV 应为44列，实际={headerFields.Length}列。");
            // 读取佩戴手侧列索引，离线分类分析必须能区分当前右腕产品域。
            int wristSideColumn = Array.IndexOf(headerFields, "佩戴手侧");
            // 读取融合模型类别列索引。
            int fusedActionColumn = Array.IndexOf(headerFields, "融合模型类别");
            // 读取分类窗口精确结束点列索引。
            int inferenceEndColumn = Array.IndexOf(headerFields, "分类窗口是否在本行结束");
            // 读取计数标记点列索引。
            int metricMarkerColumn = Array.IndexOf(headerFields, "是否计数标记点");
            // 读取计数后累计值列索引。
            int metricTotalColumn = Array.IndexOf(headerFields, "计数后累计值");
            // 五个关键列都必须存在，缺列时前面的包含判断无法保证可机器读取。
            Assert(
                wristSideColumn >= 0 && fusedActionColumn >= 0 && inferenceEndColumn >= 0 &&
                metricMarkerColumn >= 0 && metricTotalColumn >= 0,
                "IMU CSV 缺少分类或计数关键列索引。");
            // 把数据行拆成稳定列数组，导出字段不含逗号且无需 CSV 引号解析。
            string[][] dataRows = fullLines.Skip(1).Select(line => line.Split(',')).ToArray();
            // 当前产品所有导出行必须明确属于右手腕佩戴域，禁止空值进入后续训练数据。
            Assert(
                dataRows.All(fields =>
                    fields.Length == headerFields.Length &&
                    fields[wristSideColumn] == "右手腕"),
                "IMU CSV 存在缺失或错误的佩戴手侧。");
            // 至少一个样本行必须关联到已经完成的开合跳分类窗口，证明导出不是只增加空列。
            Assert(
                dataRows.Any(fields =>
                    fields.Length == headerFields.Length &&
                    fields[fusedActionColumn] == "开合跳" &&
                    fields[inferenceEndColumn] == "是"),
                "IMU CSV 没有关联到任何开合跳分类窗口结束点。");
            // 至少一个样本行必须带权威计数事件和非空累计值，便于把计数点直接叠到六轴曲线上。
            Assert(
                dataRows.Any(fields =>
                    fields.Length == headerFields.Length &&
                    fields[metricMarkerColumn] == "是" &&
                    !string.IsNullOrWhiteSpace(fields[metricTotalColumn])),
                "IMU CSV 没有导出任何权威计数标记点。");
            // 全缓存行数必须超过固定十秒视口。
            Assert(fullLines.Length > 251, "实时模式没有导出超过十秒的完整 IMU 缓存。");

            // 移动到最早可用历史窗口；属性会自动暂停并冻结当前视口。
            viewModel.RawChartOffsetSeconds = viewModel.RawChartMaximumOffsetSeconds;
            // 回看模式再次导出，只允许当前可见最多 250 点。
            await viewModel.ExportImuCsvCommand.ExecuteAsync();
            // 回看文件必须存在。
            Assert(File.Exists(viewportPath), "回看窗口 IMU CSV 未生成。");
            // 读取回看文件并验证表头加 250 点的固定窗口容量。
            string[] viewportLines = await File.ReadAllLinesAsync(viewportPath);
            // 一行中文表头加 250 个同步样本，共 251 行。
            Assert(viewportLines.Length == 251, $"回看窗口应导出 250 点，实际数据行={viewportLines.Length - 1}。");
            // 成功状态必须明确当前窗口和行数，用户可确认导出范围。
            Assert(viewModel.ImuExportStatus.Contains("当前窗口 250 条", StringComparison.Ordinal), "IMU 导出状态没有说明当前窗口行数。");
        }
        finally
        {
            // 测试结束只删除本方法创建的唯一目录。
            if (Directory.Exists(directory))
            {
                // 递归删除两份 CSV，避免污染用户项目和临时目录。
                Directory.Delete(directory, recursive: true);
            }
        }
    }

    /// <summary>验证 Mock 设备身份、电量、MTU、RSSI 和重连诊断可绑定到页面。</summary>
    private static async Task TestDeviceAndDiagnosticsViewModelsAsync()
    {
        // 创建短 tick Mock，避免测试等待真实 480ms。
        await using MockDeviceSession device = new(TimeSpan.FromMilliseconds(5), [ActionId.Squat]);
        // 使用同步测试调度器，后台事件仍按串行顺序执行。
        ImmediateUiDispatcher dispatcher = new();
        // 创建设备页 ViewModel。
        using DeviceViewModel deviceViewModel = new(device, dispatcher);
        // 创建诊断页 ViewModel。
        using DiagnosticsViewModel diagnosticsViewModel = new(device, dispatcher);
        // 通过设备页命令建立 Mock 链路并读取快照。
        await deviceViewModel.ConnectCommand.ExecuteAsync();
        // 核对设备型号、电量和 MTU 已显示。
        Assert(
            deviceViewModel.ModelNumber.Contains("Mock", StringComparison.Ordinal) &&
            deviceViewModel.BatteryText == "96%" &&
            deviceViewModel.AttMtuText == "247 字节",
            "设备页未显示 Mock 型号、电量或 MTU。");
        // 刷新诊断快照。
        diagnosticsViewModel.Refresh();
        // 核对 RSSI、MTU、版本和零错误计数。
        Assert(
            diagnosticsViewModel.RssiText == "-52 分贝毫瓦" &&
            diagnosticsViewModel.AttMtuText == "247 字节" &&
            diagnosticsViewModel.DeviceVersionText.Contains("0.1.0-MOCK", StringComparison.Ordinal) &&
            diagnosticsViewModel.ProtocolErrorText == "校验错误=0；分片错误=0",
            "诊断页未显示完整 Mock 链路指标。");
        // 先把开发者模式偏好同步到 Mock，模拟设备对命令 11 的权限检查。
        await ((IDeviceConfigurationSession)device).SetPreferencesAsync(new DevicePreferencesV1(75, true, false, 30, 1U, true));
        // 通过诊断页显式开启 RawStream。
        await diagnosticsViewModel.ToggleRawStreamCommand.ExecuteAsync();
        // 启动训练，Mock 只在活动设备状态下发布确定性原始样本。
        await device.StartAsync();
        // Windows 定时器可能合并 5 ms 测试 tick；在五秒上限内等待历史超过 250 点，而不是依赖固定睡眠猜测调度速度。
        for (int waitAttempt = 0; waitAttempt < 100 && diagnosticsViewModel.RawChartMaximumOffsetSeconds <= 0.0; waitAttempt++)
        {
            // 每轮等待 50 ms，让后台 Mock 继续发布六轴样本和分类诊断。
            await Task.Delay(50);
        }
        // 开启必须由设备确认，页面显示样本且明确不落盘。
        Assert(
            diagnosticsViewModel.RawStreamEnabled &&
            uint.Parse(diagnosticsViewModel.RawSampleCountText, System.Globalization.CultureInfo.InvariantCulture) > 0U &&
            diagnosticsViewModel.LatestRawSampleText.Contains("陀螺仪诊断码=[", StringComparison.Ordinal) &&
            diagnosticsViewModel.RawStreamStatus.Contains("不写入", StringComparison.Ordinal),
            "诊断页未显示显式开启后的 RawStream 内存样本。" );
        // 两张曲线必须消费同一批同步样本，并显示物理单位、采样率和质量状态。
        Assert(
            diagnosticsViewModel.AccelerationPoints.Count > 1 &&
            diagnosticsViewModel.GyroscopePoints.Count == diagnosticsViewModel.AccelerationPoints.Count &&
            diagnosticsViewModel.LatestPhysicalValuesText.Contains("°/s", StringComparison.Ordinal) &&
            diagnosticsViewModel.LatestPhysicalValuesText.Contains(" g", StringComparison.Ordinal) &&
            diagnosticsViewModel.RawSampleRateText.Contains("Hz", StringComparison.Ordinal) &&
            diagnosticsViewModel.RawQualityText.Contains("正常", StringComparison.Ordinal),
            "诊断页未形成同步六轴物理量曲线、采样率或质量状态。" );
        // 超过十秒的内存历史必须允许用户回看，不再永久丢弃第 251 点以前的数据。
        Assert(
            diagnosticsViewModel.RawChartMaximumOffsetSeconds > 0.0 &&
            diagnosticsViewModel.RawChartHistoryText.Contains("已缓存", StringComparison.Ordinal),
            "六轴曲线没有形成可回看的进程内历史。" );
        // 保存实时窗口末点，稍后验证回看和回到实时改变视口而不停止接收。
        double liveEndSeconds = diagnosticsViewModel.AccelerationPoints[^1].Seconds;
        // 把滑块移动到可用历史范围内的最远位置，视图应自动冻结在过去窗口。
        diagnosticsViewModel.RawChartOffsetSeconds = diagnosticsViewModel.RawChartMaximumOffsetSeconds;
        // 回看窗口必须进入暂停状态并显示过去时间范围。
        Assert(
            diagnosticsViewModel.RawChartPaused &&
            diagnosticsViewModel.RawChartOffsetSeconds > 0.0 &&
            diagnosticsViewModel.RawChartWindowText.Contains("回看", StringComparison.Ordinal) &&
            diagnosticsViewModel.AccelerationPoints[^1].Seconds < liveEndSeconds,
            "拖动曲线历史滑块没有切换到过去窗口。" );
        // 保存回看窗口末点，验证暂停期间视口保持不动。
        double historyEndSeconds = diagnosticsViewModel.AccelerationPoints[^1].Seconds;
        // 等待 RawStream 继续接收；历史应增长但当前回看窗口不得移动。
        await Task.Delay(40);
        // 暂停回看期间曲线末点保持一致。
        Assert(
            diagnosticsViewModel.AccelerationPoints[^1].Seconds == historyEndSeconds,
            "回看过去窗口时实时样本错误地推动了当前视口。" );
        // 回到实时命令应解除暂停并显示最新十秒。
        diagnosticsViewModel.GoLiveRawChartCommand.Execute(null);
        // 最新窗口末点必须晚于先前回看末点，偏移归零。
        Assert(
            !diagnosticsViewModel.RawChartPaused &&
            diagnosticsViewModel.RawChartOffsetSeconds == 0.0 &&
            diagnosticsViewModel.AccelerationPoints[^1].Seconds > historyEndSeconds &&
            diagnosticsViewModel.RawChartWindowText.Contains("实时", StringComparison.Ordinal),
            "回到实时没有跳转到最新曲线窗口。" );
        // 分类卡必须显示设备端三路模型结果、置信度和推理耗时。
        Assert(
            diagnosticsViewModel.FusedActionText == "深蹲" &&
            diagnosticsViewModel.FusedConfidenceText.EndsWith('%') &&
            diagnosticsViewModel.BaseModelText.Contains("基础模型：深蹲", StringComparison.Ordinal) &&
            diagnosticsViewModel.MaskedModelText.Contains("掩码模型：", StringComparison.Ordinal) &&
            diagnosticsViewModel.InferenceTimingText.Contains("ms", StringComparison.Ordinal) &&
            diagnosticsViewModel.InferenceQualityText.Contains("累计失败 0", StringComparison.Ordinal),
            $"诊断页未显示双 M0 分类、融合置信度、耗时或质量事实：融合={diagnosticsViewModel.FusedActionText}/{diagnosticsViewModel.FusedConfidenceText}；基础={diagnosticsViewModel.BaseModelText}；掩码={diagnosticsViewModel.MaskedModelText}；耗时={diagnosticsViewModel.InferenceTimingText}；质量={diagnosticsViewModel.InferenceQualityText}。" );
        // 记录暂停前曲线点数，验证暂停只冻结图形而不停止设备数据流。
        int chartCountBeforePause = diagnosticsViewModel.AccelerationPoints.Count;
        // 执行暂停曲线命令。
        diagnosticsViewModel.PauseRawChartCommand.Execute(null);
        // 暂停按钮必须切换为继续动作。
        Assert(diagnosticsViewModel.RawChartPaused && diagnosticsViewModel.RawChartPauseButtonText == "继续曲线", "曲线暂停状态或按钮文字错误。" );
        // 记录暂停时样本总数。
        uint sampleCountBeforePause = uint.Parse(diagnosticsViewModel.RawSampleCountText, System.Globalization.CultureInfo.InvariantCulture);
        // 等待 Mock 继续发布多个样本。
        await Task.Delay(25);
        // 曲线点数保持不变，但合法样本计数必须继续增加。
        Assert(
            diagnosticsViewModel.AccelerationPoints.Count == chartCountBeforePause &&
            uint.Parse(diagnosticsViewModel.RawSampleCountText, System.Globalization.CultureInfo.InvariantCulture) > sampleCountBeforePause,
            "暂停曲线错误地停止了 RawStream，或冻结期间仍改动图形。" );
        // 清空命令应立即移除两张内存曲线。
        diagnosticsViewModel.ClearRawChartCommand.Execute(null);
        // 两张曲线都必须为空，且采样率窗口回到等待状态。
        Assert(
            diagnosticsViewModel.AccelerationPoints.Count == 0 &&
            diagnosticsViewModel.GyroscopePoints.Count == 0 &&
            diagnosticsViewModel.RawSampleRateText.Contains("等待", StringComparison.Ordinal),
            "清空曲线没有同时清除两张图或采样率窗口。" );
        // 恢复曲线，便于后续关闭 RawStream 前验证新样本重新进入图形。
        diagnosticsViewModel.PauseRawChartCommand.Execute(null);
        // Windows 短周期定时器可能合并唤醒；等待 60ms 确保至少一个新样本进入空曲线。
        await Task.Delay(60);
        // 恢复后曲线必须重新增长。
        Assert(!diagnosticsViewModel.RawChartPaused && diagnosticsViewModel.AccelerationPoints.Count > 0, "继续曲线后没有接收新样本。" );
        // 通过同一按钮关闭 RawStream。
        await diagnosticsViewModel.ToggleRawStreamCommand.ExecuteAsync();
        // 关闭 ACK 返回后记录稳定基准；按钮调用前最后一个合法在途样本允许先完成。
        string countAfterClose = diagnosticsViewModel.RawSampleCountText;
        // 等待后台设备继续推进，确认关闭后不再发布。
        await Task.Delay(25);
        // 关闭后底层事实和页面计数都必须稳定。
        Assert(
            !diagnosticsViewModel.RawStreamEnabled &&
            !((IRawStreamSource)device).IsRawStreamEnabled &&
            diagnosticsViewModel.RawSampleCountText == countAfterClose,
            "RawStream 关闭后仍发布或计数原始样本。" );
        // 停止本次 Mock 训练，避免重连诊断混入运行态。
        _ = await device.StopAsync();
        // 主动断开后再次连接，Mock 诊断应累计一次重连。
        await deviceViewModel.DisconnectCommand.ExecuteAsync();
        // 再次连接。
        await deviceViewModel.ConnectCommand.ExecuteAsync();
        // 刷新重连计数。
        diagnosticsViewModel.Refresh();
        // 核对一次重连。
        Assert(diagnosticsViewModel.ReconnectCountText == "1", "诊断页未显示 Mock 重连次数。");
    }

    // 创建历史筛选测试使用的固定摘要。
    private static TrainingSessionSummary CreateHistorySummary(
        string deviceId,
        uint sequence,
        DateTimeOffset startedAtUtc,
        ActionId action,
        string endReason)
    {
        // 固定会话时长 90 秒。
        const uint elapsedMilliseconds = 90_000U;
        // 固定总能量 3.5 kcal。
        const uint caloriesMilliKcal = 3_500U;
        // 返回仅包含一个动作指标的摘要。
        return new TrainingSessionSummary(
            deviceId,
            sequence,
            startedAtUtc,
            startedAtUtc.AddMilliseconds(elapsedMilliseconds),
            elapsedMilliseconds,
            caloriesMilliKcal,
            endReason,
            [new ActionMetric(action, MetricKind.Repetition, 12U, elapsedMilliseconds, caloriesMilliKcal)]);
    }

    /// <summary>验证重复开始不创建新会话，重复停止返回同一摘要。</summary>
    private static async Task TestMockDeviceIdempotencyAsync()
    {
        // 使用 5ms tick 加速测试，动作脚本固定为深蹲。
        await using MockDeviceSession device = new(TimeSpan.FromMilliseconds(5), [ActionId.Squat]);
        // 建立模拟链路。
        await device.ConnectAsync();
        // 第一次开始创建会话。
        await device.StartAsync();
        // 读取首次会话序号。
        uint firstSequence = (await device.GetSnapshotAsync()).SessionSequence;
        // 重复开始必须保持同一会话。
        await device.StartAsync();
        // 读取重复命令后序号。
        uint secondSequence = (await device.GetSnapshotAsync()).SessionSequence;
        // 核对会话序号未增加。
        Assert(firstSequence == secondSequence, "Mock 重复开始创建了第二个会话。");
        // 等待若干 tick 产生指标。
        await Task.Delay(35);
        // 第一次停止创建摘要。
        TrainingSessionSummary first = await device.StopAsync() ?? throw new InvalidOperationException("Mock 停止未返回摘要。");
        // 第二次停止返回缓存摘要。
        TrainingSessionSummary second = await device.StopAsync() ?? throw new InvalidOperationException("Mock 重复停止未返回摘要。");
        // 同一引用证明没有重复生成时间不同的摘要。
        Assert(ReferenceEquals(first, second), "Mock 重复停止没有返回同一摘要实例。");
    }

    /// <summary>验证 PC 链路断开不会停止设备内部训练，重连恢复最新状态。</summary>
    private static async Task TestMockDisconnectRecoveryAsync()
    {
        // 使用短 tick 和单一行走动作。
        await using MockDeviceSession device = new(TimeSpan.FromMilliseconds(5), [ActionId.Walk]);
        // 连接并开始训练。
        await device.ConnectAsync();
        // 开始会话。
        await device.StartAsync();
        // 等待初始状态推进。
        await Task.Delay(25);
        // 记录断线前时长。
        uint before = (await device.GetSnapshotAsync()).ElapsedMilliseconds;
        // 断开 PC 链路。
        await device.DisconnectAsync();
        // 断线期间等待设备时钟继续。
        await Task.Delay(35);
        // 重连 Mock。
        await device.ConnectAsync();
        // 读取恢复快照。
        LiveState recovered = await device.GetSnapshotAsync();
        // 会话仍为 Running 且时长增加。
        Assert(recovered.DeviceState == FitnessDeviceState.Running && recovered.ElapsedMilliseconds > before, "Mock 断线后设备会话没有继续运行。");
        // 清理会话。
        _ = await device.StopAsync();
    }

    /// <summary>验证后台 Mock 状态能经串行调度器更新 ViewModel、保存摘要并触发动画。</summary>
    private static async Task TestLiveViewModelStateFlowAsync()
    {
        // 创建独立仓储目录。
        string directory = Path.Combine(Path.GetTempPath(), $"fitness-viewmodel-{Guid.NewGuid():N}");
        // 创建仓储实例。
        using JsonSessionRepository repository = new(Path.Combine(directory, "sessions.json"));
        // 初始化仓储。
        await repository.InitializeAsync();
        // 创建快速 Mock。
        await using MockDeviceSession device = new(TimeSpan.FromMilliseconds(5), [ActionId.JumpingSquat]);
        // 创建同步串行测试调度器。
        ImmediateUiDispatcher dispatcher = new();
        // 创建本地动画控制器。
        LocalActionAnimationController animation = new();
        // 创建待测实时 ViewModel。
        using LiveTrainingViewModel viewModel = new(device, repository, dispatcher, animation);

        try
        {
            // 连接设备。
            await device.ConnectAsync();
            // 通过命令启动训练。
            await viewModel.StartCommand.ExecuteAsync();
            // 等待后台状态事件。
            await Task.Delay(45);
            // ViewModel 必须应用非零修订和跳跃深蹲动作。
            Assert(viewModel.StateRevision > 0U && viewModel.ActionName == "跳跃深蹲", "实时 ViewModel 未应用后台状态。");
            // 动画控制器必须映射非未知符号。
            Assert(viewModel.ActionGlyph != "?", "实时 ViewModel 未驱动本地动作动画。");
            // 停止并保存摘要。
            await viewModel.StopCommand.ExecuteAsync();
            // 历史仓储必须出现一条记录。
            IReadOnlyList<TrainingSessionSummary> sessions = await repository.ListAsync(new SessionQuery { Limit = 10 });
            // 核对停止命令仅保存一个会话。
            Assert(sessions.Count == 1, "实时 ViewModel 停止后未保存唯一会话。");
        }
        finally
        {
            // 删除测试创建的目录。
            if (Directory.Exists(directory))
            {
                // 递归删除仓储文件。
                Directory.Delete(directory, true);
            }
        }
    }

    /// <summary>验证 11 个动作都具有本地视觉资源键和中文名。</summary>
    private static void TestLocalActionAnimationMapping()
    {
        // 创建本地动画控制器。
        LocalActionAnimationController controller = new();
        // 遍历全部可识别动作，不包含 Unknown。
        foreach (ActionId action in Enum.GetValues<ActionId>().Where(value => value != ActionId.Unknown))
        {
            // 切换到当前动作。
            controller.SetAction(action);
            // 动作、名称、符号和资源键必须完整。
            Assert(controller.Current.Action == action
                && !string.IsNullOrWhiteSpace(controller.Current.ChineseName)
                && !string.IsNullOrWhiteSpace(controller.Current.Glyph)
                && controller.Current.ResourceKey.StartsWith("action.", StringComparison.Ordinal), $"动作 {action} 缺少本地动画映射。");
        }
    }

    // 创建用于仓储测试的固定摘要。
    private static TrainingSessionSummary CreateTestSummary(string deviceId, uint sequence, uint elapsedMilliseconds, uint caloriesMilliKcal)
    {
        // 使用稳定 UTC 时间避免区域设置影响排序。
        DateTimeOffset started = new(2026, 7, 14, 1, 0, 0, TimeSpan.Zero);
        // 返回包含一个深蹲指标的摘要。
        return new TrainingSessionSummary(
            deviceId,
            sequence,
            started,
            started.AddMilliseconds(elapsedMilliseconds),
            elapsedMilliseconds,
            caloriesMilliKcal,
            "测试停止",
            [new ActionMetric(ActionId.Squat, MetricKind.Repetition, 3, elapsedMilliseconds, caloriesMilliKcal)]);
    }

    // 测试路径选择器返回固定路径，不打开 Windows 对话框。
    private sealed class FixedHistoryDestinationPicker : IHistoryExportDestinationPicker
    {
        // 保存测试输出绝对路径。
        private readonly string _path;

        /// <summary>创建固定路径选择器。</summary>
        public FixedHistoryDestinationPicker(string path)
        {
            // 保存调用者路径。
            _path = path;
        }

        /// <inheritdoc />
        public Task<string?> PickCsvPathAsync(string suggestedFileName)
        {
            // 默认文件名必须带 csv 扩展名，验证 ViewModel 建议合同。
            Assert(suggestedFileName.EndsWith(".csv", StringComparison.OrdinalIgnoreCase), "历史导出建议文件名缺少 CSV 扩展名。");
            // 返回固定测试路径。
            return Task.FromResult<string?>(_path);
        }
    }

    // 测试 IMU 路径选择器按命令顺序返回预设路径，不打开 Windows 对话框。
    private sealed class QueuedImuDestinationPicker : IImuExportDestinationPicker
    {
        // 保存尚未返回的绝对路径；队列顺序对应实时导出和回看导出。
        private readonly Queue<string> _paths;

        /// <summary>创建按顺序返回路径的 IMU 选择器。</summary>
        public QueuedImuDestinationPicker(params string[] paths)
        {
            // 路径数组不能为空引用。
            ArgumentNullException.ThrowIfNull(paths);
            // 复制到独立队列，调用者后续修改原数组不会影响测试行为。
            _paths = new Queue<string>(paths);
        }

        /// <inheritdoc />
        public Task<string?> PickCsvPathAsync(string suggestedFileName)
        {
            // 默认文件名必须是中文 IMU 范围名并带 csv 扩展名。
            Assert(
                suggestedFileName.StartsWith("IMU_", StringComparison.Ordinal) &&
                suggestedFileName.EndsWith(".csv", StringComparison.OrdinalIgnoreCase),
                "IMU 导出建议文件名不符合中文范围合同。");
            // 每次命令都必须有独立预设路径，缺路径表示 ViewModel 多开了保存对话框。
            Assert(_paths.Count > 0, "IMU 路径选择器收到超出预期的调用。" );
            // 取出当前命令对应路径。
            string selectedPath = _paths.Dequeue();
            // 返回固定路径，不显示真实系统对话框。
            return Task.FromResult<string?>(selectedPath);
        }
    }

    /// <summary>
    /// 条件为假时抛出包含业务原因的异常，统一测试失败行为。
    /// </summary>
    private static void Assert(bool condition, string message)
    {
        // 条件为真时当前断言通过，直接返回。
        if (condition)
        {
            // 不创建异常，继续执行下一项检查。
            return;
        }

        // 条件为假时抛出明确失败原因，由 Main 转换为退出码 1。
        throw new InvalidOperationException(message);
    }
}
