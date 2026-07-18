// 引入领域动作类别。
using FitnessCoach.Domain;

// 动画控制端口把权威动作与修订号送给本地 WPF 矢量姿态控件。
namespace FitnessCoach.App.Services;

/// <summary>控制完全本地的动作视觉，不依赖网络、WebView 或外部动画文件。</summary>
public interface IActionAnimationController
{
    /// <summary>当前动作视觉描述。</summary>
    ActionVisualDescriptor Current { get; }

    /// <summary>视觉切换时递增，用于界面重新触发局部动画。</summary>
    uint Revision { get; }

    /// <summary>动作视觉或修订号变化时通知 ViewModel。</summary>
    event EventHandler? VisualChanged;

    /// <summary>切换到指定动作；相同动作仍递增修订号，形成一次计数脉冲。</summary>
    void SetAction(ActionId action);
}
