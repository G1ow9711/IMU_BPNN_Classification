from __future__ import annotations

"""只在冻结验证文件上审计双 M0 对右腕角度、动作幅度和速度差异的泛化。"""

# argparse 解析数据、工件和输出路径，禁止脚本绑定当前电脑的隐式目录。
import argparse
# json 读取冻结验证角色并写出可审计指标。
import json
# math 把固定旋转角从度转换为弧度。
import math
# Path 统一处理 Windows 路径和输出父目录。
from pathlib import Path
# sys 允许直接运行本文件时导入当前仓库的 python 包。
import sys
# 类型标注明确文件集合、指标和变换映射的输入输出。
from typing import Dict, List, Sequence, Tuple

# NumPy 保存 `[窗口数,297]` 特征、标签和确定性统计。
import numpy as np

# 当前文件上一级的上一级是仓库根目录。
PROJECT_ROOT = Path(__file__).resolve().parents[1]
# 直接脚本运行时把仓库根加入模块搜索路径，优先使用当前工作树代码。
if str(PROJECT_ROOT) not in sys.path:
    # 插入首位，避免加载其它工作树的同名包。
    sys.path.insert(0, str(PROJECT_ROOT))

# 复用训练端文件划分、清洗、297维特征及物理增强函数。
from python import train_export as training
# 复用固定双 M0 加载、前向、0.85/0.15 融合、累计决策和指标函数。
from python import evaluate_fixed_ensemble as fixed

# 变换名称固定，输出顺序稳定，不能通过命令行搜索参数。
TRANSFORM_NAMES: Tuple[str, ...] = (
    # 原始冻结验证窗口。
    "clean",
    # 同一右腕的固定复合佩戴旋转。
    "rotated",
    # 动态幅度缩小到 70%。
    "amplitude_0_70",
    # 动态幅度放大到 130%。
    "amplitude_1_30",
    # 动作历时缩短到 80%。
    "duration_0_80",
    # 动作历时延长到 125%。
    "duration_1_25",
)

# 端侧正常锁定需要累计 Top-1 连续两个重叠窗一致。
PRODUCTION_LOCK_STABLE_WINDOWS = 2
# 端侧累计平均 logits 的最低 softmax 概率固定为 50%。
PRODUCTION_LOCK_MIN_PROBABILITY = 0.50
# 端侧准备态当前最多观察四窗，第四窗按累计 argmax 有界结束。
PRODUCTION_LOCK_MAX_WINDOWS = 4


def parse_args() -> argparse.Namespace:
    """解析冻结验证审计所需路径；不提供测试集或阈值搜索参数。"""
    # parser 明确本工具只做验证集泛化审计。
    parser = argparse.ArgumentParser(description="冻结验证集双 M0 个体差异泛化审计")
    # dataset-dir 必须指向用户确认的 11 类基础数据集。
    parser.add_argument("--dataset-dir", type=Path, required=True)
    # extra-train-dir 仅用于复现原 140/28/29 文件角色，不进入验证窗口。
    parser.add_argument("--extra-train-dir", type=Path, required=True)
    # base-artifact-dir 保存未掩码六分支 M0、scaler 和 validation_report。
    parser.add_argument("--base-artifact-dir", type=Path, required=True)
    # masked-artifact-dir 保存 184:232 标准分归零的第二六分支 M0。
    parser.add_argument("--masked-artifact-dir", type=Path, required=True)
    # output 保存全部变换和选择摘要 JSON。
    parser.add_argument("--output", type=Path, required=True)
    # 返回已验证的命令行命名空间。
    return parser.parse_args()


