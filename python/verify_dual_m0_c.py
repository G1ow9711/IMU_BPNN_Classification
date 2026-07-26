"""编译并运行生成 C99 头，逐值核对 297 特征、双 M0、融合和动作段累计。

297 项特征公式与边界见 ``docs/算法原理、训练与实时计数.md`` 第 5～12 节，动作段重置合同见
``docs/算法原理、训练与实时计数.md``；本脚本用明确误差门验证 Python/C 一致性。
"""

from __future__ import annotations

# argparse 提供工件、头文件、编译器和项目本地临时目录参数。
import argparse
# json 输出机器可读验证报告，供交付审计和 CI 读取。
import json
# shutil 解析 gcc 可执行文件并在缺失时给出明确错误。
import shutil
# subprocess 编译和执行临时 C99 程序，捕获标准输出和错误。
import subprocess
# Path 统一处理 Windows 路径、生成源文件和验证报告。
from pathlib import Path
# Sequence 描述可由测试注入的命令行参数。
from typing import Sequence

# NumPy 生成确定性六轴窗口、参考特征和逐层误差统计。
import numpy as np

# dual_m0_export 加载正式工件并计算 NumPy 双 M0 参考 logits。
import dual_m0_export as dme
# train_export 提供生产 297 维 Python 特征、固定融合和动作段累计参考。
import train_export as te


# FEATURE_TOLERANCE 是 Python/C 297 维最大绝对误差硬门。
FEATURE_TOLERANCE = 1.0e-4
# LOGIT_TOLERANCE 是两个 M0、融合和动作段平均 logits 的最大绝对误差硬门。
LOGIT_TOLERANCE = 1.0e-3


def _c_float(value: float) -> str:
    """把有限 Python/NumPy 浮点转换为可由 C99 编译的 float 字面量。"""
    # np.isfinite 拒绝 NaN/Inf，防止生成依赖编译器扩展的字面量。
    if not np.isfinite(value):
        # 验证输入本身异常时应先修 Python 数据，而不是让 C 结果不可判定。
        raise ValueError("C verification values must be finite")
    # 复用生产导出器保证整数值生成 0.0f 而非非法 0f。
    return te.c_float(float(value))


def _c_matrix(name: str, values: np.ndarray) -> str:
    """把二维 float32 矩阵生成为带固定列数的 C static const 数组。"""
    # array 必须是 [行数,列数]；其它维度表示调用方排列错误。
    array = np.asarray(values, dtype=np.float32)
    # 只允许二维，窗口和特征批次均满足该合同。
    if array.ndim != 2:
        # 错误包含实际形状，便于定位缺少样本轴。
        raise ValueError(f"C matrix must be 2-D, got {array.shape}")
    # lines 首行声明数组尺寸，后续每行对应一个完整样本或时间点。
    lines = [f"static const float {name}[{array.shape[0]}][{array.shape[1]}] = {{"]
    # 遍历行，保持样本和时间顺序不变。
    for row in array:
        # literals 按列顺序格式化为 C99 float。
        literals = ", ".join(_c_float(value) for value in row)
        # 每个 C 行初始化器对应 Python 的一行。
        lines.append(f"    {{ {literals} }},")
    # 闭合数组定义。
    lines.append("};")
    # 返回完整源码片段。
    return "\n".join(lines)


def build_reference_window() -> np.ndarray:
    """构造含重力、旋转和冲击的确定性 [62,6] 六轴窗口。"""
    # sample_index 形状 [62]，表示 25 Hz 下从 0 开始的离散采样序号。
    sample_index = np.arange(62, dtype=np.float32)
    # time_seconds 形状 [62]，单位秒，相邻间隔严格为 0.04 秒。
    time_seconds = sample_index / np.float32(25.0)
    # window 形状 [62,6]，通道顺序固定 gx、gy、gz、ax、ay、az。
    window = np.zeros((62, 6), dtype=np.float32)
    # gx 单位 deg/s，包含约 1.1 Hz 主旋转和小幅高频分量。
    window[:, 0] = 180.0 * np.sin(2.0 * np.pi * 1.1 * time_seconds)
    # gy 单位 deg/s，与 gx 存在相位差，覆盖水平各向异性特征。
    window[:, 1] = 95.0 * np.cos(2.0 * np.pi * 1.1 * time_seconds + 0.3)
    # gz 单位 deg/s，较慢分量覆盖垂直角速度投影。
    window[:, 2] = 55.0 * np.sin(2.0 * np.pi * 0.6 * time_seconds - 0.2)
    # ax 单位 g，模拟水平摆动。
    window[:, 3] = 0.35 * np.sin(2.0 * np.pi * 1.1 * time_seconds + 0.6)
    # ay 单位 g，模拟另一水平轴和轻微偏置。
    window[:, 4] = 0.22 * np.cos(2.0 * np.pi * 1.1 * time_seconds) + 0.03
    # az 单位 g，1.0 是重力，周期分量模拟起伏。
    window[:, 5] = 1.0 + 0.45 * np.sin(2.0 * np.pi * 1.1 * time_seconds - 0.4)
    # 在索引 39 添加多轴落地冲击；多轴共同变化不应被单轴去毛刺删除。
    window[39, 3:6] += np.asarray([0.8, -0.5, 1.4], dtype=np.float32)
    # 返回连续 float32 窗口供 Python/C 同式特征验证。
    return np.ascontiguousarray(window, dtype=np.float32)


