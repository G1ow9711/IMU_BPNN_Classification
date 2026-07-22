// 引入设备端 BLE 纯 C 核心，主机测试不依赖 FreeRTOS、NimBLE 或真实射频硬件。
#include "ble_service_core.h"
// 引入正式 Manifest TLV 构建器，测试动态设备资料、模型摘要、类别 CRC 和存储容量。
#include "ble_service_manifest.h"
// 引入绑定删除纯 C 编排器，验证断连与官方存储清除顺序。
#include "ble_service_bond_manager.h"

// 引入标准输入输出，仅用于打印唯一成功标志或失败位置。
#include <stdio.h>
// 引入标准库 exit，断言失败后立即终止，禁止后续结果掩盖首个错误。
#include <stdlib.h>
// 引入 memcpy 和 memcmp，用于构造 TLV、损坏帧和逐字节断言。
#include <string.h>

// 测试断言累计数，最终输出便于确认所有路径实际执行。
static unsigned int g_assertion_count = 0U;

// 保存忘记电脑操作调用次数和模拟底层返回码。
typedef struct test_bond_ops_context {
    // terminate_calls 记录活动连接断开请求次数。
    uint32_t terminate_calls;
    // clear_calls 记录绑定存储清空次数。
    uint32_t clear_calls;
    // last_handle 保存最近一次断开的连接句柄。
    uint16_t last_handle;
    // terminate_result 模拟 ble_gap_terminate 返回码。
    int terminate_result;
    // clear_result 模拟 ble_store_clear 返回码。
    int clear_result;
} test_bond_ops_context_t;

// 模拟 NimBLE 主动断连并记录句柄。
static int test_bond_terminate(uint16_t connection_handle, void *context)
{
    // 恢复测试上下文。
    test_bond_ops_context_t *const state = (test_bond_ops_context_t *)context;
    // 记录一次调用。
    state->terminate_calls += UINT32_C(1);
    // 保存调用方传入的当前句柄。
    state->last_handle = connection_handle;
    // 返回场景预设结果。
    return state->terminate_result;
}

// 模拟 NimBLE 官方 ble_store_clear 并记录调用次数。
static int test_bond_clear_store(void *context)
{
    // 恢复测试上下文。
    test_bond_ops_context_t *const state = (test_bond_ops_context_t *)context;
    // 每次忘记电脑都必须调用一次存储清除。
    state->clear_calls += UINT32_C(1);
    // 返回场景预设结果。
    return state->clear_result;
}

// 断言条件为假时打印文件、行号和中文原因并终止。
static void test_assert_impl(int condition, const char *message, const char *file, int line)
{
    // 每次调用均增加断言计数，包括失败断言。
    g_assertion_count += 1U;
    // 条件成立时继续执行后续场景。
    if (condition != 0) {
        // 成功断言无需输出噪声。
        return;
    }
    // 输出首个失败位置和业务原因。
    (void)fprintf(stderr, "ASSERT_FAIL %s:%d %s\n", file, line, message);
    // 使用非零退出码阻止测试脚本误报通过。
    exit(EXIT_FAILURE);
}

// TEST_ASSERT 自动补充当前源文件和行号。
#define TEST_ASSERT(condition, message) test_assert_impl((condition), (message), __FILE__, __LINE__)

// 从至少 2 字节小端区域读取 16 位值，测试响应字段偏移。
static uint16_t test_read_u16_le(const uint8_t *data)
{
    // 合并低字节和高字节。
    return (uint16_t)((uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8U));
}

// 从至少 4 字节小端区域读取 32 位值，测试 request_id 和 revision。
static uint32_t test_read_u32_le(const uint8_t *data)
{
    // 合并四个固定偏移字节。
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8U) |
        ((uint32_t)data[2] << 16U) |
        ((uint32_t)data[3] << 24U);
}

// 从至少 8 字节小端区域读取 64 位值，锁定 LittleFS 可用字节字段端序。
static uint64_t test_read_u64_le(const uint8_t *data)
{
    // 从最高偏移向最低偏移折叠，避免依赖主机 CPU 自身端序。
    uint64_t value = UINT64_C(0);
    // 固定读取八个线上字节。
    for (uint8_t byte_index = UINT8_C(8); byte_index > UINT8_C(0); --byte_index) {
        // 每轮左移八位后并入当前较低偏移字节。
        value = (value << 8U) | (uint64_t)data[byte_index - UINT8_C(1)];
    }
    // 返回解码后的无符号 64 位值。
    return value;
}

// 把 32 位值按小端序写入请求 payload。
static void test_write_u32_le(uint8_t *data, uint32_t value)
{
    // 写入最低字节。
    data[0] = (uint8_t)(value & UINT32_C(0xFF));
    // 写入第二字节。
    data[1] = (uint8_t)((value >> 8U) & UINT32_C(0xFF));
    // 写入第三字节。
    data[2] = (uint8_t)((value >> 16U) & UINT32_C(0xFF));
    // 写入最高字节。
    data[3] = (uint8_t)((value >> 24U) & UINT32_C(0xFF));
}

// 处理器上下文记录实际业务执行次数和递增状态修订号。
typedef struct test_handler_context {
    // execute_count 每次真正进入业务处理器时加一，重复请求不得增加。
    uint32_t execute_count;
    // revision 模拟设备权威状态修订号。
    uint32_t revision;
} test_handler_context_t;

// 测试业务处理器把合法命令标记成功，并返回两字节诊断 TLV。
static ble_service_status_t test_command_handler(
    const ble_service_control_request_t *request,
    ble_service_command_result_t *result,
    void *context)
{
    // 固定响应 TLV 生命周期覆盖整个测试进程。
    static const uint8_t response_tlv[2] = {UINT8_C(0xAA), UINT8_C(0x55)};
    // 把无类型上下文恢复为测试计数对象。
    test_handler_context_t *const handler_context = (test_handler_context_t *)context;
    // 验证服务层只把 v1 命令交给回调。
    TEST_ASSERT(request->command_version == UINT8_C(1), "业务处理器收到非 v1 命令。 ");
    // 增加真正业务执行次数。
    handler_context->execute_count += UINT32_C(1);
    // 每次新命令提交后递增权威修订号。
    handler_context->revision += UINT32_C(1);
    // 返回成功控制状态。
    result->status = (uint8_t)BLE_SERVICE_CONTROL_OK;
    // 成功响应错误码为 0。
    result->error_code = (uint16_t)BLE_SERVICE_ERROR_NONE;
    // 返回递增后的权威修订号。
    result->state_revision = handler_context->revision;
    // 返回固定两字节 TLV，验证缓存保留完整响应。
    result->tlv = response_tlv;
    // 设置 TLV 长度为 2。
    result->tlv_length = UINT16_C(2);
    // 业务处理成功。
    return BLE_SERVICE_STATUS_OK;
}

// 构造带可选 TLV 的完整 ControlRequest 逻辑帧。
static size_t test_build_control_frame(
    uint32_t request_id,
    uint8_t command_id,
    uint8_t command_version,
    const uint8_t *tlv,
    size_t tlv_length,
    uint8_t *output,
    size_t output_capacity)
{
    // 请求 payload 最大 256 字节，当前测试 TLV 必须不超过 250 字节。
    uint8_t payload[BLE_SERVICE_MAX_CONTROL_REQUEST_PAYLOAD];
    // 断言测试调用没有超过生产上限。
    TEST_ASSERT(tlv_length <= (BLE_SERVICE_MAX_CONTROL_REQUEST_PAYLOAD - 6U), "测试 TLV 超过控制请求上限。 ");
    // 写入 request_id。
    test_write_u32_le(&payload[0], request_id);
    // 写入命令 ID。
    payload[4] = command_id;
    // 写入命令版本。
    payload[5] = command_version;
    // 非空 TLV 复制到固定头之后。
    if (tlv_length > 0U) {
        // 调用者必须提供有效 TLV 指针。
        TEST_ASSERT(tlv != NULL, "非空测试 TLV 缺少指针。 ");
        // 复制测试参数。
        (void)memcpy(&payload[6], tlv, tlv_length);
    }
    // 保存完整逻辑帧实际长度。
    size_t output_length = 0U;
    // 通过生产编码器生成协议版本、CRC 和固定头。
    const ble_service_status_t status = ble_service_encode_message(
        (uint8_t)BLE_SERVICE_MESSAGE_CONTROL_REQUEST,
        UINT8_C(0),
        UINT16_C(7),
        UINT32_C(1234),
        payload,
        (uint16_t)(6U + tlv_length),
        output,
        output_capacity,
        &output_length);
    // 构造请求必须成功。
    TEST_ASSERT(status == BLE_SERVICE_STATUS_OK, "ControlRequest 编码失败。 ");
    // 返回完整帧长度。
    return output_length;
}

