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
# 单轴陀螺仪孤立尖峰阈值，单位 deg/s；300 deg/s 只清除明显采集毛刺，保留正常快速摆腕。
PREPROCESS_GYRO_SPIKE_THRESHOLD_DPS = 300.0
# 单轴加速度计孤立尖峰阈值，单位 g；1.5 g 不会删除落地时多个轴共同出现的真实冲击。
PREPROCESS_ACC_SPIKE_THRESHOLD_G = 1.5
# 活动段检测使用 1 秒因果块，块内至少 20% 采样点活动才确认动作已经开始或尚未结束。
MOTION_TRIGGER_RATIO = 0.20
# 离线记录裁剪在首末活动点外各保留 0.5 秒，避免切掉动作准备和动作恢复阶段。
MOTION_CONTEXT_SECONDS = 0.50
# 部署端因果 logit 平滑保存当前及过去 14 个重叠窗口，最大历史范围约 6.72 秒。
TEMPORAL_LOGIT_HISTORY = 15
# Round39 固定验证选择的 Round29 基础 M0 logit 权重。
ENSEMBLE_BASE_LOGIT_WEIGHT = 0.85
# Round39 固定验证选择的 Round37 掩码 M0 logit 权重；两者之和必须为 1。
ENSEMBLE_MASKED_LOGIT_WEIGHT = 0.15
TRAIN_RATIO = 0.70
VAL_RATIO = 0.15
TEST_RATIO = 0.15
SIT_CLASS_NAME = "sit"
HIGH_DYNAMIC_CLASSES = {"jumping_jack", "jumping_lunge", "jumping_squat", "tuck_jump"}
WEAK_CLASS_NAMES = ["jumping_squat", "squat", "tuck_jump", "jumping_lunge"]
FAMILY_SPECIALIST_CLASS_NAMES = [
    "jumping_squat",
    "squat",
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
# 297 维顺序中归一化四阶段组从索引 184 开始，前置组为 112+48+24 维。
NORMALIZED_PHASE_MODEL_START = 184
# 归一化四阶段组包含 4 个信号×4 阶段×3 统计量，共 48 维，半开区间终点为 232。
NORMALIZED_PHASE_MODEL_END = 232
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
    # 手腕角速度 PCA 主轴上每秒有效符号换向次数，单位为 Hz。
    "wrist_reversal_rate_hz",
    # 手腕角速度模长归一化自相关第二时间峰与第一时间峰之比，无量纲。
    "wrist_acf_second_first_ratio",
    # 清洗后角速度模长在 0.3～3.0 秒延迟范围的第一正自相关峰，无量纲。
    "wrist_acf_first_peak",
]

HIDDEN1 = 96
HIDDEN2 = 64
HIDDEN3 = 32


@dataclass(frozen=True)
class ImuRecord:
    path: Path
    label: str
    label_idx: int


class CausalLogitSmoother:
    """用固定 15 槽环形缓冲区计算当前及历史窗口 logits 的因果均值。"""

    def __init__(
        self,
        class_count: int,
        history_length: int = TEMPORAL_LOGIT_HISTORY,
    ) -> None:
        # class_count 必须为正，代表模型输出动作类别数。
        if class_count <= 0:
            # 非正类别数无法建立 logit 向量，立即拒绝。
            raise ValueError("class_count must be positive")
        # history_length 必须为正；部署默认 15，对应最近约 6.72 秒历史范围。
        if history_length <= 0:
            # 零或负历史长度无法计算均值。
            raise ValueError("history_length must be positive")
        # class_count 保存每次 update 要求的 logits 元素数。
        self.class_count = int(class_count)
        # history_length 保存环形槽数量，Python 测试可传较小值验证边界。
        self.history_length = int(history_length)
        # history 形状 [历史窗口数,类别数]，存放尚未被淘汰的 float32 logits。
        self.history = np.zeros(
            (self.history_length, self.class_count),
            dtype=np.float32,
        )
        # running_sum 形状 [类别数]，避免每次重新遍历全部历史槽。
        self.running_sum = np.zeros(self.class_count, dtype=np.float64)
        # count 是当前有效历史窗口数，范围 0..history_length。
        self.count = 0
        # next_index 指向下一次写入或覆盖的环形槽，范围 0..history_length-1。
        self.next_index = 0

    def reset(self) -> None:
        """清空会话历史；设备重连、用户切换或明确动作段结束时调用。"""
        # 清零历史数组，防止重置后旧会话 logits 被误读。
        self.history.fill(0.0)
        # 清零逐类累计和。
        self.running_sum.fill(0.0)
        # 有效槽数归零，下一次 update 只返回当前窗口分数。
        self.count = 0
        # 写指针回到第一个槽，保持 Python/C 确定性一致。
        self.next_index = 0

    def update(self, logits: np.ndarray) -> np.ndarray:
        """加入当前 [类别数] logits 并返回只使用当前及过去窗口的均值。"""
        # values 转为 float32 一维数组，形状必须严格等于 [class_count]。
        values = np.asarray(logits, dtype=np.float32).reshape(-1)
        # 类别数不一致会使 Python/C 环形数组错位，立即拒绝。
        if values.shape != (self.class_count,):
            # 异常同时报告实际和期望形状。
            raise ValueError(
                f"Expected logits shape ({self.class_count},), got {values.shape}"
            )
        # 缓冲区已满时，先从累计和中移除将被覆盖的最旧槽。
        if self.count == self.history_length:
            # 转为 float64 累减，降低长期更新的累计舍入误差。
            self.running_sum -= self.history[self.next_index].astype(np.float64)
        else:
            # 未满时有效窗口数增加一，最大不超过 history_length。
            self.count += 1
        # 把当前 logits 写入 next_index 指向的槽。
        self.history[self.next_index] = values
        # 累计当前窗口逐类分数。
        self.running_sum += values.astype(np.float64)
        # 写指针循环前进，末槽之后回到零槽。
        self.next_index = (self.next_index + 1) % self.history_length
        # 逐类累计和除以有效窗口数，返回 float32 无量纲平均 logits。
        return (self.running_sum / float(self.count)).astype(np.float32)


def combine_ensemble_logits(
    base_logits: np.ndarray,
    masked_logits: np.ndarray,
    masked_weight: float = ENSEMBLE_MASKED_LOGIT_WEIGHT,
) -> np.ndarray:
    """按固定验证权重组合两个同类别顺序 M0 的无量纲 logits。"""
    # base 和 masked 可为 [类别数] 或 [样本数,类别数]，形状必须完全相同。
    base = np.asarray(base_logits, dtype=np.float32)
    # masked 转为 float32，避免两个模型输出精度不同。
    masked = np.asarray(masked_logits, dtype=np.float32)
    # 形状不一致会使窗口或类别错位，立即拒绝。
    if base.shape != masked.shape:
        # 错误消息同时报告两个形状，便于定位批次或类别合同错误。
        raise ValueError(f"Ensemble logit shapes differ: {base.shape} vs {masked.shape}")
    # 至少需要一个类别维；空标量不能表示分类 logits。
    if base.ndim == 0 or base.shape[-1] <= 0:
        # 拒绝空类别输入，防止 argmax 在部署层失败。
        raise ValueError("Ensemble logits must have a non-empty class dimension")
    # 任一模型出现 NaN 或无穷值时拒绝融合，防止异常分数进入动作段长期状态。
    if not np.all(np.isfinite(base)) or not np.all(np.isfinite(masked)):
        # 调用方应丢弃当前窗口并记录模型或标准化异常，不能用零值静默替换。
        raise ValueError("Ensemble logits must be finite")
    # 权重必须有限且位于 [0,1]，否则不再是两个模型的凸组合。
    if not np.isfinite(masked_weight) or not 0.0 <= masked_weight <= 1.0:
        # 不静默夹紧，避免验证选择和部署权重不一致。
        raise ValueError("masked_weight must be finite and in [0, 1]")
    # base_weight 与 masked_weight 和为 1，保持两个模型 logit 总尺度稳定。
    base_weight = np.float32(1.0 - float(masked_weight))
    # 返回 float32 凸组合；时间复杂度 O(样本数×类别数)，无额外模型参数。
    return base_weight * base + np.float32(masked_weight) * masked


class CausalBoutLogitAccumulator:
    """从活动段开始累计当前及全部过去 logits，并在动作段结束时显式重置。"""

    def __init__(self, class_count: int) -> None:
        # class_count 必须为正，代表两个 M0 共享的输出类别数。
        if class_count <= 0:
            # 非正类别数无法建立累计向量。
            raise ValueError("class_count must be positive")
        # class_count 保存每次 update 要求的 logits 长度。
        self.class_count = int(class_count)
        # running_sum 形状 [类别数]，float64 降低长动作段累计舍入误差。
        self.running_sum = np.zeros(self.class_count, dtype=np.float64)
        # count 是当前活动段从开始到当前的窗口数，重置后为 0。
        self.count = 0

    def reset(self) -> None:
        """在静止、动作切换、设备重连或用户切换时清空当前动作段证据。"""
        # 清零全部类别累计和，防止前一动作影响后一动作。
        self.running_sum.fill(0.0)
        # 窗口计数归零，下一次 update 只由当前窗口决定。
        self.count = 0

    def update(self, logits: np.ndarray) -> np.ndarray:
        """加入当前 [类别数] logits 并返回从动作段开始到当前的因果均值。"""
        # values 转为 float32 一维数组，允许调用方传列表或 NumPy 向量。
        values = np.asarray(logits, dtype=np.float32).reshape(-1)
        # 类别维必须与构造时完全一致。
        if values.shape != (self.class_count,):
            # 异常包含实际和期望形状，防止类别顺序错位。
            raise ValueError(
                f"Expected logits shape ({self.class_count},), got {values.shape}"
            )
        # 非有限 logits 会永久污染累计和，必须在进入状态前拒绝。
        if not np.all(np.isfinite(values)):
            # NaN/Inf 通常来自模型或标准化异常，调用方应丢弃当前窗口并记录错误。
            raise ValueError("Bout logits must be finite")
        # 累加当前窗口全部类别分数，只使用当前和过去证据。
        self.running_sum += values.astype(np.float64)
        # 当前动作段有效窗口数增加一。
        self.count += 1
        # 累计和除以窗口数得到无量纲平均 logits，并转为部署一致的 float32。
        return (self.running_sum / float(self.count)).astype(np.float32)


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

    # 六组维度严格对应 build_feature_names() 的生产顺序，总和必须为 297。
    group_input_dims = (112, 48, 24, 48, 32, 33)
    # 各分支压缩到较小嵌入，限制参数量并避免 33 个弱类特征被 112 个统计量淹没。
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
        # 输入张量形状为 [批大小,297]，297 个值均为训练集统计量标准化后的无量纲特征。
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


class DeepNarrowMultiBranchBPNet(nn.Module):
    """按审核方案增加融合深度、保持逐层收缩的轻量 M1 BP。"""

    # 六组输入严格复用 297 维生产顺序，不改变任何特征边界。
    group_input_dims = (112, 48, 24, 48, 32, 33)
    # 仅把弱类分支输出增到 24，其余五个分支与 M0 相同，拼接后为 88 维。
    group_output_dims = (24, 12, 8, 12, 8, 24)

    def __init__(self, input_dim: int, class_count: int, dropout: float = DROPOUT):
        # 初始化 PyTorch 模块注册表，使全部分支和融合层参与优化及保存。
        super().__init__()
        # 输入必须等于六组总和 297，防止切片错位。
        if input_dim != sum(self.group_input_dims):
            # 错误消息同时报告期望和实际维度，便于发现旧 296/302 维缓存。
            raise ValueError(
                f"Deep-narrow model requires {sum(self.group_input_dims)} features, got {input_dim}"
            )
        # 每个分支独立执行 Linear-ReLU，避免 112 维统计组淹没 32 维弱类组。
        self.branches = nn.ModuleList(
            [
                # 当前分支把固定输入组映射到审核指定输出宽度。
                nn.Sequential(nn.Linear(input_size, output_size), nn.ReLU())
                # zip 保持六组输入和输出一一对应。
                for input_size, output_size in zip(
                    self.group_input_dims,
                    self.group_output_dims,
                )
            ]
        )
        # 融合层从 88 维逐级收缩到 24 维，不使用 BatchNorm/LayerNorm。
        self.fusion = nn.Sequential(
            # 第一融合层执行 88→64。
            nn.Linear(sum(self.group_output_dims), 64),
            # ReLU 便于 ESP32 使用 max(0,x) 精确复现。
            nn.ReLU(),
            # 只在训练时随机丢弃 64 维表示，推理时自动关闭。
            nn.Dropout(dropout),
            # 第二融合层执行 64→48。
            nn.Linear(64, 48),
            # 第二层继续使用 ReLU。
            nn.ReLU(),
            # 48 维层后使用同一 dropout，不引入额外超参数。
            nn.Dropout(dropout),
            # 第三融合层执行 48→32。
            nn.Linear(48, 32),
            # 第三层 ReLU 输出非负 32 维表示。
            nn.ReLU(),
            # 第四融合层执行 32→24，形成最终共享嵌入。
            nn.Linear(32, 24),
            # 最终嵌入同样使用 ReLU，保持全网络激活一致。
            nn.ReLU(),
        )
        # 主分类头把 24 维嵌入线性映射到 11 类 logits。
        self.classifier = nn.Linear(24, class_count)

    def forward_features(self, x: torch.Tensor) -> torch.Tensor:
        """把 [批大小,297] 输入编码为 [批大小,24] M1 嵌入。"""
        # branch_outputs 按固定物理组顺序保存六个分支输出。
        branch_outputs: List[torch.Tensor] = []
        # offset 指向当前分支在 302 维输入中的起始列。
        offset = 0
        # 顺序遍历六分支及其固定输入维度。
        for branch, input_dim in zip(self.branches, self.group_input_dims):
            # 当前切片形状为 [批大小,当前组输入维度]。
            group_values = x[:, offset : offset + input_dim]
            # 分支输出追加到列表，等待按特征组顺序拼接。
            branch_outputs.append(branch(group_values))
            # 起始列移动到下一组。
            offset += input_dim
        # 六组拼接为 88 维并通过深窄融合层输出 24 维嵌入。
        return self.fusion(torch.cat(branch_outputs, dim=1))

    def classify_features(self, embeddings: torch.Tensor) -> torch.Tensor:
        """把 [批大小,24] 嵌入映射为 [批大小,类别数] logits。"""
        # 线性分类头不使用 softmax；交叉熵内部完成稳定 log-softmax。
        return self.classifier(embeddings)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """执行 M1 六分支、深窄融合和主分类完整前向。"""
        # 先提取 24 维表示，再返回类别 logits。
        return self.classify_features(self.forward_features(x))


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


