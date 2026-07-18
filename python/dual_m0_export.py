"""把最终两套六分支 M0 验证工件导出为 ESP32-S3 可移植 C 运行时。

模型分组、标准化、掩码、融合公式和资源预算见 ``docs/算法文档.md``
第 8、9、11、12、15 节；本模块只实现文档中已经锁定的部署合同，不重新选参数。
"""

from __future__ import annotations

# hashlib 计算原始模型、便携 NPZ 和生成头文件的 SHA-256 完整性标识。
import hashlib
# argparse 提供显式工件目录和输出目录命令行，避免把本机路径写死在源码。
import argparse
# json 生成可由固件、上位机和发布流程共同读取的版本化模型清单。
import json
# tempfile 在目标输出目录内生成旧特征模板的短生命周期中间文件。
import tempfile
# dataclass 用不可变数据结构表达单模型工件和双模型组合合同。
from dataclasses import dataclass
# Path 统一处理 Windows 与其它主机上的模型、scaler 和输出路径。
from pathlib import Path
# Dict、List、Mapping 和 Sequence 描述权重表、文本行及固定顺序字符串数组。
from typing import Dict, List, Mapping, Sequence

# NumPy 实现与 C row-major 前向一致的 float32 部署参考并保存便携数组。
import numpy as np
# PyTorch 只用于加载已冻结 state_dict 和计算导出前的参考 logits。
import torch

# train_export 是 297 维特征、类别顺序、M0 结构和现有 C 特征公式的源代码真值。
import train_export as te


# NORMALIZED_PHASE_START 是第二模型需要屏蔽的标准化阶段特征起始索引，包含 184。
NORMALIZED_PHASE_START = 184
# NORMALIZED_PHASE_END 是第二模型屏蔽区间的半开末端，实际最后索引为 231。
NORMALIZED_PHASE_END = 232
# BUNDLE_FORMAT_VERSION 每次修改数组名称、顺序或 C ABI 时必须递增。
BUNDLE_FORMAT_VERSION = 1
# PRODUCTION_CLASS_NAMES 固定 11 类输出索引，必须与最终冻结报告、PC 和固件一致。
PRODUCTION_CLASS_NAMES = (
    "good_morning",
    "jumping_jack",
    "jumping_lunge",
    "jumping_squat",
    "lunge",
    "sit",
    "squat",
    "trot",
    "tuck_jump",
    "walk",
    "wave",
)
# MAIN_STATE_KEYS 按 C 数组生成顺序列出部署主路径，故意不含五个训练辅助头。
MAIN_STATE_KEYS = tuple(
    # 六个分支分别包含一层 Linear 的 row-major 权重和偏置。
    [item for branch_index in range(6) for item in (
        f"branches.{branch_index}.0.weight",
        f"branches.{branch_index}.0.bias",
    )]
    # 融合层包含 80→64、64→32 两层及最终 32→11 分类层。
    + [
        "fusion.0.weight",
        "fusion.0.bias",
        "fusion.3.weight",
        "fusion.3.bias",
        "classifier.weight",
        "classifier.bias",
    ]
)


@dataclass(frozen=True)
class M0Artifact:
    """一套六分支 M0 权重、scaler、类别和部署掩码的只读工件。"""

    # artifact_dir 指向包含 best_model.pt 与 scaler_and_config.npz 的验证输出目录。
    artifact_dir: Path
    # model_sha256 是 best_model.pt 原始字节的小写十六进制 SHA-256。
    model_sha256: str
    # model 是已加载并切到 eval 模式的 MultiBranchBPNet，仅用于参考验证。
    model: te.MultiBranchBPNet
    # state_arrays 只含部署主路径数组，键顺序与 MAIN_STATE_KEYS 完全一致。
    state_arrays: Mapping[str, np.ndarray]
    # mean 形状 [297]，表示训练角色逐特征均值，物理单位随特征变化。
    mean: np.ndarray
    # std 形状 [297]，表示训练角色逐特征标准差且每项严格大于 1e-6。
    std: np.ndarray
    # class_names 形状 [11]，顺序直接定义 C 输出索引。
    class_names: tuple[str, ...]
    # feature_names 形状 [297]，顺序直接定义 scaler 和六分支切片。
    feature_names: tuple[str, ...]
    # suppress_normalized_phase 为真时，标准化后索引 184:232 固定置零。
    suppress_normalized_phase: bool
    # window_len 是每次特征提取的采样点数，正式工件必须为 62。
    window_len: int
    # step_len 是相邻推理间新增采样点数，正式工件必须为 12。
    step_len: int
    # sample_rate_hz 是原始手腕六轴等间隔采样率，正式工件必须为 25 Hz。
    sample_rate_hz: int
    # rest_threshold 是窗口运动分数的训练角色静止门槛，无量纲。
    rest_threshold: float
    # active_point_threshold 是逐采样活动分数门槛，无量纲。
    active_point_threshold: float


@dataclass(frozen=True)
class DualM0Bundle:
    """基础 M0 与归一化阶段掩码 M0 的固定 0.85/0.15 组合。"""

    # base 使用全部 297 个标准化特征，固定融合权重为 0.85。
    base: M0Artifact
    # masked 把标准化后索引 184:232 置零，固定融合权重为 0.15。
    masked: M0Artifact


def _sha256_file(path: Path) -> str:
    """返回 path 文件全部字节的小写 SHA-256；文件不存在时由 read_bytes 抛错。"""
    # read_bytes 一次读取当前约 60 KiB 模型或数百 KiB 头文件，内存开销可控。
    payload = path.read_bytes()
    # hexdigest 返回固定 64 个小写十六进制字符，适合 JSON 和 BLE Manifest。
    return hashlib.sha256(payload).hexdigest()


def _scalar_int(values: np.ndarray, field_name: str) -> int:
    """从单元素 NumPy 字段读取整数，并拒绝空数组或多元素歧义。"""
    # array 把标量、列表和 ndarray 统一成可检查形状的 NumPy 对象。
    array = np.asarray(values)
    # size 必须恰好为一，否则同一工件可能携带互相矛盾的多个配置。
    if array.size != 1:
        # ValueError 报告字段名和实际元素数，便于定位损坏 NPZ。
        raise ValueError(f"{field_name} must contain exactly one value, got {array.size}")
    # reshape(-1)[0] 兼容标量和 [1]，最终转成 Python int。
    return int(array.reshape(-1)[0])


def _scalar_float(values: np.ndarray, field_name: str) -> float:
    """从单元素 NumPy 字段读取有限 float，防止 NaN/Inf 进入 C 常量。"""
    # array 把输入转换为 NumPy 对象并保留原始精度供有限性检查。
    array = np.asarray(values)
    # 每个门槛字段只允许一个值。
    if array.size != 1:
        # 多值配置无法映射到单个 C 常量，因此立即拒绝。
        raise ValueError(f"{field_name} must contain exactly one value, got {array.size}")
    # value 转成 Python float，后续 JSON 和 C 格式化均可直接使用。
    value = float(array.reshape(-1)[0])
    # 非有限门槛会使所有活动判断失效，禁止导出。
    if not np.isfinite(value):
        # 错误中保留字段名，便于检查原始 scaler。
        raise ValueError(f"{field_name} must be finite")
    # 返回经过有限性验证的标量。
    return value


