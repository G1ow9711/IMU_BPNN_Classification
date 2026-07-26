r"""用原 StepCounter 极值筛选器生成“一个波峰与一个波谷计一次”的离线证据。

本脚本只读取用户更新的 ``StepCounter`` 和本地开合跳数据，不修改外部参考工程、
ESP32 固件或上位机。计数定义固定为：时间相邻的一个有效波峰与一个有效波谷
组成一组完整配对，先出现峰或先出现谷均可，每组只计一次，不再把峰数乘二。三轴独立得到配对数，最终结果取
三轴中位数，以抑制佩戴方向和单轴瞬时噪声。

设滤波信号为 :math:`x[n]`，相邻接受峰和谷位置为 :math:`p_k` 与
:math:`v_k`。第 :math:`k` 次动作满足：

.. math::

   A_k=x[p_k]-x[v_k] > A_{min},\quad
   4 \le |v_k-p_k| \le 40

其中采样率为 25 Hz，故峰到谷允许 0.16–1.60 秒；完整动作次数为有效配对集合大小，
最终融合次数为 X、Y、Z 三轴配对数的中位数。时间复杂度为 O(3N)，除绘图数组外的
额外空间复杂度为 O(N)；后续移植 ESP32 时可改成常量空间的在线状态机。
"""

from __future__ import annotations

# argparse 解析数据、StepCounter 和输出目录，避免把本机路径写死到算法内部。
import argparse
# csv 写出每组峰谷的样本位置、时间、幅值和半周期宽度，供人工逐组复核。
import csv
# hashlib 流式计算输入文件哈希，确保公开教程中的结果可追溯。
import hashlib
# importlib.util 从用户给出的外部参考目录动态加载算法，不复制未授权源码。
import importlib.util
# json 写出机器可读汇总，后续用户确认后可作为 ESP32 移植输入。
import json
# dataclass 固定单轴结果字段，避免绘图与汇总使用不一致的计数口径。
from dataclasses import asdict, dataclass
# Path 统一处理 Windows 中文路径和项目本地输出目录。
from pathlib import Path
# Any 为原 StepCounter 动态模块和 Matplotlib Axes 提供类型标注。
from typing import Any

# Matplotlib 负责生成可放大的人工核数图。
import matplotlib

# Agg 后端不依赖桌面窗口，批处理可稳定输出 PNG 和 SVG。
matplotlib.use("Agg")

# pyplot 只创建和关闭 Figure；具体绘图全部通过面向对象 Axes API 完成。
import matplotlib.pyplot as plt
# NumPy 负责三轴数组、时间轴、中位数和单位换算。
import numpy as np

# 训练数据和设备采样率均固定为 25 Hz，单位为点/秒。
SAMPLE_RATE_HZ = 25
# QMI8658 数据的加速度缩放为 4096 raw/g。
ACCEL_RAW_PER_G = 4096.0
# QMI8658 数据的角速度缩放为 16.4 raw/(deg/s)。
GYRO_RAW_PER_DPS = 16.4
# 外部参考实现第一层对称均值滤波长度为 11 点。
MEAN_FILTER_LENGTH_1 = 11
# 外部参考实现第二层对称均值滤波长度为 5 点。
MEAN_FILTER_LENGTH_2 = 5
# 外部参考实现的峰谷最小幅值为 1/14 g。
PEAK_VALLEY_DIFFERENCE_RAW = int(ACCEL_RAW_PER_G) // 14
# 图中三个坐标轴使用色盲友好且在深浅背景均清晰的颜色。
AXIS_COLORS = ("#00A6D6", "#F59E0B", "#22C55E")
# 图中三个坐标轴按设备坐标系标为 X、Y、Z。
AXIS_LABELS = ("横向 X", "纵向 Y", "垂直 Z")
# 数据集动作段从文件第 26 行开始，零基下标为 25。
ACTIVE_SEGMENT_START_INDEX = 25
# 每份动作固定截取 408 点，即 16.32 秒。
ACTIVE_SEGMENT_SAMPLE_COUNT = 408
# 原 StepCounter 峰到相邻谷的最小宽度为 4 点，即 0.16 秒。
MIN_HALF_CYCLE_POINTS = 4
# 原 StepCounter 峰到相邻谷的最大宽度为 40 点，即 1.60 秒。
MAX_HALF_CYCLE_POINTS = 40
# 原 StepCounter 幅值阈值为 4096/14 raw，约 0.071 g。
MIN_PAIR_AMPLITUDE_RAW = int(ACCEL_RAW_PER_G) // 14


