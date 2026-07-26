"""验证“一个波峰与一个波谷计一次”的离线配对合同。"""

from __future__ import annotations

# os 读取可选的跨仓库真实数据路径；未配置时只跳过该环境回归。
import os
# unittest 提供正常、边界和真实数据回归断言。
import unittest
# Path 解析测试模块、外部 StepCounter 和用户数据路径。
from pathlib import Path

# NumPy 构造单位为加速度 raw 的合成周期信号。
import numpy as np


# 导入待测配对函数和同模块公开的数据处理依赖。
from python.analysis.stepcounter.peak_valley_pairs import (
    detect_axis_pairs,
    filter_original_python,
    load_original_python_module,
    read_seven_column_dataset,
    select_authoritative_active_segment,
)


class StubProcessor:
    """用固定峰谷模拟原 StepCounter 已完成阈值筛选的处理器。"""

    def __init__(self, peaks: tuple[int, ...], valleys: tuple[int, ...]) -> None:
        # p_loc 保存测试指定峰位置，单位样本点。
        self.p_loc = np.asarray(peaks, dtype=np.int32)
        # p_cnt 保存有效峰数量。
        self.p_cnt = len(peaks)
        # v_loc 保存测试指定谷位置，单位样本点。
        self.v_loc = np.asarray(valleys, dtype=np.int32)
        # v_cnt 保存有效谷数量。
        self.v_cnt = len(valleys)

    def find_possible_peak_valley(self, _: np.ndarray) -> StubProcessor:
        """保持测试极值不变并返回自身。"""

        # 测试重点是配对阶段，不重复验证外部参考工程的局部极值搜索。
        return self

    def remove_false_peak_valley(self, _: np.ndarray) -> StubProcessor:
        """保持测试极值不变并返回自身。"""

        # 假定固定峰谷已位于均值正确一侧。
        return self

    def merge_close_peaks_valleys(self, _: np.ndarray) -> StubProcessor:
        """保持测试极值不变并返回自身。"""

        # 假定固定峰谷已完成相邻同类极值合并。
        return self

    def merge_close_pole(self, _: np.ndarray, __: int) -> StubProcessor:
        """保持单类极值不变并返回自身。"""

        # 固定测试输入已经没有需要合并的相邻同类极值。
        return self

    def remove_asymmetric_peaks(self, _: np.ndarray) -> StubProcessor:
        """保持测试极值不变并返回自身。"""

        # 假定固定峰谷已通过原幅值、对称性和宽度筛选。
        return self


class StubModule:
    """提供与原模块相同的 action_processor 构造接口。"""

    def __init__(self, peaks: tuple[int, ...], valleys: tuple[int, ...]) -> None:
        # peaks 保存每次构造处理器都要返回的峰位置。
        self.peaks = peaks
        # valleys 保存每次构造处理器都要返回的谷位置。
        self.valleys = valleys

    def action_processor(self, _: int, __: int) -> StubProcessor:
        """返回带固定极值的处理器。"""

        # 每次返回新对象，避免前一测试状态污染后一测试。
        return StubProcessor(self.peaks, self.valleys)


