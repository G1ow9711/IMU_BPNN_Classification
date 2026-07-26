"""按可审计清单准备决赛补充数据，不在验证完成前写入输出目录。

输入清单的每个文件必须提供类别、角色、SHA-256 和非空行数。源文件每行是一个六轴
IMU 采样点，训练链固定解释为 ``gx、gy、gz、ax、ay、az``；角速度单位为 ``deg/s``，
加速度单位为 ``g``。本模块不解析或改写数值，只进行字节级摘要和文件级行数校验。

最重要的事务语义是“先全部校验，后统一复制”：任一文件缺失、摘要漂移、行数错误、
路径越界、角色非法或内容重复时，函数在创建训练/留出文件前失败。这样能阻止部分成功
造成的数据集污染，也保证 ``external_holdout`` 不会误入 ``train``。
"""

import argparse
import hashlib
import json
import shutil
from collections import Counter
from pathlib import Path
from typing import Dict


# 清单只允许补充训练和外部留出两种角色，测试集不接受脚本隐式生成。
VALID_ROLES = {"extra_train", "external_holdout"}


def _sha256(path: Path) -> str:
    """流式计算文件 SHA-256，并返回与清单格式一致的大写十六进制摘要。

    每次最多读取 1 MiB，因此空间复杂度为 ``O(1)``，时间复杂度为文件字节数 ``O(B)``。
    摘要用于证明待复制字节与人工审核清单一致，不表示 IMU 内容本身合理。
    """
    # digest 保存增量 SHA-256 状态，避免把大型采集文件一次读入内存。
    digest = hashlib.sha256()
    # 二进制方式读取可保证换行符和编码不会在校验时被 Python 改写。
    with path.open("rb") as file:
        # 空字节串是迭代终止标记，每轮读取上限为 1 MiB。
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            # 按原始字节顺序更新摘要，结果与 sha256sum 等标准工具一致。
            digest.update(chunk)
    # 清单统一使用大写字符，比较时不受十六进制字母大小写影响。
    return digest.hexdigest().upper()


def _nonblank_row_count(path: Path) -> int:
    """统计 UTF-8 IMU 文件的非空采样行数，空白行不计入清单合同。"""
    # 文本以 UTF-8 严格解码；无效编码直接失败，避免复制无法被训练端读取的文件。
    with path.open("r", encoding="utf-8") as file:
        # 只保留 strip 后非空的行，时间复杂度为行数 O(N)，额外空间为 O(1)。
        return sum(1 for line in file if line.strip())


