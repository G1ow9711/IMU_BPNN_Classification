// 引入领域偏好对象，文件格式不泄漏到 WPF 设置页。
using FitnessCoach.Domain;

// 设置仓储接口与会话仓储位于同一基础设施层。
namespace FitnessCoach.Infrastructure;

/// <summary>
/// 定义用户偏好的异步读取和原子保存合同。
/// </summary>
public interface IUserPreferencesStore
{
    /// <summary>读取偏好；文件不存在时返回安全默认值。</summary>
    Task<UserPreferences> LoadAsync(CancellationToken cancellationToken = default);

    /// <summary>校验并原子保存偏好。</summary>
    Task SaveAsync(UserPreferences preferences, CancellationToken cancellationToken = default);
}
