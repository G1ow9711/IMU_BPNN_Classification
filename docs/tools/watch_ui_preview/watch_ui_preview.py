"""文档工具使用 Tkinter 模拟 410×502 AMOLED 产品界面。

本工具只验证页面层级、文字布局、按钮流程和轻量动画观感，不验证 LVGL
字体度量、触摸坐标、驱动刷新时序或 ESP32 性能。页面文字与当前
``esp32/firmware/components/ui/ui_presenter.c`` 保持同一产品语义。
正式 PNG 导出使用 Pillow 离屏绘制，不读取桌面像素、不连接设备或 BLE。
"""

# 延迟解析类型注解，避免 Python 3.12 的 tkinter.Event 在运行时被当作可下标泛型。
from __future__ import annotations

# 引入命令行解析器，使无显示环境可以执行纯逻辑冒烟测试。
import argparse
# 引入 SHA-256，实现截图与生成器源码的可审计内容指纹。
import hashlib
# 引入 JSON 编码器，写出机器可读且稳定排序的截图清单。
import json
# 引入数据类，为页面模型和运行状态提供明确字段合同。
from dataclasses import dataclass, field
# 引入枚举类型，避免页面和命令依赖易拼错的裸字符串。
from enum import Enum
# 引入路径对象，统一处理正式 PNG 输出目录和跨平台字体候选。
from pathlib import Path
# 引入 Tkinter；它属于 Python 标准库，不产生第三方安装依赖。
import tkinter as tk
# 引入 Tkinter 字体模块，统一模拟屏内中文标签的字号和字重。
from tkinter import font as tkfont
# 引入类型提示，说明按钮列表、点击区域和回调的形状。
from typing import Callable

# 引入 Pillow 图像对象，用于不创建窗口的 410×502 正式 PNG 导出。
from PIL import Image, ImageDraw, ImageFont


# AMOLED 模拟内容宽度固定为厂家屏幕横向像素数，单位 px。
DISPLAY_WIDTH = 410
# AMOLED 模拟内容高度固定为厂家屏幕纵向像素数，单位 px。
DISPLAY_HEIGHT = 502
# 页面背景采用 LVGL renderer 的石墨黑，降低真实 AMOLED 发光面积。
COLOR_BACKGROUND = "#05070B"
# 主信息卡片采用低反射深蓝黑，和背景形成一级层级。
COLOR_CARD = "#0D1420"
# 抬升按钮和状态胶囊采用稍亮的蓝灰色。
COLOR_RAISED = "#151F2E"
# 主强调色采用系统级科技蓝，集中表示可操作和训练中状态。
COLOR_ACCENT = "#0A84FF"
# 次强调色采用青蓝色，只用于连接、充电和智能识别提示。
COLOR_CYAN = "#32D5FF"
# 成功状态使用高辨识度绿色。
COLOR_SUCCESS = "#30D158"
# 警告状态使用琥珀色。
COLOR_WARNING = "#FF9F0A"
# 次级文字采用冷灰蓝，降低与主指标的竞争。
COLOR_SECONDARY = "#A9B4C4"
# 主文字采用接近白色的浅灰，避免真实屏幕长期全白高亮。
COLOR_TEXT = "#F4F7FB"
# 次要文字采用设备端弱化灰色。
COLOR_MUTED = "#8996A9"
# 停止等危险操作使用系统红，和普通导航按钮区分。
COLOR_DANGER = "#FF453A"
# 分隔线和卡片描边使用低对比蓝灰色。
COLOR_BORDER = "#223047"
# 桌面窗口背景不属于 AMOLED 内容，仅用于区分模拟器控制区。
COLOR_DESKTOP = "#20252C"

# 正式 PNG 导出优先使用微软雅黑常规字重，保证中文教程截图清晰可读。
EXPORT_FONT_REGULAR_CANDIDATES: tuple[Path, ...] = (
    # Windows 10/11 默认微软雅黑字体集合。
    Path("C:/Windows/Fonts/msyh.ttc"),
    # 常见 Linux Noto CJK 简体字体路径。
    Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
    # macOS 苹方字体集合路径。
    Path("/System/Library/Fonts/PingFang.ttc"),
)
# 正式 PNG 导出优先使用匹配的粗体字体，避免标题由程序伪粗体导致锯齿。
EXPORT_FONT_BOLD_CANDIDATES: tuple[Path, ...] = (
    # Windows 10/11 默认微软雅黑粗体字体集合。
    Path("C:/Windows/Fonts/msyhbd.ttc"),
    # 常见 Linux Noto CJK 粗体字体路径。
    Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"),
    # macOS 苹方字体集合；Pillow 使用同一集合中的系统默认字重。
    Path("/System/Library/Fonts/PingFang.ttc"),
)


# 定义产品页面；列表严格覆盖生产 ui_state_t 的十三个可渲染界面状态。
class Page(Enum):
    """描述桌面预览器支持的设备页面。"""

    # 冷启动 Logo 与本地 AI 提示页。
    BOOT = "BOOT"
    # 显示、触摸、IMU 和存储自检页。
    SELF_TEST = "SELF_TEST"
    # 无活动会话的主页。
    HOME = "HOME"
    # 点击开始后立即采集并等待首个可信动作的识别页。
    PREPARE = "PREPARE"
    # 实时动作、次数和热量页。
    RUNNING = "RUNNING"
    # 暂停后冻结计数的页面。
    PAUSED = "PAUSED"
    # 停止训练的二次确认页。
    STOP_CONFIRM = "STOP_CONFIRM"
    # 会话本地保存后的总结页。
    SUMMARY = "SUMMARY"
    # 亮度、熄屏和诊断入口页。
    SETTINGS = "SETTINGS"
    # 质量位、BLE 和偏好修订号诊断页。
    DIAGNOSTICS = "DIAGNOSTICS"
    # AMOLED 逻辑熄屏页。
    SCREEN_OFF = "SCREEN_OFF"
    # 硬件或模型合同失败后的阻断训练页。
    ERROR = "ERROR"
    # 安全保存后等待 PMIC 断电的页面。
    SHUTDOWN = "SHUTDOWN"


# 桌面窗口标题使用“智能手表”产品术语；具体板型属于内部开发信息。
PREVIEW_WINDOW_TITLE = "智慧运动手表界面预览器"

# 把稳定内部页面枚举映射为用户可理解的中文页面名称；键集合必须覆盖全部十三页。
PAGE_DISPLAY_NAMES: dict[Page, str] = {
    # 冷启动页面显示产品开机动画。
    Page.BOOT: "开机动画",
    # 自检页面显示硬件检查进度。
    Page.SELF_TEST: "设备自检",
    # 主页提供开始、设置和关机入口。
    Page.HOME: "主页",
    # 识别页明确显示设备已开始采样，不再展示人为倒计时。
    Page.PREPARE: "正在识别",
    # 运行页显示实时动作与指标。
    Page.RUNNING: "训练中",
    # 暂停页冻结计数。
    Page.PAUSED: "训练暂停",
    # 停止确认页防止误触结束。
    Page.STOP_CONFIRM: "停止确认",
    # 总结页显示本次训练结果。
    Page.SUMMARY: "训练总结",
    # 设置页管理屏幕、诊断和绑定。
    Page.SETTINGS: "设备设置",
    # 诊断页显示质量码与运行状态。
    Page.DIAGNOSTICS: "设备诊断",
    # 熄屏页模拟面板完全关闭。
    Page.SCREEN_OFF: "屏幕已关闭",
    # 故障页显示稳定故障码。
    Page.ERROR: "设备故障",
    # 关机页显示保存和断电动画。
    Page.SHUTDOWN: "关机动画",
}

# 固定右侧演示按钮的纯中文标签；顺序与创建按钮的回调顺序一致。
CONTROL_BUTTON_LABELS: tuple[str, ...] = (
    # 第一项重新播放开机和自检动画。
    "重播开机动画",
    # 第二项模拟 AMOLED 熄屏。
    "模拟屏幕关闭",
    # 第三项模拟一次有效动作计数。
    "手动增加一次计数",
    # 第四项切换 11 类动作名称。
    "切换动作名称",
    # 第五项切换蓝牙连接状态。
    "切换蓝牙状态",
    # 第六项显示六位蓝牙配对码。
    "模拟蓝牙配对码",
    # 第七项减少演示电量。
    "电量减少 5%",
    # 第八项关闭桌面预览器。
    "关闭预览器",
)

# 桌面辅助区说明使用完整中文术语，明确它不属于 410×502 设备产品界面。
DESKTOP_SCOPE_TEXT = (
    # 第一行说明左侧画布用途。
    "左侧仅预览布局和流程。\n"
    # 第二行说明交互方式。
    "按钮请直接点击模拟屏。\n"
    # 第三行划清开发工具与设备产品界面的边界。
    "外部控制不属于设备界面。"
)

# 返回单个页面在桌面开发导航中的纯中文可见标签。
def development_page_label(page: Page) -> str:
    """把内部页面枚举转换为不含英文状态码的中文名称。"""

    # 直接索引固定映射；未知枚举应抛出 KeyError 暴露合同漂移。
    return PAGE_DISPLAY_NAMES[page]


# 固定开发导航顺序为 Page 枚举声明顺序，确保十三页列表与设备状态合同同步。
DEVELOPMENT_PAGE_ORDER: tuple[Page, ...] = tuple(Page)

# 定义一张教程截图的稳定文件名与页面状态，供批量导出和测试共享。
@dataclass(frozen=True)
class DocumentationPreviewVariant:
    """描述正式文档截图需要复现的单一手表状态。"""

    # 保存仓库内稳定 PNG 文件名；名称只使用小写 ASCII 和下划线。
    filename: str
    # 保存要渲染的设备业务页面。
    page: Page
    # true 表示 RUNNING 页面进入休息门，动作与累计保持不变。
    rest_preview: bool = False
    # true 表示在底层页面上覆盖六位蓝牙配对码。
    pairing_preview: bool = False
    # 保存教程中的中文用途说明，清单无需从内部枚举反推含义。
    description: str = ""


# 固定教程截图清单；增删页面时必须同步文档引用和单元测试。
DOCUMENTATION_PREVIEW_VARIANTS: tuple[DocumentationPreviewVariant, ...] = (
    # 主页展示品牌、电池、连接状态和三项主入口。
    DocumentationPreviewVariant("home.png", Page.HOME, description="主页、连接状态、电池图标和训练入口"),
    # 识别页展示点击开始后已立即采样的状态。
    DocumentationPreviewVariant("prepare.png", Page.PREPARE, description="点击开始后的即时采样与动作识别"),
    # 运行页展示稳定动作、实时次数和会话指标。
    DocumentationPreviewVariant("running.png", Page.RUNNING, description="稳定动作、实时次数、时长和热量"),
    # 休息页证明动作名称和累计不变，只暂停计数。
    DocumentationPreviewVariant(
        "running_rest.png",
        Page.RUNNING,
        rest_preview=True,
        description="休息门生效，动作名称和累计保持不变",
    ),
    # 设置页展示亮度、诊断、忘记电脑和返回入口。
    DocumentationPreviewVariant("settings.png", Page.SETTINGS, description="亮度、诊断、忘记电脑和返回入口"),
    # 配对覆盖层展示零填充六位码和超时提示。
    DocumentationPreviewVariant(
        "pairing.png",
        Page.HOME,
        pairing_preview=True,
        description="中文蓝牙配对码与超时提示",
    ),
    # 总结页展示动作、最终次数、时长和热量。
    DocumentationPreviewVariant("summary.png", Page.SUMMARY, description="会话动作、最终次数、时长和热量"),
    # 故障页展示稳定故障码、训练阻断和安全关机。
    DocumentationPreviewVariant("error.png", Page.ERROR, description="稳定故障码、训练阻断和安全关机"),
)


