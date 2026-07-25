# ESP32-S3 健身手柄 BLE 通信协议

## 1. 文档目的

本文定义 ESP32-S3 健身手柄与 Windows 上位机之间的 BLE 应用层协议 v1。协议目标：

- ESP32 断开 PC 后仍能独立识别、计数、累计卡路里并保存会话；
- PC 连接后实时显示动作动画、次数、卡路里、电量和设备状态；
- 断线重连后恢复同一会话，不重复开始、清零或保存；
- 会话摘要始终可同步，25 Hz 六轴诊断流按用户开关选择；
- C 与 C# 使用完全一致的端序、CRC、分片和字段定义；
- ATT MTU 23、185、247 都能工作；
- 异常长度、CRC 错误、缺片和乱序不得进入业务状态机。

协议不负责训练模型，也不在 PC 端重新计算动作、次数或卡路里。ESP32 是实时状态和累计值的唯一权威源。

---

## 2. 传输层约定

### 2.1 蓝牙模式

- 只使用 Bluetooth Low Energy，不使用经典蓝牙串口。
- ESP32-S3 使用 ESP-NimBLE。
- Windows 使用 `Windows.Devices.Bluetooth` 原生 API。
- 设备最多同时连接一个 PC。
- 广播名为 `BPNN-FIT-XXXX`，`XXXX` 是设备 MAC 低四位十六进制字符。

### 2.2 安全

- 首次配对使用 LE Secure Connections 和绑定。
- 设备屏幕显示六位码，Windows 端要求用户确认。
- 未绑定客户端只能读取 Manifest 和标准设备信息，不能启停会话、修改资料或下载日志。
- ESP32 的 NVS 与 Windows 系统分别保存绑定信息。
- 用户在设备端执行“忘记电脑”后，必须同时清除 ESP32 绑定记录；PC 端也提供“忘记设备”。

六位码生命周期固定如下：NimBLE 生成码后通过无阻塞邮箱交给 LVGL；配对成功、配对失败、BLE 断线、60 秒显示超时、BLE 服务停止或用户忘记电脑时，设备都必须清除 UI 码和邮箱中的旧值。设备“忘记电脑”会先断开当前连接再删除设备侧全部绑定；PC“忘记设备”会取消 Windows 系统配对并清除本地固定设备 ID。两端各自只管理自己的密钥存储，完整解除绑定建议两端都执行。

### 2.3 字节序与整数

- 所有多字节整数使用小端序。
- 线上结构只使用 `uint8`、`uint16`、`uint32`、`uint64` 和 `int16` 等固定宽度整数。
- 协议主路径不直接传输 C/C# `bool`、枚举内存布局、结构体填充或浮点数。
- 卡路里使用千分之一千卡整数，置信度使用 Q15，避免跨语言浮点舍入差异。

---

## 3. GATT 服务

自定义服务 UUID：

```text
7B2E0000-6D57-4A51-9E43-494D5542504E
```

| 尾号 | 名称 | 属性 | 方向 | 用途 |
|---|---|---|---|---|
| `0001` | Control Point | Write、Indicate | 双向 | 启停、暂停、配置、时间同步及 ACK/NACK |
| `0002` | Manifest | Read | 设备到 PC | 协议、固件、模型、类别表、能力清单 |
| `0003` | Live State | Read、Notify | 设备到 PC | 当前动作、次数、卡路里、会话状态、电量 |
| `0004` | Event | Notify | 设备到 PC | 计数、动作切换、目标完成、低电量和故障事件 |
| `0005` | Transfer Control | Write、Indicate | 双向 | 会话列表、摘要、日志下载和断点续传控制 |
| `0006` | Transfer Data | Notify | 设备到 PC | 会话摘要和原始日志数据块 |
| `0007` | Raw Stream | Notify | 设备到 PC | 开发者模式实时六轴同步诊断样本，默认关闭 |

同时启用标准服务：

- Battery Service：`0x180F`；
- Device Information Service：`0x180A`；
- Generic Attribute Service 的 Service Changed，用于 GATT 数据库升级。

PC 连接顺序固定为：

1. 使用 Uncached 模式重新枚举自定义服务；
2. 读取 Manifest 并执行版本、类别表和能力检查；
3. 订阅 Control Point indication；
4. 订阅 Live State 和 Event notification；
5. 有日志下载需求时再订阅 Transfer Data；
6. 开发者明确启用时才订阅 Raw Stream；
7. 请求一次权威状态快照；
8. 同步缺失会话摘要。

---

## 4. 逻辑帧

### 4.1 固定结构

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 2 | `magic` | 固定 `0xB17E`，线上字节 `7E B1` |
| 2 | 1 | `protocol_major` | 当前为 1 |
| 3 | 1 | `protocol_minor` | 当前为 0 |
| 4 | 1 | `message_type` | 控制、状态、事件或传输类型 |
| 5 | 1 | `flags` | 响应、错误、快照、结束等标志 |
| 6 | 2 | `sequence` | 逻辑帧序号，按 65536 回绕 |
| 8 | 4 | `monotonic_ms` | 设备开机后单调毫秒时间，约 49.7 天回绕 |
| 12 | 2 | `payload_length` | payload 字节数，范围 0～1024 |
| 14 | P | `payload` | 上层消息内容 |
| 14+P | 2 | `crc16` | 小端 CRC-16/CCITT-FALSE |

设 payload 长度为 $P$，完整逻辑帧长度为：

$$
L = 14 + P + 2
$$

因此：

$$
16 \le L \le 1040
$$

解码时要求实际输入长度严格等于 $L$。不接受尾随垃圾、两帧拼接或截断帧。

### 4.2 sequence

