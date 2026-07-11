from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

import numpy as np

try:
    from python import train_export as training
except ModuleNotFoundError:
    import train_export as training


DEFAULT_PAIR_CLASSES = ["jumping_jack", "jumping_lunge", "squat", "tuck_jump"]
DEFAULT_WEAK_CONFUSION_PAIRS = [
    ("lunge", "squat"),
    ("jumping_jack", "jumping_squat"),
    ("jumping_lunge", "tuck_jump"),
    ("jumping_lunge", "jumping_squat"),
    ("jumping_squat", "tuck_jump"),
    ("squat", "tuck_jump"),
    ("wave", "good_morning"),
    ("wave", "jumping_lunge"),
    ("wave", "lunge"),
]
CANDIDATE_EVENT_FEATURE_NAMES = [
    "event_takeoff_position",
    "event_landing_position",
    "event_takeoff_to_landing_interval",
    "event_landing_to_takeoff_peak_ratio",
    "event_free_flight_ratio",
    "event_longest_free_flight_run_ratio",
    "event_vertical_diff_max_abs",
    "event_vertical_diff_peak_position",
    "event_gyro_peak_position",
    "event_gyro_to_landing_peak_lag",
    "event_gyro_vertical_correlation",
    "event_post_takeoff_gyro_energy_ratio",
]
# 事件对齐候选只用于训练/验证无训练筛选；通过多文件证据前不得进入 288 维生产特征顺序。
EVENT_ALIGNED_FEATURE_NAMES = [
    "aligned_horizontal_acc_anisotropy",
    "aligned_horizontal_gyro_anisotropy",
    "aligned_flight_event_count",
    "aligned_complete_flight_ratio",
    "aligned_longest_flight_seconds",
    "aligned_takeoff_peak_vertical_g",
    "aligned_landing_peak_vertical_g",
    "aligned_takeoff_to_landing_seconds",
    "aligned_landing_impact_width_seconds",
    "aligned_flight_gyro_energy_mean",
    "aligned_flight_gyro_peak_deg_s",
    "aligned_flight_horizontal_gyro_integral_deg",
    "aligned_flight_vertical_gyro_integral_abs_deg",
    "aligned_adjacent_landing_impulse_cosine",
    "aligned_alternating_landing_score",
    "aligned_landing_impulse_sign_switch_rate",
]

CYCLE_SOURCES = [
    "acc_vertical",
    "acc_horizontal_mag",
    "gyro_mag",
    "acc_delta_mag",
]
CYCLE_FEATURE_SUFFIXES = [
    "spectral_dominant_hz",
    "spectral_peak_power_ratio",
    "spectral_centroid_hz",
    "spectral_low_band_ratio",
    "spectral_mid_band_ratio",
    "spectral_high_band_ratio",
    "spectral_second_harmonic_ratio",
    "autocorr_prominent_peak_count",
    "autocorr_secondary_peak",
    "autocorr_first_zero_seconds",
    "positive_peak_interval_cv",
    "positive_peak_amplitude_cv",
]
CANDIDATE_CYCLE_FEATURE_NAMES = [
    f"{source}_{suffix}"
    for source in CYCLE_SOURCES
    for suffix in CYCLE_FEATURE_SUFFIXES
]
CROSS_SERIES_PAIRS = [
    ("acc_vertical", "gyro_mag"),
    ("acc_vertical", "acc_horizontal_mag"),
    ("acc_delta_mag", "gyro_mag"),
]
for left, right in CROSS_SERIES_PAIRS:
    CANDIDATE_CYCLE_FEATURE_NAMES.extend(
        [f"{left}_to_{right}_xcorr", f"{left}_to_{right}_xcorr_lag_seconds"]
    )


