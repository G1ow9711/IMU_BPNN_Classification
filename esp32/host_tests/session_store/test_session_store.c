/* 会话存储主机测试：验证格式、幂等、掉电、损坏回退、轮转和原始块 CRC。 */
#include "session_store.h"

/* assert 提供无第三方依赖的失败定位。 */
#include <assert.h>
/* printf 输出单一成功摘要。 */
#include <stdio.h>
/* memset 构造确定性摘要和原始样本。 */
#include <string.h>

/* 每个 CHECK 统计一个实际执行断言。 */
static unsigned int g_assertion_count = 0U;

/* 执行条件断言并累计计数。 */
#define CHECK(condition)            \
    do {                            \
        /* 记录已执行检查。 */       \
        g_assertion_count += 1U;    \
        /* 条件失败立即终止。 */     \
        assert(condition);          \
    } while (0)

/* 构造字段完整且可预测的会话摘要。 */
static session_summary_t make_summary(
    const uint32_t session_seq,
    const uint32_t last_event_seq,
    const uint8_t action_id,
    const uint64_t metric_total)
{
    /* 清零结构填充和全部字段。 */
    session_summary_t summary;
    /* 确保未显式字段为 0。 */
    memset(&summary, 0, sizeof(summary));
    /* 设置幂等主键。 */
    summary.session_seq = session_seq;
    /* 设置已处理事件水位。 */
    summary.last_event_seq = last_event_seq;
    /* 设置动作索引。 */
    summary.action_id = action_id;
    /* 测试统一使用次数指标。 */
    summary.metric_kind = 0U;
    /* 标记摘要已完成。 */
    summary.flags = 1U;
    /* 构造可区分的 UTC 毫秒。 */
    summary.start_unix_ms = 1700000000000ULL + session_seq;
    /* 每个测试会话持续 60 秒。 */
    summary.duration_ms = 60000ULL;
    /* 设置最终次数。 */
    summary.metric_total = metric_total;
    /* 毛热量使用序号构造可恢复值。 */
    summary.gross_microkcal = ((uint64_t)session_seq) * 1000ULL;
    /* 活动热量同样可区分。 */
    summary.active_microkcal = ((uint64_t)session_seq) * 800ULL;
    /* 平均稳定度位于 Q15 合法范围。 */
    summary.average_stability_q15 = 30000U;
    /* 最低稳定度位于 Q15 合法范围。 */
    summary.minimum_stability_q15 = 25000U;
    /* 事件数量与事件水位一致。 */
    summary.event_count = last_event_seq;
    /* 返回值副本。 */
    return summary;
}

/* 初始化摘要内存后端和 store。 */
static void init_blank_store(
    uint8_t *buffer,
    session_memory_backend_t *memory,
    session_store_backend_t *backend,
    session_store_t *store)
{
    /* 把完整双槽缓冲初始化为 0xFF。 */
    CHECK(session_memory_backend_init(
              memory,
              buffer,
              SESSION_STORE_REQUIRED_BACKEND_SIZE,
              true) == SESSION_STORE_STATUS_OK);
    /* 构造注入函数表。 */
    *backend = session_memory_backend_interface(memory);
    /* 从空白介质建立空索引。 */
    CHECK(session_store_init(store, backend) == SESSION_STORE_STATUS_OK);
    /* 初始数量必须为 0。 */
    CHECK(session_store_count(store) == 0U);
}

