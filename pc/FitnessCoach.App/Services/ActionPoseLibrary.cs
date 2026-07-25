// 引入领域动作枚举，姿态表索引必须与 ESP32 和模型输出的 11 类顺序一致。
using FitnessCoach.Domain;

// 本地矢量动画数据位于应用服务层，测试无需创建 WPF 窗口即可验证周期和边界。
namespace FitnessCoach.App.Services;

/// <summary>定义教练人偶的 13 个关节点；坐标由 <see cref="StickFigurePose"/> 提供。</summary>
public enum FigureJoint
{
    // 头部圆心用于绘制独立圆形，不直接参与骨骼连线。
    Head,
    // 颈部连接头、双肩和躯干。
    Neck,
    // 左肩连接颈部和左肘。
    LeftShoulder,
    // 左肘连接左肩和左手。
    LeftElbow,
    // 左手是左臂末端。
    LeftHand,
    // 右肩连接颈部和右肘。
    RightShoulder,
    // 右肘连接右肩和右手。
    RightElbow,
    // 右手是右臂末端。
    RightHand,
    // 髋部连接躯干和双腿。
    Hip,
    // 左膝连接髋部和左脚。
    LeftKnee,
    // 左脚是左腿末端。
    LeftFoot,
    // 右膝连接髋部和右脚。
    RightKnee,
    // 右脚是右腿末端。
    RightFoot,
}

/// <summary>保存归一化画布中的一个关节点，X 约为 -1～1，Y 约为 0～2。</summary>
public readonly record struct PosePoint(double X, double Y)
{
    /// <summary>在两个关节点之间做线性插值；amount 必须位于 0～1。</summary>
    public static PosePoint Lerp(PosePoint start, PosePoint end, double amount)
    {
        // 横坐标按 amount 从起始点移动到终止点。
        double x = start.X + ((end.X - start.X) * amount);
        // 纵坐标按相同比例移动，保持骨架各关节同步。
        double y = start.Y + ((end.Y - start.Y) * amount);
        // 返回当前动画相位的关节点坐标。
        return new PosePoint(x, y);
    }
}