`sequence` 用于发现通知缺口，不作为会话主键。每个通知类特征维护独立序号。PC 按模 $2^{16}$ 比较：

$$
\Delta = (s_{new}-s_{old}) \bmod 65536
$$

- $\Delta=1$：连续；
- $\Delta=0$：重复帧；
- $\Delta>1$：存在缺口。

Event 缺口不得由 PC 自行补算次数。PC 应读取 Live State 快照恢复权威累计值。

### 4.3 单调时间

`monotonic_ms` 只表示设备开机后的相对时间，不受 UTC 校时影响。会话时长、事件排序和超时判断均使用单调时间。UTC 只用于历史记录的墙钟时间。

---

## 5. CRC-16/CCITT-FALSE

### 5.1 参数

| 参数 | 数值 |
|---|---|
| 宽度 | 16 bit |
| 多项式 | `0x1021` |
| 初值 | `0xFFFF` |
| 输入反射 | false |
| 输出反射 | false |
| 最终异或 | `0x0000` |
| 标准检查值 | ASCII `123456789` → `0x29B1` |

CRC 覆盖范围从偏移 2 的 `protocol_major` 开始，到 payload 最后一个字节结束。魔数和 CRC 自身不参与计算。

### 5.2 逐位公式

设当前 16 位寄存器为 $R$，输入字节为 $b$。先执行：

$$
R \leftarrow R \oplus (b \ll 8)
$$

随后对每个字节执行 8 轮：

$$
R \leftarrow
\begin{cases}
(R \ll 1) \oplus 0x1021, & R_{15}=1 \\
R \ll 1, & R_{15}=0
\end{cases}
$$

每轮只保留低 16 位。最终数值按小端序写入逻辑帧。

### 5.3 数值与资源

- 时间复杂度：$O(8L)$，等价于 $O(L)$；
- 额外空间：$O(1)$；
- ESP32 可先使用逐位参考实现，后续若使用查表优化，必须保持黄金向量完全一致；
- CRC 错误帧不得进入状态机，也不得只跳过单个损坏字段。

---

## 6. GATT 分片

### 6.1 为什么需要分片

BLE ATT Value 可用长度不是固定 244 字节，而是：

$$
V = MTU - 3
$$

最小常见 MTU 为 23，此时 $V=20$。30 字节实时状态加逻辑帧头后超过 20 字节，因此协议必须支持分片，不能假定 Windows 始终协商到 185 或 247。

### 6.2 分片包络

每个 GATT Value 都使用相同 8 字节包络：

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 2 | `logical_sequence` |
| 2 | 2 | `fragment_index`，从 0 开始 |
| 4 | 2 | `fragment_count`，至少为 1 |
| 6 | 2 | `fragment_data_length` |
| 8 | F | 完整逻辑帧的一段连续字节 |

单片数据容量为：

$$
C = MTU - 3 - 8
$$

完整帧需要的分片数为：

$$
N_f = \left\lceil \frac{L}{C} \right\rceil
$$

例如 MTU 23：

$$
C=23-3-8=12
$$

21 字节黄金逻辑帧会拆成 2 片。

### 6.3 重组规则

BLE 同一连接、同一特征的通知保持发送顺序。本实现采用严格顺序重组：

1. 只有 `fragment_index=0` 可以开始新帧；
2. 后续 sequence、fragment_count 必须与第一片一致；
3. 后续索引必须严格等于上一索引加一；
4. 包络数据长度必须等于 GATT Value 剩余长度；
5. 累计长度不得超过 1040 字节；
6. 最后一片到达后必须再执行完整逻辑帧长度和 CRC 校验；
7. 缺片、乱序、超时、跨帧混入或 CRC 错误时清空当前状态。

严格顺序策略不缓存乱序片，可把 ESP32 重组 RAM 固定为约 1040 字节。实时状态缺片后由状态快照恢复；会话日志由偏移续传恢复。

### 6.4 超时

- 控制、状态和事件重组：1 秒；
- 会话传输数据块重组：5 秒；
- 超时后调用 `imu_ble_reassembler_reset` 或 C# `Reset()`；
- 超时状态不得与下一帧继续拼接。

---

## 7. 消息类型

| 数值 | 名称 | 方向 |
|---:|---|---|
| 1 | `ControlRequest` | PC → ESP32 |
| 2 | `ControlResponse` | ESP32 → PC |
| 3 | `LiveState` | ESP32 → PC |
| 4 | `Event` | ESP32 → PC |
| 5 | `TransferRequest` | PC → ESP32 |
| 6 | `TransferResponse` | ESP32 → PC |
| 7 | `TransferData` | ESP32 → PC |
| 8 | `RawStream` | ESP32 → PC |

未知消息类型处理：

- 主版本相同：记录诊断并忽略；
- 主版本不同：禁止训练控制，只允许读取设备信息和导出可识别数据；
- 不得把未知消息解析为最接近的旧类型。

---

## 8. LiveStateV1

Live State payload 固定 30 字节：

| 偏移 | 长度 | 字段 | 单位或范围 |
|---:|---:|---|---|
| 0 | 4 | `session_seq` | 设备持久化序号 |
| 4 | 4 | `state_revision` | 权威状态修订号 |
| 8 | 4 | `elapsed_ms` | 毫秒 |
| 12 | 1 | `device_state` | 0～7 |
| 13 | 1 | `action_id` | 0～10，255 未知 |
| 14 | 1 | `metric_kind` | 0 无、1 次、2 步、3 秒 |
| 15 | 1 | `battery_percent` | 0～100，255 未知 |
| 16 | 4 | `metric_value` | 次数、步数或秒数 |
| 20 | 2 | `confidence_q15` | 0～65535 |
| 22 | 4 | `calories_mcal` | 千分之一千卡 |
| 26 | 2 | `quality_flags` | 位集合 |
| 28 | 1 | `power_flags` | 位集合 |
| 29 | 1 | `goal_percent` | 0～100，255 未设置 |

