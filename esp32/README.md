# ESP32-S3 智能手表固件

本目录保存 Waveshare `ESP32-S3-Touch-AMOLED-2.06` 手表固件、最终双 M0 模型、板级适配、主机测试和发布入口。

## 1. 当前版本边界

- 目标芯片：ESP32-S3。
- 固件框架：ESP-IDF 5.5.4。
- 厂家 BSP：`waveshare/esp32_s3_touch_amoled_2_06` 2.0.0。
- UI：LVGL 9.5.0、410×502 AMOLED、中文字体子集。
- 当前受管驱动链：SH8601 + FT5x06/FT3168 兼容路径。
- 传感器：QMI8658、AXP2101、PCF85063。
- Flash：32 MiB；PSRAM：8 MiB Octal。
- 佩戴域：右手腕。
- 会话合同：一轮只做一种主动作；次数和步数动作在休息、静止和无效数据时由活动门与质量门冻结，分类噪声只更新诊断类别；`sit` 在运行态按合法单调 tick 累计时长。
- 手表没有振动马达。计数反馈只使用 AMOLED 数字与 BLE EventV1；协议保留位固定为零。
- 当前源码启用 `APP_BENCH_ALWAYS_ON=true`。自动熄屏、Light-sleep 和 Deep-sleep 暂时关闭，开发者诊断门只在 RAM 中开启。本镜像是常亮功能联调版，不代表低功耗产品版已经通过。
- 当前 BLE 联调 PIN 固定为 `123456`，仍启用认证、加密和绑定；量产前必须替换固定 PIN 方案。

## 2. 目录结构

```text
esp32/
├─ README.md
├─ include/
│  ├─ esp32_bp_features.h
│  ├─ esp32_dual_m0_model.h
│  ├─ dual_m0_bundle.npz
│  └─ dual_m0_manifest.json
├─ firmware/
│  ├─ main/
│  │  ├─ main.c
│  │  └─ idf_component.yml
│  ├─ components/
│  ├─ partitions.csv
│  ├─ sdkconfig.defaults
│  ├─ dependencies.lock
│  └─ tools/idf_project.ps1
└─ host_tests/
   └─ run_all.ps1
```

权威入口：

- 生产应用：[`firmware/main/main.c`](firmware/main/main.c)
- 组件版本：[`firmware/dependencies.lock`](firmware/dependencies.lock)
- 默认构建配置：[`firmware/sdkconfig.defaults`](firmware/sdkconfig.defaults)
- 分区表：[`firmware/partitions.csv`](firmware/partitions.csv)
- ESP-IDF 操作脚本：[`firmware/tools/idf_project.ps1`](firmware/tools/idf_project.ps1)
- 十二组主机测试：[`host_tests/run_all.ps1`](host_tests/run_all.ps1)

## 3. 实时数据流

```text
QMI8658 异步原始帧
  -> 加速度与角速度按各自硬件时间戳进入 IMU pipeline
  -> 抗混叠与严格 25 Hz 六轴重采样
  -> 62 点窗口，12 点步长
  -> 单轴孤立尖峰清洗
  -> 297 项手工特征
  -> 基础六分支 M0 + 掩码六分支 M0
  -> 0.85 / 0.15 logits 融合
  -> 准备态因果证据确认主动作
  -> 活动/静止门与动作相位
  -> 次数、步数或持续时间
  -> AMOLED、BLE EventV1 与 LittleFS
```

六轴顺序固定为 `gx、gy、gz、ax、ay、az`。角速度单位为 `deg/s`，加速度单位为 `g`。Python 和 C 的特征顺序、标准化参数、分支维度、类别顺序和融合权重必须来自同一导出包。

当前主动作语义：

1. 准备态使用已完成分类窗确认 `selected_action`。
2. `selected_action` 固定本轮计数器、界面和摘要。
3. 主动作确认后的每窗 `inferred_action` 只写诊断。
4. 对次数和步数动作，完整休息窗或低质量数据冻结并清除未完成半周期；异类或低置信分类只写诊断值。`sit` 不使用动态活动门，暂停或停止后才结束时长累计。
5. 恢复活动后继续同一主动作，必须重新完成一个完整周期。
6. 停止后不再接受新的计数事件。

完整公式和计数状态见[算法原理、训练与实时计数](../docs/算法原理、训练与实时计数.md)。

## 4. 硬件与版本链

