import unittest
from pathlib import Path

import numpy as np

from python import analyze_wrist_candidates as analysis
from python import train_export as training


class WristCandidateAnalysisTests(unittest.TestCase):
    def test_scalar_features_are_finite_and_keep_declared_order(self) -> None:
        # 构造 2.5 秒、25 Hz 的手腕六轴窗口；列顺序固定为 gx、gy、gz、ax、ay、az。
        window = np.zeros((62, 6), dtype=np.float32)
        # 静止重力设为传感器 z 轴 1g，验证静止边界不会产生 NaN 或除零。
        window[:, 5] = 1.0
        # 提取 16 项不依赖类别模板的手腕候选值。
        features = analysis.wrist_scalar_features(window)
        # 输出维度必须与固定名称完全一致，后续 JSON 和 C 顺序才能稳定。
        self.assertEqual(features.shape, (len(analysis.WRIST_SCALAR_FEATURE_NAMES),))
        # 当前审批方案明确直接候选为 16 项。
        self.assertEqual(len(features), 16)
        # 静止和低方差输入必须全部返回有限数值。
        self.assertTrue(np.isfinite(features).all())

    def test_gyro_path_and_reversal_respond_to_wrist_swing(self) -> None:
        # 构造静止手腕和周期摆动手腕两个相同长度窗口。
        stationary = np.zeros((75, 6), dtype=np.float32)
        swinging = np.zeros((75, 6), dtype=np.float32)
        # 两个窗口使用同一 1g 静态重力，排除姿态基线差异。
        stationary[:, 5] = 1.0
        swinging[:, 5] = 1.0
        # x 轴角速度设置为 1 Hz 正弦摆动，单位为 deg/s。
        time_s = np.arange(75, dtype=np.float32) / float(training.SAMPLE_RATE)
        swinging[:, 0] = 180.0 * np.sin(2.0 * np.pi * time_s)
        # 分别提取静止与摆动候选值。
        stationary_features = analysis.wrist_scalar_features(stationary)
        swinging_features = analysis.wrist_scalar_features(swinging)
        # 查找总角速度路程和有效换向率的固定索引。
        path_index = analysis.WRIST_SCALAR_FEATURE_NAMES.index("wrist_gyro_path_deg")
        reversal_index = analysis.WRIST_SCALAR_FEATURE_NAMES.index("wrist_reversal_rate_hz")
        # 周期摆动必须产生显著更大的手腕角速度积分。
        self.assertGreater(float(swinging_features[path_index]), float(stationary_features[path_index]) + 100.0)
        # 周期摆动必须检测到有效换向，而静止窗口应保持零换向。
        self.assertGreater(float(swinging_features[reversal_index]), 0.5)
        self.assertEqual(float(stationary_features[reversal_index]), 0.0)

    def test_repetition_sequences_have_fixed_shape_and_detect_periodic_motion(self) -> None:
        # 构造 4 秒周期手腕运动，保证窗口中存在多个完整重复。
        time_s = np.arange(100, dtype=np.float32) / float(training.SAMPLE_RATE)
        window = np.zeros((100, 6), dtype=np.float32)
        # 角速度和动态加速度均采用 1 Hz 周期，模拟重复摆臂。
        window[:, 0] = 160.0 * np.sin(2.0 * np.pi * time_s)
        window[:, 5] = 1.0 + 0.8 * np.sin(2.0 * np.pi * time_s + 0.4)
        # 从窗口活动峰之间提取归一化单次重复序列。
        repetitions = analysis.extract_repetition_sequences(window)
        # 4 秒的 1 Hz 动作应至少形成两个完整峰间重复。
        self.assertGreaterEqual(len(repetitions), 2)
        # 每个重复固定为 32 个时间点和 4 个手腕通道描述量。
        self.assertTrue(all(sequence.shape == (32, 4) for sequence in repetitions))
        # 所有归一化模板值必须有限，禁止模板距离传播 NaN。
        self.assertTrue(all(np.isfinite(sequence).all() for sequence in repetitions))

    def test_dtw_distance_is_zero_for_identical_sequence(self) -> None:
        # 构造一条 32×4 的确定性模板序列。
        base = np.linspace(-1.0, 1.0, 32, dtype=np.float32)
        sequence = np.column_stack([base, base**2, np.sin(base), np.cos(base)]).astype(np.float32)
        # 相同序列的归一化 DTW 距离必须为零。
        distance = analysis.dtw_distance(sequence, sequence, band=4)
        # 仅允许浮点舍入误差。
        self.assertAlmostEqual(distance, 0.0, places=7)

    def test_grouped_folds_keep_files_disjoint_and_preserve_classes(self) -> None:
        # 构造两个类别、每类六个不同路径的虚拟采集文件。
        records = []
        # 每类文件数大于折数，保证每折均有该类验证文件。
        for label_idx, label in enumerate(["jumping_squat", "tuck_jump"]):
            # 每类生成六个稳定文件名，便于检查互斥性。
            for file_idx in range(6):
                # ImuRecord 只保存路径和标签；本测试不读取磁盘内容。
                records.append(training.ImuRecord(Path(f"{label}_{file_idx}.txt"), label, label_idx))
        # 按类别轮转构造三折文件级划分。
        folds = analysis.group_records_into_folds(records, fold_count=3)
        # 必须生成审批方案规定的三折。
        self.assertEqual(len(folds), 3)
        # 逐折验证训练/验证文件互斥且验证集合含两个类别。
        for train_records, validation_records in folds:
            # 路径集合用于确认同一采集文件绝不跨越折内边界。
            train_paths = {record.path for record in train_records}
            validation_paths = {record.path for record in validation_records}
            # 任一折的训练和验证路径交集必须为空。
            self.assertFalse(train_paths & validation_paths)
            # 每折验证集合必须同时包含两个类别。
            self.assertEqual({record.label for record in validation_records}, {"jumping_squat", "tuck_jump"})

    def test_development_record_selection_excludes_fixed_test_role(self) -> None:
        # 构造训练、验证和固定测试三个文件元数据；本测试不读取文件内容。
        train_record = training.ImuRecord(Path("roles/train.txt"), "squat", 0)
        # 验证文件允许进入无训练候选审计。
        validation_record = training.ImuRecord(Path("roles/validation.txt"), "squat", 0)
        # 测试文件位于同一数据目录，但不能因目录扫描自动进入候选分析。
        test_record = training.ImuRecord(Path("roles/test.txt"), "squat", 0)
        # 报告只把前两个路径声明为开发角色，故意不列出固定测试路径。
        report = {
            "all_experiments": [
                {
                    "train_files": [str(train_record.path.resolve())],
                    "val_files": [str(validation_record.path.resolve())],
                }
            ]
        }
        # 使用生产候选分析器的角色选择函数过滤完整目录扫描结果。
        selected = analysis.select_reported_development_records(
            [train_record, validation_record, test_record],
            report,
        )
        # 输出必须只含训练和验证角色，测试路径不得出现。
        self.assertEqual(
            {record.path for record in selected},
            {train_record.path, validation_record.path},
        )

    def test_promotion_requires_direction_effect_novelty_and_coverage(self) -> None:
        # 构造一个满足两折同向、效应量、AUC、相关性和覆盖率门槛的候选记录。
        passing = {
            "name": "wrist_gyro_path_deg",
            "same_direction_fold_count": 2,
            "robust_abs_file_d": 0.72,
            "robust_file_auc": 0.78,
            "max_abs_correlation_with_production": 0.63,
            "coverage": 1.0,
            "supported_file_count": 5,
        }
        # 复制记录并只破坏新颖性门槛，验证不会误晋级高度冗余特征。
        failing = dict(passing)
        # 使用不同名称区分两个候选输出。
        failing["name"] = "redundant_feature"
        # 0.91 超过审批方案允许的 0.85 最大相关性。
        failing["max_abs_correlation_with_production"] = 0.91
        # 执行纯规则晋级，避免训练参与候选选择。
        promoted = analysis.promote_candidates([passing, failing], maximum_count=12)
        # 仅满足全部门槛的手腕特征可以晋级。
        self.assertEqual([record["name"] for record in promoted], ["wrist_gyro_path_deg"])

    def test_complementary_selection_counts_all_qualified_pairs_and_distinct_groups(self) -> None:
        # 构造三个均通过单特征硬门槛、但覆盖类别和物理组不同的候选。
        records = [
            {
                "name": "amplitude_feature",
                "feature_group": "amplitude_reversal",
                "covered_weak_classes": ["jumping_lunge", "jumping_squat"],
                "same_direction_fold_count": 3,
                "robust_abs_file_d": 3.0,
                "robust_file_auc": 0.99,
                "max_abs_correlation_with_production": 0.60,
                "coverage": 1.0,
                "supported_file_count": 20,
            },
            {
                "name": "periodicity_feature",
                "feature_group": "periodicity",
                "covered_weak_classes": ["jumping_lunge"],
                "same_direction_fold_count": 3,
                "robust_abs_file_d": 1.4,
                "robust_file_auc": 0.85,
                "max_abs_correlation_with_production": 0.55,
                "coverage": 1.0,
                "supported_file_count": 20,
            },
            {
                "name": "template_feature",
                "feature_group": "template",
                "covered_weak_classes": ["jumping_jack", "jumping_lunge", "jumping_squat"],
                "same_direction_fold_count": 3,
                "robust_abs_file_d": 1.0,
                "robust_file_auc": 0.80,
                "max_abs_correlation_with_production": 0.40,
                "coverage": 0.90,
                "supported_file_count": 18,
            },
        ]
        # 要求每个目标类由两个不同物理组支持，避免两个高度同质值伪装成互补证据。
        selected, coverage = analysis.select_complementary_candidates(
            records,
            weak_classes=["jumping_lunge", "jumping_squat"],
            maximum_count=3,
            minimum_group_count=2,
        )
        # 振幅组和模板组已经能为两个目标类提供两种物理证据，无需强制填满第三项。
        self.assertEqual([record["name"] for record in selected], ["amplitude_feature", "template_feature"])
        # 两个目标类均应记录两个不同物理组。
        self.assertEqual(coverage["jumping_lunge"], ["amplitude_reversal", "template"])
        self.assertEqual(coverage["jumping_squat"], ["amplitude_reversal", "template"])


if __name__ == "__main__":
    # 允许通过 python -m unittest 直接运行本文件。
    unittest.main()
