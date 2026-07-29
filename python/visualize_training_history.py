"""把 train_export.py 的逐 epoch 文本日志转换为教程用训练曲线和来源清单。"""

# argparse 负责解析日志、图片和清单三个命令行路径。
import argparse
# hashlib 用于记录输入日志哈希，保证公开图片可以追溯到原始数值。
import hashlib
# json 用于输出机器可读的图表来源清单。
import json
# re 用于从稳定的 key=value 日志格式提取训练指标。
import re
# dataclass 把一次 epoch 的指标组织为带类型的不可混淆记录。
from dataclasses import asdict, dataclass
# Path 统一处理 Windows 与其它平台的文件路径。
from pathlib import Path

# matplotlib 使用非交互后端，允许在 CI 或没有桌面的环境生成 PNG。
import matplotlib

# Agg 只写图片文件，不创建窗口，也不依赖本机图形会话。
matplotlib.use("Agg")
# pyplot 负责创建四个训练指标子图。
import matplotlib.pyplot as plt


# 数值正则支持整数、小数和科学计数法，覆盖训练日志当前全部指标格式。
NUMBER_PATTERN = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"
# epoch 行必须同时包含 epoch、loss、验证指标和 best_epoch，避免误读汇总行。
EPOCH_PATTERN = re.compile(r"\bepoch=(?P<epoch>\d+)\b")
# key=value 正则把每行指标转换为字典，字段新增时不会破坏已有解析。
KEY_VALUE_PATTERN = re.compile(rf"(?P<key>[A-Za-z_][A-Za-z0-9_]*)=(?P<value>{NUMBER_PATTERN})")


@dataclass(frozen=True)
class EpochRecord:
    """保存一个 epoch 的训练目标、验证指标和早停状态。"""

    # epoch 是从 1 开始的训练轮次。
    epoch: int
    # loss 是训练脚本记录的加权总目标，无物理单位。
    loss: float
    # ce 是交叉熵分量，无物理单位。
    ce: float
    # val_acc 是验证集窗口准确率，范围 [0,1]。
    val_acc: float
    # val_f1 是验证集宏平均 F1，范围 [0,1]。
    val_f1: float
    # val_weak_f1 是预定义弱类集合的平均 F1，范围 [0,1]。
    val_weak_f1: float
    # val_worst_f1 是当前最弱类别 F1，范围 [0,1]。
    val_worst_f1: float
    # val_weak_recall 是预定义弱类集合的平均召回率，范围 [0,1]。
    val_weak_recall: float
    # val_min_recall 是当前最弱类别召回率，范围 [0,1]。
    val_min_recall: float
    # best_epoch 是截至当前轮由验证规则选出的最佳轮次。
    best_epoch: int
    # patience_left 是早停还允许连续不改进的轮数。
    patience_left: int


def sha256_file(path: Path) -> str:
    """按二进制内容计算文件 SHA-256，返回 64 位小写十六进制字符串。"""

    # 哈希对象以空状态开始，随后按块吸收日志字节。
    digest = hashlib.sha256()
    # 二进制读取避免换行或编码转换改变被审计内容。
    with path.open("rb") as stream:
        # 每轮读取 1 MiB；空字节串表示文件结束。
        while chunk := stream.read(1024 * 1024):
            # 把当前块加入累计哈希。
            digest.update(chunk)
    # 返回最终 SHA-256 文本，供文档资产清单引用。
    return digest.hexdigest()


def parse_numeric_fields(line: str) -> dict[str, float]:
    """提取一行中的数值 key=value 字段，返回无单位浮点数字典。"""

    # 遍历本行所有稳定字段；同名字段以后出现的值覆盖前值。
    fields = {
        match.group("key"): float(match.group("value"))
        for match in KEY_VALUE_PATTERN.finditer(line)
    }
    # 返回字段字典；调用者负责检查必需字段是否存在。
    return fields