def preprocess_imu_window(window: np.ndarray) -> np.ndarray:
    """修复六轴手腕 IMU 中的单轴孤立尖峰，输入和输出形状均为 [N,6]。"""
    # data 固定使用 float32，六列依次为 gx、gy、gz、ax、ay、az；前三列单位 deg/s，后三列单位 g。
    data = np.asarray(window, dtype=np.float32)
    # 非二维或通道数不为 6 的输入无法与 ESP32 固定六轴数组对应，立即拒绝以防通道错位。
    if data.ndim != 2 or data.shape[1] != 6:
        # 异常信息包含实际形状，便于定位数据文件列数或调用方切片错误。
        raise ValueError(f"Expected IMU shape (n, 6), got {data.shape}")
    # cleaned 是独立副本，调用方仍可保留原始采样用于质量追踪和问题复现。
    cleaned = data.copy()
    # 少于三个点时不存在同时具备左右邻点的中心点，直接返回原样副本。
    if len(data) < 3:
        # 返回值始终为 float32 的 [N,6] 数组，与正常清洗路径保持相同接口。
        return cleaned
    # thresholds 按六轴顺序保存判定阈值，前三项单位 deg/s，后三项单位 g。
    thresholds = np.asarray(
        [
            PREPROCESS_GYRO_SPIKE_THRESHOLD_DPS,
            PREPROCESS_GYRO_SPIKE_THRESHOLD_DPS,
            PREPROCESS_GYRO_SPIKE_THRESHOLD_DPS,
            PREPROCESS_ACC_SPIKE_THRESHOLD_G,
            PREPROCESS_ACC_SPIKE_THRESHOLD_G,
            PREPROCESS_ACC_SPIKE_THRESHOLD_G,
        ],
        dtype=np.float32,
    )
    # 遍历除首末点外的每个采样时刻；首末点缺少双侧证据，因此不做推断修复。
    for index in range(1, len(data) - 1):
        # neighbor_mean 是原始前后邻点均值；始终读取 data，避免前一次修复级联改变后续判定。
        neighbor_mean = (data[index - 1] + data[index + 1]) * np.float32(0.5)
        # center_residual 表示当前点相对邻点均值的六轴绝对偏差，单位分别为 deg/s 和 g。
        center_residual = np.abs(data[index] - neighbor_mean)
        # neighbor_gap 衡量前后邻点自身是否一致，防止在真实快速变化或冲击边沿进行插值。
        neighbor_gap = np.abs(data[index - 1] - data[index + 1])
        # candidate_mask 仅接受中心偏差超阈值且两侧差异小于半阈值的轴。
        candidate_mask = (center_residual > thresholds) & (
            neighbor_gap < thresholds * np.float32(0.5)
        )
        # candidate_axes 保存当前时刻满足孤立尖峰条件的通道编号，范围为 0..5。
        candidate_axes = np.flatnonzero(candidate_mask)
        # 只修复恰好一个轴异常的采样点；多轴同时突变按真实身体冲击或转动保留。
        if len(candidate_axes) == 1:
            # axis 是唯一异常通道，通道单位由其位于陀螺仪区或加速度区决定。
            axis = int(candidate_axes[0])
            # 用原始前后邻点均值替换该轴中心值，输出仍保持原采样率和时间长度。
            cleaned[index, axis] = neighbor_mean[axis]
    # 返回清洗后的六轴窗口；时间复杂度 O(6N)，额外空间 O(6N)。
    return cleaned


def motion_segment_bounds(
    data: np.ndarray,
    active_point_threshold: float,
) -> Tuple[int, int]:
    """返回离线记录中包含动作及上下文的半开区间 [start,end)。"""
    # cleaned 仅用于活动检测，保证孤立采集尖峰不会错误扩大保留范围。
    cleaned = preprocess_imu_window(data)
    # 空记录没有可保留采样点，返回合法空半开区间。
    if len(cleaned) == 0:
        # start=end=0 表示输入为空，不会触发负索引或越界切片。
        return 0, 0
    # scores 是逐点活动强度，形状 [N]；加速度变化单位 g，角速度项按 200 deg/s 归一化。
    scores = instantaneous_motion(cleaned)
    # active 标记每个采样点是否超过由静坐训练记录估计的活动阈值。
    active = scores > float(active_point_threshold)
    # block_points 对应 1 秒采样点数；25 Hz 下固定为 25 点。
    block_points = max(1, int(SAMPLE_RATE))
    # 记录短于 1 秒时用完整长度作为唯一检测块，避免卷积 valid 模式返回空数组。
    block_points = min(block_points, len(cleaned))
    # active_counts 统计每个滑动 1 秒块中的活动点数，输出长度为 N-block_points+1。
    active_counts = np.convolve(
        active.astype(np.int32),
        np.ones(block_points, dtype=np.int32),
        mode="valid",
    )
    # trigger_offsets 保存活动点比例不低于 20% 的块起点，用连续证据抑制单点误触发。
    trigger_offsets = np.flatnonzero(
        active_counts >= MOTION_TRIGGER_RATIO * float(block_points)
    )
    # 没有活动块时保留完整记录；静坐类别和极弱动作不会被错误裁成空数据。
    if len(trigger_offsets) == 0:
        # 返回整个 [0,N) 半开区间，调用方可以无条件按返回值切片。
        return 0, len(cleaned)
    # first_block_start 是首个确认活动块的起点索引。
    first_block_start = int(trigger_offsets[0])
    # first_block 在原记录中的范围不超过 block_points，并至少含一个活动点。
    first_block = active[first_block_start : first_block_start + block_points]
    # first_active 定位首块内第一个真实活动点，避免把整块前沿都误当作动作。
    first_active = first_block_start + int(np.flatnonzero(first_block)[0])
    # last_block_start 是最后一个确认活动块的起点索引。
    last_block_start = int(trigger_offsets[-1])
    # last_block 用于定位末块内最后一个真实活动点。
    last_block = active[last_block_start : last_block_start + block_points]
    # last_active 是闭区间活动点索引，后续转换为半开区间终点。
    last_active = last_block_start + int(np.flatnonzero(last_block)[-1])
    # context_points 将 0.5 秒换算为采样点；Python 与测试约定 25 Hz 下四舍五入为 12 点。
    context_points = int(round(MOTION_CONTEXT_SECONDS * SAMPLE_RATE))
    # start 在首活动点前保留上下文，并夹紧到记录起点 0。
    start = max(0, first_active - context_points)
    # end 在末活动点后保留上下文，+1 将闭区间活动点转换为半开区间。
    end = min(len(cleaned), last_active + 1 + context_points)
    # 返回半开区间，保证 data[start:end] 正好包含动作及指定上下文。
    return start, end