@dataclass(frozen=True)
class Dataset:
    """保存单份七列 IMU 数据及其可追溯信息。"""

    # path 是原始七列文本的绝对路径。
    path: Path
    # name 是不含扩展名的文件标识，用于生成图和 CSV。
    name: str
    # values_raw 形状为 [样本数, 7]，列顺序为 gx、gy、gz、ax、ay、az、时间戳。
    values_raw: np.ndarray
    # sha256 是原始数据文件的完整 SHA-256。
    sha256: str


def sha256_file(path: Path) -> str:
    """流式计算文件 SHA-256，避免一次性读入大文件。"""

    # digest 保存逐块累加的 SHA-256 状态。
    digest = hashlib.sha256()
    # 以二进制只读方式打开文件，确保换行符差异也被纳入哈希。
    with path.open("rb") as handle:
        # 每次读取 1 MiB；空字节串表示文件结束。
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            # 把当前块加入哈希状态。
            digest.update(block)
    # 返回 64 位小写十六进制摘要。
    return digest.hexdigest()


def read_seven_column_dataset(path: Path) -> Dataset:
    """读取 gx、gy、gz、ax、ay、az、时间戳七列整数。"""

    # rows 保存每行七个原始整数，最终形状为 [N, 7]。
    rows: list[list[int]] = []
    # 数据文件只含 ASCII 数字和逗号，使用 UTF-8 只读打开。
    with path.open("r", encoding="utf-8") as handle:
        # line_number 从 1 开始，使格式错误可直接定位原文件。
        for line_number, line in enumerate(handle, start=1):
            # 去除首尾空白和末尾逗号，兼容采集文件格式。
            stripped = line.strip().rstrip(",")
            # 空行不代表样本，直接跳过。
            if not stripped:
                continue
            # 按逗号拆分并去除每列空白。
            fields = [field.strip() for field in stripped.split(",")]
            # 当前数据合同必须恰好七列。
            if len(fields) != 7:
                # 抛出含路径和行号的明确格式错误。
                raise ValueError(f"{path}:{line_number} 不是七列数据：{len(fields)} 列")
            # 把七列十进制字符串转换为 Python 整数。
            rows.append([int(field) for field in fields])
    # 权威动作段需要至少 25+408=433 点。
    required_samples = ACTIVE_SEGMENT_START_INDEX + ACTIVE_SEGMENT_SAMPLE_COUNT
    # 数据不足时无法执行与验证集一致的切片。
    if len(rows) < required_samples:
        # 报告实际点数和最小点数，避免静默生成短片段。
        raise ValueError(f"{path} 仅 {len(rows)} 点，至少需要 {required_samples} 点")
    # 转为 int32，避免滤波求和时 int16 溢出。
    values = np.asarray(rows, dtype=np.int32)
    # 返回数据、文件名和哈希的不可变记录。
    return Dataset(path=path, name=path.stem, values_raw=values, sha256=sha256_file(path))


def select_authoritative_active_segment(dataset: Dataset) -> Dataset:
    """按数据集合同截取第 26–433 行的 408 点动作段。"""

    # segment_end_index 是 Python 右开区间终点 25+408=433。
    segment_end_index = ACTIVE_SEGMENT_START_INDEX + ACTIVE_SEGMENT_SAMPLE_COUNT
    # 调用者可能自行构造 Dataset，因此再次检查长度边界。
    if dataset.values_raw.shape[0] < segment_end_index:
        # 报告实际点数和所需点数。
        raise ValueError(
            f"{dataset.path} 仅 {dataset.values_raw.shape[0]} 点，"
            f"无法截取 [{ACTIVE_SEGMENT_START_INDEX}, {segment_end_index})"
        )
    # 复制动作段，防止后续处理误改原始数组视图。
    active_values = dataset.values_raw[
        ACTIVE_SEGMENT_START_INDEX:segment_end_index,
        :,
    ].copy()
    # 保留源路径、名称和哈希，仅替换当前分析数组。
    return Dataset(
        path=dataset.path,
        name=dataset.name,
        values_raw=active_values,
        sha256=dataset.sha256,
    )


