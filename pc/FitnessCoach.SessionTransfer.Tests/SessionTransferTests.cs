// 引入确定性小端构造测试载荷。
using System.Buffers.Binary;
// 引入待测蓝牙协议和状态机。
using FitnessCoach.Bluetooth;
// 引入领域对象。
using FitnessCoach.Domain;
// 引入 JSON 幂等仓储。
using FitnessCoach.Infrastructure;

// 独立测试命名空间避免与现有无框架测试入口耦合。
namespace FitnessCoach.SessionTransfer.Tests;

/// <summary>覆盖 Request/Response/Data codec、Windows 拉取和复合键幂等。</summary>
public static class SessionTransferTests
{
    // 记录实际断言数，避免测试空跑。
    private static int _assertionCount;

    /// <summary>运行全部异步测试。</summary>
    public static async Task RunAllAsync()
    {
        // 先测固定请求字节。
        TestRequestAndPayloadCodecs();
        // 再测真实 WindowsBleDeviceSession 的分页状态机。
        await TestWindowsSessionPullAsync().ConfigureAwait(false);
        // 最后测 JSON/SQLite 共用复合键语义。
        await TestRepositoryIdempotenceAsync().ConfigureAwait(false);
        // 至少执行 20 个断言，防止入口遗漏。
        Assert(_assertionCount >= 20, "会话同步测试断言数不足。" );
    }

    // 核对请求、响应和 80 字节摘要。
    private static void TestRequestAndPayloadCodecs()
    {
        // 构造游标 7、页大小 12、请求号 0x01020304 的 LIST。
        byte[] request = SessionTransferCodec.EncodeListRequest(0x01020304U, 7U, 12);
        // 固定长度必须为 12。
        Assert(request.Length == 12, "请求长度错误。" );
        // 核对版本、操作和小端字段。
        Assert(request[0] == 1 && request[1] == 1, "请求版本或操作错误。" );
        Assert(BinaryPrimitives.ReadUInt16LittleEndian(request.AsSpan(2, 2)) == 12, "请求页大小错误。" );
        Assert(BinaryPrimitives.ReadUInt32LittleEndian(request.AsSpan(4, 4)) == 0x01020304U, "请求号错误。" );
        Assert(BinaryPrimitives.ReadUInt32LittleEndian(request.AsSpan(8, 4)) == 7U, "请求游标错误。" );

        // 构造固定成功响应。
        byte[] responsePayload = BuildResponse(0x01020304U, 9U, 5, 2, isEnd: false);
        // 解码响应。
        Assert(SessionTransferCodec.TryDecodeResponse(responsePayload, out SessionTransferResponseV1? response, out _), "响应解码失败。" );
        // 核对游标、数量和 flags。
        Assert(response!.RequestId == 0x01020304U, "响应请求号错误。" );
        Assert(response.NextCursorSessionSequence == 9U, "响应游标错误。" );
        Assert(response.TotalCount == 5 && response.ItemCount == 2, "响应数量错误。" );
        Assert(response.HasData && !response.IsEnd, "响应 flags 错误。" );

        // 构造一条完整数据。
        byte[] dataPayload = BuildData(0x01020304U, 0, 2, 5, 8U, isLast: false, isEnd: false);
        // 解码数据。
        Assert(SessionTransferCodec.TryDecodeData(dataPayload, out SessionTransferDataV1? data, out _), "数据解码失败。" );
        // 核对摘要全部关键宽度字段。
        Assert(data!.Summary.SessionSequence == 8U, "摘要序号错误。" );
        Assert(data.Summary.StartUnixMilliseconds == 1_700_000_000_008UL, "摘要 UTC 错误。" );
        Assert(data.Summary.DurationMilliseconds == 60_008UL, "摘要时长错误。" );
        Assert(data.Summary.GrossMicroKcal == 123_008UL, "摘要热量错误。" );
        // 尾随字节必须拒绝。
        Assert(!SessionTransferCodec.TryDecodeData([.. dataPayload, 0], out _, out _), "尾随字节未拒绝。" );
    }

