"""生成计数状态机示意曲线和真板权威计数事件回放图。"""

# argparse 解析现场 CSV、会话号、输出目录和清单路径。
import argparse
# csv 使用标准库读取上位机导出的中文列名，不引入 pandas 依赖。
import csv
# hashlib 记录现场日志 SHA-256，保证图片来源可审计。
import hashlib
# json 写出示意常量、现场会话摘要和实现来源。
import json
# dataclass 保存去标识后的现场样本和权威事件。
from dataclasses import dataclass
# Path 统一处理 Windows 与其它平台路径。
from pathlib import Path

# matplotlib 使用非交互后端，允许在 CI 或没有桌面的环境生成教程图片。
import matplotlib

# Agg 只写图片文件，不创建桌面窗口。
matplotlib.use("Agg")
# pyplot 创建双面板曲线、阈值和事件标记。
import matplotlib.pyplot as plt
# NumPy 负责向量模长、示意波形和数组计算。
import numpy as np


# 固件采样周期固定为 40 ms，即 25 Hz。
SAMPLE_PERIOD_MS = 40
# 两相位方向学习下限来自 motion_phase.h，单位 deg/s。
DIRECTION_LEARN_DPS = 26.0
# 自适应端点门最小值来自 motion_phase.h，单位 deg/s。
DIRECTION_ACTIVE_MIN_DPS = 27.0
# 自适应端点门最大值来自 motion_phase.h，单位 deg/s。
DIRECTION_ACTIVE_MAX_DPS = 72.0
# 端点门使用活动角速度包络的 45%。
DIRECTION_ACTIVE_RATIO = 0.45
# 包络遇到新高值时按 25% 快速上升。
SCALE_RISE_ALPHA = 0.25
# 包络遇到较低值时按 1% 缓慢下降。
SCALE_FALL_ALPHA = 0.01
# 行走/小跑局部峰必须高于慢基线 0.16 g。
STEP_PEAK_DELTA_G = 0.16
# 步峰接受后必须回落到慢基线才重新武装。
STEP_REARM_DELTA_G = 0.0

# 上位机中文 CSV 的稳定列名集中定义，避免散落字符串写错。
COL_MONOTONIC_MS = "设备单调时间（毫秒）"
COL_SESSION = "会话序号"
COL_STATE = "设备状态"
COL_ACTION = "设备稳定动作"
COL_GX = "角速度横轴（度每秒）"
COL_GY = "角速度纵轴（度每秒）"
COL_GZ = "角速度垂直轴（度每秒）"
COL_AX = "加速度横轴（重力倍数）"
COL_AY = "加速度纵轴（重力倍数）"
COL_AZ = "加速度垂直轴（重力倍数）"
COL_TOTAL = "权威累计值"
COL_EVENT_MARKER = "是否计数标记点"
COL_EVENT_TIME_MS = "计数事件设备时间（毫秒）"
COL_EVENT_TOTAL = "计数后累计值"
COL_EVENT_ACTION = "计数动作"


@dataclass(frozen=True)
class FieldCountEvent:
    """保存一条不含设备标识的权威计数事件。"""

    # relative_seconds 是事件相对本会话第一行的设备单调秒数。
    relative_seconds: float
    # total_value 是本事件发生后的权威累计次数。
    total_value: int
    # action 是事件携带的本轮主动作中文名。
    action: str


@dataclass(frozen=True)
class FieldSession:
    """保存画图所需的去标识现场会话数组。"""

    # relative_seconds 形状 [N]，单位 s。
    relative_seconds: np.ndarray
    # gyro_magnitude_dps 形状 [N]，单位 deg/s。
    gyro_magnitude_dps: np.ndarray
    # acceleration_magnitude_g 形状 [N]，单位 g。
    acceleration_magnitude_g: np.ndarray
    # authoritative_total 形状 [N]，表示设备导出的累计事实。
    authoritative_total: np.ndarray
    # states 形状 [N]，保存识别准备、训练中或训练完成。
    states: tuple[str, ...]
    # stable_actions 形状 [N]，保存设备稳定动作显示。
    stable_actions: tuple[str, ...]
    # events 保存本会话全部权威计数事件。
    events: tuple[FieldCountEvent, ...]


def sha256_file(path: Path) -> str:
    """按二进制内容计算文件 SHA-256。"""

    # 哈希对象从空状态开始。
    digest = hashlib.sha256()
    # 二进制读取避免换行和编码转换改变源日志。
    with path.open("rb") as stream:
        # 每次读取 1 MiB；空块表示文件结束。
        while chunk := stream.read(1024 * 1024):
            # 把当前块加入累计哈希。
            digest.update(chunk)
    # 返回 64 位小写十六进制摘要。
    return digest.hexdigest()


def parse_required_float(row: dict[str, str], column: str) -> float:
    """从 CSV 行读取必需有限浮点值。"""

    # float 支持当前 CSV 的十进制文本。
    value = float(row[column])
    # 非有限物理量不能进入公开曲线。
    if not np.isfinite(value):
        # 报告具体列名，便于定位导出损坏。
        raise ValueError(f"列 {column} 包含非有限值：{row[column]}")
    # 返回已验证有限值。
    return value