/* 验证标准 IEEE CRC32 检查向量。 */
static void test_crc32_known_vector(void)
{
    /* ASCII "123456789" 的标准 CRC32 为 CBF43926。 */
    const uint8_t input[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    /* 检查标准向量。 */
    CHECK(session_store_crc32(input, sizeof(input)) == 0xCBF43926UL);
    /* 空输入标准 CRC32 为 0。 */
    CHECK(session_store_crc32(NULL, 0U) == 0U);
    /* 双槽后端大小必须等于两个固定槽。 */
    CHECK(session_store_required_backend_size() == SESSION_STORE_REQUIRED_BACKEND_SIZE);
}

/* 验证单条摘要写入、重启恢复和字段完整性。 */
static void test_basic_commit_and_recovery(void)
{
    /* 后端字节数组静态分配，避免测试堆行为影响结果。 */
    static uint8_t buffer[SESSION_STORE_REQUIRED_BACKEND_SIZE];
    /* 内存后端控制对象。 */
    session_memory_backend_t memory;
    /* 注入函数表。 */
    session_store_backend_t backend;
    /* 首次启动 store。 */
    session_store_t store;
    /* 建立空白介质。 */
    init_blank_store(buffer, &memory, &backend, &store);

    /* 构造 session 10、event 3、jumping_squat、12 次摘要。 */
    const session_summary_t input = make_summary(10U, 3U, 3U, 12ULL);
    /* 保存是否实际改变。 */
    bool changed = false;
    /* 首次 upsert 必须提交槽 0。 */
    CHECK(session_store_upsert(&store, &input, &changed) == SESSION_STORE_STATUS_OK);
    /* 新摘要应改变状态。 */
    CHECK(changed);
    /* 数量为 1，代数为 1，活动槽为 0。 */
    CHECK((store.count == 1U) && (store.generation == 1U) && (store.active_slot == 0U));

    /* 模拟重启：新 store 复用同一介质。 */
    session_store_t recovered;
    /* 双槽恢复必须成功。 */
    CHECK(session_store_init(&recovered, &backend) == SESSION_STORE_STATUS_OK);
    /* 恢复 1 条记录。 */
    CHECK(session_store_count(&recovered) == 1U);
    /* 读取最新摘要。 */
    session_summary_t output;
    /* newest_index 0 必须存在。 */
    CHECK(session_store_get_recent(&recovered, 0U, &output) == SESSION_STORE_STATUS_OK);
    /* 主键、事件水位和次数完整恢复。 */
    CHECK((output.session_seq == 10U) &&
          (output.last_event_seq == 3U) &&
          (output.metric_total == 12ULL));
    /* 热量字段完整恢复。 */
    CHECK((output.gross_microkcal == input.gross_microkcal) &&
          (output.active_microkcal == input.active_microkcal));
}

/* 验证重复/过期 event_seq 不写介质，新 event_seq 才更新。 */
static void test_idempotent_upsert(void)
{
    /* 独立双槽缓冲。 */
    static uint8_t buffer[SESSION_STORE_REQUIRED_BACKEND_SIZE];
    /* 内存后端。 */
    session_memory_backend_t memory;
    /* 函数表。 */
    session_store_backend_t backend;
    /* store。 */
    session_store_t store;
    /* 初始化。 */
    init_blank_store(buffer, &memory, &backend, &store);

    /* 首次摘要 event_seq=5。 */
    session_summary_t summary = make_summary(20U, 5U, 6U, 5ULL);
    /* 保存 changed。 */
    bool changed = false;
    /* 首次写入。 */
    CHECK(session_store_upsert(&store, &summary, &changed) == SESSION_STORE_STATUS_OK);
    /* 确认改变。 */
    CHECK(changed);
    /* 保存写调用和代数。 */
    const uint32_t writes_after_first = memory.successful_write_calls;
    /* 保存代数。 */
    const uint32_t generation_after_first = store.generation;

    /* 完全重复摘要再次提交。 */
    CHECK(session_store_upsert(&store, &summary, &changed) == SESSION_STORE_STATUS_OK);
    /* 重复事件幂等且 changed=false。 */
    CHECK(!changed);
    /* 不产生任何介质 write。 */
    CHECK(memory.successful_write_calls == writes_after_first);
    /* 代数不增加。 */
    CHECK(store.generation == generation_after_first);

    /* 更旧 event_seq=4 也必须忽略。 */
    summary.last_event_seq = 4U;
    /* 修改 metric_total 试图覆盖也不允许。 */
    summary.metric_total = 999ULL;
    /* 过期更新幂等成功。 */
    CHECK(session_store_upsert(&store, &summary, &changed) == SESSION_STORE_STATUS_OK);
    /* 不改变。 */
    CHECK(!changed);
    /* 仍无写调用。 */
    CHECK(memory.successful_write_calls == writes_after_first);

    /* 新 event_seq=6 才允许更新。 */
    summary.last_event_seq = 6U;
    /* 新总次数 6。 */
    summary.metric_total = 6ULL;
    /* 更新应提交另一槽。 */
    CHECK(session_store_upsert(&store, &summary, &changed) == SESSION_STORE_STATUS_OK);
    /* 状态改变。 */
    CHECK(changed);
    /* 代数递增 1。 */
    CHECK(store.generation == generation_after_first + 1U);
    /* 最新摘要反映 event 6。 */
    session_summary_t output;
    /* 读取最新。 */
    CHECK(session_store_get_recent(&store, 0U, &output) == SESSION_STORE_STATUS_OK);
    /* 新事件水位和总值生效。 */
    CHECK((output.last_event_seq == 6U) && (output.metric_total == 6ULL));
}

/* 验证写到半条记录断电后，重启仍选择旧活动槽。 */
static void test_power_loss_recovery(void)
{
    /* 独立介质。 */
    static uint8_t buffer[SESSION_STORE_REQUIRED_BACKEND_SIZE];
    /* 内存后端。 */
    session_memory_backend_t memory;
    /* 函数表。 */
    session_store_backend_t backend;
    /* store。 */
    session_store_t store;
    /* 初始化。 */
    init_blank_store(buffer, &memory, &backend, &store);

    /* 先成功提交会话 1 到槽 0。 */
    const session_summary_t first = make_summary(1U, 1U, 6U, 10ULL);
    /* changed 标记。 */
    bool changed = false;
    /* 首次提交成功。 */
    CHECK(session_store_upsert(&store, &first, &changed) == SESSION_STORE_STATUS_OK);
    /* 活动槽 0。 */
    CHECK(store.active_slot == 0U);

    /* 只允许写 32 字节头和下一条记录的 8 字节前缀。 */
    session_memory_backend_set_write_budget(&memory, 40U);
    /* 第二会话会写非活动槽 1。 */
    const session_summary_t second = make_summary(2U, 1U, 3U, 20ULL);
    /* 中途写失败。 */
    CHECK(session_store_upsert(&store, &second, &changed) == SESSION_STORE_STATUS_IO_ERROR);
    /* I/O 失败在内存中回滚，changed 仍 false。 */
    CHECK(!changed);
    /* 旧会话仍是唯一条目。 */
    CHECK(session_store_count(&store) == 1U);

    /* 模拟重启前取消故障预算；读取不受预算影响，但后续可继续用。 */
    session_memory_backend_set_write_budget(&memory, SIZE_MAX);
    /* 新 store 从同一缓冲恢复。 */
    session_store_t recovered;
    /* 必须忽略无提交标记的撕裂槽 1。 */
    CHECK(session_store_init(&recovered, &backend) == SESSION_STORE_STATUS_OK);
    /* 只恢复旧槽 1 条记录。 */
    CHECK(session_store_count(&recovered) == 1U);
    /* 读取最新。 */
    session_summary_t output;
    /* 查询成功。 */
    CHECK(session_store_get_recent(&recovered, 0U, &output) == SESSION_STORE_STATUS_OK);
    /* 会话 1 未丢失，会话 2 未被误认为已提交。 */
    CHECK(output.session_seq == 1U);
}

/* 验证最新槽 payload 损坏时自动回退上一代有效槽。 */
static void test_corrupt_newest_slot_fallback(void)
{
    /* 独立介质。 */
    static uint8_t buffer[SESSION_STORE_REQUIRED_BACKEND_SIZE];
    /* 内存后端。 */
    session_memory_backend_t memory;
    /* 函数表。 */
    session_store_backend_t backend;
    /* store。 */
    session_store_t store;
    /* 初始化。 */
    init_blank_store(buffer, &memory, &backend, &store);

    /* 代数 1 写槽 0。 */
    const session_summary_t first = make_summary(11U, 1U, 0U, 1ULL);
    /* 代数 2 写槽 1，并含两个会话。 */
    const session_summary_t second = make_summary(12U, 1U, 1U, 2ULL);
    /* changed。 */
    bool changed = false;
    /* 提交第一代。 */
    CHECK(session_store_upsert(&store, &first, &changed) == SESSION_STORE_STATUS_OK);
    /* 提交第二代。 */
    CHECK(session_store_upsert(&store, &second, &changed) == SESSION_STORE_STATUS_OK);
    /* 最新活动槽应为 1。 */
    CHECK(store.active_slot == 1U);

    /* 翻转槽 1 第一条 payload 的一个字节，破坏记录版本和 payload CRC。 */
    const size_t corrupt_offset = SESSION_STORE_SLOT_SIZE + SESSION_STORE_SNAPSHOT_HEADER_SIZE;
    /* 执行单字节损坏。 */
    buffer[corrupt_offset] ^= 0x01U;

    /* 模拟重启。 */
    session_store_t recovered;
    /* 应选择仍有效的槽 0，而不是整体失败。 */
    CHECK(session_store_init(&recovered, &backend) == SESSION_STORE_STATUS_OK);
    /* 槽 0 只有第一条。 */
    CHECK(session_store_count(&recovered) == 1U);
    /* 读取恢复摘要。 */
    session_summary_t output;
    /* 最新条目存在。 */
    CHECK(session_store_get_recent(&recovered, 0U, &output) == SESSION_STORE_STATUS_OK);
    /* 回退到会话 11。 */
    CHECK(output.session_seq == 11U);
}

/* 验证超过 200 条后按最旧优先轮转，并能跨重启恢复顺序。 */
static void test_capacity_rotation(void)
{
    /* 独立介质。 */
    static uint8_t buffer[SESSION_STORE_REQUIRED_BACKEND_SIZE];
    /* 内存后端。 */
    session_memory_backend_t memory;
    /* 函数表。 */
    session_store_backend_t backend;
    /* store。 */
    session_store_t store;
    /* 初始化。 */
    init_blank_store(buffer, &memory, &backend, &store);

    /* 连续提交 205 个不同会话。 */
    for (uint32_t session_seq = 1U; session_seq <= 205U; ++session_seq) {
        /* 每条使用 event_seq=1 和可区分总值。 */
        const session_summary_t summary = make_summary(
            session_seq,
            1U,
            (uint8_t)(session_seq % 11U),
            session_seq);
        /* 保存 changed。 */
        bool changed = false;
        /* 每条都应成功提交。 */
        CHECK(session_store_upsert(&store, &summary, &changed) == SESSION_STORE_STATUS_OK);
        /* 新主键必然改变。 */
        CHECK(changed);
    }
    /* 固定容量保持 200。 */
    CHECK(session_store_count(&store) == SESSION_STORE_MAX_SUMMARIES);
    /* 最新应为 205。 */
    session_summary_t output;
    /* 查询最新。 */
    CHECK(session_store_get_recent(&store, 0U, &output) == SESSION_STORE_STATUS_OK);
    /* 验证最新主键。 */
    CHECK(output.session_seq == 205U);
    /* 第 200 新即最旧保留项应为 6，1..5 已轮转淘汰。 */
    CHECK(session_store_get_recent(&store, 199U, &output) == SESSION_STORE_STATUS_OK);
    /* 验证最旧保留主键。 */
    CHECK(output.session_seq == 6U);

    /* 重启恢复后顺序必须相同。 */
    session_store_t recovered;
    /* 恢复成功。 */
    CHECK(session_store_init(&recovered, &backend) == SESSION_STORE_STATUS_OK);
    /* 数量仍为 200。 */
    CHECK(session_store_count(&recovered) == SESSION_STORE_MAX_SUMMARIES);
    /* 查询最新。 */
    CHECK(session_store_get_recent(&recovered, 0U, &output) == SESSION_STORE_STATUS_OK);
    /* 最新仍为 205。 */
    CHECK(output.session_seq == 205U);
    /* 查询最旧。 */
    CHECK(session_store_get_recent(&recovered, 199U, &output) == SESSION_STORE_STATUS_OK);
    /* 最旧仍为 6。 */
    CHECK(output.session_seq == 6U);
}

/* 验证原始 IMU 日志默认关闭、块布局、CRC 和损坏检测。 */
static void test_raw_log(void)
{
    /* 默认关闭日志不需要后端。 */
    session_raw_log_t disabled_log;
    /* 关闭初始化必须成功。 */
    CHECK(session_raw_log_init(&disabled_log, NULL, false) == SESSION_STORE_STATUS_OK);
    /* 构造一行六轴测试值。 */
    const float one_sample[SESSION_RAW_IMU_AXIS_COUNT] = {1.0F, 2.0F, 3.0F, 0.1F, 0.2F, 1.0F};
    /* 关闭状态 append 必须返回 DISABLED 且不访问 NULL 后端。 */
    CHECK(session_raw_log_append(&disabled_log, one_sample, 1U, 1000ULL, 40000U) ==
          SESSION_STORE_STATUS_DISABLED);

    /* 为启用日志分配 4096 字节内存介质。 */
    static uint8_t raw_buffer[4096];
    /* 内存后端控制对象。 */
    session_memory_backend_t memory;
    /* 初始化为空白。 */
    CHECK(session_memory_backend_init(&memory, raw_buffer, sizeof(raw_buffer), true) ==
          SESSION_STORE_STATUS_OK);
    /* 构造后端函数表。 */
    const session_store_backend_t backend = session_memory_backend_interface(&memory);
    /* 启用原始日志。 */
    session_raw_log_t log;
    /* 初始化成功。 */
    CHECK(session_raw_log_init(&log, &backend, true) == SESSION_STORE_STATUS_OK);

    /* 两点×六轴，平铺顺序为 [sample,axis]。 */
    const float samples[2U * SESSION_RAW_IMU_AXIS_COUNT] = {
        10.0F, 20.0F, 30.0F, 0.0F, 0.0F, 1.0F,
        11.0F, 21.0F, 31.0F, 0.1F, 0.2F, 0.9F
    };
    /* 追加 2 点、25 Hz 块。 */
    CHECK(session_raw_log_append(&log, samples, 2U, 5000ULL, 40000U) ==
          SESSION_STORE_STATUS_OK);
    /* 第一个块序号后 next 应为 1。 */
    CHECK(log.next_block_seq == 1U);
    /* write_offset 应为 40+2×6×4=88。 */
    CHECK(log.write_offset == 88U);

    /* 验证块头和 payload CRC。 */
    session_raw_block_info_t info;
    /* 保存完整块长度。 */
    size_t block_size = 0U;
    /* offset 0 块必须有效。 */
    CHECK(session_raw_log_validate_block(&backend, 0U, &info, &block_size) ==
          SESSION_STORE_STATUS_OK);
    /* 元数据必须与 append 输入一致。 */
    CHECK((info.block_seq == 0U) &&
          (info.sample_count == 2U) &&
          (info.start_monotonic_ms == 5000ULL) &&
          (info.sample_period_us == 40000U));
    /* payload 48 字节，完整块 88 字节。 */
    CHECK((info.payload_length == 48U) && (block_size == 88U));

    /* 翻转 payload 第 4 个字节，头仍有效但 payload CRC 应失败。 */
    raw_buffer[SESSION_RAW_BLOCK_HEADER_SIZE + 3U] ^= 0x80U;
    /* 损坏块必须返回 CORRUPT。 */
    CHECK(session_raw_log_validate_block(&backend, 0U, &info, &block_size) ==
          SESSION_STORE_STATUS_CORRUPT);
}

/* 测试入口。 */
int main(void)
{
    /* 验证 CRC 和容量常量。 */
    test_crc32_known_vector();
    /* 验证基本提交/恢复。 */
    test_basic_commit_and_recovery();
    /* 验证 event_seq 幂等。 */
    test_idempotent_upsert();
    /* 验证中途断电恢复。 */
    test_power_loss_recovery();
    /* 验证新槽损坏回退。 */
    test_corrupt_newest_slot_fallback();
    /* 验证 200 条容量轮转。 */
    test_capacity_rotation();
    /* 验证原始 IMU 默认关闭和 CRC。 */
    test_raw_log();

    /* 输出成功摘要。 */
    printf("session_store host tests: PASS (%u assertions)\n", g_assertion_count);
    /* 0 表示全部测试通过。 */
    return 0;
}