/// <summary>保存一帧完整教练人偶姿态；所有坐标均为无单位归一化坐标。</summary>
/// <param name="Head">头部圆心的无量纲画布坐标。</param>
/// <param name="Neck">连接头部、双肩和躯干的颈部坐标。</param>
/// <param name="LeftShoulder">左肩关节点坐标。</param>
/// <param name="LeftElbow">左肘关节点坐标。</param>
/// <param name="LeftHand">左臂末端手部坐标。</param>
/// <param name="RightShoulder">右肩关节点坐标。</param>
/// <param name="RightElbow">右肘关节点坐标。</param>
/// <param name="RightHand">右臂末端手部坐标。</param>
/// <param name="Hip">连接躯干与双腿的髋部中心坐标。</param>
/// <param name="LeftKnee">左膝关节点坐标。</param>
/// <param name="LeftFoot">左腿末端脚部坐标。</param>
/// <param name="RightKnee">右膝关节点坐标。</param>
/// <param name="RightFoot">右腿末端脚部坐标。</param>
public readonly record struct StickFigurePose(
    PosePoint Head,
    PosePoint Neck,
    PosePoint LeftShoulder,
    PosePoint LeftElbow,
    PosePoint LeftHand,
    PosePoint RightShoulder,
    PosePoint RightElbow,
    PosePoint RightHand,
    PosePoint Hip,
    PosePoint LeftKnee,
    PosePoint LeftFoot,
    PosePoint RightKnee,
    PosePoint RightFoot)
{
    /// <summary>读取指定关节点，供绘制器和纯逻辑测试使用。</summary>
    public PosePoint GetPoint(FigureJoint joint)
    {
        // 使用固定枚举映射，禁止依赖反射或数组顺序隐式转换。
        return joint switch
        {
            // 返回头部圆心。
            FigureJoint.Head => Head,
            // 返回颈部位置。
            FigureJoint.Neck => Neck,
            // 返回左肩位置。
            FigureJoint.LeftShoulder => LeftShoulder,
            // 返回左肘位置。
            FigureJoint.LeftElbow => LeftElbow,
            // 返回左手位置。
            FigureJoint.LeftHand => LeftHand,
            // 返回右肩位置。
            FigureJoint.RightShoulder => RightShoulder,
            // 返回右肘位置。
            FigureJoint.RightElbow => RightElbow,
            // 返回右手位置。
            FigureJoint.RightHand => RightHand,
            // 返回髋部位置。
            FigureJoint.Hip => Hip,
            // 返回左膝位置。
            FigureJoint.LeftKnee => LeftKnee,
            // 返回左脚位置。
            FigureJoint.LeftFoot => LeftFoot,
            // 返回右膝位置。
            FigureJoint.RightKnee => RightKnee,
            // 返回右脚位置。
            FigureJoint.RightFoot => RightFoot,
            // 未定义枚举表示代码合同漂移，立即抛错而不是绘制错误骨架。
            _ => throw new ArgumentOutOfRangeException(nameof(joint), joint, "未定义的教练人偶关节点。"),
        };
    }

    /// <summary>在两帧姿态间插值；用于生成平滑且完全离线的矢量动画。</summary>
    public static StickFigurePose Lerp(StickFigurePose start, StickFigurePose end, double amount)
    {
        // 把外部输入限制在 0～1，避免异常计时造成骨架外插到画布之外。
        double boundedAmount = Math.Clamp(amount, 0.0, 1.0);
        // 对 13 个关节点使用相同插值比例，保持身体拓扑连续。
        return new StickFigurePose(
            PosePoint.Lerp(start.Head, end.Head, boundedAmount),
            PosePoint.Lerp(start.Neck, end.Neck, boundedAmount),
            PosePoint.Lerp(start.LeftShoulder, end.LeftShoulder, boundedAmount),
            PosePoint.Lerp(start.LeftElbow, end.LeftElbow, boundedAmount),
            PosePoint.Lerp(start.LeftHand, end.LeftHand, boundedAmount),
            PosePoint.Lerp(start.RightShoulder, end.RightShoulder, boundedAmount),
            PosePoint.Lerp(start.RightElbow, end.RightElbow, boundedAmount),
            PosePoint.Lerp(start.RightHand, end.RightHand, boundedAmount),
            PosePoint.Lerp(start.Hip, end.Hip, boundedAmount),
            PosePoint.Lerp(start.LeftKnee, end.LeftKnee, boundedAmount),
            PosePoint.Lerp(start.LeftFoot, end.LeftFoot, boundedAmount),
            PosePoint.Lerp(start.RightKnee, end.RightKnee, boundedAmount),
            PosePoint.Lerp(start.RightFoot, end.RightFoot, boundedAmount));
    }
}

/// <summary>描述一个动作的循环时长、减少动画静态相位和关键姿态。</summary>
public sealed record ActionAnimationProfile(
    ActionId Action,
    double PeriodSeconds,
    double ReducedMotionPhase,
    IReadOnlyList<StickFigurePose> KeyFrames);

/// <summary>提供 11 类动作的本地矢量关键姿态和确定性周期采样。</summary>
public static class ActionPoseLibrary
{
    // 站立姿态是早安式、深蹲、行走和挥手等动作的共同起点。
    private static readonly StickFigurePose Stand = CreatePose(0.0, 0.22, 0.0, 0.48, 0.0, 1.12, -0.30, 0.76, -0.25, 1.06, 0.30, 0.76, 0.25, 1.06, -0.16, 1.55, -0.18, 1.90, 0.16, 1.55, 0.18, 1.90);
    // 深蹲姿态降低髋部并让双膝外展，区别于只有上身前倾的早安式。
    private static readonly StickFigurePose SquatLow = CreatePose(0.0, 0.44, 0.0, 0.68, 0.0, 1.36, -0.46, 0.78, -0.58, 0.92, 0.46, 0.78, 0.58, 0.92, -0.48, 1.50, -0.46, 1.88, 0.48, 1.50, 0.46, 1.88);

