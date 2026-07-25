/* 引入纯 C IMU 流水线合同，主机测试不链接或复制双 M0 权重。 */
#include "imu_pipeline.h"

/* 引入标准输出，失败时打印行号和值。 */
#include <stdio.h>
/* 引入字符串清零，重用测试观察结构。 */
#include <string.h>

/* 限制纯 C 流水线静态状态不超过 8 KiB，避免 ESP32-S3 集成时误把大缓冲继续堆入结构。 */
_Static_assert(sizeof(imu_pipeline_t) <= 8192U, "imu_pipeline_t exceeds 8 KiB static RAM budget");

/* 最多记录 128 个 25 Hz 点，覆盖约 5.12 秒测试流。 */
#define TEST_MAX_SAMPLES (128U)
/* 最多记录 8 个窗口摘要。 */
#define TEST_MAX_WINDOWS (8U)

/* 保存累计失败数；非零使 main 返回失败。 */
static int g_failure_count = 0;

/* 检查布尔表达式并打印调用行。 */
#define CHECK_TRUE(expression)                                                        \
    do {                                                                              \
        /* 表达式失败时记录但继续其它用例。 */                                        \
        if (!(expression)) {                                                          \
            /* 输出失败表达式便于定位合同。 */                                        \
            (void)fprintf(stderr, "FAIL line=%d expression=%s\n", __LINE__, #expression); \
            /* 累加失败数。 */                                                        \
            g_failure_count += 1;                                                     \
        }                                                                             \
    } while (0)

/* 检查整数、枚举或计数相等。 */
#define CHECK_EQ_INT(expected, actual)                                                \
    do {                                                                              \
        /* 保存期望值，避免表达式重复求值。 */                                        \
        const long long expected_value = (long long)(expected);                       \
        /* 保存实际值。 */                                                            \
        const long long actual_value = (long long)(actual);                           \
        /* 数值不等时输出差异。 */                                                    \
        if (expected_value != actual_value) {                                         \
            /* 打印期望与实际。 */                                                    \
            (void)fprintf(                                                            \
                stderr,                                                               \
                "FAIL line=%d expected=%lld actual=%lld\n",                         \
                __LINE__,                                                             \
                expected_value,                                                       \
                actual_value);                                                        \
            /* 累加失败数。 */                                                        \
            g_failure_count += 1;                                                     \
        }                                                                             \
    } while (0)

/* 比较 float 绝对误差，不依赖额外数学库。 */
static void check_close(float expected, float actual, float tolerance, int line)
{
    /* 手工计算绝对差。 */
    const float difference = expected >= actual ? expected - actual : actual - expected;
    /* 超过容差时记录。 */
    if (difference > tolerance) {
        /* 输出浮点差异。 */
        (void)fprintf(
            stderr,
            "FAIL line=%d expected=%.7f actual=%.7f tolerance=%.7f\n",
            line,
            (double)expected,
            (double)actual,
            (double)tolerance);
        /* 累加失败数。 */
        g_failure_count += 1;
    }
}

/* 自动传入调用行。 */
#define CHECK_CLOSE(expected, actual, tolerance) \
    check_close((expected), (actual), (tolerance), __LINE__)

/* 保存窗口回调的关键摘要，避免复制另一份 62×6。 */
typedef struct {
    /* 保存窗口末时间戳。 */
    uint64_t end_timestamp_us;
    /* 保存窗口序号。 */
    uint32_t sequence;
    /* 保存窗口综合质量。 */
    uint32_t quality_flags;
    /* 保存首点 gx。 */
    float first_gx;
    /* 保存末点 az。 */
    float last_az;
} test_window_summary_t;

/* 保存回调观察结果和 mock 推理调用数。 */
typedef struct {
    /* 保存收到的严格 25 Hz 点。 */
    imu_resampled_sample_t samples[TEST_MAX_SAMPLES];
    /* 保存实际点数。 */
    uint32_t sample_count;
    /* 保存窗口摘要。 */
    test_window_summary_t windows[TEST_MAX_WINDOWS];
    /* 保存实际窗口数。 */
    uint32_t window_count;
    /* 保存 mock 模型调用数。 */
    uint32_t infer_count;
    /* 保存推理结果回调数。 */
    uint32_t inference_result_count;
    /* 保存最近推理状态。 */
    int last_inference_status;
    /* 保存最近推理窗口质量。 */
    uint32_t last_inference_quality;
} test_observer_t;

/* 记录每个 25 Hz 点。 */
static void test_on_sample(void *context, const imu_resampled_sample_t *sample)
{
    /* 转换观察上下文。 */
    test_observer_t *observer = (test_observer_t *)context;
    /* 只在固定容量内复制，测试流设计保证不会溢出。 */
    if (observer->sample_count < TEST_MAX_SAMPLES) {
        /* 按值保存当前点。 */
        observer->samples[observer->sample_count] = *sample;
    }
    /* 无论是否保存都累加实际回调数，暴露意外超量。 */
    observer->sample_count += 1U;
}

/* 记录窗口摘要并核对轴顺序。 */
static void test_on_window(void *context, const imu_pipeline_window_t *window)
{
    /* 转换观察上下文。 */
    test_observer_t *observer = (test_observer_t *)context;
    /* 固定容量内保存摘要。 */
    if (observer->window_count < TEST_MAX_WINDOWS) {
        /* 定位当前摘要槽。 */
        test_window_summary_t *summary = &observer->windows[observer->window_count];
        /* 保存末时刻。 */
        summary->end_timestamp_us = window->end_timestamp_us;
        /* 保存序号。 */
        summary->sequence = window->sequence;
        /* 保存质量。 */
        summary->quality_flags = window->quality_flags;
        /* 保存首点 gx。 */
        summary->first_gx = window->samples[0][IMU_AXIS_GX];
        /* 保存末点 az。 */
        summary->last_az = window->samples[IMU_PIPELINE_WINDOW_SAMPLES - 1U][IMU_AXIS_AZ];
    }
    /* 累加实际窗口数。 */
    observer->window_count += 1U;
}

/* mock 双 M0：核对常量窗口，并按类别索引输出确定 logits。 */
static int test_mock_infer(
    void *context,
    const float window[IMU_PIPELINE_WINDOW_SAMPLES][IMU_PIPELINE_AXIS_COUNT],
    float logits[IMU_PIPELINE_CLASS_COUNT])
{
    /* 转换推理上下文。 */
    test_observer_t *observer = (test_observer_t *)context;
    /* 累加模型调用数。 */
    observer->infer_count += 1U;
    /* 常量流的首点 gx 应为 10 deg/s。 */
    CHECK_CLOSE(10.0F, window[0][IMU_AXIS_GX], 0.0001F);
    /* 常量流末点 gy 应为 -20 deg/s。 */
    CHECK_CLOSE(-20.0F, window[IMU_PIPELINE_WINDOW_SAMPLES - 1U][IMU_AXIS_GY], 0.0001F);
    /* 常量流末点 ax 应为 1 g。 */
    CHECK_CLOSE(1.0F, window[IMU_PIPELINE_WINDOW_SAMPLES - 1U][IMU_AXIS_AX], 0.0001F);
    /* 遍历 11 类输出确定值。 */
    for (uint8_t class_index = 0U;
         class_index < IMU_PIPELINE_CLASS_COUNT;
         class_index += 1U) {
        /* logit 直接等于类别索引，便于回调核对。 */
        logits[class_index] = (float)class_index;
    }
    /* 返回零模拟成功。 */
    return 0;
}

/* 保存推理结果摘要。 */
static void test_on_inference(
    void *context,
    const imu_pipeline_inference_result_t *result)
{
    /* 转换观察上下文。 */
    test_observer_t *observer = (test_observer_t *)context;
    /* 累加结果回调数。 */
    observer->inference_result_count += 1U;
    /* 保存状态。 */
    observer->last_inference_status = result->inference_status;
    /* 保存质量位。 */
    observer->last_inference_quality = result->quality_flags;
    /* mock 第 10 类 logit 应等于 10。 */
    CHECK_CLOSE(10.0F, result->logits[10], 0.0001F);
}

/* 构造测试默认配置；加速度 0.001 g/LSB，陀螺 0.1 deg/s/LSB。 */
static imu_pipeline_config_t test_default_config(test_observer_t *observer)
{
    /* 创建完整配置。 */
    const imu_pipeline_config_t config = {
        /* 1000 LSB 对应 1 g。 */
        .accel_g_per_lsb = 0.001F,
        /* 100 LSB 对应 10 deg/s。 */
        .gyro_dps_per_lsb = 0.1F,
        /* 使用 mock 模型，证明流水线不复制权重。 */
        .infer = test_mock_infer,
        /* 推理上下文使用同一观察结构。 */
        .inference_context = observer,
        /* 记录每个重采样点。 */
        .on_sample = test_on_sample,
        /* 记录窗口。 */
        .on_window = test_on_window,
        /* 记录推理结果。 */
        .on_inference = test_on_inference,
        /* 三个通知回调使用观察结构。 */
        .callback_context = observer,
    };
    /* 返回按值配置。 */
    return config;
}

/* 提交一个常量加速度点。 */
static imu_pipeline_result_t push_constant_accel(imu_pipeline_t *pipeline, uint64_t timestamp_us)
{
    /* 原始值换算后为 ax=1g、ay=-2g、az=3g。 */
    const imu_qmi_raw_sample_t sample = {
        .timestamp_us = timestamp_us,
        .raw_xyz = {1000, -2000, 3000},
        .quality_flags = IMU_QUALITY_OK,
    };
    /* 提交 125 Hz 加速度。 */
    return imu_pipeline_push_accel_raw(pipeline, &sample);
}

/* 提交一个常量陀螺点。 */
static imu_pipeline_result_t push_constant_gyro(imu_pipeline_t *pipeline, uint64_t timestamp_us)
{
    /* 原始值换算后为 gx=10、gy=-20、gz=30 deg/s。 */
    const imu_qmi_raw_sample_t sample = {
        .timestamp_us = timestamp_us,
        .raw_xyz = {100, -200, 300},
        .quality_flags = IMU_QUALITY_OK,
    };
    /* 提交 112.1 Hz 陀螺。 */
    return imu_pipeline_push_gyro_raw(pipeline, &sample);
}

/* 按时间顺序交错提交两路常量流，避免人为制造单路队列溢出。 */
static void feed_constant_streams(imu_pipeline_t *pipeline, uint64_t end_timestamp_us)
{
    /* 下一加速度时刻从零开始。 */
    uint64_t next_accel_us = 0ULL;
    /* 下一陀螺时刻从零开始。 */
    uint64_t next_gyro_us = 0ULL;
    /* 两路至少一路仍在测试范围时继续。 */
    while ((next_accel_us <= end_timestamp_us) || (next_gyro_us <= end_timestamp_us)) {
        /* 优先提交更早时刻；相等时先提交加速度再提交陀螺。 */
        if ((next_accel_us <= next_gyro_us) && (next_accel_us <= end_timestamp_us)) {
            /* 提交当前加速度点。 */
            CHECK_EQ_INT(IMU_PIPELINE_OK, push_constant_accel(pipeline, next_accel_us));
            /* 按 8,000 微秒前进。 */
            next_accel_us += IMU_PIPELINE_ACCEL_EXPECTED_PERIOD_US;
        } else if (next_gyro_us <= end_timestamp_us) {
            /* 提交当前陀螺点。 */
            CHECK_EQ_INT(IMU_PIPELINE_OK, push_constant_gyro(pipeline, next_gyro_us));
            /* 按 8,912 微秒近似 112.1 Hz 前进。 */
            next_gyro_us += IMU_PIPELINE_GYRO_EXPECTED_PERIOD_US;
        }
    }
}

/* 验证量程、严格网格、六轴顺序、62 点首窗和 12 点步长。 */
static void test_constant_stream_and_windows(void)
{
    /* 清零观察结构。 */
    test_observer_t observer;
    /* 防止旧栈数据影响计数。 */
    (void)memset(&observer, 0, sizeof(observer));
    /* 获取默认配置。 */
    const imu_pipeline_config_t config = test_default_config(&observer);
    /* 流水线较大，使用静态存储避免占用主机测试线程栈。 */
    static imu_pipeline_t pipeline;
    /* 初始化流水线。 */
    CHECK_EQ_INT(IMU_PIPELINE_OK, imu_pipeline_init(&pipeline, &config));
    /* 输入到 3 秒，足够产生 75 个 25 Hz 点和两个窗口。 */
    feed_constant_streams(&pipeline, 3000000ULL);
    /* 应产生 0~2.96 秒共 75 点。 */
    CHECK_EQ_INT(75U, observer.sample_count);
    /* 首点必须在零网格。 */
    CHECK_EQ_INT(0ULL, observer.samples[0].timestamp_us);
    /* 检查所有相邻输出严格相差 40,000 微秒。 */
    for (uint32_t index = 1U; index < observer.sample_count; index += 1U) {
        /* 时间差不能受 112.1 Hz 非整数周期影响。 */
        CHECK_EQ_INT(
            IMU_PIPELINE_OUTPUT_PERIOD_US,
            observer.samples[index].timestamp_us - observer.samples[index - 1U].timestamp_us);
    }
    /* 检查六轴顺序和陀螺单位。 */
    CHECK_CLOSE(10.0F, observer.samples[10].axes[IMU_AXIS_GX], 0.0001F);
    /* 检查 gy。 */
    CHECK_CLOSE(-20.0F, observer.samples[10].axes[IMU_AXIS_GY], 0.0001F);
    /* 检查 gz。 */
    CHECK_CLOSE(30.0F, observer.samples[10].axes[IMU_AXIS_GZ], 0.0001F);
    /* 检查 ax 单位 g。 */
    CHECK_CLOSE(1.0F, observer.samples[10].axes[IMU_AXIS_AX], 0.0001F);
    /* 检查 ay。 */
    CHECK_CLOSE(-2.0F, observer.samples[10].axes[IMU_AXIS_AY], 0.0001F);
    /* 检查 az。 */
    CHECK_CLOSE(3.0F, observer.samples[10].axes[IMU_AXIS_AZ], 0.0001F);
    /* 产生两个窗口：首窗 62 点，随后新增 12 点。 */
    CHECK_EQ_INT(2U, observer.window_count);
    /* 首窗末点为 61*40 ms=2.44 s。 */
    CHECK_EQ_INT(2440000ULL, observer.windows[0].end_timestamp_us);
    /* 第二窗末点比首窗晚 12*40 ms。 */
    CHECK_EQ_INT(2920000ULL, observer.windows[1].end_timestamp_us);
    /* 窗口序号从零开始。 */
    CHECK_EQ_INT(0U, observer.windows[0].sequence);
    /* 第二窗口序号递增。 */
    CHECK_EQ_INT(1U, observer.windows[1].sequence);
    /* 模型每个窗口调用一次。 */
    CHECK_EQ_INT(2U, observer.infer_count);
    /* 结果回调也为两次。 */
    CHECK_EQ_INT(2U, observer.inference_result_count);
    /* 最近模型状态成功。 */
    CHECK_EQ_INT(0, observer.last_inference_status);
    /* 统计与观察一致。 */
    const imu_pipeline_stats_t *stats = imu_pipeline_get_stats(&pipeline);
    /* 统计指针有效。 */
    CHECK_TRUE(stats != NULL);
    /* 检查重采样点数。 */
    CHECK_EQ_INT(75U, stats->resampled_points);
    /* 检查窗口数。 */
    CHECK_EQ_INT(2U, stats->windows_emitted);
    /* 检查推理数。 */
    CHECK_EQ_INT(2U, stats->inference_calls);
}

/* 提交高频交替加速度与零陀螺，验证 8 Hz 一阶低通衰减。 */
static void test_lowpass_attenuation(void)
{
    /* 清零观察结构。 */
    test_observer_t observer;
    /* 初始化为零。 */
    (void)memset(&observer, 0, sizeof(observer));
    /* 配置不运行模型，避免 mock 对常量窗口的断言。 */
    imu_pipeline_config_t config = test_default_config(&observer);
    /* 关闭推理。 */
    config.infer = NULL;
    /* 关闭窗口回调。 */
    config.on_window = NULL;
    /* 关闭推理回调。 */
    config.on_inference = NULL;
    /* 创建独立静态流水线。 */
    static imu_pipeline_t pipeline;
    /* 初始化。 */
    CHECK_EQ_INT(IMU_PIPELINE_OK, imu_pipeline_init(&pipeline, &config));
    /* 两路下一时刻。 */
    uint64_t next_accel_us = 0ULL;
    /* 陀螺下一时刻。 */
    uint64_t next_gyro_us = 0ULL;
    /* 加速度样本序号决定 ±1 g 交替。 */
    uint32_t accel_index = 0U;
    /* 输入到 1.2 秒。 */
    const uint64_t end_us = 1200000ULL;
    /* 按时间交错两路。 */
    while ((next_accel_us <= end_us) || (next_gyro_us <= end_us)) {
        /* 更早加速度时提交交替信号。 */
        if ((next_accel_us <= next_gyro_us) && (next_accel_us <= end_us)) {
            /* x 轴在 +1g 和 -1g 间每 8 ms 交替，频率远高于目标带宽。 */
            const int16_t raw_x = (accel_index % 2U) == 0U ? 1000 : -1000;
            /* 构造加速度点。 */
            const imu_qmi_raw_sample_t sample = {
                .timestamp_us = next_accel_us,
                .raw_xyz = {raw_x, 0, 1000},
                .quality_flags = IMU_QUALITY_OK,
            };
            /* 提交加速度。 */
            CHECK_EQ_INT(IMU_PIPELINE_OK, imu_pipeline_push_accel_raw(&pipeline, &sample));
            /* 前进样本序号。 */
            accel_index += 1U;
            /* 前进 8 ms。 */
            next_accel_us += IMU_PIPELINE_ACCEL_EXPECTED_PERIOD_US;
        } else if (next_gyro_us <= end_us) {
            /* 陀螺保持零，提供合法异步插值端点。 */
            const imu_qmi_raw_sample_t sample = {
                .timestamp_us = next_gyro_us,
                .raw_xyz = {0, 0, 0},
                .quality_flags = IMU_QUALITY_OK,
            };
            /* 提交陀螺。 */
            CHECK_EQ_INT(IMU_PIPELINE_OK, imu_pipeline_push_gyro_raw(&pipeline, &sample));
            /* 前进约 8.912 ms。 */
            next_gyro_us += IMU_PIPELINE_GYRO_EXPECTED_PERIOD_US;
        }
    }
    /* 至少得到 25 个 25 Hz 点。 */
    CHECK_TRUE(observer.sample_count >= 25U);
    /* 跳过首点初始化瞬态，后续高频交替幅值应显著低于原始 1 g。 */
    for (uint32_t index = 2U; index < observer.sample_count; index += 1U) {
        /* 读取 ax 绝对值。 */
        const float value = observer.samples[index].axes[IMU_AXIS_AX];
        /* 手工绝对值。 */
        const float magnitude = value >= 0.0F ? value : -value;
        /* 简化一阶低通后目标网格幅值应低于 0.5 g。 */
        CHECK_TRUE(magnitude < 0.5F);
    }
}

/*
 * 验证短于 25 Hz 输出周期的单个原始漏点只标记质量，不清空 62 点模型历史。
 * 125 Hz 加速度漏一帧形成 16 ms 间隔，仍可由相邻原始端点为 40 ms 输出网格做线性插值。
 */
static void test_recoverable_raw_gap_keeps_window_stride(void)
{
    /* 清零观察结构，窗口数和质量位从零开始。 */
    test_observer_t observer;
    /* 避免栈残值污染回调计数。 */
    (void)memset(&observer, 0, sizeof(observer));
    /* 使用常量六轴配置，关闭模型但保留窗口观察。 */
    imu_pipeline_config_t config = test_default_config(&observer);
    /* 本测试只审计重采样和窗口，不执行双 M0。 */
    config.infer = NULL;
    /* 推理结果回调随模型一起关闭。 */
    config.on_inference = NULL;
    /* 使用静态流水线，避免约数 KiB 窗口对象占用测试线程栈。 */
    static imu_pipeline_t pipeline;
    /* 初始化生产流水线。 */
    CHECK_EQ_INT(IMU_PIPELINE_OK, imu_pipeline_init(&pipeline, &config));
    /* 两路原始时钟从零开始。 */
    uint64_t next_accel_us = 0ULL;
    /* 陀螺名义周期为 8,912 微秒。 */
    uint64_t next_gyro_us = 0ULL;
    /* 加速度序号用于精确跳过第 20 帧，制造一次 16 ms 间隔。 */
    uint32_t accel_index = 0U;
    /* 输入到 3 秒，正常应形成首窗和一个 12 点步进窗。 */
    const uint64_t end_us = 3000000ULL;
    /* 按时间顺序交错两路，复现 QMI 轮询提交次序。 */
    while ((next_accel_us <= end_us) || (next_gyro_us <= end_us)) {
        /* 当前加速度时刻较早时处理加速度源。 */
        if ((next_accel_us <= next_gyro_us) && (next_accel_us <= end_us)) {
            /* 第 20 帧只推进硬件时间、不提交，模拟一次 DATA_READY 轮询抖动。 */
            if (accel_index != 20U) {
                /* 其它加速度点保持常量，隔离时间策略。 */
                CHECK_EQ_INT(
                    IMU_PIPELINE_OK,
                    push_constant_accel(&pipeline, next_accel_us));
            }
            /* 原始帧序号前进一。 */
            accel_index += 1U;
            /* 加速度硬件时间固定前进 8 ms。 */
            next_accel_us += IMU_PIPELINE_ACCEL_EXPECTED_PERIOD_US;
        } else if (next_gyro_us <= end_us) {
            /* 陀螺保持无丢帧，提供连续插值端点。 */
            CHECK_EQ_INT(
                IMU_PIPELINE_OK,
                push_constant_gyro(&pipeline, next_gyro_us));
            /* 陀螺时钟前进一个名义周期。 */
            next_gyro_us += IMU_PIPELINE_GYRO_EXPECTED_PERIOD_US;
        }
    }
    /* 16 ms 小缺口不得把完整窗口清零。 */
    CHECK_EQ_INT(0U, pipeline.stats.resampler_resets);
    /* 缺失一帧必须保留诊断统计，不能把容错解释成没有丢点。 */
    CHECK_TRUE(pipeline.stats.accel_raw_dropped >= 1U);
    /* 输出仍保持 0、40...2960 ms 共 75 点。 */
    CHECK_EQ_INT(75U, observer.sample_count);
    /* 首窗 62 点、随后新增 12 点，必须按正常 0.48 秒步长形成两窗。 */
    CHECK_EQ_INT(2U, observer.window_count);
    /* 第一窗口保留 ACCEL_GAP 诊断事实。 */
    CHECK_TRUE((observer.windows[0].quality_flags & IMU_QUALITY_ACCEL_GAP) != 0U);
    /* 可恢复小缺口不得伪造 RESAMPLER_RESET。 */
    CHECK_TRUE((observer.windows[0].quality_flags & IMU_QUALITY_RESAMPLER_RESET) == 0U);
}

/* 验证超过 25 Hz 输出周期的原始缺口仍作硬重置，禁止跨长缺口插值。 */
static void test_unrecoverable_raw_gap_resets_window(void)
{
    /* 清零观察结构。 */
    test_observer_t observer;
    /* 消除未定义栈数据。 */
    (void)memset(&observer, 0, sizeof(observer));
    /* 关闭模型和窗口回调断言，只检查内部连续性状态。 */
    imu_pipeline_config_t config = test_default_config(&observer);
    /* 长缺口测试无需模型。 */
    config.infer = NULL;
    /* 长缺口测试无需窗口。 */
    config.on_window = NULL;
    /* 长缺口测试无需推理结果。 */
    config.on_inference = NULL;
    /* 使用独立静态流水线。 */
    static imu_pipeline_t pipeline;
    /* 初始化生产流水线。 */
    CHECK_EQ_INT(IMU_PIPELINE_OK, imu_pipeline_init(&pipeline, &config));
    /* 首帧建立加速度时间基准。 */
    CHECK_EQ_INT(IMU_PIPELINE_OK, push_constant_accel(&pipeline, 0ULL));
    /* 48 ms 大于 40 ms 输出周期，必须重建连续段。 */
    CHECK_EQ_INT(IMU_PIPELINE_OK, push_constant_accel(&pipeline, 48000ULL));
    /* 长缺口必须增加重采样重置统计。 */
    CHECK_TRUE(pipeline.stats.resampler_resets >= 1U);
    /* 至少五个 8 ms 原始点缺失必须被统计。 */
    CHECK_TRUE(pipeline.stats.accel_raw_dropped >= 5U);
    /* 新连续段尚无 62 点，模型环必须为空。 */
    CHECK_EQ_INT(0U, pipeline.ring_count);
}

/* 验证倒退时间、驱动丢样、队列溢出和会话重置。 */
static void test_quality_and_drop_paths(void)
{
    /* 清零观察结构。 */
    test_observer_t observer;
    /* 初始化观察。 */
    (void)memset(&observer, 0, sizeof(observer));
    /* 创建不运行模型的配置。 */
    imu_pipeline_config_t config = test_default_config(&observer);
    /* 禁用推理。 */
    config.infer = NULL;
    /* 禁用窗口。 */
    config.on_window = NULL;
    /* 禁用推理结果。 */
    config.on_inference = NULL;
    /* 创建独立流水线。 */
    static imu_pipeline_t pipeline;
    /* 初始化。 */
    CHECK_EQ_INT(IMU_PIPELINE_OK, imu_pipeline_init(&pipeline, &config));
    /* 提交首个加速度点。 */
    CHECK_EQ_INT(IMU_PIPELINE_OK, push_constant_accel(&pipeline, 0ULL));
    /* 重复时间戳必须拒绝。 */
    CHECK_EQ_INT(IMU_PIPELINE_ERR_TIMESTAMP, push_constant_accel(&pipeline, 0ULL));
    /* 继续提交常量流，但辅助函数会再次提交 t=0 加速度，因此先手工从下一点输入。 */
    CHECK_EQ_INT(IMU_PIPELINE_OK, push_constant_gyro(&pipeline, 0ULL));
    /* 提交下一加速度端点。 */
    CHECK_EQ_INT(IMU_PIPELINE_OK, push_constant_accel(&pipeline, 8000ULL));
    /* 提交下一陀螺端点后产生 t=0 输出。 */
    CHECK_EQ_INT(IMU_PIPELINE_OK, push_constant_gyro(&pipeline, 8912ULL));
    /* 应产生一个输出。 */
    CHECK_EQ_INT(1U, observer.sample_count);
    /* 下一有效输出继承倒退质量位。 */
    CHECK_TRUE((observer.samples[0].quality_flags & IMU_QUALITY_OUT_OF_ORDER) != 0U);
    /* 显式报告陀螺丢 3 点。 */
    CHECK_EQ_INT(
        IMU_PIPELINE_OK,
        imu_pipeline_report_source_drop(&pipeline, IMU_SOURCE_GYRO, 3U));
    /* 连续窗口已清空。 */
    CHECK_EQ_INT(0U, pipeline.ring_count);
    /* 统计记录至少 3 个陀螺丢点。 */
    CHECK_TRUE(pipeline.stats.gyro_raw_dropped >= 3U);
    /* 统计记录一个被拒绝加速度点。 */
    CHECK_TRUE(pipeline.stats.accel_raw_dropped >= 1U);
    /* 非法数据源拒绝。 */
    CHECK_EQ_INT(
        IMU_PIPELINE_ERR_ARGUMENT,
        imu_pipeline_report_source_drop(&pipeline, (imu_source_t)99, 1U));
    /* 会话重置不应增加故障重采样计数。 */
    const uint32_t resets_before = pipeline.stats.resampler_resets;
    /* 重置新会话。 */
    imu_pipeline_reset_session(&pipeline);
    /* 检查统计未增。 */
    CHECK_EQ_INT(resets_before, pipeline.stats.resampler_resets);
    /* 检查环形清空。 */
    CHECK_EQ_INT(0U, pipeline.ring_count);
    /* 检查窗口序号归零。 */
    CHECK_EQ_INT(0U, pipeline.next_window_sequence);

    /* 创建只输入加速度的独立流水线，故意使 32 点队列溢出。 */
    static imu_pipeline_t overflow_pipeline;
    /* 初始化。 */
    CHECK_EQ_INT(IMU_PIPELINE_OK, imu_pipeline_init(&overflow_pipeline, &config));
    /* 提交 40 个连续加速度点，陀螺为空无法消费队列。 */
    for (uint32_t index = 0U; index < 40U; index += 1U) {
        /* 计算严格 8 ms 时刻。 */
        const uint64_t timestamp_us = (uint64_t)index * IMU_PIPELINE_ACCEL_EXPECTED_PERIOD_US;
        /* 提交点。 */
        CHECK_EQ_INT(IMU_PIPELINE_OK, push_constant_accel(&overflow_pipeline, timestamp_us));
    }
    /* 超出 32 槽的 8 个点应记为丢弃。 */
    CHECK_EQ_INT(8U, overflow_pipeline.stats.accel_raw_dropped);
    /* 溢出必须重置连续窗口。 */
    CHECK_TRUE(overflow_pipeline.stats.resampler_resets >= 1U);
}

/* 验证初始化参数保护。 */
static void test_invalid_config(void)
{
    /* 创建流水线存储。 */
    imu_pipeline_t pipeline;
    /* 创建零比例非法配置。 */
    imu_pipeline_config_t config;
    /* 清零配置。 */
    (void)memset(&config, 0, sizeof(config));
    /* 零比例必须拒绝。 */
    CHECK_EQ_INT(IMU_PIPELINE_ERR_SCALE, imu_pipeline_init(&pipeline, &config));
    /* 空流水线必须拒绝。 */
    CHECK_EQ_INT(IMU_PIPELINE_ERR_ARGUMENT, imu_pipeline_init(NULL, &config));
    /* 空配置必须拒绝。 */
    CHECK_EQ_INT(IMU_PIPELINE_ERR_ARGUMENT, imu_pipeline_init(&pipeline, NULL));
}

/* 运行全部用例。 */
int main(void)
{
    /* 验证非法初始化。 */
    test_invalid_config();
    /* 验证常量异步流和窗口推理。 */
    test_constant_stream_and_windows();
    /* 验证简化抗混叠低通。 */
    test_lowpass_attenuation();
    /* 验证一次原始漏帧只留质量事实，不重启 62 点窗口。 */
    test_recoverable_raw_gap_keeps_window_stride();
    /* 验证长于输出周期的原始缺口仍安全重建。 */
    test_unrecoverable_raw_gap_resets_window();
    /* 验证质量和丢样路径。 */
    test_quality_and_drop_paths();
    /* 任一失败返回非零。 */
    if (g_failure_count != 0) {
        /* 打印失败总数。 */
        (void)fprintf(stderr, "imu_pipeline_tests failed=%d\n", g_failure_count);
        /* 返回失败。 */
        return 1;
    }
    /* 输出稳定通过标志。 */
    (void)printf(
        "imu_pipeline_tests passed pipeline_bytes=%zu\n",
        sizeof(imu_pipeline_t));
    /* 返回成功。 */
    return 0;
}
