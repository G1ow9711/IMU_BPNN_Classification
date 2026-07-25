// 引入 UTF-8 和 JSON 支持，实现无外部包的离线设置仓储。
using System.Text;
// 引入 System.Text.Json 序列化器。
using System.Text.Json;
// 引入领域偏好对象。
using FitnessCoach.Domain;

// 设置文件实现位于基础设施层。
namespace FitnessCoach.Infrastructure;

/// <summary>
/// 使用原子 JSON 文件保存用户偏好。
/// </summary>
public sealed class JsonUserPreferencesStore : IUserPreferencesStore, IDisposable
{
    // 保存设置文件绝对路径。
    private readonly string _filePath;
    // 串行化读取与写入，防止快速连续点击保存造成文件竞争。
    private readonly SemaphoreSlim _gate = new(1, 1);
    // 固定 JSON 写入格式。
    private readonly JsonSerializerOptions _jsonOptions = new()
    {
        // 便于用户和开发者审计设置文件。
        WriteIndented = true,
        // 使用驼峰字段名。
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        // 允许读取旧文件的属性大小写差异。
        PropertyNameCaseInsensitive = true,
    };
    // 标记对象是否已经释放。
    private bool _disposed;

    /// <summary>
    /// 创建设置仓储。
    /// </summary>
    public JsonUserPreferencesStore(string filePath)
    {
        // 设置路径不能为空。
        if (string.IsNullOrWhiteSpace(filePath))
        {
            // 拒绝不确定存储位置。
            throw new ArgumentException("偏好文件路径不能为空。", nameof(filePath));
        }

        // 保存绝对路径。
        _filePath = Path.GetFullPath(filePath);
    }

    /// <inheritdoc />
    public async Task<UserPreferences> LoadAsync(CancellationToken cancellationToken = default)
    {
        // 检查对象还可使用。
        ThrowIfDisposed();
        // 等待当前设置事务完成。
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            // 文件不存在时返回领域安全默认值。
            if (!File.Exists(_filePath))
            {
                // 不在读取阶段自动写文件。
                return new UserPreferences();
            }

            // 读取 UTF-8 设置文件。
            string json = await File.ReadAllTextAsync(_filePath, Encoding.UTF8, cancellationToken).ConfigureAwait(false);
            // 空文件回退安全默认值，设置丢失不影响历史会话。
            if (string.IsNullOrWhiteSpace(json))
            {
                // 返回默认偏好。
                return new UserPreferences();
            }

            // 反序列化偏好，失败异常交给上层诊断页显示。
            UserPreferences? preferences = JsonSerializer.Deserialize<UserPreferences>(json, _jsonOptions);
            // null 根对象回退默认值。
            return preferences ?? new UserPreferences();
        }
        finally
        {
            // 释放设置锁。
            _gate.Release();
        }
    }

    /// <inheritdoc />
    public async Task SaveAsync(UserPreferences preferences, CancellationToken cancellationToken = default)
    {
        // 偏好对象不能为空。
        ArgumentNullException.ThrowIfNull(preferences);
        // 校验用户可编辑范围。
        Validate(preferences);
        // 检查对象还可使用。
        ThrowIfDisposed();
        // 等待当前设置事务完成。
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            // 解析并创建设置文件目录。
            string directory = Path.GetDirectoryName(_filePath) ?? throw new InvalidOperationException("无法解析偏好文件目录。");
            // 重复创建目录安全。
            Directory.CreateDirectory(directory);
            // 序列化偏好对象。
            string json = JsonSerializer.Serialize(preferences, _jsonOptions);
            // 临时文件与正式文件位于同一目录，保证原子替换。
            string temporaryPath = _filePath + ".tmp";
            // 使用无 BOM UTF-8 写入完整临时文件。
            await File.WriteAllTextAsync(temporaryPath, json, new UTF8Encoding(false), cancellationToken).ConfigureAwait(false);
            // 原子替换正式设置文件。
            File.Move(temporaryPath, _filePath, true);
        }
        finally
        {
            // 释放设置锁。
            _gate.Release();
        }
    }

    /// <summary>
    /// 释放设置仓储锁。
    /// </summary>
    public void Dispose()
    {
        // 重复释放安全返回。
        if (_disposed)
        {
            // 对象已经释放，无需再次处理。
            return;
        }

        // 标记对象已释放。
        _disposed = true;
        // 释放信号量资源。
        _gate.Dispose();
    }

    // 验证设备和卡路里估算可接受的用户输入范围。
    private static void Validate(UserPreferences preferences)
    {
        // 单位枚举只能是公制或英制，拒绝手工篡改 JSON 产生的未知数值。
        if (!Enum.IsDefined(preferences.UnitSystem))
        {
            // 报告单位配置错误，避免界面使用不确定换算规则。
            throw new ArgumentOutOfRangeException(nameof(preferences.UnitSystem), "计量单位只能选择公制或英制。");
        }

        // 体重限制在固件命令 7 同一 30～250 kg 范围，防止本地能保存但设备必然拒绝。
        if ((preferences.WeightKilograms < 30.0) || (preferences.WeightKilograms > 250.0))
        {
            // 抛出明确范围异常供设置页显示。
            throw new ArgumentOutOfRangeException(nameof(preferences.WeightKilograms), "体重必须位于 30～250 kg。");
        }

        // AMOLED 亮度至少 5%，最高 100%。
        if ((preferences.BrightnessPercent < 5) || (preferences.BrightnessPercent > 100))
        {
            // 拒绝不可显示或超范围亮度。
            throw new ArgumentOutOfRangeException(nameof(preferences.BrightnessPercent), "亮度必须位于 5～100%。");
        }

        // 熄屏时间限制在 10～300 秒。
        if ((preferences.ScreenTimeoutSeconds < 10) || (preferences.ScreenTimeoutSeconds > 300))
        {
            // 拒绝过短闪屏和过长高功耗设置。
            throw new ArgumentOutOfRangeException(nameof(preferences.ScreenTimeoutSeconds), "熄屏时间必须位于 10～300 秒。");
        }

        // 每日目标不允许为负或超过 5000 kcal。
        if ((preferences.DailyCalorieGoal < 0.0) || (preferences.DailyCalorieGoal > 5000.0))
        {
            // 拒绝无意义或明显异常目标。
            throw new ArgumentOutOfRangeException(nameof(preferences.DailyCalorieGoal), "每日卡路里目标必须位于 0～5000 kcal。");
        }
    }

    // 检查设置仓储是否已经释放。
    private void ThrowIfDisposed()
    {
        // dispose 后任何访问都属于调用错误。
        ObjectDisposedException.ThrowIf(_disposed, this);
    }
}