Q15 置信度转换公式：

$$
c = \frac{q}{65535}
$$

其中 $q$ 是 `confidence_q15`，$c$ 位于 $[0,1]$。

卡路里转换公式：

$$
E_{kcal}=\frac{E_{mcal}}{1000}
$$

PC 不根据通知事件增加次数，只显示 `metric_value`。Event 用于立即触发动画反馈；即使 Event 丢失，下一条 Live State 仍恢复正确累计值。

### 8.1 状态修订号

设备每次发生权威状态变化时递增 `state_revision`。PC 规则：

- 新 revision 大于当前值：接受；
- 相同 revision：重复帧，忽略；
- 更小 revision：旧通知，忽略；
- 设备重新开机时结合 boot ID 或重新读取 Manifest 建立新比较基线。

---

## 9. 控制点

### 9.1 请求

```text
uint32 request_id
uint8  command_id
uint8  command_version
uint8  tlv_data[]
```

### 9.2 响应

```text
uint32 request_id
uint8  command_id
uint8  status
uint16 error_code
uint32 state_revision
uint8  optional_tlv[]
```

### 9.3 v1 命令

| command_id | 命令 | 主要约束 |
|---:|---|---|
| 1 | 开始会话 | 只允许 Idle 或 Summary |
| 2 | 暂停会话 | 只允许 Running |
| 3 | 恢复会话 | 只允许 Paused |
| 4 | 停止会话 | Running 或 Paused；先保存摘要 |
| 5 | 清零当前会话 | 只允许 Paused，并要求设备端二次确认配置 |
| 6 | 同步 UTC 和时区 | 不修改单调会话时长 |
| 7 | 设置用户资料 | 至少包含体重克数和资料 revision |
| 8 | 设置训练目标 | 次数、时长或卡路里目标 |
| 9 | 设置设备偏好 | 亮度、振动、声音、熄屏时间 |
| 10 | 获取状态快照 | 任意已绑定连接 |
| 11 | 开关实时原始流 | 仅开发者模式 |

### 9.4 命令 6、7、8、9、11 的冻结 TLV v1

五条配置命令的 `tlv_data` 均由连续项目组成，每项固定为：

```text
[type:u8][length:u8][value:length bytes]
```

多字节整数全部使用小端序。布尔值长度必须为 1，且只允许 `0` 或 `1`。已知 type 重复、必填项缺失、已知项长度错误、数值越界、截断 value 或尾随不完整 TLV 必须拒绝；未知 type 只能按 `length` 跳过，不能改变已知字段语义。

#### 命令 6：同步 UTC 与时区

| type | length | value | 范围 |
|---:|---:|---|---|
| `0x01` | 8 | `utc_unix_seconds:int64LE` | 946684800～4102444799，即 2000-01-01～2099-12-31 |
| `0x02` | 2 | `timezone_offset_minutes:int16LE` | -840～840 分钟 |

UTC 只用于历史墙钟时间，不允许重写 `monotonic_ms` 或当前会话时长。Windows 公共接口接收 Unix 毫秒，编码命令 6 时使用正数整数除法除以 1000，把 0～999 毫秒尾数截断后发送 Unix 秒；不得四舍五入，也不得把毫秒原值写入该字段。Windows 会话在首次连接和自动重连完成 Manifest、订阅与快照恢复后自动发送一次当前时间。

#### 命令 7：设置用户资料

| type | length | value | 范围 |
|---:|---:|---|---|
| `0x01` | 4 | `weight_g:uint32LE` | 30000～250000 g |
| `0x02` | 4 | `profile_revision:uint32LE` | 单调修订号 |

体重单位固定为克。PC 设置页可选公制千克或英制磅，但内部先统一换算为千克，再按四舍五入发送整数克；设备卡路里链只使用设备确认并持久化的资料。界面单位不进入 TLV，不允许把磅数直接写入 `weight_g`。

#### 命令 8：设置训练目标

| type | length | value | 范围 |
|---:|---:|---|---|
| `0x01` | 1 | `kind:uint8` | 0 无、1 次数、2 时长秒、3 毫千卡 |
| `0x02` | 4 | `value:uint32LE` | 无目标时必须为 0；启用时必须大于 0 |

`kind=3` 的值使用 milli-kcal，即 1000 表示 1 kcal。目标种类与数值组合无效时必须整体拒绝，不能只保留其中一项。

#### 命令 9：设置设备偏好

| type | length | value | 范围 |
|---:|---:|---|---|
| `0x01` | 1 | `brightness_percent:uint8` | 5～100% |
| `0x02` | 1 | `haptic_enabled:bool` | 0 或 1 |
| `0x03` | 1 | `sound_enabled:bool` | 0 或 1 |
| `0x04` | 2 | `screen_timeout_seconds:uint16LE` | 10～300 s |
| `0x05` | 4 | `preferences_revision:uint32LE` | 单调修订号 |
| `0x06` | 1 | `developer_mode_enabled:bool` | 0 或 1 |

#### 命令 11：开关实时 RawStream

| type | length | value | 范围 |
|---:|---:|---|---|
| `0x01` | 1 | `enabled:bool` | 0 或 1 |

开启命令只有在命令 9 已持久化 `developer_mode_enabled=1` 后才能成功。关闭、断线或退出时 PC 不得把实时 RawStream 写入历史或文件。

### 9.5 PC 设置同步事实

