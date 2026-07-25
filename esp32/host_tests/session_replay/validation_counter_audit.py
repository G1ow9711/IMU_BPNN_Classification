"""用冻结验证文件压力测试生产 C 计数器对个体动作差异的鲁棒性。"""

# argparse 解析验证报告、生产回放器和 JSON 输出路径。
import argparse
# json 读取训练端冻结的文件级验证划分并写出结构化审计结果。
import json
# math 构造三轴旋转矩阵，模拟同一右腕上的佩戴角度变化。
import math
# re 从生产回放器的稳定摘要行提取事件数和累计值。
import re
# subprocess 把变换后的六轴流直接送入真实 C 状态机。
import subprocess
# Path 提供不受当前工作目录影响的绝对路径处理。
from pathlib import Path
# Dict、List 和 Tuple 明确中间结果的形状与字段类型。
from typing import Dict, List, Tuple

# NumPy 完成六轴单位换算、正交旋转、动态幅度缩放和时间重采样。
import numpy as np


# ACTION_IDS 与固件 fitness_action_t 0..10 完全同序，禁止单独调整。
ACTION_IDS: Dict[str, int] = {
    # 早安式使用动作枚举 0。
    "good_morning": 0,
    # 开合跳使用动作枚举 1。
    "jumping_jack": 1,
    # 跳跃弓步使用动作枚举 2。
    "jumping_lunge": 2,
    # 跳跃深蹲使用动作枚举 3。
    "jumping_squat": 3,
    # 弓步使用动作枚举 4。
    "lunge": 4,
    # 静坐使用动作枚举 5；本脚本只把它作为负样本，不统计时长事件。
    "sit": 5,
    # 深蹲使用动作枚举 6。
    "squat": 6,
    # 小跑使用动作枚举 7。
    "trot": 7,
    # 收腹跳使用动作枚举 8。
    "tuck_jump": 8,
    # 行走使用动作枚举 9。
    "walk": 9,
    # 挥手使用动作枚举 10。
    "wave": 10,
}

# REPETITION_ACTIONS 是静坐负样本需要逐一审计的八个重复动作计数器。
REPETITION_ACTIONS: Tuple[str, ...] = (
    # 早安式。
    "good_morning",
    # 开合跳。
    "jumping_jack",
    # 跳跃弓步。
    "jumping_lunge",
    # 跳跃深蹲。
    "jumping_squat",
    # 弓步。
    "lunge",
    # 深蹲。
    "squat",
    # 收腹跳。
    "tuck_jump",
    # 挥手。
    "wave",
)

# SUMMARY_PATTERN 匹配生产回放器唯一的 REPLAY_SUMMARY 行。
SUMMARY_PATTERN = re.compile(
    # 捕获事件数和最终累计；其余字段只用于保证没有误匹配普通日志。
    r"REPLAY_SUMMARY\s+session=\d+\s+action=\d+\s+samples=\d+\s+"
    r"first_ms=\d+\s+last_ms=\d+\s+events=(\d+)\s+total=(\d+)"
)


def parse_args() -> argparse.Namespace:
    """解析只读验证输入、生产回放器和 JSON 输出路径。"""
    # parser 描述该工具只做验证集压力测试，不负责选择外部六轮阈值。
    parser = argparse.ArgumentParser(description="Audit production counter robustness on frozen validation files.")
    # 验证报告必须含 all_experiments[0].val_files 文件级划分。
    parser.add_argument("--validation-report", type=Path, required=True)
    # 回放器必须由当前生产 C 源码重新编译生成。
    parser.add_argument("--replay-executable", type=Path, required=True)
    # 输出 JSON 保存逐文件、逐扰动计数及静坐误计。
    parser.add_argument("--output", type=Path, required=True)
    # 返回已解析参数。
    return parser.parse_args()


def load_imu_file(path: Path) -> np.ndarray:
    """读取原始六轴文本并转换为 `[N,6]` 的 deg/s 与 g。"""
    # raw 只读取 gx、gy、gz、ax、ay、az 六列，形状为 [采样点数,6]。
    raw = np.loadtxt(path, delimiter=",", dtype=np.float32, usecols=tuple(range(6)))
    # 单点文件需要恢复二维形状；正常训练文件通常包含数百点。
    if raw.ndim == 1:
        # reshape 保持通道顺序不变。
        raw = raw.reshape(1, -1)
    # data 是独立 float32 副本，防止单位换算覆盖 np.loadtxt 的共享视图。
    data = raw.astype(np.float32, copy=True)
    # 陀螺仪原始量程系数为 16.4 LSB/(deg/s)。
    data[:, 0:3] /= np.float32(16.4)
    # 加速度计原始量程系数为 4096 LSB/g。
    data[:, 3:6] /= np.float32(4096.0)
    # 返回固定 `[gx,gy,gz,ax,ay,az]` 顺序。
    return data


