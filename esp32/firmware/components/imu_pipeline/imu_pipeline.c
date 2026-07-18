/* 引入公开流水线合同；实现保持纯 C，不直接依赖 QMI、FreeRTOS 或 ESP-IDF。 */
#include "imu_pipeline.h"

/* 引入 isfinite，拒绝损坏的量程比例和滤波结果。 */
#include <math.h>
/* 引入 NULL 定义完成公开接口参数检查。 */
#include <stddef.h>
/* 引入 memcpy 和 memset，展开环形窗口并清空静态状态。 */
#include <string.h>

/* 一阶低通 RC 公式中的 2π 常数；float 精度足够 62 点窗口。 */
#define IMU_PIPELINE_TWO_PI (6.28318530717958647692F)

/* 描述插值尝试状态；内部枚举不暴露给驱动层。 */
typedef enum {
    /* 当前队列已经提供目标时刻左右端点。 */
    IMU_INTERPOLATE_READY = 1,
    /* 尚未收到目标时刻之后的端点，需要等待另一帧。 */
    IMU_INTERPOLATE_WAIT = 0,
    /* 队列最早点已经晚于目标，旧网格无法恢复，必须重对齐。 */
    IMU_INTERPOLATE_TARGET_TOO_OLD = -1
} imu_interpolate_status_t;

/* 返回环形队列第 offset 个只读点；调用方保证 offset 小于 queue_count。 */
static const imu_pipeline_filtered_point_t *imu_stream_point_at(
    const imu_pipeline_stream_state_t *stream,
    uint8_t offset)
{
    /* 环形下标按固定 32 槽取模，避免移动数组。 */
    const uint8_t index = (uint8_t)(
        (stream->queue_head + offset) % IMU_PIPELINE_STREAM_QUEUE_CAPACITY);
    /* 返回结构内部只读地址，下一次队列写入后可能失效。 */
    return &stream->queue[index];
}

/* 返回环形队列尾部可写槽；调用方保证 queue_count 小于容量。 */
static imu_pipeline_filtered_point_t *imu_stream_write_slot(
    imu_pipeline_stream_state_t *stream)
{
    /* 尾部位于 head+count，对容量取模后得到实际数组下标。 */
    const uint8_t index = (uint8_t)(
        (stream->queue_head + stream->queue_count) % IMU_PIPELINE_STREAM_QUEUE_CAPACITY);
    /* 返回可写槽，调用方写完后必须增加 queue_count。 */
    return &stream->queue[index];
}

/* 弹出最旧滤波点；空队列调用保持不变。 */
static void imu_stream_pop_front(imu_pipeline_stream_state_t *stream)
{
    /* 空队列没有可弹元素。 */
    if (stream->queue_count == 0U) {
        return;
    }
    /* 首下标循环前移一槽。 */
    stream->queue_head = (uint8_t)(
        (stream->queue_head + 1U) % IMU_PIPELINE_STREAM_QUEUE_CAPACITY);
    /* 有效点数减少一。 */
    stream->queue_count -= 1U;
}

/* 清空单路低通和时间队列；不修改全局统计。 */
static void imu_stream_reset(imu_pipeline_stream_state_t *stream)
{
    /* 清零时间戳、滤波值、队列和初始化标志。 */
    (void)memset(stream, 0, sizeof(*stream));
}

/* 清空 62 点连续窗口和输出网格；两路原始队列由调用场景决定是否保留。 */
static void imu_pipeline_reset_contiguity(
    imu_pipeline_t *pipeline,
    uint32_t reason_flags)
{
    /* 使下一次泵送从两路现存最早时刻重新对齐 40 ms 网格。 */
    pipeline->schedule_valid = false;
    /* 下一输出时刻归零，仅在 schedule_valid=true 时才读取。 */
    pipeline->next_output_timestamp_us = 0ULL;
    /* 清空 62 点环形索引，旧间断前数据不得进入新窗口。 */
    pipeline->ring_write_index = 0U;
    /* 清空有效环形点数。 */
    pipeline->ring_count = 0U;
    /* 新连续段必须重新积累 62 点，窗口步长计数归零。 */
    pipeline->points_since_last_window = 0U;
    /* 把间断原因传播到新连续段首个 25 Hz 点。 */
    pipeline->pending_quality_flags |= reason_flags | IMU_QUALITY_RESAMPLER_RESET;
    /* 累加连续性重置统计，便于现场判断采样链稳定性。 */
    pipeline->stats.resampler_resets += 1U;
}

