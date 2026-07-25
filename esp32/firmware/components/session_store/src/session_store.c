/* 版本化摘要、双槽恢复和最近 200 会话索引实现。 */
#include "session_store.h"

/* INT32_MAX/UINT32_MAX 用于代数回绕比较和字段保护。 */
#include <stdint.h>
/* memset/memcpy 用于固定缓冲编码，不直接持久化 C 结构体布局。 */
#include <string.h>

/* 快照魔数按小端字节呈现为 "SSV1"。 */
#define SESSION_SNAPSHOT_MAGIC 0x31565353UL
/* 快照布局版本；不兼容变化必须递增。 */
#define SESSION_SNAPSHOT_VERSION 1U
/* 单条摘要布局版本。 */
#define SESSION_SUMMARY_VERSION 1U
/* 写入成功的固定提交标记。 */
#define SESSION_COMMIT_MARKER 0xC01117EDUL
/* active_slot 尚不存在时使用 255。 */
#define SESSION_NO_ACTIVE_SLOT 255U
/* 模型动作数量固定 11。 */
#define SESSION_ACTION_COUNT 11U
/* 指标类型最大值：0 次数、1 步数、2 毫秒。 */
#define SESSION_METRIC_KIND_MAX 2U
/* Q15 合法上限。 */
#define SESSION_STABILITY_Q15_MAX 32767U

/* 快照探测结果只保存选择槽所需的可信头字段。 */
typedef struct {
    /* true 表示提交标记、头 CRC、payload CRC 和每条记录均有效。 */
    bool valid;
    /* 快照代数。 */
    uint32_t generation;
    /* 快照记录数，范围 0..200。 */
    uint16_t count;
} session_slot_probe_t;