def rotation_matrix(rx: float, ry: float, rz: float) -> np.ndarray:
    """构造右手坐标系 `Rz·Ry·Rx` 正交旋转矩阵。"""
    # sx/cx 是绕 x 轴角度的正弦和余弦。
    sx, cx = math.sin(rx), math.cos(rx)
    # sy/cy 是绕 y 轴角度的正弦和余弦。
    sy, cy = math.sin(ry), math.cos(ry)
    # sz/cz 是绕 z 轴角度的正弦和余弦。
    sz, cz = math.sin(rz), math.cos(rz)
    # rotation_x 只旋转 y-z 平面。
    rotation_x = np.asarray(((1.0, 0.0, 0.0), (0.0, cx, -sx), (0.0, sx, cx)), dtype=np.float32)
    # rotation_y 只旋转 x-z 平面。
    rotation_y = np.asarray(((cy, 0.0, sy), (0.0, 1.0, 0.0), (-sy, 0.0, cy)), dtype=np.float32)
    # rotation_z 只旋转 x-y 平面。
    rotation_z = np.asarray(((cz, -sz, 0.0), (sz, cz, 0.0), (0.0, 0.0, 1.0)), dtype=np.float32)
    # 组合矩阵保持单位模长，模拟佩戴角度变化而不修改动作能量。
    return np.asarray(rotation_z @ rotation_y @ rotation_x, dtype=np.float32)


def rotate_record(data: np.ndarray) -> np.ndarray:
    """把同一正交旋转同时应用到角速度和加速度三轴。"""
    # matrix 使用固定但非整轴角度，避免压力测试退化成简单换轴。
    matrix = rotation_matrix(math.radians(30.0), math.radians(-25.0), math.radians(20.0))
    # rotated 保留输入形状和 float32 精度。
    rotated = np.asarray(data, dtype=np.float32).copy()
    # 陀螺仪是右手坐标下的轴向量；纯旋转时与普通向量使用同一正交矩阵。
    rotated[:, 0:3] = data[:, 0:3] @ matrix.T
    # 加速度三轴执行相同佩戴旋转，保持六轴坐标合同一致。
    rotated[:, 3:6] = data[:, 3:6] @ matrix.T
    # 返回旋转后的六轴记录。
    return rotated


def scale_motion_amplitude(data: np.ndarray, scale: float) -> np.ndarray:
    """缩放动作动态量，保留窗口平均重力，模拟不同人的幅度差异。"""
    # scaled 是独立副本，输入验证文件保持只读。
    scaled = np.asarray(data, dtype=np.float32).copy()
    # 陀螺仪角速度整体乘 scale，直接表示动作更小或更大。
    scaled[:, 0:3] *= np.float32(scale)
    # gravity 取整段平均加速度，形状 [1,3]、单位 g。
    gravity = np.mean(scaled[:, 3:6], axis=0, keepdims=True, dtype=np.float64).astype(np.float32)
    # 只缩放相对重力的动态分量，禁止把 1 g 静态重力错误缩成 0.7 g 或 1.3 g。
    scaled[:, 3:6] = gravity + ((scaled[:, 3:6] - gravity) * np.float32(scale))
    # 返回物理上更合理的幅度扰动记录。
    return scaled


def resample_record(data: np.ndarray, duration_scale: float) -> np.ndarray:
    """保持 25 Hz 输出并改变整段持续时间，模拟较慢或较快的同类动作。"""
    # source_count 是原始采样点数。
    source_count = int(len(data))
    # target_count 至少保留 2 点，避免插值时间轴退化。
    target_count = max(2, int(round(source_count * duration_scale)))
    # source_timeline 把原始整段归一化到 [0,1]。
    source_timeline = np.linspace(0.0, 1.0, source_count, dtype=np.float64)
    # target_timeline 在相同归一化区间按新点数采样，因此周期数不变、速度改变。
    target_timeline = np.linspace(0.0, 1.0, target_count, dtype=np.float64)
    # result 保存 `[target_count,6]` float32 六轴输出。
    result = np.empty((target_count, 6), dtype=np.float32)
    # 逐轴线性插值；六轴互不混合，坐标和单位不变。
    for axis in range(6):
        # 当前轴插值结果写入对应列。
        result[:, axis] = np.interp(target_timeline, source_timeline, data[:, axis]).astype(np.float32)
    # 返回持续时间变化后的 25 Hz 输入。
    return result


