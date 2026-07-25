// 引入组件模型接口，为 WPF 数据绑定发布属性变化。
using System.ComponentModel;
// 引入调用方成员名，减少属性通知中的硬编码字符串。
using System.Runtime.CompilerServices;

// MVVM 基础类型集中在该命名空间，业务 ViewModel 不重复样板代码。
namespace FitnessCoach.App.Mvvm;

/// <summary>
/// 提供线程无关的属性变化通知；调用者必须通过 UI 调度器切回界面线程。
/// </summary>
public abstract class ObservableObject : INotifyPropertyChanged
{
    /// <inheritdoc />
    public event PropertyChangedEventHandler? PropertyChanged;

    /// <summary>
    /// 在字段值真正变化时写入新值并通知绑定，返回值表示是否发生变化。
    /// </summary>
    protected bool SetProperty<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        // 相同值无需触发 WPF 重绘和命令刷新。
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            // 返回 false 表示字段未改变。
            return false;
        }

        // 保存新字段值。
        field = value;
        // 通知 WPF 对应绑定属性已经变化。
        OnPropertyChanged(propertyName);
        // 返回 true 表示调用者可继续执行派生更新。
        return true;
    }

    /// <summary>显式发布指定属性变化，适用于计算属性。</summary>
    protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
    {
        // 构造标准属性变化事件参数并通知全部订阅者。
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}
