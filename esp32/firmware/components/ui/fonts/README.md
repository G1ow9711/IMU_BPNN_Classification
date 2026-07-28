# LVGL 中文字体子集

设备屏幕的用户可见汉字使用 Noto Sans SC 子集。Montserrat 只保留给拉丁字符和内部数值。四个字号分别承担：

- `16 px`：状态栏、页脚、触摸按钮；
- `20 px`：页面标题、次数和次级指标；
- `28 px`：动作名称、倒计时和主要数值；
- `36 px`：主页主状态和联调版标记。

每个字体使用 `2 bpp` 非压缩位图（`bitmap_format=0`），同时包含 ASCII `0x20～0x7E`、`ui_presenter.c` 与 `ui_lvgl_renderer.c` 当前全部用户可见汉字和常用全角标点。非压缩格式对齐厂家稳定中文字体基线，避免真板对压缩字形解码后出现重影。字符集、文件长度和 SHA-256 记录在 `ui_font_manifest.json`。

## 重新生成

先安装 Node.js，并准备 SIL Open Font License 1.1 授权的 Noto Sans SC 字体，然后在工作树根执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File esp32\firmware\tools\generate_lvgl_ui_fonts.ps1
```

脚本只从 `esp32/firmware/components/ui/ui_presenter.c` 与 `ui_lvgl_renderer.c` 的 C 字符串字面量提取汉字。修改设备可见文案后必须重新生成；禁止手改四个字体 C 文件中的位图、字形描述或 Unicode 映射。

## 许可证与审计

字体依照 SIL Open Font License 1.1 使用，完整许可见 [OFL-1.1.txt](OFL-1.1.txt)，上游许可见 [Noto CJK LICENSE](https://github.com/notofonts/noto-cjk/blob/main/Sans/LICENSE)。

发布前应核对：

- 四个字号固定为 `16/20/28/36 px`、`2 bpp` 和非压缩 `bitmap_format=0`；
- manifest 文件长度与 SHA-256 和真实生成物一致；
- `ui_presenter.c` 与 `ui_lvgl_renderer.c` 的全部可见汉字均在子集中；
- 许可证、生成头和 `lv_font_t` 符号存在。

当前四个自动生成 C 源文件合计约 801 KiB；编译后的中文字形位图约 41 KiB，另有少量字形描述与 Unicode 映射。权重和字体均作为只读常量进入 Flash，不占用等量运行时堆。