def load_field_session(path: Path, session_id: int) -> FieldSession:
    """读取一个指定会话，转换为相对时间、物理模长和权威事件。"""

    # UTF-8-sig 同时接受带或不带 UTF-8 BOM 的上位机 CSV。
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        # DictReader 使用第一行中文表头映射每个字段。
        reader = csv.DictReader(stream)
        # 表头缺失时无法定位物理量和事件。
        if reader.fieldnames is None:
            # 明确阻止空 CSV。
            raise ValueError(f"CSV 缺少表头：{path}")
        # 固定列集合覆盖时间、六轴、状态、累计和计数事件。
        required_columns = {
            COL_MONOTONIC_MS,
            COL_SESSION,
            COL_STATE,
            COL_ACTION,
            COL_GX,
            COL_GY,
            COL_GZ,
            COL_AX,
            COL_AY,
            COL_AZ,
            COL_TOTAL,
            COL_EVENT_MARKER,
            COL_EVENT_TIME_MS,
            COL_EVENT_TOTAL,
            COL_EVENT_ACTION,
        }
        # 计算当前导出缺少的列。
        missing_columns = sorted(required_columns.difference(reader.fieldnames))
        # 任一关键列缺失都不能生成部分真相图。
        if missing_columns:
            # 报告全部缺失列。
            raise ValueError(f"CSV 缺少列：{missing_columns}")
        # 只保留用户指定会话，忽略空闲和其它测试轮。
        rows = [row for row in reader if int(row[COL_SESSION] or 0) == session_id]
    # 会话号不存在时阻止生成空图。
    if not rows:
        # 报告具体会话号和文件名。
        raise ValueError(f"CSV 中没有会话 {session_id}：{path.name}")
    # 第一行设备单调时间作为相对零点，不公开 UTC 接收时间。
    start_ms = int(rows[0][COL_MONOTONIC_MS])
    # relative_seconds 保存 [N] 相对秒数。
    relative_seconds = np.asarray(
        [(int(row[COL_MONOTONIC_MS]) - start_ms) / 1000.0 for row in rows],
        dtype=np.float64,
    )
    # 时间必须严格递增，防止曲线回折。
    if np.any(np.diff(relative_seconds) <= 0.0):
        # 乱序或重复点必须先修复导出，不在图中排序掩盖。
        raise ValueError(f"会话 {session_id} 的设备时间不是严格递增")
    # gyro_axes 形状 [N,3]，单位 deg/s，顺序 gx、gy、gz。
    gyro_axes = np.asarray(
        [
            [
                parse_required_float(row, COL_GX),
                parse_required_float(row, COL_GY),
                parse_required_float(row, COL_GZ),
            ]
            for row in rows
        ],
        dtype=np.float64,
    )
    # acceleration_axes 形状 [N,3]，单位 g，顺序 ax、ay、az。
    acceleration_axes = np.asarray(
        [
            [
                parse_required_float(row, COL_AX),
                parse_required_float(row, COL_AY),
                parse_required_float(row, COL_AZ),
            ]
            for row in rows
        ],
        dtype=np.float64,
    )
    # 三轴欧氏模长消除当前图对固定轴方向的依赖。
    gyro_magnitude_dps = np.linalg.norm(gyro_axes, axis=1)
    # 加速度模长用于观察运动冲击和静止时接近 1 g 的状态。
    acceleration_magnitude_g = np.linalg.norm(acceleration_axes, axis=1)
    # 权威累计值由设备导出，空字符串按 0 处理。
    authoritative_total = np.asarray(
        [int(row[COL_TOTAL] or 0) for row in rows],
        dtype=np.int64,
    )
    # states 按时间保存设备状态，不含电脑接收时间和设备标识。
    states = tuple(row[COL_STATE] for row in rows)
    # stable_actions 保存设备稳定动作，供清单核对本会话主动作。
    stable_actions = tuple(row[COL_ACTION] for row in rows)
    # events 暂存已标记的权威计数事件。
    events: list[FieldCountEvent] = []
    # 遍历会话行并提取“是”的计数标记点。
    for row in rows:
        # 非标记行不产生事件。
        if row[COL_EVENT_MARKER] != "是":
            # 继续下一行。
            continue
        # 事件设备时间必须存在，才能放到原始曲线上。
        if not row[COL_EVENT_TIME_MS]:
            # 标记行缺时间表示导出合同损坏。
            raise ValueError("计数标记行缺少事件设备时间")
        # 保存相对时刻、累计值和动作。
        events.append(
            FieldCountEvent(
                # 事件时间使用设备原始单调时刻，不使用 BLE 到达时刻。
                relative_seconds=(int(row[COL_EVENT_TIME_MS]) - start_ms) / 1000.0,
                # 累计值必须是正整数。
                total_value=int(row[COL_EVENT_TOTAL]),
                # 动作来自权威事件负载。
                action=row[COL_EVENT_ACTION],
            )
        )
    # 至少一条事件才能作为计数回放示例。
    if not events:
        # 无事件会话应使用其它诊断图，不能标为计数示例。
        raise ValueError(f"会话 {session_id} 没有权威计数事件")
    # 事件累计值必须从 1 连续递增，证明没有重复或漏序号投影。
    event_totals = [event.total_value for event in events]
    # 期望序列为 1..N。
    if event_totals != list(range(1, len(events) + 1)):
        # 不连续时保留日志分析，不进入教程成功案例。
        raise ValueError(f"会话 {session_id} 的事件累计不连续：{event_totals}")
    # 返回去标识现场数据。
    return FieldSession(
        relative_seconds=relative_seconds,
        gyro_magnitude_dps=gyro_magnitude_dps,
        acceleration_magnitude_g=acceleration_magnitude_g,
        authoritative_total=authoritative_total,
        states=states,
        stable_actions=stable_actions,
        events=tuple(events),
    )


