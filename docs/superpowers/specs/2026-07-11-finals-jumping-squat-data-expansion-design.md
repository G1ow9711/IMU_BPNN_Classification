# Finals Jumping-Squat Data Expansion Design

## Goal

Use the three newly discovered finals `jumping_squat` sessions to improve the high-intensity coverage gap without weakening the existing file-level evaluation, BP-only deployment route, or per-class 90% recall export gate.

The deployed route remains:

```text
six-axis IMU -> hand-crafted features -> standardization -> BP MLP -> generated C header -> ESP32-S3
```

Training stays visible in PyCharm and prints every epoch.

## Fixed Data Contract

Canonical source directory:

```text
G:\Free_Project\BiShengBei_BPNN_ESP32\决赛\MATLAB\实测数据集\A类活动
```

Accepted unique inputs:

| File | SHA-256 | Role |
|---|---|---|
| `jumping_squat_scy1_20.txt` | `FE10A5B4D232DDB6D7A7BD791D3F3765008577CAA6CB317C9AF21BD4BDE0F379` | extra training |
| `jumping_squat_scy2_20.txt` | `E9A02819A55DE955B1861B4EB23C992A9873159984BC8E1C6707053A6C3A52C9` | extra training |
| `jumping_squat_scy3_20.txt` | `4B4C5420FEBDF52DCC735BE642E450ED1507C0F10E73CD7CD4B82E6A9C189111` | external holdout |

The duplicate files under `决赛/算法仿真代码/MATLAB/实测数据集/A类活动` are excluded by hash and are never copied a second time.

Local, ignored dataset layout:

```text
IMU_Dataset/
  imu_dataset_for_final/                   # existing 189-file base dataset; unchanged
  finals_jumping_squat/
    train/jumping_squat/
      jumping_squat_scy1_20.txt
      jumping_squat_scy2_20.txt
    external_holdout/jumping_squat/
      jumping_squat_scy3_20.txt
```

Raw data remains ignored by Git, consistent with the repository policy. A tracked manifest records the three source hashes, local relative paths, roles, row counts, and first-six-column format.

## Import and Split Rules

1. `load_imu_file()` reads exactly the first six comma-separated numeric fields, accepting both existing eight-column files and finals rows with a trailing comma.
2. The first three fields remain gyroscope raw values divided by `16.4`; fields four through six remain accelerometer raw values divided by `4096.0`.
3. The timestamp field and any trailing empty field are ignored.
4. The base dataset continues through the existing stratified raw-file split before windowing.
5. Extra-train records are appended only after the base split. They must never enter base validation or base test records.
6. External-holdout records are not constructed, read, or reported during `--validation-only` runs. They are evaluated only once for the validation-selected candidate.
7. Every record carries a unique source ID based on the relative dataset root plus file name, so sampler weights and contrastive positives cannot merge two files with the same base name.

## Experiment Sequence

### Stage A: Data-only ablation

Keep the production feature bank and BP exactly unchanged:

```text
264 features
264 -> 96 -> 64 -> 32 -> 11
```

Run `--validation-only` with the base dataset plus the two extra training files. Candidate selection remains lexicographic:

1. validation minimum class recall;
2. validation `jumping_squat` recall;
3. validation macro-F1;
4. validation accuracy.

The candidate must exceed the established 264-feature validation reference of accuracy `0.9163`, macro-F1 `0.9116`, and minimum recall `0.7992` before any base-test or external-holdout data is read.

### Stage B: Compact event-feature ablation, conditional

Run Stage B only if Stage A does not pass the validation reference. Keep the same data assignment and add this 12-value deterministic bank after the current 264 features:

| Source | Feature |
|---|---|
| `acc_vertical` | normalized most-negative peak position |
| `acc_vertical` | normalized post-take-off largest positive peak position |
| `acc_vertical` | normalized take-off-to-landing interval |
| `acc_vertical` | landing-to-take-off peak magnitude ratio |
| `acc_mag` | free-flight sample ratio below `0.70 g` |
| `acc_mag` | longest free-flight run ratio below `0.70 g` |
| `acc_vertical` | maximum absolute first-difference magnitude |
| `acc_vertical` | normalized first-difference peak position |
| `gyro_mag` | normalized absolute peak position |
| `gyro_mag` and `acc_vertical` | normalized gyro-to-landing peak lag |
| `gyro_mag` and `acc_vertical` | Pearson correlation |
| `gyro_mag` and `acc_vertical` | post-take-off gyro energy ratio |

Event positions use the earliest index when extrema tie. If a post-take-off landing peak does not exist, the extractor emits deterministic zeros for all dependent features. No event feature uses timestamps, sensor orientation labels, or a learned threshold.

The generated C extractor must implement the exact same ordered 276-value vector before the Stage-B candidate is eligible for final evaluation.

## Final Evaluation and Export

Only the validation-selected candidate may run full evaluation. It reports, separately:

- the unchanged base test split across all 11 classes;
- the external `scy3` `jumping_squat` windows;
- file counts, source IDs, and all per-class recalls.

Formal ESP32 export still requires all 11 base-test classes to reach recall `>= 0.90`. In addition, report the `scy3` external-holdout `jumping_squat` recall, but do not silently substitute it for the 11-class export gate.

If the model passes the base export gate, generate `outputs/.../esp32_bp_model.h`, synchronize it to `esp32/include/esp32_bp_model.h`, compile it as C99, and compare C/Python feature outputs on both one existing-data window and one finals-data window. If it does not pass, preserve reports and logs while leaving the formal ESP32 header absent.

## Tests and Documentation

Tests must demonstrate:

- trailing-comma finals rows load as the first six numeric channels;
- existing eight-column rows retain their current conversion result;
- SHA-256 manifest validation rejects an unexpected duplicate or altered input;
- extra training files cannot appear in validation or base test partitions;
- external holdout is skipped by validation-only execution;
- the event feature vector has 12 finite deterministic entries and stable zero fallbacks;
- C/Python parity covers the expanded bank if Stage B is implemented.

Update the Chinese root README, `python/README.md`, and `docs/论文依据与优化取舍.md` with the source provenance, data split, experiment outcome, external-holdout result, and any final model/header status. Commit and push source, tests, manifests, documentation, and generated header only when the model satisfies the existing export gate.

## Limits

The three finals sessions are additional sessions, not confirmed additional people. Results remain raw-file and session independent, not subject independent. The known base test set has been inspected during earlier diagnosis and remains development evidence rather than a pristine final deployment claim.
