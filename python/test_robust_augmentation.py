"""验证跨人右腕增强的物理单位、固定窗口和命令行隔离合同。"""

# json 生成与生产 validation_report.json 同结构的冻结验证合同。
import json
# sys 提供可替换的命令行参数列表。
import sys
# tempfile 创建自动清理的冻结主模型合同目录。
import tempfile
# unittest 提供独立断言和标准测试入口。
import unittest
# Path 生成跨平台临时工件路径。
from pathlib import Path
# mock 临时替换 sys.argv，不启动真实训练。
from unittest import mock

# NumPy 构造 `[62,6]` 六轴窗口并执行逐值比较。
import numpy as np
# PyTorch 构造 M0 首层并验证剪枝后的完整前向与运行时掩码等价。
import torch

# 导入待测训练端；导入不会扫描数据或启动训练。
from python import train_export as te


class RobustAugmentationTests(unittest.TestCase):
    """覆盖动态幅度、动作历时和 baseline/robust 显式选择。"""

    def test_dynamic_amplitude_preserves_gravity_and_scales_motion(self):
        # window 形状固定 `[62,6]`，顺序为 gx、gy、gz、ax、ay、az。
        window = np.zeros((62, 6), dtype=np.float32)
        # gx 使用对称线性角速度，单位 deg/s、均值为零。
        window[:, 0] = np.linspace(-100.0, 100.0, 62, dtype=np.float32)
        # gy 使用较小反向角速度，验证全部陀螺通道统一缩放。
        window[:, 1] = np.linspace(40.0, -40.0, 62, dtype=np.float32)
        # az 以 1 g 为平均重力并叠加对称动态加速度。
        window[:, 5] = 1.0 + np.linspace(-0.30, 0.30, 62, dtype=np.float32)
        # scaled 把同一动作动态幅度缩小到 70%，模拟幅度较小或疲劳用户。
        scaled = te.scale_imu_dynamic_amplitude(window, 0.70)
        # 输出点数和六轴宽度必须保持部署合同。
        self.assertEqual(scaled.shape, (62, 6))
        # 输出精度必须保持 float32。
        self.assertEqual(scaled.dtype, np.float32)
        # 三轴角速度动态量应逐点缩放到 70%。
        np.testing.assert_allclose(
            scaled[:, 0:3],
            window[:, 0:3] * 0.70,
            atol=1.0e-5,
        )
        # 三轴平均加速度代表窗口重力/姿态基线，缩放前后必须一致。
        np.testing.assert_allclose(
            np.mean(scaled[:, 3:6], axis=0),
            np.mean(window[:, 3:6], axis=0),
            atol=1.0e-6,
        )
        # original_dynamic 是原 az 相对平均重力的动态分量。
        original_dynamic = window[:, 5] - np.mean(window[:, 5])
        # scaled_dynamic 是增强后 az 动态分量。
        scaled_dynamic = scaled[:, 5] - np.mean(scaled[:, 5])
        # 动态分量必须精确缩放到 70%。
        np.testing.assert_allclose(
            scaled_dynamic,
            original_dynamic * 0.70,
            atol=1.0e-6,
        )

    def test_duration_scaling_keeps_fixed_window_contract(self):
        # window 构造可见时间索引，形状固定 `[62,6]`。
        window = np.zeros((62, 6), dtype=np.float32)
        # gx 从 0 到 61，便于检查插值覆盖范围。
        window[:, 0] = np.arange(62, dtype=np.float32)
        # az 保持常量 1 g，验证时间插值不破坏静态重力。
        window[:, 5] = 1.0
        # faster 用 0.80 历时模拟较快动作，越界采用端点保持。
        faster = te.resample_imu_duration_fixed_window(window, 0.80)
        # slower 用 1.25 历时模拟较慢动作，固定窗只读取原过程中央部分。
        slower = te.resample_imu_duration_fixed_window(window, 1.25)
        # 两种速度变化都必须保持 62 点六轴形状。
        self.assertEqual(faster.shape, window.shape)
        # 慢动作形状同样不变。
        self.assertEqual(slower.shape, window.shape)
        # 快动作输出不得含 NaN 或 Inf。
        self.assertTrue(np.all(np.isfinite(faster)))
        # 慢动作输出不得含 NaN 或 Inf。
        self.assertTrue(np.all(np.isfinite(slower)))
        # 较快动作在输出首点保持原始起点。
        self.assertAlmostEqual(float(faster[0, 0]), 0.0, places=6)
        # 较快动作在输出末点保持原始终点。
        self.assertAlmostEqual(float(faster[-1, 0]), 61.0, places=6)
        # 较慢动作在同一窗口内遍历的原进程范围应小于 61。
        self.assertLess(float(slower[-1, 0] - slower[0, 0]), 61.0)
        # 快动作的常量 1 g 通道必须保持不变。
        np.testing.assert_allclose(faster[:, 5], 1.0, atol=1.0e-7)
        # 慢动作的常量 1 g 通道必须保持不变。
        np.testing.assert_allclose(slower[:, 5], 1.0, atol=1.0e-7)

    def test_robust_profile_is_explicit_and_baseline_remains_default(self):
        # 不传增强开关时解析历史复现配置。
        with mock.patch.object(sys, "argv", ["train_export.py"]):
            # baseline_args 不读取数据，仅解析默认参数。
            baseline_args = te.parse_args()
        # 默认必须保持 baseline，防止历史 Round29/Round37 无法复现。
        self.assertEqual(baseline_args.augmentation_profile, "baseline")
        # 绝对轴统计默认保留，历史训练和旧工件函数不得隐式改变。
        self.assertFalse(baseline_args.suppress_absolute_axis_stats)
        # 显式 robust 才允许跨人角度、幅度和速度增强。
        with mock.patch.object(
            sys,
            "argv",
            [
                "train_export.py",
                "--augmentation-profile",
                "robust",
                "--suppress-absolute-axis-stats",
            ],
        ):
            # robust_args 保存显式新档位。
            robust_args = te.parse_args()
        # 参数必须原样传入训练实验。
        self.assertEqual(robust_args.augmentation_profile, "robust")
        # 结构候选只在显式命令行开关下启用。
        self.assertTrue(robust_args.suppress_absolute_axis_stats)

    def test_absolute_axis_mask_only_zeros_first_six_axis_statistics(self):
        # values 模拟两条 `[样本数,297]` 标准化输入，并用不同数值检查未屏蔽列。
        values = np.arange(2 * 297, dtype=np.float32).reshape(2, 297) + 1.0
        # masked 只启用绝对轴统计屏蔽，不启用历史归一化阶段屏蔽。
        masked = te.apply_model_feature_mask(
            values,
            suppress_normalized_phase=False,
            suppress_absolute_axis_stats=True,
        )
        # 索引 0:48 对应 gx、gy、gz、ax、ay、az 各 8 项统计量，必须全部归零。
        np.testing.assert_array_equal(masked[:, 0:48], 0.0)
        # 索引 48:297 是模长、重力相对、周期、频谱和冲击特征，必须逐值保持。
        np.testing.assert_array_equal(masked[:, 48:], values[:, 48:])
        # 输入数组不得被原地修改，避免同一验证特征被其它候选污染。
        self.assertTrue(np.all(values[:, 0:48] > 0.0))

    def test_pruned_m0_is_independent_of_absolute_axis_columns(self):
        # model 使用生产 M0 六分支结构；dropout 为零消除无关训练随机性。
        model = te.MultiBranchBPNet(297, 11, dropout=0.0)
        # eval 关闭所有训练期行为，模拟导出和 ESP32 推理。
        model.eval()
        # prune 永久清零统计分支首层的绝对轴输入列。
        te.prune_absolute_axis_input_weights(model)
        # first_weight 形状为 `[24,112]`，前 48 列必须精确为零。
        first_weight = model.branches[0][0].weight.detach().cpu().numpy()
        # 所有被删除列都应严格等于零，不能只接近零。
        np.testing.assert_array_equal(first_weight[:, 0:48], 0.0)
        # raw 构造三条任意标准化输入，包含非零绝对轴统计。
        raw = torch.randn(3, 297, dtype=torch.float32)
        # masked 复制输入并把绝对轴统计替换为训练均值零分。
        masked = raw.clone()
        # 半开区间 0:48 与 Python 特征合同完全一致。
        masked[:, 0:48] = 0.0
        # no_grad 仅比较推理函数，不构建反向图。
        with torch.no_grad():
            # raw_logits 使用任意绝对轴输入。
            raw_logits = model(raw)
            # masked_logits 使用显式归零后的同一输入。
            masked_logits = model(masked)
        # 首层权重剪枝后两路必须逐位相等，证明 C 端无需额外掩码分支。
        torch.testing.assert_close(raw_logits, masked_logits, rtol=0.0, atol=0.0)

    def test_primary_sampling_contract_restores_thresholds_and_masks(self):
        # 临时目录模拟已冻结主模型的 scaler_and_config.npz，不创建真实训练输出。
        with tempfile.TemporaryDirectory() as temp_dir:
            # artifact_dir 是当前临时工件根目录。
            artifact_dir = Path(temp_dir)
            # 写入 62 点、12 点步长、两个活动门和两种输入掩码的完整合同。
            np.savez(
                artifact_dir / "scaler_and_config.npz",
                # 端侧窗口固定 62 点。
                window_len=np.asarray([62], dtype=np.int64),
                # 训练验证窗口步长固定 12 点。
                step_len=np.asarray([12], dtype=np.int64),
                # 窗口活动门使用可精确比较的测试值。
                rest_threshold=np.asarray([0.0841871575], dtype=np.float32),
                # 逐点活动门使用另一测试值。
                active_point_threshold=np.asarray([0.131654367], dtype=np.float32),
                # 历史归一化阶段掩码在本工件关闭。
                suppress_normalized_phase=np.asarray([False], dtype=np.bool_),
                # 绝对轴统计在本工件启用屏蔽。
                suppress_absolute_axis_stats=np.asarray([True], dtype=np.bool_),
            )
            # validation_report.json 固定主模型选模种子和验证文件身份。
            (artifact_dir / "validation_report.json").write_text(
                json.dumps(
                    {
                        # seed 是 sklearn 文件级分层切分使用的确定性随机种子。
                        "seed": 20260709,
                        # all_experiments 只放一个与 62/12 点合同匹配的实验。
                        "all_experiments": [
                            {
                                # window_len 与 npz 工件保持一致。
                                "window_len": 62,
                                # step_len 与 npz 工件保持一致。
                                "step_len": 12,
                                # val_files 使用不同根目录，验证身份键不依赖绝对根路径。
                                "val_files": [
                                    r"D:\dataset\walk\walk_01.txt",
                                    r"D:\dataset\wave\wave_01.txt",
                                ],
                            }
                        ],
                    },
                    # ensure_ascii=False 保持未来中文路径可读。
                    ensure_ascii=False,
                ),
                # UTF-8 与生产报告编码合同一致。
                encoding="utf-8",
            )
            # contract 必须从工件读取，不得重新估计或读取外部数据。
            contract = te.load_primary_sampling_contract(
                artifact_dir,
                expected_window_len=62,
                expected_step_len=12,
            )
        # 静止门应保持 float32 工件允许的 1e-7 精度。
        self.assertAlmostEqual(float(contract["rest_threshold"]), 0.0841871575, places=7)
        # 逐点门同样逐值恢复。
        self.assertAlmostEqual(
            float(contract["active_point_threshold"]), 0.131654367, places=7
        )
        # 归一化阶段掩码必须保持关闭。
        self.assertFalse(bool(contract["suppress_normalized_phase"]))
        # 绝对轴掩码必须保持开启，避免专家重新使用 XYZ 姿态。
        self.assertTrue(bool(contract["suppress_absolute_axis_stats"]))
        # 文件级划分随机种子必须恢复为主模型报告值，不能继续使用候选 seed。
        self.assertEqual(int(contract["split_seed"]), 20260709)
        # 验证文件键只含类别和文件名，且按确定性顺序排序。
        self.assertEqual(
            tuple(contract["validation_file_keys"]),
            ("walk/walk_01.txt", "wave/wave_01.txt"),
        )

    def test_validation_output_preserves_primary_and_family_specialist(self):
        # feature_names 是生产 297 维顺序，主 M0 必须完整接收该维度。
        feature_names = te.build_feature_names()
        # class_names 构造三类全局输出，便于验证专家局部类别映射。
        class_names = ["jumping_lunge", "squat", "walk"]
        # specialist_indices 只取三个可审计列，测试无需依赖真实形态索引长度。
        specialist_indices = np.asarray([48, 112, 232], dtype=np.int64)
        # primary 使用六分支 M0，覆盖本轮复用主模型的真实结构。
        primary = te.MultiBranchBPNet(len(feature_names), len(class_names), dropout=0.0)
        # specialist 使用平铺 BP，输入维度等于局部索引数量，输出覆盖两个局部类别。
        specialist = te.BPNet(len(specialist_indices), 2, dropout=0.0)
        # result 模拟验证模式训练完成后的完整原子候选。
        result = {
            # 窗口长度和步长与端侧 62/12 点合同一致。
            "window_seconds": 2.5,
            "window_len": 62,
            "step_len": 12,
            # 文件级划分 seed 必须进入报告，不能退回模块默认猜测。
            "split_seed": 20260709,
            # 两个活动门只用于验证工件字段完整性。
            "rest_threshold": 0.084,
            "active_point_threshold": 0.132,
            # 主模型和专家必须写入同一个 best_model.pt。
            "model": primary,
            "specialist_model": specialist,
            # 主模型标准化参数形状均为 [297]。
            "mean": np.zeros(len(feature_names), dtype=np.float32),
            "std": np.ones(len(feature_names), dtype=np.float32),
            # 专家使用独立三维均值和标准差。
            "specialist_mean": np.zeros(len(specialist_indices), dtype=np.float32),
            "specialist_std": np.ones(len(specialist_indices), dtype=np.float32),
            # 专家只区分跳跃弓步和步行，顺序必须原样保存。
            "specialist_class_names": ["jumping_lunge", "walk"],
            # 专家列索引必须以 int64 保存，供 Python/C 逐项映射。
            "specialist_feature_indices": specialist_indices,
            # 三类验证指标为报告必需字段。
            "val_acc": 1.0,
            "val_f1": 1.0,
            "val_weak_recall": 1.0,
            "val_min_recall": 1.0,
            "val_class_recalls": {name: 1.0 for name in class_names},
            # 每类各一条预测使 classification_report 类别维完整。
            "y_val": np.asarray([0, 1, 2], dtype=np.int64),
            "val_pred": np.asarray([0, 1, 2], dtype=np.int64),
        }
        # 临时目录在测试结束后自动清理，不污染真实候选工件。
        with tempfile.TemporaryDirectory() as temp_dir:
            # output_dir 模拟 validation-only 输出目录。
            output_dir = Path(temp_dir)
            # 保存主模型、专家、标准化配置和报告。
            te.save_validation_outputs(
                result,
                [result],
                class_names,
                feature_names,
                output_dir,
            )
            # weights 只允许张量和容器，避免加载任意 pickle 对象。
            weights = torch.load(
                output_dir / "best_model.pt",
                map_location="cpu",
                weights_only=True,
            )
            # config 以无 pickle 模式读取专家标准化和索引。
            with np.load(output_dir / "scaler_and_config.npz", allow_pickle=False) as config:
                # npz_fields 固化字段集合，离开上下文后仍可断言。
                npz_fields = set(config.files)
                # saved_indices 复制出专家索引，避免引用已关闭的 zip 句柄。
                saved_indices = np.asarray(config["specialist_feature_indices"], dtype=np.int64)
            # report 验证结构名称、划分 seed 和专家类别可审计。
            report = json.loads(
                (output_dir / "validation_report.json").read_text(encoding="utf-8")
            )
        # 权重文件必须同时包含主 M0 与形态专家，禁止静默丢失任一网络。
        self.assertEqual(set(weights.keys()), {"primary", "family_specialist"})
        # 专家四项配置必须全部存在，缺一项都无法复现路由前向。
        self.assertTrue(
            {
                "specialist_mean",
                "specialist_std",
                "specialist_class_names",
                "specialist_feature_indices",
            }.issubset(npz_fields)
        )
        # 专家索引逐值保持，不能在保存时重排或转成浮点。
        np.testing.assert_array_equal(saved_indices, specialist_indices)
        # 报告必须明确这是六分支主模型加局部专家，而非普通单模型。
        self.assertEqual(
            report["classifier_type"],
            "multi_branch_plus_family_bp_specialist",
        )
        # 报告 seed 必须等于实际文件级冻结划分。
        self.assertEqual(int(report["seed"]), 20260709)
        # 专家类别顺序与训练结果完全一致。
        self.assertEqual(
            report["family_specialist_class_names"],
            ["jumping_lunge", "walk"],
        )


# 直接运行本文件时执行全部测试；discover 导入时不重复启动。
if __name__ == "__main__":
    # 使用标准 unittest 入口。
    unittest.main()
