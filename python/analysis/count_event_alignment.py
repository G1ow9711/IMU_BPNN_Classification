"""把中文 IMU CSV 的六轴、分类窗和权威计数事件对齐到同一设备时间轴。"""

# 延迟解析类型注解，保持 Python 3.10+ 兼容且不改变运行结果。
from __future__ import annotations

# argparse 固定输入日志、会话、动作标签和输出目录，使分析可以重复执行。
import argparse
# csv 按上位机中文表头读取用户显式导出的当前 44 列日志。
import csv
# json 保存机器可读的事件统计，后续红测可复用同一证据。
import json
# math 计算三轴模长和有限值统计。
import math
# dataclass 保存绘图与统计所需的不可变样本字段。
from dataclasses import dataclass
# Path 统一处理 Windows 绝对路径和输出目录。
from pathlib import Path

# Matplotlib 只负责生成可放大的诊断图，不参与计数结论。
import matplotlib.pyplot as plt
# NumPy 计算事件前窗口的均方根和百分位数。
import numpy as np

# 当前上位机导出合同固定为 44 列；新增列时应同步更新工具和教程。
EXPECTED_COLUMN_COUNT = 44
# 分析器只依赖下列字段，显式校验可把非当前格式转成可读错误。
REQUIRED_COLUMNS = (
    "设备单调时间（毫秒）",
    "角速度横轴（度每秒）",
    "角速度纵轴（度每秒）",
    "角速度垂直轴（度每秒）",
    "加速度横轴（重力倍数）",
    "加速度纵轴（重力倍数）",
    "加速度垂直轴（重力倍数）",
    "会话序号",
    "佩戴手侧",
    "分类窗口序号",
    "分类窗口结束时间（毫秒）",
    "融合模型类别",
    "融合模型置信度",
    "分类质量标志（十六进制）",
    "分类窗口是否在本行结束",
    "是否计数标记点",
    "计数后累计值",
    "计数事件设备时间（毫秒）",
)


@dataclass(frozen=True)
class ClassificationWindow:
    """保存一个设备端分类窗口结束点及其融合 Top-1。"""

    # 设备窗口序号，来自类型九推理诊断消息。
    sequence: int
    # 窗口最后一个 IMU 点的设备单调毫秒。
    end_ms: int
    # 融合模型中文类别。
    action: str
    # 融合 softmax 置信度，范围 0～1。
    confidence: float
    # 分类输入质量位原值。
    quality_flags: int


def parse_args() -> argparse.Namespace:
    """解析单份真板日志的可复现分析参数。"""

    # 创建命令行解析器。
    parser = argparse.ArgumentParser(description="对齐六轴、模型分类窗和权威计数事件")
    # 用户导出的中文 CSV 只读输入。
    parser.add_argument("--input", type=Path, required=True)
    # 明确指定非零会话序号，避免把两轮动作混在一起。
    parser.add_argument("--session", type=int, required=True)
    # 动作标签仅用于报告标题和输出文件名，可填写当前产品支持的任意动作。
    parser.add_argument("--action", required=True)
    # 用户人工真值只用于图标题和误差说明，不参与算法调参。
    parser.add_argument("--truth", type=int, required=True)
    # 输出目录保存 PNG 与 JSON。
    parser.add_argument("--output-dir", type=Path, required=True)
    # 返回完整参数。
    return parser.parse_args()


def parse_int(value: str, default: int = 0) -> int:
    """把空白或合法十进制字段转换为整数。"""

    # 去掉 CSV 字段两端空白。
    normalized = value.strip()
    # 空字段返回调用者指定默认值。
    if not normalized:
        # 当前字段没有消息事实。
        return default
    # 返回十进制整数。
    return int(normalized)


def parse_hex(value: str) -> int:
    """把空白或 0x 前缀质量位转换为整数。"""

    # 去掉 CSV 字段两端空白。
    normalized = value.strip()
    # 空字段表示没有分类窗或质量位为零。
    if not normalized:
        # 返回零位图。
        return 0
    # base=0 同时接受 0x 十六进制和十进制。
    return int(normalized, 0)


