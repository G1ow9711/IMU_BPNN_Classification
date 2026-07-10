# IMU BP Weak-Class Optimization Design

> 历史设计说明：本文记录第一轮 180 维方案。后续实现已演进为 264 维特征；当前结构和实测结果以仓库根目录中文 `README.md` 为准。

## Scope

Improve the current IMU action classifier while strictly retaining this deployable path:

```text
six-axis IMU -> hand-crafted features -> standardized BP MLP -> C header -> ESP32-S3
```

No CNN, RNN, Transformer, teacher model, or non-BP inference path is permitted. Training remains visible in PyCharm, writes a persistent console log, and prints every epoch.

## Acceptance Criteria

- Raw files are split before windowing; no source file may cross train/validation/test boundaries.
- Training batches balance classes and original files rather than treating overlapping windows as independent, equally weighted sources.
- High-dynamic rest/transition windows are filtered using thresholds learned from training data only.
- Augmentation uses joint six-axis rotation and smooth time warping; circular wrap and intensity scaling are removed.
- Python and exported C use the same ordered feature vector.
- The classifier remains a dense BP network with ReLU activations.
- Every epoch prints total loss, component losses, validation accuracy, macro-F1, weak-class F1, worst-class F1, best epoch, and patience.
- `esp32_bp_model.h` is generated only when every action has independent-test recall of at least 0.90.
- If the target is missed, model/report artifacts remain available but the deployment header remains absent.

## Data Pipeline

1. Split the current 189 raw files with class stratification.
2. Estimate rest thresholds from training `sit` files only.
3. Generate 1.5 s, 2.0 s, and 2.5 s windows with the existing 0.5 s step.
4. Retain low-motion `sit` windows.
5. For other classes, reject windows below the training-derived motion threshold.
6. For `jumping_jack`, `jumping_lunge`, `jumping_squat`, and `tuck_jump`, also require a minimum active-point ratio and a stronger motion threshold.
7. Track the source-file ID for every original and augmented window.
8. Use sampler weight `1 / (files_in_class * windows_from_file)` so every class has equal expected mass and every file within a class has equal expected mass.

The current fixed split remains available for direct before/after comparison. It is development evidence because its test labels have already been inspected. A future target-device holdout is still required for a pristine deployment claim.

## Augmentation

Each augmented training window applies the same transform to gyroscope and accelerometer vectors:

- bounded 3D rotation, maximum 20 degrees per Euler axis;
- smooth monotonic time warp with at most 3% temporal displacement;
- very small sensor noise after geometric/temporal transforms.

No `np.roll` is used, so no artificial wrap boundary appears. No amplitude scaling is used because intensity distinguishes squat from jumping squat.

## Feature Vector

The new ordered feature vector contains 180 values.

### Global Series Features: 112

Fourteen series:

```text
gx, gy, gz, ax, ay, az,
gyro_mag, acc_mag, gyro_delta_mag, acc_delta_mag,
acc_vertical, acc_horizontal_mag, gyro_vertical, gyro_horizontal_mag
```

Eight non-redundant statistics per series:

```text
mean, std, min, max, rms, mean_abs_diff, zcr, std_diff
```

The gravity-aligned series use the normalized mean acceleration vector from the window. Vertical components are projections onto gravity; horizontal magnitudes are orthogonal residual magnitudes. They are invariant to a shared rotation of both sensors.

### Four-Phase Features: 48

Four temporally important series:

```text
acc_vertical, acc_horizontal_mag, gyro_mag, acc_delta_mag
```

Each series is divided into four ordered segments. Each segment contributes mean, standard deviation, and maximum absolute value. These values retain coarse action order and landing/impulse structure.

### Temporal Descriptors: 20

Each phase source contributes:

```text
argmax_abs_position,
argmin_position,
high_activity_ratio,
first_half_energy_ratio,
peak_count_normalized
```

These distinguish normal squat, jumping squat, tuck jump, jumping lunge, and jumping jack without deploying a temporal neural network.

## BP Model and Loss

Model:

```text
180 -> 96 -> 64 -> 32 -> 11
```

The final 32-dimensional hidden vector is used only during training for representation regularization. Inference remains the same four dense layers.

Training objective:

```text
cross_entropy with class-and-file-balanced sampling
+ 0.05 * cross_file_supervised_contrastive_loss
+ 0.10 * hard_pair_margin_loss
```

The sampler already gives each class equal expected mass, so inverse-window class weights are not applied a second time. Cross-file contrastive positives share a class but come from different raw files. Same-file overlapping windows are excluded as positives. Hard-pair margins cover the observed confusion families:

```text
squat <-> jumping_squat
jumping_squat <-> jumping_jack
tuck_jump <-> jumping_lunge
tuck_jump <-> jumping_squat
```

The early-stopping score combines overall macro-F1, mean weak-class F1, and worst-class F1. The weak set is `jumping_squat`, `squat`, `tuck_jump`, and `jumping_lunge`.

## C Header Contract

`esp32_bp_model.h` contains:

- class names and dimensions;
- scaler mean and standard deviation;
- all BP weights and biases;
- the exact 180-feature extractor;
- gravity alignment, phase features, and temporal descriptors;
- motion-score and active-ratio helpers;
- `bp_predict_from_features()` and `bp_predict_from_window()`.

Training-only contrastive and margin losses are not exported and add no ESP32 inference cost.

## Verification

- Unit tests cover unit conversion, file-level split isolation, feature dimension/order, rotation invariance of derived series, non-circular augmentation, activity filtering, file-balanced weights, contrastive loss, hard-pair loss, and C-header markers.
- A short smoke training run verifies every-epoch output and artifact generation without waiting for convergence.
- Full training runs in a visible PowerShell window launched alongside PyCharm and tees all output to `outputs/training_console.log`.
- Final verification checks report consistency, per-class metrics, confusion matrix totals, header gate behavior, and absence/presence of the C header according to the 0.90 per-class-recall threshold.

## Known Limitation

The source dataset does not provide collector IDs. Results can prove raw-file independence, not subject independence. This limitation must remain in the report until collector metadata or a new target-device holdout exists.
