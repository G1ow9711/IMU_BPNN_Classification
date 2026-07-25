#ifndef IMU_HANDHELD_IMU_PIPELINE_H
#define IMU_HANDHELD_IMU_PIPELINE_H

/* 引入布尔类型，描述时间表和滤波器是否有效。 */
#include <stdbool.h>
/* 引入定长整数，保证 QMI 原始值、时间戳、计数器和质量位宽明确。 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 固定模型输入采样率为 25 Hz，对应严格 40,000 微秒时间网格。 */
#define IMU_PIPELINE_OUTPUT_RATE_HZ (25U)
/* 固定相邻重采样点间隔为 40,000 微秒。 */
#define IMU_PIPELINE_OUTPUT_PERIOD_US (40000ULL)
/* 固定模型窗口为 62 点，即 2.48 秒。 */
#define IMU_PIPELINE_WINDOW_SAMPLES (62U)
/* 固定每新增 12 点触发下一窗口，即约 0.48 秒更新一次。 */
#define IMU_PIPELINE_WINDOW_STEP (12U)
/* 固定六轴顺序为 gx、gy、gz、ax、ay、az。 */
#define IMU_PIPELINE_AXIS_COUNT (6U)
/* 固定 QMI 单个传感器三轴为 x、y、z。 */
#define IMU_PIPELINE_VECTOR_AXIS_COUNT (3U)
/* 固定最终双 M0 输出 11 类 logits。 */
#define IMU_PIPELINE_CLASS_COUNT (11U)
/* 每路滤波时间队列保存 32 个点；加速度约覆盖 256 ms。 */
#define IMU_PIPELINE_STREAM_QUEUE_CAPACITY (32U)
/* 一阶低通截止频率固定 8 Hz，低于 25 Hz 输出 Nyquist 12.5 Hz。 */
#define IMU_PIPELINE_LOWPASS_CUTOFF_HZ (8.0F)
/* QMI 加速度名义 125 Hz 周期为 8,000 微秒。 */
#define IMU_PIPELINE_ACCEL_EXPECTED_PERIOD_US (8000ULL)
/* QMI 陀螺仪名义 112.1 Hz 周期近似为 8,912 微秒。 */
#define IMU_PIPELINE_GYRO_EXPECTED_PERIOD_US (8912ULL)
/*
 * 原始流缺口只有超过一个 25 Hz 输出周期才清空滤波、插值和 62 点窗口。
 * 小于等于 40 ms 的单帧/少量漏点仍由相邻原始端点线性插值，并保留 ACCEL_GAP/GYRO_GAP
 * 质量事实；这样既不掩盖丢帧，也避免 16 ms 轮询抖动把 2.48 秒模型窗反复冷启动。
 */
#define IMU_PIPELINE_RAW_GAP_RESET_THRESHOLD_US IMU_PIPELINE_OUTPUT_PERIOD_US
/* 明确六轴下标；模型、特征头和 ESP32 必须使用同一顺序。 */
typedef enum {
    /* 角速度 x，单位 deg/s。 */
    IMU_AXIS_GX = 0,
    /* 角速度 y，单位 deg/s。 */
    IMU_AXIS_GY = 1,
    /* 角速度 z，单位 deg/s。 */
    IMU_AXIS_GZ = 2,
    /* 加速度 x，单位 g。 */
    IMU_AXIS_AX = 3,
    /* 加速度 y，单位 g。 */
    IMU_AXIS_AY = 4,
    /* 加速度 z，单位 g。 */
    IMU_AXIS_AZ = 5
} imu_axis_index_t;

/* 区分两路异步 QMI 数据源。 */
typedef enum {
    /* 加速度源名义 125 Hz，原始整数按 g/LSB 换算。 */
    IMU_SOURCE_ACCEL = 0,
    /* 陀螺仪源名义 112.1 Hz，原始整数按 deg/s/LSB 换算。 */
    IMU_SOURCE_GYRO = 1
} imu_source_t;

