// 引入文件、目录和路径 API，导出器不依赖调用进程的当前目录。
using System.IO;
// 引入 UTF-8 编码，带 BOM 的 CSV 可由中文 Windows Excel 直接打开。
using System.Text;
// 引入设备动作、状态和指标枚举，CSV 使用领域事实而不是界面字符串反解析。
using FitnessCoach.Domain;
// 引入分类诊断和权威计数事件对象，导出时按设备单调时间关联到六轴点。
using FitnessCoach.Bluetooth;
// 引入统一中文显示映射，确保界面与导出使用相同动作名称。
using FitnessCoach.App.ViewModels;
// 引入 WPF 系统保存文件对话框，只有用户确认路径后才允许写盘。
using Microsoft.Win32;

// IMU 导出服务位于应用服务层，诊断 ViewModel 只负责选择样本范围。
namespace FitnessCoach.App.Services;

/// <summary>
/// 保存一条可离线复算的 25 Hz 六轴诊断样本；通道顺序固定为 gx、gy、gz、ax、ay、az。
/// </summary>
public sealed record ImuExportSample(
    // 设备自启动后递增的 uint32 样本序号，用于检查 BLE 丢样和自然回绕。
    uint SampleIndex,
    // 设备单调毫秒时间，用作离线滤波、周期计数和曲线横轴，不受 PC 时钟调整影响。
    uint DeviceMonotonicMilliseconds,
    // PC 收到完整 BLE 记录时的 UTC 时间，用于定位无线传输和界面调度延迟。
    DateTimeOffset ReceivedAtUtc,
    // 陀螺仪 X 轴 int16 诊断码；除以 16.4 得到度每秒。
    short GxCode,
    // 陀螺仪 Y 轴 int16 诊断码；除以 16.4 得到度每秒。
    short GyCode,
    // 陀螺仪 Z 轴 int16 诊断码；除以 16.4 得到度每秒。
    short GzCode,
    // 加速度 X 轴 int16 诊断码；除以 4096 得到重力加速度倍数 g。
    short AxCode,
    // 加速度 Y 轴 int16 诊断码；除以 4096 得到重力加速度倍数 g。
    short AyCode,
    // 加速度 Z 轴 int16 诊断码；除以 4096 得到重力加速度倍数 g。
    short AzCode,
    // 陀螺仪 X 轴物理量，单位度每秒。
    double GxDegreesPerSecond,
    // 陀螺仪 Y 轴物理量，单位度每秒。
    double GyDegreesPerSecond,
    // 陀螺仪 Z 轴物理量，单位度每秒。
    double GzDegreesPerSecond,
    // 加速度 X 轴物理量，单位 g。
    double AxG,
    // 加速度 Y 轴物理量，单位 g。
    double AyG,
    // 加速度 Z 轴物理量，单位 g。
    double AzG,
    // 设备质量位原值；振动污染、丢样或重采样异常均不得在导出时丢失。
    ushort QualityFlags,
    // 当前样本所属设备持久化会话序号；零表示尚未收到权威状态。
    uint SessionSequence,
    // 收到当前样本时最近的设备权威状态。
    FitnessDeviceState? DeviceState,
    // 收到当前样本时设备已经锁定的稳定动作，Unknown 表示尚未锁类。
    ActionId StableAction,
    // 设备权威稳定动作 Q15 换算后的 0～1 置信度。
    double StableConfidence,
    // 当前设备权威累计值的业务单位。
    MetricKind MetricKind,
    // 当前样本到达时最近一次 LiveState 的权威累计值。
    uint MetricTotal);

/// <summary>保存一条带会话归属和 PC 到达时间的双 M0 分类诊断。</summary>
public sealed record InferenceExportSample(
    // 诊断到达时设备最近的持久化会话序号，用于阻止跨会话前向填充旧类别。
    uint SessionSequence,
    // PC 收到完整分类通知的 UTC，只用于诊断无线与调度延迟。
    DateTimeOffset ReceivedAtUtc,
    // 固件类型 9 发布的三路模型与窗口设备时间事实。
    InferenceDiagnosticV1 Diagnostic);

/// <summary>保存一次权威次数或步数增加事件及其精确设备单调时间。</summary>
public sealed record MetricEventExportSample(
    // PC 收到完整 EventV1 的 UTC，只用于诊断无线延迟。
    DateTimeOffset ReceivedAtUtc,
    // Event 逻辑帧中的设备单调毫秒；固件必须使用原始 MetricEvent 时刻。
    uint DeviceMonotonicMilliseconds,
    // 设备权威低延迟事件；累计值最终仍由 LiveState 恢复。
    DeviceEventV1 Event);

