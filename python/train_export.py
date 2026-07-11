import argparse
import copy
import json
import math
import os
import random
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import torch
from sklearn.metrics import accuracy_score, classification_report, confusion_matrix, f1_score
from sklearn.model_selection import train_test_split
from torch import nn
from torch.nn import functional as F
from torch.utils.data import DataLoader, TensorDataset, WeightedRandomSampler


SEED = 20260709
PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DATASET_DIR = Path("IMU_Dataset") / "imu_dataset_for_final"
FALLBACK_DATASET_DIR = Path("imu_dataset_for_final")
OUTPUT_DIR = Path("outputs")
ESP32_MODEL_HEADER = PROJECT_ROOT / "esp32" / "include" / "esp32_bp_model.h"

SAMPLE_RATE = 25
STEP_SECONDS = 0.5
WINDOW_SECONDS_LIST = (1.5, 2.0, 2.5)
TRAIN_RATIO = 0.70
VAL_RATIO = 0.15
TEST_RATIO = 0.15
SIT_CLASS_NAME = "sit"
HIGH_DYNAMIC_CLASSES = {"jumping_jack", "jumping_lunge", "jumping_squat", "tuck_jump"}
WEAK_CLASS_NAMES = ["jumping_squat", "squat", "tuck_jump", "jumping_lunge"]
FAMILY_SPECIALIST_CLASS_NAMES = [
    "jumping_jack",
    "jumping_lunge",
    "jumping_squat",
    "tuck_jump",
]
HARD_CONFUSION_PAIRS = {
    "squat": ["jumping_squat"],
    "jumping_squat": ["squat", "jumping_jack", "tuck_jump"],
    "tuck_jump": ["jumping_lunge", "jumping_squat"],
    "jumping_lunge": ["tuck_jump", "jumping_squat"],
}
TARGET_MIN_CLASS_RECALL = 0.90

MAX_EPOCHS = 350
BATCH_SIZE = 64
LEARNING_RATE = 1e-3
WEIGHT_DECAY = 1e-4
PATIENCE = 45
DROPOUT = 0.10
AUGMENT_TIMES = 2
MAX_ROTATION_DEGREES = 35.0
EXPORT_WHEN_BELOW_TARGET = False
SUPCON_WEIGHT = 0.05
HARD_PAIR_WEIGHT = 0.25

CHANNEL_NAMES = ["gx", "gy", "gz", "ax", "ay", "az"]
MAG_NAMES = ["gyro_mag", "acc_mag"]
DELTA_MAG_NAMES = ["gyro_delta_mag", "acc_delta_mag"]
GRAVITY_NAMES = [
    "acc_vertical",
    "acc_horizontal_mag",
    "gyro_vertical",
    "gyro_horizontal_mag",
]
GLOBAL_SERIES_NAMES = CHANNEL_NAMES + MAG_NAMES + DELTA_MAG_NAMES + GRAVITY_NAMES
PHASE_SOURCE_NAMES = ["acc_vertical", "acc_horizontal_mag", "gyro_mag", "acc_delta_mag"]
ONE_SERIES_FEATURES = [
    "mean",
    "std",
    "min",
    "max",
    "rms",
    "mean_abs_diff",
    "zcr",
    "std_diff",
]
PHASE_SEGMENTS = 4
PHASE_FEATURES = ["mean", "std", "max_abs"]
TEMPORAL_FEATURES = [
    "high_activity_ratio",
    "peak_count_normalized",
    "dominant_frequency_hz",
    "spectral_entropy",
    "autocorr_peak",
    "autocorr_peak_lag_seconds",
]
IMPACT_DISTRIBUTION_FEATURES = [
    "q10",
    "q25",
    "q50",
    "q75",
    "q90",
    "skew",
    "excess_kurtosis",
    "max_abs_diff",
]
EVENT_FEATURE_NAMES = [
    "event_free_flight_ratio",
    "event_longest_free_flight_run_ratio",
    "event_gyro_vertical_correlation",
]

HIDDEN1 = 96
HIDDEN2 = 64
HIDDEN3 = 32


@dataclass(frozen=True)
class ImuRecord:
    path: Path
    label: str
    label_idx: int


class BPNet(nn.Module):
    def __init__(self, input_dim: int, class_count: int, dropout: float = DROPOUT):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(input_dim, HIDDEN1),
            nn.ReLU(),
            nn.Dropout(dropout),
            nn.Linear(HIDDEN1, HIDDEN2),
            nn.ReLU(),
            nn.Dropout(dropout),
            nn.Linear(HIDDEN2, HIDDEN3),
            nn.ReLU(),
            nn.Linear(HIDDEN3, class_count),
        )

    def forward_features(self, x: torch.Tensor) -> torch.Tensor:
        for layer in list(self.net.children())[:8]:
            x = layer(x)
        return x

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.net[8](self.forward_features(x))


def cross_file_supervised_contrastive_loss(
    embeddings: torch.Tensor,
    labels: torch.Tensor,
    file_ids: torch.Tensor,
    temperature: float = 0.15,
) -> torch.Tensor:
    if len(embeddings) < 2:
        return embeddings.sum() * 0.0
    normalized = F.normalize(embeddings, dim=1)
    logits = normalized @ normalized.T / temperature
    losses: List[torch.Tensor] = []
    for anchor in range(len(embeddings)):
        different_sample = torch.arange(len(embeddings), device=embeddings.device) != anchor
        positive = (
            different_sample
            & (labels == labels[anchor])
            & (file_ids != file_ids[anchor])
        )
        valid = different_sample & (
            (labels != labels[anchor]) | (file_ids != file_ids[anchor])
        )
        if not torch.any(positive):
            continue
        denominator = torch.logsumexp(logits[anchor][valid], dim=0)
        losses.append(-(logits[anchor][positive] - denominator).mean())
    if not losses:
        return embeddings.sum() * 0.0
    return torch.stack(losses).mean()


def hard_pair_margin_loss(
    logits: torch.Tensor,
    labels: torch.Tensor,
    class_names: Sequence[str],
    margin: float = 0.5,
) -> torch.Tensor:
    name_to_idx = {name: index for index, name in enumerate(class_names)}
    losses: List[torch.Tensor] = []
    for true_name, confusing_names in HARD_CONFUSION_PAIRS.items():
        if true_name not in name_to_idx:
            continue
        true_idx = name_to_idx[true_name]
        sample_mask = labels == true_idx
        if not torch.any(sample_mask):
            continue
        true_logits = logits[sample_mask, true_idx]
        for confusing_name in confusing_names:
            if confusing_name not in name_to_idx:
                continue
            confusing_idx = name_to_idx[confusing_name]
            confusing_logits = logits[sample_mask, confusing_idx]
            losses.append(F.relu(margin - true_logits + confusing_logits).mean())
    if not losses:
        return logits.sum() * 0.0
    return torch.stack(losses).mean()


def set_seed(seed: int) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    torch.cuda.manual_seed_all(seed)
    torch.backends.cudnn.deterministic = True
    torch.backends.cudnn.benchmark = False


def resolve_dataset_dir(dataset_dir: Optional[Path]) -> Path:
    candidates = []
    if dataset_dir is not None:
        candidates.append(dataset_dir)
    candidates.extend([DEFAULT_DATASET_DIR, FALLBACK_DATASET_DIR])
    for candidate in candidates:
        if candidate.exists() and candidate.is_dir():
            return candidate
    raise FileNotFoundError(
        "Dataset directory not found. Tried: "
        + ", ".join(str(candidate) for candidate in candidates)
    )


def scan_dataset(dataset_dir: Path) -> Tuple[List[ImuRecord], List[str], Dict[str, int]]:
    class_dirs = sorted(
        [path for path in dataset_dir.iterdir() if path.is_dir() and list(path.glob("*.txt"))]
    )
    if not class_dirs:
        raise ValueError(f"No action folders with txt files found under {dataset_dir}")

    class_names = [path.name for path in class_dirs]
    label_to_idx = {name: idx for idx, name in enumerate(class_names)}
    records: List[ImuRecord] = []
    for class_dir in class_dirs:
        for txt_path in sorted(class_dir.glob("*.txt")):
            records.append(ImuRecord(txt_path, class_dir.name, label_to_idx[class_dir.name]))
    if not records:
        raise ValueError(f"No dataset txt files found under {dataset_dir}")
    return records, class_names, label_to_idx


def scan_labeled_dataset(
    dataset_dir: Path,
    label_to_idx: Dict[str, int],
) -> List[ImuRecord]:
    dataset_dir = Path(dataset_dir)
    if not dataset_dir.is_dir():
        raise FileNotFoundError(f"Additional dataset directory not found: {dataset_dir}")
    records: List[ImuRecord] = []
    for class_dir in sorted(path for path in dataset_dir.iterdir() if path.is_dir()):
        txt_paths = sorted(class_dir.glob("*.txt"))
        if not txt_paths:
            continue
        label = class_dir.name
        if label not in label_to_idx:
            raise ValueError(f"Unknown action directory in additional dataset: {label}")
        records.extend(
            ImuRecord(path, label, label_to_idx[label]) for path in txt_paths
        )
    if not records:
        raise ValueError(f"No labeled txt files found under {dataset_dir}")
    return records


def load_additional_records(
    extra_train_dir: Optional[Path],
    external_holdout_dir: Optional[Path],
    label_to_idx: Dict[str, int],
    validation_only: bool,
) -> Tuple[List[ImuRecord], List[ImuRecord]]:
    extra_records = (
        scan_labeled_dataset(extra_train_dir, label_to_idx)
        if extra_train_dir is not None
        else []
    )
    holdout_records = (
        scan_labeled_dataset(external_holdout_dir, label_to_idx)
        if external_holdout_dir is not None and not validation_only
        else []
    )
    return extra_records, holdout_records


def convert_raw_imu_units(raw: np.ndarray) -> np.ndarray:
    data = np.asarray(raw, dtype=np.float32)
    if data.ndim == 1:
        data = data.reshape(1, -1)
    if data.shape[1] < 6:
        raise ValueError(f"Expected at least 6 columns, got {data.shape[1]}")
    converted = data[:, :6].astype(np.float32, copy=True)
    converted[:, 0:3] = converted[:, 0:3] / 16.4
    converted[:, 3:6] = converted[:, 3:6] / 4096.0
    return converted


def load_imu_file(path: Path) -> np.ndarray:
    raw = np.loadtxt(
        path,
        delimiter=",",
        dtype=np.float32,
        usecols=tuple(range(6)),
    )
    return convert_raw_imu_units(raw)


def window_lengths(window_seconds: float) -> Tuple[int, int]:
    window_len = int(round(window_seconds * SAMPLE_RATE))
    step_len = int(round(STEP_SECONDS * SAMPLE_RATE))
    return window_len, max(1, step_len)


def iter_windows(data: np.ndarray, window_len: int, step_len: int) -> Iterable[np.ndarray]:
    if len(data) < window_len:
        return
    for start in range(0, len(data) - window_len + 1, step_len):
        yield data[start : start + window_len]


def motion_score(window: np.ndarray) -> float:
    gyro_mag = np.linalg.norm(window[:, 0:3], axis=1)
    acc_mag = np.linalg.norm(window[:, 3:6], axis=1)
    return float(np.std(acc_mag) + np.std(gyro_mag) / 200.0)


def instantaneous_motion(window: np.ndarray) -> np.ndarray:
    data = np.asarray(window, dtype=np.float32)
    gyro_mag = np.linalg.norm(data[:, 0:3], axis=1)
    acc_delta = np.linalg.norm(np.diff(data[:, 3:6], axis=0), axis=1)
    acc_delta = np.concatenate([np.zeros(1, dtype=np.float32), acc_delta.astype(np.float32)])
    return (acc_delta + gyro_mag / 200.0).astype(np.float32)


def active_ratio(window: np.ndarray, active_point_threshold: float) -> float:
    scores = instantaneous_motion(window)
    return float(np.mean(scores > active_point_threshold))


def keep_window_for_label(
    window: np.ndarray,
    label: str,
    rest_threshold: float,
    active_point_threshold: float,
) -> bool:
    score = motion_score(window)
    if label == SIT_CLASS_NAME:
        return score <= rest_threshold * 1.6
    if score < rest_threshold:
        return False
    if label in HIGH_DYNAMIC_CLASSES:
        return (
            score >= rest_threshold * 1.25
            and active_ratio(window, active_point_threshold) >= 0.20
        )
    return True


def file_balanced_sample_weights(labels: np.ndarray, file_ids: np.ndarray) -> np.ndarray:
    y = np.asarray(labels, dtype=np.int64)
    groups = np.asarray(file_ids, dtype=np.int64)
    if y.shape != groups.shape:
        raise ValueError(f"labels and file_ids must share shape, got {y.shape} and {groups.shape}")
    if len(y) == 0:
        return np.empty(0, dtype=np.float64)

    pair_counts: Dict[Tuple[int, int], int] = {}
    class_files: Dict[int, set[int]] = {}
    for label, file_id in zip(y.tolist(), groups.tolist()):
        pair = (label, file_id)
        pair_counts[pair] = pair_counts.get(pair, 0) + 1
        class_files.setdefault(label, set()).add(file_id)
    return np.asarray(
        [
            1.0 / (len(class_files[label]) * pair_counts[(label, file_id)])
            for label, file_id in zip(y.tolist(), groups.tolist())
        ],
        dtype=np.float64,
    )


