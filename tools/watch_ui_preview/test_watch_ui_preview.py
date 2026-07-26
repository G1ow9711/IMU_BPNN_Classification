"""验证桌面 LVGL 预览器不会把内部英文状态码暴露到可见界面。"""

# 引入 JSON 解码器，核对批量截图清单的安全边界和内容哈希。
import json
# 引入正则表达式，用于检查普通用户可见标签是否含英文字母。
import re
# 引入子进程模块，用真实命令行验证无窗口 PNG 导出入口。
import subprocess
# 引入当前 Python 路径，保证测试使用同一解释器和 Pillow 环境。
import sys
# 引入临时目录，隔离测试图片并在测试结束后自动回收。
import tempfile
# 引入标准库单元测试，避免为预览工具增加第三方依赖。
import unittest
# 引入路径对象，安全组合测试脚本和 PNG 文件位置。
from pathlib import Path

# 引入 Pillow 图像读取器，核对正式 PNG 的像素尺寸、模式和内容差异。
from PIL import Image

# 引入预览器页面枚举和中文显示辅助函数；生产内部枚举仍保持稳定英文值。
from watch_ui_preview import (
    CONTROL_BUTTON_LABELS,
    DESKTOP_SCOPE_TEXT,
    DEVELOPMENT_PAGE_ORDER,
    DOCUMENTATION_PREVIEW_VARIANTS,
    PAGE_DISPLAY_NAMES,
    Page,
    development_page_label,
    export_documentation_watch_previews,
    export_watch_preview_png,
    render_watch_preview_image,
)


# 定义纯中文显示合同测试类，覆盖十三页导航和桌面控制按钮。
class ChinesePreviewContractTests(unittest.TestCase):
    """确保预览器所有开发可见标签均使用中文。"""

    # 验证十三个页面都有唯一且不含英文字母的中文名称。
    def test_all_page_labels_are_chinese(self) -> None:
        # 页面映射必须严格覆盖生产十三页，不能漏掉故障或熄屏页。
        self.assertEqual(set(PAGE_DISPLAY_NAMES), set(DEVELOPMENT_PAGE_ORDER))
        # 逐页检查最终插入列表框的可见标签。
        for page in DEVELOPMENT_PAGE_ORDER:
            # 生成用户实际看到的页面标签。
            label = development_page_label(page)
            # 标签必须有内容，避免中文化时出现空行。
            self.assertTrue(label.strip())
            # 可见标签不得暴露 BOOT、HOME 等内部英文字母状态码。
            self.assertIsNone(re.search(r"[A-Za-z]", label), label)

    # 验证右侧桌面演示按钮也全部使用中文术语。
    def test_control_button_labels_are_chinese(self) -> None:
        # 至少保留重播、熄屏、计数、动作、蓝牙、电量和关闭等控制项。
        self.assertGreaterEqual(len(CONTROL_BUTTON_LABELS), 7)
        # 逐项阻止 BLE 等英文缩写重新进入按钮。
        for label in CONTROL_BUTTON_LABELS:
            # 控制按钮必须有明确中文文字。
            self.assertTrue(label.strip())
            # 用户可见按钮不得含英文字母。
            self.assertIsNone(re.search(r"[A-Za-z]", label), label)

    # 验证桌面工具用途说明也不夹带 UI 等英文缩写。
    def test_desktop_scope_description_is_chinese(self) -> None:
        # 说明文字必须明确左侧是预览，外部控制不属于设备界面。
        self.assertIn("外部控制不属于设备界面", DESKTOP_SCOPE_TEXT)
        # 全段不得包含英文字母。
        self.assertIsNone(re.search(r"[A-Za-z]", DESKTOP_SCOPE_TEXT), DESKTOP_SCOPE_TEXT)

    # 验证最常见页面名称语义清晰，不只机械删除英文。
    def test_key_page_names_are_natural_chinese(self) -> None:
        # 冷启动页面明确写成“开机动画”。
        self.assertEqual(PAGE_DISPLAY_NAMES[Page.BOOT], "开机动画")
        # 主页名称保持简短，适合列表宽度。
        self.assertEqual(PAGE_DISPLAY_NAMES[Page.HOME], "主页")
        # 运行页面对用户显示“训练中”。
        self.assertEqual(PAGE_DISPLAY_NAMES[Page.RUNNING], "训练中")
        # 错误页面使用用户可理解的“设备故障”。
        self.assertEqual(PAGE_DISPLAY_NAMES[Page.ERROR], "设备故障")