设置页先原子保存 `%LocalAppData%\IMUFitness\preferences.v1.json`，本地成功后才尝试设备同步。设备未连接、不支持配置命令或返回错误时，页面必须显示“本地已保存、设备未同步”及具体原因，不能把本地成功伪装成设备成功。设备连接时固定按以下顺序执行：

1. 命令 6 同步当前 UTC 和时区；
2. 命令 7 同步体重克数和资料 revision；
3. 命令 8 同步目标类型和值；
4. 命令 9 同步亮度、振动、声音、熄屏、偏好 revision 和开发者模式。

每条命令都必须收到成功 ControlResponse，页面才显示“设备同步成功”。C# `DeviceConfigurationCodec` 的黄金字节、边界和畸形测试锁定上述线上格式。

PC 单位选择只保存在本地偏好，旧偏好文件没有该字段时默认为公制。训练总结的目标进度来自设备摘要千卡除以本地每日千卡目标；“设备会话已保存”和“本地历史已保存”是两个独立事实，必须分别由 Stop ACK/摘要和本地仓储成功确认。

每个连接缓存最近 16 个 `request_id` 和对应响应。PC 在 2 秒内没有收到 indication 时，只能用同一 `request_id` 重试一次。重复请求返回缓存响应，不重复创建会话、清零或停止。

---

## 10. 事件

Event 负责低延迟提示，不是权威累计存储。v1 事件包括：

- `SESSION_STARTED`；
- `SESSION_PAUSED`；
- `SESSION_RESUMED`；
- `SESSION_STOPPED`；
- `ACTION_CHANGED`；
- `REPETITION_COUNTED`；
- `GOAL_REACHED`；
- `LOW_BATTERY`；
- `SENSOR_FAULT`；
- `STORAGE_FAULT`；
- `POWER_OFF_PENDING`。

### 10.1 EventV1 固定载荷

Event 逻辑帧的 payload 固定为 36 字节、小端序。它用于立即播放动画、振动回显和故障提示；PC 不能用事件自行累加次数或卡路里。

| 偏移 | 长度 | 字段 | 单位/范围 |
|---:|---:|---|---|
| 0 | 1 | `event_version` | 固定为 1 |
| 1 | 1 | `event_type` | 1～11，对应本节事件顺序 |
| 2 | 1 | `device_state` | 0～7 |
| 3 | 1 | `action_id` | 0～10，255 表示未知 |
| 4 | 1 | `metric_kind` | 0 无、1 次、2 步、3 秒 |
| 5 | 1 | `battery_percent` | 0～100，255 表示未知 |
| 6 | 2 | `quality_flags` | 数据质量位集合 |
| 8 | 4 | `session_sequence` | 持久化会话序号 |
| 12 | 4 | `event_sequence` | 会话内事件序号 |
| 16 | 4 | `state_revision` | 事件提交后的权威状态修订号 |
| 20 | 4 | `metric_delta` | 本次次数、步数或秒增量 |
| 24 | 4 | `metric_total` | 事件时刻累计值；掉包后由 Live State 恢复 |
| 28 | 4 | `calories_mcal` | 千分之一千卡 |
| 32 | 2 | `confidence_q15` | 0～65535 对应 0～1 |
| 34 | 2 | `detail_code` | 低电量门槛、故障或关机子码；普通事件为 0 |

事件编号固定为：1 开始、2 暂停、3 恢复、4 停止、5 动作改变、6 计数、7 目标完成、8 低电量、9 传感器故障、10 存储故障、11 准备关机。未知版本、事件、动作、状态、电量或指标单位必须拒绝，不能猜测解释。

设备端 `ble_service_encode_event_v1` 与 PC 端 `EventV1Codec` 必须逐字节一致。编解码时间和空间复杂度均为 $O(1)$；36 字节 payload 在 ATT MTU 23 下由统一 8 字节包络自动分片，PC 重组完成并验证逻辑帧 CRC 后才解释字段。

事件丢失时：

- 不重传普通计数事件；
- PC 用 Live State 的权威累计值恢复；
- 会话停止后通过摘要同步恢复最终结果；
- 控制命令结果必须走 indication，不使用易丢失 notification。

---

## 11. 会话同步与原始日志

### 11.1 唯一键

会话唯一键为：

```text
(device_id, session_seq)
```

`device_id` 来源于 Manifest。PC 当前原子 JSON 仓储以该组合作为幂等主键，重复同步不得产生第二条会话。

### 11.2 摘要

每条摘要至少包含：

- 开始和结束 UTC；
- 单调持续时间；
- 总卡路里；
- 11 类各自的指标单位和值；
- 11 类活动毫秒数；
- 固件、协议、模型和类别表版本；
- 用户资料 revision；
- 结束原因和质量标志；
- 是否存在原始日志。

摘要必须保存在 NVS 或其它非易失存储中。microSD 缺失不得阻止摘要保存。

### 11.3 TransferRequestV1

会话摘要同步只使用 `LIST` 和 `GET` 两种固定操作。TransferRequest 逻辑帧 payload 固定为 12 字节、小端序：

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 1 | `transfer_version` | 固定为 1 |
| 1 | 1 | `operation` | 1=`LIST`，2=`GET` |
| 2 | 2 | `page_size` | `LIST` 为 1～12；`GET` 固定为 0 |
| 4 | 4 | `request_id` | PC 生成的非零幂等请求号 |
| 8 | 4 | `cursor_session_seq` | `LIST` 为 PC 已持久化最大序号；`GET` 为目标序号 |

`LIST` 返回满足下式的摘要：

$$
session\_seq > cursor\_session\_seq
$$

结果按 `session_seq` 从小到大，即从旧到新排列；PC 每成功 UPSERT 一条才推进本地游标。页大小上限 12 对应设备端固定队列：

$$
12\times80=960\ \mathrm{bytes}
$$