/// <summary>定义 IMU CSV 写入边界，测试可使用临时文件核对列顺序和数值精度。</summary>
public interface IImuCsvExporter
{
    /// <summary>把六轴、分类窗口和计数事件按设备时间关联后写入 CSV；空集合仍生成合法表头。</summary>
    Task ExportAsync(
        IReadOnlyList<ImuExportSample> samples,
        IReadOnlyList<InferenceExportSample> inferenceSamples,
        IReadOnlyList<MetricEventExportSample> metricEvents,
        string filePath,
        CancellationToken cancellationToken = default);
}

/// <summary>定义 IMU 保存位置选择边界，测试替身无需打开系统窗口。</summary>
public interface IImuExportDestinationPicker
{
    /// <summary>显示保存对话框；用户取消时返回 null。</summary>
    Task<string?> PickCsvPathAsync(string suggestedFileName);
}

/// <summary>使用 Windows SaveFileDialog 选择 IMU CSV 输出路径。</summary>
public sealed class WpfImuExportDestinationPicker : IImuExportDestinationPicker
{
    /// <inheritdoc />
    public Task<string?> PickCsvPathAsync(string suggestedFileName)
    {
        // 创建只允许 CSV 的系统保存对话框。
        SaveFileDialog dialog = new()
        {
            // 使用调用者生成的范围和日期文件名，用户仍可手工修改。
            FileName = suggestedFileName,
            // 默认扩展名固定为 csv，避免生成无扩展名诊断文件。
            DefaultExt = ".csv",
            // 过滤器优先显示 CSV，再允许用户查看全部文件。
            Filter = "IMU 逗号分隔值文件|*.csv|所有文件|*.*",
            // 覆盖已有文件前由系统再次确认，避免误删现场数据。
            OverwritePrompt = true,
            // 目标目录必须存在，避免保存对话框返回不可写路径。
            CheckPathExists = true,
        };
        // WPF 返回 true 表示用户确认；false 或 null 表示取消且不得写盘。
        string? selectedPath = dialog.ShowDialog() == true ? dialog.FileName : null;
        // 接口保持异步形态，便于测试和未来替换其它桌面框架。
        return Task.FromResult(selectedPath);
    }
}

/// <summary>
/// 把设备单调时间、PC 接收时间、原始定点码、物理量和质量位写成稳定列顺序的 CSV。
/// </summary>
public sealed class ImuCsvExporter : IImuCsvExporter
{
    // 当前量产分类模型只按用户确认的右手腕佩戴域验收；导出固定写入该事实，防止离线分析混用左腕样本。
    private const string SupportedWristSide = "右手腕";