def _load_suppress_flag(config: Mapping[str, np.ndarray], expected_mask: bool) -> bool:
    """读取掩码字段；历史基础工件缺字段时只允许按 False 兼容。"""
    # 新工件包含显式字段时读取第一个且唯一一个布尔值。
    if "suppress_normalized_phase" in config:
        # 字段必须恰好一个元素，避免不同批次掩码混入同一工件。
        if np.asarray(config["suppress_normalized_phase"]).size != 1:
            # 形状错误时禁止按任意首项继续。
            raise ValueError("suppress_normalized_phase must contain exactly one value")
        # bool 把 np.bool_ 转为稳定的 Python 布尔值。
        actual = bool(np.asarray(config["suppress_normalized_phase"]).reshape(-1)[0])
    else:
        # Round29 历史基础工件生成早于该字段，合同明确等价于未屏蔽。
        actual = False
    # 工件角色与实际掩码不一致会让两个模型收到错误输入。
    if actual != expected_mask:
        # 错误同时列出期望与实际，避免误把两个目录顺序传反。
        raise ValueError(
            f"suppress_normalized_phase mismatch: expected {expected_mask}, got {actual}"
        )
    # 返回已核对的角色掩码。
    return actual


def load_m0_artifact(artifact_dir: Path, expected_mask: bool) -> M0Artifact:
    """加载一套验证工件，并验证 297 维、11 类和六分支主路径合同。"""
    # root 规范化为 Path，但不解析符号链接，便于清单保留用户给定目录。
    root = Path(artifact_dir)
    # model_path 必须是验证保存的完整 state_dict 文件。
    model_path = root / "best_model.pt"
    # scaler_path 必须包含训练集均值、标准差、类别和特征顺序。
    scaler_path = root / "scaler_and_config.npz"
    # 两个文件缺一不可；is_file 同时拒绝同名目录。
    if not model_path.is_file() or not scaler_path.is_file():
        # 错误报告工件根，避免只看到某个拼接后的文件名。
        raise FileNotFoundError(f"M0 artifact is incomplete: {root}")
    # allow_pickle=False 禁止 NPZ 反序列化任意 Python 对象，降低供应链风险。
    # with 确保 Windows 上 NPZ 文件句柄立即关闭，导出后目录可以删除或替换。
    with np.load(scaler_path, allow_pickle=False) as archive:
        # 复制全部数组到普通字典；后续校验不再依赖已关闭的 ZipFile。
        config = {name: np.array(archive[name], copy=True) for name in archive.files}
    # model_type 必须明确为 multi_branch；缺失或其它模型不能套用六分支 C ABI。
    if "model_type" not in config or str(config["model_type"].reshape(-1)[0]) != "multi_branch":
        # 旧平铺 BP 或 M1 深窄模型需要不同前向，不能静默接受。
        raise ValueError("model_type must be multi_branch")
    # class_names 按 NPZ 固定顺序转成不可变字符串元组。
    class_names = tuple(str(value) for value in config["class_names"].tolist())
    # feature_names 按 NPZ 固定顺序转成不可变字符串元组。
    feature_names = tuple(str(value) for value in config["feature_names"].tolist())
    # 生产类别顺序必须精确一致，防止 argmax 索引映射到错误动作。
    if class_names != PRODUCTION_CLASS_NAMES:
        # 错误明确指出是 class order，而不是模糊的维度错误。
        raise ValueError("class order differs from the production contract")
    # 生产特征名称和顺序必须逐项一致，防止 scaler 与 C 公式错位。
    if feature_names != tuple(te.build_feature_names()):
        # 任何缺失、重复或重排都必须重新训练，不能在导出期修补。
        raise ValueError("feature order differs from the production contract")
    # mean 转成连续 float32 一维数组，形状应为 [297]。
    mean = np.ascontiguousarray(config["mean"], dtype=np.float32).reshape(-1)
    # std 转成连续 float32 一维数组，形状应为 [297]。
    std = np.ascontiguousarray(config["std"], dtype=np.float32).reshape(-1)
    # 两个 scaler 长度都必须与特征数完全相同。
    if mean.shape != (len(feature_names),) or std.shape != (len(feature_names),):
        # 报告实际形状，便于定位截断或错误模型工件。
        raise ValueError(f"scaler shapes differ from 297 features: {mean.shape}, {std.shape}")
    # mean 与 std 必须全部有限，NaN/Inf 会污染标准化和动作段累计。
    if not np.all(np.isfinite(mean)) or not np.all(np.isfinite(std)):
        # 禁止在导出时用零替换，因为会偏离已验证 Python 模型。
        raise ValueError("scaler values must be finite")
    # 正式训练已把近零标准差替换为一；导出器拒绝小于等于 1e-6 的残留值。
    if np.any(std <= np.float32(1e-6)):
        # 该错误必须回到训练工件修复，不能只在 C 端夹紧。
        raise ValueError("scaler standard deviations must be greater than 1e-6")
    # weights_only=True 只加载张量容器，避免执行 pickle 中任意类构造代码。
    raw_state = torch.load(model_path, map_location="cpu", weights_only=True)
    # state_dict 必须提供所有六分支、两层融合和分类层数组。
    missing_keys = [key for key in MAIN_STATE_KEYS if key not in raw_state]
    # 任何主路径数组缺失都无法执行完整 M0 前向。
    if missing_keys:
        # 错误列出全部缺失键，减少逐个修复轮次。
        raise ValueError(f"M0 state_dict misses deployment keys: {missing_keys}")
    # 建立与正式 297→11 合同相同的模型，dropout=0 只影响训练期随机层。
    model = te.MultiBranchBPNet(len(feature_names), len(class_names), dropout=0.0)
    # strict=True 要求辅助头等训练期键也与当前 Python 类完全对应，避免加载错版本。
    model.load_state_dict(raw_state, strict=True)
    # eval 关闭 dropout，确保参考 logits 和 C 前向确定。
    model.eval()
    # state_arrays 逐键转成连续 float32 NumPy，存储顺序保持 PyTorch row-major。
    state_arrays: Dict[str, np.ndarray] = {
        # detach/cpu 防止梯度或设备状态进入便携数组。
        key: np.ascontiguousarray(raw_state[key].detach().cpu().numpy(), dtype=np.float32)
        # 只遍历部署白名单，主动排除 auxiliary_heads.*。
        for key in MAIN_STATE_KEYS
    }
    # 读取并验证工件角色对应的掩码状态。
    suppress_normalized_phase = _load_suppress_flag(config, expected_mask)
    # 构造不可变工件，所有字段已完成形状、有限性和顺序校验。
    return M0Artifact(
        artifact_dir=root,
        model_sha256=_sha256_file(model_path),
        model=model,
        state_arrays=state_arrays,
        mean=mean,
        std=std,
        class_names=class_names,
        feature_names=feature_names,
        suppress_normalized_phase=suppress_normalized_phase,
        window_len=_scalar_int(config["window_len"], "window_len"),
        step_len=_scalar_int(config["step_len"], "step_len"),
        sample_rate_hz=_scalar_int(config["sample_rate"], "sample_rate"),
        rest_threshold=_scalar_float(config["rest_threshold"], "rest_threshold"),
        active_point_threshold=_scalar_float(
            config["active_point_threshold"], "active_point_threshold"
        ),
    )