def safe_action_slug(action: str) -> str:
    """把动作标签转换为不会越过输出目录的文件名片段。"""

    # 去除首尾空白，避免生成只有空格的文件名。
    normalized = action.strip()
    # 空标签无法说明报告对象，立即拒绝。
    if not normalized:
        # 抛出明确错误，调用者应填写动作中文名或稳定英文标识。
        raise ValueError("动作标签不能为空")
    # 只保留字母、数字、中文、连字符和下划线；路径分隔符等字符统一替换。
    sanitized = "".join(
        character if character.isalnum() or character in ("-", "_") else "_"
        for character in normalized
    )
    # 连续替换符压缩为一个下划线，使文件名短且稳定。
    while "__" in sanitized:
        # 每轮把两个连续下划线压成一个，直到不存在重复。
        sanitized = sanitized.replace("__", "_")
    # 去除首尾分隔符，避免 Windows 产生难辨认文件名。
    sanitized = sanitized.strip("_-")
    # 若标签全由特殊字符组成，则拒绝生成含义不明的输出文件。
    if not sanitized:
        # 抛出明确错误，防止输出文件名退化为固定公共名称。
        raise ValueError(f"动作标签无法转换为安全文件名：{action!r}")
    # 返回只用于输出文件名的稳定片段，报告正文仍保留原始标签。
    return sanitized


def load_rows(path: Path, session_sequence: int) -> list[dict[str, str]]:
    """读取当前 44 列日志中的指定会话，保持设备样本顺序。"""

    # 使用 utf-8-sig 同时兼容带 BOM 和无 BOM 的上位机导出。
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        # DictReader 用中文表头映射每列，不依赖固定列位置。
        reader = csv.DictReader(handle)
        # fieldnames 保存文件首行的完整列名；空文件返回空元组。
        fieldnames = tuple(reader.fieldnames or ())
        # 列数不是 44 时说明文件不是当前产品导出合同。
        if len(fieldnames) != EXPECTED_COLUMN_COUNT:
            # 明确报告实际列数，避免非当前日志被误当成当前格式。
            raise ValueError(
                f"{path} 应为 {EXPECTED_COLUMN_COUNT} 列当前日志，实际 {len(fieldnames)} 列"
            )
        # missing_columns 保存分析需要但输入缺少的中文字段。
        missing_columns = [
            column
            for column in REQUIRED_COLUMNS
            if column not in fieldnames
        ]
        # 字段缺失时无法可靠对齐分类窗和计数事件。
        if missing_columns:
            # 一次报告全部缺失字段，方便修复导出器或输入文件。
            raise ValueError(f"{path} 缺少必要列：{', '.join(missing_columns)}")
        # 只保留目标会话，避免另一轮开始/停止状态污染时间轴。
        rows = [
            row
            for row in reader
            if parse_int(row.get("会话序号", "0")) == session_sequence
        ]
    # 空会话通常表示参数填错，立即失败而不是生成空图。
    if not rows:
        # 报错包含会话和文件，便于恢复。
        raise ValueError(f"会话 {session_sequence} 在 {path} 中没有样本")
    # 返回设备原始顺序；导出器已按样本到达顺序写出。
    return rows