/* 描述函数结果；数据间断会内部重对齐，不作为致命调用错误。 */
typedef enum {
    /* 调用成功；可能尚未凑齐下一 25 Hz 输出点。 */
    IMU_PIPELINE_OK = 0,
    /* 空指针、非法枚举或无效配置。 */
    IMU_PIPELINE_ERR_ARGUMENT = -1,
    /* 时间戳重复、倒退或接近 uint64 上限而无法建立下一网格。 */
    IMU_PIPELINE_ERR_TIMESTAMP = -2,
    /* QMI 量程换算比例不是有限正数。 */
    IMU_PIPELINE_ERR_SCALE = -3
} imu_pipeline_result_t;

/* 描述样本、窗口和推理质量；零表示未发现质量问题。 */
typedef enum {
    /* 没有质量告警。 */
    IMU_QUALITY_OK = 0U,
    /* 加速度原始时间间隔表明至少丢失一个 125 Hz 点。 */
    IMU_QUALITY_ACCEL_GAP = 1U << 0,
    /* 陀螺仪原始时间间隔表明至少丢失一个 112.1 Hz 点。 */
    IMU_QUALITY_GYRO_GAP = 1U << 1,
    /* 驱动提交重复或倒退时间戳；该原始点已拒绝。 */
    IMU_QUALITY_OUT_OF_ORDER = 1U << 2,
    /* 异步队列已满并丢弃最旧滤波点。 */
    IMU_QUALITY_QUEUE_OVERFLOW = 1U << 3,
    /* 兼容旧日志的历史执行器污染位；当前无执行器固件不会再产生该位。 */
    IMU_QUALITY_LEGACY_ACTUATOR_CONTAMINATED = 1U << 4,
    /* 已知 QMI FIFO/驱动丢样由上层显式报告。 */
    IMU_QUALITY_DRIVER_DROP = 1U << 5,
    /* 数据间断后清空 62 点环形窗并重新对齐 25 Hz 网格。 */
    IMU_QUALITY_RESAMPLER_RESET = 1U << 6,
    /* 双 M0 回调返回非零；本窗口 logits 不可信。 */
    IMU_QUALITY_INFERENCE_FAILED = 1U << 7
} imu_quality_flag_t;

/* 保存 QMI 一路三轴原始整数及采样时刻；时间戳必须来自单调微秒时钟。 */
typedef struct {
    /* 保存采样完成或 FIFO 帧对应的单调微秒时刻，不允许使用可跳变 RTC。 */
    uint64_t timestamp_us;
    /* 保存 x、y、z 原始有符号整数；具体量程由初始化比例解释。 */
    int16_t raw_xyz[IMU_PIPELINE_VECTOR_AXIS_COUNT];
    /* 保存驱动附加质量位；可传 IMU_QUALITY_OK。 */
    uint32_t quality_flags;
} imu_qmi_raw_sample_t;

/* 保存严格 25 Hz 六轴样本；轴顺序固定为 gx、gy、gz、ax、ay、az。 */
typedef struct {
    /* 保存 40,000 微秒网格上的单调时间戳。 */
    uint64_t timestamp_us;
    /* 前三项单位 deg/s，后三项单位 g。 */
    float axes[IMU_PIPELINE_AXIS_COUNT];
    /* 保存两路插值端点和间断传播得到的质量位。 */
    uint32_t quality_flags;
} imu_resampled_sample_t;

/* 保存按时间顺序展开的 62×6 窗口；回调返回后指针内容可被下一窗口覆盖。 */
typedef struct {
    /* 保存窗口最后一个 25 Hz 点的单调时间戳。 */
    uint64_t end_timestamp_us;
    /* 保存从零开始递增的窗口序号。 */
    uint32_t sequence;
    /* 保存按时间升序排列的 [62,6]，单位为 deg/s 和 g。 */
    float samples[IMU_PIPELINE_WINDOW_SAMPLES][IMU_PIPELINE_AXIS_COUNT];
    /* 保存 62 点质量位按位或结果。 */
    uint32_t quality_flags;
} imu_pipeline_window_t;