// 解码 ControlResponse 并返回 payload 视图。
static imu_ble_frame_view_t test_decode_control_response(const uint8_t *frame, size_t frame_length)
{
    // 初始化共享帧视图。
    imu_ble_frame_view_t decoded;
    // 验证响应 CRC、长度和魔数。
    const imu_ble_status_t status = imu_ble_decode_frame(frame, frame_length, &decoded);
    // 响应必须是有效逻辑帧。
    TEST_ASSERT(status == IMU_BLE_STATUS_OK, "ControlResponse 逻辑帧解码失败。 ");
    // 消息类型必须为 2。
    TEST_ASSERT(decoded.message_type == (uint8_t)BLE_SERVICE_MESSAGE_CONTROL_RESPONSE, "响应消息类型不是 ControlResponse。 ");
    // 固定响应至少 12 字节。
    TEST_ASSERT(decoded.payload_length >= UINT16_C(12), "ControlResponse payload 小于固定头。 ");
    // 返回只借用原响应缓冲区的视图。
    return decoded;
}

// 验证消息类型 1～9 全部接受，范围外明确拒绝。
static void test_message_types(void)
{
    // 遍历当前 v1 定义的全部九种类型。
    for (uint8_t type = UINT8_C(1); type <= UINT8_C(9); ++type) {
        // 每个定义值都必须通过。
        TEST_ASSERT(ble_service_validate_message_type(type) == BLE_SERVICE_STATUS_OK, "合法消息类型被拒绝。 ");
    }
    // 0 未定义，必须拒绝。
    TEST_ASSERT(ble_service_validate_message_type(UINT8_C(0)) == BLE_SERVICE_STATUS_WRONG_MESSAGE_TYPE, "消息类型 0 未被拒绝。 ");
    // 10 未定义，必须拒绝。
    TEST_ASSERT(ble_service_validate_message_type(UINT8_C(10)) == BLE_SERVICE_STATUS_WRONG_MESSAGE_TYPE, "消息类型 10 未被拒绝。 ");
}

// 验证推理诊断V1固定28字节布局，确保真板双M0证据不依赖日志文本猜测。
static void test_inference_diagnostic_v1(void)
{
    // 构造覆盖融合、基础、掩码、时间和错误累计的诊断对象。
    ble_service_inference_diagnostic_v1_t diagnostic = {
        // 当前诊断结构版本固定为1。
        .diagnostic_version = UINT8_C(1),
        // 融合输出Top-1使用类别3。
        .fused_action_id = UINT8_C(3),
        // 基础M0 Top-1使用类别4，证明字段未与融合结果混淆。
        .base_action_id = UINT8_C(4),
        // 掩码M0 Top-1使用类别5。
        .masked_action_id = UINT8_C(5),
        // 三组Q15置信度使用不同非对称值验证偏移。
        .fused_confidence_q15 = UINT16_C(0x1234),
        .base_confidence_q15 = UINT16_C(0x2345),
        .masked_confidence_q15 = UINT16_C(0x3456),
        // 质量位使用0x4567验证小端序。
        .quality_flags = UINT16_C(0x4567),
        // 窗口序号使用非对称四字节值。
        .window_sequence = UINT32_C(0x01020304),
        // 窗口末时刻单位毫秒。
        .window_end_ms = UINT32_C(0x11223344),
        // 推理耗时单位微秒。
        .inference_time_us = UINT32_C(0x55667788),
        // 累计异常窗口数。
        .failure_count = UINT32_C(0x99AABBCC)
    };
    // 固定输出缓冲严格等于协议长度。
    uint8_t payload[BLE_SERVICE_INFERENCE_DIAGNOSTIC_V1_SIZE];
    // 保存编码器返回长度。
    size_t payload_length = 0U;
    // 调用生产编码器。
    const ble_service_status_t status = ble_service_encode_inference_diagnostic_v1(
        &diagnostic,
        payload,
        sizeof(payload),
        &payload_length);
    // 合法对象必须成功编码。
    TEST_ASSERT(status == BLE_SERVICE_STATUS_OK, "推理诊断V1编码失败。 ");
    // 线上长度必须固定为28字节。
    TEST_ASSERT(payload_length == BLE_SERVICE_INFERENCE_DIAGNOSTIC_V1_SIZE, "推理诊断V1长度错误。 ");
    // 版本和三组Top-1必须位于偏移0～3。
    TEST_ASSERT(
        (payload[0] == UINT8_C(1)) &&
        (payload[1] == UINT8_C(3)) &&
        (payload[2] == UINT8_C(4)) &&
        (payload[3] == UINT8_C(5)),
        "推理诊断V1动作字段偏移错误。 ");
    // 三组Q15置信度必须位于偏移4、6、8。
    TEST_ASSERT(
        (test_read_u16_le(&payload[4]) == UINT16_C(0x1234)) &&
        (test_read_u16_le(&payload[6]) == UINT16_C(0x2345)) &&
        (test_read_u16_le(&payload[8]) == UINT16_C(0x3456)),
        "推理诊断V1置信度偏移错误。 ");
    // 质量位必须位于偏移10。
    TEST_ASSERT(test_read_u16_le(&payload[10]) == UINT16_C(0x4567), "推理诊断V1质量位偏移错误。 ");
    // 四个32位字段必须从偏移12开始连续小端编码。
    TEST_ASSERT(
        (test_read_u32_le(&payload[12]) == UINT32_C(0x01020304)) &&
        (test_read_u32_le(&payload[16]) == UINT32_C(0x11223344)) &&
        (test_read_u32_le(&payload[20]) == UINT32_C(0x55667788)) &&
        (test_read_u32_le(&payload[24]) == UINT32_C(0x99AABBCC)),
        "推理诊断V1序号、时间或异常累计偏移错误。 ");
    // 非法动作索引11必须拒绝，防止类别表错位。
    diagnostic.fused_action_id = UINT8_C(11);
    // 编码非法动作并检查稳定错误码。
    TEST_ASSERT(
        ble_service_encode_inference_diagnostic_v1(&diagnostic, payload, sizeof(payload), &payload_length) ==
            BLE_SERVICE_STATUS_INVALID_ARGUMENT,
        "推理诊断V1非法动作未被拒绝。 ");
}

// 验证 LiveStateV1 的固定长度、全部偏移和范围保护。
static void test_live_state_v1(void)
{
    // 构造覆盖全部字段的状态。
    ble_service_live_state_v1_t state = {
        // 会话序号使用非对称字节值验证小端序。
        .session_sequence = UINT32_C(0x01020304),
        // 状态修订号使用另一组非对称值。
        .state_revision = UINT32_C(0x11223344),
        // 会话时长为 5000 毫秒。
        .elapsed_ms = UINT32_C(5000),
        // Running 状态示例值为 3；2 固定表示 Preparing。
        .device_state = UINT8_C(3),
        // jumping_squat 模型索引为 3。
        .action_id = UINT8_C(3),
        // 指标单位为次数 1。
        .metric_kind = UINT8_C(1),
        // 厂家原配电池当前电量 88%。
        .battery_percent = UINT8_C(88),
        // 已完成 12 次。
        .metric_value = UINT32_C(12),
        // Q15 置信度使用 0x7FFF。
        .confidence_q15 = UINT16_C(0x7FFF),
        // 累计 2.345 千卡。
        .calories_mcal = UINT32_C(2345),
        // 质量标志使用 0x1234 验证小端序。
        .quality_flags = UINT16_C(0x1234),
        // 电源标志使用 0x05。
        .power_flags = UINT8_C(0x05),
        // 目标完成度为 67%。
        .goal_percent = UINT8_C(67)
    };
    // 固定 payload 缓冲区恰好 30 字节。
    uint8_t payload[BLE_SERVICE_LIVE_STATE_V1_SIZE];
    // 保存编码长度。
    size_t payload_length = 0U;
    // 编码完整状态。
    const ble_service_status_t status = ble_service_encode_live_state_v1(
        &state,
        payload,
        sizeof(payload),
        &payload_length);
    // 编码必须成功。
    TEST_ASSERT(status == BLE_SERVICE_STATUS_OK, "LiveStateV1 编码失败。 ");
    // 长度必须严格为 30。
    TEST_ASSERT(payload_length == BLE_SERVICE_LIVE_STATE_V1_SIZE, "LiveStateV1 长度不是 30 字节。 ");
    // 核对会话序号小端偏移 0。
    TEST_ASSERT(test_read_u32_le(&payload[0]) == state.session_sequence, "会话序号编码错误。 ");
    // 核对状态修订号偏移 4。
    TEST_ASSERT(test_read_u32_le(&payload[4]) == state.state_revision, "状态修订号编码错误。 ");
    // 核对动作、电量和目标三个单字节字段。
    TEST_ASSERT((payload[13] == UINT8_C(3)) && (payload[15] == UINT8_C(88)) && (payload[29] == UINT8_C(67)), "LiveStateV1 单字节字段偏移错误。 ");
    // 核对置信度偏移 20。
    TEST_ASSERT(test_read_u16_le(&payload[20]) == UINT16_C(0x7FFF), "Q15 置信度编码错误。 ");
    // 非法电量 101 必须拒绝。
    state.battery_percent = UINT8_C(101);
    // 编码非法状态并检查返回码。
    TEST_ASSERT(ble_service_encode_live_state_v1(&state, payload, sizeof(payload), &payload_length) == BLE_SERVICE_STATUS_INVALID_ARGUMENT, "非法电量未被拒绝。 ");
}

