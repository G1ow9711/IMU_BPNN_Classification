// 引入公开协议声明，使实现与 PC、ESP32 共用同一常量和结构定义。
#include "imu_ble_protocol.h"

// 引入 memcpy 和 memset，用于受容量检查保护的字节复制与状态清零。
#include <string.h>

// 从小端字节流读取 16 位无符号数，输入必须至少包含 2 个有效字节。
static uint16_t imu_ble_read_u16_le(const uint8_t *data)
{
    // 读取低 8 位，保持线上小端约定。
    const uint16_t low = (uint16_t)data[0];
    // 读取高 8 位并左移到目标位置。
    const uint16_t high = (uint16_t)((uint16_t)data[1] << 8U);
    // 合并两个字节并返回主机端 16 位数值。
    return (uint16_t)(low | high);
}

// 从小端字节流读取 32 位无符号数，输入必须至少包含 4 个有效字节。
static uint32_t imu_ble_read_u32_le(const uint8_t *data)
{
    // 读取最低字节，对应数值位 0 到 7。
    const uint32_t byte0 = (uint32_t)data[0];
    // 读取第二字节，对应数值位 8 到 15。
    const uint32_t byte1 = (uint32_t)((uint32_t)data[1] << 8U);
    // 读取第三字节，对应数值位 16 到 23。
    const uint32_t byte2 = (uint32_t)((uint32_t)data[2] << 16U);
    // 读取最高字节，对应数值位 24 到 31。
    const uint32_t byte3 = (uint32_t)((uint32_t)data[3] << 24U);
    // 合并四个字节并返回主机端 32 位数值。
    return byte0 | byte1 | byte2 | byte3;
}

// 把 16 位无符号数按小端序写入至少 2 字节的输出区域。
static void imu_ble_write_u16_le(uint8_t *data, uint16_t value)
{
    // 写入低 8 位，确保 Intel PC 与 ESP32 之外的平台也得到固定线上格式。
    data[0] = (uint8_t)(value & UINT16_C(0x00FF));
    // 写入高 8 位，右移后截取单字节。
    data[1] = (uint8_t)((value >> 8U) & UINT16_C(0x00FF));
}

// 把 32 位无符号数按小端序写入至少 4 字节的输出区域。
static void imu_ble_write_u32_le(uint8_t *data, uint32_t value)
{
    // 写入数值位 0 到 7。
    data[0] = (uint8_t)(value & UINT32_C(0x000000FF));
    // 写入数值位 8 到 15。
    data[1] = (uint8_t)((value >> 8U) & UINT32_C(0x000000FF));
    // 写入数值位 16 到 23。
    data[2] = (uint8_t)((value >> 16U) & UINT32_C(0x000000FF));
    // 写入数值位 24 到 31。
    data[3] = (uint8_t)((value >> 24U) & UINT32_C(0x000000FF));
}

// 计算 CRC-16/CCITT-FALSE，用于在 BLE 链路 CRC 之外检测应用层重组错误。
uint16_t imu_ble_crc16_ccitt_false(const uint8_t *data, size_t length)
{
    // 按 CCITT-FALSE 定义把 CRC 初值设为 0xFFFF。
    uint16_t crc = UINT16_C(0xFFFF);
    // 没有输入字节时直接返回初值；此时允许 data 为空。
    if (length == 0U) {
        // 空消息 CRC 等于初始寄存器值。
        return crc;
    }
    // 非空长度却没有输入指针属于错误调用；返回 0 作为不可用结果。
    if (data == NULL) {
        // 调用者应在上层参数校验中阻止该情况。
        return UINT16_C(0);
    }
    // 遍历输入的每个字节，时间复杂度为 O(length)。
    for (size_t index = 0U; index < length; ++index) {
        // 当前字节放入 CRC 高 8 位，符合非反射 CCITT-FALSE 规则。
        crc = (uint16_t)(crc ^ (uint16_t)((uint16_t)data[index] << 8U));
        // 每个输入字节依次处理 8 个比特。
        for (uint8_t bit = 0U; bit < UINT8_C(8); ++bit) {
            // 记录左移前最高位，用它决定是否异或生成多项式 0x1021。
            const uint8_t high_bit_set = (uint8_t)((crc & UINT16_C(0x8000)) != 0U);
            // CRC 寄存器左移一位，16 位类型会自然丢弃溢出位。
            crc = (uint16_t)(crc << 1U);
            // 最高位为 1 时按多项式除法异或 0x1021。
            if (high_bit_set != 0U) {
                // 应用 CCITT 生成多项式，保持与 C# 实现逐字节一致。
                crc = (uint16_t)(crc ^ UINT16_C(0x1021));
            }
        }
    }
    // 返回最终 CRC；协议不执行结果异或，也不交换字节。
    return crc;
}