    /// <inheritdoc />
    public async Task ExportAsync(
        IReadOnlyList<ImuExportSample> samples,
        IReadOnlyList<InferenceExportSample> inferenceSamples,
        IReadOnlyList<MetricEventExportSample> metricEvents,
        string filePath,
        CancellationToken cancellationToken = default)
    {
        // 样本列表不能为空引用；空集合仍可输出表头供工具识别格式。
        ArgumentNullException.ThrowIfNull(samples);
        // 分类列表不能为空引用；没有分类窗口时模型列保持空白。
        ArgumentNullException.ThrowIfNull(inferenceSamples);
        // 计数事件列表不能为空引用；没有事件时标记列为否。
        ArgumentNullException.ThrowIfNull(metricEvents);
        // 空路径没有用户授权的目标位置，必须拒绝。
        if (string.IsNullOrWhiteSpace(filePath))
        {
            // 抛出参数错误并由 ViewModel 转成中文恢复提示。
            throw new ArgumentException("IMU 输出路径不能为空。", nameof(filePath));
        }

        // 规范化绝对路径，避免工作目录变化导致文件位置漂移。
        string absolutePath = Path.GetFullPath(filePath);
        // 解析目标目录；合法绝对路径应始终包含目录部分。
        string directory = Path.GetDirectoryName(absolutePath) ?? throw new InvalidOperationException("无法解析 IMU 输出目录。");
        // 确保用户选择的目标目录存在。
        Directory.CreateDirectory(directory);
        // 按会话分组分类窗口；每组保持窗口序号顺序，导出时执行 O(N+M) 前向关联。
        Dictionary<uint, InferenceExportSample[]> inferenceBySession = inferenceSamples
            // 同一会话的窗口不能被其它会话的旧模型结论污染。
            .GroupBy(sample => sample.SessionSequence)
            // 窗口序号是设备端成功触发顺序；十分钟内不会跨越半范围歧义。
            .ToDictionary(
                group => group.Key,
                group => group.OrderBy(sample => sample.Diagnostic.WindowSequence).ToArray());
        // 保存每个会话已经推进到的最近分类窗口索引，初始缺省为负一。
        Dictionary<uint, int> inferenceCursorBySession = new();
        // 计数标记按会话和原始 MetricEvent 设备时刻建立唯一索引；同点重复通知保留最大事件序号。
        Dictionary<(uint SessionSequence, uint DeviceMilliseconds), MetricEventExportSample> metricByPoint = metricEvents
            // 只导出真正增加次数或步数的低延迟指标事件。
            .Where(sample =>
                sample.Event.EventType == DeviceEventType.RepetitionCounted &&
                (sample.Event.MetricKind == MetricKind.Repetition || sample.Event.MetricKind == MetricKind.Step) &&
                sample.Event.MetricDelta > 0U)
            // 同一会话同一重采样点理论上只有一个事件；分组用于抵抗 BLE 重复通知。
            .GroupBy(sample => (sample.Event.SessionSequence, sample.DeviceMonotonicMilliseconds))
            // 事件序号最大的记录代表设备最新幂等事实。
            .ToDictionary(group => group.Key, group => group.OrderBy(sample => sample.Event.EventSequence).Last());
        // StringBuilder 以线性空间构造最多 15000 行诊断数据，新增模型和计数列后预留更宽容量。
        StringBuilder csv = new(samples.Count * 380 + 1024);
        // 固定中文列名并在列名中写明单位；Python 可通过 UTF-8 BOM 直接读取同一文件。
        csv.AppendLine("样本序号,设备单调时间（毫秒）,上位机接收时间（协调世界时）,角速度横轴诊断码,角速度纵轴诊断码,角速度垂直轴诊断码,加速度横轴诊断码,加速度纵轴诊断码,加速度垂直轴诊断码,角速度横轴（度每秒）,角速度纵轴（度每秒）,角速度垂直轴（度每秒）,加速度横轴（重力倍数）,加速度纵轴（重力倍数）,加速度垂直轴（重力倍数）,质量标志（十六进制）,会话序号,佩戴手侧,设备状态,设备稳定动作,设备稳定动作置信度,权威指标类型,权威累计值,分类窗口序号,分类窗口结束时间（毫秒）,基础模型类别,基础模型置信度,掩码模型类别,掩码模型置信度,融合模型类别,融合模型置信度,模型是否一致,推理耗时（毫秒）,分类质量标志（十六进制）,累计推理失败数,分类窗口是否在本行结束,是否计数标记点,计数事件序号,计数动作,计数指标类型,本次计数增量,计数后累计值,计数事件设备时间（毫秒）,计数质量标志（十六进制）");

        // 按设备接收顺序逐行写出，离线工具可直接重建 25 Hz 时间序列。
        foreach (ImuExportSample sample in samples)
        {
            // 每行前检查取消请求，避免大文件导出阻塞应用退出。
            cancellationToken.ThrowIfCancellationRequested();
            // latestInference 保存不晚于当前 IMU 点的最近完成窗口；没有窗口时保持 null。
            InferenceExportSample? latestInference = null;
            // 当前会话有分类历史时，从上次游标继续推进，禁止跨会话复用旧类别。
            if (inferenceBySession.TryGetValue(sample.SessionSequence, out InferenceExportSample[]? sessionInferences))
            {
                // 读取当前会话游标；负一表示尚无窗口结束时间不晚于当前点。
                int cursor = inferenceCursorBySession.TryGetValue(sample.SessionSequence, out int savedCursor)
                    ? savedCursor
                    : -1;
                // 依次消费所有已经在当前 IMU 时刻完成的窗口，最终保留最近一个。
                while ((cursor + 1) < sessionInferences.Length &&
                       IsAtOrBefore(
                           sessionInferences[cursor + 1].Diagnostic.WindowEndMilliseconds,
                           sample.DeviceMonotonicMilliseconds))
                {
                    // 推进到下一个已完成窗口。
                    cursor += 1;
                }
                // 保存游标，下一行从当前位置继续，整体时间复杂度保持 O(N+M)。
                inferenceCursorBySession[sample.SessionSequence] = cursor;
                // 非负游标表示至少有一个已完成窗口可以关联。
                if (cursor >= 0)
                {
                    // 保存最近完成窗口；它可能早于当前点，窗口结束点列会明确区分。
                    latestInference = sessionInferences[cursor];
                }
            }
            // 按会话和精确设备时间查找权威计数事件；没有事件时保持 null。
            metricByPoint.TryGetValue(
                (sample.SessionSequence, sample.DeviceMonotonicMilliseconds),
                out MetricEventExportSample? metricMarker);
            // 当前行是否恰好是分类窗口末点；前向填充行必须标为否。
            bool isInferenceWindowEnd = latestInference is not null &&
                latestInference.Diagnostic.WindowEndMilliseconds == sample.DeviceMonotonicMilliseconds;
            // 三路一致只比较同一窗口 Top-1，不能解释为会话稳定动作已经锁定。
            bool modelsAgree = latestInference is not null &&
                latestInference.Diagnostic.FusedAction == latestInference.Diagnostic.BaseAction &&
                latestInference.Diagnostic.FusedAction == latestInference.Diagnostic.MaskedAction;
            // 固定四十四列；所有数值使用 invariant 小数点，避免中文区域设置写成逗号小数。
            string[] fields =
            [
                // 样本序号保持 uint32 十进制原值。
                sample.SampleIndex.ToString(System.Globalization.CultureInfo.InvariantCulture),
                // 设备单调时间保持毫秒整数，周期算法优先使用此列。
                sample.DeviceMonotonicMilliseconds.ToString(System.Globalization.CultureInfo.InvariantCulture),
                // UTC 接收时间使用可逆的 ISO 8601 往返格式。
                sample.ReceivedAtUtc.ToUniversalTime().ToString("O", System.Globalization.CultureInfo.InvariantCulture),
                // 六个诊断码保留 int16 原值，便于验证协议和定点缩放。
                sample.GxCode.ToString(System.Globalization.CultureInfo.InvariantCulture),
                sample.GyCode.ToString(System.Globalization.CultureInfo.InvariantCulture),
                sample.GzCode.ToString(System.Globalization.CultureInfo.InvariantCulture),
                sample.AxCode.ToString(System.Globalization.CultureInfo.InvariantCulture),
                sample.AyCode.ToString(System.Globalization.CultureInfo.InvariantCulture),
                sample.AzCode.ToString(System.Globalization.CultureInfo.InvariantCulture),
                // 角速度保留六位小数，量化误差远小于 QMI8658 诊断比例的一码。
                sample.GxDegreesPerSecond.ToString("F6", System.Globalization.CultureInfo.InvariantCulture),
                sample.GyDegreesPerSecond.ToString("F6", System.Globalization.CultureInfo.InvariantCulture),
                sample.GzDegreesPerSecond.ToString("F6", System.Globalization.CultureInfo.InvariantCulture),
                // 加速度保留九位小数，精确覆盖 1/4096 g 定点分辨率。
                sample.AxG.ToString("F9", System.Globalization.CultureInfo.InvariantCulture),
                sample.AyG.ToString("F9", System.Globalization.CultureInfo.InvariantCulture),
                sample.AzG.ToString("F9", System.Globalization.CultureInfo.InvariantCulture),
                // 质量位使用固定四位十六进制，直接对应固件位图。
                $"0x{sample.QualityFlags:X4}",
                // 会话序号用于区分同一十分钟缓存内多次训练。
                sample.SessionSequence.ToString(System.Globalization.CultureInfo.InvariantCulture),
                // 佩戴手侧固定为右手腕，与当前模型训练域和真板验收合同一致。
                SupportedWristSide,
                // 设备状态使用统一中文映射。
                sample.DeviceState.HasValue ? DisplayText.DeviceStateName(sample.DeviceState.Value) : string.Empty,
                // 稳定动作来自当前样本到达时最近 LiveState，不由 PC 模型重算。
                DisplayText.ActionName(sample.StableAction),
                // 稳定动作置信度保留六位小数，范围 0～1。
                sample.StableConfidence.ToString("F6", System.Globalization.CultureInfo.InvariantCulture),
                // 权威指标类型使用中文名称。
                DisplayText.MetricKindName(sample.MetricKind),
                // 权威累计值直接来自设备 LiveState。
                sample.MetricTotal.ToString(System.Globalization.CultureInfo.InvariantCulture),
                // 没有已完成分类窗口时保持空白，不用零伪装真实窗口。
                latestInference?.Diagnostic.WindowSequence.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? string.Empty,
                // 窗口末点使用设备单调毫秒，便于与当前行准确对齐。
                latestInference?.Diagnostic.WindowEndMilliseconds.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? string.Empty,
                // 基础 M0 Top-1 使用统一中文动作名称。
                latestInference is null ? string.Empty : DisplayText.ActionName(latestInference.Diagnostic.BaseAction),
                // 基础 M0 概率保留六位小数，范围 0～1。
                latestInference?.Diagnostic.BaseConfidence.ToString("F6", System.Globalization.CultureInfo.InvariantCulture) ?? string.Empty,
                // 掩码 M0 Top-1 使用统一中文动作名称。
                latestInference is null ? string.Empty : DisplayText.ActionName(latestInference.Diagnostic.MaskedAction),
                // 掩码 M0 概率保留六位小数，范围 0～1。
                latestInference?.Diagnostic.MaskedConfidence.ToString("F6", System.Globalization.CultureInfo.InvariantCulture) ?? string.Empty,
                // 融合 Top-1 是当前分类窗口主输出。
                latestInference is null ? string.Empty : DisplayText.ActionName(latestInference.Diagnostic.FusedAction),
                // 融合概率保留六位小数，范围 0～1。
                latestInference?.Diagnostic.FusedConfidence.ToString("F6", System.Globalization.CultureInfo.InvariantCulture) ?? string.Empty,
                // 没有分类窗口时留空；有窗口时明确一致或分歧。
                latestInference is null ? string.Empty : (modelsAgree ? "一致" : "分歧"),
                // 微秒换算为毫秒并保留三位，便于验证推理是否占用多个采样周期。
                latestInference is null
                    ? string.Empty
                    : (latestInference.Diagnostic.InferenceMicroseconds / 1000.0).ToString("F3", System.Globalization.CultureInfo.InvariantCulture),
                // 分类质量位与六轴质量位分列，避免把窗口聚合错误归给单点。
                latestInference is null ? string.Empty : $"0x{latestInference.Diagnostic.QualityFlags:X4}",
                // 设备累计推理失败数保持原值。
                latestInference?.Diagnostic.FailureCount.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? string.Empty,
                // 精确窗口末点标是，其余前向关联行标否。
                isInferenceWindowEnd ? "是" : "否",
                // 只有精确匹配到权威次数或步数事件时标是。
                metricMarker is null ? "否" : "是",
                // 计数事件序号用于检测重复或缺口。
                metricMarker?.Event.EventSequence.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? string.Empty,
                // 计数动作来自 MetricEvent，不使用当前界面动作猜测。
                metricMarker is null ? string.Empty : DisplayText.ActionName(metricMarker.Event.Action),
                // 次数与步数使用统一中文指标名称。
                metricMarker is null ? string.Empty : DisplayText.MetricKindName(metricMarker.Event.MetricKind),
                // 本次增量通常为一，仍保留协议原值。
                metricMarker?.Event.MetricDelta.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? string.Empty,
                // 事件发生后累计值来自设备 MetricEvent。
                metricMarker?.Event.MetricTotal.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? string.Empty,
                // 标记点设备时刻必须与当前 IMU 行相同。
                metricMarker?.DeviceMonotonicMilliseconds.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? string.Empty,
                // 计数事件质量位保留原始十六进制事实。
                metricMarker is null ? string.Empty : $"0x{metricMarker.Event.QualityFlags:X4}",
            ];
            // 当前字段不含逗号；直接连接可减少十分钟导出的临时对象和转义开销。
            csv.AppendLine(string.Join(',', fields));
        }

        // 使用带 BOM UTF-8 异步写盘，中文 Windows Excel 可直接识别且不阻塞 UI 线程。
        await File.WriteAllTextAsync(
                absolutePath,
                csv.ToString(),
                new UTF8Encoding(encoderShouldEmitUTF8Identifier: true),
                cancellationToken)
            .ConfigureAwait(false);
    }

    // 比较同一不超过十分钟缓存内的 uint32 设备时间，兼容自然回绕且拒绝未来窗口。
    private static bool IsAtOrBefore(uint earlierMilliseconds, uint laterMilliseconds)
    {
        // 无符号差转有符号；只要时间跨度小于 2^31 ms，非负表示 earlier 不晚于 later。
        return unchecked((int)(laterMilliseconds - earlierMilliseconds)) >= 0;
    }
}