def configure_plot_style() -> None:
    """配置中文教程图的字体、颜色和坐标轴风格。"""

    # 依次尝试 Windows 中文字体、常见 Linux 中文字体和回退字体。
    plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "Noto Sans CJK SC", "SimHei", "DejaVu Sans"]
    # 保持负号正常显示。
    plt.rcParams["axes.unicode_minus"] = False
    # 正文使用深石墨色。
    plt.rcParams["text.color"] = "#14213D"
    # 坐标轴标签使用同一深色。
    plt.rcParams["axes.labelcolor"] = "#14213D"
    # 标题半粗体建立层级。
    plt.rcParams["axes.titleweight"] = "semibold"
    # 横轴刻度使用中性灰。
    plt.rcParams["xtick.color"] = "#52606D"
    # 纵轴刻度使用中性灰。
    plt.rcParams["ytick.color"] = "#52606D"


def style_axis(axis: plt.Axes) -> None:
    """统一一个坐标轴的卡片背景、网格和边框。"""

    # 白色卡片与浅灰画布分离。
    axis.set_facecolor("#FFFFFF")
    # 只绘制水平网格，帮助读取阈值与累计值。
    axis.grid(axis="y", color="#D9E2EC", linewidth=0.8, alpha=0.75)
    # 隐藏上边框。
    axis.spines["top"].set_visible(False)
    # 隐藏右边框。
    axis.spines["right"].set_visible(False)
    # 左边框使用浅灰。
    axis.spines["left"].set_color("#CBD5E1")
    # 下边框使用浅灰。
    axis.spines["bottom"].set_color("#CBD5E1")


def build_repetition_schematic() -> tuple[np.ndarray, np.ndarray, np.ndarray, list[float], np.ndarray]:
    """生成不同节奏和幅度的往返示意，不代表某次真板测量。"""

    # 四个完整周期使用不同持续时间，覆盖慢快差异。
    durations = [1.45, 1.95, 1.30, 2.20]
    # 四个周期使用不同角速度幅度，覆盖深浅和疲劳差异。
    amplitudes = [78.0, 112.0, 62.0, 94.0]
    # 开始前保留 0.6 秒静止。
    cursor = 0.6
    # cycle_ranges 保存每个完整周期的起止时间。
    cycle_ranges: list[tuple[float, float, float]] = []
    # 依次安排四个周期，中间不插入会被误计的虚假动作。
    for duration, amplitude in zip(durations, amplitudes, strict=True):
        # 当前周期从 cursor 开始。
        start = cursor
        # 周期结束时间由个体节奏决定。
        end = start + duration
        # 保存起止和幅度。
        cycle_ranges.append((start, end, amplitude))
        # 周期间保留 0.22 秒自然换气。
        cursor = end + 0.22
    # 完成四次后追加 3.0 秒休息。
    total_seconds = cursor + 3.0
    # 以真实 25 Hz 建立时间轴。
    time_seconds = np.arange(0.0, total_seconds, SAMPLE_PERIOD_MS / 1000.0)
    # projected_dps 是沿在线主方向的示意角速度投影。
    projected_dps = np.zeros_like(time_seconds)
    # motion_active 标记每个样本是否属于完整周期。
    motion_active = np.zeros_like(time_seconds, dtype=bool)
    # event_times 保存每个闭合周期的 +1 时刻。
    event_times: list[float] = []
    # 逐周期填入不同幅度、不同节奏的余弦往返。
    for start, end, amplitude in cycle_ranges:
        # mask 选择当前周期时间点。
        mask = (time_seconds >= start) & (time_seconds <= end)
        # phase 从 0 走到 1：正主向、负回向、返回正主向。
        phase = (time_seconds[mask] - start) / (end - start)
        # 余弦波只用于解释 PRIMARY/SECONDARY/闭合顺序。
        projected_dps[mask] = amplitude * np.cos(2.0 * np.pi * phase)
        # 当前周期内活动门保持打开。
        motion_active[mask] = True
        # 周期闭合时产生一次权威候选。
        event_times.append(end)
    # envelope 保存按生产公式更新的活动幅度包络。
    envelope = np.zeros_like(time_seconds)
    # 初值为最低方向学习幅值，避免示意门从 0 开始。
    current_scale = DIRECTION_LEARN_DPS
    # 遍历每个 25 Hz 点并更新包络。
    for index, value in enumerate(np.abs(projected_dps)):
        # 休息点不参与方向和幅度学习，保持当前包络。
        if not motion_active[index]:
            # 保存未变的包络。
            envelope[index] = current_scale
            # 继续下一点。
            continue
        # 新幅度高于包络时快速上升，否则缓慢下降。
        alpha = SCALE_RISE_ALPHA if value > current_scale else SCALE_FALL_ALPHA
        # 一阶指数更新单位仍为 deg/s。
        current_scale += alpha * (value - current_scale)
        # 保存当前包络。
        envelope[index] = current_scale
    # threshold 按 45% 比例并限制在 27..72 deg/s。
    threshold = np.clip(
        DIRECTION_ACTIVE_RATIO * envelope,
        DIRECTION_ACTIVE_MIN_DPS,
        DIRECTION_ACTIVE_MAX_DPS,
    )
    # cumulative 只在 event_times 处增加，休息段保持不变。
    cumulative = np.zeros_like(time_seconds)
    # 对每个闭合事件更新后续累计值。
    for event_index, event_time in enumerate(event_times, start=1):
        # 事件及之后全部显示当前累计。
        cumulative[time_seconds >= event_time] = event_index
    # 返回示意时间、投影、阈值、事件和累计。
    return time_seconds, projected_dps, threshold, event_times, cumulative