/* 返回时间戳是否落入带前后保护的马达污染区间。 */
static bool imu_pipeline_timestamp_is_haptic(
    const imu_pipeline_t *pipeline,
    uint64_t timestamp_us)
{
    /* 区间未注册时任何点都不受马达标记。 */
    if (!pipeline->haptic_interval_valid) {
        return false;
    }
    /* 使用闭区间，端点采样也视为可能受机械振动污染。 */
    return (timestamp_us >= pipeline->haptic_start_us) &&
           (timestamp_us <= pipeline->haptic_end_us);
}

/* 把任意时刻向上对齐到 40,000 微秒网格；溢出时返回 false。 */
static bool imu_pipeline_ceil_output_grid(uint64_t timestamp_us, uint64_t *aligned_us)
{
    /* 输出指针不能为空。 */
    if (aligned_us == NULL) {
        return false;
    }
    /* 计算时间戳在 40 ms 周期内的余数。 */
    const uint64_t remainder = timestamp_us % IMU_PIPELINE_OUTPUT_PERIOD_US;
    /* 已在网格上时保持原时间戳。 */
    if (remainder == 0ULL) {
        *aligned_us = timestamp_us;
        return true;
    }
    /* 计算到下一网格点还需增加的微秒数。 */
    const uint64_t increment = IMU_PIPELINE_OUTPUT_PERIOD_US - remainder;
    /* 防止极端损坏时间戳导致 uint64 加法回绕。 */
    if (timestamp_us > (UINT64_MAX - increment)) {
        return false;
    }
    /* 输出严格大于原时刻的下一网格点。 */
    *aligned_us = timestamp_us + increment;
    return true;
}

/* 把目标时刻在线性队列中插值为三轴物理量。 */
static imu_interpolate_status_t imu_stream_interpolate(
    imu_pipeline_stream_state_t *stream,
    uint64_t target_us,
    float output_xyz[IMU_PIPELINE_VECTOR_AXIS_COUNT],
    uint32_t *quality_flags)
{
    /* 输出指针由内部调用保证有效。 */
    if ((output_xyz == NULL) || (quality_flags == NULL)) {
        return IMU_INTERPOLATE_WAIT;
    }
    /* 当第二点仍不晚于目标时，第一点已不再可能用于未来插值，安全弹出。 */
    while (stream->queue_count >= 2U) {
        /* 读取当前第二点。 */
        const imu_pipeline_filtered_point_t *second = imu_stream_point_at(stream, 1U);
        /* 第二点晚于目标时，当前首两点已经形成目标候选区间。 */
        if (second->timestamp_us > target_us) {
            break;
        }
        /* 第二点等于目标时仍可成为精确左端点；弹出旧首点减少队列占用。 */
        imu_stream_pop_front(stream);
    }
    /* 少于两个点无法同时提供左右端点。 */
    if (stream->queue_count < 2U) {
        return IMU_INTERPOLATE_WAIT;
    }
    /* 读取当前目标左右候选端点。 */
    const imu_pipeline_filtered_point_t *left = imu_stream_point_at(stream, 0U);
    /* 读取严格晚于 left 的第二端点。 */
    const imu_pipeline_filtered_point_t *right = imu_stream_point_at(stream, 1U);
    /* 最早点晚于目标说明此前队列丢失，当前输出网格无法继续。 */
    if (left->timestamp_us > target_us) {
        return IMU_INTERPOLATE_TARGET_TOO_OLD;
    }
    /* 右端点仍早于目标时等待更多点；正常 while 已尽量弹出旧点。 */
    if (right->timestamp_us < target_us) {
        return IMU_INTERPOLATE_WAIT;
    }
    /* 单调检查确保分母为正；相等时间戳已在入口拒绝。 */
    if (right->timestamp_us <= left->timestamp_us) {
        return IMU_INTERPOLATE_TARGET_TOO_OLD;
    }
    /* 用局部时间差计算 0~1 线性插值比例，避免把绝对 64 位时刻转 float。 */
    const float ratio = (float)(target_us - left->timestamp_us) /
                        (float)(right->timestamp_us - left->timestamp_us);
    /* 对 x、y、z 分别执行 y=y0+r(y1-y0)。 */
    for (uint8_t axis = 0U; axis < IMU_PIPELINE_VECTOR_AXIS_COUNT; axis += 1U) {
        /* 线性重采样保持常量和斜坡信号，并把时刻严格放在 25 Hz 网格。 */
        output_xyz[axis] = left->xyz[axis] + ratio * (right->xyz[axis] - left->xyz[axis]);
    }
    /* 传播两个插值端点的质量位。 */
    *quality_flags = left->quality_flags | right->quality_flags;
    /* 表示三轴输出已经就绪。 */
    return IMU_INTERPOLATE_READY;
}

