from __future__ import annotations

"""在冻结验证文件和外部真板日志上比较首次主动作锁定策略。"""

# argparse 解析冻结数据、模型工件、外部日志和输出路径。
import argparse
# csv 读取上位机导出的 44 列中文真板日志。
import csv
# json 读取冻结验证角色并写出完整审计结果。
import json
# Path 统一处理 Windows 绝对路径。
from pathlib import Path
# sys 支持直接执行时优先导入当前工作树。
import sys
# 类型标注约束策略、指标和真板记录形状。
from typing import Dict, List, Mapping, Sequence, Tuple

# NumPy 计算双 M0 前向输入、softmax 和文件级指标。
import numpy as np

# 当前脚本上一级的上一级是仓库根目录。
PROJECT_ROOT = Path(__file__).resolve().parents[1]
# 直接运行时把当前工作树放到模块搜索首位。
if str(PROJECT_ROOT) not in sys.path:
    # 禁止意外加载其它 worktree 的同名 Python 包。
    sys.path.insert(0, str(PROJECT_ROOT))

# 当前生产策略名称固定，报告和测试均以它作为基线。
POLICY_CUMULATIVE_STABLE = "cumulative_stable"
# 候选策略使用独立单窗 Top-1 连续确认，抑制首个起手过渡窗。
POLICY_WINDOW_STABLE = "window_stable"
# 策略顺序固定，防止根据中间结果临时增删候选。
POLICY_NAMES: Tuple[str, ...] = (
    POLICY_CUMULATIVE_STABLE,
    POLICY_WINDOW_STABLE,
)
# 两个独立证据窗属于生产实时性上限内的最小稳健门。
STABLE_WINDOWS = 2
# softmax 低于 50% 的单窗或累计候选不形成确认事实。
MIN_PROBABILITY = 0.50
# 继续沿用现有四窗上限，避免扩大准备缓存和首次显示最坏延迟。
MAX_WINDOWS = 4

# 中文日志动作名映射到冻结模型类别字符串。
ACTION_ZH_TO_CLASS: Mapping[str, str] = {
    # 早安式与模型 good_morning 一一对应。
    "早安式": "good_morning",
    # 开合跳与模型 jumping_jack 一一对应。
    "开合跳": "jumping_jack",
    # 跳跃弓步与模型 jumping_lunge 一一对应。
    "跳跃弓步": "jumping_lunge",
    # 跳跃深蹲与模型 jumping_squat 一一对应。
    "跳跃深蹲": "jumping_squat",
    # 弓步与模型 lunge 一一对应。
    "弓步": "lunge",
    # 静坐与模型 sit 一一对应。
    "静坐": "sit",
    # 深蹲与模型 squat 一一对应。
    "深蹲": "squat",
    # 小跑与模型 trot 一一对应。
    "小跑": "trot",
    # 收腹跳与模型 tuck_jump 一一对应。
    "收腹跳": "tuck_jump",
    # 行走与模型 walk 一一对应。
    "行走": "walk",
    # 挥手与模型 wave 一一对应。
    "挥手": "wave",
}


def stable_softmax_vector(logits: np.ndarray) -> np.ndarray:
    """对一维有限 logits 计算数值稳定 softmax。"""

    # values 转为 float64，降低指数和的舍入误差。
    values = np.asarray(logits, dtype=np.float64)
    # 输入必须是一维非空有限向量。
    if values.ndim != 1 or values.size == 0 or not np.all(np.isfinite(values)):
        # 与端侧失败关闭合同一致。
        raise ValueError("Softmax logits must be a finite non-empty vector")
    # shifted 减去最大值，避免 exp 上溢。
    shifted = values - float(np.max(values))
    # exponentials 每项范围为 `(0,1]`。
    exponentials = np.exp(shifted)
    # denominator 是有限正指数和。
    denominator = float(np.sum(exponentials))
    # 非有限或非正分母表示数值合同损坏。
    if not np.isfinite(denominator) or denominator <= 0.0:
        # 拒绝产生伪概率。
        raise ValueError("Softmax denominator is invalid")
    # 返回总和约为一的概率向量。
    return exponentials / denominator


