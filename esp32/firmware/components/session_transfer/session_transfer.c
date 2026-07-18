/* 实现会话同步固定载荷、分页快照和 BLE 回调适配。 */
#include "session_transfer.h"

/* memcpy/memset 用于固定缓冲区复制和清零。 */
#include <string.h>

/* TransferData data_kind=1 固定表示 SessionSummaryV1。 */
#define SESSION_TRANSFER_DATA_KIND_SUMMARY UINT8_C(1)
/* 64 字节摘要自身版本沿用 session_store v1。 */
#define SESSION_TRANSFER_SUMMARY_VERSION UINT16_C(1)
/* 稳定度 Q15 最大值由 fitness_core 使用有符号 Q15 0～32767。 */
#define SESSION_TRANSFER_STABILITY_Q15_MAX UINT16_C(32767)
/* 动作索引固定为 0～10。 */
#define SESSION_TRANSFER_ACTION_COUNT UINT8_C(11)
/* session_store 指标枚举固定为 0 次、1 步、2 持续毫秒。 */
#define SESSION_TRANSFER_METRIC_KIND_MAX UINT8_C(2)

/* 从小端字节读取 16 位整数。 */
static uint16_t session_transfer_read_u16_le(const uint8_t *data)
{
    /* 组合低字节和高字节，避免依赖主机端序或未对齐访问。 */
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

/* 从小端字节读取 32 位整数。 */
static uint32_t session_transfer_read_u32_le(const uint8_t *data)
{
    /* 每个字节先提升到 uint32_t，再按固定偏移组合。 */
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

/* 从小端字节读取 64 位整数。 */
static uint64_t session_transfer_read_u64_le(const uint8_t *data)
{
    /* 两个 32 位半字组合，避免平台无 64 位未对齐加载支持。 */
    return (uint64_t)session_transfer_read_u32_le(data) |
           ((uint64_t)session_transfer_read_u32_le(&data[4]) << 32U);
}

/* 按小端写入 16 位整数。 */
static void session_transfer_write_u16_le(uint8_t *data, uint16_t value)
{
    /* 低字节写在低偏移。 */
    data[0] = (uint8_t)(value & UINT16_C(0x00FF));
    /* 高字节写在后一偏移。 */
    data[1] = (uint8_t)((value >> 8U) & UINT16_C(0x00FF));
}

/* 按小端写入 32 位整数。 */
static void session_transfer_write_u32_le(uint8_t *data, uint32_t value)
{
    /* 依次写出四个字节。 */
    data[0] = (uint8_t)(value & UINT32_C(0xFF));
    data[1] = (uint8_t)((value >> 8U) & UINT32_C(0xFF));
    data[2] = (uint8_t)((value >> 16U) & UINT32_C(0xFF));
    data[3] = (uint8_t)((value >> 24U) & UINT32_C(0xFF));
}

/* 按小端写入 64 位整数。 */
static void session_transfer_write_u64_le(uint8_t *data, uint64_t value)
{
    /* 低 32 位写入前半。 */
    session_transfer_write_u32_le(data, (uint32_t)(value & UINT64_C(0xFFFFFFFF)));
    /* 高 32 位写入后半。 */
    session_transfer_write_u32_le(&data[4], (uint32_t)(value >> 32U));
}

/* 校验内存摘要能安全编码到 v1 线性格式。 */
static bool session_transfer_summary_is_valid(const session_summary_t *summary)
{
    /* 会话序号必须非零，枚举和 Q15 必须在固定线上范围内。 */
    return (summary != NULL) &&
           (summary->session_seq != UINT32_C(0)) &&
           (summary->action_id < SESSION_TRANSFER_ACTION_COUNT) &&
           (summary->metric_kind <= SESSION_TRANSFER_METRIC_KIND_MAX) &&
           (summary->average_stability_q15 <= SESSION_TRANSFER_STABILITY_Q15_MAX) &&
           (summary->minimum_stability_q15 <= SESSION_TRANSFER_STABILITY_Q15_MAX);
}

/* 把摘要编码为 session_store 相同的固定 64 字节格式。 */
static bool session_transfer_encode_summary(
    const session_summary_t *summary,
    uint8_t output[SESSION_STORE_SUMMARY_WIRE_SIZE])
{
    /* 无效字段不能发给 PC。 */
    if (!session_transfer_summary_is_valid(summary)) {
        /* 返回失败，调用者生成 STORAGE_ERROR。 */
        return false;
    }
    /* 先清零保留字节，保证未来兼容和逐字节测试确定。 */
    (void)memset(output, 0, SESSION_STORE_SUMMARY_WIRE_SIZE);
    /* 写摘要格式版本 1。 */
    session_transfer_write_u16_le(&output[0], SESSION_TRANSFER_SUMMARY_VERSION);
    /* 写固定长度 64。 */
    session_transfer_write_u16_le(&output[2], SESSION_STORE_SUMMARY_WIRE_SIZE);
    /* 写复合键中的 session_seq。 */
    session_transfer_write_u32_le(&output[4], summary->session_seq);
    /* 写设备端事件幂等水位。 */
    session_transfer_write_u32_le(&output[8], summary->last_event_seq);
    /* 写动作和指标枚举。 */
    output[12] = summary->action_id;
    output[13] = summary->metric_kind;
    /* 写结束原因、质量和时间有效位。 */
    session_transfer_write_u16_le(&output[14], summary->flags);
    /* 写 UTC Unix 毫秒；0 表示尚未校时。 */
    session_transfer_write_u64_le(&output[16], summary->start_unix_ms);
    /* 写单调持续毫秒。 */
    session_transfer_write_u64_le(&output[24], summary->duration_ms);
    /* 写次数、步数或持续毫秒总值。 */
    session_transfer_write_u64_le(&output[32], summary->metric_total);
    /* 写毛热量 microkcal。 */
    session_transfer_write_u64_le(&output[40], summary->gross_microkcal);
    /* 写活动热量 microkcal。 */
    session_transfer_write_u64_le(&output[48], summary->active_microkcal);
    /* 写平均和最低稳定度 Q15。 */
    session_transfer_write_u16_le(&output[56], summary->average_stability_q15);
    session_transfer_write_u16_le(&output[58], summary->minimum_stability_q15);
    /* 写摘要吸收的事件数量。 */
    session_transfer_write_u32_le(&output[60], summary->event_count);
    /* 编码成功。 */
    return true;
}

/* 解码并验证固定 64 字节摘要。 */
static bool session_transfer_decode_summary(
    const uint8_t input[SESSION_STORE_SUMMARY_WIRE_SIZE],
    session_summary_t *summary)
{
    /* 版本和长度不匹配时禁止猜测旧/新格式。 */
    if ((session_transfer_read_u16_le(&input[0]) != SESSION_TRANSFER_SUMMARY_VERSION) ||
        (session_transfer_read_u16_le(&input[2]) != SESSION_STORE_SUMMARY_WIRE_SIZE)) {
        /* 返回失败。 */
        return false;
    }
    /* 清零结构填充并逐字段读取。 */
    (void)memset(summary, 0, sizeof(*summary));
    /* 读取主键和幂等水位。 */
    summary->session_seq = session_transfer_read_u32_le(&input[4]);
    summary->last_event_seq = session_transfer_read_u32_le(&input[8]);
    /* 读取动作、指标和标志。 */
    summary->action_id = input[12];
    summary->metric_kind = input[13];
    summary->flags = session_transfer_read_u16_le(&input[14]);
    /* 读取墙钟、持续时间、指标与能量。 */
    summary->start_unix_ms = session_transfer_read_u64_le(&input[16]);
    summary->duration_ms = session_transfer_read_u64_le(&input[24]);
    summary->metric_total = session_transfer_read_u64_le(&input[32]);
    summary->gross_microkcal = session_transfer_read_u64_le(&input[40]);
    summary->active_microkcal = session_transfer_read_u64_le(&input[48]);
    /* 读取稳定度与事件数。 */
    summary->average_stability_q15 = session_transfer_read_u16_le(&input[56]);
    summary->minimum_stability_q15 = session_transfer_read_u16_le(&input[58]);
    summary->event_count = session_transfer_read_u32_le(&input[60]);
    /* 返回字段级校验结果。 */
    return session_transfer_summary_is_valid(summary);
}

bool session_transfer_decode_request_v1(
    const uint8_t *input,
    const size_t input_length,
    session_transfer_request_v1_t *request)
{
    /* 指针和长度必须严格匹配固定合同。 */
    if ((input == NULL) || (request == NULL) || (input_length != SESSION_TRANSFER_REQUEST_SIZE)) {
        /* 拒绝截断和尾随垃圾。 */
        return false;
    }
    /* 逐字段解码，不读取结构体填充。 */
    request->version = input[0];
    request->operation = input[1];
    request->page_size = session_transfer_read_u16_le(&input[2]);
    request->request_id = session_transfer_read_u32_le(&input[4]);
    request->cursor_session_seq = session_transfer_read_u32_le(&input[8]);
    /* 仅做结构解码；业务错误由响应 status 精确报告。 */
    return true;
}

bool session_transfer_encode_response_v1(
    const session_transfer_response_v1_t *response,
    uint8_t *output,
    const size_t output_capacity,
    size_t *output_length)
{
    /* 必填指针和容量必须有效。 */
    if ((response == NULL) || (output == NULL) || (output_length == NULL) ||
        (output_capacity < SESSION_TRANSFER_RESPONSE_SIZE)) {
        /* 不写部分响应。 */
        return false;
    }
    /* 版本、操作、状态、标志和数量必须在 v1 范围。 */
    if ((response->version != SESSION_TRANSFER_VERSION) ||
        (response->operation < SESSION_TRANSFER_OPERATION_LIST) ||
        (response->operation > SESSION_TRANSFER_OPERATION_GET) ||
        (response->status > SESSION_TRANSFER_RESPONSE_REQUEST_CONFLICT) ||
        ((response->flags & (uint8_t)~(SESSION_TRANSFER_FLAG_HAS_DATA | SESSION_TRANSFER_FLAG_END)) != 0U) ||
        (response->total_count > SESSION_STORE_MAX_SUMMARIES) ||
        (response->item_count > SESSION_TRANSFER_MAX_PAGE_SIZE)) {
        /* 拒绝产生 PC 无法验证的字段。 */
        return false;
    }
    /* 写固定头。 */
    output[0] = response->version;
    output[1] = response->operation;
    output[2] = response->status;
    output[3] = response->flags;
    /* 写请求号和下一游标。 */
    session_transfer_write_u32_le(&output[4], response->request_id);
    session_transfer_write_u32_le(&output[8], response->next_cursor_session_seq);
    /* 写设备总数和本页数量。 */
    session_transfer_write_u16_le(&output[12], response->total_count);
    session_transfer_write_u16_le(&output[14], response->item_count);
    /* 返回固定长度。 */
    *output_length = SESSION_TRANSFER_RESPONSE_SIZE;
    /* 编码成功。 */
    return true;
}

bool session_transfer_encode_data_v1(
    const session_transfer_data_v1_t *data,
    uint8_t *output,
    const size_t output_capacity,
    size_t *output_length)
{
    /* 必填对象和容量必须有效。 */
    if ((data == NULL) || (output == NULL) || (output_length == NULL) ||
        (output_capacity < SESSION_TRANSFER_DATA_SIZE)) {
        /* 不写截断数据。 */
        return false;
    }
    /* 固定版本、类型、保留位、索引和数量必须自洽。 */
    if ((data->version != SESSION_TRANSFER_VERSION) ||
        (data->data_kind != SESSION_TRANSFER_DATA_KIND_SUMMARY) ||
        ((data->flags & (uint8_t)~(SESSION_TRANSFER_DATA_FLAG_LAST_IN_PAGE | SESSION_TRANSFER_DATA_FLAG_END)) != 0U) ||
        (data->reserved != 0U) ||
        (data->reserved2 != 0U) ||
        (data->request_id == UINT32_C(0)) ||
        (data->item_count == UINT16_C(0)) ||
        (data->item_count > SESSION_TRANSFER_MAX_PAGE_SIZE) ||
        (data->item_index >= data->item_count) ||
        (data->total_count > SESSION_STORE_MAX_SUMMARIES)) {
        /* 拒绝非法页内位置。 */
        return false;
    }
    /* 写版本、数据类型、标志和固定 0 保留字节。 */
    output[0] = data->version;
    output[1] = data->data_kind;
    output[2] = data->flags;
    output[3] = UINT8_C(0);
    /* 写请求号、页内索引、页大小和设备总数。 */
    session_transfer_write_u32_le(&output[4], data->request_id);
    session_transfer_write_u16_le(&output[8], data->item_index);
    session_transfer_write_u16_le(&output[10], data->item_count);
    session_transfer_write_u16_le(&output[12], data->total_count);
    session_transfer_write_u16_le(&output[14], UINT16_C(0));
    /* 编码固定 64 字节摘要。 */
    if (!session_transfer_encode_summary(&data->summary, &output[SESSION_TRANSFER_DATA_HEADER_SIZE])) {
        /* 摘要字段非法时返回失败。 */
        return false;
    }
    /* 返回固定长度。 */
    *output_length = SESSION_TRANSFER_DATA_SIZE;
    /* 编码成功。 */
    return true;
}

bool session_transfer_decode_data_v1(
    const uint8_t *input,
    const size_t input_length,
    session_transfer_data_v1_t *data)
{
    /* 长度必须严格为 80 字节。 */
    if ((input == NULL) || (data == NULL) || (input_length != SESSION_TRANSFER_DATA_SIZE)) {
        /* 拒绝截断或尾随垃圾。 */
        return false;
    }
    /* 清零输出并读取固定头。 */
    (void)memset(data, 0, sizeof(*data));
    data->version = input[0];
    data->data_kind = input[1];
    data->flags = input[2];
    data->reserved = input[3];
    data->request_id = session_transfer_read_u32_le(&input[4]);
    data->item_index = session_transfer_read_u16_le(&input[8]);
    data->item_count = session_transfer_read_u16_le(&input[10]);
    data->total_count = session_transfer_read_u16_le(&input[12]);
    data->reserved2 = session_transfer_read_u16_le(&input[14]);
    /* 校验头字段，防止错误数组索引和请求关联。 */
    if ((data->version != SESSION_TRANSFER_VERSION) ||
        (data->data_kind != SESSION_TRANSFER_DATA_KIND_SUMMARY) ||
        ((data->flags & (uint8_t)~(SESSION_TRANSFER_DATA_FLAG_LAST_IN_PAGE | SESSION_TRANSFER_DATA_FLAG_END)) != 0U) ||
        (data->reserved != 0U) ||
        (data->reserved2 != 0U) ||
        (data->request_id == UINT32_C(0)) ||
        (data->item_count == UINT16_C(0)) ||
        (data->item_count > SESSION_TRANSFER_MAX_PAGE_SIZE) ||
        (data->item_index >= data->item_count) ||
        (data->total_count > SESSION_STORE_MAX_SUMMARIES)) {
        /* 非法头不得进入业务层。 */
        return false;
    }
    /* 解码并验证摘要。 */
    return session_transfer_decode_summary(&input[SESSION_TRANSFER_DATA_HEADER_SIZE], &data->summary);
}

bool session_transfer_service_init(
    session_transfer_service_t *service,
    const session_store_t *store)
{
    /* 服务与仓储都必须存在，仓储必须完成双槽恢复。 */
    if ((service == NULL) || (store == NULL) || !store->initialized) {
        /* 拒绝未就绪存储。 */
        return false;
    }
    /* 清空全部重放和队列状态。 */
    (void)memset(service, 0, sizeof(*service));
    /* 保存只读仓储指针。 */
    service->store = store;
    /* 初始化成功。 */
    return true;
}

/* 在固定仓储中按 session_seq 查找摘要。 */
static bool session_transfer_find_summary(
    const session_store_t *store,
    const uint32_t session_seq,
    session_summary_t *summary)
{
    /* 从最新到最旧扫描最多 200 条，优先返回最新同序号记录。 */
    const size_t count = session_store_count(store);
    /* newest_index=0 表示最新。 */
    for (size_t newest_index = 0U; newest_index < count; ++newest_index) {
        /* 读取当前值拷贝。 */
        session_summary_t candidate;
        /* 仓储读取异常时结束查找。 */
        if (session_store_get_recent(store, newest_index, &candidate) != SESSION_STORE_STATUS_OK) {
            /* 返回未找到/读取失败。 */
            return false;
        }
        /* 主键匹配时复制输出。 */
        if (candidate.session_seq == session_seq) {
            /* 输出独立摘要副本。 */
            *summary = candidate;
            /* 查找成功。 */
            return true;
        }
    }
    /* 所有记录均不匹配。 */
    return false;
}

/* 编码统一响应并保存为重放快照。 */
static ble_service_status_t session_transfer_finish_response(
    session_transfer_service_t *service,
    const session_transfer_response_v1_t *response,
    uint8_t *response_payload,
    const size_t response_capacity,
    uint16_t *response_length)
{
    /* 编码函数使用 size_t 长度。 */
    size_t encoded_length = 0U;
    /* 编码固定响应。 */
    if (!session_transfer_encode_response_v1(
            response,
            response_payload,
            response_capacity,
            &encoded_length)) {
        /* 容量不足或字段非法映射为 BLE 业务错误。 */
        return (response_capacity < SESSION_TRANSFER_RESPONSE_SIZE)
                   ? BLE_SERVICE_STATUS_BUFFER_TOO_SMALL
                   : BLE_SERVICE_STATUS_HANDLER_ERROR;
    }
    /* 固定 16 字节可安全收窄为 uint16_t。 */
    *response_length = (uint16_t)encoded_length;
    /* 保存响应用于同请求重放。 */
    (void)memcpy(service->last_response, response_payload, SESSION_TRANSFER_RESPONSE_SIZE);
    /* 返回成功，线上业务 status 已包含 NOT_FOUND/BUSY 等结果。 */
    return BLE_SERVICE_STATUS_OK;
}

ble_service_status_t session_transfer_service_handle_request(
    const uint8_t *request_payload,
    const uint16_t request_length,
    uint8_t *response_payload,
    const size_t response_capacity,
    uint16_t *response_length,
    void *context)
{
    /* 回调上下文必须是已初始化服务。 */
    session_transfer_service_t *const service = (session_transfer_service_t *)context;
    /* 所有必填对象必须存在。 */
    if ((service == NULL) || (service->store == NULL) || (request_payload == NULL) ||
        (response_payload == NULL) || (response_length == NULL)) {
        /* 不访问空指针。 */
        return BLE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    /* 默认没有响应长度，失败路径不留下旧长度。 */
    *response_length = UINT16_C(0);
    /* 请求固定为 12 字节。 */
    session_transfer_request_v1_t request;
    /* 结构长度错误由 BLE 服务映射为坏业务 payload。 */
    if (!session_transfer_decode_request_v1(request_payload, request_length, &request)) {
        /* 拒绝截断或尾随字节。 */
        return BLE_SERVICE_STATUS_BAD_CONTROL_PAYLOAD;
    }
    /* 同 request_id、完全相同字节时重放响应和全部数据。 */
    if (service->last_request_valid &&
        (session_transfer_read_u32_le(&service->last_request[4]) == request.request_id)) {
        /* 同 ID 不同字节为幂等冲突，不能执行新查询。 */
        if (memcmp(service->last_request, request_payload, SESSION_TRANSFER_REQUEST_SIZE) != 0) {
            /* 构造稳定冲突响应。 */
            const session_transfer_response_v1_t conflict = {
                SESSION_TRANSFER_VERSION,
                request.operation,
                SESSION_TRANSFER_RESPONSE_REQUEST_CONFLICT,
                SESSION_TRANSFER_FLAG_END,
                request.request_id,
                request.cursor_session_seq,
                (uint16_t)session_store_count(service->store),
                UINT16_C(0)};
            /* 返回线上冲突，不覆盖最近成功页。 */
            size_t conflict_length = 0U;
            /* 直接编码到调用方缓冲区。 */
            if (!session_transfer_encode_response_v1(
                    &conflict,
                    response_payload,
                    response_capacity,
                    &conflict_length)) {
                /* 容量不足。 */
                return BLE_SERVICE_STATUS_BUFFER_TOO_SMALL;
            }
            /* 写固定长度。 */
            *response_length = (uint16_t)conflict_length;
            /* 逻辑处理成功。 */
            return BLE_SERVICE_STATUS_OK;
        }
        /* 响应缓冲区必须容纳最近响应。 */
        if (response_capacity < SESSION_TRANSFER_RESPONSE_SIZE) {
            /* 不复制截断响应。 */
            return BLE_SERVICE_STATUS_BUFFER_TOO_SMALL;
        }
        /* 重放相同响应。 */
        (void)memcpy(response_payload, service->last_response, SESSION_TRANSFER_RESPONSE_SIZE);
        /* 从页首重新允许数据通知；PC 用 session_seq 幂等去重。 */
        service->pending_index = UINT16_C(0);
        /* 返回固定长度。 */
        *response_length = (uint16_t)SESSION_TRANSFER_RESPONSE_SIZE;
        /* 重放成功。 */
        return BLE_SERVICE_STATUS_OK;
    }
    /* 上一页仍未取完时拒绝新请求，防止覆盖通知缓冲区。 */
    if (service->pending_index < service->last_data_count) {
        /* 构造 BUSY 响应，不改变最近成功页和重放快照。 */
        const session_transfer_response_v1_t busy = {
            SESSION_TRANSFER_VERSION,
            request.operation,
            SESSION_TRANSFER_RESPONSE_BUSY,
            SESSION_TRANSFER_FLAG_END,
            request.request_id,
            request.cursor_session_seq,
            (uint16_t)session_store_count(service->store),
            UINT16_C(0)};
        /* 编码临时响应。 */
        size_t busy_length = 0U;
        /* 容量必须足够。 */
        if (!session_transfer_encode_response_v1(
                &busy,
                response_payload,
                response_capacity,
                &busy_length)) {
            /* 返回容量错误。 */
            return BLE_SERVICE_STATUS_BUFFER_TOO_SMALL;
        }
        /* 写响应长度。 */
        *response_length = (uint16_t)busy_length;
        /* 业务已明确拒绝。 */
        return BLE_SERVICE_STATUS_OK;
    }
    /* 建立新请求快照并清空旧页数量。 */
    (void)memcpy(service->last_request, request_payload, SESSION_TRANSFER_REQUEST_SIZE);
    service->last_request_valid = true;
    service->last_data_count = UINT16_C(0);
    service->pending_index = UINT16_C(0);
    /* 初始化响应公共字段。 */
    session_transfer_response_v1_t response = {
        SESSION_TRANSFER_VERSION,
        request.operation,
        SESSION_TRANSFER_RESPONSE_OK,
        UINT8_C(0),
        request.request_id,
        request.cursor_session_seq,
        (uint16_t)session_store_count(service->store),
        UINT16_C(0)};
    /* 版本错误只返回状态，不读取操作参数。 */
    if (request.version != SESSION_TRANSFER_VERSION) {
        /* 标记不支持版本并结束。 */
        response.status = SESSION_TRANSFER_RESPONSE_UNSUPPORTED_VERSION;
        response.flags = SESSION_TRANSFER_FLAG_END;
        /* 保存并返回响应。 */
        return session_transfer_finish_response(
            service,
            &response,
            response_payload,
            response_capacity,
            response_length);
    }
    /* 操作码必须是 LIST 或 GET。 */
    if ((request.operation != SESSION_TRANSFER_OPERATION_LIST) &&
        (request.operation != SESSION_TRANSFER_OPERATION_GET)) {
        /* 标记非法操作。 */
        response.status = SESSION_TRANSFER_RESPONSE_INVALID_OPERATION;
        response.flags = SESSION_TRANSFER_FLAG_END;
        /* operation 字段线上只能编码 1～2，未知操作回显为 LIST 以保持响应可解码。 */
        response.operation = SESSION_TRANSFER_OPERATION_LIST;
        /* 返回稳定响应。 */
        return session_transfer_finish_response(
            service,
            &response,
            response_payload,
            response_capacity,
            response_length);
    }
    /* request_id 必须非零。 */
    if (request.request_id == UINT32_C(0)) {
        /* 标记无效请求。 */
        response.status = SESSION_TRANSFER_RESPONSE_INVALID_REQUEST;
        response.flags = SESSION_TRANSFER_FLAG_END;
        /* 返回错误响应。 */
        return session_transfer_finish_response(
            service,
            &response,
            response_payload,
            response_capacity,
            response_length);
    }
    /* GET 精确读取一条摘要。 */
    if (request.operation == SESSION_TRANSFER_OPERATION_GET) {
        /* GET 不接受页大小且目标序号必须非零。 */
        if ((request.page_size != UINT16_C(0)) || (request.cursor_session_seq == UINT32_C(0))) {
            /* 标记字段错误。 */
            response.status = SESSION_TRANSFER_RESPONSE_INVALID_REQUEST;
            response.flags = SESSION_TRANSFER_FLAG_END;
            /* 返回无数据响应。 */
            return session_transfer_finish_response(
                service,
                &response,
                response_payload,
                response_capacity,
                response_length);
        }
        /* 查找目标摘要。 */
        session_summary_t summary;
        /* 不存在时返回 NOT_FOUND。 */
        if (!session_transfer_find_summary(service->store, request.cursor_session_seq, &summary)) {
            /* 设置未找到状态。 */
            response.status = SESSION_TRANSFER_RESPONSE_NOT_FOUND;
            response.flags = SESSION_TRANSFER_FLAG_END;
            /* 返回无数据响应。 */
            return session_transfer_finish_response(
                service,
                &response,
                response_payload,
                response_capacity,
                response_length);
        }
        /* 构造唯一数据帧。 */
        const session_transfer_data_v1_t data = {
            SESSION_TRANSFER_VERSION,
            SESSION_TRANSFER_DATA_KIND_SUMMARY,
            SESSION_TRANSFER_DATA_FLAG_LAST_IN_PAGE | SESSION_TRANSFER_DATA_FLAG_END,
            UINT8_C(0),
            request.request_id,
            UINT16_C(0),
            UINT16_C(1),
            response.total_count,
            UINT16_C(0),
            summary};
        /* 编码到冻结队列。 */
        size_t data_length = 0U;
        /* 摘要非法或编码失败映射存储错误。 */
        if (!session_transfer_encode_data_v1(
                &data,
                service->last_data[0],
                SESSION_TRANSFER_DATA_SIZE,
                &data_length)) {
            /* 标记存储数据不可导出。 */
            response.status = SESSION_TRANSFER_RESPONSE_STORAGE_ERROR;
            response.flags = SESSION_TRANSFER_FLAG_END;
            /* 返回无数据响应。 */
            return session_transfer_finish_response(
                service,
                &response,
                response_payload,
                response_capacity,
                response_length);
        }
        /* 队列包含一条。 */
        service->last_data_count = UINT16_C(1);
        /* 响应声明有数据并结束。 */
        response.flags = SESSION_TRANSFER_FLAG_HAS_DATA | SESSION_TRANSFER_FLAG_END;
        response.item_count = UINT16_C(1);
        response.next_cursor_session_seq = summary.session_seq;
        /* 保存并返回响应。 */
        return session_transfer_finish_response(
            service,
            &response,
            response_payload,
            response_capacity,
            response_length);
    }
    /* LIST 页大小限制为 1～12。 */
    if ((request.page_size == UINT16_C(0)) ||
        (request.page_size > SESSION_TRANSFER_MAX_PAGE_SIZE)) {
        /* 标记字段错误。 */
        response.status = SESSION_TRANSFER_RESPONSE_INVALID_REQUEST;
        response.flags = SESSION_TRANSFER_FLAG_END;
        /* 返回无数据响应。 */
        return session_transfer_finish_response(
            service,
            &response,
            response_payload,
            response_capacity,
            response_length);
    }
    /* eligible_count 统计 cursor 之后的全部记录，用于 END 判断。 */
    uint16_t eligible_count = UINT16_C(0);
    /* 仓储总数固定不超过 200。 */
    const size_t store_count = session_store_count(service->store);
    /* 从最旧到最新遍历：newest_index=count-1 是最旧。 */
    for (size_t oldest_offset = 0U; oldest_offset < store_count; ++oldest_offset) {
        /* 把旧到新位置转换为 newest_index。 */
        const size_t newest_index = store_count - UINT16_C(1) - oldest_offset;
        /* 读取摘要值副本。 */
        session_summary_t summary;
        /* 任意索引读取失败都返回存储错误，不能输出不完整页。 */
        if (session_store_get_recent(service->store, newest_index, &summary) != SESSION_STORE_STATUS_OK) {
            /* 清空已构建队列。 */
            service->last_data_count = UINT16_C(0);
            /* 标记存储失败。 */
            response.status = SESSION_TRANSFER_RESPONSE_STORAGE_ERROR;
            response.flags = SESSION_TRANSFER_FLAG_END;
            /* 返回错误。 */
            return session_transfer_finish_response(
                service,
                &response,
                response_payload,
                response_capacity,
                response_length);
        }
        /* 旧记录或已保存游标不需要再次传输。 */
        if (summary.session_seq <= request.cursor_session_seq) {
            /* 继续检查更新记录。 */
            continue;
        }
        /* 累计符合游标的总数。 */
        eligible_count = (uint16_t)(eligible_count + UINT16_C(1));
        /* 页已满时只继续计数，不再编码队列。 */
        if (service->last_data_count >= request.page_size) {
            /* 继续以便准确判断 END。 */
            continue;
        }
        /* 当前页索引等于已有数量。 */
        const uint16_t item_index = service->last_data_count;
        /* flags 在全部条目选完后再修正最后一条。 */
        const session_transfer_data_v1_t data = {
            SESSION_TRANSFER_VERSION,
            SESSION_TRANSFER_DATA_KIND_SUMMARY,
            UINT8_C(0),
            UINT8_C(0),
            request.request_id,
            item_index,
            request.page_size,
            response.total_count,
            UINT16_C(0),
            summary};
        /* 编码当前冻结摘要。 */
        size_t data_length = 0U;
        /* 非法摘要使整页失败。 */
        if (!session_transfer_encode_data_v1(
                &data,
                service->last_data[item_index],
                SESSION_TRANSFER_DATA_SIZE,
                &data_length)) {
            /* 清空页并返回存储错误。 */
            service->last_data_count = UINT16_C(0);
            response.status = SESSION_TRANSFER_RESPONSE_STORAGE_ERROR;
            response.flags = SESSION_TRANSFER_FLAG_END;
            /* 返回错误响应。 */
            return session_transfer_finish_response(
                service,
                &response,
                response_payload,
                response_capacity,
                response_length);
        }
        /* 增加页内数量。 */
        service->last_data_count = (uint16_t)(service->last_data_count + UINT16_C(1));
        /* 下一游标始终指向当前页最后序号。 */
        response.next_cursor_session_seq = summary.session_seq;
    }
    /* 实际页数量可能小于请求上限，修正每条 item_count 和最后标志。 */
    response.item_count = service->last_data_count;
    /* 当前页覆盖全部符合项时到达终点。 */
    const bool end_reached = eligible_count <= service->last_data_count;
    /* 非空页声明 HAS_DATA。 */
    if (service->last_data_count > UINT16_C(0)) {
        /* 设置响应有数据位。 */
        response.flags |= SESSION_TRANSFER_FLAG_HAS_DATA;
        /* 遍历页内数据，修正 item_count 和最后 flags。 */
        for (uint16_t index = UINT16_C(0); index < service->last_data_count; ++index) {
            /* 直接修改固定头中的 item_count。 */
            session_transfer_write_u16_le(&service->last_data[index][10], service->last_data_count);
            /* 最后一条标记 LAST；其它条保持 0。 */
            if (index == (uint16_t)(service->last_data_count - UINT16_C(1))) {
                /* 最后一帧至少设置页尾位。 */
                service->last_data[index][2] = SESSION_TRANSFER_DATA_FLAG_LAST_IN_PAGE;
                /* 同时结束同步时加入 END 位。 */
                if (end_reached) {
                    /* 标记全量游标已追平。 */
                    service->last_data[index][2] |= SESSION_TRANSFER_DATA_FLAG_END;
                }
            }
        }
    }
    /* 空页或没有更多记录时设置 END。 */
    if (end_reached) {
        /* PC 不再请求下一页。 */
        response.flags |= SESSION_TRANSFER_FLAG_END;
    }
    /* 保存并返回响应。 */
    return session_transfer_finish_response(
        service,
        &response,
        response_payload,
        response_capacity,
        response_length);
}

bool session_transfer_service_pop_data(
    session_transfer_service_t *service,
    uint8_t *output,
    const size_t output_capacity,
    size_t *output_length)
{
    /* 必填对象与容量必须有效。 */
    if ((service == NULL) || (output == NULL) || (output_length == NULL) ||
        (output_capacity < SESSION_TRANSFER_DATA_SIZE)) {
        /* 失败不写输出。 */
        return false;
    }
    /* 队列为空时返回长度 0。 */
    if (service->pending_index >= service->last_data_count) {
        /* 明确无输出。 */
        *output_length = 0U;
        /* 返回 false 表示无需通知。 */
        return false;
    }
    /* 复制下一条固定 80 字节 payload。 */
    (void)memcpy(output, service->last_data[service->pending_index], SESSION_TRANSFER_DATA_SIZE);
    /* 前移队列索引。 */
    service->pending_index = (uint16_t)(service->pending_index + UINT16_C(1));
    /* 返回固定长度。 */
    *output_length = SESSION_TRANSFER_DATA_SIZE;
    /* 取出成功。 */
    return true;
}

size_t session_transfer_service_pending_count(const session_transfer_service_t *service)
{
    /* 空对象或索引越界时没有待发送数据。 */
    if ((service == NULL) || (service->pending_index >= service->last_data_count)) {
        /* 返回 0。 */
        return 0U;
    }
    /* 返回固定队列剩余数量。 */
    return (size_t)(service->last_data_count - service->pending_index);
}
