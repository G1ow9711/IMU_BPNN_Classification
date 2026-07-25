#ifndef SESSION_TRANSFER_H
#define SESSION_TRANSFER_H

/*
 * 会话摘要同步协议 v1 的纯 C 实现。
 *
 * 请求、响应和数据均位于 BLE 逻辑帧 payload；逻辑帧层已经负责 CRC16、分片和重组，
 * 本组件只处理固定小端字段、session_seq 游标、总数和 64 字节摘要。完整公式与时序见
 * docs/BLE通信、设备配置与会话存储.md 第 11 节。
 */

/* bool 表示是否还有待发布数据。 */
#include <stdbool.h>
/* size_t 表示输出容量和队列索引。 */
#include <stddef.h>
/* 固定宽度字段保证 ESP32 与 C# 逐字节一致。 */
#include <stdint.h>

/* 引入 Transfer Control 回调返回码。 */
#include "ble_service_core.h"
/* 引入固定 64 字节会话摘要内存结构。 */
#include "session_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 当前 payload 自身版本固定为 1。 */
#define SESSION_TRANSFER_VERSION UINT8_C(1)
/* TransferRequestV1 固定 12 字节。 */
#define SESSION_TRANSFER_REQUEST_SIZE ((size_t)12U)
/* TransferResponseV1 固定 16 字节。 */
#define SESSION_TRANSFER_RESPONSE_SIZE ((size_t)16U)
/* TransferDataV1 固定头 16 字节。 */
#define SESSION_TRANSFER_DATA_HEADER_SIZE ((size_t)16U)
/* 一帧摘要数据为 16 字节头加 64 字节摘要。 */
#define SESSION_TRANSFER_DATA_SIZE \
    (SESSION_TRANSFER_DATA_HEADER_SIZE + SESSION_STORE_SUMMARY_WIRE_SIZE)
/* 单页最多 12 条摘要，队列 RAM 上限为 12×80=960 字节。 */
#define SESSION_TRANSFER_MAX_PAGE_SIZE UINT16_C(12)

/* 请求操作码保持稳定，未知值必须拒绝。 */
typedef enum session_transfer_operation {
    /* 按 session_seq 游标列出新增摘要，结果从旧到新。 */
    SESSION_TRANSFER_OPERATION_LIST = 1,
    /* 按精确 session_seq 读取单条摘要。 */
    SESSION_TRANSFER_OPERATION_GET = 2
} session_transfer_operation_t;

/* 响应状态是线上稳定值，不暴露 session_store 内部返回码。 */
typedef enum session_transfer_response_status {
    /* 请求成功；item_count 可以为 0。 */
    SESSION_TRANSFER_RESPONSE_OK = 0,
    /* payload 版本不是 1。 */
    SESSION_TRANSFER_RESPONSE_UNSUPPORTED_VERSION = 1,
    /* 操作码不是 LIST 或 GET。 */
    SESSION_TRANSFER_RESPONSE_INVALID_OPERATION = 2,
    /* request_id、page_size 或 cursor 不满足操作合同。 */
    SESSION_TRANSFER_RESPONSE_INVALID_REQUEST = 3,
    /* GET 的 session_seq 不存在。 */
    SESSION_TRANSFER_RESPONSE_NOT_FOUND = 4,
    /* 上一页数据尚未取走，调用方应稍后重试同一请求。 */
    SESSION_TRANSFER_RESPONSE_BUSY = 5,
    /* 仓储未初始化或读取失败。 */
    SESSION_TRANSFER_RESPONSE_STORAGE_ERROR = 6,
    /* 同一 request_id 被用于不同请求字节。 */
    SESSION_TRANSFER_RESPONSE_REQUEST_CONFLICT = 7
} session_transfer_response_status_t;

/* TransferResponse flags 位定义。 */
typedef enum session_transfer_response_flags {
    /* 本页至少有一条 TransferData。 */
    SESSION_TRANSFER_FLAG_HAS_DATA = 1U << 0,
    /* 当前游标之后没有更多摘要。 */
    SESSION_TRANSFER_FLAG_END = 1U << 1
} session_transfer_response_flags_t;

/* TransferData flags 位定义。 */
typedef enum session_transfer_data_flags {
    /* 当前帧是本页最后一条；PC 收齐 item_count 后仍需核对该位。 */
    SESSION_TRANSFER_DATA_FLAG_LAST_IN_PAGE = 1U << 0,
    /* 当前页也是本次游标同步终点。 */
    SESSION_TRANSFER_DATA_FLAG_END = 1U << 1
} session_transfer_data_flags_t;

/* 解码后的固定请求。 */
typedef struct session_transfer_request_v1 {
    /* payload 版本，当前必须等于 1。 */
    uint8_t version;
    /* 取值为 session_transfer_operation_t。 */
    uint8_t operation;
    /* LIST 取 1～12；GET 固定为 0。 */
    uint16_t page_size;
    /* PC 生成的非零幂等请求号。 */
    uint32_t request_id;
    /* LIST 表示已持久化的最大 session_seq；GET 表示目标 session_seq。 */
    uint32_t cursor_session_seq;
} session_transfer_request_v1_t;