def trim_record_to_motion_segment(
    data: np.ndarray,
    label: str,
    active_point_threshold: float,
) -> np.ndarray:
    """裁剪离线非静坐记录的首尾长静止段，返回原始数据视图。"""
    # 静坐本身就是目标动作，不能使用活动检测裁剪，否则会破坏该类分布。
    if label == SIT_CLASS_NAME:
        # 返回完整静坐记录，单位、形状和原数组均不改变。
        return data
    # 根据清洗后的活动证据计算半开区间，但最终切片仍取原始数据以避免重复预处理。
    start, end = motion_segment_bounds(data, active_point_threshold)
    # 返回原始六轴记录的动作区间；后续每个窗口在特征入口统一执行一次尖峰修复。
    return data[start:end]


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
    # 先清除单轴孤立毛刺，再计算运动强度，防止一个坏点把静止窗口误判成有效动作。
    cleaned = preprocess_imu_window(window)
    # score 是清洗窗口的整体活动分数，供静坐和动态动作使用同一门槛体系。
    score = motion_score(cleaned)
    if label == SIT_CLASS_NAME:
        return score <= rest_threshold * 1.6
    if score < rest_threshold:
        return False
    if label in HIGH_DYNAMIC_CLASSES:
        return (
            score >= rest_threshold * 1.25
            and active_ratio(cleaned, active_point_threshold) >= MOTION_TRIGGER_RATIO
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
        # 静坐阈值也基于清洗窗口估计，保证训练筛选与 ESP32 实时特征入口使用相同信号定义。
        scores.extend(
            motion_score(preprocess_imu_window(window))
            for window in iter_windows(data, window_len, step_len)
        )
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
            # 逐点活动阈值排除单轴尖峰贡献，防止静坐基线被传感器毛刺抬高。
            scores.extend(
                instantaneous_motion(preprocess_imu_window(window)).tolist()
            )
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


def _wrist_principal_gyro_projection(window: np.ndarray) -> np.ndarray:
    """返回手腕角速度在固定幂迭代 PCA 主轴上的投影，单位为 deg/s。"""
    # 输入转换为 float64；通道顺序固定为 gx、gy、gz、ax、ay、az。
    data = np.asarray(window, dtype=np.float64)
    # 非法或空窗口返回空序列，调用方将其解释为零换向。
    if data.ndim != 2 or data.shape[1] != 6 or len(data) == 0:
        return np.zeros(0, dtype=np.float64)
    # 取三轴手腕角速度并去除窗口均值，抑制陀螺零偏。
    centered = data[:, 0:3] - np.mean(data[:, 0:3], axis=0, keepdims=True)
    # 未除以 N 的二阶矩与协方差具有相同特征向量，矩阵形状为 [3,3]。
    moment = centered.T @ centered
    # 选择对角能量最大的传感器轴作为幂迭代初始方向，避免固定 x 轴与主轴正交。
    initial_axis = int(np.argmax(np.diag(moment)))
    # axis 是长度为 3 的单位向量，初值只有主能量轴分量为 1。
    axis = np.zeros(3, dtype=np.float64)
    # 写入确定性初始分量。
    axis[initial_axis] = 1.0
    # 固定执行 8 次幂迭代，在 3×3 对称矩阵上逼近最大特征向量。
    for _ in range(8):
        # 左乘二阶矩放大最大特征方向分量。
        next_axis = moment @ axis
        # 二范数用于恢复单位向量。
        norm = float(np.linalg.norm(next_axis))
        # 近静止窗口矩阵能量过小，没有可靠主轴，返回全零投影。
        if norm <= 1e-12:
            return np.zeros(len(data), dtype=np.float64)
        # 归一化后进入下一次固定迭代。
        axis = next_axis / norm
    # PCA 轴整体正负任意，使用绝对值最大分量固定符号。
    anchor = int(np.argmax(np.abs(axis)))
    # 锚点为负时翻转整条轴，使 Python/C 对相同窗口得到一致符号。
    if float(axis[anchor]) < 0.0:
        axis = -axis
    # 返回每个采样点沿手腕主转动方向的带符号角速度。
    return centered @ axis


def _wrist_reversal_rate_hz(window: np.ndarray) -> float:
    """计算手腕主角速度每秒有效换向次数，单位为 Hz。"""
    # 获得固定幂迭代主轴投影，形状为 [时间点数]。
    projection = _wrist_principal_gyro_projection(window)
    # 少于两个点无法形成换向。
    if len(projection) < 2:
        return 0.0
    # 边缘复制后使用三点平均，抑制单点噪声导致的伪符号翻转。
    smoothed = np.convolve(
        np.pad(projection, (1, 1), mode="edge"),
        np.ones(3, dtype=np.float64) / 3.0,
        mode="valid",
    )
    # 90 分位幅值的 15% 与 10 deg/s 取较大值，过滤静止零点附近抖动。
    threshold = max(10.0, 0.15 * float(np.percentile(np.abs(smoothed), 90)))
    # 只保留达到门槛的采样点，零附近点不参与符号比较。
    valid = smoothed[np.abs(smoothed) >= threshold]
    # 少于两个有效点时没有可靠换向。
    if len(valid) < 2:
        return 0.0
    # 相邻有效值乘积小于零表示一次主摆动方向换向。
    reversal_count = int(np.sum(valid[:-1] * valid[1:] < 0.0))
    # 窗口时长为 N/25 秒，换向次数除以时长得到 Hz。
    return reversal_count / (len(smoothed) / float(SAMPLE_RATE))


def _wrist_acf_second_first_ratio(gyro_magnitude: np.ndarray) -> float:
    """计算手腕角速度模长自相关第二时间峰与第一时间峰之比。"""
    # 输入转换为 float64 一维序列，单位为 deg/s。
    values = np.asarray(gyro_magnitude, dtype=np.float64).reshape(-1)
    # 少于 10 点时不存在审批方案要求的 0.3 秒延迟范围。
    if len(values) < 10:
        return 0.0
    # 去除窗口均值，使自相关描述动态周期而非直流幅值。
    centered = values - float(np.mean(values))
    # 零延迟能量作为全部延迟统一分母。
    energy = float(np.dot(centered, centered))
    # 近静止窗口无周期证据，返回零。
    if energy <= 1e-12:
        return 0.0
    # 最小延迟为 round(0.30*25)=8 点，排除三点平滑尺度内的伪峰。
    minimum_lag = max(2, int(round(0.30 * SAMPLE_RATE)))
    # 最大延迟为 3 秒或半窗，取较小值保证足够重叠样本。
    maximum_lag = min(int(round(3.0 * SAMPLE_RATE)), len(values) // 2)
    # 无合法延迟范围时返回零。
    if maximum_lag <= minimum_lag:
        return 0.0
    # 按时间顺序计算 lag=minimum_lag..maximum_lag 的归一化自相关。
    correlations = np.asarray(
        [
            float(np.dot(centered[:-lag], centered[lag:]) / energy)
            for lag in range(minimum_lag, maximum_lag + 1)
        ],
        dtype=np.float64,
    )
    # 少于三个相关点不能定义两个内部时间峰。
    if len(correlations) < 3:
        return 0.0
    # 内部局部峰须严格高于左点、不低于右点且为正。
    peak_offsets = np.flatnonzero(
        (correlations[1:-1] > correlations[:-2])
        & (correlations[1:-1] >= correlations[2:])
        & (correlations[1:-1] > 0.0)
    ) + 1
    # 少于两个时间峰时没有第二/第一峰结构。
    if len(peak_offsets) < 2:
        return 0.0
    # 第一峰按时间最早而非幅值最大定义。
    first_peak = max(float(correlations[int(peak_offsets[0])]), 0.0)
    # 第一峰过小时不执行除法。
    if first_peak <= 1e-12:
        return 0.0
    # 第二个时间峰除以第一峰，并限制异常边界到 [0,5]。
    return float(
        np.clip(
            max(float(correlations[int(peak_offsets[1])]), 0.0) / first_peak,
            0.0,
            5.0,
        )
    )


def _wrist_acf_first_peak(gyro_magnitude: np.ndarray) -> float:
    """返回手腕角速度模长在 0.3～3.0 秒延迟范围的第一正自相关峰。"""
    # values 是一维角速度模长，单位 deg/s；只依赖手腕三轴陀螺仪。
    values = np.asarray(gyro_magnitude, dtype=np.float64).reshape(-1)
    # 少于 10 点时无法覆盖 0.3 秒最小延迟，返回无周期证据的零值。
    if len(values) < 10:
        # 零值保持特征有限且与 C 端边界处理一致。
        return 0.0
    # centered 去除窗口均值，使相关只描述重复波形而不是角速度直流偏置。
    centered = values - float(np.mean(values))
    # energy 是零延迟中心化能量，单位 (deg/s)^2。
    energy = float(np.dot(centered, centered))
    # 近静止窗口能量过小，归一化无物理意义并可能除零。
    if energy <= 1e-12:
        # 返回零表示没有可靠周期峰。
        return 0.0
    # minimum_lag=8 点对应约 0.32 秒，排除传感器短时抖动和相邻点平滑伪峰。
    minimum_lag = max(2, int(round(0.30 * SAMPLE_RATE)))
    # maximum_lag 取 3 秒与半窗中较小值，保证每个延迟仍有足够重叠点。
    maximum_lag = min(int(round(3.0 * SAMPLE_RATE)), len(values) // 2)
    # 最大延迟不大于最小延迟时没有可分析区间。
    if maximum_lag <= minimum_lag:
        # 返回零保持短窗口行为确定。
        return 0.0
    # correlations 依时间延迟升序保存归一化自相关，形状为 [最大延迟-最小延迟+1]。
    correlations = np.asarray(
        [
            float(np.dot(centered[:-lag], centered[lag:]) / energy)
            for lag in range(minimum_lag, maximum_lag + 1)
        ],
        dtype=np.float64,
    )
    # 少于三个相关点时不能定义内部峰，退化为区间最大正相关。
    if len(correlations) < 3:
        # 与候选分析器一致，将负最大值夹紧为零。
        return max(float(np.max(correlations)), 0.0)
    # peak_offsets 找到严格高于左点、不低于右点且为正的内部局部峰。
    peak_offsets = np.flatnonzero(
        (correlations[1:-1] > correlations[:-2])
        & (correlations[1:-1] >= correlations[2:])
        & (correlations[1:-1] > 0.0)
    ) + 1
    # 没有内部峰时使用范围内最大正相关，避免周期落在搜索边界时丢失证据。
    if len(peak_offsets) == 0:
        # 最大正相关理论范围不超过 1，浮点误差由训练标准化吸收。
        return max(float(np.max(correlations)), 0.0)
    # 第一峰按时间最早定义，表示最短可靠重复周期的相关强度。
    return max(float(correlations[int(peak_offsets[0])]), 0.0)


def _additional_wrist_features(
    window: np.ndarray,
    gyro_magnitude: np.ndarray,
) -> List[float]:
    """提取 6 项三折稳定的手腕形态特征；输入形状分别为 [N,6] 和 [N]。"""
    # 转为 float64 可降低相关、能量和离散傅里叶统计的累计误差。
    data = np.asarray(window, dtype=np.float64)
    # 角速度模长单位为 deg/s，长度必须与六轴窗口时间点数一致。
    gyro_values = np.asarray(gyro_magnitude, dtype=np.float64).reshape(-1)
    # 非法输入返回固定六维零向量，避免异常窗口改变生产特征长度。
    if data.ndim != 2 or data.shape[1] != 6 or len(data) == 0 or len(gyro_values) != len(data):
        return [0.0] * 6
    # half_length 取窗口整半长度；奇数窗口中间点不参与前后形状比较。
    half_length = len(data) // 2
    # 前半段表示一次手腕外摆过程，单位仍为 deg/s。
    first_half = gyro_values[:half_length]
    # 后半段倒序后表示回摆轨迹按相同时间方向展开。
    reversed_second_half = gyro_values[-half_length:][::-1]
    # 少于两个点或近常量波形无法定义皮尔逊相关，确定性返回零。
    if half_length < 2:
        out_in_shape_correlation = 0.0
    else:
        # 两条半窗分别去均值，只比较归一化波形而不重复编码总体强度。
        first_centered = first_half - float(np.mean(first_half))
        # 倒序后半窗同样去均值。
        second_centered = reversed_second_half - float(np.mean(reversed_second_half))
        # 分母是两个中心化向量二范数乘积。
        denominator = float(np.linalg.norm(first_centered) * np.linalg.norm(second_centered))
        # 近常量输入使用零；其余结果截断到理论范围 [-1,1]。
        out_in_shape_correlation = (
            float(np.clip(np.dot(first_centered, second_centered) / denominator, -1.0, 1.0))
            if denominator > 1e-12
            else 0.0
        )
    # acceleration 形状为 [N,3]，通道顺序 ax、ay、az，单位为 g。
    acceleration = data[:, 3:6]
    # 原始加速度模长是手腕 specific-force 大小，单位为 g。
    acc_magnitude = np.linalg.norm(acceleration, axis=1)
    # 减去窗口三轴均值，得到不把静态重力方向当动作能量的局部动态加速度。
    dynamic_acceleration = acceleration - np.mean(acceleration, axis=0, keepdims=True)
    # 动态加速度模长单位为 g。
    dynamic_acc_magnitude = np.linalg.norm(dynamic_acceleration, axis=1)
    # jerk 首点无前驱，固定为零；其余点单位为 g/s。
    jerk = np.zeros(len(data), dtype=np.float64)
    # 至少两个时间点时，用相邻 specific-force 模长绝对差乘采样率计算 jerk。
    if len(data) > 1:
        jerk[1:] = np.abs(np.diff(acc_magnitude)) * float(SAMPLE_RATE)
    # 三点平滑使用边缘复制，保持输出长度并抑制单点量化尖峰。
    if len(jerk) >= 3:
        smoothed_jerk = np.convolve(
            np.pad(jerk, (1, 1), mode="edge"),
            np.ones(3, dtype=np.float64) / 3.0,
            mode="valid",
        )
    else:
        # 短输入无法形成完整三点核，直接复制原序列。
        smoothed_jerk = jerk.copy()
    # argmax 平局时返回最早索引，与 C 端严格大于才更新的规则一致。
    event_index = int(np.argmax(smoothed_jerk))
    # 0.4 秒上下文在 25 Hz 下为 10 点，并至少保留两个点。
    context_points = max(2, int(round(0.40 * SAMPLE_RATE)))
    # 事件前半开区间起点不能小于零。
    pre_start = max(0, event_index - context_points)
    # 事件后区间包含事件点，终点不能超过窗口长度。
    post_end = min(len(data), event_index + context_points + 1)
    # 事件后的平滑 jerk 局部序列用于冲击持续宽度。
    post_jerk = smoothed_jerk[event_index:post_end]
    # 理论空区间以零峰值处理；正常输入至少包含事件点。
    post_jerk_peak = float(np.max(post_jerk)) if len(post_jerk) else 0.0
    # 半高门槛为事件后局部峰值的一半。
    half_height = 0.5 * post_jerk_peak
    # 从事件点开始累计连续不低于半高的点数。
    half_width_points = 0
    # 顺序遍历最多 0.4 秒的事件后局部点，首次跌破半高即停止。
    for value in post_jerk:
        # 零峰值不应产生整段宽度；正峰值才累计有效点。
        if post_jerk_peak > 1e-12 and float(value) >= half_height:
            half_width_points += 1
        else:
            break
    # 点数除以采样率得到冲击半高宽，单位为秒。
    post_jerk_half_width = half_width_points / float(SAMPLE_RATE)
    # 事件前动态加速度平方和是准备阶段手腕运动能量代理，单位为 g^2。
    pre_dynamic_energy = float(np.sum(np.square(dynamic_acc_magnitude[pre_start:event_index])))
    # 事件后同物理时长动态能量描述落地或回收响应。
    post_dynamic_energy = float(np.sum(np.square(dynamic_acc_magnitude[event_index:post_end])))
    # 1e-9 同时保护静止窗口的零分子和零分母，输出为无量纲自然对数比。
    post_pre_log_ratio = float(
        math.log((max(post_dynamic_energy, 0.0) + 1e-9) / (max(pre_dynamic_energy, 0.0) + 1e-9))
    )
    # 动态加速度中位数建立不受孤立冲击影响的窗口基线。
    dynamic_median = float(np.median(dynamic_acc_magnitude))
    # MAD 是动态模长到中位数绝对距离的中位数，单位为 g。
    dynamic_mad = float(np.median(np.abs(dynamic_acc_magnitude - dynamic_median)))
    # 恢复门槛至少为 0.05g，避免静止量化噪声导致虚假长恢复。
    recovery_threshold = max(0.05, dynamic_median + 0.5 * dynamic_mad)
    # 默认窗口末点仍未恢复；输出因此接近一。
    recovery_index = len(data) - 1
    # 从事件后一点扫描到倒数第二点，确保 index+1 始终有效。
    for index in range(event_index + 1, max(event_index + 1, len(data) - 1)):
        # 连续两点均回到门槛内时，首点定义为恢复位置。
        if dynamic_acc_magnitude[index] <= recovery_threshold and dynamic_acc_magnitude[index + 1] <= recovery_threshold:
            recovery_index = index
            break
    # 以事件后剩余窗口长度归一化恢复时间，正常范围为 [0,1]。
    recovery_time_ratio = (recovery_index - event_index) / float(max(len(data) - 1 - event_index, 1))
    # 周期峰检测先进行与分析脚本一致的三点边缘平滑。
    if len(gyro_values) >= 3:
        smoothed_gyro = np.convolve(
            np.pad(gyro_values, (1, 1), mode="edge"),
            np.ones(3, dtype=np.float64) / 3.0,
            mode="valid",
        )
    else:
        # 少于三点时不存在内部周期峰。
        smoothed_gyro = gyro_values.copy()
    # 峰门槛使用未平滑角速度模长的中位数加 0.5 倍总体标准差。
    cycle_threshold = float(np.median(gyro_values) + 0.5 * np.std(gyro_values))
    # 找到严格高于左点、不低于右点且达到门槛的内部候选峰。
    candidates = (
        np.flatnonzero(
            (smoothed_gyro[1:-1] > smoothed_gyro[:-2])
            & (smoothed_gyro[1:-1] >= smoothed_gyro[2:])
            & (smoothed_gyro[1:-1] >= cycle_threshold)
        )
        + 1
        if len(smoothed_gyro) >= 3
        else np.zeros(0, dtype=np.int64)
    )
    # 最小峰距为 0.3 秒，25 Hz 时 round 后为 8 点。
    minimum_distance = max(2, int(round(0.30 * SAMPLE_RATE)))
    # 先按峰值降序、再按索引升序处理，近邻只保留更强峰。
    ordered_candidates = sorted(candidates.tolist(), key=lambda index: (-float(smoothed_gyro[index]), index))
    # selected_peaks 保存通过最小间隔检查的时间索引。
    selected_peaks: List[int] = []
    # 逐个处理强峰，避免弱邻峰抢占周期位置。
    for index in ordered_candidates:
        # 与全部已选峰距离均达标时保留当前峰。
        if all(abs(index - kept) >= minimum_distance for kept in selected_peaks):
            selected_peaks.append(index)
    # 时间升序后相邻差值才表示真实峰间隔。
    selected_peaks.sort()
    # 峰间隔单位为采样点；计算变异系数后单位抵消。
    cycle_intervals = np.diff(np.asarray(selected_peaks, dtype=np.float64))
    # 至少三个峰形成两个间隔时才有周期稳定性证据。
    cycle_interval_cv = (
        float(np.std(cycle_intervals) / np.mean(cycle_intervals))
        if len(cycle_intervals) >= 2 and float(np.mean(cycle_intervals)) > 1e-12
        else 0.0
    )
    # 去均值角速度模长只保留交流运动成分。
    centered_gyro = gyro_values - float(np.mean(gyro_values))
    # 汉宁窗减少非整数周期在离散频点间的能量泄漏。
    power = np.square(np.abs(np.fft.rfft(centered_gyro * np.hanning(len(centered_gyro)))))
    # 直流分量不属于动作周期，显式清零。
    if len(power):
        power[0] = 0.0
    # 近静止或频谱不足三个点时，二次谐波比定义为零。
    if len(power) < 3 or float(np.sum(power)) <= 1e-12:
        harmonic_ratio = 0.0
    else:
        # 最大功率频点作为基频索引，平局时保留较低频点。
        dominant_index = int(np.argmax(power))
        # 二次谐波索引超过奈奎斯特端时截断到最后频点。
        harmonic_index = min(2 * dominant_index, len(power) - 1)
        # 二次谐波功率除以基频功率，1e-12 防止除零。
        harmonic_ratio = float(power[harmonic_index] / max(float(power[dominant_index]), 1e-12))
    # 按 WEAK_CLASS_FEATURE_NAMES 末六项的固定顺序组装结果。
    result = np.asarray(
        [
            out_in_shape_correlation,
            post_jerk_half_width,
            post_pre_log_ratio,
            recovery_time_ratio,
            cycle_interval_cv,
            harmonic_ratio,
        ],
        dtype=np.float32,
    )
    # 极端输入若产生非有限数，统一替换为零，避免污染标准化和 BP 权重。
    return np.nan_to_num(result, nan=0.0, posinf=0.0, neginf=0.0).astype(float).tolist()


def weak_class_features(series: Dict[str, np.ndarray]) -> List[float]:
    """提取 38 项弱类特征；每个输入值形状为 [时间点数]。"""
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
    # 计算三折晋级的手腕主轴换向率，单位为 Hz。
    wrist_reversal_rate = _wrist_reversal_rate_hz(reconstructed_window)
    # 计算三折晋级的手腕角速度自相关第二/第一时间峰比。
    wrist_acf_ratio = _wrist_acf_second_first_ratio(series["gyro_mag"])
    # 计算清洗后三折晋级的第一正自相关峰，描述重复动作周期一致性。
    wrist_acf_first_peak = _wrist_acf_first_peak(series["gyro_mag"])
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
    # 将 6 项事件对齐值追加到原 24 项末尾。
    features.extend(aligned_features)
    # 追加换向率、第二/第一峰比和第一峰强度，总计 33 项弱类特征和 297 维完整输入。
    features.extend([wrist_reversal_rate, wrist_acf_ratio, wrist_acf_first_peak])
    # 返回固定顺序列表；生成 C 必须在同一位置追加完全相同的 33 个弱类值。
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
    # 所有特征统一从清洗后的 [N,6] 手腕 IMU 窗口提取，确保统计量和冲击特征不受孤立毛刺支配。
    data = preprocess_imu_window(window)

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
    # weak_feature_set 保存 33 项经动作机理或文件级证据设计的弱类特征名称。
    weak_feature_set = set(WEAK_CLASS_FEATURE_NAMES)
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
        # 专家同时读取尺度不变通用形态和全部弱类机制特征，但排除原始幅值分位数。
        if "_normalized_phase" in name
        or name.endswith(invariant_suffixes)
        or name in weak_feature_set
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
        # motion_edge_trimmed_points 记录非静坐文件首尾被删除的采样点数，单位为点。
        "motion_edge_trimmed_points": 0,
    }

    for file_id, record in enumerate(records):
        if progress_label and (file_id == 0 or file_id % 10 == 0):
            print(
                f"features {progress_label} file={file_id + 1}/{len(records)} "
                f"kept={skipped['kept_windows']}",
                flush=True,
            )
        # data 是单位已转换的原始六轴记录，形状 [记录点数,6]，通道顺序 gx、gy、gz、ax、ay、az。
        data = load_imu_file(record.path)
        # original_length 用于审计离线首尾静止段裁剪量，不影响窗口标签或文件编号。
        original_length = len(data)
        # 非静坐训练记录只保留动作及 0.5 秒上下文；静坐记录由函数内部原样保留。
        data = trim_record_to_motion_segment(
            data,
            record.label,
            active_point_threshold,
        )
        # 累计被删除点数，便于训练报告量化清洗强度并发现异常过度裁剪。
        skipped["motion_edge_trimmed_points"] += original_length - len(data)
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
                # 回退选择仍使用清洗后的运动分数，避免孤立尖峰成为“最佳”动作窗口。
                scored = [
                    (motion_score(preprocess_imu_window(window)), window)
                    for window in fallback_windows
                ]
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


def apply_model_feature_mask(
    standardized_features: np.ndarray,
    suppress_normalized_phase: bool,
) -> np.ndarray:
    """按候选配置把冗余归一化阶段组替换为训练均值对应的零标准分。"""
    # values 必须是形状 [样本数,297] 的无量纲标准化特征。
    values = np.asarray(standardized_features, dtype=np.float32)
    # 二维和固定特征维度是多分支切片及 ESP32 模型合同的前提。
    if values.ndim != 2 or values.shape[1] != len(build_feature_names()):
        # 错误消息包含实际形状，便于发现旧 296/302 维工件或专家特征误用。
        raise ValueError(
            f"Expected standardized model features (n, {len(build_feature_names())}), "
            f"got {values.shape}"
        )
    # masked 是独立副本，避免训练、验证和后续消融共享数组时产生隐式修改。
    masked = values.copy()
    # 开关关闭时保持 Round29 基线输入完全不变。
    if not suppress_normalized_phase:
        # 返回副本，调用方可安全原地处理而不影响上游标准化数组。
        return masked
    # 48 个归一化阶段标准分设为 0，等价于把原始特征替换为训练集均值。
    masked[:, NORMALIZED_PHASE_MODEL_START:NORMALIZED_PHASE_MODEL_END] = 0.0
    # 返回形状不变的 [样本数,297] 输入，模型参数和分支边界无需改变。
    return masked


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
    deep_narrow: bool = False,
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
    # M1 当前没有训练期辅助头；T2 只允许比较融合深度这一项因素。
    if auxiliary_heads and deep_narrow:
        raise ValueError("Auxiliary heads are disabled for the deep-narrow M1 ablation")
    # 深窄融合建立在六分支输入上，禁止与平铺 BP 组合。
    if deep_narrow and not multi_branch:
        raise ValueError("Deep-narrow M1 requires the multi-branch model")
    class_count = len(class_names)
    # M1 优先于 M0 多分支；两者都关闭时使用兼容旧导出器的平铺 BP。
    if deep_narrow:
        # 构造审核通过的 88→64→48→32→24 深窄融合模型。
        model: nn.Module = DeepNarrowMultiBranchBPNet(
            train_x.shape[1], class_count, dropout=dropout
        ).to(device)
    elif multi_branch:
        # 构造 80→64→32 的 M0 浅融合模型。
        model = MultiBranchBPNet(
            train_x.shape[1], class_count, dropout=dropout
        ).to(device)
    else:
        # 构造 302→96→64→32 的平铺 BP。
        model = BPNet(train_x.shape[1], class_count, dropout=dropout).to(device)
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
            # 三种模型均通过统一接口将 32 或 24 维嵌入映射到主类别 logits。
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
        # val_class_recalls 按 class_names 顺序保存每类召回率，形状为 [类别数]。
        val_weak_recall, val_min_recall, val_class_recalls = weak_and_min_recall(
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
        # weakest_index 定位本 epoch 验证集中召回率最低的类别。
        weakest_index = int(np.argmin(val_class_recalls))
        # class_recall_text 使用固定类别顺序输出全部召回率，便于可见窗口逐轮追踪弱类。
        class_recall_text = ",".join(
            f"{name}:{float(recall):.4f}"
            for name, recall in zip(class_names, val_class_recalls)
        )
        print(
            f"{label}epoch={epoch:03d} loss={avg_loss:.4f} "
            f"ce={avg_ce:.4f} supcon={avg_supcon:.4f} margin={avg_margin:.4f} "
            f"aux={avg_auxiliary:.4f} "
            f"ema={ema_decay:.3f} "
            f"smooth={label_smoothing:.3f} "
            f"val_acc={val_acc:.4f} val_f1={val_f1:.4f} "
            f"val_weak_f1={val_weak_f1:.4f} val_worst_f1={val_worst_f1:.4f} "
            f"val_weak_recall={val_weak_recall:.4f} val_min_recall={val_min_recall:.4f} "
            f"weakest_class={class_names[weakest_index]}:{float(val_class_recalls[weakest_index]):.4f} "
            f"class_recalls={{{class_recall_text}}} "
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
        "deep_narrow": deep_narrow,
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
    pk_batches: bool = False,
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
        # 三类专家只验证层级重判效果，不叠加此前已证明不稳定的监督对比损失。
        supcon_weight=0.0,
        # 可选 P×K 批次使三类等量且同类样本优先跨文件，针对会话泛化失败。
        pk_batches=pk_batches,
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
    multi_branch: bool = False,
) -> Tuple[nn.Module, np.ndarray, np.ndarray]:
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
    # 按命令行声明恢复平铺 BP 或六分支 M0；结构必须与保存参数键完全匹配。
    model: nn.Module = (
        MultiBranchBPNet(input_dim, class_count).to(device)
        if multi_branch
        else BPNet(input_dim, class_count).to(device)
    )
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
    deep_narrow: bool = False,
    pk_batches: bool = False,
    auxiliary_heads: bool = False,
    pk_prior_corrected_ce: bool = False,
    supcon_weight: float = SUPCON_WEIGHT,
    dropout: float = DROPOUT,
    suppress_normalized_phase: bool = False,
) -> Dict[str, object]:
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
        # 主模型训练输入先应用候选掩码，保证被屏蔽列在全部优化步骤中恒为零。
        train_x = apply_model_feature_mask(train_x, suppress_normalized_phase)
        # 验证输入使用同一掩码，早停和模型选择不能依赖训练时不可见的列。
        val_x = apply_model_feature_mask(val_x, suppress_normalized_phase)
        # 完整模式测试输入和验证模式空数组均保持同一 [样本数,297] 合同。
        test_x = apply_model_feature_mask(test_x, suppress_normalized_phase)
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
            deep_narrow=deep_narrow,
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
            multi_branch=multi_branch,
        )
        train_x = ((train_x_raw - mean) / std).astype(np.float32)
        val_x = ((val_x_raw - mean) / std).astype(np.float32)
        test_x = ((test_x_raw - mean) / std).astype(np.float32)
        # 加载主模型时也按当前显式开关处理训练输入，供后续专家流程和一致性检查使用。
        train_x = apply_model_feature_mask(train_x, suppress_normalized_phase)
        # 固定主模型验证输入执行相同掩码。
        val_x = apply_model_feature_mask(val_x, suppress_normalized_phase)
        # 固定主模型测试输入执行相同掩码；验证模式下数组为空但维度合法。
        test_x = apply_model_feature_mask(test_x, suppress_normalized_phase)
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
            pk_batches=pk_batches,
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
        # 验证模式没有测试样本，避免对空张量调用 predict 的 concatenate 路径。
        specialist_test_pred = (
            np.empty(0, dtype=np.int64)
            if validation_only
            else predict(specialist_model, specialist_test_x, device)
        )
        val_pred = route_family_predictions(
            flat_val_pred,
            specialist_val_pred,
            class_names,
            specialist_names,
        )
        # 验证模式保持测试预测为空；完整模式才执行测试集专家路由。
        test_pred = (
            np.empty(0, dtype=np.int64)
            if validation_only
            else route_family_predictions(
                flat_test_pred,
                specialist_test_pred,
                class_names,
                specialist_names,
            )
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
        "deep_narrow": deep_narrow,
        "pk_batches": pk_batches,
        "auxiliary_heads": auxiliary_heads,
        "pk_prior_corrected_ce": pk_prior_corrected_ce,
        "supcon_weight": supcon_weight,
        "dropout": dropout,
        "suppress_normalized_phase": suppress_normalized_phase,
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
    # 外部推理严格复用候选保存的主模型输入掩码，默认 False 兼容旧工件。
    x = apply_model_feature_mask(
        x,
        bool(best_result.get("suppress_normalized_phase", False)),
    )
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
    # suppress_normalized_phase 决定主 BP 是否把 48 个冗余阶段特征固定为训练均值零分。
    suppress_normalized_phase = bool(result.get("suppress_normalized_phase", False))
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
        f"#define SUPPRESS_NORMALIZED_PHASE {1 if suppress_normalized_phase else 0}",
        f"#define NORMALIZED_PHASE_MODEL_START {NORMALIZED_PHASE_MODEL_START}",
        f"#define NORMALIZED_PHASE_MODEL_END {NORMALIZED_PHASE_MODEL_END}",
        f"#define HAS_FAMILY_SPECIALIST {1 if has_specialist else 0}",
        f"#define SPECIALIST_CLASS_NUM {len(specialist_names) if has_specialist else 0}",
        f"#define SPECIALIST_FEATURE_DIM {specialist_feature_dim}",
        f"#define HIDDEN1 {HIDDEN1}",
        f"#define HIDDEN2 {HIDDEN2}",
        f"#define HIDDEN3 {HIDDEN3}",
        f"#define PHASE_SEGMENTS {PHASE_SEGMENTS}",
        f"#define TEMPORAL_LOGIT_HISTORY {TEMPORAL_LOGIT_HISTORY}",
        f"#define ENSEMBLE_BASE_LOGIT_WEIGHT {c_float(ENSEMBLE_BASE_LOGIT_WEIGHT)}",
        f"#define ENSEMBLE_MASKED_LOGIT_WEIGHT {c_float(ENSEMBLE_MASKED_LOGIT_WEIGHT)}",
        "",
        f"static const float REST_MOTION_THRESHOLD = {c_float(rest_threshold)};",
        f"static const float ACTIVE_POINT_THRESHOLD = {c_float(active_point_threshold)};",
        "static const float HIGH_DYNAMIC_MIN_RATIO = 0.2f;",
        f"static const float PREPROCESS_GYRO_SPIKE_THRESHOLD_DPS = {c_float(PREPROCESS_GYRO_SPIKE_THRESHOLD_DPS)};",
        f"static const float PREPROCESS_ACC_SPIKE_THRESHOLD_G = {c_float(PREPROCESS_ACC_SPIKE_THRESHOLD_G)};",
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
/*
 * 因果类别分数平滑状态。
 * history 形状为 [15,CLASS_NUM]，只保存当前及过去窗口 logits，不读取未来数据。
 * running_sum 形状为 [CLASS_NUM]，使每次更新复杂度为 O(CLASS_NUM)。
 * ESP32 RAM 占用约为 (15*CLASS_NUM+CLASS_NUM)*4+8 字节；11 类时约 712 字节。
 */
typedef struct {
    /* history 是环形 logit 缓冲区，元素为 softmax 前无量纲类别分数。 */
    float history[TEMPORAL_LOGIT_HISTORY][CLASS_NUM];
    /* running_sum 保存全部有效槽的逐类和。 */
    float running_sum[CLASS_NUM];
    /* count 是有效历史窗口数，范围 0..TEMPORAL_LOGIT_HISTORY。 */
    int count;
    /* next_index 指向下一次写入或覆盖的槽。 */
    int next_index;
} BpTemporalSmoother;

/* 清空设备重连、用户切换或动作会话结束前的全部历史。 */
static inline void bp_temporal_smoother_reset(BpTemporalSmoother* state) {
    /* 空指针表示调用方没有提供状态，直接返回避免崩溃。 */
    if (state == 0) return;
    /* 逐槽逐类清零，防止重置后读到旧会话数据。 */
    for (int slot = 0; slot < TEMPORAL_LOGIT_HISTORY; slot++) {
        /* 遍历固定 CLASS_NUM 个类别。 */
        for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
            /* 当前历史元素恢复为零。 */
            state->history[slot][class_index] = 0.0f;
        }
    }
    /* 清零逐类累计和。 */
    for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
        /* 每个类别累计和恢复为零。 */
        state->running_sum[class_index] = 0.0f;
    }
    /* 有效槽数归零。 */
    state->count = 0;
    /* 下一写槽回到索引零。 */
    state->next_index = 0;
}

