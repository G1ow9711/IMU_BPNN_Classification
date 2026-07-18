// 引入高精度单调计时器，动画不依赖系统墙上时间调整。
using System.Diagnostics;
// 引入格式化文字所需区域信息。
using System.Globalization;
// 引入 WPF 依赖属性、尺寸和基础元素。
using System.Windows;
// 引入 WPF 画刷、画笔和 DrawingContext。
using System.Windows.Media;
// 引入 UI 线程 DispatcherTimer，禁止后台线程直接绘制。
using System.Windows.Threading;
// 引入本地姿态采样器。
using FitnessCoach.App.Services;
// 引入权威动作枚举。
using FitnessCoach.Domain;

// 自定义绘制控件位于 Controls 命名空间，XAML 不需要外部图片或网络资源。
namespace FitnessCoach.App.Controls;

/// <summary>按权威 ActionId 和 AnimationRevision 绘制 11 类本地矢量火柴人动画。</summary>
public sealed class ActionStickFigure : FrameworkElement
{
    // 使用约 30 FPS，兼顾动作可读性和 Windows 上位机空闲功耗。
    private static readonly TimeSpan FrameInterval = TimeSpan.FromMilliseconds(33.0);
    // 文字字体使用 Windows 自带中文字体回退链，不携带第三方字库。
    private static readonly Typeface WaitingTypeface = new("Microsoft YaHei UI");
    // UI DispatcherTimer 保证 Tick 与 WPF 绘制运行在同一线程。
    private readonly DispatcherTimer _frameTimer;
    // 保存当前修订周期的单调起点；每次权威 revision 变化会重置。
    private long _phaseStartTimestamp;

    // 注册权威动作依赖属性；Unknown 表示等待识别。
    public static readonly DependencyProperty ActionProperty = DependencyProperty.Register(
        nameof(Action),
        typeof(ActionId),
        typeof(ActionStickFigure),
        new FrameworkPropertyMetadata(ActionId.Unknown, FrameworkPropertyMetadataOptions.AffectsRender, OnAnimationInputChanged));

    // 注册动画修订号依赖属性；状态帧变化时重启动作周期和短脉冲。
    public static readonly DependencyProperty AnimationRevisionProperty = DependencyProperty.Register(
        nameof(AnimationRevision),
        typeof(uint),
        typeof(ActionStickFigure),
        new FrameworkPropertyMetadata(0U, FrameworkPropertyMetadataOptions.AffectsRender, OnAnimationInputChanged));

    // 注册减少动画属性；true 时固定显示代表性关键姿态并停掉计时器。
    public static readonly DependencyProperty ReducedMotionProperty = DependencyProperty.Register(
        nameof(ReducedMotion),
        typeof(bool),
        typeof(ActionStickFigure),
        new FrameworkPropertyMetadata(false, FrameworkPropertyMetadataOptions.AffectsRender, OnAnimationInputChanged));

    // 注册连接状态属性；断线时必须隐藏旧动作并显示等待态。
    public static readonly DependencyProperty IsConnectedProperty = DependencyProperty.Register(
        nameof(IsConnected),
        typeof(bool),
        typeof(ActionStickFigure),
        new FrameworkPropertyMetadata(false, FrameworkPropertyMetadataOptions.AffectsRender, OnAnimationInputChanged));

    // 注册主骨架画刷，默认青绿色并允许 XAML 使用主题资源覆盖。
    public static readonly DependencyProperty StrokeBrushProperty = DependencyProperty.Register(
        nameof(StrokeBrush),
        typeof(Brush),
        typeof(ActionStickFigure),
        new FrameworkPropertyMetadata(Brushes.MediumSpringGreen, FrameworkPropertyMetadataOptions.AffectsRender));

    // 注册等待态和地面辅助画刷，默认中灰色。
    public static readonly DependencyProperty SecondaryBrushProperty = DependencyProperty.Register(
        nameof(SecondaryBrush),
        typeof(Brush),
        typeof(ActionStickFigure),
        new FrameworkPropertyMetadata(Brushes.Gray, FrameworkPropertyMetadataOptions.AffectsRender));

