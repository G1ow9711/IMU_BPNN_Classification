from __future__ import annotations

"""用冻结验证集比较 robust、不变轴及二者等权融合的三 M0 方案。"""

# argparse 解析三个基础工件、一个历史掩码工件和输出路径。
import argparse
# json 写出选择政策、完整指标和最终结论。
import json
# Path 统一处理 Windows 绝对路径。
from pathlib import Path
# sys 支持脚本直接运行时导入当前工作树。
import sys
# Mapping 描述只读候选摘要。
from typing import Mapping, Sequence

# NumPy 执行标准化、有限值检查和固定权重融合。
import numpy as np

# PROJECT_ROOT 是当前工作树根目录。
PROJECT_ROOT = Path(__file__).resolve().parents[1]
# 直接运行时优先导入当前工作树，而不是其它同名 checkout。
if str(PROJECT_ROOT) not in sys.path:
    # 插入首位保持训练、评估和测试代码版本一致。
    sys.path.insert(0, str(PROJECT_ROOT))

# 复用冻结四组合的角色检查和六条件摘要。
from python import evaluate_validation_candidate_grid as grid
# 复用模型工件加载、批量前向和分类指标。
from python import evaluate_fixed_ensemble as fixed
# 复用六种确定性变换和生产锁类状态机。
from python import evaluate_validation_generalization as generalization
# 复用数据扫描、文件切分和固定双 M0 权重合同。
from python import train_export as training

# 当前已通过外部读取前冻结验证的 robust 双 M0 是本轮基线。
ROBUST_DUAL = "robust_base__historical_masked"
# 不变轴基础单独与历史掩码融合，用于量化结构候选本身。
INVARIANT_DUAL = "invariant_base__historical_masked"
# 三模型候选把 85% 基础份额在 robust 与不变轴之间等分。
TRI_M0 = "robust_invariant_base__historical_masked"
# 历史掩码 M0 权重保持部署合同 15%。
MASKED_WEIGHT = 0.15
# robust 基础占剩余 85% 的一半。
ROBUST_WEIGHT = 0.425
# 不变轴基础占剩余 85% 的另一半。
INVARIANT_WEIGHT = 0.425


def parse_args() -> argparse.Namespace:
    """解析冻结验证输入；命令行不提供外部 CSV 或权重搜索入口。"""
    # parser 明确本脚本只比较三个预声明组合。
    parser = argparse.ArgumentParser(description="冻结验证集三 M0 互补性评估")
    # dataset-dir 指向用户批准的 11 类训练数据树。
    parser.add_argument("--dataset-dir", type=Path, required=True)
    # extra-train-dir 仅用于恢复与历史报告完全相同的文件角色。
    parser.add_argument("--extra-train-dir", type=Path, required=True)
    # robust-base 是上一轮冻结验证胜出的基础 M0。
    parser.add_argument("--robust-base", type=Path, required=True)
    # invariant-base 是首层 0:48 绝对轴权重永久清零的基础 M0。
    parser.add_argument("--invariant-base", type=Path, required=True)
    # historical-masked 是当前稳定的 184:232 掩码 M0。
    parser.add_argument("--historical-masked", type=Path, required=True)
    # output 保存三候选、六条件及最终冻结选择。
    parser.add_argument("--output", type=Path, required=True)
    # 返回解析结果。
    return parser.parse_args()