禁止只看目录名猜控制器。当前构建版本必须同时核对：

```powershell
Get-Content -LiteralPath 'esp32\firmware\dependencies.lock'
Get-Content -LiteralPath 'esp32\firmware\main\idf_component.yml'
Get-Content -LiteralPath 'esp32\firmware\sdkconfig.defaults'
```

当前锁定链：

| 项目 | 版本/配置 |
|---|---|
| ESP-IDF | 5.5.4 |
| Waveshare BSP | 2.0.0 |
| LVGL | 9.5.0 |
| LittleFS | 1.22.2 |
| 面板驱动 | `waveshare/esp_lcd_sh8601` 2.0.0 |
| 触摸驱动 | `espressif/esp_lcd_touch_ft5x06` 1.1.0~1 |
| Flash 模式/容量 | QIO / 32 MiB |
| PSRAM | 8 MiB Octal、80 MHz |

仓库同时保留 CO5300/CST9220 的板型描述，但当前受管 BSP 没有把该路径接入生产运行时。不得把 SH8601/FT3168 与 CO5300/CST9220 的初始化命令、I2C 地址或触摸坐标配置混用。

## 5. 构建环境

操作脚本只接受项目本地工具链：

```text
.codex-local/cache/esp-idf-v5.5.4
.codex-local/cache/espressif-tools
.codex-local/tmp
```

预检：

```powershell
if (-not (Test-Path -LiteralPath `
    '.codex-local\cache\esp-idf-v5.5.4\tools\idf.py')) {
    throw '缺少项目本地 ESP-IDF 5.5.4'
}

