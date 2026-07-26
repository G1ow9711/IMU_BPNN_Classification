"""从冻结验证划分生成动作特征对比图和可复现清单。

输入数据按文件级固定种子 ``20260709`` 划分为训练/验证/测试；训练文件只用于
估计静止门，四张图只读取验证文件。每个窗口形状固定为 ``[62,6]``，六列顺序
``gx、gy、gz、ax、ay、az``，前三列单位 ``deg/s``，后三列单位 ``g``。脚本
只读数据，不训练模型、不修改样本、不读取测试窗口。

统计权重单位固定为“验证文件”，而不是高度重叠的窗口。时域先在每个文件内
对所有窗口的 ``acc_vertical`` 与 ``gyro_mag`` 做稳健形状标准化，选取平均
两两平方距离最小的真实窗口（算法 medoid）；再用两种信号的稳健动态强度和
定位客观运动峰，把该峰平移到窗口中心。每个文件最终只贡献一对曲线。频谱和
297 维特征则先对同一文件的窗口逐列取中位数，每个文件仍只贡献一个样本。

时域序列复用训练端重力对齐定义：

``acc_vertical[t] = a[t] dot (mean(a) / ||mean(a)||)``

``gyro_mag[t] = sqrt(gx[t]^2 + gy[t]^2 + gz[t]^2)``

所有窗口长度相同，因此不插值。文件 medoid 的客观运动峰对齐到中心点
``c=31``，时间坐标为 ``t=(i-c)/25 s``。某类第 ``t`` 个点的中心曲线为
该类各文件代表曲线的中位数 ``Q_0.50(t)``，阴影为
``[Q_0.25(t), Q_0.75(t)]``。这一定义使用全部有效验证文件并让每个文件
权重相同，不手选文件或“最好看”的单窗。

频谱先对序列去均值并乘 Hann 窗：

``P[k] = |RFFT((x-mean(x))*hann)[k]|^2 / sum_j P[j]``

热力图先求每类特征中位数 ``m_cf``，再按特征跨类别做稳健缩放：

``z_cf = (m_cf - median_c(m_cf)) / max(IQR_c(m_cf), 1e-9)``

显示值截断到 ``[-3,3]``。输出固定为四张 300 DPI PNG 和一个 JSON 清单；
清单只含相对验证文件、数据指纹、数量、阈值和算法参数，不写盘符或绝对路径。
文件缺失、类别缺失、非法形状或筛选后无窗口均直接报错，不生成伪完整图。
"""

# 启用 Python 3.10+ 的延迟注解求值，避免类型注解在导入阶段产生额外依赖。
from __future__ import annotations

# argparse 解析数据集目录和正式图表目录两个命令行参数。
import argparse
# hashlib 计算逐文件和整套数据集的 SHA-256 指纹。
import hashlib
# json 以稳定键顺序写入不含绝对路径的图表清单。
import json
# dataclass 定义带源文件和起点信息的只读验证窗口记录。
from dataclasses import dataclass
# Path 提供跨平台且可审计的路径拼接和相对路径转换。
from pathlib import Path
# 类型别名说明各函数的数组、映射和序列合同。
from typing import Dict, List, Mapping, Sequence, Tuple

# matplotlib 必须在导入 pyplot 前选择无界面后端，保证命令行和 CI 可生成 PNG。
import matplotlib

# Agg 后端直接渲染文件，不创建窗口，也不依赖正在运行的上位机界面。
matplotlib.use("Agg")
# pyplot 仅负责创建 Figure；全部绘图均通过面向对象的 Axes API 完成。
import matplotlib.pyplot as plt
# NumPy 执行六轴派生序列、分位数、频谱和稳健标准化计算。
import numpy as np

# 包导入路径用于 ``python -m python.visualize_action_features`` 和单元测试。
try:
    # 复用训练端唯一的数据读取、切分、清洗、窗口筛选和特征提取实现。
    from .train_export import (
        HIGH_DYNAMIC_CLASSES,
        SAMPLE_RATE,
        SEED,
        SIT_CLASS_NAME,
        STEP_SECONDS,
        TRAIN_RATIO,
        VAL_RATIO,
        TEST_RATIO,
        ImuRecord,
        build_feature_names,
        build_feature_series,
        estimate_active_point_threshold,
        estimate_rest_threshold,
        extract_features,
        keep_window_for_label,
        load_imu_file,
        motion_score,
        motion_segment_bounds,
        preprocess_imu_window,
        scan_dataset,
        split_records_by_file,
        window_lengths,
    )
# 直接执行 ``python visualize_action_features.py`` 时使用同目录模块导入。
except ImportError:
    # 两种导入方式引用同一文件，不复制任何算法实现。
    from train_export import (
        HIGH_DYNAMIC_CLASSES,
        SAMPLE_RATE,
        SEED,
        SIT_CLASS_NAME,
        STEP_SECONDS,
        TRAIN_RATIO,
        VAL_RATIO,
        TEST_RATIO,
        ImuRecord,
        build_feature_names,
        build_feature_series,
        estimate_active_point_threshold,
        estimate_rest_threshold,
        extract_features,
        keep_window_for_label,
        load_imu_file,
        motion_score,
        motion_segment_bounds,
        preprocess_imu_window,
        scan_dataset,
        split_records_by_file,
        window_lengths,
    )


# 图表生成器版本在算法、参数或清单结构变化时递增；版本 2 增加逐图 PNG SHA-256。
GENERATOR_VERSION = 2
# 文档图固定采用模型部署窗口 2.5 秒，25 Hz 下由训练函数换算为 62 点。
WINDOW_SECONDS = 2.5
# 功率谱只展示 0~5 Hz 的人体动作主频段，排除高频传感器噪声对纵轴的压缩。
POWER_MAX_HZ = 5.0
# 时域图聚焦用户现场逐轮测试的六种动作，顺序与实际测试口径一致。
FIELD_ACTIONS = (
    "jumping_jack",
    "squat",
    "jumping_squat",
    "jumping_lunge",
    "wave",
    "walk",
)
# 中文名仅用于图例和标题；模型类别键仍保持训练端英文目录名。
CLASS_DISPLAY_NAMES = {
    "good_morning": "早安式",
    "jumping_jack": "开合跳",
    "jumping_lunge": "跳跃弓步",
    "jumping_squat": "跳跃深蹲",
    "lunge": "弓步",
    "sit": "静坐",
    "squat": "深蹲",
    "trot": "小跑",
    "jogging": "小跑",
    "tuck_jump": "收腹跳",
    "walk": "行走",
    "wave": "挥手",
}
# 八个特征覆盖动作强度、垂直幅度、周期、重复性、频带、冲击和手腕换向。
KEY_FEATURES = (
    ("gyro_mag_rms", "角速度模长 RMS", "deg/s"),
    ("acc_vertical_std", "垂直加速度标准差", "g"),
    ("gyro_mag_dominant_frequency_hz", "角速度主频", "Hz"),
    ("gyro_mag_autocorr_peak", "角速度自相关峰", "无量纲"),
    ("acc_vertical_spectral_high_band_ratio", "垂直加速度高频占比", "无量纲"),
    ("gyro_mag_spectral_peak_power_ratio", "角速度主峰功率占比", "无量纲"),
    ("wrist_reversal_rate_hz", "手腕主轴换向率", "Hz"),
    ("aligned_takeoff_to_landing_seconds", "起跳到落地时长", "s"),
)
# 正式输出文件名固定为中文序号，文档链接不会因重复生成而漂移。
OUTPUT_FILES = {
    "time_domain": "01_六类派生信号曲线.png",
    "power_spectrum": "02_十一类功率谱对比.png",
    "feature_distribution": "03_关键特征分布.png",
    "feature_heatmap": "04_特征中位数热力图.png",
    "manifest": "figure_manifest.json",
}
# 旧英文图名仅用于生成成功后的定向清理，禁止通配删除其它文档资产。
LEGACY_OUTPUT_FILES = (
    "action_waveform_comparison.png",
    "class_power_spectrum.png",
    "interpretable_feature_distributions.png",
    "feature_class_heatmap.png",
)
# 色盲友好的蓝色表示加速度，橙红色表示角速度。
SIGNAL_COLORS = {
    "acc_vertical": "#0072B2",
    "gyro_mag": "#D55E00",
}


@dataclass(frozen=True)
class ValidationWindow:
    """保存一个验证窗口及其相对来源，不向清单泄露本机绝对路径。"""

    # label 是模型类别目录名，例如 ``jumping_jack``。
    label: str
    # label_idx 是训练端按目录排序生成的固定类别索引。
    label_idx: int
    # source_file 是相对数据集根目录的 POSIX 路径。
    source_file: str
    # start_point 是窗口在原始文件中的起始采样点，单位为点。
    start_point: int
    # data 形状为 [62,6]，顺序 gx、gy、gz、ax、ay、az，单位 deg/s 和 g。
    data: np.ndarray


@dataclass(frozen=True)
class ValidationCorpus:
    """保存验证窗口和筛选统计，供四张图与清单共享同一数据事实。"""

    # windows 按类别、文件和起点排序，保证绘图和清单跨运行稳定。
    windows: Tuple[ValidationWindow, ...]
    # skipped 记录过短、静止过滤、无有效窗和裁剪点数量。
    skipped: Mapping[str, int]
    # class_window_counts 记录每个类别最终参与图表的窗口数。
    class_window_counts: Mapping[str, int]
    # file_window_counts 记录每个验证文件最终参与图表的窗口数。
    file_window_counts: Mapping[str, int]


@dataclass(frozen=True)
class FileTimeRepresentative:
    """保存一个验证文件的等权时域代表和可审计的自动选择位置。"""

    # source_file 是数据集内 POSIX 相对路径，作为文件级权重单位标识。
    source_file: str
    # label 是文件所属模型类别键。
    label: str
    # acc_vertical 形状 [62]、单位 g，客观运动峰已平移到中心。
    acc_vertical: np.ndarray
    # gyro_mag 形状 [62]、单位 deg/s，与加速度使用同一平移量。
    gyro_mag: np.ndarray
    # medoid_start_point 是算法 medoid 在原始文件中的窗口起点，单位为点。
    medoid_start_point: int
    # original_peak_index 是平移前客观运动峰在 medoid 窗口内的点索引。
    original_peak_index: int


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    """解析路径参数；输入为字符串序列，返回含两个 Path 的命名空间。"""

    # 创建只包含两个必需路径的命令行解析器，避免隐藏默认输出位置。
    parser = argparse.ArgumentParser(
        description="使用冻结验证划分生成动作特征对比图和可复现清单。"
    )
    # 数据集目录必须包含按动作类别分组的 TXT 文件。
    parser.add_argument(
        "--dataset-dir",
        type=Path,
        required=True,
        help="imu_dataset_for_final 数据集目录。",
    )
    # 输出目录应指向仓库内正式文档资源目录，不使用临时目录。
    parser.add_argument(
        "--output-dir",
        type=Path,
        required=True,
        help="正式 PNG 与 figure_manifest.json 输出目录。",
    )
    # 返回解析结果，调用方负责路径存在性验证和目录创建。
    return parser.parse_args(argv)


