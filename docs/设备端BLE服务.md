# ESP32-S3 设备端 BLE 服务

## 1. 目的与边界

本组件把健身手柄的设备状态、控制命令、计数事件、会话摘要和可选原始 IMU 数据暴露给 Windows 上位机。ESP32 始终是动作、次数、步数、时长和卡路里的唯一权威源；PC 只显示设备给出的累计值，不在断线期间自行猜测。

实现分两层：

```text
共享帧层 shared/protocol
    ├─ 逻辑帧 14 字节固定头
    ├─ CRC-16/CCITT-FALSE
    └─ 8 字节 ATT 分片包络与严格重组

设备服务层 esp32/firmware/components/ble_service
    ├─ ble_service_core.c：纯 C、无 FreeRTOS/NimBLE 依赖
    └─ ble_service_nimble.c：ESP-IDF 5.5.4 GATT、安全、广播和通知
```

纯 C 核心可在 Windows 使用 GCC 验证；硬件层由 ESP-NimBLE 编译并在烧录后验证射频、配对和 Windows 兼容性。

## 2. GATT 数据库

自定义主服务 UUID：

```text
7B2E0000-6D57-4A51-9E43-494D5542504E
```

| 尾号 | 名称 | 权限 | 消息 |
|---|---|---|---|
| `0001` | Control Point | 加密 Write、Indicate | 1 `ControlRequest` / 2 `ControlResponse` |
| `0002` | Manifest | Read | 原始 Manifest TLV，不占用消息类型 |
| `0003` | Live State | Read、Notify | 3 `LiveState` |
| `0004` | Event | Notify | 4 `Event` |
| `0005` | Transfer Control | 加密 Write、Indicate | 5 `TransferRequest` / 6 `TransferResponse` |
| `0006` | Transfer Data | Notify | 7 `TransferData` |
| `0007` | Raw Stream | Notify | 8 `RawStream` |

同时注册：

- Battery Service `0x180F`，Battery Level `0x2A19`，支持 Read/Notify；
- Device Information Service `0x180A`；
- Manufacturer `0x2A29`、Model `0x2A24`、Serial `0x2A25`、Hardware Revision `0x2A27`、Firmware Revision `0x2A26`；
- 标准 GATT Service Changed，供数据库升级后 Windows 重新枚举。

`0002` 和 `0003` 的 Read 返回完整属性值，ATT Long Read 的分包由协议栈负责；Write、Indicate 和 Notify 使用本文第 4 节的 8 字节应用分片包络。

## 3. 逻辑帧与消息类型

逻辑帧总长度：

$$
L = 14 + P + 2
$$

其中：

- $P$：payload 长度，范围 0～1024 字节；
- 14：固定头长度；
- 2：CRC16 长度；
- $16\le L\le1040$。

固定头、端序和 CRC 算法只由 `shared/protocol/imu_ble_protocol.c` 实现。BLE 服务不能复制另一套 CRC 或长度解释。八种消息固定为：

| 数值 | 消息 | 方向 |
|---:|---|---|
| 1 | ControlRequest | PC → ESP32 |
| 2 | ControlResponse | ESP32 → PC |
| 3 | LiveState | ESP32 → PC |
| 4 | Event | ESP32 → PC |
| 5 | TransferRequest | PC → ESP32 |
| 6 | TransferResponse | ESP32 → PC |
| 7 | TransferData | ESP32 → PC |
| 8 | RawStream | ESP32 → PC |

类型 0 或大于 8 的逻辑帧不得进入 v1 业务回调。

## 4. ATT 分片和重组

协商 ATT MTU 为 $M$ 时，一个 GATT Value 的可用长度为：

$$
V=M-3
$$

应用分片包络占 8 字节，单片逻辑帧数据容量为：

$$
C=M-3-8=M-11
$$

完整帧所需片数：

$$
N_f=\left\lceil\frac{L}{C}\right\rceil
$$

MTU 23 时，$C=12$。因此即使 30 字节 LiveState payload，也会产生多片。设备端规则：

1. 只有索引 0 可以开始新帧；
2. sequence 和 fragment_count 必须与首片一致；
3. 索引严格递增，不缓存乱序片；
4. 累计长度不得超过 1040；
5. 最后一片到达后重新校验完整帧长度和 CRC；
6. 错片、坏 CRC、断连或 NimBLE 主机复位立即清空半帧。

Control Point 与 Transfer Control 各有独立重组器，不能把 `0001` 的前半帧与 `0005` 的后半帧拼接。

## 5. 控制请求和响应

### 5.1 请求

```text
uint32 request_id        // 小端
uint8  command_id        // 1..11
uint8  command_version   // 当前为 1
uint8  tlv[]             // 可选，最多 250 字节
```

v1 命令：