// 验证 EventV1 固定 36 字节布局、端序和边界保护，确保 PC 动画不会猜测 payload 偏移。
static void test_event_v1(void)
{
    // 构造一次跳跃深蹲计数事件，覆盖全部固定字段。
    ble_service_event_v1_t event = {
        // 当前 payload 结构版本固定为 1。
        .event_version = UINT8_C(1),
        // 事件类型为完成一次重复动作。
        .event_type = (uint8_t)BLE_SERVICE_EVENT_REPETITION_COUNTED,
        // 设备状态使用 Running 的线上值 3；2 固定表示 Preparing。
        .device_state = UINT8_C(3),
        // 动作索引 3 对应 jumping_squat。
        .action_id = UINT8_C(3),
        // 指标种类 1 表示次数。
        .metric_kind = UINT8_C(1),
        // 原配 400 mAh 电池当前为 88%。
        .battery_percent = UINT8_C(88),
        // 质量位使用非对称字节验证小端序。
        .quality_flags = UINT16_C(0x1234),
        // 会话序号使用非对称四字节值。
        .session_sequence = UINT32_C(0x01020304),
        // 会话内事件序号为 7。
        .event_sequence = UINT32_C(7),
        // 权威状态修订号为 9。
        .state_revision = UINT32_C(9),
        // 本次增加 1 次。
        .metric_delta = UINT32_C(1),
        // 当前累计 12 次。
        .metric_total = UINT32_C(12),
        // 累计 2.345 kcal，线上单位为千分之一千卡。
        .calories_mcal = UINT32_C(2345),
        // Q15 稳定度使用最大正值。
        .confidence_q15 = UINT16_C(0x7FFF),
        // 普通计数没有故障或关机原因，细节码为 0。
        .detail_code = UINT16_C(0)
    };
    // 固定缓冲区必须与 EventV1 合同长度完全一致。
    uint8_t payload[BLE_SERVICE_EVENT_V1_SIZE];
    // 保存编码器返回的实际长度。
    size_t payload_length = 0U;
    // 调用生产编码器生成 EventV1 payload。
    const ble_service_status_t status = ble_service_encode_event_v1(
        &event,
        payload,
        sizeof(payload),
        &payload_length);
    // 合法事件必须编码成功。
    TEST_ASSERT(status == BLE_SERVICE_STATUS_OK, "EventV1 编码失败。 ");
    // 线上长度必须固定为 36 字节。
    TEST_ASSERT(payload_length == BLE_SERVICE_EVENT_V1_SIZE, "EventV1 长度不是 36 字节。 ");
    // 前六个单字节字段必须位于固定偏移 0～5。
    TEST_ASSERT(
        (payload[0] == UINT8_C(1)) &&
        (payload[1] == (uint8_t)BLE_SERVICE_EVENT_REPETITION_COUNTED) &&
        (payload[2] == UINT8_C(3)) &&
        (payload[3] == UINT8_C(3)) &&
        (payload[4] == UINT8_C(1)) &&
        (payload[5] == UINT8_C(88)),
        "EventV1 单字节字段偏移错误。 ");
    // 质量位位于偏移 6，按小端编码。
    TEST_ASSERT(test_read_u16_le(&payload[6]) == UINT16_C(0x1234), "EventV1 质量位编码错误。 ");
    // 会话、事件和修订号分别位于偏移 8、12、16。
    TEST_ASSERT(
        (test_read_u32_le(&payload[8]) == UINT32_C(0x01020304)) &&
        (test_read_u32_le(&payload[12]) == UINT32_C(7)) &&
        (test_read_u32_le(&payload[16]) == UINT32_C(9)),
        "EventV1 序号字段编码错误。 ");
    // 指标增量、累计和热量分别位于偏移 20、24、28。
    TEST_ASSERT(
        (test_read_u32_le(&payload[20]) == UINT32_C(1)) &&
        (test_read_u32_le(&payload[24]) == UINT32_C(12)) &&
        (test_read_u32_le(&payload[28]) == UINT32_C(2345)),
        "EventV1 指标字段编码错误。 ");
    // Q15 与细节码分别位于偏移 32、34。
    TEST_ASSERT(
        (test_read_u16_le(&payload[32]) == UINT16_C(0x7FFF)) &&
        (test_read_u16_le(&payload[34]) == UINT16_C(0)),
        "EventV1 尾部字段编码错误。 ");
    // 非法动作索引必须被拒绝，防止 PC 播放错误动画。
    event.action_id = UINT8_C(11);
    // 验证动作范围检查。
    TEST_ASSERT(
        ble_service_encode_event_v1(&event, payload, sizeof(payload), &payload_length) == BLE_SERVICE_STATUS_INVALID_ARGUMENT,
        "EventV1 非法动作未被拒绝。 ");
}

// 验证命令 1～11 全部进入回调，范围外和非 v1 版本只返回错误响应。
static void test_command_ids_and_versions(void)
{
    // 初始化连接状态。
    ble_service_connection_t connection;
    // 清空重组、缓存和响应序号。
    ble_service_connection_reset(&connection);
    // 初始化业务执行计数和修订号。
    test_handler_context_t context = {UINT32_C(0), UINT32_C(0)};
    // 请求和响应均使用协议最大固定缓冲区。
    uint8_t request_frame[IMU_BLE_MAX_FRAME_SIZE];
    // 保存响应完整逻辑帧。
    uint8_t response_frame[IMU_BLE_MAX_FRAME_SIZE];
    // 保存响应实际长度。
    size_t response_length = 0U;
    // 保存缓存命中标志。
    uint8_t from_cache = UINT8_C(0);
    // 遍历 v1 的 11 个命令编号。
    for (uint8_t command_id = UINT8_C(1); command_id <= UINT8_C(11); ++command_id) {
        // 为每个命令使用独立 request_id，避免缓存影响回调次数。
        const size_t request_length = test_build_control_frame(
            (uint32_t)(UINT32_C(1000) + (uint32_t)command_id),
            command_id,
            UINT8_C(1),
            NULL,
            0U,
            request_frame,
            sizeof(request_frame));
        // 处理当前命令。
        const ble_service_status_t status = ble_service_process_control_frame(
            &connection,
            request_frame,
            request_length,
            (uint32_t)command_id,
            test_command_handler,
            &context,
            response_frame,
            sizeof(response_frame),
            &response_length,
            &from_cache);
        // 每个定义命令都必须成功进入处理器。
        TEST_ASSERT((status == BLE_SERVICE_STATUS_OK) && (from_cache == 0U), "合法命令未成功处理。 ");
        // 解码响应并核对 command_id 和 OK 状态。
        const imu_ble_frame_view_t response = test_decode_control_response(response_frame, response_length);
        // 响应必须对应当前命令。
        TEST_ASSERT((response.payload[4] == command_id) && (response.payload[5] == (uint8_t)BLE_SERVICE_CONTROL_OK), "合法命令响应字段错误。 ");
    }
    // 11 个合法命令各执行一次。
    TEST_ASSERT(context.execute_count == UINT32_C(11), "命令 1～11 没有各执行一次。 ");
    // 构造超出范围的 command_id 12。
    size_t request_length = test_build_control_frame(
        UINT32_C(2000),
        UINT8_C(12),
        UINT8_C(1),
        NULL,
        0U,
        request_frame,
        sizeof(request_frame));
    // 服务层应生成错误响应但不调用业务处理器。
    TEST_ASSERT(
        ble_service_process_control_frame(
            &connection,
            request_frame,
            request_length,
            UINT32_C(20),
            test_command_handler,
            &context,
            response_frame,
            sizeof(response_frame),
            &response_length,
            &from_cache) == BLE_SERVICE_STATUS_OK,
        "非法命令没有生成控制响应。 ");
    // 解码非法命令响应。
    imu_ble_frame_view_t response = test_decode_control_response(response_frame, response_length);
    // 核对无效命令状态和错误码。
    TEST_ASSERT(
        (response.payload[5] == (uint8_t)BLE_SERVICE_CONTROL_INVALID_COMMAND) &&
        (test_read_u16_le(&response.payload[6]) == (uint16_t)BLE_SERVICE_ERROR_INVALID_COMMAND) &&
        (context.execute_count == UINT32_C(11)),
        "非法命令错误执行或响应字段错误。 ");
    // 构造合法 command_id 但 command_version=2。
    request_length = test_build_control_frame(
        UINT32_C(2001),
        (uint8_t)BLE_SERVICE_COMMAND_START_SESSION,
        UINT8_C(2),
        NULL,
        0U,
        request_frame,
        sizeof(request_frame));
    // 服务层应生成版本错误响应。
    TEST_ASSERT(
        ble_service_process_control_frame(
            &connection,
            request_frame,
            request_length,
            UINT32_C(21),
            test_command_handler,
            &context,
            response_frame,
            sizeof(response_frame),
            &response_length,
            &from_cache) == BLE_SERVICE_STATUS_OK,
        "非法命令版本没有生成控制响应。 ");
    // 解码版本错误响应。
    response = test_decode_control_response(response_frame, response_length);
    // 核对版本错误状态且执行次数不变。
    TEST_ASSERT(
        (response.payload[5] == (uint8_t)BLE_SERVICE_CONTROL_INVALID_VERSION) &&
        (test_read_u16_le(&response.payload[6]) == (uint16_t)BLE_SERVICE_ERROR_INVALID_COMMAND_VERSION) &&
        (context.execute_count == UINT32_C(11)),
        "非法命令版本错误执行或响应字段错误。 ");
}