def _render_c_program(window: np.ndarray, feature_samples: np.ndarray) -> str:
    """生成打印特征、单模型、融合和动作段平均值的 C99 验证程序。"""
    # window_text 生成 [62,6] 物理单位输入常量。
    window_text = _c_matrix("REFERENCE_WINDOW", window)
    # feature_text 生成若干 [297] 原始特征批次，用于独立模型前向测试。
    feature_text = _c_matrix("REFERENCE_FEATURES", feature_samples)
    # sample_count 是动作段累计更新次数。
    sample_count = int(feature_samples.shape[0])
    # 返回完整 C99 程序；每行输出使用稳定前缀供 Python 解析。
    return f'''#include <stdio.h>
#include "esp32_dual_m0_model.h"

/* REFERENCE_WINDOW 形状 [62,6]，通道顺序 gx、gy、gz、ax、ay、az，单位 deg/s 和 g。 */
{window_text}

/* REFERENCE_FEATURES 形状 [{sample_count},297]，元素是标准化前原始特征。 */
{feature_text}

/* 打印固定长度 float 数组；格式提供 9 位有效数字以保留 float32 精度。 */
static void print_values(const char* prefix, const float* values, int length) {{
    /* prefix 区分特征、基础、掩码、融合和动作段输出。 */
    printf("%s", prefix);
    /* 遍历固定 length 项并用竖线分隔，禁止本地化小数分隔符。 */
    for (int index = 0; index < length; index++) {{
        /* double 强制转换符合 printf 可变参数提升规则。 */
        printf("|%.9g", (double)values[index]);
    }}
    /* 每组数组独占一行，便于 Python 严格解析。 */
    printf("\\n");
}}

/* 主程序成功返回零；任一生成 C API 错误立即返回非零。 */
int main(void) {{
    /* extracted 形状 [297]，接收六轴窗口的生产特征。 */
    float extracted[FEATURE_DIM];
    /* 执行 C 端清洗和完整 297 维特征提取。 */
    extract_features_from_window(REFERENCE_WINDOW, extracted);
    /* 输出全部特征供 Python 最大误差计算。 */
    print_values("FEATURE", extracted, FEATURE_DIM);
    /* bout 保存当前动作段全部历史融合 logits，初始必须显式重置。 */
    BpBoutAccumulator bout;
    /* 清空累计和与窗口数。 */
    bp_bout_accumulator_reset(&bout);
    /* 遍历 {sample_count} 个确定性原始特征样本。 */
    for (int sample = 0; sample < {sample_count}; sample++) {{
        /* base_logits 形状 [11]，接收基础 M0 输出。 */
        float base_logits[CLASS_NUM];
        /* masked_logits 形状 [11]，接收 184:232 掩码 M0 输出。 */
        float masked_logits[CLASS_NUM];
        /* combined_logits 形状 [11]，接收固定 0.85/0.15 融合。 */
        float combined_logits[CLASS_NUM];
        /* averaged_logits 形状 [11]，接收从动作段起点到当前的因果均值。 */
        float averaged_logits[CLASS_NUM];
        /* status 接收双 M0 前向错误码。 */
        int status = bp_dual_m0_forward(
            REFERENCE_FEATURES[sample], base_logits, masked_logits, combined_logits
        );
        /* 非零表示标准化、模型或融合失败。 */
        if (status != 0) return 10 + sample;
        /* class_index 是动作段累计平均 logits 的 argmax。 */
        int class_index = bp_bout_accumulator_update(&bout, combined_logits, averaged_logits);
        /* 负类别表示累计器拒绝非有限值或空指针。 */
        if (class_index < 0) return 20 + sample;
        /* prefix 加样本号，保证多轮输出按原顺序关联。 */
        char prefix[32];
        /* 打印基础 M0 当前样本。 */
        snprintf(prefix, sizeof(prefix), "BASE_%d", sample);
        print_values(prefix, base_logits, CLASS_NUM);
        /* 打印掩码 M0 当前样本。 */
        snprintf(prefix, sizeof(prefix), "MASKED_%d", sample);
        print_values(prefix, masked_logits, CLASS_NUM);
        /* 打印固定融合当前样本。 */
        snprintf(prefix, sizeof(prefix), "COMBINED_%d", sample);
        print_values(prefix, combined_logits, CLASS_NUM);
        /* 打印动作段因果平均当前样本。 */
        snprintf(prefix, sizeof(prefix), "BOUT_%d", sample);
        print_values(prefix, averaged_logits, CLASS_NUM);
        /* 单独打印整数类别，检查 C/Python argmax 平局规则。 */
        printf("CLASS_%d|%d\\n", sample, class_index);
    }}
    /* 零表示全部数组和状态机已输出。 */
    return 0;
}}
'''


