// 引入设备端 BLE 纯 C 核心声明，所有业务和主机测试都通过同一接口进入。
#include "ble_service_core.h"

// 引入 memcpy、memcmp 和 memset，用于受固定容量保护的字节复制、比较和状态清零。
#include <string.h>

// 控制请求固定头长度为 request_id 4 字节、command_id 1 字节和 command_version 1 字节。
#define BLE_SERVICE_CONTROL_REQUEST_HEADER_SIZE ((size_t)6U)
// 控制响应固定头长度为 request_id 4、command_id 1、status 1、error_code 2 和 revision 4 字节。
#define BLE_SERVICE_CONTROL_RESPONSE_HEADER_SIZE ((size_t)12U)
// 当前每条控制命令只支持版本 1；命令自身未来可独立升级而不改变逻辑帧主版本。
#define BLE_SERVICE_COMMAND_VERSION_V1 UINT8_C(1)

// 从至少 4 字节的小端区域读取 32 位整数；调用者必须先完成长度检查。
static uint32_t ble_service_read_u32_le(const uint8_t *data)
{
    // 读取数值位 0～7。
    const uint32_t byte0 = (uint32_t)data[0];
    // 读取数值位 8～15。
    const uint32_t byte1 = (uint32_t)((uint32_t)data[1] << 8U);
    // 读取数值位 16～23。
    const uint32_t byte2 = (uint32_t)((uint32_t)data[2] << 16U);
    // 读取数值位 24～31。
    const uint32_t byte3 = (uint32_t)((uint32_t)data[3] << 24U);
    // 合并四个字节并返回固定宽度数值。
    return byte0 | byte1 | byte2 | byte3;
}

// 把 16 位整数按小端序写入至少 2 字节的输出区域。
static void ble_service_write_u16_le(uint8_t *data, uint16_t value)
{
    // 写入数值位 0～7。
    data[0] = (uint8_t)(value & UINT16_C(0x00FF));
    // 写入数值位 8～15。
    data[1] = (uint8_t)((value >> 8U) & UINT16_C(0x00FF));
}

// 把 32 位整数按小端序写入至少 4 字节的输出区域。
static void ble_service_write_u32_le(uint8_t *data, uint32_t value)
{
    // 写入数值位 0～7。
    data[0] = (uint8_t)(value & UINT32_C(0x000000FF));
    // 写入数值位 8～15。
    data[1] = (uint8_t)((value >> 8U) & UINT32_C(0x000000FF));
    // 写入数值位 16～23。
    data[2] = (uint8_t)((value >> 16U) & UINT32_C(0x000000FF));
    // 写入数值位 24～31。
    data[3] = (uint8_t)((value >> 24U) & UINT32_C(0x000000FF));
}

