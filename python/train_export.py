"""手腕六轴 IMU 的前处理、297维特征、BP训练、评估与ESP32模型导出主程序。"""

# argparse 解析数据路径、训练模式、模型消融和导出控制命令行参数。
import argparse
# copy 深拷贝最佳模型、EMA模型和 state_dict，避免后续训练原地覆盖检查点。
import copy
# json 保存训练报告、文件角色、逐类指标和可复现实验配置。
import json
# math 提供三角函数、平方根、对数和有限窗口频谱公式。
import math
# os 读取环境变量和设置线程等运行环境信息。
import os
# random 固定 Python 随机种子，保证文件采样与增强可复现。
import random
# shutil 把达到门槛的生成头文件同步到 ESP32 正式 include 目录。
import shutil
# dataclass 定义不可变的 IMU 文件记录，绑定路径、类别名和类别索引。
from dataclasses import dataclass
# Path 统一处理数据集、训练输出、模型工件和 ESP32 头文件路径。
from pathlib import Path
# 类型工具明确数组集合、可选参数、固定顺序和函数返回合同。
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

# matplotlib 生成无界面训练曲线和混淆矩阵图片。
import matplotlib

# 使用 Agg 后端，保证 PyCharm、服务器和无显示器环境都能保存图片。
matplotlib.use("Agg")
# pyplot 绘制训练历史和评估图表，不负责交互式窗口显示。
import matplotlib.pyplot as plt
# NumPy 承担六轴数组、窗口、特征、标准化和统计信号处理。
import numpy as np
# PyTorch 构建 BP 网络、训练张量、优化器和冻结工件加载。
import torch
# sklearn 指标函数计算准确率、宏F1、逐类报告和固定类别混淆矩阵。
from sklearn.metrics import accuracy_score, classification_report, confusion_matrix, f1_score
# train_test_split 按文件和类别分层划分训练、验证、测试角色。
from sklearn.model_selection import train_test_split
# nn 提供 Linear、ReLU、Dropout、ModuleList 和模型基类。
from torch import nn
# F 提供交叉熵、归一化和 ReLU 等无状态张量函数。
from torch.nn import functional as F
# 数据工具构造普通批次、文件均衡采样和 P×K 自定义采样器。
from torch.utils.data import DataLoader, Sampler, TensorDataset, WeightedRandomSampler


# 固定全流程随机种子，使文件划分、增强、采样和模型初始化可复现。
SEED = 20260709
# 项目根目录用于解析仓库内 ESP32、docs 和默认输出位置。
PROJECT_ROOT = Path(__file__).resolve().parents[1]
# 首选数据集目录是仓库下 IMU_Dataset/imu_dataset_for_final。
DEFAULT_DATASET_DIR = Path("IMU_Dataset") / "imu_dataset_for_final"
# 兼容旧目录结构：数据集也可直接位于当前目录下 imu_dataset_for_final。
FALLBACK_DATASET_DIR = Path("imu_dataset_for_final")
# 默认训练工件写入项目本地 outputs，避免污染源码目录。
OUTPUT_DIR = Path("outputs")
# 达标模型头文件的正式发布目标，供 ESP32 编译直接包含。
ESP32_MODEL_HEADER = PROJECT_ROOT / "esp32" / "include" / "esp32_bp_model.h"

# IMU 采样率固定 25 Hz；所有时间、频率和窗口公式都依赖该值。
SAMPLE_RATE = 25
# 相邻推理窗口目标步长为 0.5 秒，换算后当前实际为 12 点约 0.48 秒。
STEP_SECONDS = 0.5
# 默认验证 1.5、2.0、2.5 秒窗口，最终模型使用 2.5 秒。
WINDOW_SECONDS_LIST = (1.5, 2.0, 2.5)
# 命令行额外允许 4.0 秒上下文实验，但不改变默认列表。
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
# 基础数据按文件分层后，70% 文件用于训练模型和标准化参数。
TRAIN_RATIO = 0.70
# 基础数据按文件分层后，15% 文件固定为验证角色，用于选模型和决策参数。
VAL_RATIO = 0.15
# 基础数据按文件分层后，剩余 15% 文件仅在参数锁定后做测试确认。
TEST_RATIO = 0.15
# 静止类别名称固定为 sit，用于估计噪声阈值和保留静态窗口。
SIT_CLASS_NAME = "sit"
# 四个高动态类别必须同时满足更严格运动分数和活动点比例。
HIGH_DYNAMIC_CLASSES = {"jumping_jack", "jumping_lunge", "jumping_squat", "tuck_jump"}
# 弱类顺序用于检查点平均召回和日志，不改变全局11类输出顺序。
WEAK_CLASS_NAMES = ["jumping_squat", "squat", "tuck_jump", "jumping_lunge"]
# 三目标专家历史消融只覆盖跳跃深蹲、普通深蹲和收腹跳。
FAMILY_SPECIALIST_CLASS_NAMES = [
    "jumping_squat",
    "squat",
    "tuck_jump",
]
# HARD_CONFUSION_PAIRS 定义定向 logit 间隔约束；键是真类，值是需要压低的易混类列表。
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
# 普通类别发布门槛为测试召回不低于 90%。
TARGET_MIN_CLASS_RECALL = 0.90
# 经批准弱类发布门槛为测试召回不低于 85%。
WEAK_TARGET_MIN_CLASS_RECALL = 0.85
# 五个弱类使用较低 85% 门槛，其余类别仍使用 90%。
RELAXED_RECALL_CLASS_NAMES = {
    "jumping_jack",
    "jumping_lunge",
    "jumping_squat",
    "squat",
    "tuck_jump",
}

# 默认最多训练 350 个 epoch，命令行可在可见训练时覆盖。
MAX_EPOCHS = 350
# 普通 DataLoader 每批最多 64 个标准化特征向量。
BATCH_SIZE = 64
# AdamW 初始学习率固定为 1e-3。
LEARNING_RATE = 1e-3
# AdamW 权重衰减 1e-4，抑制小数据下全连接权重过大。
WEIGHT_DECAY = 1e-4
# 验证检查点连续 45 个 epoch 不改善时早停。
PATIENCE = 45
# 融合层默认丢弃 10% 训练激活；评估和部署时自动关闭。
DROPOUT = 0.10
# 每个原始训练窗口额外生成两份旋转、时间形变和噪声增强样本。
AUGMENT_TIMES = 2
# 六轴同步随机旋转最大绝对欧拉角为 35 度，用于模拟佩戴方向差异。
MAX_ROTATION_DEGREES = 35.0
# 未达逐类门槛时默认禁止把实验头文件发布到 ESP32 正式目录。
EXPORT_WHEN_BELOW_TARGET = False
# 跨文件监督对比损失默认权重为 0.05。
SUPCON_WEIGHT = 0.05
# 易混类别局部 logit 间隔损失默认权重为 0.25。
HARD_PAIR_WEIGHT = 0.25
# 五个训练期辅助任务的总损失权重；部署时不导出辅助分类头。
AUXILIARY_WEIGHT = 0.10