def build_step_schematic() -> tuple[np.ndarray, np.ndarray, np.ndarray, list[float], np.ndarray]:
    """生成带宽峰和不同步幅的步峰触发—回落示意。"""

    # 以 25 Hz 建立 8 秒步行示意。
    time_seconds = np.arange(0.0, 8.0, SAMPLE_PERIOD_MS / 1000.0)
    # 慢基线围绕 1 g 小幅漂移，模拟佩戴姿态变化。
    baseline_g = 1.0 + 0.025 * np.sin(2.0 * np.pi * time_seconds / 7.0)
    # dynamic_g 保存去基线后的脚步冲击。
    dynamic_g = np.zeros_like(time_seconds)
    # 五个步峰时间不等间隔，表示自然步速差异。
    peak_times = [0.95, 2.05, 3.20, 4.75, 6.10]
    # 五个峰幅不同，最后一个宽峰包含一个次级波瓣。
    amplitudes = [0.23, 0.31, 0.20, 0.37, 0.29]
    # 累加每个高斯脚步峰。
    for peak_time, amplitude in zip(peak_times, amplitudes, strict=True):
        # 主峰宽度 0.11 秒，形成单个局部最大值。
        dynamic_g += amplitude * np.exp(-0.5 * ((time_seconds - peak_time) / 0.11) ** 2)
    # 在最后一步后添加未回落前的低幅次级波瓣，不能计成新步。
    dynamic_g += 0.11 * np.exp(-0.5 * ((time_seconds - 6.32) / 0.10) ** 2)
    # acceleration_magnitude_g 是慢基线与动态冲击之和。
    acceleration_magnitude_g = baseline_g + dynamic_g
    # events 保存施密特触发—回落逻辑接受的局部峰。
    events: list[float] = []
    # armed 表示已回落到动态量 <= 0，可接受下一高峰。
    armed = True
    # 三点局部峰从第二个样本开始检查。
    for index in range(1, len(time_seconds) - 1):
        # 当前点回落到基线附近时重新武装。
        if dynamic_g[index] <= STEP_REARM_DELTA_G + 0.005:
            # 允许下一独立峰。
            armed = True
        # 中间点高于左侧且不低于右侧，构成局部峰。
        local_peak = (
            dynamic_g[index] > dynamic_g[index - 1]
            and dynamic_g[index] >= dynamic_g[index + 1]
        )
        # 只有已武装且峰值达到 0.16 g 才接受。
        if armed and local_peak and dynamic_g[index] >= STEP_PEAK_DELTA_G:
            # 保存事件时间。
            events.append(float(time_seconds[index]))
            # 锁住当前物理步，等待真正回落。
            armed = False
    # cumulative 在每个接受峰后增加 1。
    cumulative = np.zeros_like(time_seconds)
    # 依次写入步数阶梯。
    for event_index, event_time in enumerate(events, start=1):
        # 事件及之后显示当前累计步数。
        cumulative[time_seconds >= event_time] = event_index
    # 返回时间、动态量、基线、事件和累计。
    return time_seconds, dynamic_g, baseline_g, events, cumulative


