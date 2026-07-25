#ifndef SESSION_STORE_H
#define SESSION_STORE_H

/*
 * 会话摘要与可选原始 IMU 日志的纯 C 存储接口。
 *
 * LittleFS、TF 卡或主机内存只需实现同一个随机读写后端；本组件负责版本化编码、
 * 小端序、CRC32、双槽提交、最近 200 会话轮转和重复事件幂等。布局与恢复公式见
 * docs/会话存储与恢复.md。
 */

/* size_t 表示后端容量、偏移和读写字节数。 */
#include <stddef.h>
/* bool 表示快照是否有效、写入是否改变状态和原始日志是否启用。 */
#include <stdbool.h>
/* 定宽整数保证 ESP32、Windows 主机和文件格式字段宽度一致。 */
#include <stdint.h>

#ifdef __cplusplus
/* C++ 固件按 C ABI 调用，避免名称改编。 */
extern "C" {
#endif

/* 最近会话最多保留 200 条；第 201 条覆盖最旧条目。 */
#define SESSION_STORE_MAX_SUMMARIES 200U
/* 单条摘要线性编码固定 64 字节。 */
#define SESSION_STORE_SUMMARY_WIRE_SIZE 64U
/* 双槽快照头固定 32 字节。 */
#define SESSION_STORE_SNAPSHOT_HEADER_SIZE 32U
/* 每槽末尾固定 4 字节提交标记。 */
#define SESSION_STORE_COMMIT_MARKER_SIZE 4U
/* 单槽固定大小，包含最大 200 条摘要和提交标记。 */
#define SESSION_STORE_SLOT_SIZE \
    (SESSION_STORE_SNAPSHOT_HEADER_SIZE + \
     (SESSION_STORE_MAX_SUMMARIES * SESSION_STORE_SUMMARY_WIRE_SIZE) + \
     SESSION_STORE_COMMIT_MARKER_SIZE)
/* 双槽摘要后端所需最小容量。 */
#define SESSION_STORE_REQUIRED_BACKEND_SIZE (2U * SESSION_STORE_SLOT_SIZE)
/* 原始 IMU 固定六轴顺序 gx、gy、gz、ax、ay、az。 */
#define SESSION_RAW_IMU_AXIS_COUNT 6U
/* 单块最多 64 点，25 Hz 时对应 2.56 秒。 */
#define SESSION_RAW_MAX_SAMPLES_PER_BLOCK 64U
/* 原始块头固定 40 字节。 */
#define SESSION_RAW_BLOCK_HEADER_SIZE 40U
/* 单个原始块最大负载：64 点×6 轴×float32。 */
#define SESSION_RAW_MAX_PAYLOAD_SIZE \
    (SESSION_RAW_MAX_SAMPLES_PER_BLOCK * SESSION_RAW_IMU_AXIS_COUNT * 4U)

/* 领域层返回码；后端错误与格式错误显式区分。 */
typedef enum {
    /* 操作成功。 */
    SESSION_STORE_STATUS_OK = 0,
    /* 指针为空、枚举越界、字段不满足合同。 */
    SESSION_STORE_STATUS_INVALID_ARGUMENT = 1,
    /* 对象未初始化或调用顺序错误。 */
    SESSION_STORE_STATUS_INVALID_STATE = 2,
    /* 注入后端读、写、擦除或同步失败。 */
    SESSION_STORE_STATUS_IO_ERROR = 3,
    /* 后端容量不足或原始日志已写满。 */
    SESSION_STORE_STATUS_NO_SPACE = 4,
    /* 数据魔数、版本、长度、CRC 或提交标记不合法。 */
    SESSION_STORE_STATUS_CORRUPT = 5,
    /* 原始 IMU 日志默认关闭，本次写入被有意拒绝。 */
    SESSION_STORE_STATUS_DISABLED = 6,
    /* 查询索引超出当前会话数量。 */
    SESSION_STORE_STATUS_NOT_FOUND = 7
} session_store_status_t;

/* 注入后端的底层结果，组件会转换为 session_store_status_t。 */
typedef enum {
    /* 后端操作完整成功。 */
    SESSION_BACKEND_OK = 0,
    /* 介质或文件 I/O 失败。 */
    SESSION_BACKEND_IO_ERROR = 1,
    /* offset+length 超出后端容量。 */
    SESSION_BACKEND_OUT_OF_RANGE = 2
} session_backend_result_t;

/* 随机访问后端函数表；LittleFS 文件和 TF 文件可分别实现。 */
typedef struct {
    /* 后端私有上下文；生命周期必须覆盖 session_store_t。 */
    void *context;
    /* 后端总容量，单位字节。 */
    size_t capacity;
    /* 从 offset 读取 length 字节到 output。 */
    session_backend_result_t (*read)(void *context, size_t offset, uint8_t *output, size_t length);
    /* 从 input 向 offset 写入 length 字节。 */
    session_backend_result_t (*write)(void *context, size_t offset, const uint8_t *input, size_t length);
    /* 把 [offset,offset+length) 恢复为空白状态；文件后端可写 0xFF 或截断/重建。 */
    session_backend_result_t (*erase)(void *context, size_t offset, size_t length);
    /* 强制介质同步；提交标记前后均会调用。 */
    session_backend_result_t (*sync)(void *context);
} session_store_backend_t;

/* 固定 64 字节线性格式对应的内存摘要；不直接 memcpy 到介质。 */
typedef struct {
    /* 会话唯一序号，是幂等更新的主键。 */
    uint32_t session_seq;
    /* 已处理的最大 MetricEvent event_seq；小于等于该值的更新视为重复/过期。 */
    uint32_t last_event_seq;
    /* 模型动作索引，范围 0..10。 */
    uint8_t action_id;
    /* 0=次数、1=步数、2=持续毫秒，与 MetricEvent 一致。 */
    uint8_t metric_kind;
    /* 会话标志：完成、异常结束、外部时间有效等由上层按位定义。 */
    uint16_t flags;
    /* UTC Unix 毫秒；0 表示 RTC/PC 尚未校时。 */
    uint64_t start_unix_ms;
    /* 会话持续时间，单位 ms。 */
    uint64_t duration_ms;
    /* 最终次数、步数或持续毫秒，单位由 metric_kind 决定。 */
    uint64_t metric_total;
    /* 毛热量累计，单位 microkcal。 */
    uint64_t gross_microkcal;
    /* 活动热量累计，单位 microkcal。 */
    uint64_t active_microkcal;
    /* 会话平均稳定度 Q15，范围 0..32767。 */
    uint16_t average_stability_q15;
    /* 会话最低稳定度 Q15，范围 0..32767。 */
    uint16_t minimum_stability_q15;
    /* 被摘要吸收的 MetricEvent 数量。 */
    uint32_t event_count;
} session_summary_t;

/* 最近 200 会话的内存索引及双槽元数据。 */
typedef struct {
    /* true 表示 backend 已校验并完成恢复。 */
    bool initialized;
    /* 最近一次成功提交所在槽：0、1；255 表示尚无快照。 */
    uint8_t active_slot;
    /* 当前有效快照代数，uint32 回绕时用模序比较。 */
    uint32_t generation;
    /* 环形数组中最旧会话的位置。 */
    uint16_t head;
    /* 当前有效会话数，范围 0..200。 */
    uint16_t count;
    /* 注入的 LittleFS/内存随机访问后端。 */
    session_store_backend_t backend;
    /* 固定容量会话环；每项约 64 字节，无运行期堆分配。 */
    session_summary_t summaries[SESSION_STORE_MAX_SUMMARIES];
} session_store_t;

/* 主机/模拟器内存后端，支持写入预算和同步故障注入。 */
typedef struct {
    /* 指向调用方拥有的字节数组；生命周期覆盖后端使用期。 */
    uint8_t *data;
    /* 字节数组容量。 */
    size_t capacity;
    /* 剩余可成功写入字节数；SIZE_MAX 表示无限制。 */
    size_t write_budget;
    /* true 时 sync 返回 I/O 错误，用于模拟提交前后断电。 */
    bool fail_sync;
    /* 成功 write 调用次数，供幂等测试确认重复事件未落盘。 */
    uint32_t successful_write_calls;
} session_memory_backend_t;

/* 文件后端上下文；可用于 LittleFS 摘要文件或 TF 原始日志文件。 */
typedef struct {
    /* 指向内部持有的 FILE；头文件保持 void*，避免向所有调用者暴露 stdio 类型。 */
    void *file_handle;
    /* 固定随机访问容量，单位字节。 */
    size_t capacity;
    /* true 表示文件已经打开且预分配完成。 */
    bool initialized;
    /* 保存 UTF-8 路径副本，最多 191 字节加终止符。 */
    char path[192];
} session_file_backend_t;

/* 原始 IMU 块解码出的元数据，不包含 payload 所有权。 */
typedef struct {
    /* 块序号从 0 或调用方指定起点递增。 */
    uint32_t block_seq;
    /* 块内采样点数，范围 1..64。 */
    uint16_t sample_count;
    /* 首个采样点单调毫秒。 */
    uint64_t start_monotonic_ms;
    /* 固定采样周期，单位 us；25 Hz 通常为 40000。 */
    uint32_t sample_period_us;
    /* payload 字节数，必须等于 sample_count×6×4。 */
    uint32_t payload_length;
    /* payload 的 IEEE CRC32。 */
    uint32_t payload_crc32;
} session_raw_block_info_t;

/* 可选原始 IMU 顺序写入器；默认 enabled=false。 */
typedef struct {
    /* true 才允许写 TF/内存后端。 */
    bool enabled;
    /* true 表示 init 已调用。 */
    bool initialized;
    /* 下一块写入偏移，单位字节。 */
    size_t write_offset;
    /* 下一块序号。 */
    uint32_t next_block_seq;
    /* 注入的 TF 文件或内存后端。 */
    session_store_backend_t backend;
} session_raw_log_t;

/* 返回 IEEE CRC32：初值/终值异或均为 0xFFFFFFFF，多项式 0xEDB88320。 */
uint32_t session_store_crc32(const uint8_t *data, size_t length);

/* 返回双槽摘要后端所需最小字节数。 */
size_t session_store_required_backend_size(void);

/* 从双槽后端恢复最近会话；两个槽都无效时建立空索引而不是报损坏。 */
session_store_status_t session_store_init(
    session_store_t *store,
    const session_store_backend_t *backend);

/* 擦除两个槽并同步，清空最近会话和代数。 */
session_store_status_t session_store_format(session_store_t *store);

/*
 * 新增或更新摘要；同 session_seq 且 last_event_seq 不增时 changed=false 且不写介质。
 * 新会话超过 200 条时覆盖最旧条目。
 */
session_store_status_t session_store_upsert(
    session_store_t *store,
    const session_summary_t *summary,
    bool *changed);

/* newest_index=0 返回最新会话，1 返回次新；输出为值拷贝。 */
session_store_status_t session_store_get_recent(
    const session_store_t *store,
    size_t newest_index,
    session_summary_t *summary);

/* 返回当前索引中的有效会话数量。 */
size_t session_store_count(const session_store_t *store);

/* 初始化主机内存后端；erase_now=true 时把全部字节填为 0xFF。 */
session_store_status_t session_memory_backend_init(
    session_memory_backend_t *memory,
    uint8_t *buffer,
    size_t capacity,
    bool erase_now);

/* 生成可注入 session_store/raw_log 的函数表副本。 */
session_store_backend_t session_memory_backend_interface(session_memory_backend_t *memory);

/* 设置还能成功写入多少字节；SIZE_MAX 取消故障注入。 */
void session_memory_backend_set_write_budget(
    session_memory_backend_t *memory,
    size_t write_budget);

/* 打开/关闭 sync 故障注入。 */
void session_memory_backend_set_sync_failure(
    session_memory_backend_t *memory,
    bool fail_sync);

/*
 * 打开或创建固定容量文件；create_if_missing=false 时不存在即返回 I/O 错误。
 * 新文件和扩容区域填充 0xFF，使双槽恢复与擦除语义一致。
 */
session_store_status_t session_file_backend_open(
    session_file_backend_t *file_backend,
    const char *path,
    size_t capacity,
    bool create_if_missing);
/* 刷新并关闭文件；空对象或重复关闭安全无操作。 */
void session_file_backend_close(session_file_backend_t *file_backend);
/* 生成可注入 session_store/raw_log 的函数表副本。 */
session_store_backend_t session_file_backend_interface(session_file_backend_t *file_backend);

/* 初始化原始日志；enabled=false 时 backend 允许为空且所有 append 返回 DISABLED。 */
session_store_status_t session_raw_log_init(
    session_raw_log_t *log,
    const session_store_backend_t *backend,
    bool enabled);

/* 擦除整个原始日志后端并把序号/偏移重置为 0。 */
session_store_status_t session_raw_log_reset(session_raw_log_t *log);

/*
 * 把 [sample_count,6] float32 六轴数组编码为小端块并追加；
 * 轴顺序固定 gx、gy、gz、ax、ay、az，单位由采集协议约定为 deg/s 和 g。
 */
session_store_status_t session_raw_log_append(
    session_raw_log_t *log,
    const float *samples,
    uint16_t sample_count,
    uint64_t start_monotonic_ms,
    uint32_t sample_period_us);

/* 从后端 offset 校验一个完整块的头 CRC、长度和 payload CRC。 */
session_store_status_t session_raw_log_validate_block(
    const session_store_backend_t *backend,
    size_t offset,
    session_raw_block_info_t *info,
    size_t *block_size);

#ifdef __cplusplus
/* 结束 C ABI 声明区。 */
}
#endif

#endif /* SESSION_STORE_H */