def load_dual_m0_bundle(base_dir: Path, masked_dir: Path) -> DualM0Bundle:
    """加载基础和掩码 M0，并拒绝任何跨模型元数据不一致。"""
    # base 角色固定要求不屏蔽 184:232；Round29 缺字段时按 False 兼容。
    base = load_m0_artifact(Path(base_dir), expected_mask=False)
    # masked 角色固定要求屏蔽 184:232，缺失或 False 均视为错误。
    masked = load_m0_artifact(Path(masked_dir), expected_mask=True)
    # 两模型类别必须逐项同序，否则固定 0.85/0.15 融合没有业务意义。
    if base.class_names != masked.class_names:
        # 错误文字保留 class order，供自动测试精确识别。
        raise ValueError("class order differs between base and masked models")
    # 两模型特征名称和索引必须完全相同，虽然 scaler 数值允许不同。
    if base.feature_names != masked.feature_names:
        # 特征顺序不同时必须回到训练流程生成匹配工件。
        raise ValueError("feature order differs between base and masked models")
    # 采样率、窗口和步长共同定义原始输入时序合同。
    timing_base = (base.sample_rate_hz, base.window_len, base.step_len)
    # masked 时序元组用于单次精确比较。
    timing_masked = (masked.sample_rate_hz, masked.window_len, masked.step_len)
    # 不一致时不能共享同一 297 维特征窗口。
    if timing_base != timing_masked:
        # 报告两个元组帮助定位错误训练输出。
        raise ValueError(f"timing contract differs: {timing_base} vs {timing_masked}")
    # 正式模型只接受 25 Hz、62 点窗口和 12 点步长。
    if timing_base != (25, 62, 12):
        # 拒绝未重新验证的其它时序配置。
        raise ValueError(f"unsupported production timing contract: {timing_base}")
    # 活动阈值来自同一训练角色，双模型必须逐 float32 位一致。
    if base.rest_threshold != masked.rest_threshold:
        # 不在导出期取平均，避免改变已验证窗口选择。
        raise ValueError("rest threshold differs between models")
    # 逐点活动阈值也必须相同。
    if base.active_point_threshold != masked.active_point_threshold:
        # 不允许两个模型看到不同活动段边界。
        raise ValueError("active point threshold differs between models")
    # 返回经过全部跨模型合同校验的固定组合。
    return DualM0Bundle(base=base, masked=masked)


def _prepare_standardized_features(artifact: M0Artifact, raw_features: np.ndarray) -> np.ndarray:
    """按工件 scaler 标准化 [样本数,297] 原始特征并应用可选掩码。"""
    # values 统一成 float32；一维输入在后续转换为单样本批次。
    values = np.asarray(raw_features, dtype=np.float32)
    # 一维 [297] 输入扩展为 [1,297]，统一矩阵前向实现。
    if values.ndim == 1:
        # None 新增样本轴，不改变特征顺序。
        values = values[None, :]
    # 只接受二维批次，防止时间轴或类别轴被误当成特征轴。
    if values.ndim != 2 or values.shape[1] != len(artifact.feature_names):
        # 错误包含实际形状，便于调用方定位数组排列。
        raise ValueError(f"raw feature shape must be [samples,297], got {values.shape}")
    # 任一非有限原始特征都会污染模型和动作段累计，立即拒绝。
    if not np.all(np.isfinite(values)):
        # 不用零值替代，保持 Python/C 的故障策略一致。
        raise ValueError("raw features must be finite")
    # 标准化输出形状 [样本数,297]，各列变为无量纲标准分。
    standardized = ((values - artifact.mean) / artifact.std).astype(np.float32)
    # 第二模型把归一化四阶段组固定为训练均值，即标准分零。
    if artifact.suppress_normalized_phase:
        # copy 防止调用方输入或其它模型共享数组被原地修改。
        standardized = standardized.copy()
        # 半开区间 184:232 恰好覆盖 48 项归一化阶段特征。
        standardized[:, NORMALIZED_PHASE_START:NORMALIZED_PHASE_END] = 0.0
    # 返回 C 前向应接收的连续 float32 标准分。
    return np.ascontiguousarray(standardized, dtype=np.float32)


def numpy_m0_logits(artifact: M0Artifact, raw_features: np.ndarray) -> np.ndarray:
    """用 NumPy row-major 矩阵运算复现单个六分支 M0 的 [样本数,11] logits。"""
    # standardized 形状 [样本数,297]，已应用该模型自己的 scaler 和掩码。
    standardized = _prepare_standardized_features(artifact, raw_features)
    # branch_outputs 收集六个 [样本数,分支输出维度] ReLU 表示。
    branch_outputs: List[np.ndarray] = []
    # offset 指向当前物理特征组在 297 维向量中的起始列。
    offset = 0
    # 六个输入和输出维度来自正式 MultiBranchBPNet 类常量。
    for branch_index, input_dim in enumerate(te.MultiBranchBPNet.group_input_dims):
        # weight 形状 [分支输出维度,分支输入维度]，按 PyTorch row-major 保存。
        weight = artifact.state_arrays[f"branches.{branch_index}.0.weight"]
        # bias 形状 [分支输出维度]，由 NumPy 沿样本轴广播。
        bias = artifact.state_arrays[f"branches.{branch_index}.0.bias"]
        # group 形状 [样本数,分支输入维度]，保持生产特征连续顺序。
        group = standardized[:, offset : offset + input_dim]
        # linear 计算 group·W^T+b；maximum 实现 ReLU 且保持 float32。
        encoded = np.maximum(group @ weight.T + bias, np.float32(0.0)).astype(np.float32)
        # 保存当前分支输出，稍后按 0..5 顺序拼接为 80 维。
        branch_outputs.append(encoded)
        # offset 移到下一物理组起始列。
        offset += input_dim
    # concatenated 形状 [样本数,80]，顺序与 PyTorch torch.cat 完全一致。
    concatenated = np.concatenate(branch_outputs, axis=1).astype(np.float32)
    # fusion1_weight 形状 [64,80]。
    fusion1_weight = artifact.state_arrays["fusion.0.weight"]
    # fusion1_bias 形状 [64]。
    fusion1_bias = artifact.state_arrays["fusion.0.bias"]
    # hidden64 形状 [样本数,64]，执行第一融合 Linear-ReLU。
    hidden64 = np.maximum(
        concatenated @ fusion1_weight.T + fusion1_bias, np.float32(0.0)
    ).astype(np.float32)
    # fusion2_weight 形状 [32,64]。
    fusion2_weight = artifact.state_arrays["fusion.3.weight"]
    # fusion2_bias 形状 [32]。
    fusion2_bias = artifact.state_arrays["fusion.3.bias"]
    # hidden32 形状 [样本数,32]，是训练辅助头和主分类头共享的嵌入。
    hidden32 = np.maximum(
        hidden64 @ fusion2_weight.T + fusion2_bias, np.float32(0.0)
    ).astype(np.float32)
    # classifier_weight 形状 [11,32]，输出不应用 ReLU 或 softmax。
    classifier_weight = artifact.state_arrays["classifier.weight"]
    # classifier_bias 形状 [11]。
    classifier_bias = artifact.state_arrays["classifier.bias"]
    # logits 形状 [样本数,11]，元素为无量纲 softmax 前类别分数。
    logits = hidden32 @ classifier_weight.T + classifier_bias
    # 返回连续 float32，作为 C 逐值对照参考。
    return np.ascontiguousarray(logits, dtype=np.float32)