| ID | 含义 |
|---:|---|
| 1 | 开始会话 |
| 2 | 暂停会话 |
| 3 | 恢复会话 |
| 4 | 停止并保存会话 |
| 5 | 清零当前会话 |
| 6 | 同步 UTC 与时区 |
| 7 | 设置用户资料 |
| 8 | 设置训练目标 |
| 9 | 设置设备偏好 |
| 10 | 获取状态快照 |
| 11 | 开关开发者原始流 |

### 5.2 响应

```text
uint32 request_id
uint8  command_id
uint8  status
uint16 error_code
uint32 state_revision
uint8  tlv[]
```

控制响应必须使用 indication。它是命令提交结果，不能使用可能静默丢失的 notification。

### 5.3 16 项幂等缓存

Windows 在 2 秒内未收到 indication 时，会用同一 `request_id` 重试。若“开始”“停止”或“清零”被重复执行，会造成重复会话或数据丢失。因此每个连接缓存最近 16 项：

```text
request_id
完整请求 payload，最大 256 字节
完整响应 payload，最大 256 字节
```

判定规则：

$$
same = (L_{new}=L_{cache}) \land
\bigwedge_{i=0}^{L-1}(b_{new,i}=b_{cache,i})
$$

- `request_id` 相同且全部请求字节相同：直接返回缓存响应，业务回调执行次数不增加；
- `request_id` 相同但任一字节不同：返回 `REQUEST_CONFLICT / 0x0103`，不执行回调，也不覆盖原缓存；
- 新 `request_id`：执行一次回调，保存请求和响应；
- 第 17 个新请求覆盖最旧的第 1 个请求；
- 断连后整个缓存清零，新连接重新建立幂等范围。

本实现选择保存完整字节，而不是 CRC32 或哈希。这样不存在哈希碰撞把不同命令误判为合法重试的可能。

资源上限约为：

$$
16\times(256+256+\text{元数据})\approx 8.4\,\mathrm{KiB}
$$

加两个 1040 字节重组器后，每连接核心约 10.5 KiB。设备只允许一个 PC 连接，内存固定且没有动态分配。

## 6. LiveStateV1

实时状态固定 30 字节：

| 偏移 | 长度 | 字段 | 单位/范围 |
|---:|---:|---|---|
| 0 | 4 | `session_sequence` | 持久化序号 |
| 4 | 4 | `state_revision` | 权威修订号 |
| 8 | 4 | `elapsed_ms` | ms |
| 12 | 1 | `device_state` | 0～7 |
| 13 | 1 | `action_id` | 0～10 或 255 |
| 14 | 1 | `metric_kind` | 0无、1次、2步、3秒 |
| 15 | 1 | `battery_percent` | 0～100 或 255 |
| 16 | 4 | `metric_value` | 次、步或秒 |
| 20 | 2 | `confidence_q15` | 0～65535 |
| 22 | 4 | `calories_mcal` | 千分之一千卡 |
| 26 | 2 | `quality_flags` | 位集合 |
| 28 | 1 | `power_flags` | 位集合 |
| 29 | 1 | `goal_percent` | 0～100 或 255 |

置信度换算：

$$
c=\frac{q}{65535}
$$

卡路里换算：

$$
E_{kcal}=\frac{E_{mcal}}{1000}
$$

设备每次发布前检查状态、动作、指标、电量和目标范围。PC 只显示 `metric_value`，Event 只触发动画和提示。

## 7. Notification 与 Indication

### 7.1 Notification

Live State、Event、Transfer Data 和 Raw Stream 使用 notification：

- 逻辑帧先编码 CRC；
- 按当前 MTU 拆成 8 字节包络分片；
- 只有 PC 已订阅对应 CCCD 才发送；
- Live State 即使未连接也保存最新完整帧，重连后 Read 可恢复；
- Event 丢失不补算次数；
- Transfer Data 用文件偏移和最终 CRC32 恢复；
- Raw Stream 尽力而为，不重传。

### 7.2 Indication

NimBLE 同一连接一次只能有一个未确认 indication。设备使用单槽状态机：

```text
复制完整响应帧
    -> 发送分片 0
    -> 等 BLE_GAP_EVENT_NOTIFY_TX 确认
    -> 发送分片 1
    -> ...
    -> 最后一片确认后释放槽
```

若槽忙，新的控制写返回资源不足。业务若已经执行，其响应已进入 16 项缓存；PC 用同一 `request_id` 重试只会返回缓存，不会重复执行。

## 8. 配对与权限

安全设置：

- Bluetooth Low Energy only；
- LE Secure Connections；
- bonding；
- MITM；
- Display Only 六位随机码；
- Control Point 与 Transfer Control 使用 `WRITE_ENC`；
- 未绑定客户端可读取 Manifest 和标准设备信息；
- 重复配对不会自动删除旧密钥，必须由设备设置页执行“忘记电脑”。