def configure_plot_style() -> None:
    """设置全局中文绘图样式；无数值输入，返回 ``None``。"""

    # 优先使用 Windows 自带微软雅黑，后备字体覆盖常见中文开发环境。
    plt.rcParams["font.sans-serif"] = [
        "Microsoft YaHei",
        "SimHei",
        "Noto Sans CJK SC",
        "DejaVu Sans",
    ]
    # 禁用 Unicode 负号替换，避免部分中文字体缺少 U+2212 时出现方框。
    plt.rcParams["axes.unicode_minus"] = False
    # 白色背景适合 Markdown、PDF 和打印文档。
    plt.rcParams["figure.facecolor"] = "white"
    # 坐标区使用轻微灰白底，增强分位带和网格辨识度。
    plt.rcParams["axes.facecolor"] = "#FAFBFC"
    # 标题使用半粗体形成明确层级。
    plt.rcParams["axes.titleweight"] = "semibold"
    # 细虚线网格只辅助读数，不抢占数据曲线视觉层级。
    plt.rcParams["grid.color"] = "#D9DEE7"
    # 网格透明度限制为 0.55，保持图面克制。
    plt.rcParams["grid.alpha"] = 0.55
    # 保存边界自动裁紧，避免长中文标签被裁切。
    plt.rcParams["savefig.bbox"] = "tight"


def display_name(class_name: str) -> str:
    """把英文类别键映射为中文显示名；未知字符串原样返回。"""

    # 字典不存在时返回英文目录名，避免静默丢失新类别。
    return CLASS_DISPLAY_NAMES.get(class_name, class_name)


def _sha256_file(path: Path) -> str:
    """读取任意二进制文件并返回 64 位 SHA-256；不改变文件内容。"""

    # 新建空 SHA-256 状态，输出固定为 64 位小写十六进制。
    digest = hashlib.sha256()
    # 以二进制只读方式打开，禁止换行和编码转换改变指纹。
    with path.open("rb") as stream:
        # 逐 1 MiB 读取直到文件结束，限制峰值内存。
        while True:
            # chunk 是当前最多 1 MiB 的原始字节。
            chunk = stream.read(1024 * 1024)
            # 空字节串表示已到达文件末尾。
            if not chunk:
                # 退出循环并保留已经累计的哈希状态。
                break
            # 将当前块按原始顺序加入摘要。
            digest.update(chunk)
    # 返回小写十六进制摘要，便于 Git 文本差异保持稳定。
    return digest.hexdigest()


def validate_figure_sha256(
    figure_sha256: Mapping[str, str],
) -> Dict[str, str]:
    """校验四张正式 PNG 的摘要映射并按固定图名顺序返回副本。

    输入键必须与 ``OUTPUT_FILES`` 中除 JSON 清单外的四个 PNG 文件名完全
    一致；每个值必须是 64 位小写十六进制 SHA-256。函数不读取文件，
    仅验证调用方在图片写盘后计算出的内容指纹，错误时抛出 ``ValueError``。
    """

    # figure_names 按时域、频谱、分布、热力图固定顺序列出正式 PNG 名。
    figure_names = (
        OUTPUT_FILES["time_domain"],
        OUTPUT_FILES["power_spectrum"],
        OUTPUT_FILES["feature_distribution"],
        OUTPUT_FILES["feature_heatmap"],
    )
    # expected_names 用集合比较键范围，避免遗漏某图或混入 JSON、临时图。
    expected_names = set(figure_names)
    # actual_names 复制调用方映射的全部键，后续不依赖映射实现类型。
    actual_names = set(figure_sha256)
    # 键集合必须精确相同，缺失和多余均会使清单无法完整验证资产。
    if actual_names != expected_names:
        # missing_names 按稳定顺序列出尚未提供摘要的正式图。
        missing_names = sorted(expected_names - actual_names)
        # extra_names 按稳定顺序列出不属于正式四图的额外键。
        extra_names = sorted(actual_names - expected_names)
        # 错误同时报告缺失和多余项，便于生成器调用方一次修正。
        raise ValueError(
            "图表 SHA-256 映射不完整或含额外项："
            f"missing={missing_names}, extra={extra_names}"
        )
    # validated 保存固定顺序、已经通过格式校验的摘要副本。
    validated: Dict[str, str] = {}
    # 逐张正式图验证摘要格式，终止条件是四个固定图名全部处理完。
    for figure_name in figure_names:
        # digest 是当前 PNG 原始字节的预计算 SHA-256 文本。
        digest = figure_sha256[figure_name]
        # 摘要必须是字符串且长度恰为 SHA-256 的 64 个十六进制字符。
        if not isinstance(digest, str) or len(digest) != 64:
            # 错误只暴露图名和格式，不输出本机路径。
            raise ValueError(f"图表 SHA-256 长度或类型非法：{figure_name}")
        # 小写十六进制字符集合锁定跨平台稳定序列化格式。
        if any(character not in "0123456789abcdef" for character in digest):
            # 大写、空格、前缀和非十六进制字符均被拒绝。
            raise ValueError(f"图表 SHA-256 必须为 64 位小写十六进制：{figure_name}")
        # 将合法摘要写入新字典，避免调用方随后修改原映射影响清单。
        validated[figure_name] = digest
    # 返回固定顺序的四图摘要映射。
    return validated


def dataset_fingerprint(dataset_dir: Path) -> Tuple[str, Dict[str, int], int]:
    """扫描类别 TXT，返回整套摘要、每类文件数和总数；不含绝对路径。"""

    # 转为 Path 并验证目录存在，错误尽早暴露而不是生成空图。
    root = Path(dataset_dir)
    # 非目录输入无法形成类别树，立即拒绝。
    if not root.is_dir():
        # 错误包含用户传入路径，便于命令行定位。
        raise FileNotFoundError(f"数据集目录不存在：{root}")
    # class_directories 只接受数据根第一层动作目录，与 scan_dataset 的类别口径一致。
    class_directories = sorted(
        path for path in root.iterdir() if path.is_dir()
    )
    # files 只纳入第一层动作目录直属 TXT；根 ReadMe.txt 和嵌套说明不属于动作样本。
    files = sorted(
        (
            path
            for class_directory in class_directories
            for path in class_directory.glob("*.txt")
            if path.is_file()
        ),
        key=lambda path: path.relative_to(root).as_posix(),
    )
    # 没有数据文件时拒绝生成看似成功的空图。
    if not files:
        # 明确期望按类别目录存放 TXT。
        raise ValueError(f"数据集没有 TXT 文件：{root}")
    # overall 累计相对路径、字节数和内容摘要。
    overall = hashlib.sha256()
    # class_file_counts 按第一层目录统计每类文件数。
    class_file_counts: Dict[str, int] = {}
    # 遍历排序文件，终止于最后一个 TXT。
    for path in files:
        # relative 使用 POSIX 分隔符，使 Windows 和 Linux 产生相同清单。
        relative = path.relative_to(root).as_posix()
        # 第一段目录名必定是动作类别，因为根目录 TXT 已在枚举阶段排除。
        class_name = relative.split("/", maxsplit=1)[0]
        # 当前类别文件数加一。
        class_file_counts[class_name] = class_file_counts.get(class_name, 0) + 1
        # 相对路径编码后加入摘要，绝对盘符不会进入指纹。
        overall.update(relative.encode("utf-8"))
        # 零字节分隔路径和数字字段，避免字符串连接歧义。
        overall.update(b"\0")
        # 文件字节数以十进制文本加入摘要。
        overall.update(str(path.stat().st_size).encode("ascii"))
        # 第二个零字节分隔长度和内容哈希。
        overall.update(b"\0")
        # 单文件 SHA-256 以 ASCII 加入整套指纹。
        overall.update(_sha256_file(path).encode("ascii"))
        # 换行分隔每个文件记录，固定记录边界。
        overall.update(b"\n")
    # 返回整套摘要、排序后的类别计数和总文件数。
    return overall.hexdigest(), dict(sorted(class_file_counts.items())), len(files)


def _record_relative_path(record: ImuRecord, dataset_dir: Path) -> str:
    """把 ImuRecord 源路径转为数据集内 POSIX 相对字符串；越界时抛错。"""

    # resolve 只用于确认相对关系，返回值不会写入清单。
    resolved_root = Path(dataset_dir).resolve()
    # 记录必须位于数据集根目录内，否则清单会泄露或混入外部样本。
    try:
        # relative_to 强制源文件属于数据集树。
        relative = Path(record.path).resolve().relative_to(resolved_root)
    # ValueError 表示记录来自数据集目录外。
    except ValueError as exc:
        # 立即拒绝，避免绝对路径回退。
        raise ValueError(f"验证文件不在数据集目录内：{record.path}") from exc
    # 统一写为 POSIX 相对路径，保证跨平台稳定。
    return relative.as_posix()