def torch_m0_logits(artifact: M0Artifact, raw_features: np.ndarray) -> np.ndarray:
    """用冻结 PyTorch M0 计算参考 logits，输入前处理与部署路径完全相同。"""
    # standardized 形状 [样本数,297]，使用该模型独立 scaler 和掩码。
    standardized = _prepare_standardized_features(artifact, raw_features)
    # inference_mode 禁用梯度记录，降低内存并防止意外修改模型参数。
    with torch.inference_mode():
        # from_numpy 共享只读测试数组内存，模型输出形状 [样本数,11]。
        output = artifact.model(torch.from_numpy(standardized))
    # detach/cpu/numpy 把结果转成连续 float32 部署参考。
    return np.ascontiguousarray(output.detach().cpu().numpy(), dtype=np.float32)


def _c_array(name: str, values: np.ndarray, description: str) -> str:
    """把任意 float32 数组扁平化为带中文维度说明的 C static const 一维数组。"""
    # array 以 C row-major 顺序扁平化，线性层使用 output*input+input_index 索引。
    array = np.asarray(values, dtype=np.float32)
    # flat 保持 C 连续顺序，长度等于全部维度乘积。
    flat = array.reshape(-1)
    # lines 先写物理含义、原形状和总字节数，满足自动头文件注释要求。
    lines = [
        f"/* {description}；原形状 {list(array.shape)}，float32 占 {flat.size * 4} 字节。 */",
        f"static const float {name}[{flat.size}] = {{",
    ]
    # 每行最多 8 个数，兼顾编译器和人工审查，不改变数组顺序。
    for start in range(0, flat.size, 8):
        # chunk 是当前连续 8 项或末尾不足 8 项的切片。
        chunk = flat[start : start + 8]
        # te.c_float 生成 0.0f 等合法 C99 float 字面量。
        literals = ", ".join(te.c_float(value) for value in chunk)
        # 末尾统一保留逗号，C99 允许且简化生成逻辑。
        lines.append(f"    {literals},")
    # 闭合数组定义；相邻数组之间由调用方加入空行。
    lines.append("};")
    # 返回换行连接的完整 C 片段。
    return "\n".join(lines)


def _model_prefix(role: str) -> str:
    """把 base 或 masked 角色转换为稳定的大写 C 标识前缀。"""
    # 只允许两个正式角色，防止任意字符串生成非法或冲突标识符。
    if role not in {"base", "masked"}:
        # 非法角色属于导出器编程错误，立即抛出。
        raise ValueError(f"unsupported model role: {role}")
    # upper 产生 BP_BASE 或 BP_MASKED 数组前缀。
    return f"BP_{role.upper()}"


def _render_model_arrays(artifact: M0Artifact, role: str) -> List[str]:
    """按六分支、融合层、分类层顺序生成单个 M0 的全部 C 数组。"""
    # prefix 隔离基础和掩码模型的命名空间。
    prefix = _model_prefix(role)
    # lines 收集 scaler、六分支和三层主路径数组。
    lines: List[str] = []
    # 每个模型使用自己的训练均值，不能跨模型复用。
    lines.append(_c_array(f"{prefix}_FEATURE_MEAN", artifact.mean, f"{role} 模型 297 项训练均值"))
    # 每个模型使用自己的训练标准差，全部已验证大于 1e-6。
    lines.append(_c_array(f"{prefix}_FEATURE_STD", artifact.std, f"{role} 模型 297 项训练标准差"))
    # 遍历六个物理分支，数组顺序与 Python ModuleList 完全一致。
    for branch_index in range(6):
        # weight_key 指向 [输出维度,输入维度] Linear 权重。
        weight_key = f"branches.{branch_index}.0.weight"
        # bias_key 指向 [输出维度] Linear 偏置。
        bias_key = f"branches.{branch_index}.0.bias"
        # 生成 row-major 权重数组。
        lines.append(
            _c_array(
                f"{prefix}_BRANCH_{branch_index}_WEIGHT",
                artifact.state_arrays[weight_key],
                f"{role} 模型第 {branch_index} 分支 row-major 权重",
            )
        )
        # 生成与当前分支输出一一对应的偏置数组。
        lines.append(
            _c_array(
                f"{prefix}_BRANCH_{branch_index}_BIAS",
                artifact.state_arrays[bias_key],
                f"{role} 模型第 {branch_index} 分支偏置",
            )
        )
    # 第一融合层把 80 维拼接向量映射到 64 维。
    lines.append(
        _c_array(
            f"{prefix}_FUSION_1_WEIGHT",
            artifact.state_arrays["fusion.0.weight"],
            f"{role} 模型 80→64 row-major 融合权重",
        )
    )
    # 第一融合层偏置形状 [64]。
    lines.append(
        _c_array(
            f"{prefix}_FUSION_1_BIAS",
            artifact.state_arrays["fusion.0.bias"],
            f"{role} 模型 64 维第一融合偏置",
        )
    )
    # 第二融合层把 64 维表示映射到 32 维部署嵌入。
    lines.append(
        _c_array(
            f"{prefix}_FUSION_2_WEIGHT",
            artifact.state_arrays["fusion.3.weight"],
            f"{role} 模型 64→32 row-major 融合权重",
        )
    )
    # 第二融合层偏置形状 [32]。
    lines.append(
        _c_array(
            f"{prefix}_FUSION_2_BIAS",
            artifact.state_arrays["fusion.3.bias"],
            f"{role} 模型 32 维第二融合偏置",
        )
    )
    # 分类层把 32 维嵌入映射到固定 11 类 logits。
    lines.append(
        _c_array(
            f"{prefix}_CLASSIFIER_WEIGHT",
            artifact.state_arrays["classifier.weight"],
            f"{role} 模型 32→11 row-major 分类权重",
        )
    )
    # 分类层偏置形状 [11]，输出顺序对应 CLASS_NAMES。
    lines.append(
        _c_array(
            f"{prefix}_CLASSIFIER_BIAS",
            artifact.state_arrays["classifier.bias"],
            f"{role} 模型 11 类分类偏置",
        )
    )
    # 返回各数组片段；调用方用空行分隔以便人工检查。
    return lines


