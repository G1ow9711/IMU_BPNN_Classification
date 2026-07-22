/* 引入公开适配器声明，保证函数签名可直接填入 imu_pipeline_config_t.infer。 */
#include "imu_pipeline_dual_m0.h"
/* 引入自动生成双 M0 和 297 维特征唯一实现；权重保持只读 Flash 常量。 */
#include "esp32_dual_m0_model.h"

/* 引入 expf、isfinite 和 lroundf，实现减最大值的稳定 softmax 与 Q15 量化。 */
#include <math.h>

/* 编译期核对流水线与模型头的窗口长度，防止静默越界。 */
_Static_assert(IMU_PIPELINE_WINDOW_SAMPLES == WINDOW_LEN, "IMU window length mismatch");
/* 编译期核对六轴数量。 */
_Static_assert(IMU_PIPELINE_AXIS_COUNT == AXIS_NUM, "IMU axis count mismatch");
/* 编译期核对 11 类输出数量。 */
_Static_assert(IMU_PIPELINE_CLASS_COUNT == CLASS_NUM, "IMU class count mismatch");

/*
 * 从 11 维无量纲 logits 计算 Top-1 和对应稳定 softmax 概率。
 * 公式为 p_k = exp(z_k-z_max) / sum_j exp(z_j-z_max)，减去最大值避免 expf 上溢；
 * 输入形状 [11]，输出动作索引 0～10 和 Q15 概率 0～65535，时间复杂度 O(11)、空间 O(1)。
 */
static int imu_pipeline_dual_m0_top1_q15(
    const float logits[IMU_PIPELINE_CLASS_COUNT],
    uint8_t *action_id,
    uint16_t *confidence_q15)
{
    /* 三个指针均为必填，调用者必须提供完整 logits 和两个输出。 */
    if ((logits == NULL) || (action_id == NULL) || (confidence_q15 == NULL)) {
        /* 空参数无法形成诊断结果。 */
        return -1;
    }
    /* 首类作为最大值和 Top-1 初始值，后续遍历只在严格更大时替换。 */
    float maximum = logits[0];
    /* 初始动作索引为类别零。 */
    uint8_t maximum_index = UINT8_C(0);
    /* 首个 logits 必须有限，NaN 或无穷表示模型数值损坏。 */
    if (!isfinite(maximum)) {
        /* 返回数值错误。 */
        return -2;
    }
    /* 遍历剩余十类，寻找最大无量纲分数。 */
    for (uint8_t index = UINT8_C(1); index < IMU_PIPELINE_CLASS_COUNT; ++index) {
        /* 读取当前类别分数。 */
        const float current = logits[index];
        /* 任一非有限值都会污染 softmax 分母，必须拒绝整个窗口。 */
        if (!isfinite(current)) {
            /* 返回数值错误。 */
            return -2;
        }
        /* 严格更大时更新 Top-1；相等时保留较小类别索引，保证跨平台确定性。 */
        if (current > maximum) {
            /* 保存新最大分数。 */
            maximum = current;
            /* 保存对应类别索引。 */
            maximum_index = index;
        }
    }
    /* 保存减最大值后指数项总和，理论范围为 [1,11]。 */
    float exponential_sum = 0.0F;
    /* 遍历全部类别累加稳定 softmax 分母。 */
    for (uint8_t index = UINT8_C(0); index < IMU_PIPELINE_CLASS_COUNT; ++index) {
        /* 减去最大值后指数自变量不大于零，避免单精度上溢。 */
        exponential_sum += expf(logits[index] - maximum);
    }
    /* 分母必须有限且至少为最大类贡献的一，防止除零和异常库结果。 */
    if (!isfinite(exponential_sum) || (exponential_sum < 1.0F)) {
        /* 返回数值错误。 */
        return -2;
    }
    /* 计算最大类概率，输入和分母均已稳定化。 */
    const float probability = 1.0F / exponential_sum;
    /* 概率应处于 [0,1] 且有限；超界表示前置数值合同被破坏。 */
    if (!isfinite(probability) || (probability < 0.0F) || (probability > 1.0F)) {
        /* 返回数值错误。 */
        return -2;
    }
    /* 把概率乘以 65535 并四舍五入为 Q15 无符号整数。 */
    const long quantized = lroundf(probability * 65535.0F);
    /* 理论范围已经受概率保护，夹取用于抵御浮点边界误差。 */
    const long clamped = quantized < 0L ? 0L : (quantized > 65535L ? 65535L : quantized);
    /* 返回确定的 Top-1 类别。 */
    *action_id = maximum_index;
    /* 返回 Q15 置信度。 */
    *confidence_q15 = (uint16_t)clamped;
    /* 计算成功。 */
    return 0;
}