// 验证正常命令、精确重复和同 request_id 冲突。
static void test_control_idempotence_and_conflict(void)
{
    // 初始化连接状态。
    ble_service_connection_t connection;
    // 建连前清空重组与缓存。
    ble_service_connection_reset(&connection);
    // 初始化处理器计数和修订号。
    test_handler_context_t context = {UINT32_C(0), UINT32_C(100)};
    // 请求 TLV 模拟偏好参数。
    const uint8_t tlv_a[2] = {UINT8_C(0x10), UINT8_C(0x20)};
    // 冲突请求使用不同 TLV。
    const uint8_t tlv_b[2] = {UINT8_C(0x10), UINT8_C(0x21)};
    // 保存完整请求帧。
    uint8_t request_frame[IMU_BLE_MAX_FRAME_SIZE];
    // 构造 request_id=42 的开始会话请求。
    const size_t request_length = test_build_control_frame(
        UINT32_C(42),
        (uint8_t)BLE_SERVICE_COMMAND_START_SESSION,
        UINT8_C(1),
        tlv_a,
        sizeof(tlv_a),
        request_frame,
        sizeof(request_frame));
    // 保存完整响应帧。
    uint8_t response_frame[IMU_BLE_MAX_FRAME_SIZE];
    // 保存响应长度。
    size_t response_length = 0U;
    // 保存缓存命中标志。
    uint8_t from_cache = UINT8_C(0);
    // 第一次处理必须进入业务回调。
    ble_service_status_t status = ble_service_process_control_frame(
        &connection,
        request_frame,
        request_length,
        UINT32_C(2000),
        test_command_handler,
        &context,
        response_frame,
        sizeof(response_frame),
        &response_length,
        &from_cache);
    // 正常请求成功且不是缓存命中。
    TEST_ASSERT((status == BLE_SERVICE_STATUS_OK) && (from_cache == 0U), "首次控制请求状态错误。 ");
    // 业务只执行一次。
    TEST_ASSERT(context.execute_count == UINT32_C(1), "首次控制请求没有执行一次。 ");
    // 解码响应并核对 request_id、命令、状态和修订号。
    imu_ble_frame_view_t response = test_decode_control_response(response_frame, response_length);
    // 核对固定响应字段。
    TEST_ASSERT(
        (test_read_u32_le(&response.payload[0]) == UINT32_C(42)) &&
        (response.payload[4] == (uint8_t)BLE_SERVICE_COMMAND_START_SESSION) &&
        (response.payload[5] == (uint8_t)BLE_SERVICE_CONTROL_OK) &&
        (test_read_u32_le(&response.payload[8]) == UINT32_C(101)),
        "首次控制响应字段错误。 ");
    // 保存首次响应 payload，重复请求必须逐字节一致。
    uint8_t first_response_payload[BLE_SERVICE_MAX_CONTROL_RESPONSE_PAYLOAD];
    // 复制首次响应 payload。
    (void)memcpy(first_response_payload, response.payload, response.payload_length);
    // 用同 request_id 和完全相同帧重试。
    status = ble_service_process_control_frame(
        &connection,
        request_frame,
        request_length,
        UINT32_C(2500),
        test_command_handler,
        &context,
        response_frame,
        sizeof(response_frame),
        &response_length,
        &from_cache);
    // 重试成功且标记缓存命中。
    TEST_ASSERT((status == BLE_SERVICE_STATUS_OK) && (from_cache == 1U), "重复控制请求未命中缓存。 ");
    // 业务执行次数必须保持 1。
    TEST_ASSERT(context.execute_count == UINT32_C(1), "重复控制请求再次执行业务。 ");
    // 解码重试响应。
    response = test_decode_control_response(response_frame, response_length);
    // 响应 payload 必须与首次结果逐字节一致。
    TEST_ASSERT(memcmp(first_response_payload, response.payload, response.payload_length) == 0, "重复请求响应 payload 发生变化。 ");
    // 构造相同 request_id 但不同 TLV 的冲突帧。
    const size_t conflict_length = test_build_control_frame(
        UINT32_C(42),
        (uint8_t)BLE_SERVICE_COMMAND_START_SESSION,
        UINT8_C(1),
        tlv_b,
        sizeof(tlv_b),
        request_frame,
        sizeof(request_frame));
    // 处理冲突请求。
    status = ble_service_process_control_frame(
        &connection,
        request_frame,
        conflict_length,
        UINT32_C(3000),
        test_command_handler,
        &context,
        response_frame,
        sizeof(response_frame),
        &response_length,
        &from_cache);
    // 服务必须返回专用冲突状态并生成响应。
    TEST_ASSERT((status == BLE_SERVICE_STATUS_REQUEST_CONFLICT) && (response_length > 0U), "request_id 冲突未明确返回。 ");
    // 冲突不得执行业务回调。
    TEST_ASSERT(context.execute_count == UINT32_C(1), "request_id 冲突错误执行业务。 ");
    // 解码冲突响应。
    response = test_decode_control_response(response_frame, response_length);
    // 核对冲突状态和精确错误码。
    TEST_ASSERT(
        (response.payload[5] == (uint8_t)BLE_SERVICE_CONTROL_REQUEST_CONFLICT) &&
        (test_read_u16_le(&response.payload[6]) == (uint16_t)BLE_SERVICE_ERROR_REQUEST_ID_CONFLICT),
        "冲突响应状态或错误码错误。 ");
}

// 验证 CRC、消息类型和控制长度错误都不会进入业务回调。
static void test_invalid_frames(void)
{
    // 初始化独立连接状态。
    ble_service_connection_t connection;
    // 清空状态。
    ble_service_connection_reset(&connection);
    // 初始化处理器计数。
    test_handler_context_t context = {UINT32_C(0), UINT32_C(0)};
    // 保存输入逻辑帧。
    uint8_t request_frame[IMU_BLE_MAX_FRAME_SIZE];
    // 构造合法请求作为损坏基线。
    const size_t valid_length = test_build_control_frame(
        UINT32_C(7),
        (uint8_t)BLE_SERVICE_COMMAND_GET_SNAPSHOT,
        UINT8_C(1),
        NULL,
        0U,
        request_frame,
        sizeof(request_frame));
    // 保存响应帧。
    uint8_t response_frame[IMU_BLE_MAX_FRAME_SIZE];
    // 保存响应长度。
    size_t response_length = 0U;
    // 保存缓存标志。
    uint8_t from_cache = UINT8_C(0);
    // 翻转 CRC 前一个 payload 字节，保持长度不变但校验失败。
    request_frame[IMU_BLE_LOGICAL_HEADER_SIZE] ^= UINT8_C(0x01);
    // 处理 CRC 损坏帧。
    ble_service_status_t status = ble_service_process_control_frame(
        &connection,
        request_frame,
        valid_length,
        UINT32_C(1),
        test_command_handler,
        &context,
        response_frame,
        sizeof(response_frame),
        &response_length,
        &from_cache);
    // 坏 CRC 返回协议错误且不执行处理器。
    TEST_ASSERT((status == BLE_SERVICE_STATUS_PROTOCOL_ERROR) && (context.execute_count == 0U), "坏 CRC 进入业务处理器。 ");
    // 构造 Event 类型但 payload 恰好像控制请求的逻辑帧。
    uint8_t control_like_payload[6] = {UINT8_C(1), UINT8_C(0), UINT8_C(0), UINT8_C(0), UINT8_C(1), UINT8_C(1)};
    // 保存错误类型帧长度。
    size_t wrong_type_length = 0U;
    // 编码 Event 帧。
    TEST_ASSERT(
        ble_service_encode_message(
            (uint8_t)BLE_SERVICE_MESSAGE_EVENT,
            UINT8_C(0),
            UINT16_C(1),
            UINT32_C(1),
            control_like_payload,
            (uint16_t)sizeof(control_like_payload),
            request_frame,
            sizeof(request_frame),
            &wrong_type_length) == BLE_SERVICE_STATUS_OK,
        "错误类型测试帧编码失败。 ");
    // 控制路径处理 Event 帧。
    status = ble_service_process_control_frame(
        &connection,
        request_frame,
        wrong_type_length,
        UINT32_C(2),
        test_command_handler,
        &context,
        response_frame,
        sizeof(response_frame),
        &response_length,
        &from_cache);
    // 类型错误明确拒绝且不执行业务。
    TEST_ASSERT((status == BLE_SERVICE_STATUS_WRONG_MESSAGE_TYPE) && (context.execute_count == 0U), "错误消息类型进入业务。 ");
    // 构造只有 5 字节 payload 的 ControlRequest。
    size_t short_length = 0U;
    // 复用前五个测试字节。
    TEST_ASSERT(
        ble_service_encode_message(
            (uint8_t)BLE_SERVICE_MESSAGE_CONTROL_REQUEST,
            UINT8_C(0),
            UINT16_C(2),
            UINT32_C(2),
            control_like_payload,
            UINT16_C(5),
            request_frame,
            sizeof(request_frame),
            &short_length) == BLE_SERVICE_STATUS_OK,
        "短控制 payload 测试帧编码失败。 ");
    // 处理不足固定头的请求。
    status = ble_service_process_control_frame(
        &connection,
        request_frame,
        short_length,
        UINT32_C(3),
        test_command_handler,
        &context,
        response_frame,
        sizeof(response_frame),
        &response_length,
        &from_cache);
    // 长度错误明确拒绝。
    TEST_ASSERT((status == BLE_SERVICE_STATUS_BAD_CONTROL_PAYLOAD) && (context.execute_count == 0U), "短控制 payload 未被拒绝。 ");
}