def render_dual_m0_header(bundle: DualM0Bundle) -> str:
    """生成只含双六分支 M0、独立 scaler 和前向函数的 C99 头文本。"""
    # input_dims 是六个连续物理特征组维度，总和必须为 297。
    input_dims = te.MultiBranchBPNet.group_input_dims
    # output_dims 是六个分支 ReLU 输出维度，总和必须为 80。
    output_dims = te.MultiBranchBPNet.group_output_dims
    # class_names 由跨模型合同保证完全同序。
    class_names = bundle.base.class_names
    # lines 逐段构造头文件，避免大模板中数组和函数边界不清。
    lines: List[str] = [
        "#ifndef ESP32_DUAL_M0_MODEL_H",
        "#define ESP32_DUAL_M0_MODEL_H",
        "",
        "/* 本文件由 python/dual_m0_export.py 自动生成；禁止手工调整权重、顺序或融合比例。 */",
        "#include <math.h>",
        "#include <stdint.h>",
        "#include \"esp32_bp_features.h\"",
        "",
        "/* 六个分支严格对应 297 维特征的六个连续物理分组，数量不得单独修改。 */",
        "#define BP_M0_BRANCH_COUNT 6",
        "/* 六分支 ReLU 输出拼接为 80 维，作为第一融合全连接层的输入。 */",
        "#define BP_M0_CONCAT_DIM 80",
        "/* 第一融合层输出 64 维无量纲激活，随后送入 32 维嵌入层。 */",
        "#define BP_M0_FUSION_1_DIM 64",
        "/* 32 维嵌入既供 11 类分类层使用，也与 Python 主推理路径保持同序。 */",
        "#define BP_M0_EMBEDDING_DIM 32",
        "/* 掩码模型从该闭开区间起点屏蔽归一化四阶段标准分。 */",
        f"#define BP_MASKED_FEATURE_START {NORMALIZED_PHASE_START}",
        "/* 掩码区间终点不包含自身；范围 184:232 共屏蔽 48 个标准分。 */",
        f"#define BP_MASKED_FEATURE_END {NORMALIZED_PHASE_END}",
        "/* 工件格式版本用于拒绝结构或数组顺序不兼容的旧模型包。 */",
        f"#define BP_DUAL_M0_BUNDLE_FORMAT_VERSION {BUNDLE_FORMAT_VERSION}",
        "",
        "/* 六组输入维度对应基础统计、原始阶段、时序频谱、归一化阶段、冲击分布、弱类机制。 */",
        "static const uint16_t BP_M0_GROUP_INPUT_DIMS[BP_M0_BRANCH_COUNT] = { "
        + ", ".join(str(value) for value in input_dims)
        + " };",
        "/* 六组输出依次拼接为 80 维；顺序不得与 Python ModuleList 分离。 */",
        "static const uint16_t BP_M0_GROUP_OUTPUT_DIMS[BP_M0_BRANCH_COUNT] = { "
        + ", ".join(str(value) for value in output_dims)
        + " };",
        "/* 固定 11 类名称顺序直接对应 logits 索引。 */",
        "static const char* const BP_CLASS_NAMES[CLASS_NUM] = { "
        + ", ".join(te.c_string(name) for name in class_names)
        + " };",
        "/* 按固定索引返回类别名；越界返回空指针，函数引用数组以避免未使用常量警告。 */",
        "static inline const char* bp_class_name(int class_index) {",
        "    /* 合法索引范围为 0..CLASS_NUM-1。 */",
        "    if (class_index < 0 || class_index >= CLASS_NUM) return 0;",
        "    /* 返回只读静态字符串，生命周期覆盖整个固件运行期。 */",
        "    return BP_CLASS_NAMES[class_index];",
        "}",
        f"/* 基础模型文件 SHA-256，用于 BLE Manifest 和现场版本核对。 */\nstatic const char BP_BASE_MODEL_SHA256[] = \"{bundle.base.model_sha256}\";",
        f"/* 掩码模型文件 SHA-256，用于 BLE Manifest 和现场版本核对。 */\nstatic const char BP_MASKED_MODEL_SHA256[] = \"{bundle.masked.model_sha256}\";",
        "",
    ]
    # 先追加基础模型，再追加掩码模型，保持清单和 C 指针结构顺序一致。
    lines.extend(_render_model_arrays(bundle.base, "base"))
    # 空行分隔两套约 50 KiB 权重，便于审查差异。
    lines.append("")
    # 掩码模型拥有独立权重和 scaler。
    lines.extend(_render_model_arrays(bundle.masked, "masked"))
    # 追加模型指针结构和统一前向；中文注释说明数组形状、边界和复杂度。
    lines.append(
        r'''
/*
 * BpM0Model 保存一套 M0 的只读 Flash 指针，不拥有数组内存。
 * 全部指针在程序整个生命周期有效且不能为空；数组均为 row-major float32。
 */
typedef struct {
    /* feature_mean 和 feature_std 均指向 [FEATURE_DIM]，用于逐列标准化。 */
    const float* feature_mean;
    const float* feature_std;
    /* branch_weight[b] 指向 [输出维度,输入维度] 扁平数组。 */
    const float* branch_weight[BP_M0_BRANCH_COUNT];
    /* branch_bias[b] 指向 [输出维度] 偏置。 */
    const float* branch_bias[BP_M0_BRANCH_COUNT];
    /* fusion_1_weight 指向 [64,80]，fusion_1_bias 指向 [64]。 */
    const float* fusion_1_weight;
    const float* fusion_1_bias;
    /* fusion_2_weight 指向 [32,64]，fusion_2_bias 指向 [32]。 */
    const float* fusion_2_weight;
    const float* fusion_2_bias;
    /* classifier_weight 指向 [CLASS_NUM,32]，classifier_bias 指向 [CLASS_NUM]。 */
    const float* classifier_weight;
    const float* classifier_bias;
    /* suppress_normalized_phase 为 1 时把标准化后索引 184:232 固定置零。 */
    uint8_t suppress_normalized_phase;
} BpM0Model;

/* 基础 M0 使用全部 297 个标准化特征，固定融合权重由特征头定义为 0.85。 */
static const BpM0Model BP_BASE_MODEL = {
    BP_BASE_FEATURE_MEAN,
    BP_BASE_FEATURE_STD,
    {
        BP_BASE_BRANCH_0_WEIGHT, BP_BASE_BRANCH_1_WEIGHT, BP_BASE_BRANCH_2_WEIGHT,
        BP_BASE_BRANCH_3_WEIGHT, BP_BASE_BRANCH_4_WEIGHT, BP_BASE_BRANCH_5_WEIGHT
    },
    {
        BP_BASE_BRANCH_0_BIAS, BP_BASE_BRANCH_1_BIAS, BP_BASE_BRANCH_2_BIAS,
        BP_BASE_BRANCH_3_BIAS, BP_BASE_BRANCH_4_BIAS, BP_BASE_BRANCH_5_BIAS
    },
    BP_BASE_FUSION_1_WEIGHT,
    BP_BASE_FUSION_1_BIAS,
    BP_BASE_FUSION_2_WEIGHT,
    BP_BASE_FUSION_2_BIAS,
    BP_BASE_CLASSIFIER_WEIGHT,
    BP_BASE_CLASSIFIER_BIAS,
    0U
};

/* 掩码 M0 使用自己的 scaler，并把标准化后 48 个归一化阶段特征固定为训练均值。 */
static const BpM0Model BP_MASKED_MODEL = {
    BP_MASKED_FEATURE_MEAN,
    BP_MASKED_FEATURE_STD,
    {
        BP_MASKED_BRANCH_0_WEIGHT, BP_MASKED_BRANCH_1_WEIGHT, BP_MASKED_BRANCH_2_WEIGHT,
        BP_MASKED_BRANCH_3_WEIGHT, BP_MASKED_BRANCH_4_WEIGHT, BP_MASKED_BRANCH_5_WEIGHT
    },
    {
        BP_MASKED_BRANCH_0_BIAS, BP_MASKED_BRANCH_1_BIAS, BP_MASKED_BRANCH_2_BIAS,
        BP_MASKED_BRANCH_3_BIAS, BP_MASKED_BRANCH_4_BIAS, BP_MASKED_BRANCH_5_BIAS
    },
    BP_MASKED_FUSION_1_WEIGHT,
    BP_MASKED_FUSION_1_BIAS,
    BP_MASKED_FUSION_2_WEIGHT,
    BP_MASKED_FUSION_2_BIAS,
    BP_MASKED_CLASSIFIER_WEIGHT,
    BP_MASKED_CLASSIFIER_BIAS,
    1U
};

/* ReLU 把负输入截为零；输入和输出均为无量纲 float32。 */
static inline float bp_m0_relu(float value) {
    /* 严格大于零时保留原值，NaN 会走 false 并返回零；上层已提前拒绝非有限输入。 */
    return value > 0.0f ? value : 0.0f;
}

/*
 * 运行单个六分支 M0：原始特征 [297] → 标准分 [297] → 分支拼接 [80]
 * → ReLU64 → ReLU32 → logits [11]。
 * 时间复杂度为 12,336 MAC，栈数组约 (297+80+64+32)*4=1,892 字节。
 * 返回 0 表示成功，-1 表示空指针，-2 表示非有限输入或非法标准差。
 */
static inline int bp_m0_forward(
    const BpM0Model* model,
    const float feature_raw[FEATURE_DIM],
    float logits[CLASS_NUM]
) {
    /* 三个必要指针生命周期均由调用方保证覆盖本次同步调用。 */
    if (model == 0 || feature_raw == 0 || logits == 0) return -1;
    /* standardized 保存 [297] 无量纲输入，函数返回后失效。 */
    float standardized[FEATURE_DIM];
    /* 逐项执行 z=(x-mean)/std；std 必须大于 1e-6。 */
    for (int feature_index = 0; feature_index < FEATURE_DIM; feature_index++) {
        /* 读取当前原始特征、均值和标准差，避免输出数组别名影响输入。 */
        const float raw_value = feature_raw[feature_index];
        const float mean_value = model->feature_mean[feature_index];
        const float std_value = model->feature_std[feature_index];
        /* 非有限物理特征或 scaler 会永久污染动作段累计，立即拒绝。 */
        if (!isfinite(raw_value) || !isfinite(mean_value) || !isfinite(std_value)) return -2;
        /* 训练工件要求 std>1e-6；部署端重复检查防止损坏 Flash 数据。 */
        if (std_value <= 1.0e-6f) return -2;
        /* 当前列转成无量纲标准分。 */
        standardized[feature_index] = (raw_value - mean_value) / std_value;
    }
    /* 掩码模型把 184:232 标准分置零，等价于输入各自训练均值。 */
    if (model->suppress_normalized_phase != 0U) {
        /* 半开区间严格覆盖 48 项，不影响前后特征组。 */
        for (int feature_index = BP_MASKED_FEATURE_START; feature_index < BP_MASKED_FEATURE_END; feature_index++) {
            /* 零是标准化空间的训练均值。 */
            standardized[feature_index] = 0.0f;
        }
    }
    /* branch_output 保存六分支按固定顺序拼接的 [80] ReLU 输出。 */
    float branch_output[BP_M0_CONCAT_DIM];
    /* input_offset 指向当前分支在 297 维输入中的起始索引。 */
    int input_offset = 0;
    /* output_offset 指向当前分支在 80 维拼接输出中的起始索引。 */
    int output_offset = 0;
    /* 依次运行六个物理特征分支。 */
    for (int branch_index = 0; branch_index < BP_M0_BRANCH_COUNT; branch_index++) {
        /* input_dim 和 output_dim 来自固定模型合同。 */
        const int input_dim = (int)BP_M0_GROUP_INPUT_DIMS[branch_index];
        const int output_dim = (int)BP_M0_GROUP_OUTPUT_DIMS[branch_index];
        /* 遍历当前分支每个输出神经元。 */
        for (int output_index = 0; output_index < output_dim; output_index++) {
            /* sum 从当前输出偏置开始累加。 */
            float sum = model->branch_bias[branch_index][output_index];
            /* 当前权重行起始索引为 output_index*input_dim。 */
            const int row_offset = output_index * input_dim;
            /* 遍历该物理组全部输入标准分。 */
            for (int input_index = 0; input_index < input_dim; input_index++) {
                /* row-major 权重乘对应连续特征并累加。 */
                sum += model->branch_weight[branch_index][row_offset + input_index]
                    * standardized[input_offset + input_index];
            }
            /* 分支 Linear 后执行 ReLU，并写入固定拼接位置。 */
            branch_output[output_offset + output_index] = bp_m0_relu(sum);
        }
        /* 两个 offset 同步移到下一分支的输入和输出起点。 */
        input_offset += input_dim;
        output_offset += output_dim;
    }
    /* hidden_64 保存 80→64 第一融合层 ReLU 输出。 */
    float hidden_64[BP_M0_FUSION_1_DIM];
    /* 遍历第一融合层 64 个输出神经元。 */
    for (int output_index = 0; output_index < BP_M0_FUSION_1_DIM; output_index++) {
        /* sum 从对应偏置开始。 */
        float sum = model->fusion_1_bias[output_index];
        /* row_offset 定位 [64,80] row-major 当前行。 */
        const int row_offset = output_index * BP_M0_CONCAT_DIM;
        /* 累加 80 个拼接分支输入。 */
        for (int input_index = 0; input_index < BP_M0_CONCAT_DIM; input_index++) {
            /* 权重与对应分支输出相乘并累加。 */
            sum += model->fusion_1_weight[row_offset + input_index] * branch_output[input_index];
        }
        /* 第一融合层执行 ReLU。 */
        hidden_64[output_index] = bp_m0_relu(sum);
    }
    /* hidden_32 保存 64→32 第二融合层 ReLU 输出。 */
    float hidden_32[BP_M0_EMBEDDING_DIM];
    /* 遍历 32 个嵌入输出。 */
    for (int output_index = 0; output_index < BP_M0_EMBEDDING_DIM; output_index++) {
        /* sum 从第二融合偏置开始。 */
        float sum = model->fusion_2_bias[output_index];
        /* row_offset 定位 [32,64] row-major 当前行。 */
        const int row_offset = output_index * BP_M0_FUSION_1_DIM;
        /* 累加 64 个第一融合输出。 */
        for (int input_index = 0; input_index < BP_M0_FUSION_1_DIM; input_index++) {
            /* 乘加得到当前嵌入分量。 */
            sum += model->fusion_2_weight[row_offset + input_index] * hidden_64[input_index];
        }
        /* 第二融合层执行 ReLU。 */
        hidden_32[output_index] = bp_m0_relu(sum);
    }
    /* 分类层输出 11 个 softmax 前 logits，不执行激活。 */
    for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
        /* sum 从当前类别偏置开始。 */
        float sum = model->classifier_bias[class_index];
        /* row_offset 定位 [11,32] row-major 当前类别行。 */
        const int row_offset = class_index * BP_M0_EMBEDDING_DIM;
        /* 累加 32 维嵌入。 */
        for (int input_index = 0; input_index < BP_M0_EMBEDDING_DIM; input_index++) {
            /* 分类权重乘嵌入并累加。 */
            sum += model->classifier_weight[row_offset + input_index] * hidden_32[input_index];
        }
        /* 写入固定类别顺序的无量纲 logit。 */
        logits[class_index] = sum;
    }
    /* 返回零表示两层标准化、六分支和分类全部完成。 */
    return 0;
}

/*
 * 对同一 [297] 原始特征运行基础和掩码 M0，再按 0.85/0.15 融合。
 * 三个输出数组均为 [CLASS_NUM]；调用方可传空的单模型输出以外没有可选指针。
 */
static inline int bp_dual_m0_forward(
    const float feature_raw[FEATURE_DIM],
    float base_logits[CLASS_NUM],
    float masked_logits[CLASS_NUM],
    float combined_logits[CLASS_NUM]
) {
    /* 任一数组为空都无法完成逐值诊断或固定融合。 */
    if (feature_raw == 0 || base_logits == 0 || masked_logits == 0 || combined_logits == 0) return -1;
    /* 运行全部特征基础模型；错误码原样返回。 */
    int status = bp_m0_forward(&BP_BASE_MODEL, feature_raw, base_logits);
    /* 基础模型失败时不运行第二模型，避免输出半有效组合。 */
    if (status != 0) return status;
    /* 运行独立 scaler 和 184:232 掩码的第二模型。 */
    status = bp_m0_forward(&BP_MASKED_MODEL, feature_raw, masked_logits);
    /* 掩码模型失败时不修改 combined_logits。 */
    if (status != 0) return status;
    /* 使用特征头中的固定验证权重融合，成功返回零。 */
    return bp_combine_ensemble_logits(base_logits, masked_logits, combined_logits);
}

/* 从 [WINDOW_LEN,6] 清洗窗口提取 297 特征并运行双 M0；窗口单位为 deg/s 和 g。 */
static inline int bp_dual_m0_forward_from_window(
    const float window[WINDOW_LEN][AXIS_NUM],
    float base_logits[CLASS_NUM],
    float masked_logits[CLASS_NUM],
    float combined_logits[CLASS_NUM]
) {
    /* 任一窗口或输出为空时拒绝访问；有效生命周期只需覆盖当前同步调用。 */
    if (window == 0 || base_logits == 0 || masked_logits == 0 || combined_logits == 0) return -1;
    /* feature 保存当前窗口 297 项原始物理/统计特征，约占 1188 字节栈。 */
    float feature[FEATURE_DIM];
    /* 使用与 Python 逐值校验的唯一特征入口，内部先执行保守单轴毛刺修复。 */
    extract_features_from_window(window, feature);
    /* 运行两套 M0 并返回融合状态。 */
    return bp_dual_m0_forward(feature, base_logits, masked_logits, combined_logits);
}

#endif
'''
    )
    # 用双换行分隔数组和函数段，保持 UTF-8 中文注释可读。
    return "\n\n".join(lines)