def plot_counting_schematic(output_path: Path) -> None:
    """绘制重复动作相位和行走步峰两种生产计数路径。"""

    # 应用统一中文字体与颜色。
    configure_plot_style()
    # 生成重复动作示意。
    rep_time, projection, threshold, rep_events, rep_total = build_repetition_schematic()
    # 生成步峰示意。
    step_time, dynamic_g, baseline_g, step_events, step_total = build_step_schematic()
    # 创建上下两个面板，输出 1920×1200。
    figure, axes = plt.subplots(2, 1, figsize=(16, 10), dpi=120)
    # 整体使用近白背景。
    figure.patch.set_facecolor("#F7F9FC")
    # 两个坐标轴采用同一卡片样式。
    for axis in axes:
        # 应用网格和边框。
        style_axis(axis)
        # 横轴都是相对时间秒。
        axis.set_xlabel("相对时间（秒）")
    # 上图绘制沿在线主方向的有符号角速度投影。
    axes[0].plot(rep_time, projection, color="#2563EB", linewidth=2.1, label=r"有符号投影 $p_t$")
    # 正阈值表示 PRIMARY 门。
    axes[0].plot(rep_time, threshold, color="#10B981", linestyle="--", linewidth=1.6, label=r"主向门 $+H_t$")
    # 负阈值表示 SECONDARY 门。
    axes[0].plot(rep_time, -threshold, color="#F97316", linestyle="--", linewidth=1.6, label=r"回向门 $-H_t$")
    # 休息区域从最后一次事件后 0.22 秒开始。
    rest_start = rep_events[-1] + 0.22
    # 灰色背景强调休息段不再增加次数。
    axes[0].axvspan(rest_start, rep_time[-1], color="#E2E8F0", alpha=0.65, label="休息：计数冻结")
    # 标出每个完整闭合产生的一次事件。
    for index, event_time in enumerate(rep_events, start=1):
        # 红色竖线表示唯一 +1 时刻。
        axes[0].axvline(event_time, color="#DC2626", linewidth=1.2, alpha=0.85)
        # 在线顶端标出累计值。
        axes[0].text(event_time, 122.0, f"+1 → {index}", ha="center", va="top", fontsize=9, color="#B91C1C")
    # 次坐标轴显示累计次数阶梯。
    rep_total_axis = axes[0].twinx()
    # 阶梯只在闭合事件处变化。
    rep_total_axis.step(rep_time, rep_total, where="post", color="#7C3AED", linewidth=1.8, alpha=0.85, label="权威累计")
    # 次轴显示 0..5。
    rep_total_axis.set_ylim(-0.2, len(rep_events) + 0.8)
    # 次轴标签说明是次数。
    rep_total_axis.set_ylabel("累计次数")
    # 次轴右边框使用浅灰。
    rep_total_axis.spines["right"].set_color("#CBD5E1")
    # 上图标题说明是完整往返状态机。
    axes[0].set_title("重复动作：PRIMARY → SECONDARY → 闭合后才 +1")
    # 主轴单位 deg/s。
    axes[0].set_ylabel("角速度投影（deg/s）")
    # 给不同幅度曲线留出标注空间。
    axes[0].set_ylim(-135.0, 135.0)
    # 合并主轴和次轴图例。
    rep_handles, rep_labels = axes[0].get_legend_handles_labels()
    # 读取累计轴图例。
    total_handles, total_labels = rep_total_axis.get_legend_handles_labels()
    # 图例放在左上，避开后半休息说明。
    axes[0].legend(rep_handles + total_handles, rep_labels + total_labels, loc="upper left", ncol=3, frameon=False)
    # 下图绘制去慢基线后的动态加速度。
    axes[1].plot(step_time, dynamic_g, color="#0F766E", linewidth=2.1, label=r"动态量 $d_t=A_t-b_t$")
    # 高门固定为 0.16 g。
    axes[1].axhline(STEP_PEAK_DELTA_G, color="#F97316", linestyle="--", linewidth=1.6, label="触发门 0.16 g")
    # 回落到 0 附近后才重新武装。
    axes[1].axhline(STEP_REARM_DELTA_G, color="#64748B", linestyle=":", linewidth=1.5, label="回落/重武装门 0 g")
    # 标出每个接受的步峰。
    for index, event_time in enumerate(step_events, start=1):
        # 红点定位局部峰。
        event_index = int(np.argmin(np.abs(step_time - event_time)))
        # 在峰值处画红色圆点。
        axes[1].scatter([event_time], [dynamic_g[event_index]], color="#DC2626", s=38, zorder=5)
        # 标注累计步数。
        axes[1].text(event_time, dynamic_g[event_index] + 0.025, str(index), ha="center", va="bottom", fontsize=9, color="#B91C1C")
    # 次轴显示累计步数。
    step_total_axis = axes[1].twinx()
    # 阶梯只在接受峰后改变。
    step_total_axis.step(step_time, step_total, where="post", color="#7C3AED", linewidth=1.8, alpha=0.85, label="权威步数")
    # 次轴上界留出图例空间。
    step_total_axis.set_ylim(-0.2, len(step_events) + 0.8)
    # 标明单位为步。
    step_total_axis.set_ylabel("累计步数")
    # 次轴右边框使用浅灰。
    step_total_axis.spines["right"].set_color("#CBD5E1")
    # 下图标题说明峰触发后必须回落。
    axes[1].set_title("行走/小跑：局部峰触发，回落到慢基线后才允许下一步")
    # 主轴单位 g。
    axes[1].set_ylabel("相对慢基线的动态加速度（g）")
    # 给负噪声和峰标注留空间。
    axes[1].set_ylim(-0.04, 0.44)
    # 合并步峰主轴与累计轴图例。
    step_handles, step_labels = axes[1].get_legend_handles_labels()
    # 读取步数轴图例。
    step_total_handles, step_total_labels = step_total_axis.get_legend_handles_labels()
    # 图例放在左上。
    axes[1].legend(
        step_handles + step_total_handles,
        step_labels + step_total_labels,
        loc="upper left",
        ncol=4,
        frameon=False,
    )
    # 总标题明确这是一张算法示意，不是真板测量。
    figure.suptitle(
        "实时计数算法示意曲线（非实测）",
        fontsize=18,
        fontweight="bold",
        x=0.06,
        ha="left",
        y=0.975,
    )
    # 副标题说明示意只改变节奏和幅度，不改变生产阈值语义。
    figure.text(
        0.06,
        0.938,
        "不同幅度和节奏用于说明个体差异；阈值、包络比例、触发—回落与事件规则来自当前固件。",
        fontsize=11,
        color="#52606D",
        ha="left",
    )
    # 页脚给出当前生产实现来源。
    figure.text(
        0.06,
        0.018,
        "实现来源：esp32/firmware/components/motion_phase 与 fitness_core。"
        "示意曲线不用于报告模型准确率或真板计数结果。",
        fontsize=10,
        color="#64748B",
        ha="left",
    )
    # 留出标题、副标题和页脚空间。
    figure.subplots_adjust(left=0.07, right=0.93, top=0.88, bottom=0.09, hspace=0.34)
    # 创建用户指定输出目录。
    output_path.parent.mkdir(parents=True, exist_ok=True)
    # 固定 PNG 元数据，不写当前时间。
    figure.savefig(
        output_path,
        dpi=120,
        facecolor=figure.get_facecolor(),
        metadata={"Software": "ESP32智慧运动助手计数曲线生成器"},
    )
    # 释放图形对象。
    plt.close(figure)