def combine_three_logits(
    robust_logits: np.ndarray,
    invariant_logits: np.ndarray,
    masked_logits: np.ndarray,
) -> np.ndarray:
    """按 42.5%/42.5%/15% 固定权重融合三个同类别顺序 logits。"""
    # robust 转为 float32，保持与端侧累加精度一致。
    robust = np.asarray(robust_logits, dtype=np.float32)
    # invariant 转为相同精度。
    invariant = np.asarray(invariant_logits, dtype=np.float32)
    # masked 转为相同精度。
    masked = np.asarray(masked_logits, dtype=np.float32)
    # 三个张量必须具有完全相同的 `[窗口数,类别数]` 形状。
    if robust.shape != invariant.shape or robust.shape != masked.shape:
        # 拒绝 NumPy 广播，避免文件或类别错位。
        raise ValueError("Tri-M0 logit shapes differ")
    # 只接受二维非空类别矩阵。
    if robust.ndim != 2 or robust.shape[1] == 0:
        # 报告结构错误而不是产生空指标。
        raise ValueError("Tri-M0 logits must have shape [sample, class]")
    # 任一路 NaN/Inf 都不能进入生产 softmax。
    if not (
        np.all(np.isfinite(robust))
        and np.all(np.isfinite(invariant))
        and np.all(np.isfinite(masked))
    ):
        # 与双 M0 有限值合同保持一致。
        raise ValueError("Tri-M0 logits must be finite")
    # 返回固定凸组合；三个常量之和严格为 1.0。
    return (
        np.float32(ROBUST_WEIGHT) * robust
        + np.float32(INVARIANT_WEIGHT) * invariant
        + np.float32(MASKED_WEIGHT) * masked
    )


def metrics_from_logits(
    logits: np.ndarray,
    labels: np.ndarray,
    file_ids: np.ndarray,
    class_names: Sequence[str],
) -> dict[str, object]:
    """从冻结融合 logits 计算单窗、累计动作段和真实生产锁类指标。"""
    # single_predictions 是每个窗口的瞬时 Top-1，仅用于诊断。
    single_predictions = np.argmax(logits, axis=1)
    # bout_predictions 在每个验证文件起点清零并累计到当前窗。
    bout_predictions = fixed.cumulative_bout_predictions(logits, file_ids)
    # 返回与双 M0 网格完全相同的三层指标结构。
    return {
        # 单窗层衡量瞬时稳定性。
        "single_window": fixed.classification_metrics(
            labels,
            single_predictions,
            class_names,
        ),
        # 累计层衡量完整动作段证据。
        "cumulative_bout": fixed.classification_metrics(
            labels,
            bout_predictions,
            class_names,
        ),
        # 生产层严格复现连续两窗、50% 和第四窗兜底。
        "production_lock": generalization.production_lock_metrics(
            logits,
            labels,
            file_ids,
            class_names,
        ),
    }


def select_tri_candidate(summaries: Mapping[str, Mapping[str, float]]) -> dict:
    """以 robust 双 M0 为新基线执行 clean 双硬门和冻结排序。"""
    # robust 基线必须存在，防止错误地退回更早历史组合。
    if ROBUST_DUAL not in summaries:
        # 缺基线无法判断结构候选是否真实改善。
        raise ValueError("Robust dual baseline is missing")
    # baseline 保存上一轮已冻结的验证指标。
    baseline = summaries[ROBUST_DUAL]
    # epsilon 仅吸收 float64 舍入，不允许真实回退。
    epsilon = 1.0e-12
    # eligible 按输入顺序保存通过 clean 双门的候选；基线固定排第一。
    eligible: list[str] = []
    # rejections 保存每个淘汰候选的明确原因。
    rejections: dict[str, list[str]] = {}
    # 逐候选检查 clean 文件级准确率和最弱类召回。
    for name, summary in summaries.items():
        # reasons 收集当前候选的全部回退。
        reasons: list[str] = []
        # clean 生产锁类准确率不得低于 robust 双 M0。
        if float(summary["clean_accuracy"]) + epsilon < float(
            baseline["clean_accuracy"]
        ):
            # 记录准确率退化。
            reasons.append("clean_accuracy_regressed")
        # clean 最弱类别文件召回不得下降。
        if float(summary["clean_min_class_recall"]) + epsilon < float(
            baseline["clean_min_class_recall"]
        ):
            # 记录最弱类退化。
            reasons.append("clean_min_class_recall_regressed")
        # 有原因则淘汰，否则进入固定排序。
        if reasons:
            # 保存全部原因。
            rejections[name] = reasons
        else:
            # 保留合格候选。
            eligible.append(name)
    # robust 基线自身理论上必定合格。
    if not eligible:
        # 防御损坏的摘要输入。
        raise ValueError("No Tri-M0 candidate passed clean gates")

    # rank_key 返回越大越优的五元组。
    def rank_key(name: str) -> tuple[float, float, float, float, float]:
        # summary 是当前候选的六条件摘要。
        summary = summaries[name]
        # 最弱类、最差整体、均值优先；强制锁和等待窗数越少越好。
        return (
            float(summary["worst_transform_min_class_recall"]),
            float(summary["worst_transform_accuracy"]),
            float(summary["mean_transform_accuracy"]),
            -float(summary["forced_lock_total"]),
            -float(summary["mean_windows_used"]),
        )

    # 完全同分时 Python max 保留首项，即继续使用 robust 双 M0。
    winner = max(eligible, key=rank_key)
    # 返回全部可审计选择事实。
    return {
        "baseline": ROBUST_DUAL,
        "eligible": eligible,
        "rejections": rejections,
        "winner": winner,
        "winner_rank_key": list(rank_key(winner)),
    }


