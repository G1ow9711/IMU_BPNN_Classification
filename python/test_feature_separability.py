"""验证候选特征公式、物理单位、数值边界和训练/验证稳定性规则。

合成窗口形状统一为 ``[时间点数,6]``，通道顺序 ``gx、gy、gz、ax、ay、az``；
角速度单位 ``deg/s``，加速度单位 ``g``，采样率固定 25 Hz。测试只验证可解释数学
合同，不用同一批窗口同时证明训练与验证效果，也不把候选特征视为生产特征。
"""

import unittest

import numpy as np

from python import analyze_feature_separability as analysis


class FeatureSeparabilityTests(unittest.TestCase):
    """覆盖事件、方向、周期、Fisher 分数和跨切分效应的关键教学样例。"""

    def test_event_aligned_features_detect_flight_and_flight_rotation(self) -> None:
        """腾空点数与角速度积分应按 25 Hz 换算成秒和度。"""
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
        """协方差特征值比应区分单一方向与水平圆周运动。"""
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
        """粗事件提取应保持十二项顺序，并对正常周期输出有限值。"""
        # 构造 62 点六轴窗口；gx 从 0 到 100 deg/s，az 围绕 1g 周期变化。
        window = np.zeros((62, 6), dtype=np.float32)
        window[:, 0] = np.linspace(0.0, 100.0, 62)
        window[:, 5] = 1.0 + 0.5 * np.sin(np.linspace(0.0, 4.0 * np.pi, 62))

        # 提取十二项位置、腾空、差分、相关和能量候选。
        features = analysis.candidate_event_features(window)

        # 只锁定向量长度、名称合同和有限性，避免把合成波形过拟合为生产阈值。
        self.assertEqual(len(features), len(analysis.CANDIDATE_EVENT_FEATURE_NAMES))
        self.assertEqual(len(features), 12)
        self.assertTrue(np.isfinite(features).all())

    def test_candidate_cycle_features_capture_dominant_frequency(self) -> None:
        """单边功率谱主峰应能区分 1 Hz 与 2 Hz 垂直周期。"""
        # sample_rate 与训练和 ESP32 固件一致，单位为 Hz。
        sample_rate = 25.0
        # 四秒时间轴提供 0.25 Hz 频率分辨率，足以区分目标主频。
        time = np.arange(100, dtype=np.float32) / sample_rate
        # 两个 [100,6] 窗口只改变 az，其他通道保持零。
        one_hz = np.zeros((100, 6), dtype=np.float32)
        two_hz = np.zeros((100, 6), dtype=np.float32)
        # 在 1g 静态重力上叠加 1 Hz 和 2 Hz 正弦运动。
        one_hz[:, 5] = 1.0 + np.sin(2.0 * np.pi * time)
        two_hz[:, 5] = 1.0 + np.sin(4.0 * np.pi * time)

        # 分别提取周期候选，并读取垂直加速度主频字段。
        first = analysis.candidate_cycle_features(one_hz)
        second = analysis.candidate_cycle_features(two_hz)
        index = analysis.CANDIDATE_CYCLE_FEATURE_NAMES.index(
            "acc_vertical_spectral_dominant_hz"
        )

        # FFT 栅格允许半个频点误差；同时锁定输出长度和数值有限性。
        self.assertEqual(len(first), len(analysis.CANDIDATE_CYCLE_FEATURE_NAMES))
        self.assertTrue(np.isfinite(first).all())
        self.assertAlmostEqual(float(first[index]), 1.0, delta=0.26)
        self.assertAlmostEqual(float(second[index]), 2.0, delta=0.26)

    def test_fisher_scores_rank_separated_feature_first(self) -> None:
        """类间均值分离的特征应高于只在类内变化的特征。"""
        # 第一列的三个类别中心依次约为 -1.9、0.1、2.1；第二列各类分布相同。
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
        # 每类两个样本，标签形状为 [6]。
        labels = np.asarray([0, 0, 1, 1, 2, 2], dtype=np.int64)

        # Fisher 分数计算 S_B/S_W，不涉及模型拟合。
        scores = analysis.fisher_scores(features, labels)

        # 分离列应排名更高，且退化列不能产生 NaN 或 Inf。
        self.assertGreater(scores[0], scores[1])
        self.assertTrue(np.isfinite(scores).all())

    def test_stable_pair_effect_requires_matching_direction(self) -> None:
        """训练与验证 Cohen's d 方向相反时，稳定效应必须归零。"""
        # 训练目标组均值高于另一组，单特征矩阵形状均为 [2,1]。
        train_target = np.asarray([[3.0], [4.0]], dtype=np.float32)
        train_other = np.asarray([[0.0], [1.0]], dtype=np.float32)
        # 第一组验证数据保持同方向；交换参数后制造验证方向反转。
        val_target = np.asarray([[2.5], [3.5]], dtype=np.float32)
        val_other = np.asarray([[0.2], [1.2]], dtype=np.float32)

        # matching 返回稳定效应、训练 d 和验证 d；reversed_effect 应被方向门归零。
        matching = analysis.stable_pair_effect(
            train_target, train_other, val_target, val_other
        )
        reversed_effect = analysis.stable_pair_effect(
            train_target, train_other, val_other, val_target
        )

        # 只验证稳定规则和带符号效应，不把效应大小绑定到某个动作类别。
        self.assertGreater(matching[0], 0.0)
        self.assertEqual(reversed_effect[0], 0.0)
        self.assertGreater(matching[1][0], 0.0)
        self.assertGreater(matching[2][0], 0.0)


if __name__ == "__main__":
    unittest.main()