def prepare_dataset(
    manifest_path: Path,
    source_dir: Path,
    output_dir: Path,
) -> Dict[str, object]:
    """校验清单中全部源文件后，按角色和类别复制并返回可审计摘要。

    ``manifest_path`` 指向 UTF-8 JSON；``source_dir`` 是只读采集根目录；
    ``output_dir`` 按 ``train/<label>`` 与 ``external_holdout/<label>`` 建目录。
    返回字典记录各角色、各类别数量及每个目标绝对路径。函数不解析六轴数值，也不重排
    ``gx、gy、gz、ax、ay、az`` 通道。

    安全边界：相对源路径不得为绝对路径或包含 ``..``；同一摘要只能出现一次；所有条目
    先进入 ``verified``，第二轮才复制。因此一次失败不会留下半套新数据。
    """
    # 统一转换为 Path，兼容调用方传入字符串或 PathLike 对象。
    manifest_path = Path(manifest_path)
    # source_dir 仅用于定位和读取清单声明的源文件。
    source_dir = Path(source_dir)
    # output_dir 仅在全部条目通过后用于创建正式训练/留出目录。
    output_dir = Path(output_dir)
    # 读取 UTF-8 JSON；格式错误由 json 模块直接报告，禁止容错猜测字段。
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    # version 1 清单可在顶层给出默认类别；条目级 label 优先。
    default_label = str(manifest.get("label", "")).strip()
    # files 必须是非空列表，否则没有可复现的数据准备对象。
    entries = manifest.get("files")
    # 空清单不创建目录，也不返回伪成功摘要。
    if not isinstance(entries, list) or not entries:
        raise ValueError("Manifest requires a non-empty files list")

    # verified 暂存已核验元数据；此阶段绝不复制，形成先验后写的事务边界。
    verified = []
    # seen_hashes 以摘要为键检测同内容改名，避免同一会话跨训练/留出泄漏。
    seen_hashes: Dict[str, str] = {}
    # 第一轮遍历只读取和校验清单条目及源文件。
    for entry in entries:
        # name 是目标文件名，也是错误消息中的稳定人工标识。
        name = str(entry["name"])
        # 条目类别优先，缺失时回退到旧版清单的顶层默认类别。
        label = str(entry.get("label", default_label)).strip()
        # 无类别的文件不能放入监督学习目录。
        if not label:
            raise ValueError(f"Manifest entry requires a label: {name}")
        # source 可指向源根目录下的中文嵌套路径；缺失时与目标 name 相同。
        source_relative = Path(str(entry.get("source", name)))
        # 拒绝绝对路径和父目录跳转，保证读取范围限制在 source_dir。
        if source_relative.is_absolute() or ".." in source_relative.parts:
            raise ValueError(f"Unsafe source path for {name}: {source_relative}")
        # role 决定训练或外部留出分区，不允许调用方自由拼目标路径。
        role = str(entry["role"])
        # 摘要转为大写，与 _sha256 输出进行确定性比较。
        expected_hash = str(entry["sha256"]).upper()
        # rows 是人工清单声明的非空采样点数量。
        expected_rows = int(entry["rows"])
        # 角色白名单防止拼写错误把留出数据静默写入训练目录。
        if role not in VALID_ROLES:
            raise ValueError(f"Unsupported role for {name}: {role}")
        # 相对路径只在已限制的 source_dir 下解析。
        source_path = source_dir / source_relative
        # 缺文件立即失败，且此时尚未创建任何输出文件。
        if not source_path.is_file():
            raise FileNotFoundError(f"Finals source file not found: {source_path}")
        # 对实际源字节计算 SHA-256，检测采集文件被修改或选错。
        actual_hash = _sha256(source_path)
        # 摘要不一致时拒绝继续，避免把未审计数据混入训练。
        if actual_hash != expected_hash:
            raise ValueError(
                f"SHA-256 mismatch for {name}: expected {expected_hash}, got {actual_hash}"
            )
        # 检查同一摘要此前是否已经以另一个名称登记。
        previous_name = seen_hashes.get(actual_hash)
        # 重复内容可能跨分区造成数据泄漏，因此必须失败而不是跳过。
        if previous_name is not None:
            raise ValueError(
                f"Duplicate content in manifest: {previous_name} and {name}"
            )
        # 仅在确认唯一后登记摘要和名称。
        seen_hashes[actual_hash] = name
        # 非空行数近似采样点数，可发现截断或意外追加。
        actual_rows = _nonblank_row_count(source_path)
        # 行数不一致时保持输出目录不变。
        if actual_rows != expected_rows:
            raise ValueError(
                f"Row count mismatch for {name}: expected {expected_rows}, got {actual_rows}"
            )
        # 保存复制所需的已验证事实，第二轮不再重新解释清单。
        verified.append((source_path, name, label, role, actual_hash, actual_rows))

    # prepared_files 记录实际复制结果，供调用方保存审计清单。
    prepared_files = []
    # role_counts 分别累计补充训练文件和外部留出文件数量。
    role_counts = {"extra_train": 0, "external_holdout": 0}
    # label_counts 统计各监督类别文件数，便于发现类别不平衡。
    label_counts: Counter[str] = Counter()
    # 第二轮只处理第一轮已全部验证的不可变元组。
    for source_path, name, label, role, actual_hash, actual_rows in verified:
        # extra_train 映射到 train，其余合法角色只能是 external_holdout。
        partition = "train" if role == "extra_train" else "external_holdout"
        # 目标结构固定为 output/partition/label/name，便于训练器扫描。
        destination = output_dir / partition / label / name
        # 递归创建类别目录；exist_ok 允许重复执行相同已验证清单。
        destination.parent.mkdir(parents=True, exist_ok=True)
        # copy2 保留源文件时间等元数据，文件内容按字节复制不做单位或列变换。
        shutil.copy2(source_path, destination)
        # 复制成功后才更新角色计数，摘要与文件结果保持一致。
        role_counts[role] += 1
        # 复制成功后累计类别数量。
        label_counts[label] += 1
        # 为当前目标附加可追溯事实。
        prepared_files.append(
            {
                "name": name,
                "label": label,
                "role": role,
                "sha256": actual_hash,
                "rows": actual_rows,
                "destination": str(destination.resolve()),
            }
        )

    # summary 是调用方可直接序列化的准备结果，不包含源文件数据。
    summary = {
        "extra_train_count": role_counts["extra_train"],
        "external_holdout_count": role_counts["external_holdout"],
        "label_counts": dict(sorted(label_counts.items())),
        "files": prepared_files,
    }
    # 单类别旧版调用保留顶层 label 字段，多类别时依赖 label_counts。
    if len(label_counts) == 1:
        # next 读取唯一类别键，不引入无意义排序。
        summary["label"] = next(iter(label_counts))
    # 返回已复制文件的完整摘要，供测试和后续清单记录。
    return summary


def parse_args() -> argparse.Namespace:
    """解析数据准备命令行；路径保持为 Path，便于 Windows 中文目录使用。"""
    # 中文说明明确该入口会先校验再复制，不是普通文件同步工具。
    parser = argparse.ArgumentParser(description="按清单校验并准备决赛六轴 IMU 会话")
    # manifest 默认指向脚本同目录的跳跃深蹲历史清单，也可显式替换为多类别清单。
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).with_name("finals_jumping_squat_manifest.json"),
    )
    # source-dir 是人工采集文件的只读根目录。
    parser.add_argument("--source-dir", type=Path, required=True)
    # output-dir 是通过校验后才写入的标准数据集根目录。
    parser.add_argument("--output-dir", type=Path, required=True)
    # 返回 argparse 命名空间，由 main 统一调用纯业务函数。
    return parser.parse_args()


def main() -> None:
    """执行清单准备并把 UTF-8 中文可读摘要输出到标准输出。"""
    # 解析命令行路径，不在入口中重复实现校验逻辑。
    args = parse_args()
    # prepare_dataset 执行先全量校验、后统一复制的事务流程。
    summary = prepare_dataset(args.manifest, args.source_dir, args.output_dir)
    # ensure_ascii=False 保留中文类别与路径，indent=2 便于教程读者审计。
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
