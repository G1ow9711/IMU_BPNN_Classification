// 引入 WPF UserControl。
using System.Windows.Controls;

// 历史视图代码隐藏只加载 XAML。
namespace FitnessCoach.App.Views;

/// <summary>本地训练历史页面。</summary>
public partial class HistoryView : UserControl
{
    /// <summary>创建历史页。</summary>
    public HistoryView()
    {
        // 构建视觉树。
        InitializeComponent();
    }
}