    // 使用 fake GATT 驱动真实会话状态机完成一页拉取。
    private static async Task TestWindowsSessionPullAsync()
    {
        // fake transport 在收到 LIST 后发布响应和两条数据。
        await using FakeTransferTransport transport = new();
        // 使用短但非零超时，测试不等待真实 2 秒。
        await using WindowsBleDeviceSession session = new(
            transport,
            TimeSpan.FromSeconds(1),
            [TimeSpan.FromMilliseconds(10)],
            static (delay, token) => Task.Delay(delay, token));
        // 完成 manifest、五个订阅和 LiveState 快照。
        await session.ConnectAsync().ConfigureAwait(false);
        // 拉取 cursor 1 之后的一页。
        SessionTransferPage page = await session.PullSessionSummariesAsync(1U, 2).ConfigureAwait(false);
        // 核对页元数据。
        Assert(page.NextCursorSessionSequence == 3U, "Windows 页游标错误。" );
        Assert(page.TotalCount == 3, "Windows 页总数错误。" );
        Assert(page.IsEnd, "Windows 页应为终点。" );
        Assert(page.Summaries.Count == 2, "Windows 页摘要数错误。" );
        // 设备按旧到新返回 2、3。
        Assert(page.Summaries[0].SessionSequence == 2U, "Windows 第 1 条顺序错误。" );
        Assert(page.Summaries[1].SessionSequence == 3U, "Windows 第 2 条顺序错误。" );
        // 复合键包含设备名和序号。
        Assert(page.Summaries[0].StorageKey == "BPNN-FIT-TEST:2", "Windows 复合键错误。" );
    }

    // 重复保存同一复合键不能增加第二条历史。
    private static async Task TestRepositoryIdempotenceAsync()
    {
        // 使用项目测试进程临时目录；退出后清理。
        string directory = Path.Combine(Path.GetTempPath(), $"fitness-session-transfer-{Guid.NewGuid():N}");
        // 创建仓储路径。
        string path = Path.Combine(directory, "sessions.json");

        try
        {
            // 初始化 JSON 仓储；SQLite 实现需遵守相同 ISessionRepository 键合同。
            using JsonSessionRepository repository = new(path);
            // 创建目录和 v1 信封。
            await repository.InitializeAsync().ConfigureAwait(false);
            // 从线上摘要生成领域摘要。
            byte[] payload = BuildData(7U, 0, 1, 1, 42U, isLast: true, isEnd: true);
            Assert(SessionTransferCodec.TryDecodeData(payload, out SessionTransferDataV1? data, out _), "幂等测试摘要解码失败。" );
            TrainingSessionSummary summary = data!.Summary.ToTrainingSessionSummary("DEVICE-A");
            // 重复保存同一对象两次。
            await repository.SaveAsync(summary).ConfigureAwait(false);
            await repository.SaveAsync(summary).ConfigureAwait(false);
            // 查询设备历史。
            IReadOnlyList<TrainingSessionSummary> rows = await repository.ListAsync(new SessionQuery
            {
                // 限定同一 device_id。
                DeviceId = "DEVICE-A",
                // 读取足够覆盖测试数据。
                Limit = 10,
            }).ConfigureAwait(false);
            // (device_id,session_seq) 相同只能有一条。
            Assert(rows.Count == 1, "重复同步产生重复 JSON 记录。" );
            Assert(rows[0].StorageKey == "DEVICE-A:42", "仓储复合键错误。" );
        }
        finally
        {
            // 清理测试目录，不保留用户数据。
            if (Directory.Exists(directory))
            {
                // 递归删除本测试唯一 GUID 目录。
                Directory.Delete(directory, recursive: true);
            }
        }
    }

