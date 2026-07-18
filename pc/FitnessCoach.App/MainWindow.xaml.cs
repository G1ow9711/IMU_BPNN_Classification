// 引入 WPF Window 基类。
using System.Windows;

// 主窗口代码隐藏仅负责加载 XAML，不包含业务状态。
namespace FitnessCoach.App;

/// <summary>应用主窗口；业务逻辑全部位于 MainViewModel。</summary>
public partial class MainWindow : Window
{
    /// <summary>创建窗口并加载已编译 XAML。</summary>
    public MainWindow()
    {
        // 构建 XAML 视觉树和绑定。
        InitializeComponent();
    }
}
