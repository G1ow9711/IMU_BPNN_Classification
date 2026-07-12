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
    def test_preprocess_repairs_single_axis_spike_but_preserves_multi_axis_impact(self):
        # 构造七点静止手腕窗口，通道顺序为 gx、gy、gz、ax、ay、az。
        single_axis = np.zeros((7, 6), dtype=np.float32)
        # az 保持 1g，模拟正常重力基线。
        single_axis[:, 5] = 1.0
        # 中心 gx 突增 600 deg/s 且两邻点稳定，超过 300 deg/s 清洗门槛。
        single_axis[3, 0] = 600.0

        # 执行窗口级工程前处理。
        repaired = te.preprocess_imu_window(single_axis)

        # 单轴孤立点应被两邻点均值替换为零。
        self.assertAlmostEqual(float(repaired[3, 0]), 0.0, places=6)
        # 其余重力通道必须保持 1g。
        np.testing.assert_allclose(repaired[:, 5], np.ones(7, dtype=np.float32))

        # 复制窗口并构造陀螺与加速度同步的多轴冲击。
        multi_axis = single_axis.copy()
        # ax 同时增加 3g，使中心点有两个通道超过各自门槛。
        multi_axis[3, 3] = 3.0

        # 多轴同步变化更可能是真实落地/摆臂冲击，必须保留。
        preserved = te.preprocess_imu_window(multi_axis)

        # gx 冲击不得被单轴规则删除。
        self.assertAlmostEqual(float(preserved[3, 0]), 600.0, places=6)
        # ax 冲击同样保留。
        self.assertAlmostEqual(float(preserved[3, 3]), 3.0, places=6)

    def test_causal_logit_smoother_uses_only_recent_history_and_resets(self):
        # 使用三类、三窗口历史构造小型环形缓冲，便于精确验证淘汰顺序。
        smoother = te.CausalLogitSmoother(class_count=3, history_length=3)
        # 第一个窗口没有历史，输出必须等于当前 logits。
        first = smoother.update(np.asarray([3.0, 0.0, 0.0], dtype=np.float32))
        # 第二个窗口输出前两窗算术均值。
        second = smoother.update(np.asarray([0.0, 3.0, 0.0], dtype=np.float32))
        # 第三和第四窗口用于验证满缓冲后淘汰最旧第一窗。
        smoother.update(np.asarray([0.0, 0.0, 3.0], dtype=np.float32))
        fourth = smoother.update(np.asarray([0.0, 0.0, 6.0], dtype=np.float32))
        # 首窗输出保持原值。
        np.testing.assert_allclose(first, [3.0, 0.0, 0.0], atol=1e-7)
        # 两窗均值为 [1.5,1.5,0]。
        np.testing.assert_allclose(second, [1.5, 1.5, 0.0], atol=1e-7)
        # 第四次只保留窗口 2、3、4，均值为 [0,1,3]。
        np.testing.assert_allclose(fourth, [0.0, 1.0, 3.0], atol=1e-7)
        # 重置清除历史，使下一窗口不受前会话影响。
        smoother.reset()
        # 重置后首窗再次原样输出。
        np.testing.assert_allclose(
            smoother.update(np.asarray([1.0, 2.0, 4.0], dtype=np.float32)),
            [1.0, 2.0, 4.0],
            atol=1e-7,
        )

    def test_motion_segment_bounds_remove_inactive_edges_with_context(self):
        # 构造 200 点连续流，前 50 点和后 75 点为静止，中间 75 点为动作。
        data = np.zeros((200, 6), dtype=np.float32)
        # 静止和动作阶段均保留 1g 重力。
        data[:, 5] = 1.0
        # 中间 gx=400 deg/s，使逐点活动分数显著超过 0.13。
        data[50:125, 0] = 400.0

        # 使用 1 秒、20% 活动触发和 0.5 秒上下文计算动作半开区间。
        start, end = te.motion_segment_bounds(data, active_point_threshold=0.13)

        # 0.5 秒在 25 Hz 下为 12 点，动作起点 50 前保留至索引 38。
        self.assertEqual(start, 38)
        # 动作末点 124 后保留 12 点，半开终点为 137。
        self.assertEqual(end, 137)

        # 完全静止流必须保留全段，供 sit 类和静态状态处理。
        static_start, static_end = te.motion_segment_bounds(
            np.column_stack(
                [
                    np.zeros((200, 5), dtype=np.float32),
                    np.ones(200, dtype=np.float32),
                ]
            ),
            active_point_threshold=0.13,
        )
        # 无动作触发时不裁剪。
        self.assertEqual((static_start, static_end), (0, 200))

    def test_deep_narrow_bpnet_has_reviewed_shape_and_parameter_count(self):
        # 构造 297 维 M1；六分支输入顺序与 M0 完全一致。
        model = te.DeepNarrowMultiBranchBPNet(input_dim=297, class_count=11, dropout=0.0)
        # 三个样本用于验证批维不会因新增融合层改变。
        samples = torch.randn(3, 297)

        # M1 最终共享嵌入按审核方案收缩为 24 维。
        embeddings = model.forward_features(samples)
        # 主分类输出仍为 11 类 logits。
        logits = model(samples)
        # 汇总全部可训练张量元素，核对 ESP32 参数预算。
        parameter_count = sum(parameter.numel() for parameter in model.parameters())

        # 六组输入不变，只有弱类分支输出和融合深度变化。
        self.assertEqual(model.group_input_dims, (112, 48, 24, 48, 32, 33))
        self.assertEqual(model.group_output_dims, (24, 12, 8, 12, 8, 24))
        # 共享表示必须为 [批大小,24]。
        self.assertEqual(tuple(embeddings.shape), (3, 24))
        # 分类输出必须为 [批大小,11]。
        self.assertEqual(tuple(logits.shape), (3, 11))
        # 297 维输入和 11 类输出下，审核公式得到 16619 个参数。
        self.assertEqual(parameter_count, 16619)

    def test_multi_branch_bpnet_keeps_feature_groups_and_32_value_embedding(self):
        # 构造 297 维候选模型；六组输入必须按生产特征顺序独立编码后融合。
        model = te.MultiBranchBPNet(input_dim=297, class_count=11, dropout=0.0)
        # 四个样本覆盖前向批维，输入形状为 [4,297]。
        samples = torch.randn(4, 297)

        # 训练嵌入供监督对比损失和辅助分类头使用。
        embeddings = model.forward_features(samples)
        # 主分类前向仍输出 11 类 logits，部署时只保留该路径。
        logits = model(samples)

        # 融合嵌入固定为 32 维，与原 BP 最后一层输入合同一致。
        self.assertEqual(tuple(embeddings.shape), (4, 32))
        # 主分类输出必须保持 [批大小,类别数]。
        self.assertEqual(tuple(logits.shape), (4, 11))
        # 六个分支分别对应 112/48/24/48/32/33 维特征组。
        self.assertEqual(model.group_input_dims, (112, 48, 24, 48, 32, 33))

    def test_multi_branch_auxiliary_loss_is_finite_for_declared_tasks(self):
        # 使用完整 11 类顺序构造多分支模型和每类一个样本。
        class_names = [
            "good_morning", "jumping_jack", "jumping_lunge", "jumping_squat",
            "lunge", "sit", "squat", "trot", "tuck_jump", "walk", "wave",
        ]
        model = te.MultiBranchBPNet(297, len(class_names), dropout=0.0)
        # 嵌入形状为 [11,32]，标签覆盖所有辅助任务正负样本。
        embeddings = torch.randn(len(class_names), 32)
        labels = torch.arange(len(class_names), dtype=torch.long)

        # 计算是否跳跃、强腾空、左右交替及两组易混二分类辅助损失。
        loss = model.auxiliary_loss(embeddings, labels, class_names)

        # 辅助损失必须有限且为正，确保五个训练头均参与优化。
        self.assertTrue(torch.isfinite(loss))
        self.assertGreater(float(loss.item()), 0.0)

    def test_pk_file_batch_contains_equal_classes_and_multiple_files(self):
        # 三类各提供两个文件、每文件两个窗口，便于检查 P=3、K=2 批次。
        x = np.arange(12 * 4, dtype=np.float32).reshape(12, 4)
        y = np.repeat(np.arange(3, dtype=np.int64), 4)
        file_ids = np.tile(np.repeat(np.arange(2, dtype=np.int64), 2), 3)

        # P×K 模式忽略普通 batch_size，按每类 K=2 形成 6 样本批次。
        loader = te.make_loader(
            x, y, batch_size=64, shuffle=False, file_ids=file_ids,
            pk_file_balanced=True, pk_samples_per_class=2, seed=17,
        )
        # 读取首批的特征、标签和文件编号。
        _, batch_y, batch_files = next(iter(loader))

        # 每个类别必须恰好出现两个样本，保证 SupCon 有同类正样本。
        self.assertEqual(torch.bincount(batch_y, minlength=3).tolist(), [2, 2, 2])
        # 每类两个样本必须优先来自两个不同文件，避免同文件重叠窗伪正样本。
        for label in range(3):
            self.assertEqual(len(torch.unique(batch_files[batch_y == label])), 2)

    def test_parse_args_accepts_integrated_weak_class_training_switches(self):
        # 模拟第二阶段可见训练命令，三个开关应可同时解析。
        with mock.patch.object(
            sys,
            "argv",
            [
                "train_export.py",
                "--multi-branch",
                "--pk-batches",
                "--auxiliary-heads",
            ],
        ):
            # 解析后的布尔值将由 main 传入每个窗口实验。
            args = te.parse_args()

        # 六组特征分支必须被启用。
        self.assertTrue(args.multi_branch)
        # P×K 多文件平衡批次必须被启用。
        self.assertTrue(args.pk_batches)
        # 五个训练期辅助任务必须被启用。
        self.assertTrue(args.auxiliary_heads)

    def test_pk_ce_prior_weights_restore_original_class_mass(self):
        # 类别 0/1/2 在原训练窗口中分别出现 2/4/6 次，表示非均匀自然先验。
        labels = np.repeat(np.arange(3, dtype=np.int64), [2, 4, 6])

        # P×K 批次本身均匀，CE 权重应按原窗口计数恢复 1:2:3 的相对质量。
        weights = te.pk_ce_class_weights(labels, 3, torch.device("cpu"))

        # 权重均值归一到 1，绝对尺度不改变 PyTorch 加权 CE 的归一化结果。
        self.assertTrue(torch.allclose(weights, torch.tensor([0.5, 1.0, 1.5])))

    def test_parse_args_accepts_round25_regularization_controls(self):
        # 模拟 Round25 的先验修正、SupCon 权重和多分支 dropout 参数。
        with mock.patch.object(
            sys,
            "argv",
            [
                "train_export.py",
                "--pk-prior-corrected-ce",
                "--supcon-weight",
                "0.01",
                "--dropout",
                "0.20",
            ],
        ):
            # 解析结果由 main 逐窗口传入 train_model。
            args = te.parse_args()

        # CE 应启用原训练窗口先验修正。
        self.assertTrue(args.pk_prior_corrected_ce)
        # SupCon 权重必须保留浮点精度。
        self.assertAlmostEqual(args.supcon_weight, 0.01)
        # 多分支融合层 dropout 应为 20%。
        self.assertAlmostEqual(args.dropout, 0.20)

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

    def test_parse_args_accepts_explicit_four_second_context(self):
        with mock.patch.object(
            sys,
            "argv",
            ["train_export.py", "--window-seconds", "4.0"],
        ):
            args = te.parse_args()

        self.assertEqual(args.window_seconds, [4.0])

    def test_parse_args_accepts_additional_dataset_directories(self):
        with mock.patch.object(
            sys,
            "argv",
            [
                "train_export.py",
                "--extra-train-dir",
                "IMU_Dataset/finals/train",
                "--external-holdout-dir",
                "IMU_Dataset/finals/external_holdout",
            ],
        ):
            args = te.parse_args()

        self.assertEqual(args.extra_train_dir, Path("IMU_Dataset/finals/train"))
        self.assertEqual(
            args.external_holdout_dir,
            Path("IMU_Dataset/finals/external_holdout"),
        )

    def test_parse_args_accepts_valid_ema_decay(self):
        with mock.patch.object(
            sys,
            "argv",
            ["train_export.py", "--ema-decay", "0.9"],
        ):
            args = te.parse_args()

        self.assertEqual(args.ema_decay, 0.9)

    def test_ema_decay_rejects_values_outside_zero_to_one(self):
        for value in ("-0.1", "1.0"):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    te.parse_ema_decay(value)

    def test_parse_args_accepts_valid_label_smoothing(self):
        with mock.patch.object(
            sys,
            "argv",
            ["train_export.py", "--label-smoothing", "0.05"],
        ):
            args = te.parse_args()

        self.assertEqual(args.label_smoothing, 0.05)

    def test_label_smoothing_rejects_values_outside_zero_to_one(self):
        for value in ("-0.1", "1.0"):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    te.parse_label_smoothing(value)

    def test_update_ema_state_averages_floating_parameters(self):
        previous = {
            "weight": torch.tensor([2.0, 4.0]),
            "step": torch.tensor(3, dtype=torch.int64),
        }
        current = {
            "weight": torch.tensor([6.0, 8.0]),
            "step": torch.tensor(4, dtype=torch.int64),
        }

        updated = te.update_ema_state(previous, current, decay=0.75)

        torch.testing.assert_close(updated["weight"], torch.tensor([3.0, 5.0]))
        self.assertEqual(int(updated["step"]), 4)
        self.assertIsNot(updated["weight"], current["weight"])

    def test_convert_raw_imu_units_uses_plan_scales(self):
        raw = np.array([[16.4, -32.8, 49.2, 4096.0, -8192.0, 2048.0, 0.0, 0.0]])

        converted = te.convert_raw_imu_units(raw)

        np.testing.assert_allclose(
            converted[0],
            np.array([1.0, -2.0, 3.0, 1.0, -2.0, 0.5]),
            rtol=1e-6,
            atol=1e-6,
        )

    def test_load_imu_file_accepts_trailing_comma(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "finals.txt"
            path.write_text(
                "164,328,-164,4096,0,-4096,1025,\n",
                encoding="utf-8",
            )

            loaded = te.load_imu_file(path)

        self.assertEqual(loaded.shape, (1, 6))
        np.testing.assert_allclose(
            loaded[0],
            np.array([10.0, 20.0, -10.0, 1.0, 0.0, -1.0]),
            rtol=1e-6,
            atol=1e-6,
        )

    def test_extract_features_returns_297_ordered_values_for_six_axis_window(self):
        # 构造 62 个采样点、六轴顺序为 gx/gy/gz/ax/ay/az 的确定性测试窗口。
        window = np.arange(62 * 6, dtype=np.float32).reshape(62, 6)

        # 调用生产特征提取器，验证 Python 端最终输入向量的维度和顺序。
        features = te.extract_features(window)

        # 获取与 ESP32 头文件共享的特征名称顺序，名称索引必须与数值索引一致。
        feature_names = te.build_feature_names()
        # 已验证 296 维合同追加清洗后第一自相关峰，总维度必须为 297。
        self.assertEqual(features.shape, (297,))
        # 名称数量必须等于模型输入维度，防止标准化参数与 C 数组错位。
        self.assertEqual(len(feature_names), 297)
        self.assertEqual(feature_names[112], "acc_vertical_phase0_mean")
        self.assertEqual(feature_names[160], "acc_vertical_high_activity_ratio")
        self.assertEqual(feature_names[184], "acc_vertical_normalized_phase0_mean")
        self.assertEqual(feature_names[232], "acc_vertical_q10")
        self.assertEqual(
            # 检查最后 33 项弱类特征固定顺序，防止标准化参数和 ESP32 模型错位。
            feature_names[-33:],
            [
                "acc_delta_mag_spectral_mid_band_ratio",
                "acc_vertical_spectral_high_band_ratio",
                "gyro_mag_spectral_centroid_hz",
                "acc_horizontal_mag_spectral_centroid_hz",
                "acc_vertical_autocorr_first_zero_seconds",
                "event_gyro_vertical_correlation",
                "acc_horizontal_mag_autocorr_secondary_peak",
                "gyro_mag_spectral_high_band_ratio",
                "acc_vertical_spectral_peak_power_ratio",
                "acc_delta_mag_spectral_high_band_ratio",
                "acc_horizontal_mag_spectral_mid_band_ratio",
                "gyro_mag_spectral_peak_power_ratio",
                "acc_vertical_spectral_mid_band_ratio",
                "acc_horizontal_mag_spectral_high_band_ratio",
                "gyro_mag_positive_peak_amplitude_cv",
                "acc_vertical_positive_peak_interval_cv",
                "acc_vertical_to_gyro_mag_max_xcorr",
                "acc_vertical_to_acc_horizontal_mag_max_xcorr",
                "acc_vertical_spectral_low_band_ratio",
                "acc_horizontal_mag_spectral_low_band_ratio",
                "gyro_mag_spectral_low_band_ratio",
                "acc_horizontal_mag_autocorr_prominent_peak_count",
                "acc_horizontal_mag_positive_peak_interval_cv",
                "acc_horizontal_mag_spectral_peak_power_ratio",
                "aligned_horizontal_acc_anisotropy",
                "aligned_horizontal_gyro_anisotropy",
                "aligned_takeoff_to_landing_seconds",
                "aligned_landing_impact_width_seconds",
                "aligned_flight_horizontal_gyro_integral_deg",
                "aligned_flight_vertical_gyro_integral_abs_deg",
                "wrist_reversal_rate_hz",
                "wrist_acf_second_first_ratio",
                "wrist_acf_first_peak",
            ],
        )
        self.assertNotIn("acc_vertical_argmax_abs_position", feature_names)
        self.assertTrue(np.all(np.isfinite(features)))

    def test_promoted_wrist_values_match_no_training_analyzer(self):
        # 延迟导入无训练分析器，明确把它作为候选公式的独立参考实现。
        from python import analyze_wrist_candidates as wrist_analysis

        # 固定随机种子构造可复现的 62 点手腕六轴窗口。
        rng = np.random.default_rng(20260712)
        # 陀螺三轴模拟约 ±250 deg/s 的摆动，形状为 [62,3]。
        gyro = rng.normal(0.0, 250.0, size=(62, 3))
        # 加速度三轴以 1g 静态 z 轴为基线并叠加 0.4g 动态变化。
        acceleration = rng.normal(0.0, 0.4, size=(62, 3))
        # z 轴加上 1g，使窗口同时覆盖冲击和重力偏置处理。
        acceleration[:, 2] += 1.0
        # 按 gx、gy、gz、ax、ay、az 顺序拼成生产输入 [62,6]。
        window = np.column_stack([gyro, acceleration]).astype(np.float32)

        # 分析器返回 16 项候选，代表本轮训练前的原始公式定义。
        analyzer_values = wrist_analysis.wrist_scalar_features(window)
        # 生产提取器返回 297 项，末项是清洗后晋级的第一自相关峰。
        production_values = te.extract_features(window)
        # 第一自相关峰在独立分析器中的固定索引为 12。
        expected = analyzer_values[12]

        # 生产末项必须在 float32 精度内逐值等于分析器定义。
        np.testing.assert_allclose(production_values[-1], expected, rtol=1e-6, atol=1e-6)

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

    def test_scan_labeled_dataset_uses_existing_class_map(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            action_dir = root / "jumping_squat"
            action_dir.mkdir()
            (action_dir / "scy1.txt").write_text("1,2,3,4,5,6,7,\n", encoding="utf-8")

            records = te.scan_labeled_dataset(root, {"jumping_squat": 3})

        self.assertEqual(
            records,
            [te.ImuRecord(action_dir / "scy1.txt", "jumping_squat", 3)],
        )

    def test_scan_labeled_dataset_rejects_unknown_action(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            action_dir = root / "unknown_action"
            action_dir.mkdir()
            (action_dir / "sample.txt").write_text("1,2,3,4,5,6\n", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "Unknown action"):
                te.scan_labeled_dataset(root, {"jumping_squat": 3})

    def test_split_records_for_experiment_appends_extra_only_to_train(self):
        base = [
            te.ImuRecord(Path(f"class_a/file_{i}.txt"), "class_a", 0)
            for i in range(8)
        ] + [
            te.ImuRecord(Path(f"class_b/file_{i}.txt"), "class_b", 1)
            for i in range(8)
        ]
        extra = [te.ImuRecord(Path("extra/scy1.txt"), "class_a", 0)]

        train, val, test = te.split_records_for_experiment(base, extra, seed=7)

        self.assertIn(extra[0], train)
        self.assertNotIn(extra[0], val)
        self.assertNotIn(extra[0], test)

    def test_validation_only_does_not_scan_external_holdout(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            extra_root = root / "extra"
            action_dir = extra_root / "jumping_squat"
            action_dir.mkdir(parents=True)
            (action_dir / "scy1.txt").write_text("1,2,3,4,5,6,7,\n", encoding="utf-8")
            missing_holdout = root / "missing_holdout"

            extra, holdout = te.load_additional_records(
                extra_root,
                missing_holdout,
                {"jumping_squat": 3},
                validation_only=True,
            )

        self.assertEqual(len(extra), 1)
        self.assertEqual(holdout, [])

    def test_external_holdout_reports_separate_jumping_squat_recall(self):
        class_names = ["jumping_jack", "jumping_squat"]
        feature_names = te.build_feature_names()
        model = te.BPNet(len(feature_names), len(class_names), dropout=0.0)
        with torch.no_grad():
            for parameter in model.parameters():
                parameter.zero_()
            model.net[8].bias[1] = 10.0
        result = {
            "model": model,
            "mean": np.zeros(len(feature_names), dtype=np.float32),
            "std": np.ones(len(feature_names), dtype=np.float32),
            "window_len": 50,
            "step_len": 25,
            "rest_threshold": 0.0,
            "active_point_threshold": 0.0,
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "scy3.txt"
            rows = []
            for index in range(80):
                gyro = 2000 if index % 2 else -2000
                acc_z = 7000 if index % 3 == 0 else 2500
                rows.append(f"{gyro},0,0,0,0,{acc_z},{index},\n")
            path.write_text("".join(rows), encoding="utf-8")
            records = [te.ImuRecord(path, "jumping_squat", 1)]

            report = te.evaluate_external_holdout(
                result,
                records,
                class_names,
                torch.device("cpu"),
            )

        self.assertFalse(report["skipped"])
        self.assertEqual(report["file_count"], 1)
        self.assertGreater(report["sample_count"], 0)
        self.assertEqual(report["recall"], 1.0)
        self.assertEqual(report["files"], [str(path)])

    def test_external_holdout_reports_recall_for_each_present_class(self):
        class_names = ["jumping_jack", "jumping_lunge", "jumping_squat"]
        feature_names = te.build_feature_names()
        model = te.BPNet(len(feature_names), len(class_names), dropout=0.0)
        with torch.no_grad():
            for parameter in model.parameters():
                parameter.zero_()
            model.net[8].bias[0] = 10.0
        result = {
            "model": model,
            "mean": np.zeros(len(feature_names), dtype=np.float32),
            "std": np.ones(len(feature_names), dtype=np.float32),
            "window_len": 50,
            "step_len": 25,
            "rest_threshold": 0.0,
            "active_point_threshold": 0.0,
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            records = []
            for label, label_idx in (("jumping_jack", 0), ("jumping_lunge", 1)):
                path = root / f"{label}.txt"
                path.write_text(
                    "".join(
                        f"{2000 if index % 2 else -2000},0,0,0,0,7000,{index},\n"
                        for index in range(80)
                    ),
                    encoding="utf-8",
                )
                records.append(te.ImuRecord(path, label, label_idx))

            report = te.evaluate_external_holdout(
                result, records, class_names, torch.device("cpu")
            )

        self.assertFalse(report["skipped"])
        self.assertEqual(report["class_recalls"], {"jumping_jack": 1.0, "jumping_lunge": 0.0})
        self.assertEqual(report["min_recall"], 0.0)
        self.assertEqual(report["macro_recall"], 0.5)

    def test_external_holdout_is_explicitly_skipped_in_validation_mode(self):
        report = te.evaluate_external_holdout(
            {},
            [],
            ["jumping_squat"],
            torch.device("cpu"),
            validation_only=True,
        )

        self.assertEqual(report, {"skipped": True, "reason": "validation_only"})

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
        # 新方案仅对 lunge 与 squat 施加局部间隔，不再混合普通和跳跃深蹲。
        class_names = ["lunge", "squat", "tuck_jump"]
        # 当前样本真实类别为 squat，即索引 1。
        labels = torch.tensor([1])
        # 错误排序让易混类别 lunge 的 logit 高于真实 squat。
        wrong_order = torch.tensor([[2.0, 0.0, 0.0]])
        # 正确排序让真实 squat 的 logit 高于 lunge。
        correct_order = torch.tensor([[0.0, 2.0, 0.0]])

        # 计算违反局部间隔时的损失。
        wrong_loss = te.hard_pair_margin_loss(wrong_order, labels, class_names)
        # 计算满足局部间隔时的损失。
        correct_loss = te.hard_pair_margin_loss(correct_order, labels, class_names)

        # 错误排序必须受到更大惩罚，证明定向约束已生效。
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
                model, metadata = te.train_model(
                    train_x,
                    train_y,
                    train_file_ids,
                    val_x,
                    val_y,
                    class_names=class_names,
                    device=torch.device("cpu"),
                    progress_label="window=test",
                    ema_decay=0.9,
                    label_smoothing=0.05,
                )
        finally:
            te.MAX_EPOCHS = old_max_epochs

        log = output.getvalue()
        self.assertIn("epoch=001", log)
        self.assertIn("epoch=002", log)
        self.assertIn("ce=", log)
        self.assertIn("supcon=", log)
        self.assertIn("margin=", log)
        self.assertIn("ema=0.900", log)
        self.assertIn("smooth=0.050", log)
        self.assertIn("val_weak_f1=", log)
        self.assertIn("val_worst_f1=", log)
        self.assertIn("val_weak_recall=", log)
        self.assertIn("val_min_recall=", log)
        # 每个 epoch 必须显示最弱类别名称和数值，便于可见训练窗口即时诊断。
        self.assertIn("weakest_class=", log)
        # 每个 epoch 必须按固定顺序显示全部逐类召回，而不只给宏平均指标。
        self.assertIn("class_recalls={jumping_squat:", log)
        self.assertIsInstance(model, te.BPNet)
        self.assertEqual(metadata["ema_decay"], 0.9)
        self.assertEqual(metadata["label_smoothing"], 0.05)

    def test_exported_header_contains_297_feature_pipeline_and_activity_thresholds(self):
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
        # 生成头文件必须声明清洗后第一自相关峰加入后的 297 维，确保 C/Python 数组边界一致。
        self.assertIn("#define FEATURE_DIM 297", header)
        self.assertIn("REST_MOTION_THRESHOLD", header)
        self.assertIn("ACTIVE_POINT_THRESHOLD", header)
        self.assertIn("append_phase_features", header)
        self.assertIn("append_temporal_features", header)
        self.assertIn("append_normalized_phase_features", header)
        self.assertIn("append_impact_distribution_features", header)
        self.assertIn("spectral_entropy", header)
        self.assertIn("gravity_norm", header)
        self.assertIn("wrist_reversal_rate_hz", header)
        self.assertIn("wrist_acf_second_first_ratio", header)
        self.assertIn("wrist_acf_first_peak", header)
        # C 端必须在特征提取前执行与 Python 同式的单轴毛刺修复。
        self.assertIn("preprocess_imu_window", header)
        # 陀螺门槛单位为 deg/s，固定为 300。
        self.assertIn("PREPROCESS_GYRO_SPIKE_THRESHOLD_DPS", header)
        # 加速度门槛单位为 g，固定为 1.5。
        self.assertIn("PREPROCESS_ACC_SPIKE_THRESHOLD_G", header)
        # 生成头文件必须包含 15 窗口因果 logit 环形缓冲和重置接口。
        self.assertIn("TEMPORAL_LOGIT_HISTORY 15", header)
        self.assertIn("BpTemporalSmoother", header)
        self.assertIn("bp_temporal_smoother_reset", header)
        self.assertIn("bp_temporal_smoother_update", header)
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

    def test_full_report_keeps_external_holdout_separate_from_export_gate(self):
        feature_names = te.build_feature_names()
        class_names = ["a", "jumping_squat"]
        model = te.BPNet(len(feature_names), len(class_names), dropout=0.0)
        result = {
            "window_seconds": 2.5,
            "window_len": 62,
            "step_len": 12,
            "rest_threshold": 0.08,
            "active_point_threshold": 0.02,
            "model": model,
            "specialist_model": None,
            "specialist_class_names": [],
            "mean": np.zeros(len(feature_names), dtype=np.float32),
            "std": np.ones(len(feature_names), dtype=np.float32),
            "val_acc": 1.0,
            "val_f1": 1.0,
            "test_acc": 1.0,
            "test_f1": 1.0,
            "test_min_recall": 1.0,
            "test_class_recalls": {"a": 1.0, "jumping_squat": 1.0},
            "y_test": np.array([0, 1], dtype=np.int64),
            "test_pred": np.array([0, 1], dtype=np.int64),
            "external_holdout": {
                "skipped": False,
                "label": "jumping_squat",
                "file_count": 1,
                "sample_count": 10,
                "recall": 0.8,
                "files": ["scy3.txt"],
            },
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            reached = te.save_outputs(
                result,
                [result],
                class_names,
                feature_names,
                root / "outputs",
                export_when_below_target=False,
                repository_header_path=root / "esp32/include/esp32_bp_model.h",
            )
            report = json.loads(
                (root / "outputs/training_report.json").read_text(encoding="utf-8")
            )

        self.assertTrue(reached)
        self.assertEqual(report["external_holdout"]["recall"], 0.8)

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

    def test_deployment_gate_allows_85_percent_for_declared_weak_classes(self):
        class_names = ["jumping_lunge", "class_a"]
        y_true = np.repeat(np.arange(2, dtype=np.int64), 20)
        y_pred = y_true.copy()
        y_pred[[0, 1, 2]] = 1
        y_pred[[20, 21]] = 0

        reached, recalls = te.deployment_gate_status(
            {"y_test": y_true, "test_pred": y_pred}, class_names
        )

        self.assertTrue(reached)
        np.testing.assert_allclose(recalls, [0.85, 0.90])

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

    def test_load_primary_artifacts_restores_multi_branch_m0(self):
        # 使用当前 297 项合同构造已训练六分支 M0 参数。
        feature_names = te.build_feature_names()
        # 三类足以验证分类头维度和参数键恢复。
        model = te.MultiBranchBPNet(len(feature_names), 3, dropout=0.0)
        # 临时目录模拟 Round29 验证候选工件。
        with tempfile.TemporaryDirectory() as temp_dir:
            # artifact_dir 同时保存模型和标准化配置。
            artifact_dir = Path(temp_dir)
            # 只保存 state_dict，格式与 validation-only 输出一致。
            torch.save(model.state_dict(), artifact_dir / "best_model.pt")
            # 均值、标准差和窗口长度用于加载时合同校验。
            np.savez(
                artifact_dir / "scaler_and_config.npz",
                mean=np.zeros(len(feature_names), dtype=np.float32),
                std=np.ones(len(feature_names), dtype=np.float32),
                window_len=np.asarray([62]),
            )
            # 显式声明 multi_branch=True，禁止误建为平铺 BP。
            restored, _, _ = te.load_primary_artifacts(
                artifact_dir,
                input_dim=len(feature_names),
                class_count=3,
                expected_window_len=62,
                device=torch.device("cpu"),
                multi_branch=True,
            )
        # 恢复结果必须保持六分支 M0 类型。
        self.assertIsInstance(restored, te.MultiBranchBPNet)

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
        # 三类专家必须显式获得清洗后晋级的第一自相关峰。
        self.assertIn("wrist_acf_first_peak", selected)
        # 原有主轴换向率保留，用于区分深蹲、跳蹲和收腹跳的手腕往返节律。
        self.assertIn("wrist_reversal_rate_hz", selected)
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