def transformed_records(data: np.ndarray) -> Dict[str, np.ndarray]:
    """生成不含随机性的五类个体差异压力样本。"""
    # clean 保留验证文件原始物理单位，作为每个文件自己的基线。
    clean = np.asarray(data, dtype=np.float32)
    # 返回固定字典，保证所有运行使用完全相同的扰动集合。
    return {
        # 原始验证记录。
        "clean": clean,
        # 同一右腕约 44 度复合佩戴旋转。
        "rotated": rotate_record(clean),
        # 动作动态幅度缩小 30%。
        "amplitude_0_70": scale_motion_amplitude(clean, 0.70),
        # 动作动态幅度放大 30%。
        "amplitude_1_30": scale_motion_amplitude(clean, 1.30),
        # 整段延长至 1.25 倍，模拟较慢动作。
        "duration_1_25": resample_record(clean, 1.25),
        # 整段缩短至 0.80 倍，模拟较快动作。
        "duration_0_80": resample_record(clean, 0.80),
    }


def serialize_record(data: np.ndarray) -> str:
    """把 `[N,6]` 记录编码为生产回放器八列 ASCII 标准输入。"""
    # lines 收集 `ms,gx,gy,gz,ax,ay,az,quality`，质量位固定为干净 0。
    lines: List[str] = []
    # 逐点使用严格 40 ms 时间戳，复现固件 25 Hz 合同。
    for index, row in enumerate(np.asarray(data, dtype=np.float32)):
        # values 使用足够有效数字保留 float32 往返值。
        values = ",".join(format(float(value), ".9g") for value in row)
        # 质量 0x0000 表示本压力测试不混入断流变量。
        lines.append(f"{index * 40},{values},0x0000")
    # 末尾换行使 C fgets 能消费最后一点。
    return "\n".join(lines) + "\n"


def replay_count(executable: Path, data: np.ndarray, action_id: int) -> int:
    """运行真实生产 C 链并返回最终权威累计。"""
    # command 只传虚拟会话 1 和人工真值动作；动作只选择计数器，不参与阈值拟合。
    command = [str(executable), "1", str(action_id)]
    # completed 同步执行回放，stdin 为六轴流，stdout/stderr 全部捕获用于失败诊断。
    completed = subprocess.run(
        command,
        input=serialize_record(data),
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
        check=False,
    )
    # 非零退出表示生产链或输入合同失败，不能跳过该文件。
    if completed.returncode != 0:
        # 异常包含精确退出码和 stderr，便于复现。
        raise RuntimeError(f"replay failed rc={completed.returncode}: {completed.stderr.strip()}")
    # match 在完整 stdout 中查找唯一摘要。
    match = SUMMARY_PATTERN.search(completed.stdout)
    # 缺摘要表示运行器格式漂移或进程异常结束。
    if match is None:
        # 保留末尾输出帮助定位，不伪造零次数。
        raise RuntimeError(f"replay summary missing: {completed.stdout[-1000:]}")
    # 第二捕获组是生产状态机最终累计，必须与事件数一致。
    total = int(match.group(2))
    # 返回本次记录累计。
    return total