// 验证最近 16 项缓存按环形顺序淘汰最旧请求。
static void test_cache_rotation(void)
{
    // 初始化连接。
    ble_service_connection_t connection;
    // 清空缓存和序号。
    ble_service_connection_reset(&connection);
    // 初始化业务计数。
    test_handler_context_t context = {UINT32_C(0), UINT32_C(0)};
    // 输入和输出帧使用协议最大固定缓冲区。
    uint8_t request_frame[IMU_BLE_MAX_FRAME_SIZE];
    // 保存响应帧。
    uint8_t response_frame[IMU_BLE_MAX_FRAME_SIZE];
    // 保存每次响应长度。
    size_t response_length = 0U;
    // 保存每次缓存命中标志。
    uint8_t from_cache = UINT8_C(0);
    // 连续执行 request_id 1～17，使第 17 个覆盖最旧的第 1 个。
    for (uint32_t request_id = UINT32_C(1); request_id <= UINT32_C(17); ++request_id) {
        // 构造当前开始会话测试请求。
        const size_t request_length = test_build_control_frame(
            request_id,
            (uint8_t)BLE_SERVICE_COMMAND_GET_SNAPSHOT,
            UINT8_C(1),
            NULL,
            0U,
            request_frame,
            sizeof(request_frame));
        // 处理新请求。
        const ble_service_status_t status = ble_service_process_control_frame(
            &connection,
            request_frame,
            request_length,
            request_id,
            test_command_handler,
            &context,
            response_frame,
            sizeof(response_frame),
            &response_length,
            &from_cache);
        // 每个新请求必须执行且不命中缓存。
        TEST_ASSERT((status == BLE_SERVICE_STATUS_OK) && (from_cache == 0U), "缓存轮转中的新请求状态错误。 ");
    }
    // 前 17 个新请求各执行一次。
    TEST_ASSERT(context.execute_count == UINT32_C(17), "缓存轮转执行次数错误。 ");
    // request_id 2 仍位于最近 16 项内。
    size_t request_length = test_build_control_frame(
        UINT32_C(2),
        (uint8_t)BLE_SERVICE_COMMAND_GET_SNAPSHOT,
        UINT8_C(1),
        NULL,
        0U,
        request_frame,
        sizeof(request_frame));
    // 重试 request_id 2。
    TEST_ASSERT(
        ble_service_process_control_frame(
            &connection,
            request_frame,
            request_length,
            UINT32_C(20),
            test_command_handler,
            &context,
            response_frame,
            sizeof(response_frame),
            &response_length,
            &from_cache) == BLE_SERVICE_STATUS_OK,
        "缓存内 request_id 2 处理失败。 ");
    // request_id 2 必须命中且不增加执行次数。
    TEST_ASSERT((from_cache == 1U) && (context.execute_count == UINT32_C(17)), "缓存内请求被重复执行。 ");
    // request_id 1 已被第 17 个请求淘汰。
    request_length = test_build_control_frame(
        UINT32_C(1),
        (uint8_t)BLE_SERVICE_COMMAND_GET_SNAPSHOT,
        UINT8_C(1),
        NULL,
        0U,
        request_frame,
        sizeof(request_frame));
    // 再次发送 request_id 1，应作为新请求执行。
    TEST_ASSERT(
        ble_service_process_control_frame(
            &connection,
            request_frame,
            request_length,
            UINT32_C(21),
            test_command_handler,
            &context,
            response_frame,
            sizeof(response_frame),
            &response_length,
            &from_cache) == BLE_SERVICE_STATUS_OK,
        "被淘汰 request_id 1 重新处理失败。 ");
    // 执行次数增加到 18 且不是缓存命中。
    TEST_ASSERT((from_cache == 0U) && (context.execute_count == UINT32_C(18)), "缓存没有按最近 16 项轮转。 ");
}

// 验证 MTU23 多分片重组、最后片 CRC 失败和断连复位。
static void test_fragmentation_and_disconnect_reset(void)
{
    // 初始化连接状态。
    ble_service_connection_t connection;
    // 建连前清空状态。
    ble_service_connection_reset(&connection);
    // 初始化业务处理器上下文。
    test_handler_context_t context = {UINT32_C(0), UINT32_C(10)};
    // 8 字节 TLV 确保 MTU23 下产生多个分片。
    const uint8_t tlv[8] = {
        UINT8_C(1), UINT8_C(2), UINT8_C(3), UINT8_C(4),
        UINT8_C(5), UINT8_C(6), UINT8_C(7), UINT8_C(8)
    };
    // 保存完整逻辑请求帧。
    uint8_t request_frame[IMU_BLE_MAX_FRAME_SIZE];
    // 构造请求。
    const size_t request_length = test_build_control_frame(
        UINT32_C(900),
        (uint8_t)BLE_SERVICE_COMMAND_SET_PREFERENCES,
        UINT8_C(1),
        tlv,
        sizeof(tlv),
        request_frame,
        sizeof(request_frame));
    // 查询 MTU23 所需分片数。
    uint16_t fragment_count = UINT16_C(0);
    // 分片计数必须成功。
    TEST_ASSERT(imu_ble_get_fragment_count(request_length, UINT16_C(23), &fragment_count) == IMU_BLE_STATUS_OK, "MTU23 分片计数失败。 ");
    // 本请求至少需要两片。
    TEST_ASSERT(fragment_count >= UINT16_C(2), "MTU23 测试请求没有产生多分片。 ");
    // 保存单个 ATT Value，最大使用 23-3=20 字节。
    uint8_t fragment[20];
    // 保存分片实际长度。
    size_t fragment_length = 0U;
    // 保存最终控制响应帧。
    uint8_t response_frame[IMU_BLE_MAX_FRAME_SIZE];
    // 保存响应长度。
    size_t response_length = 0U;
    // 保存缓存命中标志。
    uint8_t from_cache = UINT8_C(0);
    // 按严格升序发送全部分片。
    for (uint16_t index = UINT16_C(0); index < fragment_count; ++index) {
        // 编码当前分片包络和数据。
        TEST_ASSERT(
            imu_ble_encode_fragment(
                request_frame,
                request_length,
                UINT16_C(23),
                UINT16_C(77),
                index,
                fragment,
                sizeof(fragment),
                &fragment_length) == IMU_BLE_STATUS_OK,
            "控制请求分片编码失败。 ");
        // 推入生产控制重组路径。
        const ble_service_status_t status = ble_service_process_control_fragment(
            &connection,
            fragment,
            fragment_length,
            UINT32_C(5000),
            test_command_handler,
            &context,
            response_frame,
            sizeof(response_frame),
            &response_length,
            &from_cache);
        // 最后一片之前只能返回 INCOMPLETE 且不能执行业务。
        if ((uint16_t)(index + UINT16_C(1)) < fragment_count) {
            // 检查未完成状态和零执行次数。
            TEST_ASSERT((status == BLE_SERVICE_STATUS_INCOMPLETE) && (context.execute_count == 0U), "未收齐分片时提前执行业务。 ");
        } else {
            // 最后一片必须完成处理并生成响应。
            TEST_ASSERT((status == BLE_SERVICE_STATUS_OK) && (response_length > 0U), "最后分片没有生成控制响应。 ");
        }
    }
    // 完整多分片请求只执行一次。
    TEST_ASSERT(context.execute_count == UINT32_C(1), "多分片请求执行次数错误。 ");
    // 模拟连接断开并立即清空最近 16 项请求缓存。
    ble_service_connection_reset(&connection);
    // 同一完整请求在新连接上必须作为新请求重新执行。
    TEST_ASSERT(
        ble_service_process_control_frame(
            &connection,
            request_frame,
            request_length,
            UINT32_C(5500),
            test_command_handler,
            &context,
            response_frame,
            sizeof(response_frame),
            &response_length,
            &from_cache) == BLE_SERVICE_STATUS_OK,
        "断连后原 request_id 没有作为新请求执行。 ");
    // 新连接不命中旧缓存，业务执行次数增加到 2。
    TEST_ASSERT((from_cache == 0U) && (context.execute_count == UINT32_C(2)), "断连没有清空 request_id 缓存。 ");
    // 构造新请求并只推入第一片，模拟断连前半帧。
    const size_t second_request_length = test_build_control_frame(
        UINT32_C(901),
        (uint8_t)BLE_SERVICE_COMMAND_GET_SNAPSHOT,
        UINT8_C(1),
        tlv,
        sizeof(tlv),
        request_frame,
        sizeof(request_frame));
    // 查询第二请求分片数。
    TEST_ASSERT(imu_ble_get_fragment_count(second_request_length, UINT16_C(23), &fragment_count) == IMU_BLE_STATUS_OK, "第二请求分片计数失败。 ");
    // 编码第一片。
    TEST_ASSERT(
        imu_ble_encode_fragment(
            request_frame,
            second_request_length,
            UINT16_C(23),
            UINT16_C(78),
            UINT16_C(0),
            fragment,
            sizeof(fragment),
            &fragment_length) == IMU_BLE_STATUS_OK,
        "断连测试第一片编码失败。 ");
    // 推入第一片并等待后续。
    TEST_ASSERT(
        ble_service_process_control_fragment(
            &connection,
            fragment,
            fragment_length,
            UINT32_C(6000),
            test_command_handler,
            &context,
            response_frame,
            sizeof(response_frame),
            &response_length,
            &from_cache) == BLE_SERVICE_STATUS_INCOMPLETE,
        "断连测试第一片未进入等待状态。 ");
    // 模拟 BLE 断连，必须清空半帧和幂等缓存。
    ble_service_connection_reset(&connection);
    // 编码索引 1 的后续片。
    TEST_ASSERT(
        imu_ble_encode_fragment(
            request_frame,
            second_request_length,
            UINT16_C(23),
            UINT16_C(78),
            UINT16_C(1),
            fragment,
            sizeof(fragment),
            &fragment_length) == IMU_BLE_STATUS_OK,
        "断连测试第二片编码失败。 ");
    // 断连后直接收到索引 1 必须拒绝，不能接续旧半帧。
    TEST_ASSERT(
        ble_service_process_control_fragment(
            &connection,
            fragment,
            fragment_length,
            UINT32_C(6001),
            test_command_handler,
            &context,
            response_frame,
            sizeof(response_frame),
            &response_length,
            &from_cache) == BLE_SERVICE_STATUS_PROTOCOL_ERROR,
        "断连后旧后续分片未被拒绝。 ");
}

