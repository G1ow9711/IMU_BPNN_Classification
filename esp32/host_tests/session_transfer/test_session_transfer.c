/* 引入待测会话同步公开合同。 */
#include "session_transfer.h"

/* printf 输出唯一成功标志和失败位置。 */
#include <stdio.h>
/* calloc/free 创建项目测试进程内双槽内存介质。 */
#include <stdlib.h>
/* memcmp/memcpy/memset 检查固定字节向量。 */
#include <string.h>

/* assertion_count 记录实际执行断言数，防止测试空跑。 */
static unsigned int assertion_count = 0U;

/* 断言失败时输出中文原因和源码行并退出。 */
#define TEST_ASSERT(condition, message)                                      \
    do {                                                                     \
        assertion_count += 1U;                                               \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "断言失败 line=%d: %s\n", __LINE__, message); \
            return EXIT_FAILURE;                                             \
        }                                                                    \
    } while (0)

/* 按小端读取测试响应中的 16 位整数。 */
static uint16_t test_read_u16_le(const uint8_t *data)
{
    /* 组合两个字节。 */
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

/* 按小端读取测试响应中的 32 位整数。 */
static uint32_t test_read_u32_le(const uint8_t *data)
{
    /* 组合四个字节。 */
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

/* 按小端写入请求中的 16 位整数。 */
static void test_write_u16_le(uint8_t *data, uint16_t value)
{
    /* 写低字节和高字节。 */
    data[0] = (uint8_t)(value & UINT16_C(0xFF));
    data[1] = (uint8_t)(value >> 8U);
}

/* 按小端写入请求中的 32 位整数。 */
static void test_write_u32_le(uint8_t *data, uint32_t value)
{
    /* 写四个小端字节。 */
    data[0] = (uint8_t)(value & UINT32_C(0xFF));
    data[1] = (uint8_t)((value >> 8U) & UINT32_C(0xFF));
    data[2] = (uint8_t)((value >> 16U) & UINT32_C(0xFF));
    data[3] = (uint8_t)(value >> 24U);
}

/* 构造合法请求 payload。 */
static void test_make_request(
    uint8_t output[SESSION_TRANSFER_REQUEST_SIZE],
    uint8_t operation,
    uint16_t page_size,
    uint32_t request_id,
    uint32_t cursor)
{
    /* 清零固定请求。 */
    (void)memset(output, 0, SESSION_TRANSFER_REQUEST_SIZE);
    /* 写版本和操作。 */
    output[0] = SESSION_TRANSFER_VERSION;
    output[1] = operation;
    /* 写页大小、请求号和游标。 */
    test_write_u16_le(&output[2], page_size);
    test_write_u32_le(&output[4], request_id);
    test_write_u32_le(&output[8], cursor);
}

/* 构造可区分的合法摘要。 */
static session_summary_t test_summary(uint32_t sequence)
{
    /* 使用序号派生全部值，便于检查排序和逐字段往返。 */
    const session_summary_t summary = {
        /* 会话序号。 */
        sequence,
        /* 最近事件序号。 */
        sequence * UINT32_C(10),
        /* 动作索引。 */
        (uint8_t)(sequence % UINT32_C(11)),
        /* 指标类型。 */
        (uint8_t)(sequence % UINT32_C(3)),
        /* 会话标志。 */
        (uint16_t)(UINT16_C(0x0100) | (uint16_t)sequence),
        /* 开始 Unix 毫秒。 */
        UINT64_C(1700000000000) + sequence,
        /* 持续毫秒。 */
        UINT64_C(60000) + sequence,
        /* 指标总量。 */
        UINT64_C(20) + sequence,
        /* 毛热量 microkcal。 */
        UINT64_C(123000) + sequence,
        /* 活动热量 microkcal。 */
        UINT64_C(100000) + sequence,
        /* 平均稳定度。 */
        UINT16_C(28000),
        /* 最低稳定度。 */
        UINT16_C(24000),
        /* 事件数。 */
        UINT32_C(5) + sequence};
    /* 返回值拷贝。 */
    return summary;
}

/* 主测试覆盖分页、单条、重放、冲突和边界。 */
int main(void)
{
    /* 分配双槽后端所需字节并清零。 */
    const size_t backend_size = session_store_required_backend_size();
    uint8_t *const storage = (uint8_t *)calloc(backend_size, sizeof(uint8_t));
    /* 内存分配必须成功。 */
    TEST_ASSERT(storage != NULL, "分配双槽测试内存失败");
    /* 建立 0xFF 空白内存后端。 */
    session_memory_backend_t memory;
    TEST_ASSERT(
        session_memory_backend_init(&memory, storage, backend_size, true) == SESSION_STORE_STATUS_OK,
        "初始化内存后端失败");
    /* 初始化会话仓储。 */
    session_store_t store;
    const session_store_backend_t backend = session_memory_backend_interface(&memory);
    TEST_ASSERT(session_store_init(&store, &backend) == SESSION_STORE_STATUS_OK, "恢复空仓储失败");
    /* 写入 5 条递增会话。 */
    for (uint32_t sequence = UINT32_C(1); sequence <= UINT32_C(5); ++sequence) {
        /* 生成当前摘要。 */
        const session_summary_t summary = test_summary(sequence);
        /* changed 必须为 true。 */
        bool changed = false;
        /* 提交到真实双槽仓储。 */
        TEST_ASSERT(session_store_upsert(&store, &summary, &changed) == SESSION_STORE_STATUS_OK, "提交摘要失败");
        /* 新序号必须改变存储。 */
        TEST_ASSERT(changed, "新摘要没有改变存储");
    }
    /* 初始化传输服务。 */
    session_transfer_service_t service;
    TEST_ASSERT(session_transfer_service_init(&service, &store), "初始化传输服务失败");

    /* 请求游标 1 之后最多 2 条，预期返回 2、3。 */
    uint8_t request[SESSION_TRANSFER_REQUEST_SIZE];
    test_make_request(request, SESSION_TRANSFER_OPERATION_LIST, UINT16_C(2), UINT32_C(100), UINT32_C(1));
    /* 响应缓冲区。 */
    uint8_t response[SESSION_TRANSFER_RESPONSE_SIZE];
    uint16_t response_length = UINT16_C(0);
    /* 调用 BLE 回调适配。 */
    TEST_ASSERT(
        session_transfer_service_handle_request(
            request,
            (uint16_t)sizeof(request),
            response,
            sizeof(response),
            &response_length,
            &service) == BLE_SERVICE_STATUS_OK,
        "LIST 请求处理失败");
    /* 核对固定响应。 */
    TEST_ASSERT(response_length == SESSION_TRANSFER_RESPONSE_SIZE, "LIST 响应长度错误");
    TEST_ASSERT(response[2] == SESSION_TRANSFER_RESPONSE_OK, "LIST 响应状态错误");
    TEST_ASSERT(response[3] == SESSION_TRANSFER_FLAG_HAS_DATA, "LIST 首页面 flags 错误");
    TEST_ASSERT(test_read_u32_le(&response[4]) == UINT32_C(100), "request_id 回显错误");
    TEST_ASSERT(test_read_u32_le(&response[8]) == UINT32_C(3), "下一游标应为 3");
    TEST_ASSERT(test_read_u16_le(&response[12]) == UINT16_C(5), "设备总数应为 5");
    TEST_ASSERT(test_read_u16_le(&response[14]) == UINT16_C(2), "页数量应为 2");
    TEST_ASSERT(session_transfer_service_pending_count(&service) == 2U, "待发送数量应为 2");

    /* 依次取两条数据并核对旧到新顺序。 */
    uint8_t data_payload[SESSION_TRANSFER_DATA_SIZE];
    size_t data_length = 0U;
    TEST_ASSERT(session_transfer_service_pop_data(&service, data_payload, sizeof(data_payload), &data_length), "缺少第 1 条数据");
    /* 解码第 1 条。 */
    session_transfer_data_v1_t data;
    TEST_ASSERT(session_transfer_decode_data_v1(data_payload, data_length, &data), "第 1 条数据无法解码");
    TEST_ASSERT(data.summary.session_seq == UINT32_C(2), "第 1 条应为 session 2");
    TEST_ASSERT(data.item_index == UINT16_C(0), "第 1 条索引错误");
    TEST_ASSERT(data.item_count == UINT16_C(2), "第 1 条页总数错误");
    TEST_ASSERT(data.flags == UINT8_C(0), "第 1 条不应标记页尾");
    TEST_ASSERT(session_transfer_service_pop_data(&service, data_payload, sizeof(data_payload), &data_length), "缺少第 2 条数据");
    /* 解码第 2 条。 */
    TEST_ASSERT(session_transfer_decode_data_v1(data_payload, data_length, &data), "第 2 条数据无法解码");
    TEST_ASSERT(data.summary.session_seq == UINT32_C(3), "第 2 条应为 session 3");
    TEST_ASSERT(data.flags == SESSION_TRANSFER_DATA_FLAG_LAST_IN_PAGE, "第 2 条应标记页尾但非终点");
    TEST_ASSERT(!session_transfer_service_pop_data(&service, data_payload, sizeof(data_payload), &data_length), "页尾后不应还有数据");

    /* 同 request_id 和相同字节重放完整页。 */
    TEST_ASSERT(
        session_transfer_service_handle_request(
            request,
            (uint16_t)sizeof(request),
            response,
            sizeof(response),
            &response_length,
            &service) == BLE_SERVICE_STATUS_OK,
        "重复请求重放失败");
    TEST_ASSERT(session_transfer_service_pending_count(&service) == 2U, "重复请求没有重放数据页");
    /* 取完重放页，允许新请求。 */
    while (session_transfer_service_pop_data(&service, data_payload, sizeof(data_payload), &data_length)) {
        /* 每轮只消费一条冻结摘要。 */
    }

    /* 同 request_id 改 cursor 必须返回冲突且不执行。 */
    test_make_request(request, SESSION_TRANSFER_OPERATION_LIST, UINT16_C(2), UINT32_C(100), UINT32_C(3));
    TEST_ASSERT(
        session_transfer_service_handle_request(
            request,
            (uint16_t)sizeof(request),
            response,
            sizeof(response),
            &response_length,
            &service) == BLE_SERVICE_STATUS_OK,
        "冲突请求没有稳定响应");
    TEST_ASSERT(response[2] == SESSION_TRANSFER_RESPONSE_REQUEST_CONFLICT, "同 ID 不同请求未报冲突");

    /* 新请求从游标 3 拉完 4、5，必须标记 END。 */
    test_make_request(request, SESSION_TRANSFER_OPERATION_LIST, UINT16_C(12), UINT32_C(101), UINT32_C(3));
    TEST_ASSERT(
        session_transfer_service_handle_request(
            request,
            (uint16_t)sizeof(request),
            response,
            sizeof(response),
            &response_length,
            &service) == BLE_SERVICE_STATUS_OK,
        "末页请求失败");
    TEST_ASSERT(response[3] == (SESSION_TRANSFER_FLAG_HAS_DATA | SESSION_TRANSFER_FLAG_END), "末页 flags 错误");
    TEST_ASSERT(test_read_u32_le(&response[8]) == UINT32_C(5), "末页游标应为 5");
    TEST_ASSERT(test_read_u16_le(&response[14]) == UINT16_C(2), "末页应有两条");
    /* 消费 4。 */
    TEST_ASSERT(session_transfer_service_pop_data(&service, data_payload, sizeof(data_payload), &data_length), "缺少 session 4");
    /* 消费 5 并核对 END。 */
    TEST_ASSERT(session_transfer_service_pop_data(&service, data_payload, sizeof(data_payload), &data_length), "缺少 session 5");
    TEST_ASSERT(session_transfer_decode_data_v1(data_payload, data_length, &data), "session 5 解码失败");
    TEST_ASSERT(data.summary.session_seq == UINT32_C(5), "末条应为 session 5");
    TEST_ASSERT(
        data.flags == (SESSION_TRANSFER_DATA_FLAG_LAST_IN_PAGE | SESSION_TRANSFER_DATA_FLAG_END),
        "末条缺少 LAST/END");

    /* GET 精确读取 session 4。 */
    test_make_request(request, SESSION_TRANSFER_OPERATION_GET, UINT16_C(0), UINT32_C(102), UINT32_C(4));
    TEST_ASSERT(
        session_transfer_service_handle_request(
            request,
            (uint16_t)sizeof(request),
            response,
            sizeof(response),
            &response_length,
            &service) == BLE_SERVICE_STATUS_OK,
        "GET 请求失败");
    TEST_ASSERT(test_read_u16_le(&response[14]) == UINT16_C(1), "GET 应返回一条");
    TEST_ASSERT(session_transfer_service_pop_data(&service, data_payload, sizeof(data_payload), &data_length), "GET 缺少数据");
    TEST_ASSERT(session_transfer_decode_data_v1(data_payload, data_length, &data), "GET 数据解码失败");
    TEST_ASSERT(data.summary.session_seq == UINT32_C(4), "GET 返回错误序号");
    TEST_ASSERT(data.summary.gross_microkcal == UINT64_C(123004), "64 字节摘要能量字段错误");

    /* GET 不存在序号返回 NOT_FOUND，不产生数据。 */
    test_make_request(request, SESSION_TRANSFER_OPERATION_GET, UINT16_C(0), UINT32_C(103), UINT32_C(99));
    TEST_ASSERT(
        session_transfer_service_handle_request(
            request,
            (uint16_t)sizeof(request),
            response,
            sizeof(response),
            &response_length,
            &service) == BLE_SERVICE_STATUS_OK,
        "NOT_FOUND 请求处理失败");
    TEST_ASSERT(response[2] == SESSION_TRANSFER_RESPONSE_NOT_FOUND, "缺失序号未报 NOT_FOUND");
    TEST_ASSERT(session_transfer_service_pending_count(&service) == 0U, "NOT_FOUND 不应产生数据");

    /* 非法页大小返回 INVALID_REQUEST。 */
    test_make_request(request, SESSION_TRANSFER_OPERATION_LIST, UINT16_C(13), UINT32_C(104), UINT32_C(0));
    TEST_ASSERT(
        session_transfer_service_handle_request(
            request,
            (uint16_t)sizeof(request),
            response,
            sizeof(response),
            &response_length,
            &service) == BLE_SERVICE_STATUS_OK,
        "非法页大小没有响应");
    TEST_ASSERT(response[2] == SESSION_TRANSFER_RESPONSE_INVALID_REQUEST, "非法页大小状态错误");

    /* 释放测试后端。 */
    free(storage);
    /* 输出唯一成功标志和断言数。 */
    (void)printf("SESSION_TRANSFER_TESTS_OK assertions=%u\n", assertion_count);
    /* 返回成功。 */
    return EXIT_SUCCESS;
}
