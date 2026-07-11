from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

import numpy as np
from sklearn.metrics import roc_auc_score

try:
    # 以包方式运行时复用生产端数据读取、窗口过滤和 294 维特征实现。
    from python import train_export as training
except ModuleNotFoundError:
    # 直接运行当前脚本时从同目录导入生产端模块。
    import train_export as training


# 16 项直接手腕候选的顺序与待审核方案保持一致；单位写入名称便于 C 端对照。
WRIST_SCALAR_FEATURE_NAMES = [
    "wrist_gyro_path_deg",
    "wrist_dynamic_acc_path_gs",
    "wrist_out_in_log_energy_ratio",
    "wrist_out_in_shape_correlation",
    "wrist_reversal_rate_hz",
    "wrist_primary_reversal_phase",
    "wrist_pre_event_gyro_impulse_deg",
    "wrist_low_motion_ratio",
    "wrist_post_event_jerk_peak_g_s",
    "wrist_post_event_jerk_half_width_s",
    "wrist_post_pre_log_energy_ratio",
    "wrist_recovery_time_ratio",
    "wrist_acf_first_peak",
    "wrist_acf_second_first_ratio",
    "wrist_cycle_interval_cv",
    "wrist_harmonic_ratio",
]
# 4 项模板候选只能由折内训练文件建立模板，禁止提前写入生产特征顺序。
WRIST_TEMPLATE_FEATURE_NAMES = [
    "wrist_template_jumping_jack_distance",
    "wrist_template_jumping_squat_distance",
    "wrist_template_jumping_lunge_distance",
    "wrist_template_nearest_margin",
]
# 全部 20 项候选的固定顺序用于 JSON、CSV 和后续晋级清单。
WRIST_CANDIDATE_FEATURE_NAMES = WRIST_SCALAR_FEATURE_NAMES + WRIST_TEMPLATE_FEATURE_NAMES
# 模板类只包含存在官方动作视频且手腕轨迹差异明确的三类。
TEMPLATE_CLASS_NAMES = ["jumping_jack", "jumping_squat", "jumping_lunge"]
# 文件级分析覆盖当前主要混淆方向；每个弱类至少出现在两个比较中。
TARGET_CONFUSION_PAIRS = [
    ("jumping_jack", "jumping_squat"),
    ("jumping_jack", "jumping_lunge"),
    ("jumping_lunge", "jumping_squat"),
    ("jumping_lunge", "tuck_jump"),
    ("jumping_squat", "squat"),
    ("jumping_squat", "tuck_jump"),
    ("squat", "tuck_jump"),
    ("lunge", "squat"),
]


def _safe_log_ratio(numerator: float, denominator: float) -> float:
    """返回带极小量保护的自然对数比，避免大能量比溢出。"""
    # 1e-9 同时保护静止窗口的零分母和零分子。
    return float(math.log((max(numerator, 0.0) + 1e-9) / (max(denominator, 0.0) + 1e-9)))


def _safe_correlation(left: np.ndarray, right: np.ndarray) -> float:
    """返回两条等长序列的皮尔逊相关；任一序列近常量时返回 0。"""
    # 转为 float64 一维数组，降低相关系数累加误差。
    first = np.asarray(left, dtype=np.float64).reshape(-1)
    # 第二条序列同样压平成一维。
    second = np.asarray(right, dtype=np.float64).reshape(-1)
    # 只比较共同长度，防止奇数窗口前后半段相差一个点。
    length = min(len(first), len(second))
    # 少于两个点无法定义相关，使用确定性零值。
    if length < 2:
        return 0.0
    # 去除均值，使点积只衡量波形同步性。
    first = first[:length] - float(np.mean(first[:length]))
    # 第二条序列也去除均值。
    second = second[:length] - float(np.mean(second[:length]))
    # 分母为两个中心化序列的二范数乘积。
    denominator = float(np.linalg.norm(first) * np.linalg.norm(second))
    # 近常量输入返回 0，禁止除零生成 NaN。
    if denominator <= 1e-12:
        return 0.0
    # 点积归一化后范围理论上为 [-1,1]。
    return float(np.clip(np.dot(first, second) / denominator, -1.0, 1.0))


def _smooth_three(values: np.ndarray) -> np.ndarray:
    """使用三点对称滑动平均抑制单点噪声，并保持输出长度不变。"""
    # 输入转换为 float64 一维序列。
    series = np.asarray(values, dtype=np.float64).reshape(-1)
    # 少于三个点时无法形成完整窗口，直接复制输入。
    if len(series) < 3:
        return series.copy()
    # 边缘复制保证卷积后长度与原始序列一致。
    padded = np.pad(series, (1, 1), mode="edge")
    # 固定核 [1,1,1]/3 在 Python 和后续 C 实现中容易保持一致。
    return np.convolve(padded, np.ones(3, dtype=np.float64) / 3.0, mode="valid")


def _principal_gyro_projection(gyro: np.ndarray) -> np.ndarray:
    """返回手腕三轴角速度在窗口 PCA 主轴上的带符号投影，单位为 deg/s。"""
    # 输入形状固定为 [时间点数,3]，并转为 float64。
    values = np.asarray(gyro, dtype=np.float64)
    # 空输入返回空序列，供边界测试稳定处理。
    if len(values) == 0:
        return np.zeros(0, dtype=np.float64)
    # 去除三轴均值，使主轴描述动态摆动而非陀螺零偏。
    centered = values - np.mean(values, axis=0, keepdims=True)
    # 二阶矩矩阵形状为 [3,3]，单位为 (deg/s)^2。
    moment = centered.T @ centered
    # 最大特征值对应手腕主要转动方向。
    principal_axis = np.linalg.eigh(moment)[1][:, -1]
    # 使用绝对值最大轴分量固定 PCA 的任意正负号。
    anchor_index = int(np.argmax(np.abs(principal_axis)))
    # 主分量为负时翻转整条轴，使相同窗口重复运行得到同一符号。
    if float(principal_axis[anchor_index]) < 0.0:
        principal_axis = -principal_axis
    # 返回每个采样点沿主轴的带符号角速度。
    return centered @ principal_axis


def _local_peaks(values: np.ndarray, minimum_distance: int, threshold: float) -> np.ndarray:
    """返回满足幅值和最小间隔约束的确定性局部峰索引。"""
    # 平滑输入减少相邻噪声点产生的重复峰。
    series = _smooth_three(values)
    # 少于三个点时不存在内部局部峰。
    if len(series) < 3:
        return np.zeros(0, dtype=np.int64)
    # 找到不低于左右邻点且达到门槛的内部候选峰。
    candidates = np.flatnonzero(
        (series[1:-1] > series[:-2])
        & (series[1:-1] >= series[2:])
        & (series[1:-1] >= float(threshold))
    ) + 1
    # 按幅值从高到低、索引从小到大选择，保证近邻峰只保留更强者。
    ordered = sorted(candidates.tolist(), key=lambda index: (-float(series[index]), index))
    # selected 保存通过最小间隔检查的峰。
    selected: List[int] = []
    # 逐个处理强峰，避免较弱邻峰先占位。
    for index in ordered:
        # 当前峰与所有已选峰距离均足够时才保留。
        if all(abs(index - kept) >= minimum_distance for kept in selected):
            selected.append(index)
    # 时间升序是周期间隔和重复分段的必要条件。
    return np.asarray(sorted(selected), dtype=np.int64)