def parse_args() -> argparse.Namespace:
    """解析冻结验证和可选真板日志输入。"""

    # parser 明确本工具只比较决策策略，不训练或改写模型。
    parser = argparse.ArgumentParser(description="冻结双 M0 首次锁类策略网格")
    # dataset-dir 指向用户指定 imu_dataset_for_final。
    parser.add_argument("--dataset-dir", type=Path, required=True)
    # extra-train-dir 仅用于恢复原冻结文件角色，不进入验证指标。
    parser.add_argument("--extra-train-dir", type=Path, required=True)
    # base-artifact-dir 指向 Round29 未掩码 M0。
    parser.add_argument("--base-artifact-dir", type=Path, required=True)
    # masked-artifact-dir 指向 Round37 掩码 M0。
    parser.add_argument("--masked-artifact-dir", type=Path, required=True)
    # true-board-csv 可选地读取本次两轮挥手日志。
    parser.add_argument("--true-board-csv", type=Path)
    # true-board-truth 使用“会话序号:英文类别”，可重复提供。
    parser.add_argument("--true-board-truth", action="append", default=[])
    # output 保存六条件、两策略和真板明细。
    parser.add_argument("--output", type=Path, required=True)
    # 返回稳定命名空间。
    return parser.parse_args()


def lock_one_file(
    scores: np.ndarray,
    policy_name: str,
) -> Dict[str, object]:
    """对单个按时间排列的 `[窗口数,类别数]` logits 执行有界因果锁类。"""

    # values 转成 float64，降低累计与 softmax 的舍入误差。
    values = np.asarray(scores, dtype=np.float64)
    # 输入必须是非空二维有限矩阵。
    if values.ndim != 2 or values.shape[0] == 0 or values.shape[1] == 0:
        # 空文件或错误形状无法形成会话级决策。
        raise ValueError("Lock policy requires a non-empty 2-D score matrix")
    # 端侧拒绝 NaN/Inf，验证器同样失败关闭。
    if not np.all(np.isfinite(values)):
        # 防止 NaN argmax 伪造正确锁类。
        raise ValueError("Lock policy scores must be finite")
    # 只允许两个预声明策略。
    if policy_name not in POLICY_NAMES:
        # 未知字符串不能静默回退到基线。
        raise ValueError(f"Unknown lock policy: {policy_name}")

    # cumulative 保存从会话起点到当前窗的 11 类无量纲 logits 和。
    cumulative = np.zeros(values.shape[1], dtype=np.float64)
    # candidate 保存上一份有效候选类别。
    candidate: int | None = None
    # consecutive 保存同一证据类别连续满足概率门的窗口数。
    consecutive = 0
    # locked_class 为 None 表示当前文件仍未锁定。
    locked_class: int | None = None
    # lock_probability 保存锁定时使用的证据概率。
    lock_probability = 0.0
    # windows_used 保存最终观察窗数。
    windows_used = 0
    # forced 表示仅因第四窗上限提交累计类别。
    forced = False
    # evidence_classes 保存每窗真正用于连续门的类别，便于审计起手变化。
    evidence_classes: List[int] = []

    # 只读取前四窗，保持现有准备缓存和最坏延迟不变。
    for local_index, current in enumerate(values[:MAX_WINDOWS], start=1):
        # 累加当前融合 logits，供基线证据和第四窗兜底共同使用。
        cumulative += current
        # averaged 是从会话起点到当前窗的因果平均 logits。
        averaged = cumulative / float(local_index)
        # 基线使用累计 Top-1；候选使用当前独立窗口 Top-1。
        evidence_logits = averaged if policy_name == POLICY_CUMULATIVE_STABLE else current
        # evidence_class 是本窗用于连续性判断的动作索引。
        evidence_class = int(np.argmax(evidence_logits))
        # evidence_probability 使用数值稳定 softmax，范围 0～1。
        evidence_probability = float(stable_softmax_vector(evidence_logits)[evidence_class])
        # 保存证据序列，输出可追溯“深蹲→早安式→挥手→挥手”。
        evidence_classes.append(evidence_class)
        # 低置信证据不能延续同类连续计数。
        if evidence_probability < MIN_PROBABILITY:
            # 清空候选，要求后续重新形成两窗完整证据。
            candidate = None
            # 连续数归零。
            consecutive = 0
        elif candidate == evidence_class:
            # 同类且当前窗仍过概率门，增加连续证据。
            consecutive += 1
        else:
            # 新的高置信类别成为候选。
            candidate = evidence_class
            # 当前窗是该候选第一份证据。
            consecutive = 1

        # 正常锁定要求两个独立有效证据窗。
        stable = consecutive >= STABLE_WINDOWS
        # 第四窗达到有界准备上限。
        reached_limit = local_index >= MAX_WINDOWS
        # 保存已读取窗口数。
        windows_used = local_index
        # 正常门和上限均未满足时继续下一窗。
        if not stable and not reached_limit:
            # 保持因果扫描。
            continue
        # 正常门使用证据类别，候选策略因此不会被首个过渡窗的累计和拖住。
        if stable and candidate is not None:
            # 提交已连续确认的类别。
            locked_class = int(candidate)
            # 保存最后一窗证据概率。
            lock_probability = evidence_probability
        else:
            # 第四窗仍不稳定时使用全部四窗累计平均作通用兜底。
            locked_class = int(np.argmax(averaged))
            # 兜底概率来自同一累计平均。
            lock_probability = float(stable_softmax_vector(averaged)[locked_class])
            # 标记强制锁定，报告不能把它混入正常确认。
            forced = True
        # 锁定后本轮主动作固定，停止读取未来窗口。
        break

    # 返回 JSON 可序列化的文件级因果事实。
    return {
        # 短文件可能仍为 None。
        "locked_class_index": locked_class,
        # forced 区分正常连续确认和有界兜底。
        "forced": forced,
        # windows_used 范围为 1～4。
        "windows_used": windows_used,
        # lock_probability 范围为 0～1。
        "lock_probability": lock_probability,
        # 证据序列只含已经观察的窗。
        "evidence_class_indices": evidence_classes,
    }


