// 引入 WPF UserControl。
using System.Windows.Controls;

// 总结视图代码隐藏保持无业务逻辑。
namespace FitnessCoach.App.Views;

/// <summary>训练结束总结页面。</summary>
public partial class SummaryView : UserControl
{
    /// <summary>创建页面并加载 XAML。</summary>
    public SummaryView()
    {
        // 构建视觉树。
        InitializeComponent();
    }
}