def state_spans(time_seconds: np.ndarray, states: tuple[str, ...]) -> list[tuple[float, float, str]]:
    """把逐点设备状态压缩为连续时间区间。"""

    # 至少有一个状态点。
    if len(states) == 0:
        # 空输入返回空区间。
        return []
    # spans 保存 [start,end,state]。
    spans: list[tuple[float, float, str]] = []
    # 当前区间从第一个点开始。
    start_index = 0
    # 从第二个状态开始查找变化。
    for index in range(1, len(states)):
        # 状态未变化时继续扩展当前区间。
        if states[index] == states[start_index]:
            # 继续下一点。
            continue
        # 状态变化时用当前点时间结束上一区间。
        spans.append((float(time_seconds[start_index]), float(time_seconds[index]), states[start_index]))
        # 新区间从当前点开始。
        start_index = index
    # 最后区间延伸到最后一个样本。
    spans.append((float(time_seconds[start_index]), float(time_seconds[-1]), states[start_index]))
    # 返回压缩区间。
    return spans


def plot_field_session(
    session: FieldSession,
    session_id: int,
    source_hash: str,
    output_path: Path,
) -> None:
    """绘制真板物理曲线、设备状态和权威累计事件。"""

    # 应用统一中文绘图风格。
    configure_plot_style()
    # 创建上下两个面板。
    figure, axes = plt.subplots(2, 1, figsize=(16, 10), dpi=120, sharex=True)
    # 整体背景使用近白色。
    figure.patch.set_facecolor("#F7F9FC")
    # 两个坐标轴采用统一卡片样式。
    for axis in axes:
        # 应用网格和边框。
        style_axis(axis)
    # 为不同设备状态配置轻量背景色。
    state_colors = {
        "识别准备": "#DBEAFE",
        "训练中": "#D1FAE5",
        "训练完成": "#E2E8F0",
        "已暂停": "#FEF3C7",
    }
    # 压缩并绘制连续状态区间。
    for start, end, state in state_spans(session.relative_seconds, session.states):
        # 未知状态使用浅灰，不隐式解释。
        color = state_colors.get(state, "#F1F5F9")
        # 两个面板使用同一状态背景。
        for axis in axes:
            # 半透明背景不遮挡物理曲线。
            axis.axvspan(start, end, color=color, alpha=0.45)
    # 上图绘制角速度模长。
    axes[0].plot(
        session.relative_seconds,
        session.gyro_magnitude_dps,
        color="#2563EB",
        linewidth=1.6,
        label=r"角速度模长 $\|\mathbf{g}_t\|$",
    )
    # 上图主轴单位 deg/s。
    axes[0].set_ylabel("角速度模长（deg/s）")
    # 上图标题说明物理曲线和状态背景。
    axes[0].set_title("真板六轴运动强度与设备状态")
    # 建立加速度模长次轴。
    acceleration_axis = axes[0].twinx()
    # 绘制加速度模长。
    acceleration_axis.plot(
        session.relative_seconds,
        session.acceleration_magnitude_g,
        color="#F97316",
        linewidth=1.3,
        alpha=0.82,
        label=r"加速度模长 $\|\mathbf{a}_t\|$",
    )
    # 次轴单位 g。
    acceleration_axis.set_ylabel("加速度模长（g）")
    # 次轴右边框使用浅灰。
    acceleration_axis.spines["right"].set_color("#CBD5E1")
    # 合并两个物理曲线图例。
    gyro_handles, gyro_labels = axes[0].get_legend_handles_labels()
    # 读取加速度图例。
    acceleration_handles, acceleration_labels = acceleration_axis.get_legend_handles_labels()
    # 图例放在右上。
    axes[0].legend(
        gyro_handles + acceleration_handles,
        gyro_labels + acceleration_labels,
        loc="upper right",
        ncol=2,
        frameon=False,
    )
    # 下图绘制设备导出的权威累计阶梯。
    axes[1].step(
        session.relative_seconds,
        session.authoritative_total,
        where="post",
        color="#7C3AED",
        linewidth=2.4,
        label="设备权威累计",
    )
    # 每条事件用红色竖线和圆点标记。
    for event in session.events:
        # 竖线表示原始 MetricEvent.monotonic_ms。
        axes[1].axvline(event.relative_seconds, color="#DC2626", linewidth=1.2, alpha=0.85)
        # 圆点定位增加后的累计值。
        axes[1].scatter(
            [event.relative_seconds],
            [event.total_value],
            color="#DC2626",
            s=42,
            zorder=5,
        )
        # 标注每次只增加 1。
        axes[1].text(
            event.relative_seconds,
            event.total_value + 0.18,
            f"+1 → {event.total_value}",
            ha="center",
            va="bottom",
            fontsize=9,
            color="#B91C1C",
        )
    # 下图标题强调事件后累计变化，尾段休息保持平坦。
    axes[1].set_title("MetricEvent 到达才更新累计；尾段停止运动后保持不变")
    # 纵轴是次数。
    axes[1].set_ylabel("累计次数")
    # 横轴是相对设备时间，不公开 UTC。
    axes[1].set_xlabel("相对设备时间（秒）")
    # 纵轴使用整数刻度。
    axes[1].set_yticks(range(0, len(session.events) + 1))
    # 给 +1 文本留出空间。
    axes[1].set_ylim(-0.25, len(session.events) + 0.8)
    # 显示累计图例。
    axes[1].legend(loc="lower right", frameon=False)
    # 从权威事件读取动作；当前示例应全部相同。
    event_actions = sorted({event.action for event in session.events})
    # 组合动作名，异常多动作会在标题可见。
    action_text = "/".join(event_actions)
    # 总标题标明真板、会话和权威事件数。
    figure.suptitle(
        f"真板计数事件回放：会话 {session_id}，{action_text}，{len(session.events)} 次",
        fontsize=18,
        fontweight="bold",
        x=0.06,
        ha="left",
        y=0.975,
    )
    # 副标题解释背景色和数据来源。
    figure.text(
        0.06,
        0.938,
        "蓝色背景=识别准备，绿色背景=训练中，灰色背景=训练完成；"
        "曲线只使用上位机导出的物理量和设备权威事件。",
        fontsize=11,
        color="#52606D",
        ha="left",
    )
    # 页脚保留来源哈希前 16 位，不公开本机路径、UTC 或设备标识。
    figure.text(
        0.06,
        0.018,
        f"来源 CSV SHA-256：{source_hash[:16]}…；原始 CSV 不随仓库发布。"
        "本图不反推未记录的在线方向、投影或动态阈值。",
        fontsize=10,
        color="#64748B",
        ha="left",
    )
    # 留出标题、副标题和页脚空间。
    figure.subplots_adjust(left=0.07, right=0.93, top=0.88, bottom=0.09, hspace=0.34)
    # 创建输出目录。
    output_path.parent.mkdir(parents=True, exist_ok=True)
    # 保存固定元数据 PNG。
    figure.savefig(
        output_path,
        dpi=120,
        facecolor=figure.get_facecolor(),
        metadata={"Software": "ESP32智慧运动助手计数曲线生成器"},
    )
    # 释放图形对象。
    plt.close(figure)


