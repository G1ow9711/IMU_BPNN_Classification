"""验证个体差异压力变换的确定性、物理单位和错误边界。"""

# unittest 提供标准库断言和测试入口。
import unittest

# NumPy 构造固定 62 点六轴窗口并比较变换结果。
import numpy as np

# 导入待测验证专用评估器；导入不得读取数据或模型文件。
from python import evaluate_validation_generalization as audit
# 导入四组合冻结选择器；导入不得扫描数据或加载模型工件。
from python import evaluate_validation_candidate_grid as candidate_grid
# 导入固定三 M0 互补性评估器；导入不得访问数据集或外部 CSV。
from python import evaluate_validation_tri_m0 as tri_m0


class ValidationGeneralizationTransformTests(unittest.TestCase):
    """覆盖六种固定验证变换的形状、确定性和边界。"""

    def test_all_transforms_preserve_window_shape_and_finite_values(self):
        # rng 使用固定种子生成 62 点六轴输入，通道顺序 gx、gy、gz、ax、ay、az。
        rng = np.random.default_rng(20260722)
        # window 形状 `[62,6]`，数值单位由测试范围近似模拟。
        window = rng.normal(0.0, 1.0, size=(62, 6)).astype(np.float32)
        # az 加 1 g，形成合理平均重力。
        window[:, 5] += 1.0
        # 遍历公开固定变换集合。
        for transform_name in audit.TRANSFORM_NAMES:
            # 子测试失败时报告具体变换名。
            with self.subTest(transform_name=transform_name):
                # transformed 执行一次确定性变换。
                transformed = audit.transform_window(window, transform_name)
                # 点数和六轴宽度必须完全不变。
                self.assertEqual(transformed.shape, window.shape)
                # 输出必须保持 float32，匹配训练与 ESP32 数值合同。
                self.assertEqual(transformed.dtype, np.float32)
                # 所有元素必须有限。
                self.assertTrue(np.all(np.isfinite(transformed)))

    def test_transforms_are_deterministic_and_do_not_modify_input(self):
        # window 使用可辨认线性六轴序列。
        window = np.arange(62 * 6, dtype=np.float32).reshape(62, 6)
        # original 保存调用前副本。
        original = window.copy()
        # 对每种压力变换调用两次。
        for transform_name in audit.TRANSFORM_NAMES:
            # first 是第一次结果。
            first = audit.transform_window(window, transform_name)
            # second 是第二次结果。
            second = audit.transform_window(window, transform_name)
            # 固定验证不允许随机性。
            np.testing.assert_array_equal(first, second)
        # 所有变换都不得原地修改输入。
        np.testing.assert_array_equal(window, original)

    def test_unknown_transform_is_rejected(self):
        # window 是最小合法两点六轴矩阵。
        window = np.zeros((2, 6), dtype=np.float32)
        # 未知名称必须明确失败，不能静默退化为 clean。
        with self.assertRaises(ValueError):
            # 传入不在固定枚举中的名称。
            audit.transform_window(window, "external_csv_tuned")