def render_feature_runtime_header(bundle: DualM0Bundle, scratch_dir: Path) -> str:
    """从现有已验证 C 模板提取无旧平铺权重的 297 维特征运行时。"""
    # scratch 必须位于项目输出目录，遵守运行期临时文件项目本地化规则。
    scratch = Path(scratch_dir)
    # parents=True 允许首次导出创建目标目录。
    scratch.mkdir(parents=True, exist_ok=True)
    # dummy 只用于调用现有模板；其平铺权重随后按稳定标记完全移除。
    dummy = te.BPNet(len(bundle.base.feature_names), len(bundle.base.class_names), dropout=0.0)
    # template_result 提供特征函数所需窗口和训练活动阈值。
    template_result = {
        "model": dummy,
        "mean": np.zeros(len(bundle.base.feature_names), dtype=np.float32),
        "std": np.ones(len(bundle.base.feature_names), dtype=np.float32),
        "window_len": bundle.base.window_len,
        "rest_threshold": bundle.base.rest_threshold,
        "active_point_threshold": bundle.base.active_point_threshold,
        "suppress_normalized_phase": False,
    }
    # TemporaryDirectory 在目标目录内创建并自动删除模板文件。
    with tempfile.TemporaryDirectory(dir=scratch) as temp_dir:
        # template_path 是旧完整平铺头的短生命周期路径。
        template_path = Path(temp_dir) / "template_flat_header.h"
        # 调用现有通过 86 项测试的 C 特征生成器，避免复制 297 项公式。
        te.export_esp32_header(
            template_result,
            bundle.base.class_names,
            bundle.base.feature_names,
            template_path,
        )
        # text 使用 UTF-8 读取中文注释和 C99 源码。
        text = template_path.read_text(encoding="utf-8")
    # arrays_marker 位于类别名及旧 FEATURE_MEAN/W1 前，之后的模型数组全部丢弃。
    arrays_marker = "static const char* CLASS_NAMES[CLASS_NUM]"
    # body_marker 位于通用平滑、融合、动作段和特征函数的稳定起点。
    body_marker = "/*\n * 因果类别分数平滑状态。"
    # model_marker 位于旧平铺 BP 前向起点，之后全部丢弃。
    model_marker = "static inline float relu_float"
    # 三个标记缺一说明上游模板结构变化，必须人工审查而不是静默截错代码。
    if arrays_marker not in text or body_marker not in text or model_marker not in text:
        # RuntimeError 明确要求同步更新提取器。
        raise RuntimeError("feature header template markers changed")
    # prefix 保留 include、维度、采样率、活动阈值和特征常量，不含任何平铺权重。
    prefix = text[: text.index(arrays_marker)]
    # body 保留平滑、固定融合、动作段累计、清洗和 297 维特征函数。
    body = text[text.index(body_marker) : text.index(model_marker)]
    # guard 从旧单模型名称替换为独立特征运行时名称，避免两个头互相屏蔽。
    prefix = prefix.replace("ESP32_BP_MODEL_H", "ESP32_BP_FEATURES_H")
    # result 以新的 include guard 结束，不保留旧模型末尾 #endif。
    result = prefix + body + "\n#endif\n"
    # 额外防御：若平铺 W1 数组仍存在则说明截取边界错误。
    if "static const float W1[" in result:
        # 禁止把未使用旧模型权重浪费到 Flash。
        raise RuntimeError("flat BP weights leaked into feature header")
    # 返回可直接写入 esp32_bp_features.h 的 UTF-8 C99 文本。
    return result