def load_original_python_module(stepcounter_root: Path) -> Any:
    """从显式目录加载外部 StepCounter Python 实现。"""

    # module_path 指向调用者合法取得的参考实现。
    module_path = stepcounter_root / "Python" / "step_counter_simulation.py"
    # 文件缺失时立即失败，禁止悄悄切换到仓库内复制品。
    if not module_path.is_file():
        # 错误消息给出应检查的精确入口。
        raise FileNotFoundError(f"外部 StepCounter 入口不存在：{module_path}")
    # 从精确路径建立模块描述，不依赖当前工作目录或 sys.path。
    specification = importlib.util.spec_from_file_location("external_stepcounter", module_path)
    # 无法建立加载器时拒绝继续。
    if specification is None or specification.loader is None:
        # 抛出明确错误，避免输出缺少来源的结果。
        raise RuntimeError(f"无法加载外部 StepCounter Python：{module_path}")
    # 根据描述创建尚未执行的模块对象。
    module = importlib.util.module_from_spec(specification)
    # 执行外部模块；其主程序应由 __name__ 保护。
    specification.loader.exec_module(module)
    # 外部文件把阈值定义在主入口时，按同一 1/14 g 公式补入模块全局量。
    module.PEAK_VALLEY_DIFFERENCE = PEAK_VALLEY_DIFFERENCE_RAW
    # 返回滤波函数与 action_processor 类所在模块。
    return module


def filter_original_python(acceleration_raw: np.ndarray, original_module: Any) -> np.ndarray:
    """按外部参考实现执行 11 点与 5 点均值并逐层向零截断。"""

    # 输入必须是 [N, 3] 三轴加速度 raw，防止通道顺序错误。
    if acceleration_raw.ndim != 2 or acceleration_raw.shape[1] != 3:
        # 错误中保留实际形状，便于调用者定位输入合同。
        raise ValueError(f"加速度数组应为 [N, 3]，实际为 {acceleration_raw.shape}")
    # filtered 保存三轴滤波值，形状为 [N, 3]，单位 raw。
    filtered = np.zeros_like(acceleration_raw, dtype=np.int32)
    # 三轴独立滤波，禁止先求模长丢失轴向相位。
    for axis_index in range(3):
        # 读取当前轴 int32 原始序列。
        axis_values = acceleration_raw[:, axis_index]
        # 第一层 11 点对称均值与外部参考仿真一致。
        stage_one = original_module.func_calculation(
            axis_values,
            MEAN_FILTER_LENGTH_1,
            np.mean,
        )
        # C 整数赋值向零截断，转换为 int32 保持该规则。
        stage_one = stage_one.astype(np.int32)
        # 第二层 5 点对称均值进一步抑制瞬时噪声。
        stage_two = original_module.func_calculation(
            stage_one,
            MEAN_FILTER_LENGTH_2,
            np.mean,
        )
        # 再次向零截断并写入当前轴。
        filtered[:, axis_index] = stage_two.astype(np.int32)
    # 返回与输入等长的三轴滤波序列。
    return filtered


def configure_matplotlib() -> None:
    """配置中文字体、字号和可打印配色。"""

    # 优先使用 Windows 微软雅黑，缺失时依次回退黑体和 DejaVu Sans。
    plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
    # 允许坐标轴正常显示负号。
    plt.rcParams["axes.unicode_minus"] = False
    # 默认正文字号 10.5 pt，高清导出仍易读。
    plt.rcParams["font.size"] = 10.5
    # 保存图默认使用 180 DPI，PNG 入口可单独提高。
    plt.rcParams["savefig.dpi"] = 180


@dataclass(frozen=True)
class PeakValleyPair:
    """保存一个时间相邻峰谷的无方向动作配对。"""

    # ordinal 是当前轴按时间排序的一基动作编号。
    ordinal: int
    # peak_index 是波峰在 408 点动作段中的零基样本下标。
    peak_index: int
    # valley_index 是配对波谷在同一动作段中的零基样本下标。
    valley_index: int
    # peak_time_s 是波峰相对动作段起点的时间，单位秒。
    peak_time_s: float
    # valley_time_s 是波谷相对动作段起点的时间，单位秒。
    valley_time_s: float
    # amplitude_g 是峰值减谷值的幅度，单位 g，正常值应为正。
    amplitude_g: float
    # half_cycle_s 是峰到谷的半周期时长，单位秒。
    half_cycle_s: float