def transform_window(window: np.ndarray, transform_name: str) -> np.ndarray:
    """对单个 `[N,6]` 窗口执行固定物理变换，保持六轴单位和点数。"""
    # data 转成独立 float32 副本，防止审计修改原验证窗口。
    data = np.asarray(window, dtype=np.float32).copy()
    # clean 不改变任何数值。
    if transform_name == "clean":
        # 返回原窗口副本。
        return data
    # rotated 使用与计数压力审计相同的 30°/-25°/20°复合右手旋转。
    if transform_name == "rotated":
        # matrix 是 `Rz·Ry·Rx` 正交矩阵，不包含左右手镜像。
        matrix = training.euler_rotation_matrix(
            math.radians(30.0),
            math.radians(-25.0),
            math.radians(20.0),
        )
        # 陀螺和加速度使用同一坐标旋转。
        return training.rotate_imu_window(data, matrix)
    # 幅度缩小保持平均重力，只改变动态分量。
    if transform_name == "amplitude_0_70":
        # 返回 70% 动态幅度。
        return training.scale_imu_dynamic_amplitude(data, 0.70)
    # 幅度放大保持平均重力，只改变动态分量。
    if transform_name == "amplitude_1_30":
        # 返回 130% 动态幅度。
        return training.scale_imu_dynamic_amplitude(data, 1.30)
    # 较快动作在固定窗口内把历时缩短到 80%。
    if transform_name == "duration_0_80":
        # 返回点数不变的速度扰动窗口。
        return training.resample_imu_duration_fixed_window(data, 0.80)
    # 较慢动作在固定窗口内把历时延长到 125%。
    if transform_name == "duration_1_25":
        # 返回点数不变的速度扰动窗口。
        return training.resample_imu_duration_fixed_window(data, 1.25)
    # 未知变换表示审计代码或调用方合同漂移。
    raise ValueError(f"Unknown validation transform: {transform_name}")


def selected_windows_for_record(
    record: training.ImuRecord,
    window_len: int,
    step_len: int,
    rest_threshold: float,
    active_point_threshold: float,
) -> List[np.ndarray]:
    """按训练端原始窗口过滤规则返回一个验证文件的干净窗口。"""
    # data 形状为 `[原始点数,6]`，单位已转为 deg/s 与 g。
    data = training.load_imu_file(record.path)
    # 非静坐记录只保留动作段及固定上下文；静坐保持原样。
    data = training.trim_record_to_motion_segment(
        data,
        record.label,
        active_point_threshold,
    )
    # 文件短于一个模型窗口时不能产生验证样本。
    if len(data) < window_len:
        # 返回空列表，与训练端 too_short 行为一致。
        return []
    # candidates 保存通过原始干净窗口过滤的时间有序窗口。
    candidates = [
        # copy 隔离后续变换，禁止 np 视图共享覆盖原记录。
        np.asarray(window, dtype=np.float32).copy()
        # iter_windows 按固定步长输出 `[window_len,6]`。
        for window in training.iter_windows(data, window_len, step_len)
        # 过滤决定只基于干净窗口，所有压力变换使用完全相同样本角色。
        if training.keep_window_for_label(
            window,
            record.label,
            rest_threshold,
            active_point_threshold,
        )
    ]
    # 至少一个窗口通过时直接返回，保持原时间顺序。
    if candidates:
        # 返回全部合法窗口。
        return candidates
    # 高动态类没有合法活动窗时训练端直接跳过，禁止从静止噪声挑“最佳”窗口。
    if record.label in training.HIGH_DYNAMIC_CLASSES:
        # 返回空列表。
        return []
    # fallback_windows 保存清洗后所有固定窗口，供低动态类复现训练端回退。
    fallback_windows = list(training.iter_windows(data, window_len, step_len))
    # 理论上长度检查后应至少有一窗；防御性处理空集合。
    if not fallback_windows:
        # 返回空列表。
        return []
    # scored 使用训练端相同预处理与运动分数，避免另建验证口径。
    scored = [
        # 每项保存运动分数和窗口引用。
        (training.motion_score(training.preprocess_imu_window(window)), window)
        # 遍历全部回退窗口。
        for window in fallback_windows
    ]
    # 静坐选择最低运动窗口，其它低动态类选择最高运动窗口。
    _, best_window = (
        min(scored, key=lambda item: item[0])
        if record.label == training.SIT_CLASS_NAME
        else max(scored, key=lambda item: item[0])
    )
    # 返回唯一回退窗口副本。
    return [np.asarray(best_window, dtype=np.float32).copy()]


