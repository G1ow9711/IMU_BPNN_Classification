from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

import numpy as np

try:
    from python import train_export as training
except ModuleNotFoundError:
    import train_export as training


DEFAULT_PAIR_CLASSES = ["jumping_jack", "jumping_lunge", "squat", "tuck_jump"]


def fisher_scores(features: np.ndarray, labels: np.ndarray) -> np.ndarray:
    x = np.asarray(features, dtype=np.float64)
    y = np.asarray(labels, dtype=np.int64)
    overall_mean = np.mean(x, axis=0)
    between = np.zeros(x.shape[1], dtype=np.float64)
    within = np.zeros(x.shape[1], dtype=np.float64)
    for label in np.unique(y):
        group = x[y == label]
        group_mean = np.mean(group, axis=0)
        between += len(group) * np.square(group_mean - overall_mean)
        within += np.sum(np.square(group - group_mean), axis=0)
    return np.divide(
        between,
        within,
        out=np.zeros_like(between),
        where=within > 1e-12,
    )


def cohens_d(target: np.ndarray, other: np.ndarray) -> np.ndarray:
    target_values = np.asarray(target, dtype=np.float64)
    other_values = np.asarray(other, dtype=np.float64)
    target_var = np.var(target_values, axis=0, ddof=1)
    other_var = np.var(other_values, axis=0, ddof=1)
    degrees = len(target_values) + len(other_values) - 2
    if degrees <= 0:
        return np.zeros(target_values.shape[1], dtype=np.float64)
    pooled_var = (
        (len(target_values) - 1) * target_var
        + (len(other_values) - 1) * other_var
    ) / float(degrees)
    denominator = np.sqrt(np.maximum(pooled_var, 0.0))
    difference = np.mean(target_values, axis=0) - np.mean(other_values, axis=0)
    return np.divide(
        difference,
        denominator,
        out=np.zeros_like(difference),
        where=denominator > 1e-12,
    )


