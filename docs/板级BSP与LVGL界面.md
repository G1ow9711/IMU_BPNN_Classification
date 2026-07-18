# Waveshare 2.06 板级 BSP 与 LVGL 9 界面

## 1. 文档范围

本文说明健身识别手柄的软件怎样接入 Waveshare `ESP32-S3-Touch-AMOLED-2.06` 的板载资源，以及 LVGL 9 页面怎样安全运行。

覆盖内容：

- 410×502 AMOLED 与电容触摸；当前厂家 BSP 使用 SH8601/FT5x06 兼容驱动路径；
- 共用 I2C 总线；
- QMI8658、AXP2101、PCF85063 的驱动边界；
- TF 卡；
- GPIO18 马达与 GPIO46 扬声器功放门控；
- 开机、主页、准备、训练、暂停、总结、设置、诊断、熄屏、错误和关机页面；
- 真实 BSP/Mock 编译切换；
- LVGL 线程安全、资源、低功耗和后续烧录验收。

相关代码：

- `esp32/firmware/components/board_adapter/`
- `esp32/firmware/components/board_runtime/`
- `esp32/firmware/components/ui/`
- `esp32/host_tests/board_ui_runtime/`

## 2. 厂商 API 事实边界

项目固定使用 managed component：

```yaml
waveshare/esp32_s3_touch_amoled_2_06: "1.0.7"
lvgl/lvgl: "9.5.0"
```

代码核对基准为 Waveshare 官方 `Waveshare-ESP32-components` 与当前产品 ESP-IDF 示例锁文件。产品仓库把物料标为 CO5300+CST9220；受管 BSP 同时依赖 SH8601 与 FT5x06 兼容驱动。因此本文把“物料名”和“驱动路径名”分开描述。

厂商 BSP v1.0.7 已公开且本项目实际调用的接口：

```c
lv_display_t *bsp_display_start(void);
lv_indev_t *bsp_display_get_input_dev(void);
bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);
esp_err_t bsp_display_brightness_set(int brightness_percent);
esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);
esp_err_t bsp_sdcard_mount(void);
esp_err_t bsp_sdcard_unmount(void);
```

重要限制：该 BSP 明确声明 `BSP_CAPS_IMU=0`。它没有提供 QMI8658、AXP2101 或 PCF85063 的高层读写函数。因此 `board_runtime` 不虚构厂商 API，而是：

1. 用 BSP 建立显示、触摸、I2C 和 TF；
2. 启动时只对 QMI8658、AXP2101、PCF85063 做 I2C ACK 探测；
3. 通过 `board_runtime_external_ops_t` 接收独立驱动回调；
4. 独立驱动通过 `board_runtime_i2c_handle()` 复用 BSP 主总线；
5. 未注册独立驱动时返回 `BOARD_RUNTIME_ERR_UNSUPPORTED`，不伪造电量、RTC 或关机成功。

当前真实后端只允许厂家 BSP 已封装的 SH8601/FT5x06 兼容路径。未实现的 `CO5300/CST9220` 原生路径不再作为用户可选 Kconfig 选项；兼容符号只为旧配置文件保留并固定关闭。兼容路径是否对应实物 CO5300/CST9220，由厂家组件内部实现和烧录验收共同确认，固件不能绕过 BSP 盲写另一控制器寄存器。

## 3. 板级资源合同

