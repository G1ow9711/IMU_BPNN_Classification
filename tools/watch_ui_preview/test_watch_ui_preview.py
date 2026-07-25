"""验证桌面 LVGL 预览器不会把内部英文状态码暴露到可见界面。"""

# 引入正则表达式，用于检查普通用户可见标签是否含英文字母。
import re
# 引入标准库单元测试，避免为预览工具增加第三方依赖。
import unittest

# 引入预览器页面枚举和中文显示辅助函数；生产内部枚举仍保持稳定英文值。
from watch_ui_preview import (
    CONTROL_BUTTON_LABELS,
    DESKTOP_SCOPE_TEXT,
    DEVELOPMENT_PAGE_ORDER,
    PAGE_DISPLAY_NAMES,
    Page,
    development_page_label,
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


# 直接运行文件时启动标准库测试运行器，返回可靠进程退出码。
if __name__ == "__main__":
    # verbosity=2 输出每个中文化合同用例名称，便于 VS Code 终端核对。
    unittest.main(verbosity=2)