def read_training_text(path: Path) -> str:
    """按 BOM 严格识别 UTF-8 或 UTF-16 训练日志，不替换损坏字符。"""

    # 训练控制台在不同 PowerShell 版本下可能保存为 UTF-8 或 UTF-16 LE。
    raw = path.read_bytes()
    # FF FE 是 UTF-16 LE 的字节序标记；utf-16 解码器会自动移除标记。
    if raw.startswith(b"\xff\xfe"):
        # 严格解码保证半个 UTF-16 字符或损坏字节会立即失败。
        return raw.decode("utf-16", errors="strict")
    # FE FF 是 UTF-16 BE 的字节序标记，也交给 utf-16 解码器处理。
    if raw.startswith(b"\xfe\xff"):
        # 当前工程通常不产生 BE 日志，但显式支持能避免跨平台误读。
        return raw.decode("utf-16", errors="strict")
    # UTF-8 BOM 由 utf-8-sig 自动移除，正文仍使用严格错误策略。
    if raw.startswith(b"\xef\xbb\xbf"):
        # 返回去除 BOM 后的 Unicode 文本。
        return raw.decode("utf-8-sig", errors="strict")
    # 没有 BOM 时按训练脚本默认 UTF-8 解码。
    return raw.decode("utf-8", errors="strict")


def parse_training_log(path: Path) -> tuple[list[EpochRecord], dict[str, str | int | float | bool]]:
    """读取逐 epoch 日志，返回训练序列和可公开的运行摘要。"""

    # PowerShell 重定向可能改变日志编码；按 BOM 严格识别后再切分行。
    lines = read_training_text(path).splitlines()
    # records 按日志顺序保存 epoch，后续会验证轮次严格递增。
    records: list[EpochRecord] = []
    # summary 保存特征维数、类别数、文件数、窗口和最终结果等运行信息。
    summary: dict[str, str | int | float | bool] = {}
    # 遍历每一行，同时识别配置、epoch、最终指标和导出状态。
    for line in lines:
        # 解析该行全部数值字段，避免为每个字段重复执行正则。
        fields = parse_numeric_fields(line)
        # 配置行包含 class_count、file_count 和 feature_dim。
        if {"class_count", "file_count", "feature_dim"}.issubset(fields):
            # 类别数转为整数，当前合同应为 11。
            summary["class_count"] = int(fields["class_count"])
            # 文件数转为整数，表示本次扫描到的动作采集文件数。
            summary["file_count"] = int(fields["file_count"])
            # 特征维数转为整数，用于在图中标注候选模型版本。
            summary["feature_dim"] = int(fields["feature_dim"])
        # window 行给出窗口秒数；只记录第一次出现的训练窗口。
        if "window" in fields and "window_seconds" not in summary:
            # 窗口长度保留浮点秒值。
            summary["window_seconds"] = float(fields["window"])
        # epoch 正则未命中时，该行不是逐轮训练记录。
        epoch_match = EPOCH_PATTERN.search(line)
        if epoch_match is not None:
            # 必需字段缺失会抛 KeyError，阻止不完整日志生成误导曲线。
            record = EpochRecord(
                # epoch 由正则按整数读取。
                epoch=int(epoch_match.group("epoch")),
                # 总损失取当前轮记录值。
                loss=float(fields["loss"]),
                # 交叉熵取当前轮记录值。
                ce=float(fields["ce"]),
                # 验证准确率取当前轮记录值。
                val_acc=float(fields["val_acc"]),
                # 验证宏 F1 取当前轮记录值。
                val_f1=float(fields["val_f1"]),
                # 弱类平均 F1 取当前轮记录值。
                val_weak_f1=float(fields["val_weak_f1"]),
                # 最弱类别 F1 取当前轮记录值。
                val_worst_f1=float(fields["val_worst_f1"]),
                # 弱类平均召回率取当前轮记录值。
                val_weak_recall=float(fields["val_weak_recall"]),
                # 最弱类别召回率取当前轮记录值。
                val_min_recall=float(fields["val_min_recall"]),
                # 截至当前轮的最佳 epoch 转成整数。
                best_epoch=int(fields["best_epoch"]),
                # 剩余耐心轮数转成整数。
                patience_left=int(fields["patience_left"]),
            )
            # 把完整记录追加到时间序列。
            records.append(record)
        # 最终汇总行同时包含 best_window、test_acc 和 test_f1。
        if {"best_window", "test_acc", "test_f1"}.issubset(fields):
            # 最佳窗口秒数来自训练结束后的验证选择。
            summary["best_window_seconds"] = float(fields["best_window"])
            # 最终恢复检查点的验证准确率。
            summary["final_val_acc"] = float(fields["val_acc"])
            # 最终恢复检查点的验证宏 F1。
            summary["final_val_f1"] = float(fields["val_f1"])
            # 测试准确率只作为该候选工件的历史结果。
            summary["test_acc"] = float(fields["test_acc"])
            # 测试宏 F1 只作为该候选工件的历史结果。
            summary["test_f1"] = float(fields["test_f1"])
        # 导出状态行明确记录是否达到训练门槛。
        if "target_reached=" in line:
            # 字符串比较只接受显式 true，其他值视为 false。
            summary["target_reached"] = "target_reached=true" in line.lower()
            # header_export_skipped=true 表示该候选没有进入端侧头文件。
            summary["header_export_skipped"] = "header_export_skipped=true" in line.lower()
    # 空日志不能生成训练曲线。
    if not records:
        # 明确指出失败文件，便于教程读者修正命令。
        raise ValueError(f"日志中没有完整 epoch 记录：{path}")
    # 取全部轮次，检查日志没有重复或逆序。
    epochs = [record.epoch for record in records]
    # 严格递增保证横轴与真实训练顺序一致。
    if epochs != sorted(set(epochs)):
        # 重复或逆序日志可能来自多次训练拼接，必须先拆分。
        raise ValueError(f"epoch 不是严格递增序列：{path}")
    # 最后一轮已经携带训练过程中最终选择的 best_epoch。
    summary["best_epoch"] = records[-1].best_epoch
    # 记录总轮数，便于教程解释早停发生位置。
    summary["epoch_count"] = len(records)
    # 返回不可变记录列表和公开摘要。
    return records, summary


