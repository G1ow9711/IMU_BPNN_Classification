# IMU BP Weak-Class Optimization Implementation Plan

> 历史实施计划：本文保留 180 维阶段的 TDD 过程。最终代码已扩展为 264 维，并增加可选 BP 专家/C 选列导出；当前状态见根目录中文 `README.md`。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve weak-class generalization while preserving a hand-feature BP model, exact C-header inference, visible PyCharm training, and per-epoch logs.

**Architecture:** Replace redundant global statistics with a 180-value orientation/phase-aware feature bank, retain the existing `96 -> 64 -> 32` BP body, and improve training with file-balanced sampling plus training-only contrastive and hard-pair losses. Window filtering and augmentation operate before feature extraction, while export reproduces the same ordered feature pipeline in C.

**Tech Stack:** Python 3.12, NumPy, PyTorch, scikit-learn, matplotlib, `unittest`, PowerShell, PyCharm, C99-compatible generated header.

**Repository Note:** The implementation began before Git initialization. Current tracked paths are `python/train_export.py` and `python/test_train_export.py`; commands run from the repository root with `python.test_train_export`.

---

### Task 1: Specify Feature and Augmentation Behavior

**Files:**
- Modify: `test_train_export.py`
- Test: `test_train_export.py`

- [x] **Step 1: Add failing feature tests**

```python
def test_extract_features_returns_180_ordered_values(self):
    window = np.arange(62 * 6, dtype=np.float32).reshape(62, 6)
    features = te.extract_features(window)
    self.assertEqual(features.shape, (180,))
    self.assertEqual(features.shape[0], len(te.build_feature_names()))
    self.assertEqual(te.build_feature_names()[112], "acc_vertical_phase0_mean")

def test_gravity_aligned_series_are_rotation_invariant(self):
    rng = np.random.default_rng(12)
    window = rng.normal(size=(62, 6)).astype(np.float32)
    rotation = te.euler_rotation_matrix(0.2, -0.1, 0.15)
    rotated = te.rotate_imu_window(window, rotation)
    original = te.gravity_aligned_series(window)
    transformed = te.gravity_aligned_series(rotated)
    for left, right in zip(original, transformed):
        np.testing.assert_allclose(left, right, atol=1e-5, rtol=1e-5)

def test_time_warp_does_not_wrap_endpoints(self):
    window = np.repeat(np.arange(50, dtype=np.float32)[:, None], 6, axis=1)
    warped = te.time_warp_window(window, np.random.default_rng(7), max_displacement=0.03)
    self.assertLess(abs(float(warped[0, 0] - window[0, 0])), 4.0)
    self.assertLess(abs(float(warped[-1, 0] - window[-1, 0])), 4.0)
```

- [x] **Step 2: Run tests and verify RED**

Run: `\.venv\Scripts\python.exe -m unittest test_train_export.py`

Expected: failures for missing rotation/gravity/time-warp functions and old feature dimension 160.

### Task 2: Implement the 180-Feature Python Extractor

**Files:**
- Modify: `train_export.py`
- Test: `test_train_export.py`

- [x] **Step 1: Add feature constants and helpers**

```python
SERIES_FEATURES = ["mean", "std", "min", "max", "rms", "mean_abs_diff", "zcr", "std_diff"]
GRAVITY_NAMES = ["acc_vertical", "acc_horizontal_mag", "gyro_vertical", "gyro_horizontal_mag"]
PHASE_SOURCE_NAMES = ["acc_vertical", "acc_horizontal_mag", "gyro_mag", "acc_delta_mag"]
PHASE_FEATURES = ["mean", "std", "max_abs"]
TEMPORAL_FEATURES = [
    "argmax_abs_position", "argmin_position", "high_activity_ratio",
    "first_half_energy_ratio", "peak_count_normalized",
]
PHASE_SEGMENTS = 4
```

- [x] **Step 2: Implement joint rotation and gravity alignment**

