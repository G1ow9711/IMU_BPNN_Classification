// 重组返回状态独立定义，使通知处理器无需依赖异常判断正常未完成状态。
namespace FitnessCoach.Bluetooth;

/// <summary>
/// 表示一次分片推入后的重组状态。
/// </summary>
public enum FragmentPushStatus
{
    /// <summary>分片已接收，但完整逻辑帧尚未到齐。</summary>
    AcceptedIncomplete = 0,
    /// <summary>最后一片已到达，完整帧通过长度和 CRC 校验。</summary>
    Completed = 1,
    /// <summary>分片被拒绝，当前重组状态已经清除。</summary>
    Rejected = 2,
}