/*
 * 加入当前窗口 logits 并输出当前及过去最多 14 个窗口的因果均值和类别。
 * raw_logits、smoothed_logits 均为 [CLASS_NUM]；两者允许指向不同数组，不允许为空。
 * 返回值是平滑后最大 logit 的全局类别索引，范围 0..CLASS_NUM-1；非法状态返回 -1。
 */
static inline int bp_temporal_smoother_update(
    BpTemporalSmoother* state,
    const float raw_logits[CLASS_NUM],
    float smoothed_logits[CLASS_NUM]
) {
    /* 任一必要指针为空时返回 -1，避免访问非法内存。 */
    if (state == 0 || raw_logits == 0 || smoothed_logits == 0) return -1;
    /* 缓冲区已满时，先移除 next_index 指向的最旧槽。 */
    if (state->count == TEMPORAL_LOGIT_HISTORY) {
        /* 逐类从累计和中减去被覆盖值。 */
        for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
            /* running_sum 保持恰好包含最近 14 个旧窗口。 */
            state->running_sum[class_index] -= state->history[state->next_index][class_index];
        }
    } else {
        /* 未满时有效槽数增加一。 */
        state->count++;
    }
    /* 写入当前窗口并更新逐类累计和。 */
    for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
        /* 当前无量纲 logit 写入下一环形槽。 */
        state->history[state->next_index][class_index] = raw_logits[class_index];
        /* 当前分数加入累计和。 */
        state->running_sum[class_index] += raw_logits[class_index];
        /* 除以有效窗口数得到因果均值。 */
        smoothed_logits[class_index] = state->running_sum[class_index] / (float)state->count;
    }
    /* 写指针循环前进。 */
    state->next_index = (state->next_index + 1) % TEMPORAL_LOGIT_HISTORY;
    /* best_index 初始为第零类。 */
    int best_index = 0;
    /* 比较其余类别；严格大于保证平局时选择更早类别，与 argmax 一致。 */
    for (int class_index = 1; class_index < CLASS_NUM; class_index++) {
        /* 当前均值更大时更新类别。 */
        if (smoothed_logits[class_index] > smoothed_logits[best_index]) best_index = class_index;
    }
    /* 返回平滑后的全局动作类别索引。 */
    return best_index;
}

