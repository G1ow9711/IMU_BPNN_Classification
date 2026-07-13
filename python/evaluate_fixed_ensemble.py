"""复现固定双 M0 融合与动作段因果累计，不在测试集搜索任何参数。"""

# 延迟解析类型注解，允许函数签名引用后续定义并减少运行时类型对象开销。
from __future__ import annotations

# argparse 解析数据、工件和输出路径，避免脚本绑定单台电脑的绝对目录。
import argparse
# json 读取冻结验证角色并输出可审计指标。
import json
# Path 统一处理 Windows 与其他平台路径。
from pathlib import Path
# sys 把仓库根目录加入模块搜索路径，使直接脚本运行与模块运行行为一致。
import sys
# 类型标注明确每个函数的输入和输出合同。
from typing import Dict, List, Sequence, Tuple

# NumPy 负责 297 维标准化、批量数组和逐类统计。
import numpy as np
# PyTorch 加载两个冻结 M0 并输出 softmax 前无量纲 logits。
import torch
# sklearn 按固定 11 类顺序计算准确率、宏 F1 和混淆矩阵。
from sklearn.metrics import accuracy_score, confusion_matrix, f1_score

# 当前文件上一级的上一级是仓库根目录。
PROJECT_ROOT = Path(__file__).resolve().parents[1]
# 直接执行 python/evaluate_fixed_ensemble.py 时，默认搜索路径缺少仓库根目录。
if str(PROJECT_ROOT) not in sys.path:
    # 把仓库根放在最前，确保导入当前工作树而不是其他同名安装包。
    sys.path.insert(0, str(PROJECT_ROOT))
# 复用训练端的数据清洗、特征提取、文件划分和模型定义，保证 Python/ESP32 合同一致。
from python import train_export as training

# 三个目标弱类的窗口级召回下限均为 85%。
TARGET_CLASS_NAMES = ("jumping_squat", "squat", "tuck_jump")
# 前向批大小只影响 CPU 内存和速度，不改变模型结果。
INFERENCE_BATCH_SIZE = 512


def parse_args() -> argparse.Namespace:
    """解析固定评估所需路径；所有模型权重和决策参数均由源码常量锁定。"""
    # parser 描述强调该工具用于确认，不允许测试集调参。
    parser = argparse.ArgumentParser(
        description="固定 0.85/0.15 双 M0 融合与动作段因果累计评估",
    )
    # 基础数据目录包含原始 11 类动作文件，通道顺序必须为 gx、gy、gz、ax、ay、az。
    parser.add_argument("--dataset-dir", type=Path, required=True)
    # 补充训练目录只用于复现文件角色，不能进入验证或测试。
    parser.add_argument("--extra-train-dir", type=Path, required=True)
    # 外部留出目录可省略；提供时沿用基础类别映射执行独立会话确认。
    parser.add_argument("--external-holdout-dir", type=Path)
    # 基础工件目录应包含 Round29 的 validation_report.json、scaler_and_config.npz 和 best_model.pt。
    parser.add_argument("--base-artifact-dir", type=Path, required=True)
    # 掩码工件目录应包含 Round37 同名文件并声明 suppress_normalized_phase=True。
    parser.add_argument("--masked-artifact-dir", type=Path, required=True)
    # 输出 JSON 保存文件角色、固定参数、逐类召回、支持数和混淆矩阵。
    parser.add_argument("--output", type=Path, required=True)
    # 返回解析结果供主流程使用。
    return parser.parse_args()


def batched_logits(model: torch.nn.Module, features: np.ndarray) -> np.ndarray:
    """输入 [样本数,297] 无量纲特征，返回 [样本数,类别数] float32 logits。"""
    # chunks 保存每批 CPU 输出，避免一次性前向占用过多内存。
    chunks: List[np.ndarray] = []
    # 评估模式关闭 dropout，使相同工件和输入得到确定性结果。
    model.eval()
    # 禁用梯度图，评估阶段不进行反向传播。
    with torch.no_grad():
        # 每轮处理不超过 512 个窗口，直到覆盖全部样本。
        for start in range(0, len(features), INFERENCE_BATCH_SIZE):
            # batch 形状为 [当前批样本数,297]，元素为 float32 无量纲标准分。
            batch = torch.from_numpy(
                features[start : start + INFERENCE_BATCH_SIZE],
            ).float()
            # 当前模型输出形状为 [当前批样本数,类别数]，转回 CPU NumPy。
            output = model(batch).cpu().numpy().astype(np.float32)
            # 保存当前批次，最终按原时间顺序拼接。
            chunks.append(output)
    # 空评估集无法形成分类指标，立即拒绝并提示数据角色异常。
    if not chunks:
        # ValueError 由调用方记录为数据或分割配置错误。
        raise ValueError("Evaluation records produced no windows")
    # 沿样本维拼接，输出类别顺序与冻结 CLASS_NAMES 一致。
    return np.vstack(chunks)