// 返回固定测试 Manifest 配置；所有指针指向静态只读数据，生命周期覆盖测试进程。
static ble_service_manifest_config_t test_manifest_config(void)
{
    // 类别顺序与生成模型 BP_CLASS_NAMES 完全一致，用于锁定生产 CRC32 黄金值。
    static const char *const class_names[] = {
        // 输出索引 0。
        "good_morning",
        // 输出索引 1。
        "jumping_jack",
        // 输出索引 2。
        "jumping_lunge",
        // 输出索引 3。
        "jumping_squat",
        // 输出索引 4。
        "lunge",
        // 输出索引 5。
        "sit",
        // 输出索引 6。
        "squat",
        // 输出索引 7。
        "trot",
        // 输出索引 8。
        "tuck_jump",
        // 输出索引 9。
        "walk",
        // 输出索引 10。
        "wave"
    };
    // 基础 SHA 依次包含 0x00～0x1F，便于逐字节验证十六进制解码顺序。
    static const char base_sha[] =
        "000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f";
    // 掩码 SHA 依次包含 0xFF～0xE0，验证大写无关且不发生整数端序翻转。
    static const char masked_sha[] =
        "fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0"
        "efeeedecebeae9e8e7e6e5e4e3e2e1e0";
    // 构造覆盖字符串、非对称整数、SHA、类别和能力位的完整配置。
    const ble_service_manifest_config_t config = {
        // 使用短设备 ID 便于核对不包含 NUL。
        .device_id = "A1B2C3D4E5F6",
        // 使用真实板卡修订格式。
        .board_revision = "2.06",
        // 主版本使用 1。
        .protocol_major = UINT8_C(1),
        // 次版本故意使用 2，避免只测零值。
        .protocol_minor = UINT8_C(2),
        // 固件版本使用短语义版本。
        .firmware_version = "1.2.3",
        // 使用非对称特征维度字节验证 u16LE。
        .feature_dimension = UINT16_C(0x1234),
        // 使用另一非对称版本验证 u16LE。
        .feature_version = UINT16_C(0xABCD),
        // 指向基础 SHA 文本。
        .base_model_sha256_hex = base_sha,
        // 指向掩码 SHA 文本。
        .masked_model_sha256_hex = masked_sha,
        // 类别数固定为 11。
        .class_count = UINT8_C(11),
        // 指向固定顺序类名表。
        .class_names = class_names,
        // 卡路里表版本用非对称字节验证 u16LE。
        .calorie_table_version = UINT16_C(0x2468),
        // 能力位使用四个不同字节验证 u32LE。
        .capabilities = UINT32_C(0x11223344),
        // LittleFS 可用字节使用八个不同字节验证 u64LE。
        .littlefs_available_bytes = UINT64_C(0x0102030405060708)
    };
    // 按值返回配置；内部指针仍引用静态数据。
    return config;
}

// 读取并验证当前 TLV 固定标签和长度，返回仅在 manifest 生命周期内有效的 value 指针。
static const uint8_t *test_manifest_expect_tlv(
    const uint8_t *manifest,
    const size_t manifest_length,
    size_t *offset,
    const uint8_t expected_tag,
    const uint8_t expected_length)
{
    // 三个输入指针都必须有效。
    TEST_ASSERT((manifest != NULL) && (offset != NULL), "Manifest TLV 测试输入为空。 ");
    // 当前偏移至少还需两字节 TLV 头。
    TEST_ASSERT(*offset <= manifest_length - 2U, "Manifest TLV 头被截断。 ");
    // 标签必须严格按 v1 固定顺序出现。
    TEST_ASSERT(manifest[*offset] == expected_tag, "Manifest TLV 标签或顺序错误。 ");
    // value 长度必须与字段合同完全一致。
    TEST_ASSERT(manifest[*offset + 1U] == expected_length, "Manifest TLV 长度错误。 ");
    // 保存 value 起点。
    const uint8_t *const value = &manifest[*offset + 2U];
    // value 不得超出完整 Manifest。
    TEST_ASSERT((size_t)expected_length <= manifest_length - *offset - 2U, "Manifest TLV value 被截断。 ");
    // 推进到下一个 TLV。
    *offset += 2U + (size_t)expected_length;
    // 返回当前值视图。
    return value;
}

// 验证类别 CRC 的规范字节串、顺序敏感性和空指针保护。
static void test_manifest_class_crc32(void)
{
    // 取得完整生产顺序配置。
    ble_service_manifest_config_t config = test_manifest_config();
    // 保存 CRC 输出。
    uint32_t crc32 = 0U;
    // 对 11 个类名计算 CRC-32/ISO-HDLC。
    TEST_ASSERT(
        ble_service_manifest_class_table_crc32(config.class_names, config.class_count, &crc32) ==
            BLE_SERVICE_STATUS_OK,
        "类别表 CRC32 计算失败。 ");
    // 黄金值基于 UTF-8 类名按顺序以单个 NUL 分隔、末尾不加 NUL。
    TEST_ASSERT(crc32 == UINT32_C(0xD8193927), "类别表 CRC32 与规范黄金值不一致。 ");
    // 使用两个互换顺序的名称证明类别索引变化会改变 CRC。
    static const char *const swapped_names[] = {"jumping_jack", "good_morning"};
    // 保存交换后的 CRC。
    uint32_t swapped_crc32 = 0U;
    // 计算交换表 CRC。
    TEST_ASSERT(
        ble_service_manifest_class_table_crc32(swapped_names, UINT8_C(2), &swapped_crc32) ==
            BLE_SERVICE_STATUS_OK,
        "交换类别表 CRC32 计算失败。 ");
    // 交换表不得碰巧等于完整生产表。
    TEST_ASSERT(swapped_crc32 != crc32, "类别表 CRC32 没有反映名称顺序。 ");
    // 空名称数组必须返回空指针错误。
    TEST_ASSERT(
        ble_service_manifest_class_table_crc32(NULL, UINT8_C(1), &crc32) ==
            BLE_SERVICE_STATUS_NULL_ARGUMENT,
        "空类别数组未被拒绝。 ");
    // 零类别必须返回范围错误。
    TEST_ASSERT(
        ble_service_manifest_class_table_crc32(config.class_names, UINT8_C(0), &crc32) ==
            BLE_SERVICE_STATUS_INVALID_ARGUMENT,
        "零类别未被拒绝。 ");
}