| 资源 | 接法/接口 | 软件行为 |
|---|---|---|
| AMOLED | 410×502，QSPI；厂家 BSP 内部走 SH8601 兼容驱动 | `bsp_display_start()` 启动显示和 LVGL 任务；`bsp_display_panel_power_set()` 发送真实 Display On/Off；实物物料名按厂家当前资料为 CO5300 |
| AMOLED 亮度 | 面板 `0x51` 命令 | `bsp_display_brightness_set(0..100)` 只调发光亮度；熄屏另用 Display Off，没有独立背光 GPIO |
| 触摸 | 厂家 BSP 内部走 FT5x06 兼容驱动，SDA15/SCL14，RST9，INT38 | BSP 随显示创建 `lv_indev_t`；策略明确停用触摸时写 PMODE hibernate，唤醒时 GPIO9 复位、重新写冻结阈值，再启用 LVGL 输入；实物控制器资料冲突仍须烧录核对 |
| QMI8658 | I2C 0x6A 或 0x6B，INT1 GPIO21 | 运行时探测地址；采样/低功耗由独立驱动回调负责 |
| AXP2101 | I2C 0x34 | 运行时探测；电量与软关机由独立驱动回调负责 |
| PCF85063 | I2C 0x51，INT GPIO39 | 运行时探测；UTC 秒读取由独立驱动回调负责 |
| TF | 1-bit SDMMC：CLK2、CMD1、D0=3 | 使用 `bsp_sdcard_mount/unmount`；按需挂载 |
| 马达 | GPIO18 经 NMOS | LEDC 5 kHz、10 位 PWM；一次定时器自动停止 |
| 扬声器功放 | GPIO46 | 启动默认低电平；v1 默认关闭，不初始化音频 codec |

本地原理图还给出 TF `CS=GPIO17`，但厂商 ESP-IDF BSP 的 TF 路径是 1-bit SDMMC，不使用 CS。若以后改 SPI 模式，必须另建组件，不能同时驱动同一张卡。

## 4. 真实/Mock 编译切换

`board_runtime/Kconfig`：

```text
CONFIG_BOARD_RUNTIME_REAL=y   使用真实 Waveshare BSP
CONFIG_BOARD_RUNTIME_MOCK=y   不访问硬件
```

默认选择 REAL。无板联调可选择 MOCK。两种后端对上层暴露完全相同的 `board_runtime_t` 和 `board_adapter_t`。

Mock 能验证：

- 显示开关和亮度状态；
- 电池百分比与充电状态；
- TF 挂载状态；
- 触摸逻辑开关；
- 振动请求参数；
- QMI、RTC、PMIC 回调边界；
- LVGL 锁端口。

Mock 通过不代表真实屏幕、触摸、马达、电源或 TF 已验收。

### 4.1 可见启动与错误页顺序

生产入口先初始化板级显示、触摸和 LVGL renderer，立即呈现中文开机页与自检页，然后才初始化 QMI8658、AXP2101、PCF85063、会话存储和业务域。顺序固定为：

```text
board runtime -> LVGL renderer -> BOOT/SELF_TEST 可见
              -> 外部传感器 -> 会话存储 -> 业务域 -> HOME
```

QMI8658、AXP2101、RTC、会话存储或业务域初始化失败时，入口把稳定故障码写入 `ui_context_t`，渲染中文 `ERROR` 页并挂起产品任务。显示本身初始化失败时没有可用像素输出，只能通过串口保留错误；文档不能声称该故障也能显示屏幕错误页。

## 5. 马达脉冲算法

马达 PWM 频率固定：

$$
f_{PWM}=5000\ \mathrm{Hz}
$$

LEDC 分辨率为 10 位，最大占空比：

$$
D_{max}=2^{10}-1=1023
$$

用户强度百分比为 $p\in[1,100]$ 时，整数占空比为：

$$
D=\left\lfloor\frac{pD_{max}+50}{100}\right\rfloor
$$

其中 `+50` 用于整数四舍五入。

一次脉冲流程：

1. 停止旧的一次定时器；
2. 写入 GPIO18 LEDC 占空比；
3. 启动 `duration_ms × 1000` 微秒的一次定时器；
4. 定时器在 `esp_timer` 任务上下文把占空比归零；
5. 定时器启动失败时立即归零，防止马达持续导通。

产品合同：有效次数振动 30 ms；walk/trot 每累计 10 步振动 30 ms。计数算法负责判定“有效事件”，板级层只执行脉冲。

时间复杂度为 $O(1)$，运行状态只保存一个定时器句柄，不动态创建每次脉冲对象。

## 6. UI 页面