def _portable_arrays(bundle: DualM0Bundle) -> Dict[str, np.ndarray]:
    """构造不含 pickle 和辅助头的便携 NPZ 字段。"""
    # fields 先保存跨模型共享的类别、特征和结构元数据。
    fields: Dict[str, np.ndarray] = {
        "bundle_format_version": np.asarray([BUNDLE_FORMAT_VERSION], dtype=np.int32),
        "class_names": np.asarray(bundle.base.class_names),
        "feature_names": np.asarray(bundle.base.feature_names),
        "group_input_dims": np.asarray(te.MultiBranchBPNet.group_input_dims, dtype=np.int32),
        "group_output_dims": np.asarray(te.MultiBranchBPNet.group_output_dims, dtype=np.int32),
        "window_len": np.asarray([bundle.base.window_len], dtype=np.int32),
        "step_len": np.asarray([bundle.base.step_len], dtype=np.int32),
        "sample_rate_hz": np.asarray([bundle.base.sample_rate_hz], dtype=np.int32),
        "ensemble_weights": np.asarray(
            [te.ENSEMBLE_BASE_LOGIT_WEIGHT, te.ENSEMBLE_MASKED_LOGIT_WEIGHT],
            dtype=np.float32,
        ),
        "masked_feature_range": np.asarray(
            [NORMALIZED_PHASE_START, NORMALIZED_PHASE_END], dtype=np.int32
        ),
    }
    # 遍历两个固定角色，写入独立 scaler、掩码和主路径权重。
    for role, artifact in (("base", bundle.base), ("masked", bundle.masked)):
        # 角色前缀保持 NPZ 键可读且跨语言稳定。
        prefix = role
        # scaler 形状均为 [297] float32。
        fields[f"{prefix}_mean"] = artifact.mean
        # 标准差形状均为 [297] float32。
        fields[f"{prefix}_std"] = artifact.std
        # 掩码保存为单元素 uint8，避免不同语言 bool 宽度差异。
        fields[f"{prefix}_suppress_normalized_phase"] = np.asarray(
            [1 if artifact.suppress_normalized_phase else 0], dtype=np.uint8
        )
        # 主路径 state key 转成不含点号的稳定 NPZ 名称。
        for state_key in MAIN_STATE_KEYS:
            # safe_key 只含字母、数字和下划线，便于 C/C#/Rust 读取。
            safe_key = state_key.replace(".", "_")
            # 数组保持原 row-major 形状和 float32 精度。
            fields[f"{prefix}_{safe_key}"] = artifact.state_arrays[state_key]
    # 返回所有字段；键中不会出现 auxiliary。
    return fields


