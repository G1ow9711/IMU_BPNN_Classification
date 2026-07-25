# 设备配置与命令 TLV v1

## 1. 目的与边界

本组件把 BLE 控制命令 6、7、8、9、11 的可变参数转换为固定宽度纯 C 对象，并把完整设备配置编码为带 CRC32 的稳定 44 字节 blob。它解决三个问题：

1. PC 与 ESP32 对字段类型、长度、单位和范围有唯一解释；
2. 任一字段错误时整条命令失败，不留下“只改了一半”的配置；
3. 配置写入 NVS 或 LittleFS 后，可在重启时检测截断、翻转和版本不兼容。

组件位置：

```text
esp32/firmware/components/device_config/
  include/device_config.h
  device_config.c
```

组件为纯 C11，不依赖 NimBLE、FreeRTOS、NVS、LittleFS 或动态内存。传输层负责从完整 BLE 控制帧取得 `command_id`、`command_version` 和 TLV 字节；本组件只负责确定性解码、范围验证、配置事务和 blob 编解码。

当前实现未修改生产 `main.c`。因此，codec 和协调器 API 已可调用，但 BLE 命令回调、RTC、显示亮度、原始流发布和实际配置介质仍需由总编排层接线。

## 2. 通用 TLV 布局

每一项固定为：

```text
+--------+--------+-------------------+
| type   | len    | value             |
| u8     | u8     | len bytes         |
+--------+--------+-------------------+
```

- `type`：命令内部字段编号；
- `len`：紧随其后的 value 字节数，范围 0～255；
- `value`：整数使用小端序；布尔只允许单字节 `0` 或 `1`；
- 多项可按任意顺序出现；
- 未知 type 在 item 完整落入输入边界时跳过，保证向前兼容；
- 已知 type 重复、缺少必填项、长度错误或值越界时，拒绝整条命令；
- 未知项若声明长度超过剩余输入，同样按畸形 TLV 拒绝，不能越界跳过。

解码器先写局部 `device_command_v1_t`，全部检查通过后才复制到调用者输出。设 TLV 总长为 $n$ 字节，扫描时间复杂度为：

$$
T(n)=O(n)
$$

除固定命令对象外不分配额外内存，因此额外空间复杂度为：

$$
S(n)=O(1)
$$

## 3. Cmd6：同步时间

命令号为 `6`，版本为 `1`。两个字段均为必填。

| type | len | 类型 | 单位 | 合法范围 |
|---:|---:|---|---|---|
| 1 | 8 | `int64` | UTC Unix 秒 | 946684800～4102444799，即 2000-01-01 到 2099-12-31 |
| 2 | 2 | `int16` | 相对 UTC 的分钟 | -720～840，即 UTC-12:00 到 UTC+14:00 |

Unix 秒只表示绝对 UTC。时区分钟只用于本地显示：

$$
t_{local}=t_{utc}+60\Delta_{tz}
$$

其中：

- $t_{utc}$：Unix 秒；
- $\Delta_{tz}$：时区分钟；
- $t_{local}$：本地显示秒。

会话耗时必须继续使用单调时钟，不能因 Cmd6 校时发生跳变。

## 4. Cmd7：设置用户资料

命令号为 `7`，版本为 `1`。两个字段均为必填。

| type | len | 类型 | 单位 | 合法范围 |
|---:|---:|---|---|---|
| 1 | 4 | `uint32` | g | 30000～250000 |
| 2 | 4 | `uint32` | 无 | `profile_revision > 0` |

体重在配置、协调器、训练引擎和卡路里核心中统一使用克。换算为千克：

$$
W_{kg}=\frac{W_g}{1000}
$$

Windows 设置页可选公制或英制。英制只影响输入框和标签，先按 $W_{kg}=W_{lb}/2.2046226218487757$ 换回千克，再四舍五入为 `weight_g`。单位枚举保存在 PC 本地偏好，不新增 TLV 字段，也不改变设备侧卡路里单位。

