# unittest 提供固定评估器的独立回归测试框架。
import unittest

# NumPy 构造按文件分段的 logits、标签和预期数组。
import numpy as np

# 被测模块包含动作段累计和固定类别指标函数。
from python import evaluate_fixed_ensemble as evaluator


class FixedEnsembleEvaluationTests(unittest.TestCase):
    """验证最终评估入口的因果边界和指标类别顺序。"""

    def test_cumulative_predictions_reset_at_each_file(self):
        # logits 形状为 [4 窗口,2 类]，前两窗属于文件 0，后两窗属于文件 1。
        logits = np.asarray(
            [
                [8.0, 0.0],
                [4.0, 0.0],
                [0.0, 3.0],
                [0.0, 5.0],
            ],
            dtype=np.float32,
        )
        # file_ids 明确第三个窗口进入新活动段，累计状态必须重置。
        file_ids = np.asarray([0, 0, 1, 1], dtype=np.int64)

        # 逐文件按时间顺序累计，不读取未来窗口。
        predictions = evaluator.cumulative_bout_predictions(logits, file_ids)

        # 新文件首窗口立即识别为第一类，证明文件 0 的第零类证据没有泄漏。
        np.testing.assert_array_equal(predictions, [0, 0, 1, 1])

    def test_cumulative_predictions_reject_shape_mismatch(self):
        # logits 提供两个窗口，但 file_ids 只提供一个边界标签。
        logits = np.zeros((2, 3), dtype=np.float32)
        # 非法 file_ids 形状不能确定第二窗口属于哪个动作段。
        file_ids = np.zeros(1, dtype=np.int64)

        # 评估器必须在状态更新前拒绝形状合同错误。
        with self.assertRaises(ValueError):
            evaluator.cumulative_bout_predictions(logits, file_ids)

    def test_classification_metrics_keep_fixed_class_order(self):
        # 三个真实窗口覆盖类别 0、1、2，其中类别 1 被误判为类别 0。
        labels = np.asarray([0, 1, 2], dtype=np.int64)
        # 预测结果使类别 0/1/2 召回分别为 1、0、1。
        predictions = np.asarray([0, 0, 2], dtype=np.int64)
        # 类别名顺序模拟模型导出的固定 CLASS_NAMES 合同。
        class_names = ["class_a", "class_b", "class_c"]

        # 计算总体和逐类指标。
        result = evaluator.classification_metrics(labels, predictions, class_names)

        # 总准确率为两个正确窗口除以三个窗口。
        self.assertAlmostEqual(result["accuracy"], 2.0 / 3.0)
        # 中间类别完全漏检，召回必须为零而不能被忽略。
        self.assertEqual(result["class_recalls"]["class_b"], 0.0)
        # 混淆矩阵行列顺序必须与 class_names 一致。
        self.assertEqual(result["confusion_matrix"], [[1, 0, 0], [1, 0, 0], [0, 0, 1]])


# 直接执行本文件时运行全部固定评估器测试。
if __name__ == "__main__":
    # unittest.main 根据 TestCase 自动发现三个测试方法。
    unittest.main()