# 六个原始通道名称固定对应数组列 0..5，前三轴 deg/s、后三轴 g。
CHANNEL_NAMES = ["gx", "gy", "gz", "ax", "ay", "az"]
# 两个模长序列分别汇总三轴角速度和三轴加速度。
MAG_NAMES = ["gyro_mag", "acc_mag"]
# 两个相邻差分模长序列突出角速度换向和加速度冲击。
DELTA_MAG_NAMES = ["gyro_delta_mag", "acc_delta_mag"]
# 四个重力对齐序列分解垂直/水平加速度与角速度。
GRAVITY_NAMES = [
    "acc_vertical",
    "acc_horizontal_mag",
    "gyro_vertical",
    "gyro_horizontal_mag",
]
# 14条全局序列各提取8项基础统计，总计112维。
GLOBAL_SERIES_NAMES = CHANNEL_NAMES + MAG_NAMES + DELTA_MAG_NAMES + GRAVITY_NAMES
# 四条动作机理核心序列用于阶段、时序和冲击分布特征。
PHASE_SOURCE_NAMES = ["acc_vertical", "acc_horizontal_mag", "gyro_mag", "acc_delta_mag"]
# 每条全局序列依次提取均值、标准差、范围、RMS和变化统计。
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
# 每个核心序列按时间等分成4个阶段。
PHASE_SEGMENTS = 4
# 每个阶段输出均值、标准差和最大绝对值三项。
PHASE_FEATURES = ["mean", "std", "max_abs"]
# 每个核心序列输出活动比例、峰数、主频、谱熵和自相关两项。
TEMPORAL_FEATURES = [
    "high_activity_ratio",
    "peak_count_normalized",
    "dominant_frequency_hz",
    "spectral_entropy",
    "autocorr_peak",
    "autocorr_peak_lag_seconds",
]
# 每个核心序列输出五个分位数、偏度、超额峰度和最大跳变。
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
# 33项弱类机制特征的名称和顺序必须与 weak_class_features 及生成C完全一致。
WEAK_CLASS_FEATURE_NAMES = [
    # 加速度变化模长中频占比，描述重复冲击的中等节奏能量。
    "acc_delta_mag_spectral_mid_band_ratio",
    # 垂直加速度高频占比，描述收腹跳等快速垂直变化。
    "acc_vertical_spectral_high_band_ratio",
    # 陀螺模长谱质心，单位 Hz，描述手腕转动速度重心。
    "gyro_mag_spectral_centroid_hz",
    # 水平加速度谱质心，单位 Hz，描述横向运动节奏重心。
    "acc_horizontal_mag_spectral_centroid_hz",
    # 垂直加速度自相关首次过零时间，单位秒。
    "acc_vertical_autocorr_first_zero_seconds",
    # 陀螺模长与垂直加速度零延迟相关，描述转动和冲击同步性。
    "event_gyro_vertical_correlation",
    # 水平加速度自相关次峰，描述重复水平摆动。
    "acc_horizontal_mag_autocorr_secondary_peak",
    # 陀螺模长高频占比，区分快速收腹与稳定跳蹲转动。
    "gyro_mag_spectral_high_band_ratio",
    # 垂直加速度主谱峰功率占比，描述垂直周期集中程度。
    "acc_vertical_spectral_peak_power_ratio",
    # 加速度变化模长高频占比，描述落地和快速冲击。
    "acc_delta_mag_spectral_high_band_ratio",
    # 水平加速度中频占比，补充分离挥手、深蹲和跳跃。
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

# 兼容旧平铺 BP 的第一隐藏层宽度为96。
HIDDEN1 = 96
# 兼容旧平铺 BP 的第二隐藏层宽度为64。
HIDDEN2 = 64
# 兼容旧平铺 BP 的第三隐藏层及部署嵌入宽度为32。
HIDDEN3 = 32


@dataclass(frozen=True)
class ImuRecord:
    """绑定一个原始IMU文件路径、动作类别名和固定全局类别索引。"""

    # path 指向一个可读取的文本记录，文件内六列顺序固定 gx、gy、gz、ax、ay、az。
    path: Path
    # label 保存目录动作名称，例如 jumping_squat。
    label: str
    # label_idx 保存 label 在排序后11类名称表中的整数位置。
    label_idx: int


class CausalLogitSmoother:
    """用固定 15 槽环形缓冲区计算当前及历史窗口 logits 的因果均值。"""

    def __init__(
        self,
        class_count: int,
        history_length: int = TEMPORAL_LOGIT_HISTORY,
    ) -> None:
        """分配类别固定的历史缓冲区，并验证类别数及历史长度。"""
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
        """分配一个活动段的 float64 logit 累计和及窗口计数。"""
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
    """兼容旧ESP32导出器的297→96→64→32→类别数平铺全连接网络。"""

    def __init__(self, input_dim: int, class_count: int, dropout: float = DROPOUT):
        """按输入特征数、类别数和训练期丢弃率构造平铺BP。"""

        # 注册所有线性层、ReLU和Dropout为可训练 PyTorch 子模块。
        super().__init__()
        # net 按固定顺序保存四个线性层，索引8是最终分类头。
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
        """把[批大小,输入维度]特征编码为[批大小,32]部署嵌入。"""

        # 依次执行最终分类头之前的8个层对象，保持与旧导出层索引一致。
        for layer in list(self.net.children())[:8]:
            # 当前层更新批量激活，批大小保持不变。
            x = layer(x)
        # 返回分类头之前的32维嵌入，供主分类和训练损失共用。
        return x

    def classify_features(self, embeddings: torch.Tensor) -> torch.Tensor:
        """把 ``[批大小,32]`` 嵌入映射为 ``[批大小,类别数]`` logits。"""
        # 将形状为 [批大小,32] 的嵌入送入原 BP 输出层，得到 [批大小,类别数] logits。
        return self.net[8](embeddings)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """输入[批大小,输入维度]并返回[批大小,类别数]未归一化logits。"""

        # 先编码32维嵌入，再调用固定输出层完成主分类。
        return self.classify_features(self.forward_features(x))


class MultiBranchBPNet(nn.Module):
    """按统计、相位、相关、时序、冲击和弱类特征分组编码的轻量 BP 网络。"""

    # 六组维度严格对应 build_feature_names() 的生产顺序，总和必须为 297。
    group_input_dims = (112, 48, 24, 48, 32, 33)
    # 各分支压缩到较小嵌入，限制参数量并避免 33 个弱类特征被 112 个统计量淹没。
    group_output_dims = (24, 12, 8, 12, 8, 16)

    def __init__(self, input_dim: int, class_count: int, dropout: float = DROPOUT):
        """构建六特征组分支、32 维融合层、主分类头和五个训练辅助头。"""
        # 初始化 PyTorch 模块注册表，使分支和辅助头参与优化及 checkpoint 保存。
        super().__init__()
        # 输入维度必须等于六组特征维度之和，否则切片会错位并破坏 Python/C 一致性。
        if input_dim != sum(self.group_input_dims):
            # 旧工件或特征顺序维度不一致时立即拒绝，避免静默错位。
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
        """把 ``[批大小,297]`` 标准化特征编码为 ``[批大小,32]`` 融合嵌入。"""
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
        """把 ``[批大小,32]`` 融合嵌入线性映射为主类别 logits。"""
        # 主分类头输出 [批大小,类别数] 未归一化 logits，供交叉熵和间隔损失使用。
        return self.classifier(embeddings)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """执行 M0 六分支编码、融合和主分类完整前向。"""
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
            """计算一个属性头的有效正负样本交叉熵，并追加到外层损失列表。"""
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
                # 把标签等于当前有效类的位置合并进 [批大小] 布尔掩码。
                mask |= labels == class_index
            # 若本批没有适用样本，跳过该头，避免对空张量计算交叉熵。
            if not torch.any(mask):
                # 无适用样本时当前辅助任务不向 losses 追加值。
                return
            # 二分类目标中 1 表示属性存在，0 表示属性不存在。
            targets = torch.zeros(int(mask.sum().item()), dtype=torch.long, device=labels.device)
            # 遍历被选样本并按主类别是否属于正类集合生成目标。
            selected_labels = labels[mask]
            # 遍历属性正类索引；每轮把对应任务样本目标从 0 改为 1。
            for class_index in positive_indices:
                # selected_labels 等于当前正类的位置写入二分类正标签 1。
                targets[selected_labels == class_index] = 1
            # 只有同时含正负样本时才训练该头，防止单类批次造成无效偏置更新。
            if torch.unique(targets).numel() < 2:
                # 单一目标类别无法形成有效二分类边界，本批跳过该任务。
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
        """构建 M1 六分支及 88→64→48→32→24 深窄融合分类网络。"""
        # 初始化 PyTorch 模块注册表，使全部分支和融合层参与优化及保存。
        super().__init__()
        # 输入必须等于六组总和 297，防止切片错位。
        if input_dim != sum(self.group_input_dims):
            # 错误消息同时报告期望和实际维度，便于发现旧 296/302 维缓存。
            raise ValueError(
                f"Deep-narrow model requires {sum(self.group_input_dims)} features, got {input_dim}"
            )
        # 每个分支独立执行 Linear-ReLU，避免 112 维统计组淹没 33 维弱类组。
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
        # offset 指向当前分支在 297 维输入中的起始列。
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
    """计算跨文件监督对比损失，使同类不同采集文件的嵌入靠近、异类嵌入分离。

    输入 ``embeddings`` 形状为 ``[批大小,嵌入维度]``，``labels`` 与 ``file_ids``
    均为 ``[批大小]``。余弦相似度先除以温度系数，再对每个锚点计算
    ``-mean(sim(anchor, positive) - logsumexp(sim(anchor, valid)))``。同一文件的同类窗口
    不作为正样本，避免模型只记住单次采集的佩戴姿态或传感器偏置。时间复杂度和额外空间
    均为 O(B^2)，其中 B 为批大小；该损失只在训练端使用，不增加 ESP32 推理开销。
    """
    # 少于两个嵌入时无法构造锚点-样本对，返回与计算图相连的标量零损失。
    if len(embeddings) < 2:
        # embeddings.sum()*0 保留设备和 dtype，并允许调用方统一执行反向传播。
        return embeddings.sum() * 0.0
    # 对嵌入最后一维做 L2 归一化，使点积等于范围 [-1,1] 的余弦相似度。
    normalized = F.normalize(embeddings, dim=1)
    # logits 形状为 [B,B]；温度越小，相似度差异在 softmax 中越突出。
    logits = normalized @ normalized.T / temperature
    # losses 保存具备至少一个跨文件同类正样本的锚点损失标量。
    losses: List[torch.Tensor] = []
    # 遍历批内 B 个嵌入；每轮固定一个锚点并搜索其有效正样本和分母样本。
    for anchor in range(len(embeddings)):
        # different_sample 形状为 [B]，排除锚点自身，避免相似度 1 形成无意义正样本。
        different_sample = torch.arange(len(embeddings), device=embeddings.device) != anchor
        # positive 仅选择“同动作类别且来自不同原始文件”的跨采集正样本。
        positive = (
            different_sample
            & (labels == labels[anchor])
            & (file_ids != file_ids[anchor])
        )
        # valid 排除锚点自身及同文件同类窗口，保留跨文件同类和所有异类作为归一化集合。
        valid = different_sample & (
            (labels != labels[anchor]) | (file_ids != file_ids[anchor])
        )
        # 当前锚点没有跨文件同类样本时无法形成监督对比目标，跳过本轮。
        if not torch.any(positive):
            # 继续处理下一个锚点，不向 losses 写入偏置性零值。
            continue
        # logsumexp 稳定计算有效集合相似度的对数和，避免直接 exp 后上溢。
        denominator = torch.logsumexp(logits[anchor][valid], dim=0)
        # 对当前锚点的全部跨文件正样本取平均负对数概率，并追加一个标量损失。
        losses.append(-(logits[anchor][positive] - denominator).mean())
    # 整个批次没有有效锚点时，返回可反传零值而不是对空列表执行 stack。
    if not losses:
        # 返回值与 embeddings 位于相同设备，保证 CPU/CUDA 路径一致。
        return embeddings.sum() * 0.0
    # 对所有有效锚点等权平均，返回供总损失加权的标量张量。
    return torch.stack(losses).mean()


def hard_pair_margin_loss(
    logits: torch.Tensor,
    labels: torch.Tensor,
    class_names: Sequence[str],
    margin: float = 0.5,
) -> torch.Tensor:
    """约束弱类真值 logit 至少比指定易混类别高 ``margin``。

    ``logits`` 形状为 ``[批大小,类别数]``，``labels`` 形状为 ``[批大小]``。
    每个困难类别对使用 ``max(0, margin-y_true+y_confusing)``；仅训练期间参与优化，
    不改变最终 BP 网络层数、参数量或 ESP32 前向公式。时间复杂度为 O(BP)，P 为配置的
    困难类别对数量，额外空间至多 O(BP)。
    """
    # name_to_idx 把报告中的类别名映射到 logits 第二维索引，避免依赖外部固定顺序。
    name_to_idx = {name: index for index, name in enumerate(class_names)}
    # losses 收集每个存在样本的“真类-易混类”间隔损失标量。
    losses: List[torch.Tensor] = []
    # 遍历预先分析得到的弱类及其易混类别列表，数量由 HARD_CONFUSION_PAIRS 固定。
    for true_name, confusing_names in HARD_CONFUSION_PAIRS.items():
        # 当前数据集不含该弱类时跳过，允许单元测试使用类别子集。
        if true_name not in name_to_idx:
            # 不访问不存在的 logits 列，继续检查下一弱类。
            continue
        # true_idx 是当前弱类在 logits 第二维中的列号。
        true_idx = name_to_idx[true_name]
        # sample_mask 形状为 [B]，只选择真值等于当前弱类的训练样本。
        sample_mask = labels == true_idx
        # 当前批次没有该弱类样本时无法计算此弱类间隔，跳过本轮。
        if not torch.any(sample_mask):
            # P×K 采样通常会覆盖所有类，但此保护兼容普通小批次。
            continue
        # true_logits 形状为 [当前弱类样本数]，保存正确类别未经 softmax 的分数。
        true_logits = logits[sample_mask, true_idx]
        # 遍历该弱类的全部易混类别；每轮新增一个二类间隔约束。
        for confusing_name in confusing_names:
            # 数据集类别子集不含易混类时跳过，避免名称映射异常。
            if confusing_name not in name_to_idx:
                # 继续处理当前弱类的下一个已配置易混类别。
                continue
            # confusing_idx 是易混类别在 logits 中的列号。
            confusing_idx = name_to_idx[confusing_name]
            # confusing_logits 与 true_logits 形状相同，来自同一批弱类样本。
            confusing_logits = logits[sample_mask, confusing_idx]
            # ReLU 实现 hinge：真类领先达到 margin 后损失为 0，否则线性惩罚差额。
            losses.append(F.relu(margin - true_logits + confusing_logits).mean())
    # 没有任何适用困难类别对时返回可反传零值，避免 stack 空列表。
    if not losses:
        # logits.sum()*0 保持当前设备、浮点类型和计算图连接。
        return logits.sum() * 0.0
    # 对全部有效困难对等权平均，返回总训练损失中的标量项。
    return torch.stack(losses).mean()


def set_seed(seed: int) -> None:
    """固定 Python、NumPy、PyTorch 和 CUDA 随机源，降低重复训练波动。"""
    # 固定 Python random，用于记录抽样、数据增强参数和普通列表随机操作。
    random.seed(seed)
    # 固定 NumPy 随机生成器，用于窗口增强和数据划分中的数组随机操作。
    np.random.seed(seed)
    # 固定当前进程的 PyTorch CPU 随机生成器，用于权重初始化和 dropout。
    torch.manual_seed(seed)
    # 固定所有可见 CUDA 设备的随机生成器；无 CUDA 时该调用安全且不分配显存。
    torch.cuda.manual_seed_all(seed)
    # 强制 cuDNN 选择确定性算子，优先复现实验结果而非最高吞吐量。
    torch.backends.cudnn.deterministic = True
    # 禁止 cuDNN 根据首批输入自动切换算法，避免相同种子产生实现差异。
    torch.backends.cudnn.benchmark = False


def update_ema_state(
    previous_state: Optional[Dict[str, torch.Tensor]],
    current_state: Dict[str, torch.Tensor],
    decay: float,
) -> Dict[str, torch.Tensor]:
    """按 ``ema=decay*old+(1-decay)*current`` 更新模型参数指数滑动平均。

    输入和返回字典键均与 ``state_dict`` 一致；浮点权重做指数平均，整数计数器直接复制。
    每次调用时间和额外空间复杂度均为 O(P)，P 为模型参数及缓冲区元素总数。
    """
    # decay 必须位于 [0,1)，否则新参数权重非正或旧状态无法衰减。
    if not 0.0 <= decay < 1.0:
        # 明确拒绝非法衰减率，避免静默导出错误权重。
        raise ValueError("EMA decay must be in [0, 1)")
    # 首次更新或显式关闭 EMA 时，直接复制当前状态作为独立快照。
    if previous_state is None or decay == 0.0:
        # detach 阻断训练计算图，clone 防止后续优化器原地修改快照。
        return {name: value.detach().clone() for name, value in current_state.items()}
    # updated 保存本轮完整 EMA 状态，键和值形状均与 current_state 一致。
    updated: Dict[str, torch.Tensor] = {}
    # 遍历模型 state_dict 的每个参数或缓冲区；每轮写入同名 EMA 值。
    for name, current_value in current_state.items():
        # 只有浮点权重和浮点缓冲区适合线性插值。
        if torch.is_floating_point(current_value):
            # 旧状态乘 decay，新状态乘 1-decay；clone 生成与训练参数无共享存储的张量。
            updated[name] = (
                previous_state[name] * decay
                + current_value.detach() * (1.0 - decay)
            ).clone()
        # 整数、布尔等非浮点缓冲区不能做加权平均，直接采用当前值。
        else:
            # detach+clone 保证返回快照与当前模型状态互不共享存储。
            updated[name] = current_value.detach().clone()
    # 返回可直接传给 load_state_dict 的完整 EMA 参数及缓冲区字典。
    return updated


def resolve_dataset_dir(dataset_dir: Optional[Path]) -> Path:
    """按命令行路径、项目默认路径、兼容路径顺序解析主数据集目录。"""
    # candidates 按优先级保存待检查路径；首个存在的目录即为本次训练输入。
    candidates = []
    # 调用方显式指定路径时把它置于最高优先级，不被默认路径覆盖。
    if dataset_dir is not None:
        # 保存用户给定 Path；此处不创建目录，防止拼写错误被掩盖。
        candidates.append(dataset_dir)
    # 依次追加项目内数据集和历史兼容数据集路径，支持无参数运行。
    candidates.extend([DEFAULT_DATASET_DIR, FALLBACK_DATASET_DIR])
    # 按优先级遍历候选；找到首个存在且确为目录的路径后立即结束。
    for candidate in candidates:
        # 同时检查存在性和目录类型，普通文件不能作为动作类别根目录。
        if candidate.exists() and candidate.is_dir():
            # 返回可供 scan_dataset 枚举类别子目录的有效 Path。
            return candidate
    # 所有候选均无效时列出检查过的路径，便于修正命令行参数或目录部署。
    raise FileNotFoundError(
        "Dataset directory not found. Tried: "
        + ", ".join(str(candidate) for candidate in candidates)
    )


def scan_dataset(dataset_dir: Path) -> Tuple[List[ImuRecord], List[str], Dict[str, int]]:
    """扫描主数据集的“类别目录/*.txt”，返回记录、类别顺序和名称索引。"""
    # class_dirs 只保留至少含一个 txt 的直接子目录，并按路径排序以冻结类别顺序。
    class_dirs = sorted(
        [path for path in dataset_dir.iterdir() if path.is_dir() and list(path.glob("*.txt"))]
    )
    # 没有合法类别目录说明路径或数据集布局错误，不能继续生成标签。
    if not class_dirs:
        # 报错包含根路径，帮助定位选择了错误目录层级的问题。
        raise ValueError(f"No action folders with txt files found under {dataset_dir}")

    # class_names 按排序后的目录名生成，顺序决定模型输出和 ESP32 类别索引。
    class_names = [path.name for path in class_dirs]
    # label_to_idx 将类别名映射到从 0 开始的稳定输出索引。
    label_to_idx = {name: idx for idx, name in enumerate(class_names)}
    # records 保存每个原始采集文件及其文本标签、整数标签，不在扫描阶段加载数值。
    records: List[ImuRecord] = []
    # 外层按稳定类别顺序遍历全部动作目录，每轮累积该类文件记录。
    for class_dir in class_dirs:
        # 内层按文件名排序遍历当前类别全部 txt，保证相同数据得到相同文件编号顺序。
        for txt_path in sorted(class_dir.glob("*.txt")):
            # 追加一条只含路径和标签元数据的 ImuRecord，实际六轴数据延迟读取。
            records.append(ImuRecord(txt_path, class_dir.name, label_to_idx[class_dir.name]))
    # 防御性检查记录列表；理论上 class_dirs 非空时至少存在一条记录。
    if not records:
        # 明确报告没有 txt 文件，避免后续划分阶段出现难懂的空数组错误。
        raise ValueError(f"No dataset txt files found under {dataset_dir}")
    # 返回文件级记录、模型类别顺序、类别名到输出索引映射。
    return records, class_names, label_to_idx


def scan_labeled_dataset(
    dataset_dir: Path,
    label_to_idx: Dict[str, int],
) -> List[ImuRecord]:
    """按主数据集类别映射扫描附加训练集或外部留出集，不允许出现新类别。"""
    # 统一转换为 Path，兼容测试或调用方传入字符串路径。
    dataset_dir = Path(dataset_dir)
    # 附加路径必须已经存在且为目录，防止将空列表误认为有效附加集。
    if not dataset_dir.is_dir():
        # 报错保留用户给定路径，便于检查命令行参数。
        raise FileNotFoundError(f"Additional dataset directory not found: {dataset_dir}")
    # records 保存附加目录下所有已知类别文件的元数据。
    records: List[ImuRecord] = []
    # 按目录名排序遍历附加集的直接子目录，使文件编号和报告可复现。
    for class_dir in sorted(path for path in dataset_dir.iterdir() if path.is_dir()):
        # txt_paths 是当前动作类别下按文件名排序的全部采集文件。
        txt_paths = sorted(class_dir.glob("*.txt"))
        # 空目录不代表一个有效类别，直接跳过且不改变主类别顺序。
        if not txt_paths:
            # 继续检查下一个子目录，不为当前空目录创建记录。
            continue
        # label 使用目录名，必须与主数据集动作名称完全一致。
        label = class_dir.name
        # 主数据集未知类别没有对应模型输出节点，因此拒绝继续。
        if label not in label_to_idx:
            # 报错指出未知目录名，避免训练标签与 ESP32 类别表错位。
            raise ValueError(f"Unknown action directory in additional dataset: {label}")
        # 批量追加当前类别所有文件；每条记录复用主数据集的整数标签。
        records.extend(
            ImuRecord(path, label, label_to_idx[label]) for path in txt_paths
        )
    # 有效附加目录必须至少提供一条已知类别记录。
    if not records:
        # 空附加集通常意味着路径层级错误，立即报错而不是静默忽略。
        raise ValueError(f"No labeled txt files found under {dataset_dir}")
    # 返回可与主数据集记录列表拼接的文件级元数据。
    return records


def load_additional_records(
    extra_train_dir: Optional[Path],
    external_holdout_dir: Optional[Path],
    label_to_idx: Dict[str, int],
    validation_only: bool,
) -> Tuple[List[ImuRecord], List[ImuRecord]]:
    """加载可选附加训练记录和外部留出记录，保持二者用途隔离。"""
    # 指定 extra_train_dir 时扫描为训练记录；未指定则返回空列表。
    extra_records = (
        scan_labeled_dataset(extra_train_dir, label_to_idx)
        if extra_train_dir is not None
        else []
    )
    # 外部留出集仅在正式训练模式加载；validation_only 搜参阶段禁止窥视外部测试数据。
    holdout_records = (
        scan_labeled_dataset(external_holdout_dir, label_to_idx)
        if external_holdout_dir is not None and not validation_only
        else []
    )
    # 返回两个独立列表：前者允许进入训练，后者只允许最终评估。
    return extra_records, holdout_records


def convert_raw_imu_units(raw: np.ndarray) -> np.ndarray:
    """把 MPU6050 原始计数转换为 ``[N,6]`` 工程单位六轴数据。

    输入前六列固定为 ``gx、gy、gz、ax、ay、az``。陀螺仪按 ±2000 deg/s 量程的
    16.4 LSB/(deg/s) 转为 deg/s；加速度计按 ±8 g 量程的 4096 LSB/g 转为 g。
    返回 float32 数组，时间和额外空间复杂度均为 O(6N)。Python 与 ESP32 必须使用
    相同通道顺序和比例常量，否则 297 维特征及标准化参数全部失配。
    """
    # data 复制/转换为 float32，既匹配后续特征精度，也接近 ESP32 单精度行为。
    data = np.asarray(raw, dtype=np.float32)
    # np.loadtxt 读取仅一行时返回 [列数]，统一提升为 [1,列数] 二维形状。
    if data.ndim == 1:
        # 第一维固定为采样点数 1，第二维保留原文件列数。
        data = data.reshape(1, -1)
    # 至少需要六列才能形成 gx、gy、gz、ax、ay、az 完整手腕 IMU 样本。
    if data.shape[1] < 6:
        # 报错包含实际列数，防止通道缺失后仍以错误列位继续训练。
        raise ValueError(f"Expected at least 6 columns, got {data.shape[1]}")
    # converted 仅复制前六列，形状 [N,6]，额外时间戳或状态列不进入模型。
    converted = data[:, :6].astype(np.float32, copy=True)
    # 前三列原始陀螺计数除以 16.4，转换为 gx、gy、gz，单位 deg/s。
    converted[:, 0:3] = converted[:, 0:3] / 16.4
    # 后三列原始加速度计数除以 4096，转换为 ax、ay、az，单位 g。
    converted[:, 3:6] = converted[:, 3:6] / 4096.0
    # 返回按固定六轴顺序排列的 float32 工程单位数组 [N,6]。
    return converted


def load_imu_file(path: Path) -> np.ndarray:
    """读取一个逗号分隔 IMU 文本，并返回 ``[N,6]`` 工程单位数组。"""
    # raw 只读取前六列原始计数；形状通常为 [采样点数,6]，dtype 为 float32。
    raw = np.loadtxt(
        path,
        delimiter=",",
        dtype=np.float32,
        usecols=tuple(range(6)),
    )
    # 调用统一单位转换，返回 gx、gy、gz(deg/s)、ax、ay、az(g) 的 [N,6] 数组。
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
    """把窗口秒数和固定步长秒数换算为采样点数。"""
    # window_len 是单个特征窗口采样点数；25 Hz 下 4 秒对应 100 点。
    window_len = int(round(window_seconds * SAMPLE_RATE))
    # step_len 是相邻窗口起点间隔；由 STEP_SECONDS 与采样率共同决定。
    step_len = int(round(STEP_SECONDS * SAMPLE_RATE))
    # 返回“窗口点数、至少为 1 的步长点数”，防止极小步长四舍五入为 0。
    return window_len, max(1, step_len)


def iter_windows(data: np.ndarray, window_len: int, step_len: int) -> Iterable[np.ndarray]:
    """按固定点数和步长生成 ``[window_len,6]`` 六轴滑动窗口视图。"""
    # 记录短于一个完整窗口时不补零、不产生样本，避免边界填充值污染特征。
    if len(data) < window_len:
        # 生成器直接结束；调用方遍历结果为空。
        return
    # 从 0 到最后一个完整窗口起点按 step_len 遍历，每轮产出一个等长窗口。
    for start in range(0, len(data) - window_len + 1, step_len):
        # yield 返回 [window_len,6] 视图，通道仍为 gx、gy、gz、ax、ay、az。
        yield data[start : start + window_len]


def motion_score(window: np.ndarray) -> float:
    """计算窗口整体活动分数 ``std(|a|)+std(|gyr|)/200``。"""
    # gyro_mag 形状 [N]，是 gx、gy、gz 的欧氏模，单位 deg/s。
    gyro_mag = np.linalg.norm(window[:, 0:3], axis=1)
    # acc_mag 形状 [N]，是 ax、ay、az 的欧氏模，单位 g。
    acc_mag = np.linalg.norm(window[:, 3:6], axis=1)
    # 加速度模标准差与归一化角速度模标准差相加，返回无量纲近似活动强度。
    return float(np.std(acc_mag) + np.std(gyro_mag) / 200.0)


def instantaneous_motion(window: np.ndarray) -> np.ndarray:
    """计算逐点活动强度 ``|a[t]-a[t-1]|+|gyr[t]|/200``，输出形状 ``[N]``。"""
    # data 统一为 float32 的 [N,6]，列顺序为 gx、gy、gz、ax、ay、az。
    data = np.asarray(window, dtype=np.float32)
    # gyro_mag 形状 [N]、单位 deg/s，表示每点三轴合成角速度。
    gyro_mag = np.linalg.norm(data[:, 0:3], axis=1)
    # acc_delta 形状 [N-1]、单位 g，表示相邻采样间三轴加速度向量变化。
    acc_delta = np.linalg.norm(np.diff(data[:, 3:6], axis=0), axis=1)
    # 首点没有前一采样，前置 0 后恢复 [N]，并统一为 float32。
    acc_delta = np.concatenate([np.zeros(1, dtype=np.float32), acc_delta.astype(np.float32)])
    # 角速度除以 200 后与加速度变化相加，返回 float32 无量纲活动序列 [N]。
    return (acc_delta + gyro_mag / 200.0).astype(np.float32)


def active_ratio(window: np.ndarray, active_point_threshold: float) -> float:
    """返回窗口中逐点活动强度超过阈值的采样比例，范围为 ``[0,1]``。"""
    # scores 形状 [N]，每项由相邻加速度变化和当前角速度共同构成。
    scores = instantaneous_motion(window)
    # 布尔均值等于活动点比例；用于拒绝只含单次冲击的伪动态窗口。
    return float(np.mean(scores > active_point_threshold))


def keep_window_for_label(
    window: np.ndarray,
    label: str,
    rest_threshold: float,
    active_point_threshold: float,
) -> bool:
    """按类别运动强度规则决定一个 ``[N,6]`` 窗口是否进入样本集。"""
    # 先清除单轴孤立毛刺，再计算运动强度，防止一个坏点把静止窗口误判成有效动作。
    cleaned = preprocess_imu_window(window)
    # score 是清洗窗口的整体活动分数，供静坐和动态动作使用同一门槛体系。
    score = motion_score(cleaned)
    # 静坐允许小幅手腕自然抖动，但拒绝明显运动窗口以降低标签噪声。
    if label == SIT_CLASS_NAME:
        # 1.6 倍静坐阈值提供自然波动余量，返回 True 表示保留该窗口。
        return score <= rest_threshold * 1.6
    # 非静坐动作低于静坐基线阈值时视为首尾静止段或无效采集。
    if score < rest_threshold:
        # 返回 False，阻止静止窗口被当作目标动作训练。
        return False
    # 四种高动态跳跃类还需同时具备更高整体强度和足够持续的活动点比例。
    if label in HIGH_DYNAMIC_CLASSES:
        # 两个条件抑制单点冲击：分数至少 1.25 倍基线，活动点比例至少 20%。
        return (
            score >= rest_threshold * 1.25
            and active_ratio(cleaned, active_point_threshold) >= MOTION_TRIGGER_RATIO
        )
    # 其他非静坐动作只要超过静坐阈值即可保留，避免弱动作被过度过滤。
    return True


def file_balanced_sample_weights(labels: np.ndarray, file_ids: np.ndarray) -> np.ndarray:
    """生成类别等权、类内文件等权、文件内窗口等权的采样权重。

    每个窗口权重为 ``1/(当前类文件数*当前类当前文件窗口数)``。因此每个类别总权重为 1，
    且同类各原始文件总权重相同，长文件切出更多窗口也不会主导训练。
    """
    # y 是形状 [样本数] 的 int64 类别索引数组。
    y = np.asarray(labels, dtype=np.int64)
    # groups 是形状 [样本数] 的 int64 原始文件编号数组。
    groups = np.asarray(file_ids, dtype=np.int64)
    # 标签和文件编号必须逐样本一一对应，否则权重会分配给错误窗口。
    if y.shape != groups.shape:
        # 报错同时给出两个形状，便于定位样本构建阶段的数据错位。
        raise ValueError(f"labels and file_ids must share shape, got {y.shape} and {groups.shape}")
    # 空样本集没有可分配权重，返回同为一维的空 float64 数组。
    if len(y) == 0:
        # float64 满足 PyTorch WeightedRandomSampler 对权重精度的默认要求。
        return np.empty(0, dtype=np.float64)

    # pair_counts 统计每个“类别索引、文件编号”组合切出的窗口数。
    pair_counts: Dict[Tuple[int, int], int] = {}
    # class_files 保存每个类别包含的唯一文件编号集合。
    class_files: Dict[int, set[int]] = {}
    # 同步遍历每个样本标签和文件编号；每轮更新组合计数及类别文件集合。
    for label, file_id in zip(y.tolist(), groups.tolist()):
        # pair 唯一标识当前样本所属类别及原始采集文件。
        pair = (label, file_id)
        # 当前组合窗口数加一；首次出现时从 0 开始。
        pair_counts[pair] = pair_counts.get(pair, 0) + 1
        # 把当前文件编号加入该类别集合，set 自动去除重复窗口。
        class_files.setdefault(label, set()).add(file_id)
    # 按原样本顺序生成 float64 权重数组，形状为 [样本数]。
    return np.asarray(
        [
            # 类内先平均到文件，再平均到该文件的窗口，使每类总权重均为 1。
            1.0 / (len(class_files[label]) * pair_counts[(label, file_id)])
            # 逐样本读取标签和文件编号，列表顺序与输入数组完全一致。
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
    """用训练集静坐窗口活动分数的指定百分位估计整体静止阈值。"""
    # scores 收集所有静坐完整窗口的无量纲 motion_score。
    scores: List[float] = []
    # 遍历训练文件级记录；每轮只处理标签为 sit 的原始采集。
    for record in records:
        # 非静坐记录不能用于定义传感器静止噪声基线，直接跳过。
        if record.label != SIT_CLASS_NAME:
            # 继续检查下一文件，不读取当前动态动作数据。
            continue
        # data 形状 [N,6]，列为 gx、gy、gz(deg/s)、ax、ay、az(g)。
        data = load_imu_file(record.path)
        # 静坐阈值也基于清洗窗口估计，保证训练筛选与 ESP32 实时特征入口使用相同信号定义。
        scores.extend(
            motion_score(preprocess_imu_window(window))
            for window in iter_windows(data, window_len, step_len)
        )
    # 没有静坐窗口时使用保守默认值，保证小型测试数据仍可构建样本。
    if not scores:
        # 0.03 是无量纲整体活动分数默认下限，不依赖不存在的统计量。
        return 0.03
    # threshold 是静坐分数指定百分位，float32 统计与后续特征精度一致。
    threshold = float(np.percentile(np.asarray(scores, dtype=np.float32), percentile))
    # 至少返回 0.01，防止近乎常量静坐记录让阈值过低并误删正常动作。
    return max(threshold, 0.01)


def estimate_active_point_threshold(
    records: Sequence[ImuRecord],
    window_len: int,
    step_len: int,
    percentile: float = 90.0,
) -> float:
    """用静坐逐点活动强度百分位估计连续活动点判定阈值。"""
    # scores 收集全部静坐窗口每个采样点的无量纲 instantaneous_motion。
    scores: List[float] = []
    # 遍历训练记录；终止条件为所有文件检查完成，每轮只累积静坐数据。
    for record in records:
        # 动态动作会抬高逐点阈值，不能参与静止噪声估计。
        if record.label != SIT_CLASS_NAME:
            # 跳过当前动态记录，继续检查下一文件。
            continue
        # data 是工程单位 [N,6] 数组，前三列 deg/s，后三列 g。
        data = load_imu_file(record.path)
        # 遍历当前静坐记录的全部完整滑窗；每轮累积窗口内 N 个逐点分数。
        for window in iter_windows(data, window_len, step_len):
            # 逐点活动阈值排除单轴尖峰贡献，防止静坐基线被传感器毛刺抬高。
            scores.extend(
                instantaneous_motion(preprocess_imu_window(window)).tolist()
            )
    # 无静坐逐点分数时返回经验默认值，避免 percentile 对空数组报错。
    if not scores:
        # 0.02 是无量纲逐点活动阈值默认值，仅用于缺少静坐样本的兼容路径。
        return 0.02
    # threshold 是静坐逐点分数指定百分位，反映传感器噪声与自然手腕微动上界。
    threshold = float(np.percentile(np.asarray(scores, dtype=np.float32), percentile))
    # 下限 0.005 防止极低噪声记录使单精度微扰被判为持续动作。
    return max(threshold, 0.005)


def series_features(values: np.ndarray) -> List[float]:
    """提取一维序列的 8 个基础统计特征。

    返回顺序固定为均值、标准差、最小值、最大值、均方根、平均绝对一阶差分、
    去均值过零率、一阶差分标准差。输入必须是非空 ``[N]`` 序列；物理单位继承来源通道，
    过零率无量纲。时间复杂度 O(N)，额外空间 O(N)。
    """
    # x 统一为非空 float32 一维序列；单位可能为 deg/s、g 或二者构造的派生量。
    x = np.asarray(values, dtype=np.float32)
    # mean 是窗口算术均值，单位与输入序列相同。
    mean = float(np.mean(x))
    # std 是总体标准差，刻画窗口内波动强度，单位与输入相同。
    std = float(np.std(x))
    # min_v 是窗口最小采样值，用于保留负向极值信息。
    min_v = float(np.min(x))
    # max_v 是窗口最大采样值，用于保留正向极值信息。
    max_v = float(np.max(x))
    # energy 是平均平方值 E[x^2]，随后开平方得到与原序列同单位的 RMS。
    energy = float(np.mean(x * x))
    # centered 是去均值序列 [N]，用于统计围绕基线的符号变化。
    centered = x - mean
    # 至少两个采样点时才能定义相邻差分和相邻符号乘积。
    if len(x) > 1:
        # diffs 形状 [N-1]，表示相邻采样的一阶差分，单位与输入相同。
        diffs = np.diff(x)
        # abs_diffs 取一阶差分绝对值，避免正负变化相互抵消。
        abs_diffs = np.abs(diffs)
        # mean_abs_diff 是平均变化幅度，反映动作平滑度和冲击强度。
        mean_abs_diff = float(np.mean(abs_diffs))
        # std_diff 是差分总体标准差，反映相邻变化的不均匀程度。
        std_diff = float(np.std(diffs))
        # sign_product 形状 [N-1]；小于 0 表示去均值序列在相邻点间穿过零轴。
        sign_product = centered[:-1] * centered[1:]
        # zcr 是过零次数占相邻点对数的比例，范围 [0,1]、无量纲。
        zcr = float(np.mean(sign_product < 0.0))
    # 单点序列没有相邻关系，三个差分/过零特征按定义退化为 0。
    else:
        # 无一阶差分时平均绝对差分设为 0，保持输出有限。
        mean_abs_diff = 0.0
        # 无一阶差分时差分标准差设为 0。
        std_diff = 0.0
        # 无相邻点对时过零率设为 0。
        zcr = 0.0
    # 按 FEATURE_STAT_NAMES 固定顺序返回 8 个 Python float，ESP32 必须保持相同顺序。
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
    """用窗口平均加速度估计重力方向，并分解垂直/水平加速度与角速度。

    输入 ``window`` 形状为 ``[N,6]``，通道顺序 ``gx、gy、gz、ax、ay、az``；
    角速度单位 deg/s，加速度单位 g。垂直分量为向量对重力单位向量的投影，水平分量为
    ``sqrt(max(|v|^2-v_vertical^2,0))``。返回四个 ``[N]`` 序列，顺序为垂直加速度、
    水平加速度、垂直角速度、水平角速度。夹紧负数用于抵消浮点舍入误差。
    """
    # data 是 float32 的 [N,6] 六轴窗口，不改变调用方原数组。
    data = np.asarray(window, dtype=np.float32)
    # gravity 形状 [3]，由 ax、ay、az 窗口均值估计，单位 g。
    gravity = np.mean(data[:, 3:6], axis=0)
    # gravity_norm 是平均加速度向量模长，单位 g，用于归一化方向。
    gravity_norm = float(np.linalg.norm(gravity))
    # 模长小于 1e-6 g 时方向不可辨，使用传感器 z 轴作为确定性后备方向。
    if gravity_norm < 1e-6:
        # gravity_unit 为 [0,0,1] 的无量纲单位向量，避免除零和 NaN。
        gravity_unit = np.asarray([0.0, 0.0, 1.0], dtype=np.float32)
    # 平均加速度模长有效时，直接归一化得到当前窗口重力单位方向。
    else:
        # 除以模长后向量范数约为 1，并转换为 float32 对齐 ESP32 单精度计算。
        gravity_unit = (gravity / gravity_norm).astype(np.float32)

    # acc_vertical 形状 [N]，是三轴加速度在重力方向的有符号投影，单位 g。
    acc_vertical = data[:, 3:6] @ gravity_unit
    # gyro_vertical 形状 [N]，是三轴角速度在同一方向的有符号投影，单位 deg/s。
    gyro_vertical = data[:, 0:3] @ gravity_unit
    # acc_squared 形状 [N]，保存每点三轴加速度模平方，单位 g^2。
    acc_squared = np.sum(data[:, 3:6] * data[:, 3:6], axis=1)
    # gyro_squared 形状 [N]，保存每点三轴角速度模平方，单位 (deg/s)^2。
    gyro_squared = np.sum(data[:, 0:3] * data[:, 0:3], axis=1)
    # acc_horizontal 根据勾股分解求非负水平模；maximum 防止舍入产生负开方数。
    acc_horizontal = np.sqrt(np.maximum(acc_squared - acc_vertical * acc_vertical, 0.0))
    # gyro_horizontal 同理求重力正交平面的角速度模，单位 deg/s。
    gyro_horizontal = np.sqrt(np.maximum(gyro_squared - gyro_vertical * gyro_vertical, 0.0))
    # 返回四个 float32 的 [N] 序列，固定顺序与 GRAVITY_NAMES 和 ESP32 实现一致。
    return (
        acc_vertical.astype(np.float32),
        acc_horizontal.astype(np.float32),
        gyro_vertical.astype(np.float32),
        gyro_horizontal.astype(np.float32),
    )


def phase_features(values: np.ndarray) -> List[float]:
    """把非空一维序列等分为四个时间相位，并提取每相位均值、标准差和绝对峰值。

    输出长度为 ``PHASE_SEGMENTS*3=12``。分段使用整数边界，保证 Python 与 ESP32 在
    窗口长度不能整除 4 时仍得到相同采样归属。时间复杂度 O(N)，额外空间 O(1)。
    """
    # x 是非空 float32 一维序列，物理单位继承来源信号。
    x = np.asarray(values, dtype=np.float32)
    # result 按相位 0→3 依次累积“均值、标准差、绝对峰值”。
    result: List[float] = []
    # 遍历固定 PHASE_SEGMENTS 个相位；每轮处理一个连续时间片并追加三个特征。
    for phase in range(PHASE_SEGMENTS):
        # start 使用整数除法计算当前相位的包含起点，范围 [0,N)。
        start = (phase * len(x)) // PHASE_SEGMENTS
        # end 是当前相位不包含终点，最后一个相位严格结束于 N。
        end = ((phase + 1) * len(x)) // PHASE_SEGMENTS
        # segment 是 x[start:end] 的连续视图，长度由整数分段决定。
        segment = x[start:end]
        # 极短序列可能让前部相位为空，此时使用最后一个样本保持统计量有限。
        if len(segment) == 0:
            # x[-1:] 保持一维形状 [1]，避免 np.mean 空数组产生 NaN。
            segment = x[-1:]
        # 当前相位追加均值、总体标准差、绝对峰值，顺序不得与特征名表错位。
        result.extend(
            [
                float(np.mean(segment)),
                float(np.std(segment)),
                float(np.max(np.abs(segment))),
            ]
        )
    # 返回长度固定为 12 的 Python float 列表，单位与输入相同。
    return result


def normalized_phase_features(values: np.ndarray) -> List[float]:
    """先对序列做窗口内 z-score，再提取 12 个无量纲相位特征。"""
    # x 是非空 float32 一维输入，单位由来源通道决定。
    x = np.asarray(values, dtype=np.float32)
    # mean 是全窗口均值，用于消除佩戴姿态和传感器直流偏置。
    mean = float(np.mean(x))
    # std 是全窗口总体标准差，用于消除动作幅度尺度差异。
    std = float(np.std(x))
    # 标准差大于 1e-6 时计算 z=(x-mean)/std；近常量序列改用全零避免除零。
    normalized = (x - mean) / std if std > 1e-6 else np.zeros_like(x)
    # 返回四相位各三个无量纲特征，长度固定为 12。
    return phase_features(normalized)


def impact_distribution_features(values: np.ndarray) -> List[float]:
    """提取冲击序列的 5 个分位数、偏度、超额峰度和最大相邻跳变。

    分位数用最近秩索引 ``floor(q*(N-1)+0.5)``，便于 ESP32 无插值复现；偏度和峰度
    基于总体标准差归一化。输出长度 8，时间复杂度由排序主导为 O(N log N)。
    """
    # x 是非空 float32 一维冲击相关序列，单位继承来源信号。
    x = np.asarray(values, dtype=np.float32)
    # ordered 是升序副本 [N]，用于确定性最近秩分位数。
    ordered = np.sort(x)
    # quantiles 依次保存 10%、25%、50%、75%、90% 五个分位值。
    quantiles = []
    # 遍历五个固定分位比例；每轮选择一个最近秩样本并追加结果。
    for fraction in (0.10, 0.25, 0.50, 0.75, 0.90):
        # index 将 [0,1] 比例映射到 [0,N-1]，加 0.5 后向最近整数舍入。
        index = int(math.floor(fraction * (len(ordered) - 1) + 0.5))
        # 追加排序数组对应值，单位与输入序列一致。
        quantiles.append(float(ordered[index]))
    # mean 是序列总体均值，用于计算标准化三阶和四阶矩。
    mean = float(np.mean(x))
    # std 是总体标准差，作为偏度和峰度分母。
    std = float(np.std(x))
    # 标准差有效时才计算高阶标准化矩，避免近常量序列数值爆炸。
    if std > 1e-6:
        # normalized 为无量纲 z-score 序列 [N]。
        normalized = (x - mean) / std
        # skew=E[z^3]，正负号反映冲击分布向高值或低值方向拖尾。
        skew = float(np.mean(normalized**3))
        # excess_kurtosis=E[z^4]-3，0 对应高斯参考峰度。
        excess_kurtosis = float(np.mean(normalized**4) - 3.0)
    # 近常量序列没有可靠高阶矩，将二者置 0 保证有限输出。
    else:
        # 偏度退化为 0，表示不提供分布不对称证据。
        skew = 0.0
        # 超额峰度退化为 0，避免除以极小标准差。
        excess_kurtosis = 0.0
    # 至少两点时取最大绝对一阶差分；单点序列没有相邻跳变，返回 0。
    max_abs_diff = float(np.max(np.abs(np.diff(x)))) if len(x) > 1 else 0.0
    # 返回固定 8 维列表：五分位数、偏度、超额峰度、最大相邻跳变。
    return quantiles + [skew, excess_kurtosis, max_abs_diff]


def temporal_features(values: np.ndarray) -> List[float]:
    """提取活动占比、峰密度、主频、谱熵和自相关周期共 6 个时序特征。

    输入为非空 ``[N]`` 序列，采样率固定 25 Hz。频率输出单位 Hz，自相关滞后输出秒，
    其余特征无量纲。FFT 复杂度 O(N log N)，滞后扫描最坏 O(N^2)；仅离线提取及
    对应 ESP32 固定短窗计算，不改变 BP 参数量。
    """
    # x 是 float32 一维序列，物理单位由来源信号决定。
    x = np.asarray(values, dtype=np.float32)
    # centered 去除直流均值，形状 [N]，供峰值、频谱和自相关共同使用。
    centered = x - float(np.mean(x))
    # activity 是去均值幅值 |x-mean|，单位与输入相同。
    activity = np.abs(centered)
    # std 是窗口总体标准差，作为显著活动点和峰值的自适应阈值。
    std = float(np.std(x))
    # 标准差有效时统计 |x-mean|>std 的点比例；常量序列活动占比定义为 0。
    high_activity_ratio = float(np.mean(activity > std)) if std > 1e-6 else 0.0

    # 至少三点且存在变化时才能通过左右邻点判断局部峰。
    if len(x) > 2 and std > 1e-6:
        # peaks 形状 [N-2]，标记高于一倍标准差且不小于右邻点的局部活动峰。
        peaks = (
            (activity[1:-1] > activity[:-2])
            & (activity[1:-1] >= activity[2:])
            & (activity[1:-1] > std)
        )
        # 峰个数除以原序列长度，得到与窗口长度弱相关的无量纲峰密度。
        peak_count_normalized = float(np.sum(peaks)) / float(len(x))
    # 极短或常量序列不存在可靠局部峰，峰密度置 0。
    else:
        # 0 表示未检测到可定义的显著活动峰。
        peak_count_normalized = 0.0

    # 对去均值序列计算实数单边 FFT；float64 降低功率和熵累加误差。
    spectrum = np.fft.rfft(centered.astype(np.float64))
    # power 形状 [floor(N/2)+1]，是各频点幅值平方，单位为输入单位平方。
    power = np.asarray(np.abs(spectrum) ** 2, dtype=np.float64)
    # 频谱非空时清除 0 Hz 直流项，避免残余均值影响主频和谱熵。
    if len(power):
        # 原地把直流功率设为 0，其他频点不变。
        power[0] = 0.0
    # power_sum 是全部非直流功率和，作为频率概率分布归一化分母。
    power_sum = float(np.sum(power))
    # 总功率有效且至少含一个非直流频点时计算主频和归一化谱熵。
    if power_sum > 1e-12 and len(power) > 1:
        # dominant_bin 是最大非直流功率所在 FFT 索引。
        dominant_bin = int(np.argmax(power))
        # 频点间隔为 SAMPLE_RATE/N，主频单位 Hz，范围 [0,Nyquist]。
        dominant_frequency_hz = dominant_bin * SAMPLE_RATE / float(len(x))
        # probabilities 对非直流频点功率归一化，和约为 1。
        probabilities = power[1:] / power_sum
        # nonzero 排除概率 0 的频点，避免计算 0*log(0) 产生 NaN。
        nonzero = probabilities > 0.0
        # Shannon 熵除以 log(频点数) 后约束到 [0,1]，衡量频谱分散程度。
        spectral_entropy = -float(
            np.sum(probabilities[nonzero] * np.log(probabilities[nonzero]))
        ) / math.log(max(len(probabilities), 2))
    # 常量、近静止或过短序列没有可解析的动态频谱，两个频域特征置 0。
    else:
        # 主频 0 Hz 表示未检测到可靠周期频率。
        dominant_frequency_hz = 0.0
        # 谱熵置 0，避免对近零功率归一化。
        spectral_entropy = 0.0

    # lag_start 把 0.15 秒换算为至少 1 点的最小自相关滞后，并夹紧到 N-1。
    lag_start = min(max(1, int(round(0.15 * SAMPLE_RATE))), max(len(x) - 1, 1))
    # lag_end 取窗口一半与 1.20 秒中较小者，限制计算量和无重叠长滞后。
    lag_end = min(len(x) // 2, int(round(1.20 * SAMPLE_RATE)))
    # autocorr_peak 默认 0，表示没有满足条件的周期相关证据。
    autocorr_peak = 0.0
    # autocorr_peak_lag_seconds 默认 0 秒，与无周期证据含义一致。
    autocorr_peak_lag_seconds = 0.0
    # 序列有波动且滞后范围非空时，扫描归一化自相关峰。
    if std > 1e-6 and lag_end >= lag_start:
        # best_correlation 初始为理论下界 -1，确保首个有效滞后可更新。
        best_correlation = -1.0
        # best_lag 初始设为最小滞后，保证即使相关都为 -1 仍有合法索引。
        best_lag = lag_start
        # 遍历闭区间 [lag_start,lag_end]；每轮比较一对错位子序列。
        for lag in range(lag_start, lag_end + 1):
            # left 是去均值序列前 N-lag 点，形状 [N-lag]。
            left = centered[:-lag]
            # right 是后移 lag 点后的 N-lag 点，与 left 一一对应。
            right = centered[lag:]
            # denominator 是两段 L2 范数乘积，用于把点积归一化到约 [-1,1]。
            denominator = math.sqrt(
                float(np.dot(left, left)) * float(np.dot(right, right))
            )
            # 分母有效时计算归一化相关；近零能量段返回 0 防止除零。
            correlation = (
                float(np.dot(left, right)) / denominator
                if denominator > 1e-12
                else 0.0
            )
            # 当前相关高于历史最佳值时更新峰值及其滞后点数。
            if correlation > best_correlation:
                # 保存无量纲最大归一化自相关值。
                best_correlation = correlation
                # 保存产生最大相关的采样点滞后。
                best_lag = lag
        # 扫描结束后把最佳相关写入返回特征。
        autocorr_peak = best_correlation
        # 滞后点数除以 25 Hz 转换为秒，便于表示动作周期。
        autocorr_peak_lag_seconds = best_lag / float(SAMPLE_RATE)
    # 返回固定 6 维时序特征，顺序必须与 TEMPORAL_FEATURE_NAMES 和 C 端一致。
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
        # 返回 float64 空数组，保持后续长度判断和数值类型确定。
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
            # 返回与窗口等长的零投影，表示没有主转动方向证据。
            return np.zeros(len(data), dtype=np.float64)
        # 归一化后进入下一次固定迭代。
        axis = next_axis / norm
    # PCA 轴整体正负任意，使用绝对值最大分量固定符号。
    anchor = int(np.argmax(np.abs(axis)))
    # 锚点为负时翻转整条轴，使 Python/C 对相同窗口得到一致符号。
    if float(axis[anchor]) < 0.0:
        # 主轴乘 -1 只固定符号，不改变 PCA 方向或投影能量。
        axis = -axis
    # 返回每个采样点沿手腕主转动方向的带符号角速度。
    return centered @ axis


def _wrist_reversal_rate_hz(window: np.ndarray) -> float:
    """计算手腕主角速度每秒有效换向次数，单位为 Hz。"""
    # 获得固定幂迭代主轴投影，形状为 [时间点数]。
    projection = _wrist_principal_gyro_projection(window)
    # 少于两个点无法形成换向。
    if len(projection) < 2:
        # 返回 0 Hz，表示没有可定义换向事件。
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
        # 返回 0 Hz，避免把零点附近噪声视为换向。
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
        # 返回无量纲零值，表示无可解析第二/第一峰结构。
        return 0.0
    # 去除窗口均值，使自相关描述动态周期而非直流幅值。
    centered = values - float(np.mean(values))
    # 零延迟能量作为全部延迟统一分母。
    energy = float(np.dot(centered, centered))
    # 近静止窗口无周期证据，返回零。
    if energy <= 1e-12:
        # 避免用近零能量归一化自相关产生不稳定值。
        return 0.0
    # 最小延迟为 round(0.30*25)=8 点，排除三点平滑尺度内的伪峰。
    minimum_lag = max(2, int(round(0.30 * SAMPLE_RATE)))
    # 最大延迟为 3 秒或半窗，取较小值保证足够重叠样本。
    maximum_lag = min(int(round(3.0 * SAMPLE_RATE)), len(values) // 2)
    # 无合法延迟范围时返回零。
    if maximum_lag <= minimum_lag:
        # 搜索区间为空，返回无周期证据的零值。
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
        # 返回零，避免把区间边界误当作两个时间峰。
        return 0.0
    # 内部局部峰须严格高于左点、不低于右点且为正。
    peak_offsets = np.flatnonzero(
        (correlations[1:-1] > correlations[:-2])
        & (correlations[1:-1] >= correlations[2:])
        & (correlations[1:-1] > 0.0)
    ) + 1
    # 少于两个时间峰时没有第二/第一峰结构。
    if len(peak_offsets) < 2:
        # 返回无量纲零值，表示未观察到两次周期重复。
        return 0.0
    # 第一峰按时间最早而非幅值最大定义。
    first_peak = max(float(correlations[int(peak_offsets[0])]), 0.0)
    # 第一峰过小时不执行除法。
    if first_peak <= 1e-12:
        # 返回零，防止第二峰除以近零第一峰造成数值爆炸。
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
        # 六个零依次对应末六项弱类特征，维度合同保持不变。
        return [0.0] * 6
    # half_length 取窗口整半长度；奇数窗口中间点不参与前后形状比较。
    half_length = len(data) // 2
    # 前半段表示一次手腕外摆过程，单位仍为 deg/s。
    first_half = gyro_values[:half_length]
    # 后半段倒序后表示回摆轨迹按相同时间方向展开。
    reversed_second_half = gyro_values[-half_length:][::-1]
    # 少于两个点或近常量波形无法定义皮尔逊相关，确定性返回零。
    if half_length < 2:
        # 前后半窗形态相关置 0，表示没有足够时间结构证据。
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
        # 首点保持 0，其余 N-1 点写入 g/s 单位 jerk 近似值。
        jerk[1:] = np.abs(np.diff(acc_magnitude)) * float(SAMPLE_RATE)
    # 三点平滑使用边缘复制，保持输出长度并抑制单点量化尖峰。
    if len(jerk) >= 3:
        # 计算三点移动平均 jerk，输出长度仍为 N。
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
            # 当前连续点达到半高，半高宽点数加一。
            half_width_points += 1
        else:
            # 首次低于半高即结束连续宽度扫描，后续回升不计入同一冲击。
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
            # 记录首次持续恢复位置，用于归一化恢复时间。
            recovery_index = index
            # 已找到最早满足条件位置，终止后续扫描。
            break
    # 以事件后剩余窗口长度归一化恢复时间，正常范围为 [0,1]。
    recovery_time_ratio = (recovery_index - event_index) / float(max(len(data) - 1 - event_index, 1))
    # 周期峰检测先进行与分析脚本一致的三点边缘平滑。
    if len(gyro_values) >= 3:
        # 计算三点移动平均角速度模，输出长度保持 N。
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
            # 追加当前峰索引，后续较弱近邻峰将被间距条件拒绝。
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
        # 将 0 Hz 功率设为零，使主峰和谐波只来自动态成分。
        power[0] = 0.0
    # 近静止或频谱不足三个点时，二次谐波比定义为零。
    if len(power) < 3 or float(np.sum(power)) <= 1e-12:
        # 返回无量纲零值，表示没有可解析基频/二次谐波结构。
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
    """提取 33 项弱类特征；字典中每个输入序列形状为 ``[时间点数]``。"""
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
    """从 ``[N,6]`` 清洗窗口构造后续特征函数复用的一维信号字典。"""
    # data 为 float32 的 [N,6]；固定通道顺序是 gx、gy、gz、ax、ay、az。
    data = np.asarray(window, dtype=np.float32)
    # series 先保存六个原始轴视图；陀螺仪单位 deg/s，加速度计单位 g。
    series: Dict[str, np.ndarray] = {
        name: data[:, axis] for axis, name in enumerate(CHANNEL_NAMES)
    }
    # gyro_mag 形状 [N]，是三轴角速度欧氏模，单位 deg/s，降低佩戴方向影响。
    series["gyro_mag"] = np.linalg.norm(data[:, 0:3], axis=1)
    # acc_mag 形状 [N]，是三轴加速度欧氏模，单位 g，保留冲击和腾空强度。
    series["acc_mag"] = np.linalg.norm(data[:, 3:6], axis=1)
    # gyro_delta_mag 形状 [N-1]，表示相邻角速度向量变化模，单位 deg/s。
    series["gyro_delta_mag"] = np.linalg.norm(np.diff(data[:, 0:3], axis=0), axis=1)
    # acc_delta_mag 形状 [N-1]，表示相邻加速度向量变化模，单位 g。
    series["acc_delta_mag"] = np.linalg.norm(np.diff(data[:, 3:6], axis=0), axis=1)
    # 遍历四个重力对齐名称及序列；每轮把 [N] 投影结果写入同名字典项。
    for name, values in zip(GRAVITY_NAMES, gravity_aligned_series(data)):
        # 保存垂直/水平加速度和角速度，顺序由 GRAVITY_NAMES 固定。
        series[name] = values
    # 返回所有基础序列；各特征组只读取字典，不重复计算模长和重力投影。
    return series


def extract_features(window: np.ndarray) -> np.ndarray:
    """把一个六轴窗口转换为与 ESP32 完全同序的 297 维 float32 特征。

    输入形状 ``[N,6]``，通道 ``gx、gy、gz、ax、ay、az``，单位分别为 deg/s 和 g。
    输出形状 ``[297]``，依次包含 112 维全局统计、48 维相位、24 维时序、48 维
    归一化相位、32 维冲击分布和 33 维弱类机制特征。具体公式见
    ``docs/算法文档.md``；特征名和 C 端顺序不得独立修改。
    """
    # 所有特征统一从清洗后的 [N,6] 手腕 IMU 窗口提取，确保统计量和冲击特征不受孤立毛刺支配。
    data = preprocess_imu_window(window)

    # series 缓存 14 个原始轴、模长、差分模和重力投影一维序列。
    series = build_feature_series(data)
    # features 按模型输入合同逐组追加，最终长度固定为 297。
    features: List[float] = []
    # 遍历 14 个全局序列；每轮追加 8 个基础统计量，共 112 维。
    for source in GLOBAL_SERIES_NAMES:
        # 追加顺序与 ONE_SERIES_FEATURES 及 C 端全局统计输出一致。
        features.extend(series_features(series[source]))
    # 遍历 4 个关键相位来源；每轮追加 4 相位×3 统计量，共 48 维。
    for source in PHASE_SOURCE_NAMES:
        # 相位边界使用整数分段，确保 Python/C 对非整除窗口一致。
        features.extend(phase_features(series[source]))
    # 遍历 4 个关键来源；每轮追加 6 个频域/周期特征，共 24 维。
    for source in PHASE_SOURCE_NAMES:
        # 时序特征顺序由 TEMPORAL_FEATURES 固定。
        features.extend(temporal_features(series[source]))
    # 遍历相同 4 个来源；每轮追加 12 个窗口内标准化相位特征，共 48 维。
    for source in PHASE_SOURCE_NAMES:
        # 归一化相位主要保留动作形态，降低绝对幅度和佩戴差异影响。
        features.extend(normalized_phase_features(series[source]))
    # 遍历相同 4 个来源；每轮追加 8 个分布/冲击特征，共 32 维。
    for source in PHASE_SOURCE_NAMES:
        # 分位数使用最近秩算法，必须与 ESP32 不插值实现一致。
        features.extend(impact_distribution_features(series[source]))
    # 追加 33 个针对跳跃弓步、跳跃深蹲、收腹跳等弱类的手腕机制特征。
    features.extend(weak_class_features(series))
    # 返回连续 float32 数组 [297]；标准化和 BP 分支均依赖此固定列顺序。
    return np.asarray(features, dtype=np.float32)


def build_feature_names() -> List[str]:
    """按 ``extract_features`` 的追加规则生成 297 个稳定特征名称。"""
    # names 按模型输入列号从 0 到 296 累积，导出时同时作为 Python/C 合同。
    names: List[str] = []
    # 外层遍历 14 个全局序列，顺序与 extract_features 第一组一致。
    for source in GLOBAL_SERIES_NAMES:
        # 内层遍历 8 个基础统计名称，每轮生成一个“来源_统计量”名称。
        for feature in ONE_SERIES_FEATURES:
            # 追加一个全局统计特征名，共形成前 112 个名称。
            names.append(f"{source}_{feature}")
    # 遍历 4 个相位来源，为每个来源生成 12 个原始相位名称。
    for source in PHASE_SOURCE_NAMES:
        # phase 从 0 到 3，对应窗口由早到晚的四个连续时间片。
        for phase in range(PHASE_SEGMENTS):
            # 每个相位按均值、标准差、绝对最大值固定顺序命名。
            for feature in PHASE_FEATURES:
                # 追加当前来源、相位编号和统计量组成的唯一名称。
                names.append(f"{source}_phase{phase}_{feature}")
    # 遍历 4 个来源，为每个来源生成 6 个频域及自相关特征名。
    for source in PHASE_SOURCE_NAMES:
        # 内层按 temporal_features 返回顺序遍历名称。
        for feature in TEMPORAL_FEATURES:
            # 追加一个“来源_时序指标”名称，共 24 个。
            names.append(f"{source}_{feature}")
    # 遍历 4 个来源，为窗口内 z-score 后的相位统计命名。
    for source in PHASE_SOURCE_NAMES:
        # phase 从 0 到 3，时间顺序不变。
        for phase in range(PHASE_SEGMENTS):
            # 每个标准化相位仍含均值、标准差、绝对最大值三项。
            for feature in PHASE_FEATURES:
                # normalized_phase 前缀区分原始单位相位特征，共追加 48 项。
                names.append(f"{source}_normalized_phase{phase}_{feature}")
    # 遍历 4 个来源，生成分位数、偏度、峰度和最大跳变名称。
    for source in PHASE_SOURCE_NAMES:
        # 内层顺序必须与 impact_distribution_features 的 8 项返回一致。
        for feature in IMPACT_DISTRIBUTION_FEATURES:
            # 追加一个冲击分布特征名，共 32 项。
            names.append(f"{source}_{feature}")
    # 在末尾追加 33 个弱类机制特征名称，与 weak_class_features 返回顺序一一对应。
    names.extend(WEAK_CLASS_FEATURE_NAMES)
    # 返回长度 297 的名称列表；索引即标准化数组和模型权重输入列索引。
    return names


def build_jump_shape_feature_indices(feature_names: Sequence[str]) -> List[int]:
    """返回跳跃家族专家使用的尺度不变形态及弱类机制特征列索引。"""
    # weak_feature_set 保存 33 项经动作机理或文件级证据设计的弱类特征名称。
    weak_feature_set = set(WEAK_CLASS_FEATURE_NAMES)
    # invariant_suffixes 列出对绝对幅度较不敏感的通用统计名称后缀。
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
    # 返回保持原 297 维顺序的索引列表，专家输入列可由该列表稳定切片。
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
    """按原始文件分层划分训练、验证、测试集，禁止同一文件窗口跨集合泄漏。"""
    # labels 与 records 等长，保存每个原始文件的动作类别整数索引。
    labels = [record.label_idx for record in records]
    # 第一次分层划分产生 70% 训练文件和 30% 临时文件；随机种子固定可复现。
    train_records, temp_records = train_test_split(
        list(records),
        train_size=TRAIN_RATIO,
        random_state=seed,
        stratify=labels,
    )
    # temp_labels 保存临时文件类别，用于第二次继续分层。
    temp_labels = [record.label_idx for record in temp_records]
    # val_fraction_of_temp=0.5，使临时集等分为 15% 验证和 15% 测试。
    val_fraction_of_temp = VAL_RATIO / (VAL_RATIO + TEST_RATIO)
    # 第二次分层划分临时文件；seed+1 与第一次随机流独立但仍可复现。
    val_records, test_records = train_test_split(
        temp_records,
        train_size=val_fraction_of_temp,
        random_state=seed + 1,
        stratify=temp_labels,
    )
    # 返回三个新列表；划分单位是 ImuRecord 文件，不是高度相关的滑动窗口。
    return list(train_records), list(val_records), list(test_records)


def split_records_for_experiment(
    base_records: Sequence[ImuRecord],
    extra_train_records: Sequence[ImuRecord] = (),
    seed: int = SEED,
) -> Tuple[List[ImuRecord], List[ImuRecord], List[ImuRecord]]:
    """划分基础数据后，仅把无重复的附加记录并入训练集。"""
    # 基础数据严格按文件分层切为训练/验证/测试三部分。
    train_records, val_records, test_records = split_records_by_file(
        base_records,
        seed,
    )
    # base_paths 保存解析后的基础文件绝对路径，用于检测跨数据源重复。
    base_paths = {record.path.resolve() for record in base_records}
    # extra_paths 按附加记录顺序保存绝对路径，后续同时检测跨集和内部重复。
    extra_paths = [record.path.resolve() for record in extra_train_records]
    # duplicate_paths 是附加训练集与基础数据集的交集；任何重复都会造成采集泄漏。
    duplicate_paths = base_paths.intersection(extra_paths)
    # 发现跨数据源重复时立即拒绝实验，避免相同文件同时出现在训练和验证/测试候选中。
    if duplicate_paths:
        # 报错按路径排序列出全部重复文件，便于清理数据清单。
        raise ValueError(
            "Extra training records duplicate base dataset paths: "
            + ", ".join(str(path) for path in sorted(duplicate_paths))
        )
    # 附加路径列表长度与 set 长度不同表示附加训练清单内部重复。
    if len(extra_paths) != len(set(extra_paths)):
        # 重复附加文件会被多次切窗并扭曲样本权重，因此拒绝继续。
        raise ValueError("Extra training records contain duplicate paths")
    # 仅训练集追加外部记录；验证和测试仍完全来自基础数据划分。
    return train_records + list(extra_train_records), val_records, test_records


def euler_rotation_matrix(rx: float, ry: float, rz: float) -> np.ndarray:
    """生成按 ``Rz @ Ry @ Rx`` 组合的 3×3 右手系欧拉旋转矩阵。

    ``rx、ry、rz`` 单位为弧度；返回 float32 正交矩阵，用于对角速度和加速度向量施加
    相同小角度旋转，模拟手腕 IMU 佩戴姿态差异。
    """
    # sx、cx 分别是绕 x 轴角 rx 的正弦和余弦。
    sx, cx = math.sin(rx), math.cos(rx)
    # sy、cy 分别是绕 y 轴角 ry 的正弦和余弦。
    sy, cy = math.sin(ry), math.cos(ry)
    # sz、cz 分别是绕 z 轴角 rz 的正弦和余弦。
    sz, cz = math.sin(rz), math.cos(rz)
    # rotation_x 是绕 x 轴旋转的 float32 3×3 矩阵。
    rotation_x = np.asarray(
        [[1.0, 0.0, 0.0], [0.0, cx, -sx], [0.0, sx, cx]], dtype=np.float32
    )
    # rotation_y 是绕 y 轴旋转的 float32 3×3 矩阵。
    rotation_y = np.asarray(
        [[cy, 0.0, sy], [0.0, 1.0, 0.0], [-sy, 0.0, cy]], dtype=np.float32
    )
    # rotation_z 是绕 z 轴旋转的 float32 3×3 矩阵。
    rotation_z = np.asarray(
        [[cz, -sz, 0.0], [sz, cz, 0.0], [0.0, 0.0, 1.0]], dtype=np.float32
    )
    # 按先 x、后 y、再 z 的列向量约定组合并返回 float32 矩阵。
    return (rotation_z @ rotation_y @ rotation_x).astype(np.float32)


def rotate_imu_window(window: np.ndarray, rotation: np.ndarray) -> np.ndarray:
    """用同一 3×3 旋转矩阵变换窗口中的角速度和加速度三维向量。"""
    # data 是 float32 的 [N,6] 输入，前三列 deg/s，后三列 g。
    data = np.asarray(window, dtype=np.float32)
    # matrix 转为 float32，保证增强输出精度与模型输入一致。
    matrix = np.asarray(rotation, dtype=np.float32)
    # 旋转矩阵必须严格为 [3,3]，否则无法与三轴行向量相乘。
    if matrix.shape != (3, 3):
        # 报错包含实际形状，避免广播产生难以察觉的通道错误。
        raise ValueError(f"Expected rotation shape (3, 3), got {matrix.shape}")
    # rotated 是独立 [N,6] 副本，增强不修改原始窗口。
    rotated = data.copy()
    # 角速度行向量右乘 matrix.T，输出仍是 gx、gy、gz，单位 deg/s。
    rotated[:, 0:3] = data[:, 0:3] @ matrix.T
    # 加速度使用完全相同旋转，保持两种传感器在同一虚拟坐标系中，单位 g。
    rotated[:, 3:6] = data[:, 3:6] @ matrix.T
    # 返回形状不变的 float32 增强窗口 [N,6]。
    return rotated


def time_warp_window(
    window: np.ndarray,
    rng: np.random.Generator,
    max_displacement: float = 0.03,
) -> np.ndarray:
    """用单调正弦时间位移重采样六轴窗口，模拟约 ±3% 动作速度变化。"""
    # data 是 float32 的 [N,6] 六轴窗口，通道顺序和单位保持不变。
    data = np.asarray(window, dtype=np.float32)
    # 少于三点无法稳定插值，非正最大位移表示关闭时间扭曲。
    if len(data) < 3 or max_displacement <= 0.0:
        # 返回独立副本，保证增强接口从不与输入共享可写存储。
        return data.copy()
    # timeline 是从 0 到 1 的等间隔原始归一化时间轴，形状 [N]。
    timeline = np.linspace(0.0, 1.0, len(data), dtype=np.float64)
    # phase 在 [0,2π) 均匀采样，随机改变局部加速/减速出现位置。
    phase = float(rng.uniform(0.0, 2.0 * math.pi))
    # amplitude 在最大位移的 25% 到 100% 之间采样，避免每次增强强度固定。
    amplitude = float(rng.uniform(0.25, 1.0)) * max_displacement
    # displacement 两端由 sin(πt) 强制为 0，中部产生平滑正负时间偏移。
    displacement = amplitude * np.sin(math.pi * timeline) * np.sin(
        2.0 * math.pi * timeline + phase
    )
    # source_timeline 是每个输出时刻对应的原始采样位置，并夹紧到 [0,1]。
    source_timeline = np.clip(timeline + displacement, 0.0, 1.0)
    # 累积最大值保证采样位置单调不回退，避免时间顺序反转。
    source_timeline = np.maximum.accumulate(source_timeline)
    # 固定首点映射到原始首点，保持窗口起始边界。
    source_timeline[0] = 0.0
    # 固定末点映射到原始末点，保持窗口终止边界。
    source_timeline[-1] = 1.0
    # warped 预分配与输入同形状、同 dtype 的 [N,6] 输出数组。
    warped = np.empty_like(data)
    # 遍历六个 IMU 通道；每轮独立线性插值但共享同一时间映射。
    for axis in range(data.shape[1]):
        # np.interp 产生 float64 后转回 float32，写入当前通道且不混合轴。
        warped[:, axis] = np.interp(source_timeline, timeline, data[:, axis]).astype(
            np.float32
        )
    # 返回时间长度、六轴顺序和物理单位均不变的增强窗口。
    return warped


def augment_window(window: np.ndarray, rng: np.random.Generator) -> np.ndarray:
    """组合小角度旋转、平滑时间扭曲和传感器噪声，生成一个训练窗口增强副本。"""
    # max_angle 把允许的最大佩戴偏角从度转换为弧度。
    max_angle = math.radians(MAX_ROTATION_DEGREES)
    # angles 形状 [3]，分别为 x、y、z 轴在 [-max_angle,max_angle] 的随机角度。
    angles = rng.uniform(-max_angle, max_angle, size=3)
    # rotation 是 float32 3×3 组合旋转矩阵，三个参数单位为弧度。
    rotation = euler_rotation_matrix(float(angles[0]), float(angles[1]), float(angles[2]))
    # 对角速度和加速度施加相同坐标旋转，保持物理一致性。
    augmented = rotate_imu_window(window, rotation)
    # 使用最多 3% 平滑时间位移模拟动作速度差异，输出仍为 [N,6]。
    augmented = time_warp_window(augmented, rng, max_displacement=0.03)
    # gyro_noise 是前三轴独立零均值高斯噪声，标准差 0.25 deg/s。
    gyro_noise = rng.normal(0.0, 0.25, size=augmented[:, 0:3].shape).astype(np.float32)
    # acc_noise 是后三轴独立零均值高斯噪声，标准差 0.003 g。
    acc_noise = rng.normal(0.0, 0.003, size=augmented[:, 3:6].shape).astype(np.float32)
    # 角速度通道原地叠加对应单位噪声，不影响加速度通道。
    augmented[:, 0:3] += gyro_noise
    # 加速度通道原地叠加对应单位噪声，不影响角速度通道。
    augmented[:, 3:6] += acc_noise
    # 返回 float32 [N,6] 增强窗口，标签和原始文件编号由调用方继承。
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
    """从文件记录生成过滤、增强后的 297 维窗口样本及审计计数。

    返回 ``X:[样本数,297]``、``y:[样本数]``、``file_ids:[样本数]`` 和统计字典。
    原始文件编号随增强样本继承，用于文件平衡采样和跨文件对比损失。
    """
    # features 按生成顺序保存每个窗口的 float32 [297] 特征向量。
    features: List[np.ndarray] = []
    # labels 保存与 features 一一对应的动作类别整数索引。
    labels: List[int] = []
    # file_ids 保存与 features 一一对应的本次 records 局部文件编号。
    file_ids: List[int] = []
    # skipped 统计文件/窗口过滤和保留数量，供训练报告追踪前处理影响。
    skipped = {
        "too_short": 0,
        "rest_filtered": 0,
        "kept_windows": 0,
        "files_without_valid_window": 0,
        # motion_edge_trimmed_points 记录非静坐文件首尾被删除的采样点数，单位为点。
        "motion_edge_trimmed_points": 0,
    }

    # 遍历全部文件记录；file_id 从 0 递增，每轮读取、裁剪并切分一个原始文件。
    for file_id, record in enumerate(records):
        # 指定进度标签时，在首文件及每 10 个文件输出一次可见特征提取进度。
        if progress_label and (file_id == 0 or file_id % 10 == 0):
            # 日志包含当前文件序号、总文件数和已保留窗口数；flush 保证 PyCharm 实时显示。
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
        # 裁剪后短于完整窗口的文件无法生成固定维特征样本。
        if len(data) < window_len:
            # 过短文件计数加一，用于数据质量报告。
            skipped["too_short"] += 1
            # 结束当前文件处理，继续下一条 ImuRecord。
            continue
        # record_kept 统计当前原始文件通过过滤的原始窗口数，不计增强副本。
        record_kept = 0
        # 按固定窗口和步长遍历当前记录；每轮决定是否保留并提取特征。
        for window in iter_windows(data, window_len, step_len):
            # 不满足当前类别活动强度规则的窗口视为首尾静止或异常窗口。
            if not keep_window_for_label(
                window,
                record.label,
                rest_threshold,
                active_point_threshold,
            ):
                # 被活动规则拒绝的窗口计数加一。
                skipped["rest_filtered"] += 1
                # 跳过当前窗口，不写入特征、标签或文件编号。
                continue
            # 提取原始窗口 297 维特征并追加到样本列表。
            features.append(extract_features(window))
            # 追加当前文件动作类别索引，与刚加入的特征一一对应。
            labels.append(record.label_idx)
            # 追加当前原始文件编号，供后续文件平衡和跨文件正样本判定。
            file_ids.append(file_id)
            # 全局保留样本数加一，包含原始窗口和后续增强窗口。
            skipped["kept_windows"] += 1
            # 当前文件有效原始窗口数加一，用于决定是否启动回退窗口策略。
            record_kept += 1
            # 只有训练集启用增强，验证/测试保持真实分布和确定性。
            if augment:
                # 为每个有效原始窗口生成固定 AUGMENT_TIMES 个随机增强副本。
                for _ in range(AUGMENT_TIMES):
                    # 先增强六轴时序，再独立提取 297 维特征并追加。
                    features.append(extract_features(augment_window(window, rng)))
                    # 增强样本继承原动作标签，不产生新类别。
                    labels.append(record.label_idx)
                    # 增强样本继承原始文件编号，防止被当成跨文件对比正样本。
                    file_ids.append(file_id)
                    # 每生成一个增强样本即更新总保留窗口计数。
                    skipped["kept_windows"] += 1
        # 当前文件所有窗口均未通过过滤时，记录数据质量事件并按类别决定是否回退。
        if record_kept == 0:
            # 无有效窗口文件数加一，便于定位阈值过严或采集失败。
            skipped["files_without_valid_window"] += 1
            # 高动态动作没有合格窗口说明缺少持续动作证据，不用弱窗口回退污染标签。
            if record.label in HIGH_DYNAMIC_CLASSES:
                # 继续下一文件，当前高动态文件不生成任何样本。
                continue
            # 非高动态类别收集裁剪后全部完整窗口，准备选一个最符合类别活动特性的回退样本。
            fallback_windows = list(iter_windows(data, window_len, step_len))
            # 至少存在一个完整窗口时才执行分数排序和回退追加。
            if fallback_windows:
                # 回退选择仍使用清洗后的运动分数，避免孤立尖峰成为“最佳”动作窗口。
                scored = [
                    (motion_score(preprocess_imu_window(window)), window)
                    for window in fallback_windows
                ]
                # 静坐回退应选择运动分数最低窗口，尽量保持安静状态。
                if record.label == SIT_CLASS_NAME:
                    # best_window 是分数最小的静坐候选，元组第一项分数不再使用。
                    _, best_window = min(scored, key=lambda item: item[0])
                # 普通非静坐动作回退选择运动分数最高窗口，保留最明显动作片段。
                else:
                    # best_window 是分数最大的候选，尽量降低静止片段标签噪声。
                    _, best_window = max(scored, key=lambda item: item[0])
                # 回退窗口提取 297 维特征，仅追加原始窗口，不做增强。
                features.append(extract_features(best_window))
                # 回退样本继承当前文件动作类别索引。
                labels.append(record.label_idx)
                # 回退样本继承当前文件编号。
                file_ids.append(file_id)
                # 总保留样本数加一，与列表长度保持一致。
                skipped["kept_windows"] += 1

    # 所有文件处理后仍无样本时无法计算标准化参数或训练模型。
    if not features:
        # 明确指出过滤后为空，便于检查窗口长度、阈值和数据单位。
        raise ValueError("No samples generated after filtering")
    # 指定进度标签时输出一次完成日志，确保 PyCharm 中可见最终样本数。
    if progress_label:
        # 最终日志把当前文件数写成总数并标记 complete=true。
        print(
            f"features {progress_label} file={len(records)}/{len(records)} "
            f"kept={skipped['kept_windows']} complete=true",
            flush=True,
        )
    # 堆叠并返回 X:[S,297]、y:[S]、file_ids:[S] 和过滤统计字典。
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
    """仅用训练集统计量执行逐特征 z-score 标准化。

    对第 i 列计算 ``z_i=(x_i-mean_i)/std_i``。三个输入形状分别为
    ``[训练样本数,297]``、``[验证样本数,297]``、``[测试样本数,297]``；返回三组
    float32 标准分及 ``mean:[297]``、``std:[297]``。验证/测试不参与统计，防止泄漏。
    """
    # mean 形状 [297]，仅由训练样本逐列求均值得到，单位继承各原始特征。
    mean = np.mean(train_x, axis=0).astype(np.float32)
    # std 形状 [297]，仅由训练样本逐列求总体标准差得到，单位同对应特征。
    std = np.std(train_x, axis=0).astype(np.float32)
    # 标准差小于 1e-6 的近常量列改用 1，避免除零；该列标准化后接近 0。
    std[std < 1e-6] = 1.0
    # 返回三组无量纲 float32 标准分以及供 ESP32 复现的训练均值和标准差。
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
    """筛出指定动作家族样本，并把全局类别索引重编码为家族局部索引。"""
    # global_indices 按 family_names 顺序查找各家族类在主模型中的输出索引。
    global_indices = [class_names.index(name) for name in family_names]
    # global_to_local 把主模型索引映射到从 0 开始的专家输出索引。
    global_to_local = {
        global_idx: local_idx for local_idx, global_idx in enumerate(global_indices)
    }
    # mask 形状 [样本数]，只保留 y 属于家族全局索引集合的样本。
    mask = np.isin(y, np.asarray(global_indices, dtype=np.int64))
    # local_y 按筛选后样本顺序把全局标签映射为 int64 家族局部标签。
    local_y = np.asarray(
        [global_to_local[int(label)] for label in np.asarray(y)[mask]],
        dtype=np.int64,
    )
    # 返回筛选后的特征、局部标签和原文件编号，三者第一维完全一致。
    return np.asarray(x)[mask], local_y, np.asarray(file_ids)[mask]


def route_family_predictions(
    primary_pred: np.ndarray,
    specialist_pred: np.ndarray,
    class_names: Sequence[str],
    family_names: Sequence[str],
) -> np.ndarray:
    """仅对主模型预测落入指定家族的样本，用专家局部预测替换全局类别。"""
    # primary 是形状 [样本数] 的主模型全局类别索引。
    primary = np.asarray(primary_pred, dtype=np.int64)
    # specialist 是同形状专家局部类别索引；非家族位置的值不会被使用。
    specialist = np.asarray(specialist_pred, dtype=np.int64)
    # 两组预测必须逐样本对齐，否则路由会把专家结果写给错误窗口。
    if primary.shape != specialist.shape:
        # 立即拒绝形状不一致输入，避免 NumPy 广播掩盖错误。
        raise ValueError("Primary and specialist predictions must have the same shape")
    # family_global 按专家输出顺序保存对应主模型全局类别索引。
    family_global = np.asarray(
        [class_names.index(name) for name in family_names], dtype=np.int64
    )
    # routed 复制主预测，非家族类别保持原值。
    routed = primary.copy()
    # mask 标记主预测属于该家族的位置，只有这些位置允许专家覆盖。
    mask = np.isin(primary, family_global)
    # 专家局部索引通过 family_global 查表转为全局索引并写回家族位置。
    routed[mask] = family_global[specialist[mask]]
    # 返回形状 [样本数] 的最终全局类别索引。
    return routed


def class_weight_tensor(labels: np.ndarray, class_count: int, device: torch.device) -> torch.Tensor:
    """按类别样本数倒数生成均值约为 1 的交叉熵权重张量。"""
    # counts 形状 [类别数]，统计每个整数标签在训练样本中的出现次数。
    counts = np.bincount(labels, minlength=class_count).astype(np.float32)
    # 缺失类别计数改为 1 以避免除零；正常完整训练集不会触发此保护。
    counts[counts == 0.0] = 1.0
    # weights_i=总样本数/(类别数*counts_i)，少数类获得更大损失权重。
    weights = counts.sum() / (class_count * counts)
    # 返回训练设备上的 float32 [类别数] 张量，供 F.cross_entropy 使用。
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
        # 缺失类别无法构造每批 P 个完整类别，停止训练并报告数据问题。
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
        """建立类别-文件-窗口三级索引，并计算每个 epoch 的 P×K 批次数。"""
        # 将标签保存为一维 int64 数组，元素表示每个窗口的类别索引。
        self.labels = np.asarray(labels, dtype=np.int64).reshape(-1)
        # 将文件编号保存为一维 int64 数组，用于避免同文件重叠窗口充当对比正样本。
        self.file_ids = np.asarray(file_ids, dtype=np.int64).reshape(-1)
        # 标签与文件编号必须逐窗口对应，否则采样出的来源约束无效。
        if self.labels.shape != self.file_ids.shape:
            # 拒绝错位元数据，避免同文件约束应用到错误样本。
            raise ValueError("labels and file_ids must have the same shape")
        # K 必须为正整数；训练配置采用 K=6，单元测试可使用更小值。
        if samples_per_class <= 0:
            # K<=0 无法形成有效批次，因此立即报告配置错误。
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
        """返回一个 epoch 产生的完整 P×K 批次数。"""
        # 返回每个 epoch 产生的 P×K 批次数，供 DataLoader 计算长度。
        return self.batch_count

    def __iter__(self) -> Iterable[List[int]]:
        """逐批生成类别均衡且优先跨文件的训练样本索引列表。"""
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
    """把 NumPy 特征、标签和可选文件编号封装为训练或推理 DataLoader。

    ``x`` 形状为 ``[样本数,特征数]``，``y`` 和 ``file_ids`` 形状为 ``[样本数]``。
    采样优先级依次为 P×K 文件平衡、普通文件平衡、常规 shuffle。
    """
    # tensors 先保存 float32 特征和 int64 标签张量，第一维均为样本数。
    tensors: List[torch.Tensor] = [torch.from_numpy(x).float(), torch.from_numpy(y).long()]
    # 训练损失需要原始文件编号时，追加第三个 int64 [样本数] 张量。
    if file_ids is not None:
        # np.asarray 统一 dtype，torch.from_numpy 不复制时仍保持整数文件编号语义。
        tensors.append(torch.from_numpy(np.asarray(file_ids, dtype=np.int64)).long())
    # TensorDataset 按样本索引同步返回 x、y 和可选 file_id，防止三者错位。
    dataset = TensorDataset(*tensors)
    # P×K 模式优先级高于普通文件加权采样，并要求提供逐窗口文件编号。
    if pk_file_balanced:
        # 缺少文件编号时无法保证同类 K 个窗口优先来自不同采集文件。
        if file_ids is None:
            # 明确拒绝缺少 file_ids 的 P×K 配置。
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
    # 普通文件平衡模式按“类-文件-窗口”反频率权重有放回采样。
    if file_balanced:
        # 文件平衡权重依赖逐窗口文件编号，缺失时不能构造正确权重。
        if file_ids is None:
            # 立即报告调用错误，避免退化为仅类别平衡的非预期行为。
            raise ValueError("file_ids are required for file-balanced sampling")
        # weights 是 double [样本数] 张量，每类、类内各文件总权重相等。
        weights = torch.as_tensor(
            file_balanced_sample_weights(y, file_ids), dtype=torch.double
        )
        # generator 是采样器独立随机源，避免受模型 dropout 随机状态影响。
        generator = torch.Generator()
        # 固定采样随机种子，使同一实验配置的数据顺序可复现。
        generator.manual_seed(seed)
        # sampler 每 epoch 有放回抽取 len(y) 个索引，长文件不会因窗口多而占优。
        sampler = WeightedRandomSampler(
            weights,
            num_samples=len(y),
            replacement=True,
            generator=generator,
        )
        # 返回按 batch_size 组批、由 sampler 决定索引顺序的 DataLoader。
        return DataLoader(dataset, batch_size=batch_size, sampler=sampler)
    # 无特殊采样时使用普通固定批大小加载器，shuffle 由调用方决定。
    return DataLoader(dataset, batch_size=batch_size, shuffle=shuffle)


def predict(model: nn.Module, x: np.ndarray, device: torch.device) -> np.ndarray:
    """批量运行模型并返回形状 ``[样本数]`` 的 argmax 类别索引。"""
    # 切换到推理模式，关闭 dropout 等仅训练时生效的随机行为。
    model.eval()
    # preds 按批保存 CPU int64 预测数组，最后沿样本维拼接。
    preds: List[np.ndarray] = []
    # 构造无打乱推理加载器；占位标签全 0，仅满足 TensorDataset 接口。
    loader = make_loader(x, np.zeros(len(x), dtype=np.int64), batch_size=512, shuffle=False)
    # 禁用梯度记录，降低评估内存和计算开销且不修改模型参数。
    with torch.no_grad():
        # 依次遍历全部推理批次；每轮处理最多 512 个标准化特征向量。
        for batch_x, _ in loader:
            # batch_x 移到目标设备，模型输出 logits 形状 [批大小,类别数]。
            logits = model(batch_x.to(device))
            # 沿类别维取最大 logit 索引并移回 CPU，追加形状 [批大小] 的预测。
            preds.append(torch.argmax(logits, dim=1).cpu().numpy())
    # 按原输入顺序拼接所有批预测，返回 int64 [样本数] 数组。
    return np.concatenate(preds)


def evaluate(model: nn.Module, x: np.ndarray, y: np.ndarray, device: torch.device) -> Tuple[float, float, np.ndarray]:
    """计算窗口级准确率、宏平均 F1，并返回逐样本预测。"""
    # y_pred 形状 [样本数]，顺序与 x、y 一致。
    y_pred = predict(model, x, device)
    # acc 是正确预测数除以样本数的窗口级准确率，范围 [0,1]。
    acc = float(accuracy_score(y, y_pred))
    # macro_f1 对各类别 F1 等权平均；缺失分母按 0 处理，突出弱类表现。
    macro_f1 = float(f1_score(y, y_pred, average="macro", zero_division=0))
    # 返回两个标量指标和 int64 [样本数] 预测，供逐类召回及混淆矩阵复用。
    return acc, macro_f1, y_pred


def weak_and_worst_f1(
    y_true: np.ndarray,
    y_pred: np.ndarray,
    class_names: Sequence[str],
) -> Tuple[float, float]:
    """返回预定义弱类平均 F1 和所有类别最小 F1。"""
    # per_class 形状 [类别数]，按 class_names 索引计算每类 F1，零分母记 0。
    per_class = f1_score(
        y_true,
        y_pred,
        labels=np.arange(len(class_names)),
        average=None,
        zero_division=0,
    )
    # weak_indices 保存当前数据集中存在的弱类输出索引。
    weak_indices = [
        class_names.index(name) for name in WEAK_CLASS_NAMES if name in class_names
    ]
    # 存在弱类时等权平均其 F1；类别子集不含弱类时退化为全类平均。
    weak_f1 = float(np.mean(per_class[weak_indices])) if weak_indices else float(np.mean(per_class))
    # worst_f1 是全部类别最低 F1；空指标数组时返回 0。
    worst_f1 = float(np.min(per_class)) if len(per_class) else 0.0
    # 返回“弱类平均 F1、最差类别 F1”两个 [0,1] 标量。
    return weak_f1, worst_f1


def per_class_recalls(
    y_true: np.ndarray,
    y_pred: np.ndarray,
    class_count: int,
) -> np.ndarray:
    """按固定类别索引计算召回率 ``TP/(TP+FN)``，输出形状 ``[类别数]``。"""
    # matrix 形状 [类别数,类别数]，行是真值、列是预测，缺失类别仍保留零行列。
    matrix = confusion_matrix(
        np.asarray(y_true),
        np.asarray(y_pred),
        labels=np.arange(class_count),
    )
    # support 是每个真值类别样本数 TP+FN，形状 [类别数]。
    support = matrix.sum(axis=1)
    # 安全逐类相除；无真值样本类别通过 out/where 返回 0 而非 NaN。
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
    """返回弱类平均召回、全类最低召回及按类别顺序的召回数组。"""
    # recalls 形状 [类别数]，顺序严格等于 class_names。
    recalls = per_class_recalls(y_true, y_pred, len(class_names))
    # weak_indices 保存本次类别表中预定义弱类的位置。
    weak_indices = [
        class_names.index(name) for name in WEAK_CLASS_NAMES if name in class_names
    ]
    # 有弱类时等权平均其召回；测试类别子集不含弱类时退化为全类平均。
    weak_recall = (
        float(np.mean(recalls[weak_indices]))
        if weak_indices
        else float(np.mean(recalls))
    )
    # min_recall 是最弱类别召回；空类别表防御性返回 0。
    min_recall = float(np.min(recalls)) if len(recalls) else 0.0
    # 返回两个标量和完整 [类别数] 召回数组，供 checkpoint 与逐 epoch 日志使用。
    return weak_recall, min_recall, recalls


def validation_checkpoint_key(
    val_min_recall: float,
    val_weak_recall: float,
    val_f1: float,
    val_acc: float,
) -> Tuple[float, float, float, float]:
    """按最小召回、弱类召回、宏 F1、准确率构造字典序 checkpoint 评分。"""
    # 元组比较先保障最弱类，再比较弱类整体，最后才比较全局 F1 和准确率。
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
    """训练主 BP 模型，逐 epoch 输出完整验证指标并按弱类优先规则早停。

    输入 ``train_x``、``val_x`` 为无量纲标准化特征 ``[样本数,297]``；标签和文件编号
    均为 ``[样本数]``。总损失为交叉熵、跨文件监督对比、困难对间隔和可选辅助头损失的
    加权和。返回载入最佳验证状态的模型和训练历史；训练期功能不增加 ESP32 推理计算。
    """
    # EMA 衰减率必须位于 [0,1)，保证旧权重和当前权重系数非负且和为 1。
    if not 0.0 <= ema_decay < 1.0:
        # 非法 EMA 配置会产生无意义权重，立即拒绝。
        raise ValueError("EMA decay must be in [0, 1)")
    # 标签平滑率必须位于 [0,1)，1 会完全抹去真值类别目标。
    if not 0.0 <= label_smoothing < 1.0:
        # 报告配置错误，避免损失静默偏离预期。
        raise ValueError("Label smoothing must be in [0, 1)")
    # SupCon 权重必须非负；0 表示保留代码路径但禁用其梯度贡献。
    if supcon_weight < 0.0:
        # 负权重会鼓励同类跨文件嵌入分离，因此禁止。
        raise ValueError("SupCon weight must be non-negative")
    # Dropout 概率遵循 PyTorch 合同，1 会丢弃全部融合表示，因此上界不包含 1。
    if not 0.0 <= dropout < 1.0:
        # 报告非法概率，避免模型构造后才出现底层异常。
        raise ValueError("Dropout must be in [0, 1)")
    # 辅助头属于多分支候选模型；平铺 BP 不具备对应运动属性头。
    if auxiliary_heads and not multi_branch:
        # 拒绝不存在 auxiliary_loss 接口的平铺模型组合。
        raise ValueError("Auxiliary heads require the multi-branch model")
    # M1 当前没有训练期辅助头；T2 只允许比较融合深度这一项因素。
    if auxiliary_heads and deep_narrow:
        # 保持消融变量单一，禁止辅助头与 M1 同时开启。
        raise ValueError("Auxiliary heads are disabled for the deep-narrow M1 ablation")
    # 深窄融合建立在六分支输入上，禁止与平铺 BP 组合。
    if deep_narrow and not multi_branch:
        # M1 需要六组固定输入边界，平铺模式无法满足结构合同。
        raise ValueError("Deep-narrow M1 requires the multi-branch model")
    # class_count 是主分类输出节点数，等于稳定类别名称表长度。
    class_count = len(class_names)
    # M1 优先于 M0 多分支；两者都关闭时使用兼容旧导出器的平铺 BP。
    if deep_narrow:
        # 构造审核通过的 88→64→48→32→24 深窄融合模型。
        model: nn.Module = DeepNarrowMultiBranchBPNet(
            train_x.shape[1], class_count, dropout=dropout
        ).to(device)
    # 未启用 M1 但启用多分支时选择浅融合 M0。
    elif multi_branch:
        # 构造 80→64→32 的 M0 浅融合模型。
        model = MultiBranchBPNet(
            train_x.shape[1], class_count, dropout=dropout
        ).to(device)
    else:
        # 构造 297→96→64→32 的平铺 BP。
        model = BPNet(train_x.shape[1], class_count, dropout=dropout).to(device)
    # EMA 开启时创建同结构评估副本；关闭时保持 None，不增加训练内存。
    ema_model = copy.deepcopy(model).to(device) if ema_decay > 0.0 else None
    # ema_state 在首个 epoch 后初始化为独立 state_dict 快照。
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
    # AdamW 优化全部可训练参数；学习率和解耦权重衰减由全局训练规格固定。
    optimizer = torch.optim.AdamW(model.parameters(), lr=LEARNING_RATE, weight_decay=WEIGHT_DECAY)
    # 训练加载器输出 (x,y,file_id)，供主损失和跨文件对比损失共同使用。
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

    # best_state 初始保存未训练模型，保证即使首轮异常评分也有可加载状态。
    best_state = copy.deepcopy(model.state_dict())
    # best_score 四项均从负无穷开始，首个有限验证结果必然成为最佳。
    best_score = (-float("inf"),) * 4
    # best_epoch=0 表示尚未保存任何训练后 checkpoint。
    best_epoch = 0
    # patience_left 是连续未改善 epoch 的剩余容忍次数。
    patience_left = PATIENCE
    # history 按 epoch 保存损失和验证指标，供 JSON 报告与曲线复现。
    history: List[Dict[str, float]] = []

    # 从 epoch 1 遍历到 MAX_EPOCHS；早停条件满足时可提前终止。
    for epoch in range(1, MAX_EPOCHS + 1):
        # 切换训练模式，使 dropout 按配置随机丢弃融合表示。
        model.train()
        # loss_sum 累计总损失乘批样本数，用于计算样本加权 epoch 均值。
        loss_sum = 0.0
        # ce_sum 累计主交叉熵乘批样本数。
        ce_sum = 0.0
        # supcon_sum 累计未乘权重的跨文件监督对比损失。
        supcon_sum = 0.0
        # margin_sum 累计未乘权重的困难类别对间隔损失。
        margin_sum = 0.0
        # 累积五个训练期运动属性任务的加权前原始损失。
        auxiliary_sum = 0.0
        # seen 统计本 epoch 已处理样本数，作为各平均损失分母。
        seen = 0
        # 遍历训练加载器全部批次；每轮完成一次前向、反向和 AdamW 更新。
        for batch_x, batch_y, batch_file_ids in loader:
            # batch_x 形状 [批大小,297]，移到 CPU/CUDA 训练设备。
            batch_x = batch_x.to(device)
            # batch_y 形状 [批大小]，保存主类别 int64 索引。
            batch_y = batch_y.to(device)
            # batch_file_ids 形状 [批大小]，用于排除同文件同类对比正样本。
            batch_file_ids = batch_file_ids.to(device)
            # 清除旧梯度并置为 None，减少不必要的梯度缓冲写入。
            optimizer.zero_grad(set_to_none=True)
            # 前向提取共享嵌入；M0/平铺为 [B,32]，M1 为 [B,24]。
            embeddings = model.forward_features(batch_x)
            # 三种模型均通过统一接口将 32 或 24 维嵌入映射到主类别 logits。
            logits = model.classify_features(embeddings)
            # ce_loss 是含可选类别权重和标签平滑的主分类交叉熵标量。
            ce_loss = criterion(logits, batch_y)
            # supcon_loss 拉近同类不同文件嵌入、分离异类嵌入。
            supcon_loss = cross_file_supervised_contrastive_loss(
                embeddings,
                batch_y,
                batch_file_ids,
            )
            # margin_loss 约束弱类真 logit 领先预定义易混类别至少固定间隔。
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
            # 反向传播总损失，计算所有主网络及可选辅助头参数梯度。
            loss.backward()
            # AdamW 使用当前批梯度更新模型参数一次。
            optimizer.step()
            # 按批样本数累加总损失，最后得到真正样本加权均值。
            loss_sum += float(loss.item()) * len(batch_x)
            # 累加未加权交叉熵诊断值。
            ce_sum += float(ce_loss.item()) * len(batch_x)
            # 累加未乘 supcon_weight 的对比损失诊断值。
            supcon_sum += float(supcon_loss.item()) * len(batch_x)
            # 累加未乘 HARD_PAIR_WEIGHT 的间隔损失诊断值。
            margin_sum += float(margin_loss.item()) * len(batch_x)
            # 按批样本数累计辅助损失，供 epoch 日志计算加权平均。
            auxiliary_sum += float(auxiliary_loss.item()) * len(batch_x)
            # 已见样本数增加当前批大小，P×K 最后一批同样固定完整。
            seen += len(batch_x)

        # EMA 模型存在时，在 epoch 结束后用当前训练模型状态更新平滑参数。
        if ema_model is not None:
            # ema_state 返回与 model.state_dict 同键同形状的独立平滑快照。
            ema_state = update_ema_state(
                ema_state,
                model.state_dict(),
                ema_decay,
            )
            # 把新 EMA 状态载入评估副本，训练模型参数保持不变。
            ema_model.load_state_dict(ema_state)
            # 本 epoch 验证和最佳 checkpoint 使用平滑模型。
            evaluation_model = ema_model
        # 未开启 EMA 时直接评估当前训练模型。
        else:
            # evaluation_model 引用 model，不创建额外状态副本。
            evaluation_model = model
        # 计算验证集窗口准确率、宏 F1 和逐样本预测。
        val_acc, val_f1, val_pred = evaluate(
            evaluation_model,
            val_x,
            val_y,
            device,
        )
        # 计算弱类平均 F1 和所有类别最小 F1，用于日志诊断。
        val_weak_f1, val_worst_f1 = weak_and_worst_f1(
            val_y, val_pred, class_names
        )
        # val_class_recalls 按 class_names 顺序保存每类召回率，形状为 [类别数]。
        val_weak_recall, val_min_recall, val_class_recalls = weak_and_min_recall(
            val_y, val_pred, class_names
        )
        # 总损失除以已见样本数；max 防御空加载器造成除零。
        avg_loss = loss_sum / max(seen, 1)
        # 计算主交叉熵 epoch 样本加权均值。
        avg_ce = ce_sum / max(seen, 1)
        # 计算监督对比损失 epoch 样本加权均值。
        avg_supcon = supcon_sum / max(seen, 1)
        # 计算困难对间隔损失 epoch 样本加权均值。
        avg_margin = margin_sum / max(seen, 1)
        # epoch 辅助损失为所有已见样本的加权平均，关闭辅助头时恒为 0。
        avg_auxiliary = auxiliary_sum / max(seen, 1)
        # score 以最弱类优先的四项字典序决定是否更新最佳 checkpoint。
        score = validation_checkpoint_key(
            val_min_recall,
            val_weak_recall,
            val_f1,
            val_acc,
        )
        # 追加当前 epoch 所有可复核训练和验证指标，不遗漏弱类召回。
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
        # 当前评分严格优于历史最佳时保存评估模型状态并重置早停耐心。
        if score > best_score:
            # 更新最佳四项评分元组。
            best_score = score
            # 深拷贝 CPU/CUDA 状态，防止后续 epoch 原地修改最佳权重。
            best_state = copy.deepcopy(evaluation_model.state_dict())
            # 记录产生当前最佳验证结果的 epoch 编号。
            best_epoch = epoch
            # 验证改善后恢复完整早停耐心。
            patience_left = PATIENCE
        # 未改善时消耗一次耐心，连续耗尽后停止训练。
        else:
            # 剩余耐心减一，允许负值前在本轮日志中显示。
            patience_left -= 1
        # progress_label 非空时追加实验/折次前缀，便于并行结果区分。
        label = f"{progress_label} " if progress_label else ""
        # weakest_index 定位本 epoch 验证集中召回率最低的类别。
        weakest_index = int(np.argmin(val_class_recalls))
        # class_recall_text 使用固定类别顺序输出全部召回率，便于可见窗口逐轮追踪弱类。
        class_recall_text = ",".join(
            f"{name}:{float(recall):.4f}"
            for name, recall in zip(class_names, val_class_recalls)
        )
        # 每个 epoch 输出总损失、各子损失、全局指标、弱类指标、逐类召回和早停状态。
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
        # 连续未改善轮数达到 PATIENCE 时触发早停，保留历史最佳 checkpoint。
        if patience_left <= 0:
            # 退出 epoch 循环，不再执行剩余最大轮次。
            break

    # 训练结束后把历史最佳验证状态载回主模型，确保后续测试和导出不使用末轮权重。
    model.load_state_dict(best_state)
    # 返回最佳模型及完整训练配置/历史，供实验报告和最终工件保存。
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
    """训练三类跳跃形态专家，并返回模型、专用标准化参数和特征列合同。"""
    # family_names 按固定专家输出顺序保留当前主类别表中存在的目标动作。
    family_names = [
        name for name in FAMILY_SPECIALIST_CLASS_NAMES if name in class_names
    ]
    # 少于两个家族类别无法形成分类问题，专家训练没有意义。
    if len(family_names) < 2:
        # 明确报告类别配置不足，避免构造单输出伪分类器。
        raise ValueError("Family specialist requires at least two configured classes")
    # specialist_feature_indices 是从 297 维输入中选择的形态不变和弱类机制列索引。
    specialist_feature_indices = build_jump_shape_feature_indices(build_feature_names())

    # 筛出训练集家族样本，并把全局标签重编码为专家局部标签。
    train_family_raw, train_family_y, train_family_file_ids = family_subset(
        train_x_raw,
        train_y,
        train_file_ids,
        class_names,
        family_names,
    )
    # 筛出验证集家族样本；临时顺序索引仅满足 family_subset 的 file_ids 接口。
    val_family_raw, val_family_y, _ = family_subset(
        val_x_raw,
        val_y,
        np.arange(len(val_y), dtype=np.int64),
        class_names,
        family_names,
    )
    # 筛出测试集家族样本；验证模式下输入可为空但二维特征维度保持 297。
    test_family_raw, test_family_y, _ = family_subset(
        test_x_raw,
        test_y,
        np.arange(len(test_y), dtype=np.int64),
        class_names,
        family_names,
    )
    # 训练专家只保留审核选定特征列，输出形状 [训练家族样本数,专家特征数]。
    train_family_raw = train_family_raw[:, specialist_feature_indices]
    # 验证专家输入使用完全相同列索引和顺序。
    val_family_raw = val_family_raw[:, specialist_feature_indices]
    # 测试专家输入使用完全相同列索引和顺序。
    test_family_raw = test_family_raw[:, specialist_feature_indices]
    # 仅用家族训练样本统计量标准化训练、验证和测试专家输入。
    (
        train_family_x,
        val_family_x,
        test_family_x,
        specialist_mean,
        specialist_std,
    ) = standardize(train_family_raw, val_family_raw, test_family_raw)

    # 输出专家类别、样本数和输入维度，PyCharm 可见训练启动状态。
    print(
        f"start {progress_label} specialist=family "
        f"classes={family_names} train={len(train_family_y)} "
        f"val={len(val_family_y)} test={len(test_family_y)} "
        f"feature_dim={len(specialist_feature_indices)}",
        flush=True,
    )
    # 复用主训练循环训练专家 BP；关闭 SupCon，保留可选 P×K 文件平衡。
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
    # 返回专家部署所需模型、标准化向量、局部类别顺序、原特征索引和训练历史。
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
    """加载已保存主模型与标准化参数，并验证输入维度及窗口长度合同。"""
    # 统一转换为 Path，兼容命令行或测试传入字符串路径。
    artifact_dir = Path(artifact_dir)
    # config_path 指向包含 mean、std、window_len 的无 pickle NPZ 配置。
    config_path = artifact_dir / "scaler_and_config.npz"
    # model_path 指向 PyTorch state_dict 文件。
    model_path = artifact_dir / "best_model.pt"
    # 以禁用 pickle 的只读方式加载标准化和窗口配置，降低不可信对象执行风险。
    with np.load(config_path, allow_pickle=False) as config:
        # mean 是 float32 [输入维度] 训练特征均值。
        mean = np.asarray(config["mean"], dtype=np.float32)
        # std 是 float32 [输入维度] 训练特征标准差，已含近零列保护。
        std = np.asarray(config["std"], dtype=np.float32)
        # saved_window_len 从标量或单元素数组统一解析为整数采样点数。
        saved_window_len = int(np.asarray(config["window_len"]).reshape(-1)[0])
    # mean/std 必须与当前特征提取维度完全一致，通常为 [297]。
    if mean.shape != (input_dim,) or std.shape != (input_dim,):
        # 拒绝旧维度工件，防止广播或列错位导致错误推理。
        raise ValueError("Primary artifact feature dimension does not match current extractor")
    # 保存模型使用的窗口点数必须与当前实验窗口一致。
    if saved_window_len != expected_window_len:
        # 报错同时给出保存值和请求值，避免不同时间尺度特征混用。
        raise ValueError(
            f"Primary artifact window_len={saved_window_len} does not match "
            f"requested window_len={expected_window_len}"
        )
    # 仅加载张量权重到目标设备，不反序列化任意 Python 对象。
    state = torch.load(model_path, map_location=device, weights_only=True)
    # 兼容包含 primary 键的主模型+专家联合 checkpoint。
    if isinstance(state, dict) and "primary" in state:
        # 提取主模型 state_dict，忽略同文件中的专家权重。
        state = state["primary"]
    # 按命令行声明恢复平铺 BP 或六分支 M0；结构必须与保存参数键完全匹配。
    model: nn.Module = (
        MultiBranchBPNet(input_dim, class_count).to(device)
        if multi_branch
        else BPNet(input_dim, class_count).to(device)
    )
    # 严格加载全部参数键和形状，结构不匹配时由 PyTorch 报错。
    model.load_state_dict(state)
    # 切换推理模式，关闭 dropout，固定后续验证输出。
    model.eval()
    # 返回可推理模型以及 [input_dim] 均值和标准差。
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
    """执行一个窗口规格的文件划分、前处理、训练、路由评估和结果汇总。"""
    # 把窗口秒数换算为窗口点数和固定步长点数。
    window_len, step_len = window_lengths(window_seconds)
    # 以原始文件为单位划分训练/验证/测试，并仅向训练集追加额外记录。
    train_records, val_records, test_records = split_records_for_experiment(
        records,
        extra_train_records,
        seed,
    )
    # 仅从训练文件静坐窗口估计整体活动阈值，避免验证/测试统计泄漏。
    rest_threshold = estimate_rest_threshold(train_records, window_len, step_len)
    # 仅从训练文件静坐逐点分数估计连续活动点阈值。
    active_point_threshold = estimate_active_point_threshold(
        train_records, window_len, step_len
    )
    # 构造该窗口规格独立增强随机流；窗口秒数进入种子以区分不同实验。
    rng = np.random.default_rng(seed + int(window_seconds * 100))

    # 训练文件切窗、过滤、增强并提取 X:[S_train,297]、标签和文件编号。
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
    # 验证文件只切窗和过滤，不增强，保持真实文件级泛化评估。
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
    # 搜参验证模式禁止生成或查看测试窗口，避免多次试验污染最终测试集。
    if validation_only:
        # test_x_raw 保持二维 [0,297] 形状，后续标准化和掩码可走统一路径。
        test_x_raw = np.empty((0, train_x_raw.shape[1]), dtype=np.float32)
        # test_y 是空 int64 标签数组 [0]。
        test_y = np.empty(0, dtype=np.int64)
        # test_stats 明确记录测试被验证模式跳过，不伪装成零样本数据质量问题。
        test_stats: Dict[str, int] = {"skipped_validation_only": 1}
    # 正式确认模式才从独立测试文件生成不增强窗口。
    else:
        # 测试样本构建规则与验证集一致，且不参与任何训练统计。
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

    # 输出窗口、步长和两个活动阈值，使 PyCharm 中可见前处理配置。
    print(
        f"start window={window_seconds:.1f}s window_len={window_len} step_len={step_len} "
        f"rest_threshold={rest_threshold:.5f} "
        f"active_point_threshold={active_point_threshold:.5f}",
        flush=True,
    )
    # 未提供已有工件时，从当前训练集计算标准化并训练新主模型。
    if primary_artifact_dir is None:
        # 标准化返回三组 [样本数,297] 无量纲输入及 [297] mean/std。
        train_x, val_x, test_x, mean, std = standardize(
            train_x_raw, val_x_raw, test_x_raw
        )
        # 主模型训练输入先应用候选掩码，保证被屏蔽列在全部优化步骤中恒为零。
        train_x = apply_model_feature_mask(train_x, suppress_normalized_phase)
        # 验证输入使用同一掩码，早停和模型选择不能依赖训练时不可见的列。
        val_x = apply_model_feature_mask(val_x, suppress_normalized_phase)
        # 完整模式测试输入和验证模式空数组均保持同一 [样本数,297] 合同。
        test_x = apply_model_feature_mask(test_x, suppress_normalized_phase)
        # 训练主 BP 并返回最佳验证 checkpoint 和逐 epoch 历史。
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
    # 提供工件目录时不重训主模型，只按当前数据执行一致性评估或训练专家。
    else:
        # 加载主模型和其训练 mean/std，并验证窗口长度、特征维度。
        model, mean, std = load_primary_artifacts(
            primary_artifact_dir,
            input_dim=train_x_raw.shape[1],
            class_count=len(class_names),
            expected_window_len=window_len,
            device=device,
            multi_branch=multi_branch,
        )
        # 用保存的 [297] mean/std 标准化当前训练原始特征。
        train_x = ((train_x_raw - mean) / std).astype(np.float32)
        # 验证特征使用相同保存参数，不能重新拟合。
        val_x = ((val_x_raw - mean) / std).astype(np.float32)
        # 测试特征使用相同保存参数；空数组路径也保持 [0,297]。
        test_x = ((test_x_raw - mean) / std).astype(np.float32)
        # 加载主模型时也按当前显式开关处理训练输入，供后续专家流程和一致性检查使用。
        train_x = apply_model_feature_mask(train_x, suppress_normalized_phase)
        # 固定主模型验证输入执行相同掩码。
        val_x = apply_model_feature_mask(val_x, suppress_normalized_phase)
        # 固定主模型测试输入执行相同掩码；验证模式下数组为空但维度合法。
        test_x = apply_model_feature_mask(test_x, suppress_normalized_phase)
        # 训练元数据标记主模型来源绝对路径，不伪造 epoch 历史。
        train_meta = {"loaded_from": str(Path(primary_artifact_dir).resolve())}
        # 输出主模型加载位置，便于可见训练过程区分“重训”和“复用”。
        print(
            f"primary_model_loaded={Path(primary_artifact_dir).resolve()}",
            flush=True,
        )
    # 评估主模型在验证集的原始准确率、宏 F1 和预测。
    flat_val_acc, flat_val_f1, flat_val_pred = evaluate(model, val_x, val_y, device)
    # 验证模式跳过测试推理，并用 NaN 明确表示“未评估”而不是零分。
    if validation_only:
        # 测试准确率设 NaN，报告序列化时保留未评估语义。
        flat_test_acc = float("nan")
        # 测试宏 F1 同样设 NaN。
        flat_test_f1 = float("nan")
        # 测试预测为空 int64 [0] 数组。
        flat_test_pred = np.empty(0, dtype=np.int64)
    # 正式模式评估主模型独立测试集。
    else:
        # 返回主模型测试准确率、宏 F1 和逐样本预测。
        flat_test_acc, flat_test_f1, flat_test_pred = evaluate(
            model, test_x, test_y, device
        )
    # training_meta 先保存主模型训练或加载元数据，专家开启时再追加。
    training_meta: Dict[str, object] = {"primary": train_meta}
    # 开启家族专家时训练局部分类器并对主模型家族预测进行重判。
    if enable_family_specialist:
        # 专家使用原始未标准化 297 维特征自行选择列并拟合专用 mean/std。
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
        # specialist_model 应为当前平铺 BP 专家，输出家族局部类别 logits。
        specialist_model = specialist["model"]
        # 运行时类型断言保护后续导出层索引合同。
        assert isinstance(specialist_model, BPNet)
        # specialist_mean 是 float32 [专家特征数] 训练均值。
        specialist_mean = np.asarray(specialist["mean"], dtype=np.float32)
        # specialist_std 是 float32 [专家特征数] 训练标准差。
        specialist_std = np.asarray(specialist["std"], dtype=np.float32)
        # specialist_names 按专家局部输出顺序保存动作名称。
        specialist_names = list(specialist["class_names"])
        # specialist_feature_indices 是专家从 297 维主特征读取的 int64 列索引。
        specialist_feature_indices = np.asarray(
            specialist["feature_indices"], dtype=np.int64
        )
        # 验证集选取专家列并按专家训练统计标准化，形状 [S_val,专家特征数]。
        specialist_val_x = (
            (val_x_raw[:, specialist_feature_indices] - specialist_mean)
            / specialist_std
        ).astype(np.float32)
        # 测试集执行相同专家列选择和标准化；验证模式下第一维为 0。
        specialist_test_x = (
            (test_x_raw[:, specialist_feature_indices] - specialist_mean)
            / specialist_std
        ).astype(np.float32)
        # 专家对所有验证窗口输出局部类别预测，路由时只使用主预测属于家族的位置。
        specialist_val_pred = predict(specialist_model, specialist_val_x, device)
        # 验证模式没有测试样本，避免对空张量调用 predict 的 concatenate 路径。
        specialist_test_pred = (
            np.empty(0, dtype=np.int64)
            if validation_only
            else predict(specialist_model, specialist_test_x, device)
        )
        # 仅替换主验证预测中的家族类别，其他八类保持主模型结果。
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
        # 在训练元数据中追加专家逐 epoch 历史及配置。
        training_meta["family_specialist"] = specialist["training"]
    # 未启用专家时部署和评估都直接使用单一主模型预测。
    else:
        # None 明确表示没有第二模型需要保存或导出。
        specialist_model = None
        # 空均值数组与无专家状态对应，避免误用主模型均值。
        specialist_mean = np.empty(0, dtype=np.float32)
        # 空标准差数组与无专家状态对应。
        specialist_std = np.empty(0, dtype=np.float32)
        # 空类别表表示路由关闭。
        specialist_names = []
        # 空特征索引表示专家不读取主特征列。
        specialist_feature_indices = np.empty(0, dtype=np.int64)
        # 最终验证预测直接引用主模型预测。
        val_pred = flat_val_pred
        # 最终测试预测直接引用主模型预测或验证模式空数组。
        test_pred = flat_test_pred
    # 根据最终路由预测计算验证窗口准确率。
    val_acc = float(accuracy_score(val_y, val_pred))
    # 根据最终路由预测计算验证宏平均 F1，弱类与强类等权。
    val_f1 = float(f1_score(val_y, val_pred, average="macro", zero_division=0))
    # 验证模式继续保持测试全局指标为 NaN。
    if validation_only:
        # 未评估测试准确率。
        test_acc = float("nan")
        # 未评估测试宏 F1。
        test_f1 = float("nan")
    # 正式模式基于最终路由预测计算独立测试指标。
    else:
        # 测试窗口级准确率范围 [0,1]。
        test_acc = float(accuracy_score(test_y, test_pred))
        # 测试宏 F1 对 11 类等权，零分母按 0。
        test_f1 = float(
            f1_score(test_y, test_pred, average="macro", zero_division=0)
        )
    # 计算验证弱类平均召回、最低类别召回和完整逐类召回数组。
    val_weak_recall, val_min_recall, val_recalls = weak_and_min_recall(
        val_y, val_pred, class_names
    )
    # 验证模式的测试召回全部标记为 NaN，禁止被误读为零召回。
    if validation_only:
        # 保持 test_acc 未评估语义，防止前面分支后续意外覆盖。
        test_acc = float("nan")
        # 测试弱类平均召回未评估。
        test_weak_recall = float("nan")
        # 测试最低类别召回未评估。
        test_min_recall = float("nan")
        # test_recalls 形状 [类别数]，每项均为 NaN。
        test_recalls = np.full(len(class_names), np.nan, dtype=np.float64)
    # 正式模式计算测试弱类、最弱类和逐类召回。
    else:
        # 返回测试召回指标，顺序与 class_names 一致。
        test_weak_recall, test_min_recall, test_recalls = weak_and_min_recall(
            test_y, test_pred, class_names
        )

    # 验证模式只输出训练/验证指标，并显式提示测试跳过。
    if validation_only:
        # 日志包含样本数、准确率、宏 F1 和最低召回，供候选筛选。
        print(
            f"window={window_seconds:.1f}s train={len(train_y)} val={len(val_y)} "
            f"val_acc={val_acc:.4f} val_f1={val_f1:.4f} "
            f"val_min_recall={val_min_recall:.4f} "
            "validation_only=true test_evaluation_skipped=true"
        )
    # 正式模式同时输出独立测试准确率、宏 F1 和最低召回。
    else:
        # 汇总日志便于 PyCharm 中直接比较一个窗口规格的最终结果。
        print(
            f"window={window_seconds:.1f}s "
            f"train={len(train_y)} val={len(val_y)} test={len(test_y)} "
            f"val_acc={val_acc:.4f} val_f1={val_f1:.4f} "
            f"val_min_recall={val_min_recall:.4f} "
            f"test_acc={test_acc:.4f} test_f1={test_f1:.4f} "
            f"test_min_recall={test_min_recall:.4f}"
        )

    # 返回模型、部署参数、数据划分、全部指标和训练历史的完整实验字典。
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
    """使用最佳主模型评估完全外部文件，并按外部存在类别报告召回率。"""
    # 验证模式禁止访问外部留出集，返回显式跳过原因。
    if validation_only:
        # skipped=True 供报告生成器区分未评估和评估失败。
        return {"skipped": True, "reason": "validation_only"}
    # 未提供任何外部记录时返回可序列化跳过结果。
    if not records:
        # 原因 no_external_holdout 表示调用方没有配置数据，并非模型错误。
        return {"skipped": True, "reason": "no_external_holdout"}
    # labels 收集外部记录实际包含的动作目录名称。
    labels = {record.label for record in records}
    # unknown_labels 是外部标签与主模型稳定类别表的差集。
    unknown_labels = labels.difference(class_names)
    # 外部数据出现未知类别时没有对应输出节点，必须拒绝评估。
    if unknown_labels:
        # 报错按名称排序列出未知类，便于修正目录或类别配置。
        raise ValueError(
            "External holdout contains unknown labels: "
            + ", ".join(sorted(unknown_labels))
        )

    # 复用最佳实验窗口点数，保证外部时域尺度与训练一致。
    window_len = int(best_result["window_len"])
    # 复用最佳实验步长点数。
    step_len = int(best_result["step_len"])
    # 复用训练静坐估计的整体活动阈值。
    rest_threshold = float(best_result["rest_threshold"])
    # 复用训练静坐估计的逐点活动阈值。
    active_point_threshold = float(best_result["active_point_threshold"])
    # 外部记录不增强，按训练相同清洗、裁剪、过滤和 297 维提取规则生成样本。
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
    # 防御性处理过滤后零窗口情况，避免标准化和预测对空输入报错。
    if len(y_true) == 0:
        # 返回文件和过滤统计，便于判断阈值过严或外部采集过短。
        return {
            "skipped": True,
            "reason": "no_kept_windows",
            "file_count": len(records),
            "files": [str(record.path) for record in records],
            "sample_stats": stats,
        }
    # mean 是最佳主模型训练集 float32 [297] 均值。
    mean = np.asarray(best_result["mean"], dtype=np.float32)
    # std 是最佳主模型训练集 float32 [297] 标准差。
    std = np.asarray(best_result["std"], dtype=np.float32)
    # 用固定训练参数标准化外部原始特征，输出无量纲 [S_ext,297]。
    x = ((raw_x - mean) / std).astype(np.float32)
    # 外部推理严格复用候选保存的主模型输入掩码，默认 False 兼容旧工件。
    x = apply_model_feature_mask(
        x,
        bool(best_result.get("suppress_normalized_phase", False)),
    )
    # 读取最佳实验主模型对象；专家外部路由未在该函数启用。
    model = best_result["model"]
    # 外部留出集允许评估平铺 BP 或多分支候选，两者均实现 nn.Module 前向接口。
    assert isinstance(model, nn.Module)
    # 批量预测外部每个窗口的主类别索引。
    y_pred = predict(model, x, device)
    # present_labels 按主类别顺序保留外部实际出现的动作名。
    present_labels = [name for name in class_names if name in labels]
    # class_recalls 保存“动作名→该动作窗口召回率”的映射。
    class_recalls = {}
    # 遍历外部存在的类别；每轮只评估该真值类别对应窗口。
    for label in present_labels:
        # label_idx 是动作名在主模型输出中的全局索引。
        label_idx = class_names.index(label)
        # target 形状 [外部样本数]，标记当前真值类别窗口。
        target = y_true == label_idx
        # 当前类召回率是正确预测为 label_idx 的目标窗口比例。
        class_recalls[label] = float(np.mean(y_pred[target] == label_idx))
    # recalls 是按 present_labels 顺序的召回标量列表，用于汇总最小值和均值。
    recalls = list(class_recalls.values())
    # report 汇总外部文件数、窗口数、逐类召回、最小/宏召回和过滤统计。
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
    # 外部留出集只含一个类别时追加便捷 label/recall 字段，兼容单弱类报告。
    if len(present_labels) == 1:
        # label 保存唯一外部动作名称。
        report["label"] = present_labels[0]
        # recall 保存该唯一动作窗口召回率。
        report["recall"] = class_recalls[present_labels[0]]
    # 返回可直接写入 JSON 的外部留出评估字典。
    return report


def save_confusion_matrix(
    y_true: np.ndarray,
    y_pred: np.ndarray,
    class_names: Sequence[str],
    save_path: Path,
) -> None:
    """保存按固定类别顺序绘制的测试集混淆矩阵 PNG。"""
    # matrix 形状 [类别数,类别数]，行是真值、列是预测，元素是窗口计数。
    matrix = confusion_matrix(y_true, y_pred, labels=list(range(len(class_names))))
    # 图宽至少 8 英寸，并随类别数增大以避免坐标标签重叠。
    fig_w = max(8.0, len(class_names) * 0.75)
    # 创建等宽高 Matplotlib 图和单一坐标轴。
    fig, ax = plt.subplots(figsize=(fig_w, fig_w))
    # 用蓝色热图显示混淆计数，矩阵位置保持行真值、列预测。
    im = ax.imshow(matrix, cmap="Blues")
    # 添加颜色条说明颜色对应计数大小。
    fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04)
    # x 轴设置一个刻度对应每个预测类别列。
    ax.set_xticks(np.arange(len(class_names)))
    # y 轴设置一个刻度对应每个真值类别行。
    ax.set_yticks(np.arange(len(class_names)))
    # x 轴类别名旋转 45 度并右对齐，降低长名称重叠。
    ax.set_xticklabels(class_names, rotation=45, ha="right", fontsize=8)
    # y 轴类别名按固定输出顺序显示。
    ax.set_yticklabels(class_names, fontsize=8)
    # x 轴标记为预测类别。
    ax.set_xlabel("Predicted")
    # y 轴标记为真实类别。
    ax.set_ylabel("True")
    # 外层遍历全部真值类别行；每轮处理一行计数文本。
    for i in range(matrix.shape[0]):
        # 内层遍历全部预测类别列；每轮在单元格中心写一个整数。
        for j in range(matrix.shape[1]):
            # 写入 matrix[i,j] 窗口计数，字号 7 适配 11×11 网格。
            ax.text(j, i, int(matrix[i, j]), ha="center", va="center", fontsize=7)
    # 自动调整边距，确保旋转标签和颜色条不被画布裁切。
    fig.tight_layout()
    # 以 180 DPI 写入目标 PNG 路径，目录由上层保存函数预先创建。
    fig.savefig(save_path, dpi=180)
    # 关闭图对象释放内存，避免多实验循环累计 Matplotlib 资源。
    plt.close(fig)


def c_float(value: float) -> str:
    """把 Python 数值转换为最多 9 位有效数字的 C ``float`` 字面量。"""
    # C 模型数组不能安全携带 NaN/Inf，非有限值统一退化为 0。
    if not np.isfinite(value):
        # 0.0 是确定性有限后备值，避免生成器输出编译器相关宏。
        value = 0.0
    # 9 位有效数字足以往返 float32，同时控制头文件体积。
    literal = f"{float(value):.9g}"
    # 纯整数字符串需要补小数点，防止与 f 后缀组合时语义不明确。
    if "." not in literal and "e" not in literal.lower():
        # 追加 .0 后明确表示浮点常量。
        literal += ".0"
    # 添加 f 后缀，要求 C/C++ 按单精度常量计算并匹配 ESP32 float。
    return f"{literal}f"


def c_string(value: str) -> str:
    """转义反斜杠和双引号，并返回合法 C 字符串字面量。"""
    # 先转义反斜杠，再转义双引号，防止特征名或类别名破坏头文件语法。
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    # 在转义内容两侧添加双引号，返回可直接嵌入 C 数组的字符串。
    return f'"{escaped}"'


def c_array_1d(name: str, data: np.ndarray, const_type: str = "float") -> str:
    """把任意形状数值展平为一行 C 静态一维常量数组定义。"""
    # values 按 NumPy C 顺序展平，并逐项转换为有限 float32 字面量。
    values = ", ".join(c_float(v) for v in np.asarray(data).reshape(-1))
    # 返回包含类型、变量名、元素数和初始化值的完整 C 声明。
    return f"static const {const_type} {name}[{len(np.asarray(data).reshape(-1))}] = {{ {values} }};"


def c_array_2d(name: str, data: np.ndarray) -> str:
    """把二维权重矩阵转换为按 ``[输出神经元][输入神经元]`` 存储的 C 数组。"""
    # arr 保持 PyTorch Linear 权重的二维 [out_features,in_features] 行主序。
    arr = np.asarray(data)
    # rows 按输出神经元顺序累积每一行 C 初始化文本。
    rows = []
    # 遍历矩阵每个输出神经元权重行；每轮生成一个花括号子数组。
    for row in arr:
        # 当前行各输入权重转换为单精度字面量并按列顺序连接。
        rows.append("  { " + ", ".join(c_float(v) for v in row) + " }")
    # 返回完整二维静态 float 数组，维度与 arr.shape 完全一致。
    return f"static const float {name}[{arr.shape[0]}][{arr.shape[1]}] = {{\n" + ",\n".join(rows) + "\n};"


def export_esp32_header(
    result: Dict[str, object],
    class_names: Sequence[str],
    feature_names: Sequence[str],
    save_path: Path,
) -> None:
    """把平铺主 BP、可选专家、标准化参数和特征提取实现生成单个 ESP32 头文件。

    Linear 权重保持 PyTorch ``[输出,输入]`` 顺序，C 前向计算
    ``y[o]=bias[o]+sum_i(weight[o][i]*x[i])``，激活为 ReLU。生成内容包含中文说明、
    297 维特征顺序、六轴预处理、标准化及推理函数，供 ESP32-S3 单精度执行。
    """
    # result 主模型必须是平铺 BP，当前单头导出器按 net 固定层索引读取权重。
    model = result["model"]
    # 类型断言防止把多分支 state_dict 按平铺层号错误导出。
    assert isinstance(model, BPNet)
    # state 保存主模型所有层参数；张量仍可能位于 CUDA。
    state = model.state_dict()
    # w1 形状 [96,297]，是输入到第一隐藏层的权重。
    w1 = state["net.0.weight"].cpu().numpy()
    # b1 形状 [96]，是第一隐藏层偏置。
    b1 = state["net.0.bias"].cpu().numpy()
    # w2 形状 [64,96]，是第二隐藏层权重。
    w2 = state["net.3.weight"].cpu().numpy()
    # b2 形状 [64]，是第二隐藏层偏置。
    b2 = state["net.3.bias"].cpu().numpy()
    # w3 形状 [32,64]，是第三隐藏层权重。
    w3 = state["net.6.weight"].cpu().numpy()
    # b3 形状 [32]，是第三隐藏层偏置。
    b3 = state["net.6.bias"].cpu().numpy()
    # w4 形状 [类别数,32]，是输出分类层权重。
    w4 = state["net.8.weight"].cpu().numpy()
    # b4 形状 [类别数]，是输出分类层偏置。
    b4 = state["net.8.bias"].cpu().numpy()

    # specialist_model 可能是三类平铺专家，也可能不存在。
    specialist_model = result.get("specialist_model")
    # has_specialist 只在对象确为 BPNet 时启用专家导出块。
    has_specialist = isinstance(specialist_model, BPNet)
    # specialist_names 按专家局部输出顺序保存类别名。
    specialist_names = list(result.get("specialist_class_names", []))
    # suppress_normalized_phase 决定主 BP 是否把 48 个冗余阶段特征固定为训练均值零分。
    suppress_normalized_phase = bool(result.get("suppress_normalized_phase", False))
    # specialist_lines 保存可选专家索引、标准化和四层参数的 C 声明文本。
    specialist_lines: List[str] = []
    # specialist_feature_dim 默认 0；有专家时改为选中特征列数量。
    specialist_feature_dim = 0
    # 存在有效专家时提取局部类别映射、输入列索引及网络权重。
    if has_specialist:
        # 再次收窄静态类型，保证以下 state_dict 层号属于 BPNet。
        assert isinstance(specialist_model, BPNet)
        # specialist_state 保存专家四个 Linear 层参数。
        specialist_state = specialist_model.state_dict()
        # specialist_global_indices 把专家局部输出索引映射回主模型全局类别索引。
        specialist_global_indices = [class_names.index(name) for name in specialist_names]
        # specialist_feature_indices 是专家从主 297 维特征向量抽取的列号。
        specialist_feature_indices = np.asarray(
            result.get("specialist_feature_indices", np.arange(len(feature_names))),
            dtype=np.int64,
        )
        # 专家输入维度等于特征索引数量。
        specialist_feature_dim = int(len(specialist_feature_indices))
        # 专家第一层输入列数必须与导出的索引数量完全一致。
        if specialist_state["net.0.weight"].shape[1] != specialist_feature_dim:
            # 不一致会导致 C 端越界或遗漏权重，立即阻止生成。
            raise ValueError("Specialist feature index count does not match model input")
        # 组装专家全局类映射、特征列、mean/std 和四层权重偏置声明。
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

    # mean 是主模型 float32 [297] 训练特征均值，C 标准化逐列使用。
    mean = np.asarray(result["mean"], dtype=np.float32)
    # std 是主模型 float32 [297] 训练特征标准差，已应用 1e-6 下限保护。
    std = np.asarray(result["std"], dtype=np.float32)
    # window_len 是实时环形缓冲区达到一次推理所需采样点数。
    window_len = int(result["window_len"])
    # rest_threshold 是整体窗口活动分数阈值，默认 0.03 兼容旧结果。
    rest_threshold = float(result.get("rest_threshold", 0.03))
    # active_point_threshold 是逐点活动强度阈值，默认 0.02 兼容旧结果。
    active_point_threshold = float(result.get("active_point_threshold", 0.02))

    # lines 按头文件顺序保存宏、中文说明、常量数组、特征函数和 BP 前向源码。
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
    # 用 UTF-8 写入完整头文件，保留自动生成的中文公式和一致性说明。
    save_path.write_text("\n".join(lines), encoding="utf-8")


def serializable_experiment(result: Dict[str, object]) -> Dict[str, object]:
    """从实验结果中筛出可直接写入 JSON 的指标、划分和样本统计字段。"""
    # keep 列出允许进入汇总报告的纯标量、列表和字典键，排除模型及 NumPy 预测数组。
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
    # 仅返回 result 中实际存在的白名单字段，兼容验证模式缺少测试键的结果。
    return {key: result[key] for key in keep if key in result}


def deployment_gate_status(
    best_result: Dict[str, object],
    class_names: Sequence[str],
) -> Tuple[bool, np.ndarray]:
    """按普通类门槛和弱类 85% 门槛判断测试逐类召回是否允许部署。"""
    # recalls 形状 [类别数]，由最佳实验独立测试真值和最终预测计算。
    recalls = per_class_recalls(
        np.asarray(best_result["y_test"]),
        np.asarray(best_result["test_pred"]),
        len(class_names),
    )
    # thresholds 按 class_names 生成每类目标；弱类使用放宽的 0.85，其余类使用全局门槛。
    thresholds = np.asarray(
        [
            WEAK_TARGET_MIN_CLASS_RECALL
            if name in RELAXED_RECALL_CLASS_NAMES
            else TARGET_MIN_CLASS_RECALL
            for name in class_names
        ],
        dtype=np.float64,
    )
    # 全部逐类召回达到各自门槛时返回 True，同时返回完整召回数组供失败报告使用。
    return bool(np.all(recalls >= thresholds)), recalls


def export_model_headers(
    best_result: Dict[str, object],
    class_names: Sequence[str],
    feature_names: Sequence[str],
    output_header_path: Path,
    repository_header_path: Path,
    export_when_below_target: bool,
) -> bool:
    """按部署门槛生成输出头文件，并仅在达标时同步到仓库 ESP32 目录。"""
    # reached_target 表示独立测试集所有类别召回达到各自部署门槛。
    reached_target, _ = deployment_gate_status(best_result, class_names)
    # 未达标且未显式允许调试导出时，禁止生成可能被误用的模型头。
    if not reached_target and not export_when_below_target:
        # 返回 False 告知主流程模型未通过部署门槛且没有写头文件。
        return False

    # 创建实验输出头文件父目录，已存在时不报错。
    output_header_path.parent.mkdir(parents=True, exist_ok=True)
    # 生成包含特征、标准化、BP 参数和推理函数的 ESP32 C 头文件。
    export_esp32_header(best_result, class_names, feature_names, output_header_path)
    # 只有正式达标模型才能更新仓库默认部署头，调试导出不得覆盖生产工件。
    if reached_target:
        # 创建仓库部署头父目录，兼容首次生成。
        repository_header_path.parent.mkdir(parents=True, exist_ok=True)
        # 输出路径与仓库路径不同才复制，避免同文件复制自身异常。
        if output_header_path.resolve() != repository_header_path.resolve():
            # 字节复制已生成 UTF-8 头文件到 ESP32 生产位置。
            shutil.copyfile(output_header_path, repository_header_path)
    # 返回部署门槛状态；True 表示仓库生产头可更新，False 仅可能生成调试头。
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
    """保存正式测试模型、标准化配置、混淆矩阵、JSON 报告和可选 ESP32 头文件。"""
    # 创建输出目录及缺失父目录，不删除已有其他实验文件。
    output_dir.mkdir(parents=True, exist_ok=True)
    # y_test 是最佳实验独立测试真值 [测试样本数]。
    y_test = np.asarray(best_result["y_test"])
    # test_pred 是最终主模型或主+专家路由预测 [测试样本数]。
    test_pred = np.asarray(best_result["test_pred"])

    # 保存 11×11 测试混淆矩阵 PNG，类别顺序与模型输出一致。
    save_confusion_matrix(y_test, test_pred, class_names, output_dir / "confusion_matrix.png")
    # 主模型对象用于保存 state_dict；正式导出路径当前要求平铺 BP。
    model = best_result["model"]
    # 类型断言保护固定平铺层索引和下游加载合同。
    assert isinstance(model, BPNet)
    # 可选 specialist_model 是三类家族重判 BP。
    specialist_model = best_result.get("specialist_model")
    # 存在专家时把主模型和专家 state_dict 写入同一命名字典。
    if isinstance(specialist_model, BPNet):
        # 保存联合 checkpoint，键名明确区分主模型与家族专家。
        torch.save(
            {
                "primary": model.state_dict(),
                "family_specialist": specialist_model.state_dict(),
            },
            output_dir / "best_model.pt",
        )
    # 无专家时保持旧格式，只保存主模型 state_dict 兼容既有加载器。
    else:
        # 写入平铺主 BP 参数到 best_model.pt。
        torch.save(model.state_dict(), output_dir / "best_model.pt")
    # scaler_config 保存 Python/ESP32 共同需要的标准化、类别、特征和窗口合同。
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
    # 存在专家时追加其专用 mean/std、局部类别顺序和主特征列索引。
    if isinstance(specialist_model, BPNet):
        # update 保持主配置键不变，并加入四个专家部署数组。
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
    # 使用 NPZ 保存全部数值/字符串数组，不启用 pickle 对象序列化。
    np.savez(output_dir / "scaler_and_config.npz", **scaler_config)

    # report 汇总最终指标、类别/特征顺序、外部留出结果和所有候选实验。
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
    # 以 UTF-8 打开正式训练报告，确保中文类别和路径可读。
    with (output_dir / "training_report.json").open("w", encoding="utf-8") as file:
        # 缩进 2 空格写入 JSON，保留非 ASCII 字符且便于人工审阅。
        json.dump(report, file, ensure_ascii=False, indent=2)

    # 尝试按部署门槛生成/同步 ESP32 头文件，并返回是否正式达标。
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
    """保存验证候选模型、配置和验证报告；不评估测试集也不导出生产头。"""
    # 创建候选输出目录及缺失父目录。
    output_dir.mkdir(parents=True, exist_ok=True)
    # 读取验证选择出的最佳主模型，可能是平铺、M0 或 M1。
    model = best_result["model"]
    # 验证候选允许保存任意 PyTorch 主模型；正式 ESP32 导出仍由门槛和专用导出器控制。
    assert isinstance(model, nn.Module)
    # 保存候选 state_dict，供后续确认训练或离线复核使用。
    torch.save(model.state_dict(), output_dir / "best_model.pt")
    # 保存候选 mean/std、类别/特征顺序、窗口阈值及模型类型，不含测试统计。
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
    # validation_keys 是允许进入候选实验列表的验证字段白名单。
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
    # report 汇总最佳验证指标、分类报告和全部候选窗口结果，不包含测试集结果。
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
    # 用 UTF-8 打开验证报告目标文件。
    with (output_dir / "validation_report.json").open("w", encoding="utf-8") as file:
        # 写入易读 JSON，保留中文和路径字符。
        json.dump(report, file, ensure_ascii=False, indent=2)


def parse_ema_decay(value: str) -> float:
    """解析命令行 EMA 衰减率并验证范围 ``[0,1)``。"""
    # 把 argparse 传入字符串转换为 Python float。
    decay = float(value)
    # EMA 系数必须非负且小于 1，才能保留当前参数的正权重。
    if not 0.0 <= decay < 1.0:
        # 抛出 ValueError，由 argparse 报告非法参数。
        raise ValueError("EMA decay must be in [0, 1)")
    # 返回通过范围检查的 EMA 衰减率。
    return decay


def parse_label_smoothing(value: str) -> float:
    """解析命令行标签平滑率并验证范围 ``[0,1)``。"""
    # 把命令行字符串转换为 Python float。
    smoothing = float(value)
    # 平滑率 1 会完全移除真类目标，负值也不符合交叉熵定义。
    if not 0.0 <= smoothing < 1.0:
        # 抛出 ValueError，由 argparse 显示合法区间。
        raise ValueError("Label smoothing must be in [0, 1)")
    # 返回通过验证的标签平滑率。
    return smoothing


def parse_nonnegative_float(value: str) -> float:
    """解析用于损失权重的非负浮点命令行参数。"""
    # 将命令行字符串转换为浮点数，供非负损失权重使用。
    parsed = float(value)
    # 负损失权重会反向优化目标，属于无效训练配置。
    if parsed < 0.0:
        # 拒绝负值，避免把辅助目标变成反向奖励。
        raise ValueError("Value must be non-negative")
    # 返回已验证的非负浮点数。
    return parsed


def parse_dropout(value: str) -> float:
    """解析 dropout 概率并验证 PyTorch 合法范围 ``[0,1)``。"""
    # 将命令行字符串转换为 dropout 概率。
    dropout = float(value)
    # PyTorch dropout 合法区间为 [0,1)，1 会丢弃全部表示。
    if not 0.0 <= dropout < 1.0:
        # 拒绝负概率和全丢弃概率。
        raise ValueError("Dropout must be in [0, 1)")
    # 返回已验证的 dropout 概率。
    return dropout


def parse_args() -> argparse.Namespace:
    """定义并解析训练、验证、消融、数据路径和模型导出命令行参数。"""
    # 创建参数解析器并给出脚本用途说明。
    parser = argparse.ArgumentParser(description="Train IMU BP model and export ESP32 header.")
    # 主数据集目录；默认按项目路径和兼容路径自动解析。
    parser.add_argument("--dataset-dir", type=Path, default=None)
    # 只并入训练集的附加已标注目录，不进入验证或测试。
    parser.add_argument("--extra-train-dir", type=Path, default=None)
    # 完成模型选择后才加载的外部留出数据目录。
    parser.add_argument("--external-holdout-dir", type=Path, default=None)
    # 模型、配置、报告和候选头文件输出目录。
    parser.add_argument("--output-dir", type=Path, default=OUTPUT_DIR)
    # 可选已验证主模型目录，用于跳过主训练并只训练家族专家。
    parser.add_argument(
        "--primary-artifact-dir",
        type=Path,
        default=None,
        help="Reuse a validated primary BP model and train only the family specialist.",
    )
    # 启用三类家族专家训练和预测路由。
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
    # 验证模式只访问训练/验证文件，禁止测试构建、外部留出和头文件导出。
    parser.add_argument(
        "--validation-only",
        action="store_true",
        help="Train and select with validation data without constructing or evaluating test windows.",
    )
    # 固定数据划分、增强、采样和模型初始化随机种子。
    parser.add_argument("--seed", type=int, default=SEED)
    # 允许未达部署门槛时生成仅供调试的输出目录头文件。
    parser.add_argument("--export-when-below-target", action="store_true")
    # 覆盖最大训练 epoch；每轮仍保留弱类优先早停。
    parser.add_argument("--max-epochs", type=int, default=MAX_EPOCHS)
    # epoch 级参数 EMA 衰减率，0 表示关闭。
    parser.add_argument(
        "--ema-decay",
        type=parse_ema_decay,
        default=0.0,
        help="Epoch-level BP parameter EMA decay; 0 disables EMA.",
    )
    # 主交叉熵标签平滑率，0 表示硬 one-hot 目标。
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
    # 指定一个或多个窗口秒数；每个值单独训练并按验证最弱类指标选优。
    parser.add_argument(
        "--window-seconds",
        type=float,
        nargs="+",
        choices=WINDOW_SECONDS_CHOICES,
        default=list(WINDOW_SECONDS_LIST),
    )
    # 返回 argparse Namespace，字段名与以上长参数去掉前缀并转下划线后一致。
    return parser.parse_args()


def main() -> None:
    """执行可见训练入口：加载数据、逐窗口训练、选择最佳模型、评估并保存工件。"""
    # 解析命令行配置，所有后续实验均以该 Namespace 为唯一运行参数来源。
    args = parse_args()
    # 辅助头依赖多分支模型的 32 维融合嵌入，命令行组合错误时立即终止。
    if args.auxiliary_heads and not args.multi_branch:
        # 平铺 BP 没有辅助头接口，拒绝不兼容配置。
        raise ValueError("--auxiliary-heads requires --multi-branch")
    # M1 必须建立在六分支编码之上。
    if args.deep_narrow and not args.multi_branch:
        # 拒绝没有六分支输入边界的 M1 配置。
        raise ValueError("--deep-narrow requires --multi-branch")
    # T2 不允许同时启用训练期辅助头，避免混入第二个实验变量。
    if args.deep_narrow and args.auxiliary_heads:
        # 拒绝深度与辅助任务同时改变的非单变量消融。
        raise ValueError("--deep-narrow cannot be combined with --auxiliary-heads")
    # 先验修正只对均匀 P×K 采样有定义，其他采样方式不能启用。
    if args.pk_prior_corrected_ce and not args.pk_batches:
        # 普通文件平衡采样下不应用 P×K 先验修正。
        raise ValueError("--pk-prior-corrected-ce requires --pk-batches")
    # 声明修改模块级 MAX_EPOCHS，使 train_model 使用命令行覆盖值。
    global MAX_EPOCHS
    # 保存本次运行最大 epoch；早停仍可提前结束。
    MAX_EPOCHS = args.max_epochs
    # 固定全部随机源，确保相同命令尽量复现划分、增强和权重初始化。
    set_seed(args.seed)

    # 解析有效主数据集根目录。
    dataset_dir = resolve_dataset_dir(args.dataset_dir)
    # 扫描主文件记录、稳定类别顺序和类别名索引映射。
    records, class_names, label_to_idx = scan_dataset(dataset_dir)
    # 搜参前只加载附加训练记录；外部留出集显式保持未加载。
    extra_train_records, _ = load_additional_records(
        args.extra_train_dir,
        args.external_holdout_dir,
        label_to_idx,
        validation_only=True,
    )
    # 生成与 extract_features 一一对应的 297 个特征名称。
    feature_names = build_feature_names()
    # CUDA 可用时训练在 GPU，否则使用 CPU；ESP32 导出始终为 float32 参数。
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    # 输出主数据集绝对路径，便于在 PyCharm 控制台核对数据来源。
    print(f"dataset_dir={dataset_dir.resolve()}")
    # 输出实际训练设备。
    print(f"device={device}")
    # 输出类别数、原始文件数和特征维度，确认 11 类/297 维合同。
    print(f"class_count={len(class_names)} file_count={len(records)} feature_dim={len(feature_names)}")
    # 输出附加训练文件数并确认外部留出尚未加载，避免测试泄漏。
    print(
        f"extra_train_file_count={len(extra_train_records)} "
        f"external_holdout_loaded=false"
    )
    # 输出稳定类别名称顺序，该顺序即模型和 ESP32 输出索引顺序。
    print(f"class_names={class_names}")
    # 输出窗口、增强、损失、结构和验证模式的完整运行配置。
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

    # all_results 按命令行窗口顺序保存每个完整实验结果字典。
    all_results: List[Dict[str, object]] = []
    # 遍历用户指定的全部窗口秒数；每轮独立划分、提特征、训练和验证。
    for window_seconds in args.window_seconds:
        # 执行当前窗口规格实验，训练期间每个 epoch 都在 PyCharm 输出完整指标。
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
        # 追加当前实验结果，供后续验证最弱类优先选优和报告保存。
        all_results.append(result)

    # 按验证最低召回、宏 F1、准确率字典序选择最佳窗口实验。
    best_result = max(
        all_results,
        key=lambda item: (
            float(item["val_min_recall"]),
            float(item["val_f1"]),
            float(item["val_acc"]),
        ),
    )
    # 验证模式保存候选后立即结束，绝不加载外部留出或独立测试结果。
    if args.validation_only:
        # 保存验证最佳模型、标准化配置和全部候选验证报告。
        save_validation_outputs(
            best_result,
            all_results,
            class_names,
            feature_names,
            args.output_dir,
        )
        # 输出最佳验证实验分隔标题。
        print("========== best validation experiment ==========")
        # 输出最佳窗口、验证准确率、宏 F1 和最低类别召回。
        print(
            f"best_window={best_result['window_seconds']}s "
            f"val_acc={best_result['val_acc']:.4f} "
            f"val_f1={best_result['val_f1']:.4f} "
            f"val_min_recall={best_result['val_min_recall']:.4f}"
        )
        # 输出验证集逐类 precision、recall、F1 和支持数。
        print(
            classification_report(
                np.asarray(best_result["y_val"]),
                np.asarray(best_result["val_pred"]),
                target_names=class_names,
                zero_division=0,
            )
        )
        # 明确提示测试、外部评估和头文件导出均已跳过。
        print("validation_only=true test_evaluation_skipped=true header_export_skipped=true")
        # 输出验证工件绝对目录。
        print(f"outputs={args.output_dir.resolve()}")
        # 验证模式任务完成，返回且不执行后续正式测试路径。
        return

    # 模型和窗口选择完成后才加载外部留出记录，保护其独立性。
    _, external_holdout_records = load_additional_records(
        None,
        args.external_holdout_dir,
        label_to_idx,
        validation_only=False,
    )
    # 使用最佳实验固定前处理、标准化和主模型评估外部留出集。
    external_holdout = evaluate_external_holdout(
        best_result,
        external_holdout_records,
        class_names,
        device,
    )
    # 把外部评估写入最佳结果，随后一并保存到正式训练报告。
    best_result["external_holdout"] = external_holdout
    # 输出外部文件数、最小和宏召回，未配置时显示 skipped 状态及 NaN。
    print(
        f"external_holdout_loaded={not bool(external_holdout['skipped'])} "
        f"external_holdout_file_count={external_holdout.get('file_count', 0)} "
        f"external_holdout_min_recall="
        f"{external_holdout.get('min_recall', external_holdout.get('recall', float('nan'))):.4f} "
        f"external_holdout_macro_recall="
        f"{external_holdout.get('macro_recall', external_holdout.get('recall', float('nan'))):.4f}"
    )

    # 保存正式工件并按逐类召回门槛决定是否导出/同步 ESP32 头。
    reached_target = save_outputs(
        best_result,
        all_results,
        class_names,
        feature_names,
        args.output_dir,
        args.export_when_below_target or EXPORT_WHEN_BELOW_TARGET,
    )

    # 输出正式最佳实验分隔标题。
    print("========== best experiment ==========")
    # 输出最佳窗口及验证/测试准确率和宏 F1。
    print(
        f"best_window={best_result['window_seconds']}s "
        f"val_acc={best_result['val_acc']:.4f} val_f1={best_result['val_f1']:.4f} "
        f"test_acc={best_result['test_acc']:.4f} test_f1={best_result['test_f1']:.4f}"
    )
    # 输出独立测试集逐类 precision、recall、F1 和支持数。
    print(
        classification_report(
            np.asarray(best_result["y_test"]),
            np.asarray(best_result["test_pred"]),
            target_names=class_names,
            zero_division=0,
        )
    )
    # 全部类别达到门槛时报告正式头文件输出和仓库同步位置。
    if reached_target:
        # target_reached=true 表示生产 ESP32 头可更新。
        print(
            f"target_reached=true output_header={args.output_dir / 'esp32_bp_model.h'} "
            f"repository_header={ESP32_MODEL_HEADER}"
        )
    # 未达门槛时列出失败类别及召回，默认不生成头文件。
    else:
        # 重新取得按 class_names 顺序的测试召回数组。
        _, test_recalls = deployment_gate_status(best_result, class_names)
        # thresholds 按类别生成普通门槛或弱类 0.85 门槛。
        thresholds = [
            WEAK_TARGET_MIN_CLASS_RECALL
            if name in RELAXED_RECALL_CLASS_NAMES
            else TARGET_MIN_CLASS_RECALL
            for name in class_names
        ]
        # failed 只保留召回低于对应门槛的“类别:召回率”文本。
        failed = [
            f"{name}:{recall:.4f}"
            for name, recall, threshold in zip(class_names, test_recalls, thresholds)
            if recall < threshold
        ]
        # 输出未达标状态、头文件跳过状态和失败类别列表。
        print(
            "target_reached=false header_export_skipped=true "
            f"failed_class_recalls={failed}"
        )
    # 无论是否达标都输出报告和模型工件所在绝对目录。
    print(f"outputs={args.output_dir.resolve()}")


# 仅直接运行脚本时启动训练；被测试或评估脚本导入时不自动执行。
if __name__ == "__main__":
    # 调用可见训练主入口，所有 epoch 日志输出到当前 PyCharm 控制台。
    main()