def cumulative_bout_predictions(logits: np.ndarray, file_ids: np.ndarray) -> np.ndarray:
    """每个文件视为活动门控划定的独立动作段，只累计当前与历史 logits。"""
    # logits 必须为 [窗口数,类别数]，file_ids 必须为 [窗口数]。
    if logits.ndim != 2 or file_ids.shape != (len(logits),):
        # 形状不一致会破坏时间顺序和状态重置边界。
        raise ValueError("logits/file_ids shape contract violated")
    # predictions 保存每个窗口的全局类别索引。
    predictions = np.empty(len(logits), dtype=np.int64)
    # 逐文件处理；数据构建器保证同文件窗口按时间递增。
    for file_id in np.unique(file_ids):
        # 每个动作段创建独立累计器，相当于活动开始时显式 reset。
        accumulator = training.CausalBoutLogitAccumulator(logits.shape[1])
        # 依次遍历当前文件的全部窗口，不读取任何未来窗口。
        for index in np.flatnonzero(file_ids == file_id):
            # 累计当前融合 logits 并选择截至当前的最大平均类别。
            predictions[index] = int(np.argmax(accumulator.update(logits[index])))
    # 返回与输入窗口一一对应的因果预测。
    return predictions


def classification_metrics(
    labels: np.ndarray,
    predictions: np.ndarray,
    class_names: Sequence[str],
) -> Dict[str, object]:
    """按固定类别顺序返回总体指标、逐类召回、支持数和混淆矩阵。"""
    # matrix 形状为 [类别数,类别数]，行是真实类，列是预测类。
    matrix = confusion_matrix(
        labels,
        predictions,
        labels=np.arange(len(class_names)),
    )
    # support 是每个真实类别的窗口数，单位为窗口。
    support = matrix.sum(axis=1)
    # 仅对支持数大于零的类别计算召回，缺失类保持 NaN 且不写入映射。
    recalls = np.divide(
        np.diag(matrix),
        support,
        out=np.full(len(class_names), np.nan, dtype=np.float64),
        where=support > 0,
    )
    # 返回 JSON 可序列化结构，所有浮点值显式转换为 Python float。
    return {
        "accuracy": float(accuracy_score(labels, predictions)),
        "macro_f1_present_classes": float(
            f1_score(labels, predictions, average="macro", zero_division=0),
        ),
        "class_recalls": {
            name: float(recalls[index])
            for index, name in enumerate(class_names)
            if support[index] > 0
        },
        "class_support": {
            name: int(support[index])
            for index, name in enumerate(class_names)
            if support[index] > 0
        },
        "confusion_matrix": matrix.tolist(),
    }


def load_artifact(
    artifact_dir: Path,
    class_count: int,
) -> Tuple[torch.nn.Module, np.ndarray, np.ndarray, bool]:
    """加载一个 297 维六分支 M0、训练集标准化参数和阶段掩码标志。"""
    # scaler 只允许无对象数组，避免加载不可信 pickle。
    scaler = np.load(artifact_dir / "scaler_and_config.npz", allow_pickle=False)
    # mean 形状为 [297]，单位与各原始特征一致。
    mean = np.asarray(scaler["mean"], dtype=np.float32)
    # std 形状为 [297]，小标准差已在训练端钳制，避免标准化除零。
    std = np.asarray(scaler["std"], dtype=np.float32)
    # 均值与标准差形状必须一致，且标准差均为有限正数。
    if mean.shape != std.shape or mean.ndim != 1 or not np.all(np.isfinite(std)) or np.any(std <= 0.0):
        # 工件合同异常时停止，禁止产生看似正常但无效的指标。
        raise ValueError(f"Invalid scaler in {artifact_dir}")
    # suppress_normalized_phase 缺失时按 false 处理，兼容未掩码基础工件。
    suppress = bool(
        np.asarray(scaler.get("suppress_normalized_phase", False)).reshape(-1)[0],
    )
    # 两个最终工件均使用相同 297 维六分支 M0 架构。
    model = training.MultiBranchBPNet(len(mean), class_count, dropout=0.0)
    # weights_only 禁止反序列化任意 Python 对象，只读取状态张量。
    state = torch.load(
        artifact_dir / "best_model.pt",
        map_location="cpu",
        weights_only=True,
    )
    # 严格加载保证层名、形状和类别数与当前代码一致。
    model.load_state_dict(state, strict=True)
    # 返回冻结模型、独立训练统计和掩码标志。
    return model, mean, std, suppress


