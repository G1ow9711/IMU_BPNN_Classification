#ifndef IMU_HANDHELD_IMU_PIPELINE_DUAL_M0_H
#define IMU_HANDHELD_IMU_PIPELINE_DUAL_M0_H

/* 引入流水线维度和推理回调签名。 */
#include "imu_pipeline.h"

/* 引入固定宽度整数，诊断动作索引、Q15 置信度和微秒耗时使用稳定宽度。 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 保存一次双 M0 前向的中间诊断；对象由调用者持有并通过 inference_context 传入。 */
typedef struct imu_pipeline_dual_m0_diagnostics {
    /* 一表示本对象与最近一次成功推理对应，零表示没有可发布的模型结果。 */
    uint8_t valid;
    /* 基础 M0 的 Top-1 类别索引，取值 0～10。 */
    uint8_t base_action_id;
    /* 掩码 M0 的 Top-1 类别索引，取值 0～10。 */
    uint8_t masked_action_id;
    /* 0.85/0.15 融合 logits 的 Top-1 类别索引，取值 0～10。 */
    uint8_t fused_action_id;
    /* 基础 M0 稳定 softmax Top-1 概率的 Q15 表示，0～65535 对应 0～1。 */
    uint16_t base_confidence_q15;
    /* 掩码 M0 稳定 softmax Top-1 概率的 Q15 表示，0～65535 对应 0～1。 */
    uint16_t masked_confidence_q15;
    /* 融合 logits 稳定 softmax Top-1 概率的 Q15 表示，0～65535 对应 0～1。 */
    uint16_t fused_confidence_q15;
    /* 297 维特征提取和双 M0 前向的总耗时，单位微秒，由应用 PM 锁包装器填写。 */
    uint32_t inference_time_us;
} imu_pipeline_dual_m0_diagnostics_t;

/*
 * 生产双 M0 适配器：直接引用 esp32/include 自动生成模型头。
 * context 允许为空；非空时必须指向 imu_pipeline_dual_m0_diagnostics_t，函数只在同步调用期间
 * 写入最近一次基础、掩码和融合 Top-1，不复制、释放或延长其生命周期。
 * window 必须非空，形状固定为 [62][6]，每行通道顺序为 gx、gy、gz、ax、ay、az；
 * 前三轴单位 deg/s，后三轴单位 g，内存生命周期必须覆盖完整同步推理调用。
 * logits 必须非空且可写，形状固定为 [11]，按类别表顺序保存无量纲融合分数；
 * 缓冲区由调用者持有，生命周期必须覆盖本次调用，函数不会保存其地址。
 * 返回 0 表示推理成功；window/logits 为空或生成模型前向失败时返回负数。
 */
int imu_pipeline_dual_m0_infer(
    /* 可为空诊断输出；非空时指向 imu_pipeline_dual_m0_diagnostics_t，由调用者长期持有。 */
    void *context,
    /* 非空只读六轴窗口，形状 [62][6]，单位依次为 deg/s、deg/s、deg/s、g、g、g。 */
    const float window[IMU_PIPELINE_WINDOW_SAMPLES][IMU_PIPELINE_AXIS_COUNT],
    /* 非空可写输出，形状 [11]，保存无量纲 logits，内存由调用者管理。 */
    float logits[IMU_PIPELINE_CLASS_COUNT]);

#ifdef __cplusplus
}
#endif

#endif
