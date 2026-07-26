"""验证决赛补充数据严格遵循“清单先全量校验、后统一复制”的事务合同。

测试源文件模拟六轴 IMU 文本：每行前六列对应 ``gx、gy、gz、ax、ay、az``，最后一列
模拟时间戳。准备器不解释数值单位，只保证字节摘要、非空行数、类别、角色和目标目录
可追溯；尤其禁止相同文件同时进入训练与外部留出分区。
"""

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from python.prepare_finals_dataset import prepare_dataset


class PrepareFinalsDatasetTests(unittest.TestCase):
    """覆盖合法复制、多类别嵌套路径、摘要漂移和跨分区重复内容。"""

    @staticmethod
    def _write_source(path: Path, rows: int, seed: int) -> str:
        """写入确定性六轴样本并返回大写 SHA-256，供清单声明使用。"""
        # 每个非空文本行模拟一个 IMU 采样点，seed 让不同测试文件字节内容不同。
        path.write_text(
            "".join(
                f"{seed + index},2,3,4,5,6,{1000 + index},\n"
                for index in range(rows)
            ),
            encoding="utf-8",
        )
        # 从落盘字节计算摘要，确保测试与生产校验路径一致。
        return hashlib.sha256(path.read_bytes()).hexdigest().upper()

    def _write_manifest(self, path: Path, files: list[dict[str, object]]) -> None:
        """写入带默认跳跃深蹲标签的旧版清单，条目由具体测试提供。"""
        # ensure_ascii=False 保留未来中文路径；UTF-8 与生产解析器一致。
        path.write_text(
            json.dumps(
                {"version": 1, "label": "jumping_squat", "files": files},
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )

    def test_prepare_dataset_copies_verified_roles(self):
        """全部摘要和行数通过后，训练与外部留出文件应进入独立目录。"""
        # 临时目录隔离文件系统副作用，测试结束自动清理。
        with tempfile.TemporaryDirectory() as temp_dir:
            # root 是本测试的独立清单、源文件和输出根目录。
            root = Path(temp_dir)
            source = root / "source"
            output = root / "output"
            # 源目录由测试显式创建，准备器不得替调用方创建采集根。
            source.mkdir()
            # entries 按实际落盘摘要和行数构造。
            entries = []
            # 前两份补训练，第三份作为从未参与训练的外部留出。
            for name, role, rows, seed in (
                ("scy1.txt", "extra_train", 3, 10),
                ("scy2.txt", "extra_train", 4, 20),
                ("scy3.txt", "external_holdout", 5, 30),
            ):
                # 先写源文件，再把真实摘要登记到清单。
                digest = self._write_source(source / name, rows, seed)
                entries.append(
                    {"name": name, "sha256": digest, "rows": rows, "role": role}
                )
            manifest = root / "manifest.json"
            self._write_manifest(manifest, entries)

            # 调用生产准备器；只有三份文件全部验证通过后才开始复制。
            prepared = prepare_dataset(manifest, source, output)

            # 汇总计数和三个目标路径共同锁定角色分区语义。
            self.assertEqual(prepared["extra_train_count"], 2)
            self.assertEqual(prepared["external_holdout_count"], 1)
            self.assertTrue((output / "train/jumping_squat/scy1.txt").is_file())
            self.assertTrue((output / "train/jumping_squat/scy2.txt").is_file())
            self.assertTrue(
                (output / "external_holdout/jumping_squat/scy3.txt").is_file()
            )

    def test_prepare_dataset_supports_multiple_labels_and_nested_sources(self):
        """多类别清单应安全读取中文嵌套源路径，并按标签建立训练目录。"""
        with tempfile.TemporaryDirectory() as temp_dir:
            # 为两类动作创建不同中文源目录，验证 Windows 中文路径合同。
            root = Path(temp_dir)
            source = root / "source"
            output = root / "output"
            (source / "A类活动").mkdir(parents=True)
            (source / "B类活动").mkdir(parents=True)
            # 两份文件内容、类别和摘要均独立。
            jack_hash = self._write_source(
                source / "A类活动/jumping_jack_scy1_20.txt", 3, 10
            )
            tuck_hash = self._write_source(source / "B类活动/tuck_jump.txt", 4, 20)
            manifest = root / "manifest.json"
            # version 2 使用条目级 label/source，不依赖顶层默认类别。
            manifest.write_text(
                json.dumps(
                    {
                        "version": 2,
                        "files": [
                            {
                                "label": "jumping_jack",
                                "source": "A类活动/jumping_jack_scy1_20.txt",
                                "name": "jumping_jack_scy1_20.txt",
                                "sha256": jack_hash,
                                "rows": 3,
                                "role": "extra_train",
                            },
                            {
                                "label": "tuck_jump",
                                "source": "B类活动/tuck_jump.txt",
                                "name": "tuck_jump.txt",
                                "sha256": tuck_hash,
                                "rows": 4,
                                "role": "extra_train",
                            },
                        ],
                    },
                    ensure_ascii=False,
                ),
                encoding="utf-8",
            )

            # 全部条目验证完成后复制到两个类别目录。
            prepared = prepare_dataset(manifest, source, output)

            # 汇总必须保留两个类别，且源目录名不能泄漏到正式目标结构。
            self.assertEqual(prepared["extra_train_count"], 2)
            self.assertEqual(prepared["label_counts"], {"jumping_jack": 1, "tuck_jump": 1})
            self.assertTrue(
                (output / "train/jumping_jack/jumping_jack_scy1_20.txt").is_file()
            )
            self.assertTrue((output / "train/tuck_jump/tuck_jump.txt").is_file())

    def test_prepare_dataset_rejects_altered_hash(self):
        """任一 SHA-256 不一致时应失败，且不能接受未经清单审核的字节。"""
        with tempfile.TemporaryDirectory() as temp_dir:
            # 写入真实源文件，但在清单中故意声明全零伪摘要。
            root = Path(temp_dir)
            source = root / "source"
            source.mkdir()
            self._write_source(source / "scy1.txt", 2, 10)
            manifest = root / "manifest.json"
            self._write_manifest(
                manifest,
                [
                    {
                        "name": "scy1.txt",
                        "sha256": "0" * 64,
                        "rows": 2,
                        "role": "extra_train",
                    }
                ],
            )

            # 摘要门必须在复制阶段之前抛出可定位错误。
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                prepare_dataset(manifest, source, root / "output")

    def test_prepare_dataset_rejects_duplicate_content(self):
        """同内容改名并跨训练/留出登记时必须失败，防止文件级数据泄漏。"""
        with tempfile.TemporaryDirectory() as temp_dir:
            # scy2 逐字节复制 scy1，两个名称共享同一真实摘要。
            root = Path(temp_dir)
            source = root / "source"
            source.mkdir()
            digest = self._write_source(source / "scy1.txt", 2, 10)
            (source / "scy2.txt").write_bytes((source / "scy1.txt").read_bytes())
            manifest = root / "manifest.json"
            self._write_manifest(
                manifest,
                [
                    {
                        "name": "scy1.txt",
                        "sha256": digest,
                        "rows": 2,
                        "role": "extra_train",
                    },
                    {
                        "name": "scy2.txt",
                        "sha256": digest,
                        "rows": 2,
                        "role": "external_holdout",
                    },
                ],
            )

            # 即使两个角色和文件名不同，摘要重复仍应在任何复制前被拒绝。
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                prepare_dataset(manifest, source, root / "output")


if __name__ == "__main__":
    unittest.main()