/* 固定响应字段。 */
typedef struct session_transfer_response_v1 {
    /* payload 版本固定为 1。 */
    uint8_t version;
    /* 回显请求操作码。 */
    uint8_t operation;
    /* 取值为 session_transfer_response_status_t。 */
    uint8_t status;
    /* HAS_DATA 与 END 位集合。 */
    uint8_t flags;
    /* 回显请求号，PC 用它匹配等待者。 */
    uint32_t request_id;
    /* LIST 返回本页最后 session_seq；空页保持输入游标。 */
    uint32_t next_cursor_session_seq;
    /* 响应生成时设备持有的全部摘要数量，范围 0～200。 */
    uint16_t total_count;
    /* 本页后续 TransferData 数量，范围 0～12。 */
    uint16_t item_count;
} session_transfer_response_v1_t;

/* 解码后的单条 TransferData。 */
typedef struct session_transfer_data_v1 {
    /* payload 版本固定为 1。 */
    uint8_t version;
    /* 数据类型当前固定为 1，表示 64 字节摘要。 */
    uint8_t data_kind;
    /* LAST_IN_PAGE 与 END 位集合。 */
    uint8_t flags;
    /* 线上保留字节必须为 0。 */
    uint8_t reserved;
    /* 对应 TransferRequest request_id。 */
    uint32_t request_id;
    /* 本页从 0 开始的条目序号。 */
    uint16_t item_index;
    /* 本页条目总数。 */
    uint16_t item_count;
    /* 当前设备全部摘要数量。 */
    uint16_t total_count;
    /* 线上保留字段必须为 0。 */
    uint16_t reserved2;
    /* 解码后的会话摘要值拷贝。 */
    session_summary_t summary;
} session_transfer_data_v1_t;

/* 服务保存最近请求快照和最多 12 条待通知数据，无动态分配。 */
typedef struct session_transfer_service {
    /* store 指向外部会话仓储，生命周期必须覆盖本服务。 */
    const session_store_t *store;
    /* last_request_valid 表示可进行同 request_id 冲突和重放判断。 */
    bool last_request_valid;
    /* last_request 保存最近完整 12 字节请求。 */
    uint8_t last_request[SESSION_TRANSFER_REQUEST_SIZE];
    /* last_response 保存最近 16 字节响应。 */
    uint8_t last_response[SESSION_TRANSFER_RESPONSE_SIZE];
    /* last_data 保存最近页完整 80 字节 payload，用于同请求可靠重放。 */
    uint8_t last_data[SESSION_TRANSFER_MAX_PAGE_SIZE][SESSION_TRANSFER_DATA_SIZE];
    /* last_data_count 表示 last_data 有效行数。 */
    uint16_t last_data_count;
    /* pending_index 指向应用下一条应发布的数据。 */
    uint16_t pending_index;
} session_transfer_service_t;

/* 解码并验证固定 12 字节请求。 */
bool session_transfer_decode_request_v1(
    const uint8_t *input,
    size_t input_length,
    session_transfer_request_v1_t *request);

/* 编码固定 16 字节响应。 */
bool session_transfer_encode_response_v1(
    const session_transfer_response_v1_t *response,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

/* 把内存摘要编码为固定 80 字节 TransferDataV1。 */
bool session_transfer_encode_data_v1(
    const session_transfer_data_v1_t *data,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

/* 解码固定 80 字节 TransferDataV1，并校验摘要版本、长度、动作和稳定度。 */
bool session_transfer_decode_data_v1(
    const uint8_t *input,
    size_t input_length,
    session_transfer_data_v1_t *data);

/* 绑定已经初始化的会话仓储并清空重放/待发送状态。 */
bool session_transfer_service_init(
    session_transfer_service_t *service,
    const session_store_t *store);

/*
 * 适配 ble_service_transfer_handler_fn：生成 TransferResponse，并把对应摘要冻结进内部队列。
 * NimBLE indication 确认后，应用 BLE 任务应循环 pop_data 并调用
 * ble_service_nimble_publish_transfer_data；不得在 GATT 写回调中阻塞发送通知。
 * context 必须非空并指向已初始化 session_transfer_service_t；函数仅在同步调用期间借用，
 * 不释放或保存新的别名，且该对象生命周期必须覆盖 BLE 服务注册和全部请求处理。
 */
ble_service_status_t session_transfer_service_handle_request(
    /* 非空只读请求 payload；生命周期覆盖本次调用，长度由 request_length 指定。 */
    const uint8_t *request_payload,
    /* 请求有效长度，单位字节；TransferRequest v1 固定为 12。 */
    uint16_t request_length,
    /* 非空可写响应缓冲区；形状为 [response_capacity] 字节。 */
    uint8_t *response_payload,
    /* 响应缓冲区容量，单位字节；至少为固定响应 16 字节。 */
    size_t response_capacity,
    /* 非空输出；成功时写入实际响应长度，单位字节。 */
    uint16_t *response_length,
    /* 非空服务上下文；生命周期覆盖 BLE 服务，所有权归应用层。 */
    void *context);

/* 取出下一条冻结的 80 字节 TransferData；队列为空时返回 false。 */
bool session_transfer_service_pop_data(
    session_transfer_service_t *service,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

/* 返回尚未取出的数据帧数，供 BLE 任务决定是否继续调度。 */
size_t session_transfer_service_pending_count(const session_transfer_service_t *service);

#ifdef __cplusplus
}
#endif

#endif