def build_transformed_validation_features(
    records: Sequence[training.ImuRecord],
    experiment: Dict[str, object],
    transform_name: str,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray, Dict[str, int]]:
    """构建固定变换下的 `[窗口数,297]` 特征、标签和文件编号。"""
    # features 按文件、时间顺序收集 297 维 float32 特征。
    features: List[np.ndarray] = []
    # labels 保存每窗真实类别索引。
    labels: List[int] = []
    # file_ids 保存每窗所属冻结验证文件编号，用于动作段累计重置。
    file_ids: List[int] = []
    # skipped_files 统计没有合法窗口的文件数。
    skipped_files = 0
    # 读取冻结窗口与过滤参数。
    window_len = int(experiment["window_len"])
    # step_len 是相邻模型窗起点间隔，单位点。
    step_len = int(experiment["step_len"])
    # rest_threshold 是训练集冻结的窗口运动分数门。
    rest_threshold = float(experiment["rest_threshold"])
    # active_point_threshold 是训练集冻结的逐点活动门。
    active_point_threshold = float(experiment["active_point_threshold"])
    # 逐文件构建窗口；records 已按冻结报告顺序排列。
    for file_id, record in enumerate(records):
        # selected 只由干净原窗口决定，压力变换不得改变样本选择。
        selected = selected_windows_for_record(
            record,
            window_len,
            step_len,
            rest_threshold,
            active_point_threshold,
        )
        # 空文件只计入统计，不伪造标签窗口。
        if not selected:
            # 记录一个无窗口文件。
            skipped_files += 1
            # 继续下一个文件。
            continue
        # 逐时间窗执行固定物理变换并提取生产同源特征。
        for window in selected:
            # transformed 保持 `[window_len,6]` 与 deg/s、g 单位。
            transformed = transform_window(window, transform_name)
            # 提取 297 维特征，顺序与 ESP32 头固定一致。
            features.append(training.extract_features(transformed))
            # 保存真实标签索引。
            labels.append(record.label_idx)
            # 保存动作段文件边界。
            file_ids.append(file_id)
    # 完全没有窗口表示数据、路径或过滤合同损坏。
    if not features:
        # 拒绝产生空指标。
        raise ValueError("Frozen validation records produced no windows")
    # 返回堆叠特征、标签、文件编号及规模统计。
    return (
        # 特征形状 `[窗口数,297]`。
        np.vstack(features).astype(np.float32),
        # 标签形状 `[窗口数]`。
        np.asarray(labels, dtype=np.int64),
        # 文件编号形状 `[窗口数]`。
        np.asarray(file_ids, dtype=np.int64),
        # 统计不包含任何测试文件。
        {
            "file_count": len(records),
            "window_count": len(labels),
            "skipped_file_count": skipped_files,
        },
    )