// 把逻辑字段编码为固定头、payload 和 CRC16 组成的连续帧。
imu_ble_status_t imu_ble_encode_frame(
    const imu_ble_frame_view_t *frame,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    // 任一必填对象为空时无法安全读取或写入，立即返回参数错误。
    if ((frame == NULL) || (output == NULL) || (output_length == NULL)) {
        // 不写入任何输出长度，防止调用者误用未完成结果。
        return IMU_BLE_STATUS_NULL_ARGUMENT;
    }
    // payload 超过协议 1024 字节上限时拒绝编码。
    if ((size_t)frame->payload_length > IMU_BLE_MAX_PAYLOAD_SIZE) {
        // 该限制保护 ESP32 固定重组缓冲区不被远端长度耗尽。
        return IMU_BLE_STATUS_BAD_LENGTH;
    }
    // 非零 payload 必须提供有效数据指针。
    if ((frame->payload_length > 0U) && (frame->payload == NULL)) {
        // 长度与指针不一致属于调用者错误。
        return IMU_BLE_STATUS_NULL_ARGUMENT;
    }
    // 计算完整帧长度，包含 14 字节头和 2 字节 CRC。
    const size_t required_length = IMU_BLE_LOGICAL_HEADER_SIZE + (size_t)frame->payload_length + IMU_BLE_CRC_SIZE;
    // 输出容量不足时返回所需长度，调用者可据此重新分配。
    if (output_capacity < required_length) {
        // 写入完整帧所需字节数，不写入不完整数据。
        *output_length = required_length;
        // 返回容量不足错误，禁止发送截断帧。
        return IMU_BLE_STATUS_BUFFER_TOO_SMALL;
    }
    // 在偏移 0 写入小端魔数 0xB17E。
    imu_ble_write_u16_le(&output[0], IMU_BLE_MAGIC);
    // 在偏移 2 写入协议主版本。
    output[2] = frame->protocol_major;
    // 在偏移 3 写入协议次版本。
    output[3] = frame->protocol_minor;
    // 在偏移 4 写入消息类型。
    output[4] = frame->message_type;
    // 在偏移 5 写入消息标志位。
    output[5] = frame->flags;
    // 在偏移 6 写入小端逻辑帧序号。
    imu_ble_write_u16_le(&output[6], frame->sequence);
    // 在偏移 8 写入小端单调毫秒时间。
    imu_ble_write_u32_le(&output[8], frame->monotonic_ms);
    // 在偏移 12 写入小端 payload 长度。
    imu_ble_write_u16_le(&output[12], frame->payload_length);
    // 非零 payload 复制到固定头之后，保持上层定义的原始字节顺序。
    if (frame->payload_length > 0U) {
        // memcpy 的长度已经由 1024 字节上限和输出容量检查保护。
        (void)memcpy(&output[IMU_BLE_LOGICAL_HEADER_SIZE], frame->payload, (size_t)frame->payload_length);
    }
    // CRC 范围从协议主版本开始，因此排除前两个魔数字节。
    const size_t crc_input_length = (IMU_BLE_LOGICAL_HEADER_SIZE - 2U) + (size_t)frame->payload_length;
    // 计算 CCITT-FALSE 校验值，覆盖版本、类型、序号、时间、长度和 payload。
    const uint16_t crc = imu_ble_crc16_ccitt_false(&output[2], crc_input_length);
    // CRC 写入位置紧跟 payload 末尾。
    const size_t crc_offset = IMU_BLE_LOGICAL_HEADER_SIZE + (size_t)frame->payload_length;
    // 按协议小端序写入 16 位 CRC。
    imu_ble_write_u16_le(&output[crc_offset], crc);
    // 返回实际完整帧长度。
    *output_length = required_length;
    // 所有字段和校验值均已成功写入。
    return IMU_BLE_STATUS_OK;
}