def write_manifest(
    manifest_path: Path,
    field_log: Path,
    session_id: int,
    session: FieldSession,
    schematic_path: Path,
    field_path: Path,
) -> None:
    """写出计数图来源、生产常量和真板事件摘要。"""

    # 计算现场日志哈希一次，图片和清单共享。
    source_hash = sha256_file(field_log)
    # 有效稳定动作去除等待识别哨兵。
    stable_actions = sorted(
        {
            action
            for action in session.stable_actions
            if action and action != "等待识别"
        }
    )
    # 组合可审计清单。
    payload = {
        # schema_version 允许以后扩展字段。
        "schema_version": 1,
        # generator 指向公开生成器。
        "generator": "python/visualize_counting_process.py",
        # schematic 描述非实测生产规则示意。
        "schematic": {
            # asset 只保存文件名，避免公开本机绝对路径。
            "asset": schematic_path.name,
            # asset_sha256 证明公开图片与本清单对应同一轮生成结果。
            "asset_sha256": sha256_file(schematic_path),
            "kind": "production-rule schematic, not measurement",
            "sample_rate_hz": 25,
            "direction_learn_dps": DIRECTION_LEARN_DPS,
            "direction_threshold_min_dps": DIRECTION_ACTIVE_MIN_DPS,
            "direction_threshold_max_dps": DIRECTION_ACTIVE_MAX_DPS,
            "direction_threshold_ratio": DIRECTION_ACTIVE_RATIO,
            "scale_rise_alpha": SCALE_RISE_ALPHA,
            "scale_fall_alpha": SCALE_FALL_ALPHA,
            "step_peak_delta_g": STEP_PEAK_DELTA_G,
            "step_rearm_delta_g": STEP_REARM_DELTA_G,
        },
        # field_replay 描述去标识真板回放。
        "field_replay": {
            # asset 只保存文件名，读者按清单所在目录定位图片。
            "asset": field_path.name,
            # asset_sha256 用于核对图片没有在清单生成后被替换。
            "asset_sha256": sha256_file(field_path),
            "source_csv_name": field_log.name,
            "source_csv_sha256": source_hash,
            "raw_csv_published": False,
            "session_id": session_id,
            "sample_count": int(session.relative_seconds.size),
            "duration_seconds": round(float(session.relative_seconds[-1]), 3),
            "stable_actions": stable_actions,
            "event_count": len(session.events),
            "event_relative_seconds": [
                round(event.relative_seconds, 3)
                for event in session.events
            ],
            "event_totals": [
                event.total_value
                for event in session.events
            ],
            "final_authoritative_total": int(session.authoritative_total[-1]),
            "published_fields": [
                "relative_seconds",
                "gyro_magnitude_dps",
                "acceleration_magnitude_g",
                "device_state",
                "authoritative_total",
                "metric_event_time",
            ],
            "excluded_fields": [
                "UTC receive time",
                "device identifier",
                "BLE address",
                "raw CSV rows",
            ],
        },
        # implementation_refs 指向当前生产公式的源码。
        "implementation_refs": [
            "esp32/firmware/components/motion_phase/include/motion_phase.h",
            "esp32/firmware/components/motion_phase/motion_phase.c",
            "esp32/firmware/components/fitness_core/src/fitness_core.c",
            "docs/算法原理、训练与实时计数.md",
        ],
    }
    # 创建清单父目录。
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    # UTF-8、中文不转义和稳定缩进便于 Git 审查。
    manifest_path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def build_argument_parser() -> argparse.ArgumentParser:
    """构造命令行参数解析器。"""

    # 程序说明指出同时生成示意图和现场回放。
    parser = argparse.ArgumentParser(description="生成计数状态机示意曲线和真板权威事件回放")
    # --field-log 是上位机中文 CSV。
    parser.add_argument("--field-log", type=Path, required=True, help="上位机导出的全部缓存 CSV")
    # --session-id 选择一个只含单一主动作的会话。
    parser.add_argument("--session-id", type=int, required=True, help="用于现场回放的会话序号")
    # --output-dir 保存两张 PNG。
    parser.add_argument("--output-dir", type=Path, required=True, help="两张计数曲线的输出目录")
    # --manifest 保存机器可读来源清单。
    parser.add_argument("--manifest", type=Path, required=True, help="计数图来源 JSON 清单")
    # 返回解析器。
    return parser