def configure_plot_style() -> None:
    """配置适合中文教程和 A4 PDF 的浅色绘图风格。"""

    # 中文字体按 Windows、常见 Linux 中文字体和回退字体依次选择。
    plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "Noto Sans CJK SC", "SimHei", "DejaVu Sans"]
    # 允许负号使用标准 Unicode 字形。
    plt.rcParams["axes.unicode_minus"] = False
    # 轴标题使用深石墨色，避免纯黑造成过强对比。
    plt.rcParams["text.color"] = "#14213D"
    # 坐标轴文字沿用同一深色。
    plt.rcParams["axes.labelcolor"] = "#14213D"
    # 坐标轴标题采用半粗体以建立层级。
    plt.rcParams["axes.titleweight"] = "semibold"
    # 刻度文字使用低一档灰度。
    plt.rcParams["xtick.color"] = "#52606D"
    # 纵轴刻度使用同一灰度。
    plt.rcParams["ytick.color"] = "#52606D"


def plot_training_history(
    records: list[EpochRecord],
    summary: dict[str, str | int | float | bool],
    output_path: Path,
) -> None:
    """绘制训练目标、总体指标和弱类指标四联图。"""

    # 应用统一字体、颜色和坐标轴规则。
    configure_plot_style()
    # 横轴是从 1 开始的 epoch 序列。
    epochs = [record.epoch for record in records]
    # best_epoch 使用训练日志最终值，而不是从测试集重新选择。
    best_epoch = int(summary["best_epoch"])
    # 建立 2×2 子图；16×10 英寸配合 120 dpi 得到 1920×1200 PNG。
    figure, axes = plt.subplots(2, 2, figsize=(16, 10), dpi=120, constrained_layout=False)
    # 整体背景使用近白色，适合屏幕与打印。
    figure.patch.set_facecolor("#F7F9FC")
    # 四个坐标轴都使用纯白卡片底。
    for axis in axes.flat:
        # 设置卡片背景。
        axis.set_facecolor("#FFFFFF")
        # 只显示浅色水平网格，帮助读取趋势而不过度装饰。
        axis.grid(axis="y", color="#D9E2EC", linewidth=0.8, alpha=0.75)
        # 隐藏上边框，减少视觉噪声。
        axis.spines["top"].set_visible(False)
        # 隐藏右边框，减少视觉噪声。
        axis.spines["right"].set_visible(False)
        # 左边框使用浅灰。
        axis.spines["left"].set_color("#CBD5E1")
        # 下边框使用浅灰。
        axis.spines["bottom"].set_color("#CBD5E1")
        # 所有子图共享 epoch 横轴标题。
        axis.set_xlabel("Epoch")
        # 最佳 epoch 用蓝色虚线统一标记。
        axis.axvline(best_epoch, color="#0EA5E9", linestyle="--", linewidth=1.4, alpha=0.9)
    # 左上显示总目标与交叉熵，两条曲线单位均为无量纲损失。
    axes[0, 0].plot(epochs, [record.loss for record in records], color="#2563EB", linewidth=2.0, label="总损失")
    # 交叉熵单独显示，帮助区分复合目标下降来源。
    axes[0, 0].plot(epochs, [record.ce for record in records], color="#F97316", linewidth=1.8, label="交叉熵")
    # 设置损失面板标题。
    axes[0, 0].set_title("训练目标")
    # 损失是无量纲标量。
    axes[0, 0].set_ylabel("损失")
    # 图例放在右上角，避免遮挡前期高损失。
    axes[0, 0].legend(loc="upper right", frameon=False)
    # 右上显示总体验证准确率与宏平均 F1。
    axes[0, 1].plot(epochs, [record.val_acc for record in records], color="#10B981", linewidth=2.0, label="验证准确率")
    # 宏 F1 对每个类别等权，能暴露类别不均衡。
    axes[0, 1].plot(epochs, [record.val_f1 for record in records], color="#8B5CF6", linewidth=1.8, label="验证宏 F1")
    # 设置总体指标面板标题。
    axes[0, 1].set_title("总体验证指标")
    # 指标纵轴固定到 [0,1]，避免视觉夸大微小波动。
    axes[0, 1].set_ylim(0.0, 1.0)
    # 指标纵轴是比例。
    axes[0, 1].set_ylabel("比例")
    # 图例放在右下角，避开前期快速上升段。
    axes[0, 1].legend(loc="lower right", frameon=False)
    # 左下显示弱类集合和最弱类别的 F1。
    axes[1, 0].plot(epochs, [record.val_weak_f1 for record in records], color="#14B8A6", linewidth=2.0, label="弱类平均 F1")
    # 最弱类别 F1 是当前轮最难类别的下界。
    axes[1, 0].plot(epochs, [record.val_worst_f1 for record in records], color="#EF4444", linewidth=1.8, label="最弱类别 F1")
    # 设置弱类 F1 面板标题。
    axes[1, 0].set_title("弱类与下界 F1")
    # F1 固定显示完整 [0,1] 范围。
    axes[1, 0].set_ylim(0.0, 1.0)
    # F1 是比例。
    axes[1, 0].set_ylabel("比例")
    # 图例放在右下角。
    axes[1, 0].legend(loc="lower right", frameon=False)
    # 右下显示弱类集合和最弱类别的召回率。
    axes[1, 1].plot(epochs, [record.val_weak_recall for record in records], color="#0F766E", linewidth=2.0, label="弱类平均召回率")
    # 最弱类别召回率直接反映是否存在被大量漏识别的类别。
    axes[1, 1].plot(epochs, [record.val_min_recall for record in records], color="#DC2626", linewidth=1.8, label="最弱类别召回率")
    # 设置召回率面板标题。
    axes[1, 1].set_title("弱类召回率")
    # 召回率固定显示完整 [0,1] 范围。
    axes[1, 1].set_ylim(0.0, 1.0)
    # 召回率是比例。
    axes[1, 1].set_ylabel("比例")
    # 图例放在右下角。
    axes[1, 1].legend(loc="lower right", frameon=False)
    # 标题包含候选维数、窗口和总 epoch，避免图片脱离正文后失去上下文。
    figure.suptitle(
        f"模型训练过程（历史候选：{summary.get('feature_dim', '未知')} 维，"
        f"{summary.get('window_seconds', '未知')} 秒窗口，{len(records)} 个 epoch）",
        fontsize=18,
        fontweight="bold",
        x=0.06,
        ha="left",
        y=0.975,
    )
    # 副标题解释蓝色虚线，且明确选择依据来自验证集。
    figure.text(
        0.06,
        0.935,
        f"蓝色虚线：验证规则选出的最佳 epoch {best_epoch}；测试集不参与选模",
        fontsize=11,
        color="#52606D",
        ha="left",
    )
    # 页脚明确历史候选与当前端侧部署不是同一个模型工件。
    figure.text(
        0.06,
        0.018,
        "用途：展示逐 epoch 训练与早停过程。此候选未导出到固件；当前部署以 "
        "esp32/include/dual_m0_manifest.json 的 297 维双 M0 为准。",
        fontsize=10,
        color="#64748B",
        ha="left",
    )
    # 手工留出标题、副标题和页脚空间，避免 PDF 转换时裁切。
    figure.subplots_adjust(left=0.07, right=0.98, top=0.88, bottom=0.09, hspace=0.32, wspace=0.18)
    # 确保输出目录存在；只创建用户明确指定图片的父目录。
    output_path.parent.mkdir(parents=True, exist_ok=True)
    # 固定 PNG 元数据，不写当前时间，便于重复生成时核对内容。
    figure.savefig(
        output_path,
        dpi=120,
        facecolor=figure.get_facecolor(),
        metadata={"Software": "ESP32智慧运动助手训练曲线生成器"},
    )
    # 释放图形对象，避免批量生成时持续占用内存。
    plt.close(figure)