六位码由 `esp_random()` 产生，范围 000000～999999，并通过 `passkey_display` 回调交给应用的原子邮箱。该回调在 NimBLE 主机任务中执行，只写固定大小邮箱并快速返回，不能直接调用 LVGL、等待触摸或分配动态内存；UI 任务再把事件渲染为中文六位码提示。

`passkey_clear` 按稳定原因清除 UI：安全绑定成功、握手失败、物理断线、用户忘记电脑或 BLE 服务停止。UI 自己还在显示满 60 秒时产生超时清除。每条清除路径都把保存码改为 0，防止旧码泄漏到后续页面或重连。

设备设置页调用 `ble_service_nimble_forget_all_bonds()`。该函数先终止当前连接，再调用 NimBLE 官方绑定存储清理；任一步失败都返回错误，UI 不能伪装成功。删除绑定不清除会话历史、用户资料或设备偏好。

当前 Numeric Comparison 路径自动接受双方相同数字。正式硬件若把确认按钮加入配对页，应改为用户确认后再调用 `ble_sm_inject_io`。

## 9. 线程和生命周期

- 配置字符串和 Manifest 在 `ble_service_nimble_start` 内深拷贝；调用者返回后可释放原字节缓冲区；
- command、transfer、passkey 显示/清除和 connection_changed 回调指针及 context 必须保持到 `ble_service_nimble_stop` 返回；
- Control 回调只会在完整帧 CRC 通过、命令 ID/版本检查之后调用；
- 回调不得阻塞 NimBLE 主机任务；耗时存储应投递到产品事件队列并返回可追踪状态；
- 断连清空两个重组器、16 项幂等缓存、订阅标志、未完成 indication 和屏幕配对码；
- 发布 API 应由单一产品 BLE/状态发布任务串行调用，避免多个业务任务竞争通知顺序。

`connection_changed(connected, att_mtu, context)` 在 PC 建连后上报 `true`，在远端断连或主动停止 BLE 前上报 `false`。回调运行在 NimBLE 主机上下文，只能把小型事件投递到应用队列；UI 图标、电源状态和重连策略由应用任务更新。`ble_service_nimble_is_connected()` 只供诊断快照读取，不能用轮询替代连接事件，否则可能漏掉短连接和断连原因。

## 10. 复杂度与功耗

| 操作 | 时间复杂度 | 固定内存 |
|---|---:|---:|
| CRC | $O(L)$ | $O(1)$ |
| 分片编码 | $O(L)$ | 最多约 1048 B 栈缓冲 |
| 严格重组 | $O(L)$ | 每特征 1040 B |
| 16 项 request_id 查找 | $O(16)$ | 约 8.4 KiB 缓存 |
| LiveState 编码 | $O(30)$ | 30 B |

运行中 Live State 建议约每 480 ms 一次，与 IMU 窗口步长一致。待机或熄屏时应降低状态通知频率并增加连接间隔；原始流默认关闭。厂家原配 400 mAh 电池预算下，BLE 不应持续以开发者原始流和最短连接间隔运行。

## 11. 主机测试

执行：

```powershell
& .\esp32\host_tests\ble_service\run_tests.ps1
```

测试使用 C11、`-Wall -Wextra -Wpedantic -Werror -fanalyzer`，覆盖：

- 消息类型 1～8；
- 控制命令 1～11；
- LiveStateV1 固定 30 字节和非法范围；
- 正常控制请求与响应；
- CRC、消息类型和控制长度错误；
- 精确重复请求不重复执行；
- 相同 `request_id` 不同 payload 冲突；
- 最近 16 项缓存轮转；
- MTU23 多分片；
- 断连清除半帧和 request_id 缓存。
- 六位码显示/成功/失败/断线/停服清除，以及忘记电脑时先断开再清绑定。

成功标志：

```text
BLE_SERVICE_TESTS_OK assertions=254
```

## 12. ESP-IDF 5.5.4 构建要求

组件 `CMakeLists.txt` 依赖：

```text
bt
esp_timer
esp_hw_support
```

必须启用：

```text
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_GATT_SERVER=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
```

当前 NimBLE API 已按官方 ESP-IDF v5.5.4 源码核对。没有真实开发板时，只能完成编译和主机测试；烧录后仍需验证：

1. Windows 枚举自定义 0001～0007、Battery 和 Device Information；
2. 六位配对码、成功/失败/断线/超时清除、绑定重连和忘记电脑；
3. MTU 23、185、247 分片；
4. 连续 100 次 indication 无队列耗尽；
5. 断连中有半帧时，重连后旧数据不续接；
6. 30 分钟 LiveState 通知无内存下降；
7. 熄屏、待机和低电量模式的连接间隔与平均电流。
