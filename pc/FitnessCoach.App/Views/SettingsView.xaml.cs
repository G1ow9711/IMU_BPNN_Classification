// 引入 WPF UserControl。
using System.Windows.Controls;

// 设置视图代码隐藏不做范围校验，校验位于仓储合同。
namespace FitnessCoach.App.Views;

/// <summary>设备和用户偏好设置页面。</summary>
public partial class SettingsView : UserControl
{
    /// <summary>创建设置页。</summary>
    public SettingsView()
    {
        // 构建视觉树。
        InitializeComponent();
    }
}
