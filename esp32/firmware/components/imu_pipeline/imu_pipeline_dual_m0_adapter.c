/* 引入公开适配器声明，保证函数签名可直接填入 imu_pipeline_config_t.infer。 */
#include "imu_pipeline_dual_m0.h"
/* 引入自动生成双 M0 和 297 维特征唯一实现；权重保持只读 Flash 常量。 */
#include "esp32_dual_m0_model.h"

/* 编译期核对流水线与模型头的窗口长度，防止静默越界。 */
_Static_assert(IMU_PIPELINE_WINDOW_SAMPLES == WINDOW_LEN, "IMU window length mismatch");
/* 编译期核对六轴数量。 */
_Static_assert(IMU_PIPELINE_AXIS_COUNT == AXIS_NUM, "IMU axis count mismatch");
/* 编译期核对 11 类输出数量。 */
_Static_assert(IMU_PIPELINE_CLASS_COUNT == CLASS_NUM, "IMU class count mismatch");

/* 运行现有双 M0；context 当前不参与计算，避免复制模型对象或权重。 */
int imu_pipeline_dual_m0_infer(
    void *context,
    const float window[IMU_PIPELINE_WINDOW_SAMPLES][IMU_PIPELINE_AXIS_COUNT],
    float logits[IMU_PIPELINE_CLASS_COUNT])
{
    /* 当前模型版本固定，显式忽略预留上下文以通过严格编译警告。 */
    (void)context;
    /* 空窗口或输出无法运行同步特征和双模型前向。 */
    if ((window == NULL) || (logits == NULL)) {
        return -1;
    }
    /* 保存基础 M0 的 11 类中间 logits，仅用于自动生成函数完整诊断合同。 */
    float base_logits[IMU_PIPELINE_CLASS_COUNT];
    /* 保存掩码 M0 的 11 类中间 logits。 */
    float masked_logits[IMU_PIPELINE_CLASS_COUNT];
    /* 自动生成函数从 [62,6] 提取 297 特征、运行双 M0 并直接写入融合输出。 */
    return bp_dual_m0_forward_from_window(window, base_logits, masked_logits, logits);
}