def policy_metrics(
    logits: np.ndarray,
    labels: np.ndarray,
    file_ids: np.ndarray,
    class_names: Sequence[str],
    policy_name: str,
) -> Dict[str, object]:
    """按冻结文件边界汇总锁类准确率、逐类召回、兜底和延迟。"""

    # scores 形状为 `[窗口数,类别数]`。
    scores = np.asarray(logits, dtype=np.float64)
    # truth 保存每窗人工动作索引。
    truth = np.asarray(labels, dtype=np.int64)
    # groups 保存每窗所属冻结文件编号。
    groups = np.asarray(file_ids, dtype=np.int64)
    # 三个首维和类别宽度必须完全一致。
    if (
        scores.ndim != 2
        or truth.ndim != 1
        or groups.ndim != 1
        or len(scores) != len(truth)
        or len(scores) != len(groups)
        or scores.shape[1] != len(class_names)
    ):
        # 防止标签、文件和 logits 错位。
        raise ValueError("Lock policy input shapes differ")

    # records 保存每个验证文件的一条结果。
    records: List[Dict[str, object]] = []
    # correct_by_class 保存每类正确文件数。
    correct_by_class = np.zeros(len(class_names), dtype=np.int64)
    # total_by_class 保存每类验证文件数。
    total_by_class = np.zeros(len(class_names), dtype=np.int64)
    # 文件编号按首次出现顺序稳定遍历。
    ordered_file_ids = list(dict.fromkeys(int(value) for value in groups.tolist()))
    # 每个文件独立清零策略状态。
    for file_id in ordered_file_ids:
        # indices 保持该文件原时间顺序。
        indices = np.flatnonzero(groups == file_id)
        # 文件标签必须唯一。
        file_labels = np.unique(truth[indices])
        # 混合标签文件不符合单轮单动作合同。
        if len(file_labels) != 1:
            # 停止而不是选择首个标签。
            raise ValueError(f"Validation file {file_id} has mixed labels")
        # true_class 是该文件人工类别。
        true_class = int(file_labels[0])
        # 当前类别文件总数增加一。
        total_by_class[true_class] += 1
        # 执行指定策略。
        result = lock_one_file(scores[indices], policy_name)
        # 读取可为空的锁定类别。
        locked_class = result["locked_class_index"]
        # correct 表示最终主动作与人工标签相同。
        correct = locked_class == true_class
        # 正确时累加逐类计数。
        if correct:
            # 保存逐类正确文件数。
            correct_by_class[true_class] += 1
        # 保存文件明细。
        records.append(
            {
                # 冻结文件编号用于和原报告逐项对齐。
                "file_id": file_id,
                # 人工类别使用稳定英文模型名。
                "true_class": class_names[true_class],
                # 未锁定输出 null。
                "locked_class": (
                    None if locked_class is None else class_names[int(locked_class)]
                ),
                # 保存最终正确性。
                "correct": bool(correct),
                # 合并策略级因果明细。
                **result,
            }
        )

    # 文件数必须大于零。
    file_count = len(records)
    # 空验证集属于输入损坏。
    if file_count == 0:
        # 拒绝除零。
        raise ValueError("Lock policy produced no file records")
    # class_recalls 保存 11 类文件级召回。
    class_recalls = {
        # 缺失类别按零报告，冻结集正常不会缺类。
        class_names[index]: (
            float(correct_by_class[index] / total_by_class[index])
            if total_by_class[index] > 0
            else 0.0
        )
        # 遍历固定模型类别顺序。
        for index in range(len(class_names))
    }
    # 返回稳定汇总。
    return {
        # 策略名称显式进入报告。
        "policy": policy_name,
        # 文件总数。
        "file_count": file_count,
        # 正确文件数。
        "correct_file_count": sum(bool(record["correct"]) for record in records),
        # 未锁定文件数。
        "unlocked_file_count": sum(
            record["locked_class_index"] is None for record in records
        ),
        # 文件级准确率，未锁定按错误。
        "accuracy": float(
            sum(bool(record["correct"]) for record in records) / file_count
        ),
        # 强制锁定文件数。
        "forced_lock_count": sum(bool(record["forced"]) for record in records),
        # 平均窗口数反映确认延迟。
        "mean_windows_used": float(
            np.mean([int(record["windows_used"]) for record in records])
        ),
        # 逐类召回用于保护弱类。
        "class_recalls": class_recalls,
        # 文件明细支持追溯。
        "records": records,
    }


