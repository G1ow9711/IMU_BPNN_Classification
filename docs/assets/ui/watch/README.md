# 手表界面预览图

这些图片由 `docs/tools/watch_ui_preview/watch_ui_preview.py` 使用 Pillow 离屏绘制，固定复现当前手表页面的产品语义、信息层级和交互状态。它们不是对 LVGL 帧缓冲的逐像素抓取，也不能替代真板显示、触摸、圆角安全区和刷新稳定性验收。

## 生成

交互预览使用 Python 标准库 Tk；PNG 导出还需要 Pillow 和可用中文字体。在仓库根目录执行：

```powershell
python .\docs\tools\watch_ui_preview\watch_ui_preview.py --export-all .\docs\assets\ui\watch
```

成功标记：

```text
WATCH_UI_PNG_EXPORT_OK count=8 size=410x502
```

单页导出示例：

```powershell
python .\docs\tools\watch_ui_preview\watch_ui_preview.py --export-png .\docs\assets\ui\watch\running_rest.png --page RUNNING --rest-preview
```

## 页面

| 文件 | 用途 |
|---|---|
| `home.png` | 品牌、连接、电池和训练入口 |
| `prepare.png` | 点击开始后的即时采样和动作识别 |
| `running.png` | 稳定动作、实时次数、时长和热量 |
| `running_rest.png` | 休息门生效，动作和累计保持不变 |
| `settings.png` | 亮度、诊断、忘记电脑和返回入口 |
| `pairing.png` | 中文六位配对码与超时提示 |
| `summary.png` | 最终次数、时长和热量总结 |
| `error.png` | 稳定故障码、训练阻断和安全关机 |

图片固定为 `410×502 RGB`。批量导出同时生成 `manifest.json`，记录生成器源码 SHA-256、页面变体、尺寸和图片摘要；清单明确标注 `PixelEquivalentToLvgl=false`。

## 真板验收边界

文档发布前仍需独立验证：

- AMOLED 文字边缘和颜色；
- 圆角安全区、触摸坐标与返回路径；
- 连续刷新无重影、卡死或看门狗复位；
- BLE 状态与页面提示一致；
- COM 端口和供电在连续操作中稳定。