def export_dual_m0_bundle(base_dir: Path, masked_dir: Path, output_dir: Path) -> Dict[str, Path]:
    """验证两个工件并原子式生成便携 NPZ、特征头、双 M0 头和 JSON 清单。"""
    # bundle 加载阶段先完成全部模型、scaler、顺序和时序合同校验。
    bundle = load_dual_m0_bundle(Path(base_dir), Path(masked_dir))
    # output 是调用方指定的项目内生成目录。
    output = Path(output_dir)
    # parents=True 支持首次创建 esp32/generated 等嵌套目录。
    output.mkdir(parents=True, exist_ok=True)
    # portable_path 保存不含 pickle 和辅助头的全部部署数组。
    portable_path = output / "dual_m0_bundle.npz"
    # feature_header_path 保存清洗和 297 维 C 特征公式。
    feature_header_path = output / "esp32_bp_features.h"
    # model_header_path 保存双六分支权重、scaler、前向和固定融合。
    model_header_path = output / "esp32_dual_m0_model.h"
    # manifest_path 保存可审计哈希、维度和算法版本。
    manifest_path = output / "dual_m0_manifest.json"
    # np.savez 写入纯数值/Unicode 数组，读取端必须使用 allow_pickle=False。
    np.savez(portable_path, **_portable_arrays(bundle))
    # 生成并写入独立特征运行时，UTF-8 保留中文自动注释。
    feature_header_path.write_text(
        render_feature_runtime_header(bundle, output), encoding="utf-8"
    )
    # 生成并写入双模型权重与前向头。
    model_header_path.write_text(render_dual_m0_header(bundle), encoding="utf-8")
    # manifest 汇总算法固定合同和四个可验证文件哈希。
    manifest = {
        "bundle_format_version": BUNDLE_FORMAT_VERSION,
        "feature_dim": len(bundle.base.feature_names),
        "class_count": len(bundle.base.class_names),
        "class_names": list(bundle.base.class_names),
        "sample_rate_hz": bundle.base.sample_rate_hz,
        "window_len": bundle.base.window_len,
        "step_len": bundle.base.step_len,
        "group_input_dims": list(te.MultiBranchBPNet.group_input_dims),
        "group_output_dims": list(te.MultiBranchBPNet.group_output_dims),
        "fusion_dims": [80, 64, 32, len(bundle.base.class_names)],
        "ensemble_weights": {
            "base": float(te.ENSEMBLE_BASE_LOGIT_WEIGHT),
            "masked": float(te.ENSEMBLE_MASKED_LOGIT_WEIGHT),
        },
        "masked_feature_range": [NORMALIZED_PHASE_START, NORMALIZED_PHASE_END],
        "models": {
            "base": {
                "sha256": bundle.base.model_sha256,
                "suppress_normalized_phase": False,
            },
            "masked": {
                "sha256": bundle.masked.model_sha256,
                "suppress_normalized_phase": True,
            },
        },
        "generated_files": {
            portable_path.name: _sha256_file(portable_path),
            feature_header_path.name: _sha256_file(feature_header_path),
            model_header_path.name: _sha256_file(model_header_path),
        },
        "hardware_validation_status": "not_run_no_hardware",
    }
    # ensure_ascii=False 保留中文状态值；indent=2 便于 Git 和人工审计。
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    # 返回固定键到实际文件路径，调用方可继续复制或编译验证。
    return {
        "portable_bundle": portable_path,
        "feature_header": feature_header_path,
        "model_header": model_header_path,
        "manifest": manifest_path,
    }


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    """解析双 M0 导出命令行；路径不存在等语义错误由加载器统一报告。"""
    # parser 的说明明确该工具只接受最终六分支 M0，不兼容旧平铺 BP。
    parser = argparse.ArgumentParser(
        description="导出最终基础 M0 与 184:232 掩码 M0 的 ESP32-S3 C 工件"
    )
    # --base-artifact-dir 指向 Round29 类未掩码验证工件目录。
    parser.add_argument(
        "--base-artifact-dir",
        type=Path,
        required=True,
        help="包含基础 best_model.pt 与 scaler_and_config.npz 的目录",
    )
    # --masked-artifact-dir 指向 Round37 类掩码验证工件目录。
    parser.add_argument(
        "--masked-artifact-dir",
        type=Path,
        required=True,
        help="包含掩码 best_model.pt 与 scaler_and_config.npz 的目录",
    )
    # --output-dir 是生成四个便携文件的唯一目标目录。
    parser.add_argument(
        "--output-dir",
        type=Path,
        required=True,
        help="写入特征头、双 M0 头、便携 NPZ 和 JSON 清单的目录",
    )
    # parse_args 支持测试传入显式 argv，也支持命令行默认读取 sys.argv。
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    """执行正式导出并逐行打印生成物，成功返回进程码 0。"""
    # args 保存三个已经由 argparse 转换为 Path 的目录参数。
    args = parse_args(argv)
    # paths 由核心导出器返回，键顺序固定为 NPZ、特征头、模型头、清单。
    paths = export_dual_m0_bundle(
        args.base_artifact_dir,
        args.masked_artifact_dir,
        args.output_dir,
    )
    # 逐项打印绝对路径和 SHA-256，便于 CI、用户和固件发布记录复核。
    for artifact_name, artifact_path in paths.items():
        # resolve 只规范化输出显示，不改变已写文件位置。
        resolved = Path(artifact_path).resolve()
        # 每行包含稳定键、路径和哈希，不输出模型权重内容。
        print(f"export={artifact_name} path={resolved} sha256={_sha256_file(resolved)}")
    # 零表示全部合同校验和四个文件写入成功。
    return 0


# 直接运行本模块时进入 CLI；作为测试或库导入时不会写任何文件。
if __name__ == "__main__":
    # SystemExit 把 main 返回值转换为操作系统进程退出码。
    raise SystemExit(main())