    /// <summary>创建矢量动画控件并绑定 UI 线程计时器生命周期。</summary>
    public ActionStickFigure()
    {
        // 创建 30 FPS DispatcherTimer；不创建后台线程。
        _frameTimer = new DispatcherTimer(DispatcherPriority.Render, Dispatcher)
        {
            // 设置帧间隔，动画每秒最多刷新约 30 次。
            Interval = FrameInterval,
        };
        // 每次 Tick 只请求 WPF 重绘，不在 Tick 内计算布局或修改 ViewModel。
        _frameTimer.Tick += OnFrameTick;
        // 控件载入视觉树时启动必要的计时器。
        Loaded += OnLoaded;
        // 控件卸载时停止计时器，避免页面切换后继续刷新。
        Unloaded += OnUnloaded;
        // 页面不可见时暂停刷新，重新可见时恢复。
        IsVisibleChanged += OnIsVisibleChanged;
        // 初始化相位起点，首次绘制从第一关键帧开始。
        ResetPhaseClock();
    }

    /// <summary>权威动作 ID；仅接受领域层固定 11 类或 Unknown。</summary>
    public ActionId Action
    {
        // 从 WPF 依赖属性存储读取当前动作。
        get => (ActionId)GetValue(ActionProperty);
        // 写入依赖属性会触发重绘和相位重置。
        set => SetValue(ActionProperty, value);
    }

    /// <summary>权威动画修订号；变化时重新对齐当前动作周期。</summary>
    public uint AnimationRevision
    {
        // 读取当前修订号。
        get => (uint)GetValue(AnimationRevisionProperty);
        // 写入新修订号并触发动画输入回调。
        set => SetValue(AnimationRevisionProperty, value);
    }

    /// <summary>true 时停用循环和脉冲，只显示动作代表姿态。</summary>
    public bool ReducedMotion
    {
        // 读取有效减少动画设置。
        get => (bool)GetValue(ReducedMotionProperty);
        // 更新设置并重新配置计时器。
        set => SetValue(ReducedMotionProperty, value);
    }

    /// <summary>true 表示设备链路已连接；false 时显示断线等待态。</summary>
    public bool IsConnected
    {
        // 读取当前链路状态。
        get => (bool)GetValue(IsConnectedProperty);
        // 更新链路状态并清除旧动作视觉。
        set => SetValue(IsConnectedProperty, value);
    }

    /// <summary>主骨架画刷。</summary>
    public Brush StrokeBrush
    {
        // 读取主题骨架画刷。
        get => (Brush)GetValue(StrokeBrushProperty);
        // 写入主题骨架画刷。
        set => SetValue(StrokeBrushProperty, value);
    }

    /// <summary>等待文字、地面和阴影辅助画刷。</summary>
    public Brush SecondaryBrush
    {
        // 读取辅助画刷。
        get => (Brush)GetValue(SecondaryBrushProperty);
        // 写入辅助画刷。
        set => SetValue(SecondaryBrushProperty, value);
    }

    /// <inheritdoc />
    protected override Size MeasureOverride(Size availableSize)
    {
        // 无明确约束时请求 360×360 逻辑像素，适合实时页左侧卡片。
        double width = double.IsInfinity(availableSize.Width) ? 360.0 : availableSize.Width;
        // 高度同样限制为可用值，避免无限尺寸进入绘制计算。
        double height = double.IsInfinity(availableSize.Height) ? 360.0 : availableSize.Height;
        // 最小尺寸保证等待文字和骨架仍可读。
        return new Size(Math.Max(180.0, width), Math.Max(220.0, height));
    }