// 验证完整 Manifest 固定顺序、字符串、SHA、整数小端和最终长度。
static void test_manifest_complete_payload(void)
{
    // 构造固定完整配置。
    const ble_service_manifest_config_t config = test_manifest_config();
    // 512 字节容量等于 NimBLE Manifest 上限。
    uint8_t manifest[512U];
    // 先填充哨兵，确保构建器只写有效长度。
    (void)memset(manifest, 0xA5, sizeof(manifest));
    // 保存实际输出长度。
    size_t manifest_length = 0U;
    // 构建全部 11 个正式 TLV。
    TEST_ASSERT(
        ble_service_manifest_build(&config, manifest, sizeof(manifest), &manifest_length) ==
            BLE_SERVICE_STATUS_OK,
        "完整 Manifest 构建失败。 ");
    // 固定测试字符串下总长度应为 132 字节。
    TEST_ASSERT(manifest_length == 132U, "完整 Manifest 总长度错误。 ");
    // 从首 TLV 开始顺序解析。
    size_t offset = 0U;
    // 读取动态设备 ID。
    const uint8_t *value = test_manifest_expect_tlv(
        manifest, manifest_length, &offset, BLE_SERVICE_MANIFEST_TAG_DEVICE_ID, UINT8_C(12));
    // 字节必须严格等于字符串且不含结尾 NUL。
    TEST_ASSERT(memcmp(value, "A1B2C3D4E5F6", 12U) == 0, "设备 ID value 错误。 ");
    // 读取板卡修订号。
    value = test_manifest_expect_tlv(
        manifest, manifest_length, &offset, BLE_SERVICE_MANIFEST_TAG_BOARD_REVISION, UINT8_C(4));
    // 核对 UTF-8/ASCII 原字节。
    TEST_ASSERT(memcmp(value, "2.06", 4U) == 0, "板卡修订 value 错误。 ");
    // 读取两字节协议版本。
    value = test_manifest_expect_tlv(
        manifest, manifest_length, &offset, BLE_SERVICE_MANIFEST_TAG_PROTOCOL_VERSION, UINT8_C(2));
    // 版本顺序固定为 major、minor。
    TEST_ASSERT((value[0] == UINT8_C(1)) && (value[1] == UINT8_C(2)), "协议主次版本顺序错误。 ");
    // 读取固件语义版本。
    value = test_manifest_expect_tlv(
        manifest, manifest_length, &offset, BLE_SERVICE_MANIFEST_TAG_FIRMWARE_VERSION, UINT8_C(5));
    // 核对不含 NUL 的版本字节。
    TEST_ASSERT(memcmp(value, "1.2.3", 5U) == 0, "固件版本 value 错误。 ");
    // 读取特征描述。
    value = test_manifest_expect_tlv(
        manifest, manifest_length, &offset, BLE_SERVICE_MANIFEST_TAG_FEATURE_DESCRIPTOR, UINT8_C(4));
    // 两个 u16 字段都必须按小端恢复。
    TEST_ASSERT(
        (test_read_u16_le(&value[0]) == UINT16_C(0x1234)) &&
            (test_read_u16_le(&value[2]) == UINT16_C(0xABCD)),
        "特征维度或版本小端编码错误。 ");
    // 读取基础模型原始摘要。
    value = test_manifest_expect_tlv(
        manifest, manifest_length, &offset, BLE_SERVICE_MANIFEST_TAG_BASE_MODEL_SHA256, UINT8_C(32));
    // 核对 0x00～0x1F 顺序，SHA 摘要不得被整数端序翻转。
    for (uint8_t index = 0U; index < UINT8_C(32); ++index) {
        // 当前摘要字节必须等于索引。
        TEST_ASSERT(value[index] == index, "基础模型 SHA 解码顺序错误。 ");
    }
    // 读取掩码模型原始摘要。
    value = test_manifest_expect_tlv(
        manifest, manifest_length, &offset, BLE_SERVICE_MANIFEST_TAG_MASKED_MODEL_SHA256, UINT8_C(32));
    // 核对 0xFF～0xE0 顺序。
    for (uint8_t index = 0U; index < UINT8_C(32); ++index) {
        // 当前摘要字节必须等于 255-index。
        TEST_ASSERT(value[index] == (uint8_t)(UINT8_MAX - index), "掩码模型 SHA 解码顺序错误。 ");
    }
    // 读取类别数和 CRC32。
    value = test_manifest_expect_tlv(
        manifest, manifest_length, &offset, BLE_SERVICE_MANIFEST_TAG_CLASS_DESCRIPTOR, UINT8_C(5));
    // 第一个字节固定 11，后四字节为黄金 CRC 小端值。
    TEST_ASSERT(
        (value[0] == UINT8_C(11)) && (test_read_u32_le(&value[1]) == UINT32_C(0xD8193927)),
        "类别数或类别表 CRC32 错误。 ");
    // 读取卡路里表版本。
    value = test_manifest_expect_tlv(
        manifest, manifest_length, &offset, BLE_SERVICE_MANIFEST_TAG_CALORIE_TABLE_VERSION, UINT8_C(2));
    // 核对 u16LE。
    TEST_ASSERT(test_read_u16_le(value) == UINT16_C(0x2468), "卡路里表版本小端编码错误。 ");
    // 读取能力位。
    value = test_manifest_expect_tlv(
        manifest, manifest_length, &offset, BLE_SERVICE_MANIFEST_TAG_CAPABILITIES, UINT8_C(4));
    // 核对 u32LE。
    TEST_ASSERT(test_read_u32_le(value) == UINT32_C(0x11223344), "能力位小端编码错误。 ");
    // 读取 LittleFS 启动快照可用字节。
    value = test_manifest_expect_tlv(
        manifest, manifest_length, &offset, BLE_SERVICE_MANIFEST_TAG_LITTLEFS_AVAILABLE_BYTES, UINT8_C(8));
    // 核对 u64LE。
    TEST_ASSERT(
        test_read_u64_le(value) == UINT64_C(0x0102030405060708),
        "LittleFS 可用字节小端编码错误。 ");
    // 解析必须恰好消费整个 Manifest，不允许尾随垃圾。
    TEST_ASSERT(offset == manifest_length, "Manifest 存在未定义尾随字节。 ");
    // 有效长度之后的第一个哨兵必须保持不变。
    TEST_ASSERT(manifest[manifest_length] == UINT8_C(0xA5), "Manifest 写越过有效长度。 ");
    // 用刚得到的精确长度再次构建，验证 capacity==needed 成功。
    size_t exact_length = 0U;
    // 精确容量构建不得误判为不足。
    TEST_ASSERT(
        ble_service_manifest_build(&config, manifest, manifest_length, &exact_length) ==
            BLE_SERVICE_STATUS_OK,
        "Manifest 精确容量构建失败。 ");
    // 两次长度必须相同。
    TEST_ASSERT(exact_length == manifest_length, "Manifest 精确容量长度漂移。 ");
}

