"""使用 Python 标准库模拟 410×502 AMOLED 产品界面。

本工具只验证页面层级、文字布局、按钮流程和轻量动画观感，不验证 LVGL
字体度量、触摸坐标、驱动刷新时序或 ESP32 性能。页面文字与当前
``esp32/firmware/components/ui/ui_presenter.c`` 保持同一产品语义。
"""

# 延迟解析类型注解，避免 Python 3.12 的 tkinter.Event 在运行时被当作可下标泛型。
from __future__ import annotations

# 引入命令行解析器，使无显示环境可以执行纯逻辑冒烟测试。
import argparse
# 引入枚举类型，避免页面和命令依赖易拼错的裸字符串。
from enum import Enum
# 引入数据类，为页面模型和运行状态提供明确字段合同。
from dataclasses import dataclass, field
# 引入 Tkinter；它属于 Python 标准库，不产生第三方安装依赖。
import tkinter as tk
# 引入 Tkinter 字体模块，统一模拟屏内中文标签的字号和字重。
from tkinter import font as tkfont
# 引入类型提示，说明按钮列表、点击区域和回调的形状。
from typing import Callable


# AMOLED 模拟内容宽度固定为厂家屏幕横向像素数，单位 px。
DISPLAY_WIDTH = 410
# AMOLED 模拟内容高度固定为厂家屏幕纵向像素数，单位 px。
DISPLAY_HEIGHT = 502
# 页面背景采用 LVGL renderer 的近黑色，降低真实 AMOLED 发光面积。
COLOR_BACKGROUND = "#05070A"
# 中央信息卡片采用 LVGL renderer 的深灰色。
COLOR_CARD = "#10151C"
# 主强调色采用设备端固定青绿色。
COLOR_ACCENT = "#32E6A1"
# 次强调色采用设备端固定蓝色。
COLOR_SECONDARY = "#45A3FF"
# 主文字采用接近白色的浅灰，避免真实屏幕长期全白高亮。
COLOR_TEXT = "#F2F5F7"
# 次要文字采用设备端弱化灰色。
COLOR_MUTED = "#8C98A4"
# 停止等危险操作使用柔和红色，和普通导航按钮区分。
COLOR_DANGER = "#FF6B6B"
# 桌面窗口背景不属于 AMOLED 内容，仅用于区分模拟器控制区。
COLOR_DESKTOP = "#20252C"
# 视觉振动反馈采用暖黄色边框，避免误认为普通页面强调色。
COLOR_HAPTIC_FLASH = "#FFD166"


# 定义产品页面；列表严格覆盖生产 ui_state_t 的十三个可渲染界面状态。
class Page(Enum):
    """描述桌面预览器支持的设备页面。"""

    # 冷启动 Logo 与本地 AI 提示页。
    BOOT = "BOOT"
    # 显示、触摸、IMU 和存储自检页。
    SELF_TEST = "SELF_TEST"
    # 无活动会话的主页。
    HOME = "HOME"
    # 三秒准备倒计时和动作锁定页。
    PREPARE = "PREPARE"
    # 实时动作、次数和热量页。
    RUNNING = "RUNNING"
    # 暂停后冻结计数的页面。
    PAUSED = "PAUSED"
    # 停止训练的二次确认页。
    STOP_CONFIRM = "STOP_CONFIRM"
    # 会话本地保存后的总结页。
    SUMMARY = "SUMMARY"
    # 亮度、振动和诊断入口页。
    SETTINGS = "SETTINGS"
    # 质量位、BLE 和偏好修订号诊断页。
    DIAGNOSTICS = "DIAGNOSTICS"
    # AMOLED 逻辑熄屏页。
    SCREEN_OFF = "SCREEN_OFF"
    # 硬件或模型合同失败后的阻断训练页。
    ERROR = "ERROR"
    # 安全保存后等待 PMIC 断电的页面。
    SHUTDOWN = "SHUTDOWN"


# 桌面窗口标题使用纯中文；具体板型属于内部开发信息，不占用用户可见标题。
PREVIEW_WINDOW_TITLE = "健身手柄界面预览器"