因此设备不需要动态内存。`GET` 只用于精确补取或诊断，目标序号为 0 时必须拒绝。

同一 `request_id` 与完全相同的 12 字节请求重试时，设备必须重放相同响应和冻结数据页，不重新读取变化中的仓储。同一 `request_id` 携带不同字节时返回 `REQUEST_CONFLICT`，不得执行第二个查询。

### 11.4 TransferResponseV1

Transfer Control indication 的逻辑帧类型为 6，payload 固定 16 字节：

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 1 | `transfer_version` | 固定为 1 |
| 1 | 1 | `operation` | 回显 1=`LIST` 或 2=`GET` |
| 2 | 1 | `status` | 见下表 |
| 3 | 1 | `flags` | bit0=`HAS_DATA`，bit1=`END` |
| 4 | 4 | `request_id` | 回显请求号 |
| 8 | 4 | `next_cursor_session_seq` | 本页最后摘要序号；空页保持输入游标 |
| 12 | 2 | `total_count` | 生成响应时设备持有的全部摘要数，0～200 |
| 14 | 2 | `item_count` | 本页 TransferData 数量，0～12 |

状态值固定为：

| 数值 | 名称 | 含义 |
|---:|---|---|
| 0 | `OK` | 请求成功，允许空页 |
| 1 | `UNSUPPORTED_VERSION` | payload 版本不支持 |
| 2 | `INVALID_OPERATION` | 未知操作码 |
| 3 | `INVALID_REQUEST` | 请求号、页大小或游标非法 |
| 4 | `NOT_FOUND` | `GET` 目标不存在 |
| 5 | `BUSY` | 上一页尚未从设备固定队列取完 |
| 6 | `STORAGE_ERROR` | 摘要仓储未初始化、读取失败或记录损坏 |
| 7 | `REQUEST_CONFLICT` | 同一请求号对应不同请求字节 |

必须满足：

$$
HAS\_DATA \iff item\_count>0
$$

`END=1` 表示当前游标已经追平设备快照。PC 仍须先成功持久化本页全部摘要，再把 `next_cursor_session_seq` 保存为新的同步游标。

### 11.5 TransferDataV1 与 64 字节摘要

Transfer Data notification 的逻辑帧类型为 7。每条 payload 固定 80 字节，由 16 字节页头和一条 64 字节摘要组成：

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 1 | `transfer_version` | 固定为 1 |
| 1 | 1 | `data_kind` | 固定为 1=`SessionSummaryV1` |
| 2 | 1 | `flags` | bit0=`LAST_IN_PAGE`，bit1=`END` |
| 3 | 1 | `reserved` | 固定为 0 |
| 4 | 4 | `request_id` | 对应请求号 |
| 8 | 2 | `item_index` | 本页索引 0～`item_count-1` |
| 10 | 2 | `item_count` | 本页总数，1～12 |
| 12 | 2 | `total_count` | 与响应一致的设备摘要总数 |
| 14 | 2 | `reserved2` | 固定为 0 |
| 16 | 64 | `summary` | 下表固定摘要 |

`SessionSummaryV1` 与设备 `session_store` 落盘记录逐字节一致：

| 摘要内偏移 | 长度 | 字段 | 单位/范围 |
|---:|---:|---|---|
| 0 | 2 | `summary_version` | 固定为 1 |
| 2 | 2 | `summary_length` | 固定为 64 |
| 4 | 4 | `session_seq` | 非零持久化序号 |
| 8 | 4 | `last_event_seq` | 已吸收最大事件序号 |
| 12 | 1 | `action_id` | 0～10 |
| 13 | 1 | `metric_kind` | 0 次、1 步、2 持续毫秒 |
| 14 | 2 | `flags` | 完成、异常、UTC 有效等位集合 |
| 16 | 8 | `start_unix_ms` | UTC Unix 毫秒；0 表示未校时 |
| 24 | 8 | `duration_ms` | 单调持续毫秒 |
| 32 | 8 | `metric_total` | 单位由 `metric_kind` 决定 |
| 40 | 8 | `gross_microkcal` | 毛热量，百万分之一千卡 |
| 48 | 8 | `active_microkcal` | 活动热量，百万分之一千卡 |
| 56 | 2 | `average_stability_q15` | 0～32767 |
| 58 | 2 | `minimum_stability_q15` | 0～32767 |
| 60 | 4 | `event_count` | 摘要吸收的事件数 |

PC 把毛热量转换为千分之一千卡：

$$
calories\_mcal=\operatorname{round}\left(\frac{gross\_microkcal}{1000}\right)
$$

持续型动作把 `metric_total` 毫秒整除 1000 后显示为秒。所有 `uint64` 字段在转换到 PC v1 的 `uint32` 前必须检查上界，禁止静默截断。

### 11.6 同步时序、幂等与资源

固定页时序：

```text
PC 写 TransferRequest(0005)
    -> ESP32 冻结最多12条摘要
    -> ESP32 indication TransferResponse(0005)
    -> indication 全部分片确认
    -> ESP32 notification 0..N-1 条 TransferData(0006)
    -> PC 按 item_index 去重并排序
    -> PC 原子幂等写入 (device_id,session_seq)
    -> PC 仅在整页落盘成功后保存 next_cursor
```

设备端 GATT 写回调只做解码和固定内存快照，不在 NimBLE 主机任务内阻塞发送 0006。应用 BLE 任务在类型 6 indication 确认后，调用 `session_transfer_service_pop_data`，再调用 `ble_service_nimble_publish_transfer_data`。

PC 订阅 0005 indication 和 0006 notification，两个特征使用独立分片重组器。2 秒没有响应或 5 秒没有收齐数据时，最多使用相同请求号和完全相同帧重试一次。重复 `item_index` 可忽略；最终仓储仍以：