# 定义无窗口 PNG 导出合同测试，覆盖尺寸、稳定命名、状态差异和真实 CLI。
class WatchPreviewPngExportTests(unittest.TestCase):
    """确保教程截图无需 Tk、设备或 BLE 即可确定性生成。"""

    # 验证批量导出严格生成约定八张 410×502 RGB 图片。
    def test_documentation_export_has_stable_names_and_dimensions(self) -> None:
        # 创建测试专用临时目录，不污染仓库正式截图目录。
        with tempfile.TemporaryDirectory() as temporary_directory:
            # 把临时路径转换为 Path 对象。
            output_directory = Path(temporary_directory)
            # 调用与 CLI 共用的批量导出函数。
            written_paths = export_documentation_watch_previews(output_directory)
            # 预期文件名直接来自唯一固定清单。
            expected_names = tuple(variant.filename for variant in DOCUMENTATION_PREVIEW_VARIANTS)
            # 返回路径顺序必须与清单顺序完全一致。
            self.assertEqual(tuple(path.name for path in written_paths), expected_names)
            # 当前教程固定交付八张关键状态图。
            self.assertEqual(len(written_paths), 8)
            # 批量导出必须同时生成固定机器清单。
            manifest_path = output_directory / "manifest.json"
            # 清单必须真实存在。
            self.assertTrue(manifest_path.is_file())
            # 使用 UTF-8 读取中文直出的 JSON。
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            # 清单明确禁止把 Pillow 语义预览冒充真板逐像素截图。
            self.assertFalse(manifest["PixelEquivalentToLvgl"])
            # 离屏导出不得连接蓝牙或读取设备。
            self.assertFalse(manifest["UsesBluetooth"])
            self.assertFalse(manifest["ReadsDevice"])
            # 清单图片数量必须和返回文件完全一致。
            self.assertEqual(len(manifest["Images"]), len(written_paths))
            # 清单稳定文件名必须严格等于固定状态清单。
            self.assertEqual(
                tuple(entry["File"] for entry in manifest["Images"]),
                expected_names,
            )
            # 逐张核对真实 PNG 合同。
            for output_path in written_paths:
                # 文件必须已写入磁盘。
                self.assertTrue(output_path.is_file(), output_path)
                # 使用上下文管理器及时关闭文件句柄。
                with Image.open(output_path) as image:
                    # 编码格式必须为 PNG。
                    self.assertEqual(image.format, "PNG")
                    # 像素尺寸必须严格匹配真表 410×502。
                    self.assertEqual(image.size, (410, 502))
                    # RGB 模式避免调色板和透明通道造成文档渲染差异。
                    self.assertEqual(image.mode, "RGB")
                    # 关键页面不得意外导出成全黑熄屏图。
                    self.assertIsNotNone(image.getbbox(), output_path)

    # 验证运行与休息图保留相同动作和累计，但画面状态确实不同。
    def test_running_and_rest_variants_are_visually_distinct(self) -> None:
        # 生成正常计数页。
        running_image = render_watch_preview_image(Page.RUNNING)
        # 生成休息计数暂停页。
        rest_image = render_watch_preview_image(Page.RUNNING, rest_preview=True)
        # 两张图必须保持真表像素合同。
        self.assertEqual(running_image.size, rest_image.size)
        # 休息状态的琥珀标题和提示应使像素内容不同。
        self.assertNotEqual(running_image.tobytes(), rest_image.tobytes())
        # 主动关闭第一张图释放像素内存。
        running_image.close()
        # 主动关闭第二张图释放像素内存。
        rest_image.close()

    # 验证配对覆盖层使用独立画面而不是主页截图改名。
    def test_pairing_overlay_changes_exported_pixels(self) -> None:
        # 生成普通主页。
        home_image = render_watch_preview_image(Page.HOME)
        # 在同一主页底层生成六位配对覆盖层。
        pairing_image = render_watch_preview_image(Page.HOME, pairing_preview=True)
        # 配对覆盖层必须改变主卡文字和按钮区域像素。
        self.assertNotEqual(home_image.tobytes(), pairing_image.tobytes())
        # 主动关闭主页图像。
        home_image.close()
        # 主动关闭配对图像。
        pairing_image.close()

    # 验证同一环境重复导出不会产生时间戳或随机像素差异。
    def test_single_page_export_is_deterministic(self) -> None:
        # 创建测试专用临时目录。
        with tempfile.TemporaryDirectory() as temporary_directory:
            # 定义第一次导出路径。
            first_path = Path(temporary_directory) / "first.png"
            # 定义第二次导出路径。
            second_path = Path(temporary_directory) / "second.png"
            # 第一次导出设置页。
            export_watch_preview_png(first_path, Page.SETTINGS)
            # 第二次使用完全相同状态导出设置页。
            export_watch_preview_png(second_path, Page.SETTINGS)
            # PNG 字节必须完全一致，证明无时间元数据和随机状态。
            self.assertEqual(first_path.read_bytes(), second_path.read_bytes())

    # 验证真实 CLI 可在不启动 Tk 主循环的情况下导出休息页面。
    def test_cli_exports_png_without_opening_preview_window(self) -> None:
        # 创建测试专用临时目录。
        with tempfile.TemporaryDirectory() as temporary_directory:
            # 组合 CLI 目标文件路径。
            output_path = Path(temporary_directory) / "running_rest.png"
            # 解析当前测试文件旁的预览器脚本绝对路径。
            preview_script = Path(__file__).with_name("watch_ui_preview.py")
            # 使用当前解释器执行真实无窗口导出命令。
            completed_process = subprocess.run(
                (
                    sys.executable,
                    str(preview_script),
                    "--export-png",
                    str(output_path),
                    "--page",
                    "RUNNING",
                    "--rest-preview",
                ),
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
            )
            # CLI 必须正常退出；失败时把标准错误直接纳入断言信息。
            self.assertEqual(completed_process.returncode, 0, completed_process.stderr)
            # 成功日志必须提供可供 CI 检索的稳定标记。
            self.assertIn("WATCH_UI_PNG_EXPORT_OK", completed_process.stdout)
            # 目标文件必须真实生成。
            self.assertTrue(output_path.is_file())
            # 读取并核对 CLI 产物尺寸。
            with Image.open(output_path) as image:
                # CLI 和函数入口必须共享同一 410×502 合同。
                self.assertEqual(image.size, (410, 502))


# 直接运行文件时启动标准库测试运行器，返回可靠进程退出码。
if __name__ == "__main__":
    # verbosity=2 输出每个中文化合同用例名称，便于 VS Code 终端核对。
    unittest.main(verbosity=2)