def estimate_rest_threshold(
    records: Sequence[ImuRecord],
    window_len: int,
    step_len: int,
    percentile: float = 85.0,
) -> float:
    scores: List[float] = []
    for record in records:
        if record.label != SIT_CLASS_NAME:
            continue
        data = load_imu_file(record.path)
        scores.extend(motion_score(window) for window in iter_windows(data, window_len, step_len))
    if not scores:
        return 0.03
    threshold = float(np.percentile(np.asarray(scores, dtype=np.float32), percentile))
    return max(threshold, 0.01)


def estimate_active_point_threshold(
    records: Sequence[ImuRecord],
    window_len: int,
    step_len: int,
    percentile: float = 90.0,
) -> float:
    scores: List[float] = []
    for record in records:
        if record.label != SIT_CLASS_NAME:
            continue
        data = load_imu_file(record.path)
        for window in iter_windows(data, window_len, step_len):
            scores.extend(instantaneous_motion(window).tolist())
    if not scores:
        return 0.02
    threshold = float(np.percentile(np.asarray(scores, dtype=np.float32), percentile))
    return max(threshold, 0.005)


def series_features(values: np.ndarray) -> List[float]:
    x = np.asarray(values, dtype=np.float32)
    mean = float(np.mean(x))
    std = float(np.std(x))
    min_v = float(np.min(x))
    max_v = float(np.max(x))
    energy = float(np.mean(x * x))
    centered = x - mean
    if len(x) > 1:
        diffs = np.diff(x)
        abs_diffs = np.abs(diffs)
        mean_abs_diff = float(np.mean(abs_diffs))
        std_diff = float(np.std(diffs))
        sign_product = centered[:-1] * centered[1:]
        zcr = float(np.mean(sign_product < 0.0))
    else:
        mean_abs_diff = 0.0
        std_diff = 0.0
        zcr = 0.0
    return [
        mean,
        std,
        min_v,
        max_v,
        math.sqrt(max(energy, 0.0)),
        mean_abs_diff,
        zcr,
        std_diff,
    ]


def gravity_aligned_series(window: np.ndarray) -> Tuple[np.ndarray, ...]:
    data = np.asarray(window, dtype=np.float32)
    gravity = np.mean(data[:, 3:6], axis=0)
    gravity_norm = float(np.linalg.norm(gravity))
    if gravity_norm < 1e-6:
        gravity_unit = np.asarray([0.0, 0.0, 1.0], dtype=np.float32)
    else:
        gravity_unit = (gravity / gravity_norm).astype(np.float32)

    acc_vertical = data[:, 3:6] @ gravity_unit
    gyro_vertical = data[:, 0:3] @ gravity_unit
    acc_squared = np.sum(data[:, 3:6] * data[:, 3:6], axis=1)
    gyro_squared = np.sum(data[:, 0:3] * data[:, 0:3], axis=1)
    acc_horizontal = np.sqrt(np.maximum(acc_squared - acc_vertical * acc_vertical, 0.0))
    gyro_horizontal = np.sqrt(np.maximum(gyro_squared - gyro_vertical * gyro_vertical, 0.0))
    return (
        acc_vertical.astype(np.float32),
        acc_horizontal.astype(np.float32),
        gyro_vertical.astype(np.float32),
        gyro_horizontal.astype(np.float32),
    )


def phase_features(values: np.ndarray) -> List[float]:
    x = np.asarray(values, dtype=np.float32)
    result: List[float] = []
    for phase in range(PHASE_SEGMENTS):
        start = (phase * len(x)) // PHASE_SEGMENTS
        end = ((phase + 1) * len(x)) // PHASE_SEGMENTS
        segment = x[start:end]
        if len(segment) == 0:
            segment = x[-1:]
        result.extend(
            [
                float(np.mean(segment)),
                float(np.std(segment)),
                float(np.max(np.abs(segment))),
            ]
        )
    return result


def normalized_phase_features(values: np.ndarray) -> List[float]:
    x = np.asarray(values, dtype=np.float32)
    mean = float(np.mean(x))
    std = float(np.std(x))
    normalized = (x - mean) / std if std > 1e-6 else np.zeros_like(x)
    return phase_features(normalized)


def impact_distribution_features(values: np.ndarray) -> List[float]:
    x = np.asarray(values, dtype=np.float32)
    ordered = np.sort(x)
    quantiles = []
    for fraction in (0.10, 0.25, 0.50, 0.75, 0.90):
        index = int(math.floor(fraction * (len(ordered) - 1) + 0.5))
        quantiles.append(float(ordered[index]))
    mean = float(np.mean(x))
    std = float(np.std(x))
    if std > 1e-6:
        normalized = (x - mean) / std
        skew = float(np.mean(normalized**3))
        excess_kurtosis = float(np.mean(normalized**4) - 3.0)
    else:
        skew = 0.0
        excess_kurtosis = 0.0
    max_abs_diff = float(np.max(np.abs(np.diff(x)))) if len(x) > 1 else 0.0
    return quantiles + [skew, excess_kurtosis, max_abs_diff]