class PeakValleyPairContractTests(unittest.TestCase):
    """覆盖一峰一谷、末尾半周期、谷复用和真实数据回归。"""

    def test_one_peak_and_one_following_valley_emit_exactly_one_repetition(self) -> None:
        """三个完整 P→V 周期必须输出三次，禁止乘二。"""

        # signal 保存 100 点合成加速度，单位 raw。
        signal = np.zeros(100, dtype=np.int32)
        # 三个峰均为 +900 raw，高于 4096/14 的幅值阈值。
        signal[[10, 40, 70]] = 900
        # 三个谷均为 -900 raw，峰谷幅值为 1800 raw。
        signal[[20, 50, 80]] = -900
        # module 固定返回三峰三谷。
        module = StubModule((10, 40, 70), (20, 50, 80))
        # 执行单轴配对检测。
        result = detect_axis_pairs(signal, 0, module)
        # 每一组峰谷只能计一次，最终必须是 3 而不是 6。
        self.assertEqual(len(result.pairs), 3)
        # 三个动作编号必须连续为 1、2、3。
        self.assertEqual(tuple(pair.ordinal for pair in result.pairs), (1, 2, 3))

    def test_trailing_peak_without_following_valley_is_not_counted(self) -> None:
        """动作段末尾只有峰、没有谷时属于半周期，不能提前计数。"""

        # signal 保存两个完整周期和一个末尾孤立峰。
        signal = np.zeros(100, dtype=np.int32)
        # 三个峰位置依次为 10、40、70。
        signal[[10, 40, 70]] = 900
        # 只有前两个峰存在右侧谷。
        signal[[20, 50]] = -900
        # module 固定返回三峰两谷。
        module = StubModule((10, 40, 70), (20, 50))
        # 执行单轴配对检测。
        result = detect_axis_pairs(signal, 1, module)
        # 只有两个完整 P→V 配对可计数。
        self.assertEqual(len(result.pairs), 2)

    def test_valley_then_peak_at_segment_boundary_is_counted(self) -> None:
        """动作段从波谷开始时，V→P 也必须作为一个完整峰谷配对。"""

        # signal 保存一个先谷后峰的边界周期，单位加速度 raw。
        signal = np.zeros(50, dtype=np.int32)
        # 首个有效谷位于样本 5。
        signal[5] = -900
        # 后续有效峰位于样本 16，间隔 11 点且幅值 1800 raw。
        signal[16] = 900
        # module 固定返回一个峰和一个更早的谷。
        module = StubModule((16,), (5,))
        # 执行无方向峰谷配对检测。
        result = detect_axis_pairs(signal, 0, module)
        # V→P 必须计一次，防止谷-峰-谷包装删除动作段边界周期。
        self.assertEqual(len(result.pairs), 1)
        # 配对仍需保存真实峰位置 16。
        self.assertEqual(result.pairs[0].peak_index, 16)
        # 配对仍需保存真实谷位置 5。
        self.assertEqual(result.pairs[0].valley_index, 5)

    def test_one_valley_cannot_be_reused_by_two_peaks(self) -> None:
        """两个接近峰共享同一谷时只能形成一次动作，防止重复计数。"""

        # signal 构造两个峰和一个谷。
        signal = np.zeros(60, dtype=np.int32)
        # 两个峰均超过幅值阈值。
        signal[[10, 15]] = 900
        # 唯一谷位于两个峰右侧。
        signal[22] = -900
        # module 固定返回两个峰和一个谷。
        module = StubModule((10, 15), (22,))
        # 执行单轴配对检测。
        result = detect_axis_pairs(signal, 2, module)
        # 第一个峰占用谷后，第二个峰不得复用同一谷。
        self.assertEqual(len(result.pairs), 1)

    def test_three_real_jumping_jack_segments_match_user_visual_counts(self) -> None:
        """三份本地 408 点数据必须匹配用户视觉数 16、15、16。"""

        # 从环境变量读取外部 StepCounter 参考工程，避免测试绑定作者磁盘布局。
        stepcounter_value = os.environ.get("IMU_STEPCOUNTER_ROOT", "").strip()
        # 从环境变量读取三份外部开合跳数据目录。
        data_value = os.environ.get("IMU_STEPCOUNTER_DATA_ROOT", "").strip()
        # 缺任一路径时跳过可选跨仓库回归；四条纯算法合同仍始终执行。
        if not stepcounter_value or not data_value:
            # skipTest 明确显示配置方法，不把缺少私有数据误判成算法失败。
            self.skipTest(
                "设置 IMU_STEPCOUNTER_ROOT 和 IMU_STEPCOUNTER_DATA_ROOT "
                "后运行真实数据回归。"
            )
        # 解析用户提供的 StepCounter 工程路径。
        stepcounter_root = Path(stepcounter_value).resolve()
        # 解析用户提供的三份开合跳数据目录。
        data_root = Path(data_value).resolve()
        # 外部参考算法入口必须存在。
        self.assertTrue(
            (stepcounter_root / "Python" / "step_counter_simulation.py").is_file()
        )
        # 数据目录必须存在，防止 glob 空集合给出误导断言。
        self.assertTrue(data_root.is_dir())
        # original_module 动态加载参考极值筛选代码。
        original_module = load_original_python_module(stepcounter_root)
        # fused_counts 保存三份数据的三轴配对中位数。
        fused_counts: list[int] = []
        # 逐份读取三条 jumping_jack_scy*_20 数据。
        for data_path in sorted(data_root.glob("jumping_jack_scy*_20.txt")):
            # 截取与 MATLAB 训练入口一致的 408 点动作段。
            dataset = select_authoritative_active_segment(read_seven_column_dataset(data_path))
            # 双均值滤波三轴加速度 raw。
            filtered = filter_original_python(dataset.values_raw[:, 3:6], original_module)
            # 逐轴计算一峰一谷配对数。
            axis_counts = [
                len(detect_axis_pairs(filtered[:, axis_index], axis_index, original_module).pairs)
                for axis_index in range(3)
            ]
            # 三轴中位数转为整数并加入结果。
            fused_counts.append(int(np.median(np.asarray(axis_counts, dtype=np.int32))))
        # 必须恰好读取三份真实数据。
        self.assertEqual(len(fused_counts), 3)
        # 三份结果必须与用户逐图视觉核数一致，防止首尾合法半周期再次被删除。
        self.assertEqual(fused_counts, [16, 15, 16])


# 直接运行该文件时执行全部 unittest。
if __name__ == "__main__":
    # verbosity=2 输出每条中文合同名称和通过状态。
    unittest.main(verbosity=2)
