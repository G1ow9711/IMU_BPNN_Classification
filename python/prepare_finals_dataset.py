import argparse
import hashlib
import json
import shutil
from collections import Counter
from pathlib import Path
from typing import Dict


VALID_ROLES = {"extra_train", "external_holdout"}


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def _nonblank_row_count(path: Path) -> int:
    with path.open("r", encoding="utf-8") as file:
        return sum(1 for line in file if line.strip())


def prepare_dataset(
    manifest_path: Path,
    source_dir: Path,
    output_dir: Path,
) -> Dict[str, object]:
    manifest_path = Path(manifest_path)
    source_dir = Path(source_dir)
    output_dir = Path(output_dir)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    default_label = str(manifest.get("label", "")).strip()
    entries = manifest.get("files")
    if not isinstance(entries, list) or not entries:
        raise ValueError("Manifest requires a non-empty files list")

    verified = []
    seen_hashes: Dict[str, str] = {}
    for entry in entries:
        name = str(entry["name"])
        label = str(entry.get("label", default_label)).strip()
        if not label:
            raise ValueError(f"Manifest entry requires a label: {name}")
        source_relative = Path(str(entry.get("source", name)))
        if source_relative.is_absolute() or ".." in source_relative.parts:
            raise ValueError(f"Unsafe source path for {name}: {source_relative}")
        role = str(entry["role"])
        expected_hash = str(entry["sha256"]).upper()
        expected_rows = int(entry["rows"])
        if role not in VALID_ROLES:
            raise ValueError(f"Unsupported role for {name}: {role}")
        source_path = source_dir / source_relative
        if not source_path.is_file():
            raise FileNotFoundError(f"Finals source file not found: {source_path}")
        actual_hash = _sha256(source_path)
        if actual_hash != expected_hash:
            raise ValueError(
                f"SHA-256 mismatch for {name}: expected {expected_hash}, got {actual_hash}"
            )
        previous_name = seen_hashes.get(actual_hash)
        if previous_name is not None:
            raise ValueError(
                f"Duplicate content in manifest: {previous_name} and {name}"
            )
        seen_hashes[actual_hash] = name
        actual_rows = _nonblank_row_count(source_path)
        if actual_rows != expected_rows:
            raise ValueError(
                f"Row count mismatch for {name}: expected {expected_rows}, got {actual_rows}"
            )
        verified.append((source_path, name, label, role, actual_hash, actual_rows))

    prepared_files = []
    role_counts = {"extra_train": 0, "external_holdout": 0}
    label_counts: Counter[str] = Counter()
    for source_path, name, label, role, actual_hash, actual_rows in verified:
        partition = "train" if role == "extra_train" else "external_holdout"
        destination = output_dir / partition / label / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_path, destination)
        role_counts[role] += 1
        label_counts[label] += 1
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

    summary = {
        "extra_train_count": role_counts["extra_train"],
        "external_holdout_count": role_counts["external_holdout"],
        "label_counts": dict(sorted(label_counts.items())),
        "files": prepared_files,
    }
    if len(label_counts) == 1:
        summary["label"] = next(iter(label_counts))
    return summary


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Prepare verified finals IMU sessions")
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).with_name("finals_jumping_squat_manifest.json"),
    )
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    summary = prepare_dataset(args.manifest, args.source_dir, args.output_dir)
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