| 状态 | 页面主要内容 | 触摸按钮 |
|---|---|---|
| `BOOT` | 标志、中文启动提示、800 ms 淡入 | 无 |
| `SELF_TEST` | 显示、触摸、IMU、存储中文自检提示 | 无 |
| `HOME` | 就绪、电量、BLE | 开始、设置、关机 |
| `PREPARE` | 3-2-1、IMU 预热 | 取消 |
| `RUNNING` | 动作、次数/步数/秒、kcal、时间、置信度 | 暂停、停止 |
| `PAUSED` | 冻结指标 | 继续、停止 |
| `SUMMARY` | 总指标、kcal、时长 | 完成 |
| `SETTINGS` | 亮度、振动、扬声器、电池规格、绑定管理 | 诊断、返回、关机、忘记电脑 |
| `DIAGNOSTICS` | 状态、数据质量、硬件待验说明 | 返回 |
| `SCREEN_OFF` | 纯黑页面 | 无 |
| `ERROR` | 错误码、训练阻止提示 | 关机 |
| `SHUTDOWN` | 保存完成、500 ms 淡入 | 无 |

页面采用接近纯黑背景，减少 AMOLED 发光功耗和静态烧屏风险。全部用户可见文案和 11 类动作使用项目生成的 Noto Sans SC 16/20/28 px 中文子集；Montserrat 只用于不含汉字的内部或数值内容。修改中文文案后必须重新生成字体子集，并复核字形 manifest、SHA-256、许可证、Flash 与页面创建 RAM。

BLE Display Only 配对码不是第 14 个独立页面，而是覆盖当前可见页面的高优先级中文提示。六位码使用固定宽度补零显示；提示存在时隐藏当前页按钮，避免用户在配对过程中误启动训练。成功、失败、断线、超时、停止 BLE 或执行“忘记电脑”时立即清除码和到期时间，旧码不能留在下一页快照中。

## 7. presenter 与 renderer 分层

### 7.1 纯 presenter

`ui_presenter_build()` 输入 `ui_context_t`，输出固定大小 `ui_page_model_t`：

- 标题；
- 主文本；
- 次文本；
- 电池/BLE 状态；
- 页脚；
- 最多三个按钮命令和标签。

它只用栈上固定缓冲和 `snprintf`，时间、空间复杂度均为 $O(1)$。主机测试不需要 LVGL。

### 7.2 LVGL renderer

`ui_lvgl_renderer_init()` 在启动时一次创建全部 screen 和子对象。运行时只更新标签、按钮显示状态和当前 screen，不反复创建页面，减少堆碎片。

页面切换和反馈动画：

- 页面切换：150 ms 淡入；
- 开机：800 ms 主文本淡入；
- 关机：500 ms 主文本淡入；
- 动作变化：150 ms 动作名淡入；
- 计数变化：180 ms 指标淡入。

不使用全屏白色、长距离滑动或无限动画。

## 8. LVGL 线程安全

LVGL 不是线程安全库。厂商 BSP 文档要求所有 `lv_...` 调用位于：

```c
bsp_display_lock(timeout_ms);
/* 读取或修改 LVGL 对象。 */
bsp_display_unlock();
```

渲染器不直接依赖 BSP，而是接收 `ui_lvgl_port_t`：

```c
const ui_lvgl_port_t port = {
    .context = &board_runtime,
    .lock = board_runtime_lvgl_lock,
    .unlock = board_runtime_lvgl_unlock,
};
```

规则：

- 初始化、更新、删除页面全部加锁；
- presenter 在锁外格式化文本，缩短临界区；
- 加锁 250 ms 超时后丢弃这一帧，不阻塞 IMU/推理任务；
- LVGL 按钮回调只把 `ui_command_t` 放入 FreeRTOS 队列；
- 按钮回调不直接调用存储、BLE、PMIC 或算法；
- UI 任务收到最新 `ui_context_t` 快照后调用 render。

## 9. 集成调用顺序

上层 `app_main` 后续应按下列顺序调用。此子任务没有修改总 `main`：

