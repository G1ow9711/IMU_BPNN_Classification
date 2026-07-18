// UI 调度接口不引用具体 Dispatcher，便于无窗口测试线程切换。
namespace FitnessCoach.App.Services;

/// <summary>串行执行属性更新；生产实现切到 WPF UI 线程，测试实现原地串行。</summary>
public interface IUiDispatcher
{
    /// <summary>调度一次同步 UI 更新，并在更新完成后结束任务。</summary>
    Task InvokeAsync(Action action);
}
