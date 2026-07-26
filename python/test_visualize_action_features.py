"""动作特征可视化的确定性纯函数和清单合同测试。"""

# json 用于验证清单序列化后不含本机绝对路径。
import json
# tempfile 为合成数据指纹测试创建自动清理目录。
import tempfile
# unittest 提供无需真实 IMU 数据集的标准测试框架。
import unittest
# Path 管理合成类别目录和 TXT 文件。
from pathlib import Path

# NumPy 生成可解析频率的正弦信号和聚合矩阵。
import numpy as np

# 被测模块包含聚合、频谱、指纹、热力图和清单纯函数。
from python import visualize_action_features as visualizer


class VisualizeActionFeaturesTests(unittest.TestCase):
    """验证图表核心统计不挑样本、不泄露路径并保持确定性。"""

    def test_aggregate_series_uses_all_rows_for_median_and_iqr(self) -> None:
        """逐时刻聚合必须使用全部验证窗而不是手选单条曲线。"""

        # values 形状 [4 窗口,3 时间点]，每列具有可手算分位数。
        values = np.asarray(
            [
                [0.0, 10.0, 20.0],
                [2.0, 12.0, 22.0],
                [4.0, 14.0, 24.0],
                [6.0, 16.0, 26.0],
            ],
            dtype=np.float64,
        )
        # 计算逐列下四分位、中位数和上四分位。
        q25, median, q75 = visualizer.aggregate_series(values)
        # NumPy 默认线性分位下第一列 25% 为 1.5。
        np.testing.assert_allclose(q25, [1.5, 11.5, 21.5])
        # 四行中位数是中间两行均值。
        np.testing.assert_allclose(median, [3.0, 13.0, 23.0])
        # 第一列 75% 为 4.5，其余列相同平移。
        np.testing.assert_allclose(q75, [4.5, 14.5, 24.5])

    def test_file_collapse_prevents_long_file_from_dominating_class_median(
        self,
    ) -> None:
        """一百窗长文件与一窗短文件必须各占一个类别权重。"""

        # long_rows 模拟同一长文件产生一百个高度重叠的零值窗口。
        long_rows = np.zeros((100, 1), dtype=np.float64)
        # short_row 模拟另一短文件仅产生一个值为十的窗口。
        short_row = np.asarray([[10.0]], dtype=np.float64)
        # values 形状 [101 窗口,1 特征]。
        values = np.vstack([long_rows, short_row])
        # source_files 明确前一百行来自 long.txt，最后一行来自 short.txt。
        source_files = ["walk/long.txt"] * 100 + ["walk/short.txt"]
        # 每文件先逐列取窗口中位数。
        file_values, ordered_files = visualizer.collapse_rows_by_file(
            values,
            source_files,
        )
        # 排序后应恰有两个文件代表。
        self.assertEqual(
            ordered_files,
            ["walk/long.txt", "walk/short.txt"],
        )
        # 两个代表值分别为零和十。
        np.testing.assert_array_equal(file_values[:, 0], [0.0, 10.0])
        # 按文件聚合后的类别中位数应为五。
        _, file_median, _ = visualizer.aggregate_series(file_values)
        # 文件等权结果不受长文件一百个窗口支配。
        np.testing.assert_allclose(file_median, [5.0])
        # 直接按窗口聚合会错误得到零，用反例锁住统计合同。
        _, incorrect_window_median, _ = visualizer.aggregate_series(values)
        # 反例证明两种口径确实不同。
        np.testing.assert_allclose(incorrect_window_median, [0.0])

    def test_medoid_and_peak_alignment_are_deterministic(self) -> None:
        """算法 medoid 平局取最早行，峰对齐不循环复制边缘。"""

        # shapes 第一、三行相同并围绕第二行，第一行应在 medoid 平局中胜出。
        shapes = np.asarray(
            [
                [0.0, 0.0],
                [1.0, 1.0],
                [0.0, 0.0],
            ],
            dtype=np.float64,
        )
        # 真实 medoid 平局由 argmin 选择最早索引零。
        self.assertEqual(visualizer.select_medoid_index(shapes), 0)
        # signal 在索引一处有峰，需要移动到索引三。
        signal = np.asarray([0.0, 5.0, 2.0, 1.0, 0.0], dtype=np.float64)
        # 对齐后向右移动两点，左边无真实采样处填 NaN。
        aligned = visualizer.align_series_to_peak(signal, 1, 3)
        # 目标索引三必须等于原峰五。
        self.assertEqual(float(aligned[3]), 5.0)
        # 左侧两个空点必须是 NaN，证明没有循环回绕。
        self.assertTrue(bool(np.all(np.isnan(aligned[:2]))))
        # 其余真实值保持原始顺序。
        np.testing.assert_array_equal(aligned[2:], [0.0, 5.0, 2.0])

    def test_relative_power_spectrum_detects_two_hz_sine(self) -> None:
        """相对功率谱应在合成 2 Hz 周期附近出现主峰。"""

        # sample_rate 与固件固定 25 Hz 合同一致。
        sample_rate = float(visualizer.SAMPLE_RATE)
        # 250 点对应 10 秒，提供 0.1 Hz 频率分辨率。
        time_seconds = np.arange(250, dtype=np.float64) / sample_rate
        # signal 是零偏置 2 Hz 正弦角速度模长替代序列。
        signal = np.sin(2.0 * np.pi * 2.0 * time_seconds)
        # 计算去均值 Hann 窗相对功率谱。
        frequencies, power = visualizer.relative_power_spectrum(
            signal,
            sample_rate=sample_rate,
        )
        # 忽略直流点后寻找最大相对功率频点。
        peak_frequency = float(frequencies[int(np.argmax(power))])
        # 主峰必须在 2 Hz 的一个频点容差内。
        self.assertAlmostEqual(peak_frequency, 2.0, delta=0.11)
        # 归一化动态谱总功率必须为一。
        self.assertAlmostEqual(float(np.sum(power)), 1.0, places=12)

    def test_relative_power_spectrum_constant_signal_is_finite_zero(self) -> None:
        """静止常量序列不能产生 NaN、Inf 或伪周期。"""

        # constant 模拟 62 点完全静止派生信号。
        constant = np.ones(62, dtype=np.float64)
        # 计算相对谱。
        _, power = visualizer.relative_power_spectrum(constant)
        # 全部频点必须有限。
        self.assertTrue(bool(np.all(np.isfinite(power))))
        # 常量去均值后总功率为零。
        np.testing.assert_array_equal(power, np.zeros_like(power))

    def test_robust_heatmap_scores_zero_constant_column(self) -> None:
        """跨类别常量特征列应安全映射为零。"""

        # values 第一列随类别增长，第二列在全部类别中恒为 5。
        values = np.asarray(
            [
                [1.0, 5.0],
                [2.0, 5.0],
                [3.0, 5.0],
                [9.0, 5.0],
            ],
            dtype=np.float64,
        )
        # 计算按列中位数/IQR 的稳健显示分数。
        scores = visualizer.robust_heatmap_scores(values)
        # 输出形状必须保持 [类别数,特征数]。
        self.assertEqual(scores.shape, values.shape)
        # 常量第二列中心化后全为零且没有除零。
        np.testing.assert_array_equal(scores[:, 1], np.zeros(4))
        # 所有显示分数必须有限并落在截断范围内。
        self.assertTrue(bool(np.all(np.isfinite(scores))))
        # 最大绝对值不得超过三。
        self.assertLessEqual(float(np.max(np.abs(scores))), 3.0)

    def test_dataset_fingerprint_ignores_absolute_root_but_tracks_content(self) -> None:
        """数据指纹只能依赖相对结构和内容，不能依赖盘符或临时根。"""

        # TemporaryDirectory 在测试结束后自动删除合成文件。
        with tempfile.TemporaryDirectory() as temporary_directory:
            # root 是当前测试私有目录。
            root = Path(temporary_directory)
            # left 和 right 模拟位于不同绝对根的同构数据集。
            left = root / "left"
            # right 是第二个数据集根。
            right = root / "right"
            # 两个根都创建同名 walk 类别目录。
            (left / "walk").mkdir(parents=True)
            # 第二个根创建相同结构。
            (right / "walk").mkdir(parents=True)
            # content 是最小六列逗号数据文本。
            content = "1,2,3,4,5,6\n7,8,9,10,11,12\n"
            # 两个绝对根写入相同相对路径和内容。
            (left / "walk/sample.txt").write_text(content, encoding="utf-8")
            # 第二份内容完全相同。
            (right / "walk/sample.txt").write_text(content, encoding="utf-8")
            # 数据根说明文件不属于第一层动作目录样本，必须从指纹排除。
            (left / "ReadMe.txt").write_text("说明甲", encoding="utf-8")
            # 第二根写不同说明内容，若错误纳入会导致两套指纹不一致。
            (right / "ReadMe.txt").write_text("说明乙", encoding="utf-8")
            # 计算第一份指纹及统计。
            left_digest, left_counts, left_file_count = (
                visualizer.dataset_fingerprint(left)
            )
            # 计算第二份指纹及统计。
            right_digest, right_counts, right_file_count = (
                visualizer.dataset_fingerprint(right)
            )
            # 绝对根不同但相对结构和内容相同，指纹必须相同。
            self.assertEqual(left_digest, right_digest)
            # 类别文件计数必须为 walk 一件。
            self.assertEqual(left_counts, {"walk": 1})
            # 两份总文件数均为一。
            self.assertEqual((left_file_count, right_file_count), (1, 1))
            # 类别统计也应完全相同。
            self.assertEqual(left_counts, right_counts)
            # 修改第二份文件内容以验证指纹对数据变化敏感。
            (right / "walk/sample.txt").write_text(
                content + "13,14,15,16,17,18\n",
                encoding="utf-8",
            )
            # 重新计算变更后指纹。
            changed_digest, _, _ = visualizer.dataset_fingerprint(right)
            # 内容变化必须改变整套数据指纹。
            self.assertNotEqual(left_digest, changed_digest)

    def test_figure_sha256_requires_complete_lowercase_hex_mapping(self) -> None:
        """四图摘要必须键完整，且每个值为 64 位小写十六进制。"""

        # figure_names 与正式输出的四个 PNG 文件名和顺序完全一致。
        figure_names = [
            visualizer.OUTPUT_FILES["time_domain"],
            visualizer.OUTPUT_FILES["power_spectrum"],
            visualizer.OUTPUT_FILES["feature_distribution"],
            visualizer.OUTPUT_FILES["feature_heatmap"],
        ]
        # valid_mapping 为四图构造不同但合法的 64 位小写十六进制摘要。
        valid_mapping = {
            figure_name: format(index, "x") * 64
            for index, figure_name in enumerate(figure_names, start=1)
        }
        # 合法映射应按固定图名顺序返回等值副本。
        validated = visualizer.validate_figure_sha256(valid_mapping)
        # 返回键顺序与正式图表顺序一致，便于稳定写入 JSON。
        self.assertEqual(list(validated), figure_names)
        # 返回摘要不能改变调用方提供的合法字节指纹。
        self.assertEqual(validated, valid_mapping)
        # missing_mapping 删除热力图摘要，模拟生成器遗漏一张图。
        missing_mapping = dict(valid_mapping)
        # 精确删除最后一张正式图的摘要。
        del missing_mapping[figure_names[-1]]
        # 缺少任意正式图必须拒绝构造清单。
        with self.assertRaises(ValueError):
            # 调用纯校验函数，不需要生成真实 PNG。
            visualizer.validate_figure_sha256(missing_mapping)
        # extra_mapping 在完整四图外混入一个临时 PNG。
        extra_mapping = dict(valid_mapping)
        # 临时图摘要本身合法，但键范围不属于正式资产。
        extra_mapping["temporary.png"] = "e" * 64
        # 多余图名必须拒绝，防止清单范围静默扩张。
        with self.assertRaises(ValueError):
            # 校验键集合必须精确等于四张正式图。
            visualizer.validate_figure_sha256(extra_mapping)
        # uppercase_mapping 把第一张图摘要改为大写十六进制。
        uppercase_mapping = dict(valid_mapping)
        # 大写 A 虽可表示十六进制，但不符合稳定的小写清单合同。
        uppercase_mapping[figure_names[0]] = "A" * 64
        # 大写摘要必须被拒绝。
        with self.assertRaises(ValueError):
            # 校验摘要字符范围。
            visualizer.validate_figure_sha256(uppercase_mapping)
        # short_mapping 把第二张图摘要缩短为 63 位。
        short_mapping = dict(valid_mapping)
        # 63 位文本不可能是完整 SHA-256。
        short_mapping[figure_names[1]] = "b" * 63
        # 长度不足必须被拒绝。
        with self.assertRaises(ValueError):
            # 校验 SHA-256 固定长度。
            visualizer.validate_figure_sha256(short_mapping)

    def test_manifest_contains_only_relative_validation_files(self) -> None:
        """清单序列化后不得包含盘符、反斜杠或数据集绝对路径。"""

        # 创建一个最小验证窗口，数据形状满足 [62,6] 合同。
        validation_window = visualizer.ValidationWindow(
            label="walk",
            label_idx=0,
            source_file="walk/sample.txt",
            start_point=0,
            data=np.zeros((62, 6), dtype=np.float32),
        )
        # corpus 提供最小窗口和统计映射。
        corpus = visualizer.ValidationCorpus(
            windows=(validation_window,),
            skipped={
                "too_short": 0,
                "rest_filtered": 0,
                "kept_windows": 1,
                "files_without_valid_window": 0,
                "motion_edge_trimmed_points": 0,
            },
            class_window_counts={"walk": 1},
            file_window_counts={"walk/sample.txt": 1},
        )
        # record 仅用于长度统计，路径故意包含 Windows 绝对盘符。
        absolute_record = visualizer.ImuRecord(
            Path(r"C:\private\imu_dataset_for_final\walk\sample.txt"),
            "walk",
            0,
        )
        # figure_sha256 为四张正式 PNG 提供可验证的测试摘要。
        figure_sha256 = {
            visualizer.OUTPUT_FILES["time_domain"]: "1" * 64,
            visualizer.OUTPUT_FILES["power_spectrum"]: "2" * 64,
            visualizer.OUTPUT_FILES["feature_distribution"]: "3" * 64,
            visualizer.OUTPUT_FILES["feature_heatmap"]: "4" * 64,
        }
        # 构造清单时显式传入相对 validation_files。
        manifest = visualizer.build_manifest(
            dataset_name="imu_dataset_for_final",
            dataset_sha256="a" * 64,
            dataset_file_count=1,
            class_file_counts={"walk": 1},
            class_names=["walk"],
            train_records=[absolute_record],
            validation_records=[absolute_record],
            test_records=[absolute_record],
            validation_files=["walk/sample.txt"],
            corpus=corpus,
            rest_threshold=0.03,
            active_point_threshold=0.02,
            window_len=62,
            step_len=12,
            figure_sha256=figure_sha256,
        )
        # 与正式写盘相同方式序列化。
        serialized = json.dumps(
            manifest,
            ensure_ascii=False,
            sort_keys=True,
        )
        # 清单不得包含私有绝对目录。
        self.assertNotIn(r"C:\private", serialized)
        # 清单路径统一为 POSIX 斜杠。
        self.assertNotIn("\\", serialized)
        # 相对验证文件必须保留。
        self.assertIn("walk/sample.txt", serialized)
        # 生成器路径必须固定在 python 正式目录。
        self.assertEqual(
            manifest["generator"]["path"],
            "python/visualize_action_features.py",
        )
        # 统计权重单位必须明确为验证文件。
        self.assertEqual(
            manifest["statistics_contract"]["weighting_unit"],
            "validation_file",
        )
        # 四个正式图名必须使用固定中文序号。
        self.assertEqual(
            list(manifest["figures"]),
            [
                "01_六类派生信号曲线.png",
                "02_十一类功率谱对比.png",
                "03_关键特征分布.png",
                "04_特征中位数热力图.png",
            ],
        )
        # 每张图节点必须保存对应 PNG 原始字节的真实摘要字段。
        for figure_name, expected_digest in figure_sha256.items():
            # 清单值必须与调用方计算的摘要逐图一致。
            self.assertEqual(
                manifest["figures"][figure_name]["sha256"],
                expected_digest,
            )


# 直接执行本文件时运行全部测试。
if __name__ == "__main__":
    # unittest.main 自动发现当前 TestCase 的九个测试方法。
    unittest.main()