def collect_validation_windows(
    records: Sequence[ImuRecord],
    dataset_dir: Path,
    window_len: int,
    step_len: int,
    rest_threshold: float,
    active_point_threshold: float,
) -> ValidationCorpus:
    """收集验证窗口。

    输入记录来自文件级验证划分；每个 TXT 经单位换算后为 ``[N,6]``，通道
    ``gx、gy、gz、ax、ay、az``，单位 ``deg/s、g``。窗口长度和步长单位为点，
    两个阈值均由训练划分静坐文件估计。返回窗口 ``[62,6]``、过滤统计和相对
    文件计数；过短文件被统计，全部为空时抛出 ``ValueError``。
    """

    # windows 保存全部有效验证窗口，不做增强、不读取测试集。
    windows: List[ValidationWindow] = []
    # skipped 与训练端 build_samples 使用同名统计字段，便于交叉审计。
    skipped = {
        "too_short": 0,
        "rest_filtered": 0,
        "kept_windows": 0,
        "files_without_valid_window": 0,
        "motion_edge_trimmed_points": 0,
    }
    # class_window_counts 对每类窗口计数。
    class_window_counts: Dict[str, int] = {}
    # file_window_counts 对每个相对文件计数，包括最终为零的文件。
    file_window_counts: Dict[str, int] = {}
    # 输入先按类别、相对路径排序，消除 train_test_split 返回顺序差异。
    ordered_records = sorted(
        records,
        key=lambda record: (
            record.label,
            _record_relative_path(record, dataset_dir),
        ),
    )
    # 逐验证文件加载、裁剪并生成固定滑窗。
    for record in ordered_records:
        # source_file 是清单唯一记录的相对来源路径。
        source_file = _record_relative_path(record, dataset_dir)
        # 文件计数先置零，保证被完全过滤的文件仍可审计。
        file_window_counts[source_file] = 0
        # raw_data 形状 [N,6]，训练加载器完成 LSB 到 deg/s、g 的单位换算。
        raw_data = load_imu_file(record.path)
        # 静坐是目标类别，不使用活动门裁剪。
        if record.label == SIT_CLASS_NAME:
            # 静坐保留完整记录，窗口起点相对原始文件从零计算。
            trim_start = 0
            # data 指向完整静坐六轴数据。
            data = raw_data
        # 动态动作按训练端相同活动段边界保留 0.5 秒上下文。
        else:
            # motion_segment_bounds 只用训练集估计的活动阈值确定边界。
            trim_start, trim_end = motion_segment_bounds(
                raw_data,
                active_point_threshold,
            )
            # data 是原始单位数据的动作段视图，清洗在每窗特征入口统一执行。
            data = raw_data[trim_start:trim_end]
            # 累计首尾被删除采样点数。
            skipped["motion_edge_trimmed_points"] += len(raw_data) - len(data)
        # 记录短于 62 点时没有合法完整窗口。
        if len(data) < window_len:
            # 过短文件数加一并继续下一文件。
            skipped["too_short"] += 1
            # 当前文件没有可用窗口。
            skipped["files_without_valid_window"] += 1
            # 结束当前文件处理。
            continue
        # valid_for_record 保存当前文件通过活动筛选的窗口和原始起点。
        valid_for_record: List[Tuple[int, np.ndarray]] = []
        # 起点从零按 step_len 推进，最后一个窗口必须完整落在裁剪数据内。
        for local_start in range(0, len(data) - window_len + 1, step_len):
            # window 形状固定 [window_len,6]。
            window = data[local_start : local_start + window_len]
            # 使用训练端标签相关门过滤静止或缺乏动态证据的窗口。
            if not keep_window_for_label(
                window,
                record.label,
                rest_threshold,
                active_point_threshold,
            ):
                # 被过滤窗口数加一。
                skipped["rest_filtered"] += 1
                # 当前窗口不进入可视化和特征统计。
                continue
            # 原始文件起点等于裁剪起点加局部窗口起点。
            original_start = trim_start + local_start
            # 保存有效窗口的独立 float32 副本，避免后续视图引用被意外修改。
            valid_for_record.append(
                (original_start, np.asarray(window, dtype=np.float32).copy())
            )
        # 没有有效窗口时执行与训练端相同的非高动态类别回退。
        if not valid_for_record:
            # 无有效窗口文件数加一。
            skipped["files_without_valid_window"] += 1
            # 高动态类别缺乏活动证据时不得强行选窗，保持 fail-closed。
            if record.label not in HIGH_DYNAMIC_CLASSES:
                # candidate_starts 枚举全部完整窗口起点。
                candidate_starts = list(
                    range(0, len(data) - window_len + 1, step_len)
                )
                # 至少存在一个完整窗口时才可回退。
                if candidate_starts:
                    # scored 保存清洗后运动分数、起点和原始窗口。
                    scored = [
                        (
                            motion_score(
                                preprocess_imu_window(
                                    data[start : start + window_len]
                                )
                            ),
                            start,
                            data[start : start + window_len],
                        )
                        for start in candidate_starts
                    ]
                    # 静坐选择运动分数最低窗口，避免偶发扰动代表静坐。
                    if record.label == SIT_CLASS_NAME:
                        # 分数并列时 min 保留更早起点，结果确定。
                        _, fallback_start, fallback_window = min(
                            scored,
                            key=lambda item: (item[0], item[1]),
                        )
                    # 其它非高动态动作选择运动分数最高窗口。
                    else:
                        # 分数并列时优先更早起点，通过负起点维持确定性。
                        _, fallback_start, fallback_window = max(
                            scored,
                            key=lambda item: (item[0], -item[1]),
                        )
                    # 回退窗口保留原始文件起点和 float32 副本。
                    valid_for_record.append(
                        (
                            trim_start + fallback_start,
                            np.asarray(fallback_window, dtype=np.float32).copy(),
                        )
                    )
        # 按时间顺序把当前文件窗口加入全局列表。
        for original_start, window in valid_for_record:
            # 构造不含绝对路径的验证窗口记录。
            windows.append(
                ValidationWindow(
                    label=record.label,
                    label_idx=record.label_idx,
                    source_file=source_file,
                    start_point=int(original_start),
                    data=window,
                )
            )
            # 当前类别窗口数加一。
            class_window_counts[record.label] = (
                class_window_counts.get(record.label, 0) + 1
            )
            # 当前文件窗口数加一。
            file_window_counts[source_file] += 1
            # 总保留窗口数加一。
            skipped["kept_windows"] += 1
    # 没有任何验证窗口时拒绝生成空白图。
    if not windows:
        # 错误指出筛选后为空，便于检查数据或阈值。
        raise ValueError("验证集经过训练端窗口筛选后没有有效窗口。")
    # 返回不可变窗口元组和排序统计映射。
    return ValidationCorpus(
        windows=tuple(windows),
        skipped=dict(skipped),
        class_window_counts=dict(sorted(class_window_counts.items())),
        file_window_counts=dict(sorted(file_window_counts.items())),
    )


def aggregate_series(values: np.ndarray) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """聚合 ``[样本数,维数]``，返回三个 ``[维数]`` 分位曲线。

    输入行通常已是文件代表；单位由列继承。输出顺序为
    ``Q0.25、Q0.50、Q0.75``，不抽样。空数组或非二维输入抛错。
    """

    # matrix 形状必须为 [样本数,维数]。
    matrix = np.asarray(values, dtype=np.float64)
    # 非二维或空输入不能形成聚合曲线。
    if matrix.ndim != 2 or matrix.shape[0] == 0 or matrix.shape[1] == 0:
        # 错误包含实际形状，便于测试和数据诊断。
        raise ValueError(f"聚合曲线需要非空二维数组，实际为 {matrix.shape}")
    # q25 表示跨输入样本逐列下四分位。
    q25 = np.percentile(matrix, 25.0, axis=0)
    # median 表示输入样本逐列中位数。
    median = np.percentile(matrix, 50.0, axis=0)
    # q75 表示跨输入样本逐列上四分位。
    q75 = np.percentile(matrix, 75.0, axis=0)
    # 三条曲线均为 [时间点数] 且保持 float64 绘图精度。
    return q25, median, q75


