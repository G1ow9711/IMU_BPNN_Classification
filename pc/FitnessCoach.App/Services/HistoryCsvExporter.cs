// 引入文件、目录和路径 API，WPF 临时编译项目不保证隐式导入 System.IO。
using System.IO;
// 引入 UTF-8 编码，CSV 使用带 BOM 文件便于 Excel 正确识别中文。
using System.Text;
// 引入 WPF 保存文件对话框。
using Microsoft.Win32;
// 引入领域会话和动作指标。
using FitnessCoach.Domain;
// 引入统一中文显示映射，导出内容不得泄露内部枚举英文名。
using FitnessCoach.App.ViewModels;

// 历史导出服务位于应用服务层，ViewModel 不直接创建窗口或操作文件编码。
namespace FitnessCoach.App.Services;

/// <summary>定义历史 CSV 写入边界，测试可使用临时文件验证字节和转义。</summary>
public interface IHistoryCsvExporter
{
    /// <summary>把筛选后的会话写入指定 CSV；每个动作指标占一行。</summary>
    Task ExportAsync(IReadOnlyList<TrainingSessionSummary> sessions, string filePath, CancellationToken cancellationToken = default);
}

/// <summary>定义保存位置选择边界，测试用替身无需打开系统对话框。</summary>
public interface IHistoryExportDestinationPicker
{
    /// <summary>显示保存对话框；用户取消时返回 null。</summary>
    Task<string?> PickCsvPathAsync(string suggestedFileName);
}

/// <summary>使用 Windows SaveFileDialog 选择 CSV 输出路径。</summary>
public sealed class WpfHistoryExportDestinationPicker : IHistoryExportDestinationPicker
{
    /// <inheritdoc />
    public Task<string?> PickCsvPathAsync(string suggestedFileName)
    {
        // 创建只允许 CSV 的系统保存对话框。
        SaveFileDialog dialog = new()
        {
            // 使用调用者生成的日期文件名，用户仍可手工修改。
            FileName = suggestedFileName,
            // 默认扩展名固定 csv，避免保存成无扩展名文件。
            DefaultExt = ".csv",
            // 过滤器先显示逗号分隔值文件，再允许用户查看全部文件。
            Filter = "逗号分隔值文件|*.csv|所有文件|*.*",
            // 覆盖已有文件时由系统弹出确认。
            OverwritePrompt = true,
            // 导航不存在路径时给出系统提示。
            CheckPathExists = true,
        };
        // WPF 对话框返回 true 表示用户确认，false/null 表示取消。
        string? selectedPath = dialog.ShowDialog() == true ? dialog.FileName : null;
        // 接口保持异步形态，未来可替换 WinUI/自定义选择器。
        return Task.FromResult(selectedPath);
    }
}

