import argparse
import copy
import json
import math
import os
import random
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import torch
from sklearn.metrics import accuracy_score, classification_report, confusion_matrix, f1_score
from sklearn.model_selection import train_test_split
from torch import nn
from torch.nn import functional as F
from torch.utils.data import DataLoader, Sampler, TensorDataset, WeightedRandomSampler


SEED = 20260709
PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DATASET_DIR = Path("IMU_Dataset") / "imu_dataset_for_final"
FALLBACK_DATASET_DIR = Path("imu_dataset_for_final")
OUTPUT_DIR = Path("outputs")
ESP32_MODEL_HEADER = PROJECT_ROOT / "esp32" / "include" / "esp32_bp_model.h"

SAMPLE_RATE = 25
STEP_SECONDS = 0.5
WINDOW_SECONDS_LIST = (1.5, 2.0, 2.5)
WINDOW_SECONDS_CHOICES = WINDOW_SECONDS_LIST + (4.0,)
TRAIN_RATIO = 0.70
VAL_RATIO = 0.15
TEST_RATIO = 0.15
SIT_CLASS_NAME = "sit"
HIGH_DYNAMIC_CLASSES = {"jumping_jack", "jumping_lunge", "jumping_squat", "tuck_jump"}
WEAK_CLASS_NAMES = ["jumping_squat", "squat", "tuck_jump", "jumping_lunge"]
FAMILY_SPECIALIST_CLASS_NAMES = [
    "jumping_jack",
    "jumping_lunge",
    "jumping_squat",
    "tuck_jump",
]
HARD_CONFUSION_PAIRS = {
    # 普通弓步与深蹲仅约束彼此的错误 logit，避免间隔损失干扰其余已稳定类别。
    "lunge": ["squat"],
    # 深蹲反向约束弓步，形成对称类别边界。
    "squat": ["lunge"],
    # 跳跃弓步只压低最相近的跳跃深蹲 logit。
    "jumping_lunge": ["jumping_squat"],
    # 跳跃深蹲同时与跳跃弓步、收腹跳建立局部间隔。
    "jumping_squat": ["jumping_lunge", "tuck_jump"],
    # 收腹跳反向约束跳跃深蹲，保留飞行姿态差异。
    "tuck_jump": ["jumping_squat"],
}
TARGET_MIN_CLASS_RECALL = 0.90
WEAK_TARGET_MIN_CLASS_RECALL = 0.85
RELAXED_RECALL_CLASS_NAMES = {
    "jumping_jack",
    "jumping_lunge",
    "jumping_squat",
    "squat",
    "tuck_jump",
}

MAX_EPOCHS = 350
BATCH_SIZE = 64
LEARNING_RATE = 1e-3
WEIGHT_DECAY = 1e-4
PATIENCE = 45
DROPOUT = 0.10
AUGMENT_TIMES = 2
MAX_ROTATION_DEGREES = 35.0
EXPORT_WHEN_BELOW_TARGET = False
SUPCON_WEIGHT = 0.05
HARD_PAIR_WEIGHT = 0.25
# 五个训练期辅助任务的总损失权重；部署时不导出辅助分类头。
AUXILIARY_WEIGHT = 0.10

CHANNEL_NAMES = ["gx", "gy", "gz", "ax", "ay", "az"]
MAG_NAMES = ["gyro_mag", "acc_mag"]
DELTA_MAG_NAMES = ["gyro_delta_mag", "acc_delta_mag"]
GRAVITY_NAMES = [
    "acc_vertical",
    "acc_horizontal_mag",
    "gyro_vertical",
    "gyro_horizontal_mag",
]
GLOBAL_SERIES_NAMES = CHANNEL_NAMES + MAG_NAMES + DELTA_MAG_NAMES + GRAVITY_NAMES
PHASE_SOURCE_NAMES = ["acc_vertical", "acc_horizontal_mag", "gyro_mag", "acc_delta_mag"]
ONE_SERIES_FEATURES = [
    "mean",
    "std",
    "min",
    "max",
    "rms",
    "mean_abs_diff",
    "zcr",
    "std_diff",
]
PHASE_SEGMENTS = 4
PHASE_FEATURES = ["mean", "std", "max_abs"]
TEMPORAL_FEATURES = [
    "high_activity_ratio",
    "peak_count_normalized",
    "dominant_frequency_hz",
    "spectral_entropy",
    "autocorr_peak",
    "autocorr_peak_lag_seconds",
]
IMPACT_DISTRIBUTION_FEATURES = [
    "q10",
    "q25",
    "q50",
    "q75",
    "q90",
    "skew",
    "excess_kurtosis",
    "max_abs_diff",
]
WEAK_CLASS_FEATURE_NAMES = [
    "acc_delta_mag_spectral_mid_band_ratio",
    "acc_vertical_spectral_high_band_ratio",
    "gyro_mag_spectral_centroid_hz",
    "acc_horizontal_mag_spectral_centroid_hz",
    "acc_vertical_autocorr_first_zero_seconds",
    "event_gyro_vertical_correlation",
    "acc_horizontal_mag_autocorr_secondary_peak",
    "gyro_mag_spectral_high_band_ratio",
    "acc_vertical_spectral_peak_power_ratio",
    "acc_delta_mag_spectral_high_band_ratio",
    "acc_horizontal_mag_spectral_mid_band_ratio",
    # 陀螺模长主谱峰功率占比，用于区分周期稳定的跳蹲与摆动更分散的弓步/收腹跳。
    "gyro_mag_spectral_peak_power_ratio",
    # 重力对齐垂直加速度的 1.20-2.40 Hz 功率占比，用于分离普通弓步与跳跃动作。
    "acc_vertical_spectral_mid_band_ratio",
    # 水平加速度模长的 2.40-5.00 Hz 功率占比，用于分离深蹲与跳蹲。
    "acc_horizontal_mag_spectral_high_band_ratio",
    # 陀螺模长正峰幅值的总体变异系数，用于描述重复动作每次转动峰值的一致性。
    "gyro_mag_positive_peak_amplitude_cv",
    # 垂直加速度正峰间隔的总体变异系数，用于区分跳跃弓步与收腹跳的节律稳定性。
    "acc_vertical_positive_peak_interval_cv",
    # 垂直加速度与陀螺模长在 ±1 秒内的最大有符号互相关，用于刻画起跳冲击与身体转动的时序耦合。
    "acc_vertical_to_gyro_mag_max_xcorr",
    # 垂直加速度与水平加速度模长的最大有符号互相关，用于区分双脚跳蹲与交替弓步/开合跳。
    "acc_vertical_to_acc_horizontal_mag_max_xcorr",
    # 垂直加速度 0.35-1.20 Hz 低频功率占比，用于区分慢速深蹲与高频收腹跳。
    "acc_vertical_spectral_low_band_ratio",
    # 水平加速度模长低频功率占比，用于分离手臂快速挥动与躯干缓慢前屈。
    "acc_horizontal_mag_spectral_low_band_ratio",
    # 陀螺模长低频功率占比，用于补充 wave 与 good_morning 的转动速度差异。
    "gyro_mag_spectral_low_band_ratio",
    # 水平加速度自相关中不小于 0.20 的显著局部峰数量，用于描述周期性手臂摆动次数。
    "acc_horizontal_mag_autocorr_prominent_peak_count",
    # 水平加速度显著正峰间隔变异系数，用于区分规则挥手与步态/弓步的不规则水平冲击。
    "acc_horizontal_mag_positive_peak_interval_cv",
    # 水平加速度主谱峰功率占比，用于衡量挥手或深蹲动作的周期能量集中程度。
    "acc_horizontal_mag_spectral_peak_power_ratio",
    # 水平动态加速度协方差两主特征值归一化差，用于区分单侧弓步与双侧深蹲/跳蹲。
    "aligned_horizontal_acc_anisotropy",
    # 水平角速度协方差各向异性，用于区分收腹跳与跳蹲/跳跃弓步的躯干转动方向。
    "aligned_horizontal_gyro_anisotropy",
    # 完整腾空事件中局部起跳峰至落地峰的中位时间，单位为秒。
    "aligned_takeoff_to_landing_seconds",
    # 完整腾空事件落地峰后连续高冲击的中位持续时间，单位为秒。
    "aligned_landing_impact_width_seconds",
    # 腾空阶段水平角速度模长积分中位数，单位为度，描述俯仰/横滚总转角。
    "aligned_flight_horizontal_gyro_integral_deg",
    # 腾空阶段垂直角速度绝对积分中位数，单位为度，描述绕重力轴总转角。
    "aligned_flight_vertical_gyro_integral_abs_deg",
]

HIDDEN1 = 96
HIDDEN2 = 64
HIDDEN3 = 32


@dataclass(frozen=True)
class ImuRecord:
    path: Path
    label: str
    label_idx: int


class BPNet(nn.Module):
    def __init__(self, input_dim: int, class_count: int, dropout: float = DROPOUT):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(input_dim, HIDDEN1),
            nn.ReLU(),
            nn.Dropout(dropout),
            nn.Linear(HIDDEN1, HIDDEN2),
            nn.ReLU(),
            nn.Dropout(dropout),
            nn.Linear(HIDDEN2, HIDDEN3),
            nn.ReLU(),
            nn.Linear(HIDDEN3, class_count),
        )

    def forward_features(self, x: torch.Tensor) -> torch.Tensor:
        for layer in list(self.net.children())[:8]:
            x = layer(x)
        return x

    def classify_features(self, embeddings: torch.Tensor) -> torch.Tensor:
        # 将形状为 [批大小,32] 的嵌入送入原 BP 输出层，得到 [批大小,类别数] logits。
        return self.net[8](embeddings)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.classify_features(self.forward_features(x))


class MultiBranchBPNet(nn.Module):
    """按统计、相位、相关、时序、冲击和弱类特征分组编码的轻量 BP 网络。"""

    # 六组维度严格对应 build_feature_names() 的生产顺序，总和必须为 294。
    group_input_dims = (112, 48, 24, 48, 32, 30)
    # 各分支压缩到较小嵌入，限制参数量并避免 30 个弱类特征被 112 个统计量淹没。
    group_output_dims = (24, 12, 8, 12, 8, 16)

    def __init__(self, input_dim: int, class_count: int, dropout: float = DROPOUT):
        # 初始化 PyTorch 模块注册表，使分支和辅助头参与优化及 checkpoint 保存。
        super().__init__()
        # 输入维度必须等于六组特征维度之和，否则切片会错位并破坏 Python/C 一致性。
        if input_dim != sum(self.group_input_dims):
            raise ValueError(
                f"Multi-branch model requires {sum(self.group_input_dims)} features, got {input_dim}"
            )
        # 每个分支执行 Linear-ReLU，将同类物理特征独立编码。
        self.branches = nn.ModuleList(
            [
                nn.Sequential(nn.Linear(input_size, output_size), nn.ReLU())
                for input_size, output_size in zip(
                    self.group_input_dims, self.group_output_dims
                )
            ]
        )
        # 六分支输出拼接为 80 维，再融合到固定 32 维部署嵌入。
        self.fusion = nn.Sequential(
            nn.Linear(sum(self.group_output_dims), 64),
            nn.ReLU(),
            nn.Dropout(dropout),
            nn.Linear(64, 32),
            nn.ReLU(),
        )
        # 主分类头将 [批大小,32] 嵌入映射到动作类别 logits。
        self.classifier = nn.Linear(32, class_count)
        # 五个二分类辅助头只约束训练嵌入，不进入 ESP32 主推理输出。
        self.auxiliary_heads = nn.ModuleDict(
            {
                "is_jump": nn.Linear(32, 2),
                "strong_flight": nn.Linear(32, 2),
                "alternating": nn.Linear(32, 2),
                "lunge_squat": nn.Linear(32, 2),
                "jump_squat_tuck": nn.Linear(32, 2),
            }
        )

    def forward_features(self, x: torch.Tensor) -> torch.Tensor:
        # 输入张量形状为 [批大小,294]，294 个值均为训练集统计量标准化后的无量纲特征。
        branch_outputs: List[torch.Tensor] = []
        # offset 指向当前分支在生产特征向量中的起始列。
        offset = 0
        # 顺序遍历六个分支，每轮消费其固定维度的连续特征切片。
        for branch, input_dim in zip(self.branches, self.group_input_dims):
            # 切片形状为 [批大小,当前组维度]，不复制或重排特征。
            group_values = x[:, offset : offset + input_dim]
            # 分支输出形状为 [批大小,当前分支输出维度]。
            branch_outputs.append(branch(group_values))
            # 移动起始列，下一轮读取紧邻的下一特征组。
            offset += input_dim
        # 拼接六组表示为 [批大小,80]，再输出 [批大小,32] 融合嵌入。
        return self.fusion(torch.cat(branch_outputs, dim=1))

    def classify_features(self, embeddings: torch.Tensor) -> torch.Tensor:
        # 主分类头输出 [批大小,类别数] 未归一化 logits，供交叉熵和间隔损失使用。
        return self.classifier(embeddings)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        # 完整主路径只包含六分支、融合层和主分类头，辅助头不会改变推理结果。
        return self.classify_features(self.forward_features(x))

    def auxiliary_loss(
        self,
        embeddings: torch.Tensor,
        labels: torch.Tensor,
        class_names: Sequence[str],
    ) -> torch.Tensor:
        """计算五个运动属性辅助二分类损失；无适用样本的任务自动跳过。"""
        # 建立类别名称到主分类索引的映射，避免依赖固定字典顺序。
        name_to_idx = {name: index for index, name in enumerate(class_names)}
        # 收集存在有效正负定义的任务损失，最后等权平均。
        losses: List[torch.Tensor] = []

        def append_binary_task(
            head_name: str,
            valid_names: Sequence[str],
            positive_names: Sequence[str],
        ) -> None:
            # 有效类别集合限定该辅助任务参与反向传播的样本范围。
            valid_indices = [name_to_idx[name] for name in valid_names if name in name_to_idx]
            # 正类索引表示目标运动属性存在，例如强腾空或交替落地。
            positive_indices = {
                name_to_idx[name] for name in positive_names if name in name_to_idx
            }
            # mask 形状为 [批大小]，只保留当前任务定义覆盖的类别。
            mask = torch.zeros_like(labels, dtype=torch.bool)
            # 遍历有效类别索引，将对应训练样本加入该辅助任务。
            for class_index in valid_indices:
                mask |= labels == class_index
            # 若本批没有适用样本，跳过该头，避免对空张量计算交叉熵。
            if not torch.any(mask):
                return
            # 二分类目标中 1 表示属性存在，0 表示属性不存在。
            targets = torch.zeros(int(mask.sum().item()), dtype=torch.long, device=labels.device)
            # 遍历被选样本并按主类别是否属于正类集合生成目标。
            selected_labels = labels[mask]
            for class_index in positive_indices:
                targets[selected_labels == class_index] = 1
            # 只有同时含正负样本时才训练该头，防止单类批次造成无效偏置更新。
            if torch.unique(targets).numel() < 2:
                return
            # 辅助头输入 [任务样本数,32]，输出 [任务样本数,2] logits。
            task_logits = self.auxiliary_heads[head_name](embeddings[mask])
            # 标准二分类交叉熵约束融合嵌入保留目标运动属性。
            losses.append(F.cross_entropy(task_logits, targets))

        # 所有类别参与“是否跳跃”，四种跳跃动作作为正类。
        append_binary_task("is_jump", list(class_names), sorted(HIGH_DYNAMIC_CLASSES))
        # 三种明显腾空动作作为正类，其余类别作为负类。
        append_binary_task(
            "strong_flight", list(class_names), ["jumping_lunge", "jumping_squat", "tuck_jump"]
        )
        # 跳跃家族内部仅 jumping_lunge 具有显著左右交替落地属性。
        append_binary_task(
            "alternating", FAMILY_SPECIALIST_CLASS_NAMES, ["jumping_lunge"]
        )
        # 普通弓步与深蹲辅助头中，弓步记为正类以学习水平各向异性。
        append_binary_task("lunge_squat", ["lunge", "squat"], ["lunge"])
        # 跳跃深蹲与收腹跳辅助头中，收腹跳记为正类以学习腾空姿态差异。
        append_binary_task(
            "jump_squat_tuck", ["jumping_squat", "tuck_jump"], ["tuck_jump"]
        )
        # P×K 批次覆盖所有类别时五项均有效；异常批次无有效任务则返回可反传零值。
        return torch.stack(losses).mean() if losses else embeddings.sum() * 0.0


def cross_file_supervised_contrastive_loss(
    embeddings: torch.Tensor,
    labels: torch.Tensor,
    file_ids: torch.Tensor,
    temperature: float = 0.15,
) -> torch.Tensor:
    if len(embeddings) < 2:
        return embeddings.sum() * 0.0
    normalized = F.normalize(embeddings, dim=1)
    logits = normalized @ normalized.T / temperature
    losses: List[torch.Tensor] = []
    for anchor in range(len(embeddings)):
        different_sample = torch.arange(len(embeddings), device=embeddings.device) != anchor
        positive = (
            different_sample
            & (labels == labels[anchor])
            & (file_ids != file_ids[anchor])
        )
        valid = different_sample & (
            (labels != labels[anchor]) | (file_ids != file_ids[anchor])
        )
        if not torch.any(positive):
            continue
        denominator = torch.logsumexp(logits[anchor][valid], dim=0)
        losses.append(-(logits[anchor][positive] - denominator).mean())
    if not losses:
        return embeddings.sum() * 0.0
    return torch.stack(losses).mean()


def hard_pair_margin_loss(
    logits: torch.Tensor,
    labels: torch.Tensor,
    class_names: Sequence[str],
    margin: float = 0.5,
) -> torch.Tensor:
    name_to_idx = {name: index for index, name in enumerate(class_names)}
    losses: List[torch.Tensor] = []
    for true_name, confusing_names in HARD_CONFUSION_PAIRS.items():
        if true_name not in name_to_idx:
            continue
        true_idx = name_to_idx[true_name]
        sample_mask = labels == true_idx
        if not torch.any(sample_mask):
            continue
        true_logits = logits[sample_mask, true_idx]
        for confusing_name in confusing_names:
            if confusing_name not in name_to_idx:
                continue
            confusing_idx = name_to_idx[confusing_name]
            confusing_logits = logits[sample_mask, confusing_idx]
            losses.append(F.relu(margin - true_logits + confusing_logits).mean())
    if not losses:
        return logits.sum() * 0.0
    return torch.stack(losses).mean()


def set_seed(seed: int) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    torch.cuda.manual_seed_all(seed)
    torch.backends.cudnn.deterministic = True
    torch.backends.cudnn.benchmark = False


def update_ema_state(
    previous_state: Optional[Dict[str, torch.Tensor]],
    current_state: Dict[str, torch.Tensor],
    decay: float,
) -> Dict[str, torch.Tensor]:
    if not 0.0 <= decay < 1.0:
        raise ValueError("EMA decay must be in [0, 1)")
    if previous_state is None or decay == 0.0:
        return {name: value.detach().clone() for name, value in current_state.items()}
    updated: Dict[str, torch.Tensor] = {}
    for name, current_value in current_state.items():
        if torch.is_floating_point(current_value):
            updated[name] = (
                previous_state[name] * decay
                + current_value.detach() * (1.0 - decay)
            ).clone()
        else:
            updated[name] = current_value.detach().clone()
    return updated


def resolve_dataset_dir(dataset_dir: Optional[Path]) -> Path:
    candidates = []
    if dataset_dir is not None:
        candidates.append(dataset_dir)
    candidates.extend([DEFAULT_DATASET_DIR, FALLBACK_DATASET_DIR])
    for candidate in candidates:
        if candidate.exists() and candidate.is_dir():
            return candidate
    raise FileNotFoundError(
        "Dataset directory not found. Tried: "
        + ", ".join(str(candidate) for candidate in candidates)
    )


def scan_dataset(dataset_dir: Path) -> Tuple[List[ImuRecord], List[str], Dict[str, int]]:
    class_dirs = sorted(
        [path for path in dataset_dir.iterdir() if path.is_dir() and list(path.glob("*.txt"))]
    )
    if not class_dirs:
        raise ValueError(f"No action folders with txt files found under {dataset_dir}")

    class_names = [path.name for path in class_dirs]
    label_to_idx = {name: idx for idx, name in enumerate(class_names)}
    records: List[ImuRecord] = []
    for class_dir in class_dirs:
        for txt_path in sorted(class_dir.glob("*.txt")):
            records.append(ImuRecord(txt_path, class_dir.name, label_to_idx[class_dir.name]))
    if not records:
        raise ValueError(f"No dataset txt files found under {dataset_dir}")
    return records, class_names, label_to_idx


def scan_labeled_dataset(
    dataset_dir: Path,
    label_to_idx: Dict[str, int],
) -> List[ImuRecord]:
    dataset_dir = Path(dataset_dir)
    if not dataset_dir.is_dir():
        raise FileNotFoundError(f"Additional dataset directory not found: {dataset_dir}")
    records: List[ImuRecord] = []
    for class_dir in sorted(path for path in dataset_dir.iterdir() if path.is_dir()):
        txt_paths = sorted(class_dir.glob("*.txt"))
        if not txt_paths:
            continue
        label = class_dir.name
        if label not in label_to_idx:
            raise ValueError(f"Unknown action directory in additional dataset: {label}")
        records.extend(
            ImuRecord(path, label, label_to_idx[label]) for path in txt_paths
        )
    if not records:
        raise ValueError(f"No labeled txt files found under {dataset_dir}")
    return records


def load_additional_records(
    extra_train_dir: Optional[Path],
    external_holdout_dir: Optional[Path],
    label_to_idx: Dict[str, int],
    validation_only: bool,
) -> Tuple[List[ImuRecord], List[ImuRecord]]:
    extra_records = (
        scan_labeled_dataset(extra_train_dir, label_to_idx)
        if extra_train_dir is not None
        else []
    )
    holdout_records = (
        scan_labeled_dataset(external_holdout_dir, label_to_idx)
        if external_holdout_dir is not None and not validation_only
        else []
    )
    return extra_records, holdout_records


def convert_raw_imu_units(raw: np.ndarray) -> np.ndarray:
    data = np.asarray(raw, dtype=np.float32)
    if data.ndim == 1:
        data = data.reshape(1, -1)
    if data.shape[1] < 6:
        raise ValueError(f"Expected at least 6 columns, got {data.shape[1]}")
    converted = data[:, :6].astype(np.float32, copy=True)
    converted[:, 0:3] = converted[:, 0:3] / 16.4
    converted[:, 3:6] = converted[:, 3:6] / 4096.0
    return converted


def load_imu_file(path: Path) -> np.ndarray:
    raw = np.loadtxt(
        path,
        delimiter=",",
        dtype=np.float32,
        usecols=tuple(range(6)),
    )
    return convert_raw_imu_units(raw)


def window_lengths(window_seconds: float) -> Tuple[int, int]:
    window_len = int(round(window_seconds * SAMPLE_RATE))
    step_len = int(round(STEP_SECONDS * SAMPLE_RATE))
    return window_len, max(1, step_len)


def iter_windows(data: np.ndarray, window_len: int, step_len: int) -> Iterable[np.ndarray]:
    if len(data) < window_len:
        return
    for start in range(0, len(data) - window_len + 1, step_len):
        yield data[start : start + window_len]


def motion_score(window: np.ndarray) -> float:
    gyro_mag = np.linalg.norm(window[:, 0:3], axis=1)
    acc_mag = np.linalg.norm(window[:, 3:6], axis=1)
    return float(np.std(acc_mag) + np.std(gyro_mag) / 200.0)


def instantaneous_motion(window: np.ndarray) -> np.ndarray:
    data = np.asarray(window, dtype=np.float32)
    gyro_mag = np.linalg.norm(data[:, 0:3], axis=1)
    acc_delta = np.linalg.norm(np.diff(data[:, 3:6], axis=0), axis=1)
    acc_delta = np.concatenate([np.zeros(1, dtype=np.float32), acc_delta.astype(np.float32)])
    return (acc_delta + gyro_mag / 200.0).astype(np.float32)


def active_ratio(window: np.ndarray, active_point_threshold: float) -> float:
    scores = instantaneous_motion(window)
    return float(np.mean(scores > active_point_threshold))


def keep_window_for_label(
    window: np.ndarray,
    label: str,
    rest_threshold: float,
    active_point_threshold: float,
) -> bool:
    score = motion_score(window)
    if label == SIT_CLASS_NAME:
        return score <= rest_threshold * 1.6
    if score < rest_threshold:
        return False
    if label in HIGH_DYNAMIC_CLASSES:
        return (
            score >= rest_threshold * 1.25
            and active_ratio(window, active_point_threshold) >= 0.20
        )
    return True


def file_balanced_sample_weights(labels: np.ndarray, file_ids: np.ndarray) -> np.ndarray:
    y = np.asarray(labels, dtype=np.int64)
    groups = np.asarray(file_ids, dtype=np.int64)
    if y.shape != groups.shape:
        raise ValueError(f"labels and file_ids must share shape, got {y.shape} and {groups.shape}")
    if len(y) == 0:
        return np.empty(0, dtype=np.float64)

    pair_counts: Dict[Tuple[int, int], int] = {}
    class_files: Dict[int, set[int]] = {}
    for label, file_id in zip(y.tolist(), groups.tolist()):
        pair = (label, file_id)
        pair_counts[pair] = pair_counts.get(pair, 0) + 1
        class_files.setdefault(label, set()).add(file_id)
    return np.asarray(
        [
            1.0 / (len(class_files[label]) * pair_counts[(label, file_id)])
            for label, file_id in zip(y.tolist(), groups.tolist())
        ],
        dtype=np.float64,
    )