def evaluate_transform(
    raw_features: np.ndarray,
    labels: np.ndarray,
    file_ids: np.ndarray,
    base_artifact: Tuple[object, np.ndarray, np.ndarray, bool],
    masked_artifact: Tuple[object, np.ndarray, np.ndarray, bool],
    class_names: Sequence[str],
) -> Dict[str, object]:
    """执行两个冻结 M0、固定融合及每文件因果累计。"""
    # 解包基础模型、训练均值、标准差和掩码标志。
    base_model, base_mean, base_std, base_suppress = base_artifact
    # 解包第二模型及其独立标准化参数。
    masked_model, masked_mean, masked_std, masked_suppress = masked_artifact
    # 工件角色必须为未掩码基础 + 掩码第二模型。
    if base_suppress or not masked_suppress:
        # 目录交换或标志损坏会改变输入语义，立即停止。
        raise ValueError("Expected unmasked base and masked secondary artifacts")
    # 基础特征按自身训练统计标准化。
    base_x = ((raw_features - base_mean) / base_std).astype(np.float32)
    # 第二特征按自身训练统计标准化。
    masked_x = ((raw_features - masked_mean) / masked_std).astype(np.float32)
    # 第二模型把 184:232 固定为训练均值零标准分。
    masked_x = training.apply_model_feature_mask(masked_x, True)
    # 基础 M0 输出无量纲 logits，形状 `[窗口数,11]`。
    base_logits = fixed.batched_logits(base_model, base_x)
    # 掩码 M0 输出相同类别顺序 logits。
    masked_logits = fixed.batched_logits(masked_model, masked_x)
    # 固定 0.85/0.15 融合，不允许本脚本搜索权重。
    combined = training.combine_ensemble_logits(base_logits, masked_logits)
    # 单窗预测只用于诊断。
    single_predictions = np.argmax(combined, axis=1)
    # 会话预测按冻结验证文件重置并因果累计历史 logits。
    bout_predictions = fixed.cumulative_bout_predictions(combined, file_ids)
    # 逐文件模拟固件正常两窗锁定和第四窗兜底，直接衡量首次主动作选择。
    production_lock = production_lock_metrics(
        combined,
        labels,
        file_ids,
        class_names,
    )
    # 返回三种决策指标。
    return {
        # 单窗指标衡量瞬时模型稳定性。
        "single_window": fixed.classification_metrics(labels, single_predictions, class_names),
        # 累计指标对应固件首次主动作确认所需的动作段证据。
        "cumulative_bout": fixed.classification_metrics(labels, bout_predictions, class_names),
        # production_lock 按文件输出端侧最终锁类、强制锁定和确认窗数。
        "production_lock": production_lock,
    }


def stable_softmax_vector(logits: np.ndarray) -> np.ndarray:
    """对一维有限 logits 计算减最大值 softmax，输出 `[类别数]` 概率。"""
    # values 强制转 float64，降低累计平均后指数求和的舍入误差。
    values = np.asarray(logits, dtype=np.float64)
    # 只接受一维非空类别向量。
    if values.ndim != 1 or values.size == 0:
        # 形状漂移不能静默广播。
        raise ValueError("Production lock logits must be a non-empty vector")
    # 非有限 logits 在固件中会被拒绝，验证器同样立即失败。
    if not np.all(np.isfinite(values)):
        # 防止 NaN argmax 产生伪锁类。
        raise ValueError("Production lock logits must be finite")
    # shifted 通过减最大值保证 exp 不上溢。
    shifted = values - float(np.max(values))
    # exponentials 形状 `[类别数]`，元素范围为 `(0,1]`。
    exponentials = np.exp(shifted)
    # denominator 理论为有限正数。
    denominator = float(np.sum(exponentials))
    # 防御指数和异常，保持与固件有限值合同一致。
    if not math.isfinite(denominator) or denominator <= 0.0:
        # 无法形成合法概率时拒绝评估。
        raise ValueError("Production lock softmax denominator is invalid")
    # 返回总和约为 1 的 float64 概率向量。
    return exponentials / denominator


