# Finals Jumping-Squat Data Expansion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two hash-verified finals `jumping_squat` sessions to training, preserve a third session as a delayed external holdout, and run a PyCharm-visible validation-first BP experiment without changing the 264-feature production extractor unless Stage A fails.

**Architecture:** A tracked JSON manifest and a small preparation script copy only verified source files into ignored local train/holdout trees. The trainer scans extra training records with the base class map and appends them after the original file-level split; it does not scan the external holdout in validation-only mode. Stage A changes data coverage only. Stage B conditionally appends 12 deterministic event features in matching Python and generated C implementations.

**Tech Stack:** Python 3, NumPy, PyTorch, scikit-learn, `unittest`, PowerShell, generated C99, Git.

---

### Task 1: Prepare hash-verified finals data

**Files:**
- Create: `python/finals_jumping_squat_manifest.json`
- Create: `python/prepare_finals_dataset.py`
- Create: `python/test_prepare_finals_dataset.py`

- [ ] **Step 1: Write failing manifest/preparation tests**

Add tests that build temporary source files and assert:

```python
prepared = prepare_dataset(manifest_path, source_dir, output_dir)
self.assertEqual(prepared["extra_train_count"], 2)
self.assertEqual(prepared["external_holdout_count"], 1)
self.assertTrue((output_dir / "train/jumping_squat/scy1.txt").exists())
```

Add separate tests asserting an altered SHA-256 raises `ValueError` and duplicate content assigned to two manifest entries raises `ValueError`.

- [ ] **Step 2: Run tests and verify RED**

Run:

```powershell
G:\Free_Project\BiShengBei_BPNN_ESP32\Project\.venv\Scripts\python.exe -m unittest python.test_prepare_finals_dataset -v
```

Expected: import failure because `python.prepare_finals_dataset` does not exist.

- [ ] **Step 3: Implement manifest validation and copying**

Create a manifest with exactly these files and hashes:

```json
{
  "version": 1,
  "label": "jumping_squat",
  "files": [
    {"name": "jumping_squat_scy1_20.txt", "sha256": "FE10A5B4D232DDB6D7A7BD791D3F3765008577CAA6CB317C9AF21BD4BDE0F379", "rows": 2975, "role": "extra_train"},
    {"name": "jumping_squat_scy2_20.txt", "sha256": "E9A02819A55DE955B1861B4EB23C992A9873159984BC8E1C6707053A6C3A52C9", "rows": 2977, "role": "extra_train"},
    {"name": "jumping_squat_scy3_20.txt", "sha256": "4B4C5420FEBDF52DCC735BE642E450ED1507C0F10E73CD7CD4B82E6A9C189111", "rows": 2969, "role": "external_holdout"}
  ]
}
```

Implement `prepare_dataset(manifest_path, source_dir, output_dir)` with `hashlib.sha256`, nonblank row counting, duplicate-hash rejection, and `shutil.copy2`. Map `extra_train` to `train/jumping_squat/` and `external_holdout` to `external_holdout/jumping_squat/`. Expose CLI arguments `--manifest`, `--source-dir`, and `--output-dir` and print a JSON summary without printing file contents.

- [ ] **Step 4: Run preparation tests and verify GREEN**

Run the Task 1 test command. Expected: all Task 1 tests pass.

- [ ] **Step 5: Prepare the real ignored local data tree**

Run:

```powershell
G:\Free_Project\BiShengBei_BPNN_ESP32\Project\.venv\Scripts\python.exe python\prepare_finals_dataset.py `
  --manifest python\finals_jumping_squat_manifest.json `
  --source-dir "G:\Free_Project\BiShengBei_BPNN_ESP32\决赛\MATLAB\实测数据集\A类活动" `
  --output-dir "G:\Free_Project\BiShengBei_BPNN_ESP32\Project\IMU_Dataset\finals_jumping_squat"
