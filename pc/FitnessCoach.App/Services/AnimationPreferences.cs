// 动画偏好完全位于应用层，不改变设备协议、领域模型或 ESP32 设置合同。
namespace FitnessCoach.App.Services;

/// <summary>暴露 Windows 系统动画约束和本次运行的用户“减少动画”开关。</summary>
public interface IAnimationPreferences
{
    /// <summary>true 表示 Windows 系统已关闭客户端动画，应用不得强制恢复循环。</summary>
    bool IsSystemReducedMotion { get; }

    /// <summary>true 表示用户在本次上位机运行中主动要求减少动画。</summary>
    bool UserRequestedReducedMotion { get; set; }

    /// <summary>系统或用户任一要求减少动画时为 true。</summary>
    bool IsReducedMotionEnabled { get; }

    /// <summary>有效设置变化时通知实时页面；事件可从 UI 线程触发。</summary>
    event EventHandler? Changed;
}

/// <summary>合并 Windows 辅助功能设置和应用内临时开关。</summary>
public sealed class AnimationPreferences : IAnimationPreferences
{
    // 保存启动时读取的 Windows 客户区动画禁用状态；运行中不反向覆盖系统选择。
    private readonly bool _systemReducedMotion;
    // 保存本次应用运行期间的用户选择。
    private bool _userRequestedReducedMotion;

    /// <summary>创建动画偏好；systemReducedMotion 来自 WPF SystemParameters.ClientAreaAnimation 的反值。</summary>
    public AnimationPreferences(bool systemReducedMotion)
    {
        // 保存系统辅助功能边界，true 时用户不能强制开启动画。
        _systemReducedMotion = systemReducedMotion;
    }

    /// <inheritdoc />
    public bool IsSystemReducedMotion => _systemReducedMotion;

    /// <inheritdoc />
    public bool UserRequestedReducedMotion
    {
        // 返回当前应用内开关。
        get => _userRequestedReducedMotion;
        // 仅在值改变时通知订阅者，避免重复刷新动画计时器。
        set
        {
            // 相同值不产生事件。
            if (_userRequestedReducedMotion == value)
            {
                // 保持当前有效设置。
                return;
            }

            // 保存用户的新选择。
            _userRequestedReducedMotion = value;
            // 通知实时页面重新计算有效减少动画状态。
            Changed?.Invoke(this, EventArgs.Empty);
        }
    }

    /// <inheritdoc />
    public bool IsReducedMotionEnabled => _systemReducedMotion || _userRequestedReducedMotion;

    /// <inheritdoc />
    public event EventHandler? Changed;
}