def write_manifest(
    manifest_path: Path,
    log_path: Path,
    output_path: Path,
    records: list[EpochRecord],
    summary: dict[str, str | int | float | bool],
) -> None:
    """写出图表来源、候选边界和当前部署清单引用。"""

    # 只公开稳定的相对语义，不把维护者本机绝对路径写入仓库。
    payload = {
        # schema_version 便于未来扩展字段时保持兼容。
        "schema_version": 1,
        # asset 记录本次生成的训练曲线文件名。
        "asset": output_path.name,
        # source_log_name 只保留日志文件名，完整本地 outputs 路径不进入公开仓库。
        "source_log_name": log_path.name,
        # source_log_sha256 证明图片来自哪一份原始日志。
        "source_log_sha256": sha256_file(log_path),
        # epoch_records 记录实际解析的轮数。
        "epoch_records": len(records),
        # candidate_training_summary 保存日志中可公开且与曲线直接相关的字段。
        "candidate_training_summary": summary,
        # purpose 明确该图只用于讲解训练过程。
        "purpose": "历史候选训练过程示例，不代表当前固件部署性能",
        # deployed_contract 指向仓库内当前权威模型清单。
        "deployed_contract": {
            "manifest": "esp32/include/dual_m0_manifest.json",
            "feature_dim": 297,
            "model": "双 M0 融合",
        },
        # fields 列出曲线真正使用的逐轮数据，方便读者自行审计。
        "fields": list(asdict(records[0]).keys()),
    }
    # 确保清单父目录存在。
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    # UTF-8、中文不转义和稳定缩进让清单适合 Git 审查。
    manifest_path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def build_argument_parser() -> argparse.ArgumentParser:
    """构造命令行参数解析器。"""

    # 程序说明指出输入是 train_export.py 的逐 epoch 日志。
    parser = argparse.ArgumentParser(description="从逐 epoch 训练日志生成教程曲线和来源清单")
    # --log 是必需输入，不允许隐式读取固定 outputs 目录。
    parser.add_argument("--log", type=Path, required=True, help="training_console.log 路径")
    # --output 是必需 PNG 输出。
    parser.add_argument("--output", type=Path, required=True, help="训练曲线 PNG 输出路径")
    # --manifest 是可选 JSON 输出；教程正式资产应提供该参数。
    parser.add_argument("--manifest", type=Path, help="可选的图表来源 JSON 清单路径")
    # 返回解析器供 main 使用。
    return parser