def evaluate_records(
    records: Sequence[training.ImuRecord],
    experiment: Dict[str, object],
    base_artifact: Tuple[torch.nn.Module, np.ndarray, np.ndarray, bool],
    masked_artifact: Tuple[torch.nn.Module, np.ndarray, np.ndarray, bool],
    class_names: Sequence[str],
    progress_label: str,
) -> Dict[str, object]:
    """对固定文件角色执行清洗、297 维特征、双 M0 融合和动作段累计。"""
    # build_samples 返回 raw_x=[窗口数,297]、labels=[窗口数]、file_ids=[窗口数]。
    raw_x, labels, file_ids, sample_stats = training.build_samples(
        records,
        int(experiment["window_len"]),
        int(experiment["step_len"]),
        float(experiment["rest_threshold"]),
        float(experiment["active_point_threshold"]),
        augment=False,
        rng=np.random.default_rng(training.SEED),
        progress_label=progress_label,
    )
    # 解包基础模型及其训练集均值、标准差；基础工件不能启用阶段掩码。
    base_model, base_mean, base_std, base_suppress = base_artifact
    # 解包掩码模型及其独立训练统计；掩码工件必须启用阶段抑制。
    masked_model, masked_mean, masked_std, masked_suppress = masked_artifact
    # 两个工件的掩码角色必须固定，防止错误目录交换或工件污染。
    if base_suppress or not masked_suppress:
        # 错误角色会改变模型输入分布，因此不能继续评估。
        raise ValueError("Expected unmasked base artifact and masked secondary artifact")
    # 基础输入按 Round29 训练统计做 z=(x-mean)/std 标准化。
    base_features = ((raw_x - base_mean) / base_std).astype(np.float32)
    # 第二模型先按 Round37 训练统计标准化。
    masked_features = ((raw_x - masked_mean) / masked_std).astype(np.float32)
    # 再把索引 184:232 固定为零标准分，即对应训练均值。
    masked_features = training.apply_model_feature_mask(masked_features, True)
    # 两个模型分别输出 [窗口数,类别数] 无量纲 logits。
    base_logits = batched_logits(base_model, base_features)
    # masked_logits 使用完全相同类别顺序。
    masked_logits = batched_logits(masked_model, masked_features)
    # 使用验证集锁定的 0.85/0.15 权重逐窗口融合。
    combined_logits = training.combine_ensemble_logits(base_logits, masked_logits)
    # 单窗口预测作为对照，不参与最终动作段决策。
    single_predictions = np.argmax(combined_logits, axis=1)
    # 最终预测从每个活动段起点累计当前和全部历史窗口证据。
    bout_predictions = cumulative_bout_predictions(combined_logits, file_ids)
    # 返回文件/窗口规模、数据过滤统计和两种决策指标。
    return {
        "file_count": len(records),
        "window_count": len(labels),
        "sample_stats": sample_stats,
        "single_window": classification_metrics(labels, single_predictions, class_names),
        "cumulative_bout": classification_metrics(labels, bout_predictions, class_names),
    }


