/* 无硬件内存后端：用于主机测试、模拟器和断电故障注入。 */
#include "session_store.h"

/* SIZE_MAX 表示不限制写入预算。 */
#include <stdint.h>
/* memcpy/memset 实现随机读写和擦除。 */
#include <string.h>

/* 检查 [offset,offset+length) 是否位于内存后端。 */
static bool session_memory_range_is_valid(
    const session_memory_backend_t *memory,
    const size_t offset,
    const size_t length)
{
    /* context、data 必须有效，先用 offset<=capacity 避免减法下溢。 */
    return (memory != NULL) &&
           (memory->data != NULL) &&
           (offset <= memory->capacity) &&
           (length <= (memory->capacity - offset));
}

/* 内存后端随机读取回调。 */
static session_backend_result_t session_memory_read(
    void *context,
    const size_t offset,
    uint8_t *output,
    const size_t length)
{
    /* 把 opaque context 恢复为内存后端。 */
    session_memory_backend_t *memory = (session_memory_backend_t *)context;
    /* 非零读取必须提供输出，范围也必须合法。 */
    if (((output == NULL) && (length != 0U)) ||
        !session_memory_range_is_valid(memory, offset, length)) {
        /* 返回越界，不访问内存。 */
        return SESSION_BACKEND_OUT_OF_RANGE;
    }
    /* 复制请求字节；length=0 时 C 标准允许跳过。 */
    if (length != 0U) {
        /* 从后端数组复制到调用方。 */
        memcpy(output, &memory->data[offset], length);
    }
    /* 读取成功。 */
    return SESSION_BACKEND_OK;
}

/* 内存后端随机写入回调，支持在任意字节位置模拟掉电。 */
static session_backend_result_t session_memory_write(
    void *context,
    const size_t offset,
    const uint8_t *input,
    const size_t length)
{
    /* 恢复内存后端对象。 */
    session_memory_backend_t *memory = (session_memory_backend_t *)context;
    /* 非零写入必须提供输入且范围合法。 */
    if (((input == NULL) && (length != 0U)) ||
        !session_memory_range_is_valid(memory, offset, length)) {
        /* 越界写入被拒绝。 */
        return SESSION_BACKEND_OUT_OF_RANGE;
    }
    /* 零长度写入直接成功，不消耗预算。 */
    if (length == 0U) {
        /* 没有状态变化。 */
        return SESSION_BACKEND_OK;
    }

    /* 有限预算小于请求长度时，先写预算内前缀，再模拟断电失败。 */
    if ((memory->write_budget != SIZE_MAX) && (memory->write_budget < length)) {
        /* 保存还能落盘的前缀长度。 */
        const size_t partial_length = memory->write_budget;
        /* 非零前缀真实写入，使新槽形成撕裂数据。 */
        if (partial_length != 0U) {
            /* 复制可写前缀。 */
            memcpy(&memory->data[offset], input, partial_length);
        }
        /* 预算耗尽。 */
        memory->write_budget = 0U;
        /* 返回 I/O 错误模拟掉电。 */
        return SESSION_BACKEND_IO_ERROR;
    }

    /* 完整复制本次写入。 */
    memcpy(&memory->data[offset], input, length);
    /* 有限预算扣除已写字节。 */
    if (memory->write_budget != SIZE_MAX) {
        /* 入口已保证预算不小于 length。 */
        memory->write_budget -= length;
    }
    /* 统计完整成功 write 调用，用于幂等不落盘验证。 */
    memory->successful_write_calls += 1U;
    /* 写入成功。 */
    return SESSION_BACKEND_OK;
}