/* 保存一次推理结果；logits 顺序与自动生成模型头 11 类顺序一致。 */
typedef struct {
    /* 保存对应窗口末时刻。 */
    uint64_t end_timestamp_us;
    /* 保存对应窗口序号。 */
    uint32_t sequence;
    /* 保存 softmax 前 11 类无量纲融合分数。 */
    float logits[IMU_PIPELINE_CLASS_COUNT];
    /* 保存窗口质量位；失败时额外包含 INFERENCE_FAILED。 */
    uint32_t quality_flags;
    /* 保存推理回调原始返回码；零表示双 M0 成功。 */
    int inference_status;
} imu_pipeline_inference_result_t;

/* 推理边界：生产实现调用 esp32/include 双 M0，主机测试可注入 mock。 */
typedef int (*imu_pipeline_infer_fn)(
    void *context,
    const float window[IMU_PIPELINE_WINDOW_SAMPLES][IMU_PIPELINE_AXIS_COUNT],
    float logits[IMU_PIPELINE_CLASS_COUNT]);
/* 每产生一个 25 Hz 点时调用；回调必须快速返回，不能阻塞 QMI 输入。 */
typedef void (*imu_pipeline_sample_callback_fn)(
    void *context,
    const imu_resampled_sample_t *sample);
/* 每形成 62 点窗口时调用；窗口内存仅在同步回调期间有效。 */
typedef void (*imu_pipeline_window_callback_fn)(
    void *context,
    const imu_pipeline_window_t *window);
/* 每次模型调用后返回状态和 logits；失败也调用以便记录。 */
typedef void (*imu_pipeline_inference_callback_fn)(
    void *context,
    const imu_pipeline_inference_result_t *result);

/* 保存初始化参数；结构被复制到流水线，回调上下文生命周期由调用方保证。 */
typedef struct {
    /* QMI 当前加速度量程每 LSB 对应多少 g，必须有限且大于零。 */
    float accel_g_per_lsb;
    /* QMI 当前陀螺量程每 LSB 对应多少 deg/s，必须有限且大于零。 */
    float gyro_dps_per_lsb;
    /* 保存双 M0 或主机 mock 推理函数；允许为空，仅输出窗口。 */
    imu_pipeline_infer_fn infer;
    /* 保存推理函数私有上下文；可为空。 */
    void *inference_context;
    /* 保存 25 Hz 点回调；允许为空。 */
    imu_pipeline_sample_callback_fn on_sample;
    /* 保存 62 点窗口回调；允许为空。 */
    imu_pipeline_window_callback_fn on_window;
    /* 保存推理结果回调；允许为空。 */
    imu_pipeline_inference_callback_fn on_inference;
    /* 保存三个通知回调共享上下文；允许为空。 */
    void *callback_context;
} imu_pipeline_config_t;

/* 保存累计诊断统计；计数溢出由自然 uint32 回绕，正常设备寿命难以达到。 */
typedef struct {
    /* 收到的加速度原始帧数，包含后续被拒绝的帧。 */
    uint32_t accel_raw_received;
    /* 收到的陀螺原始帧数，包含后续被拒绝的帧。 */
    uint32_t gyro_raw_received;
    /* 估算或驱动报告的加速度丢样数。 */
    uint32_t accel_raw_dropped;
    /* 估算或驱动报告的陀螺丢样数。 */
    uint32_t gyro_raw_dropped;
    /* 成功产生的严格 25 Hz 六轴点数。 */
    uint32_t resampled_points;
    /* 因间断、溢出或显式丢样而清空连续窗口的次数。 */
    uint32_t resampler_resets;
    /* 形成的 62 点窗口数。 */
    uint32_t windows_emitted;
    /* 实际调用推理边界的次数。 */
    uint32_t inference_calls;
    /* 推理边界返回非零的次数。 */
    uint32_t inference_failures;
} imu_pipeline_stats_t;

/* 保存低通后一个三轴点；时间戳仍对应原始异步采样时刻。 */
typedef struct {
    /* 保存单调微秒时刻。 */
    uint64_t timestamp_us;
    /* 保存滤波后三轴物理量；加速度为 g，陀螺为 deg/s。 */
    float xyz[IMU_PIPELINE_VECTOR_AXIS_COUNT];
    /* 保存该点及原始驱动质量位。 */
    uint32_t quality_flags;
} imu_pipeline_filtered_point_t;