/* 把 16 位无符号整数按小端写入固定字节缓冲。 */
static void session_write_u16_le(uint8_t *output, const uint16_t value)
{
    /* 最低 8 位写在低地址。 */
    output[0] = (uint8_t)(value & 0xFFU);
    /* 最高 8 位写在下一字节。 */
    output[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

/* 把 32 位无符号整数按小端写入固定字节缓冲。 */
static void session_write_u32_le(uint8_t *output, const uint32_t value)
{
    /* 逐字节右移，显式消除主机端序差异。 */
    for (uint8_t index = 0U; index < 4U; ++index) {
        /* 第 index 字节保存 value 的对应低位。 */
        output[index] = (uint8_t)((value >> (8U * index)) & 0xFFU);
    }
}

/* 把 64 位无符号整数按小端写入固定字节缓冲。 */
static void session_write_u64_le(uint8_t *output, const uint64_t value)
{
    /* 逐字节编码 8 个字节。 */
    for (uint8_t index = 0U; index < 8U; ++index) {
        /* 第 index 字节保存 value 的对应低位。 */
        output[index] = (uint8_t)((value >> (8U * index)) & 0xFFULL);
    }
}

/* 从小端字节缓冲读取 16 位无符号整数。 */
static uint16_t session_read_u16_le(const uint8_t *input)
{
    /* 低字节加高字节左移 8 位。 */
    return (uint16_t)input[0] | ((uint16_t)input[1] << 8U);
}

/* 从小端字节缓冲读取 32 位无符号整数。 */
static uint32_t session_read_u32_le(const uint8_t *input)
{
    /* 从 0 开始累加每个小端字节。 */
    uint32_t value = 0U;
    /* 固定读取 4 字节。 */
    for (uint8_t index = 0U; index < 4U; ++index) {
        /* 按字节位置左移后并入结果。 */
        value |= ((uint32_t)input[index]) << (8U * index);
    }
    /* 返回与主机端序无关的值。 */
    return value;
}

/* 从小端字节缓冲读取 64 位无符号整数。 */
static uint64_t session_read_u64_le(const uint8_t *input)
{
    /* 从 0 开始累加 8 字节。 */
    uint64_t value = 0ULL;
    /* 固定读取 8 字节。 */
    for (uint8_t index = 0U; index < 8U; ++index) {
        /* 按字节位置左移后并入结果。 */
        value |= ((uint64_t)input[index]) << (8U * index);
    }
    /* 返回 64 位结果。 */
    return value;
}

/* IEEE CRC32 增量更新；crc_state 输入/输出均尚未执行最终异或。 */
static uint32_t session_crc32_update(
    uint32_t crc_state,
    const uint8_t *data,
    const size_t length)
{
    /* 逐字节纳入 CRC。 */
    for (size_t byte_index = 0U; byte_index < length; ++byte_index) {
        /* 当前输入字节与 CRC 低 8 位异或。 */
        crc_state ^= data[byte_index];
        /* 每个字节按最低有效位算法迭代 8 次。 */
        for (uint8_t bit_index = 0U; bit_index < 8U; ++bit_index) {
            /* 保存最低位，用于决定是否异或反射多项式。 */
            const uint32_t lsb = crc_state & 1U;
            /* CRC 右移一位。 */
            crc_state >>= 1U;
            /* 最低位为 1 时异或 IEEE 反射多项式 0xEDB88320。 */
            if (lsb != 0U) {
                /* 纳入多项式反馈。 */
                crc_state ^= 0xEDB88320UL;
            }
        }
    }
    /* 返回未最终异或状态，供下一段继续。 */
    return crc_state;
}

uint32_t session_store_crc32(const uint8_t *data, const size_t length)
{
    /* 非零长度必须提供数据指针。 */
    if ((data == NULL) && (length != 0U)) {
        /* 参数错误无法通过返回码表达，约定返回 0；内部调用不会传此组合。 */
        return 0U;
    }
    /* CRC 初值固定为全 1。 */
    uint32_t crc_state = 0xFFFFFFFFUL;
    /* 纳入完整输入。 */
    crc_state = session_crc32_update(crc_state, data, length);
    /* 最终异或全 1 得到标准 IEEE CRC32。 */
    return crc_state ^ 0xFFFFFFFFUL;
}

size_t session_store_required_backend_size(void)
{
    /* 返回两个固定槽总容量，当前为 25672 字节。 */
    return (size_t)SESSION_STORE_REQUIRED_BACKEND_SIZE;
}

/* 把后端结果统一映射为领域状态。 */
static session_store_status_t session_map_backend_result(const session_backend_result_t result)
{
    /* 后端成功直接映射成功。 */
    if (result == SESSION_BACKEND_OK) {
        /* 返回领域成功。 */
        return SESSION_STORE_STATUS_OK;
    }
    /* 越界表示介质容量不足。 */
    if (result == SESSION_BACKEND_OUT_OF_RANGE) {
        /* 返回空间不足。 */
        return SESSION_STORE_STATUS_NO_SPACE;
    }
    /* 其它后端错误统一为 I/O 错误。 */
    return SESSION_STORE_STATUS_IO_ERROR;
}

/* 检查注入后端是否包含全部掉电安全必要操作。 */
static bool session_backend_is_valid(
    const session_store_backend_t *backend,
    const size_t minimum_capacity)
{
    /* 后端及四个函数指针必须存在，容量必须达到调用场景下限。 */
    return (backend != NULL) &&
           (backend->read != NULL) &&
           (backend->write != NULL) &&
           (backend->erase != NULL) &&
           (backend->sync != NULL) &&
           (backend->capacity >= minimum_capacity);
}

/* 检查摘要字段是否能被 v1 线性格式表达。 */
static bool session_summary_is_valid(const session_summary_t *summary)
{
    /* 摘要指针、动作、指标和 Q15 范围必须有效。 */
    return (summary != NULL) &&
           (summary->action_id < SESSION_ACTION_COUNT) &&
           (summary->metric_kind <= SESSION_METRIC_KIND_MAX) &&
           (summary->average_stability_q15 <= SESSION_STABILITY_Q15_MAX) &&
           (summary->minimum_stability_q15 <= SESSION_STABILITY_Q15_MAX);
}

/* 把内存摘要编码为固定 64 字节小端格式。 */
static void session_encode_summary(
    const session_summary_t *summary,
    uint8_t output[SESSION_STORE_SUMMARY_WIRE_SIZE])
{
    /* 先清零保留字节，确保 CRC 和未来兼容行为确定。 */
    memset(output, 0, SESSION_STORE_SUMMARY_WIRE_SIZE);
    /* offset 0：记录版本。 */
    session_write_u16_le(&output[0], SESSION_SUMMARY_VERSION);
    /* offset 2：固定记录长度 64。 */
    session_write_u16_le(&output[2], SESSION_STORE_SUMMARY_WIRE_SIZE);
    /* offset 4：会话主键。 */
    session_write_u32_le(&output[4], summary->session_seq);
    /* offset 8：幂等事件水位。 */
    session_write_u32_le(&output[8], summary->last_event_seq);
    /* offset 12：动作索引。 */
    output[12] = summary->action_id;
    /* offset 13：指标类型。 */
    output[13] = summary->metric_kind;
    /* offset 14：会话标志。 */
    session_write_u16_le(&output[14], summary->flags);
    /* offset 16：Unix 毫秒。 */
    session_write_u64_le(&output[16], summary->start_unix_ms);
    /* offset 24：持续毫秒。 */
    session_write_u64_le(&output[24], summary->duration_ms);
    /* offset 32：指标总值。 */
    session_write_u64_le(&output[32], summary->metric_total);
    /* offset 40：毛热量。 */
    session_write_u64_le(&output[40], summary->gross_microkcal);
    /* offset 48：活动热量。 */
    session_write_u64_le(&output[48], summary->active_microkcal);
    /* offset 56：平均 Q15。 */
    session_write_u16_le(&output[56], summary->average_stability_q15);
    /* offset 58：最低 Q15。 */
    session_write_u16_le(&output[58], summary->minimum_stability_q15);
    /* offset 60：事件数量。 */
    session_write_u32_le(&output[60], summary->event_count);
}

/* 从固定 64 字节格式解码并校验摘要。 */
static bool session_decode_summary(
    const uint8_t input[SESSION_STORE_SUMMARY_WIRE_SIZE],
    session_summary_t *summary)
{
    /* 记录版本和长度必须与当前实现完全一致。 */
    if ((session_read_u16_le(&input[0]) != SESSION_SUMMARY_VERSION) ||
        (session_read_u16_le(&input[2]) != SESSION_STORE_SUMMARY_WIRE_SIZE)) {
        /* 不支持的记录不能进入内存索引。 */
        return false;
    }

    /* 清零结构填充，避免主机 ABI 差异影响测试。 */
    memset(summary, 0, sizeof(*summary));
    /* 解码会话主键。 */
    summary->session_seq = session_read_u32_le(&input[4]);
    /* 解码幂等事件水位。 */
    summary->last_event_seq = session_read_u32_le(&input[8]);
    /* 解码动作索引。 */
    summary->action_id = input[12];
    /* 解码指标类型。 */
    summary->metric_kind = input[13];
    /* 解码标志。 */
    summary->flags = session_read_u16_le(&input[14]);
    /* 解码 Unix 毫秒。 */
    summary->start_unix_ms = session_read_u64_le(&input[16]);
    /* 解码持续毫秒。 */
    summary->duration_ms = session_read_u64_le(&input[24]);
    /* 解码指标总值。 */
    summary->metric_total = session_read_u64_le(&input[32]);
    /* 解码毛热量。 */
    summary->gross_microkcal = session_read_u64_le(&input[40]);
    /* 解码活动热量。 */
    summary->active_microkcal = session_read_u64_le(&input[48]);
    /* 解码平均稳定度。 */
    summary->average_stability_q15 = session_read_u16_le(&input[56]);
    /* 解码最低稳定度。 */
    summary->minimum_stability_q15 = session_read_u16_le(&input[58]);
    /* 解码事件数。 */
    summary->event_count = session_read_u32_le(&input[60]);
    /* 返回字段级校验结果。 */
    return session_summary_is_valid(summary);
}

/* 获取按时间从旧到新的第 logical_index 条记录。 */
static const session_summary_t *session_summary_oldest_at(
    const session_store_t *store,
    const uint16_t logical_index)
{
    /* 环形物理位置 = (head+逻辑位置) mod 200。 */
    const uint16_t physical_index = (uint16_t)((store->head + logical_index) %
                                               SESSION_STORE_MAX_SUMMARIES);
    /* 返回静态数组内只读指针。 */
    return &store->summaries[physical_index];
}

/* 计算当前快照 payload CRC，不分配 12.8 KiB 临时缓冲。 */
static uint32_t session_compute_payload_crc(const session_store_t *store)
{
    /* CRC 初值全 1。 */
    uint32_t crc_state = 0xFFFFFFFFUL;
    /* 每次只在栈上编码一条 64 字节记录。 */
    uint8_t record[SESSION_STORE_SUMMARY_WIRE_SIZE];
    /* 按从旧到新顺序纳入 CRC，恢复后顺序确定。 */
    for (uint16_t index = 0U; index < store->count; ++index) {
        /* 编码当前逻辑记录。 */
        session_encode_summary(session_summary_oldest_at(store, index), record);
        /* 增量纳入 64 字节。 */
        crc_state = session_crc32_update(crc_state, record, sizeof(record));
    }
    /* 执行标准最终异或。 */
    return crc_state ^ 0xFFFFFFFFUL;
}

/* 编码 32 字节快照头，并用 header_crc 字段置零后的全头计算 CRC。 */
static void session_encode_snapshot_header(
    const uint32_t generation,
    const uint16_t count,
    const uint32_t payload_crc,
    uint8_t output[SESSION_STORE_SNAPSHOT_HEADER_SIZE])
{
    /* 清零保留字段和 CRC 占位。 */
    memset(output, 0, SESSION_STORE_SNAPSHOT_HEADER_SIZE);
    /* offset 0：魔数。 */
    session_write_u32_le(&output[0], SESSION_SNAPSHOT_MAGIC);
    /* offset 4：快照版本。 */
    session_write_u16_le(&output[4], SESSION_SNAPSHOT_VERSION);
    /* offset 6：头长度。 */
    session_write_u16_le(&output[6], SESSION_STORE_SNAPSHOT_HEADER_SIZE);
    /* offset 8：代数。 */
    session_write_u32_le(&output[8], generation);
    /* offset 12：记录数。 */
    session_write_u16_le(&output[12], count);
    /* offset 14..15 保留为 0。 */
    /* offset 16：实际 payload 长度。 */
    session_write_u32_le(&output[16], ((uint32_t)count) * SESSION_STORE_SUMMARY_WIRE_SIZE);
    /* offset 20：payload CRC。 */
    session_write_u32_le(&output[20], payload_crc);
    /* offset 24 暂为 0，用全 32 字节计算头 CRC。 */
    const uint32_t header_crc = session_store_crc32(output, SESSION_STORE_SNAPSHOT_HEADER_SIZE);
    /* offset 24：头 CRC。 */
    session_write_u32_le(&output[24], header_crc);
    /* offset 28..31 保留为 0。 */
}

/* 校验快照头并提取代数、数量和 payload CRC。 */
static bool session_decode_snapshot_header(
    const uint8_t input[SESSION_STORE_SNAPSHOT_HEADER_SIZE],
    uint32_t *generation,
    uint16_t *count,
    uint32_t *payload_crc)
{
    /* 复制头以便把 CRC 字段清零后重算。 */
    uint8_t header_copy[SESSION_STORE_SNAPSHOT_HEADER_SIZE];
    /* 保存完整输入。 */
    memcpy(header_copy, input, sizeof(header_copy));
    /* 读取介质保存的头 CRC。 */
    const uint32_t stored_header_crc = session_read_u32_le(&header_copy[24]);
    /* 把 CRC 字段恢复为编码时的 0。 */
    memset(&header_copy[24], 0, 4U);
    /* 重算全头 CRC。 */
    const uint32_t computed_header_crc = session_store_crc32(header_copy, sizeof(header_copy));
    /* CRC 不符表示头撕裂或损坏。 */
    if (stored_header_crc != computed_header_crc) {
        /* 拒绝该槽。 */
        return false;
    }
    /* 魔数、版本和头长度必须匹配。 */
    if ((session_read_u32_le(&input[0]) != SESSION_SNAPSHOT_MAGIC) ||
        (session_read_u16_le(&input[4]) != SESSION_SNAPSHOT_VERSION) ||
        (session_read_u16_le(&input[6]) != SESSION_STORE_SNAPSHOT_HEADER_SIZE)) {
        /* 不兼容或随机数据不能恢复。 */
        return false;
    }

    /* 提取记录数。 */
    const uint16_t decoded_count = session_read_u16_le(&input[12]);
    /* 数量不能超过固定容量。 */
    if (decoded_count > SESSION_STORE_MAX_SUMMARIES) {
        /* 拒绝越界 payload。 */
        return false;
    }
    /* payload 长度必须精确等于 count×64。 */
    if (session_read_u32_le(&input[16]) !=
        (((uint32_t)decoded_count) * SESSION_STORE_SUMMARY_WIRE_SIZE)) {
        /* 长度不一致可能导致越界读取。 */
        return false;
    }

    /* 返回可信代数。 */
    *generation = session_read_u32_le(&input[8]);
    /* 返回可信数量。 */
    *count = decoded_count;
    /* 返回待核对的 payload CRC。 */
    *payload_crc = session_read_u32_le(&input[20]);
    /* 头校验成功。 */
    return true;
}

/* 返回槽起始偏移。 */
static size_t session_slot_offset(const uint8_t slot)
{
    /* slot 0 从 0 开始，slot 1 紧随固定槽大小。 */
    return ((size_t)slot) * ((size_t)SESSION_STORE_SLOT_SIZE);
}

/* 探测一个槽，验证提交标记、头 CRC、payload CRC 和每条摘要。 */
static void session_probe_slot(
    const session_store_backend_t *backend,
    const uint8_t slot,
    session_slot_probe_t *probe)
{
    /* 默认槽无效，任一步失败都安全回退。 */
    memset(probe, 0, sizeof(*probe));
    /* 计算槽起始。 */
    const size_t base = session_slot_offset(slot);
    /* 提交标记固定放在槽最后 4 字节。 */
    uint8_t marker_bytes[SESSION_STORE_COMMIT_MARKER_SIZE];
    /* 读取提交标记。 */
    if (backend->read(
            backend->context,
            base + SESSION_STORE_SLOT_SIZE - SESSION_STORE_COMMIT_MARKER_SIZE,
            marker_bytes,
            sizeof(marker_bytes)) != SESSION_BACKEND_OK) {
        /* 后端读取失败时该槽不可用。 */
        return;
    }
    /* 没有最终提交标记表示写入中断。 */
    if (session_read_u32_le(marker_bytes) != SESSION_COMMIT_MARKER) {
        /* 保持无效。 */
        return;
    }

    /* 读取快照头。 */
    uint8_t header[SESSION_STORE_SNAPSHOT_HEADER_SIZE];
    /* 完整读取头。 */
    if (backend->read(backend->context, base, header, sizeof(header)) != SESSION_BACKEND_OK) {
        /* 头不可读时该槽无效。 */
        return;
    }
    /* 保存头解码字段。 */
    uint32_t generation = 0U;
    /* 保存记录数。 */
    uint16_t count = 0U;
    /* 保存头声明 payload CRC。 */
    uint32_t expected_payload_crc = 0U;
    /* 校验头。 */
    if (!session_decode_snapshot_header(header, &generation, &count, &expected_payload_crc)) {
        /* 头不合法时槽无效。 */
        return;
    }

    /* 用流式 CRC 避免 12.8 KiB 栈缓冲。 */
    uint32_t crc_state = 0xFFFFFFFFUL;
    /* 每次只读一条记录。 */
    uint8_t record[SESSION_STORE_SUMMARY_WIRE_SIZE];
    /* 临时解码用于验证记录版本和字段范围。 */
    session_summary_t decoded_summary;
    /* 遍历槽内全部记录。 */
    for (uint16_t index = 0U; index < count; ++index) {
        /* 记录偏移紧随 32 字节头。 */
        const size_t record_offset = base + SESSION_STORE_SNAPSHOT_HEADER_SIZE +
                                     (((size_t)index) * SESSION_STORE_SUMMARY_WIRE_SIZE);
        /* 读取固定 64 字节。 */
        if (backend->read(backend->context, record_offset, record, sizeof(record)) !=
            SESSION_BACKEND_OK) {
            /* 任一记录不可读则整个快照无效。 */
            return;
        }
        /* 验证每条记录布局和字段。 */
        if (!session_decode_summary(record, &decoded_summary)) {
            /* 记录损坏时拒绝整个槽。 */
            return;
        }
        /* 纳入 payload CRC。 */
        crc_state = session_crc32_update(crc_state, record, sizeof(record));
    }
    /* 完成标准最终异或。 */
    const uint32_t computed_payload_crc = crc_state ^ 0xFFFFFFFFUL;
    /* payload CRC 不符时拒绝槽。 */
    if (computed_payload_crc != expected_payload_crc) {
        /* 保持 probe.valid=false。 */
        return;
    }

    /* 全部校验完成，槽可恢复。 */
    probe->valid = true;
    /* 保存代数供双槽选择。 */
    probe->generation = generation;
    /* 保存记录数供加载。 */
    probe->count = count;
}

/* 用模 2^32 序比较代数，支持回绕。 */
static bool session_generation_is_newer(const uint32_t candidate, const uint32_t reference)
{
    /* 差值转 int32；正值表示 candidate 在半个序号空间内更新。 */
    return ((int32_t)(candidate - reference)) > 0;
}

/* 从已经探测有效的槽加载所有摘要。 */
static session_store_status_t session_load_slot(
    session_store_t *store,
    const uint8_t slot,
    const session_slot_probe_t *probe)
{
    /* 槽起始偏移。 */
    const size_t base = session_slot_offset(slot);
    /* 清空内存索引，恢复后 head 固定为 0。 */
    memset(store->summaries, 0, sizeof(store->summaries));
    /* 恢复记录数。 */
    store->count = probe->count;
    /* 介质按从旧到新排列，因此 head 为 0。 */
    store->head = 0U;
    /* 保存活动槽。 */
    store->active_slot = slot;
    /* 保存代数。 */
    store->generation = probe->generation;

    /* 每次读取一条线性记录。 */
    uint8_t record[SESSION_STORE_SUMMARY_WIRE_SIZE];
    /* 遍历可信记录数量。 */
    for (uint16_t index = 0U; index < probe->count; ++index) {
        /* 计算当前记录偏移。 */
        const size_t record_offset = base + SESSION_STORE_SNAPSHOT_HEADER_SIZE +
                                     (((size_t)index) * SESSION_STORE_SUMMARY_WIRE_SIZE);
        /* 读取记录。 */
        const session_backend_result_t read_result = store->backend.read(
            store->backend.context,
            record_offset,
            record,
            sizeof(record));
        /* 理论上探测后应可读；仍防御介质在两次读取间失败。 */
        if (read_result != SESSION_BACKEND_OK) {
            /* 返回明确 I/O/容量错误。 */
            return session_map_backend_result(read_result);
        }
        /* 再次解码到内存槽，防止介质在探测后变化。 */
        if (!session_decode_summary(record, &store->summaries[index])) {
            /* 记录已变坏时报告损坏。 */
            return SESSION_STORE_STATUS_CORRUPT;
        }
    }
    /* 加载完成。 */
    return SESSION_STORE_STATUS_OK;
}

/* 把当前内存索引完整写入 inactive slot，并最后提交标记。 */
static session_store_status_t session_persist_snapshot(
    session_store_t *store,
    const uint8_t target_slot,
    const uint32_t target_generation)
{
    /* 目标槽只允许 0 或 1。 */
    if (target_slot > 1U) {
        /* 内部调用错误。 */
        return SESSION_STORE_STATUS_INVALID_ARGUMENT;
    }

    /* 计算按旧到新序列化后的 payload CRC。 */
    const uint32_t payload_crc = session_compute_payload_crc(store);
    /* 构造包含代数、数量和 CRC 的 32 字节头。 */
    uint8_t header[SESSION_STORE_SNAPSHOT_HEADER_SIZE];
    /* 编码确定性小端头。 */
    session_encode_snapshot_header(target_generation, store->count, payload_crc, header);
    /* 计算目标槽起始。 */
    const size_t base = session_slot_offset(target_slot);

    /* 先擦除整个非活动槽，确保旧提交标记被清除。 */
    session_backend_result_t backend_result = store->backend.erase(
        store->backend.context,
        base,
        SESSION_STORE_SLOT_SIZE);
    /* 擦除失败时保留旧活动槽。 */
    if (backend_result != SESSION_BACKEND_OK) {
        /* 映射后端错误。 */
        return session_map_backend_result(backend_result);
    }

    /* 写头，但此时槽尾仍没有提交标记。 */
    backend_result = store->backend.write(
        store->backend.context,
        base,
        header,
        sizeof(header));
    /* 头写失败时新槽保持无效。 */
    if (backend_result != SESSION_BACKEND_OK) {
        /* 返回错误。 */
        return session_map_backend_result(backend_result);
    }

    /* 栈上单条记录缓冲避免大内存。 */
    uint8_t record[SESSION_STORE_SUMMARY_WIRE_SIZE];
    /* 按从旧到新写入全部有效摘要。 */
    for (uint16_t index = 0U; index < store->count; ++index) {
        /* 编码逻辑位置对应记录。 */
        session_encode_summary(session_summary_oldest_at(store, index), record);
        /* 计算线性记录偏移。 */
        const size_t record_offset = base + SESSION_STORE_SNAPSHOT_HEADER_SIZE +
                                     (((size_t)index) * SESSION_STORE_SUMMARY_WIRE_SIZE);
        /* 写入完整 64 字节。 */
        backend_result = store->backend.write(
            store->backend.context,
            record_offset,
            record,
            sizeof(record));
        /* 任一记录写失败时不写提交标记。 */
        if (backend_result != SESSION_BACKEND_OK) {
            /* 返回后端错误。 */
            return session_map_backend_result(backend_result);
        }
    }

    /* 先同步头和 payload，保证它们先于提交标记落盘。 */
    backend_result = store->backend.sync(store->backend.context);
    /* 预提交同步失败时新槽仍无效。 */
    if (backend_result != SESSION_BACKEND_OK) {
        /* 返回后端错误。 */
        return session_map_backend_result(backend_result);
    }

    /* 构造小端提交标记。 */
    uint8_t marker[SESSION_STORE_COMMIT_MARKER_SIZE];
    /* 写入固定标记值。 */
    session_write_u32_le(marker, SESSION_COMMIT_MARKER);
    /* 提交标记固定写在槽最后 4 字节。 */
    backend_result = store->backend.write(
        store->backend.context,
        base + SESSION_STORE_SLOT_SIZE - SESSION_STORE_COMMIT_MARKER_SIZE,
        marker,
        sizeof(marker));
    /* 标记写失败时恢复仍会忽略新槽。 */
    if (backend_result != SESSION_BACKEND_OK) {
        /* 返回后端错误。 */
        return session_map_backend_result(backend_result);
    }

    /* 再次同步提交标记，成功后才允许内存 active_slot 切换。 */
    backend_result = store->backend.sync(store->backend.context);
    /* 最终同步失败时不确认提交。 */
    if (backend_result != SESSION_BACKEND_OK) {
        /* 返回后端错误。 */
        return session_map_backend_result(backend_result);
    }
    /* 双阶段写入完成。 */
    return SESSION_STORE_STATUS_OK;
}

session_store_status_t session_store_init(
    session_store_t *store,
    const session_store_backend_t *backend)
{
    /* store 和满足最小容量的完整后端必须存在。 */
    if ((store == NULL) ||
        !session_backend_is_valid(backend, session_store_required_backend_size())) {
        /* 拒绝不完整后端，防止提交安全假设失效。 */
        return SESSION_STORE_STATUS_INVALID_ARGUMENT;
    }

    /* 清除旧内存状态。 */
    memset(store, 0, sizeof(*store));
    /* 保存函数表副本；context 的实际对象仍由调用方拥有。 */
    store->backend = *backend;
    /* 默认尚无活动槽。 */
    store->active_slot = SESSION_NO_ACTIVE_SLOT;

    /* 分别探测两个槽。 */
    session_slot_probe_t probes[2];
    /* 验证槽 0。 */
    session_probe_slot(backend, 0U, &probes[0]);
    /* 验证槽 1。 */
    session_probe_slot(backend, 1U, &probes[1]);

    /* 两个槽都无效通常表示首次使用或已格式化，建立空索引。 */
    if (!probes[0].valid && !probes[1].valid) {
        /* 标记对象可用，首次 upsert 会写槽 0。 */
        store->initialized = true;
        /* 空索引恢复成功。 */
        return SESSION_STORE_STATUS_OK;
    }

    /* 默认选择唯一有效槽或槽 0。 */
    uint8_t selected_slot = probes[0].valid ? 0U : 1U;
    /* 两槽都有效时用回绕安全代数选择更新者。 */
    if (probes[0].valid && probes[1].valid &&
        session_generation_is_newer(probes[1].generation, probes[0].generation)) {
        /* 槽 1 代数更新。 */
        selected_slot = 1U;
    }

    /* 从选择槽加载摘要。 */
    const session_store_status_t load_status = session_load_slot(
        store,
        selected_slot,
        &probes[selected_slot]);
    /* 加载期间介质变化时返回错误。 */
    if (load_status != SESSION_STORE_STATUS_OK) {
        /* 对象保持未初始化。 */
        return load_status;
    }
    /* 完成恢复后才标记 initialized。 */
    store->initialized = true;
    /* 恢复成功。 */
    return SESSION_STORE_STATUS_OK;
}

session_store_status_t session_store_format(session_store_t *store)
{
    /* 只有已注入后端的对象可格式化。 */
    if (store == NULL) {
        /* 空指针错误。 */
        return SESSION_STORE_STATUS_INVALID_ARGUMENT;
    }
    if (!store->initialized) {
        /* 未初始化对象没有可信后端。 */
        return SESSION_STORE_STATUS_INVALID_STATE;
    }

    /* 擦除双槽全部字节。 */
    const session_backend_result_t erase_result = store->backend.erase(
        store->backend.context,
        0U,
        session_store_required_backend_size());
    /* 擦除失败时不清空内存索引。 */
    if (erase_result != SESSION_BACKEND_OK) {
        /* 返回后端错误。 */
        return session_map_backend_result(erase_result);
    }
    /* 强制空白状态落盘。 */
    const session_backend_result_t sync_result = store->backend.sync(store->backend.context);
    /* 同步失败时不声称格式化完成。 */
    if (sync_result != SESSION_BACKEND_OK) {
        /* 返回 I/O 错误。 */
        return session_map_backend_result(sync_result);
    }

    /* 清空所有内存摘要。 */
    memset(store->summaries, 0, sizeof(store->summaries));
    /* 环头回到 0。 */
    store->head = 0U;
    /* 有效数量归零。 */
    store->count = 0U;
    /* 代数归零；下一次提交为 1。 */
    store->generation = 0U;
    /* 没有已提交活动槽。 */
    store->active_slot = SESSION_NO_ACTIVE_SLOT;
    /* 格式化成功。 */
    return SESSION_STORE_STATUS_OK;
}

/* 在环形索引中查找 session_seq；找到时返回 true 和物理位置。 */
static bool session_find_summary(
    const session_store_t *store,
    const uint32_t session_seq,
    uint16_t *physical_index)
{
    /* 只遍历当前有效条目。 */
    for (uint16_t logical_index = 0U; logical_index < store->count; ++logical_index) {
        /* 计算从旧到新逻辑位置对应的物理位置。 */
        const uint16_t candidate_index = (uint16_t)((store->head + logical_index) %
                                                    SESSION_STORE_MAX_SUMMARIES);
        /* 会话主键相同即命中。 */
        if (store->summaries[candidate_index].session_seq == session_seq) {
            /* 返回物理位置。 */
            *physical_index = candidate_index;
            /* 查找成功。 */
            return true;
        }
    }
    /* 当前索引无此会话。 */
    return false;
}

session_store_status_t session_store_upsert(
    session_store_t *store,
    const session_summary_t *summary,
    bool *changed)
{
    /* 对象、摘要和结果指针必须有效，摘要字段需满足 v1 合同。 */
    if ((store == NULL) || (changed == NULL) || !session_summary_is_valid(summary)) {
        /* 不修改内存或介质。 */
        return SESSION_STORE_STATUS_INVALID_ARGUMENT;
    }
    /* store 必须已完成 init。 */
    if (!store->initialized) {
        /* 未初始化对象没有后端。 */
        return SESSION_STORE_STATUS_INVALID_STATE;
    }
    /* 默认没有状态变化。 */
    *changed = false;

    /* 查找同一 session_seq。 */
    uint16_t target_index = 0U;
    /* 保存是否为更新。 */
    const bool existing = session_find_summary(store, summary->session_seq, &target_index);
    /* 重复或过期事件水位不得覆盖更新摘要，也不得写介质。 */
    if (existing &&
        (summary->last_event_seq <= store->summaries[target_index].last_event_seq)) {
        /* 幂等成功。 */
        return SESSION_STORE_STATUS_OK;
    }

    /* 保存可被本次修改影响的单个槽位，便于 I/O 失败后回滚。 */
    session_summary_t previous_summary;
    /* 旧 head 用于容量轮转回滚。 */
    const uint16_t previous_head = store->head;
    /* 旧 count 用于新增回滚。 */
    const uint16_t previous_count = store->count;
    /* 标记目标槽此前是否含有效摘要。 */
    bool target_had_previous = false;

    /* 同会话新事件直接更新原物理槽。 */
    if (existing) {
        /* 保存旧摘要。 */
        previous_summary = store->summaries[target_index];
        /* 标记需要回滚槽内容。 */
        target_had_previous = true;
        /* 写入新摘要副本。 */
        store->summaries[target_index] = *summary;
    } else if (store->count < SESSION_STORE_MAX_SUMMARIES) {
        /* 未满时写入 tail=(head+count) mod 200。 */
        target_index = (uint16_t)((store->head + store->count) % SESSION_STORE_MAX_SUMMARIES);
        /* 目标位置在有效区外，无需保存旧内容。 */
        store->summaries[target_index] = *summary;
        /* 增加有效数量。 */
        store->count += 1U;
    } else {
        /* 已满时覆盖最旧 head。 */
        target_index = store->head;
        /* 保存将被覆盖的最旧摘要。 */
        previous_summary = store->summaries[target_index];
        /* 标记需要回滚槽内容。 */
        target_had_previous = true;
        /* 覆盖最旧摘要。 */
        store->summaries[target_index] = *summary;
        /* head 前进一格，新摘要逻辑上位于最新端。 */
        store->head = (uint16_t)((store->head + 1U) % SESSION_STORE_MAX_SUMMARIES);
    }

    /* 首次提交写槽 0；之后始终写非活动槽。 */
    const uint8_t target_slot = (store->active_slot == SESSION_NO_ACTIVE_SLOT) ?
                                0U :
                                (uint8_t)(1U - store->active_slot);
    /* 新代数自然 uint32 回绕，恢复比较使用模序。 */
    const uint32_t target_generation = store->generation + 1U;
    /* 持久化完整候选快照。 */
    const session_store_status_t persist_status = session_persist_snapshot(
        store,
        target_slot,
        target_generation);
    /* 任一 I/O 失败都把内存状态恢复到旧活动槽对应状态。 */
    if (persist_status != SESSION_STORE_STATUS_OK) {
        /* 恢复 head。 */
        store->head = previous_head;
        /* 恢复 count。 */
        store->count = previous_count;
        /* 更新或覆盖路径需要恢复槽内容。 */
        if (target_had_previous) {
            /* 写回旧摘要。 */
            store->summaries[target_index] = previous_summary;
        } else {
            /* 新增路径把未成为有效项的槽清零，方便诊断。 */
            memset(&store->summaries[target_index], 0, sizeof(store->summaries[target_index]));
        }
        /* 介质旧槽和内存旧状态仍一致。 */
        return persist_status;
    }

    /* 最终同步成功后切换活动槽。 */
    store->active_slot = target_slot;
    /* 提交新代数。 */
    store->generation = target_generation;
    /* 告知调用方状态确实变化。 */
    *changed = true;
    /* upsert 成功。 */
    return SESSION_STORE_STATUS_OK;
}

session_store_status_t session_store_get_recent(
    const session_store_t *store,
    const size_t newest_index,
    session_summary_t *summary)
{
    /* store 和输出必须存在。 */
    if ((store == NULL) || (summary == NULL)) {
        /* 空指针错误。 */
        return SESSION_STORE_STATUS_INVALID_ARGUMENT;
    }
    /* 只有初始化对象可查询。 */
    if (!store->initialized) {
        /* 返回状态错误。 */
        return SESSION_STORE_STATUS_INVALID_STATE;
    }
    /* newest_index 必须小于有效数量。 */
    if (newest_index >= store->count) {
        /* 没有对应历史。 */
        return SESSION_STORE_STATUS_NOT_FOUND;
    }

    /* 最新逻辑位置为 count-1，向旧方向减 newest_index。 */
    const size_t logical_index = ((size_t)store->count - 1U) - newest_index;
    /* 映射到环形物理位置。 */
    const uint16_t physical_index = (uint16_t)(((size_t)store->head + logical_index) %
                                               SESSION_STORE_MAX_SUMMARIES);
    /* 返回值拷贝，调用方不能修改内部索引。 */
    *summary = store->summaries[physical_index];
    /* 查询成功。 */
    return SESSION_STORE_STATUS_OK;
}

size_t session_store_count(const session_store_t *store)
{
    /* 空或未初始化对象按 0 条处理，便于只读 UI 安全降级。 */
    if ((store == NULL) || !store->initialized) {
        /* 没有可查询会话。 */
        return 0U;
    }
    /* 返回当前有效数量。 */
    return store->count;
}
