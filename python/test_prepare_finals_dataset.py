import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from python.prepare_finals_dataset import prepare_dataset


class PrepareFinalsDatasetTests(unittest.TestCase):
    @staticmethod
    def _write_source(path: Path, rows: int, seed: int) -> str:
        path.write_text(
            "".join(
                f"{seed + index},2,3,4,5,6,{1000 + index},\n"
                for index in range(rows)
            ),
            encoding="utf-8",
        )
        return hashlib.sha256(path.read_bytes()).hexdigest().upper()

    def _write_manifest(self, path: Path, files: list[dict[str, object]]) -> None:
        path.write_text(
            json.dumps(
                {"version": 1, "label": "jumping_squat", "files": files},
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )

    def test_prepare_dataset_copies_verified_roles(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "source"
            output = root / "output"
            source.mkdir()
            entries = []
            for name, role, rows, seed in (
                ("scy1.txt", "extra_train", 3, 10),
                ("scy2.txt", "extra_train", 4, 20),
                ("scy3.txt", "external_holdout", 5, 30),
            ):
                digest = self._write_source(source / name, rows, seed)
                entries.append(
                    {"name": name, "sha256": digest, "rows": rows, "role": role}
                )
            manifest = root / "manifest.json"
            self._write_manifest(manifest, entries)

            prepared = prepare_dataset(manifest, source, output)

            self.assertEqual(prepared["extra_train_count"], 2)
            self.assertEqual(prepared["external_holdout_count"], 1)
            self.assertTrue((output / "train/jumping_squat/scy1.txt").is_file())
            self.assertTrue((output / "train/jumping_squat/scy2.txt").is_file())
            self.assertTrue(
                (output / "external_holdout/jumping_squat/scy3.txt").is_file()
            )

    def test_prepare_dataset_supports_multiple_labels_and_nested_sources(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "source"
            output = root / "output"
            (source / "A类活动").mkdir(parents=True)
            (source / "B类活动").mkdir(parents=True)
            jack_hash = self._write_source(
                source / "A类活动/jumping_jack_scy1_20.txt", 3, 10
            )
            tuck_hash = self._write_source(source / "B类活动/tuck_jump.txt", 4, 20)
            manifest = root / "manifest.json"
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

            prepared = prepare_dataset(manifest, source, output)

            self.assertEqual(prepared["extra_train_count"], 2)
            self.assertEqual(prepared["label_counts"], {"jumping_jack": 1, "tuck_jump": 1})
            self.assertTrue(
                (output / "train/jumping_jack/jumping_jack_scy1_20.txt").is_file()
            )
            self.assertTrue((output / "train/tuck_jump/tuck_jump.txt").is_file())

    def test_prepare_dataset_rejects_altered_hash(self):
        with tempfile.TemporaryDirectory() as temp_dir:
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

            with self.assertRaisesRegex(ValueError, "SHA-256"):
                prepare_dataset(manifest, source, root / "output")

    def test_prepare_dataset_rejects_duplicate_content(self):
        with tempfile.TemporaryDirectory() as temp_dir:
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

            with self.assertRaisesRegex(ValueError, "Duplicate"):
                prepare_dataset(manifest, source, root / "output")


if __name__ == "__main__":
    unittest.main()