# 把稳定内部页面枚举映射为用户可理解的中文页面名称；键集合必须覆盖全部十三页。
PAGE_DISPLAY_NAMES: dict[Page, str] = {
    # 冷启动页面显示产品开机动画。
    Page.BOOT: "开机动画",
    # 自检页面显示硬件检查进度。
    Page.SELF_TEST: "设备自检",
    # 主页提供开始、设置和关机入口。
    Page.HOME: "主页",
    # 准备页显示三秒倒计时。
    Page.PREPARE: "训练准备",
    # 运行页显示实时动作与指标。
    Page.RUNNING: "训练中",
    # 暂停页冻结计数。
    Page.PAUSED: "训练暂停",
    # 停止确认页防止误触结束。
    Page.STOP_CONFIRM: "停止确认",
    # 总结页显示本次训练结果。
    Page.SUMMARY: "训练总结",
    # 设置页管理屏幕、振动和绑定。
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


# 定义页面按钮命令；命名与设备端 ui_command_t 的产品意图一致。
class Command(Enum):
    """描述用户点击后触发的纯预览命令。"""

    # 从主页进入准备倒计时。
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
    # 切换有效计数振动开关。
    TOGGLE_HAPTIC = "TOGGLE_HAPTIC"
    # 循环自动熄屏秒数。
    CYCLE_TIMEOUT = "CYCLE_TIMEOUT"
    # 触发一次诊断振动脉冲。
    TEST_HAPTIC = "TEST_HAPTIC"
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
    # 保存页面最醒目的动作、倒计时或主指标。
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
    # PREPARE 倒计时剩余秒数，范围 0～3。
    prepare_seconds: int = 3
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
    # 保存有效计数是否产生振动反馈。
    haptic_enabled: bool = True
    # 保存偏好修订号；每次设置变化递增。
    preferences_revision: int = 1
    # 保存运行期数据质量位图。
    data_quality_flags: int = 0x0021
    # 保存开发预览错误页的示例故障码；仅用于检查负号和数字布局。
    fault_code: int = -42
    # 保存动画点数量，范围 0～3。
    animation_phase: int = 0
    # 保存当前是否显示视觉振动反馈边框。
    haptic_flash_active: bool = False
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

    def toggle_haptic(self) -> None:
        """翻转振动偏好并递增偏好修订号。"""

        # 取反当前布尔值。
        self.haptic_enabled = not self.haptic_enabled
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
            # 重置三秒准备倒计时。
            self.prepare_seconds = 3
            # 进入 PREPARE。
            self.page = Page.PREPARE
            # 请求调用方启动定时倒计时。
            return "start_prepare"
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
        # 设置页振动按钮切换偏好。
        if command is Command.TOGGLE_HAPTIC and self.page is Page.SETTINGS:
            # 更新振动开关。
            self.toggle_haptic()
            # 开启后的本次点击用视觉闪烁确认反馈。
            return "haptic" if self.haptic_enabled else "none"
        # 设置页“忘记电脑”清除绑定演示事实，不改变亮度、振动或训练指标。
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
        # 诊断页 TEST VIBE 仅产生反馈，不改变业务指标。
        if command is Command.TEST_HAPTIC and self.page is Page.DIAGNOSTICS:
            # 只有振动偏好开启时模拟马达脉冲。
            return "haptic" if self.haptic_enabled else "none"
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

    def increment_count(self) -> bool:
        """在 RUNNING 中增加一次指标，并返回是否应产生振动反馈。"""

        # 非训练页不得增长权威指标。
        if self.page is not Page.RUNNING:
            # 返回不振动。
            return False
        # 增加一次动作次数。
        self.count += 1
        # 每次演示增加约 0.118 kcal；该数值仅用于布局变化，不是热量算法。
        self.calories_kcal += 0.118
        # 每次演示计数推进一秒活动时长。
        self.elapsed_seconds += 1
        # 使用小范围周期变化模拟模型置信度刷新。
        self.confidence_percent = 90.0 + float((self.count * 7) % 91) / 10.0
        # 返回当前振动偏好，保证一次计数最多一次反馈。
        return self.haptic_enabled


# 定义不依赖 Tk 的开发直达函数，使无窗口冒烟测试也能覆盖十三页导航合同。
def select_page_for_development(state: PreviewState, page: Page) -> None:
    """把纯状态切到指定页面，不启动倒计时、动画或自动计数。"""

    # 写入开发者明确选择的页面；该操作只用于桌面布局检查。
    state.page = page
    # 清除可能仍亮起的视觉振动，避免开发切页后误认为页面自带黄色边框。
    state.haptic_flash_active = False
    # PREPARE 直达时恢复初始 3 秒，便于检查最大字号数字布局。
    if page is Page.PREPARE:
        # 重置准备倒计时演示值，范围固定为 0～3 秒。
        state.prepare_seconds = 3
    # STOP_CONFIRM 直达时默认从 RUNNING 返回，保证 CANCEL 的演示结果可预测。
    if page is Page.STOP_CONFIRM:
        # 保存取消停止时应恢复的训练页。
        state.stop_resume_page = Page.RUNNING
    # SCREEN_OFF 直达时默认唤醒到 HOME，避免恢复到关机或黑屏形成循环。
    if page is Page.SCREEN_OFF:
        # 保存黑屏点击后的安全恢复页。
        state.screen_resume_page = Page.HOME


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
        return PageSpec("健身助手", f"正在启动{dots}", "", status, "本地智能识别")
    # SELF_TEST 页面列出设备端同样的四项检查。
    if state.page is Page.SELF_TEST:
        # 返回自检页面。
        return PageSpec("设备自检", "正在检查硬件", "屏幕  触摸  惯导  存储", status, "请稍候")
    # HOME 页面提供开始、设置和关机入口。
    if state.page is Page.HOME:
        # 返回主页。
        return PageSpec(
            "健身手柄",
            "设备就绪",
            "11种动作 / 离线识别",
            status,
            "点击开始训练",
            (
                ButtonSpec("开始", Command.START),
                ButtonSpec("设置", Command.OPEN_SETTINGS),
                ButtonSpec("关机", Command.SHUTDOWN, True),
            ),
        )
    # PREPARE 页面显示单个 3、2、1 或倒计时后的 MOVE。
    if state.page is Page.PREPARE:
        # 剩余秒数大于零时显示数字，否则显示“开始动作”。
        primary = str(state.prepare_seconds) if state.prepare_seconds > 0 else "开始动作"
        # 倒计时中显示预热，结束后显示动作锁定提示。
        footer = "惯导预热中" if state.prepare_seconds > 0 else "正在锁定动作..."
        # 返回准备页。
        return PageSpec(
            "准备开始",
            primary,
            "保持手腕放松",
            status,
            footer,
            (ButtonSpec("取消", Command.CANCEL),),
        )
    # RUNNING 页面显示动作、次数、热量、时间和置信度。
    if state.page is Page.RUNNING:
        # 返回训练页。
        return PageSpec(
            "训练中",
            state.action_name,
            f"{state.count} 次   {state.calories_kcal:.3f} 千卡",
            status,
            f"时间 {state.elapsed_seconds // 60:02d}:{state.elapsed_seconds % 60:02d}  置信度 {state.confidence_percent:.2f}%",
            (
                ButtonSpec("暂停", Command.PAUSE),
                ButtonSpec("停止", Command.STOP, True),
            ),
        )
    # PAUSED 页面保留停止前指标。
    if state.page is Page.PAUSED:
        # 返回暂停页。
        return PageSpec(
            "已暂停",
            state.action_name,
            f"{state.count} 次   {state.calories_kcal:.3f} 千卡",
            status,
            "计数已暂停",
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
            f"{state.count} 次合计   {state.calories_kcal:.3f} 千卡",
            status,
            "确认后保存训练记录",
            (
                ButtonSpec("停止", Command.CONFIRM_STOP, True),
                ButtonSpec("取消", Command.BACK),
            ),
        )
    # SUMMARY 页面显示最终总数、热量和时长。
    if state.page is Page.SUMMARY:
        # 返回总结页。
        return PageSpec(
            "训练总结",
            f"{state.count} 次合计",
            f"{state.calories_kcal:.3f} 千卡   {state.elapsed_seconds // 60:02d}:{state.elapsed_seconds % 60:02d}",
            status,
            "训练记录已本地保存",
            (ButtonSpec("完成", Command.DONE),),
        )
    # SETTINGS 页面显示动态亮度、振动和熄屏门槛。
    if state.page is Page.SETTINGS:
        # 把振动布尔值转换为设备端中文开关文本。
        haptic_text = "开" if state.haptic_enabled else "关"
        # 返回设置页。
        return PageSpec(
            "设置",
            f"屏幕亮度 {state.brightness_percent}%",
            f"振动 {haptic_text}   熄屏 {state.screen_timeout_seconds}秒",
            status,
            "已本地保存 / 原配400毫安时",
            (
                ButtonSpec("亮度", Command.CYCLE_BRIGHTNESS),
                ButtonSpec("振动", Command.TOGGLE_HAPTIC),
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
                ButtonSpec("测试振动", Command.TEST_HAPTIC),
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
        return PageSpec("健身助手", "训练记录已保存", f"正在关机{dots}", status, "下次训练见")
    # 理论上枚举已完整覆盖；异常状态直接抛错暴露编程错误。
    raise ValueError(f"unsupported page: {state.page}")


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
        # 创建手动计数按钮，便于检查一次计数一次视觉振动。
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
        # 创建所需字体；Tk 会在缺少指定字体时安全回退系统字体。
        self.font_title = tkfont.Font(family="Microsoft YaHei UI", size=22, weight="bold")
        # 创建主值大字体。
        self.font_primary = tkfont.Font(family="Microsoft YaHei UI", size=38, weight="bold")
        # 创建普通内容字体。
        self.font_secondary = tkfont.Font(family="Microsoft YaHei UI", size=18, weight="bold")
        # 创建状态栏字体。
        self.font_status = tkfont.Font(family="Microsoft YaHei UI", size=14, weight="bold")
        # 创建页脚字体。
        self.font_footer = tkfont.Font(family="Microsoft YaHei UI", size=14)
        # 创建按钮字体。
        self.font_button = tkfont.Font(family="Microsoft YaHei UI", size=14, weight="bold")
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
        # 标记当前为静态开发预览，旧倒计时和动画回调会安全退出。
        self.direct_page_preview = True
        # 通过纯函数切换页面并重置对应演示字段。
        select_page_for_development(self.state, DEVELOPMENT_PAGE_ORDER[selected_index])
        # 立即重绘左侧 410×502 设备画布。
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
        # 振动反馈期间使用黄色外框，否则使用深灰边框。
        border_color = COLOR_HAPTIC_FLASH if self.state.haptic_flash_active else "#17202A"
        # 绘制 AMOLED 背景。
        self.canvas.create_rectangle(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, fill=COLOR_BACKGROUND, outline="")
        # 绘制内部边框；仅模拟马达视觉反馈，不改变页面布局。
        self.canvas.create_rectangle(3, 3, DISPLAY_WIDTH - 4, DISPLAY_HEIGHT - 4, outline=border_color, width=5)
        # 绘制顶部状态栏。
        self.canvas.create_text(
            DISPLAY_WIDTH - 20,
            22,
            text=spec.status,
            fill=COLOR_MUTED,
            font=self.font_status,
            anchor="ne",
        )
        # 绘制页面标题。
        self.canvas.create_text(
            DISPLAY_WIDTH // 2,
            62,
            text=spec.title,
            fill=COLOR_TEXT,
            font=self.font_title,
            anchor="center",
        )
        # BOOT 和 SHUTDOWN 使用圆环图形体现轻量开关机动画。
        if self.state.page in (Page.BOOT, Page.SHUTDOWN):
            # 动画相位决定圆环半径的 0～6 px 呼吸变化。
            pulse = (self.state.animation_phase % 4) * 2
            # 计算圆环中心和半径。
            radius = 45 + pulse
            # 绘制青绿色圆环。
            self.canvas.create_oval(
                DISPLAY_WIDTH // 2 - radius,
                115 - radius,
                DISPLAY_WIDTH // 2 + radius,
                115 + radius,
                outline=COLOR_ACCENT,
                width=3,
            )
        # 绘制中央信息卡片。
        self._rounded_rectangle(20, 112, 390, 322, 20, fill=COLOR_CARD, outline="#1C2733", width=2)
        # 绘制主要文字。
        self.canvas.create_text(
            DISPLAY_WIDTH // 2,
            173,
            text=spec.primary,
            fill=COLOR_ACCENT,
            font=self.font_primary,
            width=340,
            justify=tk.CENTER,
            anchor="center",
        )
        # 绘制次要文字。
        self.canvas.create_text(
            DISPLAY_WIDTH // 2,
            238,
            text=spec.secondary,
            fill=COLOR_TEXT,
            font=self.font_secondary,
            width=340,
            justify=tk.CENTER,
            anchor="center",
        )
        # 绘制页脚提示。
        self.canvas.create_text(
            DISPLAY_WIDTH // 2,
            294,
            text=spec.footer,
            fill=COLOR_MUTED,
            font=self.font_footer,
            width=340,
            justify=tk.CENTER,
            anchor="center",
        )
        # 振动反馈时绘制短暂 VIBRATION 标记。
        if self.state.haptic_flash_active:
            # 文字仅用于桌面理解，真实设备由马达反馈。
            self.canvas.create_text(
                DISPLAY_WIDTH // 2,
                347,
                text="振动反馈",
                fill=COLOR_HAPTIC_FLASH,
                font=self.font_status,
                anchor="center",
            )
        # 绘制页面按钮。
        self._render_buttons(spec.buttons)

    def _render_buttons(self, buttons: tuple[ButtonSpec, ...]) -> None:
        """一到四项绘制单行，五项按三加二绘制双行。"""

        # 无按钮页面直接结束。
        if not buttons:
            # 页面保持只读。
            return
        # 五项设置按钮需要双行；其它页面继续使用原单行布局。
        if len(buttons) == 5:
            # 第一行容纳亮度、振动和诊断三个常用按钮。
            rows = (buttons[:3], buttons[3:])
            # 第一行和第二行使用 350～404、414～468 px，均保留 10 px 间距。
            vertical_bounds = ((350, 404), (414, 468))
        else:
            # 普通页面只有一行按钮。
            rows = (buttons,)
            # 单行保持原 412～466 px 触摸区域。
            vertical_bounds = ((412, 466),)
        # 固定按钮区左右边距，单位 px。
        left = 20
        # 固定按钮区右边界。
        right = DISPLAY_WIDTH - 20
        # 同一行相邻按钮间距固定 7 px。
        gap = 7
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
                # 危险操作使用红色，其它操作使用深灰卡片色。
                fill_color = "#4A2529" if button.dangerous else "#18232D"
                # 危险操作边框使用红色，其它使用强调色。
                outline_color = COLOR_DANGER if button.dangerous else COLOR_ACCENT
                # 绘制圆角按钮底板。
                self._rounded_rectangle(x1, top, x2, bottom, 14, fill=fill_color, outline=outline_color, width=2)
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
        """执行纯状态命令并安排倒计时、训练或反馈动画。"""

        # 用户点击设备屏按钮后退出静态开发预览，恢复真实交互流程与定时副作用。
        self.direct_page_preview = False
        # 先让纯状态机处理页面和设置变化。
        effect = self.state.dispatch(command)
        # 立即渲染命令结果。
        self.render()
        # START 需要启动每秒 PREPARE 更新。
        if effect == "start_prepare":
            # 一秒后把 3 更新为 2。
            self.root.after(1000, self._prepare_tick)
        # RESUME 或 WAKE 到 RUNNING 需要恢复自动计数。
        if effect == "start_running":
            # 1.2 秒后生成下一次演示计数。
            self.root.after(1200, self._running_tick)
        # 设置或诊断振动使用视觉边框替代马达。
        if effect == "haptic":
            # 触发一次 180 ms 视觉反馈。
            self.flash_haptic()
        # SHUTDOWN 播放短动画并最终进入 SCREEN_OFF。
        if effect == "shutdown":
            # 启动关机点动画。
            self._animate_shutdown()
            # 1.2 秒后模拟 PMIC 断电为纯黑屏。
            self.root.after(1200, self._complete_shutdown)

    def _prepare_tick(self) -> None:
        """每秒推进一次真实 3、2、1 倒计时。"""

        # 开发导航直达 PREPARE 时保持静态 3，不执行自动倒计时。
        if self.direct_page_preview:
            # 停止旧定时回调链。
            return
        # 用户取消后旧定时器安全退出。
        if self.state.page is not Page.PREPARE:
            # 不再安排下一轮。
            return
        # 剩余秒数大于零时减一。
        if self.state.prepare_seconds > 0:
            # 推进到下一秒。
            self.state.prepare_seconds -= 1
        # 重绘新的数字或 MOVE。
        self.render()
        # 倒计时尚未结束时继续等待一秒。
        if self.state.prepare_seconds > 0:
            # 安排下一次递减。
            self.root.after(1000, self._prepare_tick)
            # 当前回调结束。
            return
        # 倒计时结束后保留 MOVE 650 ms，模拟动作锁定阶段。
        self.root.after(650, self._finish_prepare)

    def _finish_prepare(self) -> None:
        """在 PREPARE 未被取消时进入 RUNNING。"""

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
        # 增加一次计数并取得振动偏好。
        should_haptic = self.state.increment_count()
        # 先绘制新次数。
        self.render()
        # 每次有效计数至多触发一次视觉振动。
        if should_haptic:
            # 启动短边框闪烁。
            self.flash_haptic()
        # 安排下一次演示计数。
        self.root.after(1200, self._running_tick)

    def flash_haptic(self) -> None:
        """用 180 ms 黄色边框代替真实马达振动。"""

        # 开启视觉反馈状态。
        self.state.haptic_flash_active = True
        # 立即重绘高亮边框。
        self.render()
        # 180 ms 后关闭高亮；与设备端计数反馈动画时长一致。
        self.root.after(180, self._clear_haptic_flash)

    def _clear_haptic_flash(self) -> None:
        """结束视觉振动反馈。"""

        # 清除反馈状态。
        self.state.haptic_flash_active = False
        # 重绘普通边框。
        self.render()

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
        """手动增加一次计数，便于检查视觉振动反馈。"""

        # 只在 RUNNING 中增加指标。
        should_haptic = self.state.increment_count()
        # 重绘可能变化的指标。
        self.render()
        # 开启偏好时产生一次视觉振动。
        if should_haptic:
            # 触发短反馈。
            self.flash_haptic()

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
    # START 必须进入 PREPARE 并显示 3。
    assert state.dispatch(Command.START) == "start_prepare"
    # 检查准备页。
    assert state.page is Page.PREPARE and build_page_spec(state).primary == "3"
    # 模拟倒计时结束。
    state.prepare_seconds = 0
    # 零秒必须显示“开始动作”，而不是静态 3 2 1 文本。
    assert build_page_spec(state).primary == "开始动作"
    # 模拟动作锁定进入训练。
    state.page = Page.RUNNING
    # 增加一次应得到 count=1 且允许振动。
    assert state.increment_count() and state.count == 1
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
    # 校验诊断页三个命令。
    diagnostics = build_page_spec(state)
    # 检查按钮数量和振动测试入口。
    assert diagnostics.buttons[0].command is Command.TEST_HAPTIC
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
    parser = argparse.ArgumentParser(description="410x502 AMOLED 健身手柄 UI 桌面预览器")
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
    # 解析当前进程参数。
    arguments = parser.parse_args()
    # CI/无显示环境执行纯逻辑测试。
    if arguments.smoke_test:
        # 运行断言。
        run_smoke_test()
        # 测试完成后返回。
        return
    # 创建 Tk 根窗口。
    root = tk.Tk()
    # 创建完整预览应用并保持引用到主循环结束。
    _application = WatchUiPreviewApp(root)
    # 开发者要求直接检查配对页时，在 Tk 主循环开始后安全触发同一个生产语义演示入口。
    if arguments.pairing_preview:
        # 延迟 100 ms 等待首帧控件完成布局，随后显示零填充六位码覆盖层。
        root.after(100, _application.simulate_pairing_code)
    # 进入 Tk 事件循环；所有回调在同一 UI 线程串行执行。
    root.mainloop()


# 仅直接执行文件时启动，不在单元导入时创建窗口。
if __name__ == "__main__":
    # 调用统一入口。
    main()