    // 固定动作表；每个列表至少两帧，最后一帧自动插值回第一帧形成无缝循环。
    private static readonly IReadOnlyDictionary<ActionId, ActionAnimationProfile> Profiles =
        new Dictionary<ActionId, ActionAnimationProfile>
        {
            // 早安式：髋部基本固定，上身和头部向前折叠后返回。
            [ActionId.GoodMorning] = Profile(ActionId.GoodMorning, 1.80, 0.48, Stand,
                CreatePose(0.65, 0.54, 0.47, 0.66, 0.0, 1.16, 0.28, 0.90, 0.52, 1.12, 0.62, 0.74, 0.82, 0.94, -0.15, 1.56, -0.18, 1.90, 0.15, 1.56, 0.18, 1.90)),
            // 开合跳：双脚并拢且手臂下垂，与双脚打开且双手举过头顶交替。
            [ActionId.JumpingJack] = Profile(ActionId.JumpingJack, 1.20, 0.50, Stand,
                CreatePose(0.0, 0.18, 0.0, 0.43, 0.0, 1.02, -0.50, 0.28, -0.74, 0.08, 0.50, 0.28, 0.74, 0.08, -0.42, 1.43, -0.72, 1.82, 0.42, 1.43, 0.72, 1.82)),
            // 跳跃弓步：左右弓步之间经过居中腾空帧，体现交替腿动作。
            [ActionId.JumpingLunge] = Profile(ActionId.JumpingLunge, 1.45, 0.20,
                CreatePose(-0.08, 0.25, -0.06, 0.52, -0.08, 1.06, 0.30, 0.68, 0.48, 0.92, -0.42, 0.62, -0.60, 0.82, -0.52, 1.42, -0.74, 1.84, 0.30, 1.48, 0.58, 1.84),
                CreatePose(0.0, 0.12, 0.0, 0.38, 0.0, 0.88, -0.36, 0.54, -0.52, 0.77, 0.36, 0.54, 0.52, 0.77, -0.26, 1.18, -0.38, 1.50, 0.26, 1.18, 0.38, 1.50),
                CreatePose(0.08, 0.25, 0.06, 0.52, 0.08, 1.06, -0.42, 0.62, -0.60, 0.82, 0.30, 0.68, 0.48, 0.92, -0.30, 1.48, -0.58, 1.84, 0.52, 1.42, 0.74, 1.84)),
            // 跳跃深蹲：低蹲、伸展腾空、落回低蹲，双腿始终保持对称。
            [ActionId.JumpingSquat] = Profile(ActionId.JumpingSquat, 1.50, 0.36, SquatLow,
                CreatePose(0.0, 0.08, 0.0, 0.34, 0.0, 0.82, -0.42, 0.26, -0.56, 0.08, 0.42, 0.26, 0.56, 0.08, -0.23, 1.22, -0.30, 1.58, 0.23, 1.22, 0.30, 1.58),
                SquatLow),
            // 普通弓步：站立与单侧前弓步往返，不包含腾空帧。
            [ActionId.Lunge] = Profile(ActionId.Lunge, 1.85, 0.55, Stand,
                CreatePose(-0.05, 0.28, -0.04, 0.53, -0.08, 1.12, 0.22, 0.72, 0.44, 0.95, -0.38, 0.68, -0.56, 0.91, -0.58, 1.45, -0.78, 1.86, 0.35, 1.42, 0.62, 1.86)),
            // 静坐：髋膝形成近直角，只保留极轻微呼吸位移。
            [ActionId.Sit] = Profile(ActionId.Sit, 2.40, 0.50,
                CreatePose(0.0, 0.25, 0.0, 0.51, 0.0, 1.08, -0.34, 0.78, -0.08, 1.08, 0.34, 0.78, 0.08, 1.08, -0.44, 1.08, -0.52, 1.70, 0.44, 1.08, 0.52, 1.70),
                CreatePose(0.0, 0.23, 0.0, 0.49, 0.0, 1.06, -0.34, 0.76, -0.08, 1.06, 0.34, 0.76, 0.08, 1.06, -0.44, 1.06, -0.52, 1.70, 0.44, 1.06, 0.52, 1.70)),
            // 深蹲：站立和低蹲往返，脚不离地。
            [ActionId.Squat] = Profile(ActionId.Squat, 1.80, 0.52, Stand, SquatLow),
            // 小跑：快速交替抬膝并摆动对侧手臂，周期短于行走。
            [ActionId.Trot] = Profile(ActionId.Trot, 0.82, 0.18,
                CreatePose(0.0, 0.18, 0.0, 0.44, 0.0, 1.02, 0.30, 0.66, 0.48, 0.42, -0.30, 0.68, -0.45, 0.96, -0.40, 1.24, -0.62, 1.48, 0.26, 1.46, 0.50, 1.80),
                CreatePose(0.0, 0.18, 0.0, 0.44, 0.0, 1.02, -0.30, 0.68, -0.45, 0.96, 0.30, 0.66, 0.48, 0.42, -0.26, 1.46, -0.50, 1.80, 0.40, 1.24, 0.62, 1.48)),
            // 收腹跳：由预蹲进入双膝上提的紧凑腾空帧，再落回预蹲。
            [ActionId.TuckJump] = Profile(ActionId.TuckJump, 1.35, 0.40, SquatLow,
                CreatePose(0.0, 0.08, 0.0, 0.34, 0.0, 0.76, -0.40, 0.56, -0.58, 0.88, 0.40, 0.56, 0.58, 0.88, -0.42, 1.02, -0.22, 1.30, 0.42, 1.02, 0.22, 1.30),
                SquatLow),
            // 行走：慢速交替迈步并摆动对侧手臂，双脚始终接近地面。
            [ActionId.Walk] = Profile(ActionId.Walk, 1.45, 0.18,
                CreatePose(0.0, 0.22, 0.0, 0.48, 0.0, 1.12, 0.24, 0.76, 0.42, 1.02, -0.24, 0.76, -0.42, 1.02, -0.32, 1.51, -0.56, 1.88, 0.28, 1.53, 0.48, 1.87),
                CreatePose(0.0, 0.22, 0.0, 0.48, 0.0, 1.12, -0.24, 0.76, -0.42, 1.02, 0.24, 0.76, 0.42, 1.02, -0.28, 1.53, -0.48, 1.87, 0.32, 1.51, 0.56, 1.88)),
            // 挥手：身体和双脚固定，右手在头侧左右摆动。
            [ActionId.Wave] = Profile(ActionId.Wave, 1.20, 0.25,
                CreatePose(0.0, 0.22, 0.0, 0.48, 0.0, 1.12, -0.30, 0.76, -0.25, 1.06, 0.36, 0.36, 0.58, 0.10, -0.16, 1.55, -0.18, 1.90, 0.16, 1.55, 0.18, 1.90),
                CreatePose(0.0, 0.22, 0.0, 0.48, 0.0, 1.12, -0.30, 0.76, -0.25, 1.06, 0.34, 0.34, 0.20, 0.06, -0.16, 1.55, -0.18, 1.90, 0.16, 1.55, 0.18, 1.90)),
        };

