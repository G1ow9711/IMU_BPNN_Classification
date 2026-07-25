// 领域模型位于独立命名空间，避免界面层直接依赖 BLE 字节结构。
namespace FitnessCoach.Domain;

/// <summary>
/// 定义模型输出的固定 11 类顺序；数值必须与 Python、ESP32 和动画资源索引一致。
/// </summary>
public enum ActionId : byte
{
    /// <summary>早安式体前屈，对应模型输出索引 0。</summary>
    GoodMorning = 0,
    /// <summary>开合跳，对应模型输出索引 1。</summary>
    JumpingJack = 1,
    /// <summary>跳跃弓步，对应模型输出索引 2。</summary>
    JumpingLunge = 2,
    /// <summary>跳跃深蹲，对应模型输出索引 3。</summary>
    JumpingSquat = 3,
    /// <summary>普通弓步，对应模型输出索引 4。</summary>
    Lunge = 4,
    /// <summary>静坐，对应模型输出索引 5。</summary>
    Sit = 5,
    /// <summary>普通深蹲，对应模型输出索引 6。</summary>
    Squat = 6,
    /// <summary>小跑，对应模型输出索引 7。</summary>
    Trot = 7,
    /// <summary>收腹跳，对应模型输出索引 8。</summary>
    TuckJump = 8,
    /// <summary>行走，对应模型输出索引 9。</summary>
    Walk = 9,
    /// <summary>挥手，对应模型输出索引 10。</summary>
    Wave = 10,
    /// <summary>当前没有稳定动作，线上编码固定为 255。</summary>
    Unknown = byte.MaxValue,
}