def collect_windows(rows: list[dict[str, str]]) -> list[ClassificationWindow]:
    """提取每个唯一分类窗口结束点。"""

    # 用窗口序号去重；同一窗口诊断会附着到后续多条原始样本。
    windows_by_sequence: dict[int, ClassificationWindow] = {}
    # 遍历指定会话全部样本。
    for row in rows:
        # 只有明确标记“是”的行代表本行收到新分类窗口。
        if row.get("分类窗口是否在本行结束", "").strip() != "是":
            # 跳过复用上一窗口快照的普通样本行。
            continue
        # 读取窗口序号。
        sequence = parse_int(row.get("分类窗口序号", ""), default=-1)
        # 无合法序号时不能建立窗口事实。
        if sequence < 0:
            # 跳过损坏行。
            continue
        # 按值保存当前窗口；重复序号只保留最后一条完全相同的诊断快照。
        windows_by_sequence[sequence] = ClassificationWindow(
            # 保存窗口序号。
            sequence=sequence,
            # 优先使用分类消息携带的设备窗口末点时间。
            end_ms=parse_int(row.get("分类窗口结束时间（毫秒）", "")),
            # 保存融合模型类别。
            action=row.get("融合模型类别", "未知").strip() or "未知",
            # 保存融合模型置信度。
            confidence=float(row.get("融合模型置信度", "0") or 0.0),
            # 保存分类质量位。
            quality_flags=parse_hex(row.get("分类质量标志（十六进制）", "0")),
        )
    # 按设备窗口末点排序，保证事件最近分类查询只需线性推进。
    return sorted(windows_by_sequence.values(), key=lambda item: (item.end_ms, item.sequence))


def latest_window_before(windows: list[ClassificationWindow], device_ms: int) -> ClassificationWindow | None:
    """返回不晚于目标时刻的最近分类窗口。"""

    # 初始没有可用窗口。
    latest: ClassificationWindow | None = None
    # 窗口已经按结束时刻升序排列。
    for window in windows:
        # 晚于事件的窗口不能解释该事件。
        if window.end_ms > device_ms:
            # 后续窗口更晚，提前结束。
            break
        # 当前窗口成为最近事实。
        latest = window
    # 返回最近窗口或 None。
    return latest


def build_event_rows(rows: list[dict[str, str]], windows: list[ClassificationWindow]) -> list[dict[str, object]]:
    """为每个权威计数标记计算前 1.6 秒运动统计和最近模型类别。"""

    # 把 CSV 物理量转换为数值数组，列顺序固定 gx、gy、gz、ax、ay、az。
    samples = np.asarray(
        [
            [
                float(row["设备单调时间（毫秒）"]),
                float(row["角速度横轴（度每秒）"]),
                float(row["角速度纵轴（度每秒）"]),
                float(row["角速度垂直轴（度每秒）"]),
                float(row["加速度横轴（重力倍数）"]),
                float(row["加速度纵轴（重力倍数）"]),
                float(row["加速度垂直轴（重力倍数）"]),
            ]
            for row in rows
        ],
        dtype=np.float64,
    )
    # 保存逐事件统计。
    result: list[dict[str, object]] = []
    # 遍历显式 EventV1 标记行。
    for row in rows:
        # 普通样本没有计数标记。
        if row.get("是否计数标记点", "").strip() != "是":
            # 跳过普通行。
            continue
        # 事件时刻来自 EventV1 本身，而不是 PC 接收时刻。
        event_ms = parse_int(row.get("计数事件设备时间（毫秒）", ""))
        # 统计窗口覆盖事件前 1.6 秒并包含事件点。
        mask = (samples[:, 0] >= event_ms - 1600.0) & (samples[:, 0] <= event_ms)
        # 取当前事件前窗口样本。
        window_samples = samples[mask]
        # 计算三轴角速度模长，单位度每秒。
        gyro_magnitude = np.linalg.norm(window_samples[:, 1:4], axis=1)
        # 计算三轴加速度模长偏离 1 g 的绝对值。
        acceleration_deviation = np.abs(np.linalg.norm(window_samples[:, 4:7], axis=1) - 1.0)
        # 查找事件发生前最近有效分类窗口。
        classification = latest_window_before(windows, event_ms)
        # 保存可直接进入 Markdown 或红测的数据。
        result.append(
            {
                # 权威累计值用于和用户口述分界对齐。
                "累计": parse_int(row.get("计数后累计值", "")),
                # 保存事件设备毫秒。
                "事件毫秒": event_ms,
                # 最近分类为空时写未知。
                "最近融合类别": classification.action if classification is not None else "未知",
                # 最近置信度为空时写零。
                "最近融合置信度": round(classification.confidence, 6) if classification is not None else 0.0,
                # 角速度均方根反映 1.6 秒整体活动强度。
                "角速度模长均方根": round(float(math.sqrt(np.mean(np.square(gyro_magnitude)))), 3),
                # 95 百分位抑制单点毛刺。
                "角速度模长95百分位": round(float(np.percentile(gyro_magnitude, 95)), 3),
                # 加速度偏离 1 g 的 95 百分位用于区分动态与静止腕部操作。
                "加速度模长偏差95百分位": round(float(np.percentile(acceleration_deviation, 95)), 4),
            }
        )
    # 返回按 CSV 事件顺序排列的统计。
    return result


