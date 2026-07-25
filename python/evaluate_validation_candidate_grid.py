from __future__ import annotations

"""一次构建冻结验证特征，比较历史/鲁棒基础与掩码 M0 的四种组合。"""

# argparse 解析数据、四个候选工件和输出路径。
import argparse
# json 读取冻结角色并写出完整指标。
import json
# Path 统一处理 Windows 绝对路径。
from pathlib import Path
# sys 支持直接运行时导入当前工作树包。
import sys
# Mapping 描述只读候选和指标映射。
from typing import Mapping

# NumPy 计算六条件均值、最小值和有限值检查。
import numpy as np

# 当前文件上一级的上一级是仓库根目录。
PROJECT_ROOT = Path(__file__).resolve().parents[1]
# 直接运行时优先加载当前工作树代码。
if str(PROJECT_ROOT) not in sys.path:
    # 插入首位，避免加载其它工作树的同名模块。
    sys.path.insert(0, str(PROJECT_ROOT))

# 复用训练端数据扫描和冻结文件切分。
from python import train_export as training
# 复用双 M0 工件加载器。
from python import evaluate_fixed_ensemble as fixed
# 复用六种确定性扰动、297维特征和生产锁类指标。
from python import evaluate_validation_generalization as generalization

# 历史双 M0 是当前部署基线；完全同分时保留它。
BASELINE_CANDIDATE = "historical_base__historical_masked"


def parse_args() -> argparse.Namespace:
    """解析四个预声明候选；不提供外部 CSV 或阈值搜索入口。"""
    # parser 明确本工具只做冻结验证网格。
    parser = argparse.ArgumentParser(description="冻结验证集四组合双 M0 泛化选模")
    # dataset-dir 指向用户批准的 11 类数据集。
    parser.add_argument("--dataset-dir", type=Path, required=True)
    # extra-train-dir 仅恢复 140/28/29 文件角色。
    parser.add_argument("--extra-train-dir", type=Path, required=True)
    # historical-base 指向当前基础 M0 验证工件。
    parser.add_argument("--historical-base", type=Path, required=True)
    # historical-masked 指向当前掩码 M0 验证工件。
    parser.add_argument("--historical-masked", type=Path, required=True)
    # robust-base 指向新增强基础 M0 验证工件。
    parser.add_argument("--robust-base", type=Path, required=True)
    # robust-masked 指向新增强掩码 M0 验证工件。
    parser.add_argument("--robust-masked", type=Path, required=True)
    # output 保存全部六条件指标和冻结选择结论。
    parser.add_argument("--output", type=Path, required=True)
    # 返回命令行命名空间。
    return parser.parse_args()


def candidate_directories(args: argparse.Namespace) -> dict[str, tuple[Path, Path]]:
    """按稳定顺序生成历史/鲁棒基础与掩码的四个组合。"""
    # 字典插入顺序同时定义完全同分时的稳定优先级。
    return {
        BASELINE_CANDIDATE: (args.historical_base, args.historical_masked),
        "robust_base__historical_masked": (args.robust_base, args.historical_masked),
        "historical_base__robust_masked": (args.historical_base, args.robust_masked),
        "robust_base__robust_masked": (args.robust_base, args.robust_masked),
    }


