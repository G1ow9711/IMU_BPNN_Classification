from __future__ import annotations

"""验证首次主动作锁定策略不会被单个起手过渡窗永久带偏。"""

# unittest 提供标准库红绿测试入口。
import unittest

# NumPy 构造确定性双类 logits。
import numpy as np

# 导入待验证策略实现。
from python import evaluate_lock_policy_grid as policy


class LockPolicyTests(unittest.TestCase):
    """覆盖起手过渡、干净两窗、第四窗兜底和文件级指标。"""

    def test_window_stable_ignores_single_start_transition(self) -> None:
        """独立窗口连续门应把“错误、异类、目标、目标”锁为目标。"""

        # scores 形状 `[4,3]`；首窗强错误、第二窗异类，第三和第四窗稳定为类别 2。
        scores = np.asarray(
            [
                [10.0, 0.0, 0.0],
                [0.0, 8.0, 0.0],
                [0.0, 0.0, 8.0],
                [0.0, 0.0, 9.0],
            ],
            dtype=np.float64,
        )
        # 基线累计 Top-1 会被首窗大值拖住并错误锁为类别 0。
        baseline = policy.lock_one_file(scores, policy.POLICY_CUMULATIVE_STABLE)
        # 候选按独立窗口连续证据在第四窗锁为类别 2。
        candidate = policy.lock_one_file(scores, policy.POLICY_WINDOW_STABLE)
        # 验证基线确实复现故障。
        self.assertEqual(0, baseline["locked_class_index"])
        # 验证候选修复起手污染。
        self.assertEqual(2, candidate["locked_class_index"])
        # 候选使用四个因果窗且不是强制兜底。
        self.assertEqual(4, candidate["windows_used"])
        # 两个目标窗已经满足正常门。
        self.assertFalse(candidate["forced"])

    def test_clean_pair_keeps_two_window_latency(self) -> None:
        """两个干净同类窗口仍应在第二窗确认。"""

        # 两个窗口均高置信指向类别 1。
        scores = np.asarray([[0.0, 7.0], [0.0, 8.0]], dtype=np.float64)
        # 执行候选策略。
        result = policy.lock_one_file(scores, policy.POLICY_WINDOW_STABLE)
        # 锁定类别必须正确。
        self.assertEqual(1, result["locked_class_index"])
        # 干净动作不增加额外窗口。
        self.assertEqual(2, result["windows_used"])
        # 正常两窗锁定不属于兜底。
        self.assertFalse(result["forced"])

    def test_unstable_four_windows_use_cumulative_fallback(self) -> None:
        """四窗始终交替时应有界结束并明确标记 forced。"""

        # 四个窗口在两类间交替，没有连续两窗。
        scores = np.asarray(
            [[6.0, 0.0], [0.0, 5.0], [6.0, 0.0], [0.0, 5.0]],
            dtype=np.float64,
        )
        # 执行候选策略。
        result = policy.lock_one_file(scores, policy.POLICY_WINDOW_STABLE)
        # 类别 0 的累计和更大，因此兜底选 0。
        self.assertEqual(0, result["locked_class_index"])
        # 必须明确是第四窗强制锁。
        self.assertTrue(result["forced"])
        # 观察窗数固定为四。
        self.assertEqual(4, result["windows_used"])

    def test_policy_metrics_counts_unlocked_as_error(self) -> None:
        """文件级指标必须把短文件未锁定计为错误。"""

        # 第一文件两窗正确锁类，第二文件仅一窗无法正常锁定。
        scores = np.asarray(
            [[8.0, 0.0], [9.0, 0.0], [0.0, 8.0]],
            dtype=np.float64,
        )
        # labels 和 file_ids 与三窗口逐项对齐。
        labels = np.asarray([0, 0, 1], dtype=np.int64)
        # 第二文件只有一个窗口。
        file_ids = np.asarray([0, 0, 1], dtype=np.int64)
        # 汇总文件级指标。
        metrics = policy.policy_metrics(
            scores,
            labels,
            file_ids,
            ["zero", "one"],
            policy.POLICY_WINDOW_STABLE,
        )
        # 两个文件只有一个正确。
        self.assertEqual(0.5, metrics["accuracy"])
        # 短文件未锁定数为一。
        self.assertEqual(1, metrics["unlocked_file_count"])


# 直接执行时运行本文件测试。
if __name__ == "__main__":
    # 使用详细输出便于定位具体合同。
    unittest.main(verbosity=2)
