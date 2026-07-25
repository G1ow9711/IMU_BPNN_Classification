// 引入动作动画服务和纯姿态采样器。
using FitnessCoach.App.Services;
// 引入固定 11 类动作枚举。
using FitnessCoach.Domain;

// 动画测试位于现有控制台测试程序集，不需要启动 WPF 窗口。
namespace FitnessCoach.Tests;

/// <summary>验证 11 类动画映射、周期闭合、减少动画和等待边界。</summary>
internal static class ActionAnimationTests
{
    /// <summary>顺序执行全部动作动画纯逻辑测试。</summary>
    public static void RunAll()
    {
        // 验证 11 类同时具有视觉描述和矢量关键姿态。
        TestAllActionsHaveDistinctProfiles();
        // 验证每个动画周期首尾闭合且中间帧确实发生运动。
        TestCycleBoundaryAndMotion();
        // 验证减少动画在任意时间都返回同一代表姿态。
        TestReducedMotionIsStatic();
        // 验证 Unknown、非法枚举和非有限时间不会产生错误动作。
        TestUnknownAndInvalidBoundaries();
        // 验证断线、未知和减少动画不会启动循环计时器。
        TestAnimationStartBoundaries();
        // 验证 Windows 系统设置优先于用户开关且事件只在变化时触发。
        TestAnimationPreferencePrecedence();
    }

    // 验证全部模型动作均有本地资源描述和至少两帧矢量姿态。
    private static void TestAllActionsHaveDistinctProfiles()
    {
        // 创建真实本地视觉控制器。
        LocalActionAnimationController controller = new();
        // 保存每类代表姿态指纹，防止 11 类全部误用同一占位动作。
        HashSet<string> poseFingerprints = [];
        // 遍历领域层固定动作，排除协议等待值 Unknown。
        foreach (ActionId action in Enum.GetValues<ActionId>().Where(value => value != ActionId.Unknown))
        {
            // 切换视觉描述到当前动作。
            controller.SetAction(action);
            // 视觉描述必须与请求动作一致且使用本地 action.* 资源键。
            Assert(controller.Current.Action == action
                && !string.IsNullOrWhiteSpace(controller.Current.ChineseName)
                && controller.Current.ResourceKey.StartsWith("action.", StringComparison.Ordinal), $"动作 {action} 缺少本地视觉描述。 ");
            // 姿态库必须返回有效配置。
            bool found = ActionPoseLibrary.TryGetProfile(action, out ActionAnimationProfile? profile);
            // 每类至少两个关键帧，周期必须为有限正数。
            Assert(found
                && profile is not null
                && profile.KeyFrames.Count >= 2
                && double.IsFinite(profile.PeriodSeconds)
                && profile.PeriodSeconds > 0.0, $"动作 {action} 的矢量动画配置非法。 ");
            // 在减少动画代表相位采样一帧。
            bool sampled = ActionPoseLibrary.TrySample(action, 123.0, true, out StickFigurePose pose);
            // 有效动作必须能采样。
            Assert(sampled, $"动作 {action} 无法采样代表姿态。 ");
            // 使用头、双手、髋和双脚生成粗粒度指纹，验证动作视觉不是同一占位符。
            string fingerprint = string.Create(
                System.Globalization.CultureInfo.InvariantCulture,
                $"{pose.Head.X:F2},{pose.Head.Y:F2}|{pose.LeftHand.X:F2},{pose.LeftHand.Y:F2}|{pose.RightHand.X:F2},{pose.RightHand.Y:F2}|{pose.Hip.X:F2},{pose.Hip.Y:F2}|{pose.LeftFoot.X:F2},{pose.LeftFoot.Y:F2}|{pose.RightFoot.X:F2},{pose.RightFoot.Y:F2}");
            // 保存当前动作指纹。
            poseFingerprints.Add(fingerprint);
        }

        // 允许少量动作共享站立起点，但 11 类代表姿态至少应形成 9 种明显不同轮廓。
        Assert(poseFingerprints.Count >= 9, $"11 类动作只有 {poseFingerprints.Count} 种代表轮廓，区分度不足。 ");
    }

