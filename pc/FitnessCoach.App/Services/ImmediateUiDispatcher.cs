// 测试调度器位于同一服务命名空间，避免测试项目引用 WPF Dispatcher 实例。
namespace FitnessCoach.App.Services;

/// <summary>在调用线程执行更新并用互斥锁串行化，供无窗口单元测试使用。</summary>
public sealed class ImmediateUiDispatcher : IUiDispatcher
{
    // 同一实例所有更新共享锁，模拟 UI 单线程顺序。
    private readonly object _sync = new();

    /// <inheritdoc />
    public Task InvokeAsync(Action action)
    {
        // 更新委托不能为空。
        ArgumentNullException.ThrowIfNull(action);
        // 串行进入测试 UI 临界区。
        lock (_sync)
        {
            // 执行属性更新。
            action();
        }

        // 同步执行完成，返回已完成任务。
        return Task.CompletedTask;
    }
}