/*
 * 固定双 M0 模型融合：combined = 0.85*base + 0.15*masked。
 * 三个数组形状均为 [CLASS_NUM]，元素为 softmax 前无量纲 logits；允许输出与任一输入共用缓冲区。
 * 权重只由验证集选择，部署端不得再次使用测试集调权；时间复杂度 O(CLASS_NUM)，无额外状态 RAM。
 */
static inline int bp_combine_ensemble_logits(
    const float base_logits[CLASS_NUM],
    const float masked_logits[CLASS_NUM],
    float combined_logits[CLASS_NUM]
) {
    /* 任一必要指针为空时返回 -1，避免访问非法内存。 */
    if (base_logits == 0 || masked_logits == 0 || combined_logits == 0) return -1;
    /* 逐类执行固定凸组合，类别顺序必须与 Python 导出的 CLASS_NAMES 完全一致。 */
    for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
        /* 先读取两个输入，保证 combined_logits 与输入数组共用地址时仍能得到当前类别原值。 */
        const float base_value = base_logits[class_index];
        /* masked_value 来自抑制标准化阶段 184:232 特征的第二个 M0。 */
        const float masked_value = masked_logits[class_index];
        /* 固定 0.85/0.15 融合保持 logit 总尺度，输出仍为无量纲分数。 */
        combined_logits[class_index] =
            ENSEMBLE_BASE_LOGIT_WEIGHT * base_value
            + ENSEMBLE_MASKED_LOGIT_WEIGHT * masked_value;
    }
    /* 返回零表示全部类别已完成融合。 */
    return 0;
}

/*
 * 单个动作活动段的因果累计证据状态。
 * running_sum 形状为 [CLASS_NUM]，count 是从活动段开始到当前的窗口数。
 * 11 类时 RAM 为 11*4+4=48 字节；每窗口时间复杂度 O(CLASS_NUM)。
 * 静止、动作切换、设备断连或用户切换时必须调用 bp_bout_accumulator_reset。
 */
typedef struct {
    /* running_sum 保存活动段内当前及全部历史窗口的逐类融合 logit 和。 */
    float running_sum[CLASS_NUM];
    /* count 使用 32 位无符号整数，正常健身动作段远小于其上限。 */
    uint32_t count;
} BpBoutAccumulator;

/* 清空上一动作段证据，使下一窗口从独立活动段开始判断。 */
static inline void bp_bout_accumulator_reset(BpBoutAccumulator* state) {
    /* 空指针表示调用方没有提供状态，直接返回避免崩溃。 */
    if (state == 0) return;
    /* 逐类清零累计和，防止上一动作标签影响下一动作。 */
    for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
        /* 当前类别的历史融合 logit 和恢复为零。 */
        state->running_sum[class_index] = 0.0f;
    }
    /* 窗口计数归零，下一次更新的均值等于当前窗口 logits。 */
    state->count = 0U;
}

/*
 * 加入当前融合 logits，输出从动作段开始到当前窗口的因果均值和类别。
 * combined_logits、averaged_logits 形状均为 [CLASS_NUM]；两者允许共用缓冲区。
 * 返回最大平均 logit 的类别索引 0..CLASS_NUM-1；空指针或非有限输入返回 -1 且不更新状态。
 */
static inline int bp_bout_accumulator_update(
    BpBoutAccumulator* state,
    const float combined_logits[CLASS_NUM],
    float averaged_logits[CLASS_NUM]
) {
    /* 任一必要指针为空时返回 -1，状态保持不变。 */
    if (state == 0 || combined_logits == 0 || averaged_logits == 0) return -1;
    /* 在修改状态前检查全部类别，避免 NaN 或无穷值永久污染当前动作段。 */
    for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
        /* isfinite 同时拒绝 NaN、正无穷和负无穷。 */
        if (!isfinite(combined_logits[class_index])) return -1;
    }
    /* 极端超长会话达到 uint32 上限时把和与计数同时减半，防止计数回绕。 */
    if (state->count == UINT32_MAX) {
        /* 所有类别累计和使用同一比例缩放，不改变类别排序。 */
        for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
            /* 乘 0.5 降低长期累计量级和浮点溢出风险。 */
            state->running_sum[class_index] *= 0.5f;
        }
        /* 向下取整后的正计数仍保留长期历史，且为新窗口腾出一个计数。 */
        state->count /= 2U;
    }
    /* 先增加窗口数，使首个窗口的分母为一。 */
    state->count += 1U;
    /* 逐类累加当前证据并计算从动作段起点到当前的均值。 */
    for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
        /* 当前无量纲融合 logit 加入该类别历史和。 */
        state->running_sum[class_index] += combined_logits[class_index];
        /* 除以活动段窗口数，输出无量纲平均 logit。 */
        averaged_logits[class_index] = state->running_sum[class_index] / (float)state->count;
    }
    /* best_index 从第零类开始，与 NumPy argmax 的平局规则一致。 */
    int best_index = 0;
    /* 依次比较其余类别，严格大于时才替换最优类别。 */
    for (int class_index = 1; class_index < CLASS_NUM; class_index++) {
        /* 当前平均 logit 更大时记录其全局类别索引。 */
        if (averaged_logits[class_index] > averaged_logits[best_index]) best_index = class_index;
    }
    /* 返回当前动作段累计证据对应的全局动作类别。 */
    return best_index;
}

/*
 * 修复手腕六轴 IMU 窗口中的单轴孤立尖峰。
 * 输入 raw_window 和输出 cleaned_window 形状均为 [WINDOW_LEN,6]，通道顺序固定为 gx、gy、gz、ax、ay、az。
 * 前三轴单位为 deg/s，阈值为 PREPROCESS_GYRO_SPIKE_THRESHOLD_DPS；后三轴单位为 g，阈值为 PREPROCESS_ACC_SPIKE_THRESHOLD_G。
 * 仅当中心点相对前后邻点均值超阈值、前后邻点差小于半阈值且恰好一个轴异常时进行线性插值。
 * 多轴共同冲击、快速动作边沿和首末采样点全部保留；时间复杂度 O(6N)，额外 RAM 为 6N 个 float。
 */
static inline void preprocess_imu_window(
    const float raw_window[WINDOW_LEN][AXIS_NUM],
    float cleaned_window[WINDOW_LEN][AXIS_NUM]
) {
    /* 逐点逐轴复制原始窗口，确保调用方缓冲区不被修改且输出总是完整初始化。 */
    for (int sample = 0; sample < WINDOW_LEN; sample++) {
        /* 六个通道依次复制 gx、gy、gz、ax、ay、az。 */
        for (int axis = 0; axis < AXIS_NUM; axis++) {
            /* 当前值的物理单位由 axis 决定：0..2 为 deg/s，3..5 为 g。 */
            cleaned_window[sample][axis] = raw_window[sample][axis];
        }
    }
    /* 首末点缺少双侧邻点，因此只遍历索引 1 到 WINDOW_LEN-2。 */
    for (int sample = 1; sample < WINDOW_LEN - 1; sample++) {
        /* candidate_count 统计当前时刻满足孤立尖峰条件的轴数；只有等于 1 才允许修复。 */
        int candidate_count = 0;
        /* candidate_axis 保存唯一候选轴，-1 表示尚未发现候选。 */
        int candidate_axis = -1;
        /* candidate_mean 保存唯一候选轴的前后邻点均值，单位随候选轴变化。 */
        float candidate_mean = 0.0f;
        /* 遍历六轴并分别使用角速度或加速度阈值。 */
        for (int axis = 0; axis < AXIS_NUM; axis++) {
            /* threshold 前三轴取 300 deg/s，后三轴取 1.5 g。 */
            float threshold = axis < 3
                ? PREPROCESS_GYRO_SPIKE_THRESHOLD_DPS
                : PREPROCESS_ACC_SPIKE_THRESHOLD_G;
            /* neighbor_mean 是原始前后邻点均值；不读取已修复输出，避免级联插值。 */
            float neighbor_mean =
                (raw_window[sample - 1][axis] + raw_window[sample + 1][axis]) * 0.5f;
            /* center_residual 衡量中心点相对邻点趋势的绝对偏差。 */
            float center_residual = fabsf(raw_window[sample][axis] - neighbor_mean);
            /* neighbor_gap 衡量前后邻点是否处于同一稳定趋势。 */
            float neighbor_gap = fabsf(
                raw_window[sample - 1][axis] - raw_window[sample + 1][axis]
            );
            /* 同时满足大中心偏差和小邻点差时，该轴才是孤立毛刺候选。 */
            if (center_residual > threshold && neighbor_gap < threshold * 0.5f) {
                /* 累加候选轴数，用于拒绝真实多轴冲击。 */
                candidate_count++;
                /* 保存当前候选轴；若最终候选数大于 1，该值不会被使用。 */
                candidate_axis = axis;
                /* 保存线性插值结果，避免修复阶段重复计算。 */
                candidate_mean = neighbor_mean;
            }
        }
        /* 恰好一个轴异常时才替换，保留多轴落地冲击和真实快速转动。 */
        if (candidate_count == 1) {
            /* 写入唯一候选通道，其余五轴保持原值。 */
            cleaned_window[sample][candidate_axis] = candidate_mean;
        }
    }
}

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