```text
(device_id, session_seq)
```

执行当前原子 JSON 仓储的主键替换。重复同步不能增加第二条记录；若未来更换数据库，也必须保持同一幂等语义。

Windows 产品入口把 `ISessionHistorySyncSource` 注入历史页。应用启动后的手动刷新和每次连接/重连成功都会：

1. 从本地 JSON 查询当前 `device_id` 的最大 `session_seq` 作为 cursor；
2. 每页最多请求 12 条，最多连续处理 32 页，防止异常设备造成无限循环；
3. 校验摘要 `device_id`、页内顺序和 `next_cursor` 必须前进；
4. 以 `(device_id, session_seq)` 原子幂等写入 JSON；
5. 只有整页写入成功才继续下一 cursor；
6. 同步失败时保留本地历史可读，并显示设备补传错误。

单页编解码时间复杂度为 $O(P)$，其中 $P\le12$；设备队列空间为 $O(P)$ 且上限 960 字节。PC 页内字典最多 12 项。逻辑帧 CRC16 已覆盖完整 16/80 字节 payload，所以本层不再叠加摘要 CRC；设备双槽存储自身仍由 CRC32 和提交标记保护。

### 11.7 同步诊断样本记录

RawStream v1 是开发诊断接口，不是 QMI8658 FIFO 原样转发。固件先把芯片采样经过抗混叠滤波和严格 25 Hz 重采样，得到 `deg/s` 与 `g` 物理量，再按 QMI8658 当前量程比例重新量化为 `int16LE` 固定点诊断码。该设计保持 22 字节协议稳定，便于 PC 低成本显示和定位丢样，同时明确禁止把诊断码解释为未经处理的 ADC/FIFO 原始值。

记录格式：

```text
uint32 sample_index
uint32 monotonic_ms
int16  gx_raw
int16  gy_raw
int16  gz_raw
int16  ax_raw
int16  ay_raw
int16  az_raw
uint16 quality_flags
```

元数据固定声明：

- 采样率：25 Hz；
- 通道顺序：`gx, gy, gz, ax, ay, az`；
- 陀螺仪诊断码：`code / 16.4 = filtered_dps`；
- 加速度计诊断码：`code / 4096 = filtered_g`；
- `code` 按 `round(physical_value / units_per_lsb)` 生成，并饱和到 `[-32768, 32767]`；
- `quality_flags` 描述滤波、重采样、振动污染或丢样等设备端质量事实。

因此，字段名中的 `_raw` 只为保持已经冻结的 v1 线上字段和 PC API 兼容；它表示“未在 PC 端换算的固定点码”，不表示“未经过设备端处理的 FIFO 原始码”。后续协议版本若要传输芯片 FIFO 原始样本，必须使用新的版本或特征，不得静默改变本记录语义。

单条记录 22 字节，理论数据率：

$$
22\times25=550\ \mathrm{bytes/s}
$$

一小时约：

$$
550\times3600=1{,}980{,}000\ \mathrm{bytes}
$$

不含文件头和块校验时约 1.89 MiB。

特征 `0007` 的实时 RawStream 使用上述固定 22 字节 payload，并套用统一逻辑帧、CRC16 与分片重组。Windows 会话始终订阅 `0007`，但只有诊断页显式发送命令 11 且设备成功 ACK 后才把合法样本发布给 UI。诊断页仅保存最近样本和进程内计数：

- 开发者模式默认关闭；
- 开启失败必须显示设备错误，不能显示虚假开启；
- 固定长度不是 22 字节、CRC 错误或分片错误的记录必须丢弃；
- 关闭或断线后停止发布，迟到回调不更新页面；
- UI 不提供保存按钮，不写会话 JSON、诊断日志或本地二进制文件；
- 如以后实现设备 TF 离线原始文件下载，应使用独立授权和 CRC32 续传流程，不能复用实时诊断开关。

### 11.8 下载与续传

- 每个逻辑数据块 payload 最多 1024 字节；
- 每 4 块由 PC 确认下一期望文件偏移；
- 断线后从最后已经 CRC 验证的偏移继续；
- 单个逻辑帧使用 CRC16；
- 完整文件使用 CRC32；
- 实时 Raw Stream 尽力而为，不重传，丢包通过 sample_index 缺口记录；
- SD 离线文件下载必须最终 CRC32 一致。

---

## 12. 版本与 Manifest

### 12.1 Manifest v1 外层编码

特征 `0002` 的值直接是连续 TLV，不套用第 4 节逻辑帧，不附加 CRC16。每项格式固定为：

```text
[tag:u8][length:u8][value:length bytes]
```

- `tag` 和 `length` 均为一字节；单项 value 最长 255 字节；
- 多字节整数全部使用小端序；
- 字符串使用 UTF-8 原始字节，不包含 C 字符串结尾 NUL；
- SHA-256 使用摘要显示顺序的 32 个原始字节，不按整数翻转，也不发送 64 字节十六进制文本；
- 未知 tag 必须按 `length` 跳过；重复已知 tag、截断 value 或尾随不完整 TLV 必须拒绝；
- 当前设备 Manifest 约 132 字节，超过 MTU 23 的单次 ATT Value；客户端必须支持 GATT Long Read/Read Blob，不能只读首 20 字节；
- 设备启动时先完整构建和校验，再交给 NimBLE 深拷贝；任一必填字段无效时不允许退回空 Manifest。

### 12.2 固定标签表