def production_lock_metrics(
    logits: np.ndarray,
    labels: np.ndarray,
    file_ids: np.ndarray,
    class_names: Sequence[str],
) -> Dict[str, object]:
    """按文件复现连续两窗、50% 和第四窗兜底的生产锁类策略。"""
    # scores 形状必须为 `[窗口数,类别数]`。
    scores = np.asarray(logits, dtype=np.float64)
    # truth 保存每窗真实类别索引。
    truth = np.asarray(labels, dtype=np.int64)
    # groups 保存每窗冻结验证文件编号。
    groups = np.asarray(file_ids, dtype=np.int64)
    # 三个输入首维必须相同，类别宽度必须匹配 class_names。
    if (
        scores.ndim != 2
        or truth.ndim != 1
        or groups.ndim != 1
        or len(scores) != len(truth)
        or len(scores) != len(groups)
        or scores.shape[1] != len(class_names)
    ):
        # 拒绝可能把不同文件或标签错位的输入。
        raise ValueError("Production lock input shapes differ")
    # 空验证集无法形成文件级指标。
    if len(scores) == 0:
        # 明确拒绝而不是除零。
        raise ValueError("Production lock requires at least one window")
    # 非有限模型输出不能进入端侧锁类。
    if not np.all(np.isfinite(scores)):
        # 与固件非有限 logits 拒绝合同一致。
        raise ValueError("Production lock logits must be finite")
    # records 保存每个验证文件的一条可审计锁定事实。
    records: List[Dict[str, object]] = []
    # correct_by_class 统计每类锁对文件数。
    correct_by_class = np.zeros(len(class_names), dtype=np.int64)
    # total_by_class 统计每类验证文件数。
    total_by_class = np.zeros(len(class_names), dtype=np.int64)
    # 按首次出现顺序遍历文件编号，保持报告与冻结角色一致。
    ordered_file_ids = list(dict.fromkeys(int(value) for value in groups.tolist()))
    # 每个文件独立清零累计器，禁止跨会话证据泄漏。
    for file_id in ordered_file_ids:
        # indices 保持该文件原时间窗顺序。
        indices = np.flatnonzero(groups == file_id)
        # 文件至少应有一个通过过滤的窗口。
        if len(indices) == 0:
            # 防御性跳过理论不可达空组。
            continue
        # 文件内标签必须唯一，否则数据角色损坏。
        file_labels = np.unique(truth[indices])
        # 一个动作文件只能有一个人工类别。
        if len(file_labels) != 1:
            # 拒绝把混合标签伪装成一次训练会话。
            raise ValueError(f"Validation file {file_id} has mixed labels")
        # true_class 是该文件真实动作索引。
        true_class = int(file_labels[0])
        # 标签必须落在固定类别范围。
        if true_class < 0 or true_class >= len(class_names):
            # 防止负索引或越界污染逐类召回。
            raise ValueError(f"Validation file {file_id} label is out of range")
        # 当前类别文件数增加一。
        total_by_class[true_class] += 1
        # cumulative 保存最多四窗的 11 类 logits 和。
        cumulative = np.zeros(scores.shape[1], dtype=np.float64)
        # candidate 保存上一累计 Top-1；None 表示尚无窗口。
        candidate: int | None = None
        # consecutive 保存累计 Top-1 连续保持窗数。
        consecutive = 0
        # locked_class 为 None 表示文件结束前未达到正常或兜底锁定。
        locked_class: int | None = None
        # lock_probability 保存锁定时累计 softmax 概率。
        lock_probability = 0.0
        # windows_used 保存最终观察窗数。
        windows_used = 0
        # forced 表示仅因第四窗上限锁定，而非正常稳定高置信门。
        forced = False
        # 最多读取生产合同规定的前四个窗口。
        for local_index, global_index in enumerate(
            indices[:PRODUCTION_LOCK_MAX_WINDOWS],
            start=1,
        ):
            # 当前融合 logits 加入动作段证据。
            cumulative += scores[int(global_index)]
            # 平均 logits 用于与固件相同的 softmax 置信度。
            averaged = cumulative / float(local_index)
            # NumPy argmax 与生成 C 的低索引平局规则一致。
            best_class = int(np.argmax(averaged))
            # 累计最优类保持时增加连续证据，否则从一窗重置。
            if candidate == best_class:
                # 同类连续窗增加一。
                consecutive += 1
            else:
                # 保存新累计候选。
                candidate = best_class
                # 当前窗是新候选第一窗。
                consecutive = 1
            # 计算当前累计最优类概率。
            probability = float(stable_softmax_vector(averaged)[best_class])
            # 正常门要求连续两窗且概率至少 50%。
            stable_and_confident = (
                consecutive >= PRODUCTION_LOCK_STABLE_WINDOWS
                and probability >= PRODUCTION_LOCK_MIN_PROBABILITY
            )
            # 第四窗是生产有界兜底。
            reached_limit = local_index >= PRODUCTION_LOCK_MAX_WINDOWS
            # 保存当前已观察窗数，未锁定短文件也能报告。
            windows_used = local_index
            # 两种门均未满足时继续下一窗。
            if not stable_and_confident and not reached_limit:
                # 继续累计。
                continue
            # 保存锁定类别。
            locked_class = best_class
            # 保存锁定概率。
            lock_probability = probability
            # 只有未过正常门但达到上限时标记强制锁定。
            forced = reached_limit and not stable_and_confident
            # 生产锁定后本轮不再换类，停止读取后续窗口。
            break
        # correct 表示锁定存在且与文件人工类别一致。
        correct = locked_class == true_class
        # 正确文件累加到对应类别。
        if correct:
            # 逐类正确文件加一。
            correct_by_class[true_class] += 1
        # 保存文件级审计记录。
        records.append(
            {
                # 文件编号来自冻结验证角色。
                "file_id": file_id,
                # 人工类别使用稳定字符串，便于审计。
                "true_class": class_names[true_class],
                # 未锁定时输出 null，而非未知类别字符串。
                "locked_class": None if locked_class is None else class_names[locked_class],
                # correct 是最终会话级锁类是否正确。
                "correct": bool(correct),
                # forced 区分低置信第四窗兜底。
                "forced": bool(forced),
                # windows_used 范围为 1..4。
                "windows_used": int(windows_used),
                # 未锁定时概率为 0。
                "lock_probability": float(lock_probability),
            }
        )
    # file_count 是实际产生窗口的验证文件数。
    file_count = len(records)
    # 理论上至少一个文件；空集合说明文件编号输入损坏。
    if file_count == 0:
        # 防止后续除零。
        raise ValueError("Production lock produced no file records")
    # correct_file_count 汇总正确锁定文件。
    correct_file_count = sum(1 for record in records if bool(record["correct"]))
    # locked_file_count 排除短文件未锁定。
    locked_file_count = sum(1 for record in records if record["locked_class"] is not None)
    # forced_lock_count 汇总第四窗低置信兜底。
    forced_lock_count = sum(1 for record in records if bool(record["forced"]))
    # class_recalls 保存文件级逐类召回；冻结验证每类都必须存在。
    class_recalls = {
        # 类别缺文件时返回 0，真实 11 类冻结集不会触发该边界。
        class_names[index]: (
            float(correct_by_class[index] / total_by_class[index])
            if total_by_class[index] > 0
            else 0.0
        )
        # 遍历固定模型类别顺序。
        for index in range(len(class_names))
    }
    # 返回端侧锁类文件级指标和明细。
    return {
        # 有窗口的文件总数。
        "file_count": file_count,
        # 实际锁定文件数。
        "locked_file_count": locked_file_count,
        # 短文件未锁定数。
        "unlocked_file_count": file_count - locked_file_count,
        # 正确锁定文件数。
        "correct_file_count": correct_file_count,
        # 未锁定按错误计入准确率。
        "accuracy": float(correct_file_count / file_count),
        # 低置信第四窗强制锁定数。
        "forced_lock_count": forced_lock_count,
        # 平均确认窗数反映额外实时延迟。
        "mean_windows_used": float(
            np.mean([int(record["windows_used"]) for record in records])
        ),
        # 每类文件级召回。
        "class_recalls": class_recalls,
        # 文件级明细支持追溯弱类。
        "records": records,
    }