运行中修改体重只更新 `device_coordinator_t.weight_g`，作为下一会话的启动值。正在进行的 `workout_engine_t.weight_g` 保持不变，避免已经累计的热量因中途改体重而改变含义。

设备配置默认体重为 `70000 g`，即 70 kg。底层健身核心仍可用 `0` 表示未知体重并让热量保持 0，但设备配置和 coordinator 公共设置入口不接受 0。

## 5. Cmd8：设置训练目标

命令号为 `8`，版本为 `1`。两个字段均为必填。

| type | len | 类型 | 含义 |
|---:|---:|---|---|
| 1 | 1 | `uint8` | `goal_kind` |
| 2 | 4 | `uint32` | `goal_value` |

目标种类：

| `goal_kind` | 目标 | `goal_value` 单位 | 组合约束 |
|---:|---|---|---|
| 0 | 无目标 | 无 | 必须为 0 |
| 1 | 次数 | 次 | 必须大于 0 |
| 2 | 活动时长 | 秒 | 必须大于 0 |
| 3 | 热量 | mcal | 必须大于 0 |

`1 mcal = 0.001 kcal`。LiveState 目标百分比使用整数向下取整并饱和到 100：

$$
P=\min\left(100,\left\lfloor\frac{100C}{G}\right\rfloor\right)
$$

其中：

- $C$：当前次数、排除暂停后的活动整秒或累计 mcal；
- $G$：大于 0 的目标值；
- $P$：`goal_percent`，范围 0～100。

无目标时不返回 0，而返回协议哨兵 `255`。次数目标只读取 `FITNESS_METRIC_REPETITION`，不会把 `walk/trot` 步数误算成次数。时长目标使用：

$$
C_s=\left\lfloor\frac{t_{active,ms}}{1000}\right\rfloor
$$

热量目标使用：

$$
C_{mcal}=\left\lfloor\frac{E_{\mu kcal}}{1000}\right\rfloor
$$

当 $C<G\le2^{32}-1$ 时，`100C` 小于 $100(2^{32}-1)$，安全落入 `uint64_t`；达到目标后直接返回 100，避免乘法溢出。

## 6. Cmd9：设置设备偏好

命令号为 `9`，版本为 `1`。六个字段均为必填，保证一次命令产生完整偏好快照。

| type | len | 类型 | 单位/含义 | 合法范围 |
|---:|---:|---|---|---|
| 1 | 1 | `uint8` | AMOLED 亮度百分比 | 5～100 |
| 2 | 1 | `bool` | 振动开关 | 0 或 1 |
| 3 | 1 | `bool` | 声音开关 | 0 或 1 |
| 4 | 2 | `uint16` | 自动熄屏秒数 | 10～600 |
| 5 | 4 | `uint32` | 偏好 revision | 大于 0 |
| 6 | 1 | `bool` | 开发者模式 | 0 或 1 |

默认值：亮度 35%、振动开、声音关、30 秒熄屏、revision 1、开发者模式关。

## 7. Cmd11：原始流开关

命令号为 `11`，版本为 `1`。字段为必填。

| type | len | 类型 | 含义 | 合法范围 |
|---:|---:|---|---|---|
| 1 | 1 | `bool` | 是否发布开发者六轴原始流 | 0 或 1 |

默认关闭。codec 只验证和保存开关，不负责启动 BLE notification。生产层应结合开发者模式、连接权限、MTU、发送速率和功耗策略决定是否真正发布。

## 8. 配置对象与事务应用

`device_config_t` 保存：

- UTC 是否有效、UTC Unix 秒、时区分钟；
- 体重克和资料 revision；
- 目标种类和值；
- 亮度、振动、声音、熄屏秒、偏好 revision、开发者模式；
- 原始流开关。

`device_config_apply_command()` 先复制原配置：

```text
candidate = current
candidate 对应字段 = command 字段
验证 candidate 全部语义
成功：current = candidate
失败：current 保持逐字节不变
```

因此，单条命令不会留下部分配置。时间和空间复杂度均为常数：

$$
T=O(1),\qquad S=O(1)
$$