    /// <summary>尝试取得动作动画配置；Unknown 和非法枚举没有循环动画。</summary>
    public static bool TryGetProfile(ActionId action, out ActionAnimationProfile? profile)
    {
        // 使用字典显式验证动作，防止未知协议值映射到错误姿态。
        return Profiles.TryGetValue(action, out profile);
    }

    /// <summary>判断是否需要启动循环计时器；断线、Unknown 和减少动画均返回 false。</summary>
    public static bool ShouldAnimate(bool isConnected, ActionId action, bool reducedMotion)
    {
        // 连接有效、动作已映射且辅助功能允许时才持续刷新。
        return isConnected && !reducedMotion && Profiles.ContainsKey(action);
    }

    /// <summary>按动作、经过秒数和减少动画开关采样一帧确定性姿态。</summary>
    public static bool TrySample(ActionId action, double elapsedSeconds, bool reducedMotion, out StickFigurePose pose)
    {
        // Unknown 或非法动作没有姿态，由视图改画等待态。
        if (!Profiles.TryGetValue(action, out ActionAnimationProfile? profile))
        {
            // 输出默认零姿态，但返回 false，调用方不得绘制该零姿态。
            pose = default;
            // 返回 false 表示当前应显示等待态。
            return false;
        }

        // NaN 或无穷时间统一回退到周期起点，避免索引计算异常。
        double safeElapsed = double.IsFinite(elapsedSeconds) ? elapsedSeconds : 0.0;
        // 减少动画时固定到最能表达动作的关键相位，不启动循环。
        double normalizedPhase = reducedMotion
            ? profile.ReducedMotionPhase
            : NormalizeCycle(safeElapsed, profile.PeriodSeconds);
        // 把 0～1 周期映射到关键帧区间；最后一帧会插值回第一帧。
        double framePosition = normalizedPhase * profile.KeyFrames.Count;
        // 当前帧索引使用向下取整，并限制在合法范围。
        int startIndex = Math.Min((int)Math.Floor(framePosition), profile.KeyFrames.Count - 1);
        // 下一帧采用模运算，形成无缝循环。
        int endIndex = (startIndex + 1) % profile.KeyFrames.Count;
        // 小数部分是当前关键帧到下一关键帧的插值权重。
        double amount = framePosition - Math.Floor(framePosition);
        // 生成当前时刻的完整骨架姿态。
        pose = StickFigurePose.Lerp(profile.KeyFrames[startIndex], profile.KeyFrames[endIndex], amount);
        // 返回 true 表示姿态有效。
        return true;
    }

