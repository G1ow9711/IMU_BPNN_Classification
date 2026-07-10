import unittest
import tempfile
import contextlib
import io
import json
import sys
from pathlib import Path
from unittest import mock

import numpy as np
import torch

from python import train_export as te


class TrainExportCoreTests(unittest.TestCase):
    def test_parse_args_accepts_targeted_window_list(self):
        with mock.patch.object(
            sys,
            "argv",
            ["train_export.py", "--window-seconds", "2.5"],
        ):
            args = te.parse_args()

        self.assertEqual(args.window_seconds, [2.5])

    def test_parse_args_accepts_primary_artifact_directory(self):
        with mock.patch.object(
            sys,
            "argv",
            [
                "train_export.py",
                "--primary-artifact-dir",
                "outputs/round2",
            ],
        ):
            args = te.parse_args()

        self.assertEqual(args.primary_artifact_dir, Path("outputs/round2"))

    def test_parse_args_accepts_validation_only_mode(self):
        with mock.patch.object(
            sys,
            "argv",
            ["train_export.py", "--validation-only", "--window-seconds", "2.5"],
        ):
            args = te.parse_args()

        self.assertTrue(args.validation_only)

    def test_convert_raw_imu_units_uses_plan_scales(self):
        raw = np.array([[16.4, -32.8, 49.2, 4096.0, -8192.0, 2048.0, 0.0, 0.0]])

        converted = te.convert_raw_imu_units(raw)

        np.testing.assert_allclose(
            converted[0],
            np.array([1.0, -2.0, 3.0, 1.0, -2.0, 0.5]),
            rtol=1e-6,
            atol=1e-6,
        )

    def test_extract_features_returns_264_ordered_values_for_six_axis_window(self):
        window = np.arange(62 * 6, dtype=np.float32).reshape(62, 6)

        features = te.extract_features(window)

        feature_names = te.build_feature_names()
        self.assertEqual(features.shape, (264,))
        self.assertEqual(len(feature_names), 264)
        self.assertEqual(feature_names[112], "acc_vertical_phase0_mean")
        self.assertEqual(feature_names[160], "acc_vertical_high_activity_ratio")
        self.assertEqual(feature_names[184], "acc_vertical_normalized_phase0_mean")
        self.assertEqual(feature_names[232], "acc_vertical_q10")
        self.assertNotIn("acc_vertical_argmax_abs_position", feature_names)
        self.assertTrue(np.all(np.isfinite(features)))

    def test_normalized_phase_features_ignore_offset_and_positive_scale(self):
        signal = np.linspace(-2.0, 3.0, 64, dtype=np.float32) ** 3

        original = te.normalized_phase_features(signal)
        transformed = te.normalized_phase_features(signal * 4.5 + 17.0)

        np.testing.assert_allclose(original, transformed, atol=2e-5, rtol=2e-5)

    def test_impact_distribution_features_are_finite_and_capture_max_jump(self):
        signal = np.array([0.0, 1.0, 2.0, 10.0, 3.0, 4.0], dtype=np.float32)

        features = te.impact_distribution_features(signal)

        self.assertEqual(len(features), 8)
        self.assertTrue(np.all(np.isfinite(features)))
        self.assertAlmostEqual(features[-1], 8.0)

    def test_robust_temporal_features_are_shift_stable_for_periodic_signal(self):
        timeline = np.arange(64, dtype=np.float32)
        signal = np.sin(2.0 * np.pi * 4.0 * timeline / 64.0).astype(np.float32)

        original = te.temporal_features(signal)
        shifted = te.temporal_features(np.roll(signal, 8))

        self.assertEqual(len(original), 6)
        np.testing.assert_allclose(original, shifted, atol=1e-5, rtol=1e-5)

    def test_split_records_by_file_keeps_each_source_in_one_split(self):
        records = [
            te.ImuRecord(Path(f"class_a/file_{i}.txt"), "class_a", 0) for i in range(8)
        ] + [
            te.ImuRecord(Path(f"class_b/file_{i}.txt"), "class_b", 1) for i in range(8)
        ]

        train, val, test = te.split_records_by_file(records, seed=123)

        split_paths = [
            {record.path for record in train},
            {record.path for record in val},
            {record.path for record in test},
        ]
        self.assertTrue(split_paths[0].isdisjoint(split_paths[1]))
        self.assertTrue(split_paths[0].isdisjoint(split_paths[2]))
        self.assertTrue(split_paths[1].isdisjoint(split_paths[2]))
        self.assertEqual(sum(len(paths) for paths in split_paths), len(records))

    def test_gravity_aligned_series_are_invariant_to_joint_rotation(self):
        rng = np.random.default_rng(12)
        window = rng.normal(size=(62, 6)).astype(np.float32)
        window[:, 5] += 1.0
        rotation = te.euler_rotation_matrix(0.2, -0.1, 0.15)

        rotated = te.rotate_imu_window(window, rotation)
        original_series = te.gravity_aligned_series(window)
        rotated_series = te.gravity_aligned_series(rotated)

        for original, transformed in zip(original_series, rotated_series):
            np.testing.assert_allclose(original, transformed, atol=1e-5, rtol=1e-5)

    def test_time_warp_preserves_endpoints_without_circular_wrap(self):
        window = np.repeat(np.arange(50, dtype=np.float32)[:, None], 6, axis=1)

        warped = te.time_warp_window(
            window,
            np.random.default_rng(7),
            max_displacement=0.03,
        )

        self.assertEqual(warped.shape, window.shape)
        np.testing.assert_allclose(warped[0], window[0], atol=1e-6)
        np.testing.assert_allclose(warped[-1], window[-1], atol=1e-6)
        self.assertTrue(np.all(np.diff(warped[:, 0]) >= 0.0))

    def test_dynamic_filter_rejects_low_activity_window(self):
        quiet = np.zeros((62, 6), dtype=np.float32)

        keep = te.keep_window_for_label(
            quiet,
            "tuck_jump",
            rest_threshold=0.08,
            active_point_threshold=0.02,
        )

        self.assertFalse(keep)

    def test_file_balanced_weights_equalize_class_and_file_mass(self):
        labels = np.array([0, 0, 0, 0, 1, 1], dtype=np.int64)
        file_ids = np.array([0, 0, 0, 1, 2, 3], dtype=np.int64)

        weights = te.file_balanced_sample_weights(labels, file_ids)

        self.assertAlmostEqual(
            float(weights[file_ids == 0].sum()),
            float(weights[file_ids == 1].sum()),
        )
        self.assertAlmostEqual(
            float(weights[labels == 0].sum()),
            float(weights[labels == 1].sum()),
        )

    def test_build_samples_filters_quiet_dynamic_file_and_returns_file_ids(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            quiet_path = root / "quiet.txt"
            active_path = root / "active.txt"
            quiet = np.zeros((100, 8), dtype=np.float32)
            quiet[:, 5] = 4096.0
            timeline = np.linspace(0.0, 8.0 * np.pi, 100, dtype=np.float32)
            active = quiet.copy()
            active[:, 0] = np.sin(timeline) * 300.0 * 16.4
            active[:, 5] = (1.0 + 0.8 * np.sin(timeline)) * 4096.0
            np.savetxt(quiet_path, quiet, delimiter=",")
            np.savetxt(active_path, active, delimiter=",")
            records = [
                te.ImuRecord(quiet_path, "tuck_jump", 0),
                te.ImuRecord(active_path, "tuck_jump", 0),
            ]

            features, labels, file_ids, stats = te.build_samples(
                records,
                window_len=50,
                step_len=25,
                rest_threshold=0.08,
                active_point_threshold=0.02,
                augment=False,
                rng=np.random.default_rng(3),
            )

            self.assertGreater(len(features), 0)
            self.assertEqual(len(features), len(labels))
            self.assertEqual(len(features), len(file_ids))
            self.assertTrue(np.all(file_ids == 1))
            self.assertEqual(stats["files_without_valid_window"], 1)

    def test_training_loader_uses_file_balanced_weighted_sampler(self):
        x = np.arange(6 * 4, dtype=np.float32).reshape(6, 4)
        y = np.array([0, 0, 0, 0, 1, 1], dtype=np.int64)
        file_ids = np.array([0, 0, 0, 1, 2, 3], dtype=np.int64)

        loader = te.make_loader(
            x,
            y,
            batch_size=2,
            shuffle=False,
            file_ids=file_ids,
            file_balanced=True,
            seed=11,
        )

        self.assertIsInstance(loader.sampler, torch.utils.data.WeightedRandomSampler)
        self.assertEqual(len(next(iter(loader))), 3)

    def test_cross_file_supcon_prefers_same_class_embeddings(self):
        labels = torch.tensor([0, 0, 1, 1])
        file_ids = torch.tensor([0, 1, 2, 3])
        good = torch.tensor([[1.0, 0.0], [0.9, 0.1], [0.0, 1.0], [0.1, 0.9]])
        bad = torch.tensor([[1.0, 0.0], [0.0, 1.0], [0.9, 0.1], [0.1, 0.9]])

        good_loss = te.cross_file_supervised_contrastive_loss(good, labels, file_ids)
        bad_loss = te.cross_file_supervised_contrastive_loss(bad, labels, file_ids)

        self.assertTrue(torch.isfinite(good_loss))
        self.assertGreaterEqual(float(good_loss), 0.0)
        self.assertLess(float(good_loss), float(bad_loss))

    def test_hard_pair_margin_penalizes_confusing_logit_above_true_logit(self):
        class_names = ["jumping_squat", "squat", "tuck_jump"]
        labels = torch.tensor([1])
        wrong_order = torch.tensor([[2.0, 0.0, 0.0]])
        correct_order = torch.tensor([[0.0, 2.0, 0.0]])

        wrong_loss = te.hard_pair_margin_loss(wrong_order, labels, class_names)
        correct_loss = te.hard_pair_margin_loss(correct_order, labels, class_names)

        self.assertGreater(float(wrong_loss), float(correct_loss))

    def test_bpnet_exposes_32_value_training_embedding(self):
        model = te.BPNet(input_dim=264, class_count=11, dropout=0.0)
        batch = torch.zeros((3, 264), dtype=torch.float32)

        embedding = model.forward_features(batch)
        logits = model(batch)

        self.assertEqual(tuple(embedding.shape), (3, 32))
        self.assertEqual(tuple(logits.shape), (3, 11))

    def test_train_model_prints_every_epoch_with_component_and_weak_metrics(self):
        rng = np.random.default_rng(21)
        class_names = ["jumping_squat", "squat", "tuck_jump", "jumping_lunge"]
        train_x = rng.normal(size=(24, 264)).astype(np.float32)
        train_y = np.repeat(np.arange(4, dtype=np.int64), 6)
        train_file_ids = np.tile(np.array([0, 1, 0, 1, 0, 1], dtype=np.int64), 4)
        val_x = rng.normal(size=(12, 264)).astype(np.float32)
        val_y = np.repeat(np.arange(4, dtype=np.int64), 3)
        old_max_epochs = te.MAX_EPOCHS
        te.MAX_EPOCHS = 2
        output = io.StringIO()
        try:
            with contextlib.redirect_stdout(output):
                te.train_model(
                    train_x,
                    train_y,
                    train_file_ids,
                    val_x,
                    val_y,
                    class_names=class_names,
                    device=torch.device("cpu"),
                    progress_label="window=test",
                )
        finally:
            te.MAX_EPOCHS = old_max_epochs

        log = output.getvalue()
        self.assertIn("epoch=001", log)
        self.assertIn("epoch=002", log)
        self.assertIn("ce=", log)
        self.assertIn("supcon=", log)
        self.assertIn("margin=", log)
        self.assertIn("val_weak_f1=", log)
        self.assertIn("val_worst_f1=", log)
        self.assertIn("val_weak_recall=", log)
        self.assertIn("val_min_recall=", log)

    def test_exported_header_contains_264_feature_pipeline_and_activity_thresholds(self):
        feature_names = te.build_feature_names()
        model = te.BPNet(input_dim=len(feature_names), class_count=3, dropout=0.0)
        result = {
            "model": model,
            "mean": np.zeros(len(feature_names), dtype=np.float32),
            "std": np.ones(len(feature_names), dtype=np.float32),
            "window_len": 62,
            "rest_threshold": 0.08,
            "active_point_threshold": 0.02,
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            header_path = Path(temp_dir) / "esp32_bp_model.h"

            te.export_esp32_header(
                result,
                ["class_a", "class_b", "class_c"],
                feature_names,
                header_path,
            )

            header = header_path.read_text(encoding="utf-8")
        self.assertIn("#define FEATURE_DIM 264", header)
        self.assertIn("REST_MOTION_THRESHOLD", header)
        self.assertIn("ACTIVE_POINT_THRESHOLD", header)
        self.assertIn("append_phase_features", header)
        self.assertIn("append_temporal_features", header)
        self.assertIn("append_normalized_phase_features", header)
        self.assertIn("append_impact_distribution_features", header)
        self.assertIn("spectral_entropy", header)
        self.assertIn("gravity_norm", header)
        self.assertIn("bp_predict_from_window", header)

    def test_model_header_is_published_to_esp32_only_after_target_is_reached(self):
        feature_names = te.build_feature_names()
        model = te.BPNet(input_dim=len(feature_names), class_count=3, dropout=0.0)
        result = {
            "model": model,
            "mean": np.zeros(len(feature_names), dtype=np.float32),
            "std": np.ones(len(feature_names), dtype=np.float32),
            "window_len": 62,
            "rest_threshold": 0.08,
            "active_point_threshold": 0.02,
            "y_test": np.repeat(np.arange(3, dtype=np.int64), 10),
            "test_pred": np.repeat(np.arange(3, dtype=np.int64), 10),
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            output_header = root / "outputs" / "esp32_bp_model.h"
            repository_header = root / "esp32" / "include" / "esp32_bp_model.h"

            reached = te.export_model_headers(
                result,
                ["class_a", "class_b", "class_c"],
                feature_names,
                output_header,
                repository_header,
                export_when_below_target=False,
            )

            self.assertTrue(reached)
            self.assertEqual(
                output_header.read_text(encoding="utf-8"),
                repository_header.read_text(encoding="utf-8"),
            )

            result["test_pred"] = result["test_pred"].copy()
            result["test_pred"][20:22] = 1
            below_output = root / "below" / "esp32_bp_model.h"
            below_repository = root / "below-esp32" / "esp32_bp_model.h"
            reached = te.export_model_headers(
                result,
                ["class_a", "class_b", "class_c"],
                feature_names,
                below_output,
                below_repository,
                export_when_below_target=True,
            )

            self.assertFalse(reached)
            self.assertTrue(below_output.exists())
            self.assertFalse(below_repository.exists())

    def test_validation_only_outputs_omit_test_metrics_and_header(self):
        feature_names = te.build_feature_names()
        model = te.BPNet(len(feature_names), 3, dropout=0.0)
        result = {
            "window_seconds": 2.5,
            "window_len": 62,
            "step_len": 12,
            "rest_threshold": 0.08,
            "active_point_threshold": 0.02,
            "model": model,
            "mean": np.zeros(len(feature_names), dtype=np.float32),
            "std": np.ones(len(feature_names), dtype=np.float32),
            "val_acc": 0.91,
            "val_f1": 0.90,
            "val_weak_recall": 0.89,
            "val_min_recall": 0.88,
            "val_class_recalls": {"a": 0.88, "b": 0.91, "c": 0.93},
            "y_val": np.array([0, 1, 2], dtype=np.int64),
            "val_pred": np.array([0, 1, 2], dtype=np.int64),
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            output_dir = Path(temp_dir)
            te.save_validation_outputs(
                result,
                [result],
                ["a", "b", "c"],
                feature_names,
                output_dir,
            )

            report = json.loads(
                (output_dir / "validation_report.json").read_text(encoding="utf-8")
            )
            self.assertTrue((output_dir / "best_model.pt").exists())
            self.assertTrue((output_dir / "scaler_and_config.npz").exists())
            self.assertFalse((output_dir / "esp32_bp_model.h").exists())
        self.assertEqual(report["mode"], "validation_only")
        self.assertNotIn("test_acc", report)

    def test_deployment_gate_requires_every_class_recall_at_least_90_percent(self):
        y_true = np.repeat(np.arange(3, dtype=np.int64), 10)
        all_pass = y_true.copy()
        all_pass[[0, 10, 20]] = np.array([1, 2, 0])
        one_fails = all_pass.copy()
        one_fails[21] = 1

        reached, recalls = te.deployment_gate_status(
            {"y_test": y_true, "test_pred": all_pass},
            ["class_a", "class_b", "class_c"],
        )
        failed, failed_recalls = te.deployment_gate_status(
            {"y_test": y_true, "test_pred": one_fails},
            ["class_a", "class_b", "class_c"],
        )

        self.assertTrue(reached)
        np.testing.assert_allclose(recalls, np.full(3, 0.9), atol=1e-7)
        self.assertFalse(failed)
        self.assertAlmostEqual(float(failed_recalls[2]), 0.8)

    def test_checkpoint_key_prioritizes_minimum_recall_over_macro_f1(self):
        higher_min_recall = te.validation_checkpoint_key(
            val_min_recall=0.75,
            val_weak_recall=0.81,
            val_f1=0.89,
            val_acc=0.90,
        )
        higher_overall_f1 = te.validation_checkpoint_key(
            val_min_recall=0.73,
            val_weak_recall=0.82,
            val_f1=0.91,
            val_acc=0.92,
        )

        self.assertGreater(higher_min_recall, higher_overall_f1)

    def test_family_subset_remaps_global_labels_to_local_indices(self):
        class_names = ["good_morning", "jumping_jack", "jumping_squat", "squat"]
        family_names = ["jumping_jack", "jumping_squat", "squat"]
        x = np.arange(5 * 3, dtype=np.float32).reshape(5, 3)
        y = np.array([0, 1, 2, 3, 0], dtype=np.int64)
        file_ids = np.array([10, 11, 12, 13, 14], dtype=np.int64)

        family_x, family_y, family_file_ids = te.family_subset(
            x,
            y,
            file_ids,
            class_names,
            family_names,
        )

        np.testing.assert_array_equal(family_x, x[1:4])
        np.testing.assert_array_equal(family_y, np.array([0, 1, 2]))
        np.testing.assert_array_equal(family_file_ids, file_ids[1:4])

    def test_load_primary_artifacts_restores_model_and_scaler(self):
        feature_names = te.build_feature_names()
        model = te.BPNet(len(feature_names), 3, dropout=0.0)
        with tempfile.TemporaryDirectory() as temp_dir:
            artifact_dir = Path(temp_dir)
            torch.save(model.state_dict(), artifact_dir / "best_model.pt")
            np.savez(
                artifact_dir / "scaler_and_config.npz",
                mean=np.full(len(feature_names), 2.0, dtype=np.float32),
                std=np.full(len(feature_names), 3.0, dtype=np.float32),
                feature_names=np.asarray(feature_names),
                window_len=np.asarray([62]),
            )

            restored, mean, std = te.load_primary_artifacts(
                artifact_dir,
                input_dim=len(feature_names),
                class_count=3,
                expected_window_len=62,
                device=torch.device("cpu"),
            )

        self.assertIsInstance(restored, te.BPNet)
        np.testing.assert_array_equal(mean, np.full(len(feature_names), 2.0))
        np.testing.assert_array_equal(std, np.full(len(feature_names), 3.0))

    def test_family_specialist_only_replaces_predictions_inside_family(self):
        class_names = ["good_morning", "jumping_jack", "jumping_squat", "squat"]
        family_names = ["jumping_jack", "jumping_squat", "squat"]
        primary_pred = np.array([0, 1, 2, 3, 0], dtype=np.int64)
        specialist_pred = np.array([2, 2, 0, 1, 1], dtype=np.int64)

        combined = te.route_family_predictions(
            primary_pred,
            specialist_pred,
            class_names,
            family_names,
        )

        np.testing.assert_array_equal(combined, np.array([0, 3, 1, 2, 0]))

    def test_jump_shape_feature_selection_excludes_raw_amplitude_quantiles(self):
        feature_names = te.build_feature_names()

        indices = te.build_jump_shape_feature_indices(feature_names)
        selected = [feature_names[index] for index in indices]

        self.assertGreater(len(indices), 70)
        self.assertTrue(any("normalized_phase" in name for name in selected))
        self.assertTrue(any(name.endswith("spectral_entropy") for name in selected))
        self.assertTrue(any(name.endswith("_skew") for name in selected))
        self.assertFalse(any(name.endswith("_q90") for name in selected))
        self.assertFalse(any(name.endswith("_max") for name in selected))

    def test_exported_header_contains_family_specialist_network_and_routing(self):
        feature_names = te.build_feature_names()
        class_names = ["good_morning", "jumping_jack", "jumping_squat", "squat"]
        family_names = ["jumping_jack", "jumping_squat", "squat"]
        selected_indices = [0, 17, 184]
        result = {
            "model": te.BPNet(len(feature_names), len(class_names), dropout=0.0),
            "specialist_model": te.BPNet(
                len(selected_indices), len(family_names), dropout=0.0
            ),
            "mean": np.zeros(len(feature_names), dtype=np.float32),
            "std": np.ones(len(feature_names), dtype=np.float32),
            "specialist_mean": np.full(len(selected_indices), 2.0, dtype=np.float32),
            "specialist_std": np.full(len(selected_indices), 3.0, dtype=np.float32),
            "specialist_feature_indices": selected_indices,
            "specialist_class_names": family_names,
            "window_len": 62,
            "rest_threshold": 0.08,
            "active_point_threshold": 0.02,
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            header_path = Path(temp_dir) / "esp32_bp_model.h"

            te.export_esp32_header(result, class_names, feature_names, header_path)

            header = header_path.read_text(encoding="utf-8")
        self.assertIn("#define SPECIALIST_CLASS_NUM 3", header)
        self.assertIn("#define SPECIALIST_FEATURE_DIM 3", header)
        self.assertIn("SPECIALIST_GLOBAL_CLASS_INDEX", header)
        self.assertIn("SPECIALIST_FEATURE_INDEX", header)
        self.assertIn("SPECIALIST_FEATURE_MEAN", header)
        self.assertIn("SW1", header)
        self.assertIn("bp_family_specialist_predict", header)

    def test_c_float_emits_valid_float_literals_for_integer_values(self):
        self.assertEqual(te.c_float(0.0), "0.0f")
        self.assertEqual(te.c_float(1.0), "1.0f")
        self.assertEqual(te.c_float(-2.0), "-2.0f")

    def test_serializable_experiment_keeps_activity_threshold(self):
        keys = {
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
        result = {key: 0 for key in keys}
        result["active_point_threshold"] = 0.02

        serialized = te.serializable_experiment(result)

        self.assertEqual(serialized["active_point_threshold"], 0.02)


if __name__ == "__main__":
    unittest.main()
