// 引入 WPF Application 以访问主界面 Dispatcher。
using System.Windows;

// 生产 UI 调度器位于服务命名空间。
namespace FitnessCoach.App.Services;

/// <summary>把后台 BLE/Mock 事件切回 WPF 主线程，禁止跨线程直接修改绑定属性。</summary>
public sealed class WpfUiDispatcher : IUiDispatcher
{
    /// <inheritdoc />
    public Task InvokeAsync(Action action)
    {
        // 更新委托不能为空。
        ArgumentNullException.ThrowIfNull(action);
        // 当前线程已经是 UI 线程时直接执行，减少一次消息队列延迟。
        if (Application.Current.Dispatcher.CheckAccess())
        {
            // 在 UI 线程同步更新绑定属性。
            action();
            // 返回已完成任务。
            return Task.CompletedTask;
        }

        // 把更新排入 WPF Dispatcher，并返回可等待任务。
        return Application.Current.Dispatcher.InvokeAsync(action).Task;
    }
}