## 9. 44 字节稳定 blob

### 9.1 总布局

| 偏移 | 长度 | 字段 | 编码 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `DCFG` |
| 4 | 2 | format_version | u16 little-endian，固定 1 |
| 6 | 2 | payload_length | u16 little-endian，固定 32 |
| 8 | 32 | payload | 下表 |
| 40 | 4 | crc32 | 前 40 字节 CRC32/IEEE，小端 |

### 9.2 payload 布局

| blob 偏移 | 长度 | 字段 |
|---:|---:|---|
| 8 | 1 | flags：bit0 UTC有效、bit1振动、bit2声音、bit3开发者、bit4原始流 |
| 9 | 1 | brightness_percent |
| 10 | 1 | goal_kind |
| 11 | 1 | 保留，必须为 0 |
| 12 | 2 | timezone_minutes，int16 little-endian |
| 14 | 2 | screen_timeout_seconds |
| 16 | 4 | weight_g |
| 20 | 4 | profile_revision |
| 24 | 4 | goal_value |
| 28 | 4 | preferences_revision |
| 32 | 8 | utc_unix_seconds，int64 little-endian |

只接受精确 44 字节。尾随数据、未知 flag、非零保留字节、版本错误、payload 长度错误、CRC 错误和字段语义错误全部拒绝。

### 9.3 CRC32 公式

实现使用 CRC32/IEEE 反射形式：

```text
initial = 0xFFFFFFFF
polynomial = 0xEDB88320
final_xor = 0xFFFFFFFF
```

对每个输入字节 $b_i$：

$$
c\leftarrow c\oplus b_i
$$

随后执行 8 次：

$$
c\leftarrow
\begin{cases}
(c\gg1)\oplus \mathrm{0xEDB88320},&c_0=1\\
c\gg1,&c_0=0
\end{cases}
$$

最后：

$$
CRC=c\oplus\mathrm{0xFFFFFFFF}
$$

编码和解码时间复杂度为 $O(44)$，即固定常数；无需堆内存。默认配置黄金 CRC 为 `0xE03A6746`。

CRC 只能检测意外损坏，不能提供加密或身份认证。BLE 写入仍必须依赖已有加密、绑定和命令幂等机制。

## 10. coordinator 运行时 API

### 10.1 更新下次会话体重

```c
device_coordinator_set_next_session_weight(...)
```

输入单位为克，范围 30000～250000。API 更新 `coordinator.weight_g`、递增 `state_revision` 并发布 UI/LiveState 快照；已经开始的 `workout.weight_g` 不变。下一次 `DEVICE_CONTROL_START` 才把新值复制到训练引擎。

### 10.2 保存目标

```c
device_coordinator_set_goal(...)
```

API 复用 `device_config_validate_goal()`，先累加当前活动时长，再保存目标、递增修订并发布含最新 `goal_percent` 的 LiveState。无效 kind/value、空指针或单调时间倒退时，coordinator 和 effect 保持事务语义。

两个 API 均不执行 BLE、文件、NVS、LVGL 或硬件调用。生产总编排应在应用任务中调用，并把返回的 effect 投递到现有任务队列。

## 11. 测试与一致性

主机测试入口：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  esp32\host_tests\device_config\run_tests.ps1

powershell -NoProfile -ExecutionPolicy Bypass -File `
  esp32\host_tests\device_coordinator\run_tests.ps1
```

覆盖范围：

- 五条命令正常解码；
- 未知项跳过；
- 重复、缺失、截断和长度错误；
- UTC/时区、体重、目标、亮度、熄屏、revision 和布尔双侧边界；
- 默认配置全部产品值；
- 配置事务应用和错误回滚；
- 44 字节黄金 blob、CRC 翻转和输出回滚；
- 运行中改体重只影响下一会话；
- 无目标 255、次数 0～100、时长和 mcal 目标进度。

当前测试是无硬件纯 C 验证。它不能替代真机 RTC 写入、AMOLED 亮度、马达开关、原始 BLE 流速率和断电恢复实测。