    /// <inheritdoc />
    protected override void OnRender(DrawingContext drawingContext)
    {
        // 先调用基类，保留 WPF FrameworkElement 绘制合同。
        base.OnRender(drawingContext);
        // 无可用面积时不执行坐标换算。
        if (!(ActualWidth > 1.0) || !(ActualHeight > 1.0))
        {
            // 等待下一次有效布局。
            return;
        }

        // 断线或 Unknown 时禁止显示最后一个动作，避免用户把旧动作误认为实时结果。
        if (!IsConnected || !ActionPoseLibrary.TrySample(Action, GetElapsedSeconds(), ReducedMotion, out StickFigurePose pose))
        {
            // 绘制本地等待态；文本依据连接状态区分断线和等待识别。
            DrawWaitingState(drawingContext, IsConnected ? "等待识别" : "设备未连接");
            // 等待态不再绘制任何旧骨架。
            return;
        }

        // 绘制当前动作的插值骨架。
        DrawPose(drawingContext, pose);
    }

    // 动作、修订号、减少动画或连接状态变化时重置周期并更新计时器。
    private static void OnAnimationInputChanged(DependencyObject dependencyObject, DependencyPropertyChangedEventArgs eventArgs)
    {
        // 依赖属性只注册在当前控件类型上，此转换始终安全。
        ActionStickFigure control = (ActionStickFigure)dependencyObject;
        // 权威输入变化后从新周期起点开始，AnimationRevision 因而能驱动画面反馈。
        control.ResetPhaseClock();
        // 根据连接、动作和辅助功能状态决定是否需要 30 FPS 刷新。
        control.UpdateTimerState();
        // 立即请求一帧，减少动画和等待态无需等待下一个 Tick。
        control.InvalidateVisual();
    }

    // 控件进入视觉树时配置动画。
    private void OnLoaded(object sender, RoutedEventArgs eventArgs)
    {
        // 重新载入页面时从当前动作第一帧开始。
        ResetPhaseClock();
        // 只在需要循环动画时启动计时器。
        UpdateTimerState();
    }

    // 控件离开视觉树时释放计时器活动。
    private void OnUnloaded(object sender, RoutedEventArgs eventArgs)
    {
        // 停止 UI Tick，防止隐藏页面继续消耗 CPU。
        _frameTimer.Stop();
    }

    // 可见性变化时暂停或恢复动画。
    private void OnIsVisibleChanged(object sender, DependencyPropertyChangedEventArgs eventArgs)
    {
        // 依据新可见性重算计时器状态。
        UpdateTimerState();
    }

    // 每个 UI 帧只触发重绘。
    private void OnFrameTick(object? sender, EventArgs eventArgs)
    {
        // 请求 WPF 在当前 UI 线程执行 OnRender。
        InvalidateVisual();
    }

    // 根据业务状态启动或停止刷新计时器。
    private void UpdateTimerState()
    {
        // 只有已加载、可见、已连接、已识别且未减少动画时才需要连续刷新。
        bool shouldRun = IsLoaded
            && IsVisible
            && ActionPoseLibrary.ShouldAnimate(IsConnected, Action, ReducedMotion);
        // 所有条件满足时启动计时器。
        if (shouldRun)
        {
            // DispatcherTimer.Start 可重复调用且不会创建第二个计时器。
            _frameTimer.Start();
        }
        else
        {
            // 等待态或减少动画时停止持续刷新，降低 PC 功耗。
            _frameTimer.Stop();
        }
    }

    // 重置本次动作修订对应的相位起点。
    private void ResetPhaseClock()
    {
        // Stopwatch 时间戳单调递增，不受系统时间校准影响。
        _phaseStartTimestamp = Stopwatch.GetTimestamp();
    }

    // 计算当前修订号以来的经过秒数。
    private double GetElapsedSeconds()
    {
        // 减少动画时返回零；姿态库会改用动作专用静态相位。
        if (ReducedMotion)
        {
            // 禁止时间变化进入姿态采样。
            return 0.0;
        }

        // 读取当前单调时间戳。
        long now = Stopwatch.GetTimestamp();
        // 转换为秒，范围在普通应用生命周期内不会溢出 double。
        return (now - _phaseStartTimestamp) / (double)Stopwatch.Frequency;
    }

