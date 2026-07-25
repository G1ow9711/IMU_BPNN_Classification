# ESP32-S3 智能手表端

本目录保存 Waveshare ESP32-S3-Touch-AMOLED-2.06 智能手表固件、最终双 M0 模型、板级驱动、主机替身测试和烧录入口。真表没有振动马达；计数反馈只通过 AMOLED 数字和 BLE 事件呈现，旧协议马达位固定为零。

## 1. 实时数据流

```text
QMI8658 异步原始帧
  -> 8 Hz 抗混叠与严格 25 Hz 重采样
  -> 62 点六轴窗口，12 点步长
  -> 单轴孤立尖峰清洗
  -> 297 项手工特征
  -> 基础六分支 M0 + 掩码六分支 M0
  -> 0.85 / 0.15 logits 融合
  -> 动作段因果累计
  -> 动作相位、次数/步数/时长、卡路里
  -> AMOLED、BLE 与 LittleFS
```

六轴通道顺序固定为 `gx、gy、gz、ax、ay、az`。角速度单位为 `deg/s`，加速度单位为 `g`。Python 与 C 的特征名称、顺序、标准化参数、网络层和类别顺序均由同一导出包约束，禁止在固件端自行重排。

## 2. 最终模型产物

`include/` 中的正式部署文件：

- `esp32_bp_features.h`：单轴去毛刺、297 项特征和动作段累计基础函数；
- `esp32_dual_m0_model.h`：两个六分支 M0 的权重、两套标准化参数、`184:232` 掩码、固定融合和前向函数；
- `dual_m0_bundle.npz`：便携式双模型数组包；
- `dual_m0_manifest.json`：维度、类别、权重、掩码范围和 SHA-256 清单。

单个部署 M0 为 12,523 个主路径参数和 12,336 次 MAC；双模型共 25,046 个参数、24,672 次 MAC。float32 主路径权重与偏置约 100,184 字节，两套标准化参数约 4,752 字节。

已完成 C99/Python 逐值验证：297 项特征最大绝对误差不超过 `1e-4`，双模型及融合 logits 不超过 `1e-3`，最终类别索引完全一致。

## 3. 固件工程

工程根为 `firmware/`，生产入口为 `firmware/main/main.c`。入口已经串联：

- NVS、厂家 BSP、LVGL 9、触摸和 AMOLED；
- 13 个中文 LVGL 状态页面和 16/20/28/36 px Noto Sans SC 字体子集；
- QMI8658、AXP2101、PCF85063；
- IMU pipeline、双 M0、动作相位和训练引擎；
- 计数、步数、坐姿时长和卡路里；
- NimBLE GATT、实时状态、事件、命令和会话补传；
- 六位配对码中文屏显、超时/成功/失败/断线清除和设置页“忘记电脑”；
- LittleFS 双槽摘要存储；
- 真实 AMOLED Display On/Off、FT3168 hibernate/wake、亮屏、熄屏训练、连接待机、Deep-sleep 和 PMIC 关机状态机。

启动顺序固定为“板级显示与 LVGL → 开机/自检页可见 → 外部传感器与会话存储 → 业务任务”。因此 QMI8658、AXP2101、RTC、会话存储或业务域初始化失败时，设备可进入中文 ERROR 页；显示本身初始化失败时没有可用屏幕，只能保留串口错误。

BLE 控制与会话传输使用独立 16 KiB 片内任务栈；最大帧缓冲以非内联边界拆分，并在每次业务工作后记录历史最小剩余栈字节。该容量覆盖 Windows 首次绑定后连续读取能力清单和状态快照的突发，禁止再使用已在真板溢出的 8 KiB 边界。

训练引擎在准备态同时维护独立窗口和累计证据：当前 62 点窗口 Top-1 连续两窗一致且每窗概率至少 50% 时锁定；到第四窗仍不稳定时才按累计平均 logits 的 argmax 兜底。该门避免强错误首窗通过累计惯性压住后续真实动作。锁定后的 `selected_action` 固定整轮计数器、会话摘要和界面动作；后续 `inferred_action` 只进入诊断日志，不因静坐或分类噪声切类。计数许可由训练集同源的 25 点运动/静止迟滞门和数据质量控制：完整休息窗冻结并清空未完成半周期，恢复活动后继续同一主动作。八类重复动作统一使用方向与幅度缓慢自适应的右腕“主向—回向—闭合”完整周期；walk/trot 使用高峰触发、回落慢基线后重新武装的施密特步峰。START、成功 RESUME、IMU RESET/GAP 会清除不能跨边界拼接的窗口和未完成周期，不回滚已发布累计。

厂家原配电池固定按 400 mAh 设计，并按 320 mAh 可用容量做保守预算。软件门槛为 15% 提示、8% 禁止新训练、5% 保存后安全关机。顶栏持续显示电池图标、百分比、充电色和 BLE 状态。

## 4. 构建

项目把 ESP-IDF 5.5.4、编译器、Python 环境和临时目录放在工作树 `.codex-local/`，避免污染系统环境。准备好项目本地工具链后，在仓库根执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  esp32\firmware\tools\idf_project.ps1 -Action build
```

当前已验证结果：

- `imu_fitness_handheld.bin`：`0x13def0` 字节，即 1,302,256 B；
- 最小应用分区：`0x400000` 字节；
- 剩余应用分区：`0x2c2110` 字节，约 69%；
- bootloader：`0x5700` 字节。

中文字体由 `tools/generate_lvgl_ui_fonts.ps1` 生成；修改设备可见文案后必须重跑脚本，并通过 manifest、哈希、许可证和缺字覆盖审计。

固件镜像默认位于 `firmware/build/`。构建成功只证明软件依赖、编译和链接闭环，不代表真实板卡外设已通过。

## 5. 后续烧录

连接开发板并确认串口后执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  esp32\firmware\tools\idf_project.ps1 `
  -Action flash-monitor `
  -Port COM7
```

把 `COM7` 换成设备管理器中实际串口。脚本不会自动猜串口，避免误刷其它设备。串口监视使用 `Ctrl+]` 退出。

烧录后必须验证：AMOLED 与触摸坐标、QMI8658 的 `0x6A/0x6B` 探测、RTC、AXP2101 SOC/关机、BLE 配对与 MTU、LittleFS 掉电恢复、实际功耗、任务栈/堆和一小时稳定运行。详见[手表与上位机软件详细设计](../docs/手柄与上位机软件详细设计.md)。

## 6. 无硬件测试

在仓库根执行全部 ESP32 主机替身测试：

```powershell
$env:TEMP = (Resolve-Path '.codex-local\tmp').Path
$env:TMP = $env:TEMP
powershell -NoProfile -ExecutionPolicy Bypass -File `
  esp32\host_tests\run_all.ps1
```

当前总入口包含 12 组：BLE、传感器、板级/UI、设备配置、计数与卡路里、IMU pipeline、动作相位、功耗、会话传输、会话存储、训练引擎和应用协调器。主机测试不能替代真实电流、射频、触摸和传感器测试。

## 7. 文档入口

- [完整算法文档](../docs/算法文档.md)
- [IMU 采样、重采样与推理链](../docs/IMU采样重采样与推理链.md)
- [计数与卡路里算法](../docs/计数与卡路里算法.md)
- [设备 UI 与低功耗](../docs/设备UI与低功耗.md)
- [设备端 BLE 服务](../docs/设备端BLE服务.md)
- [BLE 通信协议](../docs/BLE通信协议.md)
- [设备配置与命令 TLV v1](../docs/设备配置与命令TLV.md)
- [板载传感器驱动](../docs/板载QMI8658_AXP2101_PCF85063驱动.md)