def aggregate_aligned_series(
    values: np.ndarray,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """聚合峰对齐后的 ``[文件数,62]``，忽略平移边缘 NaN。

    每列只在当前时刻仍有真实采样的文件间计算 ``Q0.25/Q0.50/Q0.75``；
    不用首尾复制或循环回绕伪造信号。返回三条 ``[62]`` 曲线，单位继承输入。
    """

    # matrix 形状必须为 [文件数,时间点数]，允许平移边缘出现 NaN。
    matrix = np.asarray(values, dtype=np.float64)
    # 非二维或空输入不能形成文件等权聚合。
    if matrix.ndim != 2 or matrix.shape[0] == 0 or matrix.shape[1] == 0:
        # 错误包含实际形状。
        raise ValueError(f"对齐聚合需要非空二维数组，实际为 {matrix.shape}")
    # q25 先填 NaN；没有任何文件覆盖的边缘点保持空白。
    q25 = np.full(matrix.shape[1], np.nan, dtype=np.float64)
    # median 与 q25 使用相同输出长度。
    median = np.full(matrix.shape[1], np.nan, dtype=np.float64)
    # q75 与 q25 使用相同输出长度。
    q75 = np.full(matrix.shape[1], np.nan, dtype=np.float64)
    # 逐时间点仅选择有限文件值，避免 np.nanpercentile 的全空列警告。
    for point_index in range(matrix.shape[1]):
        # finite_values 是当前相对峰时间上仍有真实采样的文件值。
        finite_values = matrix[
            np.isfinite(matrix[:, point_index]),
            point_index,
        ]
        # 全部文件均无覆盖时保留 NaN，图中显示为空白而非伪零。
        if len(finite_values) == 0:
            # 继续下一时间点。
            continue
        # 当前点下四分位按文件等权计算。
        q25[point_index] = float(np.percentile(finite_values, 25.0))
        # 当前点中位数按文件等权计算。
        median[point_index] = float(np.percentile(finite_values, 50.0))
        # 当前点上四分位按文件等权计算。
        q75[point_index] = float(np.percentile(finite_values, 75.0))
    # 返回三条文件级分位曲线。
    return q25, median, q75


def collapse_rows_by_file(
    values: np.ndarray,
    source_files: Sequence[str],
) -> Tuple[np.ndarray, List[str]]:
    """把窗口矩阵按文件逐列取中位数，返回文件等权矩阵和排序文件名。

    ``values`` 形状 ``[窗口数,特征数]``，单位按列继承；``source_files`` 长度
    等于窗口数。每个文件无论包含多少重叠窗口都只产生一行。
    """

    # matrix 统一为 float64，降低多窗口分位数累计误差。
    matrix = np.asarray(values, dtype=np.float64)
    # 输入必须是非空二维窗口矩阵。
    if matrix.ndim != 2 or matrix.shape[0] == 0 or matrix.shape[1] == 0:
        # 报告非法形状。
        raise ValueError(f"文件折叠需要非空二维数组，实际为 {matrix.shape}")
    # 文件标签数必须与窗口行数一一对应。
    if len(source_files) != matrix.shape[0]:
        # 报告两个长度。
        raise ValueError(
            f"窗口数与文件标签数不一致：{matrix.shape[0]} != {len(source_files)}"
        )
    # ordered_files 按 POSIX 相对路径排序，保证跨运行输出稳定。
    ordered_files = sorted(set(source_files))
    # file_rows 保存每个文件的逐列窗口中位数。
    file_rows: List[np.ndarray] = []
    # 逐文件选择全部窗口行。
    for source_file in ordered_files:
        # row_indices 是当前文件在窗口矩阵中的行号。
        row_indices = [
            index
            for index, candidate in enumerate(source_files)
            if candidate == source_file
        ]
        # 每个 ordered_files 成员至少对应一行；保护分支用于防御后续重构。
        if not row_indices:
            # 不允许生成空文件代表。
            raise ValueError(f"文件没有对应窗口：{source_file}")
        # 当前文件逐列中位数形成唯一等权代表行。
        file_rows.append(np.median(matrix[row_indices], axis=0))
    # 返回 [文件数,特征数] 和相同顺序的文件键。
    return np.vstack(file_rows), ordered_files


def _robust_shape_rows(values: np.ndarray) -> np.ndarray:
    """逐行稳健标准化 ``[窗口数,时间点数]``，返回同形状无量纲波形。"""

    # matrix 统一为 float64。
    matrix = np.asarray(values, dtype=np.float64)
    # 输入必须为非空二维序列矩阵。
    if matrix.ndim != 2 or matrix.shape[0] == 0 or matrix.shape[1] == 0:
        # 报告非法形状。
        raise ValueError(f"形状标准化需要非空二维数组，实际为 {matrix.shape}")
    # center 是每个窗口自身中位数，形状 [窗口数,1]。
    center = np.median(matrix, axis=1, keepdims=True)
    # q25 是每个窗口自身下四分位。
    q25 = np.percentile(matrix, 25.0, axis=1, keepdims=True)
    # q75 是每个窗口自身上四分位。
    q75 = np.percentile(matrix, 75.0, axis=1, keepdims=True)
    # scale 初始使用 IQR，抑制落地冲击极值对尺度的支配。
    scale = q75 - q25
    # fallback_std 为 IQR 近零窗口提供标准差后备。
    fallback_std = np.std(matrix, axis=1, keepdims=True)
    # IQR 近零时改用标准差。
    scale = np.where(scale >= 1e-9, scale, fallback_std)
    # 常量窗口最终使用 1，中心化后仍为全零。
    scale = np.where(scale >= 1e-9, scale, 1.0)
    # 返回无量纲形状，供不同物理单位联合计算 medoid。
    return (matrix - center) / scale


def select_medoid_index(shape_rows: np.ndarray) -> int:
    """返回 ``[窗口数,形状维数]`` 中平均两两平方距离最小的真实行索引。

    输入已无量纲；返回整数索引。相同得分由 ``argmin`` 选择最早行，调用方
    预先按原始起点排序，因此平局确定且不手选。
    """

    # matrix 转为 float64，避免 Gram 矩阵舍入产生明显负距离。
    matrix = np.asarray(shape_rows, dtype=np.float64)
    # 至少一行一列才能选择 medoid。
    if matrix.ndim != 2 or matrix.shape[0] == 0 or matrix.shape[1] == 0:
        # 报告非法形状。
        raise ValueError(f"medoid 需要非空二维数组，实际为 {matrix.shape}")
    # 单窗口文件的唯一窗口自然是 medoid。
    if matrix.shape[0] == 1:
        # 返回唯一索引零。
        return 0
    # squared_norms 保存每行平方范数，形状 [窗口数]。
    squared_norms = np.sum(matrix * matrix, axis=1)
    # pairwise 使用 ||a-b||²=||a||²+||b||²-2a·b，形状 [窗口数,窗口数]。
    pairwise = (
        squared_norms[:, None]
        + squared_norms[None, :]
        - 2.0 * (matrix @ matrix.T)
    )
    # 浮点舍入产生的微小负数截为零。
    pairwise = np.maximum(pairwise, 0.0)
    # total_distance 是每行到全部候选的平方距离总和。
    total_distance = np.sum(pairwise, axis=1)
    # argmin 返回第一个最小值，保证平局选择更早窗口。
    return int(np.argmin(total_distance))


def objective_motion_peak_index(
    acc_vertical: np.ndarray,
    gyro_mag: np.ndarray,
) -> int:
    """定位联合运动峰，输入两个 ``[62]`` 序列，返回点索引。

    垂直加速度单位 ``g``，角速度模长单位 ``deg/s``。两者分别用 IQR/标准差
    转为无量纲动态强度，等权相加并用 ``[0.25,0.5,0.25]`` 平滑后取最早峰。
    """

    # acc 和 gyro 转为等长 float64 一维数组。
    acc = np.asarray(acc_vertical, dtype=np.float64)
    # gyro 是角速度模长。
    gyro = np.asarray(gyro_mag, dtype=np.float64)
    # 两个信号必须一维、等长且非空。
    if acc.ndim != 1 or gyro.ndim != 1 or len(acc) == 0 or len(acc) != len(gyro):
        # 报告两个实际形状。
        raise ValueError(f"运动峰输入形状不一致：{acc.shape} 与 {gyro.shape}")

    # 内部函数把一维动态强度缩放为无量纲量。
    def scale_activity(activity: np.ndarray) -> np.ndarray:
        """输入 ``[N]`` 非负强度，返回 IQR/标准差缩放后的无量纲 ``[N]``。"""

        # q25 和 q75 构造稳健尺度。
        q25 = float(np.percentile(activity, 25.0))
        # 上四分位用于 IQR。
        q75 = float(np.percentile(activity, 75.0))
        # scale 优先使用 IQR。
        scale = q75 - q25
        # IQR 近零时使用总体标准差。
        if scale < 1e-9:
            # 标准差仍继承输入单位。
            scale = float(np.std(activity))
        # 常量强度返回零，避免除零。
        if scale < 1e-9:
            # zeros_like 保持长度和 float64。
            return np.zeros_like(activity)
        # 除以尺度得到无量纲强度。
        return activity / scale

    # acc_activity 使用相对中位重力方向的绝对偏移。
    acc_activity = np.abs(acc - float(np.median(acc)))
    # gyro_activity 使用高于低四分位的正向角速度动态。
    gyro_activity = np.maximum(
        gyro - float(np.percentile(gyro, 25.0)),
        0.0,
    )
    # 两种无量纲动态强度等权相加，不让 deg/s 数值尺度支配 g。
    combined = 0.5 * scale_activity(acc_activity) + 0.5 * scale_activity(
        gyro_activity
    )
    # 三点对称核抑制单点尖锐噪声，权重和为一。
    smoothed = np.convolve(
        combined,
        np.asarray([0.25, 0.50, 0.25], dtype=np.float64),
        mode="same",
    )
    # argmax 在并列时返回最早索引，结果确定。
    return int(np.argmax(smoothed))


def align_series_to_peak(
    values: np.ndarray,
    peak_index: int,
    target_index: int,
) -> np.ndarray:
    """把一维序列峰平移到目标点，返回同长度数组，空边缘填 NaN。

    输入和输出单位相同；不插值、不循环回绕、不复制边缘，因此不会伪造动作。
    """

    # source 是任意物理单位的一维 float64 序列。
    source = np.asarray(values, dtype=np.float64)
    # 非一维或空输入无法平移。
    if source.ndim != 1 or len(source) == 0:
        # 报告实际形状。
        raise ValueError(f"峰对齐需要非空一维数组，实际为 {source.shape}")
    # 两个索引必须位于序列范围内。
    if not 0 <= peak_index < len(source) or not 0 <= target_index < len(source):
        # 报告非法索引和长度。
        raise ValueError(
            f"峰索引越界：peak={peak_index} target={target_index} length={len(source)}"
        )
    # shift 大于零表示向右移动。
    shift = int(target_index - peak_index)
    # aligned 初始全为 NaN，表示平移后没有真实采样覆盖的边缘。
    aligned = np.full(len(source), np.nan, dtype=np.float64)
    # 源起点在左移时跳过被移出窗口的前段。
    source_start = max(0, -shift)
    # 目标起点在右移时从 shift 开始写。
    target_start = max(0, shift)
    # copy_length 是源和目标剩余长度的较小值。
    copy_length = min(
        len(source) - source_start,
        len(source) - target_start,
    )
    # 把仍位于窗口内的真实采样复制到目标位置。
    aligned[target_start : target_start + copy_length] = source[
        source_start : source_start + copy_length
    ]
    # 返回含真实值和边缘 NaN 的同长序列。
    return aligned


def file_time_representatives(
    windows: Sequence[ValidationWindow],
) -> List[FileTimeRepresentative]:
    """把验证窗口压成每文件一个时域代表。

    每文件先对 ``acc_vertical[g]`` 与 ``gyro_mag[deg/s]`` 联合形状选择真实
    medoid，再把客观联合运动峰对齐到点 ``len/2``。返回按相对文件名排序的
   代表列表，每文件权重恰为一。
    """

    # groups 按相对源文件收集窗口。
    groups: Dict[str, List[ValidationWindow]] = {}
    # 逐窗口加入对应文件。
    for window in windows:
        # setdefault 首次创建文件列表。
        groups.setdefault(window.source_file, []).append(window)
    # 没有窗口时不能形成文件代表。
    if not groups:
        # fail-closed 报错。
        raise ValueError("没有可生成文件级时域代表的验证窗口。")
    # representatives 保存按文件名排序的结果。
    representatives: List[FileTimeRepresentative] = []
    # 逐文件生成唯一代表。
    for source_file in sorted(groups):
        # file_windows 按原始起点排序，medoid 平局时选择更早窗口。
        file_windows = sorted(
            groups[source_file],
            key=lambda item: item.start_point,
        )
        # 一个文件只能属于一个类别。
        labels = {item.label for item in file_windows}
        # 多类别表示源文件归属或上游标签损坏。
        if len(labels) != 1:
            # 报告相对文件和标签集合。
            raise ValueError(f"同一文件出现多类别：{source_file} {sorted(labels)}")
        # acc_rows 保存每窗垂直加速度。
        acc_rows: List[np.ndarray] = []
        # gyro_rows 保存每窗角速度模长。
        gyro_rows: List[np.ndarray] = []
        # 逐窗复用训练派生序列。
        for window in file_windows:
            # series 是清洗后派生信号映射。
            series = build_feature_series(preprocess_imu_window(window.data))
            # 垂直加速度转为 float64。
            acc_rows.append(
                np.asarray(series["acc_vertical"], dtype=np.float64)
            )
            # 角速度模长转为 float64。
            gyro_rows.append(np.asarray(series["gyro_mag"], dtype=np.float64))
        # acc_matrix 形状 [文件窗口数,62]。
        acc_matrix = np.vstack(acc_rows)
        # gyro_matrix 与 acc_matrix 同形状。
        gyro_matrix = np.vstack(gyro_rows)
        # 两种信号分别逐窗稳健标准化，再拼成 [窗口数,124] 联合形状。
        joint_shape = np.hstack(
            [
                _robust_shape_rows(acc_matrix),
                _robust_shape_rows(gyro_matrix),
            ]
        )
        # medoid_index 是平均两两距离最小的真实窗口索引。
        medoid_index = select_medoid_index(joint_shape)
        # medoid_acc 保留真实物理单位。
        medoid_acc = acc_matrix[medoid_index]
        # medoid_gyro 与加速度来自同一窗口。
        medoid_gyro = gyro_matrix[medoid_index]
        # peak_index 用联合动态强度客观定位。
        peak_index = objective_motion_peak_index(medoid_acc, medoid_gyro)
        # target_index 固定为窗口中心；62 点时为索引 31。
        target_index = len(medoid_acc) // 2
        # 对齐后的加速度边缘用 NaN 表示无真实采样。
        aligned_acc = align_series_to_peak(
            medoid_acc,
            peak_index,
            target_index,
        )
        # 角速度使用完全相同平移量，保持跨信号时序关系。
        aligned_gyro = align_series_to_peak(
            medoid_gyro,
            peak_index,
            target_index,
        )
        # 加入当前文件唯一代表。
        representatives.append(
            FileTimeRepresentative(
                source_file=source_file,
                label=file_windows[0].label,
                acc_vertical=aligned_acc,
                gyro_mag=aligned_gyro,
                medoid_start_point=file_windows[medoid_index].start_point,
                original_peak_index=peak_index,
            )
        )
    # 返回文件等权代表列表。
    return representatives


def relative_power_spectrum(
    values: np.ndarray,
    sample_rate: float = float(SAMPLE_RATE),
) -> Tuple[np.ndarray, np.ndarray]:
    """计算一维时序的单边相对功率谱。

    输入 ``values`` 形状 ``[时间点数]``，单位可为 ``deg/s`` 或 ``g``；
    ``sample_rate`` 单位 Hz。返回频率 ``[Hz]`` 和无量纲相对功率，两者形状
    ``[floor(N/2)+1]``；常量序列返回零谱。
    """

    # x 是一维物理量序列，通常为角速度模长，单位 deg/s。
    x = np.asarray(values, dtype=np.float64)
    # 少于 4 点无法提供有效动作频率分辨率。
    if x.ndim != 1 or len(x) < 4:
        # 立即拒绝形状或长度错误，避免生成误导频谱。
        raise ValueError("功率谱输入必须是一维且至少包含 4 个采样点。")
    # 去除直流均值后乘 Hann 窗，减小有限窗口边缘的频谱泄漏。
    tapered = (x - float(np.mean(x))) * np.hanning(len(x))
    # rfft 计算实数输入的非负频率复数谱。
    spectrum = np.fft.rfft(tapered)
    # 功率是复数幅值平方，单位为输入单位平方。
    power = np.square(np.abs(spectrum))
    # frequencies 给出每个单边频点的 Hz 坐标。
    frequencies = np.fft.rfftfreq(len(x), d=1.0 / float(sample_rate))
    # 直流点强制为零，与训练端谱特征定义一致。
    power[0] = 0.0
    # total 是所有非直流功率之和。
    total = float(np.sum(power))
    # 近常量信号总功率过小时返回全零有限谱。
    if total <= 1e-12:
        # zeros_like 保持频点数量和 float64 类型。
        relative_power = np.zeros_like(power)
    # 有效动态信号转换为总和为 1 的无量纲相对功率。
    else:
        # 相除消除不同人员动作幅度对频谱形状的直接支配。
        relative_power = power / total
    # 返回频率和相对功率，两者形状均为 [floor(N/2)+1]。
    return frequencies, relative_power


def robust_heatmap_scores(class_medians: np.ndarray) -> np.ndarray:
    """缩放 ``[类别数,特征数]`` 中位数，返回同形状无量纲 ``[-3,3]`` 值。"""

    # values 形状为 [类别数,关键特征数]。
    values = np.asarray(class_medians, dtype=np.float64)
    # 热力图至少需要一个类别和一个特征。
    if values.ndim != 2 or values.shape[0] == 0 or values.shape[1] == 0:
        # 拒绝空数组，避免下游 percentile 返回警告和 NaN。
        raise ValueError(f"热力图需要非空二维数组，实际为 {values.shape}")
    # center 是每个特征跨类别中位数。
    center = np.median(values, axis=0)
    # q25 是每个特征跨类别下四分位。
    q25 = np.percentile(values, 25.0, axis=0)
    # q75 是每个特征跨类别上四分位。
    q75 = np.percentile(values, 75.0, axis=0)
    # scale 使用 IQR；常量列改为 1，避免除零且输出全零。
    scale = q75 - q25
    # 小于 1e-9 的列视为无类别差异。
    scale[scale < 1e-9] = 1.0
    # scores 的正负表示高于或低于所有类别的稳健中心。
    scores = (values - center) / scale
    # 截断到 [-3,3]，防止单个极端类别压缩其余色阶。
    return np.clip(scores, -3.0, 3.0)


def _windows_by_class(
    windows: Sequence[ValidationWindow],
    class_names: Sequence[str],
) -> Dict[str, List[ValidationWindow]]:
    """按类别键分组 ValidationWindow；返回包含全部已知类别键的映射。"""

    # mapping 为每个已知类别预建空列表，保证缺失类别可被显式检查。
    mapping: Dict[str, List[ValidationWindow]] = {
        class_name: [] for class_name in class_names
    }
    # 逐窗口追加到对应类别。
    for window in windows:
        # 未知标签表示类别映射与扫描结果不一致。
        if window.label not in mapping:
            # 立即拒绝，避免图中静默遗漏类别。
            raise ValueError(f"验证窗口出现未知类别：{window.label}")
        # 保留输入的确定时间顺序。
        mapping[window.label].append(window)
    # 返回包含全部类别键的映射。
    return mapping


def validation_feature_table(
    windows: Sequence[ValidationWindow],
) -> Tuple[np.ndarray, np.ndarray, List[str], List[str]]:
    """把窗口特征折叠为文件等权表。

    输入 ``K`` 个 ``[62,6]``，通道顺序 ``gx、gy、gz、ax、ay、az``；先提取
    ``[K,297]``，再对每个文件的窗口逐列取中位数。返回
    ``[文件数,297]``、``[文件数]`` 标签、名称表和排序相对文件；每文件等权。
    """

    # feature_names 是 Python/ESP32 固定的 297 维顺序。
    feature_names = build_feature_names()
    # rows 保存每个验证窗口的 297 维 float32 特征。
    rows: List[np.ndarray] = []
    # source_files 保存每个窗口对应的相对文件，用于文件内折叠。
    source_files: List[str] = []
    # source_labels 保存每个文件唯一的类别索引。
    source_labels: Dict[str, int] = {}
    # 按确定顺序逐窗提取，不增强、不随机抽样。
    for index, window in enumerate(windows):
        # 每 100 窗输出一次进度，长数据集运行时保持可观察。
        if index == 0 or index % 100 == 0:
            # 日志只含窗口计数，不输出本机绝对路径。
            print(
                f"features validation window={index + 1}/{len(windows)}",
                flush=True,
            )
        # extract_features 内部执行同一尖峰清洗并返回 [297]。
        feature_row = extract_features(window.data)
        # 维数漂移会使文档特征名与模型错位，立即拒绝。
        if len(feature_row) != len(feature_names):
            # 错误同时报告数值和名称维度。
            raise ValueError(
                f"特征维数不一致：values={len(feature_row)} names={len(feature_names)}"
            )
        # 当前特征行加入矩阵。
        rows.append(np.asarray(feature_row, dtype=np.float32))
        # 当前窗口相对文件加入平行列表。
        source_files.append(window.source_file)
        # 同一文件首次出现时记录类别索引。
        if window.source_file not in source_labels:
            # 保存训练端固定类别索引。
            source_labels[window.source_file] = int(window.label_idx)
        # 已记录文件再次出现时必须保持同一类别。
        elif source_labels[window.source_file] != int(window.label_idx):
            # 标签冲突时立即拒绝。
            raise ValueError(
                f"同一文件类别索引不一致：{window.source_file}"
            )
    # 结束时输出总窗口数。
    print(
        f"features validation window={len(windows)}/{len(windows)} complete=true",
        flush=True,
    )
    # window_matrix 形状 [窗口数,297]。
    window_matrix = np.vstack(rows).astype(np.float32)
    # 每个文件先对自身重叠窗口逐列取中位数。
    file_matrix, ordered_files = collapse_rows_by_file(
        window_matrix,
        source_files,
    )
    # file_labels 与 ordered_files 一一对应。
    file_labels = np.asarray(
        [source_labels[source_file] for source_file in ordered_files],
        dtype=np.int64,
    )
    # 返回文件级特征、文件级标签、名称和相对文件。
    return (
        file_matrix.astype(np.float32),
        file_labels,
        feature_names,
        ordered_files,
    )


def _save_figure(figure: plt.Figure, output_path: Path) -> None:
    """把 Figure 写为指定正式 PNG，固定 300 DPI；返回 ``None`` 并关闭图。"""

    # 保存为不透明白底 PNG，适合 Git、Markdown 和打印。
    figure.savefig(
        output_path,
        dpi=300,
        facecolor="white",
        transparent=False,
    )
    # 关闭 Figure，避免连续生成四张高分辨率图时累积内存。
    plt.close(figure)


def render_time_domain_figure(
    windows_by_class: Mapping[str, Sequence[ValidationWindow]],
    output_path: Path,
) -> None:
    """绘制六类时域图。

    输入映射中的窗口均为 ``[62,6]``；输出左列 ``acc_vertical[g]``、右列
    ``gyro_mag[deg/s]`` 的文件中位数/IQR，时间轴 ``(i-31)/25 s``，
    写入 300 DPI PNG。
    """

    # 创建六行两列：每个动作独占一行，避免双 Y 轴造成尺度误读。
    figure, axes = plt.subplots(
        len(FIELD_ACTIONS),
        2,
        figsize=(14.0, 17.0),
        sharex=True,
    )
    # 总标题说明中心线和阴影的统计含义。
    figure.suptitle(
        "六种现场动作的文件等权时域特征\n"
        "每文件先选联合形状 medoid 并对齐客观运动峰；实线为文件中位数，阴影为文件 IQR",
        fontsize=18,
        y=0.995,
    )
    # 逐动作绘制左侧垂直加速度和右侧角速度模长。
    for row_index, class_name in enumerate(FIELD_ACTIONS):
        # 当前动作必须存在有效验证窗口。
        class_windows = list(windows_by_class.get(class_name, ()))
        # 缺失时 fail-closed，避免文档宣称覆盖但图为空。
        if not class_windows:
            # 错误给出类别键。
            raise ValueError(f"现场动作缺少有效验证窗口：{class_name}")
        # 每个验证文件通过联合形状 medoid 和客观峰对齐产生一个等权代表。
        file_representatives = file_time_representatives(class_windows)
        # 两列固定派生序列及其中文单位标签。
        for column_index, (series_name, y_label) in enumerate(
            (
                ("acc_vertical", "垂直加速度 (g)"),
                ("gyro_mag", "角速度模长 (deg/s)"),
            )
        ):
            # axis 是当前动作和当前信号的坐标区。
            axis = axes[row_index, column_index]
            # matrix 形状 [当前类验证文件数,62]，每文件恰好一行。
            matrix = np.vstack(
                [
                    np.asarray(
                        getattr(representative, series_name),
                        dtype=np.float64,
                    )
                    for representative in file_representatives
                ]
            )
            # 对峰对齐文件代表逐点计算中位数和 IQR，忽略无真实覆盖的 NaN 边缘。
            q25, median, q75 = aggregate_aligned_series(matrix)
            # 时间坐标以客观运动峰为零，62 点窗口中心索引为 31。
            time_seconds = (
                np.arange(matrix.shape[1], dtype=np.float64)
                - float(matrix.shape[1] // 2)
            ) / float(SAMPLE_RATE)
            # 当前信号使用固定色盲友好颜色。
            color = SIGNAL_COLORS[series_name]
            # 中位数实线展示类别典型波形。
            axis.plot(
                time_seconds,
                median,
                color=color,
                linewidth=1.8,
                label="验证文件中位数",
            )
            # 分位带展示文件间动作执行差异，而不是隐藏自然变化。
            axis.fill_between(
                time_seconds,
                q25,
                q75,
                color=color,
                alpha=0.22,
                linewidth=0.0,
                label="25%~75%",
            )
            # 每个子图启用轻网格便于比较周期和幅值。
            axis.grid(True, linestyle="--", linewidth=0.6)
            # Y 轴保留物理单位。
            axis.set_ylabel(y_label, fontsize=10)
            # 第一行显示两列信号名称。
            if row_index == 0:
                # 标题包含英文特征键，便于对应公式文档和代码。
                axis.set_title(f"{y_label.split(' (')[0]} · {series_name}", fontsize=12)
            # 左列在图内标注动作和有效文件数。
            if column_index == 0:
                # 文字位于左上角，不覆盖主波形中心。
                axis.text(
                    0.015,
                    0.92,
                    f"{display_name(class_name)}  文件 n={len(file_representatives)}",
                    transform=axis.transAxes,
                    ha="left",
                    va="top",
                    fontsize=11,
                    fontweight="semibold",
                    bbox={
                        "boxstyle": "round,pad=0.25",
                        "facecolor": "white",
                        "edgecolor": "#C9D2DF",
                        "alpha": 0.90,
                    },
                )
            # 最后一行显示公共时间轴标签。
            if row_index == len(FIELD_ACTIONS) - 1:
                # 时间轴单位为秒，零点是客观联合运动峰。
                axis.set_xlabel("相对客观运动峰时间 (s)")
    # 自动调整子图间距，同时为总标题保留顶部空间。
    figure.tight_layout(rect=(0.0, 0.0, 1.0, 0.982))
    # 保存正式 PNG 并释放内存。
    _save_figure(figure, output_path)


def render_power_spectrum_figure(
    windows_by_class: Mapping[str, Sequence[ValidationWindow]],
    class_names: Sequence[str],
    output_path: Path,
) -> None:
    """绘制全部类别 ``gyro_mag`` 文件等权相对功率谱，写入 300 DPI PNG。"""

    # 4x3 网格容纳 11 类，最后一个空格隐藏。
    figure, axes = plt.subplots(4, 3, figsize=(15.0, 14.0), sharex=True)
    # 总标题说明去均值、Hann 窗和相对功率口径。
    figure.suptitle(
        "11 类动作文件等权角速度模长相对功率谱\n"
        "每文件先取窗口谱中位数；实线为文件中位数，阴影为文件 IQR",
        fontsize=18,
        y=0.995,
    )
    # tab20 提供足够区分的离散类别颜色。
    palette = plt.get_cmap("tab20")
    # 扁平化坐标区，按固定 class_names 顺序绘制。
    flat_axes = axes.ravel()
    # 逐类别先计算窗口谱，再折叠为文件等权相对谱。
    for class_index, class_name in enumerate(class_names):
        # 当前类别窗口列表。
        class_windows = list(windows_by_class.get(class_name, ()))
        # 任一类别缺失会破坏 11 类对比完整性。
        if not class_windows:
            # fail-closed 报错。
            raise ValueError(f"功率谱类别缺少有效验证窗口：{class_name}")
        # spectra_by_file 按相对文件保存窗口相对功率列表。
        spectra_by_file: Dict[str, List[np.ndarray]] = {}
        # frequencies 在固定 62 点窗口下对全部窗口相同。
        frequencies: np.ndarray | None = None
        # 逐窗口计算角速度模长谱。
        for window in class_windows:
            # 清洗后构建与模型一致的 gyro_mag。
            gyro_mag = build_feature_series(
                preprocess_imu_window(window.data)
            )["gyro_mag"]
            # 当前频率和相对功率均为单边数组。
            current_frequencies, relative_power = relative_power_spectrum(gyro_mag)
            # 首个窗口保存公共频率坐标。
            if frequencies is None:
                # 复制为 float64，避免后续引用变化。
                frequencies = np.asarray(current_frequencies, dtype=np.float64)
            # 当前相对谱加入所属文件，重叠窗口不会直接进入类别统计。
            spectra_by_file.setdefault(window.source_file, []).append(
                np.asarray(relative_power, dtype=np.float64)
            )
        # 类型检查后 frequencies 必定由至少一个窗口赋值。
        assert frequencies is not None
        # file_spectra 保存每文件窗口谱逐频点中位数。
        file_spectra = [
            np.median(np.vstack(spectra_by_file[source_file]), axis=0)
            for source_file in sorted(spectra_by_file)
        ]
        # spectrum_matrix 形状 [当前类验证文件数,频点数]，每文件恰好一行。
        spectrum_matrix = np.vstack(file_spectra)
        # 只展示 0~5 Hz 人体动作主频段。
        frequency_mask = frequencies <= POWER_MAX_HZ + 1e-12
        # 截取后的频率坐标以 Hz 为单位。
        shown_frequencies = frequencies[frequency_mask]
        # 相对功率乘 100 转为百分比，便于跨类别解释。
        shown_spectra = spectrum_matrix[:, frequency_mask] * 100.0
        # 逐频点计算 IQR 和中位数。
        q25, median, q75 = aggregate_series(shown_spectra)
        # 当前类别对应 tab20 的确定颜色。
        color = palette(class_index % palette.N)
        # axis 是当前类别子图。
        axis = flat_axes[class_index]
        # 中位数谱显示主频位置和能量集中程度。
        axis.plot(
            shown_frequencies,
            median,
            color=color,
            linewidth=1.8,
        )
        # IQR 阴影展示文件间频谱变化。
        axis.fill_between(
            shown_frequencies,
            q25,
            q75,
            color=color,
            alpha=0.24,
            linewidth=0.0,
        )
        # 标题包含中文类别和有效文件数。
        axis.set_title(
            f"{display_name(class_name)}  文件 n={len(file_spectra)}",
            fontsize=11,
        )
        # 频率范围固定为 0~5 Hz。
        axis.set_xlim(0.0, POWER_MAX_HZ)
        # 相对功率下限固定为零。
        axis.set_ylim(bottom=0.0)
        # 轻网格辅助读取谱峰。
        axis.grid(True, linestyle="--", linewidth=0.6)
        # 左列显示纵轴单位。
        if class_index % 3 == 0:
            # 每频点相对功率以百分比表示。
            axis.set_ylabel("相对功率 (%)")
        # 最后一行显示频率单位。
        if class_index >= 9:
            # 横轴为 Hz。
            axis.set_xlabel("频率 (Hz)")
    # 隐藏超过类别数的空坐标区。
    for unused_axis in flat_axes[len(class_names) :]:
        # set_visible(False) 不留下边框或刻度。
        unused_axis.set_visible(False)
    # 调整布局并保留总标题。
    figure.tight_layout(rect=(0.0, 0.0, 1.0, 0.972))
    # 保存正式 PNG。
    _save_figure(figure, output_path)


def _key_feature_indices(feature_names: Sequence[str]) -> List[int]:
    """从 297 个名称解析八个关键列，返回整数索引；缺失名称时抛错。"""

    # name_to_index 建立名称到位置的唯一映射。
    name_to_index = {
        feature_name: index for index, feature_name in enumerate(feature_names)
    }
    # missing 收集训练端未提供的关键特征。
    missing = [
        feature_name
        for feature_name, _, _ in KEY_FEATURES
        if feature_name not in name_to_index
    ]
    # 任何缺失都表示文档脚本与特征合同漂移。
    if missing:
        # 一次报告全部缺失名称。
        raise KeyError(f"关键特征不存在：{missing}")
    # 按 KEY_FEATURES 固定顺序返回索引。
    return [name_to_index[item[0]] for item in KEY_FEATURES]


def render_feature_distribution_figure(
    feature_matrix: np.ndarray,
    labels: np.ndarray,
    feature_names: Sequence[str],
    class_names: Sequence[str],
    output_path: Path,
) -> None:
    """绘制 ``[K,297]`` 文件代表特征的 11 类箱线分布，写入 300 DPI PNG。

    ``K`` 是有效验证文件数，``labels`` 形状 ``[K]``；八个特征单位由
    ``KEY_FEATURES`` 给出。离群文件仅不渲染，其文件代表仍参与统计。
    """

    # 解析关键特征列索引。
    feature_indices = _key_feature_indices(feature_names)
    # 创建 4x2 网格容纳八个特征。
    figure, axes = plt.subplots(4, 2, figsize=(17.0, 15.0))
    # 总标题说明箱体和须的统计含义。
    figure.suptitle(
        "11 类动作文件等权关键特征分布\n"
        "每文件先取窗口特征中位数；箱体为文件 IQR，中线为文件中位数",
        fontsize=18,
        y=0.995,
    )
    # tab20 为每个类别分配固定颜色。
    palette = plt.get_cmap("tab20")
    # 扁平坐标区按 KEY_FEATURES 顺序绘制。
    for panel_index, (feature_index, feature_spec) in enumerate(
        zip(feature_indices, KEY_FEATURES)
    ):
        # 解包特征键、中文名和物理单位。
        feature_name, title, unit = feature_spec
        # distributions 按 class_names 固定顺序保存一维数组。
        distributions: List[np.ndarray] = []
        # 逐类别选出当前特征的文件代表值。
        for class_index in range(len(class_names)):
            # class_values 保留当前类全部文件代表，不按窗口数加权。
            class_values = np.asarray(
                feature_matrix[labels == class_index, feature_index],
                dtype=np.float64,
            )
            # 缺失类别不能形成完整对比图。
            if len(class_values) == 0:
                # 错误报告类别和特征。
                raise ValueError(
                    f"类别 {class_names[class_index]} 没有特征 {feature_name} 样本。"
                )
            # 当前类别分布加入箱线图输入。
            distributions.append(class_values)
        # axis 是当前特征子图。
        axis = axes.ravel()[panel_index]
        # 创建箱线图；离群点隐藏只为避免视觉压缩，不从统计数组删除。
        boxplot = axis.boxplot(
            distributions,
            patch_artist=True,
            showfliers=False,
            widths=0.66,
            medianprops={"color": "#111827", "linewidth": 1.4},
            whiskerprops={"color": "#6B7280", "linewidth": 0.9},
            capprops={"color": "#6B7280", "linewidth": 0.9},
        )
        # 逐类别给箱体填充固定颜色。
        for class_index, box in enumerate(boxplot["boxes"]):
            # 边框使用同色，填充保持半透明。
            box.set(
                facecolor=palette(class_index % palette.N),
                edgecolor=palette(class_index % palette.N),
                alpha=0.68,
                linewidth=1.0,
            )
        # 设置 1~11 的箱体位置刻度。
        axis.set_xticks(np.arange(1, len(class_names) + 1))
        # 中文类别名旋转 38 度，避免相邻标签重叠。
        axis.set_xticklabels(
            [display_name(name) for name in class_names],
            rotation=38,
            ha="right",
            fontsize=8,
        )
        # 标题同时显示中文含义和代码特征键。
        axis.set_title(f"{title}\n{feature_name}", fontsize=11)
        # Y 轴显示物理单位或无量纲。
        axis.set_ylabel(unit)
        # 仅启用 Y 轴网格，避免箱体之间出现过密竖线。
        axis.grid(True, axis="y", linestyle="--", linewidth=0.6)
    # 调整布局并为总标题留空。
    figure.tight_layout(rect=(0.0, 0.0, 1.0, 0.972))
    # 保存正式 PNG。
    _save_figure(figure, output_path)


def render_feature_heatmap_figure(
    feature_matrix: np.ndarray,
    labels: np.ndarray,
    feature_names: Sequence[str],
    class_names: Sequence[str],
    output_path: Path,
) -> np.ndarray:
    """绘制 ``[K,297]`` 文件代表的类别热力图并返回 ``[类别数,8]`` 分数。

    ``K`` 是有效验证文件数；返回值无量纲且截断到 ``[-3,3]``。
    """

    # 获取关键特征固定列索引。
    feature_indices = _key_feature_indices(feature_names)
    # class_medians 形状 [11,8]，每格是当前类文件代表中位数。
    class_medians = np.zeros(
        (len(class_names), len(feature_indices)),
        dtype=np.float64,
    )
    # 逐类别计算关键特征中位数。
    for class_index in range(len(class_names)):
        # class_rows 选择当前类别的所有文件代表。
        class_rows = feature_matrix[labels == class_index]
        # 缺失类别不能形成 11 类热力图。
        if len(class_rows) == 0:
            # 错误报告类别名。
            raise ValueError(f"热力图类别没有有效验证文件：{class_names[class_index]}")
        # 对文件维取中位数并仅保留八个关键列。
        class_medians[class_index] = np.median(
            class_rows[:, feature_indices],
            axis=0,
        )
    # scores 是按列中位数/IQR 标准化并截断到 [-3,3] 的显示值。
    scores = robust_heatmap_scores(class_medians)
    # 创建单一热力图坐标区。
    figure, axis = plt.subplots(figsize=(14.5, 9.0))
    # RdBu_r 让高于类别中心为红、低于中心为蓝、中心为白。
    image = axis.imshow(
        scores,
        cmap="RdBu_r",
        vmin=-3.0,
        vmax=3.0,
        aspect="auto",
    )
    # X 轴刻度位于八个特征列中心。
    axis.set_xticks(np.arange(len(KEY_FEATURES)))
    # 使用简短中文特征名，旋转 32 度。
    axis.set_xticklabels(
        [item[1] for item in KEY_FEATURES],
        rotation=32,
        ha="right",
        fontsize=10,
    )
    # Y 轴刻度位于 11 个类别行中心。
    axis.set_yticks(np.arange(len(class_names)))
    # 显示中文动作名。
    axis.set_yticklabels(
        [display_name(name) for name in class_names],
        fontsize=10,
    )
    # 标题明确数值不是模型标准化值，而是跨类别稳健显示分数。
    axis.set_title(
        "11 类动作文件等权关键特征中位数热力图\n"
        "文件内先取窗口中位数；每列按类别中位数中心化、IQR 缩放",
        fontsize=15,
        pad=18,
    )
    # 逐格写入一位小数，帮助黑白打印和精确比较。
    for row_index in range(scores.shape[0]):
        # 遍历当前行全部八列。
        for column_index in range(scores.shape[1]):
            # 深色格使用白字，浅色格使用深灰字。
            text_color = (
                "white"
                if abs(float(scores[row_index, column_index])) >= 1.45
                else "#1F2937"
            )
            # 当前稳健分数写在格中心。
            axis.text(
                column_index,
                row_index,
                f"{scores[row_index, column_index]:.1f}",
                ha="center",
                va="center",
                color=text_color,
                fontsize=8.5,
            )
    # 颜色条解释稳健 IQR 分数。
    colorbar = figure.colorbar(image, ax=axis, fraction=0.035, pad=0.025)
    # 色条标签注明截断范围。
    colorbar.set_label("跨类别稳健分数（截断到 ±3）")
    # 调整布局防止中文标签裁切。
    figure.tight_layout()
    # 保存正式 PNG。
    _save_figure(figure, output_path)
    # 返回显示分数供清单测试或后续审计。
    return scores


def build_manifest(
    *,
    dataset_name: str,
    dataset_sha256: str,
    dataset_file_count: int,
    class_file_counts: Mapping[str, int],
    class_names: Sequence[str],
    train_records: Sequence[ImuRecord],
    validation_records: Sequence[ImuRecord],
    test_records: Sequence[ImuRecord],
    validation_files: Sequence[str],
    corpus: ValidationCorpus,
    rest_threshold: float,
    active_point_threshold: float,
    window_len: int,
    step_len: int,
    figure_sha256: Mapping[str, str],
) -> Dict[str, object]:
    """构造纯 JSON 对象。

    输入计数和阈值分别为文件/窗口数量及训练门分数，``figure_sha256`` 为
    四张 PNG 原始字节的摘要映射。返回对象只含相对文件、无量纲指纹与数值
    参数；不含运行时间或 Path 绝对值。
    """

    # validated_figure_sha256 锁定四图映射完整性和 64 位小写十六进制格式。
    validated_figure_sha256 = validate_figure_sha256(figure_sha256)
    # figures 记录四张图的稳定文件名、内容摘要和统计口径。
    figures = {
        OUTPUT_FILES["time_domain"]: {
            "classes": list(FIELD_ACTIONS),
            "signals": ["acc_vertical", "gyro_mag"],
            "per_file_summary": "joint_shape_medoid_then_objective_peak_alignment",
            "class_summary": "pointwise_file_median_and_iqr",
        },
        OUTPUT_FILES["power_spectrum"]: {
            "classes": list(class_names),
            "signal": "gyro_mag",
            "per_file_summary": "pointwise_window_median_relative_power",
            "class_summary": "pointwise_file_median_and_iqr",
            "maximum_frequency_hz": POWER_MAX_HZ,
        },
        OUTPUT_FILES["feature_distribution"]: {
            "classes": list(class_names),
            "features": [item[0] for item in KEY_FEATURES],
            "per_file_summary": "componentwise_window_median",
            "class_summary": "boxplot_across_equal_weight_files_without_rendered_outliers",
        },
        OUTPUT_FILES["feature_heatmap"]: {
            "classes": list(class_names),
            "features": [item[0] for item in KEY_FEATURES],
            "per_file_summary": "componentwise_window_median",
            "class_summary": "file_median_then_cross_class_median_iqr_scaling",
            "display_clip": [-3.0, 3.0],
        },
    }
    # 逐个正式图节点加入其 PNG 原始字节 SHA-256，供教程资产完整性复核。
    for figure_name, digest in validated_figure_sha256.items():
        # figures 已由相同固定图名构造，当前赋值不会创建未知节点。
        figures[figure_name]["sha256"] = digest
    # valid_file_labels 按相对文件记录唯一类别，计算文件等权统计范围。
    valid_file_labels: Dict[str, str] = {}
    # 逐验证窗口收集有效文件类别。
    for window in corpus.windows:
        # 同一文件首次出现时记录类别。
        if window.source_file not in valid_file_labels:
            # 保存英文类别键。
            valid_file_labels[window.source_file] = window.label
        # 同一文件后续窗口必须保持类别不变。
        elif valid_file_labels[window.source_file] != window.label:
            # 标签冲突表示数据结构损坏。
            raise ValueError(f"同一有效文件出现多类别：{window.source_file}")
    # validation_class_file_counts 统计每类实际贡献权重的文件数。
    validation_class_file_counts = {
        class_name: sum(
            1
            for label in valid_file_labels.values()
            if label == class_name
        )
        for class_name in class_names
    }
    # manifest 只保存逻辑数据集名、相对文件和数值参数。
    manifest: Dict[str, object] = {
        "schema_version": 1,
        "generator": {
            "path": "python/visualize_action_features.py",
            "version": GENERATOR_VERSION,
        },
        "dataset": {
            "name": dataset_name,
            "sha256": dataset_sha256,
            "file_count": int(dataset_file_count),
            "class_file_counts": dict(sorted(class_file_counts.items())),
        },
        "split": {
            "seed": SEED,
            "train_ratio": TRAIN_RATIO,
            "validation_ratio": VAL_RATIO,
            "test_ratio": TEST_RATIO,
            "train_file_count": len(train_records),
            "validation_file_count": len(validation_records),
            "test_file_count": len(test_records),
            "validation_files": sorted(validation_files),
        },
        "signal_contract": {
            "channel_order": ["gx", "gy", "gz", "ax", "ay", "az"],
            "gyro_unit": "deg/s",
            "acceleration_unit": "g",
            "sample_rate_hz": SAMPLE_RATE,
            "window_seconds": WINDOW_SECONDS,
            "window_points": int(window_len),
            "step_seconds": STEP_SECONDS,
            "step_points": int(step_len),
        },
        "training_split_thresholds": {
            "rest_motion_score": float(rest_threshold),
            "active_point_score": float(active_point_threshold),
        },
        "statistics_contract": {
            "weighting_unit": "validation_file",
            "overlapping_windows_are_not_independent_samples": True,
            "time_domain": {
                "per_file_representative": "minimum_total_pairwise_squared_distance_medoid",
                "medoid_shape": "robust_scaled_acc_vertical_concatenated_with_robust_scaled_gyro_mag",
                "peak_score": "equal_weight_robust_acc_deviation_and_positive_gyro_activity",
                "peak_smoothing_kernel": [0.25, 0.5, 0.25],
                "peak_target_point": int(window_len // 2),
                "alignment_padding": "nan_without_wrap_or_interpolation",
                "class_summary": "pointwise_median_q25_q75_across_files",
            },
            "power_spectrum": {
                "per_window": "demean_hann_rfft_power_normalized_by_total_power",
                "per_file": "pointwise_window_median",
                "class_summary": "pointwise_median_q25_q75_across_files",
            },
            "features": {
                "per_window_dimension": len(build_feature_names()),
                "per_file": "componentwise_window_median",
                "class_distribution_unit": "file",
                "heatmap": "class_file_median_then_cross_class_median_iqr_scaling",
            },
        },
        "validation": {
            "total_window_count": len(corpus.windows),
            "valid_file_count": len(valid_file_labels),
            "class_file_counts": validation_class_file_counts,
            "class_window_counts": dict(corpus.class_window_counts),
            "file_window_counts": dict(corpus.file_window_counts),
            "filter_statistics": dict(corpus.skipped),
        },
        "figures": figures,
        "limitations": [
            "图表只使用固定文件级验证划分，不使用测试集，也不用于选择模型或阈值。",
            "数据集未提供完整受试者身份和佩戴侧元数据，图表不能证明跨人或左右手泛化。",
            "时域不人工挑选窗口；文件内算法 medoid 和客观运动峰对齐只用于可视化，不改变模型输入。",
            "每个验证文件权重相同；文件内重叠窗口仅用于形成该文件代表，不能解释为独立样本。",
        ],
    }
    # 返回可直接 JSON 序列化的纯相对清单。
    return manifest


def write_manifest(manifest: Mapping[str, object], output_path: Path) -> None:
    """把 JSON 映射稳定写入正式路径；UTF-8、排序键、LF，返回 ``None``。"""

    # ensure_ascii=False 保留中文，sort_keys=True 固定键顺序。
    serialized = json.dumps(
        manifest,
        ensure_ascii=False,
        indent=2,
        sort_keys=True,
    )
    # 统一使用 LF 和末尾换行，减少跨平台 Git 差异。
    output_path.write_text(serialized + "\n", encoding="utf-8", newline="\n")


def remove_legacy_english_figures(output_dir: Path) -> List[str]:
    """删除输出目录内四个已知旧英文 PNG，返回实际删除的文件名。

    只按 ``LEGACY_OUTPUT_FILES`` 精确名称处理，不使用通配符、不递归、不触碰
    其它资产。调用方应在全部新图和清单成功后执行。
    """

    # destination 绑定用户明确传入的正式图表目录。
    destination = Path(output_dir)
    # removed 保存实际存在并删除的旧文件名。
    removed: List[str] = []
    # 逐个精确旧名检查。
    for legacy_name in LEGACY_OUTPUT_FILES:
        # legacy_path 必定是 destination 的直属文件。
        legacy_path = destination / legacy_name
        # 只删除普通文件；目录或不存在目标均跳过。
        if not legacy_path.is_file():
            # 继续下一个已知旧名。
            continue
        # unlink 删除单个已知旧 PNG，不影响目录其余内容。
        legacy_path.unlink()
        # 记录已删除文件名供日志和测试审计。
        removed.append(legacy_name)
    # 返回确定顺序的删除清单。
    return removed


def generate_visualizations(dataset_dir: Path, output_dir: Path) -> Dict[str, object]:
    """读取类别 TXT 并生成四张 300 DPI PNG 和 JSON，返回清单映射。

    数据输入转换后为 ``[N,6]``，顺序 ``gx、gy、gz、ax、ay、az``，单位
    ``deg/s、g``。训练划分只估阈值，图表只用验证划分，测试划分不读取。
    """

    # dataset_root 转为 Path 并保留用户输入位置用于读取。
    dataset_root = Path(dataset_dir)
    # 输出目录是正式仓库资源位置；不存在时递归创建。
    destination = Path(output_dir)
    # parents=True 允许首次创建 docs/assets/algorithm。
    destination.mkdir(parents=True, exist_ok=True)
    # 设置统一中文和文档绘图风格。
    configure_plot_style()
    # 扫描类别目录和 TXT 记录，类别按目录名排序。
    records, class_names, _ = scan_dataset(dataset_root)
    # 文件级固定种子划分，窗口不会跨训练、验证和测试集合。
    train_records, validation_records, test_records = split_records_by_file(
        records,
        seed=SEED,
    )
    # 2.5 秒窗口和 0.5 秒步长由训练函数换算为整数点。
    window_len, step_len = window_lengths(WINDOW_SECONDS)
    # 静止窗口阈值只从训练划分静坐文件估计。
    rest_threshold = estimate_rest_threshold(
        train_records,
        window_len,
        step_len,
    )
    # 逐点活动阈值同样只从训练划分静坐文件估计。
    active_point_threshold = estimate_active_point_threshold(
        train_records,
        window_len,
        step_len,
    )
    # 使用训练端相同裁剪和筛选规则收集验证窗口。
    corpus = collect_validation_windows(
        validation_records,
        dataset_root,
        window_len,
        step_len,
        rest_threshold,
        active_point_threshold,
    )
    # 确认 11 个扫描类别均至少存在一个有效验证窗口。
    missing_classes = [
        class_name
        for class_name in class_names
        if corpus.class_window_counts.get(class_name, 0) == 0
    ]
    # 缺失类别时停止生成，避免文档对比不完整。
    if missing_classes:
        # 一次报告全部缺失类别。
        raise ValueError(f"验证集缺少有效窗口类别：{missing_classes}")
    # 建立类别到验证窗口的稳定映射。
    windows_by_class = _windows_by_class(corpus.windows, class_names)
    # 绘制六种现场动作时域聚合曲线。
    render_time_domain_figure(
        windows_by_class,
        destination / OUTPUT_FILES["time_domain"],
    )
    # 绘制 11 类相对功率谱。
    render_power_spectrum_figure(
        windows_by_class,
        class_names,
        destination / OUTPUT_FILES["power_spectrum"],
    )
    # 一次提取验证窗口 297 维特征并折叠到文件级，供两图共享。
    feature_matrix, labels, feature_names, feature_files = (
        validation_feature_table(corpus.windows)
    )
    # 文件级特征行数必须等于有效验证文件数。
    if len(feature_files) != sum(
        1 for count in corpus.file_window_counts.values() if count > 0
    ):
        # 不一致表示文件折叠遗漏或重复。
        raise ValueError("文件级特征行数与有效验证文件数不一致。")
    # 绘制八个可解释特征的 11 类箱线分布。
    render_feature_distribution_figure(
        feature_matrix,
        labels,
        feature_names,
        class_names,
        destination / OUTPUT_FILES["feature_distribution"],
    )
    # 绘制 11 类 × 八特征稳健热力图。
    render_feature_heatmap_figure(
        feature_matrix,
        labels,
        feature_names,
        class_names,
        destination / OUTPUT_FILES["feature_heatmap"],
    )
    # figure_sha256 在四张 PNG 全部写盘并关闭后读取原始字节，避免记录绘图参数摘要。
    figure_sha256 = {
        # 当前键是正式文件名，值是对应 PNG 文件内容的 64 位小写 SHA-256。
        OUTPUT_FILES[output_key]: _sha256_file(
            destination / OUTPUT_FILES[output_key]
        )
        # 四个逻辑键固定顺序遍历；JSON 清单自身不属于图表资产。
        for output_key in (
            "time_domain",
            "power_spectrum",
            "feature_distribution",
            "feature_heatmap",
        )
    }
    # 数据集指纹只依赖相对路径、字节数和文件内容。
    dataset_sha256, class_file_counts, dataset_file_count = dataset_fingerprint(
        dataset_root
    )
    # 验证文件清单全部转换为数据集内相对 POSIX 路径。
    validation_files = [
        _record_relative_path(record, dataset_root)
        for record in validation_records
    ]
    # 构造无时间戳、无绝对路径的稳定清单。
    manifest = build_manifest(
        dataset_name=dataset_root.name,
        dataset_sha256=dataset_sha256,
        dataset_file_count=dataset_file_count,
        class_file_counts=class_file_counts,
        class_names=class_names,
        train_records=train_records,
        validation_records=validation_records,
        test_records=test_records,
        validation_files=validation_files,
        corpus=corpus,
        rest_threshold=rest_threshold,
        active_point_threshold=active_point_threshold,
        window_len=window_len,
        step_len=step_len,
        figure_sha256=figure_sha256,
    )
    # 写入正式 JSON 清单。
    write_manifest(
        manifest,
        destination / OUTPUT_FILES["manifest"],
    )
    # 新中文图和清单全部成功后，精确删除四个旧英文图。
    removed_legacy_files = remove_legacy_english_figures(destination)
    # 输出摘要只包含相对事实和数值，不打印本机数据集绝对路径。
    print(
        "visualization complete "
        f"classes={len(class_names)} files={dataset_file_count} "
        f"validation_windows={len(corpus.windows)} "
        f"validation_files={len(feature_files)} "
        f"removed_legacy={len(removed_legacy_files)} "
        f"dataset_sha256={dataset_sha256}",
        flush=True,
    )
    # 返回清单供调用方或集成测试继续验证。
    return manifest


def main(argv: Sequence[str] | None = None) -> int:
    """接收 CLI 字符串序列；成功生成全部正式文件后返回整数零。"""

    # 解析两个必需路径。
    args = parse_args(argv)
    # 生成正式图和清单；异常直接向命令行传播并返回非零。
    generate_visualizations(args.dataset_dir, args.output_dir)
    # 零表示全部四张图和清单均已写完。
    return 0


# 直接执行脚本时进入命令行入口；作为模块导入时不产生文件。
if __name__ == "__main__":
    # SystemExit 把 main 返回值传给 PowerShell 或 CI。
    raise SystemExit(main())