    // 绘制一个完整火柴人姿态。
    private void DrawPose(DrawingContext drawingContext, StickFigurePose pose)
    {
        // 坐标系宽约 2.0、高约 2.0，额外留边避免手脚贴住卡片边缘。
        double scale = Math.Min(ActualWidth / 2.45, ActualHeight / 2.28);
        // 水平中心位于控件正中。
        double centerX = ActualWidth * 0.5;
        // 垂直原点居中并留出头部外圈。
        double topY = Math.Max(8.0, (ActualHeight - (2.02 * scale)) * 0.5);
        // 骨架线宽随控件尺寸变化，并限制在 4～9 像素。
        double strokeWidth = Math.Clamp(scale * 0.045, 4.0, 9.0);
        // 创建圆头圆尾画笔，让火柴人关节在缩放后仍平滑。
        Pen bodyPen = new(StrokeBrush, strokeWidth)
        {
            // 起点使用圆头，避免手脚末端出现方块。
            StartLineCap = PenLineCap.Round,
            // 终点使用圆头。
            EndLineCap = PenLineCap.Round,
            // 折线连接使用圆角。
            LineJoin = PenLineJoin.Round,
        };
        // 创建较细辅助画笔绘制地面基准。
        Pen floorPen = new(SecondaryBrush, Math.Max(1.0, strokeWidth * 0.22));
        // 地面从画布 18% 宽度延伸到 82% 宽度。
        Point floorStart = new(ActualWidth * 0.18, topY + (1.96 * scale));
        // 地面终点与起点对称。
        Point floorEnd = new(ActualWidth * 0.82, topY + (1.96 * scale));
        // 绘制半透明感由主题灰色画刷自身提供。
        drawingContext.DrawLine(floorPen, floorStart, floorEnd);

        // 绘制颈部到左肩。
        DrawBone(drawingContext, bodyPen, pose.Neck, pose.LeftShoulder, centerX, topY, scale);
        // 绘制左上臂。
        DrawBone(drawingContext, bodyPen, pose.LeftShoulder, pose.LeftElbow, centerX, topY, scale);
        // 绘制左前臂。
        DrawBone(drawingContext, bodyPen, pose.LeftElbow, pose.LeftHand, centerX, topY, scale);
        // 绘制颈部到右肩。
        DrawBone(drawingContext, bodyPen, pose.Neck, pose.RightShoulder, centerX, topY, scale);
        // 绘制右上臂。
        DrawBone(drawingContext, bodyPen, pose.RightShoulder, pose.RightElbow, centerX, topY, scale);
        // 绘制右前臂。
        DrawBone(drawingContext, bodyPen, pose.RightElbow, pose.RightHand, centerX, topY, scale);
        // 绘制颈部到髋部的躯干。
        DrawBone(drawingContext, bodyPen, pose.Neck, pose.Hip, centerX, topY, scale);
        // 绘制左大腿。
        DrawBone(drawingContext, bodyPen, pose.Hip, pose.LeftKnee, centerX, topY, scale);
        // 绘制左小腿。
        DrawBone(drawingContext, bodyPen, pose.LeftKnee, pose.LeftFoot, centerX, topY, scale);
        // 绘制右大腿。
        DrawBone(drawingContext, bodyPen, pose.Hip, pose.RightKnee, centerX, topY, scale);
        // 绘制右小腿。
        DrawBone(drawingContext, bodyPen, pose.RightKnee, pose.RightFoot, centerX, topY, scale);
        // 头部圆心转换为 WPF 像素坐标。
        Point headCenter = TransformPoint(pose.Head, centerX, topY, scale);
        // 头部半径随控件缩放并与骨架线宽保持比例。
        double headRadius = Math.Clamp(scale * 0.13, 13.0, 31.0);
        // 只绘制圆环不填充，保持真黑界面和轻量矢量风格。
        drawingContext.DrawEllipse(null, bodyPen, headCenter, headRadius, headRadius);

        // 减少动画时不显示任何修订脉冲，满足辅助功能要求。
        if (!ReducedMotion)
        {
            // 修订后的 220ms 内计算从 1 线性衰减到 0 的脉冲强度。
            double pulse = Math.Max(0.0, 1.0 - (GetElapsedSeconds() / 0.22));
            // 仅在脉冲有效时绘制外圈，避免常驻装饰干扰动作姿态。
            if (pulse > 0.0)
            {
                // 脉冲外圈半径随衰减逐渐向外扩散。
                double pulseRadius = headRadius + (10.0 * (1.0 - pulse));
                // 脉冲画笔使用主色且随强度变细。
                Pen pulsePen = new(StrokeBrush, Math.Max(1.0, strokeWidth * 0.35 * pulse));
                // 绘制 revision 驱动的短外圈。
                drawingContext.DrawEllipse(null, pulsePen, headCenter, pulseRadius, pulseRadius);
            }
        }
    }