def main() -> int:
    """解析参数、生成两张图和清单。"""

    # 解析当前命令行。
    args = build_argument_parser().parse_args()
    # 把现场日志解析为绝对路径，错误信息更明确。
    field_log = args.field_log.resolve()
    # 日志必须真实存在。
    if not field_log.is_file():
        # 缺失时不生成示意或现场图，避免只有半套资产。
        raise FileNotFoundError(f"现场日志不存在：{field_log}")
    # 读取指定单动作会话。
    session = load_field_session(field_log, args.session_id)
    # 固定示意图片名，编号接在现有四张算法图之后。
    schematic_path = args.output_dir / "05_计数算法示意曲线.png"
    # 固定现场回放图片名。
    field_path = args.output_dir / "06_真板深蹲计数事件回放.png"
    # 生成生产规则示意图。
    plot_counting_schematic(schematic_path)
    # 计算现场日志哈希供图片页脚使用。
    source_hash = sha256_file(field_log)
    # 生成真板物理曲线和权威事件图。
    plot_field_session(session, args.session_id, source_hash, field_path)
    # 写出两张图的来源、常量和事件摘要。
    write_manifest(
        args.manifest,
        field_log,
        args.session_id,
        session,
        schematic_path,
        field_path,
    )
    # 输出稳定成功标记。
    print(
        f"COUNTING_FIGURES_OK session={args.session_id} "
        f"events={len(session.events)} output={args.output_dir}"
    )
    # 0 表示两张图片和清单全部生成。
    return 0


# 直接执行脚本时调用 main；导入时只提供解析和绘图函数。
if __name__ == "__main__":
    # SystemExit 把 main 返回值交给操作系统。
    raise SystemExit(main())