/* 展开当前 62 点环形窗口并同步调用窗口和模型回调。 */
static void imu_pipeline_emit_window(imu_pipeline_t *pipeline)
{
    /* 环形区未满时不能形成模型要求的 [62,6]。 */
    if (pipeline->ring_count < IMU_PIPELINE_WINDOW_SAMPLES) {
        return;
    }
    /* 满环时 ring_write_index 指向最旧点，也是时间升序展开起点。 */
    const uint8_t oldest_index = pipeline->ring_write_index;
    /* 初始化窗口质量为零，循环中按位汇总 62 点。 */
    uint32_t window_quality = IMU_QUALITY_OK;
    /* 依次复制 62 个时间升序点到连续 scratch。 */
    for (uint8_t sample_index = 0U;
         sample_index < IMU_PIPELINE_WINDOW_SAMPLES;
         sample_index += 1U) {
        /* 把逻辑时间下标映射到环形物理下标。 */
        const uint8_t ring_index = (uint8_t)(
            (oldest_index + sample_index) % IMU_PIPELINE_WINDOW_SAMPLES);
        /* 复制当前点六轴 float，保持 gx、gy、gz、ax、ay、az 顺序。 */
        (void)memcpy(
            pipeline->window_scratch.samples[sample_index],
            pipeline->ring_samples[ring_index],
            sizeof(pipeline->window_scratch.samples[sample_index]));
        /* 汇总当前点的质量位。 */
        window_quality |= pipeline->ring_quality_flags[ring_index];
    }
    /* 保存窗口末点时间；环形写指针前一槽是最新点。 */
    const uint8_t latest_index = (uint8_t)(
        (pipeline->ring_write_index + IMU_PIPELINE_WINDOW_SAMPLES - 1U) %
        IMU_PIPELINE_WINDOW_SAMPLES);
    /* 写入最新严格 25 Hz 时刻。 */
    pipeline->window_scratch.end_timestamp_us = pipeline->ring_timestamps_us[latest_index];
    /* 分配单调窗口序号并为下一窗口递增。 */
    pipeline->window_scratch.sequence = pipeline->next_window_sequence;
    /* 写入 62 点综合质量。 */
    pipeline->window_scratch.quality_flags = window_quality;
    /* 窗口序号向前推进；uint32 回绕不影响单次会话顺序比较。 */
    pipeline->next_window_sequence += 1U;
    /* 累加窗口统计。 */
    pipeline->stats.windows_emitted += 1U;
    /* 有窗口观察者时同步通知；回调不得保存 scratch 指针或重入 push 接口。 */
    if (pipeline->config.on_window != NULL) {
        pipeline->config.on_window(pipeline->config.callback_context, &pipeline->window_scratch);
    }
    /* 未配置模型时到此结束，主机或采集固件仍可只验证窗口。 */
    if (pipeline->config.infer == NULL) {
        return;
    }
    /* 创建推理结果并清零 logits，失败时不会泄露上一次窗口值。 */
    imu_pipeline_inference_result_t result;
    /* 清零整个结果结构。 */
    (void)memset(&result, 0, sizeof(result));
    /* 复制窗口末时刻。 */
    result.end_timestamp_us = pipeline->window_scratch.end_timestamp_us;
    /* 复制窗口序号。 */
    result.sequence = pipeline->window_scratch.sequence;
    /* 复制窗口质量。 */
    result.quality_flags = window_quality;
    /* 调用 mock 或生产双 M0，同步输入 [62,6]、输出 [11]。 */
    result.inference_status = pipeline->config.infer(
        pipeline->config.inference_context,
        pipeline->window_scratch.samples,
        result.logits);
    /* 累加模型调用次数。 */
    pipeline->stats.inference_calls += 1U;
    /* 非零状态标记窗口失败并累加统计。 */
    if (result.inference_status != 0) {
        result.quality_flags |= IMU_QUALITY_INFERENCE_FAILED;
        pipeline->stats.inference_failures += 1U;
    }
    /* 无论成功或失败都通知结果观察者，便于 UI/日志显示故障。 */
    if (pipeline->config.on_inference != NULL) {
        pipeline->config.on_inference(pipeline->config.callback_context, &result);
    }
}