def estimate_rest_threshold(
    records: Sequence[ImuRecord],
    window_len: int,
    step_len: int,
    percentile: float = 85.0,
) -> float:
    scores: List[float] = []
    for record in records:
        if record.label != SIT_CLASS_NAME:
            continue
        data = load_imu_file(record.path)
        scores.extend(motion_score(window) for window in iter_windows(data, window_len, step_len))
    if not scores:
        return 0.03
    threshold = float(np.percentile(np.asarray(scores, dtype=np.float32), percentile))
    return max(threshold, 0.01)


def estimate_active_point_threshold(
    records: Sequence[ImuRecord],
    window_len: int,
    step_len: int,
    percentile: float = 90.0,
) -> float:
    scores: List[float] = []
    for record in records:
        if record.label != SIT_CLASS_NAME:
            continue
        data = load_imu_file(record.path)
        for window in iter_windows(data, window_len, step_len):
            scores.extend(instantaneous_motion(window).tolist())
    if not scores:
        return 0.02
    threshold = float(np.percentile(np.asarray(scores, dtype=np.float32), percentile))
    return max(threshold, 0.005)


def series_features(values: np.ndarray) -> List[float]:
    x = np.asarray(values, dtype=np.float32)
    mean = float(np.mean(x))
    std = float(np.std(x))
    min_v = float(np.min(x))
    max_v = float(np.max(x))
    energy = float(np.mean(x * x))
    centered = x - mean
    if len(x) > 1:
        diffs = np.diff(x)
        abs_diffs = np.abs(diffs)
        mean_abs_diff = float(np.mean(abs_diffs))
        std_diff = float(np.std(diffs))
        sign_product = centered[:-1] * centered[1:]
        zcr = float(np.mean(sign_product < 0.0))
    else:
        mean_abs_diff = 0.0
        std_diff = 0.0
        zcr = 0.0
    return [
        mean,
        std,
        min_v,
        max_v,
        math.sqrt(max(energy, 0.0)),
        mean_abs_diff,
        zcr,
        std_diff,
    ]


def gravity_aligned_series(window: np.ndarray) -> Tuple[np.ndarray, ...]:
    data = np.asarray(window, dtype=np.float32)
    gravity = np.mean(data[:, 3:6], axis=0)
    gravity_norm = float(np.linalg.norm(gravity))
    if gravity_norm < 1e-6:
        gravity_unit = np.asarray([0.0, 0.0, 1.0], dtype=np.float32)
    else:
        gravity_unit = (gravity / gravity_norm).astype(np.float32)

    acc_vertical = data[:, 3:6] @ gravity_unit
    gyro_vertical = data[:, 0:3] @ gravity_unit
    acc_squared = np.sum(data[:, 3:6] * data[:, 3:6], axis=1)
    gyro_squared = np.sum(data[:, 0:3] * data[:, 0:3], axis=1)
    acc_horizontal = np.sqrt(np.maximum(acc_squared - acc_vertical * acc_vertical, 0.0))
    gyro_horizontal = np.sqrt(np.maximum(gyro_squared - gyro_vertical * gyro_vertical, 0.0))
    return (
        acc_vertical.astype(np.float32),
        acc_horizontal.astype(np.float32),
        gyro_vertical.astype(np.float32),
        gyro_horizontal.astype(np.float32),
    )


def phase_features(values: np.ndarray) -> List[float]:
    x = np.asarray(values, dtype=np.float32)
    result: List[float] = []
    for phase in range(PHASE_SEGMENTS):
        start = (phase * len(x)) // PHASE_SEGMENTS
        end = ((phase + 1) * len(x)) // PHASE_SEGMENTS
        segment = x[start:end]
        if len(segment) == 0:
            segment = x[-1:]
        result.extend(
            [
                float(np.mean(segment)),
                float(np.std(segment)),
                float(np.max(np.abs(segment))),
            ]
        )
    return result


def normalized_phase_features(values: np.ndarray) -> List[float]:
    x = np.asarray(values, dtype=np.float32)
    mean = float(np.mean(x))
    std = float(np.std(x))
    normalized = (x - mean) / std if std > 1e-6 else np.zeros_like(x)
    return phase_features(normalized)


def impact_distribution_features(values: np.ndarray) -> List[float]:
    x = np.asarray(values, dtype=np.float32)
    ordered = np.sort(x)
    quantiles = []
    for fraction in (0.10, 0.25, 0.50, 0.75, 0.90):
        index = int(math.floor(fraction * (len(ordered) - 1) + 0.5))
        quantiles.append(float(ordered[index]))
    mean = float(np.mean(x))
    std = float(np.std(x))
    if std > 1e-6:
        normalized = (x - mean) / std
        skew = float(np.mean(normalized**3))
        excess_kurtosis = float(np.mean(normalized**4) - 3.0)
    else:
        skew = 0.0
        excess_kurtosis = 0.0
    max_abs_diff = float(np.max(np.abs(np.diff(x)))) if len(x) > 1 else 0.0
    return quantiles + [skew, excess_kurtosis, max_abs_diff]