def draw_alignment(
    rows: list[dict[str, str]],
    windows: list[ClassificationWindow],
    event_rows: list[dict[str, object]],
    output_path: Path,
    title: str,
) -> None:
    """生成六轴、融合类别和权威事件同轴对齐图。"""

    # 优先使用 Windows 自带微软雅黑显示中文，缺字体时由 Matplotlib 回退。
    plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
    # 正确显示坐标轴负号。
    plt.rcParams["axes.unicode_minus"] = False
    # 设备首点作为相对时间零点，横轴单位秒。
    start_ms = float(rows[0]["设备单调时间（毫秒）"])
    # 生成相对秒数组。
    seconds = np.asarray(
        [(float(row["设备单调时间（毫秒）"]) - start_ms) / 1000.0 for row in rows],
        dtype=np.float64,
    )
    # 提取三轴加速度。
    acceleration = np.asarray(
        [
            [
                float(row["加速度横轴（重力倍数）"]),
                float(row["加速度纵轴（重力倍数）"]),
                float(row["加速度垂直轴（重力倍数）"]),
            ]
            for row in rows
        ],
        dtype=np.float64,
    )
    # 提取三轴角速度。
    gyroscope = np.asarray(
        [
            [
                float(row["角速度横轴（度每秒）"]),
                float(row["角速度纵轴（度每秒）"]),
                float(row["角速度垂直轴（度每秒）"]),
            ]
            for row in rows
        ],
        dtype=np.float64,
    )
    # 创建三行共享横轴的高分辨率图。
    figure, axes = plt.subplots(3, 1, figsize=(18, 11), sharex=True, constrained_layout=True)
    # 三个通道固定颜色，和上位机曲线保持易辨认对应。
    colors = ("#2563EB", "#10B981", "#F59E0B")
    # 绘制三轴加速度。
    for axis, label, color in zip(range(3), ("横轴", "纵轴", "垂直轴"), colors, strict=True):
        # 每条曲线使用同一线宽和透明度。
        axes[0].plot(seconds, acceleration[:, axis], label=label, color=color, linewidth=1.15)
    # 标注加速度单位。
    axes[0].set_ylabel("加速度 / g")
    # 显示通道图例。
    axes[0].legend(loc="upper right", ncols=3)
    # 绘制三轴角速度。
    for axis, label, color in zip(range(3), ("横轴", "纵轴", "垂直轴"), colors, strict=True):
        # 每条曲线使用同一线宽和透明度。
        axes[1].plot(seconds, gyroscope[:, axis], label=label, color=color, linewidth=1.0)
    # 标注角速度单位。
    axes[1].set_ylabel("角速度 / (度/秒)")
    # 显示通道图例。
    axes[1].legend(loc="upper right", ncols=3)
    # 建立图中出现类别到整数纵坐标的稳定映射。
    action_names = list(dict.fromkeys(window.action for window in windows))
    # 把每个分类窗口绘制为散点，不对分类结果做插值。
    for window in windows:
        # 计算窗口相对秒。
        window_second = (window.end_ms - start_ms) / 1000.0
        # 类别纵坐标来自当前映射位置。
        action_index = action_names.index(window.action)
        # 点大小按置信度略微变化，但不改变真实类别位置。
        axes[2].scatter(window_second, action_index, s=35.0 + 45.0 * window.confidence, color="#475569", zorder=3)
        # 在点上方写置信度，便于定位冻结时刻。
        axes[2].text(window_second, action_index + 0.12, f"{window.confidence:.2f}", fontsize=7, ha="center")
    # 配置类别刻度。
    axes[2].set_yticks(range(len(action_names)), labels=action_names)
    # 标注分类含义。
    axes[2].set_ylabel("融合类别")
    # 标注共享横轴。
    axes[2].set_xlabel("本轮开始后的设备时间 / 秒")
    # 每个权威事件在三幅图上画同一红色竖线和累计编号。
    for event in event_rows:
        # 计算事件相对秒。
        event_second = (float(event["事件毫秒"]) - start_ms) / 1000.0
        # 遍历三行坐标轴绘制垂直标记。
        for plot_axis in axes:
            # 红虚线只表示固件真正发布的 MetricEvent。
            plot_axis.axvline(event_second, color="#DC2626", linewidth=0.9, alpha=0.65, linestyle="--")
        # 在加速度图顶部写事件后累计值。
        axes[0].text(event_second, axes[0].get_ylim()[1], str(event["累计"]), color="#B91C1C", fontsize=8, ha="center", va="top")
    # 三行均显示浅网格，便于人工周期核数。
    for plot_axis in axes:
        # 网格不覆盖曲线。
        plot_axis.grid(True, color="#CBD5E1", alpha=0.35, linewidth=0.6)
    # 写总标题。
    figure.suptitle(title, fontsize=15, fontweight="semibold")
    # 确保输出目录存在。
    output_path.parent.mkdir(parents=True, exist_ok=True)
    # 以 180 DPI 保存可放大 PNG。
    figure.savefig(output_path, dpi=180)
    # 关闭图对象释放内存和文件句柄。
    plt.close(figure)