    // 验证周期采样的数学边界。
    private static void TestCycleBoundaryAndMotion()
    {
        // 遍历全部有效动作。
        foreach (ActionId action in Enum.GetValues<ActionId>().Where(value => value != ActionId.Unknown))
        {
            // 取得当前动作周期。
            bool found = ActionPoseLibrary.TryGetProfile(action, out ActionAnimationProfile? profile);
            // 配置在上一测试已保证存在，这里仍独立断言避免测试顺序依赖。
            Assert(found && profile is not null, $"动作 {action} 周期测试缺少配置。 ");
            // 采样周期起点。
            bool startOk = ActionPoseLibrary.TrySample(action, 0.0, false, out StickFigurePose start);
            // 采样整整一个周期后的姿态。
            bool endOk = ActionPoseLibrary.TrySample(action, profile!.PeriodSeconds, false, out StickFigurePose end);
            // 采样四分之一周期，应该已经离开起始关键帧。
            bool movingOk = ActionPoseLibrary.TrySample(action, profile.PeriodSeconds * 0.25, false, out StickFigurePose moving);
            // 三次采样都必须成功。
            Assert(startOk && endOk && movingOk, $"动作 {action} 周期采样失败。 ");
            // 周期首尾所有关节点必须闭合。
            Assert(PosesNear(start, end, 1e-12), $"动作 {action} 周期首尾不闭合。 ");
            // 四分之一周期至少一个关节点应移动 0.001 以上，证明不是静态占位图。
            Assert(!PosesNear(start, moving, 1e-3), $"动作 {action} 周期中没有可见运动。 ");
        }
    }

    // 验证减少动画不会随时间变化。
    private static void TestReducedMotionIsStatic()
    {
        // 遍历 11 个有效动作。
        foreach (ActionId action in Enum.GetValues<ActionId>().Where(value => value != ActionId.Unknown))
        {
            // 在零秒采样减少动画姿态。
            bool firstOk = ActionPoseLibrary.TrySample(action, 0.0, true, out StickFigurePose first);
            // 在很晚时间采样同一减少动画姿态。
            bool laterOk = ActionPoseLibrary.TrySample(action, 9876.5, true, out StickFigurePose later);
            // 两次都必须成功且逐关节完全相同。
            Assert(firstOk && laterOk && PosesNear(first, later, 0.0), $"动作 {action} 在减少动画模式下仍随时间变化。 ");
        }
    }

    // 验证无动画输入和异常时间边界。
    private static void TestUnknownAndInvalidBoundaries()
    {
        // Unknown 不得映射到任一有效动作配置。
        Assert(!ActionPoseLibrary.TryGetProfile(ActionId.Unknown, out _), "Unknown 被错误映射为有效动画。 ");
        // Unknown 采样必须返回 false，让控件显示等待态。
        Assert(!ActionPoseLibrary.TrySample(ActionId.Unknown, 0.0, false, out _), "Unknown 被错误采样为动作姿态。 ");
        // 未定义字节 200 也必须返回 false。
        Assert(!ActionPoseLibrary.TrySample((ActionId)200, 0.0, false, out _), "非法 action_id 被错误采样。 ");
        // NaN 时间对有效动作回退到周期起点，不抛异常。
        bool nanOk = ActionPoseLibrary.TrySample(ActionId.Squat, double.NaN, false, out StickFigurePose nanPose);
        // 取得正常起点用于比较。
        bool zeroOk = ActionPoseLibrary.TrySample(ActionId.Squat, 0.0, false, out StickFigurePose zeroPose);
        // 非有限输入必须安全回退且结果有限。
        Assert(nanOk && zeroOk && PosesNear(nanPose, zeroPose, 0.0) && PoseIsFinite(nanPose), "NaN 时间未安全回退到周期起点。 ");
    }