@dataclass(frozen=True)
class AxisPairResult:
    """保存单轴接受极值和最终峰谷配对。"""

    # axis_index 按 0、1、2 对应加速度 X、Y、Z。
    axis_index: int
    # accepted_peak_indices 保存原 StepCounter 阈值筛选后的峰位置。
    accepted_peak_indices: tuple[int, ...]
    # accepted_valley_indices 保存筛选流程保留的谷位置。
    accepted_valley_indices: tuple[int, ...]
    # pairs 保存时间相邻峰谷的一对一动作周期。
    pairs: tuple[PeakValleyPair, ...]


@dataclass(frozen=True)
class DatasetPairResult:
    """保存一份动作段的三轴配对汇总和证据路径。"""

    # dataset 是原始数据文件名，不包含扩展名。
    dataset: str
    # samples 固定为权威动作段 408 点。
    samples: int
    # duration_s 是 408/25=16.32 秒。
    duration_s: float
    # axis_pair_counts 按 X、Y、Z 保存配对数。
    axis_pair_counts: tuple[int, int, int]
    # fused_repetitions 是三轴配对数中位数，不乘二。
    fused_repetitions: int
    # overview_png 是高分辨率人工核数图绝对路径。
    overview_png: str
    # overview_svg 是可无限放大的矢量图绝对路径。
    overview_svg: str
    # pair_csv 是逐轴逐组峰谷明细绝对路径。
    pair_csv: str


def detect_axis_pairs(
    filtered_axis_raw: np.ndarray,
    axis_index: int,
    original_module: Any,
) -> AxisPairResult:
    """用原 StepCounter 候选极值，把时间相邻的峰和谷配成一次动作。"""

    # 处理器容量使用动作段长度，保证 408 点内所有候选极值均可保存。
    processor = original_module.action_processor(
        filtered_axis_raw.size,
        filtered_axis_raw.size,
    )
    # 找出严格局部峰和局部谷，输入单位为 QMI8658 加速度 raw。
    processor = processor.find_possible_peak_valley(filtered_axis_raw)
    # 删除落在整段均值错误一侧的峰谷，使峰高于均值、谷低于均值。
    processor = processor.remove_false_peak_valley(filtered_axis_raw)
    # 只合并相邻同类峰，不调用会强制删除“首峰”的谷-峰-谷包装函数。
    if processor.p_cnt > 1:
        # 保留更高的同类峰，输入单位仍为加速度 raw。
        processor = processor.merge_close_pole(filtered_axis_raw, 1)
    # 只合并相邻同类谷，保留更低的同类谷。
    if processor.v_cnt > 1:
        # direction=-1 表示比较谷值深度。
        processor = processor.merge_close_pole(filtered_axis_raw, -1)
    # peaks 复制最终候选峰坐标，避免依赖原处理器数组的后续生命周期。
    peaks = tuple(int(value) for value in processor.p_loc[: processor.p_cnt])
    # valleys 复制最终候选谷坐标，并按时间升序供相邻极值配对。
    valleys = tuple(sorted(int(value) for value in processor.v_loc[: processor.v_cnt]))
    # events 把峰谷合并为时间有序事件，类型 P/V 用于防止同类极值直接配对。
    events = sorted(
        [(peak_index, "P") for peak_index in peaks]
        + [(valley_index, "V") for valley_index in valleys],
        key=lambda item: item[0],
    )
    # pairs 累计一峰一谷动作配对；每个极值最多使用一次。
    pairs: list[PeakValleyPair] = []
    # pending_event 保存尚未配对的第一个极值；None 表示等待新周期起点。
    pending_event: tuple[int, str] | None = None
    # 按时间遍历全部候选极值，先峰后谷和先谷后峰采用同一逻辑。
    for event_index, event_kind in events:
        # 没有待配极值时，把当前极值保存为本组起点。
        if pending_event is None:
            # 保存样本下标与 P/V 类型。
            pending_event = (event_index, event_kind)
            continue
        # pending_index 是待配极值的样本下标。
        pending_index = pending_event[0]
        # pending_kind 是待配极值的 P/V 类型。
        pending_kind = pending_event[1]
        # 同类极值不能组成周期，只保留更极端的一个作为候选。
        if event_kind == pending_kind:
            # 两个峰保留值更高者；两个谷保留值更低者。
            should_replace = (
                event_kind == "P"
                and filtered_axis_raw[event_index] > filtered_axis_raw[pending_index]
            ) or (
                event_kind == "V"
                and filtered_axis_raw[event_index] < filtered_axis_raw[pending_index]
            )
            # 当前极值更显著时替换待配极值。
            if should_replace:
                # 保存新的更显著极值。
                pending_event = (event_index, event_kind)
            continue
        # half_cycle_points 是相邻异类极值的时间距离，单位点。
        half_cycle_points = event_index - pending_index
        # 根据事件类型还原真实峰和谷位置，允许 V→P 或 P→V。
        peak_index = pending_index if pending_kind == "P" else event_index
        # 另一位置就是配对谷。
        valley_index = pending_index if pending_kind == "V" else event_index
        # amplitude_raw 是峰减谷的正振幅，单位 raw。
        amplitude_raw = int(filtered_axis_raw[peak_index]) - int(
            filtered_axis_raw[valley_index]
        )
        # 太短或幅值太小属于同一极值附近毛刺，保留更显著端继续等待。
        if half_cycle_points < MIN_HALF_CYCLE_POINTS or amplitude_raw <= MIN_PAIR_AMPLITUDE_RAW:
            # 当前事件成为新的待配端点，使下一次反向变化仍有机会组成周期。
            pending_event = (event_index, event_kind)
            continue
        # 间隔超过 40 点说明前一端点已过期，从当前极值重新开始。
        if half_cycle_points > MAX_HALF_CYCLE_POINTS:
            # 丢弃过期端点，不把超慢漂移误算为动作。
            pending_event = (event_index, event_kind)
            continue
        # 追加不可变配对；编号从 1 开始，便于用户看图逐个核对。
        pairs.append(
            PeakValleyPair(
                ordinal=len(pairs) + 1,
                peak_index=peak_index,
                valley_index=valley_index,
                peak_time_s=peak_index / SAMPLE_RATE_HZ,
                valley_time_s=valley_index / SAMPLE_RATE_HZ,
                amplitude_g=amplitude_raw / ACCEL_RAW_PER_G,
                half_cycle_s=half_cycle_points / SAMPLE_RATE_HZ,
            )
        )
        # 本组两个极值均已消费，下一事件开始新动作配对。
        pending_event = None
    # 返回当前轴的原接受极值和一峰一谷配对。
    return AxisPairResult(
        axis_index=axis_index,
        accepted_peak_indices=peaks,
        accepted_valley_indices=valleys,
        pairs=tuple(pairs),
    )