def fused_logits(
    raw_features: np.ndarray,
    base_artifact: Tuple[object, np.ndarray, np.ndarray, bool],
    masked_artifact: Tuple[object, np.ndarray, np.ndarray, bool],
) -> np.ndarray:
    """用当前 0.85/0.15 双 M0 对 `[窗口数,297]` 特征执行冻结前向。"""

    # 仅正式评估进入本函数时加载 PyTorch 双 M0 工具，纯策略单测不承担框架启动成本。
    from python import evaluate_fixed_ensemble as fixed
    # 仅正式评估加载训练端掩码和融合合同。
    from python import train_export as training

    # 解包基础模型和独立标准化参数。
    base_model, base_mean, base_std, base_suppress = base_artifact
    # 解包掩码模型和独立标准化参数。
    masked_model, masked_mean, masked_std, masked_suppress = masked_artifact
    # 工件角色必须为未掩码基础加掩码第二模型。
    if base_suppress or not masked_suppress:
        # 角色交换会破坏端侧一致性。
        raise ValueError("Expected unmasked base and masked secondary artifacts")
    # 基础输入执行 z=(x-mean)/std。
    base_x = ((raw_features - base_mean) / base_std).astype(np.float32)
    # 掩码输入先用自身统计标准化。
    masked_x = ((raw_features - masked_mean) / masked_std).astype(np.float32)
    # 第二模型把 184:232 标准分固定为零。
    masked_x = training.apply_model_feature_mask(masked_x, True)
    # 基础 M0 输出 `[窗口数,11]`。
    base_scores = fixed.batched_logits(base_model, base_x)
    # 掩码 M0 输出相同形状和类别顺序。
    masked_scores = fixed.batched_logits(masked_model, masked_x)
    # 使用当前部署 0.85/0.15 固定融合。
    return training.combine_ensemble_logits(base_scores, masked_scores)