/* 把一个严格 25 Hz 点加入 62 点环形窗，并按首窗/12 点步长触发。 */
static void imu_pipeline_append_resampled(
    imu_pipeline_t *pipeline,
    const imu_resampled_sample_t *sample)
{
    /* 写入当前槽六轴物理量。 */
    (void)memcpy(
        pipeline->ring_samples[pipeline->ring_write_index],
        sample->axes,
        sizeof(sample->axes));
    /* 写入当前槽时间戳。 */
    pipeline->ring_timestamps_us[pipeline->ring_write_index] = sample->timestamp_us;
    /* 写入当前槽质量位。 */
    pipeline->ring_quality_flags[pipeline->ring_write_index] = sample->quality_flags;
    /* 下一写槽循环前进。 */
    pipeline->ring_write_index = (uint8_t)(
        (pipeline->ring_write_index + 1U) % IMU_PIPELINE_WINDOW_SAMPLES);
    /* 未满时增加有效点数；满后保持 62 并覆盖最旧点。 */
    if (pipeline->ring_count < IMU_PIPELINE_WINDOW_SAMPLES) {
        pipeline->ring_count += 1U;
    }
    /* 新连续段以来或上窗口以来新增点数增加一。 */
    pipeline->points_since_last_window += 1U;
    /* 只有满 62 点且新增数达到 12 才触发；首窗在 62 点时自然满足条件。 */
    if ((pipeline->ring_count == IMU_PIPELINE_WINDOW_SAMPLES) &&
        (pipeline->points_since_last_window >= IMU_PIPELINE_WINDOW_STEP)) {
        /* 展开并推理当前窗口。 */
        imu_pipeline_emit_window(pipeline);
        /* 窗口触发后从零计下一批 12 个新点。 */
        pipeline->points_since_last_window = 0U;
    }
}

/* 尝试用两路队列产生尽可能多的严格 25 Hz 点。 */
static imu_pipeline_result_t imu_pipeline_pump(imu_pipeline_t *pipeline)
{
    /* 两路任一路为空时无法建立共同时间区间。 */
    if ((pipeline->accel_stream.queue_count == 0U) ||
        (pipeline->gyro_stream.queue_count == 0U)) {
        return IMU_PIPELINE_OK;
    }
    /* 尚无网格时，从两路最早可用点中较晚者向上对齐。 */
    if (!pipeline->schedule_valid) {
        /* 读取两路最早滤波点。 */
        const uint64_t accel_start = imu_stream_point_at(&pipeline->accel_stream, 0U)->timestamp_us;
        /* 读取陀螺最早滤波点。 */
        const uint64_t gyro_start = imu_stream_point_at(&pipeline->gyro_stream, 0U)->timestamp_us;
        /* 共同起点必须不早于任一路首点。 */
        const uint64_t common_start = accel_start >= gyro_start ? accel_start : gyro_start;
        /* 对齐到全局 40 ms 网格；极端溢出直接报告时间戳错误。 */
        if (!imu_pipeline_ceil_output_grid(common_start, &pipeline->next_output_timestamp_us)) {
            return IMU_PIPELINE_ERR_TIMESTAMP;
        }
        /* 标记网格有效，后续只按固定 40,000 微秒递增。 */
        pipeline->schedule_valid = true;
    }
    /* 当前两路队列可能包含多个目标点，循环尽量排空可插值输出。 */
    while (pipeline->schedule_valid) {
        /* 保存当前目标时刻，避免回调中读取变化后的 next。 */
        const uint64_t target_us = pipeline->next_output_timestamp_us;
        /* 保存加速度插值输出，单位 g。 */
        float accel_xyz[IMU_PIPELINE_VECTOR_AXIS_COUNT];
        /* 保存陀螺插值输出，单位 deg/s。 */
        float gyro_xyz[IMU_PIPELINE_VECTOR_AXIS_COUNT];
        /* 保存加速度端点质量。 */
        uint32_t accel_quality = IMU_QUALITY_OK;
        /* 保存陀螺端点质量。 */
        uint32_t gyro_quality = IMU_QUALITY_OK;
        /* 尝试获取加速度目标值。 */
        const imu_interpolate_status_t accel_status = imu_stream_interpolate(
            &pipeline->accel_stream,
            target_us,
            accel_xyz,
            &accel_quality);
        /* 尝试获取陀螺目标值。 */
        const imu_interpolate_status_t gyro_status = imu_stream_interpolate(
            &pipeline->gyro_stream,
            target_us,
            gyro_xyz,
            &gyro_quality);
        /* 任一路队列首点已跨过目标，丢弃旧连续窗口并从现存共同点重对齐。 */
        if ((accel_status == IMU_INTERPOLATE_TARGET_TOO_OLD) ||
            (gyro_status == IMU_INTERPOLATE_TARGET_TOO_OLD)) {
            imu_pipeline_reset_contiguity(pipeline, IMU_QUALITY_QUEUE_OVERFLOW);
            /* 重置后由下一轮 pump 使用当前队列首点重新建立网格。 */
            return imu_pipeline_pump(pipeline);
        }
        /* 任一路尚缺右端点时退出，等待下一帧 push 再继续。 */
        if ((accel_status != IMU_INTERPOLATE_READY) ||
            (gyro_status != IMU_INTERPOLATE_READY)) {
            return IMU_PIPELINE_OK;
        }
        /* 创建一个严格网格六轴点。 */
        imu_resampled_sample_t output;
        /* 写入目标单调时刻。 */
        output.timestamp_us = target_us;
        /* 前三轴严格写入 gx、gy、gz，单位 deg/s。 */
        output.axes[IMU_AXIS_GX] = gyro_xyz[0];
        /* 写入 gy。 */
        output.axes[IMU_AXIS_GY] = gyro_xyz[1];
        /* 写入 gz。 */
        output.axes[IMU_AXIS_GZ] = gyro_xyz[2];
        /* 后三轴严格写入 ax、ay、az，单位 g。 */
        output.axes[IMU_AXIS_AX] = accel_xyz[0];
        /* 写入 ay。 */
        output.axes[IMU_AXIS_AY] = accel_xyz[1];
        /* 写入 az。 */
        output.axes[IMU_AXIS_AZ] = accel_xyz[2];
        /* 合并两路端点和此前间断质量。 */
        output.quality_flags = accel_quality | gyro_quality | pipeline->pending_quality_flags;
        /* 目标时刻落在马达区间时显式增加污染位。 */
        if (imu_pipeline_timestamp_is_haptic(pipeline, target_us)) {
            output.quality_flags |= IMU_QUALITY_HAPTIC_CONTAMINATED;
        }
        /* 间断质量只附着新连续段首点，环形窗仍会把它传播到相应窗口。 */
        pipeline->pending_quality_flags = IMU_QUALITY_OK;
        /* 累加输出点统计。 */
        pipeline->stats.resampled_points += 1U;
        /* 先通知单点观察者；回调只读且不得重入 push。 */
        if (pipeline->config.on_sample != NULL) {
            pipeline->config.on_sample(pipeline->config.callback_context, &output);
        }
        /* 再写入 62 点环形窗并按 12 点步长触发。 */
        imu_pipeline_append_resampled(pipeline, &output);
        /* 下一个时刻固定增加 40,000 微秒；先检查 uint64 溢出。 */
        if (pipeline->next_output_timestamp_us >
            (UINT64_MAX - IMU_PIPELINE_OUTPUT_PERIOD_US)) {
            pipeline->schedule_valid = false;
            return IMU_PIPELINE_ERR_TIMESTAMP;
        }
        /* 严格递增下一目标网格，不使用原始 ODR 反推。 */
        pipeline->next_output_timestamp_us += IMU_PIPELINE_OUTPUT_PERIOD_US;
    }
    /* 理论上循环只从内部 return 退出，保留返回满足编译器控制流检查。 */
    return IMU_PIPELINE_OK;
}