def _horizontal_motion_vectors(
    window: np.ndarray,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """返回水平动态加速度、水平角速度和垂直角速度，形状分别为 [N,3]、[N,3]、[N]。"""
    # 将窗口统一为 float64；列顺序固定为 gx、gy、gz、ax、ay、az。
    data = np.asarray(window, dtype=np.float64)
    # 使用全窗三轴加速度均值估计重力方向，输入单位为 g。
    gravity = np.mean(data[:, 3:6], axis=0)
    # 计算重力向量模长，过小时使用传感器 z 轴作为确定性退化方向。
    gravity_norm = float(np.linalg.norm(gravity))
    # 生成单位重力向量；该向量用于从三维运动中移除垂直分量。
    gravity_unit = (
        gravity / gravity_norm
        if gravity_norm > 1e-6
        else np.asarray([0.0, 0.0, 1.0], dtype=np.float64)
    )
    # 从原始加速度减去静态重力估计，得到单位为 g 的动态加速度。
    dynamic_acc = data[:, 3:6] - gravity
    # 计算每个动态加速度在重力方向上的标量投影，形状为 [N]。
    acc_vertical_projection = dynamic_acc @ gravity_unit
    # 移除垂直投影，保留重力正交平面内的三维表示，避免水平模长丢失方向。
    horizontal_acc = dynamic_acc - np.outer(acc_vertical_projection, gravity_unit)
    # 计算陀螺三轴在重力方向上的分量，单位为 deg/s，形状为 [N]。
    gyro_vertical = data[:, 0:3] @ gravity_unit
    # 移除陀螺垂直分量，得到水平俯仰/横滚角速度向量。
    horizontal_gyro = data[:, 0:3] - np.outer(gyro_vertical, gravity_unit)
    # 返回三个方向保持量，供各向异性、腾空积分和交替冲量共同复用。
    return horizontal_acc, horizontal_gyro, gyro_vertical


def _plane_anisotropy(vectors: np.ndarray) -> float:
    """返回平面协方差两主特征值的归一化差，范围为 [0,1]。"""
    # 转为 float64 的 [N,3] 向量，降低协方差和特征值计算的舍入误差。
    values = np.asarray(vectors, dtype=np.float64)
    # 少于两个采样点无法估计协方差，返回 0 表示无方向证据。
    if len(values) < 2:
        # 零值是有限退化输出，不代表真实运动各向同性。
        return 0.0
    # 去除窗口向量均值，使协方差只描述动态方向分布。
    centered = values - np.mean(values, axis=0, keepdims=True)
    # 使用总体协方差 C=X^T X/N，矩阵形状为 [3,3]，单位为输入单位平方。
    covariance = centered.T @ centered / float(len(centered))
    # 对称协方差使用 eigvalsh，返回从小到大的三个非负实特征值。
    eigenvalues = np.maximum(np.linalg.eigvalsh(covariance), 0.0)
    # 重力平面理论上只有两个非零方向，取最大的两个特征值 λ1>=λ2。
    lambda_1 = float(eigenvalues[-1])
    # 第二大特征值表示与主运动方向正交的水平动态能量。
    lambda_2 = float(eigenvalues[-2])
    # 总水平方差过小时没有可靠方向，返回 0 防止微小噪声被放大。
    if lambda_1 + lambda_2 <= 1e-12:
        # 零值保持静止窗口的数值稳定性。
        return 0.0
    # 归一化差 (λ1-λ2)/(λ1+λ2) 在单一方向时接近 1，各向同性时接近 0。
    return (lambda_1 - lambda_2) / (lambda_1 + lambda_2)


def _true_runs(mask: np.ndarray, minimum_length: int = 2) -> List[Tuple[int, int]]:
    """返回布尔序列中长度不少于 minimum_length 的半开连续区间 [start,end)。"""
    # 将输入转换为一维布尔数组，True 表示当前采样属于候选腾空段。
    values = np.asarray(mask, dtype=bool)
    # runs 保存按时间升序发现的连续区间。
    runs: List[Tuple[int, int]] = []
    # start 为当前连续 True 区间起点；None 表示尚未进入区间。
    start: int | None = None
    # 顺序遍历全部采样点，并额外处理末尾仍未闭合的区间。
    for index, active in enumerate(values.tolist()):
        # 遇到第一个 True 时记录连续区间起点。
        if active and start is None:
            # 当前索引是半开区间的包含起点。
            start = index
        # 遇到 False 时关闭此前的 True 区间，并检查最小长度。
        elif not active and start is not None:
            # 仅保留至少两个采样点的低支持力段，抑制单点噪声。
            if index - start >= minimum_length:
                # end 使用当前 False 索引，符合 Python 半开切片约定。
                runs.append((start, index))
            # 无论是否保留，都清除起点以等待下一段。
            start = None
    # 窗口末尾仍处于 True 时，用 len(values) 关闭半开区间。
    if start is not None and len(values) - start >= minimum_length:
        # 末尾区间的 end 等于窗口长度。
        runs.append((start, len(values)))
    # 返回按时间排序的区间，供相邻周期特征使用。
    return runs


def candidate_event_aligned_features(window: np.ndarray) -> np.ndarray:
    """提取 16 项事件对齐候选；只用于无训练筛选，输入形状为 [N,6]。"""
    # 转为 float64 并校验六轴列数，避免静默使用错误通道顺序。
    data = np.asarray(window, dtype=np.float64)
    # 空窗口或非六轴窗口不能形成动作事件，返回固定长度零向量。
    if data.ndim != 2 or data.shape[1] != 6 or len(data) == 0:
        # 零向量保持后续矩阵堆叠和统计指标维度稳定。
        return np.zeros(len(EVENT_ALIGNED_FEATURE_NAMES), dtype=np.float32)
    # 复用生产重力对齐逻辑，获得垂直加速度序列，单位为 g，形状为 [N]。
    vertical = np.asarray(training.gravity_aligned_series(data)[0], dtype=np.float64)
    # 加速度模长用于检测低支持力腾空段，单位为 g。
    acc_magnitude = np.linalg.norm(data[:, 3:6], axis=1)
    # 陀螺模长用于腾空角运动能量和峰值，单位为 deg/s。
    gyro_magnitude = np.linalg.norm(data[:, 0:3], axis=1)
    # 计算方向保持的水平加速度、水平角速度和垂直角速度。
    horizontal_acc, horizontal_gyro, gyro_vertical = _horizontal_motion_vectors(data)
    # 分别计算水平加速度与水平角速度的协方差各向异性。
    horizontal_acc_anisotropy = _plane_anisotropy(horizontal_acc)
    # 角速度各向异性用于描述躯干俯仰/横滚是否集中在单一方向。
    horizontal_gyro_anisotropy = _plane_anisotropy(horizontal_gyro)
    # 将加速度模长低于 0.70g 且连续不少于两个点的区间定义为候选腾空事件。
    flight_runs = _true_runs(acc_magnitude < 0.70, minimum_length=2)
    # 事件数是窗口内可解析腾空周期数量，输出为非负 float。
    flight_event_count = float(len(flight_runs))
    # 完整事件需要起点前和终点后都有样本，才能同时估计起跳与落地。
    complete_runs = [(start, end) for start, end in flight_runs if start > 0 and end < len(data)]
    # 完整事件比例反映固定窗口截断周期的程度；无事件时定义为 0。
    complete_flight_ratio = (
        len(complete_runs) / float(len(flight_runs)) if flight_runs else 0.0
    )
    # 最长腾空点数除以 25 Hz 得到秒；无事件时输出 0 秒。
    longest_flight_seconds = (
        max(end - start for start, end in flight_runs) / float(training.SAMPLE_RATE)
        if flight_runs
        else 0.0
    )
    # 以下列表按完整事件累计物理量，最终用中位数降低异常单周期影响。
    takeoff_peaks: List[float] = []
    # landing_peaks 保存每个事件落地局部垂直峰值，单位为 g。
    landing_peaks: List[float] = []
    # event_intervals 保存局部起跳峰到落地峰时间，单位为秒。
    event_intervals: List[float] = []
    # impact_widths 保存落地后连续高冲击持续时间，单位为秒。
    impact_widths: List[float] = []
    # flight_energies 保存腾空段陀螺模长平方均值，单位为 (deg/s)^2。
    flight_energies: List[float] = []
    # flight_gyro_peaks 保存腾空段最大角速度，单位为 deg/s。
    flight_gyro_peaks: List[float] = []
    # flight_horizontal_integrals 保存腾空水平角速度模长积分，单位为度。
    flight_horizontal_integrals: List[float] = []
    # flight_vertical_integrals 保存腾空垂直角速度绝对值积分，单位为度。
    flight_vertical_integrals: List[float] = []
    # landing_impulses 保存落地后水平动态加速度积分向量，单位为 g*s。
    landing_impulses: List[np.ndarray] = []
    # 对每个完整腾空段独立提取起跳、腾空和落地阶段量。
    for start, end in complete_runs:
        # 起跳搜索区间取腾空前最多 5 点并包含腾空起点前一刻。
        pre_start = max(0, start - 5)
        # 在局部区间取最大垂直加速度作为推进峰，平局时 np.argmax 取最早值。
        takeoff_local = int(np.argmax(vertical[pre_start:start]))
        # 将局部索引转换为窗口绝对索引。
        takeoff_index = pre_start + takeoff_local
        # 落地搜索区间取腾空终点后最多 6 点，覆盖冲击与初始吸收阶段。
        post_end = min(len(data), end + 6)
        # 在落地局部区间取最大垂直加速度峰。
        landing_local = int(np.argmax(vertical[end:post_end]))
        # 将局部落地峰索引转换为窗口绝对索引。
        landing_index = end + landing_local
        # 保存两个垂直峰的原始 g 值，供双脚/单脚推进与落地比较。
        takeoff_peaks.append(float(vertical[takeoff_index]))
        # 落地峰通常大于 1g；保留原值而非绝对值以维持物理方向。
        landing_peaks.append(float(vertical[landing_index]))
        # 峰间采样点数除以采样率得到起跳到落地时间。
        event_intervals.append((landing_index - takeoff_index) / float(training.SAMPLE_RATE))
        # 高冲击门槛取 1.20g 与全窗均值加半个标准差中的较大者。
        impact_threshold = max(1.20, float(np.mean(vertical) + 0.5 * np.std(vertical)))
        # impact_points 从落地峰开始统计连续超过门槛的点数。
        impact_points = 0
        # 遍历落地峰到局部搜索终点，首次低于门槛即结束冲击宽度。
        for value in vertical[landing_index:post_end]:
            # 当前点仍为高冲击时累计持续点数。
            if float(value) >= impact_threshold:
                # 每个采样点对应 1/25 秒。
                impact_points += 1
            # 冲击序列出现首个低点后停止，避免把恢复期第二峰并入宽度。
            else:
                # 跳出局部冲击宽度循环。
                break
        # 将连续点数换算为秒并保存。
        impact_widths.append(impact_points / float(training.SAMPLE_RATE))
        # 提取当前腾空区间陀螺模长，形状为 [腾空点数]。
        flight_gyro = gyro_magnitude[start:end]
        # 平方均值衡量腾空姿态调整能量；区间至少两点，因此分母非零。
        flight_energies.append(float(np.mean(np.square(flight_gyro))))
        # 最大角速度描述收腹动作峰值转动强度。
        flight_gyro_peaks.append(float(np.max(flight_gyro)))
        # 水平角速度模长积分近似腾空期间俯仰/横滚总转角，单位为度。
        flight_horizontal_integrals.append(
            float(np.sum(np.linalg.norm(horizontal_gyro[start:end], axis=1)))
            / float(training.SAMPLE_RATE)
        )
        # 垂直角速度绝对积分描述绕重力轴的总转角，单位为度。
        flight_vertical_integrals.append(
            float(np.sum(np.abs(gyro_vertical[start:end])))
            / float(training.SAMPLE_RATE)
        )
        # 落地后最多 5 点的水平动态加速度积分作为单侧着地冲量代理。
        impulse_end = min(len(data), end + 5)
        # 按时间累加三维水平向量并除以采样率，保留方向用于奇偶比较。
        landing_impulses.append(
            np.sum(horizontal_acc[end:impulse_end], axis=0)
            / float(training.SAMPLE_RATE)
        )

    # 定义稳健聚合器：有完整事件时取中位数，否则返回 0。
    def robust_median(values: Sequence[float]) -> float:
        # 中位数降低单个误检峰对文件级效应量的影响。
        return float(np.median(values)) if values else 0.0

    # adjacent_cosines 保存相邻落地水平冲量的方向余弦，范围 [-1,1]。
    adjacent_cosines: List[float] = []
    # 仅在至少两个完整事件时计算交替方向信息。
    for first, second in zip(landing_impulses[:-1], landing_impulses[1:]):
        # 余弦分母是两个冲量模长乘积，单位在相除后抵消。
        denominator = float(np.linalg.norm(first) * np.linalg.norm(second))
        # 两个冲量均有效时保存方向余弦；近零冲量不制造伪方向。
        if denominator > 1e-12:
            # 点积除以模长乘积得到方向相似度。
            adjacent_cosines.append(float(np.dot(first, second) / denominator))
    # 相邻余弦均值：同向接近 1，反向交替接近 -1，无有效对时为 0。
    adjacent_impulse_cosine = robust_median(adjacent_cosines)
    # 交替得分取负余弦的正部分；反向接近 1，同向和无效情况为 0。
    alternating_landing_score = robust_median(
        [max(-value, 0.0) for value in adjacent_cosines]
    )
    # 默认没有足够落地事件时符号切换率为 0。
    impulse_sign_switch_rate = 0.0
    # 至少两个冲量时沿其主变化方向统计相邻符号切换。
    if len(landing_impulses) >= 2:
        # 堆叠为 [事件数,3] 矩阵并去除事件均值。
        impulse_matrix = np.vstack(landing_impulses).astype(np.float64)
        # 使用未中心化二阶矩寻找稳定主方向，避免两个反向冲量中心化后退化。
        moment = impulse_matrix.T @ impulse_matrix
        # 最大特征值对应的特征向量是水平冲量主轴，整体正负号不影响切换率。
        principal_axis = np.linalg.eigh(moment)[1][:, -1]
        # 将每个落地冲量投影到主轴，得到带符号标量序列。
        signed_impulses = impulse_matrix @ principal_axis
        # 仅保留绝对值大于数值阈值的有效冲量符号。
        valid_signs = np.sign(signed_impulses[np.abs(signed_impulses) > 1e-9])
        # 至少两个有效符号时，统计相邻乘积小于零的比例。
        if len(valid_signs) >= 2:
            # 布尔均值即相邻落地左右方向切换率，范围 [0,1]。
            impulse_sign_switch_rate = float(np.mean(valid_signs[:-1] * valid_signs[1:] < 0.0))

    # 按 EVENT_ALIGNED_FEATURE_NAMES 的固定顺序组装 16 项 float32 候选。
    return np.asarray(
        [
            horizontal_acc_anisotropy,
            horizontal_gyro_anisotropy,
            flight_event_count,
            complete_flight_ratio,
            longest_flight_seconds,
            robust_median(takeoff_peaks),
            robust_median(landing_peaks),
            robust_median(event_intervals),
            robust_median(impact_widths),
            robust_median(flight_energies),
            robust_median(flight_gyro_peaks),
            robust_median(flight_horizontal_integrals),
            robust_median(flight_vertical_integrals),
            adjacent_impulse_cosine,
            alternating_landing_score,
            impulse_sign_switch_rate,
        ],
        dtype=np.float32,
    )


def candidate_event_features(window: np.ndarray) -> np.ndarray:
    series = training.build_feature_series(window)
    vertical = np.asarray(series["acc_vertical"], dtype=np.float32)
    acc_mag = np.asarray(series["acc_mag"], dtype=np.float32)
    gyro_mag = np.asarray(series["gyro_mag"], dtype=np.float32)
    n = len(vertical)
    if n == 0:
        return np.zeros(len(CANDIDATE_EVENT_FEATURE_NAMES), dtype=np.float32)
    position_denominator = float(max(n - 1, 1))

    takeoff_index = int(np.argmin(vertical))
    takeoff_position = takeoff_index / position_denominator
    has_landing = takeoff_index + 1 < n
    if has_landing:
        landing_index = takeoff_index + 1 + int(
            np.argmax(vertical[takeoff_index + 1 :])
        )
        landing_position = landing_index / position_denominator
        takeoff_to_landing = (landing_index - takeoff_index) / position_denominator
        takeoff_peak = abs(float(vertical[takeoff_index]))
        landing_peak_ratio = (
            abs(float(vertical[landing_index])) / takeoff_peak
            if takeoff_peak > 1e-6
            else 0.0
        )
    else:
        landing_index = 0
        landing_position = 0.0
        takeoff_to_landing = 0.0
        landing_peak_ratio = 0.0

    free_flight = acc_mag < 0.70
    free_flight_ratio = float(np.mean(free_flight))
    longest_run = 0
    current_run = 0
    for is_free_flight in free_flight.tolist():
        current_run = current_run + 1 if is_free_flight else 0
        longest_run = max(longest_run, current_run)
    longest_free_flight_run_ratio = longest_run / float(n)

    if n > 1:
        vertical_diff = np.abs(np.diff(vertical))
        vertical_diff_index = int(np.argmax(vertical_diff))
        vertical_diff_max = float(vertical_diff[vertical_diff_index])
        vertical_diff_position = (vertical_diff_index + 1) / position_denominator
    else:
        vertical_diff_max = 0.0
        vertical_diff_position = 0.0

    gyro_peak_index = int(np.argmax(gyro_mag))
    gyro_peak_position = gyro_peak_index / position_denominator
    gyro_to_landing_lag = (
        (gyro_peak_index - landing_index) / position_denominator
        if has_landing
        else 0.0
    )
    centered_gyro = gyro_mag - float(np.mean(gyro_mag))
    centered_vertical = vertical - float(np.mean(vertical))
    correlation_denominator = math.sqrt(
        float(np.dot(centered_gyro, centered_gyro))
        * float(np.dot(centered_vertical, centered_vertical))
    )
    gyro_vertical_correlation = (
        float(np.dot(centered_gyro, centered_vertical)) / correlation_denominator
        if correlation_denominator > 1e-12
        else 0.0
    )
    total_gyro_energy = float(np.dot(gyro_mag, gyro_mag))
    post_takeoff_gyro_energy_ratio = (
        float(np.dot(gyro_mag[takeoff_index + 1 :], gyro_mag[takeoff_index + 1 :]))
        / total_gyro_energy
        if has_landing and total_gyro_energy > 1e-12
        else 0.0
    )
    return np.asarray(
        [
            takeoff_position,
            landing_position,
            takeoff_to_landing,
            landing_peak_ratio,
            free_flight_ratio,
            longest_free_flight_run_ratio,
            vertical_diff_max,
            vertical_diff_position,
            gyro_peak_position,
            gyro_to_landing_lag,
            gyro_vertical_correlation,
            post_takeoff_gyro_energy_ratio,
        ],
        dtype=np.float32,
    )


def _coefficient_of_variation(values: np.ndarray) -> float:
    values = np.asarray(values, dtype=np.float64)
    if len(values) < 2:
        return 0.0
    mean = float(np.mean(np.abs(values)))
    return float(np.std(values) / mean) if mean > 1e-12 else 0.0


def _cycle_series_features(values: np.ndarray) -> List[float]:
    x = np.asarray(values, dtype=np.float64)
    n = len(x)
    if n < 4:
        return [0.0] * len(CYCLE_FEATURE_SUFFIXES)
    centered = x - float(np.mean(x))
    spectrum = np.fft.rfft(centered * np.hanning(n))
    power = np.square(np.abs(spectrum))
    frequencies = np.fft.rfftfreq(n, d=1.0 / training.SAMPLE_RATE)
    power[0] = 0.0
    total_power = float(np.sum(power))
    if total_power > 1e-12:
        dominant_index = int(np.argmax(power))
        dominant_hz = float(frequencies[dominant_index])
        peak_power_ratio = float(power[dominant_index] / total_power)
        centroid = float(np.dot(frequencies, power) / total_power)

        def band_ratio(low: float, high: float) -> float:
            mask = (frequencies >= low) & (frequencies < high)
            return float(np.sum(power[mask]) / total_power)

        harmonic_hz = 2.0 * dominant_hz
        harmonic_index = int(np.argmin(np.abs(frequencies - harmonic_hz)))
        harmonic_ratio = float(power[harmonic_index] / max(power[dominant_index], 1e-12))
    else:
        dominant_hz = peak_power_ratio = centroid = harmonic_ratio = 0.0
        band_ratio = lambda _low, _high: 0.0

    energy = float(np.dot(centered, centered))
    max_lag = min(n // 2, int(training.SAMPLE_RATE * 3.0))
    if energy > 1e-12 and max_lag >= 2:
        autocorr = np.asarray(
            [float(np.dot(centered[:-lag], centered[lag:]) / energy) for lag in range(1, max_lag + 1)],
            dtype=np.float64,
        )
        local_peaks = np.flatnonzero(
            (autocorr[1:-1] > autocorr[:-2])
            & (autocorr[1:-1] >= autocorr[2:])
            & (autocorr[1:-1] >= 0.20)
        ) + 1
        prominent_count = float(len(local_peaks))
        secondary_peak = float(np.max(autocorr[local_peaks])) if len(local_peaks) else 0.0
        zero_indices = np.flatnonzero(autocorr <= 0.0)
        first_zero_seconds = (
            float((zero_indices[0] + 1) / training.SAMPLE_RATE)
            if len(zero_indices)
            else float(max_lag / training.SAMPLE_RATE)
        )
    else:
        prominent_count = secondary_peak = first_zero_seconds = 0.0

    threshold = float(np.mean(x) + 0.5 * np.std(x))
    peaks = np.flatnonzero(
        (x[1:-1] > x[:-2]) & (x[1:-1] >= x[2:]) & (x[1:-1] >= threshold)
    ) + 1
    intervals = np.diff(peaks).astype(np.float64)
    peak_values = x[peaks] - float(np.median(x))
    return [
        dominant_hz,
        peak_power_ratio,
        centroid,
        band_ratio(0.35, 1.20),
        band_ratio(1.20, 2.40),
        band_ratio(2.40, 5.00),
        harmonic_ratio,
        prominent_count,
        secondary_peak,
        first_zero_seconds,
        _coefficient_of_variation(intervals),
        _coefficient_of_variation(peak_values),
    ]


def _max_cross_correlation(left: np.ndarray, right: np.ndarray) -> Tuple[float, float]:
    a = np.asarray(left, dtype=np.float64)
    b = np.asarray(right, dtype=np.float64)
    n = min(len(a), len(b))
    if n < 4:
        return 0.0, 0.0
    a = a[:n] - float(np.mean(a[:n]))
    b = b[:n] - float(np.mean(b[:n]))
    max_lag = min(n // 4, int(training.SAMPLE_RATE))
    best_corr = 0.0
    best_lag = 0
    for lag in range(-max_lag, max_lag + 1):
        if lag < 0:
            left_slice, right_slice = a[-lag:], b[: n + lag]
        elif lag > 0:
            left_slice, right_slice = a[: n - lag], b[lag:]
        else:
            left_slice, right_slice = a, b
        denominator = math.sqrt(
            float(np.dot(left_slice, left_slice)) * float(np.dot(right_slice, right_slice))
        )
        correlation = (
            float(np.dot(left_slice, right_slice) / denominator)
            if denominator > 1e-12
            else 0.0
        )
        if abs(correlation) > abs(best_corr):
            best_corr = correlation
            best_lag = lag
    return best_corr, best_lag / float(training.SAMPLE_RATE)


def candidate_cycle_features(window: np.ndarray) -> np.ndarray:
    series = training.build_feature_series(window)
    features: List[float] = []
    for source in CYCLE_SOURCES:
        features.extend(_cycle_series_features(series[source]))
    for left, right in CROSS_SERIES_PAIRS:
        features.extend(_max_cross_correlation(series[left], series[right]))
    return np.asarray(features, dtype=np.float32)


def build_analysis_samples(
    records: Sequence[training.ImuRecord],
    window_len: int,
    step_len: int,
    rest_threshold: float,
    active_point_threshold: float,
    progress_label: str,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray, Dict[str, int]]:
    features: List[np.ndarray] = []
    labels: List[int] = []
    file_ids: List[int] = []
    stats = {
        "too_short": 0,
        "rest_filtered": 0,
        "kept_windows": 0,
        "files_without_valid_window": 0,
    }
    for file_id, record in enumerate(records):
        if file_id == 0 or file_id % 10 == 0:
            print(
                f"features {progress_label} file={file_id + 1}/{len(records)} "
                f"kept={stats['kept_windows']}",
                flush=True,
            )
        data = training.load_imu_file(record.path)
        if len(data) < window_len:
            stats["too_short"] += 1
            continue
        record_kept = 0
        for window in training.iter_windows(data, window_len, step_len):
            if not training.keep_window_for_label(
                window,
                record.label,
                rest_threshold,
                active_point_threshold,
            ):
                stats["rest_filtered"] += 1
                continue
            features.append(
                np.concatenate(
                    [
                        training.extract_features(window),
                        candidate_event_features(window),
                        candidate_event_aligned_features(window),
                        candidate_cycle_features(window),
                    ]
                )
            )
            labels.append(record.label_idx)
            file_ids.append(file_id)
            stats["kept_windows"] += 1
            record_kept += 1
        if record_kept == 0:
            stats["files_without_valid_window"] += 1
            if record.label in training.HIGH_DYNAMIC_CLASSES:
                continue
            fallback_windows = list(training.iter_windows(data, window_len, step_len))
            if fallback_windows:
                scored = [(training.motion_score(window), window) for window in fallback_windows]
                if record.label == training.SIT_CLASS_NAME:
                    _, selected_window = min(scored, key=lambda item: item[0])
                else:
                    _, selected_window = max(scored, key=lambda item: item[0])
                features.append(
                    np.concatenate(
                        [
                            training.extract_features(selected_window),
                            candidate_event_features(selected_window),
                            candidate_event_aligned_features(selected_window),
                            candidate_cycle_features(selected_window),
                        ]
                    )
                )
                labels.append(record.label_idx)
                file_ids.append(file_id)
                stats["kept_windows"] += 1
    if not features:
        raise ValueError("No samples generated for feature analysis")
    print(
        f"features {progress_label} file={len(records)}/{len(records)} "
        f"kept={stats['kept_windows']} complete=true",
        flush=True,
    )
    return (
        np.vstack(features).astype(np.float32),
        np.asarray(labels, dtype=np.int64),
        np.asarray(file_ids, dtype=np.int64),
        stats,
    )


def fisher_scores(features: np.ndarray, labels: np.ndarray) -> np.ndarray:
    x = np.asarray(features, dtype=np.float64)
    y = np.asarray(labels, dtype=np.int64)
    overall_mean = np.mean(x, axis=0)
    between = np.zeros(x.shape[1], dtype=np.float64)
    within = np.zeros(x.shape[1], dtype=np.float64)
    for label in np.unique(y):
        group = x[y == label]
        group_mean = np.mean(group, axis=0)
        between += len(group) * np.square(group_mean - overall_mean)
        within += np.sum(np.square(group - group_mean), axis=0)
    return np.divide(
        between,
        within,
        out=np.zeros_like(between),
        where=within > 1e-12,
    )


def cohens_d(target: np.ndarray, other: np.ndarray) -> np.ndarray:
    target_values = np.asarray(target, dtype=np.float64)
    other_values = np.asarray(other, dtype=np.float64)
    target_var = np.var(target_values, axis=0, ddof=1)
    other_var = np.var(other_values, axis=0, ddof=1)
    degrees = len(target_values) + len(other_values) - 2
    if degrees <= 0:
        return np.zeros(target_values.shape[1], dtype=np.float64)
    pooled_var = (
        (len(target_values) - 1) * target_var
        + (len(other_values) - 1) * other_var
    ) / float(degrees)
    denominator = np.sqrt(np.maximum(pooled_var, 0.0))
    difference = np.mean(target_values, axis=0) - np.mean(other_values, axis=0)
    return np.divide(
        difference,
        denominator,
        out=np.zeros_like(difference),
        where=denominator > 1e-12,
    )


def stable_pair_effect(
    train_target: np.ndarray,
    train_other: np.ndarray,
    val_target: np.ndarray,
    val_other: np.ndarray,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    train_effect = cohens_d(train_target, train_other)
    val_effect = cohens_d(val_target, val_other)
    same_direction = np.sign(train_effect) == np.sign(val_effect)
    stable = np.where(
        same_direction,
        np.minimum(np.abs(train_effect), np.abs(val_effect)),
        0.0,
    )
    return stable, train_effect, val_effect


def aggregate_file_medians(
    features: np.ndarray,
    labels: np.ndarray,
    file_ids: np.ndarray,
) -> Tuple[np.ndarray, np.ndarray]:
    medians: List[np.ndarray] = []
    file_labels: List[int] = []
    for file_id in np.unique(file_ids):
        mask = file_ids == file_id
        medians.append(np.median(features[mask], axis=0))
        file_labels.append(int(labels[mask][0]))
    return np.vstack(medians).astype(np.float32), np.asarray(file_labels, dtype=np.int64)


def feature_record(
    index: int,
    names: Sequence[str],
    train_fisher: np.ndarray,
    val_fisher: np.ndarray,
    file_train_fisher: np.ndarray,
    file_val_fisher: np.ndarray,
) -> Dict[str, object]:
    return {
        "index": index,
        "name": names[index],
        "train_fisher": float(train_fisher[index]),
        "val_fisher": float(val_fisher[index]),
        "stable_fisher": float(math.sqrt(train_fisher[index] * val_fisher[index])),
        "file_train_fisher": float(file_train_fisher[index]),
        "file_val_fisher": float(file_val_fisher[index]),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="训练/验证特征类间分离度分析")
    parser.add_argument("--dataset-dir", type=Path, required=True)
    parser.add_argument("--extra-train-dir", type=Path)
    parser.add_argument("--validation-report", type=Path, required=True)
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--output-csv", type=Path, required=True)
    parser.add_argument("--target-class", default="jumping_squat")
    parser.add_argument("--pair-class", action="append", dest="pair_classes")
    parser.add_argument("--top-k", type=int, default=30)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    report = json.loads(args.validation_report.read_text(encoding="utf-8"))
    base_records, class_names, label_to_idx = training.scan_dataset(args.dataset_dir)
    extra_records = (
        training.scan_labeled_dataset(args.extra_train_dir, label_to_idx)
        if args.extra_train_dir is not None
        else []
    )
    train_records, val_records, _ = training.split_records_for_experiment(
        base_records,
        extra_records,
    )
    window_len = int(round(float(report["best_window_seconds"]) * training.SAMPLE_RATE))
    step_len = int(report["all_experiments"][0]["step_len"])
    rest_threshold = float(report["all_experiments"][0]["rest_threshold"])
    active_threshold = float(report["all_experiments"][0]["active_point_threshold"])
    train_x, train_y, train_file_ids, train_stats = build_analysis_samples(
        train_records,
        window_len,
        step_len,
        rest_threshold,
        active_threshold,
        progress_label="separability_train",
    )
    val_x, val_y, val_file_ids, val_stats = build_analysis_samples(
        val_records,
        window_len,
        step_len,
        rest_threshold,
        active_threshold,
        progress_label="separability_val",
    )

    production_feature_names = training.build_feature_names()
    feature_names = (
        production_feature_names
        + CANDIDATE_EVENT_FEATURE_NAMES
        + EVENT_ALIGNED_FEATURE_NAMES
        + CANDIDATE_CYCLE_FEATURE_NAMES
    )
    train_fisher = fisher_scores(train_x, train_y)
    val_fisher = fisher_scores(val_x, val_y)
    file_train_x, file_train_y = aggregate_file_medians(train_x, train_y, train_file_ids)
    file_val_x, file_val_y = aggregate_file_medians(val_x, val_y, val_file_ids)
    file_train_fisher = fisher_scores(file_train_x, file_train_y)
    file_val_fisher = fisher_scores(file_val_x, file_val_y)
    stable_fisher = np.sqrt(train_fisher * val_fisher)
    ranked_indices = np.argsort(-stable_fisher)

    target_idx = label_to_idx[args.target_class]
    pair_classes = args.pair_classes or DEFAULT_PAIR_CLASSES
    pair_reports: Dict[str, object] = {}
    event_start = len(production_feature_names)
    aligned_start = event_start + len(CANDIDATE_EVENT_FEATURE_NAMES)
    cycle_start = aligned_start + len(EVENT_ALIGNED_FEATURE_NAMES)
    event_indices = list(range(event_start, aligned_start))
    aligned_indices = list(range(aligned_start, cycle_start))
    cycle_indices = list(range(cycle_start, len(feature_names)))
    candidate_indices = event_indices + aligned_indices + cycle_indices
    for pair_class in pair_classes:
        other_idx = label_to_idx[pair_class]
        stable, train_effect, val_effect = stable_pair_effect(
            train_x[train_y == target_idx],
            train_x[train_y == other_idx],
            val_x[val_y == target_idx],
            val_x[val_y == other_idx],
        )
        file_stable, file_train_effect, file_val_effect = stable_pair_effect(
            file_train_x[file_train_y == target_idx],
            file_train_x[file_train_y == other_idx],
            file_val_x[file_val_y == target_idx],
            file_val_x[file_val_y == other_idx],
        )
        pair_ranked = np.argsort(-stable)

        def effect_record(index: int) -> Dict[str, object]:
            return {
                "index": int(index),
                "name": feature_names[index],
                "stable_abs_effect": float(stable[index]),
                "train_effect": float(train_effect[index]),
                "val_effect": float(val_effect[index]),
                "stable_file_abs_effect": float(file_stable[index]),
                "file_train_effect": float(file_train_effect[index]),
                "file_val_effect": float(file_val_effect[index]),
            }

        pair_reports[pair_class] = {
            "top_features": [effect_record(int(i)) for i in pair_ranked[: args.top_k]],
            "event_features": [
                effect_record(i)
                for i in sorted(
                    range(event_start, cycle_start),
                    key=lambda item: stable[item],
                    reverse=True,
                )
            ],
            "cycle_features": [
                effect_record(i)
                for i in sorted(cycle_indices, key=lambda item: stable[item], reverse=True)
            ],
            "event_aligned_features": [
                effect_record(i)
                for i in sorted(aligned_indices, key=lambda item: stable[item], reverse=True)
            ],
            "event_features_stable_ge_0_5": int(
                np.sum(stable[event_start:cycle_start] >= 0.5)
            ),
        }

    correlation = np.corrcoef(train_x, rowvar=False)
    candidate_novelty = []
    for index in candidate_indices:
        prior_correlations = np.abs(correlation[index, :event_start])
        finite_correlations = prior_correlations[np.isfinite(prior_correlations)]
        candidate_novelty.append(
            {
                "index": index,
                "name": feature_names[index],
                "max_abs_correlation_with_prior_features": float(
                    np.max(finite_correlations) if len(finite_correlations) else 0.0
                ),
            }
        )

    weak_confusion_effects: Dict[str, object] = {}
    for target_name, other_name in DEFAULT_WEAK_CONFUSION_PAIRS:
        target_label = label_to_idx[target_name]
        other_label = label_to_idx[other_name]
        stable, train_effect, val_effect = stable_pair_effect(
            train_x[train_y == target_label],
            train_x[train_y == other_label],
            val_x[val_y == target_label],
            val_x[val_y == other_label],
        )
        file_stable, file_train_effect, file_val_effect = stable_pair_effect(
            file_train_x[file_train_y == target_label],
            file_train_x[file_train_y == other_label],
            file_val_x[file_val_y == target_label],
            file_val_x[file_val_y == other_label],
        )
        robust_effect = np.minimum(stable, file_stable)
        ranked_candidates = sorted(
            candidate_indices, key=lambda index: robust_effect[index], reverse=True
        )
        weak_confusion_effects[f"{target_name}__vs__{other_name}"] = [
            {
                "index": int(index),
                "name": feature_names[index],
                "robust_abs_effect": float(robust_effect[index]),
                "window_stable_abs_effect": float(stable[index]),
                "file_stable_abs_effect": float(file_stable[index]),
                "train_effect": float(train_effect[index]),
                "val_effect": float(val_effect[index]),
                "file_train_effect": float(file_train_effect[index]),
                "file_val_effect": float(file_val_effect[index]),
            }
            for index in ranked_candidates
        ]

    top_features = [
        feature_record(
            int(index),
            feature_names,
            train_fisher,
            val_fisher,
            file_train_fisher,
            file_val_fisher,
        )
        for index in ranked_indices[: args.top_k]
    ]
    def ranked_feature_records(indices: Sequence[int]) -> List[Dict[str, object]]:
        return [
            feature_record(
                index,
                feature_names,
                train_fisher,
                val_fisher,
                file_train_fisher,
                file_val_fisher,
            )
            for index in sorted(indices, key=lambda item: stable_fisher[item], reverse=True)
        ]

    candidate_class_distributions: Dict[str, object] = {}
    for index in candidate_indices:
        per_class = {}
        for label, label_idx in label_to_idx.items():
            train_values = train_x[train_y == label_idx, index]
            val_values = val_x[val_y == label_idx, index]
            train_file_values = file_train_x[file_train_y == label_idx, index]
            val_file_values = file_val_x[file_val_y == label_idx, index]
            per_class[label] = {
                "train_median": float(np.median(train_values)),
                "train_iqr": float(np.percentile(train_values, 75) - np.percentile(train_values, 25)),
                "val_median": float(np.median(val_values)),
                "val_iqr": float(np.percentile(val_values, 75) - np.percentile(val_values, 25)),
                "train_file_median": float(np.median(train_file_values)),
                "val_file_median": float(np.median(val_file_values)),
            }
        candidate_class_distributions[feature_names[index]] = per_class

    event_features = ranked_feature_records(event_indices)
    event_aligned_features = ranked_feature_records(aligned_indices)
    cycle_features = ranked_feature_records(cycle_indices)
    result = {
        "scope": "train_validation_only",
        "test_read": False,
        "external_holdout_read": False,
        "target_class": args.target_class,
        "class_names": class_names,
        "feature_count": len(feature_names),
        "event_feature_count": len(CANDIDATE_EVENT_FEATURE_NAMES),
        "event_aligned_feature_count": len(EVENT_ALIGNED_FEATURE_NAMES),
        "cycle_feature_count": len(cycle_indices),
        "train_window_count": len(train_x),
        "val_window_count": len(val_x),
        "train_file_count": len(train_records),
        "val_file_count": len(val_records),
        "train_stats": train_stats,
        "val_stats": val_stats,
        "top_features": top_features,
        "event_features": event_features,
        "event_aligned_features": event_aligned_features,
        "cycle_features": cycle_features,
        "candidate_novelty": candidate_novelty,
        "candidate_class_distributions": candidate_class_distributions,
        "target_pair_effects": pair_reports,
        "weak_confusion_effects": weak_confusion_effects,
    }
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(
        json.dumps(result, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.output_csv.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(top_features[0].keys()))
        writer.writeheader()
        writer.writerows(top_features)
    print(
        f"SEPARABILITY_OK train={len(train_x)} val={len(val_x)} "
        f"features={len(feature_names)} output={args.output_json}",
        flush=True,
    )


if __name__ == "__main__":
    main()