/*
 * 计算手腕角速度 PCA 主轴上的有效换向率，单位为 Hz。
 * 输入 window 为 [WINDOW_LEN,6]，前三列单位为 deg/s；只使用手腕陀螺数据。
 * 主轴用固定 8 次幂迭代求解，90 分位门槛与 Python 一致；时间复杂度 O(N^2)，额外 RAM 约 5N 个 float。
 */
static inline float wrist_reversal_rate_hz(const float window[WINDOW_LEN][AXIS_NUM]) {
    /* mean 保存 gx、gy、gz 的窗口均值，用于去除陀螺零偏。 */
    float mean[3] = { 0.0f, 0.0f, 0.0f };
    /* 遍历全部手腕采样点并累计三轴角速度。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* 逐轴累加，通道 0..2 固定对应 gx、gy、gz。 */
        for (int axis = 0; axis < 3; axis++) mean[axis] += window[i][axis];
    }
    /* 除以窗口长度得到三轴均值，单位仍为 deg/s。 */
    for (int axis = 0; axis < 3; axis++) mean[axis] /= (float)WINDOW_LEN;
    /* moment 是去均值角速度的 3×3 二阶矩，单位为 (deg/s)^2。 */
    float moment[3][3] = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
    /* 遍历窗口累计外积，未除以 N 不影响主特征向量。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* centered 保存当前点三轴动态角速度。 */
        float centered[3];
        /* 从原始值减去各轴均值。 */
        for (int axis = 0; axis < 3; axis++) centered[axis] = window[i][axis] - mean[axis];
        /* 累计 3×3 对称二阶矩。 */
        for (int row = 0; row < 3; row++) {
            /* 每个 row 与三个 column 形成外积元素。 */
            for (int column = 0; column < 3; column++) moment[row][column] += centered[row] * centered[column];
        }
    }
    /* initial_axis 选择对角能量最大的传感器轴，避免初值与主轴正交。 */
    int initial_axis = 0;
    /* 比较 y、z 轴对角能量并更新索引；严格大于保证平局取更早轴。 */
    for (int axis_index = 1; axis_index < 3; axis_index++) if (moment[axis_index][axis_index] > moment[initial_axis][initial_axis]) initial_axis = axis_index;
    /* axis 是幂迭代单位向量，初始为最大能量坐标轴。 */
    float axis[3] = { 0.0f, 0.0f, 0.0f };
    /* 写入唯一非零初始分量。 */
    axis[initial_axis] = 1.0f;
    /* 固定执行 8 次，与 Python 保持一致且避免收敛条件分支差异。 */
    for (int iteration = 0; iteration < 8; iteration++) {
        /* next_axis 接收 moment×axis。 */
        float next_axis[3] = { 0.0f, 0.0f, 0.0f };
        /* 三行矩阵分别与当前轴做点积。 */
        for (int row = 0; row < 3; row++) {
            /* 遍历三列完成当前输出分量。 */
            for (int column = 0; column < 3; column++) next_axis[row] += moment[row][column] * axis[column];
        }
        /* norm 是下一轴的二范数。 */
        float norm = sqrtf(next_axis[0] * next_axis[0] + next_axis[1] * next_axis[1] + next_axis[2] * next_axis[2]);
        /* 近静止窗口没有可靠主轴，返回 0 次/秒。 */
        if (norm <= 1e-12f) return 0.0f;
        /* 归一化三轴分量进入下一次迭代。 */
        for (int component = 0; component < 3; component++) axis[component] = next_axis[component] / norm;
    }
    /* anchor 选择绝对值最大的主轴分量，用于固定 PCA 任意符号。 */
    int anchor = 0;
    /* 比较剩余两个分量绝对值，严格大于保证平局确定。 */
    for (int component = 1; component < 3; component++) if (fabsf(axis[component]) > fabsf(axis[anchor])) anchor = component;
    /* 锚点为负时翻转整条轴，使 Python/C 主轴符号一致。 */
    if (axis[anchor] < 0.0f) for (int component = 0; component < 3; component++) axis[component] = -axis[component];
    /* projection 保存每个采样点沿手腕主转动方向的角速度，单位 deg/s。 */
    float projection[WINDOW_LEN];
    /* 逐点计算去均值三轴与主轴的点积。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* 点积三项显式展开，减少 ESP32 循环开销。 */
        projection[i] =
            (window[i][0] - mean[0]) * axis[0] +
            (window[i][1] - mean[1]) * axis[1] +
            (window[i][2] - mean[2]) * axis[2];
    }
    /* smoothed 保存三点对称平均，边缘使用自身复制。 */
    float smoothed[WINDOW_LEN];
    /* 每个位置读取左、中、右三个投影。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* 左边缘索引在 i=0 时固定为 0。 */
        int left = i > 0 ? i - 1 : 0;
        /* 右边缘索引在末点时固定为 WINDOW_LEN-1。 */
        int right = i + 1 < WINDOW_LEN ? i + 1 : WINDOW_LEN - 1;
        /* 三点和除以 3 得到平滑角速度。 */
        smoothed[i] = (projection[left] + projection[i] + projection[right]) / 3.0f;
    }
    /* ordered_abs 用于计算与 NumPy 线性插值一致的 90 分位幅值。 */
    float ordered_abs[WINDOW_LEN];
    /* 复制全部平滑投影绝对值。 */
    for (int i = 0; i < WINDOW_LEN; i++) ordered_abs[i] = fabsf(smoothed[i]);
    /* 插入排序适合 N=62 的固定小数组，额外 RAM 为 O(N)。 */
    for (int i = 1; i < WINDOW_LEN; i++) {
        /* value 保存当前待插入绝对值。 */
        float value = ordered_abs[i];
        /* j 从已排序区末尾向前移动。 */
        int j = i - 1;
        /* 将所有大于 value 的元素右移一位。 */
        while (j >= 0 && ordered_abs[j] > value) { ordered_abs[j + 1] = ordered_abs[j]; j--; }
        /* 把 value 写入空出的有序位置。 */
        ordered_abs[j + 1] = value;
    }
    /* percentile_position 对应 NumPy percentile 的 0.9*(N-1) 线性位置。 */
    float percentile_position = 0.90f * (float)(WINDOW_LEN - 1);
    /* lower_index 是线性插值左端索引。 */
    int lower_index = (int)floorf(percentile_position);
    /* upper_index 是右端索引，限制不超过数组末尾。 */
    int upper_index = lower_index + 1 < WINDOW_LEN ? lower_index + 1 : lower_index;
    /* fraction 是左右两个顺序统计量之间的插值比例。 */
    float fraction = percentile_position - (float)lower_index;
    /* percentile90 得到 90 分位绝对角速度，单位 deg/s。 */
    float percentile90 = ordered_abs[lower_index] * (1.0f - fraction) + ordered_abs[upper_index] * fraction;
    /* 有效换向门槛取 10 deg/s 与 15% q90 中较大者。 */
    float threshold = 0.15f * percentile90;
    /* 物理下限过滤静止手腕零点抖动。 */
    if (threshold < 10.0f) threshold = 10.0f;
    /* reversal_count 记录相邻有效点符号翻转次数。 */
    int reversal_count = 0;
    /* has_previous 表示已遇到第一个达到门槛的点。 */
    int has_previous = 0;
    /* previous 保存上一个有效投影。 */
    float previous = 0.0f;
    /* 顺序遍历平滑投影，低于门槛的点不参与符号比较。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* 当前绝对值不足门槛时继续下一点。 */
        if (fabsf(smoothed[i]) < threshold) continue;
        /* 已有前一有效点且乘积为负时累计一次换向。 */
        if (has_previous && previous * smoothed[i] < 0.0f) reversal_count++;
        /* 更新上一有效点值。 */
        previous = smoothed[i];
        /* 标记有效历史存在。 */
        has_previous = 1;
    }
    /* 次数除以 N/采样率得到每秒换向次数。 */
    return (float)reversal_count / ((float)WINDOW_LEN / (float)SAMPLE_RATE_HZ);
}

/*
 * 计算手腕角速度模长自相关第二时间峰与第一时间峰之比。
 * 搜索延迟为 0.30～3.00 秒且不超过半窗；输出无量纲并限制在 [0,5]。
 * 时间复杂度 O(N^2)，额外 RAM 不超过 WINDOW_LEN 个 float。
 */
/*
 * 返回角速度模长在 0.3～3.0 秒延迟范围内的第一正自相关峰。
 * 输入 gyro_magnitude 长度为 n，单位 deg/s；输出无量纲，典型范围 [0,1]。
 * 时间复杂度 O(N^2)，额外 RAM 不超过 WINDOW_LEN 个 float。
 */
static inline float wrist_acf_first_peak(const float* gyro_magnitude, int n) {
    /* 少于 10 点时无法覆盖 0.3 秒最小延迟。 */
    if (n < 10) return 0.0f;
    /* mean 是角速度模长窗口均值，单位 deg/s。 */
    float mean = 0.0f;
    /* 累加全部 n 个点用于计算均值。 */
    for (int i = 0; i < n; i++) mean += gyro_magnitude[i];
    /* 除以点数得到窗口均值。 */
    mean /= (float)n;
    /* energy 是中心化序列的零延迟能量，单位 (deg/s)^2。 */
    float energy = 0.0f;
    /* 逐点累计中心化平方和。 */
    for (int i = 0; i < n; i++) {
        /* centered 去除角速度模长直流偏置。 */
        float centered = gyro_magnitude[i] - mean;
        /* 累加当前点能量。 */
        energy += centered * centered;
    }
    /* 近静止窗口不能稳定归一化，返回零周期证据。 */
    if (energy <= 1e-12f) return 0.0f;
    /* minimum_lag 在 25 Hz 下为 round(7.5)=8 点。 */
    int minimum_lag = (int)(0.30f * (float)SAMPLE_RATE_HZ + 0.5f);
    /* 至少保留两个点延迟，保证后续局部峰左右邻点存在。 */
    if (minimum_lag < 2) minimum_lag = 2;
    /* maximum_lag 初始取半窗，保证每个延迟有足够重叠样本。 */
    int maximum_lag = n / 2;
    /* 三秒上限换算为采样点。 */
    int three_seconds = 3 * SAMPLE_RATE_HZ;
    /* 半窗超过三秒时截断到部署合同上限。 */
    if (maximum_lag > three_seconds) maximum_lag = three_seconds;
    /* 空延迟范围返回零。 */
    if (maximum_lag <= minimum_lag) return 0.0f;
    /* correlation 按延迟升序保存归一化自相关。 */
    float correlation[WINDOW_LEN];
    /* correlation_count 是实际延迟点数。 */
    int correlation_count = 0;
    /* 从最小延迟遍历到最大延迟并计算重叠点积。 */
    for (int lag = minimum_lag; lag <= maximum_lag; lag++) {
        /* dot 累加当前延迟的中心化交叉乘积。 */
        float dot = 0.0f;
        /* 重叠长度为 n-lag，索引不会越过输入末尾。 */
        for (int i = 0; i < n - lag; i++) {
            /* 两个延迟对应点均减去同一窗口均值。 */
            dot += (gyro_magnitude[i] - mean) * (gyro_magnitude[i + lag] - mean);
        }
        /* 统一除以零延迟能量，保持与 Python 候选公式一致。 */
        correlation[correlation_count++] = dot / energy;
    }
    /* 少于三个相关点时无法定义内部峰，返回区间最大正值。 */
    if (correlation_count < 3) {
        /* best 从第一个相关值开始，避免读取未初始化内存。 */
        float best = correlation[0];
        /* 扫描剩余相关值寻找最大值。 */
        for (int i = 1; i < correlation_count; i++) if (correlation[i] > best) best = correlation[i];
        /* 负最大值表示没有正周期证据，夹紧为零。 */
        return best > 0.0f ? best : 0.0f;
    }
    /* 按时间顺序寻找第一个正内部局部峰。 */
    for (int i = 1; i < correlation_count - 1; i++) {
        /* 当前点严格高于左点、不低于右点且为正时立即返回。 */
        if (correlation[i] > correlation[i - 1] && correlation[i] >= correlation[i + 1] && correlation[i] > 0.0f) return correlation[i];
    }
    /* 没有内部峰时退化为搜索范围内最大正相关，覆盖边界周期。 */
    float best = correlation[0];
    /* 遍历全部相关值寻找最大项。 */
    for (int i = 1; i < correlation_count; i++) if (correlation[i] > best) best = correlation[i];
    /* 返回非负第一峰强度。 */
    return best > 0.0f ? best : 0.0f;
}