// 验证空指针、空字符串、超长字符串、非法 SHA 和容量不足均安全失败。
static void test_manifest_invalid_inputs(void)
{
    // 构造可修改配置副本。
    ble_service_manifest_config_t config = test_manifest_config();
    // 使用小缓冲区并填充哨兵，验证容量错误不产生部分输出。
    uint8_t small_output[16];
    // 全部字节设为固定哨兵。
    (void)memset(small_output, 0x5A, sizeof(small_output));
    // 使用非零旧值验证失败路径清零。
    size_t output_length = 99U;
    // 空配置必须拒绝并清零长度。
    TEST_ASSERT(
        ble_service_manifest_build(NULL, small_output, sizeof(small_output), &output_length) ==
            BLE_SERVICE_STATUS_NULL_ARGUMENT,
        "空 Manifest 配置未被拒绝。 ");
    // 长度必须清零。
    TEST_ASSERT(output_length == 0U, "空配置失败后长度未清零。 ");
    // 空输出缓冲区必须拒绝。
    TEST_ASSERT(
        ble_service_manifest_build(&config, NULL, sizeof(small_output), &output_length) ==
            BLE_SERVICE_STATUS_NULL_ARGUMENT,
        "空 Manifest 输出未被拒绝。 ");
    // 空长度指针必须拒绝且不得访问它。
    TEST_ASSERT(
        ble_service_manifest_build(&config, small_output, sizeof(small_output), NULL) ==
            BLE_SERVICE_STATUS_NULL_ARGUMENT,
        "空 Manifest 长度指针未被拒绝。 ");
    // 容量明显不足必须返回 BUFFER_TOO_SMALL。
    TEST_ASSERT(
        ble_service_manifest_build(&config, small_output, sizeof(small_output), &output_length) ==
            BLE_SERVICE_STATUS_BUFFER_TOO_SMALL,
        "Manifest 小容量未被拒绝。 ");
    // 容量错误后长度保持零。
    TEST_ASSERT(output_length == 0U, "Manifest 小容量失败后长度未清零。 ");
    // 检查全部哨兵，证明容量错误前没有写部分 TLV。
    for (size_t index = 0U; index < sizeof(small_output); ++index) {
        // 任一改变都表示非原子失败。
        TEST_ASSERT(small_output[index] == UINT8_C(0x5A), "Manifest 小容量路径修改了输出。 ");
    }
    // 空设备 ID 无法作为幂等主键。
    config.device_id = "";
    // 空字符串必须返回范围错误。
    TEST_ASSERT(
        ble_service_manifest_build(&config, small_output, sizeof(small_output), &output_length) ==
            BLE_SERVICE_STATUS_INVALID_ARGUMENT,
        "空设备 ID 未被拒绝。 ");
    // 恢复完整配置。
    config = test_manifest_config();
    // 构造含非法 g 字符的 64 字节摘要。
    config.base_model_sha256_hex =
        "g00102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f";
    // 非十六进制字符必须拒绝。
    TEST_ASSERT(
        ble_service_manifest_build(&config, small_output, sizeof(small_output), &output_length) ==
            BLE_SERVICE_STATUS_INVALID_ARGUMENT,
        "非法 SHA-256 字符未被拒绝。 ");
    // 恢复配置并改成 63 字符摘要。
    config = test_manifest_config();
    // 最后一个字符缺失。
    config.masked_model_sha256_hex =
        "fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0"
        "efeeedecebeae9e8e7e6e5e4e3e2e1e";
    // 截断摘要必须拒绝。
    TEST_ASSERT(
        ble_service_manifest_build(&config, small_output, sizeof(small_output), &output_length) ==
            BLE_SERVICE_STATUS_INVALID_ARGUMENT,
        "截断 SHA-256 未被拒绝。 ");
    // 构造 255 字节合法设备 ID。
    char max_device_id[256];
    // 前 255 字节填充 ASCII A。
    (void)memset(max_device_id, 'A', 255U);
    // 最后一字节写 NUL。
    max_device_id[255] = '\0';
    // 恢复配置并替换设备 ID。
    config = test_manifest_config();
    // 单字段长度 255 应合法。
    config.device_id = max_device_id;
    // 使用完整 512 字节输出。
    uint8_t full_output[512U];
    // 255 字节字段仍应构建成功。
    TEST_ASSERT(
        ble_service_manifest_build(&config, full_output, sizeof(full_output), &output_length) ==
            BLE_SERVICE_STATUS_OK,
        "255 字节 Manifest 字符串被错误拒绝。 ");
    // 构造 256 字节非法设备 ID。
    char too_long_device_id[257];
    // 前 256 字节填充 ASCII B。
    (void)memset(too_long_device_id, 'B', 256U);
    // 最后一字节写 NUL。
    too_long_device_id[256] = '\0';
    // 替换为超出 u8 length 的字段。
    config.device_id = too_long_device_id;
    // 256 字节字符串必须在任何输出前拒绝。
    TEST_ASSERT(
        ble_service_manifest_build(&config, full_output, sizeof(full_output), &output_length) ==
            BLE_SERVICE_STATUS_INVALID_ARGUMENT,
        "256 字节 Manifest 字符串未被拒绝。 ");
}

// 验证忘记电脑操作始终清除绑定，并正确处理活动连接与失败顺序。
static void test_forget_all_bonds_orchestration(void)
{
    // 初始化成功场景上下文；NimBLE 成功码固定为 0。
    test_bond_ops_context_t context = {0};
    // 配置可替换底层操作和返回码语义。
    const ble_service_bond_ops_t ops = {
        // 活动连接通过模拟 terminate 断开。
        .terminate = test_bond_terminate,
        // 所有场景通过模拟 ble_store_clear 删除绑定。
        .clear_store = test_bond_clear_store,
        // 两个回调共享计数上下文。
        .context = &context,
        // NimBLE API 返回 0 表示成功。
        .success_code = 0,
        // 测试使用 -7 表示连接已先行断开。
        .not_connected_code = -7,
    };
    // 无活动连接时只需清除官方绑定存储。
    TEST_ASSERT(
        ble_service_bond_forget_all(&ops, false, UINT16_C(0xFFFF)) ==
            BLE_SERVICE_BOND_FORGET_OK,
        "无连接忘记电脑失败。 ");
    // 无连接不得调用 terminate。
    TEST_ASSERT(context.terminate_calls == UINT32_C(0), "无连接仍调用了 terminate。 ");
    // 绑定存储必须清除一次。
    TEST_ASSERT(context.clear_calls == UINT32_C(1), "无连接未清除绑定存储。 ");

    // 清零调用计数，进入活动连接场景。
    (void)memset(&context, 0, sizeof(context));
    // 当前句柄 0x1234 必须先请求断开，再清除全部记录。
    TEST_ASSERT(
        ble_service_bond_forget_all(&ops, true, UINT16_C(0x1234)) ==
            BLE_SERVICE_BOND_FORGET_OK,
        "活动连接忘记电脑失败。 ");
    // 活动连接只断开一次。
    TEST_ASSERT(context.terminate_calls == UINT32_C(1), "活动连接断开次数错误。 ");
    // 断开句柄必须保持完整 16 位值。
    TEST_ASSERT(context.last_handle == UINT16_C(0x1234), "活动连接句柄错误。 ");
    // 断开后仍清除一次全部绑定对象。
    TEST_ASSERT(context.clear_calls == UINT32_C(1), "活动连接未清除绑定存储。 ");

    // 模拟连接刚好已由远端断开；该竞态不应阻止忘记电脑。
    (void)memset(&context, 0, sizeof(context));
    // 把 terminate 结果设为约定的 not-connected 码。
    context.terminate_result = -7;
    // 竞态断连仍应返回成功。
    TEST_ASSERT(
        ble_service_bond_forget_all(&ops, true, UINT16_C(9)) ==
            BLE_SERVICE_BOND_FORGET_OK,
        "连接竞态被错误判为失败。 ");
    // 存储仍必须清除。
    TEST_ASSERT(context.clear_calls == UINT32_C(1), "连接竞态未清除绑定存储。 ");

    // 模拟其它断连错误；实现仍必须尝试清除持久化密钥。
    (void)memset(&context, 0, sizeof(context));
    // 非成功且非 not-connected 的 -8 表示真实 GAP 错误。
    context.terminate_result = -8;
    // 最终结果应保留断连失败事实。
    TEST_ASSERT(
        ble_service_bond_forget_all(&ops, true, UINT16_C(10)) ==
            BLE_SERVICE_BOND_FORGET_ERR_TERMINATE,
        "断连错误未向上报告。 ");
    // 即使断连失败也应删除下次重连可用的绑定密钥。
    TEST_ASSERT(context.clear_calls == UINT32_C(1), "断连失败后跳过了绑定清除。 ");

    // 模拟官方存储清除失败。
    (void)memset(&context, 0, sizeof(context));
    // 非零存储结果表示 NVS 或 NimBLE store 回调失败。
    context.clear_result = -12;
    // 存储错误优先于已经成功的断连结果。
    TEST_ASSERT(
        ble_service_bond_forget_all(&ops, true, UINT16_C(11)) ==
            BLE_SERVICE_BOND_FORGET_ERR_STORE,
        "绑定存储失败未向上报告。 ");
    // 断开与清除均应各尝试一次。
    TEST_ASSERT(
        (context.terminate_calls == UINT32_C(1)) &&
        (context.clear_calls == UINT32_C(1)),
        "绑定存储失败场景调用次数错误。 ");

    // 空操作表必须拒绝，避免生产路径空函数指针。
    TEST_ASSERT(
        ble_service_bond_forget_all(NULL, false, UINT16_C(0)) ==
            BLE_SERVICE_BOND_FORGET_ERR_ARGUMENT,
        "空绑定操作表未被拒绝。 ");
}

// 主入口按固定顺序执行全部离线场景。
int main(void)
{
    // 验证九种消息类型范围。
    test_message_types();
    // 验证 30 字节实时状态编码。
    test_live_state_v1();
    // 验证28字节双M0推理诊断编码。
    test_inference_diagnostic_v1();
    // 验证事件固定布局和范围保护。
    test_event_v1();
    // 验证控制命令 1～11 和命令版本保护。
    test_command_ids_and_versions();
    // 验证正常、重复和冲突控制请求。
    test_control_idempotence_and_conflict();
    // 验证 CRC、类型和长度错误。
    test_invalid_frames();
    // 验证最近 16 项幂等缓存轮转。
    test_cache_rotation();
    // 验证分片重组和断连复位。
    test_fragmentation_and_disconnect_reset();
    // 验证类别表 CRC32 的规范输入和黄金值。
    test_manifest_class_crc32();
    // 验证完整 Manifest 的 11 个 TLV、端序和 SHA 原始字节。
    test_manifest_complete_payload();
    // 验证 Manifest 全部关键异常输入和原子失败行为。
    test_manifest_invalid_inputs();
    // 验证忘记电脑的断连、绑定清除和错误优先级。
    test_forget_all_bonds_orchestration();
    // 输出唯一成功标志与断言总数，供 PowerShell 脚本识别。
    (void)printf("BLE_SERVICE_TESTS_OK assertions=%u\n", g_assertion_count);
    // 返回 0 表示全部场景通过。
    return EXIT_SUCCESS;
}