def main() -> None:
    """审计验证文件扰动稳定性和静坐负样本误计。"""
    # args 保存三个显式路径。
    args = parse_args()
    # 验证报告和回放器都必须真实存在。
    if not args.validation_report.is_file() or not args.replay_executable.is_file():
        # 精确报出缺失输入，禁止自动寻找另一份工件。
        raise FileNotFoundError("validation report or replay executable is missing")
    # report 从冻结 JSON 读取，不重新随机切分文件。
    report = json.loads(args.validation_report.read_text(encoding="utf-8"))
    # experiments 必须至少包含一个正式窗口配置。
    experiments = report.get("all_experiments", [])
    # 空实验无法提供验证文件角色。
    if not experiments:
        # 立即失败，禁止把训练或测试文件误当验证集。
        raise ValueError("validation report has no experiment")
    # val_files 直接使用历史训练报告冻结的按文件验证划分。
    val_files = [Path(value) for value in experiments[0].get("val_files", [])]
    # 空验证文件集合属于报告损坏。
    if not val_files:
        # 不退化为扫描整个数据集。
        raise ValueError("validation report has no val_files")
    # dynamic_rows 保存非静坐逐文件的原始及五种扰动计数。
    dynamic_rows: List[Dict[str, object]] = []
    # sit_rows 保存每个静坐文件被错误送入八类重复计数器后的次数。
    sit_rows: List[Dict[str, object]] = []
    # 按报告顺序遍历，确保输出可与训练工件逐项比对。
    for file_index, path in enumerate(val_files, start=1):
        # 文件必须仍位于报告记录的精确路径。
        if not path.is_file():
            # 禁止用同名文件替换。
            raise FileNotFoundError(path)
        # label 取父目录英文类别名，与模型/固件枚举映射一致。
        label = path.parent.name
        # 未知父目录说明报告和当前 11 类合同不一致。
        if label not in ACTION_IDS:
            # 立即失败，避免静默错选计数器。
            raise ValueError(f"unknown action label: {label}")
        # data 转成生产单位 `[gx,gy,gz,ax,ay,az]`。
        data = load_imu_file(path)
        # 静坐只作为负样本，逐个重复计数器检查幽灵次数。
        if label == "sit":
            # false_counts 保存八个动作名到误计次数的映射。
            false_counts = {
                # 对同一静坐流使用每个重复动作计数器，不改变信号。
                action_name: replay_count(args.replay_executable, data, ACTION_IDS[action_name])
                # 固定遍历八类重复动作。
                for action_name in REPETITION_ACTIONS
            }
            # 记录文件、时长和逐计数器误计。
            sit_rows.append(
                {
                    # 保存绝对路径以保持可审计性。
                    "file": str(path.resolve()),
                    # duration_seconds 使用 25 Hz 点数换算，仅用于误计率归一化。
                    "duration_seconds": float(len(data) / 25.0),
                    # false_counts 不包含静坐时长事件。
                    "false_counts": false_counts,
                }
            )
            # 静坐不进入动态稳定性统计。
            continue
        # counts 保存 clean 与五种确定性扰动的生产累计。
        counts = {
            # 每种变换仍使用同一真实类别计数器。
            transform_name: replay_count(args.replay_executable, transformed, ACTION_IDS[label])
            # 逐项生成后立即回放，避免保存大型中间数组。
            for transform_name, transformed in transformed_records(data).items()
        }
        # clean_count 是本文件无扰动基线。
        clean_count = int(counts["clean"])
        # tolerance 允许最多 1 次或 clean 的 15%，只判断扰动稳定性，不声称真实计数准确率。
        tolerance = max(1, int(math.ceil(clean_count * 0.15)))
        # robust_pass 要求每种扰动相对同文件 clean 均不超容差。
        robust_pass = all(abs(int(value) - clean_count) <= tolerance for value in counts.values())
        # 保存逐文件结果。
        dynamic_rows.append(
            {
                # 文件索引只用于稳定排序。
                "validation_index": file_index,
                # 保存绝对文件路径。
                "file": str(path.resolve()),
                # 保存英文动作名。
                "label": label,
                # 原始时长按 25 Hz 换算。
                "duration_seconds": float(len(data) / 25.0),
                # counts 是全部生产累计。
                "counts": counts,
                # tolerance 记录稳定性判定边界。
                "stability_tolerance": tolerance,
                # robust_pass 是个体差异压力门结果。
                "robust_pass": robust_pass,
            }
        )
    # labels 收集动态验证集中出现的类别，保持字母顺序便于对比。
    labels = sorted({str(row["label"]) for row in dynamic_rows})
    # per_class 汇总原始事件率和扰动通过率；不把事件率当人工真值准确率。
    per_class: Dict[str, Dict[str, object]] = {}
    # 逐类别汇总。
    for label in labels:
        # rows 选择当前类别的验证文件。
        rows = [row for row in dynamic_rows if row["label"] == label]
        # clean_counts 收集每文件原始累计。
        clean_counts = [int(row["counts"]["clean"]) for row in rows]
        # rates 使用原始时长换算每分钟事件数，仅描述结构覆盖。
        rates = [count / (float(row["duration_seconds"]) / 60.0) for count, row in zip(clean_counts, rows)]
        # 写入类别级结构统计。
        per_class[label] = {
            # 当前类别冻结验证文件数。
            "file_count": len(rows),
            # 至少产生一个事件的文件比例。
            "nonzero_file_rate": float(np.mean([count > 0 for count in clean_counts])),
            # 原始每分钟事件数中位数，不表示真实次数准确率。
            "median_events_per_minute": float(np.median(np.asarray(rates, dtype=np.float64))),
            # 五种扰动均在容差内的文件比例。
            "robust_file_rate": float(np.mean([bool(row["robust_pass"]) for row in rows])),
        }
    # total_sit_minutes 是全部静坐负样本总时长。
    total_sit_minutes = sum(float(row["duration_seconds"]) for row in sit_rows) / 60.0
    # false_totals 按计数器累计全部静坐误计。
    false_totals = {
        # 每个动作汇总所有静坐文件。
        action_name: sum(int(row["false_counts"][action_name]) for row in sit_rows)
        # 遍历八类重复动作。
        for action_name in REPETITION_ACTIONS
    }
    # result 保存来源、逐文件结果和不夸大的结构级指标。
    result = {
        # schema_version 便于未来字段升级时拒绝旧结果。
        "schema_version": 1,
        # validation_report 保存精确工件来源。
        "validation_report": str(args.validation_report.resolve()),
        # replay_executable 保存生产 C 回放器来源。
        "replay_executable": str(args.replay_executable.resolve()),
        # 说明该报告没有逐次人工标签，不能计算计数 MAE。
        "count_accuracy_claim": "not_available_no_per_rep_ground_truth",
        # perturbations 明确五种个体差异压力条件。
        "perturbations": ["rotated", "amplitude_0_70", "amplitude_1_30", "duration_1_25", "duration_0_80"],
        # dynamic_file_count 是非静坐验证文件数。
        "dynamic_file_count": len(dynamic_rows),
        # robust_file_rate 是全部动态文件压力门通过率。
        "robust_file_rate": float(np.mean([bool(row["robust_pass"]) for row in dynamic_rows])),
        # nonzero_file_rate 是结构覆盖率。
        "nonzero_file_rate": float(np.mean([int(row["counts"]["clean"]) > 0 for row in dynamic_rows])),
        # per_class 保存类别级结构统计。
        "per_class": per_class,
        # sit_negative 保存误计总数和每分钟率。
        "sit_negative": {
            # 静坐文件数。
            "file_count": len(sit_rows),
            # 静坐总时长。
            "total_minutes": total_sit_minutes,
            # 各重复计数器误计总数。
            "false_totals": false_totals,
            # 各重复计数器每分钟误计；零时长防御为 0。
            "false_events_per_minute": {
                action_name: (float(total) / total_sit_minutes if total_sit_minutes > 0.0 else 0.0)
                for action_name, total in false_totals.items()
            },
        },
        # dynamic_files 保留全部原始与扰动累计。
        "dynamic_files": dynamic_rows,
        # sit_files 保留逐文件静坐误计。
        "sit_files": sit_rows,
    }
    # 创建输出父目录，限制在调用方显式路径内。
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # 以 UTF-8 和缩进 JSON 写出，中文说明保持可读。
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    # 输出总门摘要，供 CI/人工快速读取。
    print(
        f"VALIDATION_COUNTER_AUDIT dynamic_files={len(dynamic_rows)} "
        f"nonzero_rate={result['nonzero_file_rate']:.4f} "
        f"robust_rate={result['robust_file_rate']:.4f} "
        f"sit_files={len(sit_rows)}"
    )
    # 逐类别输出结构覆盖和扰动稳定率。
    for label, metrics in per_class.items():
        # 每行只包含稳定键值，便于日志解析。
        print(
            f"VALIDATION_CLASS label={label} files={metrics['file_count']} "
            f"nonzero_rate={metrics['nonzero_file_rate']:.4f} "
            f"median_per_min={metrics['median_events_per_minute']:.3f} "
            f"robust_rate={metrics['robust_file_rate']:.4f}"
        )
    # 输出静坐误计汇总。
    print("VALIDATION_SIT_FALSE " + " ".join(f"{name}={value}" for name, value in false_totals.items()))


# 仅作为脚本执行时启动审计；被单元测试导入时不访问文件或子进程。
if __name__ == "__main__":
    # 执行主流程。
    main()