```c
board_runtime_t board_runtime = {0};
board_runtime_config_t board_config = {
    .external_ops = product_external_drivers,
    .initial_brightness_percent = 35,
};

board_runtime_init(&board_runtime, &board_config);

ui_lvgl_renderer_t renderer = {0};
ui_lvgl_port_t lvgl_port = {
    .context = &board_runtime,
    .lock = board_runtime_lvgl_lock,
    .unlock = board_runtime_lvgl_unlock,
};

ui_lvgl_renderer_init(
    &renderer,
    &lvgl_port,
    enqueue_ui_command,
    &ui_command_queue);

ui_lvgl_renderer_render(&renderer, &ui_context);
```

所需组件依赖：

```text
board_runtime -> board_adapter
board_runtime -> waveshare__esp32_s3_touch_amoled_2_06
board_runtime -> lvgl, esp_driver_gpio, esp_driver_i2c, esp_driver_ledc, esp_timer
ui -> lvgl
```

QMI8658 采样驱动应把严格时间戳原始点送入 `imu_pipeline`，不应由 UI 或 board_runtime 读取并重采样。

## 10. 原厂电池与低功耗说明

当前硬件使用厂家原配 400 mAh 电池。板级层不根据电压猜百分比，百分比必须来自经过验证的 AXP2101 独立驱动。

现有电源门槛：

- 15%：低电量警告；
- 8%：非充电时禁止开始新会话；
- 5%：保存会话并请求 PMIC 安全关机。

显示熄屏执行两层动作：先把面板亮度降为 0，再通过 `bsp_display_panel_power_set(false)` 发送真实 Display Off。触摸停用先执行 `lv_indev_enable(false)`，再通过 FT5x06/FT3168 驱动把 PMODE 写为 hibernate。唤醒顺序相反：GPIO9 复位触摸、重新初始化并写回冻结阈值，启用 LVGL 输入，最后执行面板 Display On 并恢复目标亮度。

Deep-sleep 入口先验证 QMI8658 WOM、RTC/触摸 Light-sleep 候选和 GPIO21 RTC IO 等唤醒前提，验证成功后才停止 BLE、关闭面板并休眠触摸。若前提失败，电源任务保留当前外设和运行状态，不允许先熄屏再因睡眠失败把设备留在不可交互状态。

上述调用和顺序已完成源码/主机替身验证；面板命令、电容触摸实际功耗、GPIO9 复位时序和唤醒后首击仍必须烧录实测。

## 11. 软件验证与硬件待验

已完成软件验证：

- `power_ui_tests passed`；
- `board_ui_runtime_tests passed`；
- `board_runtime_esp.c` 对 ESP-IDF/Waveshare v1.0.7 签名桩通过 `-Wall -Wextra -Werror -fsyntax-only`；
- `ui_lvgl_renderer.c` 对厂家仓库 LVGL 9 头通过同等语法编译；
- 设置/诊断状态导航、全部页面 presenter、Mock 电池/TF/振动/QMI/RTC/PMIC 边界有主机测试。
- AMOLED Display On/Off、FT3168 hibernate/wake 和“先验证唤醒源、后关闭外设”的调用合同有源码测试。
- 六位配对码显示/清除、设置页“忘记电脑”和绑定删除边界有纯 C/主机测试。

没有硬件，以下项目明确待用户烧录验证：

1. SH8601 首帧、Display On/Off、亮度 0/35/100 和烧屏保护；
2. FT3168 坐标、方向、边缘按钮、PMODE hibernate、GPIO9 复位恢复和 GPIO38 唤醒；
3. QMI8658 地址、INT1 GPIO21、ODR、FIFO 与时间戳；
4. AXP2101 电量、充电、5% 安全关机；
5. PCF85063 时间和 GPIO39 中断；
6. TF 插拔、1-bit SDMMC、断电前卸载；
7. GPIO18 30 ms 振动波形、强度、反电动势和 IMU 污染窗口；
8. GPIO46 默认低电平、开机无爆音；
9. LVGL 页面创建峰值 RAM、任务栈高水位和 1 小时稳定性；
10. 400 mAh 原厂电池的训练电流、熄屏电流和 Deep-sleep 电流。
11. Windows 首次配对时六位码显示、成功/失败/断线清除，以及设备“忘记电脑”后旧密钥不能重连。