/// <summary>
/// 把本地会话导出为 RFC4180 风格 CSV；字段内逗号、双引号和换行使用双引号转义。
/// </summary>
public sealed class HistoryCsvExporter : IHistoryCsvExporter
{
    /// <inheritdoc />
    public async Task ExportAsync(
        IReadOnlyList<TrainingSessionSummary> sessions,
        string filePath,
        CancellationToken cancellationToken = default)
    {
        // 会话列表不能为空引用；空集合仍生成只有表头的合法文件。
        ArgumentNullException.ThrowIfNull(sessions);
        // 输出路径不能为空，否则无法确定用户授权位置。
        if (string.IsNullOrWhiteSpace(filePath))
        {
            // 拒绝空路径，避免误写当前目录。
            throw new ArgumentException("表格输出路径不能为空。", nameof(filePath));
        }

        // 规范化绝对路径，避免工作目录变化造成导出位置漂移。
        string absolutePath = Path.GetFullPath(filePath);
        // 获取目标目录；绝对文件路径正常情况下始终包含目录。
        string directory = Path.GetDirectoryName(absolutePath) ?? throw new InvalidOperationException("无法解析表格输出目录。");
        // 保存对话框允许选择尚未存在的子目录时，确保目录存在。
        Directory.CreateDirectory(directory);
        // StringBuilder 减少多行拼接的临时字符串数量。
        StringBuilder csv = new();
        // 写入固定中文表头；Excel 读取 UTF-8 BOM 后可直接显示。
        csv.AppendLine("设备标识,会话序号,开始时间,结束时间,时长（秒）,总卡路里（千卡）,结束原因,动作,指标类型,指标值,活动时长（秒）,动作卡路里（千卡）");

        // 遍历当前筛选结果，保持 ViewModel 展示的开始时间倒序。
        foreach (TrainingSessionSummary session in sessions)
        {
            // 无动作指标的空会话仍导出一行，避免历史记录在 CSV 中消失。
            IReadOnlyList<ActionMetric?> metrics = session.ActionMetrics.Count == 0
                ? new ActionMetric?[] { null }
                : session.ActionMetrics.Cast<ActionMetric?>().ToArray();

            // 每个动作指标单独一行，便于 Excel 透视表按动作统计。
            foreach (ActionMetric? metric in metrics)
            {
                // 响应调用者取消，避免大批量导出阻塞应用退出。
                cancellationToken.ThrowIfCancellationRequested();
                // 构造固定 12 列，数值使用不受区域小数逗号影响的 invariant 格式。
                string[] fields =
                [
                    session.DeviceId,
                    session.SessionSequence.ToString(System.Globalization.CultureInfo.InvariantCulture),
                    session.StartedAtUtc.ToLocalTime().ToString("yyyy-MM-dd HH:mm:ss zzz", System.Globalization.CultureInfo.InvariantCulture),
                    session.EndedAtUtc.ToLocalTime().ToString("yyyy-MM-dd HH:mm:ss zzz", System.Globalization.CultureInfo.InvariantCulture),
                    (session.ElapsedMilliseconds / 1000.0).ToString("F3", System.Globalization.CultureInfo.InvariantCulture),
                    session.CaloriesKcal.ToString("F3", System.Globalization.CultureInfo.InvariantCulture),
                    session.EndReason,
                    metric is null ? string.Empty : DisplayText.ActionName(metric.Action),
                    metric is null ? string.Empty : DisplayText.MetricKindName(metric.MetricKind),
                    metric?.MetricValue.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? string.Empty,
                    metric is null ? string.Empty : (metric.ActiveMilliseconds / 1000.0).ToString("F3", System.Globalization.CultureInfo.InvariantCulture),
                    metric is null ? string.Empty : (metric.CaloriesMilliKcal / 1000.0).ToString("F3", System.Globalization.CultureInfo.InvariantCulture),
                ];
                // 逐列执行 RFC4180 转义，再用英文逗号连接。
                csv.AppendLine(string.Join(',', fields.Select(EscapeField)));
            }
        }

        // 使用带 BOM UTF-8 写入，兼容中文 Windows Excel 的直接双击打开行为。
        await File.WriteAllTextAsync(absolutePath, csv.ToString(), new UTF8Encoding(encoderShouldEmitUTF8Identifier: true), cancellationToken)
            .ConfigureAwait(false);
    }

    // 对 CSV 字段执行双引号转义。
    private static string EscapeField(string value)
    {
        // null 不会从当前调用路径出现，仍防御未来可空字段。
        string safeValue = value ?? string.Empty;
        // 只要包含逗号、双引号或换行，就必须整体加双引号。
        bool requiresQuotes = safeValue.IndexOfAny([',', '"', '\r', '\n']) >= 0;
        // 不含特殊字符时直接返回，减少文件体积。
        if (!requiresQuotes)
        {
            // 返回原始字段文本。
            return safeValue;
        }

        // RFC4180 使用两个连续双引号表示字段内一个双引号。
        string escaped = safeValue.Replace("\"", "\"\"", StringComparison.Ordinal);
        // 在转义后字段两侧添加双引号。
        return $"\"{escaped}\"";
    }
}
