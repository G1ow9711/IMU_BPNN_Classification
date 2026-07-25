#ifndef BLE_SERVICE_CORE_H
#define BLE_SERVICE_CORE_H

// 完整帧、CRC 与 ATT 分片统一复用 shared/protocol，禁止设备端维护第二套线上格式。
#include "imu_ble_protocol.h"

// 引入 size_t，所有缓冲区容量均使用无符号平台尺寸类型表达。
#include <stddef.h>
// 引入固定宽度整数，确保 ESP32 与 Windows 对线上字段宽度理解一致。
#include <stdint.h>

#ifdef __cplusplus
// C++ 调用者进入 C 链接约定，避免 ESP-IDF C++ 组件发生符号改名。
extern "C" {
#endif

// 每个 BLE 连接必须保留最近 16 个控制请求，以保证超时重试不会重复执行业务动作。
#define BLE_SERVICE_REQUEST_CACHE_SIZE ((size_t)16U)
// 控制请求 payload 上限为 256 字节，含 6 字节固定头和最多 250 字节 TLV。
#define BLE_SERVICE_MAX_CONTROL_REQUEST_PAYLOAD ((size_t)256U)
// 控制响应 payload 上限为 256 字节，含 12 字节固定头和最多 244 字节 TLV。
#define BLE_SERVICE_MAX_CONTROL_RESPONSE_PAYLOAD ((size_t)256U)
// LiveStateV1 payload 固定为 30 字节，字段偏移必须与 C# LiveStateCodec 完全一致。
#define BLE_SERVICE_LIVE_STATE_V1_SIZE ((size_t)30U)
// EventV1 payload 固定为 36 字节，供 PC 立即播放动画但不取代 LiveState 权威累计值。
#define BLE_SERVICE_EVENT_V1_SIZE ((size_t)36U)
// InferenceDiagnosticV1 payload 固定为 28 字节，传输双 M0 与融合分类的真机诊断证据。
#define BLE_SERVICE_INFERENCE_DIAGNOSTIC_V1_SIZE ((size_t)28U)
// 未知动作使用 255，避免把预热或低置信度状态错误映射成任一真实动作。
#define BLE_SERVICE_ACTION_UNKNOWN UINT8_C(255)
// 未知电量使用 255，区别于真实的 0% 电量。
#define BLE_SERVICE_BATTERY_UNKNOWN UINT8_C(255)
// 未设置训练目标使用 255，区别于已设置但完成度为 0%。
#define BLE_SERVICE_GOAL_NOT_SET UINT8_C(255)

// v1 定义的九种逻辑消息类型；数值直接写入逻辑帧偏移 4。
typedef enum ble_service_message_type {
    // PC 写入控制点的启停、暂停、配置或快照请求。
    BLE_SERVICE_MESSAGE_CONTROL_REQUEST = 1,
    // ESP32 通过控制点 indication 返回的可靠命令响应。
    BLE_SERVICE_MESSAGE_CONTROL_RESPONSE = 2,
    // ESP32 通过实时状态特征发布的 30 字节权威快照。
    BLE_SERVICE_MESSAGE_LIVE_STATE = 3,
    // ESP32 通过事件特征发布的低延迟非权威提示。
    BLE_SERVICE_MESSAGE_EVENT = 4,
    // PC 写入传输控制特征的会话列表或文件续传请求。
    BLE_SERVICE_MESSAGE_TRANSFER_REQUEST = 5,
    // ESP32 通过传输控制 indication 返回的可靠传输响应。
    BLE_SERVICE_MESSAGE_TRANSFER_RESPONSE = 6,
    // ESP32 通过传输数据 notification 发布的摘要或日志块。
    BLE_SERVICE_MESSAGE_TRANSFER_DATA = 7,
    // ESP32 通过原始流 notification 发布的开发者六轴样本。
    BLE_SERVICE_MESSAGE_RAW_STREAM = 8,
    // ESP32 通过原始流 notification 发布的双 M0 与融合分类诊断。
    BLE_SERVICE_MESSAGE_INFERENCE_DIAGNOSTIC = 9
} ble_service_message_type_t;

// v1 控制命令编号与 docs/BLE通信、设备配置与会话存储.md 第 9.3 节保持逐项一致。
typedef enum ble_service_command_id {
    // 从 Idle 或 Summary 状态开始新训练会话。
    BLE_SERVICE_COMMAND_START_SESSION = 1,
    // 暂停 Running 会话并保留全部累计值。
    BLE_SERVICE_COMMAND_PAUSE_SESSION = 2,
    // 从 Paused 状态恢复同一会话。
    BLE_SERVICE_COMMAND_RESUME_SESSION = 3,
    // 停止 Running 或 Paused 会话并先保存摘要。
    BLE_SERVICE_COMMAND_STOP_SESSION = 4,
    // 仅在 Paused 且设备允许时清零当前会话。
    BLE_SERVICE_COMMAND_RESET_SESSION = 5,
    // 同步 UTC 和时区，但不得改变单调会话时长。
    BLE_SERVICE_COMMAND_SYNC_TIME = 6,
    // 设置体重等用户资料和资料修订号。
    BLE_SERVICE_COMMAND_SET_USER_PROFILE = 7,
    // 设置次数、时长或卡路里目标。
    BLE_SERVICE_COMMAND_SET_GOAL = 8,
// 设置亮度、声音和熄屏时间等偏好；旧马达字段固定为零。
    BLE_SERVICE_COMMAND_SET_PREFERENCES = 9,
    // 请求设备立即返回一份权威状态快照。
    BLE_SERVICE_COMMAND_GET_SNAPSHOT = 10,
    // 仅开发者模式允许开关实时原始六轴流。
    BLE_SERVICE_COMMAND_SET_RAW_STREAM = 11
} ble_service_command_id_t;

// 纯 C 服务函数返回码用于主机测试、GATT ATT 错误映射和设备诊断。
typedef enum ble_service_status {
    // 操作成功，输出缓冲区和长度字段均已写入。
    BLE_SERVICE_STATUS_OK = 0,
    // 必填指针为空，函数没有读取或写入不安全内存。
    BLE_SERVICE_STATUS_NULL_ARGUMENT = 1,
    // 枚举、范围、版本或固定字段不满足 v1 合同。
    BLE_SERVICE_STATUS_INVALID_ARGUMENT = 2,
    // 输出缓冲区容量不足，调用者应扩大容量后重试。
    BLE_SERVICE_STATUS_BUFFER_TOO_SMALL = 3,
    // shared/protocol 返回魔数、长度、CRC 或分片错误。
    BLE_SERVICE_STATUS_PROTOCOL_ERROR = 4,
    // 收到的逻辑消息类型不允许出现在当前 GATT 路径。
    BLE_SERVICE_STATUS_WRONG_MESSAGE_TYPE = 5,
    // 控制 payload 小于固定头、超过业务上限或字段非法。
    BLE_SERVICE_STATUS_BAD_CONTROL_PAYLOAD = 6,
    // 业务处理器没有提供或处理过程失败。
    BLE_SERVICE_STATUS_HANDLER_ERROR = 7,
    // 同一 request_id 携带不同 payload，设备已拒绝且没有执行业务回调。
    BLE_SERVICE_STATUS_REQUEST_CONFLICT = 8,
    // 当前分片已保存，但完整逻辑帧仍未到齐。
    BLE_SERVICE_STATUS_INCOMPLETE = 9
} ble_service_status_t;

// 控制响应 status 字段为稳定的线上值，PC 不依赖 ESP-IDF 内部错误码。
typedef enum ble_service_control_status {
    // 命令成功，设备状态已经按业务语义提交。
    BLE_SERVICE_CONTROL_OK = 0,
    // 当前设备状态或安全策略拒绝命令。
    BLE_SERVICE_CONTROL_REJECTED = 1,
    // command_id 不在 1～11 范围内。
    BLE_SERVICE_CONTROL_INVALID_COMMAND = 2,
    // command_version 不是当前支持的版本 1。
    BLE_SERVICE_CONTROL_INVALID_VERSION = 3,
    // request_id 已用于不同 payload，客户端必须生成新的 request_id。
    BLE_SERVICE_CONTROL_REQUEST_CONFLICT = 4,
    // 设备内部处理器缺失或执行失败。
    BLE_SERVICE_CONTROL_INTERNAL_ERROR = 5
} ble_service_control_status_t;

// v1 设备端通用错误码；业务组件可以在 0x0200 以后扩展自己的精确原因。
typedef enum ble_service_error_code {
    // 没有错误。
    BLE_SERVICE_ERROR_NONE = 0x0000,
    // command_id 超出 v1 的 1～11 范围。
    BLE_SERVICE_ERROR_INVALID_COMMAND = 0x0101,
    // command_version 不是 1。
    BLE_SERVICE_ERROR_INVALID_COMMAND_VERSION = 0x0102,
    // 同一 request_id 与缓存请求字节不一致。
    BLE_SERVICE_ERROR_REQUEST_ID_CONFLICT = 0x0103,
    // 没有注册业务命令处理器。
    BLE_SERVICE_ERROR_HANDLER_MISSING = 0x0104,
    // 业务处理器返回失败，状态没有被服务层假定为已提交。
    BLE_SERVICE_ERROR_HANDLER_FAILED = 0x0105
} ble_service_error_code_t;

// EventV1 事件类型与 docs/BLE通信、设备配置与会话存储.md 第 10 节保持固定数值，未知类型不得猜测解释。
typedef enum ble_service_event_type {
    // 新会话已创建并进入准备或运行状态。
    BLE_SERVICE_EVENT_SESSION_STARTED = 1,
    // 运行会话已暂停，累计值保持不变。
    BLE_SERVICE_EVENT_SESSION_PAUSED = 2,
    // 已暂停会话恢复运行。
    BLE_SERVICE_EVENT_SESSION_RESUMED = 3,
    // 会话已停止且摘要已提交。
    BLE_SERVICE_EVENT_SESSION_STOPPED = 4,
    // 动作锁定结果改变，PC 可立即切换离线矢量动画。
    BLE_SERVICE_EVENT_ACTION_CHANGED = 5,
    // 完成一次重复动作或达到一个步数反馈批次。
    BLE_SERVICE_EVENT_REPETITION_COUNTED = 6,
    // 用户配置的次数、时长或卡路里目标已完成。
    BLE_SERVICE_EVENT_GOAL_REACHED = 7,
    // 原配电池进入 15%、8% 或 5% 产品门槛。
    BLE_SERVICE_EVENT_LOW_BATTERY = 8,
    // QMI8658 读取、时间戳或连续性发生故障。
    BLE_SERVICE_EVENT_SENSOR_FAULT = 9,
    // LittleFS 或 TF 摘要/日志事务失败。
    BLE_SERVICE_EVENT_STORAGE_FAULT = 10,
    // 设备即将进入软件关机或 AXP2101 真关机流程。
    BLE_SERVICE_EVENT_POWER_OFF_PENDING = 11
} ble_service_event_type_t;

// LiveStateV1 使用固定宽度整数，编码后严格为 30 字节小端 payload。
typedef struct ble_service_live_state_v1 {
    // 持久化会话序号，与设备 ID 共同构成历史会话唯一键。
    uint32_t session_sequence;
    // 权威状态修订号，每次业务状态改变后递增。
    uint32_t state_revision;
    // 当前会话单调时长，单位毫秒，不受 UTC 校时影响。
    uint32_t elapsed_ms;
    // 设备状态固定为 0 Booting、1 Idle、2 Preparing、3 Running、4 Paused、5 Summary、6 Error、7 Shutdown。
    uint8_t device_state;
    // 动作索引取值 0～10，或 255 表示未知。
    uint8_t action_id;
    // 指标单位取值 0 无、1 次、2 步、3 秒。
    uint8_t metric_kind;
    // 电量取值 0～100，或 255 表示 PMIC 暂无有效值。
    uint8_t battery_percent;
    // 当前动作的次数、步数或秒数，由 metric_kind 决定含义。
    uint32_t metric_value;
    // Q15 置信度，0～65535 对应 0～1。
    uint16_t confidence_q15;
    // 累计卡路里，单位为千分之一千卡。
    uint32_t calories_mcal;
    // 传感器、预热、插值和存储质量位集合。
    uint16_t quality_flags;
    // USB、充电和低电量状态位集合。
    uint8_t power_flags;
    // 目标完成百分比 0～100，或 255 表示未设置目标。
    uint8_t goal_percent;
} ble_service_live_state_v1_t;

// EventV1 使用固定 36 字节小端布局；事件只承担低延迟提示，累计值仍以 LiveState/摘要为准。
typedef struct ble_service_event_v1 {
    // 事件 payload 自身版本，当前固定为 1，便于未来只升级事件结构。
    uint8_t event_version;
    // 事件类型取值 1～11，对应 ble_service_event_type_t。
    uint8_t event_type;
    // 设备状态固定为 0 Booting、1 Idle、2 Preparing、3 Running、4 Paused、5 Summary、6 Error、7 Shutdown。
    uint8_t device_state;
    // 动作索引 0～10，或 255 表示该事件不关联具体动作。
    uint8_t action_id;
    // 指标单位 0 无、1 次、2 步、3 秒；非指标事件固定为 0。
    uint8_t metric_kind;
    // 电池百分比 0～100，或 255 表示 PMIC 暂无有效值。
    uint8_t battery_percent;
    // 数据质量位集合，与最近一次 LiveState 的质量定义一致。
    uint16_t quality_flags;
    // 持久化会话序号；无会话事件允许为 0。
    uint32_t session_sequence;
    // 会话内事件序号；无会话事件允许为 0。
    uint32_t event_sequence;
    // 事件提交后的权威状态修订号。
    uint32_t state_revision;
    // 本次指标增量；非指标事件固定为 0。
    uint32_t metric_delta;
    // 当前指标权威累计值；PC 只用于动画标签，最终仍以 LiveState 恢复。
    uint32_t metric_total;
    // 当前累计热量，单位为千分之一千卡。
    uint32_t calories_mcal;
    // Q15 稳定度或置信度，0～65535 对应 0～1。
    uint16_t confidence_q15;
    // 事件专用原因码，例如低电量门槛或故障子码；普通事件为 0。
    uint16_t detail_code;
} ble_service_event_v1_t;

// InferenceDiagnosticV1 使用固定 28 字节小端布局，只用于阶段一真板分类诊断，不替代权威训练状态。
typedef struct ble_service_inference_diagnostic_v1 {
    // 诊断 payload 版本，当前固定为 1，允许后续独立扩展字段。
    uint8_t diagnostic_version;
    // 0.85/0.15 融合 logits 的 Top-1 动作索引，取值 0～10 或 255 未知。
    uint8_t fused_action_id;
    // 基础 M0 的 Top-1 动作索引，取值 0～10 或 255 未知。
    uint8_t base_action_id;
    // 掩码 M0 的 Top-1 动作索引，取值 0～10 或 255 未知。
    uint8_t masked_action_id;
    // 融合 logits 经稳定 softmax 得到的 Q15 置信度，0～65535 对应 0～1。
    uint16_t fused_confidence_q15;
    // 基础 M0 经稳定 softmax 得到的 Q15 置信度，0～65535 对应 0～1。
    uint16_t base_confidence_q15;
    // 掩码 M0 经稳定 softmax 得到的 Q15 置信度，0～65535 对应 0～1。
    uint16_t masked_confidence_q15;
    // 当前推理窗口的数据质量位集合，与 IMU pipeline 质量定义一致。
    uint16_t quality_flags;
    // 完成推理的窗口序号，从设备启动后按成功触发顺序单调递增并允许自然回绕。
    uint32_t window_sequence;
    // 当前窗口最后一个重采样点的设备单调时间，单位毫秒。
    uint32_t window_end_ms;
    // 297 维特征提取与双 M0 前向的总耗时，单位微秒。
    uint32_t inference_time_us;
    // pipeline 启动后累计失败窗口数，用于发现传感器或数值异常。
    uint32_t failure_count;
} ble_service_inference_diagnostic_v1_t;

// 控制请求视图只借用已解码帧 payload；回调返回后不得继续保存 tlv 指针。
typedef struct ble_service_control_request {
    // request_id 由 PC 生成，同一连接内用于可靠重试和幂等去重。
    uint32_t request_id;
    // command_id 取值 1～11。
    uint8_t command_id;
    // command_version 当前固定为 1。
    uint8_t command_version;
    // tlv 指向请求固定 6 字节头之后的可选参数；长度为零时允许为空。
    const uint8_t *tlv;
    // tlv_length 表示可选参数字节数，最大为 250。
    uint16_t tlv_length;
} ble_service_control_request_t;

// 业务处理结果由主程序回调填写，服务层负责转换为稳定的控制响应 payload。
typedef struct ble_service_command_result {
    // status 使用 ble_service_control_status_t 的线上数值。
    uint8_t status;
    // error_code 保存精确业务失败原因，成功时应为 0。
    uint16_t error_code;
    // state_revision 是命令处理完成后的权威修订号。
    uint32_t state_revision;
    // tlv 指向可选响应参数；编码完成前必须保持有效，长度为零时允许为空。
    const uint8_t *tlv;
    // tlv_length 最大为 244，使响应固定头与 TLV 总计不超过 256 字节。
    uint16_t tlv_length;
} ble_service_command_result_t;

// 命令处理回调是唯一允许改变会话、配置或流开关业务状态的位置。
typedef ble_service_status_t (*ble_service_command_handler_fn)(
    const ble_service_control_request_t *request,
    ble_service_command_result_t *result,
    void *context);

// 单条缓存保存请求原始 payload 和对应响应 payload，以字节级比较避免哈希碰撞误判。
typedef struct ble_service_request_cache_entry {
    // valid 为 1 表示本槽含有效请求与响应，为 0 表示空槽。
    uint8_t valid;
    // request_id 加速查找；最终冲突判定仍比较全部请求字节。
    uint32_t request_id;
    // request_length 表示 request_payload 中的有效字节数。
    uint16_t request_length;
    // request_payload 保存包含 request_id 的完整控制请求 payload，最大 256 字节。
    uint8_t request_payload[BLE_SERVICE_MAX_CONTROL_REQUEST_PAYLOAD];
    // response_length 表示 response_payload 中的有效字节数。
    uint16_t response_length;
    // response_payload 保存对应控制响应 payload，重复请求直接复用该字节序列。
    uint8_t response_payload[BLE_SERVICE_MAX_CONTROL_RESPONSE_PAYLOAD];
} ble_service_request_cache_entry_t;

// 每连接状态拥有两个独立重组器、16 项幂等缓存和控制响应序号。
typedef struct ble_service_connection {
    // control_reassembler 只接收特征 0001 的 ControlRequest 分片。
    imu_ble_reassembler_t control_reassembler;
    // transfer_reassembler 只接收特征 0005 的 TransferRequest 分片。
    imu_ble_reassembler_t transfer_reassembler;
    // request_cache 按环形槽保存最近 16 个已处理控制请求。
    ble_service_request_cache_entry_t request_cache[BLE_SERVICE_REQUEST_CACHE_SIZE];
    // next_cache_slot 指向下一个新请求要覆盖的最旧槽位。
    uint8_t next_cache_slot;
    // next_control_sequence 为控制响应逻辑帧分配 16 位序号并允许自然回绕。
    uint16_t next_control_sequence;
    // latest_state_revision 保存最近一次业务回调结果，冲突响应不得虚构新修订号。
    uint32_t latest_state_revision;
} ble_service_connection_t;

// 清空连接重组器、幂等缓存和发送序号；建立连接和断连时都必须调用。
void ble_service_connection_reset(ble_service_connection_t *connection);

// 验证消息类型是否属于 v1 的 1～9 范围。
ble_service_status_t ble_service_validate_message_type(uint8_t message_type);

// 把 LiveStateV1 编码为固定 30 字节小端 payload，并校验动作、电量和目标范围。
ble_service_status_t ble_service_encode_live_state_v1(
    const ble_service_live_state_v1_t *state,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

// 把 EventV1 编码为固定 36 字节小端 payload，并校验版本、事件、动作、电量和单位范围。
ble_service_status_t ble_service_encode_event_v1(
    const ble_service_event_v1_t *event,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

// 把 InferenceDiagnosticV1 编码为固定 28 字节小端 payload，并校验版本和三组动作索引。
ble_service_status_t ble_service_encode_inference_diagnostic_v1(
    const ble_service_inference_diagnostic_v1_t *diagnostic,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

// 构造任一 v1 逻辑消息，实际帧格式和 CRC 由 shared/protocol 统一编码。
ble_service_status_t ble_service_encode_message(
    // v1 消息类型，合法范围 1～9；超界返回 BLE_SERVICE_STATUS_PROTOCOL_ERROR。
    uint8_t message_type,
    // 协议标志位；未知位按 shared/protocol 合同保留或拒绝。
    uint8_t flags,
    // 当前连接内的 16 位逻辑帧序号，达到 65535 后允许自然回绕。
    uint16_t sequence,
    // 设备单调时间，单位毫秒；只用于排序和诊断，不代表 Unix 时间。
    uint32_t monotonic_ms,
    // 只读 payload 首地址；长度为零时允许为空，否则生命周期必须覆盖本次编码调用。
    const uint8_t *payload,
    // payload 有效字节数，单位字节；不得超过协议单帧上限。
    uint16_t payload_length,
    // 非空可写输出缓冲区；形状为 [output_capacity] 字节，生命周期由调用者管理。
    uint8_t *output,
    // output 可写容量，单位字节；不足时返回缓冲区错误且不写越界。
    size_t output_capacity,
    // 非空输出指针；成功时写入完整逻辑帧长度，单位字节，生命周期仅本次调用借用。
    size_t *output_length);

// 处理一帧完整 ControlRequest，执行业务回调或返回缓存响应，并编码 ControlResponse 逻辑帧。
// handler_context 是可为空的业务上下文；本函数仅在同步 handler 调用期间借用，生命周期和所有权归调用者。
ble_service_status_t ble_service_process_control_frame(
    // 非空连接状态；函数在调用期间可更新重组器、幂等缓存、序号和 revision。
    ble_service_connection_t *connection,
    // 非空只读完整逻辑帧；形状为 [request_frame_length] 字节，生命周期覆盖本次调用。
    const uint8_t *request_frame,
    // 请求帧有效长度，单位字节；必须同时覆盖头、payload 和 CRC。
    size_t request_frame_length,
    // 响应采用的设备单调时间，单位毫秒。
    uint32_t response_monotonic_ms,
    // 非空同步业务处理函数；不得保存本函数提供的临时 payload 指针。
    ble_service_command_handler_fn handler,
    // 可为空业务上下文；只在同步回调期间借用，函数不会释放或保存其地址。
    void *handler_context,
    // 非空可写响应缓冲区；形状为 [response_capacity] 字节，生命周期由调用者管理。
    uint8_t *response_frame,
    // 响应缓冲区容量，单位字节；不足时返回缓冲区错误。
    size_t response_capacity,
    // 非空输出指针；成功时写入响应逻辑帧长度，单位字节。
    size_t *response_length,
    // 非空输出指针；写 1 表示命中幂等缓存，写 0 表示本次实际执行命令。
    uint8_t *from_cache);

// 接收一片 Control Point GATT Value；完整后调用控制处理器并输出待 indication 的逻辑响应帧。
// handler_context 是可为空的业务上下文；本函数仅在完整帧形成后的同步回调期间借用。
ble_service_status_t ble_service_process_control_fragment(
    // 非空连接状态；重组进度和幂等缓存的生命周期覆盖整条 BLE 连接。
    ble_service_connection_t *connection,
    // 非空只读 ATT Value；形状为 [fragment_length] 字节，只在本次调用期间有效。
    const uint8_t *fragment,
    // 当前分片有效长度，单位字节；必须符合协商 MTU 和分片包络。
    size_t fragment_length,
    // 完整请求形成后响应采用的设备单调时间，单位毫秒。
    uint32_t response_monotonic_ms,
    // 非空同步业务处理函数；分片未完成时不会调用。
    ble_service_command_handler_fn handler,
    // 可为空业务上下文；仅在同步回调期间借用，函数不取得所有权。
    void *handler_context,
    // 非空可写响应缓冲区；形状为 [response_capacity] 字节，由调用者持有。
    uint8_t *response_frame,
    // 响应缓冲区容量，单位字节；分片未完成时不会写业务响应。
    size_t response_capacity,
    // 非空输出指针；成功形成响应时写入逻辑帧长度，单位字节。
    size_t *response_length,
    // 非空输出指针；写 1 表示完整请求命中缓存，写 0 表示新执行或尚未完成。
    uint8_t *from_cache);

#ifdef __cplusplus
// 结束 C 链接约定。
}
#endif

#endif