def temporal_features(values: np.ndarray) -> List[float]:
    x = np.asarray(values, dtype=np.float32)
    centered = x - float(np.mean(x))
    activity = np.abs(centered)
    std = float(np.std(x))
    high_activity_ratio = float(np.mean(activity > std)) if std > 1e-6 else 0.0

    if len(x) > 2 and std > 1e-6:
        peaks = (
            (activity[1:-1] > activity[:-2])
            & (activity[1:-1] >= activity[2:])
            & (activity[1:-1] > std)
        )
        peak_count_normalized = float(np.sum(peaks)) / float(len(x))
    else:
        peak_count_normalized = 0.0

    spectrum = np.fft.rfft(centered.astype(np.float64))
    power = np.asarray(np.abs(spectrum) ** 2, dtype=np.float64)
    if len(power):
        power[0] = 0.0
    power_sum = float(np.sum(power))
    if power_sum > 1e-12 and len(power) > 1:
        dominant_bin = int(np.argmax(power))
        dominant_frequency_hz = dominant_bin * SAMPLE_RATE / float(len(x))
        probabilities = power[1:] / power_sum
        nonzero = probabilities > 0.0
        spectral_entropy = -float(
            np.sum(probabilities[nonzero] * np.log(probabilities[nonzero]))
        ) / math.log(max(len(probabilities), 2))
    else:
        dominant_frequency_hz = 0.0
        spectral_entropy = 0.0

    lag_start = min(max(1, int(round(0.15 * SAMPLE_RATE))), max(len(x) - 1, 1))
    lag_end = min(len(x) // 2, int(round(1.20 * SAMPLE_RATE)))
    autocorr_peak = 0.0
    autocorr_peak_lag_seconds = 0.0
    if std > 1e-6 and lag_end >= lag_start:
        best_correlation = -1.0
        best_lag = lag_start
        for lag in range(lag_start, lag_end + 1):
            left = centered[:-lag]
            right = centered[lag:]
            denominator = math.sqrt(
                float(np.dot(left, left)) * float(np.dot(right, right))
            )
            correlation = (
                float(np.dot(left, right)) / denominator
                if denominator > 1e-12
                else 0.0
            )
            if correlation > best_correlation:
                best_correlation = correlation
                best_lag = lag
        autocorr_peak = best_correlation
        autocorr_peak_lag_seconds = best_lag / float(SAMPLE_RATE)
    return [
        high_activity_ratio,
        peak_count_normalized,
        dominant_frequency_hz,
        spectral_entropy,
        autocorr_peak,
        autocorr_peak_lag_seconds,
    ]


def _selected_spectral_features(
    values: np.ndarray,
) -> Tuple[float, float, float, float, float, float]:
    """返回三频带比、谱质心、主峰占比和二次谐波比，公式见中文特征文档。"""
    # 将单通道窗口转换为 float64；输入形状为 [时间点数]，物理单位由来源序列决定。
    x = np.asarray(values, dtype=np.float64)
    # 少于 4 点时频率分辨率不足，返回六个有限零值，避免空频带和除零。
    if len(x) < 4:
        # 返回顺序固定为低/中/高频比、谱质心 Hz、主峰占比、二次谐波比。
        return 0.0, 0.0, 0.0, 0.0, 0.0, 0.0
    # 去除直流均值后乘 Hann 窗，降低有限窗口边界造成的频谱泄漏。
    centered = x - float(np.mean(x))
    # 计算实数单边 FFT 的功率谱；输出形状为 [floor(N/2)+1]。
    power = np.square(np.abs(np.fft.rfft(centered * np.hanning(len(x)))))
    # 根据 25 Hz 采样率生成每个单边频点对应的 Hz 坐标。
    frequencies = np.fft.rfftfreq(len(x), d=1.0 / SAMPLE_RATE)
    # 显式清零直流功率，避免残余均值主导动作周期特征。
    power[0] = 0.0
    # 累加所有非直流频点功率，作为比例特征和质心的共同分母。
    total = float(np.sum(power))
    # 近零总功率表示常量或近静止序列，返回零以阻断 NaN/Inf。
    if total <= 1e-12:
        # 六个零值表示窗口没有可解析的动态频谱。
        return 0.0, 0.0, 0.0, 0.0, 0.0, 0.0
    # 低频比累加 0.35<=f<1.20 Hz 的功率并除以总功率，输出无量纲 [0,1]。
    low = float(np.sum(power[(frequencies >= 0.35) & (frequencies < 1.20)]) / total)
    # 中频比累加 1.20<=f<2.40 Hz 的功率并除以总功率，输出无量纲 [0,1]。
    mid = float(np.sum(power[(frequencies >= 1.20) & (frequencies < 2.40)]) / total)
    # 高频比累加 2.40<=f<5.00 Hz 的功率并除以总功率，输出无量纲 [0,1]。
    high = float(np.sum(power[(frequencies >= 2.40) & (frequencies < 5.00)]) / total)
    # 谱质心是频率对功率的加权均值，输出单位为 Hz，范围为 [0,12.5]。
    centroid = float(np.dot(frequencies, power) / total)
    # 主谱峰索引取功率最大频点；np.argmax 在平局时返回最早索引，保证确定性。
    dominant_index = int(np.argmax(power))
    # 主谱峰占比取最大单频功率除以总功率，衡量重复周期能量集中程度。
    peak_power_ratio = float(power[dominant_index] / total)
    # 二次谐波目标频率是主频两倍；超出 Nyquist 时取最近的最高频点。
    harmonic_index = int(
        np.argmin(np.abs(frequencies - 2.0 * frequencies[dominant_index]))
    )
    # 二次谐波比以主谱峰功率为分母，输出无量纲且分母受 1e-12 保护。
    second_harmonic_ratio = float(
        power[harmonic_index] / max(power[dominant_index], 1e-12)
    )
    # 按模型固定顺序返回六个频谱指标，C 端函数参数顺序完全相同。
    return low, mid, high, centroid, peak_power_ratio, second_harmonic_ratio


def _free_flight_features(acc_magnitude: np.ndarray) -> Tuple[float, float]:
    """返回加速度模长低于 0.70g 的比例和最长连续区间比例。"""
    # 输入转换为 float64 一维数组，物理单位必须为 g。
    values = np.asarray(acc_magnitude, dtype=np.float64)
    # 空窗口没有采样点，两个比例均定义为 0 防止除零。
    if len(values) == 0:
        # 第一个零是总腾空比例，第二个零是最长连续腾空比例。
        return 0.0, 0.0
    # 低于 0.70g 视为低支持力候选采样点；输出布尔形状为 [时间点数]。
    free_flight = values < 0.70
    # 布尔均值等于全部低支持力点占窗口长度的比例，范围 [0,1]。
    ratio = float(np.mean(free_flight))
    # longest_run 保存目前发现的最长连续 True 点数。
    longest_run = 0
    # current_run 保存当前连续 True 区间长度。
    current_run = 0
    # 按时间顺序遍历布尔序列，终止于最后一个采样点。
    for is_free_flight in free_flight.tolist():
        # 当前点满足阈值时连续长度加一，否则重置为零。
        current_run = current_run + 1 if is_free_flight else 0
        # 更新历史最长连续长度。
        longest_run = max(longest_run, current_run)
    # 最长连续点数除以窗口长度得到无量纲比例，范围 [0,1]。
    longest_ratio = longest_run / float(len(values))
    # 返回总比例和最长连续比例，顺序与特征名称和 C 端一致。
    return ratio, longest_ratio


def _autocorr_first_zero_seconds(values: np.ndarray) -> float:
    """返回去均值自相关第一次不大于零的延迟时间，单位为秒。"""
    # 转为 float64 的一维窗口，降低自相关点积的累计误差。
    x = np.asarray(values, dtype=np.float64)
    # 少于 4 点不能形成稳定延迟结构，返回 0 秒。
    if len(x) < 4:
        # 0 表示没有可解析的首次过零位置。
        return 0.0
    # 去除均值，使自相关反映波形形状而不是直流偏置。
    centered = x - float(np.mean(x))
    # 计算零延迟能量，用于判断序列是否近似常量。
    energy = float(np.dot(centered, centered))
    # 能量过小时返回 0，避免常量窗口的任意过零位置污染特征。
    if energy <= 1e-12:
        # 0 表示没有动态自相关结构。
        return 0.0
    # 搜索上限取半窗或 3 秒中的较小值，当前 62 点窗口最多搜索 31 点。
    max_lag = min(len(x) // 2, int(SAMPLE_RATE * 3.0))
    # 从 1 个采样点延迟开始顺序查找，保证返回最早过零位置。
    for lag in range(1, max_lag + 1):
        # 点积不大于零表示波形由正相关转为不相关或负相关。
        if float(np.dot(centered[:-lag], centered[lag:])) <= 0.0:
            # 将采样点延迟除以 25 Hz，返回秒单位时间。
            return lag / float(SAMPLE_RATE)
    # 搜索区间内没有过零时返回最大延迟秒数，保持输出有限。
    return max_lag / float(SAMPLE_RATE)


def _autocorr_secondary_peak(values: np.ndarray) -> float:
    """返回归一化自相关中不含零延迟主峰的最大显著局部峰。"""
    # 转为 float64 的一维窗口，输入单位不影响归一化相关值。
    x = np.asarray(values, dtype=np.float64)
    # 少于 5 点无法同时形成左右邻点和多个延迟位置，返回 0。
    if len(x) < 5:
        # 0 表示没有可解析的自相关次峰。
        return 0.0
    # 去均值后计算动态波形，消除传感器静态偏置。
    centered = x - float(np.mean(x))
    # 零延迟能量作为归一化分母，使结果无量纲。
    energy = float(np.dot(centered, centered))
    # 常量序列能量过小，返回 0 避免除零。
    if energy <= 1e-12:
        # 0 表示没有动态周期。
        return 0.0
    # 延迟上限与首次过零特征一致，限制计算量和长延迟噪声。
    max_lag = min(len(x) // 2, int(SAMPLE_RATE * 3.0))
    # 逐延迟计算 C[lag]/C[0]，结果形状为 [max_lag]，范围通常位于 [-1,1]。
    autocorr = np.asarray(
        [
            float(np.dot(centered[:-lag], centered[lag:]) / energy)
            for lag in range(1, max_lag + 1)
        ],
        dtype=np.float64,
    )
    # 检测高于左点、不低于右点且不小于 0.20 的显著自相关局部峰。
    peaks = np.flatnonzero(
        (autocorr[1:-1] > autocorr[:-2])
        & (autocorr[1:-1] >= autocorr[2:])
        & (autocorr[1:-1] >= 0.20)
    ) + 1
    # 有候选时返回最大次峰，否则返回 0；输出无量纲且保持有限。
    return float(np.max(autocorr[peaks])) if len(peaks) else 0.0


def _autocorr_prominent_peak_count(values: np.ndarray) -> float:
    """返回归一化自相关中不小于 0.20 的显著局部峰数量。"""
    # 转为 float64 一维窗口，输入物理单位会在归一化时抵消。
    x = np.asarray(values, dtype=np.float64)
    # 少于 5 点无法形成带左右邻点的延迟局部峰，返回 0 个峰。
    if len(x) < 5:
        # 浮点零与模型其余连续特征保持统一类型。
        return 0.0
    # 去除全窗均值，使自相关只反映动态波形重复性。
    centered = x - float(np.mean(x))
    # 零延迟能量作为所有延迟点积的归一化分母。
    energy = float(np.dot(centered, centered))
    # 常量或近静止序列没有可靠周期峰，直接返回 0 防止除零。
    if energy <= 1e-12:
        # 0 表示搜索范围内不存在可解析周期结构。
        return 0.0
    # 搜索上限取半窗或 3 秒中的较小值，限制噪声和 ESP32 运算量。
    max_lag = min(len(x) // 2, int(SAMPLE_RATE * 3.0))
    # 逐延迟计算 C[lag]/C[0]，输出长度为 max_lag 的无量纲序列。
    autocorr = np.asarray(
        [
            float(np.dot(centered[:-lag], centered[lag:]) / energy)
            for lag in range(1, max_lag + 1)
        ],
        dtype=np.float64,
    )
    # 检测严格高于左点、不低于右点且幅值至少为 0.20 的显著局部峰。
    peaks = np.flatnonzero(
        (autocorr[1:-1] > autocorr[:-2])
        & (autocorr[1:-1] >= autocorr[2:])
        & (autocorr[1:-1] >= 0.20)
    ) + 1
    # 返回峰数量的浮点表示，理论范围为 [0,max_lag-2]。
    return float(len(peaks))


def _series_correlation(left: np.ndarray, right: np.ndarray) -> float:
    """计算两个等长派生序列的零延迟 Pearson 相关系数。"""
    # 左序列转为 float64，一维形状为 [时间点数]。
    a = np.asarray(left, dtype=np.float64)
    # 右序列转为 float64，调用方保证与左序列等长。
    b = np.asarray(right, dtype=np.float64)
    # 左序列去均值，消除物理单位中的静态偏置。
    centered_a = a - float(np.mean(a))
    # 右序列去均值，使点积只衡量同步变化。
    centered_b = b - float(np.mean(b))
    # 分母是两个去均值序列 L2 范数的乘积。
    denominator = math.sqrt(
        float(np.dot(centered_a, centered_a))
        * float(np.dot(centered_b, centered_b))
    )
    # 分母有效时返回 [-1,1] 相关系数，否则返回 0 防止除零。
    return (
        float(np.dot(centered_a, centered_b)) / denominator
        if denominator > 1e-12
        else 0.0
    )


def _max_cross_correlation(left: np.ndarray, right: np.ndarray) -> float:
    """返回 ±1 秒内绝对值最大的有符号归一化互相关。"""
    # 左序列转为 float64，输入形状为 [时间点数]，单位由派生序列决定。
    a = np.asarray(left, dtype=np.float64)
    # 右序列转为 float64；若长度不同，仅使用共同前缀以保证索引安全。
    b = np.asarray(right, dtype=np.float64)
    # 共同长度决定实际参与计算的采样点数。
    n = min(len(a), len(b))
    # 少于 4 点无法形成稳定的正负延迟重叠区间，返回 0。
    if n < 4:
        # 0 表示没有可解析的跨通道时序耦合。
        return 0.0
    # 截取共同前缀并去除全窗均值，保持与候选分析器和 C 端一致。
    centered_a = a[:n] - float(np.mean(a[:n]))
    # 右序列使用自身全窗均值去中心化，保留相位和符号关系。
    centered_b = b[:n] - float(np.mean(b[:n]))
    # 最大延迟取四分之一窗口或 1 秒中的较小值；62 点窗口为 15 点。
    max_lag = min(n // 4, int(SAMPLE_RATE))
    # 初始相关系数为 0；只有绝对值严格增大时才更新，保证最早延迟的确定性。
    best_correlation = 0.0
    # 从负最大延迟到正最大延迟遍历，终止条件包含两个端点。
    for lag in range(-max_lag, max_lag + 1):
        # 负延迟表示左序列向前截去 -lag 点，右序列保留前 n+lag 点。
        if lag < 0:
            # 两个切片长度均为 n+lag，形状严格相同。
            left_slice, right_slice = centered_a[-lag:], centered_b[: n + lag]
        # 正延迟表示右序列向前截去 lag 点，左序列保留前 n-lag 点。
        elif lag > 0:
            # 两个切片长度均为 n-lag，形状严格相同。
            left_slice, right_slice = centered_a[: n - lag], centered_b[lag:]
        # 零延迟直接比较两个完整去均值窗口。
        else:
            # 不复制数据，只建立完整数组视图。
            left_slice, right_slice = centered_a, centered_b
        # 分母是当前重叠区间两个 L2 范数乘积，物理单位在相除后抵消。
        denominator = math.sqrt(
            float(np.dot(left_slice, left_slice))
            * float(np.dot(right_slice, right_slice))
        )
        # 有效能量时计算 [-1,1] 相关系数，近常量区间则定义为 0。
        correlation = (
            float(np.dot(left_slice, right_slice)) / denominator
            if denominator > 1e-12
            else 0.0
        )
        # 仅当绝对相关更强时更新，同时保留相关符号供 BP 区分同相和反相运动。
        if abs(correlation) > abs(best_correlation):
            # 保存当前最强有符号相关系数。
            best_correlation = correlation
    # 返回无量纲有限值，浮点误差下理论范围接近 [-1,1]。
    return best_correlation


def _positive_peak_shape_features(values: np.ndarray) -> Tuple[float, float]:
    """返回正峰幅值和峰间隔的总体变异系数，公式见 docs/弱类频谱与峰形特征说明.md。"""
    # 将单通道时序转换为 float64，形状为 [时间点数]，降低方差累计时的舍入误差。
    x = np.asarray(values, dtype=np.float64)
    # 少于 3 点无法形成“前一点-峰值-后一点”，因此返回两个零值表示无有效峰形。
    if len(x) < 3:
        # 第一个零是峰幅变异系数，第二个零是峰间隔变异系数。
        return 0.0, 0.0
    # 计算序列均值，单位继承输入：陀螺模长为 deg/s，垂直加速度为 g。
    mean = float(np.mean(x))
    # 计算总体标准差，作为抑制噪声小峰的自适应幅值尺度。
    std = float(np.std(x))
    # 峰值门槛设为均值加 0.5 倍标准差，仅保留动作周期中的显著正峰。
    threshold = mean + 0.5 * std
    # 检测严格高于前点、且不低于后点并越过门槛的局部峰；结果是峰位置索引。
    peaks = np.flatnonzero(
        (x[1:-1] > x[:-2]) & (x[1:-1] >= x[2:]) & (x[1:-1] >= threshold)
    ) + 1
    # 少于 2 个峰无法计算峰间隔方差，也不足以评估重复峰幅稳定性。
    if len(peaks) < 2:
        # 无法估计时返回零，避免 NaN 进入标准化和 BP 前向传播。
        return 0.0, 0.0
    # 以全窗中位数作为稳健基线，得到每个正峰相对基线的幅值。
    peak_amplitudes = x[peaks] - float(np.median(x))
    # 相邻峰索引差表示重复周期长度，单位为采样点；比例特征无需再换算为秒。
    peak_intervals = np.diff(peaks).astype(np.float64)

    # 内部函数按 CV=总体标准差/绝对均值计算无量纲离散程度。
    def population_cv(samples: np.ndarray) -> float:
        # 绝对均值保证峰幅可能跨越零基线时分母仍表示典型幅值尺度。
        denominator = float(np.mean(np.abs(samples)))
        # 分母过小表示序列近似常量，返回零以避免除零和无意义放大。
        if denominator <= 1e-12:
            # 零表示没有可解析的相对离散程度。
            return 0.0
        # 返回总体标准差与绝对均值之比，输出无量纲且理论范围为 [0,+∞)。
        return float(np.std(samples) / denominator)

    # 先返回峰幅变异系数，再返回峰间隔变异系数，顺序与特征名称和 C 端一致。
    return population_cv(peak_amplitudes), population_cv(peak_intervals)


def _horizontal_plane_anisotropy(vectors: np.ndarray) -> float:
    """计算重力水平面向量协方差的各向异性，公式见 docs/弱类联合优化方案.md。"""
    # 输入统一为 float64 的 [时间点数,3] 向量，降低协方差累计误差。
    values = np.asarray(vectors, dtype=np.float64)
    # 少于两个采样点无法估计方向分布，返回有限零值。
    if len(values) < 2:
        # 零表示没有可靠各向异性证据。
        return 0.0
    # 去除三维向量均值，使协方差只反映动态方向变化。
    centered = values - np.mean(values, axis=0, keepdims=True)
    # 使用总体协方差 C=X^T X/N；矩阵形状为 [3,3]，单位为输入单位平方。
    covariance = centered.T @ centered / float(len(centered))
    # 对称协方差用 eigvalsh 求实特征值，并截断浮点舍入产生的极小负数。
    eigenvalues = np.maximum(np.linalg.eigvalsh(covariance), 0.0)
    # 最大特征值表示水平主运动方向能量。
    lambda_1 = float(eigenvalues[-1])
    # 第二大特征值表示水平正交方向能量；第三个值理论上对应重力方向零能量。
    lambda_2 = float(eigenvalues[-2])
    # 两个水平特征值总和过小时，方向由噪声主导，返回 0 防止放大。
    if lambda_1 + lambda_2 <= 1e-12:
        # 静止或近静止窗口没有可解释方向。
        return 0.0
    # 返回 (λ1-λ2)/(λ1+λ2)，单一主方向接近 1，各向同性接近 0。
    return (lambda_1 - lambda_2) / (lambda_1 + lambda_2)


def _event_aligned_selected_features(window: np.ndarray) -> List[float]:
    """提取 6 项通过 Round23 无训练筛选的事件对齐特征，输入形状为 [N,6]。"""
    # 转为 float64；六轴列顺序固定为 gx、gy、gz、ax、ay、az。
    data = np.asarray(window, dtype=np.float64)
    # 非法或空窗口返回六个零，保证 BP 输入维度和有限性。
    if data.ndim != 2 or data.shape[1] != 6 or len(data) == 0:
        # 六个零依次对应两项各向异性、两项事件时间和两项角度积分。
        return [0.0] * 6
    # 用全窗加速度均值估计重力向量，单位为 g。
    gravity = np.mean(data[:, 3:6], axis=0)
    # 计算重力模长；传感器异常导致模长过小时使用 z 轴退化方向。
    gravity_norm = float(np.linalg.norm(gravity))
    # gravity_unit 是单位重力方向，形状为 [3]。
    gravity_unit = (
        gravity / gravity_norm
        if gravity_norm > 1e-6
        else np.asarray([0.0, 0.0, 1.0], dtype=np.float64)
    )
    # 动态加速度从原始加速度减去窗口重力均值，单位为 g。
    dynamic_acc = data[:, 3:6] - gravity
    # 计算动态加速度在重力方向的标量投影，形状为 [N]。
    dynamic_vertical = dynamic_acc @ gravity_unit
    # 移除垂直投影，保留方向不丢失的水平动态加速度向量。
    horizontal_acc = dynamic_acc - np.outer(dynamic_vertical, gravity_unit)
    # 计算原始陀螺在重力方向的角速度分量，单位为 deg/s。
    gyro_vertical = data[:, 0:3] @ gravity_unit
    # 移除垂直分量，得到水平俯仰/横滚角速度向量。
    horizontal_gyro = data[:, 0:3] - np.outer(gyro_vertical, gravity_unit)
    # 第一项描述水平加速度是否集中在单一方向。
    acc_anisotropy = _horizontal_plane_anisotropy(horizontal_acc)
    # 第二项描述水平躯干转动是否集中在单一轴向。
    gyro_anisotropy = _horizontal_plane_anisotropy(horizontal_gyro)
    # 重力对齐垂直加速度保留约 1g 静态分量，用于定位推进和落地峰。
    vertical = data[:, 3:6] @ gravity_unit
    # 加速度模长低于 0.70g 且连续至少两个点时视为候选腾空段。
    flight_mask = np.linalg.norm(data[:, 3:6], axis=1) < 0.70
    # runs 保存半开腾空区间 [start,end)，按时间升序排列。
    runs: List[Tuple[int, int]] = []
    # start 记录当前连续低支持力段起点，None 表示尚未进入。
    start: Optional[int] = None
    # 顺序遍历所有腾空布尔值，并在 False 处关闭区间。
    for index, is_flight in enumerate(flight_mask.tolist()):
        # 首个 True 点开启候选腾空段。
        if is_flight and start is None:
            # 当前采样索引作为半开区间起点。
            start = index
        # False 点关闭此前已开启的区间。
        elif not is_flight and start is not None:
            # 只保留至少两个点的区间，抑制单点噪声误检。
            if index - start >= 2:
                # 当前 False 索引作为不包含的区间终点。
                runs.append((start, index))
            # 清除状态以检测后续独立腾空段。
            start = None
    # 窗口结束时仍处于腾空状态，需要用窗口长度关闭区间。
    if start is not None and len(data) - start >= 2:
        # 末尾半开区间终点等于采样点数。
        runs.append((start, len(data)))
    # 只有起点前和终点后均有样本的区间，才可完整估计起跳与落地。
    complete_runs = [(run_start, run_end) for run_start, run_end in runs if run_start > 0 and run_end < len(data)]
    # event_intervals 保存每个完整事件起跳局部峰到落地局部峰的秒数。
    event_intervals: List[float] = []
    # impact_widths 保存每个落地连续高冲击宽度，单位为秒。
    impact_widths: List[float] = []
    # horizontal_integrals 保存每次腾空水平角速度模长积分，单位为度。
    horizontal_integrals: List[float] = []
    # vertical_integrals 保存每次腾空垂直角速度绝对积分，单位为度。
    vertical_integrals: List[float] = []
    # 逐事件提取推进、落地和腾空姿态量；循环终止于最后一个完整区间。
    for run_start, run_end in complete_runs:
        # 起跳局部搜索覆盖腾空前最多 5 个采样点。
        pre_start = max(0, run_start - 5)
        # 在局部范围内取最大垂直加速度推进峰，平局时取最早索引。
        takeoff_index = pre_start + int(np.argmax(vertical[pre_start:run_start]))
        # 落地局部搜索覆盖腾空结束后最多 6 个采样点。
        post_end = min(len(data), run_end + 6)
        # 在落地范围内取最大垂直冲击峰。
        landing_index = run_end + int(np.argmax(vertical[run_end:post_end]))
        # 峰间采样差除以 25 Hz，得到局部起跳至落地时间。
        event_intervals.append(
            (landing_index - takeoff_index) / float(SAMPLE_RATE)
        )
        # 高冲击门槛取 1.20g 与全窗均值加半个标准差中的较大者。
        impact_threshold = max(
            1.20, float(np.mean(vertical) + 0.5 * np.std(vertical))
        )
        # impact_points 记录落地峰后连续高于门槛的采样点数。
        impact_points = 0
        # 从落地峰开始遍历局部恢复区，首次低于门槛即结束。
        for value in vertical[landing_index:post_end]:
            # 当前点仍属于连续冲击时累加一点。
            if float(value) >= impact_threshold:
                # 每一点对应 1/25 秒。
                impact_points += 1
            # 首个低点表示连续冲击结束。
            else:
                # 退出当前事件的冲击宽度循环。
                break
        # 将冲击点数换算为秒并保存。
        impact_widths.append(impact_points / float(SAMPLE_RATE))
        # 对腾空段每点水平角速度模长求和并除以采样率，近似总俯仰/横滚角度。
        horizontal_integrals.append(
            float(np.sum(np.linalg.norm(horizontal_gyro[run_start:run_end], axis=1)))
            / float(SAMPLE_RATE)
        )
        # 对腾空段垂直角速度绝对值积分，近似绕重力轴总转角。
        vertical_integrals.append(
            float(np.sum(np.abs(gyro_vertical[run_start:run_end])))
            / float(SAMPLE_RATE)
        )
    # 定义事件级稳健聚合器：完整事件存在时取中位数，否则返回 0。
    def event_median(values: Sequence[float]) -> float:
        # 中位数降低单个误检事件对最终窗口特征的影响。
        return float(np.median(values)) if values else 0.0
    # 按特征名称固定顺序返回六个值，供 Python 模型和生成 C 共同使用。
    return [
        acc_anisotropy,
        gyro_anisotropy,
        event_median(event_intervals),
        event_median(impact_widths),
        event_median(horizontal_integrals),
        event_median(vertical_integrals),
    ]


def weak_class_features(series: Dict[str, np.ndarray]) -> List[float]:
    """提取 24 项已通过 Round21 验证的弱类特征；每个输入值形状为 [时间点数]。"""
    # 计算加速度变化模长频谱，输入单位为 g/采样点，获得中/高频占比。
    _, acc_delta_mid, acc_delta_high, _, _, _ = _selected_spectral_features(
        series["acc_delta_mag"]
    )
    # 计算重力对齐垂直加速度频谱，输入单位为 g，并保留中/高频和主峰占比。
    (
        acc_vertical_low,
        acc_vertical_mid,
        acc_vertical_high,
        _,
        acc_vertical_peak_ratio,
        _,
    ) = _selected_spectral_features(series["acc_vertical"])
    # 计算陀螺模长频谱，输入单位为 deg/s，并保留高频、谱质心和主峰占比。
    gyro_low, _, gyro_high, gyro_centroid, gyro_peak_ratio, _ = _selected_spectral_features(
        series["gyro_mag"]
    )
    # 计算重力正交平面内的加速度模长频谱，输入单位为 g，并保留中/高频和质心。
    (
        horizontal_low,
        horizontal_mid,
        horizontal_high,
        horizontal_centroid,
        horizontal_peak_ratio,
        _,
    ) = _selected_spectral_features(series["acc_horizontal_mag"])
    # 计算陀螺正峰幅值 CV；峰间隔 CV 属于已拒绝的 Round22 候选，不进入生产顺序。
    gyro_peak_amplitude_cv, _ = _positive_peak_shape_features(
        series["gyro_mag"]
    )
    # 计算垂直加速度正峰间隔 CV；第一个返回值是峰幅 CV，本轮不采用。
    _, vertical_peak_interval_cv = _positive_peak_shape_features(
        series["acc_vertical"]
    )
    # 计算水平加速度正峰间隔 CV；第一个返回值为本轮未采用的峰幅 CV。
    _, horizontal_peak_interval_cv = _positive_peak_shape_features(
        series["acc_horizontal_mag"]
    )
    # 按 gx、gy、gz、ax、ay、az 顺序重建 [时间点数,6] 窗口，供方向保持的事件特征使用。
    reconstructed_window = np.column_stack(
        [series[channel_name] for channel_name in CHANNEL_NAMES]
    )
    # 提取 6 项事件对齐候选；其多文件效应证据记录在 Round23 无训练报告中。
    aligned_features = _event_aligned_selected_features(reconstructed_window)
    # 先组装 24 项 Round21 特征，固定顺序与前一版标准化合同一致。
    features = [
        acc_delta_mid,
        acc_vertical_high,
        gyro_centroid,
        horizontal_centroid,
        _autocorr_first_zero_seconds(series["acc_vertical"]),
        _series_correlation(series["gyro_mag"], series["acc_vertical"]),
        _autocorr_secondary_peak(series["acc_horizontal_mag"]),
        gyro_high,
        acc_vertical_peak_ratio,
        acc_delta_high,
        horizontal_mid,
        gyro_peak_ratio,
        acc_vertical_mid,
        horizontal_high,
        gyro_peak_amplitude_cv,
        vertical_peak_interval_cv,
        _max_cross_correlation(series["acc_vertical"], series["gyro_mag"]),
        _max_cross_correlation(
            series["acc_vertical"], series["acc_horizontal_mag"]
        ),
        acc_vertical_low,
        horizontal_low,
        gyro_low,
        _autocorr_prominent_peak_count(series["acc_horizontal_mag"]),
        horizontal_peak_interval_cv,
        horizontal_peak_ratio,
    ]
    # 将 6 项事件对齐值追加到末尾，总计 30 项弱类特征和 294 维完整输入。
    features.extend(aligned_features)
    # 返回固定顺序列表；生成 C 必须在同一位置追加完全相同的六个值。
    return features


def build_feature_series(window: np.ndarray) -> Dict[str, np.ndarray]:
    data = np.asarray(window, dtype=np.float32)
    series: Dict[str, np.ndarray] = {
        name: data[:, axis] for axis, name in enumerate(CHANNEL_NAMES)
    }
    series["gyro_mag"] = np.linalg.norm(data[:, 0:3], axis=1)
    series["acc_mag"] = np.linalg.norm(data[:, 3:6], axis=1)
    series["gyro_delta_mag"] = np.linalg.norm(np.diff(data[:, 0:3], axis=0), axis=1)
    series["acc_delta_mag"] = np.linalg.norm(np.diff(data[:, 3:6], axis=0), axis=1)
    for name, values in zip(GRAVITY_NAMES, gravity_aligned_series(data)):
        series[name] = values
    return series


def extract_features(window: np.ndarray) -> np.ndarray:
    data = np.asarray(window, dtype=np.float32)
    if data.ndim != 2 or data.shape[1] != 6:
        raise ValueError(f"Expected window shape (n, 6), got {data.shape}")

    series = build_feature_series(data)
    features: List[float] = []
    for source in GLOBAL_SERIES_NAMES:
        features.extend(series_features(series[source]))
    for source in PHASE_SOURCE_NAMES:
        features.extend(phase_features(series[source]))
    for source in PHASE_SOURCE_NAMES:
        features.extend(temporal_features(series[source]))
    for source in PHASE_SOURCE_NAMES:
        features.extend(normalized_phase_features(series[source]))
    for source in PHASE_SOURCE_NAMES:
        features.extend(impact_distribution_features(series[source]))
    features.extend(weak_class_features(series))
    return np.asarray(features, dtype=np.float32)


def build_feature_names() -> List[str]:
    names: List[str] = []
    for source in GLOBAL_SERIES_NAMES:
        for feature in ONE_SERIES_FEATURES:
            names.append(f"{source}_{feature}")
    for source in PHASE_SOURCE_NAMES:
        for phase in range(PHASE_SEGMENTS):
            for feature in PHASE_FEATURES:
                names.append(f"{source}_phase{phase}_{feature}")
    for source in PHASE_SOURCE_NAMES:
        for feature in TEMPORAL_FEATURES:
            names.append(f"{source}_{feature}")
    for source in PHASE_SOURCE_NAMES:
        for phase in range(PHASE_SEGMENTS):
            for feature in PHASE_FEATURES:
                names.append(f"{source}_normalized_phase{phase}_{feature}")
    for source in PHASE_SOURCE_NAMES:
        for feature in IMPACT_DISTRIBUTION_FEATURES:
            names.append(f"{source}_{feature}")
    names.extend(WEAK_CLASS_FEATURE_NAMES)
    return names


def build_jump_shape_feature_indices(feature_names: Sequence[str]) -> List[int]:
    invariant_suffixes = (
        "_zcr",
        "_high_activity_ratio",
        "_peak_count_normalized",
        "_dominant_frequency_hz",
        "_spectral_entropy",
        "_autocorr_peak",
        "_autocorr_peak_lag_seconds",
        "_skew",
        "_excess_kurtosis",
    )
    return [
        index
        for index, name in enumerate(feature_names)
        if "_normalized_phase" in name or name.endswith(invariant_suffixes)
    ]


def split_records_by_file(
    records: Sequence[ImuRecord],
    seed: int = SEED,
) -> Tuple[List[ImuRecord], List[ImuRecord], List[ImuRecord]]:
    labels = [record.label_idx for record in records]
    train_records, temp_records = train_test_split(
        list(records),
        train_size=TRAIN_RATIO,
        random_state=seed,
        stratify=labels,
    )
    temp_labels = [record.label_idx for record in temp_records]
    val_fraction_of_temp = VAL_RATIO / (VAL_RATIO + TEST_RATIO)
    val_records, test_records = train_test_split(
        temp_records,
        train_size=val_fraction_of_temp,
        random_state=seed + 1,
        stratify=temp_labels,
    )
    return list(train_records), list(val_records), list(test_records)


def split_records_for_experiment(
    base_records: Sequence[ImuRecord],
    extra_train_records: Sequence[ImuRecord] = (),
    seed: int = SEED,
) -> Tuple[List[ImuRecord], List[ImuRecord], List[ImuRecord]]:
    train_records, val_records, test_records = split_records_by_file(
        base_records,
        seed,
    )
    base_paths = {record.path.resolve() for record in base_records}
    extra_paths = [record.path.resolve() for record in extra_train_records]
    duplicate_paths = base_paths.intersection(extra_paths)
    if duplicate_paths:
        raise ValueError(
            "Extra training records duplicate base dataset paths: "
            + ", ".join(str(path) for path in sorted(duplicate_paths))
        )
    if len(extra_paths) != len(set(extra_paths)):
        raise ValueError("Extra training records contain duplicate paths")
    return train_records + list(extra_train_records), val_records, test_records


def euler_rotation_matrix(rx: float, ry: float, rz: float) -> np.ndarray:
    sx, cx = math.sin(rx), math.cos(rx)
    sy, cy = math.sin(ry), math.cos(ry)
    sz, cz = math.sin(rz), math.cos(rz)
    rotation_x = np.asarray(
        [[1.0, 0.0, 0.0], [0.0, cx, -sx], [0.0, sx, cx]], dtype=np.float32
    )
    rotation_y = np.asarray(
        [[cy, 0.0, sy], [0.0, 1.0, 0.0], [-sy, 0.0, cy]], dtype=np.float32
    )
    rotation_z = np.asarray(
        [[cz, -sz, 0.0], [sz, cz, 0.0], [0.0, 0.0, 1.0]], dtype=np.float32
    )
    return (rotation_z @ rotation_y @ rotation_x).astype(np.float32)


def rotate_imu_window(window: np.ndarray, rotation: np.ndarray) -> np.ndarray:
    data = np.asarray(window, dtype=np.float32)
    matrix = np.asarray(rotation, dtype=np.float32)
    if matrix.shape != (3, 3):
        raise ValueError(f"Expected rotation shape (3, 3), got {matrix.shape}")
    rotated = data.copy()
    rotated[:, 0:3] = data[:, 0:3] @ matrix.T
    rotated[:, 3:6] = data[:, 3:6] @ matrix.T
    return rotated


def time_warp_window(
    window: np.ndarray,
    rng: np.random.Generator,
    max_displacement: float = 0.03,
) -> np.ndarray:
    data = np.asarray(window, dtype=np.float32)
    if len(data) < 3 or max_displacement <= 0.0:
        return data.copy()
    timeline = np.linspace(0.0, 1.0, len(data), dtype=np.float64)
    phase = float(rng.uniform(0.0, 2.0 * math.pi))
    amplitude = float(rng.uniform(0.25, 1.0)) * max_displacement
    displacement = amplitude * np.sin(math.pi * timeline) * np.sin(
        2.0 * math.pi * timeline + phase
    )
    source_timeline = np.clip(timeline + displacement, 0.0, 1.0)
    source_timeline = np.maximum.accumulate(source_timeline)
    source_timeline[0] = 0.0
    source_timeline[-1] = 1.0
    warped = np.empty_like(data)
    for axis in range(data.shape[1]):
        warped[:, axis] = np.interp(source_timeline, timeline, data[:, axis]).astype(
            np.float32
        )
    return warped


def augment_window(window: np.ndarray, rng: np.random.Generator) -> np.ndarray:
    max_angle = math.radians(MAX_ROTATION_DEGREES)
    angles = rng.uniform(-max_angle, max_angle, size=3)
    rotation = euler_rotation_matrix(float(angles[0]), float(angles[1]), float(angles[2]))
    augmented = rotate_imu_window(window, rotation)
    augmented = time_warp_window(augmented, rng, max_displacement=0.03)
    gyro_noise = rng.normal(0.0, 0.25, size=augmented[:, 0:3].shape).astype(np.float32)
    acc_noise = rng.normal(0.0, 0.003, size=augmented[:, 3:6].shape).astype(np.float32)
    augmented[:, 0:3] += gyro_noise
    augmented[:, 3:6] += acc_noise
    return augmented


def build_samples(
    records: Sequence[ImuRecord],
    window_len: int,
    step_len: int,
    rest_threshold: float,
    active_point_threshold: float,
    augment: bool,
    rng: np.random.Generator,
    progress_label: Optional[str] = None,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray, Dict[str, int]]:
    features: List[np.ndarray] = []
    labels: List[int] = []
    file_ids: List[int] = []
    skipped = {
        "too_short": 0,
        "rest_filtered": 0,
        "kept_windows": 0,
        "files_without_valid_window": 0,
    }

    for file_id, record in enumerate(records):
        if progress_label and (file_id == 0 or file_id % 10 == 0):
            print(
                f"features {progress_label} file={file_id + 1}/{len(records)} "
                f"kept={skipped['kept_windows']}",
                flush=True,
            )
        data = load_imu_file(record.path)
        if len(data) < window_len:
            skipped["too_short"] += 1
            continue
        record_kept = 0
        for window in iter_windows(data, window_len, step_len):
            if not keep_window_for_label(
                window,
                record.label,
                rest_threshold,
                active_point_threshold,
            ):
                skipped["rest_filtered"] += 1
                continue
            features.append(extract_features(window))
            labels.append(record.label_idx)
            file_ids.append(file_id)
            skipped["kept_windows"] += 1
            record_kept += 1
            if augment:
                for _ in range(AUGMENT_TIMES):
                    features.append(extract_features(augment_window(window, rng)))
                    labels.append(record.label_idx)
                    file_ids.append(file_id)
                    skipped["kept_windows"] += 1
        if record_kept == 0:
            skipped["files_without_valid_window"] += 1
            if record.label in HIGH_DYNAMIC_CLASSES:
                continue
            fallback_windows = list(iter_windows(data, window_len, step_len))
            if fallback_windows:
                scored = [(motion_score(window), window) for window in fallback_windows]
                if record.label == SIT_CLASS_NAME:
                    _, best_window = min(scored, key=lambda item: item[0])
                else:
                    _, best_window = max(scored, key=lambda item: item[0])
                features.append(extract_features(best_window))
                labels.append(record.label_idx)
                file_ids.append(file_id)
                skipped["kept_windows"] += 1

    if not features:
        raise ValueError("No samples generated after filtering")
    if progress_label:
        print(
            f"features {progress_label} file={len(records)}/{len(records)} "
            f"kept={skipped['kept_windows']} complete=true",
            flush=True,
        )
    return (
        np.vstack(features).astype(np.float32),
        np.asarray(labels, dtype=np.int64),
        np.asarray(file_ids, dtype=np.int64),
        skipped,
    )


def standardize(
    train_x: np.ndarray,
    val_x: np.ndarray,
    test_x: np.ndarray,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    mean = np.mean(train_x, axis=0).astype(np.float32)
    std = np.std(train_x, axis=0).astype(np.float32)
    std[std < 1e-6] = 1.0
    return (
        ((train_x - mean) / std).astype(np.float32),
        ((val_x - mean) / std).astype(np.float32),
        ((test_x - mean) / std).astype(np.float32),
        mean,
        std,
    )


def family_subset(
    x: np.ndarray,
    y: np.ndarray,
    file_ids: np.ndarray,
    class_names: Sequence[str],
    family_names: Sequence[str],
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    global_indices = [class_names.index(name) for name in family_names]
    global_to_local = {
        global_idx: local_idx for local_idx, global_idx in enumerate(global_indices)
    }
    mask = np.isin(y, np.asarray(global_indices, dtype=np.int64))
    local_y = np.asarray(
        [global_to_local[int(label)] for label in np.asarray(y)[mask]],
        dtype=np.int64,
    )
    return np.asarray(x)[mask], local_y, np.asarray(file_ids)[mask]


def route_family_predictions(
    primary_pred: np.ndarray,
    specialist_pred: np.ndarray,
    class_names: Sequence[str],
    family_names: Sequence[str],
) -> np.ndarray:
    primary = np.asarray(primary_pred, dtype=np.int64)
    specialist = np.asarray(specialist_pred, dtype=np.int64)
    if primary.shape != specialist.shape:
        raise ValueError("Primary and specialist predictions must have the same shape")
    family_global = np.asarray(
        [class_names.index(name) for name in family_names], dtype=np.int64
    )
    routed = primary.copy()
    mask = np.isin(primary, family_global)
    routed[mask] = family_global[specialist[mask]]
    return routed


def class_weight_tensor(labels: np.ndarray, class_count: int, device: torch.device) -> torch.Tensor:
    counts = np.bincount(labels, minlength=class_count).astype(np.float32)
    counts[counts == 0.0] = 1.0
    weights = counts.sum() / (class_count * counts)
    return torch.tensor(weights, dtype=torch.float32, device=device)


def pk_ce_class_weights(
    labels: np.ndarray,
    class_count: int,
    device: torch.device,
) -> torch.Tensor:
    """恢复 P×K 均匀批次被抹去的原训练窗口类别先验。"""
    # 统计增强后训练集中每类窗口数，形状为 [类别数]。
    counts = np.bincount(labels, minlength=class_count).astype(np.float32)
    # 任一类别缺失都会使 P×K 和主分类合同无效，因此立即报错而不伪造权重。
    if np.any(counts <= 0.0):
        raise ValueError("Every class requires at least one training window")
    # P×K 让采样概率变为 1/P；CE 乘以相对计数即可恢复原窗口先验。
    weights = counts / float(np.mean(counts))
    # 返回位于训练设备上的 float32 权重，均值为 1 且相对比例等于类别计数比例。
    return torch.tensor(weights, dtype=torch.float32, device=device)


class PKFileBatchSampler(Sampler[List[int]]):
    """每批从全部 P 个类别各取 K 个窗口，并优先让同类样本来自不同文件。"""

    def __init__(
        self,
        labels: np.ndarray,
        file_ids: np.ndarray,
        samples_per_class: int,
        seed: int,
    ):
        # 将标签保存为一维 int64 数组，元素表示每个窗口的类别索引。
        self.labels = np.asarray(labels, dtype=np.int64).reshape(-1)
        # 将文件编号保存为一维 int64 数组，用于避免同文件重叠窗口充当对比正样本。
        self.file_ids = np.asarray(file_ids, dtype=np.int64).reshape(-1)
        # 标签与文件编号必须逐窗口对应，否则采样出的来源约束无效。
        if self.labels.shape != self.file_ids.shape:
            raise ValueError("labels and file_ids must have the same shape")
        # K 必须为正整数；训练配置采用 K=6，单元测试可使用更小值。
        if samples_per_class <= 0:
            raise ValueError("samples_per_class must be positive")
        # 保存每类每批样本数 K。
        self.samples_per_class = int(samples_per_class)
        # 保存基础随机种子，保证相同配置可复现。
        self.seed = int(seed)
        # 每次完整迭代视为一个 epoch，并改变随机序列以避免重复相同批次。
        self.epoch = 0
        # 只保留数据中实际存在的类别，按索引升序形成 P 个类别。
        self.classes = np.unique(self.labels).tolist()
        # 每个 epoch 的批次数向上取整，使总抽样量不低于原训练窗口数。
        self.batch_count = max(
            1,
            math.ceil(
                len(self.labels) / max(len(self.classes) * self.samples_per_class, 1)
            ),
        )
        # 按“类别 -> 文件 -> 窗口索引”建立候选池，后续先抽文件再抽窗口。
        self.indices_by_class_file: Dict[int, Dict[int, np.ndarray]] = {}
        # 遍历 P 个存在类别，为每类建立独立文件池。
        for class_index in self.classes:
            # 当前类别的窗口索引形状为 [该类窗口数]。
            class_indices = np.flatnonzero(self.labels == class_index)
            # 当前类别内实际存在的文件编号集合。
            class_files = np.unique(self.file_ids[class_indices])
            # 为每个文件保存属于当前类别且属于该文件的全部窗口索引。
            self.indices_by_class_file[class_index] = {
                int(file_id): class_indices[self.file_ids[class_indices] == file_id]
                for file_id in class_files
            }

    def __len__(self) -> int:
        # 返回每个 epoch 产生的 P×K 批次数，供 DataLoader 计算长度。
        return self.batch_count

    def __iter__(self) -> Iterable[List[int]]:
        # 使用“基础种子+epoch”构造独立随机流，兼顾复现和逐 epoch 洗牌。
        rng = np.random.default_rng(self.seed + self.epoch)
        # 迭代开始后递增 epoch，下一次 DataLoader 遍历将使用不同抽样序列。
        self.epoch += 1
        # 生成固定数量批次，每批包含 P 个类别、每类 K 个样本。
        for _ in range(self.batch_count):
            # batch_indices 累积本批全部 P×K 个数据集索引。
            batch_indices: List[int] = []
            # 逐类采样，确保类别数严格平衡。
            for class_index in self.classes:
                # 读取当前类别的文件候选池。
                file_pool = self.indices_by_class_file[class_index]
                # 打乱文件顺序并优先从不同文件各取一个窗口。
                shuffled_files = rng.permutation(list(file_pool.keys())).tolist()
                # 前 min(K,文件数) 个样本来自互不相同的文件。
                for file_id in shuffled_files[: self.samples_per_class]:
                    # 在所选文件内均匀抽取一个窗口索引。
                    batch_indices.append(int(rng.choice(file_pool[int(file_id)])))
                # 文件数少于 K 时，从该类所有窗口中有放回补足，保证批形状稳定。
                missing = self.samples_per_class - min(
                    self.samples_per_class, len(shuffled_files)
                )
                # 仅在需要补样本时拼接该类全部文件窗口，避免正常路径额外分配内存。
                if missing > 0:
                    # all_indices 形状为 [当前类别全部窗口数]。
                    all_indices = np.concatenate(list(file_pool.values()))
                    # 有放回抽样允许文件不足的类别仍达到 K 个窗口。
                    batch_indices.extend(
                        int(index)
                        for index in rng.choice(all_indices, size=missing, replace=True)
                    )
            # 打乱批内类别顺序，避免优化器连续看到同类样本段。
            rng.shuffle(batch_indices)
            # 将完整 P×K 索引列表交给 DataLoader 读取特征、标签和文件编号。
            yield batch_indices


def make_loader(
    x: np.ndarray,
    y: np.ndarray,
    batch_size: int,
    shuffle: bool,
    file_ids: Optional[np.ndarray] = None,
    file_balanced: bool = False,
    pk_file_balanced: bool = False,
    pk_samples_per_class: int = 6,
    seed: int = SEED,
) -> DataLoader:
    tensors: List[torch.Tensor] = [torch.from_numpy(x).float(), torch.from_numpy(y).long()]
    if file_ids is not None:
        tensors.append(torch.from_numpy(np.asarray(file_ids, dtype=np.int64)).long())
    dataset = TensorDataset(*tensors)
    # P×K 模式优先级高于普通文件加权采样，并要求提供逐窗口文件编号。
    if pk_file_balanced:
        # 缺少文件编号时无法保证同类 K 个窗口优先来自不同采集文件。
        if file_ids is None:
            raise ValueError("file_ids are required for P x K file-balanced sampling")
        # 批采样器直接输出完整 P×K 索引列表，因此 DataLoader 不再接收 batch_size。
        batch_sampler = PKFileBatchSampler(
            y,
            file_ids,
            samples_per_class=pk_samples_per_class,
            seed=seed,
        )
        # 每批输出 (x,y,file_id)，形状分别为 [P*K,特征数]、[P*K]、[P*K]。
        return DataLoader(dataset, batch_sampler=batch_sampler)
    if file_balanced:
        if file_ids is None:
            raise ValueError("file_ids are required for file-balanced sampling")
        weights = torch.as_tensor(
            file_balanced_sample_weights(y, file_ids), dtype=torch.double
        )
        generator = torch.Generator()
        generator.manual_seed(seed)
        sampler = WeightedRandomSampler(
            weights,
            num_samples=len(y),
            replacement=True,
            generator=generator,
        )
        return DataLoader(dataset, batch_size=batch_size, sampler=sampler)
    return DataLoader(dataset, batch_size=batch_size, shuffle=shuffle)


def predict(model: nn.Module, x: np.ndarray, device: torch.device) -> np.ndarray:
    model.eval()
    preds: List[np.ndarray] = []
    loader = make_loader(x, np.zeros(len(x), dtype=np.int64), batch_size=512, shuffle=False)
    with torch.no_grad():
        for batch_x, _ in loader:
            logits = model(batch_x.to(device))
            preds.append(torch.argmax(logits, dim=1).cpu().numpy())
    return np.concatenate(preds)


def evaluate(model: nn.Module, x: np.ndarray, y: np.ndarray, device: torch.device) -> Tuple[float, float, np.ndarray]:
    y_pred = predict(model, x, device)
    acc = float(accuracy_score(y, y_pred))
    macro_f1 = float(f1_score(y, y_pred, average="macro", zero_division=0))
    return acc, macro_f1, y_pred


def weak_and_worst_f1(
    y_true: np.ndarray,
    y_pred: np.ndarray,
    class_names: Sequence[str],
) -> Tuple[float, float]:
    per_class = f1_score(
        y_true,
        y_pred,
        labels=np.arange(len(class_names)),
        average=None,
        zero_division=0,
    )
    weak_indices = [
        class_names.index(name) for name in WEAK_CLASS_NAMES if name in class_names
    ]
    weak_f1 = float(np.mean(per_class[weak_indices])) if weak_indices else float(np.mean(per_class))
    worst_f1 = float(np.min(per_class)) if len(per_class) else 0.0
    return weak_f1, worst_f1


def per_class_recalls(
    y_true: np.ndarray,
    y_pred: np.ndarray,
    class_count: int,
) -> np.ndarray:
    matrix = confusion_matrix(
        np.asarray(y_true),
        np.asarray(y_pred),
        labels=np.arange(class_count),
    )
    support = matrix.sum(axis=1)
    return np.divide(
        np.diag(matrix),
        support,
        out=np.zeros(class_count, dtype=np.float64),
        where=support > 0,
    )


def weak_and_min_recall(
    y_true: np.ndarray,
    y_pred: np.ndarray,
    class_names: Sequence[str],
) -> Tuple[float, float, np.ndarray]:
    recalls = per_class_recalls(y_true, y_pred, len(class_names))
    weak_indices = [
        class_names.index(name) for name in WEAK_CLASS_NAMES if name in class_names
    ]
    weak_recall = (
        float(np.mean(recalls[weak_indices]))
        if weak_indices
        else float(np.mean(recalls))
    )
    min_recall = float(np.min(recalls)) if len(recalls) else 0.0
    return weak_recall, min_recall, recalls


def validation_checkpoint_key(
    val_min_recall: float,
    val_weak_recall: float,
    val_f1: float,
    val_acc: float,
) -> Tuple[float, float, float, float]:
    return val_min_recall, val_weak_recall, val_f1, val_acc


def train_model(
    train_x: np.ndarray,
    train_y: np.ndarray,
    train_file_ids: np.ndarray,
    val_x: np.ndarray,
    val_y: np.ndarray,
    class_names: Sequence[str],
    device: torch.device,
    progress_label: str = "",
    ema_decay: float = 0.0,
    label_smoothing: float = 0.0,
    multi_branch: bool = False,
    pk_batches: bool = False,
    auxiliary_heads: bool = False,
    pk_prior_corrected_ce: bool = False,
    supcon_weight: float = SUPCON_WEIGHT,
    dropout: float = DROPOUT,
) -> Tuple[nn.Module, Dict[str, object]]:
    if not 0.0 <= ema_decay < 1.0:
        raise ValueError("EMA decay must be in [0, 1)")
    if not 0.0 <= label_smoothing < 1.0:
        raise ValueError("Label smoothing must be in [0, 1)")
    # SupCon 权重必须非负；0 表示保留代码路径但禁用其梯度贡献。
    if supcon_weight < 0.0:
        raise ValueError("SupCon weight must be non-negative")
    # Dropout 概率遵循 PyTorch 合同，1 会丢弃全部融合表示，因此上界不包含 1。
    if not 0.0 <= dropout < 1.0:
        raise ValueError("Dropout must be in [0, 1)")
    # 辅助头属于多分支候选模型；平铺 BP 不具备对应运动属性头。
    if auxiliary_heads and not multi_branch:
        raise ValueError("Auxiliary heads require the multi-branch model")
    class_count = len(class_names)
    # 按命令行开关选择多分支候选或兼容原 ESP32 导出器的平铺 BP。
    model: nn.Module = (
        MultiBranchBPNet(train_x.shape[1], class_count, dropout=dropout).to(device)
        if multi_branch
        else BPNet(train_x.shape[1], class_count, dropout=dropout).to(device)
    )
    ema_model = copy.deepcopy(model).to(device) if ema_decay > 0.0 else None
    ema_state: Optional[Dict[str, torch.Tensor]] = None
    # 仅在 P×K 模式显式请求时恢复原训练窗口类别先验；普通采样无需二次修正。
    ce_weights = (
        pk_ce_class_weights(train_y, class_count, device)
        if pk_batches and pk_prior_corrected_ce
        else None
    )
    # 加权交叉熵仍支持标签平滑，输出标量供主分类反向传播。
    criterion = nn.CrossEntropyLoss(
        weight=ce_weights,
        label_smoothing=label_smoothing,
    )
    optimizer = torch.optim.AdamW(model.parameters(), lr=LEARNING_RATE, weight_decay=WEIGHT_DECAY)
    loader = make_loader(
        train_x,
        train_y,
        BATCH_SIZE,
        shuffle=False,
        file_ids=train_file_ids,
        # P×K 关闭时沿用按文件反频率加权采样，保持旧训练路径不变。
        file_balanced=not pk_batches,
        # P×K 开启时每批包含全部 P 类、每类 K=6 个窗口。
        pk_file_balanced=pk_batches,
        pk_samples_per_class=6,
        seed=SEED,
    )

    best_state = copy.deepcopy(model.state_dict())
    best_score = (-float("inf"),) * 4
    best_epoch = 0
    patience_left = PATIENCE
    history: List[Dict[str, float]] = []

    for epoch in range(1, MAX_EPOCHS + 1):
        model.train()
        loss_sum = 0.0
        ce_sum = 0.0
        supcon_sum = 0.0
        margin_sum = 0.0
        # 累积五个训练期运动属性任务的加权前原始损失。
        auxiliary_sum = 0.0
        seen = 0
        for batch_x, batch_y, batch_file_ids in loader:
            batch_x = batch_x.to(device)
            batch_y = batch_y.to(device)
            batch_file_ids = batch_file_ids.to(device)
            optimizer.zero_grad(set_to_none=True)
            embeddings = model.forward_features(batch_x)
            # 两种模型均通过统一接口将 32 维嵌入映射到主类别 logits。
            logits = model.classify_features(embeddings)
            ce_loss = criterion(logits, batch_y)
            supcon_loss = cross_file_supervised_contrastive_loss(
                embeddings,
                batch_y,
                batch_file_ids,
            )
            margin_loss = hard_pair_margin_loss(logits, batch_y, class_names)
            # 仅多分支辅助模式计算五个属性头；关闭时返回与图相连的零值。
            auxiliary_loss = (
                model.auxiliary_loss(embeddings, batch_y, class_names)
                if auxiliary_heads and isinstance(model, MultiBranchBPNet)
                else embeddings.sum() * 0.0
            )
            # 总损失联合主分类、跨文件监督对比、定向间隔和训练期辅助任务。
            loss = (
                ce_loss
                + supcon_weight * supcon_loss
                + HARD_PAIR_WEIGHT * margin_loss
                + AUXILIARY_WEIGHT * auxiliary_loss
            )
            loss.backward()
            optimizer.step()
            loss_sum += float(loss.item()) * len(batch_x)
            ce_sum += float(ce_loss.item()) * len(batch_x)
            supcon_sum += float(supcon_loss.item()) * len(batch_x)
            margin_sum += float(margin_loss.item()) * len(batch_x)
            # 按批样本数累计辅助损失，供 epoch 日志计算加权平均。
            auxiliary_sum += float(auxiliary_loss.item()) * len(batch_x)
            seen += len(batch_x)

        if ema_model is not None:
            ema_state = update_ema_state(
                ema_state,
                model.state_dict(),
                ema_decay,
            )
            ema_model.load_state_dict(ema_state)
            evaluation_model = ema_model
        else:
            evaluation_model = model
        val_acc, val_f1, val_pred = evaluate(
            evaluation_model,
            val_x,
            val_y,
            device,
        )
        val_weak_f1, val_worst_f1 = weak_and_worst_f1(
            val_y, val_pred, class_names
        )
        val_weak_recall, val_min_recall, _ = weak_and_min_recall(
            val_y, val_pred, class_names
        )
        avg_loss = loss_sum / max(seen, 1)
        avg_ce = ce_sum / max(seen, 1)
        avg_supcon = supcon_sum / max(seen, 1)
        avg_margin = margin_sum / max(seen, 1)
        # epoch 辅助损失为所有已见样本的加权平均，关闭辅助头时恒为 0。
        avg_auxiliary = auxiliary_sum / max(seen, 1)
        score = validation_checkpoint_key(
            val_min_recall,
            val_weak_recall,
            val_f1,
            val_acc,
        )
        history.append(
            {
                "epoch": float(epoch),
                "loss": avg_loss,
                "ce_loss": avg_ce,
                "supcon_loss": avg_supcon,
                "margin_loss": avg_margin,
                "auxiliary_loss": avg_auxiliary,
                "val_acc": val_acc,
                "val_f1": val_f1,
                "val_weak_f1": val_weak_f1,
                "val_worst_f1": val_worst_f1,
                "val_weak_recall": val_weak_recall,
                "val_min_recall": val_min_recall,
            }
        )
        if score > best_score:
            best_score = score
            best_state = copy.deepcopy(evaluation_model.state_dict())
            best_epoch = epoch
            patience_left = PATIENCE
        else:
            patience_left -= 1
        label = f"{progress_label} " if progress_label else ""
        print(
            f"{label}epoch={epoch:03d} loss={avg_loss:.4f} "
            f"ce={avg_ce:.4f} supcon={avg_supcon:.4f} margin={avg_margin:.4f} "
            f"aux={avg_auxiliary:.4f} "
            f"ema={ema_decay:.3f} "
            f"smooth={label_smoothing:.3f} "
            f"val_acc={val_acc:.4f} val_f1={val_f1:.4f} "
            f"val_weak_f1={val_weak_f1:.4f} val_worst_f1={val_worst_f1:.4f} "
            f"val_weak_recall={val_weak_recall:.4f} val_min_recall={val_min_recall:.4f} "
            f"best_epoch={best_epoch} patience_left={patience_left}",
            flush=True,
        )
        if patience_left <= 0:
            break

    model.load_state_dict(best_state)
    return model, {
        "best_epoch": best_epoch,
        "ema_decay": ema_decay,
        "label_smoothing": label_smoothing,
        "multi_branch": multi_branch,
        "pk_batches": pk_batches,
        "auxiliary_heads": auxiliary_heads,
        "pk_prior_corrected_ce": pk_prior_corrected_ce,
        "supcon_weight": supcon_weight,
        "dropout": dropout,
        "history": history,
    }


def train_family_specialist(
    train_x_raw: np.ndarray,
    train_y: np.ndarray,
    train_file_ids: np.ndarray,
    val_x_raw: np.ndarray,
    val_y: np.ndarray,
    test_x_raw: np.ndarray,
    test_y: np.ndarray,
    class_names: Sequence[str],
    device: torch.device,
    progress_label: str,
) -> Dict[str, object]:
    family_names = [
        name for name in FAMILY_SPECIALIST_CLASS_NAMES if name in class_names
    ]
    if len(family_names) < 2:
        raise ValueError("Family specialist requires at least two configured classes")
    specialist_feature_indices = build_jump_shape_feature_indices(build_feature_names())

    train_family_raw, train_family_y, train_family_file_ids = family_subset(
        train_x_raw,
        train_y,
        train_file_ids,
        class_names,
        family_names,
    )
    val_family_raw, val_family_y, _ = family_subset(
        val_x_raw,
        val_y,
        np.arange(len(val_y), dtype=np.int64),
        class_names,
        family_names,
    )
    test_family_raw, test_family_y, _ = family_subset(
        test_x_raw,
        test_y,
        np.arange(len(test_y), dtype=np.int64),
        class_names,
        family_names,
    )
    train_family_raw = train_family_raw[:, specialist_feature_indices]
    val_family_raw = val_family_raw[:, specialist_feature_indices]
    test_family_raw = test_family_raw[:, specialist_feature_indices]
    (
        train_family_x,
        val_family_x,
        test_family_x,
        specialist_mean,
        specialist_std,
    ) = standardize(train_family_raw, val_family_raw, test_family_raw)

    print(
        f"start {progress_label} specialist=family "
        f"classes={family_names} train={len(train_family_y)} "
        f"val={len(val_family_y)} test={len(test_family_y)} "
        f"feature_dim={len(specialist_feature_indices)}",
        flush=True,
    )
    specialist_model, specialist_meta = train_model(
        train_family_x,
        train_family_y,
        train_family_file_ids,
        val_family_x,
        val_family_y,
        family_names,
        device,
        progress_label=f"{progress_label} specialist=family",
    )
    return {
        "model": specialist_model,
        "mean": specialist_mean,
        "std": specialist_std,
        "class_names": family_names,
        "feature_indices": specialist_feature_indices,
        "training": specialist_meta,
    }


def load_primary_artifacts(
    artifact_dir: Path,
    input_dim: int,
    class_count: int,
    expected_window_len: int,
    device: torch.device,
) -> Tuple[BPNet, np.ndarray, np.ndarray]:
    artifact_dir = Path(artifact_dir)
    config_path = artifact_dir / "scaler_and_config.npz"
    model_path = artifact_dir / "best_model.pt"
    with np.load(config_path, allow_pickle=False) as config:
        mean = np.asarray(config["mean"], dtype=np.float32)
        std = np.asarray(config["std"], dtype=np.float32)
        saved_window_len = int(np.asarray(config["window_len"]).reshape(-1)[0])
    if mean.shape != (input_dim,) or std.shape != (input_dim,):
        raise ValueError("Primary artifact feature dimension does not match current extractor")
    if saved_window_len != expected_window_len:
        raise ValueError(
            f"Primary artifact window_len={saved_window_len} does not match "
            f"requested window_len={expected_window_len}"
        )
    state = torch.load(model_path, map_location=device, weights_only=True)
    if isinstance(state, dict) and "primary" in state:
        state = state["primary"]
    model = BPNet(input_dim, class_count).to(device)
    model.load_state_dict(state)
    model.eval()
    return model, mean, std


def train_one_experiment(
    window_seconds: float,
    records: Sequence[ImuRecord],
    class_names: Sequence[str],
    device: torch.device,
    seed: int,
    primary_artifact_dir: Optional[Path] = None,
    enable_family_specialist: bool = False,
    validation_only: bool = False,
    extra_train_records: Sequence[ImuRecord] = (),
    ema_decay: float = 0.0,
    label_smoothing: float = 0.0,
    multi_branch: bool = False,
    pk_batches: bool = False,
    auxiliary_heads: bool = False,
    pk_prior_corrected_ce: bool = False,
    supcon_weight: float = SUPCON_WEIGHT,
    dropout: float = DROPOUT,
) -> Dict[str, object]:
    if validation_only and enable_family_specialist:
        raise ValueError("Family specialist is not supported in validation-only mode")
    # 已保存主模型当前仅支持旧平铺 BP，禁止与多分支候选混用造成结构不匹配。
    if primary_artifact_dir is not None and multi_branch:
        raise ValueError("A primary artifact cannot be loaded into the multi-branch model")
    window_len, step_len = window_lengths(window_seconds)
    train_records, val_records, test_records = split_records_for_experiment(
        records,
        extra_train_records,
        seed,
    )
    rest_threshold = estimate_rest_threshold(train_records, window_len, step_len)
    active_point_threshold = estimate_active_point_threshold(
        train_records, window_len, step_len
    )
    rng = np.random.default_rng(seed + int(window_seconds * 100))

    train_x_raw, train_y, train_file_ids, train_stats = build_samples(
        train_records,
        window_len,
        step_len,
        rest_threshold,
        active_point_threshold,
        augment=True,
        rng=rng,
        progress_label=f"window={window_seconds:.1f}s split=train",
    )
    val_x_raw, val_y, _, val_stats = build_samples(
        val_records,
        window_len,
        step_len,
        rest_threshold,
        active_point_threshold,
        augment=False,
        rng=rng,
        progress_label=f"window={window_seconds:.1f}s split=val",
    )
    if validation_only:
        test_x_raw = np.empty((0, train_x_raw.shape[1]), dtype=np.float32)
        test_y = np.empty(0, dtype=np.int64)
        test_stats: Dict[str, int] = {"skipped_validation_only": 1}
    else:
        test_x_raw, test_y, _, test_stats = build_samples(
            test_records,
            window_len,
            step_len,
            rest_threshold,
            active_point_threshold,
            augment=False,
            rng=rng,
            progress_label=f"window={window_seconds:.1f}s split=test",
        )

    print(
        f"start window={window_seconds:.1f}s window_len={window_len} step_len={step_len} "
        f"rest_threshold={rest_threshold:.5f} "
        f"active_point_threshold={active_point_threshold:.5f}",
        flush=True,
    )
    if primary_artifact_dir is None:
        train_x, val_x, test_x, mean, std = standardize(
            train_x_raw, val_x_raw, test_x_raw
        )
        model, train_meta = train_model(
            train_x,
            train_y,
            train_file_ids,
            val_x,
            val_y,
            class_names,
            device,
            progress_label=f"window={window_seconds:.1f}s",
            ema_decay=ema_decay,
            label_smoothing=label_smoothing,
            multi_branch=multi_branch,
            pk_batches=pk_batches,
            auxiliary_heads=auxiliary_heads,
            pk_prior_corrected_ce=pk_prior_corrected_ce,
            supcon_weight=supcon_weight,
            dropout=dropout,
        )
    else:
        model, mean, std = load_primary_artifacts(
            primary_artifact_dir,
            input_dim=train_x_raw.shape[1],
            class_count=len(class_names),
            expected_window_len=window_len,
            device=device,
        )
        train_x = ((train_x_raw - mean) / std).astype(np.float32)
        val_x = ((val_x_raw - mean) / std).astype(np.float32)
        test_x = ((test_x_raw - mean) / std).astype(np.float32)
        train_meta = {"loaded_from": str(Path(primary_artifact_dir).resolve())}
        print(
            f"primary_model_loaded={Path(primary_artifact_dir).resolve()}",
            flush=True,
        )
    flat_val_acc, flat_val_f1, flat_val_pred = evaluate(model, val_x, val_y, device)
    if validation_only:
        flat_test_acc = float("nan")
        flat_test_f1 = float("nan")
        flat_test_pred = np.empty(0, dtype=np.int64)
    else:
        flat_test_acc, flat_test_f1, flat_test_pred = evaluate(
            model, test_x, test_y, device
        )
    training_meta: Dict[str, object] = {"primary": train_meta}
    if enable_family_specialist:
        specialist = train_family_specialist(
            train_x_raw,
            train_y,
            train_file_ids,
            val_x_raw,
            val_y,
            test_x_raw,
            test_y,
            class_names,
            device,
            progress_label=f"window={window_seconds:.1f}s",
        )
        specialist_model = specialist["model"]
        assert isinstance(specialist_model, BPNet)
        specialist_mean = np.asarray(specialist["mean"], dtype=np.float32)
        specialist_std = np.asarray(specialist["std"], dtype=np.float32)
        specialist_names = list(specialist["class_names"])
        specialist_feature_indices = np.asarray(
            specialist["feature_indices"], dtype=np.int64
        )
        specialist_val_x = (
            (val_x_raw[:, specialist_feature_indices] - specialist_mean)
            / specialist_std
        ).astype(np.float32)
        specialist_test_x = (
            (test_x_raw[:, specialist_feature_indices] - specialist_mean)
            / specialist_std
        ).astype(np.float32)
        specialist_val_pred = predict(specialist_model, specialist_val_x, device)
        specialist_test_pred = predict(specialist_model, specialist_test_x, device)
        val_pred = route_family_predictions(
            flat_val_pred,
            specialist_val_pred,
            class_names,
            specialist_names,
        )
        test_pred = route_family_predictions(
            flat_test_pred,
            specialist_test_pred,
            class_names,
            specialist_names,
        )
        training_meta["family_specialist"] = specialist["training"]
    else:
        specialist_model = None
        specialist_mean = np.empty(0, dtype=np.float32)
        specialist_std = np.empty(0, dtype=np.float32)
        specialist_names = []
        specialist_feature_indices = np.empty(0, dtype=np.int64)
        val_pred = flat_val_pred
        test_pred = flat_test_pred
    val_acc = float(accuracy_score(val_y, val_pred))
    val_f1 = float(f1_score(val_y, val_pred, average="macro", zero_division=0))
    if validation_only:
        test_acc = float("nan")
        test_f1 = float("nan")
    else:
        test_acc = float(accuracy_score(test_y, test_pred))
        test_f1 = float(
            f1_score(test_y, test_pred, average="macro", zero_division=0)
        )
    val_weak_recall, val_min_recall, val_recalls = weak_and_min_recall(
        val_y, val_pred, class_names
    )
    if validation_only:
        test_acc = float("nan")
        test_weak_recall = float("nan")
        test_min_recall = float("nan")
        test_recalls = np.full(len(class_names), np.nan, dtype=np.float64)
    else:
        test_weak_recall, test_min_recall, test_recalls = weak_and_min_recall(
            test_y, test_pred, class_names
        )

    if validation_only:
        print(
            f"window={window_seconds:.1f}s train={len(train_y)} val={len(val_y)} "
            f"val_acc={val_acc:.4f} val_f1={val_f1:.4f} "
            f"val_min_recall={val_min_recall:.4f} "
            "validation_only=true test_evaluation_skipped=true"
        )
    else:
        print(
            f"window={window_seconds:.1f}s "
            f"train={len(train_y)} val={len(val_y)} test={len(test_y)} "
            f"val_acc={val_acc:.4f} val_f1={val_f1:.4f} "
            f"val_min_recall={val_min_recall:.4f} "
            f"test_acc={test_acc:.4f} test_f1={test_f1:.4f} "
            f"test_min_recall={test_min_recall:.4f}"
        )

    return {
        "window_seconds": window_seconds,
        "window_len": window_len,
        "step_len": step_len,
        "rest_threshold": rest_threshold,
        "active_point_threshold": active_point_threshold,
        "ema_decay": ema_decay,
        "label_smoothing": label_smoothing,
        "multi_branch": multi_branch,
        "pk_batches": pk_batches,
        "auxiliary_heads": auxiliary_heads,
        "pk_prior_corrected_ce": pk_prior_corrected_ce,
        "supcon_weight": supcon_weight,
        "dropout": dropout,
        "model": model,
        "specialist_model": specialist_model,
        "specialist_mean": specialist_mean,
        "specialist_std": specialist_std,
        "specialist_class_names": specialist_names,
        "specialist_feature_indices": specialist_feature_indices,
        "mean": mean,
        "std": std,
        "flat_val_acc": flat_val_acc,
        "flat_val_f1": flat_val_f1,
        "flat_test_acc": flat_test_acc,
        "flat_test_f1": flat_test_f1,
        "val_acc": val_acc,
        "val_f1": val_f1,
        "val_weak_recall": val_weak_recall,
        "val_min_recall": val_min_recall,
        "val_class_recalls": {
            name: float(value) for name, value in zip(class_names, val_recalls)
        },
        "test_acc": test_acc,
        "test_f1": test_f1,
        "test_weak_recall": test_weak_recall,
        "test_min_recall": test_min_recall,
        "test_class_recalls": {
            name: float(value) for name, value in zip(class_names, test_recalls)
        },
        "val_pred": val_pred,
        "test_pred": test_pred,
        "y_val": val_y,
        "y_test": test_y,
        "train_sample_count": int(len(train_y)),
        "val_sample_count": int(len(val_y)),
        "test_sample_count": int(len(test_y)),
        "train_file_count": len(train_records),
        "val_file_count": len(val_records),
        "test_file_count": 0 if validation_only else len(test_records),
        "train_files": [str(record.path) for record in train_records],
        "val_files": [str(record.path) for record in val_records],
        "test_files": (
            [] if validation_only else [str(record.path) for record in test_records]
        ),
        "sample_stats": {"train": train_stats, "val": val_stats, "test": test_stats},
        "training": training_meta,
    }


def evaluate_external_holdout(
    best_result: Dict[str, object],
    records: Sequence[ImuRecord],
    class_names: Sequence[str],
    device: torch.device,
    validation_only: bool = False,
) -> Dict[str, object]:
    if validation_only:
        return {"skipped": True, "reason": "validation_only"}
    if not records:
        return {"skipped": True, "reason": "no_external_holdout"}
    labels = {record.label for record in records}
    unknown_labels = labels.difference(class_names)
    if unknown_labels:
        raise ValueError(
            "External holdout contains unknown labels: "
            + ", ".join(sorted(unknown_labels))
        )

    window_len = int(best_result["window_len"])
    step_len = int(best_result["step_len"])
    rest_threshold = float(best_result["rest_threshold"])
    active_point_threshold = float(best_result["active_point_threshold"])
    raw_x, y_true, _, stats = build_samples(
        records,
        window_len,
        step_len,
        rest_threshold,
        active_point_threshold,
        augment=False,
        rng=np.random.default_rng(SEED),
        progress_label="external_holdout=" + ",".join(sorted(labels)),
    )
    if len(y_true) == 0:
        return {
            "skipped": True,
            "reason": "no_kept_windows",
            "file_count": len(records),
            "files": [str(record.path) for record in records],
            "sample_stats": stats,
        }
    mean = np.asarray(best_result["mean"], dtype=np.float32)
    std = np.asarray(best_result["std"], dtype=np.float32)
    x = ((raw_x - mean) / std).astype(np.float32)
    model = best_result["model"]
    # 外部留出集允许评估平铺 BP 或多分支候选，两者均实现 nn.Module 前向接口。
    assert isinstance(model, nn.Module)
    y_pred = predict(model, x, device)
    present_labels = [name for name in class_names if name in labels]
    class_recalls = {}
    for label in present_labels:
        label_idx = class_names.index(label)
        target = y_true == label_idx
        class_recalls[label] = float(np.mean(y_pred[target] == label_idx))
    recalls = list(class_recalls.values())
    report = {
        "skipped": False,
        "file_count": len(records),
        "sample_count": int(len(y_true)),
        "class_recalls": class_recalls,
        "min_recall": float(min(recalls)),
        "macro_recall": float(np.mean(recalls)),
        "files": [str(record.path) for record in records],
        "sample_stats": stats,
    }
    if len(present_labels) == 1:
        report["label"] = present_labels[0]
        report["recall"] = class_recalls[present_labels[0]]
    return report


def save_confusion_matrix(
    y_true: np.ndarray,
    y_pred: np.ndarray,
    class_names: Sequence[str],
    save_path: Path,
) -> None:
    matrix = confusion_matrix(y_true, y_pred, labels=list(range(len(class_names))))
    fig_w = max(8.0, len(class_names) * 0.75)
    fig, ax = plt.subplots(figsize=(fig_w, fig_w))
    im = ax.imshow(matrix, cmap="Blues")
    fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04)
    ax.set_xticks(np.arange(len(class_names)))
    ax.set_yticks(np.arange(len(class_names)))
    ax.set_xticklabels(class_names, rotation=45, ha="right", fontsize=8)
    ax.set_yticklabels(class_names, fontsize=8)
    ax.set_xlabel("Predicted")
    ax.set_ylabel("True")
    for i in range(matrix.shape[0]):
        for j in range(matrix.shape[1]):
            ax.text(j, i, int(matrix[i, j]), ha="center", va="center", fontsize=7)
    fig.tight_layout()
    fig.savefig(save_path, dpi=180)
    plt.close(fig)


def c_float(value: float) -> str:
    if not np.isfinite(value):
        value = 0.0
    literal = f"{float(value):.9g}"
    if "." not in literal and "e" not in literal.lower():
        literal += ".0"
    return f"{literal}f"


def c_string(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def c_array_1d(name: str, data: np.ndarray, const_type: str = "float") -> str:
    values = ", ".join(c_float(v) for v in np.asarray(data).reshape(-1))
    return f"static const {const_type} {name}[{len(np.asarray(data).reshape(-1))}] = {{ {values} }};"


def c_array_2d(name: str, data: np.ndarray) -> str:
    arr = np.asarray(data)
    rows = []
    for row in arr:
        rows.append("  { " + ", ".join(c_float(v) for v in row) + " }")
    return f"static const float {name}[{arr.shape[0]}][{arr.shape[1]}] = {{\n" + ",\n".join(rows) + "\n};"


def export_esp32_header(
    result: Dict[str, object],
    class_names: Sequence[str],
    feature_names: Sequence[str],
    save_path: Path,
) -> None:
    model = result["model"]
    assert isinstance(model, BPNet)
    state = model.state_dict()
    w1 = state["net.0.weight"].cpu().numpy()
    b1 = state["net.0.bias"].cpu().numpy()
    w2 = state["net.3.weight"].cpu().numpy()
    b2 = state["net.3.bias"].cpu().numpy()
    w3 = state["net.6.weight"].cpu().numpy()
    b3 = state["net.6.bias"].cpu().numpy()
    w4 = state["net.8.weight"].cpu().numpy()
    b4 = state["net.8.bias"].cpu().numpy()

    specialist_model = result.get("specialist_model")
    has_specialist = isinstance(specialist_model, BPNet)
    specialist_names = list(result.get("specialist_class_names", []))
    specialist_lines: List[str] = []
    specialist_feature_dim = 0
    if has_specialist:
        assert isinstance(specialist_model, BPNet)
        specialist_state = specialist_model.state_dict()
        specialist_global_indices = [class_names.index(name) for name in specialist_names]
        specialist_feature_indices = np.asarray(
            result.get("specialist_feature_indices", np.arange(len(feature_names))),
            dtype=np.int64,
        )
        specialist_feature_dim = int(len(specialist_feature_indices))
        if specialist_state["net.0.weight"].shape[1] != specialist_feature_dim:
            raise ValueError("Specialist feature index count does not match model input")
        specialist_lines = [
            "static const int SPECIALIST_GLOBAL_CLASS_INDEX[SPECIALIST_CLASS_NUM] = { "
            + ", ".join(str(index) for index in specialist_global_indices)
            + " };",
            "static const int SPECIALIST_FEATURE_INDEX[SPECIALIST_FEATURE_DIM] = { "
            + ", ".join(str(int(index)) for index in specialist_feature_indices)
            + " };",
            c_array_1d(
                "SPECIALIST_FEATURE_MEAN",
                np.asarray(result["specialist_mean"], dtype=np.float32),
            ),
            c_array_1d(
                "SPECIALIST_FEATURE_STD",
                np.asarray(result["specialist_std"], dtype=np.float32),
            ),
            c_array_2d("SW1", specialist_state["net.0.weight"].cpu().numpy()),
            c_array_1d("SB1", specialist_state["net.0.bias"].cpu().numpy()),
            c_array_2d("SW2", specialist_state["net.3.weight"].cpu().numpy()),
            c_array_1d("SB2", specialist_state["net.3.bias"].cpu().numpy()),
            c_array_2d("SW3", specialist_state["net.6.weight"].cpu().numpy()),
            c_array_1d("SB3", specialist_state["net.6.bias"].cpu().numpy()),
            c_array_2d("SW4", specialist_state["net.8.weight"].cpu().numpy()),
            c_array_1d("SB4", specialist_state["net.8.bias"].cpu().numpy()),
        ]

    mean = np.asarray(result["mean"], dtype=np.float32)
    std = np.asarray(result["std"], dtype=np.float32)
    window_len = int(result["window_len"])
    rest_threshold = float(result.get("rest_threshold", 0.03))
    active_point_threshold = float(result.get("active_point_threshold", 0.02))

    lines = [
        "#ifndef ESP32_BP_MODEL_H",
        "#define ESP32_BP_MODEL_H",
        "",
        "#include <math.h>",
        "#include <stdint.h>",
        "",
        f"#define WINDOW_LEN {window_len}",
        "#define AXIS_NUM 6",
        f"#define SAMPLE_RATE_HZ {SAMPLE_RATE}",
        f"#define FEATURE_DIM {len(feature_names)}",
        f"#define CLASS_NUM {len(class_names)}",
        f"#define HAS_FAMILY_SPECIALIST {1 if has_specialist else 0}",
        f"#define SPECIALIST_CLASS_NUM {len(specialist_names) if has_specialist else 0}",
        f"#define SPECIALIST_FEATURE_DIM {specialist_feature_dim}",
        f"#define HIDDEN1 {HIDDEN1}",
        f"#define HIDDEN2 {HIDDEN2}",
        f"#define HIDDEN3 {HIDDEN3}",
        f"#define PHASE_SEGMENTS {PHASE_SEGMENTS}",
        "",
        f"static const float REST_MOTION_THRESHOLD = {c_float(rest_threshold)};",
        f"static const float ACTIVE_POINT_THRESHOLD = {c_float(active_point_threshold)};",
        "static const float HIGH_DYNAMIC_MIN_RATIO = 0.2f;",
        "",
        "static const char* CLASS_NAMES[CLASS_NUM] = { "
        + ", ".join(c_string(name) for name in class_names)
        + " };",
        c_array_1d("FEATURE_MEAN", mean),
        c_array_1d("FEATURE_STD", std),
        c_array_2d("W1", w1),
        c_array_1d("B1", b1),
        c_array_2d("W2", w2),
        c_array_1d("B2", b2),
        c_array_2d("W3", w3),
        c_array_1d("B3", b3),
        c_array_2d("W4", w4),
        c_array_1d("B4", b4),
        *specialist_lines,
        r"""
static inline void append_series_features(const float* x, int n, float* feature, int* idx) {
    float sum = 0.0f;
    float sum2 = 0.0f;
    float min_v = x[0];
    float max_v = x[0];
    for (int i = 0; i < n; i++) {
        float v = x[i];
        sum += v;
        sum2 += v * v;
        if (v < min_v) min_v = v;
        if (v > max_v) max_v = v;
    }
    float mean = sum / (float)n;
    float energy = sum2 / (float)n;
    float var = energy - mean * mean;
    if (var < 0.0f) var = 0.0f;
    float std = sqrtf(var);
    float mean_abs_diff = 0.0f;
    float diff_sum = 0.0f;
    float diff_sum2 = 0.0f;
    float zcr_count = 0.0f;
    for (int i = 1; i < n; i++) {
        float diff = x[i] - x[i - 1];
        mean_abs_diff += fabsf(diff);
        diff_sum += diff;
        diff_sum2 += diff * diff;
        float a = x[i - 1] - mean;
        float b = x[i] - mean;
        if (a * b < 0.0f) zcr_count += 1.0f;
    }
    float std_diff = 0.0f;
    if (n > 1) {
        mean_abs_diff = mean_abs_diff / (float)(n - 1);
        float mean_diff = diff_sum / (float)(n - 1);
        float diff_var = diff_sum2 / (float)(n - 1) - mean_diff * mean_diff;
        if (diff_var < 0.0f) diff_var = 0.0f;
        std_diff = sqrtf(diff_var);
        zcr_count = zcr_count / (float)(n - 1);
    }
    feature[(*idx)++] = mean;
    feature[(*idx)++] = std;
    feature[(*idx)++] = min_v;
    feature[(*idx)++] = max_v;
    feature[(*idx)++] = sqrtf(energy);
    feature[(*idx)++] = mean_abs_diff;
    feature[(*idx)++] = zcr_count;
    feature[(*idx)++] = std_diff;
}

static inline void append_phase_features(const float* x, int n, float* feature, int* idx) {
    for (int phase = 0; phase < PHASE_SEGMENTS; phase++) {
        int start = (phase * n) / PHASE_SEGMENTS;
        int end = ((phase + 1) * n) / PHASE_SEGMENTS;
        if (end <= start) end = start + 1;
        if (end > n) end = n;
        float sum = 0.0f;
        float sum2 = 0.0f;
        float max_abs = 0.0f;
        for (int i = start; i < end; i++) {
            float value = x[i];
            sum += value;
            sum2 += value * value;
            float abs_value = fabsf(value);
            if (abs_value > max_abs) max_abs = abs_value;
        }
        int count = end - start;
        float mean = sum / (float)count;
        float variance = sum2 / (float)count - mean * mean;
        if (variance < 0.0f) variance = 0.0f;
        feature[(*idx)++] = mean;
        feature[(*idx)++] = sqrtf(variance);
        feature[(*idx)++] = max_abs;
    }
}

static inline void append_normalized_phase_features(const float* x, int n, float* feature, int* idx) {
    float sum = 0.0f;
    float sum2 = 0.0f;
    float normalized[WINDOW_LEN];
    for (int i = 0; i < n; i++) {
        sum += x[i];
        sum2 += x[i] * x[i];
    }
    float mean = sum / (float)n;
    float variance = sum2 / (float)n - mean * mean;
    if (variance < 0.0f) variance = 0.0f;
    float std = sqrtf(variance);
    for (int i = 0; i < n; i++) {
        normalized[i] = std > 1e-6f ? (x[i] - mean) / std : 0.0f;
    }
    append_phase_features(normalized, n, feature, idx);
}

static inline void append_impact_distribution_features(const float* x, int n, float* feature, int* idx) {
    float ordered[WINDOW_LEN];
    float sum = 0.0f;
    float sum2 = 0.0f;
    float max_abs_diff = 0.0f;
    for (int i = 0; i < n; i++) {
        ordered[i] = x[i];
        sum += x[i];
        sum2 += x[i] * x[i];
        if (i > 0) {
            float abs_diff = fabsf(x[i] - x[i - 1]);
            if (abs_diff > max_abs_diff) max_abs_diff = abs_diff;
        }
    }
    for (int i = 1; i < n; i++) {
        float value = ordered[i];
        int j = i - 1;
        while (j >= 0 && ordered[j] > value) {
            ordered[j + 1] = ordered[j];
            j--;
        }
        ordered[j + 1] = value;
    }
    const float fractions[5] = { 0.10f, 0.25f, 0.50f, 0.75f, 0.90f };
    for (int q = 0; q < 5; q++) {
        int position = (int)floorf(fractions[q] * (float)(n - 1) + 0.5f);
        feature[(*idx)++] = ordered[position];
    }
    float mean = sum / (float)n;
    float variance = sum2 / (float)n - mean * mean;
    if (variance < 0.0f) variance = 0.0f;
    float std = sqrtf(variance);
    float skew = 0.0f;
    float kurtosis = 0.0f;
    if (std > 1e-6f) {
        for (int i = 0; i < n; i++) {
            float z = (x[i] - mean) / std;
            float z2 = z * z;
            skew += z2 * z;
            kurtosis += z2 * z2;
        }
        skew /= (float)n;
        kurtosis = kurtosis / (float)n - 3.0f;
    }
    feature[(*idx)++] = skew;
    feature[(*idx)++] = kurtosis;
    feature[(*idx)++] = max_abs_diff;
}

static inline void append_temporal_features(const float* x, int n, float* feature, int* idx) {
    float sum = 0.0f;
    float sum2 = 0.0f;
    for (int i = 0; i < n; i++) {
        sum += x[i];
        sum2 += x[i] * x[i];
    }
    float mean = sum / (float)n;
    float variance = sum2 / (float)n - mean * mean;
    if (variance < 0.0f) variance = 0.0f;
    float std = sqrtf(variance);
    int high_count = 0;
    int peak_count = 0;
    for (int i = 0; i < n; i++) {
        float activity = fabsf(x[i] - mean);
        if (std > 1e-6f && activity > std) high_count++;
        if (i > 0 && i < n - 1 && std > 1e-6f) {
            float previous = fabsf(x[i - 1] - mean);
            float next = fabsf(x[i + 1] - mean);
            if (activity > previous && activity >= next && activity > std) peak_count++;
        }
    }

    int frequency_bin_count = n / 2;
    float spectral_power[WINDOW_LEN / 2 + 1];
    float total_power = 0.0f;
    float dominant_power = -1.0f;
    int dominant_bin = 0;
    const float two_pi = 6.2831853071795864769f;
    for (int k = 1; k <= frequency_bin_count; k++) {
        float real = 0.0f;
        float imaginary = 0.0f;
        for (int sample = 0; sample < n; sample++) {
            float centered = x[sample] - mean;
            float angle = two_pi * (float)k * (float)sample / (float)n;
            real += centered * cosf(angle);
            imaginary -= centered * sinf(angle);
        }
        float power = real * real + imaginary * imaginary;
        spectral_power[k - 1] = power;
        total_power += power;
        if (power > dominant_power) {
            dominant_power = power;
            dominant_bin = k;
        }
    }
    float dominant_frequency_hz = 0.0f;
    float spectral_entropy = 0.0f;
    if (total_power > 1e-12f && frequency_bin_count > 0) {
        dominant_frequency_hz = (float)dominant_bin * (float)SAMPLE_RATE_HZ / (float)n;
        if (frequency_bin_count > 1) {
            for (int k = 0; k < frequency_bin_count; k++) {
                float probability = spectral_power[k] / total_power;
                if (probability > 0.0f) {
                    spectral_entropy -= probability * logf(probability);
                }
            }
            spectral_entropy /= logf((float)frequency_bin_count);
        }
    }

    int lag_start = (int)(0.15f * (float)SAMPLE_RATE_HZ + 0.5f);
    if (lag_start < 1) lag_start = 1;
    if (lag_start > n - 1) lag_start = n - 1;
    int lag_end = n / 2;
    int max_lag = (int)(1.20f * (float)SAMPLE_RATE_HZ + 0.5f);
    if (lag_end > max_lag) lag_end = max_lag;
    float autocorr_peak = 0.0f;
    int autocorr_peak_lag = 0;
    if (std > 1e-6f && lag_end >= lag_start) {
        float best_correlation = -1.0f;
        int best_lag = lag_start;
        for (int lag = lag_start; lag <= lag_end; lag++) {
            float dot = 0.0f;
            float left_energy = 0.0f;
            float right_energy = 0.0f;
            for (int i = 0; i < n - lag; i++) {
                float left = x[i] - mean;
                float right = x[i + lag] - mean;
                dot += left * right;
                left_energy += left * left;
                right_energy += right * right;
            }
            float denominator = sqrtf(left_energy * right_energy);
            float correlation = denominator > 1e-12f ? dot / denominator : 0.0f;
            if (correlation > best_correlation) {
                best_correlation = correlation;
                best_lag = lag;
            }
        }
        autocorr_peak = best_correlation;
        autocorr_peak_lag = best_lag;
    }

    feature[(*idx)++] = std > 1e-6f ? (float)high_count / (float)n : 0.0f;
    feature[(*idx)++] = (float)peak_count / (float)n;
    feature[(*idx)++] = dominant_frequency_hz;
    feature[(*idx)++] = spectral_entropy;
    feature[(*idx)++] = autocorr_peak;
    feature[(*idx)++] = (float)autocorr_peak_lag / (float)SAMPLE_RATE_HZ;
}

/*
 * 对单通道窗口执行去均值、Hann 加窗和单边直接 DFT。
 * x 长度为 n，单位可为 g、deg/s 或 g/采样点；三个频带比和峰比无量纲，质心单位为 Hz。
 * 频带使用 [0.35,1.20)、[1.20,2.40)、[2.40,5.00) Hz；总功率过小时五个输出均为 0。
 * 时间复杂度 O(n^2)，额外空间 O(1)；公式见 docs/弱类频谱与峰形特征说明.md。
 */
static inline void selected_spectral_features(
    const float* x, /* 指向 n 个连续单通道采样值，生命周期覆盖本函数调用。 */
    int n, /* 输入采样点数，正式推理时等于 WINDOW_LEN=62。 */
    float* low_ratio, /* 输出 0.35-1.20 Hz 功率占比，不能为空。 */
    float* mid_ratio, /* 输出 1.20-2.40 Hz 功率占比，不能为空。 */
    float* high_ratio, /* 输出 2.40-5.00 Hz 功率占比，不能为空。 */
    float* centroid_hz, /* 输出非直流谱质心，单位 Hz，不能为空。 */
    float* peak_power_ratio /* 输出最大单频功率占比，不能为空。 */
) {
    /* mean 累加并保存输入窗口均值，用于消除直流偏置。 */
    float mean = 0.0f;
    /* 遍历 n 个采样点并累计原始输入。 */
    for (int i = 0; i < n; i++) mean += x[i];
    /* 除以采样点数得到与输入同单位的窗口均值。 */
    mean /= (float)n;
    /* total_power 累加 k=1..floor(n/2) 的全部非直流功率。 */
    float total_power = 0.0f;
    /* low_power 累加 0.35<=f<1.20 Hz 的功率。 */
    float low_power = 0.0f;
    /* mid_power 累加 1.20<=f<2.40 Hz 的功率。 */
    float mid_power = 0.0f;
    /* high_power 累加 2.40<=f<5.00 Hz 的功率。 */
    float high_power = 0.0f;
    /* weighted_frequency 累加 f_k*P[k]，用于计算谱质心。 */
    float weighted_frequency = 0.0f;
    /* peak_power 保存最大非直流单频功率。 */
    float peak_power = 0.0f;
    /* two_pi 是 DFT 和 Hann 窗共同使用的 2*pi 单精度常量。 */
    const float two_pi = 6.2831853071795864769f;
    /* 遍历单边频点 k=1..floor(n/2)，显式跳过直流频点 k=0。 */
    for (int k = 1; k <= n / 2; k++) {
        /* real 累加当前频点 DFT 的实部。 */
        float real = 0.0f;
        /* imaginary 累加当前频点 DFT 的虚部。 */
        float imaginary = 0.0f;
        /* 遍历全部时域采样，计算当前频点的直接 DFT。 */
        for (int sample = 0; sample < n; sample++) {
            /* Hann 系数降低有限窗口边界频谱泄漏；n<=1 时回退为 1。 */
            float hann = n > 1
                ? 0.5f - 0.5f * cosf(two_pi * (float)sample / (float)(n - 1))
                : 1.0f;
            /* value 是去均值并加窗后的当前采样，单位继承输入。 */
            float value = (x[sample] - mean) * hann;
            /* angle 是当前频点和采样位置对应的 DFT 相位，单位 rad。 */
            float angle = two_pi * (float)k * (float)sample / (float)n;
            /* 按 cos 分量累计 DFT 实部。 */
            real += value * cosf(angle);
            /* 按 -sin 分量累计 DFT 虚部，与 numpy.fft.rfft 符号一致。 */
            imaginary -= value * sinf(angle);
        }
        /* power=real^2+imaginary^2，单位为输入单位平方。 */
        float power = real * real + imaginary * imaginary;
        /* frequency=k*fs/n，单位 Hz，范围为 (0,fs/2]。 */
        float frequency = (float)k * (float)SAMPLE_RATE_HZ / (float)n;
        /* 累加全部非直流功率，作为比例和质心的共同分母。 */
        total_power += power;
        /* 当前功率更大时更新主谱峰功率。 */
        if (power > peak_power) peak_power = power;
        /* 累加频率加权功率，最终除以总功率得到 Hz 质心。 */
        weighted_frequency += frequency * power;
        /* 半开低频带内的功率累加到 low_power。 */
        if (frequency >= 0.35f && frequency < 1.20f) low_power += power;
        /* 半开中频带内的功率累加到 mid_power。 */
        if (frequency >= 1.20f && frequency < 2.40f) mid_power += power;
        /* 半开高频带内的功率累加到 high_power。 */
        if (frequency >= 2.40f && frequency < 5.00f) high_power += power;
    }
    /* 总功率有效时计算五个有限输出。 */
    if (total_power > 1e-12f) {
        /* 低频功率除以全部非直流功率，理论范围 [0,1]。 */
        *low_ratio = low_power / total_power;
        /* 中频功率除以全部非直流功率，理论范围 [0,1]。 */
        *mid_ratio = mid_power / total_power;
        /* 高频功率除以全部非直流功率，理论范围 [0,1]。 */
        *high_ratio = high_power / total_power;
        /* 频率加权功率除以总功率，输出范围 [0,fs/2] Hz。 */
        *centroid_hz = weighted_frequency / total_power;
        /* 最大单频功率除以总功率，理论范围 [0,1]。 */
        *peak_power_ratio = peak_power / total_power;
    } else {
        /* 近静止或常量窗口没有可靠低频比例，定义为 0。 */
        *low_ratio = 0.0f;
        /* 近静止或常量窗口没有可靠中频比例，定义为 0。 */
        *mid_ratio = 0.0f;
        /* 近静止或常量窗口没有可靠高频比例，定义为 0。 */
        *high_ratio = 0.0f;
        /* 近静止或常量窗口没有可靠谱质心，定义为 0 Hz。 */
        *centroid_hz = 0.0f;
        /* 近静止或常量窗口没有可靠主谱峰比例，定义为 0。 */
        *peak_power_ratio = 0.0f;
    }
}

/*
 * 计算单通道 Hann 加窗频谱的二次谐波功率/主谱峰功率。
 * 输入 x 长度为 n，输出无量纲；主峰功率不大于 1e-12 时返回 0。
 * 时间复杂度 O(n^2)，额外空间为 WINDOW_LEN/2+1 个 float。
 */
static inline float spectral_second_harmonic_ratio(const float* x, int n) {
    /* mean 累加并保存输入窗口均值，用于去除直流偏置。 */
    float mean = 0.0f;
    /* 遍历 n 个采样点并累计原始输入。 */
    for (int i = 0; i < n; i++) mean += x[i];
    /* 除以 n 得到与输入同单位的全窗均值。 */
    mean /= (float)n;
    /* powers 按频点索引保存 k=1..floor(n/2) 的单边功率。 */
    float powers[WINDOW_LEN / 2 + 1];
    /* peak_power 保存当前最大非直流单频功率。 */
    float peak_power = 0.0f;
    /* peak_index 保存主谱峰索引；平局时因严格大于判断保留最早索引。 */
    int peak_index = 1;
    /* two_pi 是 Hann 窗和 DFT 使用的 2*pi 常量。 */
    const float two_pi = 6.2831853071795864769f;
    /* 遍历全部单边非直流频点。 */
    for (int k = 1; k <= n / 2; k++) {
        /* real 累加当前频点的 DFT 实部。 */
        float real = 0.0f;
        /* imaginary 累加当前频点的 DFT 虚部。 */
        float imaginary = 0.0f;
        /* 遍历全部时域采样并执行直接 DFT。 */
        for (int sample = 0; sample < n; sample++) {
            /* Hann 系数与 Python np.hanning 完全一致。 */
            float hann = n > 1
                ? 0.5f - 0.5f * cosf(two_pi * (float)sample / (float)(n - 1))
                : 1.0f;
            /* value 是去均值并加窗后的当前采样。 */
            float value = (x[sample] - mean) * hann;
            /* angle 是频点 k 和采样 sample 对应的 DFT 相位，单位 rad。 */
            float angle = two_pi * (float)k * (float)sample / (float)n;
            /* 按余弦分量累计实部。 */
            real += value * cosf(angle);
            /* 按负正弦分量累计虚部，与 numpy.fft.rfft 符号一致。 */
            imaginary -= value * sinf(angle);
        }
        /* 当前功率为实部平方与虚部平方之和。 */
        float power = real * real + imaginary * imaginary;
        /* 保存当前频点功率，后续按二次谐波索引读取。 */
        powers[k] = power;
        /* 严格更大时更新主峰，保证平局时选择较低频率。 */
        if (power > peak_power) {
            /* 保存新的最大功率。 */
            peak_power = power;
            /* 保存新的主谱峰频点索引。 */
            peak_index = k;
        }
    }
    /* 主峰功率过小表示常量或近静止窗口，返回 0 防止除零。 */
    if (peak_power <= 1e-12f) return 0.0f;
    /* 等间隔 DFT 上二次谐波索引等于 2*主峰索引。 */
    int harmonic_index = 2 * peak_index;
    /* 超出 Nyquist 时取最近的最高频点，与 Python argmin 行为一致。 */
    if (harmonic_index > n / 2) harmonic_index = n / 2;
    /* 返回二次谐波功率除以主谱峰功率，输出无量纲。 */
    return powers[harmonic_index] / peak_power;
}

/*
 * 从六轴窗口计算加速度模长低于 0.70g 的总比例和最长连续比例。
 * window 通道顺序为 gx,gy,gz,ax,ay,az；仅使用后三轴 g 值，两个输出范围均为 [0,1]。
 * 时间复杂度 O(WINDOW_LEN)，额外空间 O(1)。
 */
static inline void free_flight_features_from_window(
    const float window[WINDOW_LEN][AXIS_NUM],
    float* ratio,
    float* longest_ratio
) {
    /* free_count 统计全窗加速度模长低于 0.70g 的采样点数。 */
    int free_count = 0;
    /* current_run 记录当前连续低支持力区间长度。 */
    int current_run = 0;
    /* longest_run 记录全窗最长连续低支持力区间长度。 */
    int longest_run = 0;
    /* 按时间顺序遍历 WINDOW_LEN 个六轴采样点。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* ax 是传感器 x 轴加速度，单位 g。 */
        float ax = window[i][3];
        /* ay 是传感器 y 轴加速度，单位 g。 */
        float ay = window[i][4];
        /* az 是传感器 z 轴加速度，单位 g。 */
        float az = window[i][5];
        /* acc_mag 是三轴加速度欧氏模长，单位 g 且非负。 */
        float acc_mag = sqrtf(ax * ax + ay * ay + az * az);
        /* 严格低于 0.70g 时判为低支持力/腾空候选点，与 Python 一致。 */
        if (acc_mag < 0.70f) {
            /* 累加低支持力总点数。 */
            free_count++;
            /* 当前连续区间增加一个采样点。 */
            current_run++;
            /* 当前连续长度超过历史最大值时更新。 */
            if (current_run > longest_run) longest_run = current_run;
        } else {
            /* 不满足阈值时终止当前连续区间并重置为 0。 */
            current_run = 0;
        }
    }
    /* 总点数除以窗口长度得到无量纲低支持力比例。 */
    *ratio = (float)free_count / (float)WINDOW_LEN;
    /* 最长连续点数除以窗口长度得到无量纲连续区间比例。 */
    *longest_ratio = (float)longest_run / (float)WINDOW_LEN;
}

static inline float autocorr_first_zero_seconds(const float* x, int n) {
    float mean = 0.0f;
    for (int i = 0; i < n; i++) mean += x[i];
    mean /= (float)n;
    float energy = 0.0f;
    for (int i = 0; i < n; i++) {
        float centered = x[i] - mean;
        energy += centered * centered;
    }
    if (energy <= 1e-12f) return 0.0f;
    int max_lag = n / 2;
    int three_seconds = 3 * SAMPLE_RATE_HZ;
    if (max_lag > three_seconds) max_lag = three_seconds;
    for (int lag = 1; lag <= max_lag; lag++) {
        float dot = 0.0f;
        for (int i = 0; i < n - lag; i++) {
            dot += (x[i] - mean) * (x[i + lag] - mean);
        }
        if (dot <= 0.0f) return (float)lag / (float)SAMPLE_RATE_HZ;
    }
    return (float)max_lag / (float)SAMPLE_RATE_HZ;
}

static inline float autocorr_secondary_peak(const float* x, int n) {
    float mean = 0.0f;
    for (int i = 0; i < n; i++) mean += x[i];
    mean /= (float)n;
    float energy = 0.0f;
    for (int i = 0; i < n; i++) {
        float centered = x[i] - mean;
        energy += centered * centered;
    }
    if (energy <= 1e-12f) return 0.0f;
    int max_lag = n / 2;
    int three_seconds = 3 * SAMPLE_RATE_HZ;
    if (max_lag > three_seconds) max_lag = three_seconds;
    float autocorr[WINDOW_LEN / 2];
    for (int lag = 1; lag <= max_lag; lag++) {
        float dot = 0.0f;
        for (int i = 0; i < n - lag; i++) {
            dot += (x[i] - mean) * (x[i + lag] - mean);
        }
        autocorr[lag - 1] = dot / energy;
    }
    float secondary_peak = 0.0f;
    for (int i = 1; i < max_lag - 1; i++) {
        float value = autocorr[i];
        if (
            value > autocorr[i - 1] &&
            value >= autocorr[i + 1] &&
            value >= 0.20f &&
            value > secondary_peak
        ) {
            secondary_peak = value;
        }
    }
    return secondary_peak;
}

/*
 * 统计归一化自相关中不小于 0.20 的显著局部峰数量。
 * 输入 x 长度为 n，物理单位在 C[lag]/C[0] 中抵消；输出为无量纲非负整数的 float 表示。
 * 搜索最多 min(n/2,3 秒) 个延迟，时间复杂度 O(n^2)，额外空间 O(n)。
 */
static inline float autocorr_prominent_peak_count(const float* x, int n) {
    /* 累加全窗均值，用于去除静态偏置。 */
    float mean = 0.0f;
    /* 遍历 n 个采样点并累计输入值。 */
    for (int i = 0; i < n; i++) mean += x[i];
    /* 将总和除以 n 得到与输入同单位的均值。 */
    mean /= (float)n;
    /* energy 保存零延迟去均值能量 C[0]，单位为输入单位平方。 */
    float energy = 0.0f;
    /* 遍历窗口累计去均值平方和。 */
    for (int i = 0; i < n; i++) {
        /* centered 是当前采样相对全窗均值的动态分量。 */
        float centered = x[i] - mean;
        /* 累加动态能量，后续作为自相关归一化分母。 */
        energy += centered * centered;
    }
    /* 近常量序列没有可靠周期结构，返回 0 防止除零。 */
    if (energy <= 1e-12f) return 0.0f;
    /* 延迟上限先取半窗，保证每个点积仍有足够重叠样本。 */
    int max_lag = n / 2;
    /* 三秒延迟对应 3*SAMPLE_RATE_HZ 个采样点。 */
    int three_seconds = 3 * SAMPLE_RATE_HZ;
    /* 超过三秒时截断搜索，限制噪声和 ESP32 运算量。 */
    if (max_lag > three_seconds) max_lag = three_seconds;
    /* autocorr 保存 lag=1..max_lag 的归一化自相关，最大占用 WINDOW_LEN/2 个 float。 */
    float autocorr[WINDOW_LEN / 2];
    /* 逐一计算每个正延迟的去均值点积。 */
    for (int lag = 1; lag <= max_lag; lag++) {
        /* dot 保存当前延迟下两个重叠区间的点积。 */
        float dot = 0.0f;
        /* 重叠长度为 n-lag，循环终止于最后一个有效配对。 */
        for (int i = 0; i < n - lag; i++) {
            /* 累加相隔 lag 点的两个动态分量乘积。 */
            dot += (x[i] - mean) * (x[i + lag] - mean);
        }
        /* 除以零延迟能量得到无量纲自相关值，与 Python 公式一致。 */
        autocorr[lag - 1] = dot / energy;
    }
    /* peak_count 记录满足左右邻点和 0.20 门槛的显著局部峰数。 */
    int peak_count = 0;
    /* 从第二个延迟点遍历到倒数第二个，保证左右邻点存在。 */
    for (int i = 1; i < max_lag - 1; i++) {
        /* value 是当前延迟的归一化自相关值。 */
        float value = autocorr[i];
        /* 严格高于左点、不低于右点且达到 0.20 时计为显著周期峰。 */
        if (value > autocorr[i - 1] && value >= autocorr[i + 1] && value >= 0.20f) {
            /* 每发现一个显著峰，计数增加一。 */
            peak_count++;
        }
    }
    /* 返回峰数量的 float 表示，便于写入统一特征数组。 */
    return (float)peak_count;
}

/*
 * 计算显著正峰的幅值变异系数和峰间隔变异系数。
 * 输入 x 指向长度为 n 的单通道窗口；陀螺输入单位为 deg/s，加速度输入单位为 g。
 * 输出 amplitude_cv 和 interval_cv 均无量纲；无至少两个有效峰时写入 0，避免除零。
 * 数学公式、边界条件和 Python/C 一致性要求见 docs/弱类频谱与峰形特征说明.md。
 */
static inline void positive_peak_shape_features(
    const float* x,
    int n,
    float* amplitude_cv,
    float* interval_cv
) {
    /* 累加输入序列的一阶矩和二阶矩，用于总体均值与总体标准差。 */
    float sum = 0.0f;
    /* 二阶矩累加值的单位是输入物理量的平方。 */
    float sum2 = 0.0f;
    /* sorted 保存输入副本，最大长度 WINDOW_LEN，用于计算抗离群中位数。 */
    float sorted[WINDOW_LEN];
    /* 遍历 n 个采样点，同时累计矩并复制到排序缓冲区。 */
    for (int i = 0; i < n; i++) {
        /* 当前采样值继承 x 的物理单位。 */
        float value = x[i];
        /* 一阶矩用于计算均值。 */
        sum += value;
        /* 二阶矩用于计算总体方差。 */
        sum2 += value * value;
        /* 复制输入，后续插入排序不会修改原始窗口。 */
        sorted[i] = value;
    }
    /* 输入均值用于自适应峰值门槛。 */
    float mean = sum / (float)n;
    /* 由 E[x^2]-E[x]^2 计算总体方差。 */
    float variance = sum2 / (float)n - mean * mean;
    /* 浮点舍入可能产生极小负方差，截断到 0 后再开方。 */
    if (variance < 0.0f) variance = 0.0f;
    /* 总体标准差与输入单位相同。 */
    float std = sqrtf(variance);
    /* 显著正峰必须不低于均值加 0.5 倍标准差。 */
    float threshold = mean + 0.5f * std;

    /* 使用插入排序得到升序副本；n 最大为 62，O(n^2) 开销可控且无需动态内存。 */
    for (int i = 1; i < n; i++) {
        /* key 是本轮待插入的采样值。 */
        float key = sorted[i];
        /* j 从已排序区间末端向前移动。 */
        int j = i - 1;
        /* 将所有大于 key 的元素右移一位，直到找到插入位置。 */
        while (j >= 0 && sorted[j] > key) {
            /* 右移元素，保持 sorted[0..i] 有序。 */
            sorted[j + 1] = sorted[j];
            /* 继续检查前一个已排序元素。 */
            j--;
        }
        /* 将 key 放入最终位置。 */
        sorted[j + 1] = key;
    }
    /* 奇数长度取中间值，偶数长度取两个中间值平均，结果单位与输入相同。 */
    float median = n % 2 == 1
        ? sorted[n / 2]
        : 0.5f * (sorted[n / 2 - 1] + sorted[n / 2]);

    /* peak_indices 保存显著正峰的采样位置，元素范围为 [1,n-2]。 */
    int peak_indices[WINDOW_LEN];
    /* peak_count 记录已检测的显著正峰数量。 */
    int peak_count = 0;
    /* 遍历所有拥有左右邻点的采样位置，检测局部最大值。 */
    for (int i = 1; i < n - 1; i++) {
        /* 当前值严格高于左点、不低于右点且越过门槛时判为显著正峰。 */
        if (x[i] > x[i - 1] && x[i] >= x[i + 1] && x[i] >= threshold) {
            /* 保存峰位置，用于后续计算幅值和相邻峰间隔。 */
            peak_indices[peak_count++] = i;
        }
    }
    /* 少于两个峰无法估计重复周期离散程度，两个输出都设为 0。 */
    if (peak_count < 2) {
        /* 0 表示没有可解析的峰幅变异。 */
        *amplitude_cv = 0.0f;
        /* 0 表示没有可解析的峰间隔变异。 */
        *interval_cv = 0.0f;
        /* 提前返回，避免访问不存在的峰间隔。 */
        return;
    }

    /* amp_sum 累加峰值相对中位数的有符号幅值。 */
    float amp_sum = 0.0f;
    /* amp_abs_sum 累加绝对幅值，作为 CV 的稳定正分母。 */
    float amp_abs_sum = 0.0f;
    /* amp_sum2 累加峰幅平方，用于总体方差。 */
    float amp_sum2 = 0.0f;
    /* 遍历所有显著正峰，累计相对中位数的幅值统计量。 */
    for (int i = 0; i < peak_count; i++) {
        /* 峰幅等于峰值减去全窗中位数，单位与输入一致。 */
        float amplitude = x[peak_indices[i]] - median;
        /* 累加有符号峰幅。 */
        amp_sum += amplitude;
        /* 累加绝对峰幅，防止正负抵消导致分母过小。 */
        amp_abs_sum += fabsf(amplitude);
        /* 累加峰幅平方。 */
        amp_sum2 += amplitude * amplitude;
    }
    /* 峰幅总体均值用于方差计算。 */
    float amp_mean = amp_sum / (float)peak_count;
    /* 峰幅总体方差按 E[a^2]-E[a]^2 计算。 */
    float amp_variance = amp_sum2 / (float)peak_count - amp_mean * amp_mean;
    /* 舍入保护：负的微小方差截断为 0。 */
    if (amp_variance < 0.0f) amp_variance = 0.0f;
    /* 绝对峰幅均值是无量纲 CV 的分母。 */
    float amp_denominator = amp_abs_sum / (float)peak_count;
    /* 分母有效时计算总体标准差/绝对均值，否则输出 0。 */
    *amplitude_cv = amp_denominator > 1e-12f
        ? sqrtf(amp_variance) / amp_denominator
        : 0.0f;

    /* interval_count 等于相邻峰对数量。 */
    int interval_count = peak_count - 1;
    /* interval_sum 累加相邻峰间隔，单位为采样点。 */
    float interval_sum = 0.0f;
    /* interval_sum2 累加峰间隔平方。 */
    float interval_sum2 = 0.0f;
    /* 遍历相邻峰对，计算位置索引差。 */
    for (int i = 0; i < interval_count; i++) {
        /* 峰间隔始终为正，范围为 [1,n-2] 个采样点。 */
        float interval = (float)(peak_indices[i + 1] - peak_indices[i]);
        /* 累加峰间隔。 */
        interval_sum += interval;
        /* 累加峰间隔平方。 */
        interval_sum2 += interval * interval;
    }
    /* 平均峰间隔作为 CV 的正分母。 */
    float interval_mean = interval_sum / (float)interval_count;
    /* 峰间隔总体方差按 E[d^2]-E[d]^2 计算。 */
    float interval_variance =
        interval_sum2 / (float)interval_count - interval_mean * interval_mean;
    /* 舍入保护：负的微小方差截断为 0。 */
    if (interval_variance < 0.0f) interval_variance = 0.0f;
    /* 平均间隔有效时计算总体标准差/平均间隔，否则输出 0。 */
    *interval_cv = interval_mean > 1e-12f
        ? sqrtf(interval_variance) / interval_mean
        : 0.0f;
}

static inline float series_correlation(const float* left, const float* right, int n) {
    float left_mean = 0.0f;
    float right_mean = 0.0f;
    for (int i = 0; i < n; i++) {
        left_mean += left[i];
        right_mean += right[i];
    }
    left_mean /= (float)n;
    right_mean /= (float)n;
    float dot = 0.0f;
    float left_energy = 0.0f;
    float right_energy = 0.0f;
    for (int i = 0; i < n; i++) {
        float a = left[i] - left_mean;
        float b = right[i] - right_mean;
        dot += a * b;
        left_energy += a * a;
        right_energy += b * b;
    }
    float denominator = sqrtf(left_energy * right_energy);
    return denominator > 1e-12f ? dot / denominator : 0.0f;
}

/*
 * 返回 ±1 秒内绝对值最大的有符号归一化互相关。
 * left/right 长度均为 n；输入可分别为 g 与 deg/s，归一化后输出无量纲且接近 [-1,1]。
 * 最大延迟为 min(n/4,SAMPLE_RATE_HZ)，时间复杂度 O(n*SAMPLE_RATE_HZ)，额外空间 O(n)。
 */
static inline float max_cross_correlation(const float* left, const float* right, int n) {
    /* left_mean 累加并保存左序列全窗均值。 */
    float left_mean = 0.0f;
    /* right_mean 累加并保存右序列全窗均值。 */
    float right_mean = 0.0f;
    /* 遍历等长输入，同时累计两个序列的一阶矩。 */
    for (int i = 0; i < n; i++) {
        /* 累加左序列采样值。 */
        left_mean += left[i];
        /* 累加右序列采样值。 */
        right_mean += right[i];
    }
    /* 除以 n 得到左序列均值，单位继承 left。 */
    left_mean /= (float)n;
    /* 除以 n 得到右序列均值，单位继承 right。 */
    right_mean /= (float)n;
    /* 最大延迟先取四分之一窗口，62 点窗口对应 15 点。 */
    int max_lag = n / 4;
    /* 超过一秒时截断为采样率对应的点数。 */
    if (max_lag > SAMPLE_RATE_HZ) max_lag = SAMPLE_RATE_HZ;
    /* best_correlation 保存绝对值最强的相关系数并保留正负号。 */
    float best_correlation = 0.0f;
    /* 从负最大延迟遍历到正最大延迟，顺序与 Python 完全一致。 */
    for (int lag = -max_lag; lag <= max_lag; lag++) {
        /* overlap 是当前延迟下两个序列的重叠样本数。 */
        int overlap = n - (lag < 0 ? -lag : lag);
        /* left_start 对应 Python 在负延迟时截去左序列前 -lag 点。 */
        int left_start = lag < 0 ? -lag : 0;
        /* right_start 对应 Python 在正延迟时截去右序列前 lag 点。 */
        int right_start = lag > 0 ? lag : 0;
        /* dot 保存当前重叠区间的去均值点积。 */
        float dot = 0.0f;
        /* left_energy 保存左重叠区间的平方和。 */
        float left_energy = 0.0f;
        /* right_energy 保存右重叠区间的平方和。 */
        float right_energy = 0.0f;
        /* 遍历 overlap 个成对采样点并累计归一化所需统计量。 */
        for (int i = 0; i < overlap; i++) {
            /* a 是左序列当前重叠采样减去左全窗均值。 */
            float a = left[left_start + i] - left_mean;
            /* b 是右序列当前重叠采样减去右全窗均值。 */
            float b = right[right_start + i] - right_mean;
            /* 累加跨序列点积。 */
            dot += a * b;
            /* 累加左序列重叠能量。 */
            left_energy += a * a;
            /* 累加右序列重叠能量。 */
            right_energy += b * b;
        }
        /* 两个 L2 范数乘积是相关系数分母。 */
        float denominator = sqrtf(left_energy * right_energy);
        /* 分母有效时计算有符号相关，近常量重叠区间定义为 0。 */
        float correlation = denominator > 1e-12f ? dot / denominator : 0.0f;
        /* 仅当绝对值严格更大时更新，平局保留更早遍历到的延迟。 */
        if (fabsf(correlation) > fabsf(best_correlation)) {
            /* 保存当前绝对值最强的有符号相关系数。 */
            best_correlation = correlation;
        }
    }
    /* 返回无量纲最大互相关，供标准化和单 BP 输入。 */
    return best_correlation;
}

static inline float bp_window_motion_score(const float window[WINDOW_LEN][AXIS_NUM]) {
    float gyro_sum = 0.0f;
    float gyro_sum2 = 0.0f;
    float acc_sum = 0.0f;
    float acc_sum2 = 0.0f;
    for (int i = 0; i < WINDOW_LEN; i++) {
        float gyro_mag = sqrtf(
            window[i][0] * window[i][0] +
            window[i][1] * window[i][1] +
            window[i][2] * window[i][2]
        );
        float acc_mag = sqrtf(
            window[i][3] * window[i][3] +
            window[i][4] * window[i][4] +
            window[i][5] * window[i][5]
        );
        gyro_sum += gyro_mag;
        gyro_sum2 += gyro_mag * gyro_mag;
        acc_sum += acc_mag;
        acc_sum2 += acc_mag * acc_mag;
    }
    float gyro_mean = gyro_sum / (float)WINDOW_LEN;
    float acc_mean = acc_sum / (float)WINDOW_LEN;
    float gyro_var = gyro_sum2 / (float)WINDOW_LEN - gyro_mean * gyro_mean;
    float acc_var = acc_sum2 / (float)WINDOW_LEN - acc_mean * acc_mean;
    if (gyro_var < 0.0f) gyro_var = 0.0f;
    if (acc_var < 0.0f) acc_var = 0.0f;
    return sqrtf(acc_var) + sqrtf(gyro_var) / 200.0f;
}

static inline float bp_window_active_ratio(const float window[WINDOW_LEN][AXIS_NUM]) {
    int active_count = 0;
    for (int i = 0; i < WINDOW_LEN; i++) {
        float gyro_mag = sqrtf(
            window[i][0] * window[i][0] +
            window[i][1] * window[i][1] +
            window[i][2] * window[i][2]
        );
        float acc_delta = 0.0f;
        if (i > 0) {
            float dx = window[i][3] - window[i - 1][3];
            float dy = window[i][4] - window[i - 1][4];
            float dz = window[i][5] - window[i - 1][5];
            acc_delta = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        if (acc_delta + gyro_mag / 200.0f > ACTIVE_POINT_THRESHOLD) active_count++;
    }
    return (float)active_count / (float)WINDOW_LEN;
}

static inline int bp_window_is_dynamic_candidate(const float window[WINDOW_LEN][AXIS_NUM]) {
    return (
        bp_window_motion_score(window) >= REST_MOTION_THRESHOLD * 1.25f &&
        bp_window_active_ratio(window) >= HIGH_DYNAMIC_MIN_RATIO
    );
}

/* 对事件数组执行插入排序并返回中位数；count=0 时返回 0。 */
static inline float event_median_c(const float* values, int count) {
    /* 空事件没有可聚合物理量，返回确定性零。 */
    if (count <= 0) return 0.0f;
    /* 62 点窗口最多包含 31 个长度不少于两点的事件。 */
    float sorted[WINDOW_LEN / 2];
    /* 复制有效事件值，避免修改调用者数组。 */
    for (int i = 0; i < count; i++) sorted[i] = values[i];
    /* 使用稳定插入排序；固定小数组最坏时间复杂度 O(count^2)。 */
    for (int i = 1; i < count; i++) {
        /* key 保存当前待插入事件值。 */
        float key = sorted[i];
        /* j 从有序前缀末尾向前搜索。 */
        int j = i - 1;
        /* 将所有大于 key 的值右移一位。 */
        while (j >= 0 && sorted[j] > key) {
            /* 右移当前较大元素。 */
            sorted[j + 1] = sorted[j];
            /* 继续比较前一个元素。 */
            j--;
        }
        /* 将 key 写入最终位置。 */
        sorted[j + 1] = key;
    }
    /* 奇数长度直接返回中央值。 */
    if ((count & 1) != 0) return sorted[count / 2];
    /* 偶数长度返回两个中央值均值，与 numpy.median 一致。 */
    return 0.5f * (sorted[count / 2 - 1] + sorted[count / 2]);
}

/*
 * 计算水平加速度或角速度协方差各向异性。
 * gyro_source=0 时输入单位为 g；gyro_source=1 时输入单位为 deg/s；输出无量纲 [0,1]。
 */
static inline float horizontal_anisotropy_from_window(
    const float window[WINDOW_LEN][AXIS_NUM],
    int gyro_source
) {
    /* gravity 保存窗口三轴加速度均值，单位 g。 */
    float gravity[3] = {0.0f, 0.0f, 0.0f};
    /* 累计 ax、ay、az 以估计重力方向。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* 三个加速度分量位于列 3..5。 */
        for (int axis = 0; axis < 3; axis++) gravity[axis] += window[i][axis + 3];
    }
    /* 除以窗口长度得到重力均值。 */
    for (int axis = 0; axis < 3; axis++) gravity[axis] /= (float)WINDOW_LEN;
    /* 计算重力模长，过小时退化为 z 轴。 */
    float gravity_norm = sqrtf(
        gravity[0] * gravity[0] + gravity[1] * gravity[1] + gravity[2] * gravity[2]
    );
    /* gravity_unit 是单位重力方向。 */
    float gravity_unit[3];
    /* 正常输入按模长归一化。 */
    if (gravity_norm > 1e-6f) {
        /* 三个分量分别除以重力模长。 */
        for (int axis = 0; axis < 3; axis++) gravity_unit[axis] = gravity[axis] / gravity_norm;
    } else {
        /* 异常近零重力时使用传感器 z 轴，防止除零。 */
        gravity_unit[0] = 0.0f; gravity_unit[1] = 0.0f; gravity_unit[2] = 1.0f;
    }
    /* horizontal 保存每点投影到重力正交平面的方向向量。 */
    float horizontal[WINDOW_LEN][3];
    /* mean 保存三个水平分量的窗口均值。 */
    float mean[3] = {0.0f, 0.0f, 0.0f};
    /* 构建水平向量并累计分量均值。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* source 保存当前三轴加速度动态量或三轴角速度。 */
        float source[3];
        /* 根据 gyro_source 选择物理来源；加速度分支先减去重力均值。 */
        for (int axis = 0; axis < 3; axis++) {
            /* 陀螺取列 0..2；加速度取列 3..5 并移除静态重力。 */
            source[axis] = gyro_source ? window[i][axis] : window[i][axis + 3] - gravity[axis];
        }
        /* vertical 是 source 在重力方向的标量投影。 */
        float vertical =
            source[0] * gravity_unit[0] +
            source[1] * gravity_unit[1] +
            source[2] * gravity_unit[2];
        /* 逐轴移除垂直分量并累计水平向量均值。 */
        for (int axis = 0; axis < 3; axis++) {
            /* 水平向量仍使用原来源单位。 */
            horizontal[i][axis] = source[axis] - vertical * gravity_unit[axis];
            /* 累计该水平分量。 */
            mean[axis] += horizontal[i][axis];
        }
    }
    /* 除以窗口长度得到水平向量均值。 */
    for (int axis = 0; axis < 3; axis++) mean[axis] /= (float)WINDOW_LEN;
    /* 六个变量保存对称总体协方差的独立元素。 */
    float c00 = 0.0f, c11 = 0.0f, c22 = 0.0f;
    /* 三个变量保存协方差上三角交叉项。 */
    float c01 = 0.0f, c02 = 0.0f, c12 = 0.0f;
    /* 累计去均值水平向量外积。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* x、y、z 是当前向量相对窗口均值的分量。 */
        float x = horizontal[i][0] - mean[0];
        float y = horizontal[i][1] - mean[1];
        float z = horizontal[i][2] - mean[2];
        /* 累计三个平方项。 */
        c00 += x * x; c11 += y * y; c22 += z * z;
        /* 累计三个交叉项。 */
        c01 += x * y; c02 += x * z; c12 += y * z;
    }
    /* 总体协方差使用分母 N，与 Python 实现一致。 */
    float inverse_n = 1.0f / (float)WINDOW_LEN;
    /* 六个独立元素统一除以 N。 */
    c00 *= inverse_n; c11 *= inverse_n; c22 *= inverse_n;
    c01 *= inverse_n; c02 *= inverse_n; c12 *= inverse_n;
    /* trace 等于水平面两个非零特征值之和。 */
    float trace = c00 + c11 + c22;
    /* 总动态能量过小时返回 0，避免噪声方向放大。 */
    if (trace <= 1e-12f) return 0.0f;
    /* trace_square=trace(C^2)，对称非对角项计两次。 */
    float trace_square =
        c00 * c00 + c11 * c11 + c22 * c22 +
        2.0f * (c01 * c01 + c02 * c02 + c12 * c12);
    /* 两非零特征值差平方为 2*trace(C^2)-trace(C)^2。 */
    float difference_squared = 2.0f * trace_square - trace * trace;
    /* 单精度舍入产生负小数时截断为 0。 */
    if (difference_squared < 0.0f) difference_squared = 0.0f;
    /* 特征值差除以特征值和，得到无量纲各向异性。 */
    float anisotropy = sqrtf(difference_squared) / trace;
    /* 理论上限为 1，显式截断单精度超限。 */
    if (anisotropy > 1.0f) anisotropy = 1.0f;
    /* 返回 0 表示各向同性，1 表示单一主方向。 */
    return anisotropy;
}

/* 提取并追加四项完整腾空事件中位数；公式及边界见 docs/弱类联合优化方案.md。 */
static inline void append_aligned_event_medians(
    const float window[WINDOW_LEN][AXIS_NUM],
    float* feature,
    int* index
) {
    /* gravity 保存加速度窗口均值，用于重力方向估计。 */
    float gravity[3] = {0.0f, 0.0f, 0.0f};
    /* 累计三轴加速度。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* 加速度通道位于列 3..5。 */
        for (int axis = 0; axis < 3; axis++) gravity[axis] += window[i][axis + 3];
    }
    /* 除以 N 得到重力均值。 */
    for (int axis = 0; axis < 3; axis++) gravity[axis] /= (float)WINDOW_LEN;
    /* 计算重力模长并构造单位方向。 */
    float norm = sqrtf(gravity[0] * gravity[0] + gravity[1] * gravity[1] + gravity[2] * gravity[2]);
    /* unit 保存单位重力向量。 */
    float unit[3];
    /* 正常模长时归一化，异常时退化为 z 轴。 */
    if (norm > 1e-6f) {
        /* 三轴分别除以模长。 */
        for (int axis = 0; axis < 3; axis++) unit[axis] = gravity[axis] / norm;
    } else {
        /* 确定性退化方向防止除零。 */
        unit[0] = 0.0f; unit[1] = 0.0f; unit[2] = 1.0f;
    }
    /* vertical 为含重力垂直加速度；gyro_vertical 为绕重力轴角速度。 */
    float vertical[WINDOW_LEN], gyro_vertical[WINDOW_LEN];
    /* horizontal_gyro_mag 为水平角速度模长。 */
    float horizontal_gyro_mag[WINDOW_LEN];
    /* acc_mag 为三轴加速度模长，用于 0.70g 腾空判定。 */
    float acc_mag[WINDOW_LEN];
    /* 同时构造四个事件检测序列。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* 垂直加速度投影单位为 g。 */
        vertical[i] = window[i][3] * unit[0] + window[i][4] * unit[1] + window[i][5] * unit[2];
        /* 垂直角速度投影单位为 deg/s。 */
        gyro_vertical[i] = window[i][0] * unit[0] + window[i][1] * unit[1] + window[i][2] * unit[2];
        /* 陀螺总能量减去垂直分量平方得到水平模长平方。 */
        float gyro_horizontal_squared =
            window[i][0] * window[i][0] + window[i][1] * window[i][1] + window[i][2] * window[i][2] -
            gyro_vertical[i] * gyro_vertical[i];
        /* 舍入可能产生极小负数，开方前截断。 */
        if (gyro_horizontal_squared < 0.0f) gyro_horizontal_squared = 0.0f;
        /* 水平角速度模长单位为 deg/s。 */
        horizontal_gyro_mag[i] = sqrtf(gyro_horizontal_squared);
        /* 加速度模长单位为 g。 */
        acc_mag[i] = sqrtf(
            window[i][3] * window[i][3] + window[i][4] * window[i][4] + window[i][5] * window[i][5]
        );
    }
    /* 计算垂直加速度总体均值和二阶矩。 */
    float mean = 0.0f, second = 0.0f;
    /* 累计全部垂直样本。 */
    for (int i = 0; i < WINDOW_LEN; i++) { mean += vertical[i]; second += vertical[i] * vertical[i]; }
    /* 总体均值使用分母 N。 */
    mean /= (float)WINDOW_LEN;
    /* 总体方差由 E[x^2]-E[x]^2 得到。 */
    float variance = second / (float)WINDOW_LEN - mean * mean;
    /* 截断负舍入误差。 */
    if (variance < 0.0f) variance = 0.0f;
    /* 落地冲击门槛取 1.20g 与均值加半标准差的较大值。 */
    float impact_threshold = mean + 0.5f * sqrtf(variance);
    /* 施加最低物理门槛。 */
    if (impact_threshold < 1.20f) impact_threshold = 1.20f;
    /* 四个数组保存完整事件的秒或度值。 */
    float intervals[WINDOW_LEN / 2], widths[WINDOW_LEN / 2];
    /* 两个角度数组分别保存水平和垂直积分。 */
    float horizontal_integrals[WINDOW_LEN / 2], vertical_integrals[WINDOW_LEN / 2];
    /* count 是四个数组的共同有效事件数。 */
    int count = 0;
    /* scan 是连续腾空段扫描索引。 */
    int scan = 0;
    /* 顺序扫描所有采样点。 */
    while (scan < WINDOW_LEN) {
        /* 非腾空点直接前进。 */
        if (acc_mag[scan] >= 0.70f) { scan++; continue; }
        /* start 记录当前连续低支持力段起点。 */
        int start = scan;
        /* 前进到首个非腾空点或窗口末尾。 */
        while (scan < WINDOW_LEN && acc_mag[scan] < 0.70f) scan++;
        /* end 是半开区间终点。 */
        int end = scan;
        /* 丢弃单点噪声和被窗口边界截断的事件。 */
        if (end - start < 2 || start <= 0 || end >= WINDOW_LEN) continue;
        /* 起跳搜索覆盖腾空前最多 5 点。 */
        int pre_start = start - 5;
        /* 左边界截断到 0。 */
        if (pre_start < 0) pre_start = 0;
        /* takeoff 初始为局部首点。 */
        int takeoff = pre_start;
        /* 严格更大才更新，平局保持最早索引。 */
        for (int j = pre_start + 1; j < start; j++) if (vertical[j] > vertical[takeoff]) takeoff = j;
        /* 落地搜索覆盖腾空结束后最多 6 点。 */
        int post_end = end + 6;
        /* 右边界截断到窗口长度。 */
        if (post_end > WINDOW_LEN) post_end = WINDOW_LEN;
        /* landing 初始为腾空结束后的首点。 */
        int landing = end;
        /* 严格更大才更新，匹配 numpy.argmax 最早平局规则。 */
        for (int j = end + 1; j < post_end; j++) if (vertical[j] > vertical[landing]) landing = j;
        /* 保存起跳峰到落地峰的秒数。 */
        intervals[count] = (float)(landing - takeoff) / (float)SAMPLE_RATE_HZ;
        /* points 统计落地峰后连续高冲击点数。 */
        int points = 0;
        /* 首个低于门槛的点结束连续宽度。 */
        for (int j = landing; j < post_end; j++) { if (vertical[j] >= impact_threshold) points++; else break; }
        /* 点数除以采样率得到冲击宽度秒数。 */
        widths[count] = (float)points / (float)SAMPLE_RATE_HZ;
        /* 两项角速度积分从零开始累计。 */
        float horizontal_sum = 0.0f, vertical_sum = 0.0f;
        /* 遍历半开腾空区间的全部采样点。 */
        for (int j = start; j < end; j++) {
            /* 累加水平角速度模长。 */
            horizontal_sum += horizontal_gyro_mag[j];
            /* 累加垂直角速度绝对值，避免方向抵消。 */
            vertical_sum += fabsf(gyro_vertical[j]);
        }
        /* 除以采样率得到水平总转角，单位度。 */
        horizontal_integrals[count] = horizontal_sum / (float)SAMPLE_RATE_HZ;
        /* 除以采样率得到垂直总转角，单位度。 */
        vertical_integrals[count] = vertical_sum / (float)SAMPLE_RATE_HZ;
        /* 完整事件数增加一。 */
        count++;
    }
    /* 按 Python 固定顺序追加四项事件中位数。 */
    feature[(*index)++] = event_median_c(intervals, count);
    /* 追加落地冲击宽度中位数。 */
    feature[(*index)++] = event_median_c(widths, count);
    /* 追加腾空水平角速度积分中位数。 */
    feature[(*index)++] = event_median_c(horizontal_integrals, count);
    /* 追加腾空垂直角速度绝对积分中位数。 */
    feature[(*index)++] = event_median_c(vertical_integrals, count);
}

static inline void extract_features_from_window(const float window[WINDOW_LEN][AXIS_NUM], float feature[FEATURE_DIM]) {
    int idx = 0;
    float temp[WINDOW_LEN];
    float phase_sources[4][WINDOW_LEN];
    int phase_lengths[4] = { WINDOW_LEN, WINDOW_LEN, WINDOW_LEN, WINDOW_LEN - 1 };

    /* gx, gy, gz, ax, ay, az */
    for (int axis = 0; axis < AXIS_NUM; axis++) {
        for (int i = 0; i < WINDOW_LEN; i++) temp[i] = window[i][axis];
        append_series_features(temp, WINDOW_LEN, feature, &idx);
    }

    /* gyro_mag */
    for (int i = 0; i < WINDOW_LEN; i++) {
        float gx = window[i][0];
        float gy = window[i][1];
        float gz = window[i][2];
        temp[i] = sqrtf(gx * gx + gy * gy + gz * gz);
        phase_sources[2][i] = temp[i];
    }
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    /* acc_mag */
    for (int i = 0; i < WINDOW_LEN; i++) {
        float ax = window[i][3];
        float ay = window[i][4];
        float az = window[i][5];
        temp[i] = sqrtf(ax * ax + ay * ay + az * az);
    }
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    /* gyro_delta_mag */
    for (int i = 0; i < WINDOW_LEN - 1; i++) {
        float dx = window[i + 1][0] - window[i][0];
        float dy = window[i + 1][1] - window[i][1];
        float dz = window[i + 1][2] - window[i][2];
        temp[i] = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    append_series_features(temp, WINDOW_LEN - 1, feature, &idx);

    /* acc_delta_mag */
    for (int i = 0; i < WINDOW_LEN - 1; i++) {
        float dx = window[i + 1][3] - window[i][3];
        float dy = window[i + 1][4] - window[i][4];
        float dz = window[i + 1][5] - window[i][5];
        temp[i] = sqrtf(dx * dx + dy * dy + dz * dz);
        phase_sources[3][i] = temp[i];
    }
    append_series_features(temp, WINDOW_LEN - 1, feature, &idx);

    /* Gravity-aligned vertical and horizontal components. */
    float gravity_x = 0.0f;
    float gravity_y = 0.0f;
    float gravity_z = 0.0f;
    for (int i = 0; i < WINDOW_LEN; i++) {
        gravity_x += window[i][3];
        gravity_y += window[i][4];
        gravity_z += window[i][5];
    }
    gravity_x /= (float)WINDOW_LEN;
    gravity_y /= (float)WINDOW_LEN;
    gravity_z /= (float)WINDOW_LEN;
    float gravity_norm = sqrtf(
        gravity_x * gravity_x + gravity_y * gravity_y + gravity_z * gravity_z
    );
    if (gravity_norm < 1e-6f) {
        gravity_x = 0.0f;
        gravity_y = 0.0f;
        gravity_z = 1.0f;
    } else {
        gravity_x /= gravity_norm;
        gravity_y /= gravity_norm;
        gravity_z /= gravity_norm;
    }

    /* acc_vertical */
    for (int i = 0; i < WINDOW_LEN; i++) {
        float vertical =
            window[i][3] * gravity_x +
            window[i][4] * gravity_y +
            window[i][5] * gravity_z;
        temp[i] = vertical;
        phase_sources[0][i] = vertical;
    }
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    /* acc_horizontal_mag */
    for (int i = 0; i < WINDOW_LEN; i++) {
        float vertical = phase_sources[0][i];
        float total_squared =
            window[i][3] * window[i][3] +
            window[i][4] * window[i][4] +
            window[i][5] * window[i][5];
        float horizontal_squared = total_squared - vertical * vertical;
        if (horizontal_squared < 0.0f) horizontal_squared = 0.0f;
        temp[i] = sqrtf(horizontal_squared);
        phase_sources[1][i] = temp[i];
    }
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    /* gyro_vertical */
    for (int i = 0; i < WINDOW_LEN; i++) {
        temp[i] =
            window[i][0] * gravity_x +
            window[i][1] * gravity_y +
            window[i][2] * gravity_z;
    }
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    /* gyro_horizontal_mag */
    for (int i = 0; i < WINDOW_LEN; i++) {
        float vertical =
            window[i][0] * gravity_x +
            window[i][1] * gravity_y +
            window[i][2] * gravity_z;
        float total_squared =
            window[i][0] * window[i][0] +
            window[i][1] * window[i][1] +
            window[i][2] * window[i][2];
        float horizontal_squared = total_squared - vertical * vertical;
        if (horizontal_squared < 0.0f) horizontal_squared = 0.0f;
        temp[i] = sqrtf(horizontal_squared);
    }
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    for (int source = 0; source < 4; source++) {
        append_phase_features(
            phase_sources[source], phase_lengths[source], feature, &idx
        );
    }
    for (int source = 0; source < 4; source++) {
        append_temporal_features(
            phase_sources[source], phase_lengths[source], feature, &idx
        );
    }
    for (int source = 0; source < 4; source++) {
        append_normalized_phase_features(
            phase_sources[source], phase_lengths[source], feature, &idx
        );
    }
    for (int source = 0; source < 4; source++) {
        append_impact_distribution_features(
            phase_sources[source], phase_lengths[source], feature, &idx
        );
    }

    float low_ratio = 0.0f;
    float mid_ratio = 0.0f;
    float high_ratio = 0.0f;
    float centroid_hz = 0.0f;
    float peak_power_ratio = 0.0f;
    selected_spectral_features(
        phase_sources[3], phase_lengths[3], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = mid_ratio;
    selected_spectral_features(
        phase_sources[0], phase_lengths[0], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = high_ratio;
    selected_spectral_features(
        phase_sources[2], phase_lengths[2], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = centroid_hz;
    selected_spectral_features(
        phase_sources[1], phase_lengths[1], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = centroid_hz;
    feature[idx++] = autocorr_first_zero_seconds(
        phase_sources[0], phase_lengths[0]
    );
    feature[idx++] = series_correlation(
        phase_sources[2], phase_sources[0], WINDOW_LEN
    );
    feature[idx++] = autocorr_secondary_peak(
        phase_sources[1], phase_lengths[1]
    );
    selected_spectral_features(
        phase_sources[2], phase_lengths[2], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = high_ratio;
    selected_spectral_features(
        phase_sources[0], phase_lengths[0], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = peak_power_ratio;
    selected_spectral_features(
        phase_sources[3], phase_lengths[3], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = high_ratio;
    selected_spectral_features(
        phase_sources[1], phase_lengths[1], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = mid_ratio;
    /* 写入陀螺模长主谱峰功率占比，输出无量纲且范围为 [0,1]。 */
    selected_spectral_features(
        phase_sources[2], phase_lengths[2], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = peak_power_ratio;
    /* 写入垂直加速度 1.20-2.40 Hz 中频功率占比，输出无量纲。 */
    selected_spectral_features(
        phase_sources[0], phase_lengths[0], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = mid_ratio;
    /* 写入水平加速度模长 2.40-5.00 Hz 高频功率占比，输出无量纲。 */
    selected_spectral_features(
        phase_sources[1], phase_lengths[1], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = high_ratio;
    /* peak_amplitude_cv 接收陀螺正峰幅值 CV，peak_interval_cv 接收峰间隔 CV。 */
    float peak_amplitude_cv = 0.0f;
    /* 初始化峰间隔 CV，调用函数后覆盖为对应通道的无量纲结果。 */
    float peak_interval_cv = 0.0f;
    /* 计算陀螺模长峰形，只写入本轮选用的峰幅变异系数。 */
    positive_peak_shape_features(
        phase_sources[2], phase_lengths[2], &peak_amplitude_cv, &peak_interval_cv
    );
    feature[idx++] = peak_amplitude_cv;
    /* 计算垂直加速度峰形，只写入本轮选用的峰间隔变异系数。 */
    positive_peak_shape_features(
        phase_sources[0], phase_lengths[0], &peak_amplitude_cv, &peak_interval_cv
    );
    feature[idx++] = peak_interval_cv;
    /* 写入垂直加速度与陀螺模长在 ±1 秒内的最强有符号互相关。 */
    feature[idx++] = max_cross_correlation(
        phase_sources[0], phase_sources[2], WINDOW_LEN
    );
    /* 写入垂直与水平加速度在 ±1 秒内的最强有符号互相关。 */
    feature[idx++] = max_cross_correlation(
        phase_sources[0], phase_sources[1], WINDOW_LEN
    );
    /* 重新计算垂直加速度频谱并写入 0.35-1.20 Hz 低频功率占比。 */
    selected_spectral_features(
        phase_sources[0], phase_lengths[0], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = low_ratio;
    /* 重新计算水平加速度频谱并写入低频功率占比。 */
    selected_spectral_features(
        phase_sources[1], phase_lengths[1], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = low_ratio;
    /* 重新计算陀螺模长频谱并写入低频功率占比。 */
    selected_spectral_features(
        phase_sources[2], phase_lengths[2], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = low_ratio;
    /* 写入水平加速度归一化自相关显著局部峰数量。 */
    feature[idx++] = autocorr_prominent_peak_count(
        phase_sources[1], phase_lengths[1]
    );
    /* 计算水平加速度正峰形并写入峰间隔总体变异系数。 */
    positive_peak_shape_features(
        phase_sources[1], phase_lengths[1], &peak_amplitude_cv, &peak_interval_cv
    );
    feature[idx++] = peak_interval_cv;
    /* 水平频谱结果仍保存在 peak_power_ratio，写入主谱峰功率占比。 */
    selected_spectral_features(
        phase_sources[1], phase_lengths[1], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = peak_power_ratio;
    /* 追加水平动态加速度协方差各向异性，输出无量纲 [0,1]。 */
    feature[idx++] = horizontal_anisotropy_from_window(window, 0);
    /* 追加水平角速度协方差各向异性，输出无量纲 [0,1]。 */
    feature[idx++] = horizontal_anisotropy_from_window(window, 1);
    /* 追加起跳-落地时间、冲击宽度及两项腾空角度积分中位数。 */
    append_aligned_event_medians(window, feature, &idx);
}

static inline float relu_float(float x) {
    return x > 0.0f ? x : 0.0f;
}

#if HAS_FAMILY_SPECIALIST
static inline int bp_family_specialist_predict(const float feature_raw[FEATURE_DIM], float* confidence) {
    float x[SPECIALIST_FEATURE_DIM];
    float h1[HIDDEN1];
    float h2[HIDDEN2];
    float h3[HIDDEN3];
    float out[SPECIALIST_CLASS_NUM];
    for (int i = 0; i < SPECIALIST_FEATURE_DIM; i++) {
        int feature_index = SPECIALIST_FEATURE_INDEX[i];
        x[i] = (feature_raw[feature_index] - SPECIALIST_FEATURE_MEAN[i]) / SPECIALIST_FEATURE_STD[i];
    }
    for (int o = 0; o < HIDDEN1; o++) {
        float sum = SB1[o];
        for (int i = 0; i < SPECIALIST_FEATURE_DIM; i++) sum += SW1[o][i] * x[i];
        h1[o] = relu_float(sum);
    }
    for (int o = 0; o < HIDDEN2; o++) {
        float sum = SB2[o];
        for (int i = 0; i < HIDDEN1; i++) sum += SW2[o][i] * h1[i];
        h2[o] = relu_float(sum);
    }
    for (int o = 0; o < HIDDEN3; o++) {
        float sum = SB3[o];
        for (int i = 0; i < HIDDEN2; i++) sum += SW3[o][i] * h2[i];
        h3[o] = relu_float(sum);
    }
    float max_logit = -3.4028235e38f;
    for (int o = 0; o < SPECIALIST_CLASS_NUM; o++) {
        float sum = SB4[o];
        for (int i = 0; i < HIDDEN3; i++) sum += SW4[o][i] * h3[i];
        out[o] = sum;
        if (sum > max_logit) max_logit = sum;
    }
    float exp_sum = 0.0f;
    for (int o = 0; o < SPECIALIST_CLASS_NUM; o++) {
        out[o] = expf(out[o] - max_logit);
        exp_sum += out[o];
    }
    int best_local_idx = 0;
    float best_prob = 0.0f;
    for (int o = 0; o < SPECIALIST_CLASS_NUM; o++) {
        float prob = out[o] / exp_sum;
        if (prob > best_prob) {
            best_prob = prob;
            best_local_idx = o;
        }
    }
    if (confidence != 0) *confidence = best_prob;
    return SPECIALIST_GLOBAL_CLASS_INDEX[best_local_idx];
}
#endif

static inline int bp_predict_from_features(const float feature_raw[FEATURE_DIM], float* confidence) {
    float x[FEATURE_DIM];
    float h1[HIDDEN1];
    float h2[HIDDEN2];
    float h3[HIDDEN3];
    float out[CLASS_NUM];
    for (int i = 0; i < FEATURE_DIM; i++) x[i] = (feature_raw[i] - FEATURE_MEAN[i]) / FEATURE_STD[i];
    for (int o = 0; o < HIDDEN1; o++) {
        float sum = B1[o];
        for (int i = 0; i < FEATURE_DIM; i++) sum += W1[o][i] * x[i];
        h1[o] = relu_float(sum);
    }
    for (int o = 0; o < HIDDEN2; o++) {
        float sum = B2[o];
        for (int i = 0; i < HIDDEN1; i++) sum += W2[o][i] * h1[i];
        h2[o] = relu_float(sum);
    }
    for (int o = 0; o < HIDDEN3; o++) {
        float sum = B3[o];
        for (int i = 0; i < HIDDEN2; i++) sum += W3[o][i] * h2[i];
        h3[o] = relu_float(sum);
    }
    float max_logit = -3.4028235e38f;
    for (int o = 0; o < CLASS_NUM; o++) {
        float sum = B4[o];
        for (int i = 0; i < HIDDEN3; i++) sum += W4[o][i] * h3[i];
        out[o] = sum;
        if (sum > max_logit) max_logit = sum;
    }
    float exp_sum = 0.0f;
    for (int o = 0; o < CLASS_NUM; o++) {
        out[o] = expf(out[o] - max_logit);
        exp_sum += out[o];
    }
    int best_idx = 0;
    float best_prob = 0.0f;
    for (int o = 0; o < CLASS_NUM; o++) {
        float prob = out[o] / exp_sum;
        if (prob > best_prob) {
            best_prob = prob;
            best_idx = o;
        }
    }
#if HAS_FAMILY_SPECIALIST
    for (int local_idx = 0; local_idx < SPECIALIST_CLASS_NUM; local_idx++) {
        if (SPECIALIST_GLOBAL_CLASS_INDEX[local_idx] == best_idx) {
            return bp_family_specialist_predict(feature_raw, confidence);
        }
    }
#endif
    if (confidence != 0) *confidence = best_prob;
    return best_idx;
}

static inline int bp_predict_from_window(const float window[WINDOW_LEN][AXIS_NUM], float* confidence) {
    float feature[FEATURE_DIM];
    extract_features_from_window(window, feature);
    return bp_predict_from_features(feature, confidence);
}

#endif
""",
    ]
    save_path.write_text("\n".join(lines), encoding="utf-8")


def serializable_experiment(result: Dict[str, object]) -> Dict[str, object]:
    keep = {
        "window_seconds",
        "window_len",
        "step_len",
        "rest_threshold",
        "active_point_threshold",
        "ema_decay",
        "label_smoothing",
        "val_acc",
        "val_f1",
        "val_weak_recall",
        "val_min_recall",
        "val_class_recalls",
        "test_acc",
        "test_f1",
        "test_weak_recall",
        "test_min_recall",
        "test_class_recalls",
        "flat_val_acc",
        "flat_val_f1",
        "flat_test_acc",
        "flat_test_f1",
        "train_sample_count",
        "val_sample_count",
        "test_sample_count",
        "train_file_count",
        "val_file_count",
        "test_file_count",
        "train_files",
        "val_files",
        "test_files",
        "sample_stats",
    }
    return {key: result[key] for key in keep if key in result}


def deployment_gate_status(
    best_result: Dict[str, object],
    class_names: Sequence[str],
) -> Tuple[bool, np.ndarray]:
    recalls = per_class_recalls(
        np.asarray(best_result["y_test"]),
        np.asarray(best_result["test_pred"]),
        len(class_names),
    )
    thresholds = np.asarray(
        [
            WEAK_TARGET_MIN_CLASS_RECALL
            if name in RELAXED_RECALL_CLASS_NAMES
            else TARGET_MIN_CLASS_RECALL
            for name in class_names
        ],
        dtype=np.float64,
    )
    return bool(np.all(recalls >= thresholds)), recalls


def export_model_headers(
    best_result: Dict[str, object],
    class_names: Sequence[str],
    feature_names: Sequence[str],
    output_header_path: Path,
    repository_header_path: Path,
    export_when_below_target: bool,
) -> bool:
    reached_target, _ = deployment_gate_status(best_result, class_names)
    if not reached_target and not export_when_below_target:
        return False

    output_header_path.parent.mkdir(parents=True, exist_ok=True)
    export_esp32_header(best_result, class_names, feature_names, output_header_path)
    if reached_target:
        repository_header_path.parent.mkdir(parents=True, exist_ok=True)
        if output_header_path.resolve() != repository_header_path.resolve():
            shutil.copyfile(output_header_path, repository_header_path)
    return reached_target


def save_outputs(
    best_result: Dict[str, object],
    all_results: Sequence[Dict[str, object]],
    class_names: Sequence[str],
    feature_names: Sequence[str],
    output_dir: Path,
    export_when_below_target: bool,
    repository_header_path: Path = ESP32_MODEL_HEADER,
) -> bool:
    output_dir.mkdir(parents=True, exist_ok=True)
    y_test = np.asarray(best_result["y_test"])
    test_pred = np.asarray(best_result["test_pred"])

    save_confusion_matrix(y_test, test_pred, class_names, output_dir / "confusion_matrix.png")
    model = best_result["model"]
    assert isinstance(model, BPNet)
    specialist_model = best_result.get("specialist_model")
    if isinstance(specialist_model, BPNet):
        torch.save(
            {
                "primary": model.state_dict(),
                "family_specialist": specialist_model.state_dict(),
            },
            output_dir / "best_model.pt",
        )
    else:
        torch.save(model.state_dict(), output_dir / "best_model.pt")
    scaler_config = {
        "mean": np.asarray(best_result["mean"], dtype=np.float32),
        "std": np.asarray(best_result["std"], dtype=np.float32),
        "class_names": np.asarray(class_names),
        "feature_names": np.asarray(feature_names),
        "window_len": np.asarray([int(best_result["window_len"])]),
        "step_len": np.asarray([int(best_result["step_len"])]),
        "sample_rate": np.asarray([SAMPLE_RATE]),
        "rest_threshold": np.asarray(
            [float(best_result["rest_threshold"])], dtype=np.float32
        ),
        "active_point_threshold": np.asarray(
            [float(best_result["active_point_threshold"])], dtype=np.float32
        ),
        "ema_decay": np.asarray(
            [float(best_result.get("ema_decay", 0.0))], dtype=np.float32
        ),
        "label_smoothing": np.asarray(
            [float(best_result.get("label_smoothing", 0.0))], dtype=np.float32
        ),
    }
    if isinstance(specialist_model, BPNet):
        scaler_config.update(
            {
                "specialist_mean": np.asarray(
                    best_result["specialist_mean"], dtype=np.float32
                ),
                "specialist_std": np.asarray(
                    best_result["specialist_std"], dtype=np.float32
                ),
                "specialist_class_names": np.asarray(
                    best_result["specialist_class_names"]
                ),
                "specialist_feature_indices": np.asarray(
                    best_result["specialist_feature_indices"], dtype=np.int64
                ),
            }
        )
    np.savez(output_dir / "scaler_and_config.npz", **scaler_config)

    report = {
        "seed": SEED,
        "sample_rate": SAMPLE_RATE,
        "target_min_class_recall": TARGET_MIN_CLASS_RECALL,
        "weak_target_min_class_recall": WEAK_TARGET_MIN_CLASS_RECALL,
        "relaxed_recall_class_names": sorted(RELAXED_RECALL_CLASS_NAMES),
        "class_names": list(class_names),
        "feature_names": list(feature_names),
        "best_window_seconds": best_result["window_seconds"],
        "best_window_len": best_result["window_len"],
        "step_len": best_result["step_len"],
        "rest_threshold": best_result["rest_threshold"],
        "active_point_threshold": best_result["active_point_threshold"],
        "val_acc": best_result["val_acc"],
        "val_f1": best_result["val_f1"],
        "test_acc": best_result["test_acc"],
        "test_f1": best_result["test_f1"],
        "test_min_recall": best_result["test_min_recall"],
        "test_class_recalls": best_result["test_class_recalls"],
        "external_holdout": best_result.get(
            "external_holdout",
            {"skipped": True, "reason": "not_configured"},
        ),
        "classifier_type": (
            "flat_bp_plus_family_bp_specialist"
            if isinstance(specialist_model, BPNet)
            else "flat_bp"
        ),
        "family_specialist_class_names": list(
            best_result.get("specialist_class_names", [])
        ),
        "flat_val_acc": best_result.get("flat_val_acc"),
        "flat_val_f1": best_result.get("flat_val_f1"),
        "flat_test_acc": best_result.get("flat_test_acc"),
        "flat_test_f1": best_result.get("flat_test_f1"),
        "classification_report": classification_report(
            y_test, test_pred, target_names=class_names, output_dict=True, zero_division=0
        ),
        "all_experiments": [serializable_experiment(result) for result in all_results],
    }
    with (output_dir / "training_report.json").open("w", encoding="utf-8") as file:
        json.dump(report, file, ensure_ascii=False, indent=2)

    return export_model_headers(
        best_result,
        class_names,
        feature_names,
        output_dir / "esp32_bp_model.h",
        repository_header_path,
        export_when_below_target,
    )


def save_validation_outputs(
    best_result: Dict[str, object],
    all_results: Sequence[Dict[str, object]],
    class_names: Sequence[str],
    feature_names: Sequence[str],
    output_dir: Path,
) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    model = best_result["model"]
    # 验证候选允许保存任意 PyTorch 主模型；正式 ESP32 导出仍由门槛和专用导出器控制。
    assert isinstance(model, nn.Module)
    torch.save(model.state_dict(), output_dir / "best_model.pt")
    np.savez(
        output_dir / "scaler_and_config.npz",
        mean=np.asarray(best_result["mean"], dtype=np.float32),
        std=np.asarray(best_result["std"], dtype=np.float32),
        class_names=np.asarray(class_names),
        feature_names=np.asarray(feature_names),
        window_len=np.asarray([int(best_result["window_len"])]),
        step_len=np.asarray([int(best_result["step_len"])]),
        sample_rate=np.asarray([SAMPLE_RATE]),
        rest_threshold=np.asarray(
            [float(best_result["rest_threshold"])], dtype=np.float32
        ),
        active_point_threshold=np.asarray(
            [float(best_result["active_point_threshold"])], dtype=np.float32
        ),
        ema_decay=np.asarray(
            [float(best_result.get("ema_decay", 0.0))], dtype=np.float32
        ),
        label_smoothing=np.asarray(
            [float(best_result.get("label_smoothing", 0.0))], dtype=np.float32
        ),
    )
    validation_keys = {
        "window_seconds",
        "window_len",
        "step_len",
        "rest_threshold",
        "active_point_threshold",
        "ema_decay",
        "label_smoothing",
        "val_acc",
        "val_f1",
        "val_weak_recall",
        "val_min_recall",
        "val_class_recalls",
        "flat_val_acc",
        "flat_val_f1",
        "train_sample_count",
        "val_sample_count",
        "train_file_count",
        "val_file_count",
        "train_files",
        "val_files",
        "sample_stats",
    }
    report = {
        "mode": "validation_only",
        "seed": SEED,
        "sample_rate": SAMPLE_RATE,
        "class_names": list(class_names),
        "feature_names": list(feature_names),
        "best_window_seconds": best_result["window_seconds"],
        "val_acc": best_result["val_acc"],
        "val_f1": best_result["val_f1"],
        "val_min_recall": best_result["val_min_recall"],
        "val_class_recalls": best_result["val_class_recalls"],
        "classification_report": classification_report(
            np.asarray(best_result["y_val"]),
            np.asarray(best_result["val_pred"]),
            target_names=class_names,
            output_dict=True,
            zero_division=0,
        ),
        "all_experiments": [
            {key: result[key] for key in validation_keys if key in result}
            for result in all_results
        ],
    }
    with (output_dir / "validation_report.json").open("w", encoding="utf-8") as file:
        json.dump(report, file, ensure_ascii=False, indent=2)


def parse_ema_decay(value: str) -> float:
    decay = float(value)
    if not 0.0 <= decay < 1.0:
        raise ValueError("EMA decay must be in [0, 1)")
    return decay


def parse_label_smoothing(value: str) -> float:
    smoothing = float(value)
    if not 0.0 <= smoothing < 1.0:
        raise ValueError("Label smoothing must be in [0, 1)")
    return smoothing


def parse_nonnegative_float(value: str) -> float:
    # 将命令行字符串转换为浮点数，供非负损失权重使用。
    parsed = float(value)
    # 负损失权重会反向优化目标，属于无效训练配置。
    if parsed < 0.0:
        raise ValueError("Value must be non-negative")
    # 返回已验证的非负浮点数。
    return parsed


def parse_dropout(value: str) -> float:
    # 将命令行字符串转换为 dropout 概率。
    dropout = float(value)
    # PyTorch dropout 合法区间为 [0,1)，1 会丢弃全部表示。
    if not 0.0 <= dropout < 1.0:
        raise ValueError("Dropout must be in [0, 1)")
    # 返回已验证的 dropout 概率。
    return dropout


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Train IMU BP model and export ESP32 header.")
    parser.add_argument("--dataset-dir", type=Path, default=None)
    parser.add_argument("--extra-train-dir", type=Path, default=None)
    parser.add_argument("--external-holdout-dir", type=Path, default=None)
    parser.add_argument("--output-dir", type=Path, default=OUTPUT_DIR)
    parser.add_argument(
        "--primary-artifact-dir",
        type=Path,
        default=None,
        help="Reuse a validated primary BP model and train only the family specialist.",
    )
    parser.add_argument("--enable-family-specialist", action="store_true")
    # 启用六组物理特征独立编码后融合的候选 BP 结构。
    parser.add_argument("--multi-branch", action="store_true")
    # 启用每批 P 个类别、每类 K=6 个且优先跨文件的批采样策略。
    parser.add_argument("--pk-batches", action="store_true")
    # 在均匀 P×K 批次中按原训练窗口计数加权 CE，恢复类别先验。
    parser.add_argument("--pk-prior-corrected-ce", action="store_true")
    # 启用五个仅训练期使用的运动属性辅助分类头。
    parser.add_argument("--auxiliary-heads", action="store_true")
    parser.add_argument(
        "--validation-only",
        action="store_true",
        help="Train and select with validation data without constructing or evaluating test windows.",
    )
    parser.add_argument("--seed", type=int, default=SEED)
    parser.add_argument("--export-when-below-target", action="store_true")
    parser.add_argument("--max-epochs", type=int, default=MAX_EPOCHS)
    parser.add_argument(
        "--ema-decay",
        type=parse_ema_decay,
        default=0.0,
        help="Epoch-level BP parameter EMA decay; 0 disables EMA.",
    )
    parser.add_argument(
        "--label-smoothing",
        type=parse_label_smoothing,
        default=0.0,
        help="Cross-entropy label smoothing; 0 disables smoothing.",
    )
    # 暴露监督对比损失权重，便于限制其相对主交叉熵的梯度占比。
    parser.add_argument(
        "--supcon-weight",
        type=parse_nonnegative_float,
        default=SUPCON_WEIGHT,
    )
    # 暴露主模型 dropout；Round25 使用 0.20 抑制多分支过拟合。
    parser.add_argument("--dropout", type=parse_dropout, default=DROPOUT)
    parser.add_argument(
        "--window-seconds",
        type=float,
        nargs="+",
        choices=WINDOW_SECONDS_CHOICES,
        default=list(WINDOW_SECONDS_LIST),
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    # 辅助头依赖多分支模型的 32 维融合嵌入，命令行组合错误时立即终止。
    if args.auxiliary_heads and not args.multi_branch:
        raise ValueError("--auxiliary-heads requires --multi-branch")
    # 先验修正只对均匀 P×K 采样有定义，其他采样方式不能启用。
    if args.pk_prior_corrected_ce and not args.pk_batches:
        raise ValueError("--pk-prior-corrected-ce requires --pk-batches")
    global MAX_EPOCHS
    MAX_EPOCHS = args.max_epochs
    set_seed(args.seed)

    dataset_dir = resolve_dataset_dir(args.dataset_dir)
    records, class_names, label_to_idx = scan_dataset(dataset_dir)
    extra_train_records, _ = load_additional_records(
        args.extra_train_dir,
        args.external_holdout_dir,
        label_to_idx,
        validation_only=True,
    )
    feature_names = build_feature_names()
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    print(f"dataset_dir={dataset_dir.resolve()}")
    print(f"device={device}")
    print(f"class_count={len(class_names)} file_count={len(records)} feature_dim={len(feature_names)}")
    print(
        f"extra_train_file_count={len(extra_train_records)} "
        f"external_holdout_loaded=false"
    )
    print(f"class_names={class_names}")
    print(
        f"window_seconds={args.window_seconds} augment_times={AUGMENT_TIMES} "
        f"max_rotation_degrees={MAX_ROTATION_DEGREES:.1f} "
        f"supcon_weight={args.supcon_weight:.3f} hard_pair_weight={HARD_PAIR_WEIGHT:.3f} "
        f"ema_decay={args.ema_decay:.3f} "
        f"label_smoothing={args.label_smoothing:.3f} "
        f"family_specialist={args.enable_family_specialist} "
        f"multi_branch={args.multi_branch} "
        f"pk_batches={args.pk_batches} "
        f"pk_prior_corrected_ce={args.pk_prior_corrected_ce} "
        f"auxiliary_heads={args.auxiliary_heads} "
        f"dropout={args.dropout:.3f} "
        f"validation_only={args.validation_only}"
    )

    all_results: List[Dict[str, object]] = []
    for window_seconds in args.window_seconds:
        result = train_one_experiment(
            window_seconds,
            records,
            class_names,
            device,
            args.seed,
            primary_artifact_dir=args.primary_artifact_dir,
            enable_family_specialist=args.enable_family_specialist,
            validation_only=args.validation_only,
            extra_train_records=extra_train_records,
            ema_decay=args.ema_decay,
            label_smoothing=args.label_smoothing,
            multi_branch=args.multi_branch,
            pk_batches=args.pk_batches,
            auxiliary_heads=args.auxiliary_heads,
            pk_prior_corrected_ce=args.pk_prior_corrected_ce,
            supcon_weight=args.supcon_weight,
            dropout=args.dropout,
        )
        all_results.append(result)

    best_result = max(
        all_results,
        key=lambda item: (
            float(item["val_min_recall"]),
            float(item["val_f1"]),
            float(item["val_acc"]),
        ),
    )
    if args.validation_only:
        save_validation_outputs(
            best_result,
            all_results,
            class_names,
            feature_names,
            args.output_dir,
        )
        print("========== best validation experiment ==========")
        print(
            f"best_window={best_result['window_seconds']}s "
            f"val_acc={best_result['val_acc']:.4f} "
            f"val_f1={best_result['val_f1']:.4f} "
            f"val_min_recall={best_result['val_min_recall']:.4f}"
        )
        print(
            classification_report(
                np.asarray(best_result["y_val"]),
                np.asarray(best_result["val_pred"]),
                target_names=class_names,
                zero_division=0,
            )
        )
        print("validation_only=true test_evaluation_skipped=true header_export_skipped=true")
        print(f"outputs={args.output_dir.resolve()}")
        return

    _, external_holdout_records = load_additional_records(
        None,
        args.external_holdout_dir,
        label_to_idx,
        validation_only=False,
    )
    external_holdout = evaluate_external_holdout(
        best_result,
        external_holdout_records,
        class_names,
        device,
    )
    best_result["external_holdout"] = external_holdout
    print(
        f"external_holdout_loaded={not bool(external_holdout['skipped'])} "
        f"external_holdout_file_count={external_holdout.get('file_count', 0)} "
        f"external_holdout_min_recall="
        f"{external_holdout.get('min_recall', external_holdout.get('recall', float('nan'))):.4f} "
        f"external_holdout_macro_recall="
        f"{external_holdout.get('macro_recall', external_holdout.get('recall', float('nan'))):.4f}"
    )

    reached_target = save_outputs(
        best_result,
        all_results,
        class_names,
        feature_names,
        args.output_dir,
        args.export_when_below_target or EXPORT_WHEN_BELOW_TARGET,
    )

    print("========== best experiment ==========")
    print(
        f"best_window={best_result['window_seconds']}s "
        f"val_acc={best_result['val_acc']:.4f} val_f1={best_result['val_f1']:.4f} "
        f"test_acc={best_result['test_acc']:.4f} test_f1={best_result['test_f1']:.4f}"
    )
    print(
        classification_report(
            np.asarray(best_result["y_test"]),
            np.asarray(best_result["test_pred"]),
            target_names=class_names,
            zero_division=0,
        )
    )
    if reached_target:
        print(
            f"target_reached=true output_header={args.output_dir / 'esp32_bp_model.h'} "
            f"repository_header={ESP32_MODEL_HEADER}"
        )
    else:
        _, test_recalls = deployment_gate_status(best_result, class_names)
        thresholds = [
            WEAK_TARGET_MIN_CLASS_RECALL
            if name in RELAXED_RECALL_CLASS_NAMES
            else TARGET_MIN_CLASS_RECALL
            for name in class_names
        ]
        failed = [
            f"{name}:{recall:.4f}"
            for name, recall, threshold in zip(class_names, test_recalls, thresholds)
            if recall < threshold
        ]
        print(
            "target_reached=false header_export_skipped=true "
            f"failed_class_recalls={failed}"
        )
    print(f"outputs={args.output_dir.resolve()}")


if __name__ == "__main__":
    main()