    // 构造固定 16 字节响应。
    private static byte[] BuildResponse(uint requestId, uint nextCursor, ushort totalCount, ushort itemCount, bool isEnd)
    {
        // 分配固定载荷。
        byte[] output = new byte[16];
        // 写版本、LIST、OK 和 flags。
        output[0] = 1;
        output[1] = 1;
        output[2] = 0;
        output[3] = (byte)((itemCount > 0 ? 1 : 0) | (isEnd ? 2 : 0));
        // 写小端字段。
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(4, 4), requestId);
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(8, 4), nextCursor);
        BinaryPrimitives.WriteUInt16LittleEndian(output.AsSpan(12, 2), totalCount);
        BinaryPrimitives.WriteUInt16LittleEndian(output.AsSpan(14, 2), itemCount);
        // 返回载荷。
        return output;
    }

    // 构造固定 80 字节摘要数据。
    private static byte[] BuildData(
        uint requestId,
        ushort itemIndex,
        ushort itemCount,
        ushort totalCount,
        uint sessionSequence,
        bool isLast,
        bool isEnd)
    {
        // 分配固定载荷。
        byte[] output = new byte[80];
        // 写数据头版本、摘要类型和 flags。
        output[0] = 1;
        output[1] = 1;
        output[2] = (byte)((isLast ? 1 : 0) | (isEnd ? 2 : 0));
        // 写请求和分页字段。
        BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(4, 4), requestId);
        BinaryPrimitives.WriteUInt16LittleEndian(output.AsSpan(8, 2), itemIndex);
        BinaryPrimitives.WriteUInt16LittleEndian(output.AsSpan(10, 2), itemCount);
        BinaryPrimitives.WriteUInt16LittleEndian(output.AsSpan(12, 2), totalCount);
        // 摘要起点固定偏移 16。
        Span<byte> summary = output.AsSpan(16, 64);
        // 写摘要版本、长度、主键和事件水位。
        BinaryPrimitives.WriteUInt16LittleEndian(summary.Slice(0, 2), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(summary.Slice(2, 2), 64);
        BinaryPrimitives.WriteUInt32LittleEndian(summary.Slice(4, 4), sessionSequence);
        BinaryPrimitives.WriteUInt32LittleEndian(summary.Slice(8, 4), sessionSequence * 10U);
        // 动作固定为 squat=6；指标固定为重复次数=0。
        summary[12] = 6;
        summary[13] = 0;
        // 写 flags、UTC、时长、指标和热量。
        BinaryPrimitives.WriteUInt16LittleEndian(summary.Slice(14, 2), 0x0100);
        BinaryPrimitives.WriteUInt64LittleEndian(summary.Slice(16, 8), 1_700_000_000_000UL + sessionSequence);
        BinaryPrimitives.WriteUInt64LittleEndian(summary.Slice(24, 8), 60_000UL + sessionSequence);
        BinaryPrimitives.WriteUInt64LittleEndian(summary.Slice(32, 8), 20UL + sessionSequence);
        BinaryPrimitives.WriteUInt64LittleEndian(summary.Slice(40, 8), 123_000UL + sessionSequence);
        BinaryPrimitives.WriteUInt64LittleEndian(summary.Slice(48, 8), 100_000UL + sessionSequence);
        // 写稳定度和事件数。
        BinaryPrimitives.WriteUInt16LittleEndian(summary.Slice(56, 2), 28_000);
        BinaryPrimitives.WriteUInt16LittleEndian(summary.Slice(58, 2), 24_000);
        BinaryPrimitives.WriteUInt32LittleEndian(summary.Slice(60, 4), 5U + sessionSequence);
        // 返回完整载荷。
        return output;
    }

    // 创建逻辑帧并编码 CRC16。
    private static byte[] BuildFrame(ProtocolMessageType messageType, ushort sequence, ReadOnlyMemory<byte> payload)
    {
        // 使用固定协议版本和测试单调时间。
        return BleFrameCodec.Encode(new BleLogicalFrame(1, 0, (byte)messageType, 0, sequence, 1234U, payload.Span));
    }

    // 轻量断言抛出异常，控制台入口以非零退出码失败。
    private static void Assert(bool condition, string message)
    {
        // 累计断言数。
        _assertionCount++;
        // 条件失败时终止测试。
        if (!condition)
        {
            // 抛出可读原因。
            throw new InvalidOperationException(message);
        }
    }

    // fake transport 只实现 Connect 所需快照和 TransferRequest 页响应。
    private sealed class FakeTransferTransport : IWindowsBleTransport
    {
        // 固定 132 字节黄金 Manifest，保证独立会话同步测试也经过正式兼容解析。
        private static readonly byte[] CompatibleManifest =
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
        // Transfer Control 写入可能按 MTU 分片，使用真实重组器。
        private readonly BleFrameReassembler _requestReassembler = new();
        // 连接阶段自动校时写 Control Point，使用独立重组器避免和 Transfer 序列混合。
        private readonly BleFrameReassembler _controlReassembler = new();
        // 记录连接状态。
        private bool _connected;
        // notification 逻辑序号。
        private ushort _sequence;

        /// <inheritdoc />
        public event EventHandler<BleGattValueReceivedEventArgs>? ValueReceived;
        /// <inheritdoc />
        public event EventHandler<BleTransportDisconnectedEventArgs>? Disconnected;
        /// <inheritdoc />
        public bool IsConnected => _connected;

        /// <inheritdoc />
        public Task<BleConnectedDevice> ScanAndConnectAsync(string? preferredDeviceId, CancellationToken cancellationToken)
        {
            // 尊重调用方取消。
            cancellationToken.ThrowIfCancellationRequested();
            // 标记链路可用。
            _connected = true;
            // 返回 MTU247，仍会经过统一分片包络。
            return Task.FromResult(new BleConnectedDevice("win-device-test", "BPNN-FIT-TEST", 247));
        }

        /// <inheritdoc />
        public Task<byte[]> ReadAsync(Guid characteristicUuid, CancellationToken cancellationToken)
        {
            // 尊重取消。
            cancellationToken.ThrowIfCancellationRequested();
            // Manifest 必须满足正式 1.0、297维、11类CRC和能力位合同。
            if (characteristicUuid == ProtocolConstants.ManifestUuid)
            {
                // 返回独立 132 字节黄金 TLV 副本。
                return Task.FromResult(CompatibleManifest.ToArray());
            }

            // LiveState 构造合法空闲快照。
            if (characteristicUuid == ProtocolConstants.LiveStateUuid)
            {
                // 创建领域状态。
                LiveState state = new(
                    1U,
                    1U,
                    0U,
                    FitnessDeviceState.Idle,
                    ActionId.Unknown,
                    FitnessCoach.Domain.MetricKind.None,
                    100,
                    0U,
                    0,
                    0U,
                    DataQualityFlags.None,
                    PowerFlags.None,
                    byte.MaxValue);
                // 编码状态 payload 和逻辑帧。
                return Task.FromResult(BuildFrame(ProtocolMessageType.LiveState, ++_sequence, LiveStateCodec.Encode(state)));
            }

            // 其它特征不支持读取。
            throw new InvalidOperationException($"测试 transport 不支持读取 {characteristicUuid}。" );
        }

        /// <inheritdoc />
        public Task SubscribeAsync(Guid characteristicUuid, bool useIndication, CancellationToken cancellationToken)
        {
            // 测试只验证会话确实发起订阅，不需要保存 CCCD。
            cancellationToken.ThrowIfCancellationRequested();
            // 返回完成。
            return Task.CompletedTask;
        }

        /// <inheritdoc />
        public Task WriteAsync(Guid characteristicUuid, ReadOnlyMemory<byte> value, bool withResponse, CancellationToken cancellationToken)
        {
            // 尊重取消并要求链路存在。
            cancellationToken.ThrowIfCancellationRequested();
            if (!_connected)
            {
                // 模拟真实 transport 断线错误。
                throw new InvalidOperationException("测试 BLE 未连接。" );
            }

            // 配置控制点使用有响应写；连接阶段自动校时必须收到成功 indication。
            if (characteristicUuid == ProtocolConstants.ControlPointUuid)
            {
                // 控制写必须要求 GATT Write With Response。
                if (!withResponse)
                {
                    // 拒绝与产品合同不符的无响应控制写。
                    throw new InvalidOperationException("测试只接受有响应 Control Point 写入。" );
                }

                // 使用控制点专属重组器拼接完整逻辑帧。
                FragmentPushStatus controlStatus = _controlReassembler.Push(value.Span, out byte[]? controlFrameBytes, out ProtocolDecodeError controlError);
                // 中间片等待后续写入。
                if (controlStatus == FragmentPushStatus.AcceptedIncomplete)
                {
                    // 当前写已被合法接收。
                    return Task.CompletedTask;
                }

                // 坏分片或缺少完整帧时立即暴露测试错误。
                if ((controlStatus != FragmentPushStatus.Completed) || (controlFrameBytes is null))
                {
                    // 报告控制帧重组错误。
                    throw new InvalidDataException($"测试校时请求重组失败：{controlError}。" );
                }

                // 解码完整控制逻辑帧并验证 CRC/长度。
                if (!BleFrameCodec.TryDecode(controlFrameBytes, out BleLogicalFrame? controlFrame, out ProtocolDecodeError controlFrameError))
                {
                    // 报告逻辑帧解码错误。
                    throw new InvalidDataException($"测试校时请求帧错误：{controlFrameError}。" );
                }

                // 连接阶段只预期命令 6；其它配置命令不属于本独立补传测试。
                if ((controlFrame!.MessageType != (byte)ProtocolMessageType.ControlRequest) ||
                    (controlFrame.Payload.Length < ControlPointCodec.RequestHeaderSize) ||
                    (controlFrame.Payload.Span[4] != (byte)ControlCommandId.SyncTime))
                {
                    // 防止 Fake 静默接受错误命令或短 payload。
                    throw new InvalidDataException("独立补传测试收到非预期控制命令。" );
                }

                // 从控制请求读取小端 request_id，用于匹配等待中的命令。
                uint controlRequestId = BinaryPrimitives.ReadUInt32LittleEndian(controlFrame.Payload.Span.Slice(0, 4));
                // 分配固定 12 字节成功响应头。
                byte[] controlResponse = new byte[ControlPointCodec.ResponseHeaderSize];
                // 回写相同 request_id。
                BinaryPrimitives.WriteUInt32LittleEndian(controlResponse.AsSpan(0, 4), controlRequestId);
                // 回写命令 6。
                controlResponse[4] = (byte)ControlCommandId.SyncTime;
                // status=0 表示设备执行成功。
                controlResponse[5] = 0;
                // error_code=0 表示无设备错误。
                BinaryPrimitives.WriteUInt16LittleEndian(controlResponse.AsSpan(6, 2), 0);
                // 返回任意单调正 revision；本测试只验证校时不阻塞连接。
                BinaryPrimitives.WriteUInt32LittleEndian(controlResponse.AsSpan(8, 4), 2U);
                // 预先编码完整 Control Point indication；真实 BLE indication 在写响应返回后异步到达。
                byte[] encodedControlResponse = BuildFrame(ProtocolMessageType.ControlResponse, ++_sequence, controlResponse);
                // 异步发布避免 Fake 在 GATT Write 调用栈内重入通知泵，贴近 WinRT 实际回调时序。
                _ = Task.Run(async () =>
                {
                    // 短暂让出执行权，确保 WriteAsync 已返回且会话进入 indication 等待。
                    await Task.Delay(1).ConfigureAwait(false);
                    // 发布 Control Point indication，唤醒真实会话的自动校时等待。
                    ValueReceived?.Invoke(
                        this,
                        new BleGattValueReceivedEventArgs(
                            ProtocolConstants.ControlPointUuid,
                            encodedControlResponse));
                });
                // 完成当前控制写。
                return Task.CompletedTask;
            }

            // 除自动校时外，本测试只允许 Transfer Control 有响应写入。
            if ((characteristicUuid != ProtocolConstants.TransferControlUuid) || !withResponse)
            {
                // 报告错误路径。
                throw new InvalidOperationException("测试只接受有响应 Transfer Control 写入。" );
            }

            // 重组真实分片。
            FragmentPushStatus status = _requestReassembler.Push(value.Span, out byte[]? completeFrame, out ProtocolDecodeError error);
            // 中间片正常返回。
            if (status == FragmentPushStatus.AcceptedIncomplete)
            {
                // 等待后续片。
                return Task.CompletedTask;
            }

            // 拒绝坏分片。
            if (status != FragmentPushStatus.Completed || completeFrame is null)
            {
                // 暴露测试解码原因。
                throw new InvalidDataException($"测试请求重组失败：{error}。" );
            }

            // 解码完整类型 5 帧。
            if (!BleFrameCodec.TryDecode(completeFrame, out BleLogicalFrame? frame, out ProtocolDecodeError frameError))
            {
                // 报告 CRC/长度错误。
                throw new InvalidDataException($"测试请求帧错误：{frameError}。" );
            }

            // 读取固定请求号和游标。
            uint requestId = BinaryPrimitives.ReadUInt32LittleEndian(frame!.Payload.Span.Slice(4, 4));
            uint cursor = BinaryPrimitives.ReadUInt32LittleEndian(frame.Payload.Span.Slice(8, 4));
            // 本测试固定 cursor=1，返回 session 2、3。
            if (cursor != 1U)
            {
                // 防止测试悄悄覆盖错误游标。
                throw new InvalidDataException("测试收到非预期游标。" );
            }

            // 先发布类型 6 indication。
            ValueReceived?.Invoke(
                this,
                new BleGattValueReceivedEventArgs(
                    ProtocolConstants.TransferControlUuid,
                    BuildFrame(ProtocolMessageType.TransferResponse, ++_sequence, BuildResponse(requestId, 3U, 3, 2, isEnd: true))));
            // 再发布两条类型 7 notification。
            ValueReceived?.Invoke(
                this,
                new BleGattValueReceivedEventArgs(
                    ProtocolConstants.TransferDataUuid,
                    BuildFrame(ProtocolMessageType.TransferData, ++_sequence, BuildData(requestId, 0, 2, 3, 2U, isLast: false, isEnd: false))));
            ValueReceived?.Invoke(
                this,
                new BleGattValueReceivedEventArgs(
                    ProtocolConstants.TransferDataUuid,
                    BuildFrame(ProtocolMessageType.TransferData, ++_sequence, BuildData(requestId, 1, 2, 3, 3U, isLast: true, isEnd: true))));
            // GATT 写完成。
            return Task.CompletedTask;
        }

        /// <inheritdoc />
        public Task DisconnectAsync(CancellationToken cancellationToken)
        {
            // 尊重取消。
            cancellationToken.ThrowIfCancellationRequested();
            // 标记断开；主动路径不触发 Disconnected。
            _connected = false;
            // 返回完成。
            return Task.CompletedTask;
        }

        /// <inheritdoc />
        public ValueTask DisposeAsync()
        {
            // 清除状态。
            _connected = false;
            // 保留事件字段引用以满足接口；无非托管资源。
            _ = Disconnected;
            // 返回完成。
            return ValueTask.CompletedTask;
        }
    }
}