def _autocorrelation_peaks(values: np.ndarray) -> Tuple[float, float]:
    """返回 0.3～3.0 秒范围内第一峰和第二峰/第一峰比。"""
    # 输入序列去除均值后计算归一化自相关。
    series = np.asarray(values, dtype=np.float64).reshape(-1)
    # 窗口过短时没有可用延迟范围。
    if len(series) < 10:
        return 0.0, 0.0
    # 去均值后能量作为所有延迟的统一分母。
    centered = series - float(np.mean(series))
    # 静止窗口能量近零时返回零周期证据。
    energy = float(np.dot(centered, centered))
    # 能量保护防止零除。
    if energy <= 1e-12:
        return 0.0, 0.0
    # 最小延迟 0.3 秒，排除三点平滑导致的短延迟伪峰。
    minimum_lag = max(2, int(round(0.30 * training.SAMPLE_RATE)))
    # 最大延迟受 3 秒和半窗长度共同限制。
    maximum_lag = min(int(round(3.0 * training.SAMPLE_RATE)), len(series) // 2)
    # 无合法延迟时返回零值。
    if maximum_lag <= minimum_lag:
        return 0.0, 0.0
    # 逐延迟计算归一化点积，输出顺序对应 minimum_lag 开始的延迟。
    correlations = np.asarray(
        [
            float(np.dot(centered[:-lag], centered[lag:]) / energy)
            for lag in range(minimum_lag, maximum_lag + 1)
        ],
        dtype=np.float64,
    )
    # 相关序列少于三个点时直接取最大正相关作为第一峰。
    if len(correlations) < 3:
        first = max(float(np.max(correlations)), 0.0)
        return first, 0.0
    # 找到相关值为正的内部局部峰。
    peak_offsets = np.flatnonzero(
        (correlations[1:-1] > correlations[:-2])
        & (correlations[1:-1] >= correlations[2:])
        & (correlations[1:-1] > 0.0)
    ) + 1
    # 没有内部峰时使用范围内最大正相关，第二峰比定义为零。
    if len(peak_offsets) == 0:
        first = max(float(np.max(correlations)), 0.0)
        return first, 0.0
    # 第一峰按时间最早定义，符合动作重复周期的第一有效峰。
    first_peak = max(float(correlations[int(peak_offsets[0])]), 0.0)
    # 不足两个峰时第二/第一比为零。
    if len(peak_offsets) < 2 or first_peak <= 1e-12:
        return first_peak, 0.0
    # 第二峰同样按时间顺序选择并除以第一峰。
    second_ratio = max(float(correlations[int(peak_offsets[1])]), 0.0) / first_peak
    # 限制异常边界，避免第一峰极小时放大数值。
    return first_peak, float(np.clip(second_ratio, 0.0, 5.0))


def wrist_scalar_features(window: np.ndarray) -> np.ndarray:
    """提取 16 项仅由手腕六轴窗口定义的候选特征，输入形状为 [N,6]。"""
    # 转为 float64 并验证通道数，通道顺序为 gx、gy、gz、ax、ay、az。
    data = np.asarray(window, dtype=np.float64)
    # 非法或空输入返回固定长度零向量，保持批量堆叠稳定。
    if data.ndim != 2 or data.shape[1] != 6 or len(data) == 0:
        return np.zeros(len(WRIST_SCALAR_FEATURE_NAMES), dtype=np.float32)
    # 陀螺三轴单位为 deg/s，形状为 [N,3]。
    gyro = data[:, 0:3]
    # 加速度三轴单位为 g，形状为 [N,3]。
    acceleration = data[:, 3:6]
    # 角速度模长对设备轴置换和符号翻转不敏感。
    gyro_magnitude = np.linalg.norm(gyro, axis=1)
    # 减去窗口均值向量得到手腕动态加速度代理，不解释为质心加速度。
    dynamic_acceleration = acceleration - np.mean(acceleration, axis=0, keepdims=True)
    # 动态加速度模长单位为 g。
    dynamic_acc_magnitude = np.linalg.norm(dynamic_acceleration, axis=1)
    # 加速度原始模长用于计算手腕 specific-force 的快速变化。
    acc_magnitude = np.linalg.norm(acceleration, axis=1)
    # 采样周期固定为 1/25 秒。
    delta_time = 1.0 / float(training.SAMPLE_RATE)
    # 角速度模长积分得到手腕总转动路程代理，单位为度。
    gyro_path = float(np.sum(gyro_magnitude) * delta_time)
    # 动态加速度模长积分得到手腕平动活跃度，单位为 g*s。
    dynamic_acc_path = float(np.sum(dynamic_acc_magnitude) * delta_time)
    # 前后半段使用相同长度，奇数窗口忽略中间单点以维持可比性。
    half_length = len(data) // 2
    # 前半段角速度平方和表示外摆阶段能量代理。
    first_energy = float(np.sum(np.square(gyro_magnitude[:half_length])))
    # 后半段角速度平方和表示回摆阶段能量代理。
    second_energy = float(np.sum(np.square(gyro_magnitude[-half_length:])))
    # 对数比使前后能量悬殊时仍保持可训练数值范围。
    out_in_log_ratio = _safe_log_ratio(first_energy, second_energy)
    # 后半段时间反转后与前半段相关，衡量是否沿相似路径返回。
    out_in_shape_correlation = _safe_correlation(
        gyro_magnitude[:half_length], gyro_magnitude[-half_length:][::-1]
    )
    # PCA 主轴投影保留手腕主要摆动方向和换向符号。
    principal_gyro = _smooth_three(_principal_gyro_projection(gyro))
    # 有效摆动门槛至少 10 deg/s，同时随窗口 q90 自适应。
    reversal_threshold = max(10.0, 0.15 * float(np.percentile(np.abs(principal_gyro), 90)))
    # 只保留幅值达到门槛的点，静止噪声不会制造符号换向。
    valid_indices = np.flatnonzero(np.abs(principal_gyro) >= reversal_threshold)
    # 默认换向次数和主换向相位为零。
    reversal_indices: List[int] = []
    # 相邻有效点符号不同时记为一次换向。
    for previous, current in zip(valid_indices[:-1], valid_indices[1:]):
        # 两点符号乘积为负表示跨越零轴。
        if float(principal_gyro[previous] * principal_gyro[current]) < 0.0:
            reversal_indices.append(int(current))
    # 窗口持续时间至少为一个采样周期，防止单点输入除零。
    duration_seconds = max(len(data) * delta_time, delta_time)
    # 换向率单位为次/秒，使不同窗口长度可比较。
    reversal_rate = len(reversal_indices) / duration_seconds
    # 有换向时选择两侧最小幅值最大的主换向，减少弱噪声换向影响。
    if reversal_indices:
        primary_reversal = max(
            reversal_indices,
            key=lambda index: min(abs(float(principal_gyro[max(index - 1, 0)])), abs(float(principal_gyro[index]))),
        )
        # 相位归一化到 [0,1]。
        primary_reversal_phase = primary_reversal / float(max(len(data) - 1, 1))
    else:
        # 无换向窗口使用 0 表示缺少该事件。
        primary_reversal_phase = 0.0
    # specific-force 模长一阶差分除以采样周期得到手腕 jerk，单位 g/s。
    jerk = np.zeros(len(data), dtype=np.float64)
    # 从第二点开始填入绝对差分，第一点保持零。
    if len(data) > 1:
        jerk[1:] = np.abs(np.diff(acc_magnitude)) / delta_time
    # 三点平滑后的最大 jerk 位置作为主要快速冲击事件。
    smoothed_jerk = _smooth_three(jerk)
    # 平局时 np.argmax 选择最早事件，保证 Python/C 可复现。
    event_index = int(np.argmax(smoothed_jerk))
    # 事件前 0.4 秒作为摆臂准备区间。
    context_points = max(2, int(round(0.40 * training.SAMPLE_RATE)))
    # 计算前区间半开起点。
    pre_start = max(0, event_index - context_points)
    # 角速度模长积分描述冲击前手腕摆动强度，单位为度。
    pre_event_gyro_impulse = float(np.sum(gyro_magnitude[pre_start:event_index]) * delta_time)
    # 固定物理门槛定义低手腕运动：角速度低于 30 deg/s 且动态加速度低于 0.15g。
    low_motion_ratio = float(np.mean((gyro_magnitude < 30.0) & (dynamic_acc_magnitude < 0.15)))
    # 事件后局部区间最多 0.4 秒且包含事件点。
    post_end = min(len(data), event_index + context_points + 1)
    # 局部后区间为空的理论边界使用事件点本身。
    post_jerk = smoothed_jerk[event_index:post_end]
    # 局部峰值单位为 g/s。
    post_jerk_peak = float(np.max(post_jerk)) if len(post_jerk) else 0.0
    # 半高门槛用于测量冲击峰持续宽度。
    half_height = 0.5 * post_jerk_peak
    # 从事件点开始累计连续不低于半高的点数。
    half_width_points = 0
    # 顺序扫描直到首次低于半高。
    for value in post_jerk:
        # 峰值为零时宽度定义为零，避免静止窗口得到全区间宽度。
        if post_jerk_peak > 1e-12 and float(value) >= half_height:
            half_width_points += 1
        else:
            break
    # 采样点数换算为秒。
    post_jerk_half_width = half_width_points * delta_time
    # 事件前动态加速度平方和表示冲击前手腕运动能量。
    pre_dynamic_energy = float(np.sum(np.square(dynamic_acc_magnitude[pre_start:event_index])))
    # 事件后同长度区间的动态加速度能量用于比较恢复响应。
    post_dynamic_energy = float(np.sum(np.square(dynamic_acc_magnitude[event_index:post_end])))
    # 对数能量比控制异常冲击的数值范围。
    post_pre_log_ratio = _safe_log_ratio(post_dynamic_energy, pre_dynamic_energy)
    # 恢复门槛取动态加速度中位数加 0.5 倍 MAD，属于窗口内部稳健基线。
    dynamic_median = float(np.median(dynamic_acc_magnitude))
    # MAD 对孤立冲击不敏感。
    dynamic_mad = float(np.median(np.abs(dynamic_acc_magnitude - dynamic_median)))
    # 至少保留 0.05g 的实际门槛，避免纯静止量化噪声导致长恢复时间。
    recovery_threshold = max(0.05, dynamic_median + 0.5 * dynamic_mad)
    # 默认恢复点为窗口末尾，表示事件后未观察到稳定恢复。
    recovery_index = len(data) - 1
    # 从事件后逐点寻找连续两个低于门槛的采样。
    for index in range(event_index + 1, max(event_index + 1, len(data) - 1)):
        # 当前点和下一点均低于门槛时判定恢复。
        if dynamic_acc_magnitude[index] <= recovery_threshold and dynamic_acc_magnitude[index + 1] <= recovery_threshold:
            recovery_index = index
            break
    # 恢复时间除以事件后剩余长度，范围约为 [0,1]。
    recovery_time_ratio = (recovery_index - event_index) / float(max(len(data) - 1 - event_index, 1))
    # 自相关使用角速度模长，返回第一周期峰和第二/第一峰比。
    acf_first_peak, acf_second_first_ratio = _autocorrelation_peaks(gyro_magnitude)
    # 周期峰门槛取中位数加 0.5 标准差，最小间隔为 0.3 秒。
    cycle_threshold = float(np.median(gyro_magnitude) + 0.5 * np.std(gyro_magnitude))
    # 提取角速度活动峰用于重复间隔稳定性。
    cycle_peaks = _local_peaks(
        gyro_magnitude,
        minimum_distance=max(2, int(round(0.30 * training.SAMPLE_RATE))),
        threshold=cycle_threshold,
    )
    # 相邻峰间隔以采样点计，比例计算后单位抵消。
    cycle_intervals = np.diff(cycle_peaks).astype(np.float64)
    # 至少两个间隔时计算变异系数，否则没有稳定周期证据。
    if len(cycle_intervals) >= 2 and float(np.mean(cycle_intervals)) > 1e-12:
        cycle_interval_cv = float(np.std(cycle_intervals) / np.mean(cycle_intervals))
    else:
        cycle_interval_cv = 0.0
    # 汉宁窗降低非整数周期的频谱泄漏。
    centered_gyro = gyro_magnitude - float(np.mean(gyro_magnitude))
    # 实数 FFT 输出非负频率功率。
    power = np.square(np.abs(np.fft.rfft(centered_gyro * np.hanning(len(centered_gyro)))))
    # 直流分量不代表动作周期，显式清零。
    if len(power):
        power[0] = 0.0
    # 总功率近零时谐波比定义为零。
    if float(np.sum(power)) <= 1e-12 or len(power) < 3:
        harmonic_ratio = 0.0
    else:
        # 最大功率频点作为基频索引。
        dominant_index = int(np.argmax(power))
        # 二次谐波索引限制在频谱末端以内。
        harmonic_index = min(2 * dominant_index, len(power) - 1)
        # 二次谐波功率除以基频功率。
        harmonic_ratio = float(power[harmonic_index] / max(float(power[dominant_index]), 1e-12))
    # 按名称常量顺序组装 16 项 float32 候选值。
    result = np.asarray(
        [
            gyro_path,
            dynamic_acc_path,
            out_in_log_ratio,
            out_in_shape_correlation,
            reversal_rate,
            primary_reversal_phase,
            pre_event_gyro_impulse,
            low_motion_ratio,
            post_jerk_peak,
            post_jerk_half_width,
            post_pre_log_ratio,
            recovery_time_ratio,
            acf_first_peak,
            acf_second_first_ratio,
            cycle_interval_cv,
            harmonic_ratio,
        ],
        dtype=np.float32,
    )
    # 极端输入若仍产生非有限值，统一替换为零，避免污染后续统计。
    return np.nan_to_num(result, nan=0.0, posinf=0.0, neginf=0.0)


def _resample_series(values: np.ndarray, output_length: int = 32) -> np.ndarray:
    """将一维序列线性重采样到固定长度，保持首尾点。"""
    # 输入转换为 float64 一维数组。
    series = np.asarray(values, dtype=np.float64).reshape(-1)
    # 空序列返回固定长度零值。
    if len(series) == 0:
        return np.zeros(output_length, dtype=np.float64)
    # 单点序列复制到全部输出位置。
    if len(series) == 1:
        return np.full(output_length, float(series[0]), dtype=np.float64)
    # 原始归一化时间轴覆盖 [0,1]。
    source_axis = np.linspace(0.0, 1.0, len(series), dtype=np.float64)
    # 目标时间轴同样覆盖 [0,1]。
    target_axis = np.linspace(0.0, 1.0, output_length, dtype=np.float64)
    # 一维线性插值返回固定长度序列。
    return np.interp(target_axis, source_axis, series)


def extract_repetition_sequences(window: np.ndarray) -> List[np.ndarray]:
    """从手腕窗口活动峰之间提取若干 32×4 的归一化重复序列。"""
    # 校验输入为非空六轴窗口。
    data = np.asarray(window, dtype=np.float64)
    # 非法输入没有可用重复。
    if data.ndim != 2 or data.shape[1] != 6 or len(data) < 10:
        return []
    # 角速度模长描述手腕旋转活动。
    gyro_magnitude = np.linalg.norm(data[:, 0:3], axis=1)
    # 动态加速度减去窗口均值向量，只描述手腕局部变化。
    dynamic_acc = data[:, 3:6] - np.mean(data[:, 3:6], axis=0, keepdims=True)
    # 动态加速度模长单位为 g。
    dynamic_acc_magnitude = np.linalg.norm(dynamic_acc, axis=1)
    # PCA 主轴角速度保留主要摆动的符号轨迹。
    principal_gyro = _principal_gyro_projection(data[:, 0:3])
    # specific-force 模长用于构造 jerk。
    acc_magnitude = np.linalg.norm(data[:, 3:6], axis=1)
    # jerk 第一项无前驱点，保持零。
    jerk = np.zeros(len(data), dtype=np.float64)
    # 后续项为模长绝对差分除以采样周期。
    jerk[1:] = np.abs(np.diff(acc_magnitude)) * float(training.SAMPLE_RATE)
    # z-score 函数只用于组合不同单位的活动序列。
    def standardized(values: np.ndarray) -> np.ndarray:
        # 标准差小于阈值时返回零，静止通道不影响活动峰。
        scale = float(np.std(values))
        return (values - float(np.mean(values))) / scale if scale > 1e-9 else np.zeros_like(values)
    # 角速度和动态加速度 z-score 等权相加，覆盖大幅摆臂和小摆幅冲击两种动作。
    activity = _smooth_three(standardized(gyro_magnitude) + standardized(dynamic_acc_magnitude))
    # 峰门槛取活动中位数加 0.25 标准差，避免只保留极端冲击。
    peak_threshold = float(np.median(activity) + 0.25 * np.std(activity))
    # 重复周期最短 0.3 秒，峰间距离使用相同下限。
    minimum_points = max(8, int(round(0.30 * training.SAMPLE_RATE)))
    # 最长周期 2 秒，超过时通常表示窗口截断或漏峰。
    maximum_points = int(round(2.0 * training.SAMPLE_RATE))
    # 提取满足幅值和距离约束的活动峰。
    peaks = _local_peaks(activity, minimum_distance=minimum_points, threshold=peak_threshold)
    # repetitions 保存每个有效峰间周期的 32×4 表示。
    repetitions: List[np.ndarray] = []
    # 相邻峰构成一个完整重复候选。
    for start, end in zip(peaks[:-1], peaks[1:]):
        # 周期长度超出 0.3～2.0 秒范围时丢弃。
        if end - start < minimum_points or end - start > maximum_points:
            continue
        # 终点加一使两个活动峰均进入重复序列。
        segment = slice(int(start), int(end) + 1)
        # 四条手腕序列分别重采样为 32 点。
        matrix = np.column_stack(
            [
                _resample_series(gyro_magnitude[segment]),
                _resample_series(dynamic_acc_magnitude[segment]),
                _resample_series(principal_gyro[segment]),
                _resample_series(jerk[segment]),
            ]
        )
        # 每条通道在单次重复内部标准化，降低佩戴松紧和执行强度差异。
        channel_mean = np.mean(matrix, axis=0, keepdims=True)
        # 标准差下限 1e-6 防止静止通道除零。
        channel_std = np.maximum(np.std(matrix, axis=0, keepdims=True), 1e-6)
        # 固定输出形状 [32,4]，值为无量纲 z-score。
        normalized = ((matrix - channel_mean) / channel_std).astype(np.float32)
        # 非有限模板不进入距离计算。
        if np.isfinite(normalized).all():
            repetitions.append(normalized)
    # 返回窗口内全部完整重复，覆盖率由分析主流程单独统计。
    return repetitions


def dtw_distance(left: np.ndarray, right: np.ndarray, band: int = 4) -> float:
    """计算两条多通道序列的窄带归一化 DTW 距离。"""
    # 两条序列均转为 float64 二维数组。
    first = np.asarray(left, dtype=np.float64)
    # 第二条序列形状应为 [时间点数,通道数]。
    second = np.asarray(right, dtype=np.float64)
    # 维度或通道数不一致时无法比较，返回正无穷表示无模板证据。
    if first.ndim != 2 or second.ndim != 2 or first.shape[1] != second.shape[1] or len(first) == 0 or len(second) == 0:
        return float("inf")
    # 窄带至少覆盖两条序列长度差，保证终点可达。
    effective_band = max(int(band), abs(len(first) - len(second)))
    # 动态规划矩阵额外一行一列作为空前缀，初值为正无穷。
    costs = np.full((len(first) + 1, len(second) + 1), np.inf, dtype=np.float64)
    # 两个空前缀距离为零。
    costs[0, 0] = 0.0
    # 顺序填充第一条序列的每个时间点。
    for first_index in range(1, len(first) + 1):
        # Sakoe-Chiba 带限制第二条序列的最小索引。
        second_start = max(1, first_index - effective_band)
        # 最大索引包含带宽边界。
        second_end = min(len(second), first_index + effective_band)
        # 遍历当前窄带内所有配对。
        for second_index in range(second_start, second_end + 1):
            # 局部代价取四通道均方差，量纲为标准化值平方。
            local_cost = float(np.mean(np.square(first[first_index - 1] - second[second_index - 1])))
            # 允许匹配、跳过第一序列点或跳过第二序列点三种路径。
            previous_cost = min(
                costs[first_index - 1, second_index - 1],
                costs[first_index - 1, second_index],
                costs[first_index, second_index - 1],
            )
            # 累计最小路径代价。
            costs[first_index, second_index] = local_cost + previous_cost
    # 终点不可达时返回正无穷。
    if not np.isfinite(costs[-1, -1]):
        return float("inf")
    # 用两条序列总长度近似路径长度归一化，使不同周期长度可比较。
    return float(costs[-1, -1] / float(len(first) + len(second)))


def build_medoid_template(sequences: Sequence[np.ndarray]) -> np.ndarray | None:
    """从训练折重复序列中确定性选择近似 medoid 模板。"""
    # 只保留形状为 [32,4] 且全部有限的序列。
    valid = [np.asarray(sequence, dtype=np.float32) for sequence in sequences if np.asarray(sequence).shape == (32, 4) and np.isfinite(sequence).all()]
    # 没有训练序列时该类模板不可用。
    if not valid:
        return None
    # 参考集合最多均匀抽取 96 条，控制三折计算量。
    reference_indices = np.linspace(0, len(valid) - 1, min(len(valid), 96), dtype=int)
    # 候选集合最多均匀抽取 24 条，形成确定性近似 medoid 搜索。
    candidate_indices = np.linspace(0, len(valid) - 1, min(len(valid), 24), dtype=int)
    # best_score 保存当前候选到参考集合的中位距离。
    best_score = float("inf")
    # best_sequence 保存得分最低的训练序列。
    best_sequence = valid[int(candidate_indices[0])]
    # 逐候选比较其到参考集合的窄带 DTW 距离。
    for candidate_index in candidate_indices.tolist():
        # 当前候选来自训练折，绝不使用验证序列。
        candidate = valid[int(candidate_index)]
        # 中位距离降低异常重复对 medoid 的影响。
        distances = [dtw_distance(candidate, valid[int(index)], band=4) for index in reference_indices.tolist()]
        # 有限距离中位数作为候选总得分。
        score = float(np.median(distances))
        # 严格更小时更新；平局保留更早候选以确保确定性。
        if score < best_score:
            best_score = score
            best_sequence = candidate
    # 返回副本，避免调用方意外修改原始训练序列。
    return best_sequence.copy()


def template_distance_features(repetitions: Sequence[np.ndarray], templates: Dict[str, np.ndarray | None]) -> np.ndarray:
    """计算窗口到三类折内训练模板的中位 DTW 距离和最近间隔。"""
    # 缺少完整重复时四项模板特征均标记为 NaN，供覆盖率门槛淘汰。
    if not repetitions:
        return np.full(len(WRIST_TEMPLATE_FEATURE_NAMES), np.nan, dtype=np.float32)
    # distances 按 TEMPLATE_CLASS_NAMES 固定顺序保存三类距离。
    distances: List[float] = []
    # 逐类读取仅由当前折训练文件建立的模板。
    for class_name in TEMPLATE_CLASS_NAMES:
        # 模板缺失时该类距离为 NaN。
        template = templates.get(class_name)
        # 计算窗口内每个完整重复到该类模板的距离。
        repetition_distances = [dtw_distance(sequence, template, band=4) for sequence in repetitions] if template is not None else []
        # 取中位距离降低某次分段误差；无模板时保留 NaN。
        distances.append(float(np.median(repetition_distances)) if repetition_distances else float("nan"))
    # 三类距离均有效时计算最近和次近距离差，值越大表示模板判别越明确。
    if np.isfinite(distances).all():
        ordered = np.sort(np.asarray(distances, dtype=np.float64))
        margin = float(ordered[1] - ordered[0])
    else:
        # 任一模板缺失时不能形成一致三类 margin。
        margin = float("nan")
    # 返回三类距离加最近间隔，共四项。
    return np.asarray(distances + [margin], dtype=np.float32)


def group_records_into_folds(records: Sequence[training.ImuRecord], fold_count: int = 3) -> List[Tuple[List[training.ImuRecord], List[training.ImuRecord]]]:
    """按类别轮转构造文件级折，保证同一采集文件不跨训练和验证。"""
    # 折数至少为 2，否则无法同时形成训练和验证集合。
    if fold_count < 2:
        raise ValueError("fold_count must be at least 2")
    # grouped 将记录按标签组织，避免类别文件数差异破坏验证覆盖。
    grouped: Dict[str, List[training.ImuRecord]] = {}
    # 遍历全部记录并按标签累积。
    for record in records:
        grouped.setdefault(record.label, []).append(record)
    # validation_folds 保存每折的验证记录。
    validation_folds: List[List[training.ImuRecord]] = [[] for _ in range(fold_count)]
    # 每个类别内部按规范化路径排序，划分结果不依赖文件系统枚举顺序。
    for label in sorted(grouped):
        # 路径字符串小写排序兼容 Windows 大小写不敏感语义。
        class_records = sorted(grouped[label], key=lambda record: str(record.path).lower())
        # 轮转分配到各验证折，使每类文件数尽量均衡。
        for record_index, record in enumerate(class_records):
            validation_folds[record_index % fold_count].append(record)
    # all_paths 用于按路径构造训练补集。
    all_records = list(records)
    # folds 保存每折的训练和验证记录二元组。
    folds: List[Tuple[List[training.ImuRecord], List[training.ImuRecord]]] = []
    # 逐验证折建立路径互斥的训练补集。
    for validation_records in validation_folds:
        # resolve 不要求文件存在；Path 字符串足以作为记录身份。
        validation_paths = {str(record.path).lower() for record in validation_records}
        # 不属于当前验证路径的记录全部进入训练折。
        train_records = [record for record in all_records if str(record.path).lower() not in validation_paths]
        # 保存稳定路径排序后的两组记录。
        folds.append(
            (
                sorted(train_records, key=lambda record: (record.label, str(record.path).lower())),
                sorted(validation_records, key=lambda record: (record.label, str(record.path).lower())),
            )
        )
    # 返回指定数量的文件级折。
    return folds


def _cohens_d(first: np.ndarray, second: np.ndarray) -> float:
    """计算两个一维样本的带符号 Cohen's d。"""
    # 移除 NaN 和 Inf，模板缺失不会污染效应量。
    left = np.asarray(first, dtype=np.float64)
    # 第一组仅保留有限值。
    left = left[np.isfinite(left)]
    # 第二组同样过滤非有限值。
    right = np.asarray(second, dtype=np.float64)
    # 第二组仅保留有限值。
    right = right[np.isfinite(right)]
    # 任一组少于两个样本无法稳定估计方差。
    if len(left) < 2 or len(right) < 2:
        return 0.0
    # 合并自由度用于总体方差估计。
    degrees = len(left) + len(right) - 2
    # 计算两组样本方差的加权池化值。
    pooled_variance = ((len(left) - 1) * np.var(left, ddof=1) + (len(right) - 1) * np.var(right, ddof=1)) / float(degrees)
    # 近零方差无法形成可靠标准化效应。
    if pooled_variance <= 1e-12:
        return 0.0
    # 正号表示第一类均值高于第二类均值。
    return float((np.mean(left) - np.mean(right)) / math.sqrt(pooled_variance))


def _oriented_auc(first: np.ndarray, second: np.ndarray) -> float:
    """返回不依赖效应方向的二分类 AUC，范围 [0.5,1]。"""
    # 分别过滤两组有限值。
    left = np.asarray(first, dtype=np.float64)
    # 第一组有限值作为正类分数。
    left = left[np.isfinite(left)]
    # 第二组有限值作为负类分数。
    right = np.asarray(second, dtype=np.float64)
    # 移除第二组非有限值。
    right = right[np.isfinite(right)]
    # 任一组为空时没有 AUC 证据。
    if len(left) == 0 or len(right) == 0:
        return 0.5
    # 标签 1 对应第一类，0 对应第二类。
    labels = np.concatenate([np.ones(len(left), dtype=np.int64), np.zeros(len(right), dtype=np.int64)])
    # 特征值直接作为连续分类分数。
    scores = np.concatenate([left, right])
    # 原始 AUC 小于 0.5 时取反向，保留分离能力而不预设大小方向。
    auc = float(roc_auc_score(labels, scores))
    # 返回方向无关 AUC。
    return max(auc, 1.0 - auc)


def promote_candidates(records: Sequence[Dict[str, object]], maximum_count: int = 12) -> List[Dict[str, object]]:
    """按审批门槛筛选候选并最多返回 maximum_count 项。"""
    # eligible 保存满足全部硬门槛的候选。
    eligible: List[Dict[str, object]] = []
    # 逐候选执行纯统计规则，不读取模型训练结果。
    for record in records:
        # 至少两折效应方向一致。
        direction_ok = int(record.get("same_direction_fold_count", 0)) >= 2
        # 效应量或模板 AUC 至少满足其一。
        separation_ok = float(record.get("robust_abs_file_d", 0.0)) >= 0.50 or float(record.get("robust_file_auc", 0.5)) >= 0.70
        # 与当前 294 维特征最大绝对相关不得超过 0.85。
        novelty_ok = float(record.get("max_abs_correlation_with_production", 1.0)) <= 0.85
        # 需要重复分割的模板特征覆盖率必须达到 70%；直接特征通常为 1。
        coverage_ok = float(record.get("coverage", 0.0)) >= 0.70
        # 至少两个独立文件具有有效值。
        files_ok = int(record.get("supported_file_count", 0)) >= 2
        # 仅全部通过时加入晋级集合。
        if direction_ok and separation_ok and novelty_ok and coverage_ok and files_ok:
            eligible.append(dict(record))
    # 优先按稳健文件效应量，再按 AUC 和较低相关性排序。
    eligible.sort(
        key=lambda record: (
            -float(record.get("robust_abs_file_d", 0.0)),
            -float(record.get("robust_file_auc", 0.5)),
            float(record.get("max_abs_correlation_with_production", 1.0)),
            str(record.get("name", "")),
        )
    )
    # 截断到审批上限，禁止无约束堆叠特征。
    return eligible[: max(0, int(maximum_count))]


def candidate_feature_group(feature_name: str) -> str:
    """返回候选所属物理组，用于约束每个弱类至少获得两类互补证据。"""
    # 前六项描述摆臂幅度、前后形状和角速度换向。
    if feature_name in WRIST_SCALAR_FEATURE_NAMES[:6]:
        return "amplitude_reversal"
    # 中间六项描述手腕冲击前后和恢复过程。
    if feature_name in WRIST_SCALAR_FEATURE_NAMES[6:12]:
        return "impact_recovery"
    # 后四项直接候选描述自相关、周期稳定和谐波。
    if feature_name in WRIST_SCALAR_FEATURE_NAMES[12:]:
        return "periodicity"
    # 四项折内距离均属于模板形状组。
    if feature_name in WRIST_TEMPLATE_FEATURE_NAMES:
        return "template"
    # 未知名称标记为 unknown，不能伪装成已有物理组。
    return "unknown"


def attach_qualified_pair_coverage(
    records: Sequence[Dict[str, object]],
    pair_reports: Dict[str, Sequence[Dict[str, object]]],
    weak_classes: Sequence[str],
) -> List[Dict[str, object]]:
    """把每项特征在全部合格混淆对上的证据附加到候选汇总。"""
    # weak_set 用于过滤普通类别，只统计用户指定的五个弱类。
    weak_set = set(weak_classes)
    # enriched 保存不修改输入对象的复制记录。
    enriched: List[Dict[str, object]] = []
    # 逐候选名称查找其在所有混淆对中的折级证据。
    for source_record in records:
        # 复制原记录，保留最佳对指标供单特征门槛排序。
        record = dict(source_record)
        # 当前候选名称是跨 pair_reports 查找的稳定键。
        feature_name = str(record.get("name", ""))
        # qualified_pairs 保存方向、效应量或 AUC 均通过的全部目标对。
        qualified_pairs: List[str] = []
        # covered_classes 保存这些合格对涉及的弱类并去重。
        covered_classes: set[str] = set()
        # 遍历所有预声明混淆对。
        for pair_name, pair_feature_records in pair_reports.items():
            # 找到当前特征在该对中的唯一记录。
            pair_record = next(
                (item for item in pair_feature_records if str(item.get("name", "")) == feature_name),
                None,
            )
            # 缺少记录时跳过，不能构造虚假覆盖。
            if pair_record is None:
                continue
            # 至少两折方向一致是硬条件。
            direction_ok = int(pair_record.get("same_direction_fold_count", 0)) >= 2
            # 文件级 d>=0.5 或 AUC>=0.70 满足分离条件。
            separation_ok = float(pair_record.get("robust_abs_file_d", 0.0)) >= 0.50 or float(pair_record.get("robust_file_auc", 0.5)) >= 0.70
            # 未同时满足方向和分离条件时不覆盖该混淆对。
            if not direction_ok or not separation_ok:
                continue
            # 记录合格混淆对名称。
            qualified_pairs.append(pair_name)
            # 拆分 pair 键得到两侧类别。
            for class_name in pair_name.split("__vs__"):
                # 只累计五个弱类，普通 lunge 不作为用户弱类覆盖目标。
                if class_name in weak_set:
                    covered_classes.add(class_name)
        # 附加物理组，后续同组多个特征对同一弱类只计一种证据。
        record["feature_group"] = candidate_feature_group(feature_name)
        # 排序后的合格对使 JSON 输出确定。
        record["qualified_pairs"] = sorted(qualified_pairs)
        # 排序后的弱类集合用于互补选择。
        record["covered_weak_classes"] = sorted(covered_classes)
        # 保存增强后的候选。
        enriched.append(record)
    # 返回全部增强记录。
    return enriched


def select_complementary_candidates(
    records: Sequence[Dict[str, object]],
    weak_classes: Sequence[str],
    maximum_count: int = 12,
    minimum_group_count: int = 2,
) -> Tuple[List[Dict[str, object]], Dict[str, List[str]]]:
    """贪心选择候选，使每个弱类由至少 minimum_group_count 个不同物理组支持。"""
    # 先执行方向、分离、新颖性、覆盖率和文件数全部硬门槛。
    eligible = promote_candidates(records, maximum_count=max(len(records), 1))
    # remaining 保存尚未选择的合格候选。
    remaining = [dict(record) for record in eligible]
    # selected 保存按互补覆盖顺序选出的候选。
    selected: List[Dict[str, object]] = []
    # coverage_sets 记录每个弱类已经获得的不同物理组。
    coverage_sets: Dict[str, set[str]] = {class_name: set() for class_name in weak_classes}
    # 在达到上限或没有新增覆盖前持续选择。
    while remaining and len(selected) < max(0, int(maximum_count)):
        # 若所有弱类均已有足够不同物理组，立即停止，避免无约束填满 12 项。
        if all(len(groups) >= minimum_group_count for groups in coverage_sets.values()):
            break
        # 计算每个候选能为尚未达标弱类新增多少“类别-物理组”覆盖。
        def selection_score(record: Dict[str, object]) -> Tuple[int, float, float, float, str]:
            # 当前候选物理组名称。
            group = str(record.get("feature_group", "unknown"))
            # 当前候选覆盖的弱类集合。
            covered = set(str(name) for name in record.get("covered_weak_classes", []))
            # gain 只统计尚未达到组数且当前组尚未出现的弱类。
            gain = sum(
                1
                for class_name, groups in coverage_sets.items()
                if class_name in covered and len(groups) < minimum_group_count and group not in groups
            )
            # 同 gain 时优先文件 d、AUC、更低生产相关性和稳定名称。
            return (
                gain,
                float(record.get("robust_abs_file_d", 0.0)),
                float(record.get("robust_file_auc", 0.5)),
                -float(record.get("max_abs_correlation_with_production", 1.0)),
                str(record.get("name", "")),
            )
        # 选择当前增益最大的候选。
        best = max(remaining, key=selection_score)
        # 增益为零说明剩余特征不能增加互补覆盖，停止选择。
        if selection_score(best)[0] <= 0:
            break
        # 将候选加入最终顺序。
        selected.append(best)
        # 从剩余集合移除同名候选。
        remaining = [record for record in remaining if str(record.get("name", "")) != str(best.get("name", ""))]
        # 当前候选物理组用于更新覆盖集合。
        best_group = str(best.get("feature_group", "unknown"))
        # 对其全部合格弱类增加该物理组。
        for class_name in best.get("covered_weak_classes", []):
            # 非目标弱类不进入覆盖字典。
            if str(class_name) in coverage_sets:
                coverage_sets[str(class_name)].add(best_group)
    # 将集合转为排序列表，便于 JSON 和测试确定比较。
    coverage = {class_name: sorted(groups) for class_name, groups in coverage_sets.items()}
    # 返回最小互补候选集及逐类物理组覆盖。
    return selected, coverage


def parse_args() -> argparse.Namespace:
    """解析三折手腕候选分析命令行参数。"""
    # 创建中文说明的参数解析器。
    parser = argparse.ArgumentParser(description="手腕 IMU 候选特征三折文件级区分分析")
    # 基础 11 类数据集只允许指向训练开发数据根目录。
    parser.add_argument("--dataset-dir", type=Path, required=True)
    # 决赛新增训练角色目录可选；不得传入 external_holdout。
    parser.add_argument("--extra-train-dir", type=Path)
    # 使用既有验证报告读取窗口和过滤参数，不读取其测试预测。
    parser.add_argument("--validation-report", type=Path, required=True)
    # JSON 保存完整折级统计、晋级和拒绝原因。
    parser.add_argument("--output-json", type=Path, required=True)
    # 默认三折符合审批方案。
    parser.add_argument("--fold-count", type=int, default=3)
    # 晋级数硬上限默认为 12。
    parser.add_argument("--maximum-promoted", type=int, default=12)
    # 返回解析后的命名空间。
    return parser.parse_args()


def main() -> None:
    """执行只读训练角色分析并输出三折文件级证据。"""
    # 读取命令行参数。
    args = parse_args()
    # 路径字符串出现 external_holdout 时立即拒绝，避免误读 scy3。
    forbidden_paths = [args.dataset_dir, args.extra_train_dir] if args.extra_train_dir is not None else [args.dataset_dir]
    # 对全部输入目录执行不区分大小写检查。
    if any("external_holdout" in str(path).lower() for path in forbidden_paths):
        raise ValueError("external_holdout/scy3 is forbidden during candidate analysis")
    # 读取既有验证报告，只提取窗口和过滤超参数。
    validation_report = json.loads(args.validation_report.read_text(encoding="utf-8"))
    # 扫描基础数据集并获得固定类别顺序。
    base_records, class_names, label_to_idx = training.scan_dataset(args.dataset_dir)
    # 仅扫描显式 extra_train 目录；外部留出目录不会被递归发现。
    extra_records = training.scan_labeled_dataset(args.extra_train_dir, label_to_idx) if args.extra_train_dir is not None else []
    # 合并训练开发角色记录，按类别和路径排序保证确定性。
    records = sorted(base_records + extra_records, key=lambda record: (record.label, str(record.path).lower()))
    # 从报告读取最佳窗口秒数，当前 Round21/25 均为 2.5 秒。
    window_seconds = float(validation_report["best_window_seconds"])
    # 秒数乘 25 Hz 并四舍五入得到窗口采样点数。
    window_length = int(round(window_seconds * training.SAMPLE_RATE))
    # 第一个实验项保存固定步长和活动过滤参数。
    experiment_config = validation_report["all_experiments"][0]
    # 步长单位为采样点。
    step_length = int(experiment_config["step_len"])
    # 静止过滤阈值单位为生产 motion_score。
    rest_threshold = float(experiment_config["rest_threshold"])
    # 活跃点阈值同样沿用生产验证设置。
    active_threshold = float(experiment_config["active_point_threshold"])
    # samples 保存每个有效窗口的直接特征、生产特征和重复模板。
    samples: List[Dict[str, object]] = []
    # 顺序处理全部训练角色文件，并输出可见进度。
    for record_index, record in enumerate(records):
        # 每十个文件及首个文件输出一次进度。
        if record_index == 0 or record_index % 10 == 0:
            print(f"wrist_features file={record_index + 1}/{len(records)} samples={len(samples)}", flush=True)
        # 使用生产读取器完成原始量程换算，返回 [时间点,6]。
        data = training.load_imu_file(record.path)
        # 文件短于窗口时无法分析，直接跳过。
        if len(data) < window_length:
            continue
        # 遍历与训练一致的重叠窗口。
        for window in training.iter_windows(data, window_length, step_length):
            # 使用标签相关活动门槛过滤明显静止或过渡窗口。
            if not training.keep_window_for_label(window, record.label, rest_threshold, active_threshold):
                continue
            # 保存窗口级直接候选和当前生产 294 维特征。
            samples.append(
                {
                    "path": str(record.path),
                    "label": record.label,
                    "label_idx": record.label_idx,
                    "scalar": wrist_scalar_features(window),
                    "production": training.extract_features(window),
                    "repetitions": extract_repetition_sequences(window),
                }
            )
    # 无有效样本时停止，避免生成空报告。
    if not samples:
        raise ValueError("No valid training-role windows for wrist analysis")
    # 输出特征提取完成进度。
    print(f"wrist_features file={len(records)}/{len(records)} samples={len(samples)} complete=true", flush=True)
    # 构造审批规定的三折文件级划分。
    folds = group_records_into_folds(records, fold_count=args.fold_count)
    # candidate_matrix 保存每个样本的 20 项交叉拟合候选，模板列初始为 NaN。
    candidate_matrix = np.full((len(samples), len(WRIST_CANDIDATE_FEATURE_NAMES)), np.nan, dtype=np.float32)
    # 前 16 列与折无关，直接从样本复制。
    candidate_matrix[:, : len(WRIST_SCALAR_FEATURE_NAMES)] = np.vstack([sample["scalar"] for sample in samples])
    # production_matrix 用于新颖性相关分析。
    production_matrix = np.vstack([sample["production"] for sample in samples]).astype(np.float32)
    # labels 保存每个窗口类别名称，避免依赖标签索引排序。
    labels = np.asarray([str(sample["label"]) for sample in samples], dtype=object)
    # paths 保存每个窗口的采集文件身份。
    paths = np.asarray([str(sample["path"]) for sample in samples], dtype=object)
    # sample_fold 初始为 -1，全部训练角色文件应恰好进入一次验证折。
    sample_fold = np.full(len(samples), -1, dtype=np.int64)
    # fold_template_counts 记录每折三类模板的训练重复数量。
    fold_template_counts: List[Dict[str, int]] = []
    # 逐折只用训练路径构建模板，再给验证路径计算距离。
    for fold_index, (train_records, validation_records) in enumerate(folds):
        # 当前折训练路径集合用于模板来源过滤。
        train_paths = {str(record.path).lower() for record in train_records}
        # 当前折验证路径集合用于交叉拟合输出位置。
        validation_paths = {str(record.path).lower() for record in validation_records}
        # 按三类收集当前折训练样本中的完整重复。
        template_sequences: Dict[str, List[np.ndarray]] = {class_name: [] for class_name in TEMPLATE_CLASS_NAMES}
        # 遍历全部样本，但只读取当前折训练路径和模板类别。
        for sample in samples:
            # 当前样本路径标准化为小写进行 Windows 路径比较。
            sample_path = str(sample["path"]).lower()
            # 非训练路径或非模板类别样本不得参与模板。
            if sample_path not in train_paths or str(sample["label"]) not in template_sequences:
                continue
            # 将该训练窗口内全部完整重复加入对应类别。
            template_sequences[str(sample["label"])].extend(sample["repetitions"])
        # 为三类分别建立训练折近似 medoid。
        templates = {class_name: build_medoid_template(template_sequences[class_name]) for class_name in TEMPLATE_CLASS_NAMES}
        # 记录每类训练重复数量，便于审计模板覆盖。
        fold_template_counts.append({class_name: len(template_sequences[class_name]) for class_name in TEMPLATE_CLASS_NAMES})
        # 遍历样本索引，为当前折验证路径计算四项模板特征。
        for sample_index, sample in enumerate(samples):
            # 非当前验证路径样本留给其所属折处理。
            if str(sample["path"]).lower() not in validation_paths:
                continue
            # 标记该样本所属验证折。
            sample_fold[sample_index] = fold_index
            # 使用当前折训练模板计算距离，绝不读取其他折验证模板。
            candidate_matrix[sample_index, len(WRIST_SCALAR_FEATURE_NAMES) :] = template_distance_features(sample["repetitions"], templates)
        # 输出当前折模板和验证文件进度。
        print(f"wrist_fold={fold_index + 1}/{len(folds)} validation_files={len(validation_records)} template_counts={fold_template_counts[-1]}", flush=True)
    # 任何样本未进入验证折表示路径映射错误，立即停止而非静默使用训练内模板。
    if np.any(sample_fold < 0):
        raise RuntimeError("Some samples were not assigned to a validation fold")
    # 建立每个文件的窗口索引，用于文件中位数聚合。
    unique_paths = sorted(set(paths.tolist()), key=str.lower)
    # file_candidate_rows 保存 20 项文件中位数。
    file_candidate_rows: List[np.ndarray] = []
    # file_production_rows 保存 294 项生产特征文件中位数。
    file_production_rows: List[np.ndarray] = []
    # file_labels 保存文件类别名称。
    file_labels: List[str] = []
    # file_folds 保存文件所属交叉验证折。
    file_folds: List[int] = []
    # 逐文件聚合窗口，降低长文件窗口数对统计的支配。
    for path in unique_paths:
        # 当前文件窗口布尔掩码。
        mask = paths == path
        # 忽略模板 NaN，按列计算候选中位数。
        with np.errstate(all="ignore"):
            file_candidate_rows.append(np.nanmedian(candidate_matrix[mask], axis=0))
        # 生产特征全部有限，直接取列中位数。
        file_production_rows.append(np.median(production_matrix[mask], axis=0))
        # 同一文件标签固定，取第一个窗口标签。
        file_labels.append(str(labels[mask][0]))
        # 同一文件只属于一个验证折，取第一个样本折号。
        file_folds.append(int(sample_fold[mask][0]))
    # 堆叠为 [文件数,20]。
    file_candidates = np.vstack(file_candidate_rows).astype(np.float64)
    # 堆叠为 [文件数,294]。
    file_production = np.vstack(file_production_rows).astype(np.float64)
    # 文件标签转为对象数组便于布尔比较。
    file_label_array = np.asarray(file_labels, dtype=object)
    # 文件折号转为整数数组。
    file_fold_array = np.asarray(file_folds, dtype=np.int64)
    # candidate_summaries 保存每项候选跨所有目标混淆对的最佳稳健证据。
    candidate_summaries: List[Dict[str, object]] = []
    # pair_reports 保存每个目标对、每项特征的逐折细节。
    pair_reports: Dict[str, List[Dict[str, object]]] = {}
    # 预先计算每个候选与 294 维生产文件中位数的最大绝对相关。
    novelty_values: List[float] = []
    # 逐候选计算有限文件上的相关性。
    for candidate_index in range(len(WRIST_CANDIDATE_FEATURE_NAMES)):
        # 当前候选文件值。
        candidate_values = file_candidates[:, candidate_index]
        # 仅有限候选文件可参与相关。
        finite_mask = np.isfinite(candidate_values)
        # 少于三个文件时新颖性无法可靠估计，按 1.0 处理使其淘汰。
        if int(np.sum(finite_mask)) < 3 or float(np.std(candidate_values[finite_mask])) <= 1e-12:
            novelty_values.append(1.0)
            continue
        # 与每个生产特征逐列计算相关，常量列返回零。
        correlations: List[float] = []
        # 遍历 294 个生产特征列。
        for production_index in range(file_production.shape[1]):
            # 当前生产列在有限候选文件上的值。
            production_values = file_production[finite_mask, production_index]
            # 常量生产列不构成冗余证据。
            if float(np.std(production_values)) <= 1e-12:
                correlations.append(0.0)
            else:
                # 安全相关返回 [-1,1]。
                correlations.append(abs(_safe_correlation(candidate_values[finite_mask], production_values)))
        # 保存最大绝对相关。
        novelty_values.append(max(correlations) if correlations else 1.0)
    # 逐目标混淆对计算三折文件效应和 AUC。
    pair_feature_records: Dict[Tuple[str, str], List[Dict[str, object]]] = {}
    # 遍历预声明的目标混淆方向。
    for first_class, second_class in TARGET_CONFUSION_PAIRS:
        # 当前数据缺任一类别时跳过并在报告中保留空列表。
        pair_key = f"{first_class}__vs__{second_class}"
        # feature_records 保存该混淆对的 20 项结果。
        feature_records: List[Dict[str, object]] = []
        # 逐候选计算三折指标。
        for candidate_index, feature_name in enumerate(WRIST_CANDIDATE_FEATURE_NAMES):
            # fold_effects 保存带符号文件 Cohen's d。
            fold_effects: List[float] = []
            # fold_aucs 保存方向无关文件 AUC。
            fold_aucs: List[float] = []
            # 逐验证折只使用该折验证文件中位数。
            for fold_index in range(args.fold_count):
                # 第一类当前折文件掩码。
                first_mask = (file_label_array == first_class) & (file_fold_array == fold_index)
                # 第二类当前折文件掩码。
                second_mask = (file_label_array == second_class) & (file_fold_array == fold_index)
                # 计算带符号效应量。
                fold_effects.append(_cohens_d(file_candidates[first_mask, candidate_index], file_candidates[second_mask, candidate_index]))
                # 计算方向无关 AUC。
                fold_aucs.append(_oriented_auc(file_candidates[first_mask, candidate_index], file_candidates[second_mask, candidate_index]))
            # 非零效应符号用于检查至少两折方向一致。
            signs = [int(np.sign(effect)) for effect in fold_effects if abs(effect) > 1e-12]
            # 正向和负向折数取最大值作为同向折数。
            same_direction_count = max(signs.count(1), signs.count(-1)) if signs else 0
            # 主方向取出现更多的符号；平局取第一项符号。
            if signs:
                dominant_sign = 1 if signs.count(1) >= signs.count(-1) else -1
            else:
                dominant_sign = 0
            # 只聚合同主方向折的绝对效应。
            matching_effects = [abs(effect) for effect in fold_effects if int(np.sign(effect)) == dominant_sign and dominant_sign != 0]
            # 稳健效应取同向折中位数；无同向折为零。
            robust_abs_d = float(np.median(matching_effects)) if matching_effects else 0.0
            # AUC 取三折中位数，缺类别的折会贡献 0.5。
            robust_auc = float(np.median(fold_aucs))
            # 记录逐折和聚合值。
            feature_records.append(
                {
                    "name": feature_name,
                    "fold_file_d": fold_effects,
                    "fold_file_auc": fold_aucs,
                    "same_direction_fold_count": same_direction_count,
                    "robust_abs_file_d": robust_abs_d,
                    "robust_file_auc": robust_auc,
                }
            )
        # 保存结构化结果供候选汇总复用。
        pair_feature_records[(first_class, second_class)] = feature_records
        # JSON 使用字符串键。
        pair_reports[pair_key] = feature_records
    # 逐候选从全部目标混淆对中选择最佳合格证据。
    for candidate_index, feature_name in enumerate(WRIST_CANDIDATE_FEATURE_NAMES):
        # 收集当前候选在各混淆对中的记录。
        evidence = [pair_feature_records[pair][candidate_index] | {"pair": f"{pair[0]}__vs__{pair[1]}"} for pair in TARGET_CONFUSION_PAIRS]
        # 先按同向折数、稳健 d 和 AUC 排序选择最佳目标对。
        best = max(
            evidence,
            key=lambda record: (
                int(record["same_direction_fold_count"]),
                float(record["robust_abs_file_d"]),
                float(record["robust_file_auc"]),
            ),
        )
        # 覆盖率按窗口有限值比例计算；模板缺少完整重复会降低该值。
        coverage = float(np.mean(np.isfinite(candidate_matrix[:, candidate_index])))
        # 支持文件数按文件中位数有限值计数。
        supported_file_count = int(np.sum(np.isfinite(file_candidates[:, candidate_index])))
        # 汇总晋级规则需要的全部字段。
        candidate_summaries.append(
            {
                "name": feature_name,
                "best_pair": best["pair"],
                "same_direction_fold_count": int(best["same_direction_fold_count"]),
                "robust_abs_file_d": float(best["robust_abs_file_d"]),
                "robust_file_auc": float(best["robust_file_auc"]),
                "max_abs_correlation_with_production": float(novelty_values[candidate_index]),
                "coverage": coverage,
                "supported_file_count": supported_file_count,
            }
        )
    # 五个弱类是互补选择的覆盖目标。
    weak_classes = ["jumping_jack", "jumping_lunge", "jumping_squat", "squat", "tuck_jump"]
    # 把每项特征在全部合格混淆对上的证据附加到汇总，不能只保留最强对。
    candidate_summaries = attach_qualified_pair_coverage(
        candidate_summaries,
        pair_reports,
        weak_classes,
    )
    # 在全部硬门槛基础上选择最小互补集合，每个弱类至少覆盖两个不同物理组。
    promoted, weak_class_group_coverage = select_complementary_candidates(
        candidate_summaries,
        weak_classes=weak_classes,
        maximum_count=args.maximum_promoted,
        minimum_group_count=2,
    )
    # promoted_names 用于标记所有候选的最终状态。
    promoted_names = {str(record["name"]) for record in promoted}
    # 给每项候选附加布尔晋级状态，便于审阅拒绝项。
    for record in candidate_summaries:
        record["promoted"] = str(record["name"]) in promoted_names
    # 每个弱类至少两个不同物理组是进入训练的最终门槛。
    feature_gate_passed = len(promoted) > 0 and all(len(groups) >= 2 for groups in weak_class_group_coverage.values())
    # 组装只读分析报告。
    result = {
        "scope": "training_roles_grouped_three_fold_only",
        "test_read": False,
        "external_holdout_read": False,
        "sensor_location": "wrist_only",
        "class_names": class_names,
        "record_count": len(records),
        "window_count": len(samples),
        "fold_count": args.fold_count,
        "window_seconds": window_seconds,
        "production_feature_count": production_matrix.shape[1],
        "candidate_feature_count": len(WRIST_CANDIDATE_FEATURE_NAMES),
        "fold_template_training_sequence_counts": fold_template_counts,
        "candidate_summaries": candidate_summaries,
        "pair_reports": pair_reports,
        "promoted_features": promoted,
        "promoted_feature_count": len(promoted),
        "weak_class_promoted_group_coverage": weak_class_group_coverage,
        "feature_gate_passed": feature_gate_passed,
    }
    # 创建项目本地输出目录。
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    # 使用 UTF-8 中文可读 JSON 保存完整证据。
    args.output_json.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    # 输出最终门槛摘要，供自动化和用户观察。
    print(
        f"WRIST_ANALYSIS_OK windows={len(samples)} records={len(records)} promoted={len(promoted)} gate={feature_gate_passed} output={args.output_json}",
        flush=True,
    )


if __name__ == "__main__":
    # 直接运行脚本时执行只读三折分析。
    main()
