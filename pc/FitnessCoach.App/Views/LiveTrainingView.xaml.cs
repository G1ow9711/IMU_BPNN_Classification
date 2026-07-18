// 引入 WPF UserControl。
using System.Windows.Controls;

// 实时训练视图代码隐藏不包含计数或设备业务。
namespace FitnessCoach.App.Views;

/// <summary>实时动作、指标和控制页面。</summary>
public partial class LiveTrainingView : UserControl
{
    /// <summary>创建页面并加载 XAML。</summary>
    public LiveTrainingView()
    {
        // 构建视觉树和绑定。
        InitializeComponent();
    }
}