```

Expected: `extra_train_count=2`, `external_holdout_count=1`, no duplicate or hash error.

### Task 2: Make IMU loading and extra-train partitioning explicit

**Files:**
- Modify: `python/train_export.py`
- Modify: `python/test_train_export.py`

- [ ] **Step 1: Write failing loader tests**

Add one test with a finals-style row `"164,328,-164,4096,0,-4096,1025,\n"` and assert `load_imu_file()` returns shape `(1, 6)` with gyro `[10, 20, -10]` and acceleration `[1, 0, -1]`. Keep the existing eight-column conversion test as the regression guard.

- [ ] **Step 2: Run the targeted loader test and verify RED**

Run:

```powershell
G:\Free_Project\BiShengBei_BPNN_ESP32\Project\.venv\Scripts\python.exe -m unittest python.test_train_export.TrainExportTests.test_load_imu_file_accepts_trailing_comma -v
```

Expected: `ValueError` from the empty trailing field.

- [ ] **Step 3: Implement the six-column loader**

Change `load_imu_file()` to:

```python
raw = np.loadtxt(path, delimiter=",", dtype=np.float32, usecols=tuple(range(6)))
return convert_raw_imu_units(raw)
```

- [ ] **Step 4: Run loader tests and verify GREEN**

Run the targeted test, then the full `python.test_train_export` suite. Expected: all pass.

- [ ] **Step 5: Write failing partition and CLI tests**

Add tests for wished-for APIs:

```python
extra = te.scan_labeled_dataset(extra_root, {"jumping_squat": 3})
train, val, test = te.split_records_for_experiment(base, extra, seed=7)
self.assertTrue(set(extra).issubset(set(train)))
self.assertTrue(set(extra).isdisjoint(set(val + test)))
```

Add a CLI parser test asserting `--extra-train-dir` and `--external-holdout-dir` are accepted. Add a validation-only orchestration test that passes a missing external-holdout path and verifies no attempt is made to scan it.

- [ ] **Step 6: Run partition tests and verify RED**

Expected: missing `scan_labeled_dataset`, `split_records_for_experiment`, and parser options.

- [ ] **Step 7: Implement extra-train scanning and delayed holdout loading**

Implement:

```python
def scan_labeled_dataset(dataset_dir: Path, label_to_idx: Dict[str, int]) -> List[ImuRecord]
def split_records_for_experiment(base_records, extra_train_records, seed=SEED)
```

`scan_labeled_dataset` rejects unknown label directories. `split_records_for_experiment` calls the existing base split and appends extra records only to train. Extend `train_one_experiment()` with `extra_train_records=()` and replace its split call with the new helper.

Add parser options:

```text
--extra-train-dir PATH
--external-holdout-dir PATH
```

In `main()`, scan extra training before experiments. Scan external holdout only after candidate selection and only when `validation_only` is false. Print `extra_train_file_count` and `external_holdout_loaded` states.

- [ ] **Step 8: Run partition and full tests and verify GREEN**

Expected: extra files appear only in train; validation-only does not touch holdout; all tests pass.

### Task 3: Report the external holdout separately

**Files:**
- Modify: `python/train_export.py`
- Modify: `python/test_train_export.py`

- [ ] **Step 1: Write failing external-evaluation tests**

Add tests for `evaluate_external_holdout(result, records, class_names, device)` using a tiny deterministic BP model and one temporary `jumping_squat` record. Assert the returned dictionary contains `file_count`, `sample_count`, `recall`, `files`, and `skipped=False`. Add a validation-only result test expecting `{"skipped": True, "reason": "validation_only"}`.

- [ ] **Step 2: Run targeted tests and verify RED**

Expected: missing external evaluation helper/result field.

- [ ] **Step 3: Implement separate holdout evaluation**

Use the selected result's `window_len`, `step_len`, thresholds, scaler, and model. Build unaugmented holdout samples, standardize with the selected training mean/std, predict, and calculate recall against the mapped `jumping_squat` label. Store the result under `external_holdout` in `training_report.json`; do not include it in `target_reached`.

- [ ] **Step 4: Run tests and verify GREEN**

Run the full unit suite. Expected: all tests pass and export-gate tests remain unchanged.

### Task 4: Run Stage A visibly in PyCharm

**Files:**
- Create locally ignored: `.codex-local/tmp/run_finals_stage_a_visible.ps1`
- Create locally ignored: `outputs/round8_finals_data_validation_20260711/`

- [ ] **Step 1: Open the isolated worktree in PyCharm**

Launch PyCharm with the worktree path and `python/train_export.py`, confirming a responsive window title before training.

- [ ] **Step 2: Run a two-epoch smoke check**

Run validation-only with `--max-epochs 2` if the CLI exposes the existing test override; otherwise call the trainer's smoke path used by current tests. Confirm both epochs print and the holdout path is not opened.

- [ ] **Step 3: Launch the full unbuffered Stage-A run**

Run:

```powershell
G:\Free_Project\BiShengBei_BPNN_ESP32\Project\.venv\Scripts\python.exe -u python\train_export.py `
  --dataset-dir "G:\Free_Project\BiShengBei_BPNN_ESP32\Project\IMU_Dataset\imu_dataset_for_final" `
  --extra-train-dir "G:\Free_Project\BiShengBei_BPNN_ESP32\Project\IMU_Dataset\finals_jumping_squat\train" `
  --external-holdout-dir "G:\Free_Project\BiShengBei_BPNN_ESP32\Project\IMU_Dataset\finals_jumping_squat\external_holdout" `
  --validation-only --window-seconds 2.5 `
  --output-dir outputs\round8_finals_data_validation_20260711
```