/* 把低通点加入单路队列；队列满时丢弃最旧点并重置连续窗口。 */
static void imu_stream_enqueue(
    imu_pipeline_t *pipeline,
    imu_pipeline_stream_state_t *stream,
    imu_source_t source,
    const imu_pipeline_filtered_point_t *point)
{
    /* 满队列表示另一异步流长期未跟上，最旧点无法继续保留。 */
    if (stream->queue_count == IMU_PIPELINE_STREAM_QUEUE_CAPACITY) {
        /* 丢弃最旧滤波点。 */
        imu_stream_pop_front(stream);
        /* 按数据源累加丢点统计。 */
        if (source == IMU_SOURCE_ACCEL) {
            pipeline->stats.accel_raw_dropped += 1U;
        } else {
            pipeline->stats.gyro_raw_dropped += 1U;
        }
        /* 队列溢出破坏连续插值，旧窗口必须清空。 */
        imu_pipeline_reset_contiguity(pipeline, IMU_QUALITY_QUEUE_OVERFLOW);
    }
    /* 获取当前尾部空槽。 */
    imu_pipeline_filtered_point_t *slot = imu_stream_write_slot(stream);
    /* 复制时间、三轴和质量位。 */
    *slot = *point;
    /* 有效点数增加一。 */
    stream->queue_count += 1U;
}