if (-not (Test-Path -LiteralPath `
    '.codex-local\cache\espressif-tools')) {
    throw '缺少项目本地 ESP-IDF 工具链'
}
```

脚本不静默回退到系统其它 ESP-IDF，也不会自动猜串口。

## 6. 测试与构建

### 6.1 固件主机测试

从仓库根执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  esp32\host_tests\run_all.ps1 -StopOnFailure
```

通过标志：

```text
HOST_TESTS_OK count=12
```

十二组覆盖 BLE、板载传感器、UI runtime、设备配置、健身核心、IMU pipeline、动作相位、功耗/UI、会话传输、会话存储、训练引擎和总协调器。

### 6.2 ESP-IDF 构建

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  esp32\firmware\tools\idf_project.ps1 `
  -Action build
```

完整清理只在构建图或配置缓存确实损坏时使用：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  esp32\firmware\tools\idf_project.ps1 `
  -Action fullclean
```

构建结果位于 `esp32/firmware/build/`。该目录可再生且不进入 Git。

每次构建后读取当前事实：

```powershell
Get-Item -LiteralPath `
  'esp32\firmware\build\imu_fitness_handheld.bin' |
  Select-Object FullName, Length, LastWriteTime

Get-FileHash -Algorithm SHA256 -LiteralPath `
  'esp32\firmware\build\imu_fitness_handheld.bin'

Get-Content -LiteralPath 'esp32\firmware\build\flash_args'
```

README 不固定记录镜像大小或 SHA；发布结果必须来自本次命令输出。

## 7. 烧录与串口监视

### 7.1 确认目标 USB

目标必须精确匹配 `VID_303A&PID_1001`：

```powershell
$targets = @(
    Get-CimInstance Win32_PnPEntity |
        Where-Object {
            ($_.PNPDeviceID -match 'VID_303A&PID_1001') -and
            ($_.Name -match '\(COM\d+\)')
        }
)

if ($targets.Count -ne 1) {
    throw "目标设备数量不是 1，实际=$($targets.Count)"
}

$targets | Select-Object Name, PNPDeviceID

if ($targets[0].Name -notmatch '\((COM\d+)\)') {
    throw '无法解析目标 COM 端口'
}

$port = $Matches[1]
```

### 7.2 烧录

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  esp32\firmware\tools\idf_project.ps1 `
  -Action flash `
  -Port $port
```

当前四段布局：

| 地址 | 镜像 |
|---:|---|
| `0x0` | `bootloader/bootloader.bin` |
| `0x8000` | `partition_table/partition-table.bin` |
| `0x10000` | `ota_data_initial.bin` |
| `0x30000` | `imu_fitness_handheld.bin` |

烧录通过必须出现四次 `Hash of data verified`，随后执行硬复位。少一段都不能写“烧录成功”。

### 7.3 串口监视

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  esp32\firmware\tools\idf_project.ps1 `
  -Action monitor `
  -Port $port
```

使用 `Ctrl+]` 退出。烧录后先观察端口至少 30 秒，避免把反复重枚举误判为稳定启动。

完整证据标准见[测试验收与故障排查](../docs/测试验收与故障排查.md)。

## 8. 启动顺序与错误页

完整产品链顺序：

```text
NVS/设备配置
  -> 队列、锁和推理 PM 锁
  -> 厂家 BSP、AMOLED、触摸和 LVGL
  -> BOOT / SELF_TEST
  -> QMI8658、AXP2101、PCF85063
  -> LittleFS 会话恢复
  -> 协调器、IMU pipeline 和双 M0
  -> NimBLE 与广播
  -> 业务任务
  -> HOME
```

故障码：

| 故障码 | 含义 |
|---:|---|
| `1001` | QMI8658、AXP2101 或 PCF85063 关键初始化失败 |
| `1002` | LittleFS 或双槽会话仓储无法安全使用 |
| `1003` | 协调器、IMU pipeline 或模型领域初始化失败 |
| `1004` | BLE 启动后不能为完整业务任务分配栈 |

显示或 LVGL 在错误页创建之前失败时，屏幕无法显示故障码，只能依赖 USB Serial/JTAG 日志。`1004` 的当前处理是保留稳定错误页。

## 9. 首次启动与配对

1. 烧录后等待 HOME。
2. 启动上位机，进入设备页并重新扫描。
3. 选择名称以 `BPNN-FIT-` 开头的设备。
4. 点击快速连接。
5. 当前联调版使用固定 PIN `123456`。上位机通过 Windows `CustomPairing` 响应 `ProvidePin`，手表显示配对状态。
6. 成功后上位机读取设备 Manifest、LiveState、Battery Service 和协议能力。

当前 NimBLE 只允许一个 PC 连接。Windows 与手表保存的绑定不一致时：

1. 在上位机点击“忘记设备”；
2. 在手表设置页点击“忘记电脑”；
3. 必要时在 Windows 蓝牙设置删除对应设备；
4. 重启手表；
5. 重新扫描并配对。

只清除一端会留下不一致密钥，常见症状是“能扫描到但 Windows BLE 配对失败”。

## 10. 手表操作

### 10.1 训练

1. 右手佩戴。
2. HOME 点击“开始”。
3. 立即开始本轮唯一动作。
4. 首次识别完成后，主动作名称固定。
5. 每完成一个完整周期，次数或步数增加 1。
6. 中途站立或静坐时次数保持，主动作名称不变。
7. 恢复同一动作后从新完整周期继续。
8. 点击停止并确认，摘要保存后返回 HOME。

### 10.2 设置

设置页固定四个按钮：

- 亮度；
- 诊断；
- 忘记电脑；
- 返回。

亮度按 `15% -> 35% -> 60% -> 100% -> 15%` 循环。按钮区禁止滚动；松手后仍应停留在设置页。看不到亮度或返回时属于布局/固件版本错误，不是正常交互。

## 11. 模型更新

模型更新不能只替换一个头文件。必须同时更新：

- `dual_m0_bundle.npz`
- `dual_m0_manifest.json`
- `esp32_bp_features.h`
- `esp32_dual_m0_model.h`

### 11.1 导出候选

从仓库根设置两套已经冻结的验证工件：

```powershell
$baseArtifact = '替换为基础 M0 工件绝对路径'
$maskedArtifact = '替换为掩码 M0 工件绝对路径'
$candidateDir = '.codex-local\tmp\dual-m0-candidate'

if (Test-Path -LiteralPath $candidateDir) {
    Remove-Item -LiteralPath $candidateDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $candidateDir | Out-Null

& '.venv\Scripts\python.exe' python\dual_m0_export.py `
    --base-artifact-dir $baseArtifact `
    --masked-artifact-dir $maskedArtifact `
    --output-dir $candidateDir
```

候选目录只用于验证，完成后删除，不能留在仓库根或提交 Git。

### 11.2 发布前逐值核对

正式替换模型头文件前，必须在本地验证环境中用同一批窗口分别运行 Python 和候选 C 实现，并保存逐特征、逐模型和融合输出的误差报告。测试与验证脚本属于本地发布工具，不进入公开源码树；公开仓库只保留可审计的输入合同、误差阈值和最终模型清单。

硬门：

- 297 维特征最大绝对误差 `≤1e-4`；
- 基础 M0、掩码 M0、固定融合和动作段累计 logits 最大绝对误差 `≤1e-3`；
- 类别索引完全一致；
- 清单固定为 25 Hz、62 点窗口、12 点步长、297 维、11 类；
- 掩码范围固定为半开区间 `184:232`；
- 融合权重固定为 `0.85/0.15`，除非重新完成冻结验证选模。

### 11.3 晋升正式工件

候选通过后才复制四个文件：

```powershell
Copy-Item -LiteralPath (Join-Path $candidateDir 'dual_m0_bundle.npz') `
    -Destination 'esp32\include\dual_m0_bundle.npz'
Copy-Item -LiteralPath (Join-Path $candidateDir 'dual_m0_manifest.json') `
    -Destination 'esp32\include\dual_m0_manifest.json'
Copy-Item -LiteralPath (Join-Path $candidateDir 'esp32_bp_features.h') `
    -Destination 'esp32\include\esp32_bp_features.h'
Copy-Item -LiteralPath (Join-Path $candidateDir 'esp32_dual_m0_model.h') `
    -Destination 'esp32\include\esp32_dual_m0_model.h'
```

随后依次运行公开工程门：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  esp32\host_tests\run_all.ps1 -StopOnFailure

dotnet build pc\FitnessCoach.sln -c Release

powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  esp32\firmware\tools\idf_project.ps1 -Action build
```

只有主机测试、PC Release 构建、ESP-IDF 新链接和真板验收均通过，才能更新 manifest 的硬件验证状态。单次用户日志不得参与训练、选模后又作为独立验证。

完成后清理候选目录：

```powershell
Remove-Item -LiteralPath $candidateDir, $verifyDir -Recurse -Force
```

训练与冻结选模方法见[Python 训练端说明](../python/README.md)。

## 12. Waveshare 官方基线

涉及屏幕、触摸、PMIC、IMU、RTC、Flash 分区、启动或低功耗时，先核对厂家第一方资料：

- [Waveshare 官方仓库](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06)
- [ESP-IDF 例程](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/tree/main/examples/esp-idf)
- [Arduino 例程](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/tree/main/examples/arduino)
- [出厂与恢复固件](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/tree/main/FirmWare)
- [硬件原理图](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/tree/main/Schematic)

最低规则：

1. 显示与触摸优先对照官方 `02_lvgl_demo_v9`。
2. 保留 `bsp_display_start()`、BSP LVGL 锁、官方 flush 完成回调和控制器版本链。
3. 所有 `lv_` 对象操作必须位于 BSP LVGL 锁内。
4. 异步 QSPI flush 期间禁止在持锁更新函数内调用 `lv_refr_now()`。
5. AXP2101、QMI8658 和 PCF85063 分别对照对应官方例程。
6. 官方 Demo 稳定而自定义固件失败时，默认是自定义集成问题，不能先归因于硬件损坏。
7. 面板或触摸控制器身份不明确时，先烧录对应官方 Demo 做 A/B，再修改自定义驱动。

## 13. 发布安全规则

1. 软件门失败时不烧录。
2. ESP-IDF 构建失败或本次 BIN 不完整时禁止烧录。
3. USB 身份不唯一时不访问任何串口。
4. 四次 Hash 未全部出现时不进入真板测试。
5. UI、触摸、BLE 和训练链稳定前不启用低功耗。
6. 每轮只改变一个硬件集成变量；不能同时改驱动、布局、字体、亮度和睡眠。
7. 烧录前准备厂家恢复固件或经验证的回退工件，并记录完整 SHA 和回退步骤。
8. 真板验证至少覆盖 30 秒显示、设置页四按钮、20 轮开始/停止、全新配对、保存绑定重连、动作计数和一小时稳定性。
9. 构建缓存、临时脚本和本地发布包不进入 Git。

## 14. 文档入口

- [文档索引](../docs/README.md)
- [算法原理、训练与实时计数](../docs/算法原理、训练与实时计数.md)
- [系统架构与业务流程](../docs/系统架构与业务流程.md)
- [BLE 通信、设备配置与会话存储](../docs/BLE通信、设备配置与会话存储.md)
- [硬件平台、手表界面与低功耗](../docs/硬件平台、手表界面与低功耗.md)
- [测试验收与故障排查](../docs/测试验收与故障排查.md)