# 定义页面按钮命令；命名与设备端 ui_command_t 的产品意图一致。
class Command(Enum):
    """描述用户点击后触发的纯预览命令。"""

    # 从主页进入即时采样和动作识别。
    START = "START"
    # 取消 PREPARE，返回主页。
    CANCEL = "CANCEL"
    # 暂停训练。
    PAUSE = "PAUSE"
    # 恢复训练。
    RESUME = "RESUME"
    # 首次停止点击，只打开确认页。
    STOP = "STOP"
    # 在确认页真正结束训练。
    CONFIRM_STOP = "CONFIRM_STOP"
    # 关闭总结并返回主页。
    DONE = "DONE"
    # 从主页进入设置页。
    OPEN_SETTINGS = "OPEN_SETTINGS"
    # 从设置页进入诊断页。
    OPEN_DIAGNOSTICS = "OPEN_DIAGNOSTICS"
    # 返回上一级或取消停止确认。
    BACK = "BACK"
    # 循环 AMOLED 用户亮度。
    CYCLE_BRIGHTNESS = "CYCLE_BRIGHTNESS"
    # 循环自动熄屏秒数。
    CYCLE_TIMEOUT = "CYCLE_TIMEOUT"
    # 在设置页删除设备端保存的电脑绑定，并清除当前连接演示状态。
    FORGET_COMPUTER = "FORGET_COMPUTER"
    # 从逻辑熄屏恢复此前页面。
    WAKE = "WAKE"
    # 进入安全关机动画。
    SHUTDOWN = "SHUTDOWN"


# 定义单个屏内按钮的数据模型。
@dataclass(frozen=True)
class ButtonSpec:
    """保存按钮文字、命令和危险操作标记。"""

    # 保存屏幕上实际显示的中文标签。
    label: str
    # 保存点击后交给状态机的命令。
    command: Command
    # true 表示使用红色危险操作样式。
    dangerous: bool = False


# 定义 presenter 输出的纯页面模型。
@dataclass(frozen=True)
class PageSpec:
    """保存一次渲染所需的全部文字和按钮。"""

    # 保存页面标题。
    title: str = ""
    # 保存页面最醒目的动作、识别状态或主指标。
    primary: str = ""
    # 保存次数、卡路里或诊断摘要。
    secondary: str = ""
    # 保存电池与 BLE 顶栏状态。
    status: str = ""
    # 保存页面底部提示。
    footer: str = ""
    # 保存零到五个可点击按钮；设置页第五项用于忘记电脑。
    buttons: tuple[ButtonSpec, ...] = ()


# 定义完整预览运行状态；所有字段均为桌面演示值，不写入设备配置。
@dataclass
class PreviewState:
    """保存页面、训练指标、设置和恢复点。"""

    # 当前页面冷启动固定为 BOOT。
    page: Page = Page.BOOT
    # 熄屏前页面默认指向 HOME，避免无效恢复目标。
    screen_resume_page: Page = Page.HOME
    # 停止确认前页面默认指向 RUNNING。
    stop_resume_page: Page = Page.RUNNING
    # 当前稳定动作显示名；预览默认使用最难弱类“跳跃深蹲”。
    action_name: str = "跳跃深蹲"
    # 保存动作列表索引，供桌面控制面板切换动作。
    action_index: int = 3
    # 保存当前动作次数或步数。
    count: int = 0
    # 保存累计热量，单位 kcal。
    calories_kcal: float = 0.0
    # 保存排除暂停后的训练秒数。
    elapsed_seconds: int = 0
    # 保存模型置信度百分比，范围 0～100。
    confidence_percent: float = 92.34
    # 保存厂家原配电池百分比演示值。
    battery_percent: int = 82
    # 保存 BLE 是否连接 PC。
    ble_connected: bool = True
    # 标记是否以覆盖层显示 Display Only 六位配对码。
    pairing_active: bool = False
    # 保存 0～999999 的演示配对码；显示时必须补足六位前导零。
    pairing_code: int = 123456
    # 保存是否处于充电状态。
    charging: bool = False
    # 保存 AMOLED 用户亮度，范围来自正式设置循环值。
    brightness_percent: int = 35
    # 保存自动熄屏门槛，单位秒。
    screen_timeout_seconds: int = 30
    # 保存偏好修订号；每次设置变化递增。
    preferences_revision: int = 1
    # 保存运行期数据质量位图。
    data_quality_flags: int = 0x0021
    # 保存开发预览错误页的示例故障码；仅用于检查负号和数字布局。
    fault_code: int = -42
    # 保存动画点数量，范围 0～3。
    animation_phase: int = 0
    # 保存实时动作门是否允许计数；false 表示用户休息或窗口不确定。
    counting_enabled: bool = True
    # 保存点击区域；每项由坐标和命令组成，渲染时重建。
    click_regions: list[tuple[int, int, int, int, Command]] = field(default_factory=list)

    # 定义 11 类动作的设备端固定显示顺序。
    ACTION_NAMES = (
        "早安式体前屈",
        "开合跳",
        "跳跃弓步",
        "跳跃深蹲",
        "弓步",
        "静坐",
        "深蹲",
        "小跑",
        "收腹跳",
        "行走",
        "挥手",
    )
    # 定义设置页亮度循环，与当前 ui_presenter.h 注释保持一致。
    BRIGHTNESS_LEVELS = (15, 35, 60, 100)
    # 定义诊断页熄屏时间循环，与当前 ui_presenter.h 注释保持一致。
    TIMEOUT_LEVELS = (15, 30, 60, 120)

    def status_text(self) -> str:
        """返回设备端同语义的电池与 BLE 顶栏文字。"""

        # 充电时在电量数字后追加中文“充”，与生产 presenter 同步。
        charging_suffix = " 充" if self.charging else ""
        # BLE 布尔值转换为设备端中文连接文本。
        ble_text = "已连" if self.ble_connected else "未连"
        # 返回固定格式；电池单位为百分比。
        return f"电量 {self.battery_percent}%{charging_suffix}  蓝牙 {ble_text}"

    def cycle_brightness(self) -> None:
        """切换到下一个亮度并递增偏好修订号。"""

        # 查找当前亮度在固定循环表中的索引。
        current_index = self.BRIGHTNESS_LEVELS.index(self.brightness_percent)
        # 使用取模回到首项，确保不会越界。
        next_index = (current_index + 1) % len(self.BRIGHTNESS_LEVELS)
        # 提交新的亮度百分比。
        self.brightness_percent = self.BRIGHTNESS_LEVELS[next_index]
        # 模拟 NVS 成功保存后修订号递增。
        self.preferences_revision += 1

    def cycle_timeout(self) -> None:
        """切换到下一个熄屏门槛并递增偏好修订号。"""

        # 查找当前秒数在固定循环表中的索引。
        current_index = self.TIMEOUT_LEVELS.index(self.screen_timeout_seconds)
        # 使用取模计算下一档。
        next_index = (current_index + 1) % len(self.TIMEOUT_LEVELS)
        # 提交新的熄屏秒数。
        self.screen_timeout_seconds = self.TIMEOUT_LEVELS[next_index]
        # 模拟配置持久化成功。
        self.preferences_revision += 1

    def cycle_action(self) -> None:
        """切换到下一个动作，仅用于桌面布局检查。"""

        # 计算下一个固定类别索引。
        self.action_index = (self.action_index + 1) % len(self.ACTION_NAMES)
        # 同步当前动作显示名。
        self.action_name = self.ACTION_NAMES[self.action_index]

    def enter_screen_off(self) -> None:
        """保存恢复页并进入逻辑熄屏。"""

        # 只有尚未熄屏时才覆盖恢复点。
        if self.page is not Page.SCREEN_OFF:
            # 保存精确当前页面。
            self.screen_resume_page = self.page
        # 切换到黑屏页。
        self.page = Page.SCREEN_OFF

    def dispatch(self, command: Command) -> str:
        """处理一次按钮命令并返回需要执行的动画副作用名称。"""

        # START 只允许从主页开始新演示会话。
        if command is Command.START and self.page is Page.HOME:
            # 清零旧次数。
            self.count = 0
            # 清零旧热量。
            self.calories_kcal = 0.0
            # 清零旧训练时长。
            self.elapsed_seconds = 0
            # 进入即时采样和首动作识别页。
            self.page = Page.PREPARE
            # 请求调用方演示首个可信窗口完成后的页面切换；采样已经立即开始。
            return "start_identification"
        # PREPARE 的 CANCEL 直接返回主页，不生成摘要。
        if command is Command.CANCEL and self.page is Page.PREPARE:
            # 结束未开始的会话。
            self.page = Page.HOME
            # 返回无额外副作用。
            return "none"
        # RUNNING 的 PAUSE 冻结自动计数。
        if command is Command.PAUSE and self.page is Page.RUNNING:
            # 进入暂停页。
            self.page = Page.PAUSED
            # 返回无额外副作用。
            return "none"
        # PAUSED 的 RESUME 恢复自动计数。
        if command is Command.RESUME and self.page is Page.PAUSED:
            # 返回训练页。
            self.page = Page.RUNNING
            # 请求调用方重新安排计数定时器。
            return "start_running"
        # 首次 STOP 只保存恢复点并打开确认页。
        if command is Command.STOP and self.page in (Page.RUNNING, Page.PAUSED):
            # 保存取消确认时应回到的精确页面。
            self.stop_resume_page = self.page
            # 进入停止确认页。
            self.page = Page.STOP_CONFIRM
            # 返回无额外副作用。
            return "none"
        # 确认页的 BACK 取消停止并恢复原页面。
        if command is Command.BACK and self.page is Page.STOP_CONFIRM:
            # 恢复 RUNNING 或 PAUSED。
            self.page = self.stop_resume_page
            # 只有 RUNNING 需要重新启动自动计数。
            return "start_running" if self.page is Page.RUNNING else "none"
        # CONFIRM_STOP 才进入总结页。
        if command is Command.CONFIRM_STOP and self.page is Page.STOP_CONFIRM:
            # 提交总结页面。
            self.page = Page.SUMMARY
            # 返回无额外副作用。
            return "none"
        # SUMMARY 的 DONE 返回主页。
        if command is Command.DONE and self.page is Page.SUMMARY:
            # 进入主页。
            self.page = Page.HOME
            # 返回无额外副作用。
            return "none"
        # HOME 的 SETTINGS 进入设置页。
        if command is Command.OPEN_SETTINGS and self.page is Page.HOME:
            # 进入设置页。
            self.page = Page.SETTINGS
            # 返回无额外副作用。
            return "none"
        # SETTINGS 的 DIAG 进入诊断页。
        if command is Command.OPEN_DIAGNOSTICS and self.page is Page.SETTINGS:
            # 进入诊断页。
            self.page = Page.DIAGNOSTICS
            # 返回无额外副作用。
            return "none"
        # SETTINGS 的 BACK 返回主页。
        if command is Command.BACK and self.page is Page.SETTINGS:
            # 进入主页。
            self.page = Page.HOME
            # 返回无额外副作用。
            return "none"
        # DIAGNOSTICS 的 BACK 返回设置页。
        if command is Command.BACK and self.page is Page.DIAGNOSTICS:
            # 进入设置页。
            self.page = Page.SETTINGS
            # 返回无额外副作用。
            return "none"
        # 设置页亮度按钮循环真实产品档位。
        if command is Command.CYCLE_BRIGHTNESS and self.page is Page.SETTINGS:
            # 更新亮度演示值。
            self.cycle_brightness()
            # 返回无额外副作用。
            return "none"
        # 设置页“忘记电脑”清除绑定演示事实，不改变亮度、熄屏或训练指标。
        if command is Command.FORGET_COMPUTER and self.page is Page.SETTINGS:
            # 清除安全连接状态，模拟设备删除 NimBLE 绑定并断开当前电脑。
            self.ble_connected = False
            # 清除可能仍显示的配对码，防止敏感码在解绑后残留。
            self.pairing_active = False
            # 返回无动画副作用。
            return "none"
        # 诊断页 SCREEN 按钮循环熄屏门槛。
        if command is Command.CYCLE_TIMEOUT and self.page is Page.DIAGNOSTICS:
            # 更新熄屏秒数。
            self.cycle_timeout()
            # 返回无额外副作用。
            return "none"
        # 熄屏页任意有效触摸恢复此前页面。
        if command is Command.WAKE and self.page is Page.SCREEN_OFF:
            # 恢复准确页面。
            self.page = self.screen_resume_page
            # 恢复 RUNNING 时重新启动自动计数。
            return "start_running" if self.page is Page.RUNNING else "none"
        # 主页、设置或故障页的关机命令进入安全关机动画。
        if command is Command.SHUTDOWN and self.page in (Page.HOME, Page.SETTINGS, Page.ERROR):
            # 切换关机页。
            self.page = Page.SHUTDOWN
            # 请求调用方播放关机动画后熄屏。
            return "shutdown"
        # 当前页面不接受该命令时安全忽略。
        return "none"

    def increment_count(self) -> None:
        """在 RUNNING 且计数门开启时增加一次权威指标。"""

        # 非训练页或休息门关闭时不得增长权威指标。
        if (self.page is not Page.RUNNING) or (not self.counting_enabled):
            # 直接返回；次数、热量和活动时长均保持不变。
            return
        # 增加一次动作次数。
        self.count += 1
        # 每次演示增加约 0.118 kcal；该数值仅用于布局变化，不是热量算法。
        self.calories_kcal += 0.118
        # 每次演示计数推进一秒活动时长。
        self.elapsed_seconds += 1
        # 使用小范围周期变化模拟模型置信度刷新。
        self.confidence_percent = 90.0 + float((self.count * 7) % 91) / 10.0
        # 函数只更新预览指标，不产生不存在的硬件反馈。
        return