def main() -> None:
    """验证工件角色后生成六种确定性条件的双 M0 泛化报告。"""
    # args 保存全部显式输入输出路径。
    args = parse_args()
    # base_report 冻结类别、窗口参数和 val_files，不读取 test_files 内容。
    base_report = json.loads(
        (args.base_artifact_dir / "validation_report.json").read_text(encoding="utf-8")
    )
    # masked_report 必须声明同一验证角色。
    masked_report = json.loads(
        (args.masked_artifact_dir / "validation_report.json").read_text(encoding="utf-8")
    )
    # 两个候选均只允许一个正式 2.5 秒实验。
    experiment = dict(base_report["all_experiments"][0])
    # class_names 固定为模型输出 11 类顺序。
    class_names = list(base_report["class_names"])
    # 两模型类别顺序必须逐项一致。
    if class_names != list(masked_report["class_names"]):
        # 类别漂移会导致 logits 错位。
        raise ValueError("Base and masked class order differs")
    # 两报告的冻结验证路径集合必须一致。
    base_val_paths = [str(Path(value).resolve()).lower() for value in experiment["val_files"]]
    # masked_experiment 只用于核对验证角色，不覆盖基础阈值。
    masked_experiment = masked_report["all_experiments"][0]
    # 规范化第二模型验证路径。
    masked_val_paths = [
        str(Path(value).resolve()).lower() for value in masked_experiment["val_files"]
    ]
    # 路径顺序和集合都必须相同，保证逐窗 A/B 对齐。
    if base_val_paths != masked_val_paths:
        # 拒绝不同切分模型融合。
        raise ValueError("Base and masked validation roles differ")
    # 扫描用户指定基础数据集并恢复类别映射。
    records, scanned_names, label_to_index = training.scan_dataset(args.dataset_dir)
    # 扫描类别顺序必须与工件一致。
    if scanned_names != class_names:
        # 数据目录或类别合同漂移。
        raise ValueError("Dataset class order differs from artifacts")
    # 额外8文件只用于复现训练角色，不会出现在返回 val_records。
    extra_records = training.scan_labeled_dataset(args.extra_train_dir, label_to_index)
    # 固定 seed 恢复 train/val/test 角色；仅保留 val_records。
    _, val_records, _ = training.split_records_for_experiment(
        records,
        extra_records,
        int(base_report.get("seed", training.SEED)),
    )
    # 当前数据树恢复的验证路径必须逐项匹配训练报告。
    actual_val_paths = [str(record.path.resolve()).lower() for record in val_records]
    # 顺序不同会改变文件编号和累计边界，必须停止。
    if actual_val_paths != base_val_paths:
        # 不允许自动搜索同名替代文件。
        raise ValueError("Frozen validation role no longer matches dataset split")
    # 加载未掩码基础 M0。
    base_artifact = fixed.load_artifact(args.base_artifact_dir, len(class_names))
    # 加载掩码第二 M0。
    masked_artifact = fixed.load_artifact(args.masked_artifact_dir, len(class_names))
    # results 保存每种固定物理扰动指标。
    results: Dict[str, Dict[str, object]] = {}
    # 按固定顺序遍历，禁止根据中间结果增删扰动。
    for transform_name in TRANSFORM_NAMES:
        # 构建当前变换的验证特征；样本角色只由 clean 窗口决定。
        raw_x, labels, file_ids, sample_stats = build_transformed_validation_features(
            val_records,
            experiment,
            transform_name,
        )
        # 执行固定双 M0 和因果累计。
        metrics = evaluate_transform(
            raw_x,
            labels,
            file_ids,
            base_artifact,
            masked_artifact,
            class_names,
        )
        # 合并规模统计与分类指标。
        results[transform_name] = {"sample_stats": sample_stats, **metrics}
        # 输出当前进度，长时间特征计算期间保持可见。
        print(
            f"VALIDATION_GENERALIZATION transform={transform_name} "
            f"windows={sample_stats['window_count']} "
            f"bout_acc={metrics['cumulative_bout']['accuracy']:.6f} "
            f"lock_acc={metrics['production_lock']['accuracy']:.6f} "
            f"forced={metrics['production_lock']['forced_lock_count']}",
            flush=True,
        )
    # stress_names 排除 clean，只汇总个体差异压力条件。
    stress_names = [name for name in TRANSFORM_NAMES if name != "clean"]
    # stress_accuracies 收集动作段累计准确率。
    stress_accuracies = [
        float(results[name]["cumulative_bout"]["accuracy"]) for name in stress_names
    ]
    # stress_min_recalls 收集每种条件的最弱类召回。
    stress_min_recalls = [
        min(float(value) for value in results[name]["cumulative_bout"]["class_recalls"].values())
        for name in stress_names
    ]
    # stress_lock_accuracies 收集端侧文件级最终锁类准确率。
    stress_lock_accuracies = [
        float(results[name]["production_lock"]["accuracy"]) for name in stress_names
    ]
    # stress_lock_min_recalls 收集端侧文件级逐类最弱召回。
    stress_lock_min_recalls = [
        min(float(value) for value in results[name]["production_lock"]["class_recalls"].values())
        for name in stress_names
    ]
    # stress_forced_locks 汇总压力条件低置信第四窗兜底数量。
    stress_forced_locks = [
        int(results[name]["production_lock"]["forced_lock_count"]) for name in stress_names
    ]
    # report 明确来源、未读取测试集及选择摘要。
    report = {
        # schema_version 便于未来拒绝不兼容字段。
        "schema_version": 1,
        # evaluation_role 强制说明没有构建 test 窗口。
        "evaluation_role": "frozen_validation_only_test_not_loaded",
        # dataset_dir 保存用户指定数据根绝对路径。
        "dataset_dir": str(args.dataset_dir.resolve()),
        # base_artifact_dir 保存未掩码候选来源。
        "base_artifact_dir": str(args.base_artifact_dir.resolve()),
        # masked_artifact_dir 保存掩码候选来源。
        "masked_artifact_dir": str(args.masked_artifact_dir.resolve()),
        # class_names 固定模型类别顺序。
        "class_names": class_names,
        # transforms 保存全部逐条件指标。
        "transforms": results,
        # selection_summary 供 baseline/robust A/B 比较，不自动决定部署。
        "selection_summary": {
            # 原始验证动作段累计准确率。
            "clean_bout_accuracy": float(results["clean"]["cumulative_bout"]["accuracy"]),
            # 五种压力条件平均准确率。
            "stress_mean_bout_accuracy": float(np.mean(stress_accuracies)),
            # 五种压力条件最差准确率。
            "stress_worst_bout_accuracy": float(np.min(stress_accuracies)),
            # 五种压力条件中最差的最弱类召回。
            "stress_worst_min_class_recall": float(np.min(stress_min_recalls)),
            # 原始验证文件按生产策略最终锁类准确率。
            "clean_production_lock_accuracy": float(
                results["clean"]["production_lock"]["accuracy"]
            ),
            # 原始验证第四窗低置信强制锁定数量。
            "clean_production_forced_lock_count": int(
                results["clean"]["production_lock"]["forced_lock_count"]
            ),
            # 五种压力条件生产锁类平均准确率。
            "stress_mean_production_lock_accuracy": float(
                np.mean(stress_lock_accuracies)
            ),
            # 五种压力条件生产锁类最差准确率。
            "stress_worst_production_lock_accuracy": float(
                np.min(stress_lock_accuracies)
            ),
            # 五种压力条件中生产锁类最弱类别召回最差值。
            "stress_worst_production_lock_min_class_recall": float(
                np.min(stress_lock_min_recalls)
            ),
            # 五种压力条件第四窗低置信强制锁定总数。
            "stress_total_production_forced_lock_count": int(
                np.sum(stress_forced_locks)
            ),
        },
    }
    # 创建显式输出父目录；不写入数据集或模型工件目录。
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # 以 UTF-8 缩进 JSON 写出完整审计证据。
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    # 输出最终摘要供自动门读取。
    print(
        "VALIDATION_GENERALIZATION_SUMMARY "
        f"clean={report['selection_summary']['clean_bout_accuracy']:.6f} "
        f"stress_mean={report['selection_summary']['stress_mean_bout_accuracy']:.6f} "
        f"stress_worst={report['selection_summary']['stress_worst_bout_accuracy']:.6f} "
        f"stress_min_recall={report['selection_summary']['stress_worst_min_class_recall']:.6f} "
        f"clean_lock={report['selection_summary']['clean_production_lock_accuracy']:.6f} "
        f"stress_lock={report['selection_summary']['stress_mean_production_lock_accuracy']:.6f} "
        f"forced_total={report['selection_summary']['stress_total_production_forced_lock_count']}"
    )


# 仅脚本执行时开始读取数据和模型；被测试导入时无外部副作用。
if __name__ == "__main__":
    # 执行冻结验证泛化审计。
    main()