/* 运行现有双 M0；非空 context 接收三路 Top-1 诊断，不复制模型对象或权重。 */
int imu_pipeline_dual_m0_infer(
    void *context,
    const float window[IMU_PIPELINE_WINDOW_SAMPLES][IMU_PIPELINE_AXIS_COUNT],
    float logits[IMU_PIPELINE_CLASS_COUNT])
{
    /* 非空 context 指向调用者长期持有的诊断对象。 */
    imu_pipeline_dual_m0_diagnostics_t *const diagnostics =
        (imu_pipeline_dual_m0_diagnostics_t *)context;
    /* 每次调用先清除有效位，失败不得沿用上一窗口诊断。 */
    if (diagnostics != NULL) {
        /* 零表示当前内容尚未完成。 */
        diagnostics->valid = UINT8_C(0);
        /* 本函数不计时，应用包装器会在返回后覆盖本字段。 */
        diagnostics->inference_time_us = UINT32_C(0);
    }
    /* 空窗口或输出无法运行同步特征和双模型前向。 */
    if ((window == NULL) || (logits == NULL)) {
        return -1;
    }
    /* 保存基础 M0 的 11 类中间 logits，仅用于自动生成函数完整诊断合同。 */
    float base_logits[IMU_PIPELINE_CLASS_COUNT];
    /* 保存掩码 M0 的 11 类中间 logits。 */
    float masked_logits[IMU_PIPELINE_CLASS_COUNT];
    /* 自动生成函数从 [62,6] 提取 297 特征、运行双 M0 并直接写入融合输出。 */
    const int forward_status = bp_dual_m0_forward_from_window(window, base_logits, masked_logits, logits);
    /* 前向失败时返回原错误码，诊断有效位保持零。 */
    if (forward_status != 0) {
        /* 返回生成模型原始状态。 */
        return forward_status;
    }
    /* 调用者不需要中间诊断时直接返回成功，避免额外三个 softmax 计算。 */
    if (diagnostics == NULL) {
        /* 融合 logits 已写入调用者缓冲。 */
        return 0;
    }
    /* 计算基础 M0 的 Top-1 和 Q15 概率。 */
    if (imu_pipeline_dual_m0_top1_q15(
            base_logits,
            &diagnostics->base_action_id,
            &diagnostics->base_confidence_q15) != 0) {
        /* 非有限 logits 作为推理失败返回。 */
        return -2;
    }
    /* 计算掩码 M0 的 Top-1 和 Q15 概率。 */
    if (imu_pipeline_dual_m0_top1_q15(
            masked_logits,
            &diagnostics->masked_action_id,
            &diagnostics->masked_confidence_q15) != 0) {
        /* 非有限 logits 作为推理失败返回。 */
        return -2;
    }
    /* 计算固定 0.85/0.15 融合输出的 Top-1 和 Q15 概率。 */
    if (imu_pipeline_dual_m0_top1_q15(
            logits,
            &diagnostics->fused_action_id,
            &diagnostics->fused_confidence_q15) != 0) {
        /* 非有限 logits 作为推理失败返回。 */
        return -2;
    }
    /* 三路诊断均完成后最后设置有效位，回调不会观察半写状态。 */
    diagnostics->valid = UINT8_C(1);
    /* 前向和诊断计算全部成功。 */
    return 0;
}
