// 引入领域动作枚举。
using FitnessCoach.Domain;

// 本地资源映射实现位于应用服务层。
namespace FitnessCoach.App.Services;

/// <summary>把权威动作映射为稳定资源键；WPF 矢量控件按同一动作 ID 绘制本地关键姿态。</summary>
public sealed class LocalActionAnimationController : IActionAnimationController
{
    // 固定 11 类视觉表；Glyph 仅供诊断回退，正式实时页使用 ActionPoseLibrary 矢量骨架。
    private static readonly IReadOnlyDictionary<ActionId, ActionVisualDescriptor> Visuals =
        new Dictionary<ActionId, ActionVisualDescriptor>
        {
            // 早安式使用弯腰方向符号。
            [ActionId.GoodMorning] = new(ActionId.GoodMorning, "早安式", "↘", "action.good_morning"),
            // 开合跳使用张开的人形符号。
            [ActionId.JumpingJack] = new(ActionId.JumpingJack, "开合跳", "✦", "action.jumping_jack"),
            // 跳跃弓步使用交替箭头。
            [ActionId.JumpingLunge] = new(ActionId.JumpingLunge, "跳跃弓步", "⇄", "action.jumping_lunge"),
            // 跳跃深蹲使用向上箭头。
            [ActionId.JumpingSquat] = new(ActionId.JumpingSquat, "跳跃深蹲", "↑", "action.jumping_squat"),
            // 普通弓步使用前向步伐符号。
            [ActionId.Lunge] = new(ActionId.Lunge, "弓步", "⇥", "action.lunge"),
            // 静坐使用座椅符号。
            [ActionId.Sit] = new(ActionId.Sit, "静坐", "▰", "action.sit"),
            // 深蹲使用上下运动符号。
            [ActionId.Squat] = new(ActionId.Squat, "深蹲", "↕", "action.squat"),
            // 小跑使用快速前进符号。
            [ActionId.Trot] = new(ActionId.Trot, "小跑", "»", "action.trot"),
            // 收腹跳使用收拢符号。
            [ActionId.TuckJump] = new(ActionId.TuckJump, "收腹跳", "⇧", "action.tuck_jump"),
            // 行走使用脚步符号。
            [ActionId.Walk] = new(ActionId.Walk, "行走", "›", "action.walk"),
            // 挥手使用波纹符号。
            [ActionId.Wave] = new(ActionId.Wave, "挥手", "≈", "action.wave"),
            // 未识别状态使用问号。
            [ActionId.Unknown] = new(ActionId.Unknown, "等待识别", "?", "action.unknown"),
        };

    // 当前视觉初始为等待识别。
    private ActionVisualDescriptor _current = Visuals[ActionId.Unknown];
    // 修订号用于触发计数和动作切换脉冲。
    private uint _revision;

    /// <inheritdoc />
    public ActionVisualDescriptor Current => _current;

    /// <inheritdoc />
    public uint Revision => _revision;

    /// <inheritdoc />
    public event EventHandler? VisualChanged;

    /// <inheritdoc />
    public void SetAction(ActionId action)
    {
        // 未知枚举值统一回退到等待视觉，防止资源键查找失败。
        _current = Visuals.TryGetValue(action, out ActionVisualDescriptor? visual)
            ? visual
            : Visuals[ActionId.Unknown];
        // 每次稳定状态到达都递增修订号，供 UI 产生轻微缩放脉冲。
        _revision = unchecked(_revision + 1U);
        // 通知 ViewModel 读取新视觉和修订号。
        VisualChanged?.Invoke(this, EventArgs.Empty);
    }
}