def main() -> None:
    """执行单日志事件对齐并输出 JSON/PNG。"""

    # 解析命令行参数。
    args = parse_args()
    # 创建输出目录。
    args.output_dir.mkdir(parents=True, exist_ok=True)
    # 读取原始字典行供分类窗和事件统计使用。
    rows = load_rows(args.input, args.session)
    # 提取唯一分类窗口。
    windows = collect_windows(rows)
    # 提取权威计数事件及最近分类。
    event_rows = build_event_rows(rows, windows)
    # 把动作标签转换为安全文件名；原始标签仍写入报告和图标题。
    action_slug = safe_action_slug(args.action)
    # 组装机器可读结论。
    summary = {
        # 保存输入绝对路径。
        "输入": str(args.input.resolve()),
        # 保存目标会话。
        "会话": args.session,
        # 保存人工真值。
        "人工真值": args.truth,
        # 保存调用者给出的动作标签，便于多动作批量报告汇总。
        "动作": args.action,
        # 保存设备最终累计。
        "设备最终累计": max((int(event["累计"]) for event in event_rows), default=0),
        # 保存唯一模型窗。
        "分类窗口": [window.__dict__ for window in windows],
        # 保存逐事件统计。
        "计数事件": event_rows,
    }
    # JSON 使用中文键并保留缩进，便于人工审计。
    json_path = args.output_dir / f"会话{args.session}_{action_slug}_对齐.json"
    # 写入 UTF-8 JSON 并保留末尾换行。
    json_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    # 生成同名对齐图。
    plot_path = args.output_dir / f"会话{args.session}_{action_slug}_对齐.png"
    # 标题同时显示人工真值与设备累计。
    draw_alignment(
        rows,
        windows,
        event_rows,
        plot_path,
        f"会话 {args.session}（{args.action}）：人工 {args.truth}，"
        f"设备 {summary['设备最终累计']}；红线为权威计数点",
    )
    # 输出简洁成功标志和关键路径。
    print(f"COUNT_ALIGNMENT_OK session={args.session} events={len(event_rows)} json={json_path} plot={plot_path}")


# 直接运行时执行；导入本模块不会读取用户日志。
if __name__ == "__main__":
    # 启动主流程。
    main()