/* 提交一路 QMI 原始点：换算、丢样检查、一阶低通、入队和泵送。 */
static imu_pipeline_result_t imu_pipeline_push_source_raw(
    imu_pipeline_t *pipeline,
    imu_source_t source,
    const imu_qmi_raw_sample_t *sample)
{
    /* 流水线和样本必须有效且已经初始化。 */
    if ((pipeline == NULL) || (sample == NULL) || !pipeline->initialized) {
        return IMU_PIPELINE_ERR_ARGUMENT;
    }
    /* 只接受明确的加速度或陀螺源。 */
    if ((source != IMU_SOURCE_ACCEL) && (source != IMU_SOURCE_GYRO)) {
        return IMU_PIPELINE_ERR_ARGUMENT;
    }
    /* 选择对应流状态。 */
    imu_pipeline_stream_state_t *stream = source == IMU_SOURCE_ACCEL ?
        &pipeline->accel_stream : &pipeline->gyro_stream;
    /* 选择 QMI 量程换算系数。 */
    const float scale = source == IMU_SOURCE_ACCEL ?
        pipeline->config.accel_g_per_lsb : pipeline->config.gyro_dps_per_lsb;
    /* 选择名义原始周期。 */
    const uint64_t expected_period_us = source == IMU_SOURCE_ACCEL ?
        IMU_PIPELINE_ACCEL_EXPECTED_PERIOD_US : IMU_PIPELINE_GYRO_EXPECTED_PERIOD_US;
    /* 收到帧即累加统计，包括后续被时间戳检查拒绝的帧。 */
    if (source == IMU_SOURCE_ACCEL) {
        pipeline->stats.accel_raw_received += 1U;
    } else {
        pipeline->stats.gyro_raw_received += 1U;
    }
    /* 已初始化流要求时间严格递增。 */
    if (stream->filter_initialized &&
        (sample->timestamp_us <= stream->last_raw_timestamp_us)) {
        /* 记录拒绝原因，传播到下一有效输出。 */
        pipeline->pending_quality_flags |= IMU_QUALITY_OUT_OF_ORDER;
        /* 被拒绝帧计为源丢点。 */
        if (source == IMU_SOURCE_ACCEL) {
            pipeline->stats.accel_raw_dropped += 1U;
        } else {
            pipeline->stats.gyro_raw_dropped += 1U;
        }
        /* 返回时间戳错误且不修改滤波器。 */
        return IMU_PIPELINE_ERR_TIMESTAMP;
    }
    /* 计算与上一原始帧间隔；首帧没有可比较间隔。 */
    uint64_t delta_us = 0ULL;
    /* 有上一帧时执行丢样检测。 */
    if (stream->filter_initialized) {
        /* 单调检查已通过，减法不会下溢。 */
        delta_us = sample->timestamp_us - stream->last_raw_timestamp_us;
        /* 超过 1.5 个名义周期视为至少丢一帧，避免跨缺口线性插值。 */
        if (delta_us > (expected_period_us + expected_period_us / 2ULL)) {
            /* 以最接近的周期数估算缺失数量。 */
            const uint64_t interval_count =
                (delta_us + expected_period_us / 2ULL) / expected_period_us;
            /* 至少跨过两个周期时缺失数为 interval_count-1。 */
            const uint32_t missing_count = interval_count > 1ULL ?
                (uint32_t)(interval_count - 1ULL) : 1U;
            /* 选择数据源质量位。 */
            const uint32_t gap_flag = source == IMU_SOURCE_ACCEL ?
                IMU_QUALITY_ACCEL_GAP : IMU_QUALITY_GYRO_GAP;
            /* 累加估算丢点。 */
            if (source == IMU_SOURCE_ACCEL) {
                pipeline->stats.accel_raw_dropped += missing_count;
            } else {
                pipeline->stats.gyro_raw_dropped += missing_count;
            }
            /* 当前流从本帧重新启动滤波和插值。 */
            imu_stream_reset(stream);
            /* 清空旧 62 点连续窗口，并把间断质量传播到新段。 */
            imu_pipeline_reset_contiguity(pipeline, gap_flag);
            /* 重置后本帧按首帧处理，不使用跨缺口 delta。 */
            delta_us = 0ULL;
        }
    }
    /* 把原始整数换算为三轴物理量。 */
    float physical_xyz[IMU_PIPELINE_VECTOR_AXIS_COUNT];
    /* 遍历 x、y、z，比例单位分别是 g/LSB 或 deg/s/LSB。 */
    for (uint8_t axis = 0U; axis < IMU_PIPELINE_VECTOR_AXIS_COUNT; axis += 1U) {
        /* int16 转 float 后乘量程比例。 */
        physical_xyz[axis] = (float)sample->raw_xyz[axis] * scale;
    }
    /* 首帧直接作为滤波初值，避免从零起步产生人为长过渡。 */
    if (!stream->filter_initialized) {
        /* 逐轴复制物理量到低通状态。 */
        for (uint8_t axis = 0U; axis < IMU_PIPELINE_VECTOR_AXIS_COUNT; axis += 1U) {
            /* 常量输入首点保持不变。 */
            stream->filtered_xyz[axis] = physical_xyz[axis];
        }
        /* 标记滤波器已建立。 */
        stream->filter_initialized = true;
    } else {
        /* 一阶 RC 低通时间常数，单位微秒：RC=1/(2πfc)。 */
        const float rc_us = 1000000.0F /
                            (IMU_PIPELINE_TWO_PI * IMU_PIPELINE_LOWPASS_CUTOFF_HZ);
        /* 离散系数 α=Δt/(RC+Δt)，范围严格位于 0~1。 */
        const float alpha = (float)delta_us / (rc_us + (float)delta_us);
        /* 对三轴独立执行 y[n]=y[n-1]+α(x[n]-y[n-1])。 */
        for (uint8_t axis = 0U; axis < IMU_PIPELINE_VECTOR_AXIS_COUNT; axis += 1U) {
            /* 更新低通状态，衰减 12.5 Hz 以上内容以降低下采样混叠。 */
            stream->filtered_xyz[axis] +=
                alpha * (physical_xyz[axis] - stream->filtered_xyz[axis]);
        }
    }
    /* 当前原始时刻成为下一帧单调检查基准。 */
    stream->last_raw_timestamp_us = sample->timestamp_us;
    /* 构造低通后队列点。 */
    imu_pipeline_filtered_point_t point;
    /* 保留原始时间戳用于异步线性插值。 */
    point.timestamp_us = sample->timestamp_us;
    /* 复制三轴滤波值。 */
    (void)memcpy(point.xyz, stream->filtered_xyz, sizeof(point.xyz));
    /* 继承驱动质量位。 */
    point.quality_flags = sample->quality_flags;
    /* 当前原始时刻落入马达保护区时增加污染标志。 */
    if (imu_pipeline_timestamp_is_haptic(pipeline, sample->timestamp_us)) {
        point.quality_flags |= IMU_QUALITY_HAPTIC_CONTAMINATED;
    }
    /* 把低通点放入异步时间队列。 */
    imu_stream_enqueue(pipeline, stream, source, &point);
    /* 两路队列可能已共同覆盖多个目标时刻，立即尽量输出。 */
    return imu_pipeline_pump(pipeline);
}