| tag | length | value | 当前来源 |
|---:|---:|---|---|
| `0x01` | 可变 | `device_id` UTF-8 | 完整 48 位蓝牙 MAC，例如 `A1B2C3D4E5F6` |
| `0x02` | 可变 | 板卡修订 UTF-8 | `2.06` |
| `0x03` | 2 | `[protocol_major:u8, protocol_minor:u8]` | 共享协议宏，当前 `1,0` |
| `0x04` | 可变 | 固件语义版本 UTF-8 | 当前 `0.1.0` |
| `0x05` | 4 | `[feature_dim:u16LE, feature_version:u16LE]` | 当前 `297,1` |
| `0x06` | 32 | 基础 M0 SHA-256 原始摘要 | 自动生成 `BP_BASE_MODEL_SHA256` |
| `0x07` | 32 | 掩码 M0 SHA-256 原始摘要 | 自动生成 `BP_MASKED_MODEL_SHA256` |
| `0x08` | 5 | `[class_count:u8, class_table_crc32:u32LE]` | 当前 `11,0xD8193927` |
| `0x09` | 2 | 卡路里 milliMET 表版本 `u16LE` | 当前 `1` |
| `0x0A` | 4 | 能力位 `u32LE` | 见 12.4 节 |
| `0x0B` | 8 | Manifest 构建时 LittleFS 可用字节 `u64LE` | `total_bytes-used_bytes`，异常下溢钳制为 0 |

本版只发布实际部署的基础 M0 与掩码 M0 两个 SHA。`dual_m0_manifest.json` 组合清单没有独立编译期常量，因此 v1 不把“组合清单 SHA”列为必填 tag；以后需要时必须分配新 tag，旧客户端按未知 TLV 跳过，不能复用 `0x06` 或 `0x07`。

`0x0B` 只是启动/Manifest 构建时快照。后续保存会话会减少剩余容量，PC 只能把它当诊断提示，不能当作写入承诺。`esp_littlefs_info` 失败时设备不发布缺字段 Manifest，BLE 保持离线，设备仍可独立训练。

### 12.3 类别表 CRC32

类别 CRC 使用 CRC-32/ISO-HDLC：

| 参数 | 数值 |
|---|---|
| 宽度 | 32 bit |
| 正向多项式 | `0x04C11DB7` |
| 反射实现多项式 | `0xEDB88320` |
| 初值 | `0xFFFFFFFF` |
| 输入/输出反射 | true |
| 最终异或 | `0xFFFFFFFF` |

设模型输出顺序中的 UTF-8 类名为 $n_0,\ldots,n_{C-1}$。规范输入字节串为：

$$
B=\operatorname{UTF8}(n_0)\Vert 0x00\Vert
\operatorname{UTF8}(n_1)\Vert 0x00\Vert\cdots\Vert
0x00\Vert\operatorname{UTF8}(n_{C-1})
$$

相邻类名之间恰好一个 `0x00`，最后一个类名末尾不加 `0x00`。当前 11 类固定顺序为：

```text
good_morning, jumping_jack, jumping_lunge, jumping_squat, lunge,
sit, squat, trot, tuck_jump, walk, wave
```

该规范字节串的黄金结果为：

```text
class_table_crc32 = 0xD8193927
```

类别名内容或索引顺序任一变化都必须改变 CRC。PC 只有 CRC 相同才允许把 logits/action ID 映射为本地动作名称和动画。

CRC 时间复杂度为 $O(S)$，$S$ 为全部类名 UTF-8 字节数；额外空间 $O(1)$。设备只在启动构建 Manifest 时计算一次。

### 12.4 能力位

| bit | 掩码 | 语义 | 当前发布 |
|---:|---:|---|---|
| 0 | `0x00000001` | 有效重复动作的板载马达振动反馈 | 是 |
| 1 | `0x00000002` | LittleFS 最近会话摘要 LIST/GET 分页补传 | 是 |
| 2 | `0x00000004` | 内部 Flash LittleFS 双槽会话持久化 | 是 |
| 3 | `0x00000008` | 开发者 25 Hz 六轴同步诊断流已接入主应用 | 否 |
| 4 | `0x00000010` | TF 离线原始日志和续传闭环 | 否 |
| 5 | `0x00000020` | 音频提示闭环 | 否 |
| 6～31 | - | 保留，必须为 0 | 否 |

“板上存在硬件”或“GATT 已预留特征”不等于能力已实现。当前发布值严格为：

```text
capabilities = 0x00000007
```

SD、Raw Stream 和音频在主应用没有端到端闭环，因此不得置位。

### 12.5 构建安全与资源

Manifest builder 先验证全部字符串、SHA、类表和所需容量，随后一次写入：

- 空指针、空必填字符串、超过 255 字节字符串、非 64 位十六进制 SHA 均拒绝；
- 容量不足时 `output_length=0`，输出缓冲区保持不变；
- 时间复杂度 $O(M+S)$，$M$ 为 Manifest 字节数，$S$ 为类名总字节数；
- 固定临时空间为两个 32 字节 SHA、少量整数 value 和调用方 512 字节静态输出；
- LittleFS 可用字节采用 `u64LE`，从 `size_t` 转换前先检查 `used<=total`，防止无符号下溢。

兼容规则：

- 主版本不同：禁止训练控制，允许读取设备信息和导出仍可识别的数据；
- 次版本更高：忽略未知 TLV 和未知消息；
- 类别表 CRC 不同：禁止动作名称和动画映射，提示升级 PC；
- 模型哈希变化但类别表相同：允许使用，并把哈希保存到会话；
- GATT 数据库变化：设备发送 Service Changed，PC 使用 Uncached 重新枚举。

---

## 13. 断线、超时与恢复

### 13.1 实时状态