# 定义不依赖 Tk 的开发直达函数，使无窗口冒烟测试也能覆盖十三页导航合同。
def select_page_for_development(state: PreviewState, page: Page) -> None:
    """把纯状态切到指定页面，不启动识别、动画或自动计数。"""

    # 写入开发者明确选择的页面；该操作只用于桌面布局检查。
    state.page = page
    # 开发直达运行页默认开启计数门，避免旧休息状态污染布局检查。
    state.counting_enabled = True
    # STOP_CONFIRM 直达时默认从 RUNNING 返回，保证 CANCEL 的演示结果可预测。
    if page is Page.STOP_CONFIRM:
        # 保存取消停止时应恢复的训练页。
        state.stop_resume_page = Page.RUNNING
    # SCREEN_OFF 直达时默认唤醒到 HOME，避免恢复到关机或黑屏形成循环。
    if page is Page.SCREEN_OFF:
        # 保存黑屏点击后的安全恢复页。
        state.screen_resume_page = Page.HOME


# 为静态截图写入统一演示数据，避免 Tk 预览和 PNG 导出出现两套示例值。
def configure_state_for_capture(
    state: PreviewState,
    page: Page,
    rest_preview: bool = False,
    pairing_preview: bool = False,
) -> None:
    """把状态配置成可重复导出的教程截图场景。"""

    # 通过统一开发直达逻辑切换页面并重置页面相关恢复点。
    select_page_for_development(state, page)
    # 每次配置先明确覆盖层开关，避免同一状态对象复用时残留旧配对码。
    state.pairing_active = pairing_preview
    # 教程配对码固定为 012345，用于验证前导零和六位排版。
    state.pairing_code = 12345
    # 教程截图固定使用 82% 电量，便于比较所有页面的状态栏一致性。
    state.battery_percent = 82
    # 教程截图默认展示已连接状态，错误页也能说明业务故障与 BLE 状态相互独立。
    state.ble_connected = True
    # 训练类页面使用同一组权威指标，确保页面切换前后数字可直接比较。
    if page in (Page.RUNNING, Page.PAUSED, Page.STOP_CONFIRM, Page.SUMMARY):
        # 权威累计固定为 7 次，避免把演示值误解为目标次数。
        state.count = 7
        # 活动时长固定为 18 秒，单位 s。
        state.elapsed_seconds = 18
        # 热量固定为 0.826 kcal，只服务界面排版演示。
        state.calories_kcal = 0.826
        # 分类置信度固定为 94%，用于检查运行页脚信息密度。
        state.confidence_percent = 94.0
    # 只有 RUNNING 页面接受休息变体；其它页面传入该开关也不得改变业务语义。
    state.counting_enabled = not (rest_preview and page is Page.RUNNING)


# 根据动态状态生成与设备 presenter 对齐的页面模型。
def build_page_spec(state: PreviewState) -> PageSpec:
    """把预览状态转换为文字和按钮，不创建任何 Tk 对象。"""

    # 除 SCREEN_OFF 外，所有页面显示统一电池/BLE 顶栏。
    status = "" if state.page is Page.SCREEN_OFF else state.status_text()
    # 活动配对码覆盖当前业务页，与生产 presenter 的全局覆盖层语义一致。
    if state.pairing_active and state.page is not Page.SCREEN_OFF:
        # 六位格式必须保留前导零，且覆盖层不提供按钮，避免用户误触训练控制。
        return PageSpec(
            "蓝牙配对",
            f"{state.pairing_code:06d}",
            "请在电脑上输入",
            status,
            "60秒后失效",
        )
    # BOOT 页面使用动画点模拟轻量淡入过程。
    if state.page is Page.BOOT:
        # 生成 1～3 个循环点，避免全屏动画。
        dots = "." * ((state.animation_phase % 3) + 1)
        # 返回开机页面。
        return PageSpec("系统启动", f"智慧运动助手{dots}", "正在准备设备", status, "请稍候")
    # SELF_TEST 页面列出设备端同样的四项检查。
    if state.page is Page.SELF_TEST:
        # 返回自检页面。
        return PageSpec("设备自检", "正在检查硬件", "屏幕  触摸  惯导  存储", status, "请稍候")
    # HOME 页面提供开始、设置和关机入口。
    if state.page is Page.HOME:
        # 返回主页。
        return PageSpec(
            "训练中心",
            "准备开始训练",
            "右手佩戴 / 自动识别",
            status,
            "点击开始  自动识别动作",
            (
                ButtonSpec("开始", Command.START),
                ButtonSpec("设置", Command.OPEN_SETTINGS),
                ButtonSpec("关机", Command.SHUTDOWN, True),
            ),
        )
    # PREPARE 页面明确表示采样已经开始，并等待首个可信分类窗口。
    if state.page is Page.PREPARE:
        # 返回准备页。
        return PageSpec(
            "动作识别",
            "开始做动作",
            "正在建立动作模型",
            status,
            "右手佩戴  识别后自动记录",
            (ButtonSpec("取消", Command.CANCEL),),
        )
    # RUNNING 页面显示动作、次数、热量、时间和置信度。
    if state.page is Page.RUNNING:
        # 根据实时动作门生成计数或休息状态标题。
        title = "计数中" if state.counting_enabled else "休息  计数暂停"
        # 正常计数显示时间、热量和置信度；休息时显示恢复条件。
        footer = (
            f"{state.elapsed_seconds // 60:02d}:{state.elapsed_seconds % 60:02d}  "
            f"{state.calories_kcal:.3f}千卡  置信度{int(state.confidence_percent)}%"
            if state.counting_enabled
            else "累计保持  恢复完整动作后继续"
        )
        # 返回训练页。
        return PageSpec(
            title,
            state.action_name,
            f"{state.count} 次",
            status,
            footer,
            (
                ButtonSpec("暂停", Command.PAUSE),
                ButtonSpec("停止", Command.STOP, True),
            ),
        )
    # PAUSED 页面保留停止前指标。
    if state.page is Page.PAUSED:
        # 返回暂停页。
        return PageSpec(
            "训练暂停",
            state.action_name,
            f"{state.count} 次",
            status,
            f"{state.elapsed_seconds // 60:02d}:{state.elapsed_seconds % 60:02d}  "
            f"{state.calories_kcal:.3f}千卡  累计保持",
            (
                ButtonSpec("继续", Command.RESUME),
                ButtonSpec("停止", Command.STOP, True),
            ),
        )
    # STOP_CONFIRM 页面必须保留指标且提供确认与取消。
    if state.page is Page.STOP_CONFIRM:
        # 返回停止确认页。
        return PageSpec(
            "停止训练？",
            state.action_name,
            f"{state.count} 次",
            status,
            f"{state.calories_kcal:.3f}千卡  确认后保存记录",
            (
                ButtonSpec("停止", Command.CONFIRM_STOP, True),
                ButtonSpec("取消", Command.BACK),
            ),
        )
    # SUMMARY 页面显示最终总数、热量和时长。
    if state.page is Page.SUMMARY:
        # 返回总结页。
        return PageSpec(
            "训练完成",
            state.action_name,
            f"{state.count} 次",
            status,
            f"{state.elapsed_seconds // 60:02d}:{state.elapsed_seconds % 60:02d}  "
            f"{state.calories_kcal:.3f}千卡  已保存",
            (ButtonSpec("完成", Command.DONE),),
        )
    # SETTINGS 页面显示动态亮度和熄屏门槛，不提供不存在的振动功能。
    if state.page is Page.SETTINGS:
        # 返回设置页。
        return PageSpec(
            "设备设置",
            f"{state.brightness_percent}% 亮度",
            f"{state.screen_timeout_seconds}秒 自动熄屏",
            status,
            "配置已本地保存  原配400毫安时",
            (
                ButtonSpec("亮度", Command.CYCLE_BRIGHTNESS),
                ButtonSpec("诊断", Command.OPEN_DIAGNOSTICS),
                ButtonSpec("忘记电脑", Command.FORGET_COMPUTER, True),
                ButtonSpec("返回", Command.BACK),
            ),
        )
    # DIAGNOSTICS 页面显示质量位、连接事实和修订号。
    if state.page is Page.DIAGNOSTICS:
        # 把 BLE 布尔值转换为中文连接状态。
        ble_text = "已连" if state.ble_connected else "未连"
        # 返回诊断页。
        return PageSpec(
            "设备诊断",
            "开机检查通过",
            f"质量码 {state.data_quality_flags:04X}  蓝牙 {ble_text}  {state.screen_timeout_seconds}秒",
            status,
            f"设置版本 {state.preferences_revision}",
            (
                ButtonSpec("熄屏", Command.CYCLE_TIMEOUT),
                ButtonSpec("返回", Command.BACK),
            ),
        )
    # SCREEN_OFF 页面保持全黑，点击区域由窗口级事件处理。
    if state.page is Page.SCREEN_OFF:
        # 返回空页面。
        return PageSpec()
    # ERROR 页面显示示例故障码、训练阻断事实和安全关机入口。
    if state.page is Page.ERROR:
        # 返回与生产 ERROR 状态一致的中文错误页。
        return PageSpec(
            "设备故障",
            f"故障码 {state.fault_code}",
            "重启后打开设备诊断",
            status,
            "训练已禁用",
            (ButtonSpec("关机", Command.SHUTDOWN, True),),
        )
    # SHUTDOWN 页面播放短点动画并禁止按钮输入。
    if state.page is Page.SHUTDOWN:
        # 生成 1～3 个循环点。
        dots = "." * ((state.animation_phase % 3) + 1)
        # 返回关机页。
        return PageSpec("安全关机", "训练记录已保存", f"正在关机{dots}", status, "下次训练见")
    # 理论上枚举已完整覆盖；异常状态直接抛错暴露编程错误。
    raise ValueError(f"unsupported page: {state.page}")


# 选择当前平台可用的中文字体文件，缺失时明确阻止生成乱码截图。
def _select_export_font_path(bold: bool) -> Path:
    """返回常规或粗体中文字体的绝对路径。"""

    # 根据字重选择固定候选表，保持常规正文和粗体标题的视觉层级。
    candidates = EXPORT_FONT_BOLD_CANDIDATES if bold else EXPORT_FONT_REGULAR_CANDIDATES
    # 依次检查 Windows、Linux 和 macOS 的常见系统字体位置。
    for candidate in candidates:
        # 只接受真实普通文件，目录或失效链接不能交给 Pillow。
        if candidate.is_file():
            # 返回第一个可复现中文字符的字体文件。
            return candidate
    # 没有中文字体时拒绝回退到不含中文字形的 Pillow 默认字体。
    raise RuntimeError(
        "无法导出手表 PNG：未找到微软雅黑、Noto Sans CJK 或苹方中文字体。"
    )


