# 智能手表界面桌面预览器

本目录位于文档工程内，提供仅依赖 Python 标准库 Tkinter 的 410×502 AMOLED 预览工具，用于检查“ESP32智慧运动助手”的智能手表产品界面。

## 当前视觉系统

- 石墨黑 AMOLED 背景，减少发光面积；
- 科技蓝作为唯一主强调色，青色用于 BLE、充电和智能识别状态；
- 左上角“32”平台方标、`ESP32` 平台品牌和 `智慧运动助手` 产品名称；
- 右上角真实电池图形、动态填充、百分比、充电色和 BLE 状态；
- 圆角训练主卡、状态胶囊、大号动作名、大号权威次数、时间/热量/置信度；
- 训练、休息、警告、故障使用不同语义色；
- 普通页面使用大触摸按钮，设置页使用二乘二功能网格；
- 休息或实时窗口不确定时，主动作和累计次数保持不变，只显示“计数暂停”。

手表没有振动马达。预览器和固件都不提供振动开关、测试入口或模拟振动反馈。

## 重要边界

本工具是布局与交互流程预览器，不是 LVGL 数值一致性验证器，也不是 ESP32 仿真器。

它可以验证：

- 410×502 内容区域的品牌顶栏、训练主卡和按钮布局；
- BOOT、SELF_TEST、HOME、PREPARE、RUNNING、PAUSED、STOP_CONFIRM、SUMMARY、SETTINGS、DIAGNOSTICS、SCREEN_OFF、ERROR、SHUTDOWN 共 13 个生产页面；
- PREPARE 已开始采样且明确右手佩戴，不插入人为倒计时；
- RUNNING 的主动作稳定、权威次数逐次增加、休息时累计保持；
- 首次 STOP 只进入确认页，确认后才进入 SUMMARY；
- 电量、BLE、亮度、熄屏、诊断和解绑状态；
- 熄屏后点击屏幕恢复原页面。

它不能验证：

- LVGL 9 的真实字体字宽、抗锯齿和对象坐标舍入；
- 显示与触摸驱动、AMOLED 亮度、ESP32-S3 帧率、任务栈和功耗；
- 固件 presenter 与 LVGL renderer 的逐像素一致性；
- 真板分类、计数、BLE 或电池采样准确性。

最终界面仍须通过 LVGL 编译边界、字体覆盖、ESP-IDF 真链接和真板触摸/显示验收。

## 启动

需要 Python 3.10 或更高版本且包含 Tkinter。无需安装第三方包。

```powershell
python docs\tools\watch_ui_preview\watch_ui_preview.py
```

左侧 410×502 区域代表设备 AMOLED。右侧页面导航和演示控制均是开发工具，不属于设备屏幕。

## 基本流程

1. 启动后播放 BOOT 和 SELF_TEST。
2. HOME 点击“开始”，PREPARE 立即采样并提示开始做动作。
3. 识别稳定后进入 RUNNING，每约 1.2 秒模拟一次完整动作计数。
4. 暂停或休息时，动作名和次数保持；恢复后从新完整周期继续。
5. 点击“停止”进入二次确认，确认后保存并显示 SUMMARY。
6. 设置页提供亮度、诊断、忘记电脑和返回；诊断页提供熄屏时间和返回。

右侧控制可重播开机、模拟熄屏、增加一次计数、切换动作、切换 BLE、显示六位配对码和降低电量。

快捷键：

- `R`：重播开机动画；
- `O`：模拟熄屏；
- `Space`：在 RUNNING 页面增加一次计数。

## 冒烟检查

```powershell
python -m py_compile docs\tools\watch_ui_preview\watch_ui_preview.py
python docs\tools\watch_ui_preview\watch_ui_preview.py --smoke-test
```

冒烟成功标志：

```text
WATCH_UI_PREVIEW_SMOKE_OK pages=13 display=410x502 nav=13 language=zh-CN
```

## 与固件同步

产品文字和状态来自：

- `esp32/firmware/components/ui/ui_presenter.c`
- `esp32/firmware/components/ui/include/ui_presenter.h`
- `esp32/firmware/components/ui/include/ui_state_machine.h`

真实像素布局和颜色来自：

- `esp32/firmware/components/ui/ui_lvgl_renderer.c`

预览器中的自动计数、热量和置信度仅用于布局变化，不属于正式算法。