/* 初始化配置和静态状态。 */
imu_pipeline_result_t imu_pipeline_init(
    imu_pipeline_t *pipeline,
    const imu_pipeline_config_t *config)
{
    /* 两个指针都必须有效。 */
    if ((pipeline == NULL) || (config == NULL)) {
        return IMU_PIPELINE_ERR_ARGUMENT;
    }
    /* 两个量程比例必须是有限正数，防止 NaN/零比例污染全部特征。 */
    if (!isfinite(config->accel_g_per_lsb) ||
        !isfinite(config->gyro_dps_per_lsb) ||
        (config->accel_g_per_lsb <= 0.0F) ||
        (config->gyro_dps_per_lsb <= 0.0F)) {
        return IMU_PIPELINE_ERR_SCALE;
    }
    /* 清零全部队列、环形窗、统计和标志。 */
    (void)memset(pipeline, 0, sizeof(*pipeline));
    /* 复制量程和回调；函数不拥有外部上下文。 */
    pipeline->config = *config;
    /* 最后标记初始化成功，避免部分配置对象被 push 使用。 */
    pipeline->initialized = true;
    /* 返回成功。 */
    return IMU_PIPELINE_OK;
}

/* 清空一次会话的信号连续性，保留量程、回调和累计统计。 */
void imu_pipeline_reset_session(imu_pipeline_t *pipeline)
{
    /* 空指针或未初始化对象无需处理。 */
    if ((pipeline == NULL) || !pipeline->initialized) {
        return;
    }
    /* 清空加速度滤波和队列。 */
    imu_stream_reset(&pipeline->accel_stream);
    /* 清空陀螺滤波和队列。 */
    imu_stream_reset(&pipeline->gyro_stream);
    /* 新会话尚无间断错误，直接清空输出网格而不增加故障统计。 */
    pipeline->schedule_valid = false;
    /* 下一输出时刻等待两路首点重新建立。 */
    pipeline->next_output_timestamp_us = 0ULL;
    /* 清除上一会话残留质量位。 */
    pipeline->pending_quality_flags = IMU_QUALITY_OK;
    /* 环形写指针回到零。 */
    pipeline->ring_write_index = 0U;
    /* 环形有效点数归零。 */
    pipeline->ring_count = 0U;
    /* 首窗口重新积累 62 点。 */
    pipeline->points_since_last_window = 0U;
    /* 新会话窗口序号从零开始。 */
    pipeline->next_window_sequence = 0U;
    /* 清除上一会话马达区间。 */
    pipeline->haptic_interval_valid = false;
    /* 清除马达起点。 */
    pipeline->haptic_start_us = 0ULL;
    /* 清除马达终点。 */
    pipeline->haptic_end_us = 0ULL;
}