Use `Tee-Object` to persist `training_console.log`. Every epoch must be visible in the launched PowerShell/PyCharm workflow.

- [ ] **Step 4: Apply the fixed validation gate**

Read `validation_report.json` once. Stage A passes only if accuracy `> 0.9163`, macro-F1 `> 0.9116`, and minimum recall `> 0.7992`. Record `jumping_squat` validation recall separately.

### Task 5: Conditionally add the 12 event features

**Condition:** Execute only when Stage A fails the fixed validation gate.

**Files:**
- Modify: `python/train_export.py`
- Modify: `python/test_train_export.py`

- [ ] **Step 1: Write failing event-feature tests**

Assert `event_features(window)` returns exactly 12 finite values, extrema ties use the earliest index, no post-take-off segment yields deterministic dependent zeros, `build_feature_names()` ends with the 12 specified names, and total dimension is 276.

- [ ] **Step 2: Run event tests and verify RED**

Expected: missing `event_features` and dimension remains 264.

- [ ] **Step 3: Implement minimal Python event features**

Use `build_feature_series(window)` outputs, `np.argmin`, post-index `np.argmax`, threshold `acc_mag < 0.70`, first differences, `np.corrcoef` with zero-variance fallback, and normalized indices divided by `max(n - 1, 1)`. Append the 12 features and names after the existing bank.

- [ ] **Step 4: Run Python tests and verify GREEN**

Expected: all tests pass with 276 features.

- [ ] **Step 5: Write failing generated-C contract/parity tests**

Require `append_event_features` in the generated header and compile a parity harness that compares all 276 values on a synthetic jump window.

- [ ] **Step 6: Run C tests and verify RED**

Expected: generated header lacks event extraction or returns only 264 values.

- [ ] **Step 7: Implement matching C event extraction**

Add deterministic extrema, free-flight run, first-difference, correlation, lag, and post-take-off energy calculations using fixed loops and zero-denominator guards. Append values in the exact Python order.

- [ ] **Step 8: Verify C/Python parity and run Stage B visibly**

Require maximum absolute parity error `<= 1e-4`, then run a validation-only visible training output under `outputs/round9_finals_event_validation_20260711/`. Apply the same fixed validation gate.

### Task 6: Final evaluation, documentation, and publication

**Files:**
- Modify: `README.md`
- Modify: `python/README.md`
- Modify: `docs/论文依据与优化取舍.md`
- Conditionally create: `esp32/include/esp32_bp_model.h`

- [ ] **Step 1: Select exactly one validation candidate**

Choose Stage A or Stage B only by the fixed validation gate and lexicographic score. If neither passes, stop without loading base test or `scy3`, document the failed validation results, and keep the formal header absent.

- [ ] **Step 2: Run one full visible evaluation when a candidate passes**

Use the selected feature/data configuration without `--validation-only`. Load base test and external holdout once. Save complete epoch logs and `training_report.json`.

- [ ] **Step 3: Enforce export and parity gates**

Require every base-test class recall `>= 0.90` for formal header export. Report external `scy3` recall separately. If exported, compile C99 and compare Python/C features on one base window and one finals window with maximum absolute error `<= 1e-4`.

- [ ] **Step 4: Update Chinese documentation**

Document source hashes, role assignment, validation results, external-holdout result if evaluated, limits, final feature dimension, header status, and exact reproduction commands.

- [ ] **Step 5: Run fresh verification**

Run:

```powershell
G:\Free_Project\BiShengBei_BPNN_ESP32\Project\.venv\Scripts\python.exe -m py_compile python\train_export.py python\prepare_finals_dataset.py
G:\Free_Project\BiShengBei_BPNN_ESP32\Project\.venv\Scripts\python.exe -m unittest discover -s python -p "test_*.py"
git diff --check
git status --short
```

Expected: syntax succeeds, all tests pass, diff check is clean, raw data/output directories remain ignored.

- [ ] **Step 6: Commit and push the feature branch**

Stage only source, tests, manifest, docs, and a gate-qualified header if one exists. Commit with a Chinese message describing finals data integration, push the `codex/finals-jumping-squat` branch, and report the exact final metrics and any unmet gate.
