// 引入领域动作枚举，动画资源键必须与 11 类顺序一致。
using FitnessCoach.Domain;

// 动作视觉描述位于服务层，不让 ViewModel 硬编码资源表。
namespace FitnessCoach.App.Services;

/// <summary>描述本地动作动画的名称、诊断回退符号和稳定资源键。</summary>
public sealed record ActionVisualDescriptor(ActionId Action, string ChineseName, string Glyph, string ResourceKey);
