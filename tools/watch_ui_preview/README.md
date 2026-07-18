# 手柄界面桌面预览器

本目录提供一个仅依赖 Python 标准库 Tkinter 的桌面预览工具，用于在没有开发板时检查 410×502 AMOLED 的页面层级、文字密度、按钮位置、倒计时、停止确认和轻量动画。

## 重要边界

本工具是**布局与交互流程预览器**，不是 LVGL 数值一致性验证器，也不是 ESP32 仿真器。

它可以验证：

- 410×502 内容区域能否容纳当前文字和最多四个按钮；
- BOOT、SELF_TEST、HOME、PREPARE、RUNNING、PAUSED、STOP_CONFIRM、SUMMARY、SETTINGS、DIAGNOSTICS、SCREEN_OFF、ERROR、SHUTDOWN 共 13 个生产页面是否可达；
- PREPARE 是否按 `3 → 2 → 1 → 开始动作` 更新；
- 首次 STOP 是否只进入确认页，确认后才进入 SUMMARY；
- 设置页亮度、振动和诊断入口是否能演示；
- 有效计数是否只产生一次视觉振动反馈；
- 熄屏后点击屏幕是否恢复原页面。

它不能验证：

- LVGL 9 的真实字体字宽、抗锯齿和对象坐标舍入；
- SH8601/兼容显示驱动的颜色、刷新率和 AMOLED 亮度；
- CST9220/兼容触摸驱动的坐标、抖动和误触；
- ESP32-S3 的任务调度、内存、帧率和功耗；
- 固件 `ui_presenter.c` 与 LVGL renderer 的逐像素一致性；
- 真机马达强度。本工具用 180 ms 黄色边框和“振动反馈”字样代替马达。

最终界面仍需在厂家原配开发板和电池上烧录验收。

## 环境

- Python 3.10 或更高版本；
- Python 安装需包含 Tkinter；
- 无第三方 Python 包，无需执行 `pip install`。

检查 Tkinter：

```powershell
python -c "import tkinter; print(tkinter.TkVersion)"
```

## 启动

从仓库根目录执行：

```powershell
python tools\watch_ui_preview\watch_ui_preview.py
```

左侧黑色区域严格按 410×502 像素创建，代表设备 AMOLED 内容。右侧“全部页面（开发导航）”和“桌面演示控制”均位于设备区域之外，不属于设备屏幕设计。

## 全部页面开发导航

屏幕右侧第一栏列出生产状态机全部 13 个 `Page`。单击任一项可直接显示对应页面：

- 直达页面保持静态，不会立即被开机、自检、倒计时、自动计数或关机定时器覆盖；
- `PREPARE` 直达固定显示初始数字 3；
- `ERROR` 显示示例故障码、训练禁用提示和安全关机按钮；
- `SCREEN_OFF` 保持纯黑，单击左侧屏幕可唤醒；
- 再点击左侧设备按钮后退出静态预览，继续按真实交互流程运行。

导航栏明确标注“仅开发工具，不属于设备界面”。它解决某些页面只能经过定时流程、正常演示中停留时间太短而不易检查的问题。

模拟屏内所有用户可见标题、正文、状态、按钮和 11 类动作名均使用中文。右侧开发导航也只显示“开机动画”“主页”“训练中”等中文页面名；`BOOT`、`RUNNING` 等内部枚举只保留在代码和日志中，不再出现在预览窗口。

## 基本演示流程

1. 启动后自动播放约 800 ms BOOT 和约 350 ms SELF_TEST。
2. 在 HOME 点击“开始”。
3. PREPARE 显示 `3、2、1`，结束后短暂显示“开始动作”，随后进入 RUNNING。
4. RUNNING 每约 1.2 秒模拟一次有效计数；开启振动时，每次只闪烁一次黄色边框。
5. 点击“暂停”可冻结指标，点击“继续”恢复。
6. 点击“停止”只进入 STOP_CONFIRM；再次点击红色“停止”才进入 SUMMARY，点击“取消”返回此前 RUNNING 或 PAUSED。
7. HOME 的“设置”可循环亮度、切换振动并进入 DIAGNOSTICS。
8. DIAGNOSTICS 可测试视觉振动、循环熄屏时间并返回设置页。
9. 主页的“关机”按钮播放关机动画，随后进入屏幕关闭状态。

## 桌面辅助控制

右侧按钮支持：

- 重播开机动画；
- 模拟屏幕关闭；
- 手动增加一次计数；
- 在 11 类动作名之间切换；
- 切换蓝牙状态；
- 模拟蓝牙六位配对码；
- 逐次降低电量；
- 关闭预览器。

快捷键：

- `R`：重播开机动画；
- `O`：模拟熄屏；
- `Space`：在 RUNNING 页面手动增加一次计数。

SCREEN_OFF 页面保持纯黑，点击左侧模拟屏可唤醒。

## 无窗口冒烟测试

以下命令不会创建 Tk 窗口，适合自动验证页面集合和关键状态流：

```powershell
python tools\watch_ui_preview\watch_ui_preview.py --smoke-test
```

成功标志：

```text
WATCH_UI_PREVIEW_SMOKE_OK pages=13 display=410x502 nav=13 language=zh-CN
```

语法检查：

```powershell
python -m py_compile tools\watch_ui_preview\watch_ui_preview.py
```

## 与固件同步

页面标题、主信息、次信息、页脚和屏内按钮语义参考：

- `esp32/firmware/components/ui/ui_presenter.c`
- `esp32/firmware/components/ui/include/ui_presenter.h`
- `esp32/firmware/components/ui/include/ui_state_machine.h`

若固件新增页面、修改中文按钮或调整设置档位，应同步修改本工具的 `Page`、`Command`、`build_page_spec()`、开发导航和冒烟测试。预览器的热量增长和置信度变化仅为布局演示数据，不属于正式识别或卡路里算法。