def parse_true_board_truth(values: Sequence[str]) -> Dict[int, str]:
    """解析重复的“会话序号:英文类别”参数。"""

    # truth 保存会话到模型类别的唯一映射。
    truth: Dict[int, str] = {}
    # 逐参数解析。
    for value in values:
        # 只按首个冒号切分，拒绝缺字段。
        parts = value.split(":", 1)
        # 参数必须恰有会话和类别。
        if len(parts) != 2:
            # 给出稳定错误。
            raise ValueError(f"Invalid true-board truth: {value}")
        # 会话序号转十进制整数。
        session_seq = int(parts[0])
        # 类别去除首尾空白。
        class_name = parts[1].strip()
        # 同一会话重复声明会产生歧义。
        if session_seq in truth:
            # 拒绝覆盖。
            raise ValueError(f"Duplicate true-board session: {session_seq}")
        # 保存人工真值。
        truth[session_seq] = class_name
    # 返回映射。
    return truth


def evaluate_logged_sessions(
    csv_path: Path,
    truth: Mapping[int, str],
) -> Dict[str, object]:
    """用设备已导出的独立窗口类别/置信度验证候选连续门。"""

    # 以 UTF-8 BOM 兼容方式读取 44 列日志。
    with csv_path.open("r", encoding="utf-8-sig", newline="") as handle:
        # rows 保存字典行，列名来自导出合同。
        rows = list(csv.DictReader(handle))
    # records 保存每个声明会话的候选决策。
    records: List[Dict[str, object]] = []
    # 逐人工会话稳定遍历。
    for session_seq, true_class in truth.items():
        # windows 按分类窗口结束时间收集并去重。
        windows: List[Tuple[int, str, float]] = []
        # seen 防止每个 IMU 行携带的最近分类快照重复计算。
        seen: set[Tuple[str, str]] = set()
        # 扫描全部日志行。
        for row in rows:
            # 只读取目标会话。
            if row["会话序号"].strip() != str(session_seq):
                # 继续下一行。
                continue
            # 只有真实窗口结束行才形成一份证据。
            if row["分类窗口是否在本行结束"].strip() != "是":
                # 快照行不重复计算。
                continue
            # key 由窗口序号和结束时间唯一标识。
            key = (
                row["分类窗口序号"].strip(),
                row["分类窗口结束时间（毫秒）"].strip(),
            )
            # 重复窗口安全跳过。
            if key in seen:
                # 继续下一行。
                continue
            # 标记已读取。
            seen.add(key)
            # 中文类别必须存在于固定映射。
            class_name = ACTION_ZH_TO_CLASS[row["融合模型类别"].strip()]
            # 置信度由十进制文本转 0～1 浮点。
            probability = float(row["融合模型置信度"])
            # 保存窗口序号、类别和概率。
            windows.append((int(key[0]), class_name, probability))
        # 会话必须至少有一个窗口。
        if not windows:
            # 缺日志事实时停止。
            raise ValueError(f"True-board session {session_seq} has no windows")
        # candidate 保存上一高置信独立窗口类别。
        candidate: str | None = None
        # consecutive 保存连续窗数。
        consecutive = 0
        # locked_class 保存候选策略结果。
        locked_class: str | None = None
        # windows_used 保存相对观察窗数。
        windows_used = 0
        # evidence 保存前四窗审计序列。
        evidence: List[Dict[str, object]] = []
        # 候选策略最多读取四窗。
        for local_index, (window_sequence, class_name, probability) in enumerate(
            windows[:MAX_WINDOWS],
            start=1,
        ):
            # 保存当前窗供报告。
            evidence.append(
                {
                    # 设备窗口序号从零开始。
                    "window_sequence": window_sequence,
                    # 使用英文模型类别。
                    "class": class_name,
                    # 保存设备融合置信度。
                    "probability": probability,
                }
            )
            # 低置信窗打断连续证据。
            if probability < MIN_PROBABILITY:
                # 清空候选。
                candidate = None
                # 连续数归零。
                consecutive = 0
            elif candidate == class_name:
                # 高置信同类增加一窗。
                consecutive += 1
            else:
                # 新高置信类别成为候选。
                candidate = class_name
                # 当前窗是第一份证据。
                consecutive = 1
            # 保存观察窗数。
            windows_used = local_index
            # 两窗稳定时立即锁定。
            if consecutive >= STABLE_WINDOWS:
                # 提交候选。
                locked_class = candidate
                # 停止读取未来窗口。
                break
        # 保存会话结果；未正常锁定不伪造第四窗累计兜底。
        records.append(
            {
                # 会话序号来自设备。
                "session_seq": session_seq,
                # 人工真值由用户声明。
                "true_class": true_class,
                # 候选锁类可为空。
                "locked_class": locked_class,
                # 正确性明确计算。
                "correct": locked_class == true_class,
                # 确认窗数反映延迟。
                "windows_used": windows_used,
                # 前四窗证据支持复核。
                "evidence": evidence,
            }
        )
    # 返回文件和会话明细。
    return {
        # 保存绝对路径便于审计。
        "csv_path": str(csv_path.resolve()),
        # 保存会话数。
        "session_count": len(records),
        # 全部会话都必须正确。
        "all_correct": all(bool(record["correct"]) for record in records),
        # 保存逐会话事实。
        "records": records,
    }


