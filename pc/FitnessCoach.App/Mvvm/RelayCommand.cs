// 引入 WPF 命令合同，按钮通过该接口调用 ViewModel。
using System.Windows.Input;

// 同步命令位于 MVVM 基础命名空间。
namespace FitnessCoach.App.Mvvm;

/// <summary>把同步委托包装为可绑定命令，并允许主动刷新可执行状态。</summary>
public sealed class RelayCommand : ICommand
{
    // 保存命令执行逻辑；参数由绑定系统传入。
    private readonly Action<object?> _execute;
    // 保存可选可执行条件；null 表示始终允许。
    private readonly Func<object?, bool>? _canExecute;

    /// <summary>创建同步命令。</summary>
    public RelayCommand(Action<object?> execute, Func<object?, bool>? canExecute = null)
    {
        // 执行委托不能为空，否则点击按钮没有确定行为。
        ArgumentNullException.ThrowIfNull(execute);
        // 保存执行逻辑。
        _execute = execute;
        // 保存可选条件。
        _canExecute = canExecute;
    }

    /// <inheritdoc />
    public event EventHandler? CanExecuteChanged;

    /// <inheritdoc />
    public bool CanExecute(object? parameter)
    {
        // 未设置条件时返回 true；否则转发当前参数。
        return _canExecute?.Invoke(parameter) ?? true;
    }

    /// <inheritdoc />
    public void Execute(object? parameter)
    {
        // 执行构造时注入的业务逻辑。
        _execute(parameter);
    }

    /// <summary>通知 WPF 重新查询按钮启用状态。</summary>
    public void RaiseCanExecuteChanged()
    {
        // 在当前 UI 线程发布命令状态变化。
        CanExecuteChanged?.Invoke(this, EventArgs.Empty);
    }
}
