// 引入 WPF 命令接口，为异步设备操作提供绑定入口。
using System.Windows.Input;

// 异步命令位于 MVVM 基础命名空间。
namespace FitnessCoach.App.Mvvm;

/// <summary>串行执行异步委托，运行中自动禁用，避免用户重复点击制造并发命令。</summary>
public sealed class AsyncRelayCommand : ICommand
{
    // 保存异步执行逻辑。
    private readonly Func<Task> _executeAsync;
    // 保存业务可执行条件。
    private readonly Func<bool>? _canExecute;
    // 运行标志由 Interlocked 保护，防止双击竞争。
    private int _isRunning;

    /// <summary>创建异步命令。</summary>
    public AsyncRelayCommand(Func<Task> executeAsync, Func<bool>? canExecute = null)
    {
        // 异步委托不能为空。
        ArgumentNullException.ThrowIfNull(executeAsync);
        // 保存执行逻辑。
        _executeAsync = executeAsync;
        // 保存可选条件。
        _canExecute = canExecute;
    }

    /// <inheritdoc />
    public event EventHandler? CanExecuteChanged;

    /// <summary>true 表示命令仍在等待设备或仓储完成。</summary>
    public bool IsRunning => Volatile.Read(ref _isRunning) != 0;

    /// <inheritdoc />
    public bool CanExecute(object? parameter)
    {
        // 运行中禁止再次进入；空条件表示业务允许。
        return !IsRunning && (_canExecute?.Invoke() ?? true);
    }

    /// <inheritdoc />
    public async void Execute(object? parameter)
    {
        // WPF ICommand 只能暴露 void；异常由 ViewModel 包装方法转为用户消息。
        await ExecuteAsync().ConfigureAwait(true);
    }

    /// <summary>供测试和代码直接等待命令完成。</summary>
    public async Task ExecuteAsync()
    {
        // 条件不满足时保持幂等，不执行委托。
        if (!CanExecute(null))
        {
            // 没有需要等待的业务操作。
            return;
        }

        // 原子占用运行标志；并发调用只有一个成功。
        if (Interlocked.Exchange(ref _isRunning, 1) != 0)
        {
            // 已有相同命令运行，直接返回。
            return;
        }

        // 通知 WPF 禁用按钮。
        RaiseCanExecuteChanged();

        try
        {
            // 等待真实设备或本地仓储异步操作完成。
            await _executeAsync().ConfigureAwait(true);
        }
        finally
        {
            // 无论成功还是失败都释放运行标志。
            Volatile.Write(ref _isRunning, 0);
            // 通知 WPF 恢复或重新评估按钮。
            RaiseCanExecuteChanged();
        }
    }

    /// <summary>通知 WPF 重新查询可执行状态。</summary>
    public void RaiseCanExecuteChanged()
    {
        // 发布标准命令状态事件。
        CanExecuteChanged?.Invoke(this, EventArgs.Empty);
    }
}