/* 保存单路异步滤波和插值队列；调用方不得直接修改字段。 */
typedef struct {
    /* 保存最近接受的原始时间戳。 */
    uint64_t last_raw_timestamp_us;
    /* 保存一阶低通当前输出。 */
    float filtered_xyz[IMU_PIPELINE_VECTOR_AXIS_COUNT];
    /* 标记低通是否已有首点。 */
    bool filter_initialized;
    /* 环形队列首元素下标。 */
    uint8_t queue_head;
    /* 环形队列有效元素数，范围 0~32。 */
    uint8_t queue_count;
    /* 保存低通后异步点。 */
    imu_pipeline_filtered_point_t queue[IMU_PIPELINE_STREAM_QUEUE_CAPACITY];
} imu_pipeline_stream_state_t;

/* 保存完整流水线静态状态；不使用动态内存，建议放在静态区而非任务栈。 */
typedef struct {
    /* 保存量程、推理和通知回调配置副本。 */
    imu_pipeline_config_t config;
    /* 保存加速度 125 Hz 流。 */
    imu_pipeline_stream_state_t accel_stream;
    /* 保存陀螺 112.1 Hz 流。 */
    imu_pipeline_stream_state_t gyro_stream;
    /* 标记下一 25 Hz 目标时刻是否已对齐。 */
    bool schedule_valid;
    /* 保存下一严格 40,000 微秒网格时刻。 */
    uint64_t next_output_timestamp_us;
    /* 保存下一输出应继承的间断/拒绝质量位。 */
    uint32_t pending_quality_flags;
    /* 保存 62×6 环形样本，顺序在窗口回调前展开。 */
    float ring_samples[IMU_PIPELINE_WINDOW_SAMPLES][IMU_PIPELINE_AXIS_COUNT];
    /* 保存每个环形点时间戳。 */
    uint64_t ring_timestamps_us[IMU_PIPELINE_WINDOW_SAMPLES];
    /* 保存每个环形点质量位。 */
    uint32_t ring_quality_flags[IMU_PIPELINE_WINDOW_SAMPLES];
    /* 指向下一写入槽。 */
    uint8_t ring_write_index;
    /* 保存有效环形点数，范围 0~62。 */
    uint8_t ring_count;
    /* 保存上个窗口后新增点数，达到 12 触发下一窗口。 */
    uint8_t points_since_last_window;
    /* 保存下一窗口序号。 */
    uint32_t next_window_sequence;
    /* 保存展开后的连续窗口，避免每次推理在任务栈分配约 1.5 KiB。 */
    imu_pipeline_window_t window_scratch;
    /* 保存累计诊断统计。 */
    imu_pipeline_stats_t stats;
    /* 标记初始化已经成功。 */
    bool initialized;
} imu_pipeline_t;

/* 初始化流水线并验证量程比例；不启动 QMI 或 FreeRTOS 任务。 */
imu_pipeline_result_t imu_pipeline_init(
    imu_pipeline_t *pipeline,
    const imu_pipeline_config_t *config);
/* 清空两路滤波、重采样和窗口状态，保留配置与累计统计。 */
void imu_pipeline_reset_session(imu_pipeline_t *pipeline);
/* 提交一帧 125 Hz 加速度原始整数；函数内部换算为 g。 */
imu_pipeline_result_t imu_pipeline_push_accel_raw(
    imu_pipeline_t *pipeline,
    const imu_qmi_raw_sample_t *sample);
/* 提交一帧 112.1 Hz 陀螺原始整数；函数内部换算为 deg/s。 */
imu_pipeline_result_t imu_pipeline_push_gyro_raw(
    imu_pipeline_t *pipeline,
    const imu_qmi_raw_sample_t *sample);
/* 报告 QMI FIFO/驱动已知丢样并重置连续窗口，count 必须大于零。 */
imu_pipeline_result_t imu_pipeline_report_source_drop(
    imu_pipeline_t *pipeline,
    imu_source_t source,
    uint32_t count);
/* 返回只读累计统计指针；流水线生命周期内有效，空流水线返回空指针。 */
const imu_pipeline_stats_t *imu_pipeline_get_stats(const imu_pipeline_t *pipeline);

#ifdef __cplusplus
}
#endif

#endif
