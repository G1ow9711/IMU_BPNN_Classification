"""验证最终双六分支 M0 便携工件、C 头合同和错误拒绝路径。"""

# hashlib 用于核对测试模型文件的 SHA-256，模拟正式模型清单。
import hashlib
# json 用于读取导出器生成的机器可读模型清单。
import json
# tempfile 为每个测试提供自动清理的独立目录，避免污染工程输出。
import tempfile
# unittest 提供项目现有测试体系一致的标准库断言。
import unittest
# Path 以跨平台方式构造临时工件和导出路径。
from pathlib import Path

# NumPy 构造形状为 [样本数,297] 的 float32 原始特征和 scaler。
import numpy as np
# PyTorch 创建与正式 M0 相同的六分支测试网络并计算参考 logits。
import torch

# dual_m0_export 是已经完成的生产导出模块；本测试防止后续修改破坏已锁定部署合同。
import dual_m0_export as dme
# train_export 提供正式 297 维名称、类别和 MultiBranchBPNet 结构。
import train_export as te


class DualM0ExportTests(unittest.TestCase):
    """使用确定性小工件验证最终两个 M0 的完整部署合同。"""

    def _write_artifact(self, root: Path, seed: int, masked: bool) -> Path:
        """在 root 写入一个与正式验证工件同结构的确定性 M0 目录。"""
        # 固定 PyTorch 种子，使每次测试权重和期望 logits 完全可复现。
        torch.manual_seed(seed)
        # 类别顺序直接复用生产合同，输出形状固定为 [样本数,11]。
        class_names = list(dme.PRODUCTION_CLASS_NAMES)
        # 特征名称直接复用生产顺序，输入形状固定为 [样本数,297]。
        feature_names = te.build_feature_names()
        # 建立无 dropout 的六分支 M0；推理模式不会包含训练期随机性。
        model = te.MultiBranchBPNet(len(feature_names), len(class_names), dropout=0.0)
        # 切换到推理模式，确保未来若网络包含 dropout 时参考输出仍确定。
        model.eval()
        # 工件目录按调用方给定路径创建，parents=True 支持嵌套临时目录。
        root.mkdir(parents=True, exist_ok=True)
        # 保存完整 state_dict；其中辅助头存在，但部署导出器必须主动排除。
        torch.save(model.state_dict(), root / "best_model.pt")
        # mean 形状 [297]，使用小幅线性变化检查逐列标准化而非零均值捷径。
        mean = np.linspace(-0.25, 0.25, len(feature_names), dtype=np.float32)
        # std 形状 [297] 且严格大于零，覆盖不同尺度下的标准化。
        std = np.linspace(0.5, 1.5, len(feature_names), dtype=np.float32)
        # scaler 字段模拟 validation-only 工件，并显式记录模型类型和掩码。
        np.savez(
            root / "scaler_and_config.npz",
            mean=mean,
            std=std,
            class_names=np.asarray(class_names),
            feature_names=np.asarray(feature_names),
            window_len=np.asarray([62], dtype=np.int32),
            step_len=np.asarray([12], dtype=np.int32),
            sample_rate=np.asarray([25], dtype=np.int32),
            rest_threshold=np.asarray([0.08418716], dtype=np.float32),
            active_point_threshold=np.asarray([0.13165437], dtype=np.float32),
            suppress_normalized_phase=np.asarray([masked], dtype=np.bool_),
            model_type=np.asarray(["multi_branch"]),
        )
        # 返回工件目录，供加载器和错误修改测试复用。
        return root

    def test_numpy_logits_match_torch_for_both_models(self) -> None:
        """纯 NumPy 部署参考必须与 PyTorch 两个 M0 的 logits 在 1e-5 内一致。"""
        # 临时目录保证模型、scaler 和导出文件在测试结束后删除。
        with tempfile.TemporaryDirectory() as temp_dir:
            # base 工件不屏蔽归一化阶段特征。
            base_dir = self._write_artifact(Path(temp_dir) / "base", seed=7, masked=False)
            # masked 工件屏蔽标准化后索引 184:232。
            masked_dir = self._write_artifact(Path(temp_dir) / "masked", seed=11, masked=True)
            # 加载并验证两个工件的特征、类别、架构和掩码合同。
            bundle = dme.load_dual_m0_bundle(base_dir, masked_dir)
            # 固定随机数生成器产生形状 [5,297] 的有限 float32 原始特征。
            raw = np.random.default_rng(19).normal(size=(5, 297)).astype(np.float32)
            # 遍历 base 和 masked 两个模型，分别核对独立 scaler 与掩码路径。
            for artifact in (bundle.base, bundle.masked):
                # NumPy 前向模拟 ESP32 C 的逐层 row-major 乘加和 ReLU。
                actual = dme.numpy_m0_logits(artifact, raw)
                # 使用同一工件加载的 PyTorch 模型计算 [5,11] 参考 logits。
                expected = dme.torch_m0_logits(artifact, raw)
                # 最大绝对误差必须小于 1e-5，严于最终 C 合同 1e-3。
                np.testing.assert_allclose(actual, expected, rtol=0.0, atol=1e-5)

    def test_export_writes_dual_headers_bundle_and_manifest(self) -> None:
        """正式导出必须同时生成特征头、双模型头、NPZ 和哈希清单。"""
        # 临时目录隔离输入工件与导出目录。
        with tempfile.TemporaryDirectory() as temp_dir:
            # root 统一承载两个测试模型和最终 out 目录。
            root = Path(temp_dir)
            # 创建未掩码基础模型工件。
            base_dir = self._write_artifact(root / "base", seed=23, masked=False)
            # 创建屏蔽 184:232 的互补模型工件。
            masked_dir = self._write_artifact(root / "masked", seed=29, masked=True)
            # out 是导出器唯一允许写入的目标目录。
            output_dir = root / "out"
            # 执行一站式导出，并取得机器可读结果路径。
            paths = dme.export_dual_m0_bundle(base_dir, masked_dir, output_dir)
            # 四个必要交付物必须实际存在，不能只返回计划路径。
            for artifact_path in paths.values():
                # 每个路径都应指向普通文件。
                self.assertTrue(Path(artifact_path).is_file(), artifact_path)
            # 读取双模型头，核对正式架构、掩码、融合和动作段累计接口。
            model_header = Path(paths["model_header"]).read_text(encoding="utf-8")
            # 头文件必须声明六个物理分支，而不是旧平铺 BPNet。
            self.assertIn("#define BP_M0_BRANCH_COUNT 6", model_header)
            # 两个模型必须拥有各自独立的 scaler 数组。
            self.assertIn("BP_BASE_FEATURE_MEAN", model_header)
            # 屏蔽模型的标准化参数不能复用基础模型数组。
            self.assertIn("BP_MASKED_FEATURE_MEAN", model_header)
            # C 前向必须暴露单 M0 logits 接口，便于逐值测试和诊断。
            self.assertIn("bp_m0_forward", model_header)
            # C 端必须暴露固定双模型融合前向。
            self.assertIn("bp_dual_m0_forward", model_header)
            # 掩码半开区间必须与 Python 的 184:232 完全一致。
            self.assertIn("BP_MASKED_FEATURE_START 184", model_header)
            # 训练辅助头不能进入部署头，避免无用 Flash 和错误前向。
            self.assertNotIn("auxiliary_heads", model_header)
            # 特征头必须保留 297 维生产提取函数。
            feature_header = Path(paths["feature_header"]).read_text(encoding="utf-8")
            # 生成的特征头声明固定输入维度。
            self.assertIn("#define FEATURE_DIM 297", feature_header)
            # 特征头必须提供六轴窗口到 297 维的唯一生产入口。
            self.assertIn("extract_features_from_window", feature_header)
            # 特征头不得携带旧平铺 BP 四层权重数组。
            self.assertNotIn("static const float W1[", feature_header)
            # 清单必须记录两个输入模型哈希和便携包哈希。
            manifest = json.loads(Path(paths["manifest"]).read_text(encoding="utf-8"))
            # base 模型哈希应与原始文件逐字节 SHA-256 相同。
            expected_hash = hashlib.sha256((base_dir / "best_model.pt").read_bytes()).hexdigest()
            # 清单哈希使用小写十六进制，便于跨语言比较。
            self.assertEqual(manifest["models"]["base"]["sha256"], expected_hash)
            # 清单必须明确第二模型启用归一化阶段屏蔽。
            self.assertTrue(manifest["models"]["masked"]["suppress_normalized_phase"])
            # 便携 NPZ 只保留部署主路径；任何 key 都不得含辅助头名称。
            # with 确保 Windows 文件句柄在临时目录清理前关闭。
            with np.load(paths["portable_bundle"], allow_pickle=False) as portable:
                # 遍历全部键，保证未把五个训练辅助头误打包。
                self.assertFalse(any("auxiliary" in key for key in portable.files))

    def test_loader_rejects_class_order_mismatch(self) -> None:
        """两个模型类别顺序不一致时必须在导出前失败。"""
        # 临时目录承载可故意破坏的两个工件。
        with tempfile.TemporaryDirectory() as temp_dir:
            # 创建有效基础工件。
            base_dir = self._write_artifact(Path(temp_dir) / "base", seed=31, masked=False)
            # 创建有效掩码工件，随后只破坏类别顺序。
            masked_dir = self._write_artifact(Path(temp_dir) / "masked", seed=37, masked=True)
            # 读取原 NPZ 的全部数组，保持除类别顺序外的字段不变。
            # with 保证读取完成后释放 Windows 对 NPZ 的共享锁。
            with np.load(masked_dir / "scaler_and_config.npz", allow_pickle=False) as source:
                # 将只读 NpzFile 转成独立数组字典，便于关闭文件后替换单一字段。
                fields = {name: np.array(source[name], copy=True) for name in source.files}
            # 交换前两类，模拟 PC、Python 与 ESP32 类别索引错位。
            fields["class_names"] = fields["class_names"][[1, 0, *range(2, 11)]]
            # 覆盖测试 NPZ；模型权重保持原样。
            np.savez(masked_dir / "scaler_and_config.npz", **fields)
            # 加载器必须给出明确 ValueError，禁止生成不可用头文件。
            with self.assertRaisesRegex(ValueError, "class order"):
                # 调用双工件加载入口触发跨模型合同校验。
                dme.load_dual_m0_bundle(base_dir, masked_dir)


# 直接执行本文件时启动 unittest；discover 模式也会发现同一测试类。
if __name__ == "__main__":
    # verbosity=2 显示每个导出合同名称，便于定位失败阶段。
    unittest.main(verbosity=2)