    // 验证控制器启动循环的纯逻辑边界。
    private static void TestAnimationStartBoundaries()
    {
        // 已连接、有效动作且未减少动画时应循环。
        Assert(ActionPoseLibrary.ShouldAnimate(true, ActionId.Walk, false), "有效连接和动作未启动动画。 ");
        // 断线时必须停止循环并显示等待态。
        Assert(!ActionPoseLibrary.ShouldAnimate(false, ActionId.Walk, false), "断线状态仍在运行动画。 ");
        // Unknown 即使已连接也只能显示等待识别。
        Assert(!ActionPoseLibrary.ShouldAnimate(true, ActionId.Unknown, false), "Unknown 状态仍在运行动画。 ");
        // 减少动画时保留代表姿态但不得运行计时器。
        Assert(!ActionPoseLibrary.ShouldAnimate(true, ActionId.Walk, true), "减少动画状态仍在运行动画。 ");
        // 非法协议动作不得启动循环。
        Assert(!ActionPoseLibrary.ShouldAnimate(true, (ActionId)200, false), "非法 action_id 启动了动画。 ");
    }

    // 验证系统辅助功能优先级和事件去重。
    private static void TestAnimationPreferencePrecedence()
    {
        // 模拟 Windows 已关闭客户端动画。
        AnimationPreferences systemReduced = new(true);
        // 即使用户没有勾选，有效状态仍必须减少动画。
        Assert(systemReduced.IsReducedMotionEnabled && !systemReduced.UserRequestedReducedMotion, "Windows 减少动画没有优先生效。 ");
        // 统计用户设置变化事件。
        int changeCount = 0;
        // 订阅事件并累计。
        systemReduced.Changed += (_, _) => changeCount++;
        // 第一次设为 true 必须触发事件。
        systemReduced.UserRequestedReducedMotion = true;
        // 重复设为 true 不得触发第二次事件。
        systemReduced.UserRequestedReducedMotion = true;
        // 改回 false 必须再次触发，但系统约束仍保持有效减少动画。
        systemReduced.UserRequestedReducedMotion = false;
        // 核对事件去重和系统优先级。
        Assert(changeCount == 2 && systemReduced.IsReducedMotionEnabled, "减少动画事件去重或系统优先级错误。 ");
        // 模拟 Windows 允许动画。
        AnimationPreferences userControlled = new(false);
        // 默认应允许运行动画。
        Assert(!userControlled.IsReducedMotionEnabled, "Windows 允许动画时默认仍被禁用。 ");
        // 用户勾选后必须立即生效。
        userControlled.UserRequestedReducedMotion = true;
        // 核对应用内开关。
        Assert(userControlled.IsReducedMotionEnabled, "用户减少动画开关未生效。 ");
    }

    // 比较两帧全部 13 个关节点。
    private static bool PosesNear(StickFigurePose left, StickFigurePose right, double tolerance)
    {
        // 遍历固定关节点枚举，任何一点超差即返回 false。
        foreach (FigureJoint joint in Enum.GetValues<FigureJoint>())
        {
            // 读取左侧姿态关节点。
            PosePoint leftPoint = left.GetPoint(joint);
            // 读取右侧姿态关节点。
            PosePoint rightPoint = right.GetPoint(joint);
            // 横纵坐标任一误差超过阈值表示姿态不同。
            if ((Math.Abs(leftPoint.X - rightPoint.X) > tolerance)
                || (Math.Abs(leftPoint.Y - rightPoint.Y) > tolerance))
            {
                // 提前返回 false。
                return false;
            }
        }

        // 全部关节点均在误差内。
        return true;
    }

    // 检查姿态内没有 NaN 或无穷坐标。
    private static bool PoseIsFinite(StickFigurePose pose)
    {
        // 遍历全部关节点。
        foreach (FigureJoint joint in Enum.GetValues<FigureJoint>())
        {
            // 读取当前关节点。
            PosePoint point = pose.GetPoint(joint);
            // 任一坐标非有限立即失败。
            if (!double.IsFinite(point.X) || !double.IsFinite(point.Y))
            {
                // 返回 false 表示姿态不可绘制。
                return false;
            }
        }

        // 所有坐标均有限。
        return true;
    }

    // 统一测试断言，失败时抛出含业务原因的异常。
    private static void Assert(bool condition, string message)
    {
        // 条件成立时当前检查通过。
        if (condition)
        {
            // 继续下一项检查。
            return;
        }

        // 条件失败时由现有控制台测试入口转换为非零退出码。
        throw new InvalidOperationException(message);
    }
}