def validate_artifact_roles(
    directories: Mapping[str, tuple[Path, Path]],
) -> tuple[dict, list[str]]:
    """核对全部工件共享类别、seed 和逐项验证文件顺序。"""
    # baseline_dir 是历史组合的基础 M0 目录。
    baseline_dir = directories[BASELINE_CANDIDATE][0]
    # baseline_report 提供唯一冻结实验合同。
    baseline_report = json.loads(
        (baseline_dir / "validation_report.json").read_text(encoding="utf-8")
    )
    # baseline_experiment 固定验证文件、窗口和活动阈值。
    baseline_experiment = baseline_report["all_experiments"][0]
    # class_names 固定 11 类输出顺序。
    class_names = list(baseline_report["class_names"])
    # baseline_paths 规范化逐项验证文件路径。
    baseline_paths = [
        str(Path(value).resolve()).lower() for value in baseline_experiment["val_files"]
    ]
    # baseline_seed 冻结文件切分随机种子。
    baseline_seed = int(baseline_report.get("seed", training.SEED))
    # visited 防止同一历史或鲁棒目录重复读取。
    visited: set[str] = set()
    # 遍历四组合中的两个角色目录。
    for base_dir, masked_dir in directories.values():
        # 基础和掩码都必须独立核对。
        for artifact_dir in (base_dir, masked_dir):
            # key 规范化目录身份。
            key = str(artifact_dir.resolve()).lower()
            # 已核对目录无需重复读取。
            if key in visited:
                # 继续下一个目录。
                continue
            # 标记已处理。
            visited.add(key)
            # report 读取当前候选训练与验证事实。
            report = json.loads(
                (artifact_dir / "validation_report.json").read_text(encoding="utf-8")
            )
            # 类别顺序不同会造成融合 logits 错位。
            if list(report["class_names"]) != class_names:
                # 拒绝错误工件。
                raise ValueError(f"Class order differs: {artifact_dir}")
            # seed 不同意味着文件角色不同。
            if int(report.get("seed", training.SEED)) != baseline_seed:
                # 拒绝不同切分。
                raise ValueError(f"Seed differs: {artifact_dir}")
            # current_paths 读取当前工件验证文件顺序。
            current_paths = [
                str(Path(value).resolve()).lower()
                for value in report["all_experiments"][0]["val_files"]
            ]
            # 必须逐项一致，集合相同但顺序不同也不允许。
            if current_paths != baseline_paths:
                # 拒绝角色漂移。
                raise ValueError(f"Validation roles differ: {artifact_dir}")
    # 返回基线报告和类别顺序供主流程复用。
    return baseline_report, class_names


def summarize_candidate(results: Mapping[str, Mapping[str, object]]) -> dict:
    """把六种变换压缩为预声明选模指标。"""
    # locks 按固定变换顺序读取生产锁类指标。
    locks = [
        dict(results[name]["production_lock"])
        for name in generalization.TRANSFORM_NAMES
    ]
    # accuracies 保存六条件文件级锁类准确率。
    accuracies = np.asarray(
        [float(item["accuracy"]) for item in locks],
        dtype=np.float64,
    )
    # transform_min_classes 保存每个条件的最弱类别文件召回。
    transform_min_classes = np.asarray(
        [
            min(float(value) for value in dict(item["class_recalls"]).values())
            for item in locks
        ],
        dtype=np.float64,
    )
    # clean_lock 固定为首个原始验证条件。
    clean_lock = locks[0]
    # clean_min_class 保护原始验证最弱类别不退化。
    clean_min_class = min(
        float(value) for value in dict(clean_lock["class_recalls"]).values()
    )
    # forced_total 汇总六条件第四窗低置信兜底数。
    forced_total = int(sum(int(item["forced_lock_count"]) for item in locks))
    # mean_windows 汇总首次主动作确认延迟。
    mean_windows = float(
        np.mean([float(item["mean_windows_used"]) for item in locks])
    )
    # finite_values 收集所有参与硬门或排序的浮点数。
    finite_values = np.concatenate(
        [
            accuracies,
            transform_min_classes,
            np.asarray([clean_min_class, mean_windows], dtype=np.float64),
        ]
    )
    # 非有限值会使排序不可靠，必须停止。
    if not np.all(np.isfinite(finite_values)):
        # 抛出明确工件错误。
        raise ValueError("Candidate summary contains non-finite metrics")
    # 返回 JSON 可序列化摘要。
    return {
        "clean_accuracy": float(accuracies[0]),
        "clean_min_class_recall": float(clean_min_class),
        "worst_transform_min_class_recall": float(np.min(transform_min_classes)),
        "worst_transform_accuracy": float(np.min(accuracies)),
        "mean_transform_accuracy": float(np.mean(accuracies)),
        "forced_lock_total": forced_total,
        "mean_windows_used": mean_windows,
    }