class ProductionLockMetricTests(unittest.TestCase):
    """覆盖端侧连续两窗、第四窗兜底、短文件和瞬态误窗。"""

    def test_lock_metrics_report_normal_forced_and_unlocked_files(self):
        # logits 依次包含：两窗高置信类0、四窗低置信类1、一窗短文件类2。
        logits = np.asarray(
            [
                # 文件0第一窗高置信类0。
                [3.0, 0.0, 0.0],
                # 文件0第二窗保持类0，正常锁定。
                [3.0, 0.0, 0.0],
                # 文件1四窗仅略偏类1，概率始终低于50%。
                [0.0, 0.1, 0.0],
                [0.0, 0.1, 0.0],
                [0.0, 0.1, 0.0],
                [0.0, 0.1, 0.0],
                # 文件2只有一窗，生产准备态不会提前锁定。
                [0.0, 0.0, 3.0],
            ],
            dtype=np.float32,
        )
        # labels 保存每窗文件真值，形状 `[7]`。
        labels = np.asarray([0, 0, 1, 1, 1, 1, 2], dtype=np.int64)
        # file_ids 把七窗分成三个独立动作文件。
        file_ids = np.asarray([0, 0, 1, 1, 1, 1, 2], dtype=np.int64)
        # 执行生产锁类文件级评估。
        metrics = audit.production_lock_metrics(
            logits,
            labels,
            file_ids,
            ["a", "b", "c"],
        )
        # 三个文件均进入统计。
        self.assertEqual(metrics["file_count"], 3)
        # 前两个文件锁定，短文件未锁定。
        self.assertEqual(metrics["locked_file_count"], 2)
        # 正常和兜底文件均锁对，短文件按错误计入。
        self.assertEqual(metrics["correct_file_count"], 2)
        # 文件级准确率为 2/3。
        self.assertAlmostEqual(metrics["accuracy"], 2.0 / 3.0)
        # 只有低置信类1在第四窗强制锁定。
        self.assertEqual(metrics["forced_lock_count"], 1)
        # 观察窗数分别为 2、4、1。
        self.assertAlmostEqual(metrics["mean_windows_used"], 7.0 / 3.0)
        # 短文件类2召回为零。
        self.assertEqual(metrics["class_recalls"]["c"], 0.0)

    def test_transient_first_window_does_not_lock_wrong_class(self):
        # 第一窗错误偏类0，后两窗强烈支持真实类1。
        logits = np.asarray(
            [
                # 起步瞬态误窗。
                [4.0, 0.0],
                # 第二窗累计 Top-1 转为类1，但只有一窗连续证据。
                [0.0, 8.0],
                # 第三窗保持类1并越过50%概率门。
                [0.0, 8.0],
            ],
            dtype=np.float32,
        )
        # 三窗均属于真实类1。
        labels = np.asarray([1, 1, 1], dtype=np.int64)
        # 三窗属于同一冻结验证文件。
        file_ids = np.asarray([7, 7, 7], dtype=np.int64)
        # 执行生产策略。
        metrics = audit.production_lock_metrics(
            logits,
            labels,
            file_ids,
            ["wrong", "right"],
        )
        # 最终必须锁定真实类。
        self.assertEqual(metrics["records"][0]["locked_class"], "right")
        # 累计类别改变后重新计连续窗，因此第三窗才锁定。
        self.assertEqual(metrics["records"][0]["windows_used"], 3)
        # 该锁定通过正常门，不是第四窗兜底。
        self.assertFalse(metrics["records"][0]["forced"])

    def test_lock_metrics_reject_non_finite_logits(self):
        # NaN 在固件中必须拒绝，验证器不能静默 argmax。
        logits = np.asarray([[np.nan, 0.0]], dtype=np.float32)
        # 标签和文件编号形状合法，错误只来自 logits。
        labels = np.asarray([0], dtype=np.int64)
        # 唯一文件编号为零。
        file_ids = np.asarray([0], dtype=np.int64)
        # 预期明确 ValueError。
        with self.assertRaises(ValueError):
            # 执行异常输入。
            audit.production_lock_metrics(logits, labels, file_ids, ["a", "b"])


class ValidationCandidateGridTests(unittest.TestCase):
    """覆盖 clean 硬门、最弱类排序和完全同分保留历史权重。"""

    def test_clean_regression_is_rejected_before_stress_ranking(self):
        # 历史组合提供两个 clean 硬门和压力排序基线。
        baseline = {
            "clean_accuracy": 0.90,
            "clean_min_class_recall": 0.50,
            "worst_transform_min_class_recall": 0.20,
            "worst_transform_accuracy": 0.60,
            "mean_transform_accuracy": 0.75,
            "forced_lock_total": 2,
            "mean_windows_used": 2.5,
        }
        # 候选压力集更强，但 clean 准确率下降，必须先淘汰。
        regressed = {
            "clean_accuracy": 0.89,
            "clean_min_class_recall": 0.60,
            "worst_transform_min_class_recall": 0.80,
            "worst_transform_accuracy": 0.85,
            "mean_transform_accuracy": 0.88,
            "forced_lock_total": 0,
            "mean_windows_used": 2.0,
        }
        # 执行预声明选择规则。
        result = candidate_grid.select_candidate(
            {
                candidate_grid.BASELINE_CANDIDATE: baseline,
                "regressed": regressed,
            }
        )
        # clean 回退候选不得胜出。
        self.assertEqual(result["winner"], candidate_grid.BASELINE_CANDIDATE)
        # 报告必须说明准确率回退原因。
        self.assertIn("clean_accuracy_regressed", result["rejections"]["regressed"])

    def test_worst_class_recall_ranks_before_average_accuracy(self):
        # 历史组合 clean 指标为 90%/50%，压力最弱类为 20%。
        baseline = {
            "clean_accuracy": 0.90,
            "clean_min_class_recall": 0.50,
            "worst_transform_min_class_recall": 0.20,
            "worst_transform_accuracy": 0.70,
            "mean_transform_accuracy": 0.82,
            "forced_lock_total": 1,
            "mean_windows_used": 2.2,
        }
        # 鲁棒候选 clean 不退化，最弱类更高但平均准确率略低。
        robust = {
            "clean_accuracy": 0.90,
            "clean_min_class_recall": 0.50,
            "worst_transform_min_class_recall": 0.30,
            "worst_transform_accuracy": 0.69,
            "mean_transform_accuracy": 0.81,
            "forced_lock_total": 1,
            "mean_windows_used": 2.2,
        }
        # 运行固定排序。
        result = candidate_grid.select_candidate(
            {
                candidate_grid.BASELINE_CANDIDATE: baseline,
                "robust": robust,
            }
        )
        # 跨人泛化优先保护最弱类别，因此选择 robust。
        self.assertEqual(result["winner"], "robust")

    def test_exact_tie_keeps_historical_weights(self):
        # 完全相同摘要验证无收益时不替换端侧权重。
        summary = {
            "clean_accuracy": 0.90,
            "clean_min_class_recall": 0.50,
            "worst_transform_min_class_recall": 0.20,
            "worst_transform_accuracy": 0.70,
            "mean_transform_accuracy": 0.82,
            "forced_lock_total": 1,
            "mean_windows_used": 2.2,
        }
        # 历史组合按稳定顺序位于首项。
        result = candidate_grid.select_candidate(
            {
                candidate_grid.BASELINE_CANDIDATE: dict(summary),
                "equal_candidate": dict(summary),
            }
        )
        # 完全同分必须保留历史组合。
        self.assertEqual(result["winner"], candidate_grid.BASELINE_CANDIDATE)