def plot_pair_result(
    dataset: Dataset,
    filtered_raw: np.ndarray,
    axis_results: tuple[AxisPairResult, AxisPairResult, AxisPairResult],
    fused_repetitions: int,
    output_directory: Path,
) -> tuple[Path, Path]:
    """绘制原始六轴与三轴峰谷配对编号图。"""

    # time_seconds 是 408 点动作段的统一时间轴，单位秒。
    time_seconds = np.arange(dataset.values_raw.shape[0], dtype=np.float64) / SAMPLE_RATE_HZ
    # acceleration_g 把第 4–6 列原始加速度从 raw 转为 g。
    acceleration_g = dataset.values_raw[:, 3:6].astype(np.float64) / ACCEL_RAW_PER_G
    # filtered_g 把双均值滤波结果从 raw 转为 g。
    filtered_g = filtered_raw.astype(np.float64) / ACCEL_RAW_PER_G
    # gyroscope_dps 把第 1–3 列角速度从 raw 转为度每秒。
    gyroscope_dps = dataset.values_raw[:, 0:3].astype(np.float64) / GYRO_RAW_PER_DPS
    # 创建五行共享时间轴图，三条中间轴专门用于逐组视觉核数。
    figure, axes = plt.subplots(
        5,
        1,
        figsize=(18, 17),
        sharex=True,
        constrained_layout=True,
        gridspec_kw={"height_ratios": [1.0, 1.25, 1.25, 1.25, 1.0]},
    )
    # 画布使用浅灰底，白色绘图区和标记具有清晰层级。
    figure.patch.set_facecolor("#F4F7FB")
    # counts_text 显示 X/Y/Z 三轴配对数，便于检查中位数融合。
    counts_text = "/".join(str(len(result.pairs)) for result in axis_results)
    # 主标题明确“峰谷一组计一次、不乘二”，防止再次误读。
    figure.suptitle(
        f"{dataset.name}｜相邻峰谷一组计 1 次（不乘 2）｜X/Y/Z={counts_text}｜中位数={fused_repetitions}",
        fontsize=17,
        fontweight="bold",
        color="#10243E",
    )
    # 第一行绘制三轴原始加速度，供用户核对可见周期数量。
    for axis_index, color in enumerate(AXIS_COLORS):
        # 每条曲线使用相同时间轴和 g 单位。
        axes[0].plot(
            time_seconds,
            acceleration_g[:, axis_index],
            color=color,
            linewidth=0.9,
            alpha=0.88,
            label=AXIS_LABELS[axis_index],
        )
    # 标明原始加速度物理单位。
    axes[0].set_ylabel("原始加速度 / g")
    # 三轴图例置于右上角。
    axes[0].legend(loc="upper right", ncols=3, frameon=False)
    # 逐轴绘制滤波信号、峰、配对谷、连接线和动作编号。
    for axis_result in axis_results:
        # axis_index 也是对应中间子图偏移量。
        axis_index = axis_result.axis_index
        # plot_axis 指向当前 X、Y 或 Z 滤波子图。
        plot_axis = axes[axis_index + 1]
        # 绘制滤波波形，保留完整 16.32 秒动作段。
        plot_axis.plot(
            time_seconds,
            filtered_g[:, axis_index],
            color=AXIS_COLORS[axis_index],
            linewidth=1.25,
            label=f"{AXIS_LABELS[axis_index]} 双均值滤波",
        )
        # 逐组配对绘制半透明区间和编号，用户可直接数 1、2、3……。
        for pair in axis_result.pairs:
            # 当前峰和谷的时间坐标，单位秒；连线端点顺序不影响配对含义。
            pair_times = [pair.peak_time_s, pair.valley_time_s]
            # 当前峰和谷的滤波幅值，单位 g。
            pair_values = [
                filtered_g[pair.peak_index, axis_index],
                filtered_g[pair.valley_index, axis_index],
            ]
            # 淡色背景标出本组峰到谷的区间。
            plot_axis.axvspan(
                min(pair.peak_time_s, pair.valley_time_s),
                max(pair.peak_time_s, pair.valley_time_s),
                color=AXIS_COLORS[axis_index],
                alpha=0.08,
                zorder=1,
            )
            # 细虚线连接峰和谷，表达“这两个极值合成一次”。
            plot_axis.plot(
                pair_times,
                pair_values,
                color="#475569",
                linewidth=0.9,
                linestyle="--",
                alpha=0.8,
                zorder=4,
            )
            # 空心圆标记峰值 P。
            plot_axis.scatter(
                [pair.peak_time_s],
                [pair_values[0]],
                s=38,
                facecolors="white",
                edgecolors="#DC2626",
                linewidths=1.5,
                zorder=5,
            )
            # 空心倒三角标记配对谷值 V。
            plot_axis.scatter(
                [pair.valley_time_s],
                [pair_values[1]],
                s=42,
                marker="v",
                facecolors="white",
                edgecolors="#2563EB",
                linewidths=1.5,
                zorder=5,
            )
            # 编号写在峰谷中间上方，避免只标峰导致“峰和谷各算一次”的误解。
            plot_axis.annotate(
                str(pair.ordinal),
                xy=(
                    (pair.peak_time_s + pair.valley_time_s) / 2.0,
                    max(pair_values),
                ),
                xytext=(0, 8),
                textcoords="offset points",
                ha="center",
                va="bottom",
                fontsize=8,
                fontweight="bold",
                color="#111827",
                zorder=6,
            )
        # y 轴标签同时显示当前轴配对总数。
        plot_axis.set_ylabel(f"{AXIS_LABELS[axis_index]} / g\n{len(axis_result.pairs)} 组")
        # 图例只保留波形名称；标记含义写入子图标题。
        plot_axis.legend(loc="upper right", frameon=False)
        # 子图标题说明红圈、蓝三角和编号定义。
        plot_axis.set_title(
            "红圈=P 波峰，蓝三角=V 波谷；一条虚线连接的峰谷计 1 次",
            loc="left",
            fontsize=10,
            color="#475569",
        )
    # 最后一行绘制三轴角速度，辅助用户与加速度周期交叉核对。
    for axis_index, color in enumerate(AXIS_COLORS):
        # 角速度单位为度每秒，不参与当前离线计数。
        axes[4].plot(
            time_seconds,
            gyroscope_dps[:, axis_index],
            color=color,
            linewidth=0.9,
            alpha=0.88,
            label=AXIS_LABELS[axis_index],
        )
    # 标明角速度物理单位。
    axes[4].set_ylabel("角速度 / (°/s)")
    # 最底行显示统一时间轴。
    axes[4].set_xlabel("动作段时间 / s")
    # 三轴角速度图例置于右上。
    axes[4].legend(loc="upper right", ncols=3, frameon=False)
    # 统一五个子图的背景、网格、边框和时间范围。
    for plot_axis in axes:
        # 白色绘图区提高彩色波形和编号对比度。
        plot_axis.set_facecolor("#FFFFFF")
        # 淡灰网格辅助对齐相邻周期。
        plot_axis.grid(True, color="#CBD5E1", alpha=0.55, linewidth=0.7)
        # 去除顶部边框减少视觉噪声。
        plot_axis.spines["top"].set_visible(False)
        # 去除右侧边框减少视觉噪声。
        plot_axis.spines["right"].set_visible(False)
        # 横轴覆盖完整 408 点，不隐藏首尾不完整周期。
        plot_axis.set_xlim(0.0, float(time_seconds[-1]))
    # png_path 保存 240 DPI 位图，适合在 Codex 和图片查看器中放大。
    png_path = output_directory / f"{dataset.name}_峰谷配对计数.png"
    # svg_path 保存矢量图，放大查看极值位置不会失真。
    svg_path = output_directory / f"{dataset.name}_峰谷配对计数.svg"
    # 写出高分辨率 PNG，并裁掉无用外边距。
    figure.savefig(png_path, dpi=240, bbox_inches="tight")
    # 写出 SVG 矢量证据。
    figure.savefig(svg_path, bbox_inches="tight")
    # 关闭 Figure，防止三份大图同时占用内存。
    plt.close(figure)
    # 返回两种图的绝对路径。
    return png_path.resolve(), svg_path.resolve()