```python
def rotate_imu_window(window: np.ndarray, rotation: np.ndarray) -> np.ndarray:
    result = np.asarray(window, dtype=np.float32).copy()
    result[:, :3] = result[:, :3] @ rotation.T
    result[:, 3:6] = result[:, 3:6] @ rotation.T
    return result

def gravity_aligned_series(window: np.ndarray) -> tuple[np.ndarray, ...]:
    gravity = np.mean(window[:, 3:6], axis=0)
    gravity /= max(float(np.linalg.norm(gravity)), 1e-6)
    acc_vertical = window[:, 3:6] @ gravity
    gyro_vertical = window[:, :3] @ gravity
    acc_horizontal = np.sqrt(np.maximum(np.sum(window[:, 3:6] ** 2, axis=1) - acc_vertical ** 2, 0.0))
    gyro_horizontal = np.sqrt(np.maximum(np.sum(window[:, :3] ** 2, axis=1) - gyro_vertical ** 2, 0.0))
    return acc_vertical, acc_horizontal, gyro_vertical, gyro_horizontal
```

- [x] **Step 3: Implement phase and temporal descriptors and return exactly 180 values**

Use integer segment boundaries `(phase * n) // 4` and `((phase + 1) * n) // 4`. Normalize temporal positions and peak count by `max(n - 1, 1)` and `max(n, 1)` respectively.

- [x] **Step 4: Run tests and verify GREEN**

Run: `\.venv\Scripts\python.exe -m unittest test_train_export.py`

Expected: all feature/augmentation tests pass.

### Task 3: Add Activity Filtering and File-Balanced Sampling

**Files:**
- Modify: `test_train_export.py`
- Modify: `train_export.py`

- [x] **Step 1: Add failing filtering and sampler tests**

```python
def test_dynamic_filter_rejects_low_activity_window(self):
    quiet = np.zeros((62, 6), dtype=np.float32)
    self.assertFalse(te.keep_window_for_label(quiet, "tuck_jump", 0.08, 0.02))

def test_file_balanced_weights_equalize_class_and_file_mass(self):
    labels = np.array([0, 0, 0, 0, 1, 1])
    files = np.array([0, 0, 0, 1, 2, 3])
    weights = te.file_balanced_sample_weights(labels, files)
    self.assertAlmostEqual(float(weights[files == 0].sum()), float(weights[files == 1].sum()))
    self.assertAlmostEqual(float(weights[labels == 0].sum()), float(weights[labels == 1].sum()))
```

- [x] **Step 2: Run tests and verify RED**

Run: `\.venv\Scripts\python.exe -m unittest test_train_export.py`

Expected: missing `keep_window_for_label` and `file_balanced_sample_weights` failures.

- [x] **Step 3: Implement active-point ratio and label-aware filtering**

```python
HIGH_DYNAMIC_CLASSES = {"jumping_jack", "jumping_lunge", "jumping_squat", "tuck_jump"}

def keep_window_for_label(window, label, rest_threshold, active_point_threshold):
    score = motion_score(window)
    if label == SIT_CLASS_NAME:
        return score <= rest_threshold * 1.6
    if score < rest_threshold:
        return False
    if label in HIGH_DYNAMIC_CLASSES:
        return score >= rest_threshold * 1.25 and active_ratio(window, active_point_threshold) >= 0.20
    return True
```

- [x] **Step 4: Return file IDs from `build_samples` and use a weighted sampler**

Each sample receives a local integer source-file ID. Weight each sample by `1 / (files_in_class * samples_from_file)` and use `WeightedRandomSampler(replacement=True, num_samples=len(train_y))`.

- [x] **Step 5: Run tests and verify GREEN**

Run: `\.venv\Scripts\python.exe -m unittest test_train_export.py`

Expected: all tests pass.

### Task 4: Add Cross-File and Hard-Pair Training Losses

**Files:**
- Modify: `test_train_export.py`
- Modify: `train_export.py`

- [x] **Step 1: Add failing loss tests**

```python
def test_cross_file_supcon_is_finite_and_nonnegative(self):
    z = torch.tensor([[1., 0.], [0.9, 0.1], [0., 1.], [0.1, 0.9]])
    y = torch.tensor([0, 0, 1, 1])
    files = torch.tensor([0, 1, 2, 3])
    loss = te.cross_file_supervised_contrastive_loss(z, y, files)
    self.assertTrue(torch.isfinite(loss))
    self.assertGreaterEqual(float(loss), 0.0)

def test_hard_pair_margin_penalizes_wrong_pair_order(self):
    names = ["jumping_squat", "squat", "tuck_jump"]
    logits = torch.tensor([[2.0, 0.0, 0.0]])
    labels = torch.tensor([1])
    self.assertGreater(float(te.hard_pair_margin_loss(logits, labels, names)), 0.0)
```

- [x] **Step 2: Run tests and verify RED**

Run: `\.venv\Scripts\python.exe -m unittest test_train_export.py`