/* 内存后端擦除回调，用 0xFF 模拟空白 flash/文件槽。 */
static session_backend_result_t session_memory_erase(
    void *context,
    const size_t offset,
    const size_t length)
{
    /* 恢复内存对象。 */
    session_memory_backend_t *memory = (session_memory_backend_t *)context;
    /* 范围必须完整位于数组。 */
    if (!session_memory_range_is_valid(memory, offset, length)) {
        /* 拒绝越界擦除。 */
        return SESSION_BACKEND_OUT_OF_RANGE;
    }
    /* 非零范围填充 0xFF。 */
    if (length != 0U) {
        /* 模拟介质空白态。 */
        memset(&memory->data[offset], 0xFF, length);
    }
    /* 擦除成功。 */
    return SESSION_BACKEND_OK;
}

/* 内存后端同步回调；可注入失败模拟提交边界掉电。 */
static session_backend_result_t session_memory_sync(void *context)
{
    /* 恢复内存对象。 */
    session_memory_backend_t *memory = (session_memory_backend_t *)context;
    /* 空上下文属于 I/O 配置错误。 */
    if (memory == NULL) {
        /* 返回 I/O 错误。 */
        return SESSION_BACKEND_IO_ERROR;
    }
    /* 故障开关开启时模拟 fsync 失败。 */
    if (memory->fail_sync) {
        /* 调用方不得确认新槽。 */
        return SESSION_BACKEND_IO_ERROR;
    }
    /* 内存无需真实刷新。 */
    return SESSION_BACKEND_OK;
}

session_store_status_t session_memory_backend_init(
    session_memory_backend_t *memory,
    uint8_t *buffer,
    const size_t capacity,
    const bool erase_now)
{
    /* 对象、缓冲和非零容量必须有效。 */
    if ((memory == NULL) || (buffer == NULL) || (capacity == 0U)) {
        /* 拒绝无存储空间配置。 */
        return SESSION_STORE_STATUS_INVALID_ARGUMENT;
    }

    /* 清除统计和故障开关。 */
    memset(memory, 0, sizeof(*memory));
    /* 保存调用方缓冲。 */
    memory->data = buffer;
    /* 保存容量。 */
    memory->capacity = capacity;
    /* 默认无限写入。 */
    memory->write_budget = SIZE_MAX;
    /* 可选把整个缓冲初始化为空白态。 */
    if (erase_now) {
        /* 填充 0xFF，模拟首次使用介质。 */
        memset(buffer, 0xFF, capacity);
    }
    /* 初始化成功。 */
    return SESSION_STORE_STATUS_OK;
}

session_store_backend_t session_memory_backend_interface(session_memory_backend_t *memory)
{
    /* 先清零函数表，空 memory 时返回不可用后端而不崩溃。 */
    session_store_backend_t backend;
    /* 清除全部字段。 */
    memset(&backend, 0, sizeof(backend));
    /* 有效内存对象才填函数表。 */
    if (memory == NULL) {
        /* 返回全零后端，session_store_init 会拒绝。 */
        return backend;
    }

    /* context 指向调用方内存对象。 */
    backend.context = memory;
    /* 复制容量。 */
    backend.capacity = memory->capacity;
    /* 注入随机读回调。 */
    backend.read = session_memory_read;
    /* 注入可故障写回调。 */
    backend.write = session_memory_write;
    /* 注入擦除回调。 */
    backend.erase = session_memory_erase;
    /* 注入同步回调。 */
    backend.sync = session_memory_sync;
    /* 返回可复制函数表。 */
    return backend;
}

void session_memory_backend_set_write_budget(
    session_memory_backend_t *memory,
    const size_t write_budget)
{
    /* void 配置接口对空指针安全忽略。 */
    if (memory == NULL) {
        /* 无对象可配置。 */
        return;
    }
    /* 保存剩余完整/部分写入预算。 */
    memory->write_budget = write_budget;
}

void session_memory_backend_set_sync_failure(
    session_memory_backend_t *memory,
    const bool fail_sync)
{
    /* void 配置接口对空指针安全忽略。 */
    if (memory == NULL) {
        /* 无对象可配置。 */
        return;
    }
    /* 保存同步故障开关。 */
    memory->fail_sync = fail_sync;
}
