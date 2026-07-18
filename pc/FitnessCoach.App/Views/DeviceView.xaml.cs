// 引入 WPF UserControl。
using System.Windows.Controls;

// 设备视图代码隐藏不包含业务逻辑。
namespace FitnessCoach.App.Views;

/// <summary>设备连接页面。</summary>
public partial class DeviceView : UserControl
{
    /// <summary>创建页面并加载 XAML。</summary>
    public DeviceView()
    {
        // 构建视觉树。
        InitializeComponent();
    }
}