def main() -> None:
    """执行冻结六条件策略网格并可选复核真板两轮挥手。"""

    # 正式入口才加载 PyTorch 双 M0 工件工具。
    from python import evaluate_fixed_ensemble as fixed
    # 正式入口才加载冻结六扰动与特征构建器。
    from python import evaluate_validation_generalization as generalization
    # 正式入口才加载数据扫描和固定切分。
    from python import train_export as training

    # 解析全部显式输入。
    args = parse_args()
    # 读取基础验证报告，恢复唯一类别、seed 和 val 文件身份。
    base_report = json.loads(
        (args.base_artifact_dir / "validation_report.json").read_text(encoding="utf-8")
    )
    # 读取掩码模型报告用于核对角色。
    masked_report = json.loads(
        (args.masked_artifact_dir / "validation_report.json").read_text(encoding="utf-8")
    )
    # experiment 固定窗口、步长和活动过滤阈值。
    experiment = dict(base_report["all_experiments"][0])
    # class_names 固定 11 类输出顺序。
    class_names = list(base_report["class_names"])
    # 两模型类别顺序必须一致。
    if class_names != list(masked_report["class_names"]):
        # 防止 logits 索引错配。
        raise ValueError("Base and masked class order differs")
    # 两模型验证文件身份和顺序必须一致。
    if list(experiment["val_files"]) != list(
        masked_report["all_experiments"][0]["val_files"]
    ):
        # 禁止融合不同切分模型。
        raise ValueError("Base and masked validation roles differ")

    # 扫描用户批准的 11 类基础数据集。
    records, scanned_names, label_to_index = training.scan_dataset(args.dataset_dir)
    # 数据类别必须与模型完全一致。
    if scanned_names != class_names:
        # 类别漂移时停止。
        raise ValueError("Dataset class order differs from artifacts")
    # 扫描额外训练角色，仅用于恢复原 split。
    extra_records = training.scan_labeled_dataset(args.extra_train_dir, label_to_index)
    # 用冻结 seed 恢复 train/val/test 文件角色。
    _, val_records, _ = training.split_records_for_experiment(
        records,
        extra_records,
        int(base_report.get("seed", training.SEED)),
    )
    # 把当前路径规范化为小写绝对路径。
    actual_val_paths = [str(record.path.resolve()).lower() for record in val_records]
    # 把报告路径按同一规则规范化。
    expected_val_paths = [
        str(Path(value).resolve()).lower() for value in experiment["val_files"]
    ]
    # 冻结验证身份必须逐项一致。
    if actual_val_paths != expected_val_paths:
        # 不自动搜索同名文件替代。
        raise ValueError("Frozen validation role no longer matches dataset split")

    # 加载 Round29 基础 M0。
    base_artifact = fixed.load_artifact(args.base_artifact_dir, len(class_names))
    # 加载 Round37 掩码 M0。
    masked_artifact = fixed.load_artifact(args.masked_artifact_dir, len(class_names))
    # transforms 保存六种物理条件的两策略指标。
    transforms: Dict[str, Dict[str, object]] = {}
    # 固定遍历 clean、旋转、幅度和速度条件。
    for transform_name in generalization.TRANSFORM_NAMES:
        # 构建 `[窗口数,297]` 特征、标签和文件编号。
        raw_x, labels, file_ids, sample_stats = (
            generalization.build_transformed_validation_features(
                val_records,
                experiment,
                transform_name,
            )
        )
        # 执行当前双 M0 固定融合。
        scores = fused_logits(raw_x, base_artifact, masked_artifact)
        # policies 保存本条件两个预声明策略。
        policies = {
            # 逐策略在完全相同 logits 上计算。
            policy_name: policy_metrics(
                scores,
                labels,
                file_ids,
                class_names,
                policy_name,
            )
            # 按稳定顺序遍历。
            for policy_name in POLICY_NAMES
        }
        # 保存样本规模和策略指标。
        transforms[transform_name] = {
            # 窗口和文件规模来自冻结构建器。
            "sample_stats": sample_stats,
            # 两策略详细结果。
            "policies": policies,
        }
        # 输出短进度，长特征计算期间保持可见。
        print(
            "LOCK_POLICY "
            f"transform={transform_name} "
            f"baseline={policies[POLICY_CUMULATIVE_STABLE]['accuracy']:.6f} "
            f"candidate={policies[POLICY_WINDOW_STABLE]['accuracy']:.6f} "
            f"candidate_windows={policies[POLICY_WINDOW_STABLE]['mean_windows_used']:.3f}",
            flush=True,
        )

    # report 固化全部来源和结果。
    report: Dict[str, object] = {
        # schema_version 便于未来拒绝不兼容报告。
        "schema_version": 1,
        # 明确只加载冻结验证，不加载测试角色。
        "evaluation_role": "frozen_validation_only_test_not_loaded",
        # 保存数据根。
        "dataset_dir": str(args.dataset_dir.resolve()),
        # 保存两模型来源。
        "base_artifact_dir": str(args.base_artifact_dir.resolve()),
        "masked_artifact_dir": str(args.masked_artifact_dir.resolve()),
        # 保存固定类别顺序。
        "class_names": class_names,
        # 保存策略常量。
        "policy_contract": {
            # 连续窗固定为二。
            "stable_windows": STABLE_WINDOWS,
            # 概率门固定为 0.50。
            "minimum_probability": MIN_PROBABILITY,
            # 最多四窗。
            "maximum_windows": MAX_WINDOWS,
        },
        # 保存六条件结果。
        "transforms": transforms,
    }
    # 提供真板日志时复核外部会话。
    if args.true_board_csv is not None:
        # 解析人工真值。
        truth = parse_true_board_truth(args.true_board_truth)
        # 真值不能为空，防止无标签日志输出伪准确率。
        if not truth:
            # 拒绝空外部评估。
            raise ValueError("True-board CSV requires at least one truth mapping")
        # 保存候选策略的设备日志复现。
        report["true_board"] = evaluate_logged_sessions(args.true_board_csv, truth)

    # 创建项目本地输出目录。
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # 以 UTF-8 缩进 JSON 写盘。
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    # 输出完成标记。
    print(f"LOCK_POLICY_GRID_OK output={args.output.resolve()}", flush=True)


# 仅直接执行时读取数据和模型；测试导入无外部副作用。
if __name__ == "__main__":
    # 执行冻结审计。
    main()
