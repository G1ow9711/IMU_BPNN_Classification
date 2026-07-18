// 引入 WPF 基础元素、依赖属性和尺寸类型。
using System.Windows;
// 引入 WPF 矢量绘图、画笔和几何路径类型。
using System.Windows.Media;

// 六轴诊断曲线控件位于应用控件命名空间，避免 ViewModel 依赖具体页面。
namespace FitnessCoach.App.Controls;

/// <summary>保存同一时刻三个物理轴的不可变绘图点；时间单位为秒，轴值单位由图表标题定义。</summary>
public sealed record ImuPlotPoint(double Seconds, double X, double Y, double Z);

/// <summary>绘制最近十秒三轴 IMU 折线；控件不保存数据，也不启动后台线程。</summary>
public sealed class ImuLineChart : FrameworkElement
{
    // X 轴使用青色，便于在深色背景上区分。
    private static readonly Pen XAxisPen = CreateFrozenPen(Color.FromRgb(0x31, 0xD7, 0xFF));
    // Y 轴使用橙色，避免与 X、Z 轴混淆。
    private static readonly Pen YAxisPen = CreateFrozenPen(Color.FromRgb(0xFF, 0xA4, 0x3A));
    // Z 轴使用绿色，保持三条曲线颜色稳定。
    private static readonly Pen ZAxisPen = CreateFrozenPen(Color.FromRgb(0x65, 0xE5, 0x72));
    // 网格使用低对比度灰色，不遮挡实时曲线。
    private static readonly Pen GridPen = CreateFrozenPen(Color.FromRgb(0x35, 0x42, 0x55), 1.0);
    // 零值中心线略亮，用于判断正负方向。
    private static readonly Pen ZeroPen = CreateFrozenPen(Color.FromRgb(0x6A, 0x78, 0x8E), 1.0);
    // 背景使用接近页面卡片的深蓝黑色。
    private static readonly Brush BackgroundBrush = CreateFrozenBrush(Color.FromRgb(0x0C, 0x12, 0x1C));