// 把 shared/protocol 状态转换为 BLE 服务层状态，保留业务层稳定错误域。
static ble_service_status_t ble_service_map_protocol_status(imu_ble_status_t status)
{
    // shared/protocol 成功时直接返回服务成功。
    if (status == IMU_BLE_STATUS_OK) {
        // 调用者可以继续消费完整帧或分片结果。
        return BLE_SERVICE_STATUS_OK;
    }
    // 空指针属于调用者错误，而不是线上坏包。
    if (status == IMU_BLE_STATUS_NULL_ARGUMENT) {
        // 返回统一空参数状态。
        return BLE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    // 输出容量不足需要调用者扩大本地缓冲区。
    if (status == IMU_BLE_STATUS_BUFFER_TOO_SMALL) {
        // 返回容量不足，不把它误报为远端 CRC 错误。
        return BLE_SERVICE_STATUS_BUFFER_TOO_SMALL;
    }
    // 其它状态均来自魔数、长度、CRC 或分片顺序异常。
    return BLE_SERVICE_STATUS_PROTOCOL_ERROR;
}

// 把控制响应字段编码为不超过 256 字节的小端 payload。
static ble_service_status_t ble_service_encode_control_response_payload(
    uint32_t request_id,
    uint8_t command_id,
    const ble_service_command_result_t *result,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    // 结果、输出和长度指针均为必填对象。
    if ((result == NULL) || (output == NULL) || (output_length == NULL)) {
        // 缺失对象时不尝试写入部分响应。
        return BLE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    // 响应 TLV 长度必须使总 payload 不超过 256 字节。
    if ((size_t)result->tlv_length > (BLE_SERVICE_MAX_CONTROL_RESPONSE_PAYLOAD - BLE_SERVICE_CONTROL_RESPONSE_HEADER_SIZE)) {
        // 过大业务响应应改走 Transfer Data，而不能塞入控制点。
        return BLE_SERVICE_STATUS_BAD_CONTROL_PAYLOAD;
    }
    // 非零 TLV 长度必须提供有效指针。
    if ((result->tlv_length > 0U) && (result->tlv == NULL)) {
        // 拒绝长度和指针不一致的业务结果。
        return BLE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    // 控制状态只允许 0～5，防止业务枚举未初始化或未来值被旧 PC 误解。
    if (result->status > (uint8_t)BLE_SERVICE_CONTROL_INTERNAL_ERROR) {
        // 返回参数错误，调用者应修复业务处理器。
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    // 计算固定头与可选 TLV 的总 payload 长度。
    const size_t required_length = BLE_SERVICE_CONTROL_RESPONSE_HEADER_SIZE + (size_t)result->tlv_length;
    // 输出容量不足时返回所需长度，禁止写入截断响应。
    if (output_capacity < required_length) {
        // 把所需字节数返回给调用者。
        *output_length = required_length;
        // 指示调用者扩大缓冲区。
        return BLE_SERVICE_STATUS_BUFFER_TOO_SMALL;
    }
    // 偏移 0 写入原请求 ID，PC 用它匹配 indication。
    ble_service_write_u32_le(&output[0], request_id);
    // 偏移 4 写入原命令 ID。
    output[4] = command_id;
    // 偏移 5 写入稳定控制状态。
    output[5] = result->status;
    // 偏移 6 写入小端精确错误码。
    ble_service_write_u16_le(&output[6], result->error_code);
    // 偏移 8 写入命令处理后的权威状态修订号。
    ble_service_write_u32_le(&output[8], result->state_revision);
    // 可选 TLV 非空时复制到固定头之后。
    if (result->tlv_length > 0U) {
        // 长度已经由 244 字节上限和输出容量保护。
        (void)memcpy(&output[BLE_SERVICE_CONTROL_RESPONSE_HEADER_SIZE], result->tlv, (size_t)result->tlv_length);
    }
    // 返回实际响应 payload 长度。
    *output_length = required_length;
    // 所有固定字段和 TLV 均已成功编码。
    return BLE_SERVICE_STATUS_OK;
}

// 在 16 项缓存中查找 request_id；返回空表示从未处理该 ID。
static ble_service_request_cache_entry_t *ble_service_find_cache_entry(
    ble_service_connection_t *connection,
    uint32_t request_id)
{
    // 遍历固定 16 个槽，时间复杂度为 O(16)，无需动态容器。
    for (size_t index = 0U; index < BLE_SERVICE_REQUEST_CACHE_SIZE; ++index) {
        // current 指向连接对象内部槽位，生命周期与连接一致。
        ble_service_request_cache_entry_t *const current = &connection->request_cache[index];
        // 只比较有效槽，避免清零后的 request_id=0 被误认为真实命中。
        if ((current->valid != 0U) && (current->request_id == request_id)) {
            // 返回精确命中槽，调用者继续做全 payload 字节比较。
            return current;
        }
    }
    // 最近 16 个请求中不存在该 ID。
    return NULL;
}

// 把新请求和响应写入下一个环形槽；第 17 个请求覆盖最旧的第 1 个请求。
static void ble_service_store_cache_entry(
    ble_service_connection_t *connection,
    uint32_t request_id,
    const uint8_t *request_payload,
    uint16_t request_length,
    const uint8_t *response_payload,
    uint16_t response_length)
{
    // slot 指向当前最旧或空闲槽位，写完后环形下标前移。
    ble_service_request_cache_entry_t *const slot = &connection->request_cache[connection->next_cache_slot];
    // 先清零整个槽，防止较短新请求留下旧尾部字节影响调试转储。
    (void)memset(slot, 0, sizeof(*slot));
    // 保存 request_id 供后续 O(16) 查找。
    slot->request_id = request_id;
    // 保存完整请求 payload 有效长度。
    slot->request_length = request_length;
    // 复制请求原始字节，使冲突判定不依赖可碰撞的哈希值。
    (void)memcpy(slot->request_payload, request_payload, (size_t)request_length);
    // 保存对应响应 payload 有效长度。
    slot->response_length = response_length;
    // 复制响应原始字节，重复请求不得再次执行回调或重新计算状态。
    (void)memcpy(slot->response_payload, response_payload, (size_t)response_length);
    // 最后设置有效位，使调试器不会观察到半写槽位。
    slot->valid = UINT8_C(1);
    // 环形下标前移并在 16 处回到 0。
    connection->next_cache_slot = (uint8_t)(((size_t)connection->next_cache_slot + 1U) % BLE_SERVICE_REQUEST_CACHE_SIZE);
}

// 使用给定响应 payload 构造 ControlResponse 完整逻辑帧并分配连接内序号。
static ble_service_status_t ble_service_wrap_control_response(
    ble_service_connection_t *connection,
    const uint8_t *response_payload,
    uint16_t response_payload_length,
    uint32_t response_monotonic_ms,
    uint8_t *response_frame,
    size_t response_capacity,
    size_t *response_length)
{
    // 调用统一逻辑帧编码器，消息类型固定为 2，标志位保留为 0。
    const ble_service_status_t status = ble_service_encode_message(
        (uint8_t)BLE_SERVICE_MESSAGE_CONTROL_RESPONSE,
        UINT8_C(0),
        connection->next_control_sequence,
        response_monotonic_ms,
        response_payload,
        response_payload_length,
        response_frame,
        response_capacity,
        response_length);
    // 只有完整帧成功编码后才消耗序号，容量不足重试仍使用原序号。
    if (status == BLE_SERVICE_STATUS_OK) {
        // uint16 自然回绕符合协议 sequence 模 65536 规则。
        connection->next_control_sequence = (uint16_t)(connection->next_control_sequence + UINT16_C(1));
    }
    // 返回完整帧编码结果。
    return status;
}

// 清空连接状态；断连后旧分片和旧 request_id 不得影响下一位 PC。
void ble_service_connection_reset(ble_service_connection_t *connection)
{
    // 空连接指针没有可清理对象，函数安全返回。
    if (connection == NULL) {
        // 避免对空指针执行 memset。
        return;
    }
    // 清零全部缓存、序号和最近修订号。
    (void)memset(connection, 0, sizeof(*connection));
    // 显式调用共享重组器复位，记录断连边界的协议意图。
    imu_ble_reassembler_reset(&connection->control_reassembler);
    // 传输控制使用独立重组器，不能与控制点跨特征拼接。
    imu_ble_reassembler_reset(&connection->transfer_reassembler);
}

// 验证消息类型属于 v1 连续范围 1～8。
ble_service_status_t ble_service_validate_message_type(uint8_t message_type)
{
    // 小于 ControlRequest 或大于 RawStream 都是未知消息。
    if ((message_type < (uint8_t)BLE_SERVICE_MESSAGE_CONTROL_REQUEST) ||
        (message_type > (uint8_t)BLE_SERVICE_MESSAGE_RAW_STREAM)) {
        // 调用者必须记录并忽略未知次版本消息。
        return BLE_SERVICE_STATUS_WRONG_MESSAGE_TYPE;
    }
    // 1～8 均为当前协议定义消息。
    return BLE_SERVICE_STATUS_OK;
}

// 编码 LiveStateV1 固定 30 字节 payload。
ble_service_status_t ble_service_encode_live_state_v1(
    const ble_service_live_state_v1_t *state,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    // 三个对象均为必填，避免返回未初始化长度。
    if ((state == NULL) || (output == NULL) || (output_length == NULL)) {
        // 空参数不产生任何部分 payload。
        return BLE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    // 固定 30 字节输出容量不足时返回所需长度。
    if (output_capacity < BLE_SERVICE_LIVE_STATE_V1_SIZE) {
        // 告知调用者精确固定长度。
        *output_length = BLE_SERVICE_LIVE_STATE_V1_SIZE;
        // 返回容量不足。
        return BLE_SERVICE_STATUS_BUFFER_TOO_SMALL;
    }
    // v1 设备状态仅允许 0～7。
    if (state->device_state > UINT8_C(7)) {
        // 未知状态可能属于更高协议版本，当前编码器拒绝误发。
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    // 动作只允许模型类别 0～10 或 255 未知。
    if ((state->action_id > UINT8_C(10)) && (state->action_id != BLE_SERVICE_ACTION_UNKNOWN)) {
        // 类别表错位会播放错误动画，因此必须拒绝。
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    // 指标单位只允许无、次、步、秒四种值 0～3。
    if (state->metric_kind > UINT8_C(3)) {
        // 未知单位不能安全显示。
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    // 电量只允许 0～100 或 255 未知。
    if ((state->battery_percent > UINT8_C(100)) && (state->battery_percent != BLE_SERVICE_BATTERY_UNKNOWN)) {
        // 非法百分比不得进入 PC 进度条。
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    // 目标完成度只允许 0～100 或 255 未设置。
    if ((state->goal_percent > UINT8_C(100)) && (state->goal_percent != BLE_SERVICE_GOAL_NOT_SET)) {
        // 非法目标值说明上游状态损坏。
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    // 偏移 0 写入会话序号。
    ble_service_write_u32_le(&output[0], state->session_sequence);
    // 偏移 4 写入状态修订号。
    ble_service_write_u32_le(&output[4], state->state_revision);
    // 偏移 8 写入单调会话时长，单位毫秒。
    ble_service_write_u32_le(&output[8], state->elapsed_ms);
    // 偏移 12 写入设备状态。
    output[12] = state->device_state;
    // 偏移 13 写入动作索引。
    output[13] = state->action_id;
    // 偏移 14 写入指标单位。
    output[14] = state->metric_kind;
    // 偏移 15 写入电量百分比或未知值。
    output[15] = state->battery_percent;
    // 偏移 16 写入次数、步数或秒数。
    ble_service_write_u32_le(&output[16], state->metric_value);
    // 偏移 20 写入 Q15 置信度。
    ble_service_write_u16_le(&output[20], state->confidence_q15);
    // 偏移 22 写入千分之一千卡。
    ble_service_write_u32_le(&output[22], state->calories_mcal);
    // 偏移 26 写入质量标志。
    ble_service_write_u16_le(&output[26], state->quality_flags);
    // 偏移 28 写入电源标志。
    output[28] = state->power_flags;
    // 偏移 29 写入目标完成度。
    output[29] = state->goal_percent;
    // 固定 payload 始终写入 30 字节。
    *output_length = BLE_SERVICE_LIVE_STATE_V1_SIZE;
    // 编码成功。
    return BLE_SERVICE_STATUS_OK;
}

// 编码 EventV1 固定 36 字节 payload；所有整数按小端序输出。
ble_service_status_t ble_service_encode_event_v1(
    const ble_service_event_v1_t *event,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    // 事件对象、输出缓冲和长度指针均为必填，避免返回未初始化数据。
    if ((event == NULL) || (output == NULL) || (output_length == NULL)) {
        // 空参数不写入任何部分 payload。
        return BLE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    // 固定 36 字节容量不足时返回精确需求，不产生截断事件。
    if (output_capacity < BLE_SERVICE_EVENT_V1_SIZE) {
        // 告知调用者需要的完整字节数。
        *output_length = BLE_SERVICE_EVENT_V1_SIZE;
        // 返回容量不足供调用者扩大静态缓冲区。
        return BLE_SERVICE_STATUS_BUFFER_TOO_SMALL;
    }
    // 当前只支持事件 payload 版本 1。
    if (event->event_version != UINT8_C(1)) {
        // 未知布局不能按 v1 偏移编码。
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    // 事件类型只允许当前定义的 1～11。
    if ((event->event_type < (uint8_t)BLE_SERVICE_EVENT_SESSION_STARTED) ||
        (event->event_type > (uint8_t)BLE_SERVICE_EVENT_POWER_OFF_PENDING)) {
        // 未知事件不能让 PC 猜测业务含义。
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    // v1 设备状态只允许 Booting～Error 的 0～7。
    if (event->device_state > UINT8_C(7)) {
        // 非法状态不能进入 PC 状态提示。
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    // 动作只允许模型类别 0～10 或 255 未知。
    if ((event->action_id > UINT8_C(10)) && (event->action_id != BLE_SERVICE_ACTION_UNKNOWN)) {
        // 类别错位会播放错误动作动画。
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    // 指标单位只允许无、次、步、秒四种值 0～3。
    if (event->metric_kind > UINT8_C(3)) {
        // 未知单位不能安全渲染。
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    // 电量只允许 0～100 或 255 未知。
    if ((event->battery_percent > UINT8_C(100)) && (event->battery_percent != BLE_SERVICE_BATTERY_UNKNOWN)) {
        // 非法电量不能进入低电量动画。
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    // 偏移 0 写入事件结构版本。
    output[0] = event->event_version;
    // 偏移 1 写入事件类型。
    output[1] = event->event_type;
    // 偏移 2 写入事件后的设备状态。
    output[2] = event->device_state;
    // 偏移 3 写入动作索引或 255 未知。
    output[3] = event->action_id;
    // 偏移 4 写入指标单位。
    output[4] = event->metric_kind;
    // 偏移 5 写入电量百分比或未知值。
    output[5] = event->battery_percent;
    // 偏移 6 写入小端质量位集合。
    ble_service_write_u16_le(&output[6], event->quality_flags);
    // 偏移 8 写入会话序号。
    ble_service_write_u32_le(&output[8], event->session_sequence);
    // 偏移 12 写入会话内事件序号。
    ble_service_write_u32_le(&output[12], event->event_sequence);
    // 偏移 16 写入权威状态修订号。
    ble_service_write_u32_le(&output[16], event->state_revision);
    // 偏移 20 写入本次指标增量。
    ble_service_write_u32_le(&output[20], event->metric_delta);
    // 偏移 24 写入当前指标累计值。
    ble_service_write_u32_le(&output[24], event->metric_total);
    // 偏移 28 写入千分之一千卡累计值。
    ble_service_write_u32_le(&output[28], event->calories_mcal);
    // 偏移 32 写入 Q15 稳定度或置信度。
    ble_service_write_u16_le(&output[32], event->confidence_q15);
    // 偏移 34 写入事件专用细节码。
    ble_service_write_u16_le(&output[34], event->detail_code);
    // 固定 payload 始终写入 36 字节。
    *output_length = BLE_SERVICE_EVENT_V1_SIZE;
    // 全部字段编码完成。
    return BLE_SERVICE_STATUS_OK;
}

// 构造任一 v1 完整逻辑消息。
ble_service_status_t ble_service_encode_message(
    uint8_t message_type,
    uint8_t flags,
    uint16_t sequence,
    uint32_t monotonic_ms,
    const uint8_t *payload,
    uint16_t payload_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    // 先验证消息类型，未知值不得进入线上帧。
    const ble_service_status_t type_status = ble_service_validate_message_type(message_type);
    // 消息类型错误时直接返回。
    if (type_status != BLE_SERVICE_STATUS_OK) {
        // 保留精确类型错误供诊断。
        return type_status;
    }
    // 初始化共享帧视图，payload 只借用调用者内存到编码函数返回。
    imu_ble_frame_view_t frame;
    // 写入协议主版本 1。
    frame.protocol_major = IMU_BLE_PROTOCOL_MAJOR;
    // 写入协议次版本 0。
    frame.protocol_minor = IMU_BLE_PROTOCOL_MINOR;
    // 写入上层消息类型 1～8。
    frame.message_type = message_type;
    // 写入上层标志位。
    frame.flags = flags;
    // 写入逻辑帧序号。
    frame.sequence = sequence;
    // 写入设备单调毫秒时间。
    frame.monotonic_ms = monotonic_ms;
    // 引用调用者 payload，非零长度时共享编码器会校验非空指针。
    frame.payload = payload;
    // 写入 payload 长度，最大值由 shared/protocol 校验为 1024。
    frame.payload_length = payload_length;
    // 复用统一帧编码和 CRC 实现。
    const imu_ble_status_t protocol_status = imu_ble_encode_frame(&frame, output, output_capacity, output_length);
    // 映射为服务层稳定状态。
    return ble_service_map_protocol_status(protocol_status);
}

// 处理完整 ControlRequest 逻辑帧并输出 ControlResponse。
ble_service_status_t ble_service_process_control_frame(
    ble_service_connection_t *connection,
    const uint8_t *request_frame,
    size_t request_frame_length,
    uint32_t response_monotonic_ms,
    ble_service_command_handler_fn handler,
    void *handler_context,
    uint8_t *response_frame,
    size_t response_capacity,
    size_t *response_length,
    uint8_t *from_cache)
{
    // 所有输入、输出对象均为必填；业务上下文允许为空并由回调自行解释。
    if ((connection == NULL) || (request_frame == NULL) || (response_frame == NULL) ||
        (response_length == NULL) || (from_cache == NULL)) {
        // 空参数时不尝试解析或执行业务。
        return BLE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    // 默认没有可发送响应，错误分支不会泄漏上次调用长度。
    *response_length = 0U;
    // 默认表示本次不是缓存命中。
    *from_cache = UINT8_C(0);
    // frame_view 只在共享解码成功后包含有效 payload 视图。
    imu_ble_frame_view_t frame_view;
    // 先验证魔数、长度和 CRC，坏帧绝不进入业务回调。
    const imu_ble_status_t decode_status = imu_ble_decode_frame(request_frame, request_frame_length, &frame_view);
    // 逻辑帧校验失败时返回协议错误。
    if (decode_status != IMU_BLE_STATUS_OK) {
        // 映射并保留坏 CRC、坏长度等统一协议域。
        return ble_service_map_protocol_status(decode_status);
    }
    // 主版本不同表示二进制合同不兼容，禁止控制业务。
    if (frame_view.protocol_major != IMU_BLE_PROTOCOL_MAJOR) {
        // 返回参数错误，NimBLE 层可记录版本不兼容诊断。
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    // 控制点只接受消息类型 1，Event 或 TransferRequest 不得串入。
    if (frame_view.message_type != (uint8_t)BLE_SERVICE_MESSAGE_CONTROL_REQUEST) {
        // 不产生响应，避免把未知消息误解释为控制请求。
        return BLE_SERVICE_STATUS_WRONG_MESSAGE_TYPE;
    }
    // 控制 payload 至少包含 6 字节固定头，且业务上限为 256 字节。
    if ((frame_view.payload_length < BLE_SERVICE_CONTROL_REQUEST_HEADER_SIZE) ||
        ((size_t)frame_view.payload_length > BLE_SERVICE_MAX_CONTROL_REQUEST_PAYLOAD)) {
        // 不足 6 字节时无法可靠读取 request_id 并构造关联响应。
        return BLE_SERVICE_STATUS_BAD_CONTROL_PAYLOAD;
    }
    // payload 非空且已经由共享解码器指向输入帧内部。
    const uint8_t *const request_payload = frame_view.payload;
    // 从偏移 0 读取 PC 请求 ID。
    const uint32_t request_id = ble_service_read_u32_le(&request_payload[0]);
    // 从偏移 4 读取命令 ID。
    const uint8_t command_id = request_payload[4];
    // 从偏移 5 读取命令版本。
    const uint8_t command_version = request_payload[5];
    // 查找最近 16 个相同 request_id。
    ble_service_request_cache_entry_t *const cached = ble_service_find_cache_entry(connection, request_id);
    // 命中缓存时必须继续比较完整请求字节，防止同 ID 不同命令被误当重试。
    if (cached != NULL) {
        // 长度相同且每个字节相同才属于合法重试。
        const uint8_t request_matches = (uint8_t)(
            ((size_t)cached->request_length == (size_t)frame_view.payload_length) &&
            (memcmp(cached->request_payload, request_payload, (size_t)frame_view.payload_length) == 0));
        // 完全相同请求直接包装缓存响应，不执行业务回调。
        if (request_matches != 0U) {
            // 标记缓存命中，供诊断统计 PC 重试次数。
            *from_cache = UINT8_C(1);
            // 使用新逻辑帧 sequence 包装同一响应 payload。
            return ble_service_wrap_control_response(
                connection,
                cached->response_payload,
                cached->response_length,
                response_monotonic_ms,
                response_frame,
                response_capacity,
                response_length);
        }
        // 同 request_id 不同 payload 构造明确冲突响应，且绝不覆盖原缓存。
        ble_service_command_result_t conflict_result;
        // 线上状态写入 request conflict。
        conflict_result.status = (uint8_t)BLE_SERVICE_CONTROL_REQUEST_CONFLICT;
        // 精确错误码要求 PC 改用新 request_id。
        conflict_result.error_code = (uint16_t)BLE_SERVICE_ERROR_REQUEST_ID_CONFLICT;
        // 冲突没有改变业务状态，沿用最近修订号。
        conflict_result.state_revision = connection->latest_state_revision;
        // 冲突响应没有可选 TLV。
        conflict_result.tlv = NULL;
        // TLV 长度为零。
        conflict_result.tlv_length = UINT16_C(0);
        // 在栈上保存最多 256 字节冲突响应 payload。
        uint8_t conflict_payload[BLE_SERVICE_MAX_CONTROL_RESPONSE_PAYLOAD];
        // 保存实际冲突 payload 长度。
        size_t conflict_payload_length = 0U;
        // 编码固定响应字段。
        const ble_service_status_t conflict_payload_status = ble_service_encode_control_response_payload(
            request_id,
            command_id,
            &conflict_result,
            conflict_payload,
            sizeof(conflict_payload),
            &conflict_payload_length);
        // 理论固定缓冲区足够；仍保留失败保护。
        if (conflict_payload_status != BLE_SERVICE_STATUS_OK) {
            // 返回具体本地编码错误。
            return conflict_payload_status;
        }
        // 把冲突 payload 包装为完整逻辑响应帧。
        const ble_service_status_t wrap_status = ble_service_wrap_control_response(
            connection,
            conflict_payload,
            (uint16_t)conflict_payload_length,
            response_monotonic_ms,
            response_frame,
            response_capacity,
            response_length);
        // 容量等本地错误优先返回，避免声称已有可发送冲突帧。
        if (wrap_status != BLE_SERVICE_STATUS_OK) {
            // 返回实际包装错误。
            return wrap_status;
        }
        // 响应已经生成，同时用专用状态提示调用者记录协议冲突。
        return BLE_SERVICE_STATUS_REQUEST_CONFLICT;
    }
    // 初始化业务请求视图，TLV 只借用当前完整帧缓冲区。
    ble_service_control_request_t request;
    // 写入请求 ID。
    request.request_id = request_id;
    // 写入命令 ID。
    request.command_id = command_id;
    // 写入命令版本。
    request.command_version = command_version;
    // 有 TLV 时指向固定头之后，否则使用空指针。
    request.tlv = (frame_view.payload_length == BLE_SERVICE_CONTROL_REQUEST_HEADER_SIZE)
        ? NULL
        : &request_payload[BLE_SERVICE_CONTROL_REQUEST_HEADER_SIZE];
    // 计算 TLV 长度，范围 0～250。
    request.tlv_length = (uint16_t)(frame_view.payload_length - (uint16_t)BLE_SERVICE_CONTROL_REQUEST_HEADER_SIZE);
    // 初始化响应结果为内部错误，所有分支必须显式覆盖成功语义。
    ble_service_command_result_t result;
    // 默认状态为内部错误。
    result.status = (uint8_t)BLE_SERVICE_CONTROL_INTERNAL_ERROR;
    // 默认错误码为业务处理失败。
    result.error_code = (uint16_t)BLE_SERVICE_ERROR_HANDLER_FAILED;
    // 默认修订号保持连接最近值。
    result.state_revision = connection->latest_state_revision;
    // 默认没有响应 TLV。
    result.tlv = NULL;
    // 默认 TLV 长度为零。
    result.tlv_length = UINT16_C(0);
    // handler_status 记录是否需要向诊断层返回业务处理失败。
    ble_service_status_t handler_status = BLE_SERVICE_STATUS_OK;
    // 命令 ID 超出 1～11 时由服务层直接生成错误，不调用业务处理器。
    if ((command_id < (uint8_t)BLE_SERVICE_COMMAND_START_SESSION) ||
        (command_id > (uint8_t)BLE_SERVICE_COMMAND_SET_RAW_STREAM)) {
        // 设置线上无效命令状态。
        result.status = (uint8_t)BLE_SERVICE_CONTROL_INVALID_COMMAND;
        // 写入稳定错误码。
        result.error_code = (uint16_t)BLE_SERVICE_ERROR_INVALID_COMMAND;
    } else if (command_version != BLE_SERVICE_COMMAND_VERSION_V1) {
        // 命令版本不是 1 时拒绝，但逻辑帧主版本仍可兼容。
        result.status = (uint8_t)BLE_SERVICE_CONTROL_INVALID_VERSION;
        // 写入稳定命令版本错误码。
        result.error_code = (uint16_t)BLE_SERVICE_ERROR_INVALID_COMMAND_VERSION;
    } else if (handler == NULL) {
        // 未注册业务处理器时返回内部错误，不假定任何状态变化。
        result.status = (uint8_t)BLE_SERVICE_CONTROL_INTERNAL_ERROR;
        // 告知集成层处理器缺失。
        result.error_code = (uint16_t)BLE_SERVICE_ERROR_HANDLER_MISSING;
        // 返回值记录处理器错误，但仍生成可 indication 的响应。
        handler_status = BLE_SERVICE_STATUS_HANDLER_ERROR;
    } else {
        // 唯一一次调用业务处理器；重复请求在上方缓存分支已经返回。
        const ble_service_status_t callback_status = handler(&request, &result, handler_context);
        // 回调失败时覆盖为稳定内部错误，避免发送未初始化业务字段。
        if (callback_status != BLE_SERVICE_STATUS_OK) {
            // 设置内部错误状态。
            result.status = (uint8_t)BLE_SERVICE_CONTROL_INTERNAL_ERROR;
            // 设置业务处理失败错误码。
            result.error_code = (uint16_t)BLE_SERVICE_ERROR_HANDLER_FAILED;
            // 状态未确认提交，沿用最近已知修订号。
            result.state_revision = connection->latest_state_revision;
            // 丢弃失败回调可能留下的 TLV 指针。
            result.tlv = NULL;
            // 清零失败响应 TLV 长度。
            result.tlv_length = UINT16_C(0);
            // 保存诊断返回值。
            handler_status = BLE_SERVICE_STATUS_HANDLER_ERROR;
        }
    }
    // 在栈上保存最多 256 字节控制响应 payload。
    uint8_t response_payload[BLE_SERVICE_MAX_CONTROL_RESPONSE_PAYLOAD];
    // 保存实际响应 payload 长度。
    size_t response_payload_length = 0U;
    // 编码 request_id、命令、状态、错误、修订号和可选 TLV。
    const ble_service_status_t payload_status = ble_service_encode_control_response_payload(
        request_id,
        command_id,
        &result,
        response_payload,
        sizeof(response_payload),
        &response_payload_length);
    // 业务返回过大 TLV 等错误时不能缓存不完整响应。
    if (payload_status != BLE_SERVICE_STATUS_OK) {
        // 返回精确响应编码错误。
        return payload_status;
    }
    // 记录最新已知状态修订号；无效命令和失败回调保持原值。
    connection->latest_state_revision = result.state_revision;
    // 缓存原始请求与对应响应；即使是无效命令，重试也必须得到同一响应。
    ble_service_store_cache_entry(
        connection,
        request_id,
        request_payload,
        frame_view.payload_length,
        response_payload,
        (uint16_t)response_payload_length);
    // 把控制响应 payload 包装成带 CRC 的完整逻辑帧。
    const ble_service_status_t wrap_status = ble_service_wrap_control_response(
        connection,
        response_payload,
        (uint16_t)response_payload_length,
        response_monotonic_ms,
        response_frame,
        response_capacity,
        response_length);
    // 完整帧包装失败时返回容量或协议错误。
    if (wrap_status != BLE_SERVICE_STATUS_OK) {
        // 请求已执行并缓存；客户端使用同 request_id 重试会安全返回缓存响应。
        return wrap_status;
    }
    // 处理器失败时仍已生成明确内部错误响应，返回诊断状态供 NimBLE 记录。
    return handler_status;
}

// 接收并严格顺序重组 Control Point 分片。
ble_service_status_t ble_service_process_control_fragment(
    ble_service_connection_t *connection,
    const uint8_t *fragment,
    size_t fragment_length,
    uint32_t response_monotonic_ms,
    ble_service_command_handler_fn handler,
    void *handler_context,
    uint8_t *response_frame,
    size_t response_capacity,
    size_t *response_length,
    uint8_t *from_cache)
{
    // 必填连接、分片和输出对象必须有效。
    if ((connection == NULL) || (fragment == NULL) || (response_frame == NULL) ||
        (response_length == NULL) || (from_cache == NULL)) {
        // 空参数不改变重组状态。
        return BLE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    // 默认没有完整响应帧。
    *response_length = 0U;
    // 默认不是缓存命中。
    *from_cache = UINT8_C(0);
    // completed_frame 借用 control_reassembler 内部 1040 字节缓冲区。
    const uint8_t *completed_frame = NULL;
    // 保存重组后完整逻辑帧长度。
    size_t completed_frame_length = 0U;
    // complete 为 1 表示本片到达后完整 CRC 已通过。
    uint8_t complete = UINT8_C(0);
    // 调用共享严格顺序重组器，禁止跨 sequence 或乱序拼接。
    const imu_ble_status_t reassembly_status = imu_ble_reassembler_push(
        &connection->control_reassembler,
        fragment,
        fragment_length,
        &completed_frame,
        &completed_frame_length,
        &complete);
    // 任一分片错误后显式重置，下一包必须从索引 0 开始。
    if (reassembly_status != IMU_BLE_STATUS_OK) {
        // 清除半帧，避免坏数据污染后续请求。
        imu_ble_reassembler_reset(&connection->control_reassembler);
        // 返回统一协议错误。
        return ble_service_map_protocol_status(reassembly_status);
    }
    // 尚未收到最后一片时返回 INCOMPLETE，不调用业务处理器。
    if (complete == 0U) {
        // 当前片已经保存，调用者等待下一片。
        return BLE_SERVICE_STATUS_INCOMPLETE;
    }
    // 完整逻辑帧已经过共享 CRC 校验，进入控制业务层。
    return ble_service_process_control_frame(
        connection,
        completed_frame,
        completed_frame_length,
        response_monotonic_ms,
        handler,
        handler_context,
        response_frame,
        response_capacity,
        response_length,
        from_cache);
}