def stable_pair_effect(
    train_target: np.ndarray,
    train_other: np.ndarray,
    val_target: np.ndarray,
    val_other: np.ndarray,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    train_effect = cohens_d(train_target, train_other)
    val_effect = cohens_d(val_target, val_other)
    same_direction = np.sign(train_effect) == np.sign(val_effect)
    stable = np.where(
        same_direction,
        np.minimum(np.abs(train_effect), np.abs(val_effect)),
        0.0,
    )
    return stable, train_effect, val_effect


def aggregate_file_medians(
    features: np.ndarray,
    labels: np.ndarray,
    file_ids: np.ndarray,
) -> Tuple[np.ndarray, np.ndarray]:
    medians: List[np.ndarray] = []
    file_labels: List[int] = []
    for file_id in np.unique(file_ids):
        mask = file_ids == file_id
        medians.append(np.median(features[mask], axis=0))
        file_labels.append(int(labels[mask][0]))
    return np.vstack(medians).astype(np.float32), np.asarray(file_labels, dtype=np.int64)


def feature_record(
    index: int,
    names: Sequence[str],
    train_fisher: np.ndarray,
    val_fisher: np.ndarray,
    file_train_fisher: np.ndarray,
    file_val_fisher: np.ndarray,
) -> Dict[str, object]:
    return {
        "index": index,
        "name": names[index],
        "train_fisher": float(train_fisher[index]),
        "val_fisher": float(val_fisher[index]),
        "stable_fisher": float(math.sqrt(train_fisher[index] * val_fisher[index])),
        "file_train_fisher": float(file_train_fisher[index]),
        "file_val_fisher": float(file_val_fisher[index]),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="训练/验证特征类间分离度分析")
    parser.add_argument("--dataset-dir", type=Path, required=True)
    parser.add_argument("--extra-train-dir", type=Path)
    parser.add_argument("--validation-report", type=Path, required=True)
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--output-csv", type=Path, required=True)
    parser.add_argument("--target-class", default="jumping_squat")
    parser.add_argument("--pair-class", action="append", dest="pair_classes")
    parser.add_argument("--top-k", type=int, default=30)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    report = json.loads(args.validation_report.read_text(encoding="utf-8"))
    base_records, class_names, label_to_idx = training.scan_dataset(args.dataset_dir)
    extra_records = (
        training.scan_labeled_dataset(args.extra_train_dir, label_to_idx)
        if args.extra_train_dir is not None
        else []
    )
    train_records, val_records, _ = training.split_records_for_experiment(
        base_records,
        extra_records,
    )
    window_len = int(round(float(report["best_window_seconds"]) * training.SAMPLE_RATE))
    step_len = int(report["all_experiments"][0]["step_len"])
    rest_threshold = float(report["all_experiments"][0]["rest_threshold"])
    active_threshold = float(report["all_experiments"][0]["active_point_threshold"])
    rng = np.random.default_rng(training.SEED)
    train_x, train_y, train_file_ids, train_stats = training.build_samples(
        train_records,
        window_len,
        step_len,
        rest_threshold,
        active_threshold,
        augment=False,
        rng=rng,
        progress_label="separability_train",
    )
    val_x, val_y, val_file_ids, val_stats = training.build_samples(
        val_records,
        window_len,
        step_len,
        rest_threshold,
        active_threshold,
        augment=False,
        rng=rng,
        progress_label="separability_val",
    )

    feature_names = training.build_feature_names()
    train_fisher = fisher_scores(train_x, train_y)
    val_fisher = fisher_scores(val_x, val_y)
    file_train_x, file_train_y = aggregate_file_medians(train_x, train_y, train_file_ids)
    file_val_x, file_val_y = aggregate_file_medians(val_x, val_y, val_file_ids)
    file_train_fisher = fisher_scores(file_train_x, file_train_y)
    file_val_fisher = fisher_scores(file_val_x, file_val_y)
    stable_fisher = np.sqrt(train_fisher * val_fisher)
    ranked_indices = np.argsort(-stable_fisher)

    target_idx = label_to_idx[args.target_class]
    pair_classes = args.pair_classes or DEFAULT_PAIR_CLASSES
    pair_reports: Dict[str, object] = {}
    event_start = len(feature_names) - len(training.EVENT_FEATURE_NAMES)
    event_indices = list(range(event_start, len(feature_names)))
    for pair_class in pair_classes:
        other_idx = label_to_idx[pair_class]
        stable, train_effect, val_effect = stable_pair_effect(
            train_x[train_y == target_idx],
            train_x[train_y == other_idx],
            val_x[val_y == target_idx],
            val_x[val_y == other_idx],
        )
        pair_ranked = np.argsort(-stable)

        def effect_record(index: int) -> Dict[str, object]:
            return {
                "index": int(index),
                "name": feature_names[index],
                "stable_abs_effect": float(stable[index]),
                "train_effect": float(train_effect[index]),
                "val_effect": float(val_effect[index]),
            }

        pair_reports[pair_class] = {
            "top_features": [effect_record(int(i)) for i in pair_ranked[: args.top_k]],
            "event_features": [
                effect_record(i)
                for i in sorted(event_indices, key=lambda item: stable[item], reverse=True)
            ],
            "event_features_stable_ge_0_5": int(
                np.sum(stable[event_indices] >= 0.5)
            ),
        }

    correlation = np.corrcoef(train_x, rowvar=False)
    event_novelty = []
    for index in event_indices:
        prior_correlations = np.abs(correlation[index, :event_start])
        finite_correlations = prior_correlations[np.isfinite(prior_correlations)]
        event_novelty.append(
            {
                "index": index,
                "name": feature_names[index],
                "max_abs_correlation_with_prior_features": float(
                    np.max(finite_correlations) if len(finite_correlations) else 0.0
                ),
            }
        )

    top_features = [
        feature_record(
            int(index),
            feature_names,
            train_fisher,
            val_fisher,
            file_train_fisher,
            file_val_fisher,
        )
        for index in ranked_indices[: args.top_k]
    ]
    event_features = [
        feature_record(
            index,
            feature_names,
            train_fisher,
            val_fisher,
            file_train_fisher,
            file_val_fisher,
        )
        for index in sorted(event_indices, key=lambda item: stable_fisher[item], reverse=True)
    ]
    result = {
        "scope": "train_validation_only",
        "test_read": False,
        "external_holdout_read": False,
        "target_class": args.target_class,
        "class_names": class_names,
        "feature_count": len(feature_names),
        "event_feature_count": len(event_indices),
        "train_window_count": len(train_x),
        "val_window_count": len(val_x),
        "train_file_count": len(train_records),
        "val_file_count": len(val_records),
        "train_stats": train_stats,
        "val_stats": val_stats,
        "top_features": top_features,
        "event_features": event_features,
        "event_novelty": event_novelty,
        "target_pair_effects": pair_reports,
    }
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(
        json.dumps(result, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.output_csv.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(top_features[0].keys()))
        writer.writeheader()
        writer.writerows(top_features)
    print(
        f"SEPARABILITY_OK train={len(train_x)} val={len(val_x)} "
        f"features={len(feature_names)} output={args.output_json}",
        flush=True,
    )


if __name__ == "__main__":
    main()