// 校验魔数、长度和 CRC 后，把连续逻辑帧解析为零拷贝字段视图。
imu_ble_status_t imu_ble_decode_frame(
    const uint8_t *input,
    size_t input_length,
    imu_ble_frame_view_t *frame)
{
    // 输入或输出视图为空时无法安全解析。
    if ((input == NULL) || (frame == NULL)) {
        // 调用者必须提供至少一个完整帧和有效输出结构。
        return IMU_BLE_STATUS_NULL_ARGUMENT;
    }
    // 最短帧必须包含 14 字节头和 2 字节 CRC。
    if (input_length < (IMU_BLE_LOGICAL_HEADER_SIZE + IMU_BLE_CRC_SIZE)) {
        // 截断帧无法读取全部固定字段。
        return IMU_BLE_STATUS_BAD_LENGTH;
    }
    // 验证线上小端魔数，尽早拒绝其它协议数据。
    if (imu_ble_read_u16_le(&input[0]) != IMU_BLE_MAGIC) {
        // 魔数不匹配时不继续解释长度字段。
        return IMU_BLE_STATUS_BAD_MAGIC;
    }
    // 从固定偏移读取 payload 字节数。
    const uint16_t payload_length = imu_ble_read_u16_le(&input[12]);
    // 超过 1024 字节上限的 payload 会耗尽固定重组缓冲区，因此拒绝。
    if ((size_t)payload_length > IMU_BLE_MAX_PAYLOAD_SIZE) {
        // 返回长度错误，不执行任何越界访问。
        return IMU_BLE_STATUS_BAD_LENGTH;
    }
    // 计算长度字段所声明的完整帧字节数。
    const size_t expected_length = IMU_BLE_LOGICAL_HEADER_SIZE + (size_t)payload_length + IMU_BLE_CRC_SIZE;
    // 要求输入恰好是一帧，禁止尾随垃圾和拼接帧被静默接受。
    if (input_length != expected_length) {
        // 上层流式传输必须先按分片包络分离逻辑帧。
        return IMU_BLE_STATUS_BAD_LENGTH;
    }
    // 定位 payload 后的线上 CRC16。
    const size_t crc_offset = IMU_BLE_LOGICAL_HEADER_SIZE + (size_t)payload_length;
    // 按小端序读取发送端 CRC。
    const uint16_t received_crc = imu_ble_read_u16_le(&input[crc_offset]);
    // CRC 输入排除魔数并排除末尾 CRC 自身。
    const size_t crc_input_length = (IMU_BLE_LOGICAL_HEADER_SIZE - 2U) + (size_t)payload_length;
    // 使用同一 CCITT-FALSE 参数重新计算逻辑帧 CRC。
    const uint16_t calculated_crc = imu_ble_crc16_ccitt_false(&input[2], crc_input_length);
    // 校验值不同表示帧损坏、分片缺失或实现不一致。
    if (received_crc != calculated_crc) {
        // 损坏帧不得进入业务状态机。
        return IMU_BLE_STATUS_BAD_CRC;
    }
    // 读取协议主版本供上层执行兼容性判断。
    frame->protocol_major = input[2];
    // 读取协议次版本供上层决定是否忽略新增字段。
    frame->protocol_minor = input[3];
    // 读取消息类型供上层选择 payload 解码器。
    frame->message_type = input[4];
    // 读取标志位供上层识别响应、错误和结束块。
    frame->flags = input[5];
    // 读取小端逻辑帧序号。
    frame->sequence = imu_ble_read_u16_le(&input[6]);
    // 读取小端单调毫秒时间。
    frame->monotonic_ms = imu_ble_read_u32_le(&input[8]);
    // payload 指针直接引用输入缓冲区，调用者必须保持 input 生命周期。
    frame->payload = (payload_length == 0U) ? NULL : &input[IMU_BLE_LOGICAL_HEADER_SIZE];
    // 保存已经校验的 payload 长度。
    frame->payload_length = payload_length;
    // 所有结构和校验均有效。
    return IMU_BLE_STATUS_OK;
}

