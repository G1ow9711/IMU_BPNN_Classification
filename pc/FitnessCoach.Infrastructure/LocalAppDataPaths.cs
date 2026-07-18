// 应用路径集中生成，避免页面或仓储各自拼接不同目录。
namespace FitnessCoach.Infrastructure;

/// <summary>
/// 保存 Windows 离线数据文件位置。
/// </summary>
public sealed class LocalAppDataPaths
{
    /// <summary>
    /// 创建并规范化应用数据路径。
    /// </summary>
    public LocalAppDataPaths(string rootDirectory)
    {
        // 根目录不能为空，否则文件可能落到不可预测的当前目录。
        if (string.IsNullOrWhiteSpace(rootDirectory))
        {
            // 拒绝空路径，调用者必须显式提供目录。
            throw new ArgumentException("应用数据根目录不能为空。", nameof(rootDirectory));
        }

        // 转换为绝对路径，防止工作目录变化影响仓储位置。
        RootDirectory = Path.GetFullPath(rootDirectory);
        // 会话 JSON 文件保存在根目录，未来可替换为 fitness.db。
        SessionsFile = Path.Combine(RootDirectory, "sessions.v1.json");
        // 用户偏好单独保存，避免会话写入失败影响设置。
        PreferencesFile = Path.Combine(RootDirectory, "preferences.v1.json");
        // 原始日志目录只保存文件，不写入 JSON 大对象。
        RawLogsDirectory = Path.Combine(RootDirectory, "raw");
        // 诊断日志目录供未来滚动日志实现使用。
        LogsDirectory = Path.Combine(RootDirectory, "logs");
    }

    /// <summary>应用数据绝对根目录。</summary>
    public string RootDirectory { get; }

    /// <summary>当前 JSON 会话仓储文件。</summary>
    public string SessionsFile { get; }

    /// <summary>用户偏好 JSON 文件。</summary>
    public string PreferencesFile { get; }

    /// <summary>可选原始 IMU 日志目录。</summary>
    public string RawLogsDirectory { get; }

    /// <summary>本地诊断日志目录。</summary>
    public string LogsDirectory { get; }

    /// <summary>
    /// 使用 LocalApplicationData 生成正式应用默认目录。
    /// </summary>
    public static LocalAppDataPaths CreateDefault()
    {
        // 读取当前 Windows 用户的本地应用数据目录。
        string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        // 在用户隔离目录下创建 IMUFitness 子目录合同。
        return new LocalAppDataPaths(Path.Combine(localAppData, "IMUFitness"));
    }

    /// <summary>
    /// 创建所有必需目录；重复调用不报错。
    /// </summary>
    public void EnsureDirectories()
    {
        // 创建应用根目录。
        Directory.CreateDirectory(RootDirectory);
        // 创建原始日志目录。
        Directory.CreateDirectory(RawLogsDirectory);
        // 创建诊断日志目录。
        Directory.CreateDirectory(LogsDirectory);
    }
}