def main() -> int:
    """解析参数、生成曲线并返回进程退出码。"""

    # 解析当前命令行参数。
    args = build_argument_parser().parse_args()
    # 把输入路径解析为绝对路径，错误信息更明确。
    log_path = args.log.resolve()
    # 日志不存在时直接失败，不生成空图片。
    if not log_path.is_file():
        # FileNotFoundError 会让 CLI 返回非零并显示缺失路径。
        raise FileNotFoundError(f"训练日志不存在：{log_path}")
    # 解析真实逐 epoch 数据和候选摘要。
    records, summary = parse_training_log(log_path)
    # 生成训练曲线 PNG。
    plot_training_history(records, summary, args.output)
    # 用户提供清单路径时同步写出来源审计信息。
    if args.manifest is not None:
        # 清单与图片使用同一 records 和 summary，避免二次解析产生漂移。
        write_manifest(args.manifest, log_path, args.output, records, summary)
    # 输出简短完成信息，便于 CI 和教程读者核对轮数与最佳 epoch。
    print(
        f"TRAINING_CURVE_OK epochs={len(records)} "
        f"best_epoch={summary['best_epoch']} output={args.output}"
    )
    # 0 表示图片与可选清单均成功生成。
    return 0


# 直接执行脚本时调用 main；被导入时只暴露解析和绘图函数。
if __name__ == "__main__":
    # SystemExit 把 main 返回值传给操作系统。
    raise SystemExit(main())