// 计算给定 ATT MTU 下完整逻辑帧需要多少个顺序分片。
imu_ble_status_t imu_ble_get_fragment_count(
    size_t frame_length,
    uint16_t att_mtu,
    uint16_t *fragment_count)
{
    // 输出计数指针为空时无法返回结果。
    if (fragment_count == NULL) {
        // 调用者必须提供有效 16 位计数位置。
        return IMU_BLE_STATUS_NULL_ARGUMENT;
    }
    // 帧长度必须至少包含固定头和 CRC，且不得超过固定缓冲区上限。
    if ((frame_length < (IMU_BLE_LOGICAL_HEADER_SIZE + IMU_BLE_CRC_SIZE)) || (frame_length > IMU_BLE_MAX_FRAME_SIZE)) {
        // 非法完整帧不参与分片。
        return IMU_BLE_STATUS_BAD_LENGTH;
    }
    // ATT Notification/Write Value 最多占 mtu 减去 3 字节 ATT 操作头。
    if ((size_t)att_mtu <= (size_t)3U + IMU_BLE_FRAGMENT_HEADER_SIZE) {
        // MTU 太小，连 8 字节包络和 1 字节数据都放不下。
        return IMU_BLE_STATUS_INVALID_ARGUMENT;
    }
    // 计算单个 GATT Value 能承载的逻辑帧数据字节数。
    const size_t fragment_data_capacity = (size_t)att_mtu - 3U - IMU_BLE_FRAGMENT_HEADER_SIZE;
    // 使用向上取整公式得到总分片数，避免最后不足一片的数据丢失。
    const size_t count = (frame_length + fragment_data_capacity - 1U) / fragment_data_capacity;
    // 16 位分片计数字段不能表示超过 65535 的结果。
    if ((count == 0U) || (count > (size_t)UINT16_MAX)) {
        // 当前 1040 字节最大帧不会触发该情况，但仍保留完整边界保护。
        return IMU_BLE_STATUS_BAD_LENGTH;
    }
    // 把经过范围检查的结果写入输出参数。
    *fragment_count = (uint16_t)count;
    // 分片数计算成功。
    return IMU_BLE_STATUS_OK;
}

// 把完整逻辑帧的指定片段封装为一个 GATT Value。
imu_ble_status_t imu_ble_encode_fragment(
    const uint8_t *frame,
    size_t frame_length,
    uint16_t att_mtu,
    uint16_t logical_sequence,
    uint16_t fragment_index,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    // 完整帧、输出缓冲区和输出长度都属于必填参数。
    if ((frame == NULL) || (output == NULL) || (output_length == NULL)) {
        // 空指针不允许进入偏移计算或复制。
        return IMU_BLE_STATUS_NULL_ARGUMENT;
    }
    // 先调用公共函数计算总分片数，保持分片公式唯一。
    uint16_t fragment_count = 0U;
    // 保存分片数计算状态，错误时原样返回。
    const imu_ble_status_t count_status = imu_ble_get_fragment_count(frame_length, att_mtu, &fragment_count);
    // MTU 或完整帧长度非法时停止编码。
    if (count_status != IMU_BLE_STATUS_OK) {
        // 传播精确错误码，便于调用者记录原因。
        return count_status;
    }
    // 分片索引必须落在 0 到总数减 1 的范围内。
    if (fragment_index >= fragment_count) {
        // 越界索引无法对应完整帧区域。
        return IMU_BLE_STATUS_BAD_FRAGMENT;
    }
    // 计算每片最多可承载的逻辑帧字节数。
    const size_t fragment_data_capacity = (size_t)att_mtu - 3U - IMU_BLE_FRAGMENT_HEADER_SIZE;
    // 计算当前分片在完整帧中的起始偏移。
    const size_t source_offset = (size_t)fragment_index * fragment_data_capacity;
    // 计算从起始偏移到帧末尾的剩余字节数。
    const size_t remaining = frame_length - source_offset;
    // 最后一片可能不足容量，其余分片使用满容量。
    const size_t fragment_data_length = (remaining < fragment_data_capacity) ? remaining : fragment_data_capacity;
    // 当前 GATT Value 总长度包含 8 字节包络和实际片段数据。
    const size_t required_length = IMU_BLE_FRAGMENT_HEADER_SIZE + fragment_data_length;
    // 输出缓冲区不足时返回所需长度，且不产生截断分片。
    if (output_capacity < required_length) {
        // 告知调用者需要的精确容量。
        *output_length = required_length;
        // 返回容量不足错误。
        return IMU_BLE_STATUS_BUFFER_TOO_SMALL;
    }
    // 包络偏移 0 写入完整逻辑帧 sequence。
    imu_ble_write_u16_le(&output[0], logical_sequence);
    // 包络偏移 2 写入当前分片索引。
    imu_ble_write_u16_le(&output[2], fragment_index);
    // 包络偏移 4 写入总分片数。
    imu_ble_write_u16_le(&output[4], fragment_count);
    // 包络偏移 6 写入当前片段的数据长度。
    imu_ble_write_u16_le(&output[6], (uint16_t)fragment_data_length);
    // 从完整帧对应偏移复制当前片段，长度已经受 MTU 和帧上限保护。
    (void)memcpy(&output[IMU_BLE_FRAGMENT_HEADER_SIZE], &frame[source_offset], fragment_data_length);
    // 返回实际 GATT Value 长度。
    *output_length = required_length;
    // 当前分片编码成功。
    return IMU_BLE_STATUS_OK;
}

