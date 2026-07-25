// 引入 WPF UserControl。
using System.Windows.Controls;

// 诊断视图代码隐藏不执行硬件操作。
namespace FitnessCoach.App.Views;

/// <summary>协议、设备模式和产品阈值诊断页面。</summary>
public partial class DiagnosticsView : UserControl
{
    /// <summary>创建诊断页。</summary>
    public DiagnosticsView()
    {
        // 构建视觉树。
        InitializeComponent();
    }
}