Expected: missing loss-function failures.

- [x] **Step 3: Expose the final BP embedding and implement losses**

`BPNet.forward_features()` returns the third ReLU output; `forward()` applies only the final linear layer to that embedding. Contrastive positives require equal labels and different file IDs. Hard-pair loss applies `relu(0.5 - true_logit + confusing_logit)` for configured confusion pairs.

- [x] **Step 4: Update epoch output and early stopping**

Print every completed epoch before checking patience exhaustion:

```text
window=2.0s epoch=001 loss=... ce=... supcon=... margin=... val_acc=... val_f1=... val_weak_f1=... val_worst_f1=... best_epoch=... patience_left=...
```

- [x] **Step 5: Run tests and verify GREEN**

Run: `\.venv\Scripts\python.exe -m unittest test_train_export.py`

Expected: all tests pass.

### Task 5: Update C Header Export

**Files:**
- Modify: `test_train_export.py`
- Modify: `train_export.py`

- [x] **Step 1: Add a failing generated-header contract test**

Create a temporary BP result, call `export_esp32_header`, and assert the output contains `#define FEATURE_DIM 180`, `append_phase_features`, `append_temporal_features`, `gravity_norm`, and `bp_predict_from_window`.

- [x] **Step 2: Run the test and verify RED**

Run: `\.venv\Scripts\python.exe -m unittest test_train_export.py`

Expected: old header lacks 180-dimensional phase/gravity markers.

- [x] **Step 3: Implement C feature parity**

Emit the same 14 global sources, four phase sources, four phase segments, and five temporal descriptors in exactly the Python order. Export training-derived motion thresholds as constants and keep the BP layer order unchanged.

- [x] **Step 4: Run tests and verify GREEN**

Run: `\.venv\Scripts\python.exe -m unittest test_train_export.py`

Expected: all tests pass and the generated header contract is satisfied.

### Task 6: Smoke-Train and Verify Output

**Files:**
- Modify: `.codex-local/tmp/run_training_visible.ps1`
- Generate: `.codex-local/tmp/smoke_outputs/`

- [x] **Step 1: Add explicit unbuffered arguments and log reset**

The runner invokes:

```powershell
.\.venv\Scripts\python.exe -u train_export.py --dataset-dir "IMU_Dataset\imu_dataset_for_final" 2>&1 |
    Tee-Object -FilePath "outputs\training_console.log"
```

- [x] **Step 2: Run a two-epoch smoke test**

Run: `\.venv\Scripts\python.exe -u train_export.py --dataset-dir IMU_Dataset\imu_dataset_for_final --output-dir .codex-local\tmp\smoke_outputs --max-epochs 2`

Expected: each window prints epochs 001 and 002; report/model/scaler/confusion artifacts exist; no final header is produced below the gate.

- [x] **Step 3: Run the complete test suite**

Run: `\.venv\Scripts\python.exe -m unittest test_train_export.py`

Expected: all tests pass with zero failures.

### Task 7: Launch Visible Full Training

**Files:**
- Use: `.codex-local/tmp/run_training_visible.ps1`
- Generate: `outputs/training_console.log`
- Generate: `outputs/training_report.json`
- Generate: `outputs/best_model.pt`
- Generate: `outputs/scaler_and_config.npz`
- Generate: `outputs/confusion_matrix.png`
- Conditional generate: `outputs/esp32_bp_model.h`

- [ ] **Step 1: Open PyCharm at the project root**

Run `pycharm64.exe G:\Free_Project\BiShengBei_BPNN_ESP32\Project` and verify a PyCharm process exists.

- [ ] **Step 2: Launch the visible PowerShell training window**

Use `Start-Process powershell.exe -WindowStyle Normal -ArgumentList ...run_training_visible.ps1`. The window remains open after training so the user can inspect every epoch.

- [ ] **Step 3: Monitor without hiding output**

Poll the process and `outputs/training_console.log`; do not start a second training process. Relay material window summaries while leaving the visible terminal intact.

- [ ] **Step 4: Evaluate and iterate inside the BP-only boundary**

Compare macro-F1 and per-class F1 against the prior 0.8599/0.57/0.64/0.72 baseline. If improvement is insufficient, change one BP-compatible factor at a time and repeat the visible run.

- [ ] **Step 5: Enforce the deployment gate**

Generate `esp32_bp_model.h` only when all 11 test-set per-class recalls reach 0.90. Otherwise verify the header is absent and report the remaining weak classes.