def main() -> None:
    """复现冻结文件角色并生成基础测试与可选外部留出确认报告。"""
    # 解析命令行，不提供任何可调融合权重或决策模式参数。
    args = parse_args()
    # 基础验证报告保存冻结类别顺序、窗口长度、阈值和验证文件集合。
    validation_report = json.loads(
        (args.base_artifact_dir / "validation_report.json").read_text(encoding="utf-8"),
    )
    # 最终 Round29 只训练一个 2.5 秒窗口实验，取第零项复现。
    experiment = validation_report["all_experiments"][0]
    # class_names 是两个模型和 ESP32 输出共享的固定类别顺序。
    class_names = list(validation_report["class_names"])
    # 扫描基础数据并建立类别名到索引映射。
    records, scanned_names, label_to_index = training.scan_dataset(args.dataset_dir)
    # 类别顺序漂移会使冻结权重输出错位，必须停止。
    if scanned_names != class_names:
        # 报告明确指出冻结工件与当前数据树不兼容。
        raise ValueError("Class order differs from frozen artifact")
    # 额外会话只追加到训练角色，用于精确复现原分割。
    extra_records = training.scan_labeled_dataset(args.extra_train_dir, label_to_index)
    # 以固定种子重新得到训练、验证和测试文件角色。
    _, val_records, test_records = training.split_records_for_experiment(
        records,
        extra_records,
        training.SEED,
    )
    # expected_val_paths 来自训练时保存的冻结验证文件清单。
    expected_val_paths = {
        str(Path(path).resolve()).lower() for path in experiment["val_files"]
    }
    # actual_val_paths 来自当前数据树和固定分割算法。
    actual_val_paths = {str(record.path.resolve()).lower() for record in val_records}
    # 两者必须完全一致，防止数据树改变后错误读取测试角色。
    if actual_val_paths != expected_val_paths:
        # 任何新增、删除或移动都要求重新训练并重新冻结角色。
        raise ValueError("Frozen validation role no longer matches current split")
    # 加载未掩码 Round29 基础工件。
    base_artifact = load_artifact(args.base_artifact_dir, len(class_names))
    # 加载掩码 Round37 第二工件。
    masked_artifact = load_artifact(args.masked_artifact_dir, len(class_names))
    # 在固定基础测试角色上执行最终确认，不搜索任何参数。
    test_metrics = evaluate_records(
        test_records,
        experiment,
        base_artifact,
        masked_artifact,
        class_names,
        "fixed_ensemble_base_test",
    )
    # 提取三个目标类动作段累计召回。
    target_recalls = {
        name: test_metrics["cumulative_bout"]["class_recalls"][name]
        for name in TARGET_CLASS_NAMES
    }
    # 三类最小召回达到 0.85 才通过用户验收门槛。
    target_gate_passed = bool(min(target_recalls.values()) >= 0.85)
    # result 保存固定参数来源和测试使用边界，便于后续审计。
    result: Dict[str, object] = {
        "original_frozen_test_previously_opened": True,
        "test_used_for_current_weight_selection": False,
        "ensemble_weights_selected_on_validation": {
            "base": training.ENSEMBLE_BASE_LOGIT_WEIGHT,
            "masked": training.ENSEMBLE_MASKED_LOGIT_WEIGHT,
        },
        "decision_mode_selected_on_validation": "causal_cumulative_bout",
        "feature_dim": len(base_artifact[1]),
        "base_test": test_metrics,
        "base_test_target_recalls": target_recalls,
        "base_test_target_gate_passed": target_gate_passed,
    }
    # 用户提供外部留出目录时，沿用同一参数和类别映射评估独立会话。
    if args.external_holdout_dir is not None:
        # external_records 不参与训练、验证或权重选择。
        external_records = training.scan_labeled_dataset(
            args.external_holdout_dir,
            label_to_index,
        )
        # 外部结果写入同一报告，便于比较基础测试和真实新增会话。
        result["external_holdout"] = evaluate_records(
            external_records,
            experiment,
            base_artifact,
            masked_artifact,
            class_names,
            "fixed_ensemble_external_holdout",
        )
    # 创建输出父目录，允许直接写入新的 outputs 子目录。
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # 使用 UTF-8 中文和缩进 JSON 保存完整审计结果。
    args.output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    # 控制台输出最终门槛、三类召回和报告路径，便于 PyCharm/PowerShell 可见确认。
    print(
        f"FIXED_ENSEMBLE gate={target_gate_passed} "
        f"target_recalls={target_recalls} report={args.output}",
        flush=True,
    )


# 作为脚本运行时执行固定评估；作为模块导入时仅暴露可测试函数。
if __name__ == "__main__":
    # 主流程只读取冻结工件，不训练、不调参。
    main()