- Running 状态约每 480 ms 发布一次 Live State；
- 计数、状态或故障变化立即发布 Event；
- 3 秒没有新状态：PC 显示“数据延迟”；
- 10 秒没有新状态：PC 释放旧 GATT 对象并开始重连；
- 重连退避：1、2、4、8、15 秒，之后保持 15 秒。

### 13.2 重连同步

重连后固定执行：

1. 读取 Manifest；
2. 检查协议主版本和类别 CRC；
3. 重新订阅 indication/notification；
4. 获取完整状态快照；
5. 使用 `(device_id, session_seq)` 恢复当前会话；
6. 根据本地 `sync_cursor` 请求缺失摘要；
7. 有未完成文件下载时从最后确认偏移继续。

ESP32 在 PC 断开期间继续识别、计数和累计卡路里。PC 不根据断线时长猜测结果。

---

## 14. Windows 上位机边界

### 14.1 项目层次

- `FitnessCoach.Domain`：动作、设备状态、指标单位和实时状态；
- `FitnessCoach.Bluetooth`：帧、CRC、分片、重组、payload codec、Mock 与 WinRT BLE 会话；
- `FitnessCoach.Infrastructure`：原子 JSON 会话/偏好仓储、路径和导出边界；
- `FitnessCoach.SessionTransfer`：设备历史分页、幂等写入和同步游标；
- `FitnessCoach.App`：六页 WPF、11 类本地矢量动画、实时状态和历史页面。

BLE 回调不得直接更新 WPF 控件。正式程序采用：

```text
GattCharacteristic.ValueChanged
    -> Channel<byte[]>
    -> 后台重组和解码
    -> 权威状态库
    -> Dispatcher 更新 UI
```

### 14.2 本地存储

当前位置：

```text
%LocalAppData%\IMUFitness\sessions.v1.json
%LocalAppData%\IMUFitness\preferences.v1.json
```

`JsonSessionRepository` 和 `JsonUserPreferencesStore` 使用同目录临时文件完成原子替换，不依赖 SQLite。会话以 `(device_id, session_seq)` 幂等更新。当前实时 RawStream 只在诊断页内存显示，不创建 `raw` 目录、不写二进制文件，也不把样本嵌入 JSON。未来若增加设备 TF 离线文件下载，必须另行设计用户授权、路径、元数据、CRC32 和续传状态。

---

## 15. 黄金向量

共享黄金文件：`shared/protocol/golden_vectors.json`。

标准 CRC：

```text
ASCII: 123456789
CRC:   29B1
```

逻辑帧：

```text
7EB1010003013412040302010500102030405092F9
```

MTU23 第一片：

```text
3412000002000C007EB101000301341204030201
```

MTU23 第二片：

```text
34120100020009000500102030405092F9
```

C 与 C# 必须同时通过上述向量，任何一个字节不同都视为协议不兼容。

---

## 16. 测试与验收

### 16.1 C 参考实现

```powershell
gcc -std=c11 -Wall -Wextra -Werror -pedantic `
  shared\protocol\imu_ble_protocol.c `
  shared\protocol\test_imu_ble_protocol.c `
  -Ishared\protocol `
  -o .codex-local\tmp\imu_ble_protocol_test.exe

.\.codex-local\tmp\imu_ble_protocol_test.exe
```

预期唯一成功标志：

```text
C_PROTOCOL_TESTS_OK
```

### 16.2 C# 离线测试

```powershell
dotnet run --project pc\FitnessCoach.Tests\FitnessCoach.Tests.csproj
```

预期唯一成功标志：

```text
CSHARP_PROTOCOL_TESTS_OK
```

### 16.3 必测场景

- CRC 标准向量；
- C/C# 完整帧逐字节一致；
- MTU23、185、247 分片；
- CRC 错误、截断、尾随垃圾；
- 缺少索引 0、乱序、重复片和跨 sequence；
- LiveState 全字段往返；
- 非法动作、电量、目标和指标单位；
- 断线重连后的状态快照；
- 重复控制请求幂等；
- 会话重复同步不产生重复记录；
- 文件下载中断和 CRC32 续传；
- 网络完全断开时上位机仍可运行；
- 协议主版本和类别表 CRC 不兼容时禁止错误控制和动画。

---

## 17. 当前实现范围

当前已经实现：

- C 逻辑帧编码和解码；
- C CRC-16/CCITT-FALSE；
- C MTU 自适应分片和严格顺序重组；
- C# 同结构编解码；
- C# CRC、分片、重组；
- C# LiveStateV1 编解码与领域对象；
- C/C# 共用黄金向量测试；
- ESP-NimBLE GATT 服务、broker、安全参数、控制命令、Manifest、LiveState、Event和Transfer；
- 设备LittleFS最近200条双槽会话摘要与BLE分页传输；
- Windows扫描、WinRT连接、订阅、同ID重试、退避重连和历史同步；
- 设备六位码邮箱/中文显示/全生命周期清除、设备端忘记电脑和 Windows 端忘记设备；
- Windows六页WPF、11类30 FPS本地矢量动画和Mock BLE演示；
- PC原子JSON会话/偏好仓储、公英制体重显示、目标与双端保存状态、连接后自动校时、设置配置同步、重连历史补传和开发者 RawStream 内存诊断；
- 2026-07-15 ESP-IDF 5.5.4整机构建，应用镜像`0x13def0`，4 MiB应用分区余`0x2c2110`，约69%。

尚待真板验证：

- AMOLED六位码安全配对与绑定删除；
- MTU23和实际协商MTU下的分片、通知与indication；
- 弱信号、断线重连、历史恢复和一小时持续通信；
- LittleFS写入阶段掉电、真实Flash寿命、TF拔卡和可选原始日志续传；
- 真板BLE、UI、存储并发时的任务栈、堆、功耗和长时间稳定性。
