import unittest

import numpy as np

from python import analyze_feature_separability as analysis


class FeatureSeparabilityTests(unittest.TestCase):
    def test_event_aligned_features_detect_flight_and_flight_rotation(self) -> None:
        # 构造 62 点腰部 IMU 窗口；通道顺序为 gx、gy、gz、ax、ay、az。
        window = np.zeros((62, 6), dtype=np.float32)
        # 静止阶段加速度为 1g，重力方向沿传感器 z 轴。
        window[:, 5] = 1.0
        # 第 20 到 27 点模拟连续 8 点腾空，模长 0.2g，持续 0.32 秒。
        window[20:28, 5] = 0.2
        # 腾空阶段绕水平 x 轴以 100 deg/s 转动，用于验证腾空姿态特征。
        window[20:28, 0] = 100.0
        # 第 28 点模拟落地垂直冲击，峰值 2.5g。
        window[28, 5] = 2.5

        # 提取只用于无训练筛选的事件对齐候选值。
        features = analysis.candidate_event_aligned_features(window)
        # 名称数量与数值数量必须一致，保证后续报告索引稳定。
        self.assertEqual(len(features), len(analysis.EVENT_ALIGNED_FEATURE_NAMES))
        # 所有边界输入必须得到有限值，禁止 NaN 或 Inf 污染 Fisher/Cohen's d。
        self.assertTrue(np.isfinite(features).all())
        # 最长腾空持续时间必须等于 8/25 秒，误差只允许 float32 舍入。
        flight_index = analysis.EVENT_ALIGNED_FEATURE_NAMES.index(
            "aligned_longest_flight_seconds"
        )
        self.assertAlmostEqual(float(features[flight_index]), 8.0 / 25.0, places=5)
        # 腾空水平角速度积分应为 100*8/25=32 度。
        rotation_index = analysis.EVENT_ALIGNED_FEATURE_NAMES.index(
            "aligned_flight_horizontal_gyro_integral_deg"
        )
        self.assertAlmostEqual(float(features[rotation_index]), 32.0, places=4)

    def test_horizontal_anisotropy_separates_linear_and_isotropic_motion(self) -> None:
        # 构造相同重力和长度的两个窗口，避免静态姿态差异影响结果。
        linear = np.zeros((62, 6), dtype=np.float32)
        isotropic = np.zeros((62, 6), dtype=np.float32)
        linear[:, 5] = 1.0
        isotropic[:, 5] = 1.0
        # 线性窗口只沿 x 方向往复，模拟弓步更强的主方向运动。
        phase = np.linspace(0.0, 4.0 * np.pi, 62, dtype=np.float32)
        linear[:, 3] = 0.4 * np.sin(phase)
        # 各向同性窗口在水平面画圆，使两个协方差特征值接近。
        isotropic[:, 3] = 0.4 * np.sin(phase)
        isotropic[:, 4] = 0.4 * np.cos(phase)

        # 分别提取事件/方向候选，并读取水平加速度各向异性索引。
        linear_features = analysis.candidate_event_aligned_features(linear)
        isotropic_features = analysis.candidate_event_aligned_features(isotropic)
        index = analysis.EVENT_ALIGNED_FEATURE_NAMES.index(
            "aligned_horizontal_acc_anisotropy"
        )
        # 单一主方向的各向异性应明显高于水平圆周运动。
        self.assertGreater(float(linear_features[index]), 0.90)
        self.assertLess(float(isotropic_features[index]), 0.10)

    def test_candidate_event_features_are_finite_and_complete(self) -> None:
        window = np.zeros((62, 6), dtype=np.float32)
        window[:, 0] = np.linspace(0.0, 100.0, 62)
        window[:, 5] = 1.0 + 0.5 * np.sin(np.linspace(0.0, 4.0 * np.pi, 62))

        features = analysis.candidate_event_features(window)

        self.assertEqual(len(features), len(analysis.CANDIDATE_EVENT_FEATURE_NAMES))
        self.assertEqual(len(features), 12)
        self.assertTrue(np.isfinite(features).all())

    def test_candidate_cycle_features_capture_dominant_frequency(self) -> None:
        sample_rate = 25.0
        time = np.arange(100, dtype=np.float32) / sample_rate
        one_hz = np.zeros((100, 6), dtype=np.float32)
        two_hz = np.zeros((100, 6), dtype=np.float32)
        one_hz[:, 5] = 1.0 + np.sin(2.0 * np.pi * time)
        two_hz[:, 5] = 1.0 + np.sin(4.0 * np.pi * time)

        first = analysis.candidate_cycle_features(one_hz)
        second = analysis.candidate_cycle_features(two_hz)
        index = analysis.CANDIDATE_CYCLE_FEATURE_NAMES.index(
            "acc_vertical_spectral_dominant_hz"
        )

        self.assertEqual(len(first), len(analysis.CANDIDATE_CYCLE_FEATURE_NAMES))
        self.assertTrue(np.isfinite(first).all())
        self.assertAlmostEqual(float(first[index]), 1.0, delta=0.26)
        self.assertAlmostEqual(float(second[index]), 2.0, delta=0.26)

    def test_fisher_scores_rank_separated_feature_first(self) -> None:
        features = np.asarray(
            [
                [-2.0, 0.0],
                [-1.8, 1.0],
                [0.0, 0.0],
                [0.2, 1.0],
                [2.0, 0.0],
                [2.2, 1.0],
            ],
            dtype=np.float32,
        )
        labels = np.asarray([0, 0, 1, 1, 2, 2], dtype=np.int64)

        scores = analysis.fisher_scores(features, labels)

        self.assertGreater(scores[0], scores[1])
        self.assertTrue(np.isfinite(scores).all())

    def test_stable_pair_effect_requires_matching_direction(self) -> None:
        train_target = np.asarray([[3.0], [4.0]], dtype=np.float32)
        train_other = np.asarray([[0.0], [1.0]], dtype=np.float32)
        val_target = np.asarray([[2.5], [3.5]], dtype=np.float32)
        val_other = np.asarray([[0.2], [1.2]], dtype=np.float32)

        matching = analysis.stable_pair_effect(
            train_target, train_other, val_target, val_other
        )
        reversed_effect = analysis.stable_pair_effect(
            train_target, train_other, val_other, val_target
        )

        self.assertGreater(matching[0], 0.0)
        self.assertEqual(reversed_effect[0], 0.0)
        self.assertGreater(matching[1][0], 0.0)
        self.assertGreater(matching[2][0], 0.0)


if __name__ == "__main__":
    unittest.main()