def temporal_features(values: np.ndarray) -> List[float]:
    x = np.asarray(values, dtype=np.float32)
    centered = x - float(np.mean(x))
    activity = np.abs(centered)
    std = float(np.std(x))
    high_activity_ratio = float(np.mean(activity > std)) if std > 1e-6 else 0.0

    if len(x) > 2 and std > 1e-6:
        peaks = (
            (activity[1:-1] > activity[:-2])
            & (activity[1:-1] >= activity[2:])
            & (activity[1:-1] > std)
        )
        peak_count_normalized = float(np.sum(peaks)) / float(len(x))
    else:
        peak_count_normalized = 0.0

    spectrum = np.fft.rfft(centered.astype(np.float64))
    power = np.asarray(np.abs(spectrum) ** 2, dtype=np.float64)
    if len(power):
        power[0] = 0.0
    power_sum = float(np.sum(power))
    if power_sum > 1e-12 and len(power) > 1:
        dominant_bin = int(np.argmax(power))
        dominant_frequency_hz = dominant_bin * SAMPLE_RATE / float(len(x))
        probabilities = power[1:] / power_sum
        nonzero = probabilities > 0.0
        spectral_entropy = -float(
            np.sum(probabilities[nonzero] * np.log(probabilities[nonzero]))
        ) / math.log(max(len(probabilities), 2))
    else:
        dominant_frequency_hz = 0.0
        spectral_entropy = 0.0

    lag_start = min(max(1, int(round(0.15 * SAMPLE_RATE))), max(len(x) - 1, 1))
    lag_end = min(len(x) // 2, int(round(1.20 * SAMPLE_RATE)))
    autocorr_peak = 0.0
    autocorr_peak_lag_seconds = 0.0
    if std > 1e-6 and lag_end >= lag_start:
        best_correlation = -1.0
        best_lag = lag_start
        for lag in range(lag_start, lag_end + 1):
            left = centered[:-lag]
            right = centered[lag:]
            denominator = math.sqrt(
                float(np.dot(left, left)) * float(np.dot(right, right))
            )
            correlation = (
                float(np.dot(left, right)) / denominator
                if denominator > 1e-12
                else 0.0
            )
            if correlation > best_correlation:
                best_correlation = correlation
                best_lag = lag
        autocorr_peak = best_correlation
        autocorr_peak_lag_seconds = best_lag / float(SAMPLE_RATE)
    return [
        high_activity_ratio,
        peak_count_normalized,
        dominant_frequency_hz,
        spectral_entropy,
        autocorr_peak,
        autocorr_peak_lag_seconds,
    ]


def build_feature_series(window: np.ndarray) -> Dict[str, np.ndarray]:
    data = np.asarray(window, dtype=np.float32)
    series: Dict[str, np.ndarray] = {
        name: data[:, axis] for axis, name in enumerate(CHANNEL_NAMES)
    }
    series["gyro_mag"] = np.linalg.norm(data[:, 0:3], axis=1)
    series["acc_mag"] = np.linalg.norm(data[:, 3:6], axis=1)
    series["gyro_delta_mag"] = np.linalg.norm(np.diff(data[:, 0:3], axis=0), axis=1)
    series["acc_delta_mag"] = np.linalg.norm(np.diff(data[:, 3:6], axis=0), axis=1)
    for name, values in zip(GRAVITY_NAMES, gravity_aligned_series(data)):
        series[name] = values
    return series


def event_features(window: np.ndarray) -> List[float]:
    series = build_feature_series(window)
    vertical = np.asarray(series["acc_vertical"], dtype=np.float32)
    acc_mag = np.asarray(series["acc_mag"], dtype=np.float32)
    gyro_mag = np.asarray(series["gyro_mag"], dtype=np.float32)
    n = len(vertical)
    if n == 0:
        return [0.0] * len(EVENT_FEATURE_NAMES)

    free_flight = acc_mag < 0.70
    free_flight_ratio = float(np.mean(free_flight))
    longest_run = 0
    current_run = 0
    for is_free_flight in free_flight.tolist():
        current_run = current_run + 1 if is_free_flight else 0
        longest_run = max(longest_run, current_run)
    longest_free_flight_run_ratio = longest_run / float(n)

    centered_gyro = gyro_mag - float(np.mean(gyro_mag))
    centered_vertical = vertical - float(np.mean(vertical))
    correlation_denominator = math.sqrt(
        float(np.dot(centered_gyro, centered_gyro))
        * float(np.dot(centered_vertical, centered_vertical))
    )
    gyro_vertical_correlation = (
        float(np.dot(centered_gyro, centered_vertical)) / correlation_denominator
        if correlation_denominator > 1e-12
        else 0.0
    )

    return [
        free_flight_ratio,
        longest_free_flight_run_ratio,
        gyro_vertical_correlation,
    ]


def extract_features(window: np.ndarray) -> np.ndarray:
    data = np.asarray(window, dtype=np.float32)
    if data.ndim != 2 or data.shape[1] != 6:
        raise ValueError(f"Expected window shape (n, 6), got {data.shape}")

    series = build_feature_series(data)
    features: List[float] = []
    for source in GLOBAL_SERIES_NAMES:
        features.extend(series_features(series[source]))
    for source in PHASE_SOURCE_NAMES:
        features.extend(phase_features(series[source]))
    for source in PHASE_SOURCE_NAMES:
        features.extend(temporal_features(series[source]))
    for source in PHASE_SOURCE_NAMES:
        features.extend(normalized_phase_features(series[source]))
    for source in PHASE_SOURCE_NAMES:
        features.extend(impact_distribution_features(series[source]))
    features.extend(event_features(data))
    return np.asarray(features, dtype=np.float32)


def build_feature_names() -> List[str]:
    names: List[str] = []
    for source in GLOBAL_SERIES_NAMES:
        for feature in ONE_SERIES_FEATURES:
            names.append(f"{source}_{feature}")
    for source in PHASE_SOURCE_NAMES:
        for phase in range(PHASE_SEGMENTS):
            for feature in PHASE_FEATURES:
                names.append(f"{source}_phase{phase}_{feature}")
    for source in PHASE_SOURCE_NAMES:
        for feature in TEMPORAL_FEATURES:
            names.append(f"{source}_{feature}")
    for source in PHASE_SOURCE_NAMES:
        for phase in range(PHASE_SEGMENTS):
            for feature in PHASE_FEATURES:
                names.append(f"{source}_normalized_phase{phase}_{feature}")
    for source in PHASE_SOURCE_NAMES:
        for feature in IMPACT_DISTRIBUTION_FEATURES:
            names.append(f"{source}_{feature}")
    names.extend(EVENT_FEATURE_NAMES)
    return names


def build_jump_shape_feature_indices(feature_names: Sequence[str]) -> List[int]:
    invariant_suffixes = (
        "_zcr",
        "_high_activity_ratio",
        "_peak_count_normalized",
        "_dominant_frequency_hz",
        "_spectral_entropy",
        "_autocorr_peak",
        "_autocorr_peak_lag_seconds",
        "_skew",
        "_excess_kurtosis",
    )
    return [
        index
        for index, name in enumerate(feature_names)
        if "_normalized_phase" in name or name.endswith(invariant_suffixes)
    ]


def split_records_by_file(
    records: Sequence[ImuRecord],
    seed: int = SEED,
) -> Tuple[List[ImuRecord], List[ImuRecord], List[ImuRecord]]:
    labels = [record.label_idx for record in records]
    train_records, temp_records = train_test_split(
        list(records),
        train_size=TRAIN_RATIO,
        random_state=seed,
        stratify=labels,
    )
    temp_labels = [record.label_idx for record in temp_records]
    val_fraction_of_temp = VAL_RATIO / (VAL_RATIO + TEST_RATIO)
    val_records, test_records = train_test_split(
        temp_records,
        train_size=val_fraction_of_temp,
        random_state=seed + 1,
        stratify=temp_labels,
    )
    return list(train_records), list(val_records), list(test_records)


def split_records_for_experiment(
    base_records: Sequence[ImuRecord],
    extra_train_records: Sequence[ImuRecord] = (),
    seed: int = SEED,
) -> Tuple[List[ImuRecord], List[ImuRecord], List[ImuRecord]]:
    train_records, val_records, test_records = split_records_by_file(
        base_records,
        seed,
    )
    base_paths = {record.path.resolve() for record in base_records}
    extra_paths = [record.path.resolve() for record in extra_train_records]
    duplicate_paths = base_paths.intersection(extra_paths)
    if duplicate_paths:
        raise ValueError(
            "Extra training records duplicate base dataset paths: "
            + ", ".join(str(path) for path in sorted(duplicate_paths))
        )
    if len(extra_paths) != len(set(extra_paths)):
        raise ValueError("Extra training records contain duplicate paths")
    return train_records + list(extra_train_records), val_records, test_records


def euler_rotation_matrix(rx: float, ry: float, rz: float) -> np.ndarray:
    sx, cx = math.sin(rx), math.cos(rx)
    sy, cy = math.sin(ry), math.cos(ry)
    sz, cz = math.sin(rz), math.cos(rz)
    rotation_x = np.asarray(
        [[1.0, 0.0, 0.0], [0.0, cx, -sx], [0.0, sx, cx]], dtype=np.float32
    )
    rotation_y = np.asarray(
        [[cy, 0.0, sy], [0.0, 1.0, 0.0], [-sy, 0.0, cy]], dtype=np.float32
    )
    rotation_z = np.asarray(
        [[cz, -sz, 0.0], [sz, cz, 0.0], [0.0, 0.0, 1.0]], dtype=np.float32
    )
    return (rotation_z @ rotation_y @ rotation_x).astype(np.float32)


def rotate_imu_window(window: np.ndarray, rotation: np.ndarray) -> np.ndarray:
    data = np.asarray(window, dtype=np.float32)
    matrix = np.asarray(rotation, dtype=np.float32)
    if matrix.shape != (3, 3):
        raise ValueError(f"Expected rotation shape (3, 3), got {matrix.shape}")
    rotated = data.copy()
    rotated[:, 0:3] = data[:, 0:3] @ matrix.T
    rotated[:, 3:6] = data[:, 3:6] @ matrix.T
    return rotated


def time_warp_window(
    window: np.ndarray,
    rng: np.random.Generator,
    max_displacement: float = 0.03,
) -> np.ndarray:
    data = np.asarray(window, dtype=np.float32)
    if len(data) < 3 or max_displacement <= 0.0:
        return data.copy()
    timeline = np.linspace(0.0, 1.0, len(data), dtype=np.float64)
    phase = float(rng.uniform(0.0, 2.0 * math.pi))
    amplitude = float(rng.uniform(0.25, 1.0)) * max_displacement
    displacement = amplitude * np.sin(math.pi * timeline) * np.sin(
        2.0 * math.pi * timeline + phase
    )
    source_timeline = np.clip(timeline + displacement, 0.0, 1.0)
    source_timeline = np.maximum.accumulate(source_timeline)
    source_timeline[0] = 0.0
    source_timeline[-1] = 1.0
    warped = np.empty_like(data)
    for axis in range(data.shape[1]):
        warped[:, axis] = np.interp(source_timeline, timeline, data[:, axis]).astype(
            np.float32
        )
    return warped


def augment_window(window: np.ndarray, rng: np.random.Generator) -> np.ndarray:
    max_angle = math.radians(MAX_ROTATION_DEGREES)
    angles = rng.uniform(-max_angle, max_angle, size=3)
    rotation = euler_rotation_matrix(float(angles[0]), float(angles[1]), float(angles[2]))
    augmented = rotate_imu_window(window, rotation)
    augmented = time_warp_window(augmented, rng, max_displacement=0.03)
    gyro_noise = rng.normal(0.0, 0.25, size=augmented[:, 0:3].shape).astype(np.float32)
    acc_noise = rng.normal(0.0, 0.003, size=augmented[:, 3:6].shape).astype(np.float32)
    augmented[:, 0:3] += gyro_noise
    augmented[:, 3:6] += acc_noise
    return augmented


def build_samples(
    records: Sequence[ImuRecord],
    window_len: int,
    step_len: int,
    rest_threshold: float,
    active_point_threshold: float,
    augment: bool,
    rng: np.random.Generator,
    progress_label: Optional[str] = None,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray, Dict[str, int]]:
    features: List[np.ndarray] = []
    labels: List[int] = []
    file_ids: List[int] = []
    skipped = {
        "too_short": 0,
        "rest_filtered": 0,
        "kept_windows": 0,
        "files_without_valid_window": 0,
    }

    for file_id, record in enumerate(records):
        if progress_label and (file_id == 0 or file_id % 10 == 0):
            print(
                f"features {progress_label} file={file_id + 1}/{len(records)} "
                f"kept={skipped['kept_windows']}",
                flush=True,
            )
        data = load_imu_file(record.path)
        if len(data) < window_len:
            skipped["too_short"] += 1
            continue
        record_kept = 0
        for window in iter_windows(data, window_len, step_len):
            if not keep_window_for_label(
                window,
                record.label,
                rest_threshold,
                active_point_threshold,
            ):
                skipped["rest_filtered"] += 1
                continue
            features.append(extract_features(window))
            labels.append(record.label_idx)
            file_ids.append(file_id)
            skipped["kept_windows"] += 1
            record_kept += 1
            if augment:
                for _ in range(AUGMENT_TIMES):
                    features.append(extract_features(augment_window(window, rng)))
                    labels.append(record.label_idx)
                    file_ids.append(file_id)
                    skipped["kept_windows"] += 1
        if record_kept == 0:
            skipped["files_without_valid_window"] += 1
            if record.label in HIGH_DYNAMIC_CLASSES:
                continue
            fallback_windows = list(iter_windows(data, window_len, step_len))
            if fallback_windows:
                scored = [(motion_score(window), window) for window in fallback_windows]
                if record.label == SIT_CLASS_NAME:
                    _, best_window = min(scored, key=lambda item: item[0])
                else:
                    _, best_window = max(scored, key=lambda item: item[0])
                features.append(extract_features(best_window))
                labels.append(record.label_idx)
                file_ids.append(file_id)
                skipped["kept_windows"] += 1

    if not features:
        raise ValueError("No samples generated after filtering")
    if progress_label:
        print(
            f"features {progress_label} file={len(records)}/{len(records)} "
            f"kept={skipped['kept_windows']} complete=true",
            flush=True,
        )
    return (
        np.vstack(features).astype(np.float32),
        np.asarray(labels, dtype=np.int64),
        np.asarray(file_ids, dtype=np.int64),
        skipped,
    )


def standardize(
    train_x: np.ndarray,
    val_x: np.ndarray,
    test_x: np.ndarray,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    mean = np.mean(train_x, axis=0).astype(np.float32)
    std = np.std(train_x, axis=0).astype(np.float32)
    std[std < 1e-6] = 1.0
    return (
        ((train_x - mean) / std).astype(np.float32),
        ((val_x - mean) / std).astype(np.float32),
        ((test_x - mean) / std).astype(np.float32),
        mean,
        std,
    )


def family_subset(
    x: np.ndarray,
    y: np.ndarray,
    file_ids: np.ndarray,
    class_names: Sequence[str],
    family_names: Sequence[str],
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    global_indices = [class_names.index(name) for name in family_names]
    global_to_local = {
        global_idx: local_idx for local_idx, global_idx in enumerate(global_indices)
    }
    mask = np.isin(y, np.asarray(global_indices, dtype=np.int64))
    local_y = np.asarray(
        [global_to_local[int(label)] for label in np.asarray(y)[mask]],
        dtype=np.int64,
    )
    return np.asarray(x)[mask], local_y, np.asarray(file_ids)[mask]


def route_family_predictions(
    primary_pred: np.ndarray,
    specialist_pred: np.ndarray,
    class_names: Sequence[str],
    family_names: Sequence[str],
) -> np.ndarray:
    primary = np.asarray(primary_pred, dtype=np.int64)
    specialist = np.asarray(specialist_pred, dtype=np.int64)
    if primary.shape != specialist.shape:
        raise ValueError("Primary and specialist predictions must have the same shape")
    family_global = np.asarray(
        [class_names.index(name) for name in family_names], dtype=np.int64
    )
    routed = primary.copy()
    mask = np.isin(primary, family_global)
    routed[mask] = family_global[specialist[mask]]
    return routed


def class_weight_tensor(labels: np.ndarray, class_count: int, device: torch.device) -> torch.Tensor:
    counts = np.bincount(labels, minlength=class_count).astype(np.float32)
    counts[counts == 0.0] = 1.0
    weights = counts.sum() / (class_count * counts)
    return torch.tensor(weights, dtype=torch.float32, device=device)


def make_loader(
    x: np.ndarray,
    y: np.ndarray,
    batch_size: int,
    shuffle: bool,
    file_ids: Optional[np.ndarray] = None,
    file_balanced: bool = False,
    seed: int = SEED,
) -> DataLoader:
    tensors: List[torch.Tensor] = [torch.from_numpy(x).float(), torch.from_numpy(y).long()]
    if file_ids is not None:
        tensors.append(torch.from_numpy(np.asarray(file_ids, dtype=np.int64)).long())
    dataset = TensorDataset(*tensors)
    if file_balanced:
        if file_ids is None:
            raise ValueError("file_ids are required for file-balanced sampling")
        weights = torch.as_tensor(
            file_balanced_sample_weights(y, file_ids), dtype=torch.double
        )
        generator = torch.Generator()
        generator.manual_seed(seed)
        sampler = WeightedRandomSampler(
            weights,
            num_samples=len(y),
            replacement=True,
            generator=generator,
        )
        return DataLoader(dataset, batch_size=batch_size, sampler=sampler)
    return DataLoader(dataset, batch_size=batch_size, shuffle=shuffle)


def predict(model: nn.Module, x: np.ndarray, device: torch.device) -> np.ndarray:
    model.eval()
    preds: List[np.ndarray] = []
    loader = make_loader(x, np.zeros(len(x), dtype=np.int64), batch_size=512, shuffle=False)
    with torch.no_grad():
        for batch_x, _ in loader:
            logits = model(batch_x.to(device))
            preds.append(torch.argmax(logits, dim=1).cpu().numpy())
    return np.concatenate(preds)


def evaluate(model: nn.Module, x: np.ndarray, y: np.ndarray, device: torch.device) -> Tuple[float, float, np.ndarray]:
    y_pred = predict(model, x, device)
    acc = float(accuracy_score(y, y_pred))
    macro_f1 = float(f1_score(y, y_pred, average="macro", zero_division=0))
    return acc, macro_f1, y_pred


def weak_and_worst_f1(
    y_true: np.ndarray,
    y_pred: np.ndarray,
    class_names: Sequence[str],
) -> Tuple[float, float]:
    per_class = f1_score(
        y_true,
        y_pred,
        labels=np.arange(len(class_names)),
        average=None,
        zero_division=0,
    )
    weak_indices = [
        class_names.index(name) for name in WEAK_CLASS_NAMES if name in class_names
    ]
    weak_f1 = float(np.mean(per_class[weak_indices])) if weak_indices else float(np.mean(per_class))
    worst_f1 = float(np.min(per_class)) if len(per_class) else 0.0
    return weak_f1, worst_f1


def per_class_recalls(
    y_true: np.ndarray,
    y_pred: np.ndarray,
    class_count: int,
) -> np.ndarray:
    matrix = confusion_matrix(
        np.asarray(y_true),
        np.asarray(y_pred),
        labels=np.arange(class_count),
    )
    support = matrix.sum(axis=1)
    return np.divide(
        np.diag(matrix),
        support,
        out=np.zeros(class_count, dtype=np.float64),
        where=support > 0,
    )


def weak_and_min_recall(
    y_true: np.ndarray,
    y_pred: np.ndarray,
    class_names: Sequence[str],
) -> Tuple[float, float, np.ndarray]:
    recalls = per_class_recalls(y_true, y_pred, len(class_names))
    weak_indices = [
        class_names.index(name) for name in WEAK_CLASS_NAMES if name in class_names
    ]
    weak_recall = (
        float(np.mean(recalls[weak_indices]))
        if weak_indices
        else float(np.mean(recalls))
    )
    min_recall = float(np.min(recalls)) if len(recalls) else 0.0
    return weak_recall, min_recall, recalls


def validation_checkpoint_key(
    val_min_recall: float,
    val_weak_recall: float,
    val_f1: float,
    val_acc: float,
) -> Tuple[float, float, float, float]:
    return val_min_recall, val_weak_recall, val_f1, val_acc


def train_model(
    train_x: np.ndarray,
    train_y: np.ndarray,
    train_file_ids: np.ndarray,
    val_x: np.ndarray,
    val_y: np.ndarray,
    class_names: Sequence[str],
    device: torch.device,
    progress_label: str = "",
) -> Tuple[BPNet, Dict[str, object]]:
    class_count = len(class_names)
    model = BPNet(train_x.shape[1], class_count).to(device)
    criterion = nn.CrossEntropyLoss()
    optimizer = torch.optim.AdamW(model.parameters(), lr=LEARNING_RATE, weight_decay=WEIGHT_DECAY)
    loader = make_loader(
        train_x,
        train_y,
        BATCH_SIZE,
        shuffle=False,
        file_ids=train_file_ids,
        file_balanced=True,
        seed=SEED,
    )

    best_state = copy.deepcopy(model.state_dict())
    best_score = (-float("inf"),) * 4
    best_epoch = 0
    patience_left = PATIENCE
    history: List[Dict[str, float]] = []

    for epoch in range(1, MAX_EPOCHS + 1):
        model.train()
        loss_sum = 0.0
        ce_sum = 0.0
        supcon_sum = 0.0
        margin_sum = 0.0
        seen = 0
        for batch_x, batch_y, batch_file_ids in loader:
            batch_x = batch_x.to(device)
            batch_y = batch_y.to(device)
            batch_file_ids = batch_file_ids.to(device)
            optimizer.zero_grad(set_to_none=True)
            embeddings = model.forward_features(batch_x)
            logits = model.net[8](embeddings)
            ce_loss = criterion(logits, batch_y)
            supcon_loss = cross_file_supervised_contrastive_loss(
                embeddings,
                batch_y,
                batch_file_ids,
            )
            margin_loss = hard_pair_margin_loss(logits, batch_y, class_names)
            loss = ce_loss + SUPCON_WEIGHT * supcon_loss + HARD_PAIR_WEIGHT * margin_loss
            loss.backward()
            optimizer.step()
            loss_sum += float(loss.item()) * len(batch_x)
            ce_sum += float(ce_loss.item()) * len(batch_x)
            supcon_sum += float(supcon_loss.item()) * len(batch_x)
            margin_sum += float(margin_loss.item()) * len(batch_x)
            seen += len(batch_x)

        val_acc, val_f1, val_pred = evaluate(model, val_x, val_y, device)
        val_weak_f1, val_worst_f1 = weak_and_worst_f1(
            val_y, val_pred, class_names
        )
        val_weak_recall, val_min_recall, _ = weak_and_min_recall(
            val_y, val_pred, class_names
        )
        avg_loss = loss_sum / max(seen, 1)
        avg_ce = ce_sum / max(seen, 1)
        avg_supcon = supcon_sum / max(seen, 1)
        avg_margin = margin_sum / max(seen, 1)
        score = validation_checkpoint_key(
            val_min_recall,
            val_weak_recall,
            val_f1,
            val_acc,
        )
        history.append(
            {
                "epoch": float(epoch),
                "loss": avg_loss,
                "ce_loss": avg_ce,
                "supcon_loss": avg_supcon,
                "margin_loss": avg_margin,
                "val_acc": val_acc,
                "val_f1": val_f1,
                "val_weak_f1": val_weak_f1,
                "val_worst_f1": val_worst_f1,
                "val_weak_recall": val_weak_recall,
                "val_min_recall": val_min_recall,
            }
        )
        if score > best_score:
            best_score = score
            best_state = copy.deepcopy(model.state_dict())
            best_epoch = epoch
            patience_left = PATIENCE
        else:
            patience_left -= 1
        label = f"{progress_label} " if progress_label else ""
        print(
            f"{label}epoch={epoch:03d} loss={avg_loss:.4f} "
            f"ce={avg_ce:.4f} supcon={avg_supcon:.4f} margin={avg_margin:.4f} "
            f"val_acc={val_acc:.4f} val_f1={val_f1:.4f} "
            f"val_weak_f1={val_weak_f1:.4f} val_worst_f1={val_worst_f1:.4f} "
            f"val_weak_recall={val_weak_recall:.4f} val_min_recall={val_min_recall:.4f} "
            f"best_epoch={best_epoch} patience_left={patience_left}",
            flush=True,
        )
        if patience_left <= 0:
            break

    model.load_state_dict(best_state)
    return model, {"best_epoch": best_epoch, "history": history}


def train_family_specialist(
    train_x_raw: np.ndarray,
    train_y: np.ndarray,
    train_file_ids: np.ndarray,
    val_x_raw: np.ndarray,
    val_y: np.ndarray,
    test_x_raw: np.ndarray,
    test_y: np.ndarray,
    class_names: Sequence[str],
    device: torch.device,
    progress_label: str,
) -> Dict[str, object]:
    family_names = [
        name for name in FAMILY_SPECIALIST_CLASS_NAMES if name in class_names
    ]
    if len(family_names) < 2:
        raise ValueError("Family specialist requires at least two configured classes")
    specialist_feature_indices = build_jump_shape_feature_indices(build_feature_names())

    train_family_raw, train_family_y, train_family_file_ids = family_subset(
        train_x_raw,
        train_y,
        train_file_ids,
        class_names,
        family_names,
    )
    val_family_raw, val_family_y, _ = family_subset(
        val_x_raw,
        val_y,
        np.arange(len(val_y), dtype=np.int64),
        class_names,
        family_names,
    )
    test_family_raw, test_family_y, _ = family_subset(
        test_x_raw,
        test_y,
        np.arange(len(test_y), dtype=np.int64),
        class_names,
        family_names,
    )
    train_family_raw = train_family_raw[:, specialist_feature_indices]
    val_family_raw = val_family_raw[:, specialist_feature_indices]
    test_family_raw = test_family_raw[:, specialist_feature_indices]
    (
        train_family_x,
        val_family_x,
        test_family_x,
        specialist_mean,
        specialist_std,
    ) = standardize(train_family_raw, val_family_raw, test_family_raw)

    print(
        f"start {progress_label} specialist=family "
        f"classes={family_names} train={len(train_family_y)} "
        f"val={len(val_family_y)} test={len(test_family_y)} "
        f"feature_dim={len(specialist_feature_indices)}",
        flush=True,
    )
    specialist_model, specialist_meta = train_model(
        train_family_x,
        train_family_y,
        train_family_file_ids,
        val_family_x,
        val_family_y,
        family_names,
        device,
        progress_label=f"{progress_label} specialist=family",
    )
    return {
        "model": specialist_model,
        "mean": specialist_mean,
        "std": specialist_std,
        "class_names": family_names,
        "feature_indices": specialist_feature_indices,
        "training": specialist_meta,
    }


def load_primary_artifacts(
    artifact_dir: Path,
    input_dim: int,
    class_count: int,
    expected_window_len: int,
    device: torch.device,
) -> Tuple[BPNet, np.ndarray, np.ndarray]:
    artifact_dir = Path(artifact_dir)
    config_path = artifact_dir / "scaler_and_config.npz"
    model_path = artifact_dir / "best_model.pt"
    with np.load(config_path, allow_pickle=False) as config:
        mean = np.asarray(config["mean"], dtype=np.float32)
        std = np.asarray(config["std"], dtype=np.float32)
        saved_window_len = int(np.asarray(config["window_len"]).reshape(-1)[0])
    if mean.shape != (input_dim,) or std.shape != (input_dim,):
        raise ValueError("Primary artifact feature dimension does not match current extractor")
    if saved_window_len != expected_window_len:
        raise ValueError(
            f"Primary artifact window_len={saved_window_len} does not match "
            f"requested window_len={expected_window_len}"
        )
    state = torch.load(model_path, map_location=device, weights_only=True)
    if isinstance(state, dict) and "primary" in state:
        state = state["primary"]
    model = BPNet(input_dim, class_count).to(device)
    model.load_state_dict(state)
    model.eval()
    return model, mean, std


def train_one_experiment(
    window_seconds: float,
    records: Sequence[ImuRecord],
    class_names: Sequence[str],
    device: torch.device,
    seed: int,
    primary_artifact_dir: Optional[Path] = None,
    enable_family_specialist: bool = False,
    validation_only: bool = False,
    extra_train_records: Sequence[ImuRecord] = (),
) -> Dict[str, object]:
    if validation_only and enable_family_specialist:
        raise ValueError("Family specialist is not supported in validation-only mode")
    window_len, step_len = window_lengths(window_seconds)
    train_records, val_records, test_records = split_records_for_experiment(
        records,
        extra_train_records,
        seed,
    )
    rest_threshold = estimate_rest_threshold(train_records, window_len, step_len)
    active_point_threshold = estimate_active_point_threshold(
        train_records, window_len, step_len
    )
    rng = np.random.default_rng(seed + int(window_seconds * 100))

    train_x_raw, train_y, train_file_ids, train_stats = build_samples(
        train_records,
        window_len,
        step_len,
        rest_threshold,
        active_point_threshold,
        augment=True,
        rng=rng,
        progress_label=f"window={window_seconds:.1f}s split=train",
    )
    val_x_raw, val_y, _, val_stats = build_samples(
        val_records,
        window_len,
        step_len,
        rest_threshold,
        active_point_threshold,
        augment=False,
        rng=rng,
        progress_label=f"window={window_seconds:.1f}s split=val",
    )
    if validation_only:
        test_x_raw = np.empty((0, train_x_raw.shape[1]), dtype=np.float32)
        test_y = np.empty(0, dtype=np.int64)
        test_stats: Dict[str, int] = {"skipped_validation_only": 1}
    else:
        test_x_raw, test_y, _, test_stats = build_samples(
            test_records,
            window_len,
            step_len,
            rest_threshold,
            active_point_threshold,
            augment=False,
            rng=rng,
            progress_label=f"window={window_seconds:.1f}s split=test",
        )

    print(
        f"start window={window_seconds:.1f}s window_len={window_len} step_len={step_len} "
        f"rest_threshold={rest_threshold:.5f} "
        f"active_point_threshold={active_point_threshold:.5f}",
        flush=True,
    )
    if primary_artifact_dir is None:
        train_x, val_x, test_x, mean, std = standardize(
            train_x_raw, val_x_raw, test_x_raw
        )
        model, train_meta = train_model(
            train_x,
            train_y,
            train_file_ids,
            val_x,
            val_y,
            class_names,
            device,
            progress_label=f"window={window_seconds:.1f}s",
        )
    else:
        model, mean, std = load_primary_artifacts(
            primary_artifact_dir,
            input_dim=train_x_raw.shape[1],
            class_count=len(class_names),
            expected_window_len=window_len,
            device=device,
        )
        train_x = ((train_x_raw - mean) / std).astype(np.float32)
        val_x = ((val_x_raw - mean) / std).astype(np.float32)
        test_x = ((test_x_raw - mean) / std).astype(np.float32)
        train_meta = {"loaded_from": str(Path(primary_artifact_dir).resolve())}
        print(
            f"primary_model_loaded={Path(primary_artifact_dir).resolve()}",
            flush=True,
        )
    flat_val_acc, flat_val_f1, flat_val_pred = evaluate(model, val_x, val_y, device)
    if validation_only:
        flat_test_acc = float("nan")
        flat_test_f1 = float("nan")
        flat_test_pred = np.empty(0, dtype=np.int64)
    else:
        flat_test_acc, flat_test_f1, flat_test_pred = evaluate(
            model, test_x, test_y, device
        )
    training_meta: Dict[str, object] = {"primary": train_meta}
    if enable_family_specialist:
        specialist = train_family_specialist(
            train_x_raw,
            train_y,
            train_file_ids,
            val_x_raw,
            val_y,
            test_x_raw,
            test_y,
            class_names,
            device,
            progress_label=f"window={window_seconds:.1f}s",
        )
        specialist_model = specialist["model"]
        assert isinstance(specialist_model, BPNet)
        specialist_mean = np.asarray(specialist["mean"], dtype=np.float32)
        specialist_std = np.asarray(specialist["std"], dtype=np.float32)
        specialist_names = list(specialist["class_names"])
        specialist_feature_indices = np.asarray(
            specialist["feature_indices"], dtype=np.int64
        )
        specialist_val_x = (
            (val_x_raw[:, specialist_feature_indices] - specialist_mean)
            / specialist_std
        ).astype(np.float32)
        specialist_test_x = (
            (test_x_raw[:, specialist_feature_indices] - specialist_mean)
            / specialist_std
        ).astype(np.float32)
        specialist_val_pred = predict(specialist_model, specialist_val_x, device)
        specialist_test_pred = predict(specialist_model, specialist_test_x, device)
        val_pred = route_family_predictions(
            flat_val_pred,
            specialist_val_pred,
            class_names,
            specialist_names,
        )
        test_pred = route_family_predictions(
            flat_test_pred,
            specialist_test_pred,
            class_names,
            specialist_names,
        )
        training_meta["family_specialist"] = specialist["training"]
    else:
        specialist_model = None
        specialist_mean = np.empty(0, dtype=np.float32)
        specialist_std = np.empty(0, dtype=np.float32)
        specialist_names = []
        specialist_feature_indices = np.empty(0, dtype=np.int64)
        val_pred = flat_val_pred
        test_pred = flat_test_pred
    val_acc = float(accuracy_score(val_y, val_pred))
    val_f1 = float(f1_score(val_y, val_pred, average="macro", zero_division=0))
    if validation_only:
        test_acc = float("nan")
        test_f1 = float("nan")
    else:
        test_acc = float(accuracy_score(test_y, test_pred))
        test_f1 = float(
            f1_score(test_y, test_pred, average="macro", zero_division=0)
        )
    val_weak_recall, val_min_recall, val_recalls = weak_and_min_recall(
        val_y, val_pred, class_names
    )
    if validation_only:
        test_acc = float("nan")
        test_weak_recall = float("nan")
        test_min_recall = float("nan")
        test_recalls = np.full(len(class_names), np.nan, dtype=np.float64)
    else:
        test_weak_recall, test_min_recall, test_recalls = weak_and_min_recall(
            test_y, test_pred, class_names
        )

    if validation_only:
        print(
            f"window={window_seconds:.1f}s train={len(train_y)} val={len(val_y)} "
            f"val_acc={val_acc:.4f} val_f1={val_f1:.4f} "
            f"val_min_recall={val_min_recall:.4f} "
            "validation_only=true test_evaluation_skipped=true"
        )
    else:
        print(
            f"window={window_seconds:.1f}s "
            f"train={len(train_y)} val={len(val_y)} test={len(test_y)} "
            f"val_acc={val_acc:.4f} val_f1={val_f1:.4f} "
            f"val_min_recall={val_min_recall:.4f} "
            f"test_acc={test_acc:.4f} test_f1={test_f1:.4f} "
            f"test_min_recall={test_min_recall:.4f}"
        )

    return {
        "window_seconds": window_seconds,
        "window_len": window_len,
        "step_len": step_len,
        "rest_threshold": rest_threshold,
        "active_point_threshold": active_point_threshold,
        "model": model,
        "specialist_model": specialist_model,
        "specialist_mean": specialist_mean,
        "specialist_std": specialist_std,
        "specialist_class_names": specialist_names,
        "specialist_feature_indices": specialist_feature_indices,
        "mean": mean,
        "std": std,
        "flat_val_acc": flat_val_acc,
        "flat_val_f1": flat_val_f1,
        "flat_test_acc": flat_test_acc,
        "flat_test_f1": flat_test_f1,
        "val_acc": val_acc,
        "val_f1": val_f1,
        "val_weak_recall": val_weak_recall,
        "val_min_recall": val_min_recall,
        "val_class_recalls": {
            name: float(value) for name, value in zip(class_names, val_recalls)
        },
        "test_acc": test_acc,
        "test_f1": test_f1,
        "test_weak_recall": test_weak_recall,
        "test_min_recall": test_min_recall,
        "test_class_recalls": {
            name: float(value) for name, value in zip(class_names, test_recalls)
        },
        "val_pred": val_pred,
        "test_pred": test_pred,
        "y_val": val_y,
        "y_test": test_y,
        "train_sample_count": int(len(train_y)),
        "val_sample_count": int(len(val_y)),
        "test_sample_count": int(len(test_y)),
        "train_file_count": len(train_records),
        "val_file_count": len(val_records),
        "test_file_count": 0 if validation_only else len(test_records),
        "train_files": [str(record.path) for record in train_records],
        "val_files": [str(record.path) for record in val_records],
        "test_files": (
            [] if validation_only else [str(record.path) for record in test_records]
        ),
        "sample_stats": {"train": train_stats, "val": val_stats, "test": test_stats},
        "training": training_meta,
    }


def evaluate_external_holdout(
    best_result: Dict[str, object],
    records: Sequence[ImuRecord],
    class_names: Sequence[str],
    device: torch.device,
    validation_only: bool = False,
) -> Dict[str, object]:
    if validation_only:
        return {"skipped": True, "reason": "validation_only"}
    if not records:
        return {"skipped": True, "reason": "no_external_holdout"}
    labels = {record.label for record in records}
    if labels != {"jumping_squat"}:
        raise ValueError(
            "External holdout must contain only jumping_squat records, got "
            + ", ".join(sorted(labels))
        )

    window_len = int(best_result["window_len"])
    step_len = int(best_result["step_len"])
    rest_threshold = float(best_result["rest_threshold"])
    active_point_threshold = float(best_result["active_point_threshold"])
    raw_x, y_true, _, stats = build_samples(
        records,
        window_len,
        step_len,
        rest_threshold,
        active_point_threshold,
        augment=False,
        rng=np.random.default_rng(SEED),
        progress_label="external_holdout=jumping_squat",
    )
    if len(y_true) == 0:
        return {
            "skipped": True,
            "reason": "no_kept_windows",
            "file_count": len(records),
            "files": [str(record.path) for record in records],
            "sample_stats": stats,
        }
    mean = np.asarray(best_result["mean"], dtype=np.float32)
    std = np.asarray(best_result["std"], dtype=np.float32)
    x = ((raw_x - mean) / std).astype(np.float32)
    model = best_result["model"]
    assert isinstance(model, BPNet)
    y_pred = predict(model, x, device)
    jumping_squat_idx = class_names.index("jumping_squat")
    target = y_true == jumping_squat_idx
    recall = float(np.mean(y_pred[target] == jumping_squat_idx))
    return {
        "skipped": False,
        "label": "jumping_squat",
        "file_count": len(records),
        "sample_count": int(len(y_true)),
        "recall": recall,
        "files": [str(record.path) for record in records],
        "sample_stats": stats,
    }


def save_confusion_matrix(
    y_true: np.ndarray,
    y_pred: np.ndarray,
    class_names: Sequence[str],
    save_path: Path,
) -> None:
    matrix = confusion_matrix(y_true, y_pred, labels=list(range(len(class_names))))
    fig_w = max(8.0, len(class_names) * 0.75)
    fig, ax = plt.subplots(figsize=(fig_w, fig_w))
    im = ax.imshow(matrix, cmap="Blues")
    fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04)
    ax.set_xticks(np.arange(len(class_names)))
    ax.set_yticks(np.arange(len(class_names)))
    ax.set_xticklabels(class_names, rotation=45, ha="right", fontsize=8)
    ax.set_yticklabels(class_names, fontsize=8)
    ax.set_xlabel("Predicted")
    ax.set_ylabel("True")
    for i in range(matrix.shape[0]):
        for j in range(matrix.shape[1]):
            ax.text(j, i, int(matrix[i, j]), ha="center", va="center", fontsize=7)
    fig.tight_layout()
    fig.savefig(save_path, dpi=180)
    plt.close(fig)


def c_float(value: float) -> str:
    if not np.isfinite(value):
        value = 0.0
    literal = f"{float(value):.9g}"
    if "." not in literal and "e" not in literal.lower():
        literal += ".0"
    return f"{literal}f"


def c_string(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def c_array_1d(name: str, data: np.ndarray, const_type: str = "float") -> str:
    values = ", ".join(c_float(v) for v in np.asarray(data).reshape(-1))
    return f"static const {const_type} {name}[{len(np.asarray(data).reshape(-1))}] = {{ {values} }};"


def c_array_2d(name: str, data: np.ndarray) -> str:
    arr = np.asarray(data)
    rows = []
    for row in arr:
        rows.append("  { " + ", ".join(c_float(v) for v in row) + " }")
    return f"static const float {name}[{arr.shape[0]}][{arr.shape[1]}] = {{\n" + ",\n".join(rows) + "\n};"


def export_esp32_header(
    result: Dict[str, object],
    class_names: Sequence[str],
    feature_names: Sequence[str],
    save_path: Path,
) -> None:
    model = result["model"]
    assert isinstance(model, BPNet)
    state = model.state_dict()
    w1 = state["net.0.weight"].cpu().numpy()
    b1 = state["net.0.bias"].cpu().numpy()
    w2 = state["net.3.weight"].cpu().numpy()
    b2 = state["net.3.bias"].cpu().numpy()
    w3 = state["net.6.weight"].cpu().numpy()
    b3 = state["net.6.bias"].cpu().numpy()
    w4 = state["net.8.weight"].cpu().numpy()
    b4 = state["net.8.bias"].cpu().numpy()

    specialist_model = result.get("specialist_model")
    has_specialist = isinstance(specialist_model, BPNet)
    specialist_names = list(result.get("specialist_class_names", []))
    specialist_lines: List[str] = []
    specialist_feature_dim = 0
    if has_specialist:
        assert isinstance(specialist_model, BPNet)
        specialist_state = specialist_model.state_dict()
        specialist_global_indices = [class_names.index(name) for name in specialist_names]
        specialist_feature_indices = np.asarray(
            result.get("specialist_feature_indices", np.arange(len(feature_names))),
            dtype=np.int64,
        )
        specialist_feature_dim = int(len(specialist_feature_indices))
        if specialist_state["net.0.weight"].shape[1] != specialist_feature_dim:
            raise ValueError("Specialist feature index count does not match model input")
        specialist_lines = [
            "static const int SPECIALIST_GLOBAL_CLASS_INDEX[SPECIALIST_CLASS_NUM] = { "
            + ", ".join(str(index) for index in specialist_global_indices)
            + " };",
            "static const int SPECIALIST_FEATURE_INDEX[SPECIALIST_FEATURE_DIM] = { "
            + ", ".join(str(int(index)) for index in specialist_feature_indices)
            + " };",
            c_array_1d(
                "SPECIALIST_FEATURE_MEAN",
                np.asarray(result["specialist_mean"], dtype=np.float32),
            ),
            c_array_1d(
                "SPECIALIST_FEATURE_STD",
                np.asarray(result["specialist_std"], dtype=np.float32),
            ),
            c_array_2d("SW1", specialist_state["net.0.weight"].cpu().numpy()),
            c_array_1d("SB1", specialist_state["net.0.bias"].cpu().numpy()),
            c_array_2d("SW2", specialist_state["net.3.weight"].cpu().numpy()),
            c_array_1d("SB2", specialist_state["net.3.bias"].cpu().numpy()),
            c_array_2d("SW3", specialist_state["net.6.weight"].cpu().numpy()),
            c_array_1d("SB3", specialist_state["net.6.bias"].cpu().numpy()),
            c_array_2d("SW4", specialist_state["net.8.weight"].cpu().numpy()),
            c_array_1d("SB4", specialist_state["net.8.bias"].cpu().numpy()),
        ]

    mean = np.asarray(result["mean"], dtype=np.float32)
    std = np.asarray(result["std"], dtype=np.float32)
    window_len = int(result["window_len"])
    rest_threshold = float(result.get("rest_threshold", 0.03))
    active_point_threshold = float(result.get("active_point_threshold", 0.02))

    lines = [
        "#ifndef ESP32_BP_MODEL_H",
        "#define ESP32_BP_MODEL_H",
        "",
        "#include <math.h>",
        "#include <stdint.h>",
        "",
        f"#define WINDOW_LEN {window_len}",
        "#define AXIS_NUM 6",
        f"#define SAMPLE_RATE_HZ {SAMPLE_RATE}",
        f"#define FEATURE_DIM {len(feature_names)}",
        f"#define CLASS_NUM {len(class_names)}",
        f"#define HAS_FAMILY_SPECIALIST {1 if has_specialist else 0}",
        f"#define SPECIALIST_CLASS_NUM {len(specialist_names) if has_specialist else 0}",
        f"#define SPECIALIST_FEATURE_DIM {specialist_feature_dim}",
        f"#define HIDDEN1 {HIDDEN1}",
        f"#define HIDDEN2 {HIDDEN2}",
        f"#define HIDDEN3 {HIDDEN3}",
        f"#define PHASE_SEGMENTS {PHASE_SEGMENTS}",
        "",
        f"static const float REST_MOTION_THRESHOLD = {c_float(rest_threshold)};",
        f"static const float ACTIVE_POINT_THRESHOLD = {c_float(active_point_threshold)};",
        "static const float HIGH_DYNAMIC_MIN_RATIO = 0.2f;",
        "",
        "static const char* CLASS_NAMES[CLASS_NUM] = { "
        + ", ".join(c_string(name) for name in class_names)
        + " };",
        c_array_1d("FEATURE_MEAN", mean),
        c_array_1d("FEATURE_STD", std),
        c_array_2d("W1", w1),
        c_array_1d("B1", b1),
        c_array_2d("W2", w2),
        c_array_1d("B2", b2),
        c_array_2d("W3", w3),
        c_array_1d("B3", b3),
        c_array_2d("W4", w4),
        c_array_1d("B4", b4),
        *specialist_lines,
        r"""
static inline void append_series_features(const float* x, int n, float* feature, int* idx) {
    float sum = 0.0f;
    float sum2 = 0.0f;
    float min_v = x[0];
    float max_v = x[0];
    for (int i = 0; i < n; i++) {
        float v = x[i];
        sum += v;
        sum2 += v * v;
        if (v < min_v) min_v = v;
        if (v > max_v) max_v = v;
    }
    float mean = sum / (float)n;
    float energy = sum2 / (float)n;
    float var = energy - mean * mean;
    if (var < 0.0f) var = 0.0f;
    float std = sqrtf(var);
    float mean_abs_diff = 0.0f;
    float diff_sum = 0.0f;
    float diff_sum2 = 0.0f;
    float zcr_count = 0.0f;
    for (int i = 1; i < n; i++) {
        float diff = x[i] - x[i - 1];
        mean_abs_diff += fabsf(diff);
        diff_sum += diff;
        diff_sum2 += diff * diff;
        float a = x[i - 1] - mean;
        float b = x[i] - mean;
        if (a * b < 0.0f) zcr_count += 1.0f;
    }
    float std_diff = 0.0f;
    if (n > 1) {
        mean_abs_diff = mean_abs_diff / (float)(n - 1);
        float mean_diff = diff_sum / (float)(n - 1);
        float diff_var = diff_sum2 / (float)(n - 1) - mean_diff * mean_diff;
        if (diff_var < 0.0f) diff_var = 0.0f;
        std_diff = sqrtf(diff_var);
        zcr_count = zcr_count / (float)(n - 1);
    }
    feature[(*idx)++] = mean;
    feature[(*idx)++] = std;
    feature[(*idx)++] = min_v;
    feature[(*idx)++] = max_v;
    feature[(*idx)++] = sqrtf(energy);
    feature[(*idx)++] = mean_abs_diff;
    feature[(*idx)++] = zcr_count;
    feature[(*idx)++] = std_diff;
}

static inline void append_phase_features(const float* x, int n, float* feature, int* idx) {
    for (int phase = 0; phase < PHASE_SEGMENTS; phase++) {
        int start = (phase * n) / PHASE_SEGMENTS;
        int end = ((phase + 1) * n) / PHASE_SEGMENTS;
        if (end <= start) end = start + 1;
        if (end > n) end = n;
        float sum = 0.0f;
        float sum2 = 0.0f;
        float max_abs = 0.0f;
        for (int i = start; i < end; i++) {
            float value = x[i];
            sum += value;
            sum2 += value * value;
            float abs_value = fabsf(value);
            if (abs_value > max_abs) max_abs = abs_value;
        }
        int count = end - start;
        float mean = sum / (float)count;
        float variance = sum2 / (float)count - mean * mean;
        if (variance < 0.0f) variance = 0.0f;
        feature[(*idx)++] = mean;
        feature[(*idx)++] = sqrtf(variance);
        feature[(*idx)++] = max_abs;
    }
}

static inline void append_normalized_phase_features(const float* x, int n, float* feature, int* idx) {
    float sum = 0.0f;
    float sum2 = 0.0f;
    float normalized[WINDOW_LEN];
    for (int i = 0; i < n; i++) {
        sum += x[i];
        sum2 += x[i] * x[i];
    }
    float mean = sum / (float)n;
    float variance = sum2 / (float)n - mean * mean;
    if (variance < 0.0f) variance = 0.0f;
    float std = sqrtf(variance);
    for (int i = 0; i < n; i++) {
        normalized[i] = std > 1e-6f ? (x[i] - mean) / std : 0.0f;
    }
    append_phase_features(normalized, n, feature, idx);
}

static inline void append_impact_distribution_features(const float* x, int n, float* feature, int* idx) {
    float ordered[WINDOW_LEN];
    float sum = 0.0f;
    float sum2 = 0.0f;
    float max_abs_diff = 0.0f;
    for (int i = 0; i < n; i++) {
        ordered[i] = x[i];
        sum += x[i];
        sum2 += x[i] * x[i];
        if (i > 0) {
            float abs_diff = fabsf(x[i] - x[i - 1]);
            if (abs_diff > max_abs_diff) max_abs_diff = abs_diff;
        }
    }
    for (int i = 1; i < n; i++) {
        float value = ordered[i];
        int j = i - 1;
        while (j >= 0 && ordered[j] > value) {
            ordered[j + 1] = ordered[j];
            j--;
        }
        ordered[j + 1] = value;
    }
    const float fractions[5] = { 0.10f, 0.25f, 0.50f, 0.75f, 0.90f };
    for (int q = 0; q < 5; q++) {
        int position = (int)floorf(fractions[q] * (float)(n - 1) + 0.5f);
        feature[(*idx)++] = ordered[position];
    }
    float mean = sum / (float)n;
    float variance = sum2 / (float)n - mean * mean;
    if (variance < 0.0f) variance = 0.0f;
    float std = sqrtf(variance);
    float skew = 0.0f;
    float kurtosis = 0.0f;
    if (std > 1e-6f) {
        for (int i = 0; i < n; i++) {
            float z = (x[i] - mean) / std;
            float z2 = z * z;
            skew += z2 * z;
            kurtosis += z2 * z2;
        }
        skew /= (float)n;
        kurtosis = kurtosis / (float)n - 3.0f;
    }
    feature[(*idx)++] = skew;
    feature[(*idx)++] = kurtosis;
    feature[(*idx)++] = max_abs_diff;
}

static inline void append_event_features(
    const float* acc_vertical,
    const float* acc_mag,
    const float* gyro_mag,
    int n,
    float* feature,
    int* idx
) {
    int free_flight_count = 0;
    int longest_free_flight_run = 0;
    int current_free_flight_run = 0;
    for (int i = 0; i < n; i++) {
        if (acc_mag[i] < 0.70f) {
            free_flight_count++;
            current_free_flight_run++;
            if (current_free_flight_run > longest_free_flight_run) {
                longest_free_flight_run = current_free_flight_run;
            }
        } else {
            current_free_flight_run = 0;
        }
    }
    float free_flight_ratio = (float)free_flight_count / (float)n;
    float longest_free_flight_run_ratio =
        (float)longest_free_flight_run / (float)n;

    float gyro_sum = 0.0f;
    float vertical_sum = 0.0f;
    for (int i = 0; i < n; i++) {
        gyro_sum += gyro_mag[i];
        vertical_sum += acc_vertical[i];
    }
    float gyro_mean = gyro_sum / (float)n;
    float vertical_mean = vertical_sum / (float)n;
    float correlation_numerator = 0.0f;
    float centered_gyro_energy = 0.0f;
    float centered_vertical_energy = 0.0f;
    for (int i = 0; i < n; i++) {
        float centered_gyro = gyro_mag[i] - gyro_mean;
        float centered_vertical = acc_vertical[i] - vertical_mean;
        correlation_numerator += centered_gyro * centered_vertical;
        centered_gyro_energy += centered_gyro * centered_gyro;
        centered_vertical_energy += centered_vertical * centered_vertical;
    }
    float correlation_denominator = sqrtf(
        centered_gyro_energy * centered_vertical_energy
    );
    float gyro_vertical_correlation = correlation_denominator > 1e-12f
        ? correlation_numerator / correlation_denominator
        : 0.0f;

    feature[(*idx)++] = free_flight_ratio;
    feature[(*idx)++] = longest_free_flight_run_ratio;
    feature[(*idx)++] = gyro_vertical_correlation;
}

static inline void append_temporal_features(const float* x, int n, float* feature, int* idx) {
    float sum = 0.0f;
    float sum2 = 0.0f;
    for (int i = 0; i < n; i++) {
        sum += x[i];
        sum2 += x[i] * x[i];
    }
    float mean = sum / (float)n;
    float variance = sum2 / (float)n - mean * mean;
    if (variance < 0.0f) variance = 0.0f;
    float std = sqrtf(variance);
    int high_count = 0;
    int peak_count = 0;
    for (int i = 0; i < n; i++) {
        float activity = fabsf(x[i] - mean);
        if (std > 1e-6f && activity > std) high_count++;
        if (i > 0 && i < n - 1 && std > 1e-6f) {
            float previous = fabsf(x[i - 1] - mean);
            float next = fabsf(x[i + 1] - mean);
            if (activity > previous && activity >= next && activity > std) peak_count++;
        }
    }

    int frequency_bin_count = n / 2;
    float spectral_power[WINDOW_LEN / 2 + 1];
    float total_power = 0.0f;
    float dominant_power = -1.0f;
    int dominant_bin = 0;
    const float two_pi = 6.2831853071795864769f;
    for (int k = 1; k <= frequency_bin_count; k++) {
        float real = 0.0f;
        float imaginary = 0.0f;
        for (int sample = 0; sample < n; sample++) {
            float centered = x[sample] - mean;
            float angle = two_pi * (float)k * (float)sample / (float)n;
            real += centered * cosf(angle);
            imaginary -= centered * sinf(angle);
        }
        float power = real * real + imaginary * imaginary;
        spectral_power[k - 1] = power;
        total_power += power;
        if (power > dominant_power) {
            dominant_power = power;
            dominant_bin = k;
        }
    }
    float dominant_frequency_hz = 0.0f;
    float spectral_entropy = 0.0f;
    if (total_power > 1e-12f && frequency_bin_count > 0) {
        dominant_frequency_hz = (float)dominant_bin * (float)SAMPLE_RATE_HZ / (float)n;
        if (frequency_bin_count > 1) {
            for (int k = 0; k < frequency_bin_count; k++) {
                float probability = spectral_power[k] / total_power;
                if (probability > 0.0f) {
                    spectral_entropy -= probability * logf(probability);
                }
            }
            spectral_entropy /= logf((float)frequency_bin_count);
        }
    }

    int lag_start = (int)(0.15f * (float)SAMPLE_RATE_HZ + 0.5f);
    if (lag_start < 1) lag_start = 1;
    if (lag_start > n - 1) lag_start = n - 1;
    int lag_end = n / 2;
    int max_lag = (int)(1.20f * (float)SAMPLE_RATE_HZ + 0.5f);
    if (lag_end > max_lag) lag_end = max_lag;
    float autocorr_peak = 0.0f;
    int autocorr_peak_lag = 0;
    if (std > 1e-6f && lag_end >= lag_start) {
        float best_correlation = -1.0f;
        int best_lag = lag_start;
        for (int lag = lag_start; lag <= lag_end; lag++) {
            float dot = 0.0f;
            float left_energy = 0.0f;
            float right_energy = 0.0f;
            for (int i = 0; i < n - lag; i++) {
                float left = x[i] - mean;
                float right = x[i + lag] - mean;
                dot += left * right;
                left_energy += left * left;
                right_energy += right * right;
            }
            float denominator = sqrtf(left_energy * right_energy);
            float correlation = denominator > 1e-12f ? dot / denominator : 0.0f;
            if (correlation > best_correlation) {
                best_correlation = correlation;
                best_lag = lag;
            }
        }
        autocorr_peak = best_correlation;
        autocorr_peak_lag = best_lag;
    }

    feature[(*idx)++] = std > 1e-6f ? (float)high_count / (float)n : 0.0f;
    feature[(*idx)++] = (float)peak_count / (float)n;
    feature[(*idx)++] = dominant_frequency_hz;
    feature[(*idx)++] = spectral_entropy;
    feature[(*idx)++] = autocorr_peak;
    feature[(*idx)++] = (float)autocorr_peak_lag / (float)SAMPLE_RATE_HZ;
}

static inline float bp_window_motion_score(const float window[WINDOW_LEN][AXIS_NUM]) {
    float gyro_sum = 0.0f;
    float gyro_sum2 = 0.0f;
    float acc_sum = 0.0f;
    float acc_sum2 = 0.0f;
    for (int i = 0; i < WINDOW_LEN; i++) {
        float gyro_mag = sqrtf(
            window[i][0] * window[i][0] +
            window[i][1] * window[i][1] +
            window[i][2] * window[i][2]
        );
        float acc_mag = sqrtf(
            window[i][3] * window[i][3] +
            window[i][4] * window[i][4] +
            window[i][5] * window[i][5]
        );
        gyro_sum += gyro_mag;
        gyro_sum2 += gyro_mag * gyro_mag;
        acc_sum += acc_mag;
        acc_sum2 += acc_mag * acc_mag;
    }
    float gyro_mean = gyro_sum / (float)WINDOW_LEN;
    float acc_mean = acc_sum / (float)WINDOW_LEN;
    float gyro_var = gyro_sum2 / (float)WINDOW_LEN - gyro_mean * gyro_mean;
    float acc_var = acc_sum2 / (float)WINDOW_LEN - acc_mean * acc_mean;
    if (gyro_var < 0.0f) gyro_var = 0.0f;
    if (acc_var < 0.0f) acc_var = 0.0f;
    return sqrtf(acc_var) + sqrtf(gyro_var) / 200.0f;
}

static inline float bp_window_active_ratio(const float window[WINDOW_LEN][AXIS_NUM]) {
    int active_count = 0;
    for (int i = 0; i < WINDOW_LEN; i++) {
        float gyro_mag = sqrtf(
            window[i][0] * window[i][0] +
            window[i][1] * window[i][1] +
            window[i][2] * window[i][2]
        );
        float acc_delta = 0.0f;
        if (i > 0) {
            float dx = window[i][3] - window[i - 1][3];
            float dy = window[i][4] - window[i - 1][4];
            float dz = window[i][5] - window[i - 1][5];
            acc_delta = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        if (acc_delta + gyro_mag / 200.0f > ACTIVE_POINT_THRESHOLD) active_count++;
    }
    return (float)active_count / (float)WINDOW_LEN;
}

static inline int bp_window_is_dynamic_candidate(const float window[WINDOW_LEN][AXIS_NUM]) {
    return (
        bp_window_motion_score(window) >= REST_MOTION_THRESHOLD * 1.25f &&
        bp_window_active_ratio(window) >= HIGH_DYNAMIC_MIN_RATIO
    );
}

static inline void extract_features_from_window(const float window[WINDOW_LEN][AXIS_NUM], float feature[FEATURE_DIM]) {
    int idx = 0;
    float temp[WINDOW_LEN];
    float event_gyro_mag[WINDOW_LEN];
    float event_acc_mag[WINDOW_LEN];
    float phase_sources[4][WINDOW_LEN];
    int phase_lengths[4] = { WINDOW_LEN, WINDOW_LEN, WINDOW_LEN, WINDOW_LEN - 1 };

    /* gx, gy, gz, ax, ay, az */
    for (int axis = 0; axis < AXIS_NUM; axis++) {
        for (int i = 0; i < WINDOW_LEN; i++) temp[i] = window[i][axis];
        append_series_features(temp, WINDOW_LEN, feature, &idx);
    }

    /* gyro_mag */
    for (int i = 0; i < WINDOW_LEN; i++) {
        float gx = window[i][0];
        float gy = window[i][1];
        float gz = window[i][2];
        temp[i] = sqrtf(gx * gx + gy * gy + gz * gz);
        event_gyro_mag[i] = temp[i];
        phase_sources[2][i] = temp[i];
    }
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    /* acc_mag */
    for (int i = 0; i < WINDOW_LEN; i++) {
        float ax = window[i][3];
        float ay = window[i][4];
        float az = window[i][5];
        temp[i] = sqrtf(ax * ax + ay * ay + az * az);
        event_acc_mag[i] = temp[i];
    }
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    /* gyro_delta_mag */
    for (int i = 0; i < WINDOW_LEN - 1; i++) {
        float dx = window[i + 1][0] - window[i][0];
        float dy = window[i + 1][1] - window[i][1];
        float dz = window[i + 1][2] - window[i][2];
        temp[i] = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    append_series_features(temp, WINDOW_LEN - 1, feature, &idx);

    /* acc_delta_mag */
    for (int i = 0; i < WINDOW_LEN - 1; i++) {
        float dx = window[i + 1][3] - window[i][3];
        float dy = window[i + 1][4] - window[i][4];
        float dz = window[i + 1][5] - window[i][5];
        temp[i] = sqrtf(dx * dx + dy * dy + dz * dz);
        phase_sources[3][i] = temp[i];
    }
    append_series_features(temp, WINDOW_LEN - 1, feature, &idx);

    /* Gravity-aligned vertical and horizontal components. */
    float gravity_x = 0.0f;
    float gravity_y = 0.0f;
    float gravity_z = 0.0f;
    for (int i = 0; i < WINDOW_LEN; i++) {
        gravity_x += window[i][3];
        gravity_y += window[i][4];
        gravity_z += window[i][5];
    }
    gravity_x /= (float)WINDOW_LEN;
    gravity_y /= (float)WINDOW_LEN;
    gravity_z /= (float)WINDOW_LEN;
    float gravity_norm = sqrtf(
        gravity_x * gravity_x + gravity_y * gravity_y + gravity_z * gravity_z
    );
    if (gravity_norm < 1e-6f) {
        gravity_x = 0.0f;
        gravity_y = 0.0f;
        gravity_z = 1.0f;
    } else {
        gravity_x /= gravity_norm;
        gravity_y /= gravity_norm;
        gravity_z /= gravity_norm;
    }

    /* acc_vertical */
    for (int i = 0; i < WINDOW_LEN; i++) {
        float vertical =
            window[i][3] * gravity_x +
            window[i][4] * gravity_y +
            window[i][5] * gravity_z;
        temp[i] = vertical;
        phase_sources[0][i] = vertical;
    }
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    /* acc_horizontal_mag */
    for (int i = 0; i < WINDOW_LEN; i++) {
        float vertical = phase_sources[0][i];
        float total_squared =
            window[i][3] * window[i][3] +
            window[i][4] * window[i][4] +
            window[i][5] * window[i][5];
        float horizontal_squared = total_squared - vertical * vertical;
        if (horizontal_squared < 0.0f) horizontal_squared = 0.0f;
        temp[i] = sqrtf(horizontal_squared);
        phase_sources[1][i] = temp[i];
    }
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    /* gyro_vertical */
    for (int i = 0; i < WINDOW_LEN; i++) {
        temp[i] =
            window[i][0] * gravity_x +
            window[i][1] * gravity_y +
            window[i][2] * gravity_z;
    }
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    /* gyro_horizontal_mag */
    for (int i = 0; i < WINDOW_LEN; i++) {
        float vertical =
            window[i][0] * gravity_x +
            window[i][1] * gravity_y +
            window[i][2] * gravity_z;
        float total_squared =
            window[i][0] * window[i][0] +
            window[i][1] * window[i][1] +
            window[i][2] * window[i][2];
        float horizontal_squared = total_squared - vertical * vertical;
        if (horizontal_squared < 0.0f) horizontal_squared = 0.0f;
        temp[i] = sqrtf(horizontal_squared);
    }
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    for (int source = 0; source < 4; source++) {
        append_phase_features(
            phase_sources[source], phase_lengths[source], feature, &idx
        );
    }
    for (int source = 0; source < 4; source++) {
        append_temporal_features(
            phase_sources[source], phase_lengths[source], feature, &idx
        );
    }
    for (int source = 0; source < 4; source++) {
        append_normalized_phase_features(
            phase_sources[source], phase_lengths[source], feature, &idx
        );
    }
    for (int source = 0; source < 4; source++) {
        append_impact_distribution_features(
            phase_sources[source], phase_lengths[source], feature, &idx
        );
    }
    append_event_features(
        phase_sources[0], event_acc_mag, event_gyro_mag,
        WINDOW_LEN, feature, &idx
    );
}

static inline float relu_float(float x) {
    return x > 0.0f ? x : 0.0f;
}

#if HAS_FAMILY_SPECIALIST
static inline int bp_family_specialist_predict(const float feature_raw[FEATURE_DIM], float* confidence) {
    float x[SPECIALIST_FEATURE_DIM];
    float h1[HIDDEN1];
    float h2[HIDDEN2];
    float h3[HIDDEN3];
    float out[SPECIALIST_CLASS_NUM];
    for (int i = 0; i < SPECIALIST_FEATURE_DIM; i++) {
        int feature_index = SPECIALIST_FEATURE_INDEX[i];
        x[i] = (feature_raw[feature_index] - SPECIALIST_FEATURE_MEAN[i]) / SPECIALIST_FEATURE_STD[i];
    }
    for (int o = 0; o < HIDDEN1; o++) {
        float sum = SB1[o];
        for (int i = 0; i < SPECIALIST_FEATURE_DIM; i++) sum += SW1[o][i] * x[i];
        h1[o] = relu_float(sum);
    }
    for (int o = 0; o < HIDDEN2; o++) {
        float sum = SB2[o];
        for (int i = 0; i < HIDDEN1; i++) sum += SW2[o][i] * h1[i];
        h2[o] = relu_float(sum);
    }
    for (int o = 0; o < HIDDEN3; o++) {
        float sum = SB3[o];
        for (int i = 0; i < HIDDEN2; i++) sum += SW3[o][i] * h2[i];
        h3[o] = relu_float(sum);
    }
    float max_logit = -3.4028235e38f;
    for (int o = 0; o < SPECIALIST_CLASS_NUM; o++) {
        float sum = SB4[o];
        for (int i = 0; i < HIDDEN3; i++) sum += SW4[o][i] * h3[i];
        out[o] = sum;
        if (sum > max_logit) max_logit = sum;
    }
    float exp_sum = 0.0f;
    for (int o = 0; o < SPECIALIST_CLASS_NUM; o++) {
        out[o] = expf(out[o] - max_logit);
        exp_sum += out[o];
    }
    int best_local_idx = 0;
    float best_prob = 0.0f;
    for (int o = 0; o < SPECIALIST_CLASS_NUM; o++) {
        float prob = out[o] / exp_sum;
        if (prob > best_prob) {
            best_prob = prob;
            best_local_idx = o;
        }
    }
    if (confidence != 0) *confidence = best_prob;
    return SPECIALIST_GLOBAL_CLASS_INDEX[best_local_idx];
}
#endif

static inline int bp_predict_from_features(const float feature_raw[FEATURE_DIM], float* confidence) {
    float x[FEATURE_DIM];
    float h1[HIDDEN1];
    float h2[HIDDEN2];
    float h3[HIDDEN3];
    float out[CLASS_NUM];
    for (int i = 0; i < FEATURE_DIM; i++) x[i] = (feature_raw[i] - FEATURE_MEAN[i]) / FEATURE_STD[i];
    for (int o = 0; o < HIDDEN1; o++) {
        float sum = B1[o];
        for (int i = 0; i < FEATURE_DIM; i++) sum += W1[o][i] * x[i];
        h1[o] = relu_float(sum);
    }
    for (int o = 0; o < HIDDEN2; o++) {
        float sum = B2[o];
        for (int i = 0; i < HIDDEN1; i++) sum += W2[o][i] * h1[i];
        h2[o] = relu_float(sum);
    }
    for (int o = 0; o < HIDDEN3; o++) {
        float sum = B3[o];
        for (int i = 0; i < HIDDEN2; i++) sum += W3[o][i] * h2[i];
        h3[o] = relu_float(sum);
    }
    float max_logit = -3.4028235e38f;
    for (int o = 0; o < CLASS_NUM; o++) {
        float sum = B4[o];
        for (int i = 0; i < HIDDEN3; i++) sum += W4[o][i] * h3[i];
        out[o] = sum;
        if (sum > max_logit) max_logit = sum;
    }
    float exp_sum = 0.0f;
    for (int o = 0; o < CLASS_NUM; o++) {
        out[o] = expf(out[o] - max_logit);
        exp_sum += out[o];
    }
    int best_idx = 0;
    float best_prob = 0.0f;
    for (int o = 0; o < CLASS_NUM; o++) {
        float prob = out[o] / exp_sum;
        if (prob > best_prob) {
            best_prob = prob;
            best_idx = o;
        }
    }
#if HAS_FAMILY_SPECIALIST
    for (int local_idx = 0; local_idx < SPECIALIST_CLASS_NUM; local_idx++) {
        if (SPECIALIST_GLOBAL_CLASS_INDEX[local_idx] == best_idx) {
            return bp_family_specialist_predict(feature_raw, confidence);
        }
    }
#endif
    if (confidence != 0) *confidence = best_prob;
    return best_idx;
}

static inline int bp_predict_from_window(const float window[WINDOW_LEN][AXIS_NUM], float* confidence) {
    float feature[FEATURE_DIM];
    extract_features_from_window(window, feature);
    return bp_predict_from_features(feature, confidence);
}

#endif
""",
    ]
    save_path.write_text("\n".join(lines), encoding="utf-8")


def serializable_experiment(result: Dict[str, object]) -> Dict[str, object]:
    keep = {
        "window_seconds",
        "window_len",
        "step_len",
        "rest_threshold",
        "active_point_threshold",
        "val_acc",
        "val_f1",
        "val_weak_recall",
        "val_min_recall",
        "val_class_recalls",
        "test_acc",
        "test_f1",
        "test_weak_recall",
        "test_min_recall",
        "test_class_recalls",
        "flat_val_acc",
        "flat_val_f1",
        "flat_test_acc",
        "flat_test_f1",
        "train_sample_count",
        "val_sample_count",
        "test_sample_count",
        "train_file_count",
        "val_file_count",
        "test_file_count",
        "train_files",
        "val_files",
        "test_files",
        "sample_stats",
    }
    return {key: result[key] for key in keep if key in result}


def deployment_gate_status(
    best_result: Dict[str, object],
    class_names: Sequence[str],
) -> Tuple[bool, np.ndarray]:
    recalls = per_class_recalls(
        np.asarray(best_result["y_test"]),
        np.asarray(best_result["test_pred"]),
        len(class_names),
    )
    return bool(np.all(recalls >= TARGET_MIN_CLASS_RECALL)), recalls


def export_model_headers(
    best_result: Dict[str, object],
    class_names: Sequence[str],
    feature_names: Sequence[str],
    output_header_path: Path,
    repository_header_path: Path,
    export_when_below_target: bool,
) -> bool:
    reached_target, _ = deployment_gate_status(best_result, class_names)
    if not reached_target and not export_when_below_target:
        return False

    output_header_path.parent.mkdir(parents=True, exist_ok=True)
    export_esp32_header(best_result, class_names, feature_names, output_header_path)
    if reached_target:
        repository_header_path.parent.mkdir(parents=True, exist_ok=True)
        if output_header_path.resolve() != repository_header_path.resolve():
            shutil.copyfile(output_header_path, repository_header_path)
    return reached_target


def save_outputs(
    best_result: Dict[str, object],
    all_results: Sequence[Dict[str, object]],
    class_names: Sequence[str],
    feature_names: Sequence[str],
    output_dir: Path,
    export_when_below_target: bool,
    repository_header_path: Path = ESP32_MODEL_HEADER,
) -> bool:
    output_dir.mkdir(parents=True, exist_ok=True)
    y_test = np.asarray(best_result["y_test"])
    test_pred = np.asarray(best_result["test_pred"])

    save_confusion_matrix(y_test, test_pred, class_names, output_dir / "confusion_matrix.png")
    model = best_result["model"]
    assert isinstance(model, BPNet)
    specialist_model = best_result.get("specialist_model")
    if isinstance(specialist_model, BPNet):
        torch.save(
            {
                "primary": model.state_dict(),
                "family_specialist": specialist_model.state_dict(),
            },
            output_dir / "best_model.pt",
        )
    else:
        torch.save(model.state_dict(), output_dir / "best_model.pt")
    scaler_config = {
        "mean": np.asarray(best_result["mean"], dtype=np.float32),
        "std": np.asarray(best_result["std"], dtype=np.float32),
        "class_names": np.asarray(class_names),
        "feature_names": np.asarray(feature_names),
        "window_len": np.asarray([int(best_result["window_len"])]),
        "step_len": np.asarray([int(best_result["step_len"])]),
        "sample_rate": np.asarray([SAMPLE_RATE]),
        "rest_threshold": np.asarray(
            [float(best_result["rest_threshold"])], dtype=np.float32
        ),
        "active_point_threshold": np.asarray(
            [float(best_result["active_point_threshold"])], dtype=np.float32
        ),
    }
    if isinstance(specialist_model, BPNet):
        scaler_config.update(
            {
                "specialist_mean": np.asarray(
                    best_result["specialist_mean"], dtype=np.float32
                ),
                "specialist_std": np.asarray(
                    best_result["specialist_std"], dtype=np.float32
                ),
                "specialist_class_names": np.asarray(
                    best_result["specialist_class_names"]
                ),
                "specialist_feature_indices": np.asarray(
                    best_result["specialist_feature_indices"], dtype=np.int64
                ),
            }
        )
    np.savez(output_dir / "scaler_and_config.npz", **scaler_config)

    report = {
        "seed": SEED,
        "sample_rate": SAMPLE_RATE,
        "target_min_class_recall": TARGET_MIN_CLASS_RECALL,
        "class_names": list(class_names),
        "feature_names": list(feature_names),
        "best_window_seconds": best_result["window_seconds"],
        "best_window_len": best_result["window_len"],
        "step_len": best_result["step_len"],
        "rest_threshold": best_result["rest_threshold"],
        "active_point_threshold": best_result["active_point_threshold"],
        "val_acc": best_result["val_acc"],
        "val_f1": best_result["val_f1"],
        "test_acc": best_result["test_acc"],
        "test_f1": best_result["test_f1"],
        "test_min_recall": best_result["test_min_recall"],
        "test_class_recalls": best_result["test_class_recalls"],
        "external_holdout": best_result.get(
            "external_holdout",
            {"skipped": True, "reason": "not_configured"},
        ),
        "classifier_type": (
            "flat_bp_plus_family_bp_specialist"
            if isinstance(specialist_model, BPNet)
            else "flat_bp"
        ),
        "family_specialist_class_names": list(
            best_result.get("specialist_class_names", [])
        ),
        "flat_val_acc": best_result.get("flat_val_acc"),
        "flat_val_f1": best_result.get("flat_val_f1"),
        "flat_test_acc": best_result.get("flat_test_acc"),
        "flat_test_f1": best_result.get("flat_test_f1"),
        "classification_report": classification_report(
            y_test, test_pred, target_names=class_names, output_dict=True, zero_division=0
        ),
        "all_experiments": [serializable_experiment(result) for result in all_results],
    }
    with (output_dir / "training_report.json").open("w", encoding="utf-8") as file:
        json.dump(report, file, ensure_ascii=False, indent=2)

    return export_model_headers(
        best_result,
        class_names,
        feature_names,
        output_dir / "esp32_bp_model.h",
        repository_header_path,
        export_when_below_target,
    )


def save_validation_outputs(
    best_result: Dict[str, object],
    all_results: Sequence[Dict[str, object]],
    class_names: Sequence[str],
    feature_names: Sequence[str],
    output_dir: Path,
) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    model = best_result["model"]
    assert isinstance(model, BPNet)
    torch.save(model.state_dict(), output_dir / "best_model.pt")
    np.savez(
        output_dir / "scaler_and_config.npz",
        mean=np.asarray(best_result["mean"], dtype=np.float32),
        std=np.asarray(best_result["std"], dtype=np.float32),
        class_names=np.asarray(class_names),
        feature_names=np.asarray(feature_names),
        window_len=np.asarray([int(best_result["window_len"])]),
        step_len=np.asarray([int(best_result["step_len"])]),
        sample_rate=np.asarray([SAMPLE_RATE]),
        rest_threshold=np.asarray(
            [float(best_result["rest_threshold"])], dtype=np.float32
        ),
        active_point_threshold=np.asarray(
            [float(best_result["active_point_threshold"])], dtype=np.float32
        ),
    )
    validation_keys = {
        "window_seconds",
        "window_len",
        "step_len",
        "rest_threshold",
        "active_point_threshold",
        "val_acc",
        "val_f1",
        "val_weak_recall",
        "val_min_recall",
        "val_class_recalls",
        "flat_val_acc",
        "flat_val_f1",
        "train_sample_count",
        "val_sample_count",
        "train_file_count",
        "val_file_count",
        "train_files",
        "val_files",
        "sample_stats",
    }
    report = {
        "mode": "validation_only",
        "seed": SEED,
        "sample_rate": SAMPLE_RATE,
        "class_names": list(class_names),
        "feature_names": list(feature_names),
        "best_window_seconds": best_result["window_seconds"],
        "val_acc": best_result["val_acc"],
        "val_f1": best_result["val_f1"],
        "val_min_recall": best_result["val_min_recall"],
        "val_class_recalls": best_result["val_class_recalls"],
        "classification_report": classification_report(
            np.asarray(best_result["y_val"]),
            np.asarray(best_result["val_pred"]),
            target_names=class_names,
            output_dict=True,
            zero_division=0,
        ),
        "all_experiments": [
            {key: result[key] for key in validation_keys if key in result}
            for result in all_results
        ],
    }
    with (output_dir / "validation_report.json").open("w", encoding="utf-8") as file:
        json.dump(report, file, ensure_ascii=False, indent=2)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Train IMU BP model and export ESP32 header.")
    parser.add_argument("--dataset-dir", type=Path, default=None)
    parser.add_argument("--extra-train-dir", type=Path, default=None)
    parser.add_argument("--external-holdout-dir", type=Path, default=None)
    parser.add_argument("--output-dir", type=Path, default=OUTPUT_DIR)
    parser.add_argument(
        "--primary-artifact-dir",
        type=Path,
        default=None,
        help="Reuse a validated primary BP model and train only the family specialist.",
    )
    parser.add_argument("--enable-family-specialist", action="store_true")
    parser.add_argument(
        "--validation-only",
        action="store_true",
        help="Train and select with validation data without constructing or evaluating test windows.",
    )
    parser.add_argument("--seed", type=int, default=SEED)
    parser.add_argument("--export-when-below-target", action="store_true")
    parser.add_argument("--max-epochs", type=int, default=MAX_EPOCHS)
    parser.add_argument(
        "--window-seconds",
        type=float,
        nargs="+",
        choices=WINDOW_SECONDS_LIST,
        default=list(WINDOW_SECONDS_LIST),
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    global MAX_EPOCHS
    MAX_EPOCHS = args.max_epochs
    set_seed(args.seed)

    dataset_dir = resolve_dataset_dir(args.dataset_dir)
    records, class_names, label_to_idx = scan_dataset(dataset_dir)
    extra_train_records, _ = load_additional_records(
        args.extra_train_dir,
        args.external_holdout_dir,
        label_to_idx,
        validation_only=True,
    )
    feature_names = build_feature_names()
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    print(f"dataset_dir={dataset_dir.resolve()}")
    print(f"device={device}")
    print(f"class_count={len(class_names)} file_count={len(records)} feature_dim={len(feature_names)}")
    print(
        f"extra_train_file_count={len(extra_train_records)} "
        f"external_holdout_loaded=false"
    )
    print(f"class_names={class_names}")
    print(
        f"window_seconds={args.window_seconds} augment_times={AUGMENT_TIMES} "
        f"max_rotation_degrees={MAX_ROTATION_DEGREES:.1f} "
        f"supcon_weight={SUPCON_WEIGHT:.3f} hard_pair_weight={HARD_PAIR_WEIGHT:.3f} "
        f"family_specialist={args.enable_family_specialist} "
        f"validation_only={args.validation_only}"
    )

    all_results: List[Dict[str, object]] = []
    for window_seconds in args.window_seconds:
        result = train_one_experiment(
            window_seconds,
            records,
            class_names,
            device,
            args.seed,
            primary_artifact_dir=args.primary_artifact_dir,
            enable_family_specialist=args.enable_family_specialist,
            validation_only=args.validation_only,
            extra_train_records=extra_train_records,
        )
        all_results.append(result)

    best_result = max(
        all_results,
        key=lambda item: (
            float(item["val_min_recall"]),
            float(item["val_f1"]),
            float(item["val_acc"]),
        ),
    )
    if args.validation_only:
        save_validation_outputs(
            best_result,
            all_results,
            class_names,
            feature_names,
            args.output_dir,
        )
        print("========== best validation experiment ==========")
        print(
            f"best_window={best_result['window_seconds']}s "
            f"val_acc={best_result['val_acc']:.4f} "
            f"val_f1={best_result['val_f1']:.4f} "
            f"val_min_recall={best_result['val_min_recall']:.4f}"
        )
        print(
            classification_report(
                np.asarray(best_result["y_val"]),
                np.asarray(best_result["val_pred"]),
                target_names=class_names,
                zero_division=0,
            )
        )
        print("validation_only=true test_evaluation_skipped=true header_export_skipped=true")
        print(f"outputs={args.output_dir.resolve()}")
        return

    _, external_holdout_records = load_additional_records(
        None,
        args.external_holdout_dir,
        label_to_idx,
        validation_only=False,
    )
    external_holdout = evaluate_external_holdout(
        best_result,
        external_holdout_records,
        class_names,
        device,
    )
    best_result["external_holdout"] = external_holdout
    print(
        f"external_holdout_loaded={not bool(external_holdout['skipped'])} "
        f"external_holdout_file_count={external_holdout.get('file_count', 0)} "
        f"external_holdout_recall={external_holdout.get('recall', float('nan')):.4f}"
    )

    reached_target = save_outputs(
        best_result,
        all_results,
        class_names,
        feature_names,
        args.output_dir,
        args.export_when_below_target or EXPORT_WHEN_BELOW_TARGET,
    )

    print("========== best experiment ==========")
    print(
        f"best_window={best_result['window_seconds']}s "
        f"val_acc={best_result['val_acc']:.4f} val_f1={best_result['val_f1']:.4f} "
        f"test_acc={best_result['test_acc']:.4f} test_f1={best_result['test_f1']:.4f}"
    )
    print(
        classification_report(
            np.asarray(best_result["y_test"]),
            np.asarray(best_result["test_pred"]),
            target_names=class_names,
            zero_division=0,
        )
    )
    if reached_target:
        print(
            f"target_reached=true output_header={args.output_dir / 'esp32_bp_model.h'} "
            f"repository_header={ESP32_MODEL_HEADER}"
        )
    else:
        _, test_recalls = deployment_gate_status(best_result, class_names)
        failed = [
            f"{name}:{recall:.4f}"
            for name, recall in zip(class_names, test_recalls)
            if recall < TARGET_MIN_CLASS_RECALL
        ]
        print(
            "target_reached=false header_export_skipped=true "
            f"failed_class_recalls={failed}"
        )
    print(f"outputs={args.output_dir.resolve()}")


if __name__ == "__main__":
    main()