    // 把任意经过时间转换为 0～1 周期相位。
    private static double NormalizeCycle(double elapsedSeconds, double periodSeconds)
    {
        // 周期在静态表中已保证大于零，此检查防止未来配置漂移造成除零。
        if (!(periodSeconds > 0.0) || !double.IsFinite(periodSeconds))
        {
            // 非法周期回退到第一关键帧。
            return 0.0;
        }

        // 使用 IEEE 余数保留亚秒精度，并允许测试输入负时间。
        double remainder = elapsedSeconds % periodSeconds;
        // 负时间增加一个周期，使结果稳定落在 0～period 区间。
        if (remainder < 0.0)
        {
            // 平移到当前周期的正相位。
            remainder += periodSeconds;
        }

        // 除以周期得到 0～1 的无量纲相位。
        return remainder / periodSeconds;
    }

    // 创建并校验单个动作配置。
    private static ActionAnimationProfile Profile(ActionId action, double periodSeconds, double reducedMotionPhase, params StickFigurePose[] frames)
    {
        // 每个循环至少需要两帧，否则无法表达动作方向。
        if (frames.Length < 2)
        {
            // 静态表错误必须在类型初始化时直接暴露。
            throw new ArgumentException("动作动画至少需要两个关键姿态。", nameof(frames));
        }

        // 返回只读配置；period 单位为秒，ReducedMotionPhase 范围为 0～1。
        return new ActionAnimationProfile(action, periodSeconds, Math.Clamp(reducedMotionPhase, 0.0, 1.0), frames);
    }

    // 使用头、颈、髋和四肢末端参数创建完整 13 关节点姿态。
    private static StickFigurePose CreatePose(
        double headX,
        double headY,
        double neckX,
        double neckY,
        double hipX,
        double hipY,
        double leftElbowX,
        double leftElbowY,
        double leftHandX,
        double leftHandY,
        double rightElbowX,
        double rightElbowY,
        double rightHandX,
        double rightHandY,
        double leftKneeX,
        double leftKneeY,
        double leftFootX,
        double leftFootY,
        double rightKneeX,
        double rightKneeY,
        double rightFootX,
        double rightFootY)
    {
        // 肩宽固定为归一化 0.32，肩中心跟随颈部实现上身前倾。
        PosePoint leftShoulder = new(neckX - 0.16, neckY + 0.06);
        // 右肩与左肩关于颈部对称。
        PosePoint rightShoulder = new(neckX + 0.16, neckY + 0.06);
        // 组装完整姿态，所有点使用同一归一化坐标系。
        return new StickFigurePose(
            new PosePoint(headX, headY),
            new PosePoint(neckX, neckY),
            leftShoulder,
            new PosePoint(leftElbowX, leftElbowY),
            new PosePoint(leftHandX, leftHandY),
            rightShoulder,
            new PosePoint(rightElbowX, rightElbowY),
            new PosePoint(rightHandX, rightHandY),
            new PosePoint(hipX, hipY),
            new PosePoint(leftKneeX, leftKneeY),
            new PosePoint(leftFootX, leftFootY),
            new PosePoint(rightKneeX, rightKneeY),
            new PosePoint(rightFootX, rightFootY));
    }
}