def write_pair_csv(
    path: Path,
    axis_results: tuple[AxisPairResult, AxisPairResult, AxisPairResult],
) -> None:
    """写出每个波峰—波谷配对的时间、幅值和半周期。"""

    # UTF-8 BOM 确保 Windows Excel 直接打开中文列名不乱码。
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        # writer 负责标准 CSV 转义。
        writer = csv.writer(handle)
        # 表头固定峰谷配对的完整审计字段。
        writer.writerow(
            [
                "轴",
                "动作编号",
                "峰样本",
                "谷样本",
                "峰时间_s",
                "谷时间_s",
                "峰谷幅值_g",
                "峰到谷时长_s",
                "本组计数",
            ]
        )
        # 按 X、Y、Z 顺序输出，方便与图中三行对应。
        for axis_result in axis_results:
            # 当前轴的每一组相邻峰谷各写一行，计数恒为 1。
            for pair in axis_result.pairs:
                # 数值保留三至六位小数，兼顾可读性与复算精度。
                writer.writerow(
                    [
                        AXIS_LABELS[axis_result.axis_index],
                        pair.ordinal,
                        pair.peak_index,
                        pair.valley_index,
                        f"{pair.peak_time_s:.3f}",
                        f"{pair.valley_time_s:.3f}",
                        f"{pair.amplitude_g:.6f}",
                        f"{pair.half_cycle_s:.3f}",
                        1,
                    ]
                )