static inline float wrist_acf_second_first_ratio(const float* gyro_magnitude, int n) {
    /* 少于 10 点时没有合法 0.3 秒延迟范围。 */
    if (n < 10) return 0.0f;
    /* mean 是角速度模长窗口均值，单位 deg/s。 */
    float mean = 0.0f;
    /* 遍历输入累计均值分子。 */
    for (int i = 0; i < n; i++) mean += gyro_magnitude[i];
    /* 除以点数得到均值。 */
    mean /= (float)n;
    /* energy 是去均值零延迟能量。 */
    float energy = 0.0f;
    /* 累计中心化平方和。 */
    for (int i = 0; i < n; i++) { float centered = gyro_magnitude[i] - mean; energy += centered * centered; }
    /* 近常量窗口没有周期证据。 */
    if (energy <= 1e-12f) return 0.0f;
    /* 最小延迟 round(0.30*25)=8 点。 */
    int minimum_lag = (int)(0.30f * (float)SAMPLE_RATE_HZ + 0.5f);
    /* 数值下限为 2，确保局部峰左右点存在。 */
    if (minimum_lag < 2) minimum_lag = 2;
    /* 最大延迟先取半窗。 */
    int maximum_lag = n / 2;
    /* 三秒对应 3*SAMPLE_RATE_HZ 点。 */
    int three_seconds = 3 * SAMPLE_RATE_HZ;
    /* 超过三秒时截断。 */
    if (maximum_lag > three_seconds) maximum_lag = three_seconds;
    /* 无合法范围时返回零。 */
    if (maximum_lag <= minimum_lag) return 0.0f;
    /* correlation 保存 minimum_lag..maximum_lag 的归一化自相关。 */
    float correlation[WINDOW_LEN];
    /* correlation_count 是有效延迟数量。 */
    int correlation_count = 0;
    /* 按延迟时间升序计算中心化点积。 */
    for (int lag = minimum_lag; lag <= maximum_lag; lag++) {
        /* dot 累加当前延迟的重叠点积。 */
        float dot = 0.0f;
        /* 重叠区长度为 n-lag。 */
        for (int i = 0; i < n - lag; i++) dot += (gyro_magnitude[i] - mean) * (gyro_magnitude[i + lag] - mean);
        /* 与 Python 一致统一除以零延迟能量。 */
        correlation[correlation_count++] = dot / energy;
    }
    /* 少于三个相关点不能定义两个内部峰。 */
    if (correlation_count < 3) return 0.0f;
    /* first_peak 和 second_peak 按时间先后保存前两个正局部峰。 */
    float first_peak = 0.0f, second_peak = 0.0f;
    /* peak_count 记录已找到的时间峰数量。 */
    int peak_count = 0;
    /* 遍历内部相关点，保证左右邻点存在。 */
    for (int i = 1; i < correlation_count - 1; i++) {
        /* 当前值须严格高于左点、不低于右点且为正。 */
        if (correlation[i] > correlation[i - 1] && correlation[i] >= correlation[i + 1] && correlation[i] > 0.0f) {
            /* 第一个时间峰写入 first_peak。 */
            if (peak_count == 0) first_peak = correlation[i];
            /* 第二个时间峰写入后即可停止。 */
            else { second_peak = correlation[i]; break; }
            /* 峰计数增加一。 */
            peak_count++;
        }
    }
    /* 未找到两个峰或第一峰过小时返回零。 */
    if (peak_count < 1 || second_peak <= 0.0f || first_peak <= 1e-12f) return 0.0f;
    /* 计算第二/第一时间峰比。 */
    float ratio = second_peak / first_peak;
    /* 理论下限为零。 */
    if (ratio < 0.0f) ratio = 0.0f;
    /* 限制异常上限为 5，与 Python np.clip 一致。 */
    if (ratio > 5.0f) ratio = 5.0f;
    /* 返回无量纲比值。 */
    return ratio;
}

/*
 * 对最多 WINDOW_LEN 个手腕标量执行插入排序并返回中位数。
 * 输入数组只读；额外 RAM 为 WINDOW_LEN 个 float，时间复杂度 O(N^2)。
 */
static inline float wrist_window_median(const float* values, int count) {
    /* 空输入没有稳健中心，返回确定性零。 */
    if (count <= 0) return 0.0f;
    /* sorted 保存不超过一个推理窗口的副本，避免修改调用者数据。 */
    float sorted[WINDOW_LEN];
    /* 复制 count 个有效值。 */
    for (int i = 0; i < count; i++) sorted[i] = values[i];
    /* 对固定小数组执行稳定插入排序。 */
    for (int i = 1; i < count; i++) {
        /* key 是本轮待插入值。 */
        float key = sorted[i];
        /* j 从有序前缀末尾向前移动。 */
        int j = i - 1;
        /* 大于 key 的元素逐项右移。 */
        while (j >= 0 && sorted[j] > key) {
            /* 为 key 腾出插入位置。 */
            sorted[j + 1] = sorted[j];
            /* 继续检查前一个值。 */
            j--;
        }
        /* 写入 key 的最终有序位置。 */
        sorted[j + 1] = key;
    }
    /* 奇数点返回中央顺序统计量。 */
    if ((count & 1) != 0) return sorted[count / 2];
    /* 偶数点返回两个中央值均值，与 numpy.median 一致。 */
    return 0.5f * (sorted[count / 2 - 1] + sorted[count / 2]);
}

/*
 * 追加六项经文件分组三折验证的手腕形态、冲击恢复和周期特征。
 * window 通道顺序固定为 gx、gy、gz、ax、ay、az；角速度单位 deg/s，加速度单位 g。
 * 输出顺序必须与 WEAK_CLASS_FEATURE_NAMES 末六项一致；时间复杂度由直接 DFT 主导为 O(N^2)。
 */
static inline void append_additional_wrist_features(
    const float window[WINDOW_LEN][AXIS_NUM],
    const float* gyro_magnitude,
    float* feature,
    int* index
) {
    /* half_length 是前后摆动形状比较的共同长度。 */
    int half_length = WINDOW_LEN / 2;
    /* reversed_second 保存时间反转后的后半段角速度模长。 */
    float reversed_second[WINDOW_LEN / 2];
    /* 复制后半段并反转时间方向，使其与前半段外摆轨迹对齐。 */
    for (int i = 0; i < half_length; i++) reversed_second[i] = gyro_magnitude[WINDOW_LEN - 1 - i];
    /* 皮尔逊相关比较前半段和反转后半段的归一化波形。 */
    feature[(*index)++] = series_correlation(gyro_magnitude, reversed_second, half_length);

    /* acc_mean 保存 ax、ay、az 的全窗均值，单位 g。 */
    float acc_mean[3] = {0.0f, 0.0f, 0.0f};
    /* 累加三轴加速度以估计窗口内静态分量。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* ax 对应 window 第 3 列。 */
        acc_mean[0] += window[i][3];
        /* ay 对应 window 第 4 列。 */
        acc_mean[1] += window[i][4];
        /* az 对应 window 第 5 列。 */
        acc_mean[2] += window[i][5];
    }
    /* 三轴累加和除以窗口长度得到均值。 */
    for (int axis = 0; axis < 3; axis++) acc_mean[axis] /= (float)WINDOW_LEN;
    /* acc_magnitude 保存原始 specific-force 模长，单位 g。 */
    float acc_magnitude[WINDOW_LEN];
    /* dynamic_magnitude 保存去三轴均值后的动态加速度模长，单位 g。 */
    float dynamic_magnitude[WINDOW_LEN];
    /* 遍历窗口并计算两个加速度模长。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* ax、ay、az 是当前手腕加速度三轴值。 */
        float ax = window[i][3], ay = window[i][4], az = window[i][5];
        /* 原始模长供 jerk 计算。 */
        acc_magnitude[i] = sqrtf(ax * ax + ay * ay + az * az);
        /* dx、dy、dz 是相对窗口均值的动态分量。 */
        float dx = ax - acc_mean[0], dy = ay - acc_mean[1], dz = az - acc_mean[2];
        /* 动态模长供能量和恢复时间计算。 */
        dynamic_magnitude[i] = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    /* jerk 首点无前驱，固定为零；其余点单位 g/s。 */
    float jerk[WINDOW_LEN];
    /* smoothed_jerk 是边缘复制的三点均值结果。 */
    float smoothed_jerk[WINDOW_LEN];
    /* 第一 jerk 采样没有前一时刻。 */
    jerk[0] = 0.0f;
    /* 相邻模长绝对差乘采样率得到离散 jerk。 */
    for (int i = 1; i < WINDOW_LEN; i++) jerk[i] = fabsf(acc_magnitude[i] - acc_magnitude[i - 1]) * (float)SAMPLE_RATE_HZ;
    /* 三点平滑遍历全部时间点，首尾使用自身复制值。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* left 在首点复制 jerk[0]。 */
        float left = jerk[i > 0 ? i - 1 : 0];
        /* right 在末点复制 jerk[WINDOW_LEN-1]。 */
        float right = jerk[i + 1 < WINDOW_LEN ? i + 1 : WINDOW_LEN - 1];
        /* 固定核 [1,1,1]/3 与 Python np.convolve 一致。 */
        smoothed_jerk[i] = (left + jerk[i] + right) / 3.0f;
    }
    /* event_index 保存最早的最大平滑 jerk 位置。 */
    int event_index = 0;
    /* strictly greater 保证平局不覆盖早期事件。 */
    for (int i = 1; i < WINDOW_LEN; i++) if (smoothed_jerk[i] > smoothed_jerk[event_index]) event_index = i;
    /* context_points 对应 round(0.4*25)=10 点。 */
    int context_points = (int)(0.40f * (float)SAMPLE_RATE_HZ + 0.5f);
    /* 至少保留两个上下文点。 */
    if (context_points < 2) context_points = 2;
    /* pre_start 限制在窗口左边界。 */
    int pre_start = event_index - context_points;
    /* 负起点截断为零。 */
    if (pre_start < 0) pre_start = 0;
    /* post_end 为包含事件点的右侧半开终点。 */
    int post_end = event_index + context_points + 1;
    /* 超过窗口时截断。 */
    if (post_end > WINDOW_LEN) post_end = WINDOW_LEN;
    /* post_peak 保存事件后 0.4 秒内最大平滑 jerk。 */
    float post_peak = smoothed_jerk[event_index];
    /* 扫描局部后区间更新峰值。 */
    for (int i = event_index + 1; i < post_end; i++) if (smoothed_jerk[i] > post_peak) post_peak = smoothed_jerk[i];
    /* half_height 是冲击局部峰值一半。 */
    float half_height = 0.5f * post_peak;
    /* half_width_points 统计事件起始后的连续半高点。 */
    int half_width_points = 0;
    /* 从事件点顺序扫描，首次低于半高即终止。 */
    for (int i = event_index; i < post_end; i++) {
        /* 正峰值且当前点达到半高时累计。 */
        if (post_peak > 1e-12f && smoothed_jerk[i] >= half_height) half_width_points++;
        /* 零峰或跌破半高均结束连续宽度。 */
        else break;
    }
    /* 点数除以采样率得到秒。 */
    feature[(*index)++] = (float)half_width_points / (float)SAMPLE_RATE_HZ;
    /* pre_energy 和 post_energy 是动态加速度模长平方和，单位 g^2。 */
    float pre_energy = 0.0f, post_energy = 0.0f;
    /* 累计事件前半开区间能量。 */
    for (int i = pre_start; i < event_index; i++) pre_energy += dynamic_magnitude[i] * dynamic_magnitude[i];
    /* 累计包含事件点的事件后区间能量。 */
    for (int i = event_index; i < post_end; i++) post_energy += dynamic_magnitude[i] * dynamic_magnitude[i];
    /* 1e-9 保护零能量窗口，输出无量纲自然对数比。 */
    feature[(*index)++] = logf((post_energy + 1e-9f) / (pre_energy + 1e-9f));
    /* dynamic_median 是动态加速度稳健中心。 */
    float dynamic_median = wrist_window_median(dynamic_magnitude, WINDOW_LEN);
    /* deviations 保存每点到中位数的绝对距离，单位 g。 */
    float deviations[WINDOW_LEN];
    /* 生成 MAD 所需绝对偏差。 */
    for (int i = 0; i < WINDOW_LEN; i++) deviations[i] = fabsf(dynamic_magnitude[i] - dynamic_median);
    /* MAD 对孤立冲击不敏感。 */
    float dynamic_mad = wrist_window_median(deviations, WINDOW_LEN);
    /* 稳健恢复门槛为 median+0.5*MAD。 */
    float recovery_threshold = dynamic_median + 0.5f * dynamic_mad;
    /* 实际下限 0.05g 避免静止量化噪声产生长恢复。 */
    if (recovery_threshold < 0.05f) recovery_threshold = 0.05f;
    /* 默认到窗口末点仍未恢复。 */
    int recovery_index = WINDOW_LEN - 1;
    /* 寻找事件后连续两个不超过门槛的采样。 */
    for (int i = event_index + 1; i < WINDOW_LEN - 1; i++) {
        /* 首个连续双点满足条件的位置定义为恢复点。 */
        if (dynamic_magnitude[i] <= recovery_threshold && dynamic_magnitude[i + 1] <= recovery_threshold) {
            /* 保存最早恢复位置。 */
            recovery_index = i;
            /* 只保留第一次恢复。 */
            break;
        }
    }
    /* denominator 至少为一，避免事件位于末点时除零。 */
    int recovery_denominator = WINDOW_LEN - 1 - event_index;
    /* 末点事件使用一作为分母。 */
    if (recovery_denominator < 1) recovery_denominator = 1;
    /* 输出事件后归一化恢复时长。 */
    feature[(*index)++] = (float)(recovery_index - event_index) / (float)recovery_denominator;

    /* gyro_smoothed 是用于周期峰检测的三点平滑角速度模长。 */
    float gyro_smoothed[WINDOW_LEN];
    /* 平滑全部角速度点，边界复制。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* left 取前点，首点使用自身。 */
        float left = gyro_magnitude[i > 0 ? i - 1 : 0];
        /* right 取后点，末点使用自身。 */
        float right = gyro_magnitude[i + 1 < WINDOW_LEN ? i + 1 : WINDOW_LEN - 1];
        /* 三点均值与 Python 一致。 */
        gyro_smoothed[i] = (left + gyro_magnitude[i] + right) / 3.0f;
    }
    /* gyro_mean 和 gyro_variance 由未平滑模长计算峰门槛。 */
    float gyro_mean = 0.0f, gyro_variance = 0.0f;
    /* 累加均值分子。 */
    for (int i = 0; i < WINDOW_LEN; i++) gyro_mean += gyro_magnitude[i];
    /* 除以点数得到 deg/s 均值。 */
    gyro_mean /= (float)WINDOW_LEN;
    /* 累加总体方差分子。 */
    for (int i = 0; i < WINDOW_LEN; i++) { float delta = gyro_magnitude[i] - gyro_mean; gyro_variance += delta * delta; }
    /* 除以 N 与 numpy.std 默认 ddof=0 一致。 */
    gyro_variance /= (float)WINDOW_LEN;
    /* 中位数加 0.5 倍标准差作为周期活动峰门槛。 */
    float cycle_threshold = wrist_window_median(gyro_magnitude, WINDOW_LEN) + 0.5f * sqrtf(gyro_variance);
    /* candidate 标记满足局部峰和幅值门槛的内部点。 */
    int candidate[WINDOW_LEN] = {0};
    /* 扫描所有具有左右邻点的采样。 */
    for (int i = 1; i < WINDOW_LEN - 1; i++) {
        /* 严格高于左点、不低于右点且达到门槛时成为候选。 */
        if (gyro_smoothed[i] > gyro_smoothed[i - 1] && gyro_smoothed[i] >= gyro_smoothed[i + 1] && gyro_smoothed[i] >= cycle_threshold) candidate[i] = 1;
    }
    /* selected 保存按强度筛选后的峰索引。 */
    int selected[WINDOW_LEN];
    /* selected_count 是当前保留峰数。 */
    int selected_count = 0;
    /* minimum_distance 对应 round(0.3*25)=8 点。 */
    int minimum_distance = (int)(0.30f * (float)SAMPLE_RATE_HZ + 0.5f);
    /* 数值下限为两个采样点。 */
    if (minimum_distance < 2) minimum_distance = 2;
    /* 每轮取剩余候选中幅值最大、索引最小者。 */
    while (1) {
        /* best_index=-1 表示已无候选峰。 */
        int best_index = -1;
        /* 线性扫描实现 Python 的 (-幅值,索引) 排序顺序。 */
        for (int i = 1; i < WINDOW_LEN - 1; i++) if (candidate[i] && (best_index < 0 || gyro_smoothed[i] > gyro_smoothed[best_index])) best_index = i;
        /* 无候选时结束强峰选择。 */
        if (best_index < 0) break;
        /* 当前候选处理后清除标记。 */
        candidate[best_index] = 0;
        /* keep 默认保留，若靠近任何已选强峰则取消。 */
        int keep = 1;
        /* 检查与全部已选峰的距离。 */
        for (int j = 0; j < selected_count; j++) if (abs(best_index - selected[j]) < minimum_distance) keep = 0;
        /* 通过最小距离约束时追加峰索引。 */
        if (keep) selected[selected_count++] = best_index;
    }
    /* 对已选索引升序排序，便于计算时间间隔。 */
    for (int i = 1; i < selected_count; i++) {
        /* key 是当前待插入峰索引。 */
        int key = selected[i];
        /* j 从有序前缀末尾开始。 */
        int j = i - 1;
        /* 大于 key 的峰索引右移。 */
        while (j >= 0 && selected[j] > key) { selected[j + 1] = selected[j]; j--; }
        /* 将 key 写入时间顺序位置。 */
        selected[j + 1] = key;
    }
    /* cycle_cv 默认无周期证据为零。 */
    float cycle_cv = 0.0f;
    /* 至少三个峰形成两个间隔时才计算 CV。 */
    if (selected_count >= 3) {
        /* interval_count 比峰数少一。 */
        int interval_count = selected_count - 1;
        /* interval_mean 累加峰间隔采样点数。 */
        float interval_mean = 0.0f;
        /* 累加全部相邻峰间隔。 */
        for (int i = 0; i < interval_count; i++) interval_mean += (float)(selected[i + 1] - selected[i]);
        /* 除以间隔数得到均值。 */
        interval_mean /= (float)interval_count;
        /* interval_variance 累加总体方差。 */
        float interval_variance = 0.0f;
        /* 遍历间隔并累计离均差平方。 */
        for (int i = 0; i < interval_count; i++) { float delta = (float)(selected[i + 1] - selected[i]) - interval_mean; interval_variance += delta * delta; }
        /* 有效均值时按总体标准差除以均值得到无量纲 CV。 */
        if (interval_mean > 1e-12f) cycle_cv = sqrtf(interval_variance / (float)interval_count) / interval_mean;
    }
    /* 追加周期峰间隔变异系数。 */
    feature[(*index)++] = cycle_cv;
    /* two_pi 是 Hann 窗和直接 DFT 的 2*pi 单精度常量。 */
    const float two_pi = 6.2831853071795864769f;
    /* total_power 用于识别近静止窗口。 */
    float total_power = 0.0f;
    /* dominant_power 和 dominant_index 保存最早最大非直流频点。 */
    float dominant_power = 0.0f;
    int dominant_index = 0;
    /* 遍历 rfft 的非直流单边频点。 */
    for (int k = 1; k <= WINDOW_LEN / 2; k++) {
        /* real 和 imaginary 累加当前频点 DFT。 */
        float real = 0.0f, imaginary = 0.0f;
        /* 遍历窗口计算去均值、Hann 加窗后的 DFT。 */
        for (int sample = 0; sample < WINDOW_LEN; sample++) {
            /* Hann 系数与 numpy.hanning(WINDOW_LEN) 一致。 */
            float hann = 0.5f - 0.5f * cosf(two_pi * (float)sample / (float)(WINDOW_LEN - 1));
            /* value 是中心化角速度模长乘 Hann 系数。 */
            float value = (gyro_magnitude[sample] - gyro_mean) * hann;
            /* angle 是当前频点相位，单位 rad。 */
            float angle = two_pi * (float)k * (float)sample / (float)WINDOW_LEN;
            /* 累加实部。 */
            real += value * cosf(angle);
            /* 累加负正弦虚部。 */
            imaginary -= value * sinf(angle);
        }
        /* 当前功率是实部和虚部平方和。 */
        float power = real * real + imaginary * imaginary;
        /* 累加非直流总功率。 */
        total_power += power;
        /* 严格更大才更新，平局保留较低频率。 */
        if (power > dominant_power) { dominant_power = power; dominant_index = k; }
    }
    /* harmonic_ratio 默认近静止或频谱不足时为零。 */
    float harmonic_ratio = 0.0f;
    /* 有效主频功率和总功率时计算二次谐波。 */
    if (total_power > 1e-12f && dominant_power > 1e-12f && WINDOW_LEN / 2 >= 2) {
        /* 二次谐波超出单边频谱时截断到最后频点。 */
        int harmonic_index = 2 * dominant_index;
        /* 限制到 rfft 末端。 */
        if (harmonic_index > WINDOW_LEN / 2) harmonic_index = WINDOW_LEN / 2;
        /* 重新计算目标谐波频点 DFT。 */
        float real = 0.0f, imaginary = 0.0f;
        /* 遍历全部时域点。 */
        for (int sample = 0; sample < WINDOW_LEN; sample++) {
            /* Hann 窗系数。 */
            float hann = 0.5f - 0.5f * cosf(two_pi * (float)sample / (float)(WINDOW_LEN - 1));
            /* 去均值加窗角速度模长。 */
            float value = (gyro_magnitude[sample] - gyro_mean) * hann;
            /* 二次谐波 DFT 相位。 */
            float angle = two_pi * (float)harmonic_index * (float)sample / (float)WINDOW_LEN;
            /* 累加实部。 */
            real += value * cosf(angle);
            /* 累加虚部。 */
            imaginary -= value * sinf(angle);
        }
        /* 谐波功率除以基频功率得到无量纲比值。 */
        harmonic_ratio = (real * real + imaginary * imaginary) / dominant_power;
    }
    /* 追加二次谐波/基频功率比，完成六项输出。 */
    feature[(*index)++] = harmonic_ratio;
}

