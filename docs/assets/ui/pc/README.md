# 上位机界面截图

这些图片由 `pc/FitnessCoach.UiCapture` 使用生产 `FitnessCoach.App` 的 XAML、控件、主题和 ViewModel 离屏生成。生成器使用固定内存数据与 WPF `RenderTargetBitmap`，不会启动正式 `App.OnStartup`，不会创建 Windows BLE 会话，也不会读取或修改 `%LOCALAPPDATA%\IMUFitness`。

## 生成

在仓库根目录执行：

```powershell
dotnet run --project .\pc\FitnessCoach.UiCapture\FitnessCoach.UiCapture.csproj -c Release -- --output .\docs\assets\ui\pc
```

如果本机只安装更高主版本的 Windows Desktop Runtime，可在当前终端临时设置：

```powershell
$env:DOTNET_ROLL_FORWARD = 'Major'
```

成功标记：

```text
PC_UI_CAPTURE_OK pages=6 size=1440x900 mode=deterministic-mock
```

## 页面

| 文件 | 用途 |
|---|---|
| `pc-device.png` | 确定性模拟设备、连接状态和设备摘要 |
| `pc-live-training.png` | 深蹲动作示范、8 次累计、时长和热量 |
| `pc-training-monitor.png` | 25 Hz、250 点六轴曲线与分类诊断 |
| `pc-history.png` | 固定三条教程会话及详情 |
| `pc-settings.png` | 公制、亮度、熄屏和减少动画 |
| `pc-summary.png` | 最近会话的训练总结和保存状态 |

所有图片固定为 `1440×900`、96 DPI。`manifest.json` 使用 `SchemaVersion`、生成器版本和排序源码内容哈希描述来源，不记录 Git HEAD 或时间戳。

## 安全边界

- 模拟设备 ID 固定为 `DOCS-MOCK-01`。
- 六轴曲线由固定数学波形生成，单位和 25 Hz 采样合同与生产页面一致。
- 训练摘要、分类置信度和计数标记均为教程数据。
- 生成器只写 `--output` 指定目录；目标目录之外不产生偏好或会话文件。