// 清空重组状态，使下一次 push 只能从分片索引 0 开始。
void imu_ble_reassembler_reset(imu_ble_reassembler_t *state)
{
    // 空状态指针允许安全忽略，方便断连清理路径直接调用。
    if (state == NULL) {
        // 没有对象可清理时立即返回。
        return;
    }
    // 清零整个状态，包括旧帧数据，避免诊断时暴露上一会话内容。
    (void)memset(state, 0, sizeof(*state));
}

// 按 BLE 保序语义接收分片；发现缺片或乱序时拒绝当前逻辑帧。
imu_ble_status_t imu_ble_reassembler_push(
    imu_ble_reassembler_t *state,
    const uint8_t *fragment,
    size_t fragment_length,
    const uint8_t **frame_data,
    size_t *frame_length,
    uint8_t *complete)
{
    // 所有输入和输出对象都必须有效，避免状态更新到一半后无法返回结果。
    if ((state == NULL) || (fragment == NULL) || (frame_data == NULL) || (frame_length == NULL) || (complete == NULL)) {
        // 空参数不修改已有重组状态。
        return IMU_BLE_STATUS_NULL_ARGUMENT;
    }
    // 默认返回“尚未完成”和空帧，只有最后一片通过后才覆盖。
    *frame_data = NULL;
    // 默认完整帧长度为零。
    *frame_length = 0U;
    // 默认完成标志为零。
    *complete = UINT8_C(0);
    // 分片必须至少包含 8 字节包络。
    if (fragment_length < IMU_BLE_FRAGMENT_HEADER_SIZE) {
        // 截断包络无法安全读取长度和序号。
        return IMU_BLE_STATUS_BAD_FRAGMENT;
    }
    // 读取当前分片所属的逻辑帧 sequence。
    const uint16_t logical_sequence = imu_ble_read_u16_le(&fragment[0]);
    // 读取当前分片索引。
    const uint16_t fragment_index = imu_ble_read_u16_le(&fragment[2]);
    // 读取发送端声明的总分片数。
    const uint16_t fragment_count = imu_ble_read_u16_le(&fragment[4]);
    // 读取当前分片数据区长度。
    const uint16_t fragment_data_length = imu_ble_read_u16_le(&fragment[6]);
    // 总分片数必须大于零，且当前索引必须小于总数。
    if ((fragment_count == 0U) || (fragment_index >= fragment_count)) {
        // 非法索引关系无法形成完整逻辑帧。
        return IMU_BLE_STATUS_BAD_FRAGMENT;
    }
    // 包络中的数据长度必须与实际 GATT Value 剩余长度完全一致。
    if ((size_t)fragment_data_length != (fragment_length - IMU_BLE_FRAGMENT_HEADER_SIZE)) {
        // 不接受尾随垃圾或截断数据。
        return IMU_BLE_STATUS_BAD_FRAGMENT;
    }
    // 索引 0 明确开始一个新逻辑帧，并覆盖任何超时后残留状态。
    if (fragment_index == 0U) {
        // 重置旧长度，新的帧从 buffer 偏移 0 开始写入。
        state->length = 0U;
        // 保存新逻辑帧 sequence，供后续分片一致性检查。
        state->logical_sequence = logical_sequence;
        // 保存总分片数，后续分片不得改变。
        state->fragment_count = fragment_count;
        // 第一片之后期望索引为 0，写入完成后再递增。
        state->next_fragment_index = 0U;
        // 标记重组已经激活。
        state->active = UINT8_C(1);
    }
    // 非起始片必须已有激活状态。
    if (state->active == 0U) {
        // 缺少索引 0 表示通知丢包，调用者应请求状态快照或重传。
        return IMU_BLE_STATUS_BAD_FRAGMENT;
    }
    // sequence、总数和期望索引必须全部匹配当前重组帧。
    if ((logical_sequence != state->logical_sequence) ||
        (fragment_count != state->fragment_count) ||
        (fragment_index != state->next_fragment_index)) {
        // 乱序或跨帧混入时清空 active，禁止继续拼接错误字节。
        state->active = UINT8_C(0);
        // 旧长度不再代表有效帧。
        state->length = 0U;
        // 返回分片错误，上层可统计序列缺口。
        return IMU_BLE_STATUS_BAD_FRAGMENT;
    }
    // 检查追加后是否超过固定 1040 字节帧缓冲区。
    if (state->length + (size_t)fragment_data_length > IMU_BLE_MAX_FRAME_SIZE) {
        // 超限时清除激活状态，防止任何越界写入。
        state->active = UINT8_C(0);
        // 清零有效长度，旧缓冲区不得作为完整帧使用。
        state->length = 0U;
        // 返回长度错误，说明远端声明的逻辑帧超过协议上限。
        return IMU_BLE_STATUS_BAD_LENGTH;
    }
    // 把当前数据区追加到固定重组缓冲区末尾。
    (void)memcpy(&state->buffer[state->length], &fragment[IMU_BLE_FRAGMENT_HEADER_SIZE], (size_t)fragment_data_length);
    // 累加已经接收的完整逻辑帧字节数。
    state->length += (size_t)fragment_data_length;
    // 下一次只接受当前索引加一的分片。
    state->next_fragment_index = (uint16_t)(state->next_fragment_index + UINT16_C(1));
    // 尚未收到最后一片时保持激活并返回成功。
    if (state->next_fragment_index < state->fragment_count) {
        // 当前片已经安全保存，但完整帧还不可解码。
        return IMU_BLE_STATUS_OK;
    }
    // 最后一片已到达，先对重组结果执行完整逻辑帧校验。
    imu_ble_frame_view_t decoded_frame;
    // 调用统一解码器验证魔数、长度和 CRC。
    const imu_ble_status_t decode_status = imu_ble_decode_frame(state->buffer, state->length, &decoded_frame);
    // 校验失败时不得把损坏帧交给业务层。
    if (decode_status != IMU_BLE_STATUS_OK) {
        // 清除激活状态，等待下一帧索引 0。
        state->active = UINT8_C(0);
        // 返回解码器的精确错误码，例如 BAD_CRC。
        return decode_status;
    }
    // 返回指向状态内部固定缓冲区的完整帧指针。
    *frame_data = state->buffer;
    // 返回完整帧实际字节数。
    *frame_length = state->length;
    // 设置完成标志，通知调用者可以立即处理逻辑帧。
    *complete = UINT8_C(1);
    // 当前帧完成后退出激活状态，下一包必须从索引 0 开始。
    state->active = UINT8_C(0);
    // 重组与完整帧校验均成功。
    return IMU_BLE_STATUS_OK;
}