    // 注册只读点序列依赖属性；绑定替换数组时自动触发重绘。
    public static readonly DependencyProperty PointsProperty = DependencyProperty.Register(
        nameof(Points),
        typeof(IReadOnlyList<ImuPlotPoint>),
        typeof(ImuLineChart),
        new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));

    // 注册纵轴绝对上限；值会被裁剪到正负该范围。
    public static readonly DependencyProperty VerticalLimitProperty = DependencyProperty.Register(
        nameof(VerticalLimit),
        typeof(double),
        typeof(ImuLineChart),
        new FrameworkPropertyMetadata(1.0, FrameworkPropertyMetadataOptions.AffectsRender, null, CoerceVerticalLimit));

    /// <summary>按时间递增的三轴物理量点；ViewModel 每次发布不可变快照。</summary>
    public IReadOnlyList<ImuPlotPoint>? Points
    {
        // 从 WPF 属性系统读取当前点序列。
        get => (IReadOnlyList<ImuPlotPoint>?)GetValue(PointsProperty);
        // 把新快照写入 WPF 属性系统并触发渲染。
        set => SetValue(PointsProperty, value);
    }

    /// <summary>纵轴正负对称范围；加速度建议 2 g，角速度建议 250 度每秒。</summary>
    public double VerticalLimit
    {
        // 从 WPF 属性系统读取当前正幅值。
        get => (double)GetValue(VerticalLimitProperty);
        // 写入正幅值；非法值会由强制函数改为 1。
        set => SetValue(VerticalLimitProperty, value);
    }

    /// <inheritdoc />
    protected override void OnRender(DrawingContext drawingContext)
    {
        // 先执行 WPF 基础渲染流程。
        base.OnRender(drawingContext);
        // 读取控件实际宽度，单位为设备无关像素。
        double width = ActualWidth;
        // 读取控件实际高度，单位为设备无关像素。
        double height = ActualHeight;
        // 绘图区没有有效面积时不构造几何对象。
        if (width <= 1.0 || height <= 1.0)
        {
            // 返回表示本帧没有可绘制区域。
            return;
        }

        // 用固定深色背景覆盖整个曲线区域。
        drawingContext.DrawRectangle(BackgroundBrush, null, new Rect(0.0, 0.0, width, height));
        // 绘制四等分网格，便于观察十秒窗口内的时间变化。
        for (int division = 1; division < 4; division++)
        {
            // 计算当前竖向网格线的横坐标。
            double x = width * division / 4.0;
            // 绘制从顶部到底部的竖向网格线。
            drawingContext.DrawLine(GridPen, new Point(x, 0.0), new Point(x, height));
            // 计算当前横向网格线的纵坐标。
            double y = height * division / 4.0;
            // 绘制从左到右的横向网格线。
            drawingContext.DrawLine(GridPen, new Point(0.0, y), new Point(width, y));
        }

        // 绘制零值中心线，正值在上、负值在下。
        drawingContext.DrawLine(ZeroPen, new Point(0.0, height / 2.0), new Point(width, height / 2.0));
        // 获取当前不可变点快照；空序列仅显示网格。
        IReadOnlyList<ImuPlotPoint>? points = Points;
        // 至少两个点才能形成线段。
        if (points is null || points.Count < 2)
        {
            // 返回表示暂无足够实时样本。
            return;
        }

        // 分别绘制 X、Y、Z 三个物理轴，轴索引固定为零、一、二。
        DrawSeries(drawingContext, points, width, height, VerticalLimit, 0, XAxisPen);
        // 绘制 Y 轴曲线。
        DrawSeries(drawingContext, points, width, height, VerticalLimit, 1, YAxisPen);
        // 绘制 Z 轴曲线。
        DrawSeries(drawingContext, points, width, height, VerticalLimit, 2, ZAxisPen);
    }

    // 把一个轴的点序列构造成连续流几何，避免为每个相邻点创建独立对象。
    private static void DrawSeries(
        DrawingContext drawingContext,
        IReadOnlyList<ImuPlotPoint> points,
        double width,
        double height,
        double verticalLimit,
        int axis,
        Pen pen)
    {
        // 创建轻量流几何存储当前轴的连续折线。
        StreamGeometry geometry = new();
        // 打开几何写入上下文；using 保证写入结束后正确封闭。
        using (StreamGeometryContext context = geometry.Open())
        {
            // 遍历当前窗口全部点，横轴按样本顺序均匀铺满。
            for (int index = 0; index < points.Count; index++)
            {
                // 读取当前不可变三轴点。
                ImuPlotPoint point = points[index];
                // 按固定轴索引选择 X、Y 或 Z 值。
                double value = axis == 0 ? point.X : axis == 1 ? point.Y : point.Z;
                // 把异常非有限值替换为零，防止 WPF 几何抛出异常。
                value = double.IsFinite(value) ? value : 0.0;
                // 把物理量裁剪到可视纵轴范围，峰值不会越出控件。
                double normalized = Math.Clamp(value / verticalLimit, -1.0, 1.0);
                // 将样本索引映射到零至实际宽度。
                double x = width * index / (points.Count - 1.0);
                // 将正值映射到中心线上方，并保留上下各 5% 边距。
                double y = height / 2.0 - normalized * height * 0.45;
                // 构造当前屏幕坐标点。
                Point screenPoint = new(x, y);
                // 第一个点设置路径起点，不产生线段。
                if (index == 0)
                {
                    // 从首个合法样本开始连续路径。
                    context.BeginFigure(screenPoint, false, false);
                }
                else
                {
                    // 后续点追加直线段；不启用额外平滑，保留真实采样变化。
                    context.LineTo(screenPoint, true, false);
                }
            }
        }

        // 冻结几何以降低 WPF 跨帧资源检查开销。
        geometry.Freeze();
        // 用当前轴固定颜色绘制折线，不填充封闭区域。
        drawingContext.DrawGeometry(null, pen, geometry);
    }

    // 非法或非正纵轴范围强制为 1，避免除零和 NaN 坐标。
    private static object CoerceVerticalLimit(DependencyObject dependencyObject, object baseValue)
    {
        // 读取绑定提供的双精度上限。
        double value = (double)baseValue;
        // 有限且大于零时保留调用者值，否则返回安全默认值 1。
        return double.IsFinite(value) && value > 0.0 ? value : 1.0;
    }

    // 创建并冻结折线画笔；默认宽度为 2 像素。
    private static Pen CreateFrozenPen(Color color, double thickness = 2.0)
    {
        // 创建指定纯色画刷。
        SolidColorBrush brush = new(color);
        // 冻结画刷，避免渲染线程重复检查可变状态。
        brush.Freeze();
        // 创建带圆角端点的折线画笔。
        Pen pen = new(brush, thickness)
        {
            // 圆角线帽让相邻采样点连接更清晰。
            StartLineCap = PenLineCap.Round,
            // 末端同样使用圆角线帽。
            EndLineCap = PenLineCap.Round,
            // 拐点使用圆角连接，减少尖锐锯齿。
            LineJoin = PenLineJoin.Round,
        };
        // 冻结画笔，供所有控件实例安全共享。
        pen.Freeze();
        // 返回不可变画笔。
        return pen;
    }

    // 创建并冻结纯色背景画刷。
    private static Brush CreateFrozenBrush(Color color)
    {
        // 创建指定颜色的 WPF 纯色画刷。
        SolidColorBrush brush = new(color);
        // 冻结画刷以减少渲染开销。
        brush.Freeze();
        // 返回不可变背景画刷。
        return brush;
    }
}