def select_candidate(summaries: Mapping[str, Mapping[str, float]]) -> dict:
    """先执行 clean 双硬门，再按最弱类、最差整体、均值和实时性排序。"""
    # 历史组合必须存在，作为部署基线。
    if BASELINE_CANDIDATE not in summaries:
        # 缺基线时不能做相对选模。
        raise ValueError("Historical baseline candidate is missing")
    # baseline 保存当前部署 clean 指标。
    baseline = summaries[BASELINE_CANDIDATE]
    # epsilon 只吸收 float64 舍入，不允许真实回退。
    epsilon = 1.0e-12
    # eligible 保存通过 clean 准确率和最弱类召回的候选。
    eligible: list[str] = []
    # rejections 保存每个淘汰候选的硬门原因。
    rejections: dict[str, list[str]] = {}
    # 按稳定输入顺序逐候选检查。
    for name, summary in summaries.items():
        # reasons 收集当前候选全部回退。
        reasons: list[str] = []
        # clean 文件级锁类准确率不得下降。
        if float(summary["clean_accuracy"]) + epsilon < float(baseline["clean_accuracy"]):
            # 记录准确率回退。
            reasons.append("clean_accuracy_regressed")
        # clean 最弱类别文件召回不得下降。
        if float(summary["clean_min_class_recall"]) + epsilon < float(
            baseline["clean_min_class_recall"]
        ):
            # 记录最弱类回退。
            reasons.append("clean_min_class_recall_regressed")
        # 有原因则淘汰，否则进入排序。
        if reasons:
            # 保存原因列表。
            rejections[name] = reasons
        else:
            # 保存合格候选。
            eligible.append(name)
    # 历史组合自身理论上必定通过。
    if not eligible:
        # 防御损坏摘要。
        raise ValueError("No candidate passed clean validation gates")

    # rank_key 返回越大越优的固定五元组。
    def rank_key(name: str) -> tuple[float, float, float, float, float]:
        # summary 是当前候选指标。
        summary = summaries[name]
        # 最弱类优先；强制锁和窗数取负使更少者更优。
        return (
            float(summary["worst_transform_min_class_recall"]),
            float(summary["worst_transform_accuracy"]),
            float(summary["mean_transform_accuracy"]),
            -float(summary["forced_lock_total"]),
            -float(summary["mean_windows_used"]),
        )

    # Python max 完全同分时保留列表首项，历史组合固定最先插入。
    winner = max(eligible, key=rank_key)
    # 返回合格集、淘汰原因、胜者和排序键。
    return {
        "baseline": BASELINE_CANDIDATE,
        "eligible": eligible,
        "rejections": rejections,
        "winner": winner,
        "winner_rank_key": list(rank_key(winner)),
    }


