#ifndef IMU_HANDHELD_IMU_PIPELINE_DUAL_M0_H
#define IMU_HANDHELD_IMU_PIPELINE_DUAL_M0_H

/* 引入流水线维度和推理回调签名。 */
#include "imu_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 生产双 M0 适配器：直接引用 esp32/include 自动生成模型头。
 * context 是预留模型选择上下文，当前允许为空；函数只在本次调用期间借用该指针，
 * 不读取、复制、释放或延长其生命周期，调用结束后所有权仍归调用者。
 * window 必须非空，形状固定为 [62][6]，每行通道顺序为 gx、gy、gz、ax、ay、az；
 * 前三轴单位 deg/s，后三轴单位 g，内存生命周期必须覆盖完整同步推理调用。
 * logits 必须非空且可写，形状固定为 [11]，按类别表顺序保存无量纲融合分数；
 * 缓冲区由调用者持有，生命周期必须覆盖本次调用，函数不会保存其地址。
 * 返回 0 表示推理成功；window/logits 为空或生成模型前向失败时返回负数。
 */
int imu_pipeline_dual_m0_infer(
    /* 可为空的预留上下文；仅在调用期间借用且当前不解引用。 */
    void *context,
    /* 非空只读六轴窗口，形状 [62][6]，单位依次为 deg/s、deg/s、deg/s、g、g、g。 */
    const float window[IMU_PIPELINE_WINDOW_SAMPLES][IMU_PIPELINE_AXIS_COUNT],
    /* 非空可写输出，形状 [11]，保存无量纲 logits，内存由调用者管理。 */
    float logits[IMU_PIPELINE_CLASS_COUNT]);

#ifdef __cplusplus
}
#endif

#endif