/* 提交加速度 QMI 原始点。 */
imu_pipeline_result_t imu_pipeline_push_accel_raw(
    imu_pipeline_t *pipeline,
    const imu_qmi_raw_sample_t *sample)
{
    /* 使用统一入口并明确选择 125 Hz 加速度源。 */
    return imu_pipeline_push_source_raw(pipeline, IMU_SOURCE_ACCEL, sample);
}

/* 提交陀螺 QMI 原始点。 */
imu_pipeline_result_t imu_pipeline_push_gyro_raw(
    imu_pipeline_t *pipeline,
    const imu_qmi_raw_sample_t *sample)
{
    /* 使用统一入口并明确选择 112.1 Hz 陀螺源。 */
    return imu_pipeline_push_source_raw(pipeline, IMU_SOURCE_GYRO, sample);
}

/* 报告驱动/FIFO 已知丢样；旧 62 点连续窗必须作废。 */
imu_pipeline_result_t imu_pipeline_report_source_drop(
    imu_pipeline_t *pipeline,
    imu_source_t source,
    uint32_t count)
{
    /* 流水线必须初始化，丢样数必须大于零。 */
    if ((pipeline == NULL) || !pipeline->initialized || (count == 0U)) {
        return IMU_PIPELINE_ERR_ARGUMENT;
    }
    /* 加速度丢样清空其滤波/队列并累加统计。 */
    if (source == IMU_SOURCE_ACCEL) {
        pipeline->stats.accel_raw_dropped += count;
        imu_stream_reset(&pipeline->accel_stream);
        imu_pipeline_reset_contiguity(
            pipeline,
            IMU_QUALITY_DRIVER_DROP | IMU_QUALITY_ACCEL_GAP);
        return IMU_PIPELINE_OK;
    }
    /* 陀螺丢样采用同一处理。 */
    if (source == IMU_SOURCE_GYRO) {
        pipeline->stats.gyro_raw_dropped += count;
        imu_stream_reset(&pipeline->gyro_stream);
        imu_pipeline_reset_contiguity(
            pipeline,
            IMU_QUALITY_DRIVER_DROP | IMU_QUALITY_GYRO_GAP);
        return IMU_PIPELINE_OK;
    }
    /* 未知数据源拒绝。 */
    return IMU_PIPELINE_ERR_ARGUMENT;
}

/* 注册带 20 ms 前后保护的马达污染区间。 */
imu_pipeline_result_t imu_pipeline_mark_haptic_interval(
    imu_pipeline_t *pipeline,
    uint64_t start_timestamp_us,
    uint64_t end_timestamp_us)
{
    /* 流水线必须初始化，结束时刻不得早于开始时刻。 */
    if ((pipeline == NULL) || !pipeline->initialized ||
        (end_timestamp_us < start_timestamp_us)) {
        return IMU_PIPELINE_ERR_ARGUMENT;
    }
    /* 起点不足 20 ms 时饱和到零，避免无符号下溢。 */
    pipeline->haptic_start_us = start_timestamp_us > IMU_PIPELINE_HAPTIC_GUARD_US ?
        start_timestamp_us - IMU_PIPELINE_HAPTIC_GUARD_US : 0ULL;
    /* 终点加保护前检查 uint64 溢出。 */
    pipeline->haptic_end_us = end_timestamp_us >
        (UINT64_MAX - IMU_PIPELINE_HAPTIC_GUARD_US) ?
        UINT64_MAX : end_timestamp_us + IMU_PIPELINE_HAPTIC_GUARD_US;
    /* 标记区间有效。 */
    pipeline->haptic_interval_valid = true;
    /* 返回成功。 */
    return IMU_PIPELINE_OK;
}

/* 返回累计统计只读指针。 */
const imu_pipeline_stats_t *imu_pipeline_get_stats(const imu_pipeline_t *pipeline)
{
    /* 空或未初始化对象没有可信统计。 */
    if ((pipeline == NULL) || !pipeline->initialized) {
        return NULL;
    }
    /* 返回结构内部地址，调用方不得修改或在流水线销毁后保留。 */
    return &pipeline->stats;
}