# 按像素字号加载中文字体；Pillow 使用 px，不复用依赖 Tk 显示缩放的字体对象。
def _load_export_font(size_px: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    """加载适用于正式 PNG 的确定性中文 TrueType 字体。"""

    # 字号必须为正整数，防止错误参数在 Pillow 内部产生难懂异常。
    if size_px <= 0:
        # 主动抛出包含业务含义的参数错误。
        raise ValueError("PNG 字体像素高度必须大于零")
    # 选择当前平台存在的明确中文字体路径。
    font_path = _select_export_font_path(bold)
    # 使用 Pillow FreeType 加载器创建固定像素字号字体。
    return ImageFont.truetype(str(font_path), size=size_px)


# 根据可用宽度缩小单行文字，保证长动作名和设置说明不会越出卡片安全区。
def _fit_export_font(
    draw: ImageDraw.ImageDraw,
    text: str,
    maximum_width_px: int,
    preferred_size_px: int,
    minimum_size_px: int,
    bold: bool = False,
) -> ImageFont.FreeTypeFont:
    """返回能在指定宽度内完整显示文字的最大中文字体。"""

    # 从首选字号向下枚举，优先保留成熟产品界面的强层级。
    for size_px in range(preferred_size_px, minimum_size_px - 1, -1):
        # 加载当前候选字号。
        font = _load_export_font(size_px, bold)
        # 读取单行文字边界；返回值单位为像素。
        left, _top, right, _bottom = draw.textbbox((0, 0), text, font=font)
        # 实际文字宽度不得超过调用方提供的安全区。
        if (right - left) <= maximum_width_px:
            # 返回当前最大可用字号。
            return font
    # 极端长文字使用最小字号；调用方仍可通过文档截图发现内容设计问题。
    return _load_export_font(minimum_size_px, bold)


# 在指定中心点绘制一行自动缩放的中文文字，统一所有页面的对齐规则。
def _draw_export_centered_text(
    draw: ImageDraw.ImageDraw,
    center_x_px: int,
    center_y_px: int,
    text: str,
    color: str,
    maximum_width_px: int,
    preferred_size_px: int,
    minimum_size_px: int,
    bold: bool = False,
) -> None:
    """绘制水平和垂直居中的单行文字。"""

    # 根据文字长度选择当前安全区内的最大字体。
    font = _fit_export_font(
        draw,
        text,
        maximum_width_px,
        preferred_size_px,
        minimum_size_px,
        bold,
    )
    # 使用 mm 锚点把文本几何中心固定在给定像素位置。
    draw.text(
        (center_x_px, center_y_px),
        text,
        fill=color,
        font=font,
        anchor="mm",
    )


# 使用与 Tk 预览相同的触摸区域绘制正式 PNG 按钮。
def _draw_export_buttons(
    draw: ImageDraw.ImageDraw,
    buttons: tuple[ButtonSpec, ...],
) -> None:
    """把零到四个页面按钮绘制到固定底部区域。"""

    # 无按钮页面保持底部留白，不生成假操作入口。
    if not buttons:
        # 提前结束，避免创建空行布局。
        return
    # 四项设置入口使用固定二乘二网格。
    if len(buttons) == 4:
        # 每行保存两个按钮，与生产设置页触摸布局一致。
        rows = (buttons[:2], buttons[2:])
        # 两行按钮分别占用 354～414 和 424～484 像素。
        vertical_bounds = ((354, 414), (424, 484))
    else:
        # 其它页面把全部操作放在单行。
        rows = (buttons,)
        # 单行按钮占用 396～466 像素，保持 70 px 触摸高度。
        vertical_bounds = ((396, 466),)
    # 按钮组左边界固定为 32 px。
    left_px = 32
    # 按钮组右边界固定为 378 px。
    right_px = DISPLAY_WIDTH - 32
    # 同行按钮间保留 10 px 可视间隔。
    gap_px = 10
    # 逐行绘制，行数最多为二。
    for row_index, row_buttons in enumerate(rows):
        # 根据行内按钮数量计算等宽按钮，单位 px。
        button_width_px = (
            right_px - left_px - gap_px * (len(row_buttons) - 1)
        ) / len(row_buttons)
        # 读取当前行纵向边界。
        top_px, bottom_px = vertical_bounds[row_index]
        # 逐个绘制当前行的按钮底板和文字。
        for button_index, button in enumerate(row_buttons):
            # 计算当前按钮左边界。
            x1_px = int(left_px + button_index * (button_width_px + gap_px))
            # 计算当前按钮右边界。
            x2_px = int(x1_px + button_width_px)
            # 危险操作用暗红，普通首按钮用科技蓝，其它入口用抬升蓝灰。
            fill_color = (
                "#451B22"
                if button.dangerous
                else COLOR_ACCENT
                if button_index == 0
                else COLOR_RAISED
            )
            # 危险操作保留红色描边，普通按钮与填充同色。
            outline_color = COLOR_DANGER if button.dangerous else fill_color
            # 绘制 18 px 圆角按钮底板。
            draw.rounded_rectangle(
                (x1_px, top_px, x2_px, bottom_px),
                radius=18,
                fill=fill_color,
                outline=outline_color,
                width=1,
            )
            # 在按钮几何中心绘制自动缩放的中文标签。
            _draw_export_centered_text(
                draw,
                (x1_px + x2_px) // 2,
                (top_px + bottom_px) // 2,
                button.label,
                COLOR_TEXT,
                max(40, x2_px - x1_px - 8),
                18,
                13,
                bold=True,
            )


# 把纯预览状态离屏渲染为精确 410×502 RGB 图像，不创建 Tk 窗口。
def render_preview_state_to_image(state: PreviewState) -> Image.Image:
    """返回与桌面画布同语义的正式手表页面图像。"""

    # 创建固定尺寸 RGB 画布；背景色与 AMOLED 产品页面一致。
    image = Image.new("RGB", (DISPLAY_WIDTH, DISPLAY_HEIGHT), COLOR_BACKGROUND)
    # 创建 Pillow 绘图上下文，所有操作只写入内存图像。
    draw = ImageDraw.Draw(image)
    # 熄屏页必须为完整纯黑图像，不残留状态栏或按钮。
    if state.page is Page.SCREEN_OFF:
        # 用纯黑覆盖整个 410×502 区域。
        draw.rectangle((0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1), fill="#000000")
        # 返回无任何可见元素的熄屏图像。
        return image
    # 使用同一 presenter 生成页面文字和按钮，避免离屏渲染复制业务状态判断。
    spec = build_page_spec(state)
    # 绘制左上角科技蓝品牌方标。
    draw.rounded_rectangle((32, 28, 70, 66), radius=12, fill=COLOR_ACCENT)
    # 在品牌标中绘制白色“毕”字。
    _draw_export_centered_text(draw, 51, 47, "毕", "#FFFFFF", 30, 22, 18, bold=True)
    # 加载品牌标题字体。
    brand_font = _load_export_font(19, bold=True)
    # 绘制“毕昇杯”主品牌。
    draw.text((80, 28), "毕昇杯", fill=COLOR_TEXT, font=brand_font, anchor="la")
    # 加载品牌副标题字体。
    brand_subtitle_font = _load_export_font(13)
    # 绘制“智慧运动助手”产品主题。
    draw.text(
        (80, 51),
        "智慧运动助手",
        fill=COLOR_SECONDARY,
        font=brand_subtitle_font,
        anchor="la",
    )
    # 根据充电和电量区间选择电池语义色。
    battery_color = (
        COLOR_CYAN
        if state.charging
        else COLOR_SUCCESS
        if state.battery_percent >= 40
        else COLOR_WARNING
        if state.battery_percent >= 20
        else COLOR_DANGER
    )
    # 电量文字使用小号粗体，和电池图标形成一个整体状态组件。
    battery_text_font = _load_export_font(13, bold=True)
    # 绘制电量数字；充电状态附加中文“充”。
    draw.text(
        (307, 31),
        f"{'充 ' if state.charging else ''}{state.battery_percent}%",
        fill=battery_color,
        font=battery_text_font,
        anchor="ra",
    )
    # 绘制电池外框。
    draw.rounded_rectangle(
        (312, 31, 356, 51),
        radius=5,
        fill=COLOR_BACKGROUND,
        outline=COLOR_SECONDARY,
        width=2,
    )
    # 绘制电池正极极柱。
    draw.rounded_rectangle((358, 37, 362, 45), radius=2, fill=COLOR_SECONDARY)
    # 把 0～100% 电量线性映射到 36 px 内部宽度，并保留至少 2 px 可见值。
    battery_fill_width_px = max(
        2,
        int(36 * max(0, min(100, state.battery_percent)) / 100),
    )
    # 绘制动态电池填充。
    draw.rounded_rectangle(
        (316, 35, 316 + battery_fill_width_px, 47),
        radius=3,
        fill=battery_color,
    )
    # 已连接使用青色状态点，未连接使用弱化灰色。
    ble_color = COLOR_CYAN if state.ble_connected else COLOR_MUTED
    # 绘制 BLE 状态点。
    draw.ellipse((281, 63, 289, 71), fill=ble_color)
    # 绘制连接状态短文字。
    draw.text(
        (295, 67),
        "已连接" if state.ble_connected else "未连接",
        fill=COLOR_SECONDARY,
        font=battery_text_font,
        anchor="lm",
    )
    # 绘制主信息卡片。
    draw.rounded_rectangle(
        (32, 94, 378, 344),
        radius=28,
        fill=COLOR_CARD,
        outline=COLOR_BORDER,
        width=1,
    )
    # 根据故障、休息、运行和普通页面选择唯一语义强调色。
    semantic_color = (
        COLOR_DANGER
        if state.page is Page.ERROR
        else COLOR_WARNING
        if (state.page is Page.PAUSED)
        or (state.page is Page.RUNNING and not state.counting_enabled)
        else COLOR_SUCCESS
        if state.page is Page.RUNNING
        else COLOR_ACCENT
    )
    # 绘制状态胶囊底板。
    draw.rounded_rectangle((50, 112, 204, 144), radius=16, fill=COLOR_RAISED)
    # 绘制胶囊左侧语义状态点。
    draw.ellipse((62, 123, 72, 133), fill=semantic_color)
    # 绘制页面状态标题。
    _draw_export_centered_text(
        draw,
        138,
        128,
        spec.title,
        COLOR_TEXT,
        116,
        17,
        13,
        bold=True,
    )
    # 绘制动作名或当前页面主结果。
    _draw_export_centered_text(
        draw,
        DISPLAY_WIDTH // 2,
        190,
        spec.primary,
        semantic_color,
        306,
        34,
        20,
        bold=True,
    )
    # 绘制权威次数或页面次级信息。
    _draw_export_centered_text(
        draw,
        DISPLAY_WIDTH // 2,
        248,
        spec.secondary,
        COLOR_TEXT,
        306,
        29,
        16,
        bold=True,
    )
    # 绘制计时、热量、置信度或恢复条件。
    _draw_export_centered_text(
        draw,
        DISPLAY_WIDTH // 2,
        298,
        spec.footer,
        COLOR_SECONDARY,
        306,
        15,
        11,
    )
    # 绘制会话节奏进度底轨。
    draw.rounded_rectangle((52, 322, 358, 327), radius=3, fill=COLOR_BORDER)
    # 训练相关页面按十二次视觉尺度显示进度，动画页按相位，其它页显示稳定小比例。
    progress_ratio = (
        min(1.0, state.count / 12.0)
        if state.page in (Page.RUNNING, Page.PAUSED, Page.STOP_CONFIRM, Page.SUMMARY)
        else ((state.animation_phase % 4) + 1) / 4.0
        if state.page in (Page.BOOT, Page.SHUTDOWN)
        else 0.22
    )
    # 把比例换算为 10～306 px 的可见进度宽度。
    progress_width_px = max(10, int(306 * progress_ratio))
    # 绘制当前语义色进度条。
    draw.rounded_rectangle(
        (52, 322, 52 + progress_width_px, 327),
        radius=3,
        fill=semantic_color,
    )
    # 绘制当前页面按钮。
    _draw_export_buttons(draw, spec.buttons)
    # 返回完整内存图像；调用方决定是否保存到正式仓库路径。
    return image


# 创建全新状态并渲染单个页面变体，避免调用方复用状态导致截图污染。
def render_watch_preview_image(
    page: Page,
    rest_preview: bool = False,
    pairing_preview: bool = False,
) -> Image.Image:
    """返回指定页面及休息/配对变体的 410×502 图像。"""

    # 每张图使用独立纯状态，保证导出结果与调用顺序无关。
    state = PreviewState()
    # 写入稳定教程指标和页面变体。
    configure_state_for_capture(state, page, rest_preview, pairing_preview)
    # 调用无窗口 Pillow 渲染器生成图像。
    return render_preview_state_to_image(state)


# 把单个页面变体写入明确 PNG 文件，供 CLI、测试和文档流水线共用。
def export_watch_preview_png(
    output_path: Path,
    page: Page,
    rest_preview: bool = False,
    pairing_preview: bool = False,
) -> Path:
    """导出一张无窗口、无设备依赖的正式 PNG。"""

    # 把调用方路径规范化为 Path 对象。
    resolved_output_path = Path(output_path)
    # 创建正式目标的父目录；已存在时保持原目录内容。
    resolved_output_path.parent.mkdir(parents=True, exist_ok=True)
    # 生成指定页面的独立 RGB 图像。
    image = render_watch_preview_image(page, rest_preview, pairing_preview)
    # 以固定 PNG 压缩级别保存；PNG 不写入时间戳，便于同环境重复生成。
    image.save(
        resolved_output_path,
        format="PNG",
        optimize=False,
        compress_level=9,
    )
    # 主动关闭 Pillow 图像对象，及时释放批量导出占用的内存。
    image.close()
    # 返回实际写入路径，便于调用方输出清单。
    return resolved_output_path


# 计算单个文件的 SHA-256，给教程图片和生成器源码提供内容级溯源。
def calculate_file_sha256(file_path: Path) -> str:
    """返回指定文件的小写 SHA-256 十六进制摘要。"""

    # 一次读取单张小尺寸 PNG 或脚本源码；最大文件远低于可用内存。
    file_bytes = Path(file_path).read_bytes()
    # 计算 256 位摘要并转成跨平台稳定的小写十六进制字符串。
    return hashlib.sha256(file_bytes).hexdigest()


# 写出手表教程截图清单，明确这些图片是离屏预览而非真板屏幕照片。
def write_documentation_watch_manifest(
    output_directory: Path,
    written_paths: tuple[Path, ...],
) -> Path:
    """为八张固定截图生成带尺寸、状态和 SHA-256 的 JSON 清单。"""

    # 截图数量必须与固定状态清单一致，防止漏图后仍生成貌似完整的 manifest。
    if len(written_paths) != len(DOCUMENTATION_PREVIEW_VARIANTS):
        # 抛出明确错误，让 CLI 返回失败而不是发布不完整教程资产。
        raise ValueError("截图数量与固定教程状态清单不一致")
    # 获取当前生成器源码路径，用源码内容指纹标记资产来源。
    generator_source_path = Path(__file__)
    # 按固定清单顺序构造每张图片的机器可读元数据。
    image_entries: list[dict[str, object]] = []
    # 同时遍历状态定义与实际输出，保证描述不会按文件名猜测。
    for variant, written_path in zip(
        DOCUMENTATION_PREVIEW_VARIANTS,
        written_paths,
        strict=True,
    ):
        # 写入稳定文件名、页面、状态变体、尺寸、用途和内容摘要。
        image_entries.append(
            {
                "File": written_path.name,
                "Page": PAGE_DISPLAY_NAMES[variant.page],
                "InternalPage": variant.page.value,
                "RestPreview": variant.rest_preview,
                "PairingPreview": variant.pairing_preview,
                "Description": variant.description,
                "Width": DISPLAY_WIDTH,
                "Height": DISPLAY_HEIGHT,
                "Sha256": calculate_file_sha256(written_path),
            }
        )
    # 清单明确安全边界，防止文档把语义预览误称为真板逐像素截图。
    manifest: dict[str, object] = {
        "Generator": "docs/tools/watch_ui_preview/watch_ui_preview.py",
        "GeneratorSha256": calculate_file_sha256(generator_source_path),
        "CaptureMode": "deterministic-pillow-production-semantics-preview",
        "UsesBluetooth": False,
        "ReadsDevice": False,
        "PixelEquivalentToLvgl": False,
        "Images": image_entries,
    }
    # 固定清单文件名，文档和 CI 无需查找带日期的临时文件。
    manifest_path = Path(output_directory) / "manifest.json"
    # 使用 UTF-8、中文直出和固定缩进；末尾换行符合仓库文本规范。
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    # 返回实际清单路径，供测试核对。
    return manifest_path


# 批量生成开源教程固定使用的八张页面图。
def export_documentation_watch_previews(output_directory: Path) -> tuple[Path, ...]:
    """按稳定文件名导出全部正式手表界面截图。"""

    # 把调用方目录规范化为 Path 对象。
    resolved_output_directory = Path(output_directory)
    # 创建正式输出目录；已存在的同名 PNG 将由确定性新版本覆盖。
    resolved_output_directory.mkdir(parents=True, exist_ok=True)
    # 保存本轮实际写入的文件路径。
    written_paths: list[Path] = []
    # 按固定清单顺序导出，保证 CLI 日志和文档审计顺序稳定。
    for variant in DOCUMENTATION_PREVIEW_VARIANTS:
        # 组合正式输出目录和稳定文件名。
        output_path = resolved_output_directory / variant.filename
        # 导出当前页面和状态变体。
        written_path = export_watch_preview_png(
            output_path,
            variant.page,
            variant.rest_preview,
            variant.pairing_preview,
        )
        # 记录成功写入的路径。
        written_paths.append(written_path)
    # 图片全部成功后才写清单，避免清单引用半成品。
    write_documentation_watch_manifest(
        resolved_output_directory,
        tuple(written_paths),
    )
    # 返回不可变路径元组，防止调用方无意修改结果清单。
    return tuple(written_paths)


# 定义 Tkinter 桌面应用；业务状态仍由 PreviewState 纯对象持有。
class WatchUiPreviewApp:
    """渲染可点击的 410×502 AMOLED 布局预览。"""

    def __init__(self, root: tk.Tk) -> None:
        """创建窗口、模拟屏幕和桌面演示控制。"""

        # 保存 Tk 根窗口，生命周期覆盖整个应用。
        self.root = root
        # 创建独立状态对象。
        self.state = PreviewState()
        # false 表示按真实交互流程运行；开发直达页面时改为 true 以冻结自动跳页定时器。
        self.direct_page_preview = False
        # 设置窗口标题，明确这是桌面预览器。
        self.root.title(PREVIEW_WINDOW_TITLE)
        # 禁止缩放，保证 410×502 模拟区域不会被 Tk 自动拉伸。
        self.root.resizable(False, False)
        # 配置桌面背景色。
        self.root.configure(bg=COLOR_DESKTOP)
        # 创建水平容器，左侧模拟屏、右侧桌面工具栏。
        body = tk.Frame(self.root, bg=COLOR_DESKTOP, padx=18, pady=18)
        # 使用 pack 放置容器。
        body.pack()
        # 创建严格 410×502 的画布；highlightthickness=0 防止边框增加内容尺寸。
        self.canvas = tk.Canvas(
            body,
            width=DISPLAY_WIDTH,
            height=DISPLAY_HEIGHT,
            bg=COLOR_BACKGROUND,
            highlightthickness=0,
            bd=0,
        )
        # 把模拟 AMOLED 放在左侧。
        self.canvas.pack(side=tk.LEFT)
        # 创建十二个正常页面加一个故障页的开发直达导航；导航完全位于设备屏区域之外。
        self._create_page_navigation(body)
        # 创建桌面演示控制栏；它不属于设备显示内容。
        control_panel = tk.Frame(body, bg=COLOR_DESKTOP, width=250, padx=18)
        # 把控制栏放在右侧并填充纵向空间。
        control_panel.pack(side=tk.LEFT, fill=tk.Y)
        # 禁止子控件改变控制栏宽度。
        control_panel.pack_propagate(False)
        # 创建中文说明标题。
        tk.Label(
            control_panel,
            text="桌面演示控制",
            bg=COLOR_DESKTOP,
            fg=COLOR_TEXT,
            font=("Microsoft YaHei UI", 14, "bold"),
        ).pack(anchor="w", pady=(4, 8))
        # 创建当前页面变量，便于每次 render 更新。
        self.page_name_var = tk.StringVar(value=development_page_label(self.state.page))
        # 显示当前页面的纯中文名称，不暴露内部英文枚举。
        tk.Label(
            control_panel,
            textvariable=self.page_name_var,
            bg=COLOR_DESKTOP,
            fg=COLOR_ACCENT,
            font=("Microsoft YaHei UI", 11, "bold"),
        ).pack(anchor="w", pady=(0, 12))
        # 显示用途边界，防止把本工具误当 LVGL 数值验证器。
        tk.Label(
            control_panel,
            text=DESKTOP_SCOPE_TEXT,
            justify=tk.LEFT,
            bg=COLOR_DESKTOP,
            fg=COLOR_MUTED,
            font=("Microsoft YaHei UI", 10),
        ).pack(anchor="w", pady=(0, 16))
        # 依次创建桌面辅助按钮。
        self._create_control_button(control_panel, CONTROL_BUTTON_LABELS[0], self.start_boot_sequence)
        # 创建熄屏入口，便于直接检查 SCREEN_OFF。
        self._create_control_button(control_panel, CONTROL_BUTTON_LABELS[1], self.enter_screen_off)
        # 创建手动计数按钮，便于检查权威次数逐次增长。
        self._create_control_button(control_panel, CONTROL_BUTTON_LABELS[2], self.manual_count)
        # 创建动作切换按钮，便于检查长短动作名布局。
        self._create_control_button(control_panel, CONTROL_BUTTON_LABELS[3], self.cycle_action)
        # 创建 BLE 状态切换按钮。
        self._create_control_button(control_panel, CONTROL_BUTTON_LABELS[4], self.toggle_ble)
        # 创建 Display Only 六位码覆盖层入口，便于检查此前不可见的配对界面。
        self._create_control_button(control_panel, CONTROL_BUTTON_LABELS[5], self.simulate_pairing_code)
        # 创建电池减少按钮，检查两位和一位百分比布局。
        self._create_control_button(control_panel, CONTROL_BUTTON_LABELS[6], self.reduce_battery)
        # 创建退出按钮。
        self._create_control_button(control_panel, CONTROL_BUTTON_LABELS[7], self.root.destroy)
        # 创建品牌字标字体；Tk 缺少指定字体时会安全回退系统中文字体。
        self.font_brand_mark = tkfont.Font(family="Microsoft YaHei UI", size=17, weight="bold")
        # 创建“毕昇杯”品牌字体。
        self.font_brand = tkfont.Font(family="Microsoft YaHei UI", size=14, weight="bold")
        # 创建“智慧运动助手”副品牌字体。
        self.font_brand_subtitle = tkfont.Font(family="Microsoft YaHei UI", size=10)
        # 创建电量和 BLE 状态字体。
        self.font_top_status = tkfont.Font(family="Microsoft YaHei UI", size=10, weight="bold")
        # 创建状态胶囊字体。
        self.font_title = tkfont.Font(family="Microsoft YaHei UI", size=12, weight="bold")
        # 创建主动作或主结果大字体。
        self.font_primary = tkfont.Font(family="Microsoft YaHei UI", size=27, weight="bold")
        # 创建权威次数中号字体。
        self.font_secondary = tkfont.Font(family="Microsoft YaHei UI", size=23, weight="bold")
        # 创建页脚字体。
        self.font_footer = tkfont.Font(family="Microsoft YaHei UI", size=11)
        # 创建按钮字体。
        self.font_button = tkfont.Font(family="Microsoft YaHei UI", size=13, weight="bold")
        # 绑定模拟屏点击事件。
        self.canvas.bind("<Button-1>", self.on_canvas_click)
        # 绑定 O 键快速熄屏。
        self.root.bind("<KeyPress-o>", lambda _event: self.enter_screen_off())
        # 绑定 R 键重播开机动画。
        self.root.bind("<KeyPress-r>", lambda _event: self.start_boot_sequence())
        # 绑定空格键手动增加一次计数。
        self.root.bind("<space>", lambda _event: self.manual_count())
        # 首次绘制当前 BOOT 页面。
        self.render()
        # 自动播放产品开机和自检流程。
        self.start_boot_sequence()

    def _create_page_navigation(self, parent: tk.Widget) -> None:
        """创建屏幕外十三页开发导航并绑定单击直达事件。"""

        # 创建固定宽度导航栏；该区域不占用 410×502 设备画布。
        navigation_panel = tk.Frame(parent, bg="#171B20", width=230, padx=14, pady=10)
        # 把导航栏放在模拟屏右侧、演示控制左侧。
        navigation_panel.pack(side=tk.LEFT, fill=tk.Y, padx=(14, 0))
        # 固定导航栏宽度，防止长枚举名改变窗口布局。
        navigation_panel.pack_propagate(False)
        # 显示导航标题，明确它只服务开发检查。
        tk.Label(
            navigation_panel,
            text="全部页面（开发导航）",
            bg="#171B20",
            fg=COLOR_ACCENT,
            font=("Microsoft YaHei UI", 12, "bold"),
        ).pack(anchor="w", pady=(0, 4))
        # 显示边界警告，避免把右侧列表误认为 AMOLED 产品界面。
        tk.Label(
            navigation_panel,
            text="仅开发工具，不属于设备界面\n单击页面后保持静态预览",
            justify=tk.LEFT,
            bg="#171B20",
            fg=COLOR_DANGER,
            font=("Microsoft YaHei UI", 9, "bold"),
        ).pack(anchor="w", pady=(0, 8))
        # 创建可显示十三行的页面列表；exportselection=false 保证切换焦点后仍保留高亮。
        self.page_listbox = tk.Listbox(
            navigation_panel,
            height=len(DEVELOPMENT_PAGE_ORDER),
            exportselection=False,
            bg="#0D1116",
            fg=COLOR_TEXT,
            selectbackground="#245B4C",
            selectforeground="#FFFFFF",
            activestyle="none",
            relief=tk.FLAT,
            highlightthickness=1,
            highlightbackground="#38424E",
            font=("Microsoft YaHei UI", 10),
        )
        # 逐项加入纯中文页面名；内部枚举值只保留在代码和日志中。
        for page in DEVELOPMENT_PAGE_ORDER:
            # 插入自然中文名称，列表不再显示 BOOT、HOME 等内部状态码。
            self.page_listbox.insert(tk.END, development_page_label(page))
        # 让列表填满导航栏宽度。
        self.page_listbox.pack(fill=tk.X)
        # 绑定列表选择事件；鼠标单击和键盘选择都走同一安全入口。
        self.page_listbox.bind("<<ListboxSelect>>", self.on_development_page_select)

    def on_development_page_select(self, _event: tk.Event[tk.Misc]) -> None:
        """根据开发导航选中项直接显示页面，不执行真实状态跳转副作用。"""

        # 读取当前列表选中索引；列表失焦或清空时可能为空。
        selected_indices = self.page_listbox.curselection()
        # 没有有效选项时保持当前页面。
        if not selected_indices:
            # 不修改业务状态。
            return
        # 读取首个且唯一选中项的整数索引。
        selected_index = int(selected_indices[0])
        # 索引必须位于固定十三页数组内，防止未来列表内容漂移。
        if selected_index >= len(DEVELOPMENT_PAGE_ORDER):
            # 非法列表索引不进入页面模型。
            return
        # 标记当前为静态开发预览，旧识别和动画回调会安全退出。
        self.direct_page_preview = True
        # 通过纯函数切换页面并重置对应演示字段。
        select_page_for_development(self.state, DEVELOPMENT_PAGE_ORDER[selected_index])
        # 立即重绘左侧 410×502 设备画布。
        self.render()

    def show_page_for_capture(self, page: Page, rest_preview: bool = False) -> None:
        """直达指定页面并填入稳定演示指标，供截图和人工视觉验收。"""

        # 标记为静态开发预览，阻止旧开机、识别和自动计数回调覆盖截图页面。
        self.direct_page_preview = True
        # 使用与无窗口 PNG 相同的稳定数据配置，避免两种预览产物语义漂移。
        configure_state_for_capture(self.state, page, rest_preview)
        # 重绘指定页面。
        self.render()

    def _create_control_button(
        self,
        parent: tk.Widget,
        label: str,
        callback: Callable[[], None],
    ) -> None:
        """创建一个不属于 AMOLED 内容的桌面辅助按钮。"""

        # 创建标准 Tk 按钮；command 在 UI 线程同步调用。
        button = tk.Button(
            parent,
            text=label,
            command=callback,
            bg="#303844",
            fg=COLOR_TEXT,
            activebackground="#3B4654",
            activeforeground=COLOR_TEXT,
            relief=tk.FLAT,
            font=("Microsoft YaHei UI", 10),
            padx=10,
            pady=7,
        )
        # 纵向排列并保留间距。
        button.pack(fill=tk.X, pady=4)

    def _rounded_rectangle(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        radius: int,
        **options: object,
    ) -> int:
        """使用平滑多边形绘制圆角矩形并返回 Canvas 对象 ID。"""

        # 构造顺时针圆角控制点；每个角重复端点帮助 smooth 形成圆弧。
        points = (
            x1 + radius,
            y1,
            x2 - radius,
            y1,
            x2,
            y1,
            x2,
            y1 + radius,
            x2,
            y2 - radius,
            x2,
            y2,
            x2 - radius,
            y2,
            x1 + radius,
            y2,
            x1,
            y2,
            x1,
            y2 - radius,
            x1,
            y1 + radius,
            x1,
            y1,
        )
        # 创建平滑多边形；splinesteps 控制圆角细分数。
        return self.canvas.create_polygon(points, smooth=True, splinesteps=24, **options)

    def render(self) -> None:
        """根据当前纯状态完整重绘 410×502 模拟屏。"""

        # 清除上一帧全部 Canvas 对象和点击区域。
        self.canvas.delete("all")
        # 清空旧按钮点击坐标。
        self.state.click_regions.clear()
        # 更新桌面控制栏页面名称。
        self.page_name_var.set(development_page_label(self.state.page))
        # 取得当前页面在开发导航固定顺序中的索引。
        page_index = DEVELOPMENT_PAGE_ORDER.index(self.state.page)
        # 清除旧页面高亮，保证导航只标记当前真实画布页面。
        self.page_listbox.selection_clear(0, tk.END)
        # 高亮当前页面，程序化 selection_set 不会触发用户选择事件。
        self.page_listbox.selection_set(page_index)
        # 滚动到当前页面，确保 ERROR 和 SHUTDOWN 等末尾状态始终可见。
        self.page_listbox.see(page_index)
        # SCREEN_OFF 必须保持全黑且不显示预览边框。
        if self.state.page is Page.SCREEN_OFF:
            # 绘制纯黑屏幕。
            self.canvas.create_rectangle(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, fill="#000000", outline="")
            # 在全屏注册 WAKE 命令，但不绘制按钮。
            self.state.click_regions.append((0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, Command.WAKE))
            # 本页渲染结束。
            return
        # 取得纯 presenter 页面模型。
        spec = build_page_spec(self.state)
        # 绘制 AMOLED 背景。
        self.canvas.create_rectangle(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, fill=COLOR_BACKGROUND, outline="")
        # 绘制蓝色品牌方标，左上形成稳定的产品识别锚点。
        self._rounded_rectangle(32, 28, 70, 66, 12, fill=COLOR_ACCENT, outline="")
        # 品牌方标只使用一个“毕”字，避免小屏塞入复杂图形。
        self.canvas.create_text(
            51,
            47,
            text="毕",
            fill="#FFFFFF",
            font=self.font_brand_mark,
            anchor="center",
        )
        # 绘制主品牌“毕昇杯”。
        self.canvas.create_text(
            80,
            28,
            text="毕昇杯",
            fill=COLOR_TEXT,
            font=self.font_brand,
            anchor="nw",
        )
        # 绘制产品主题“智慧运动助手”，满足设备开机后持续可见的品牌合同。
        self.canvas.create_text(
            80,
            51,
            text="智慧运动助手",
            fill=COLOR_SECONDARY,
            font=self.font_brand_subtitle,
            anchor="nw",
        )
        # 根据电量和充电状态选择电池填充色。
        battery_color = (
            COLOR_CYAN
            if self.state.charging
            else COLOR_SUCCESS
            if self.state.battery_percent >= 40
            else COLOR_WARNING
            if self.state.battery_percent >= 20
            else COLOR_DANGER
        )
        # 绘制电量百分比；充电时增加“充”字而不依赖不可用图标字体。
        self.canvas.create_text(
            307,
            31,
            text=f"{'充 ' if self.state.charging else ''}{self.state.battery_percent}%",
            fill=battery_color,
            font=self.font_top_status,
            anchor="ne",
        )
        # 绘制电池外壳；44×20 px 轮廓和右侧极柱接近熟悉的智能手表系统图标。
        self._rounded_rectangle(312, 31, 356, 51, 5, fill=COLOR_BACKGROUND, outline=COLOR_SECONDARY, width=2)
        # 绘制电池正极极柱。
        self._rounded_rectangle(358, 37, 362, 45, 2, fill=COLOR_SECONDARY, outline="")
        # 电池内部可用宽度为 36 px，百分比经过 0～100 边界夹紧后换算。
        battery_fill_width = max(2, int(36 * max(0, min(100, self.state.battery_percent)) / 100))
        # 绘制动态电池填充，不用文字替代电池形状。
        self._rounded_rectangle(
            316,
            35,
            316 + battery_fill_width,
            47,
            3,
            fill=battery_color,
            outline="",
        )
        # BLE 已连接显示青色状态点，未连接显示低对比灰色点。
        ble_color = COLOR_CYAN if self.state.ble_connected else COLOR_MUTED
        # 绘制 BLE 状态点；纵向独立放在电池外框底部 51 像素以下。
        self.canvas.create_oval(281, 63, 289, 71, fill=ble_color, outline="")
        # 绘制简短连接事实，避免向用户暴露协议枚举。
        self.canvas.create_text(
            295,
            67,
            text="已连接" if self.state.ble_connected else "未连接",
            fill=COLOR_SECONDARY,
            font=self.font_top_status,
            anchor="w",
        )
        # 绘制训练主卡；统一 32 px 安全边距和 28 px 圆角。
        self._rounded_rectangle(32, 94, 378, 344, 28, fill=COLOR_CARD, outline=COLOR_BORDER, width=1)
        # 错误、休息、计数中和普通页面分别使用红、琥珀、绿和科技蓝语义色。
        semantic_color = (
            COLOR_DANGER
            if self.state.page is Page.ERROR
            else COLOR_WARNING
            if (self.state.page is Page.PAUSED)
            or (self.state.page is Page.RUNNING and not self.state.counting_enabled)
            else COLOR_SUCCESS
            if self.state.page is Page.RUNNING
            else COLOR_ACCENT
        )
        # 状态胶囊的深色底板承载当前页面唯一状态。
        self._rounded_rectangle(50, 112, 204, 144, 16, fill=COLOR_RAISED, outline="")
        # 胶囊左侧状态点让训练、休息和故障可在一眼内区分。
        self.canvas.create_oval(62, 123, 72, 133, fill=semantic_color, outline="")
        # 绘制当前状态标题。
        self.canvas.create_text(
            80,
            128,
            text=spec.title,
            fill=COLOR_TEXT,
            font=self.font_title,
            anchor="w",
        )
        # 绘制动作名或页面主结果；科技蓝仅服务正常页面，休息和错误沿用语义色。
        self.canvas.create_text(
            DISPLAY_WIDTH // 2,
            190,
            text=spec.primary,
            fill=semantic_color,
            font=self.font_primary,
            width=306,
            justify=tk.CENTER,
            anchor="center",
        )
        # 绘制权威次数或页面次级信息。
        self.canvas.create_text(
            DISPLAY_WIDTH // 2,
            248,
            text=spec.secondary,
            fill=COLOR_TEXT,
            font=self.font_secondary,
            width=306,
            justify=tk.CENTER,
            anchor="center",
        )
        # 绘制计时、热量、置信度或恢复条件。
        self.canvas.create_text(
            DISPLAY_WIDTH // 2,
            298,
            text=spec.footer,
            fill=COLOR_SECONDARY,
            font=self.font_footer,
            width=306,
            justify=tk.CENTER,
            anchor="center",
        )
        # 绘制活动进度底轨；它只表达当前会话节奏，不承诺目标次数。
        self._rounded_rectangle(52, 322, 358, 327, 3, fill=COLOR_BORDER, outline="")
        # 训练中按 12 次视觉尺度推进；其它页面按动画相位或稳定小比例显示。
        progress_ratio = (
            min(1.0, self.state.count / 12.0)
            if self.state.page in (Page.RUNNING, Page.PAUSED, Page.STOP_CONFIRM, Page.SUMMARY)
            else ((self.state.animation_phase % 4) + 1) / 4.0
            if self.state.page in (Page.BOOT, Page.SHUTDOWN)
            else 0.22
        )
        # 计算最小 10 px 的可见进度宽度。
        progress_width = max(10, int(306 * progress_ratio))
        # 绘制活动进度，颜色继承当前页面语义。
        self._rounded_rectangle(52, 322, 52 + progress_width, 327, 3, fill=semantic_color, outline="")
        # 绘制页面按钮。
        self._render_buttons(spec.buttons)

    def _render_buttons(self, buttons: tuple[ButtonSpec, ...]) -> None:
        """设置四项绘制二乘二网格，其它按钮绘制单行。"""

        # 无按钮页面直接结束。
        if not buttons:
            # 页面保持只读。
            return
        # 四项设置按钮采用二乘二网格，保证每个触摸目标大于 60 px 高。
        if len(buttons) == 4:
            # 每行两个按钮，避免小屏出现五个拥挤入口。
            rows = (buttons[:2], buttons[2:])
            # 设置区紧接主卡下方，并保留 10 px 行间距。
            vertical_bounds = ((354, 414), (424, 484))
        else:
            # 其它页面只有一行主操作。
            rows = (buttons,)
            # 单行保持 70 px 高触摸区域，适合手指点击。
            vertical_bounds = ((396, 466),)
        # 固定按钮区左右边距，单位 px。
        left = 32
        # 固定按钮区右边界。
        right = DISPLAY_WIDTH - 32
        # 同一行相邻按钮间距固定 10 px。
        gap = 10
        # 逐行绘制按钮；rows 与 vertical_bounds 具有相同固定长度。
        for row_index, row_buttons in enumerate(rows):
            # 当前行按钮按行内数量等宽，第二行两个按钮会自动获得更宽触摸区。
            button_width = (right - left - gap * (len(row_buttons) - 1)) / len(row_buttons)
            # 读取当前行顶边和底边。
            top, bottom = vertical_bounds[row_index]
            # 依次绘制当前行全部按钮。
            for index, button in enumerate(row_buttons):
                # 计算当前按钮左边界。
                x1 = int(left + index * (button_width + gap))
                # 计算当前按钮右边界。
                x2 = int(x1 + button_width)
                # 危险操作使用暗红；每行首个普通按钮使用实心科技蓝，其它使用抬升卡片色。
                fill_color = (
                    "#451B22"
                    if button.dangerous
                    else COLOR_ACCENT
                    if index == 0
                    else COLOR_RAISED
                )
                # 危险操作使用红色细描边，其它按钮不添加多余描边。
                outline_color = COLOR_DANGER if button.dangerous else fill_color
                # 绘制圆角按钮底板。
                self._rounded_rectangle(x1, top, x2, bottom, 18, fill=fill_color, outline=outline_color, width=1)
                # 绘制按钮标签。
                self.canvas.create_text(
                    (x1 + x2) // 2,
                    (top + bottom) // 2,
                    text=button.label,
                    fill=COLOR_TEXT,
                    font=self.font_button,
                    width=max(40, x2 - x1 - 8),
                    justify=tk.CENTER,
                    anchor="center",
                )
                # 保存当前按钮点击区域和命令。
                self.state.click_regions.append((x1, top, x2, bottom, button.command))

    def on_canvas_click(self, event: tk.Event[tk.Misc]) -> None:
        """把 Canvas 点击坐标映射为页面按钮命令。"""

        # 遍历当前页面的有限点击区域。
        for x1, y1, x2, y2, command in self.state.click_regions:
            # 检查点击点是否位于当前矩形内。
            if x1 <= event.x <= x2 and y1 <= event.y <= y2:
                # 命中后处理命令。
                self.handle_command(command)
                # 每次点击最多触发一个命令。
                return

    def handle_command(self, command: Command) -> None:
        """执行纯状态命令并安排识别、训练或反馈动画。"""

        # 用户点击设备屏按钮后退出静态开发预览，恢复真实交互流程与定时副作用。
        self.direct_page_preview = False
        # 先让纯状态机处理页面和设置变化。
        effect = self.state.dispatch(command)
        # 立即渲染命令结果。
        self.render()
        # START 已立即开始采样；预览器只模拟首个可信窗口稍后完成。
        if effect == "start_identification":
            # 1.2 秒后进入训练页；该延时只演示识别状态，不是产品倒计时合同。
            self.root.after(1200, self._finish_prepare)
        # RESUME 或 WAKE 到 RUNNING 需要恢复自动计数。
        if effect == "start_running":
            # 1.2 秒后生成下一次演示计数。
            self.root.after(1200, self._running_tick)
        # SHUTDOWN 播放短动画并最终进入 SCREEN_OFF。
        if effect == "shutdown":
            # 启动关机点动画。
            self._animate_shutdown()
            # 1.2 秒后模拟 PMIC 断电为纯黑屏。
            self.root.after(1200, self._complete_shutdown)

    def _finish_prepare(self) -> None:
        """在首个可信动作已锁定且 PREPARE 未被取消时进入 RUNNING。"""

        # 开发直达页不得被旧准备定时器覆盖。
        if self.direct_page_preview:
            # 保持当前静态页面。
            return
        # 用户取消或熄屏后不得自动跳页。
        if self.state.page is not Page.PREPARE:
            # 安全退出。
            return
        # 进入实时训练页。
        self.state.page = Page.RUNNING
        # 绘制训练页。
        self.render()
        # 安排第一次自动计数。
        self.root.after(1200, self._running_tick)

    def _running_tick(self) -> None:
        """在 RUNNING 页面周期生成一次演示计数。"""

        # 开发直达 RUNNING 时只展示布局，计数由右侧手动按钮触发。
        if self.direct_page_preview:
            # 停止自动计数链。
            return
        # 暂停、停止或离开页面时旧定时器安全退出。
        if self.state.page is not Page.RUNNING:
            # 不再安排下一轮。
            return
        # 增加一次权威计数。
        self.state.increment_count()
        # 先绘制新次数。
        self.render()
        # 安排下一次演示计数。
        self.root.after(1200, self._running_tick)

    def start_boot_sequence(self) -> None:
        """从 BOOT 重播 800 ms 开机和 350 ms 自检流程。"""

        # 重播按钮显式恢复真实动画流程。
        self.direct_page_preview = False
        # 重置到 BOOT 页面。
        self.state.page = Page.BOOT
        # 清零动画相位。
        self.state.animation_phase = 0
        # 绘制首帧。
        self.render()
        # 启动轻量点动画。
        self._animate_boot()
        # 800 ms 后进入 SELF_TEST，与设备默认 boot_ms 一致。
        self.root.after(800, self._show_self_test)

    def _animate_boot(self) -> None:
        """每 160 ms 更新 BOOT 点和圆环呼吸。"""

        # 开发直达 BOOT 时保持静态帧。
        if self.direct_page_preview:
            # 停止动画回调链。
            return
        # 只在 BOOT 页面继续动画。
        if self.state.page is not Page.BOOT:
            # 页面已切换，停止链。
            return
        # 推进动画相位。
        self.state.animation_phase += 1
        # 重绘 BOOT。
        self.render()
        # 安排下一帧。
        self.root.after(160, self._animate_boot)

    def _show_self_test(self) -> None:
        """从 BOOT 进入 SELF_TEST 并保持 350 ms。"""

        # 开发直达页面后旧开机计时器不得覆盖选择结果。
        if self.direct_page_preview:
            # 保持开发者选中的页面。
            return
        # 重播期间若用户已离开 BOOT，则不覆盖当前页。
        if self.state.page is not Page.BOOT:
            # 安全退出。
            return
        # 进入自检页。
        self.state.page = Page.SELF_TEST
        # 绘制自检内容。
        self.render()
        # 350 ms 后进入 HOME。
        self.root.after(350, self._show_home_after_self_test)

    def _show_home_after_self_test(self) -> None:
        """自检演示未被中断时进入主页。"""

        # 开发直达 SELF_TEST 时保持静态自检内容。
        if self.direct_page_preview:
            # 停止自动进入主页。
            return
        # 只接受 SELF_TEST 的定时完成。
        if self.state.page is not Page.SELF_TEST:
            # 安全退出。
            return
        # 进入主页。
        self.state.page = Page.HOME
        # 绘制主页。
        self.render()

    def _animate_shutdown(self) -> None:
        """每 160 ms 更新 SHUTDOWN 点动画。"""

        # 开发直达 SHUTDOWN 时保持静态关机页。
        if self.direct_page_preview:
            # 停止动画回调链。
            return
        # 只在关机页继续。
        if self.state.page is not Page.SHUTDOWN:
            # 页面已变化，停止链。
            return
        # 推进动画相位。
        self.state.animation_phase += 1
        # 重绘关机页。
        self.render()
        # 安排下一帧。
        self.root.after(160, self._animate_shutdown)

    def _complete_shutdown(self) -> None:
        """把 SHUTDOWN 动画完成结果转成 SCREEN_OFF。"""

        # 开发直达关机页时不得被旧定时器自动切成黑屏。
        if self.direct_page_preview:
            # 保持静态预览。
            return
        # 只处理仍在关机页的定时回调。
        if self.state.page is not Page.SHUTDOWN:
            # 安全退出。
            return
        # 关机后唤醒恢复点设为 HOME，桌面演示可点击重启。
        self.state.screen_resume_page = Page.HOME
        # 进入全黑屏幕。
        self.state.page = Page.SCREEN_OFF
        # 绘制黑屏。
        self.render()

    def enter_screen_off(self) -> None:
        """通过桌面工具栏进入 SCREEN_OFF。"""

        # 桌面控制按钮属于动态演示流程，不保持开发直达冻结状态。
        self.direct_page_preview = False
        # 让纯状态对象保存恢复点。
        self.state.enter_screen_off()
        # 绘制纯黑屏。
        self.render()

    def manual_count(self) -> None:
        """手动增加一次计数，便于检查权威次数布局。"""

        # 只在 RUNNING 中增加指标。
        self.state.increment_count()
        # 重绘可能变化的指标。
        self.render()

    def cycle_action(self) -> None:
        """通过桌面工具栏切换动作名称。"""

        # 更新动作名。
        self.state.cycle_action()
        # 重绘当前页面。
        self.render()

    def toggle_ble(self) -> None:
        """通过桌面工具栏切换 BLE 连接事实。"""

        # 翻转连接状态。
        self.state.ble_connected = not self.state.ble_connected
        # 重绘顶栏或诊断摘要。
        self.render()

    def simulate_pairing_code(self) -> None:
        """显示六位配对码覆盖层，并在 60 秒后按生产合同自动清除。"""

        # 配对演示属于动态流程，退出开发直达冻结状态。
        self.direct_page_preview = False
        # 使用含前导零效果的稳定演示值，检查六位格式和中文字库布局。
        self.state.pairing_code = 12345
        # 打开全局覆盖层；当前业务页面保留不变。
        self.state.pairing_active = True
        # 立即重绘配对界面。
        self.render()
        # 60 秒后清除；回调会再次确认覆盖层仍活动，避免覆盖新一轮配对。
        self.root.after(60000, self._clear_pairing_code)

    def _clear_pairing_code(self) -> None:
        """清除已经超时的演示配对码。"""

        # 已由忘记电脑、连接完成或其它演示操作清除时不重复刷新。
        if not self.state.pairing_active:
            # 无活动配对码，安全返回。
            return
        # 关闭覆盖层并保留底层业务页面。
        self.state.pairing_active = False
        # 重绘恢复后的页面。
        self.render()

    def reduce_battery(self) -> None:
        """通过桌面工具栏把电量减少 5%。"""

        # 使用 max 防止电量低于零。
        self.state.battery_percent = max(0, self.state.battery_percent - 5)
        # 重绘顶栏。
        self.render()


# 执行无窗口冒烟测试；该函数不创建 Tk 根窗口，适合 CI 和无显示主机。
def run_smoke_test() -> None:
    """验证页面集合、动态字段和关键按钮流程。"""

    # 屏幕像素合同必须保持 410×502。
    assert (DISPLAY_WIDTH, DISPLAY_HEIGHT) == (410, 502)
    # 生产状态机的十三个页面必须全部存在，包含此前遗漏的 ERROR 故障页。
    assert len(Page) == 13
    # 开发导航必须逐项覆盖全部页面且保持枚举声明顺序。
    assert DEVELOPMENT_PAGE_ORDER == tuple(Page)
    # 创建独立纯状态。
    state = PreviewState()
    # 遍历全部页面，保证 presenter 不漏分支。
    for page in Page:
        # 设置当前页面。
        state.page = page
        # 通过开发直达函数切换当前页，验证静态导航合同不依赖 Tk。
        select_page_for_development(state, page)
        # 每页必须成功构造纯模型。
        spec = build_page_spec(state)
        # 按钮数量不得超过当前设备端五槽合同。
        assert len(spec.buttons) <= 5
    # HOME 必须提供开始、设置和关机。
    state.page = Page.HOME
    # 构造主页。
    home = build_page_spec(state)
    # 校验三个命令顺序。
    assert tuple(button.command for button in home.buttons) == (
        Command.START,
        Command.OPEN_SETTINGS,
        Command.SHUTDOWN,
    )
    # START 必须立即进入采样识别流程，不经过人为倒计时。
    assert state.dispatch(Command.START) == "start_identification"
    # 检查准备页。
    prepare = build_page_spec(state)
    # 识别页必须明确点击后已经开始动作，不插入人为倒计时。
    assert state.page is Page.PREPARE and prepare.primary == "开始做动作"
    # 固定佩戴合同必须进入设备可见信息，避免左右手域混用。
    assert "右手佩戴" in prepare.footer
    # 模拟动作锁定进入训练。
    state.page = Page.RUNNING
    # 增加一次应得到 count=1，不依赖不存在的振动马达。
    state.increment_count()
    # 校验权威累计逐次增长。
    assert state.count == 1
    # 休息门关闭后不得继续增长。
    state.counting_enabled = False
    # 尝试在休息状态增加一次。
    state.increment_count()
    # 休息期间次数必须保持。
    assert state.count == 1
    # 恢复计数门，继续验证停止流程。
    state.counting_enabled = True
    # 首次 STOP 只能打开确认页。
    state.dispatch(Command.STOP)
    # 校验停止确认页。
    assert state.page is Page.STOP_CONFIRM
    # 确认页必须含真正停止与取消两个命令。
    stop_page = build_page_spec(state)
    # 校验命令集合。
    assert tuple(button.command for button in stop_page.buttons) == (
        Command.CONFIRM_STOP,
        Command.BACK,
    )
    # CANCEL 必须回到 RUNNING。
    state.dispatch(Command.BACK)
    # 校验恢复页。
    assert state.page is Page.RUNNING
    # 再次停止并确认后进入 SUMMARY。
    state.dispatch(Command.STOP)
    # 真正确认停止。
    state.dispatch(Command.CONFIRM_STOP)
    # 校验总结页。
    assert state.page is Page.SUMMARY
    # DONE 后返回 HOME。
    state.dispatch(Command.DONE)
    # 进入设置页。
    state.dispatch(Command.OPEN_SETTINGS)
    # 循环亮度必须改变数值并增加修订号。
    old_revision = state.preferences_revision
    # 执行亮度循环。
    state.dispatch(Command.CYCLE_BRIGHTNESS)
    # 校验亮度和修订号。
    assert state.brightness_percent == 60 and state.preferences_revision == old_revision + 1
    # 设置页必须包含设备端删除绑定入口。
    settings = build_page_spec(state)
    # 忘记电脑按钮必须位于设置页且使用明确命令。
    assert Command.FORGET_COMPUTER in tuple(button.command for button in settings.buttons)
    # 配对覆盖层必须补足六位前导零并隐藏全部业务按钮。
    state.pairing_code = 12345
    # 打开覆盖层。
    state.pairing_active = True
    # 构造覆盖模型。
    pairing = build_page_spec(state)
    # 校验中文标题、六位数字和零按钮。
    assert pairing.title == "蓝牙配对" and pairing.primary == "012345" and not pairing.buttons
    # 关闭覆盖层，继续验证设置与诊断流程。
    state.pairing_active = False
    # 进入诊断页。
    state.dispatch(Command.OPEN_DIAGNOSTICS)
    # 校验诊断页两个命令。
    diagnostics = build_page_spec(state)
    # 诊断页首项必须是熄屏设置，不能出现不存在的振动测试入口。
    assert diagnostics.buttons[0].command is Command.CYCLE_TIMEOUT
    # 返回设置再返回主页。
    state.dispatch(Command.BACK)
    # 设置页返回主页。
    state.dispatch(Command.BACK)
    # 进入逻辑熄屏。
    state.enter_screen_off()
    # WAKE 必须恢复主页。
    state.dispatch(Command.WAKE)
    # 校验恢复结果。
    assert state.page is Page.HOME
    # 开发直达 ERROR 必须显示故障码并提供唯一安全关机按钮。
    select_page_for_development(state, Page.ERROR)
    # 构造中文故障页。
    error_page = build_page_spec(state)
    # 检查标题、示例故障码和关机命令均存在。
    assert error_page.title == "设备故障" and "-42" in error_page.primary
    # 故障页只能提供安全关机，不能绕过阻断启动训练。
    assert tuple(button.command for button in error_page.buttons) == (Command.SHUTDOWN,)
    # 输出稳定成功标记，供脚本和人工检查。
    print("WATCH_UI_PREVIEW_SMOKE_OK pages=13 display=410x502 nav=13 language=zh-CN")


# 解析命令行并选择无窗口测试或交互预览。
def main() -> None:
    """程序入口。"""

    # 创建命令行解析器。
    parser = argparse.ArgumentParser(description="410x502 AMOLED 智能运动手表 UI 桌面预览器")
    # 添加无窗口冒烟测试开关。
    parser.add_argument(
        "--smoke-test",
        action="store_true",
        help="不创建窗口，只验证页面、按钮和状态流程",
    )
    # 添加开发展示配对覆盖层开关，使启动后可立即检查此前缺失的中文六位码界面。
    parser.add_argument(
        "--pairing-preview",
        action="store_true",
        help="创建窗口后立即显示中文蓝牙配对覆盖层，60 秒后自动清除",
    )
    # 添加页面直达参数，便于对主页、训练、休息和设置页面制作稳定截图。
    parser.add_argument(
        "--page",
        choices=tuple(page.value for page in Page),
        help="创建窗口后直达指定内部页面枚举，例如 HOME 或 RUNNING",
    )
    # 添加休息状态预览；仅与 RUNNING 页面组合时关闭实时计数门。
    parser.add_argument(
        "--rest-preview",
        action="store_true",
        help="RUNNING 页面显示休息状态，动作和累计保持不变",
    )
    # 添加单文件 PNG 导出参数；该模式不创建 Tk 窗口。
    parser.add_argument(
        "--export-png",
        type=Path,
        help="把 --page 指定状态离屏导出为精确 410×502 PNG",
    )
    # 添加教程八页面批量导出参数；文件名由固定清单决定。
    parser.add_argument(
        "--export-all",
        type=Path,
        help="离屏导出 HOME、PREPARE、RUNNING、休息、设置、配对、总结和故障页面",
    )
    # 解析当前进程参数。
    arguments = parser.parse_args()
    # 冒烟测试与任何 PNG 导出模式互斥，防止命令含义不明确。
    if arguments.smoke_test and (arguments.export_png or arguments.export_all):
        # 通过 argparse 返回标准参数错误退出码。
        parser.error("--smoke-test 不能与 --export-png 或 --export-all 同时使用")
    # 批量导出使用固定八状态，不接受单页状态参数覆盖。
    if arguments.export_all and (
        arguments.page or arguments.rest_preview or arguments.pairing_preview
    ):
        # 提示调用方改用单页导出入口。
        parser.error("--export-all 使用固定状态，不可组合 --page、--rest-preview 或 --pairing-preview")
    # 休息预览只对 RUNNING 有业务含义。
    if arguments.rest_preview and arguments.page != Page.RUNNING.value:
        # 阻止生成文件名正确但业务内容错误的教程截图。
        parser.error("--rest-preview 必须与 --page RUNNING 同时使用")
    # CI/无显示环境执行纯逻辑测试。
    if arguments.smoke_test:
        # 运行断言。
        run_smoke_test()
        # 测试完成后返回。
        return
    # 批量 PNG 模式生成固定教程截图后立即退出，不创建桌面窗口。
    if arguments.export_all:
        # 导出固定八个页面及状态变体。
        written_paths = export_documentation_watch_previews(arguments.export_all)
        # 输出稳定成功标记和文件数，便于 CI 与人工审计。
        print(
            "WATCH_UI_PNG_EXPORT_OK "
            f"count={len(written_paths)} size={DISPLAY_WIDTH}x{DISPLAY_HEIGHT} "
            f"directory={arguments.export_all}"
        )
        # 导出完成，不进入 Tk 主循环。
        return
    # 单页 PNG 模式按页面、休息和配对参数生成指定文件。
    if arguments.export_png:
        # 未显式指定页面时使用主页，配对覆盖层也以主页作为稳定底层页面。
        target_page = Page(arguments.page) if arguments.page else Page.HOME
        # 导出一张正式 PNG。
        written_path = export_watch_preview_png(
            arguments.export_png,
            target_page,
            arguments.rest_preview,
            arguments.pairing_preview,
        )
        # 输出稳定成功标记和精确状态。
        print(
            "WATCH_UI_PNG_EXPORT_OK "
            f"count=1 size={DISPLAY_WIDTH}x{DISPLAY_HEIGHT} "
            f"page={target_page.value} rest={int(arguments.rest_preview)} "
            f"pairing={int(arguments.pairing_preview)} path={written_path}"
        )
        # 导出完成，不创建 Tk 根窗口。
        return
    # 创建 Tk 根窗口。
    root = tk.Tk()
    # 创建完整预览应用并保持引用到主循环结束。
    _application = WatchUiPreviewApp(root)
    # 开发者要求直接检查配对页时，在 Tk 主循环开始后安全触发同一个生产语义演示入口。
    if arguments.pairing_preview:
        # 延迟 100 ms 等待首帧控件完成布局，随后显示零填充六位码覆盖层。
        root.after(100, _application.simulate_pairing_code)
    # 指定页面时在首帧布局完成后切到稳定静态验收状态。
    if arguments.page:
        # 把 argparse 已校验的字符串恢复为强类型页面枚举。
        target_page = Page(arguments.page)
        # 延迟 100 ms 后直达页面，覆盖构造函数已安排的开机动画。
        root.after(
            100,
            lambda: _application.show_page_for_capture(target_page, arguments.rest_preview),
        )
    # 进入 Tk 事件循环；所有回调在同一 UI 线程串行执行。
    root.mainloop()


# 仅直接执行文件时启动，不在单元导入时创建窗口。
if __name__ == "__main__":
    # 调用统一入口。
    main()