class TriM0ValidationTests(unittest.TestCase):
    """覆盖固定三模型权重、有限值边界和 robust 基线保护。"""

    def test_fixed_three_model_weights_form_declared_convex_combination(self):
        # robust 只在类别0输出 2，形状为 `[1,2]`。
        robust = np.asarray([[2.0, 0.0]], dtype=np.float32)
        # invariant 只在类别1输出 4。
        invariant = np.asarray([[0.0, 4.0]], dtype=np.float32)
        # masked 两类均输出 10，验证 15% 公共贡献。
        masked = np.asarray([[10.0, 10.0]], dtype=np.float32)
        # combined 执行固定 42.5%/42.5%/15% 融合。
        combined = tri_m0.combine_three_logits(robust, invariant, masked)
        # expected 手算类别0为 0.425*2+0.15*10，类别1为 0.425*4+0.15*10。
        expected = np.asarray([[2.35, 3.20]], dtype=np.float32)
        # float32 乘加允许一个极小舍入误差。
        np.testing.assert_allclose(combined, expected, atol=1.0e-6, rtol=0.0)
        # 三个公开权重之和必须严格接近一，防止 logits 尺度漂移。
        self.assertAlmostEqual(
            tri_m0.ROBUST_WEIGHT
            + tri_m0.INVARIANT_WEIGHT
            + tri_m0.MASKED_WEIGHT,
            1.0,
            places=12,
        )

    def test_three_model_fusion_rejects_shape_and_finite_value_errors(self):
        # valid 是最小合法 `[1,2]` logits。
        valid = np.zeros((1, 2), dtype=np.float32)
        # wrong_shape 多一个样本，禁止 NumPy 广播或截断。
        wrong_shape = np.zeros((2, 2), dtype=np.float32)
        # 形状不同必须抛出 ValueError。
        with self.assertRaises(ValueError):
            # robust 和 masked 合法，invariant 首维错误。
            tri_m0.combine_three_logits(valid, wrong_shape, valid)
        # non_finite 含 NaN，不能进入生产 softmax。
        non_finite = np.asarray([[np.nan, 0.0]], dtype=np.float32)
        # 非有限输入必须抛出 ValueError。
        with self.assertRaises(ValueError):
            # 只污染 robust 路，验证任一路都会拒绝。
            tri_m0.combine_three_logits(non_finite, valid, valid)

    def test_tri_selection_protects_robust_baseline_and_keeps_exact_tie(self):
        # baseline 是上一轮已冻结 robust 双 M0 摘要。
        baseline = {
            "clean_accuracy": 0.90,
            "clean_min_class_recall": 0.50,
            "worst_transform_min_class_recall": 0.30,
            "worst_transform_accuracy": 0.80,
            "mean_transform_accuracy": 0.86,
            "forced_lock_total": 2,
            "mean_windows_used": 2.2,
        }
        # regressed 虽提高压力均值，但 clean 准确率下降，必须淘汰。
        regressed = {
            **baseline,
            "clean_accuracy": 0.89,
            "mean_transform_accuracy": 0.95,
        }
        # 执行 robust 基线保护选择。
        result = tri_m0.select_tri_candidate(
            {
                tri_m0.ROBUST_DUAL: dict(baseline),
                tri_m0.TRI_M0: dict(baseline),
                "regressed": regressed,
            }
        )
        # 完全同分时保留首项 robust 双 M0，避免无收益增加第三模型。
        self.assertEqual(result["winner"], tri_m0.ROBUST_DUAL)
        # clean 回退原因必须可审计。
        self.assertIn(
            "clean_accuracy_regressed",
            result["rejections"]["regressed"],
        )


# 直接运行本文件时执行全部测试；被 discover 导入时不重复启动。
if __name__ == "__main__":
    # 使用标准 unittest 入口。
    unittest.main()
