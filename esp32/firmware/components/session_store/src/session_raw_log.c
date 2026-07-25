/* 可选原始六轴 IMU 小端二进制块写入与 CRC 校验。 */
#include "session_store.h"

/* memcpy 在 float 与 uint32 位模式间安全转换，避免别名违规。 */
#include <string.h>

/* 原始块魔数按小端字节呈现为 "IMU1"。 */
#define SESSION_RAW_MAGIC 0x31554D49UL
/* 原始块布局版本。 */
#define SESSION_RAW_VERSION 1U
/* 通道数固定 6。 */
#define SESSION_RAW_CHANNEL_COUNT 6U
/* 样本格式 1 表示 IEEE754 float32 小端。 */
#define SESSION_RAW_SAMPLE_FORMAT_FLOAT32 1U

/* 原始日志本地后端完整性检查。 */
static bool session_raw_backend_is_valid(const session_store_backend_t *backend)
{
    /* append/reset/validate 需要全部随机访问与同步操作。 */
    return (backend != NULL) &&
           (backend->read != NULL) &&
           (backend->write != NULL) &&
           (backend->erase != NULL) &&
           (backend->sync != NULL) &&
           (backend->capacity >= (SESSION_RAW_BLOCK_HEADER_SIZE + 24U));
}

/* 小端写 16 位整数。 */
static void session_raw_write_u16_le(uint8_t *output, const uint16_t value)
{
    /* 写低字节。 */
    output[0] = (uint8_t)(value & 0xFFU);
    /* 写高字节。 */
    output[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

/* 小端写 32 位整数。 */
static void session_raw_write_u32_le(uint8_t *output, const uint32_t value)
{
    /* 固定写 4 字节。 */
    for (uint8_t index = 0U; index < 4U; ++index) {
        /* 当前字节保存对应低位。 */
        output[index] = (uint8_t)((value >> (8U * index)) & 0xFFU);
    }
}

/* 小端写 64 位整数。 */
static void session_raw_write_u64_le(uint8_t *output, const uint64_t value)
{
    /* 固定写 8 字节。 */
    for (uint8_t index = 0U; index < 8U; ++index) {
        /* 当前字节保存对应低位。 */
        output[index] = (uint8_t)((value >> (8U * index)) & 0xFFULL);
    }
}

/* 小端读 16 位整数。 */
static uint16_t session_raw_read_u16_le(const uint8_t *input)
{
    /* 组合低、高字节。 */
    return (uint16_t)input[0] | ((uint16_t)input[1] << 8U);
}

/* 小端读 32 位整数。 */
static uint32_t session_raw_read_u32_le(const uint8_t *input)
{
    /* 初始结果为 0。 */
    uint32_t value = 0U;
    /* 纳入 4 个字节。 */
    for (uint8_t index = 0U; index < 4U; ++index) {
        /* 按小端位移合并。 */
        value |= ((uint32_t)input[index]) << (8U * index);
    }
    /* 返回结果。 */
    return value;
}

/* 小端读 64 位整数。 */
static uint64_t session_raw_read_u64_le(const uint8_t *input)
{
    /* 初始结果为 0。 */
    uint64_t value = 0ULL;
    /* 纳入 8 个字节。 */
    for (uint8_t index = 0U; index < 8U; ++index) {
        /* 按小端位移合并。 */
        value |= ((uint64_t)input[index]) << (8U * index);
    }
    /* 返回结果。 */
    return value;
}

/* 把 float32 的 IEEE 位模式按小端编码。 */
static void session_raw_encode_float(float value, uint8_t output[4])
{
    /* C11 目标要求 float 为 4 字节；运行期用数组大小合同校验。 */
    uint32_t bits = 0U;
    /* memcpy 避免通过不兼容指针读取产生未定义行为。 */
    memcpy(&bits, &value, sizeof(bits));
    /* 按小端写出位模式。 */
    session_raw_write_u32_le(output, bits);
}

/* 把后端错误映射为原始日志状态。 */
static session_store_status_t session_raw_map_backend(const session_backend_result_t result)
{
    /* 成功保持成功。 */
    if (result == SESSION_BACKEND_OK) {
        /* 返回成功。 */
        return SESSION_STORE_STATUS_OK;
    }
    /* 越界表示空间不足。 */
    if (result == SESSION_BACKEND_OUT_OF_RANGE) {
        /* 返回空间不足。 */
        return SESSION_STORE_STATUS_NO_SPACE;
    }
    /* 其它错误视为 I/O 失败。 */
    return SESSION_STORE_STATUS_IO_ERROR;
}

session_store_status_t session_raw_log_init(
    session_raw_log_t *log,
    const session_store_backend_t *backend,
    const bool enabled)
{
    /* log 对象必须存在。 */
    if (log == NULL) {
        /* 空指针错误。 */
        return SESSION_STORE_STATUS_INVALID_ARGUMENT;
    }
    /* 开启原始日志时必须提供完整后端。 */
    if (enabled && !session_raw_backend_is_valid(backend)) {
        /* 拒绝会在首块写入时失败的配置。 */
        return SESSION_STORE_STATUS_INVALID_ARGUMENT;
    }

    /* 清除偏移、序号和旧后端。 */
    memset(log, 0, sizeof(*log));
    /* 保存默认关闭/显式开启状态。 */
    log->enabled = enabled;
    /* 标记 init 已调用。 */
    log->initialized = true;
    /* 只有启用时复制后端；关闭时允许 backend=NULL。 */
    if (enabled) {
        /* 保存函数表副本。 */
        log->backend = *backend;
    }
    /* 初始化成功。 */
    return SESSION_STORE_STATUS_OK;
}

session_store_status_t session_raw_log_reset(session_raw_log_t *log)
{
    /* log 必须存在。 */
    if (log == NULL) {
        /* 空指针错误。 */
        return SESSION_STORE_STATUS_INVALID_ARGUMENT;
    }
    /* 未初始化不能重置。 */
    if (!log->initialized) {
        /* 返回状态错误。 */
        return SESSION_STORE_STATUS_INVALID_STATE;
    }
    /* 默认关闭时拒绝介质操作。 */
    if (!log->enabled) {
        /* 明确返回关闭。 */
        return SESSION_STORE_STATUS_DISABLED;
    }

    /* 擦除整个日志后端。 */
    session_backend_result_t result = log->backend.erase(
        log->backend.context,
        0U,
        log->backend.capacity);
    /* 擦除失败不重置内存偏移。 */
    if (result != SESSION_BACKEND_OK) {
        /* 映射后端错误。 */
        return session_raw_map_backend(result);
    }
    /* 强制空白介质落盘。 */
    result = log->backend.sync(log->backend.context);
    /* 同步失败不声称完成。 */
    if (result != SESSION_BACKEND_OK) {
        /* 映射 I/O 错误。 */
        return session_raw_map_backend(result);
    }
    /* 下一块从 offset 0 写。 */
    log->write_offset = 0U;
    /* 下一块序号回到 0。 */
    log->next_block_seq = 0U;
    /* 重置成功。 */
    return SESSION_STORE_STATUS_OK;
}

session_store_status_t session_raw_log_append(
    session_raw_log_t *log,
    const float *samples,
    const uint16_t sample_count,
    const uint64_t start_monotonic_ms,
    const uint32_t sample_period_us)
{
    /* 对象和初始化状态必须有效。 */
    if (log == NULL) {
        /* 空指针错误。 */
        return SESSION_STORE_STATUS_INVALID_ARGUMENT;
    }
    if (!log->initialized) {
        /* 要求先 init。 */
        return SESSION_STORE_STATUS_INVALID_STATE;
    }
    /* 默认关闭时不写任何字节。 */
    if (!log->enabled) {
        /* 返回显式关闭。 */
        return SESSION_STORE_STATUS_DISABLED;
    }
    /* 样本指针、数量、周期和 float 大小必须满足格式合同。 */
    if ((samples == NULL) || (sample_count == 0U) ||
        (sample_count > SESSION_RAW_MAX_SAMPLES_PER_BLOCK) ||
        (sample_period_us == 0U) || (sizeof(float) != 4U)) {
        /* 拒绝无法编码的输入。 */
        return SESSION_STORE_STATUS_INVALID_ARGUMENT;
    }

    /* payload 长度 = 点数×6轴×4字节。 */
    const uint32_t payload_length = ((uint32_t)sample_count) *
                                    SESSION_RAW_IMU_AXIS_COUNT *
                                    4U;
    /* 完整块长度 = 40 字节头 + payload。 */
    const size_t block_size = SESSION_RAW_BLOCK_HEADER_SIZE + ((size_t)payload_length);
    /* 用减法检查剩余空间，避免 offset+size 溢出。 */
    if ((log->write_offset > log->backend.capacity) ||
        (block_size > (log->backend.capacity - log->write_offset))) {
        /* 后端写满，调用方可轮转新 TF 文件。 */
        return SESSION_STORE_STATUS_NO_SPACE;
    }

    /* 先按实际小端 payload 逐 float 计算 CRC。 */
    uint32_t crc_state = 0xFFFFFFFFUL;
    /* 单个 float 编码缓冲。 */
    uint8_t encoded_float[4];
    /* 总 float 数量为 sample_count×6。 */
    const size_t value_count = ((size_t)sample_count) * SESSION_RAW_IMU_AXIS_COUNT;
    /* 遍历 [sample_count,6] 平铺数组。 */
    for (size_t value_index = 0U; value_index < value_count; ++value_index) {
        /* 编码当前 float 位模式。 */
        session_raw_encode_float(samples[value_index], encoded_float);
        /* 本文件复用公开 CRC 需要完整缓冲；这里手动逐字节执行同一算法。 */
        for (uint8_t byte_index = 0U; byte_index < 4U; ++byte_index) {
            /* 把当前字节纳入 CRC 低位。 */
            crc_state ^= encoded_float[byte_index];
            /* 每字节迭代 8 位。 */
            for (uint8_t bit_index = 0U; bit_index < 8U; ++bit_index) {
                /* 保存反馈位。 */
                const uint32_t lsb = crc_state & 1U;
                /* CRC 右移。 */
                crc_state >>= 1U;
                /* 反馈位为 1 时异或 IEEE 多项式。 */
                if (lsb != 0U) {
                    /* 纳入反馈。 */
                    crc_state ^= 0xEDB88320UL;
                }
            }
        }
    }
    /* 执行最终异或得到 payload CRC。 */
    const uint32_t payload_crc = crc_state ^ 0xFFFFFFFFUL;

    /* 构造 40 字节小端头。 */
    uint8_t header[SESSION_RAW_BLOCK_HEADER_SIZE];
    /* 清零 CRC 占位和保留字段。 */
    memset(header, 0, sizeof(header));
    /* offset 0：魔数 IMU1。 */
    session_raw_write_u32_le(&header[0], SESSION_RAW_MAGIC);
    /* offset 4：版本。 */
    session_raw_write_u16_le(&header[4], SESSION_RAW_VERSION);
    /* offset 6：头长度 40。 */
    session_raw_write_u16_le(&header[6], SESSION_RAW_BLOCK_HEADER_SIZE);
    /* offset 8：块序号。 */
    session_raw_write_u32_le(&header[8], log->next_block_seq);
    /* offset 12：采样点数。 */
    session_raw_write_u16_le(&header[12], sample_count);
    /* offset 14：固定 6 通道。 */
    header[14] = SESSION_RAW_CHANNEL_COUNT;
    /* offset 15：float32 格式。 */
    header[15] = SESSION_RAW_SAMPLE_FORMAT_FLOAT32;
    /* offset 16：首点单调毫秒。 */
    session_raw_write_u64_le(&header[16], start_monotonic_ms);
    /* offset 24：采样周期 us。 */
    session_raw_write_u32_le(&header[24], sample_period_us);
    /* offset 28：payload 长度。 */
    session_raw_write_u32_le(&header[28], payload_length);
    /* offset 32：payload CRC。 */
    session_raw_write_u32_le(&header[32], payload_crc);
    /* offset 36 暂为 0，按全头计算头 CRC。 */
    const uint32_t header_crc = session_store_crc32(header, sizeof(header));
    /* offset 36：头 CRC。 */
    session_raw_write_u32_le(&header[36], header_crc);

    /* 写头。 */
    session_backend_result_t result = log->backend.write(
        log->backend.context,
        log->write_offset,
        header,
        sizeof(header));
    /* 头写失败时偏移不前进。 */
    if (result != SESSION_BACKEND_OK) {
        /* 映射错误。 */
        return session_raw_map_backend(result);
    }

    /* payload 写入位置紧随头。 */
    size_t payload_offset = log->write_offset + SESSION_RAW_BLOCK_HEADER_SIZE;
    /* 逐 float 写 4 字节，避免 1536 字节临时栈缓冲。 */
    for (size_t value_index = 0U; value_index < value_count; ++value_index) {
        /* 编码当前值。 */
        session_raw_encode_float(samples[value_index], encoded_float);
        /* 写 4 字节。 */
        result = log->backend.write(
            log->backend.context,
            payload_offset,
            encoded_float,
            sizeof(encoded_float));
        /* 中途失败留下 CRC 无效尾块，扫描器会停止。 */
        if (result != SESSION_BACKEND_OK) {
            /* 偏移和序号保持不变，允许重试覆盖尾块。 */
            return session_raw_map_backend(result);
        }
        /* 前进 4 字节。 */
        payload_offset += sizeof(encoded_float);
    }

    /* 同步完整块。 */
    result = log->backend.sync(log->backend.context);
    /* 同步失败时不推进逻辑偏移。 */
    if (result != SESSION_BACKEND_OK) {
        /* 返回 I/O 错误。 */
        return session_raw_map_backend(result);
    }

    /* 只有完整同步后才推进写偏移。 */
    log->write_offset += block_size;
    /* 下一块序号递增，uint32 自然回绕。 */
    log->next_block_seq += 1U;
    /* 追加成功。 */
    return SESSION_STORE_STATUS_OK;
}

session_store_status_t session_raw_log_validate_block(
    const session_store_backend_t *backend,
    const size_t offset,
    session_raw_block_info_t *info,
    size_t *block_size)
{
    /* 后端和输出必须有效。 */
    if (!session_raw_backend_is_valid(backend) || (info == NULL) || (block_size == NULL)) {
        /* 拒绝不完整配置。 */
        return SESSION_STORE_STATUS_INVALID_ARGUMENT;
    }
    /* 至少要有完整头。 */
    if ((offset > backend->capacity) ||
        (SESSION_RAW_BLOCK_HEADER_SIZE > (backend->capacity - offset))) {
        /* 介质尾部不足一个头。 */
        return SESSION_STORE_STATUS_NO_SPACE;
    }

    /* 读取固定头。 */
    uint8_t header[SESSION_RAW_BLOCK_HEADER_SIZE];
    /* 执行后端读取。 */
    session_backend_result_t result = backend->read(
        backend->context,
        offset,
        header,
        sizeof(header));
    /* 读取失败。 */
    if (result != SESSION_BACKEND_OK) {
        /* 映射错误。 */
        return session_raw_map_backend(result);
    }

    /* 保存介质头 CRC。 */
    const uint32_t stored_header_crc = session_raw_read_u32_le(&header[36]);
    /* 清零 CRC 字段以重算。 */
    memset(&header[36], 0, 4U);
    /* 重算头 CRC。 */
    const uint32_t computed_header_crc = session_store_crc32(header, sizeof(header));
    /* 校验魔数、版本、长度、通道、格式和 CRC。 */
    if ((session_raw_read_u32_le(&header[0]) != SESSION_RAW_MAGIC) ||
        (session_raw_read_u16_le(&header[4]) != SESSION_RAW_VERSION) ||
        (session_raw_read_u16_le(&header[6]) != SESSION_RAW_BLOCK_HEADER_SIZE) ||
        (header[14] != SESSION_RAW_CHANNEL_COUNT) ||
        (header[15] != SESSION_RAW_SAMPLE_FORMAT_FLOAT32) ||
        (stored_header_crc != computed_header_crc)) {
        /* 头损坏或版本不兼容。 */
        return SESSION_STORE_STATUS_CORRUPT;
    }

    /* 解码采样数和 payload 长度。 */
    const uint16_t sample_count = session_raw_read_u16_le(&header[12]);
    /* 解码 payload 长度。 */
    const uint32_t payload_length = session_raw_read_u32_le(&header[28]);
    /* 数量和长度必须精确一致。 */
    if ((sample_count == 0U) || (sample_count > SESSION_RAW_MAX_SAMPLES_PER_BLOCK) ||
        (payload_length != (((uint32_t)sample_count) * SESSION_RAW_IMU_AXIS_COUNT * 4U))) {
        /* 拒绝可能越界的块。 */
        return SESSION_STORE_STATUS_CORRUPT;
    }

    /* 计算完整块长度。 */
    const size_t decoded_block_size = SESSION_RAW_BLOCK_HEADER_SIZE + ((size_t)payload_length);
    /* 确认 payload 完整位于后端。 */
    if (decoded_block_size > (backend->capacity - offset)) {
        /* 文件尾块被截断。 */
        return SESSION_STORE_STATUS_CORRUPT;
    }

    /* 流式读取 payload，固定 64 字节栈缓冲。 */
    uint8_t chunk[64];
    /* CRC 初值全 1。 */
    uint32_t crc_state = 0xFFFFFFFFUL;
    /* 已验证 payload 字节数。 */
    size_t processed = 0U;
    /* 遍历完整 payload。 */
    while (processed < payload_length) {
        /* 本次读取取剩余与 64 的较小值。 */
        const size_t remaining = ((size_t)payload_length) - processed;
        /* 计算当前块长度。 */
        const size_t chunk_length = (remaining < sizeof(chunk)) ? remaining : sizeof(chunk);
        /* 从 payload 对应位置读取。 */
        result = backend->read(
            backend->context,
            offset + SESSION_RAW_BLOCK_HEADER_SIZE + processed,
            chunk,
            chunk_length);
        /* 读取失败。 */
        if (result != SESSION_BACKEND_OK) {
            /* 映射错误。 */
            return session_raw_map_backend(result);
        }
        /* 逐字节更新 CRC。 */
        for (size_t byte_index = 0U; byte_index < chunk_length; ++byte_index) {
            /* 纳入当前字节。 */
            crc_state ^= chunk[byte_index];
            /* 每字节迭代 8 位。 */
            for (uint8_t bit_index = 0U; bit_index < 8U; ++bit_index) {
                /* 保存反馈位。 */
                const uint32_t lsb = crc_state & 1U;
                /* 右移 CRC。 */
                crc_state >>= 1U;
                /* 反馈位为 1 时异或多项式。 */
                if (lsb != 0U) {
                    /* 纳入反馈。 */
                    crc_state ^= 0xEDB88320UL;
                }
            }
        }
        /* 前进已处理字节数。 */
        processed += chunk_length;
    }
    /* 最终异或。 */
    const uint32_t computed_payload_crc = crc_state ^ 0xFFFFFFFFUL;
    /* 与头声明 CRC 比较。 */
    const uint32_t stored_payload_crc = session_raw_read_u32_le(&header[32]);
    /* payload 损坏时拒绝。 */
    if (computed_payload_crc != stored_payload_crc) {
        /* 返回损坏。 */
        return SESSION_STORE_STATUS_CORRUPT;
    }

    /* 清零输出结构填充。 */
    memset(info, 0, sizeof(*info));
    /* 返回块序号。 */
    info->block_seq = session_raw_read_u32_le(&header[8]);
    /* 返回采样数。 */
    info->sample_count = sample_count;
    /* 返回首点时间。 */
    info->start_monotonic_ms = session_raw_read_u64_le(&header[16]);
    /* 返回采样周期。 */
    info->sample_period_us = session_raw_read_u32_le(&header[24]);
    /* 返回 payload 长度。 */
    info->payload_length = payload_length;
    /* 返回 payload CRC。 */
    info->payload_crc32 = stored_payload_crc;
    /* 返回完整块长度，供扫描下一块。 */
    *block_size = decoded_block_size;
    /* 块完整有效。 */
    return SESSION_STORE_STATUS_OK;
}