    // 绘制两个归一化关节点之间的一根骨骼。
    private static void DrawBone(DrawingContext drawingContext, Pen pen, PosePoint start, PosePoint end, double centerX, double topY, double scale)
    {
        // 转换骨骼起点到 WPF 像素坐标。
        Point startPoint = TransformPoint(start, centerX, topY, scale);
        // 转换骨骼终点到 WPF 像素坐标。
        Point endPoint = TransformPoint(end, centerX, topY, scale);
        // 绘制线段，DrawingContext 不保留额外 UIElement，减少布局开销。
        drawingContext.DrawLine(pen, startPoint, endPoint);
    }

    // 把归一化姿态坐标转换为当前控件像素坐标。
    private static Point TransformPoint(PosePoint point, double centerX, double topY, double scale)
    {
        // X 以控件中心为零，正值向右。
        double x = centerX + (point.X * scale);
        // Y 以顶部原点为零，正值向下，符合 WPF 坐标系。
        double y = topY + (point.Y * scale);
        // 返回绘图坐标。
        return new Point(x, y);
    }

    // 绘制断线或未知动作等待态。
    private void DrawWaitingState(DrawingContext drawingContext, string statusText)
    {
        // 等待圆环中心位于控件中部略偏上。
        Point center = new(ActualWidth * 0.5, ActualHeight * 0.44);
        // 等待圆环半径限制在 34～68 像素。
        double radius = Math.Clamp(Math.Min(ActualWidth, ActualHeight) * 0.14, 34.0, 68.0);
        // 使用低亮度画笔，避免等待态被误认为有效动作。
        Pen waitPen = new(SecondaryBrush, 3.0);
        // 绘制等待状态外圈。
        drawingContext.DrawEllipse(null, waitPen, center, radius, radius);
        // 三个点水平分布在圆环内部。
        for (int dotIndex = -1; dotIndex <= 1; dotIndex++)
        {
            // 每个点间距为半径的 0.38。
            Point dotCenter = new(center.X + (dotIndex * radius * 0.38), center.Y);
            // 填充辅助色圆点；等待态不循环跳动，减少视觉干扰。
            drawingContext.DrawEllipse(SecondaryBrush, null, dotCenter, 4.5, 4.5);
        }

        // 获取当前显示器 DPI，保证文字在 125%/150% 缩放下清晰。
        double pixelsPerDip = VisualTreeHelper.GetDpi(this).PixelsPerDip;
        // 创建本地状态文字，不依赖网络字体或图片。
        FormattedText text = new(
            statusText,
            CultureInfo.CurrentUICulture,
            FlowDirection.LeftToRight,
            WaitingTypeface,
            15.0,
            SecondaryBrush,
            pixelsPerDip);
        // 把文字水平居中并放在圆环下方。
        Point textOrigin = new((ActualWidth - text.Width) * 0.5, center.Y + radius + 20.0);
        // 绘制状态文字。
        drawingContext.DrawText(text, textOrigin);
    }
}