def _parse_vector(lines: dict[str, list[float]], key: str, expected_length: int) -> np.ndarray:
    """从 C 输出字典取固定长度向量并转换为 float32。"""
    # key 缺失表示 C 程序未执行到预期阶段。
    if key not in lines:
        # KeyError 保留具体输出前缀。
        raise KeyError(f"missing C output line: {key}")
    # values 转为 float32 一维数组。
    values = np.asarray(lines[key], dtype=np.float32)
    # 长度必须与特征维或类别数完全相同。
    if values.shape != (expected_length,):
        # 报告实际形状，定位 printf 或解析协议错误。
        raise ValueError(f"{key} has shape {values.shape}, expected {(expected_length,)}")
    # 返回固定形状向量。
    return values


def verify_dual_m0_c(
    base_dir: Path,
    masked_dir: Path,
    header_dir: Path,
    work_dir: Path,
    compiler: str = "gcc",
) -> dict[str, object]:
    """编译并执行 C99 运行时，返回包含四类最大误差和分类一致性的报告。"""
    # bundle 加载正式工件并验证全部跨模型合同。
    bundle = dme.load_dual_m0_bundle(base_dir, masked_dir)
    # include_dir 必须含本轮生成的两个头文件。
    include_dir = Path(header_dir)
    # 逐个检查必要头，避免 GCC 给出难读的级联错误。
    for header_name in ("esp32_bp_features.h", "esp32_dual_m0_model.h"):
        # is_file 拒绝缺失和同名目录。
        if not (include_dir / header_name).is_file():
            # FileNotFoundError 指出完整预期路径。
            raise FileNotFoundError(include_dir / header_name)
    # compiler_path 优先使用显式路径，否则从 PATH 解析 gcc。
    compiler_path = shutil.which(compiler) if not Path(compiler).is_file() else str(Path(compiler))
    # 找不到编译器时不能声称 C 验证通过。
    if compiler_path is None:
        # RuntimeError 给出安装或参数修复方向。
        raise RuntimeError(f"C compiler not found: {compiler}")
    # scratch 是项目本地可删除构建目录。
    scratch = Path(work_dir)
    # parents=True 支持 `.codex-local/tmp/...` 首次创建。
    scratch.mkdir(parents=True, exist_ok=True)
    # window 是确定性 [62,6] 六轴物理量输入。
    window = build_reference_window()
    # python_features 是经同一保守清洗后的 [297] 生产参考。
    python_features = te.extract_features(window).astype(np.float32)
    # rng 固定种子生成三个额外原始特征样本，覆盖正负和不同尺度。
    rng = np.random.default_rng(20260713)
    # feature_samples 形状 [4,297]；首项使用真实窗口特征，其余为确定性随机值。
    feature_samples = np.vstack(
        [
            python_features,
            np.zeros(297, dtype=np.float32),
            np.linspace(-2.0, 2.0, 297, dtype=np.float32),
            rng.normal(0.0, 1.2, size=297).astype(np.float32),
        ]
    ).astype(np.float32)
    # source_path 保存由本脚本生成的 C99 验证程序。
    source_path = scratch / "verify_dual_m0.c"
    # executable_path 在 Windows 使用 .exe；MinGW 对无后缀也可运行，但显式后缀更清楚。
    executable_path = scratch / "verify_dual_m0.exe"
    # 写入 UTF-8 C 源；生成文件属于项目本地运行工件，不提交仓库。
    source_path.write_text(_render_c_program(window, feature_samples), encoding="utf-8")
    # compile_command 开启 C99、O0 和全部警告即错误，避免优化隐藏未初始化问题。
    compile_command = [
        compiler_path,
        "-std=c99",
        "-O0",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pedantic",
        f"-I{include_dir}",
        str(source_path),
        "-lm",
        "-o",
        str(executable_path),
    ]
    # check=True 使编译错误立即成为失败；capture_output 保留完整诊断。
    # 编译失败时把 GCC 标准错误包装进异常，避免只看到返回码。
    try:
        # check=True 使任何警告即错误或语法问题进入异常路径。
        subprocess.run(compile_command, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as error:
        # RuntimeError 保留完整命令输出，便于修正生成 C 而非放宽编译门槛。
        raise RuntimeError(f"C compilation failed:\n{error.stdout}\n{error.stderr}") from error
    # 执行 C 程序；输出只包含稳定前缀和值。
    completed = subprocess.run(
        [str(executable_path)], check=True, capture_output=True, text=True
    )
    # parsed 保存 prefix 到浮点字段；整数类别稍后同样以 float 读取再转 int。
    parsed: dict[str, list[float]] = {}
    # 逐行解析 `PREFIX|value|value...`。
    for raw_line in completed.stdout.splitlines():
        # 空行没有业务含义，直接跳过。
        if not raw_line.strip():
            # continue 进入下一 C 输出行。
            continue
        # parts[0] 是稳定前缀，后续字段均应为有限十进制数。
        parts = raw_line.strip().split("|")
        # 至少需要一个前缀和一个数值。
        if len(parts) < 2:
            # 非法输出可能来自意外 printf，立即拒绝。
            raise ValueError(f"invalid C output line: {raw_line}")
        # 把数值字段转换为 Python float；非数值会自然抛 ValueError。
        parsed[parts[0]] = [float(value) for value in parts[1:]]
    # c_features 是 C 提取的 [297] 特征。
    c_features = _parse_vector(parsed, "FEATURE", 297)
    # feature_error 使用 float64 差值避免最大误差统计再次舍入。
    feature_error = float(
        np.max(np.abs(c_features.astype(np.float64) - python_features.astype(np.float64)))
    )
    # Python 基础 M0 输出形状 [4,11]。
    base_expected = dme.numpy_m0_logits(bundle.base, feature_samples)
    # Python 掩码 M0 输出形状 [4,11]。
    masked_expected = dme.numpy_m0_logits(bundle.masked, feature_samples)
    # 固定融合输出形状 [4,11]。
    combined_expected = te.combine_ensemble_logits(base_expected, masked_expected)
    # bout_reference 从空状态开始逐样本累计，模拟同一活动段。
    bout_reference = te.CausalBoutLogitAccumulator(len(bundle.base.class_names))
    # bout_expected 收集每一步 [11] 因果平均。
    bout_expected = np.vstack(
        [bout_reference.update(row) for row in combined_expected]
    ).astype(np.float32)
    # 初始化四类最大误差。
    base_error = 0.0
    # masked_error 记录第二 M0 C/Python 最大差。
    masked_error = 0.0
    # combined_error 记录固定融合最大差。
    combined_error = 0.0
    # bout_error 记录动作段平均最大差。
    bout_error = 0.0
    # class_matches 只有全部样本 C/Python argmax 一致时保持真。
    class_matches = True
    # 逐样本读取并更新最大误差和分类一致性。
    for sample_index in range(feature_samples.shape[0]):
        # 读取 C 基础 logits。
        base_actual = _parse_vector(parsed, f"BASE_{sample_index}", 11)
        # 读取 C 掩码 logits。
        masked_actual = _parse_vector(parsed, f"MASKED_{sample_index}", 11)
        # 读取 C 固定融合 logits。
        combined_actual = _parse_vector(parsed, f"COMBINED_{sample_index}", 11)
        # 读取 C 动作段平均 logits。
        bout_actual = _parse_vector(parsed, f"BOUT_{sample_index}", 11)
        # 更新基础模型最大绝对误差。
        base_error = max(
            base_error,
            float(np.max(np.abs(base_actual.astype(np.float64) - base_expected[sample_index]))),
        )
        # 更新掩码模型最大绝对误差。
        masked_error = max(
            masked_error,
            float(
                np.max(
                    np.abs(masked_actual.astype(np.float64) - masked_expected[sample_index])
                )
            ),
        )
        # 更新融合最大绝对误差。
        combined_error = max(
            combined_error,
            float(
                np.max(
                    np.abs(combined_actual.astype(np.float64) - combined_expected[sample_index])
                )
            ),
        )
        # 更新动作段累计最大绝对误差。
        bout_error = max(
            bout_error,
            float(np.max(np.abs(bout_actual.astype(np.float64) - bout_expected[sample_index]))),
        )
        # C 类别行应只含一个整数值。
        class_values = parsed.get(f"CLASS_{sample_index}")
        # 缺失或多值都判为失败。
        if class_values is None or len(class_values) != 1:
            # class_matches 记录失败，最终报告仍给出其它误差。
            class_matches = False
        else:
            # expected_class 使用 NumPy argmax 的首个最大值规则。
            expected_class = int(np.argmax(bout_expected[sample_index]))
            # 转 int 后与 C 返回类别比较。
            class_matches = class_matches and int(class_values[0]) == expected_class
    # passed 同时要求特征、四类 logits 和类别全部满足硬门。
    passed = (
        feature_error <= FEATURE_TOLERANCE
        and base_error <= LOGIT_TOLERANCE
        and masked_error <= LOGIT_TOLERANCE
        and combined_error <= LOGIT_TOLERANCE
        and bout_error <= LOGIT_TOLERANCE
        and class_matches
    )
    # 返回机器可读报告；该字段声明本工具只验证 C/Python 数值，不编码真板结论。
    return {
        "passed": passed,
        "feature_max_abs_error": feature_error,
        "base_logit_max_abs_error": base_error,
        "masked_logit_max_abs_error": masked_error,
        "combined_logit_max_abs_error": combined_error,
        "bout_logit_max_abs_error": bout_error,
        "class_indices_exact": class_matches,
        "feature_tolerance": FEATURE_TOLERANCE,
        "logit_tolerance": LOGIT_TOLERANCE,
        "sample_count": int(feature_samples.shape[0]),
        "compiler": compiler_path,
        "hardware_validation_status": "not_encoded_c_parity_only",
    }


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    """解析 C99 逐值验证命令行。"""
    # parser 说明该工具会真实编译并执行 C，不只是检查字符串。
    parser = argparse.ArgumentParser(description="编译并逐值验证最终双 M0 C99 运行时")
    # 基础模型工件目录参数。
    parser.add_argument("--base-artifact-dir", type=Path, required=True)
    # 掩码模型工件目录参数。
    parser.add_argument("--masked-artifact-dir", type=Path, required=True)
    # 已生成两个头文件所在目录参数。
    parser.add_argument("--header-dir", type=Path, required=True)
    # 项目本地临时构建目录参数。
    parser.add_argument("--work-dir", type=Path, required=True)
    # 可选 C 编译器名称或绝对路径，默认从 PATH 查找 gcc。
    parser.add_argument("--compiler", default="gcc")
    # 可选 JSON 报告路径；未提供时只打印标准输出。
    parser.add_argument("--report", type=Path)
    # 返回解析后的不可变语义集合。
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    """运行 C99 验证，写报告并用退出码表达硬门结果。"""
    # args 保存调用方提供的模型、头、临时目录和编译器。
    args = parse_args(argv)
    # report 执行完整编译、运行和逐值比较。
    report = verify_dual_m0_c(
        args.base_artifact_dir,
        args.masked_artifact_dir,
        args.header_dir,
        args.work_dir,
        args.compiler,
    )
    # serialized 使用中文安全 UTF-8 和稳定缩进。
    serialized = json.dumps(report, ensure_ascii=False, indent=2)
    # 标准输出提供 CI 和人工可见结果。
    print(serialized)
    # 提供报告路径时创建父目录并写入末尾换行。
    if args.report is not None:
        # parents=True 支持首次创建 docs/results 或 .codex-local/report。
        args.report.parent.mkdir(parents=True, exist_ok=True)
        # UTF-8 写入机器可读验证证据。
        args.report.write_text(serialized + "\n", encoding="utf-8")
    # 通过返回零，任一误差或类别失败返回一。
    return 0 if bool(report["passed"]) else 1


# 直接运行时进入 CLI；被单元测试导入时不编译或写文件。
if __name__ == "__main__":
    # SystemExit 把硬门结果传递给 PowerShell、CI 或发布脚本。
    raise SystemExit(main())