def main() -> None:
    """一次构建六条件特征，比较 robust、不变轴和固定三模型融合。"""
    # args 保存全部显式路径。
    args = parse_args()
    # role_dirs 复用四组合工具核对类别、seed 和验证文件逐项顺序。
    role_dirs = {
        grid.BASELINE_CANDIDATE: (args.robust_base, args.historical_masked),
        INVARIANT_DUAL: (args.invariant_base, args.historical_masked),
    }
    # baseline_report 提供冻结文件角色和窗口合同。
    baseline_report, class_names = grid.validate_artifact_roles(role_dirs)
    # experiment 固定验证文件、窗口长度和活动阈值。
    experiment = dict(baseline_report["all_experiments"][0])
    # 扫描用户批准的数据树。
    records, scanned_names, label_to_index = training.scan_dataset(args.dataset_dir)
    # 数据类别顺序必须与三个模型完全一致。
    if scanned_names != class_names:
        # 拒绝类别索引漂移。
        raise ValueError("Dataset class order differs from Tri-M0 artifacts")
    # 额外弱类文件只用于恢复训练角色，不进入验证。
    extra_records = training.scan_labeled_dataset(args.extra_train_dir, label_to_index)
    # 使用冻结 seed 恢复同一 140/28/29 文件切分。
    _, val_records, _ = training.split_records_for_experiment(
        records,
        extra_records,
        int(baseline_report.get("seed", training.SEED)),
    )
    # actual_paths 保存当前数据树恢复的验证文件顺序。
    actual_paths = [str(record.path.resolve()).lower() for record in val_records]
    # expected_paths 来自冻结验证报告。
    expected_paths = [
        str(Path(value).resolve()).lower() for value in experiment["val_files"]
    ]
    # 文件角色必须逐项一致。
    if actual_paths != expected_paths:
        # 禁止自动搜索替代文件或重新切分。
        raise ValueError("Frozen validation role no longer matches dataset split")
    # robust_artifact 加载上一轮增强基础 M0 及其独立标准化参数。
    robust_artifact = fixed.load_artifact(args.robust_base, len(class_names))
    # invariant_artifact 加载首层绝对轴剪枝 M0。
    invariant_artifact = fixed.load_artifact(args.invariant_base, len(class_names))
    # masked_artifact 加载历史 184:232 输入掩码 M0。
    masked_artifact = fixed.load_artifact(args.historical_masked, len(class_names))
    # 三路基础角色标志必须为未归一化阶段掩码、未掩码、已掩码。
    if robust_artifact[3] or invariant_artifact[3] or not masked_artifact[3]:
        # 目录或工件标志错误会改变融合语义。
        raise ValueError("Tri-M0 artifact roles are invalid")
    # candidate_results 保存三个预声明候选的六条件完整指标。
    candidate_results: dict[str, dict[str, object]] = {
        ROBUST_DUAL: {},
        TRI_M0: {},
        INVARIANT_DUAL: {},
    }
    # 每个物理变换只构建一次窗口特征。
    for transform_name in generalization.TRANSFORM_NAMES:
        # raw_x 形状 `[窗口数,297]`，labels/file_ids 与其首维严格对齐。
        raw_x, labels, file_ids, sample_stats = (
            generalization.build_transformed_validation_features(
                val_records,
                experiment,
                transform_name,
            )
        )
        # 解包 robust 模型和标准化参数。
        robust_model, robust_mean, robust_std, _ = robust_artifact
        # robust_x 使用其训练集统计转换为无量纲输入。
        robust_x = ((raw_x - robust_mean) / robust_std).astype(np.float32)
        # robust_logits 形状 `[窗口数,11]`。
        robust_logits = fixed.batched_logits(robust_model, robust_x)
        # 解包不变轴模型和独立标准化参数。
        invariant_model, invariant_mean, invariant_std, _ = invariant_artifact
        # 首层 0:48 权重已永久清零，因此无需运行时输入分支。
        invariant_x = ((raw_x - invariant_mean) / invariant_std).astype(np.float32)
        # invariant_logits 使用相同类别顺序。
        invariant_logits = fixed.batched_logits(invariant_model, invariant_x)
        # 解包历史掩码模型和标准化参数。
        masked_model, masked_mean, masked_std, _ = masked_artifact
        # masked_x 先按自身训练统计标准化。
        masked_x = ((raw_x - masked_mean) / masked_std).astype(np.float32)
        # 历史掩码模型固定把 184:232 替换为训练均值零分。
        masked_x = training.apply_model_feature_mask(masked_x, True)
        # masked_logits 形状与两路基础 logits 完全一致。
        masked_logits = fixed.batched_logits(masked_model, masked_x)
        # robust_dual 复现当前冻结胜者 85%/15%。
        robust_dual = training.combine_ensemble_logits(robust_logits, masked_logits)
        # invariant_dual 单独量化结构候选 85%/15%。
        invariant_dual = training.combine_ensemble_logits(
            invariant_logits,
            masked_logits,
        )
        # tri_logits 使用固定 42.5%/42.5%/15%，不搜索权重。
        tri_logits = combine_three_logits(
            robust_logits,
            invariant_logits,
            masked_logits,
        )
        # candidate_logits 按稳定顺序保存三组融合输出。
        candidate_logits = {
            ROBUST_DUAL: robust_dual,
            TRI_M0: tri_logits,
            INVARIANT_DUAL: invariant_dual,
        }
        # 逐候选计算同一标签和文件角色上的三层指标。
        for candidate_name, logits in candidate_logits.items():
            # metrics 包含单窗、累计和生产锁类。
            metrics = metrics_from_logits(logits, labels, file_ids, class_names)
            # 保存变换规模和完整指标。
            candidate_results[candidate_name][transform_name] = {
                "sample_stats": sample_stats,
                **metrics,
            }
            # 输出短进度，长时间特征计算不会被误判为卡住。
            print(
                f"TRI_M0 candidate={candidate_name} transform={transform_name} "
                f"lock_acc={metrics['production_lock']['accuracy']:.6f} "
                f"forced={metrics['production_lock']['forced_lock_count']}",
                flush=True,
            )
    # summaries 复用双 M0 六条件压缩口径。
    summaries = {
        name: grid.summarize_candidate(results)
        for name, results in candidate_results.items()
    }
    # selection 以 robust 双 M0 为基线执行双硬门和固定排序。
    selection = select_tri_candidate(summaries)
    # output 保存政策、权重、目录、完整指标和结论。
    output = {
        "selection_policy": {
            "baseline": ROBUST_DUAL,
            "clean_accuracy_must_not_regress": True,
            "clean_min_class_recall_must_not_regress": True,
            "fixed_weights": {
                "robust_base": ROBUST_WEIGHT,
                "invariant_base": INVARIANT_WEIGHT,
                "historical_masked": MASKED_WEIGHT,
            },
            "external_csv_used": False,
        },
        "artifact_directories": {
            "robust_base": str(args.robust_base.resolve()),
            "invariant_base": str(args.invariant_base.resolve()),
            "historical_masked": str(args.historical_masked.resolve()),
        },
        "summaries": summaries,
        "selection": selection,
        "candidates": candidate_results,
    }
    # 创建项目本地输出父目录。
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # UTF-8 写出完整可审计 JSON。
    args.output.write_text(
        json.dumps(output, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    # 输出冻结胜者和报告路径。
    print(
        f"TRI_M0_OK winner={selection['winner']} output={args.output.resolve()}",
        flush=True,
    )


# 直接运行时执行评估；被测试导入时不扫描数据或加载模型。
if __name__ == "__main__":
    # 调用主流程。
    main()