def main() -> None:
    """一次构建六套验证特征，依次前向四组合并冻结胜者。"""
    # args 保存全部显式输入输出。
    args = parse_args()
    # directories 固定四组合及完全同分优先级。
    directories = candidate_directories(args)
    # 核对全部工件角色并读取基线合同。
    baseline_report, class_names = validate_artifact_roles(directories)
    # experiment 保存窗口、活动过滤和验证文件路径。
    experiment = dict(baseline_report["all_experiments"][0])
    # 扫描用户批准的数据集。
    records, scanned_names, label_to_index = training.scan_dataset(args.dataset_dir)
    # 数据类别顺序必须与模型一致。
    if scanned_names != class_names:
        # 拒绝数据树漂移。
        raise ValueError("Dataset class order differs from artifacts")
    # 额外弱类文件只恢复训练角色，不进入验证。
    extra_records = training.scan_labeled_dataset(args.extra_train_dir, label_to_index)
    # 使用冻结 seed 恢复同一文件切分。
    _, val_records, _ = training.split_records_for_experiment(
        records,
        extra_records,
        int(baseline_report.get("seed", training.SEED)),
    )
    # actual_paths 是当前数据树恢复的验证顺序。
    actual_paths = [str(record.path.resolve()).lower() for record in val_records]
    # expected_paths 来自历史工件。
    expected_paths = [
        str(Path(value).resolve()).lower() for value in experiment["val_files"]
    ]
    # 文件角色必须逐项一致。
    if actual_paths != expected_paths:
        # 禁止自动搜索同名替代文件。
        raise ValueError("Frozen validation role no longer matches dataset split")
    # artifacts 为每个组合加载自己的 scaler、权重和掩码合同。
    artifacts = {
        name: (
            fixed.load_artifact(base_dir, len(class_names)),
            fixed.load_artifact(masked_dir, len(class_names)),
        )
        for name, (base_dir, masked_dir) in directories.items()
    }
    # candidate_results 保存四组合六条件完整指标。
    candidate_results: dict[str, dict[str, object]] = {
        name: {} for name in directories
    }
    # 每种物理扰动只构建一次特征，再供四组合复用。
    for transform_name in generalization.TRANSFORM_NAMES:
        # 构建冻结窗口、标签、文件编号和样本规模。
        raw_x, labels, file_ids, sample_stats = (
            generalization.build_transformed_validation_features(
                val_records,
                experiment,
                transform_name,
            )
        )
        # 逐组合执行各自标准化和双 M0 前向。
        for name, (base_artifact, masked_artifact) in artifacts.items():
            # metrics 包含单窗、累计动作段和生产锁类三层指标。
            metrics = generalization.evaluate_transform(
                raw_x,
                labels,
                file_ids,
                base_artifact,
                masked_artifact,
                class_names,
            )
            # 保存当前变换规模和完整结果。
            candidate_results[name][transform_name] = {
                "sample_stats": sample_stats,
                **metrics,
            }
            # 输出短进度，长时间特征计算不被误判为卡住。
            print(
                f"CANDIDATE_GRID candidate={name} transform={transform_name} "
                f"lock_acc={metrics['production_lock']['accuracy']:.6f} "
                f"forced={metrics['production_lock']['forced_lock_count']}",
                flush=True,
            )
    # summaries 压缩预声明硬门和排序指标。
    summaries = {
        name: summarize_candidate(results) for name, results in candidate_results.items()
    }
    # selection 执行 clean 双硬门及鲁棒性排序。
    selection = select_candidate(summaries)
    # output 保存政策、目录、摘要、结论和逐文件明细。
    output = {
        "selection_policy": {
            "clean_accuracy_must_not_regress": True,
            "clean_min_class_recall_must_not_regress": True,
            "rank_order": [
                "worst_transform_min_class_recall",
                "worst_transform_accuracy",
                "mean_transform_accuracy",
                "fewer_forced_locks",
                "fewer_mean_windows",
            ],
            "external_csv_used": False,
        },
        "candidate_directories": {
            name: {"base": str(base.resolve()), "masked": str(masked.resolve())}
            for name, (base, masked) in directories.items()
        },
        "summaries": summaries,
        "selection": selection,
        "candidates": candidate_results,
    }
    # 创建项目本地输出父目录。
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # UTF-8 写出可审计 JSON。
    args.output.write_text(
        json.dumps(output, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    # 输出冻结胜者和报告路径。
    print(
        f"CANDIDATE_GRID_OK winner={selection['winner']} output={args.output.resolve()}",
        flush=True,
    )


# 直接运行时执行网格；被测试导入时不扫描数据或加载模型。
if __name__ == "__main__":
    # 调用主流程。
    main()