def parse_arguments() -> argparse.Namespace:
    """解析三条必需路径参数。"""

    # parser 说明本脚本仅生成离线峰谷配对证据。
    parser = argparse.ArgumentParser(description="开合跳峰谷配对离线核数")
    # data_root 指向包含三份 jumping_jack_scy*_20.txt 的本地数据目录。
    parser.add_argument("--data-root", type=Path, required=True)
    # stepcounter_root 指向用户更新的只读 StepCounter 参考工程。
    parser.add_argument("--stepcounter-root", type=Path, required=True)
    # output_root 保存 PNG、SVG、CSV 和 JSON，不写回原数据目录。
    parser.add_argument("--output-root", type=Path, required=True)
    # 返回已解析参数。
    return parser.parse_args()


def main() -> int:
    """运行三份权威 408 点开合跳切片并生成峰谷配对证据。"""

    # arguments 保存用户提供的三个绝对目录。
    arguments = parse_arguments()
    # 创建项目本地输出目录；已存在时保留其它工件。
    arguments.output_root.mkdir(parents=True, exist_ok=True)
    # 配置中文字体、字号和无窗口绘图参数。
    configure_matplotlib()
    # 动态加载用户更新的原 StepCounter Python 极值筛选实现。
    original_module = load_original_python_module(arguments.stepcounter_root)
    # 精确选择三份文件名带 scy 和 20 后缀的开合跳数据。
    data_paths = sorted(arguments.data_root.glob("jumping_jack_scy*_20.txt"))
    # 文件数量不是三份时拒绝静默漏图。
    if len(data_paths) != 3:
        raise RuntimeError(f"预期三份 jumping_jack_scy*_20.txt，实际 {len(data_paths)} 份")
    # summaries 累计三份数据的结果，最终写入 JSON 清单。
    summaries: list[DatasetPairResult] = []
    # 逐份数据执行相同读取、滤波、配对和绘图流程。
    for data_path in data_paths:
        # 读取完整七列文件并计算源文件 SHA-256。
        full_dataset = read_seven_column_dataset(data_path)
        # 按 MATLAB DataCreateTxt.m 截取第 26–433 行共 408 点动作段。
        dataset = select_authoritative_active_segment(full_dataset)
        # 双均值滤波仅处理第 4–6 列加速度 raw，输出形状 [408, 3]。
        filtered_raw = filter_original_python(dataset.values_raw[:, 3:6], original_module)
        # 三轴独立检测峰谷配对，禁止先求模长丢失轴向相位。
        axis_results = tuple(
            detect_axis_pairs(filtered_raw[:, axis_index], axis_index, original_module)
            for axis_index in range(3)
        )
        # 类型收窄：上面的固定 range(3) 必然生成三个 AxisPairResult。
        typed_axis_results = (
            axis_results[0],
            axis_results[1],
            axis_results[2],
        )
        # axis_counts 保存 X、Y、Z 峰谷配对数。
        axis_counts = tuple(len(result.pairs) for result in typed_axis_results)
        # 三轴中位数抑制单轴噪声；结果就是动作次数，不乘二。
        fused_repetitions = int(np.median(np.asarray(axis_counts, dtype=np.int32)))
        # pair_csv_path 保存逐组明细。
        pair_csv_path = arguments.output_root / f"{dataset.name}_峰谷配对明细.csv"
        # 写出三轴所有配对，供用户按编号复算。
        write_pair_csv(pair_csv_path, typed_axis_results)
        # 生成高分辨率 PNG 和 SVG 人工核数图。
        png_path, svg_path = plot_pair_result(
            dataset,
            filtered_raw,
            typed_axis_results,
            fused_repetitions,
            arguments.output_root,
        )
        # 保存当前数据的不可变汇总。
        summaries.append(
            DatasetPairResult(
                dataset=dataset.name,
                samples=dataset.values_raw.shape[0],
                duration_s=dataset.values_raw.shape[0] / SAMPLE_RATE_HZ,
                axis_pair_counts=(axis_counts[0], axis_counts[1], axis_counts[2]),
                fused_repetitions=fused_repetitions,
                overview_png=str(png_path),
                overview_svg=str(svg_path),
                pair_csv=str(pair_csv_path.resolve()),
            )
        )
        # 控制台输出一行紧凑结果，便于自动化读取。
        print(
            f"{dataset.name}: X/Y/Z={axis_counts[0]}/{axis_counts[1]}/{axis_counts[2]}, "
            f"峰谷配对中位数={fused_repetitions}"
        )
    # manifest_path 保存本轮算法规则、来源哈希和三份结果。
    manifest_path = arguments.output_root / "peak_valley_pair_manifest.json"
    # manifest 记录不乘二规则和可追溯源文件哈希。
    manifest = {
        "count_rule": "one_adjacent_peak_and_valley_in_either_order_equals_one_repetition",
        "multiply_by_two": False,
        "sample_rate_hz": SAMPLE_RATE_HZ,
        "active_segment_samples": ACTIVE_SEGMENT_SAMPLE_COUNT,
        "stepcounter_python_sha256": sha256_file(
            arguments.stepcounter_root / "Python" / "step_counter_simulation.py"
        ),
        "results": [asdict(summary) for summary in summaries],
    }
    # UTF-8 JSON 使用中文可读格式和两个空格缩进。
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    # 返回 0 表示三份数据、图、CSV 和清单全部生成成功。
    return 0


# 仅直接执行脚本时运行 main；被测试导入时不产生文件。
if __name__ == "__main__":
    # 把 main 返回码交给操作系统和自动化工具。
    raise SystemExit(main())