static inline void extract_features_from_window(const float raw_window[WINDOW_LEN][AXIS_NUM], float feature[FEATURE_DIM]) {
    /* cleaned_window 保存尖峰修复后的 [WINDOW_LEN,6] 六轴数据，约占 WINDOW_LEN*24 字节 RAM。 */
    float cleaned_window[WINDOW_LEN][AXIS_NUM];
    /* 按 Python 相同阈值和单轴判据清洗输入，保证部署端与训练端特征数值一致。 */
    preprocess_imu_window(raw_window, cleaned_window);
    /* window 指向清洗后数组；后续全部特征函数无需修改参数形式。 */
    const float (*window)[AXIS_NUM] = cleaned_window;
    /* idx 记录下一个待写入特征位置，最终必须等于 FEATURE_DIM。 */
    int idx = 0;
    /* temp 复用为当前一维序列缓冲区，长度上限为 WINDOW_LEN。 */
    float temp[WINDOW_LEN];
    /* phase_sources 保存四条相位特征源序列，形状为 [4,WINDOW_LEN]。 */
    float phase_sources[4][WINDOW_LEN];
    /* phase_lengths 标记前三条长度为 WINDOW_LEN，差分加速度长度为 WINDOW_LEN-1。 */
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
    /* 追加手腕 PCA 主轴每秒有效换向次数，单位 Hz。 */
    feature[idx++] = wrist_reversal_rate_hz(window);
    /* 追加手腕角速度模长自相关第二时间峰与第一时间峰之比。 */
    feature[idx++] = wrist_acf_second_first_ratio(
        phase_sources[2], phase_lengths[2]
    );
    /* 追加清洗后晋级的角速度模长第一正自相关峰，形成 297 维最终候选。 */
    feature[idx++] = wrist_acf_first_peak(
        phase_sources[2], phase_lengths[2]
    );
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
#if SUPPRESS_NORMALIZED_PHASE
    /* 将 48 个归一化阶段标准分固定为 0，等价于输入训练集均值并与 Python 掩码一致。 */
    for (int i = NORMALIZED_PHASE_MODEL_START; i < NORMALIZED_PHASE_MODEL_END; i++) x[i] = 0.0f;
#endif
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
        "suppress_normalized_phase",
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
        # 保存主 BP 的 48 维输入抑制开关，ESP32 和后续加载必须读取同一合同。
        "suppress_normalized_phase": np.asarray(
            [bool(best_result.get("suppress_normalized_phase", False))], dtype=np.bool_
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
        "suppress_normalized_phase": bool(
            best_result.get("suppress_normalized_phase", False)
        ),
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
        # 保存候选掩码，防止后续把权重按未屏蔽输入加载。
        suppress_normalized_phase=np.asarray(
            [bool(best_result.get("suppress_normalized_phase", False))], dtype=np.bool_
        ),
        # 保存验证候选模型类型，避免后续把 M1 state_dict 误加载到 M0。
        model_type=np.asarray(
            [
                "deep_narrow_multi_branch"
                if isinstance(model, DeepNarrowMultiBranchBPNet)
                else "multi_branch"
                if isinstance(model, MultiBranchBPNet)
                else "flat_bp"
            ]
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
        "suppress_normalized_phase",
        "multi_branch",
        "deep_narrow",
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
        "suppress_normalized_phase": bool(
            best_result.get("suppress_normalized_phase", False)
        ),
        # 分类器类型明确区分 24 维 M1、32 维 M0 和平铺 BP。
        "classifier_type": (
            "deep_narrow_multi_branch"
            if isinstance(model, DeepNarrowMultiBranchBPNet)
            else "multi_branch"
            if isinstance(model, MultiBranchBPNet)
            else "flat_bp"
        ),
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
    # 在六分支基础上启用 88→64→48→32→24 的 M1 深窄融合结构。
    parser.add_argument("--deep-narrow", action="store_true")
    # 启用每批 P 个类别、每类 K=6 个且优先跨文件的批采样策略。
    parser.add_argument("--pk-batches", action="store_true")
    # 在均匀 P×K 批次中按原训练窗口计数加权 CE，恢复类别先验。
    parser.add_argument("--pk-prior-corrected-ce", action="store_true")
    # 启用五个仅训练期使用的运动属性辅助分类头。
    parser.add_argument("--auxiliary-heads", action="store_true")
    # 将 48 个归一化四阶段特征在标准化后设为训练均值零分，用于 Round36 证据候选。
    parser.add_argument("--suppress-normalized-phase", action="store_true")
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
    # M1 必须建立在六分支编码之上。
    if args.deep_narrow and not args.multi_branch:
        raise ValueError("--deep-narrow requires --multi-branch")
    # T2 不允许同时启用训练期辅助头，避免混入第二个实验变量。
    if args.deep_narrow and args.auxiliary_heads:
        raise ValueError("--deep-narrow cannot be combined with --auxiliary-heads")
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
        f"deep_narrow={args.deep_narrow} "
        f"pk_batches={args.pk_batches} "
        f"pk_prior_corrected_ce={args.pk_prior_corrected_ce} "
        f"auxiliary_heads={args.auxiliary_heads} "
        f"suppress_normalized_phase={args.suppress_normalized_phase} "
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
            deep_narrow=args.deep_narrow,
            pk_batches=args.pk_batches,
            auxiliary_heads=args.auxiliary_heads,
            pk_prior_corrected_ce=args.pk_prior_corrected_ce,
            supcon_weight=args.supcon_weight,
            dropout=args.dropout,
            suppress_normalized_phase=args.suppress_normalized_phase,
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
