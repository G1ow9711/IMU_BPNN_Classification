/* 引入文件后端与双槽会话存储合同。 */
#include "session_store.h"

/* 引入 stdio 输出错误和成功摘要。 */
#include <stdio.h>
/* 引入 string.h 构造过长路径和摘要。 */
#include <string.h>

/* 保存断言总数。 */
static unsigned int g_assertions = 0U;

/* 失败时打印表达式和行号并返回非零。 */
#define CHECK(expression)                                                       \
    do {                                                                        \
        g_assertions += 1U;                                                      \
        if (!(expression)) {                                                    \
            (void)fprintf(stderr, "CHECK failed line=%d: %s\n", __LINE__, #expression); \
            return 1;                                                           \
        }                                                                       \
    } while (0)

/* 主函数从脚本接收项目本地临时文件路径。 */
int main(const int argc, char *argv[])
{
    /* 必须提供一个测试文件路径。 */
    CHECK(argc == 2);
    /* 路径指针必须有效。 */
    CHECK(argv[1] != NULL);

    /* 创建文件后端上下文。 */
    session_file_backend_t file_backend;
    /* 打开或创建固定双槽容量文件。 */
    CHECK(session_file_backend_open(
              &file_backend,
              argv[1],
              session_store_required_backend_size(),
              true) == SESSION_STORE_STATUS_OK);
    /* 生成通用后端函数表。 */
    session_store_backend_t backend = session_file_backend_interface(&file_backend);
    /* 容量必须精确匹配双槽合同。 */
    CHECK(backend.capacity == session_store_required_backend_size());

    /* 从全 0xFF 新文件恢复空索引。 */
    session_store_t store;
    /* 初始化必须成功。 */
    CHECK(session_store_init(&store, &backend) == SESSION_STORE_STATUS_OK);
    /* 新文件没有会话。 */
    CHECK(session_store_count(&store) == 0U);

    /* 构造一个完整摘要。 */
    session_summary_t summary;
    /* 清零保留字段。 */
    (void)memset(&summary, 0, sizeof(summary));
    /* 设置会话主键。 */
    summary.session_seq = 42U;
    /* 设置最新事件水位。 */
    summary.last_event_seq = 7U;
    /* 设置 squat 类。 */
    summary.action_id = 6U;
    /* 设置次数指标。 */
    summary.metric_kind = 0U;
    /* 设置会话时长 12.345 秒。 */
    summary.duration_ms = 12345ULL;
    /* 设置最终 9 次。 */
    summary.metric_total = 9ULL;
    /* 设置毛热量。 */
    summary.gross_microkcal = 1234567ULL;
    /* 设置活动热量。 */
    summary.active_microkcal = 1000000ULL;
    /* 设置平均稳定度。 */
    summary.average_stability_q15 = 30000U;
    /* 设置最低稳定度。 */
    summary.minimum_stability_q15 = 25000U;
    /* 设置事件数。 */
    summary.event_count = 9U;
    /* 保存是否真正写入。 */
    bool changed = false;
    /* 第一次 upsert 必须写入并提交。 */
    CHECK(session_store_upsert(&store, &summary, &changed) == SESSION_STORE_STATUS_OK);
    /* 必须报告变化。 */
    CHECK(changed);
    /* 关闭文件模拟设备重启。 */
    session_file_backend_close(&file_backend);

    /* 不允许创建地重新打开已存在文件。 */
    CHECK(session_file_backend_open(
              &file_backend,
              argv[1],
              session_store_required_backend_size(),
              false) == SESSION_STORE_STATUS_OK);
    /* 更新函数表上下文。 */
    backend = session_file_backend_interface(&file_backend);
    /* 用新 store 恢复。 */
    session_store_t recovered;
    /* 恢复必须成功。 */
    CHECK(session_store_init(&recovered, &backend) == SESSION_STORE_STATUS_OK);
    /* 恢复一条摘要。 */
    CHECK(session_store_count(&recovered) == 1U);
    /* 读取最新摘要。 */
    session_summary_t decoded;
    /* 查询必须成功。 */
    CHECK(session_store_get_recent(&recovered, 0U, &decoded) == SESSION_STORE_STATUS_OK);
    /* 核对主键、水位和次数。 */
    CHECK((decoded.session_seq == 42U) &&
          (decoded.last_event_seq == 7U) &&
          (decoded.metric_total == 9ULL));
    /* 重复相同摘要必须幂等且不改变。 */
    changed = true;
    /* upsert 成功。 */
    CHECK(session_store_upsert(&recovered, &summary, &changed) == SESSION_STORE_STATUS_OK);
    /* 必须报告未变化。 */
    CHECK(!changed);
    /* 格式化双槽。 */
    CHECK(session_store_format(&recovered) == SESSION_STORE_STATUS_OK);
    /* 格式化后为空。 */
    CHECK(session_store_count(&recovered) == 0U);
    /* 关闭文件。 */
    session_file_backend_close(&file_backend);

    /* 过长路径必须在 fopen 前拒绝。 */
    char long_path[256];
    /* 全部填充为可打印字符。 */
    (void)memset(long_path, 'A', sizeof(long_path));
    /* 最后一字节终止。 */
    long_path[sizeof(long_path) - 1U] = '\0';
    /* 检查参数错误。 */
    CHECK(session_file_backend_open(
              &file_backend,
              long_path,
              1024U,
              true) == SESSION_STORE_STATUS_INVALID_ARGUMENT);
    /* 重复关闭安全无操作。 */
    session_file_backend_close(&file_backend);
    /* 输出稳定成功摘要。 */
    (void)printf("session_file_backend host tests: PASS (%u assertions)\n", g_assertions);
    /* 返回成功。 */
    return 0;
}
