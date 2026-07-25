/* 本文件由 Python 导出器生成；维度、阈值和算法顺序必须与训练端保持一致。 */
/* 头文件保护宏避免同一编译单元重复包含时产生类型或函数重定义。 */
#ifndef ESP32_BP_FEATURES_H
#define ESP32_BP_FEATURES_H

/* math.h 提供 sqrtf、fabsf、cosf、sinf、logf、floorf、expf 和 isfinite。 */
#include <math.h>
/* stdint.h 提供动作段累计器使用的 uint32_t 与 UINT32_MAX。 */
#include <stdint.h>

/* WINDOW_LEN 是单次推理窗口采样点数；62 点在 25 Hz 下对应 2.48 秒。 */
#define WINDOW_LEN 62
/* AXIS_NUM 固定为六轴，索引顺序必须为 gx、gy、gz、ax、ay、az。 */
#define AXIS_NUM 6
/* SAMPLE_RATE_HZ 是六轴同步采样率，单位 Hz。 */
#define SAMPLE_RATE_HZ 25
/* FEATURE_DIM 是 extract_features_from_window 输出的 float 特征数量。 */
#define FEATURE_DIM 297
/* CLASS_NUM 是分类输出数量，索引顺序由训练工件 class_names 固定。 */
#define CLASS_NUM 11
/* SUPPRESS_NORMALIZED_PHASE 为一时把标准化后 184:232 特征固定为训练均值。 */
#define SUPPRESS_NORMALIZED_PHASE 0
/* NORMALIZED_PHASE_MODEL_START 是归一化四阶段组的半开起始索引。 */
#define NORMALIZED_PHASE_MODEL_START 184
/* NORMALIZED_PHASE_MODEL_END 是归一化四阶段组的半开结束索引。 */
#define NORMALIZED_PHASE_MODEL_END 232
/* HAS_FAMILY_SPECIALIST 标记旧平铺模型是否附带动作族专家；最终双 M0 特征头为零。 */
#define HAS_FAMILY_SPECIALIST 0
/* SPECIALIST_CLASS_NUM 是可选专家输出类别数；未启用时为零。 */
#define SPECIALIST_CLASS_NUM 0
/* SPECIALIST_FEATURE_DIM 是可选专家选取的原始特征数量；未启用时为零。 */
#define SPECIALIST_FEATURE_DIM 0
/* HIDDEN1 是旧平铺 BP 第一隐藏层宽度，仅兼容历史单模型导出。 */
#define HIDDEN1 96
/* HIDDEN2 是旧平铺 BP 第二隐藏层宽度，仅兼容历史单模型导出。 */
#define HIDDEN2 64
/* HIDDEN3 是旧平铺 BP 第三隐藏层宽度，仅兼容历史单模型导出。 */
#define HIDDEN3 32
/* PHASE_SEGMENTS 把关键序列等分为四段，用于动作相位统计。 */
#define PHASE_SEGMENTS 4
/* TEMPORAL_LOGIT_HISTORY 是兼容的固定历史平滑槽数；最终主路径使用动作段累计。 */
#define TEMPORAL_LOGIT_HISTORY 15
/* 基础 M0 融合权重由开发验证集锁定，部署端不得再次调参。 */
#define ENSEMBLE_BASE_LOGIT_WEIGHT 0.85f
/* 掩码 M0 融合权重与基础权重之和为一。 */
#define ENSEMBLE_MASKED_LOGIT_WEIGHT 0.15f

/* REST_MOTION_THRESHOLD 是训练角色静坐窗口估计的无量纲静止门槛。 */
static const float REST_MOTION_THRESHOLD = 0.0841871575f;
/* ACTIVE_POINT_THRESHOLD 是逐采样活动分数门槛，活动分数无量纲。 */
static const float ACTIVE_POINT_THRESHOLD = 0.131654367f;
/* HIGH_DYNAMIC_MIN_RATIO 要求高动态动作窗口至少 20% 采样点处于活动状态。 */
static const float HIGH_DYNAMIC_MIN_RATIO = 0.2f;
/* 陀螺单轴孤立尖峰阈值，单位 deg/s。 */
static const float PREPROCESS_GYRO_SPIKE_THRESHOLD_DPS = 300.0f;
/* 加速度计单轴孤立尖峰阈值，单位 g。 */
static const float PREPROCESS_ACC_SPIKE_THRESHOLD_G = 1.5f;

/* CLASS_NAMES 把 0..CLASS_NUM-1 输出索引映射到固定英文动作标识。 */
/*
 * 因果类别分数平滑状态。
 * history 形状为 [15,CLASS_NUM]，只保存当前及过去窗口 logits，不读取未来数据。
 * running_sum 形状为 [CLASS_NUM]，使每次更新复杂度为 O(CLASS_NUM)。
 * ESP32 RAM 占用约为 (15*CLASS_NUM+CLASS_NUM)*4+8 字节；11 类时约 712 字节。
 */
typedef struct {
    /* history 是环形 logit 缓冲区，元素为 softmax 前无量纲类别分数。 */
    float history[TEMPORAL_LOGIT_HISTORY][CLASS_NUM];
    /* running_sum 保存全部有效槽的逐类和。 */
    float running_sum[CLASS_NUM];
    /* count 是有效历史窗口数，范围 0..TEMPORAL_LOGIT_HISTORY。 */
    int count;
    /* next_index 指向下一次写入或覆盖的槽。 */
    int next_index;
} BpTemporalSmoother;

/* 清空设备重连、用户切换或动作会话结束前的全部历史。 */
static inline void bp_temporal_smoother_reset(BpTemporalSmoother* state) {
    /* 空指针表示调用方没有提供状态，直接返回避免崩溃。 */
    if (state == 0) return;
    /* 逐槽逐类清零，防止重置后读到旧会话数据。 */
    for (int slot = 0; slot < TEMPORAL_LOGIT_HISTORY; slot++) {
        /* 遍历固定 CLASS_NUM 个类别。 */
        for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
            /* 当前历史元素恢复为零。 */
            state->history[slot][class_index] = 0.0f;
        }
    }
    /* 清零逐类累计和。 */
    for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
        /* 每个类别累计和恢复为零。 */
        state->running_sum[class_index] = 0.0f;
    }
    /* 有效槽数归零。 */
    state->count = 0;
    /* 下一写槽回到索引零。 */
    state->next_index = 0;
}

/*
 * 加入当前窗口 logits 并输出当前及过去最多 14 个窗口的因果均值和类别。
 * raw_logits、smoothed_logits 均为 [CLASS_NUM]；两者允许指向不同数组，不允许为空。
 * 返回值是平滑后最大 logit 的全局类别索引，范围 0..CLASS_NUM-1；非法状态返回 -1。
 */
static inline int bp_temporal_smoother_update(
    BpTemporalSmoother* state,
    const float raw_logits[CLASS_NUM],
    float smoothed_logits[CLASS_NUM]
) {
    /* 任一必要指针为空时返回 -1，避免访问非法内存。 */
    if (state == 0 || raw_logits == 0 || smoothed_logits == 0) return -1;
    /* 缓冲区已满时，先移除 next_index 指向的最旧槽。 */
    if (state->count == TEMPORAL_LOGIT_HISTORY) {
        /* 逐类从累计和中减去被覆盖值。 */
        for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
            /* running_sum 保持恰好包含最近 14 个旧窗口。 */
            state->running_sum[class_index] -= state->history[state->next_index][class_index];
        }
    } else {
        /* 未满时有效槽数增加一。 */
        state->count++;
    }
    /* 写入当前窗口并更新逐类累计和。 */
    for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
        /* 当前无量纲 logit 写入下一环形槽。 */
        state->history[state->next_index][class_index] = raw_logits[class_index];
        /* 当前分数加入累计和。 */
        state->running_sum[class_index] += raw_logits[class_index];
        /* 除以有效窗口数得到因果均值。 */
        smoothed_logits[class_index] = state->running_sum[class_index] / (float)state->count;
    }
    /* 写指针循环前进。 */
    state->next_index = (state->next_index + 1) % TEMPORAL_LOGIT_HISTORY;
    /* best_index 初始为第零类。 */
    int best_index = 0;
    /* 比较其余类别；严格大于保证平局时选择更早类别，与 argmax 一致。 */
    for (int class_index = 1; class_index < CLASS_NUM; class_index++) {
        /* 当前均值更大时更新类别。 */
        if (smoothed_logits[class_index] > smoothed_logits[best_index]) best_index = class_index;
    }
    /* 返回平滑后的全局动作类别索引。 */
    return best_index;
}

/*
 * 固定双 M0 模型融合：combined = 0.85*base + 0.15*masked。
 * 三个数组形状均为 [CLASS_NUM]，元素为 softmax 前无量纲 logits；允许输出与任一输入共用缓冲区。
 * 权重只由验证集选择，部署端不得再次使用测试集调权；时间复杂度 O(CLASS_NUM)，无额外状态 RAM。
 */
static inline int bp_combine_ensemble_logits(
    const float base_logits[CLASS_NUM],
    const float masked_logits[CLASS_NUM],
    float combined_logits[CLASS_NUM]
) {
    /* 任一必要指针为空时返回 -1，避免访问非法内存。 */
    if (base_logits == 0 || masked_logits == 0 || combined_logits == 0) return -1;
    /* 逐类执行固定凸组合，类别顺序必须与 Python 导出的 CLASS_NAMES 完全一致。 */
    for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
        /* 先读取两个输入，保证 combined_logits 与输入数组共用地址时仍能得到当前类别原值。 */
        const float base_value = base_logits[class_index];
        /* masked_value 来自抑制标准化阶段 184:232 特征的第二个 M0。 */
        const float masked_value = masked_logits[class_index];
        /* 固定 0.85/0.15 融合保持 logit 总尺度，输出仍为无量纲分数。 */
        combined_logits[class_index] =
            ENSEMBLE_BASE_LOGIT_WEIGHT * base_value
            + ENSEMBLE_MASKED_LOGIT_WEIGHT * masked_value;
    }
    /* 返回零表示全部类别已完成融合。 */
    return 0;
}

/*
 * 单个动作活动段的因果累计证据状态。
 * running_sum 形状为 [CLASS_NUM]，count 是从活动段开始到当前的窗口数。
 * 11 类时 RAM 为 11*4+4=48 字节；每窗口时间复杂度 O(CLASS_NUM)。
 * 静止、动作切换、设备断连或用户切换时必须调用 bp_bout_accumulator_reset。
 */
typedef struct {
    /* running_sum 保存活动段内当前及全部历史窗口的逐类融合 logit 和。 */
    float running_sum[CLASS_NUM];
    /* count 使用 32 位无符号整数，正常健身动作段远小于其上限。 */
    uint32_t count;
} BpBoutAccumulator;

/* 清空上一动作段证据，使下一窗口从独立活动段开始判断。 */
static inline void bp_bout_accumulator_reset(BpBoutAccumulator* state) {
    /* 空指针表示调用方没有提供状态，直接返回避免崩溃。 */
    if (state == 0) return;
    /* 逐类清零累计和，防止上一动作标签影响下一动作。 */
    for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
        /* 当前类别的历史融合 logit 和恢复为零。 */
        state->running_sum[class_index] = 0.0f;
    }
    /* 窗口计数归零，下一次更新的均值等于当前窗口 logits。 */
    state->count = 0U;
}

/*
 * 加入当前融合 logits，输出从动作段开始到当前窗口的因果均值和类别。
 * combined_logits、averaged_logits 形状均为 [CLASS_NUM]；两者允许共用缓冲区。
 * 返回最大平均 logit 的类别索引 0..CLASS_NUM-1；空指针或非有限输入返回 -1 且不更新状态。
 */
static inline int bp_bout_accumulator_update(
    BpBoutAccumulator* state,
    const float combined_logits[CLASS_NUM],
    float averaged_logits[CLASS_NUM]
) {
    /* 任一必要指针为空时返回 -1，状态保持不变。 */
    if (state == 0 || combined_logits == 0 || averaged_logits == 0) return -1;
    /* 在修改状态前检查全部类别，避免 NaN 或无穷值永久污染当前动作段。 */
    for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
        /* isfinite 同时拒绝 NaN、正无穷和负无穷。 */
        if (!isfinite(combined_logits[class_index])) return -1;
    }
    /* 极端超长会话达到 uint32 上限时把和与计数同时减半，防止计数回绕。 */
    if (state->count == UINT32_MAX) {
        /* 所有类别累计和使用同一比例缩放，不改变类别排序。 */
        for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
            /* 乘 0.5 降低长期累计量级和浮点溢出风险。 */
            state->running_sum[class_index] *= 0.5f;
        }
        /* 向下取整后的正计数仍保留长期历史，且为新窗口腾出一个计数。 */
        state->count /= 2U;
    }
    /* 先增加窗口数，使首个窗口的分母为一。 */
    state->count += 1U;
    /* 逐类累加当前证据并计算从动作段起点到当前的均值。 */
    for (int class_index = 0; class_index < CLASS_NUM; class_index++) {
        /* 当前无量纲融合 logit 加入该类别历史和。 */
        state->running_sum[class_index] += combined_logits[class_index];
        /* 除以活动段窗口数，输出无量纲平均 logit。 */
        averaged_logits[class_index] = state->running_sum[class_index] / (float)state->count;
    }
    /* best_index 从第零类开始，与 NumPy argmax 的平局规则一致。 */
    int best_index = 0;
    /* 依次比较其余类别，严格大于时才替换最优类别。 */
    for (int class_index = 1; class_index < CLASS_NUM; class_index++) {
        /* 当前平均 logit 更大时记录其全局类别索引。 */
        if (averaged_logits[class_index] > averaged_logits[best_index]) best_index = class_index;
    }
    /* 返回当前动作段累计证据对应的全局动作类别。 */
    return best_index;
}

/*
 * 修复手腕六轴 IMU 窗口中的单轴孤立尖峰。
 * 输入 raw_window 和输出 cleaned_window 形状均为 [WINDOW_LEN,6]，通道顺序固定为 gx、gy、gz、ax、ay、az。
 * 前三轴单位为 deg/s，阈值为 PREPROCESS_GYRO_SPIKE_THRESHOLD_DPS；后三轴单位为 g，阈值为 PREPROCESS_ACC_SPIKE_THRESHOLD_G。
 * 仅当中心点相对前后邻点均值超阈值、前后邻点差小于半阈值且恰好一个轴异常时进行线性插值。
 * 多轴共同冲击、快速动作边沿和首末采样点全部保留；时间复杂度 O(6N)，额外 RAM 为 6N 个 float。
 */
static inline void preprocess_imu_window(
    const float raw_window[WINDOW_LEN][AXIS_NUM],
    float cleaned_window[WINDOW_LEN][AXIS_NUM]
) {
    /* 逐点逐轴复制原始窗口，确保调用方缓冲区不被修改且输出总是完整初始化。 */
    for (int sample = 0; sample < WINDOW_LEN; sample++) {
        /* 六个通道依次复制 gx、gy、gz、ax、ay、az。 */
        for (int axis = 0; axis < AXIS_NUM; axis++) {
            /* 当前值的物理单位由 axis 决定：0..2 为 deg/s，3..5 为 g。 */
            cleaned_window[sample][axis] = raw_window[sample][axis];
        }
    }
    /* 首末点缺少双侧邻点，因此只遍历索引 1 到 WINDOW_LEN-2。 */
    for (int sample = 1; sample < WINDOW_LEN - 1; sample++) {
        /* candidate_count 统计当前时刻满足孤立尖峰条件的轴数；只有等于 1 才允许修复。 */
        int candidate_count = 0;
        /* candidate_axis 保存唯一候选轴，-1 表示尚未发现候选。 */
        int candidate_axis = -1;
        /* candidate_mean 保存唯一候选轴的前后邻点均值，单位随候选轴变化。 */
        float candidate_mean = 0.0f;
        /* 遍历六轴并分别使用角速度或加速度阈值。 */
        for (int axis = 0; axis < AXIS_NUM; axis++) {
            /* threshold 前三轴取 300 deg/s，后三轴取 1.5 g。 */
            float threshold = axis < 3
                ? PREPROCESS_GYRO_SPIKE_THRESHOLD_DPS
                : PREPROCESS_ACC_SPIKE_THRESHOLD_G;
            /* neighbor_mean 是原始前后邻点均值；不读取已修复输出，避免级联插值。 */
            float neighbor_mean =
                (raw_window[sample - 1][axis] + raw_window[sample + 1][axis]) * 0.5f;
            /* center_residual 衡量中心点相对邻点趋势的绝对偏差。 */
            float center_residual = fabsf(raw_window[sample][axis] - neighbor_mean);
            /* neighbor_gap 衡量前后邻点是否处于同一稳定趋势。 */
            float neighbor_gap = fabsf(
                raw_window[sample - 1][axis] - raw_window[sample + 1][axis]
            );
            /* 同时满足大中心偏差和小邻点差时，该轴才是孤立毛刺候选。 */
            if (center_residual > threshold && neighbor_gap < threshold * 0.5f) {
                /* 累加候选轴数，用于拒绝真实多轴冲击。 */
                candidate_count++;
                /* 保存当前候选轴；若最终候选数大于 1，该值不会被使用。 */
                candidate_axis = axis;
                /* 保存线性插值结果，避免修复阶段重复计算。 */
                candidate_mean = neighbor_mean;
            }
        }
        /* 恰好一个轴异常时才替换，保留多轴落地冲击和真实快速转动。 */
        if (candidate_count == 1) {
            /* 写入唯一候选通道，其余五轴保持原值。 */
            cleaned_window[sample][candidate_axis] = candidate_mean;
        }
    }
}

/*
 * 追加一条等间隔序列的八项基础统计：均值、总体标准差、最小值、最大值、RMS、
 * 平均绝对一阶差、围绕均值的过零率和一阶差总体标准差。
 * x 长度为 n，输入单位可为 deg/s、g 或其采样差；feature 形状为 [FEATURE_DIM]，idx 指向下一写入索引。
 * 要求 n>=2 且指针有效；时间复杂度 O(n)，额外空间 O(1)，公式见 docs/算法原理、训练与实时计数.md 第 7.2 节。
 */
static inline void append_series_features(const float* x, int n, float* feature, int* idx) {
    /* sum 累计输入一阶矩，单位继承 x。 */
    float sum = 0.0f;
    /* sum2 累计输入平方和，用于 RMS，单位为 x 的平方。 */
    float sum2 = 0.0f;
    /* min_v 从首样本开始维护最小值，避免依赖固定量程哨兵。 */
    float min_v = x[0];
    /* max_v 从首样本开始维护最大值。 */
    float max_v = x[0];
    /* 第一遍遍历 n 个样本，累计一阶矩、平方和与范围。 */
    for (int i = 0; i < n; i++) {
        /* v 是当前输入样本，物理单位继承 x。 */
        float v = x[i];
        /* 累加原始值供均值计算。 */
        sum += v;
        /* 累加平方值供均方根计算。 */
        sum2 += v * v;
        /* 当前样本更小时更新最小值。 */
        if (v < min_v) min_v = v;
        /* 当前样本更大时更新最大值。 */
        if (v > max_v) max_v = v;
    }
    /* mean 是总体均值，单位继承 x。 */
    float mean = sum / (float)n;
    /* energy 是均方值 E[x^2]，开方后得到 RMS。 */
    float energy = sum2 / (float)n;
    /* variance_sum 采用与 NumPy std 更接近的两遍中心差平方和，避免大均值下 E[x^2]-E[x]^2 消减。 */
    float variance_sum = 0.0f;
    /* 第二遍只累计围绕已知均值的平方差，输入单位平方。 */
    for (int i = 0; i < n; i++) {
        /* centered 是当前值到总体均值的有符号偏差。 */
        float centered = x[i] - mean;
        /* 累计总体方差分子；n 最多 62，float32 不会溢出正常 IMU 范围。 */
        variance_sum += centered * centered;
    }
    /* 除以 n 得到与 np.std 默认 ddof=0 一致的总体方差。 */
    float var = variance_sum / (float)n;
    /* 理论方差非负；夹紧负零附近舍入误差，保护 sqrtf。 */
    if (var < 0.0f) var = 0.0f;
    /* std 是输入序列总体标准差，单位继承 x。 */
    float std = sqrtf(var);
    /* mean_abs_diff 累计相邻绝对差，描述平均变化速度。 */
    float mean_abs_diff = 0.0f;
    /* diff_sum 累计一阶差，用于差分均值。 */
    float diff_sum = 0.0f;
    /* diff_sum2 累计一阶差平方，用于差分方差。 */
    float diff_sum2 = 0.0f;
    /* zcr_count 先累计穿过全窗均值的相邻点对数量。 */
    float zcr_count = 0.0f;
    /* 从第二个样本开始遍历全部 n-1 个相邻差。 */
    for (int i = 1; i < n; i++) {
        /* diff 是当前样本减前一样本的一阶差。 */
        float diff = x[i] - x[i - 1];
        /* 累加相邻绝对差。 */
        mean_abs_diff += fabsf(diff);
        /* 累加有符号差分。 */
        diff_sum += diff;
        /* 累加差分平方。 */
        diff_sum2 += diff * diff;
        /* a 是前一样本相对全窗均值的偏差。 */
        float a = x[i - 1] - mean;
        /* b 是当前样本相对全窗均值的偏差。 */
        float b = x[i] - mean;
        /* 符号乘积为负表示相邻样本严格穿过均值，累计一次。 */
        if (a * b < 0.0f) zcr_count += 1.0f;
    }
    /* std_diff 默认零；n<=1 时不存在有效差分。 */
    float std_diff = 0.0f;
    /* 只有至少两个采样点时才把差分累计量转换为统计量。 */
    if (n > 1) {
        /* 除以相邻点对数得到平均绝对差。 */
        mean_abs_diff = mean_abs_diff / (float)(n - 1);
        /* mean_diff 是一阶差总体均值。 */
        float mean_diff = diff_sum / (float)(n - 1);
        /* diff_var 使用 E[d^2]-E[d]^2，与 Python 差分总体方差合同一致。 */
        float diff_var = diff_sum2 / (float)(n - 1) - mean_diff * mean_diff;
        /* 浮点消减出现微小负值时夹紧到零。 */
        if (diff_var < 0.0f) diff_var = 0.0f;
        /* 开方得到一阶差总体标准差。 */
        std_diff = sqrtf(diff_var);
        /* 把过零计数归一化为 [0,1] 比例。 */
        zcr_count = zcr_count / (float)(n - 1);
    }
    /* 按 Python build_feature_names 固定顺序追加均值。 */
    feature[(*idx)++] = mean;
    /* 追加总体标准差。 */
    feature[(*idx)++] = std;
    /* 追加最小值。 */
    feature[(*idx)++] = min_v;
    /* 追加最大值。 */
    feature[(*idx)++] = max_v;
    /* 追加 RMS；energy 非负。 */
    feature[(*idx)++] = sqrtf(energy);
    /* 追加平均绝对一阶差。 */
    feature[(*idx)++] = mean_abs_diff;
    /* 追加围绕全窗均值的过零比例。 */
    feature[(*idx)++] = zcr_count;
    /* 追加一阶差总体标准差，完成八项输出。 */
    feature[(*idx)++] = std_diff;
}

/*
 * 把长度 n 的序列等分为四个时间相位，并为每段追加均值、总体标准差和最大绝对值。
 * 输入单位继承 x，输出 12 项保持对应物理单位；时间复杂度 O(n)，额外空间 O(1)。
 */
static inline void append_phase_features(const float* x, int n, float* feature, int* idx) {
    /* 依次处理 0..PHASE_SEGMENTS-1，整数边界覆盖全部 n 个样本且不重叠。 */
    for (int phase = 0; phase < PHASE_SEGMENTS; phase++) {
        /* start 是当前半开区间起点。 */
        int start = (phase * n) / PHASE_SEGMENTS;
        /* end 是当前半开区间终点。 */
        int end = ((phase + 1) * n) / PHASE_SEGMENTS;
        /* 极短输入发生空段时至少保留一个采样点。 */
        if (end <= start) end = start + 1;
        /* 末段边界不得超过输入长度。 */
        if (end > n) end = n;
        /* sum 累计当前相位样本，用于均值。 */
        float sum = 0.0f;
        /* max_abs 保存当前相位最大绝对幅值。 */
        float max_abs = 0.0f;
        /* 第一遍处理当前相位的全部样本。 */
        for (int i = start; i < end; i++) {
            /* value 是当前相位样本，单位继承 x。 */
            float value = x[i];
            /* 累加样本值。 */
            sum += value;
            /* abs_value 用于与当前最大绝对值比较。 */
            float abs_value = fabsf(value);
            /* 发现更大绝对幅值时更新。 */
            if (abs_value > max_abs) max_abs = abs_value;
        }
        /* count 是当前相位实际样本数，经过边界保护后至少为一。 */
        int count = end - start;
        /* mean 是相位均值。 */
        float mean = sum / (float)count;
        /* variance_sum 使用两遍中心差平方和，降低大幅角速度相位段的消减误差。 */
        float variance_sum = 0.0f;
        /* 再遍历当前相位段，计算相对已知均值的平方偏差。 */
        for (int i = start; i < end; i++) {
            /* centered 单位与原序列一致。 */
            float centered = x[i] - mean;
            /* 累计总体方差分子。 */
            variance_sum += centered * centered;
        }
        /* ddof=0 与 Python np.std 保持一致。 */
        float variance = variance_sum / (float)count;
        /* 理论方差非负；夹紧舍入产生的微小负值。 */
        if (variance < 0.0f) variance = 0.0f;
        /* 按均值、标准差、最大绝对值固定顺序追加。 */
        feature[(*idx)++] = mean;
        /* 追加相位总体标准差。 */
        feature[(*idx)++] = sqrtf(variance);
        /* 追加相位最大绝对幅值。 */
        feature[(*idx)++] = max_abs;
    }
}

/*
 * 先在全窗执行 z=(x-mean)/std，再对无量纲序列追加四相位统计。
 * std<=1e-6 时全部标准分取零，防止除零；时间复杂度 O(n)，额外空间 WINDOW_LEN 个 float。
 */
static inline void append_normalized_phase_features(const float* x, int n, float* feature, int* idx) {
    /* sum 累计原序列一阶矩。 */
    float sum = 0.0f;
    /* sum2 累计原序列平方和。 */
    float sum2 = 0.0f;
    /* normalized 保存最多 WINDOW_LEN 个无量纲标准分。 */
    float normalized[WINDOW_LEN];
    /* 遍历输入并累计均值与方差所需统计量。 */
    for (int i = 0; i < n; i++) {
        /* 累加当前原始值。 */
        sum += x[i];
        /* 累加当前原始值平方。 */
        sum2 += x[i] * x[i];
    }
    /* mean 是全窗均值，单位继承 x。 */
    float mean = sum / (float)n;
    /* variance 是总体方差，单位为输入单位平方。 */
    float variance = sum2 / (float)n - mean * mean;
    /* 夹紧浮点消减导致的负值。 */
    if (variance < 0.0f) variance = 0.0f;
    /* std 是全窗总体标准差。 */
    float std = sqrtf(variance);
    /* 逐点生成无量纲标准分；近常量序列统一置零。 */
    for (int i = 0; i < n; i++) {
        /* 三元表达式阻止 std 近零时出现 NaN 或无穷值。 */
        normalized[i] = std > 1e-6f ? (x[i] - mean) / std : 0.0f;
    }
    /* 复用相位统计函数追加 12 项无量纲特征。 */
    append_phase_features(normalized, n, feature, idx);
}

/*
 * 追加 10/25/50/75/90 百分位、偏度、超额峰度和最大相邻跳变共八项冲击分布特征。
 * 输入 x 长度为 n 且 n<=WINDOW_LEN；分位数使用最近位置规则，时间最坏 O(n^2)，空间 O(n)。
 * 偏度和峰度无量纲，分位数与最大跳变单位继承 x；公式见 docs/算法原理、训练与实时计数.md 第 7.6 节。
 */
static inline void append_impact_distribution_features(const float* x, int n, float* feature, int* idx) {
    /* ordered 保存输入副本并就地插入排序，不修改调用者序列。 */
    float ordered[WINDOW_LEN];
    /* sum 累计一阶矩。 */
    float sum = 0.0f;
    /* sum2 累计平方和。 */
    float sum2 = 0.0f;
    /* max_abs_diff 保存相邻采样最大绝对跳变。 */
    float max_abs_diff = 0.0f;
    /* 复制 n 个输入并收集矩与跳变统计。 */
    for (int i = 0; i < n; i++) {
        /* 保存当前输入供后续排序。 */
        ordered[i] = x[i];
        /* 累加当前输入。 */
        sum += x[i];
        /* 累加当前输入平方。 */
        sum2 += x[i] * x[i];
        /* 首点没有前驱，从第二点开始计算相邻差。 */
        if (i > 0) {
            /* abs_diff 是当前点与前一点的绝对变化。 */
            float abs_diff = fabsf(x[i] - x[i - 1]);
            /* 更大跳变出现时更新最大值。 */
            if (abs_diff > max_abs_diff) max_abs_diff = abs_diff;
        }
    }
    /* 插入排序从第二个元素开始，固定小窗无需动态内存。 */
    for (int i = 1; i < n; i++) {
        /* value 保存当前待插入值。 */
        float value = ordered[i];
        /* j 从已排序前缀末端向前寻找位置。 */
        int j = i - 1;
        /* 大于 value 的元素依次右移。 */
        while (j >= 0 && ordered[j] > value) {
            /* 当前较大元素右移一格。 */
            ordered[j + 1] = ordered[j];
            /* 继续检查更前元素。 */
            j--;
        }
        /* 把 value 写入最终有序位置。 */
        ordered[j + 1] = value;
    }
    /* fractions 固定五个百分位，顺序必须与 Python 特征名一致。 */
    const float fractions[5] = { 0.10f, 0.25f, 0.50f, 0.75f, 0.90f };
    /* 逐百分位按 floor(q*(n-1)+0.5) 选取最近秩位置。 */
    for (int q = 0; q < 5; q++) {
        /* position 经过 q 范围保护后位于 0..n-1。 */
        int position = (int)floorf(fractions[q] * (float)(n - 1) + 0.5f);
        /* 追加当前百分位值。 */
        feature[(*idx)++] = ordered[position];
    }
    /* mean 是原序列总体均值。 */
    float mean = sum / (float)n;
    /* variance 使用总体二阶矩计算。 */
    float variance = sum2 / (float)n - mean * mean;
    /* 夹紧浮点消减造成的微小负值。 */
    if (variance < 0.0f) variance = 0.0f;
    /* std 是总体标准差。 */
    float std = sqrtf(variance);
    /* skew 默认零，近常量序列没有稳定偏度。 */
    float skew = 0.0f;
    /* kurtosis 默认零，近常量序列没有稳定峰度。 */
    float kurtosis = 0.0f;
    /* 只有标准差大于 1e-6 才执行高阶标准化，避免除零放大噪声。 */
    if (std > 1e-6f) {
        /* 遍历全部样本累计标准分三次方和四次方。 */
        for (int i = 0; i < n; i++) {
            /* z 是当前样本无量纲标准分。 */
            float z = (x[i] - mean) / std;
            /* z2 复用平方，减少乘法。 */
            float z2 = z * z;
            /* 累计三阶标准矩。 */
            skew += z2 * z;
            /* 累计四阶标准矩。 */
            kurtosis += z2 * z2;
        }
        /* 除以 n 得到总体偏度。 */
        skew /= (float)n;
        /* 四阶总体标准矩减三得到超额峰度。 */
        kurtosis = kurtosis / (float)n - 3.0f;
    }
    /* 追加无量纲偏度。 */
    feature[(*idx)++] = skew;
    /* 追加无量纲超额峰度。 */
    feature[(*idx)++] = kurtosis;
    /* 追加最大相邻绝对跳变，完成八项输出。 */
    feature[(*idx)++] = max_abs_diff;
}

/*
 * 追加高活动比例、峰密度、主频、归一化谱熵、最强自相关和对应延迟六项时序特征。
 * 输入长度 n<=WINDOW_LEN，采样率 SAMPLE_RATE_HZ；频率单位 Hz，其余输出无量纲或秒。
 * 直接 DFT 与自相关使时间复杂度为 O(n^2)，额外空间 O(n)；近常量序列返回确定性零。
 */
static inline void append_temporal_features(const float* x, int n, float* feature, int* idx) {
    /* sum 累计序列一阶矩。 */
    float sum = 0.0f;
    /* sum2 累计序列平方和。 */
    float sum2 = 0.0f;
    /* 遍历全部输入样本收集总体统计量。 */
    for (int i = 0; i < n; i++) {
        /* 累加当前样本。 */
        sum += x[i];
        /* 累加当前样本平方。 */
        sum2 += x[i] * x[i];
    }
    /* mean 是全窗总体均值。 */
    float mean = sum / (float)n;
    /* variance 是总体方差。 */
    float variance = sum2 / (float)n - mean * mean;
    /* 夹紧舍入产生的微小负值，保护平方根。 */
    if (variance < 0.0f) variance = 0.0f;
    /* std 是总体标准差。 */
    float std = sqrtf(variance);
    /* high_count 统计偏离均值超过一个标准差的活动点。 */
    int high_count = 0;
    /* peak_count 统计显著绝对局部峰。 */
    int peak_count = 0;
    /* 遍历每个样本计算活动量，并在有双邻点时检测峰。 */
    for (int i = 0; i < n; i++) {
        /* activity 是当前样本相对均值的绝对偏差。 */
        float activity = fabsf(x[i] - mean);
        /* 非常量序列中，超过一倍标准差视为高活动点。 */
        if (std > 1e-6f && activity > std) high_count++;
        /* 局部峰必须有前后邻点且序列方差有效。 */
        if (i > 0 && i < n - 1 && std > 1e-6f) {
            /* previous 是前一样本相对均值的绝对偏差。 */
            float previous = fabsf(x[i - 1] - mean);
            /* next 是后一样本相对均值的绝对偏差。 */
            float next = fabsf(x[i + 1] - mean);
            /* 当前严格高于前点、不低于后点且超过标准差时累计显著峰。 */
            if (activity > previous && activity >= next && activity > std) peak_count++;
        }
    }

    /* frequency_bin_count 是不含直流的单边频点数。 */
    int frequency_bin_count = n / 2;
    /* spectral_power 保存频点 1..n/2 的功率，最大长度 WINDOW_LEN/2+1。 */
    float spectral_power[WINDOW_LEN / 2 + 1];
    /* total_power 累计全部非直流频点功率。 */
    float total_power = 0.0f;
    /* dominant_power 从负值开始，确保首个频点可成为主频。 */
    float dominant_power = -1.0f;
    /* dominant_bin 保存最强功率频点索引。 */
    int dominant_bin = 0;
    /* two_pi 是直接 DFT 相位使用的 2*pi 单精度常量。 */
    const float two_pi = 6.2831853071795864769f;
    /* 遍历不含直流的单边频点并计算去均值 DFT。 */
    for (int k = 1; k <= frequency_bin_count; k++) {
        /* real 累加当前频点实部。 */
        float real = 0.0f;
        /* imaginary 累加当前频点虚部。 */
        float imaginary = 0.0f;
        /* 遍历 n 个时域样本执行直接 DFT。 */
        for (int sample = 0; sample < n; sample++) {
            /* centered 去除直流均值，单位继承输入。 */
            float centered = x[sample] - mean;
            /* angle 是当前频点与采样位置的相位，单位 rad。 */
            float angle = two_pi * (float)k * (float)sample / (float)n;
            /* 累加余弦实部。 */
            real += centered * cosf(angle);
            /* 使用负正弦约定累加虚部。 */
            imaginary -= centered * sinf(angle);
        }
        /* power 是当前频点幅值平方。 */
        float power = real * real + imaginary * imaginary;
        /* 数组索引 k-1 对应频点 k。 */
        spectral_power[k - 1] = power;
        /* 累加非直流总功率。 */
        total_power += power;
        /* 严格更大时更新主频，平局保留较低频点。 */
        if (power > dominant_power) {
            /* 保存新的最大功率。 */
            dominant_power = power;
            /* 保存新的主频索引。 */
            dominant_bin = k;
        }
    }
    /* dominant_frequency_hz 默认零，覆盖无有效频谱场景。 */
    float dominant_frequency_hz = 0.0f;
    /* spectral_entropy 默认零，覆盖无功率或单频点场景。 */
    float spectral_entropy = 0.0f;
    /* 总功率有效且至少一个频点时才计算频率与熵。 */
    if (total_power > 1e-12f && frequency_bin_count > 0) {
        /* k*fs/n 把主频索引换算为 Hz。 */
        dominant_frequency_hz = (float)dominant_bin * (float)SAMPLE_RATE_HZ / (float)n;
        /* 至少两个频点才能用 log(K) 归一化谱熵。 */
        if (frequency_bin_count > 1) {
            /* 遍历全部单边非直流频点。 */
            for (int k = 0; k < frequency_bin_count; k++) {
                /* probability 是当前频点功率占总功率比例。 */
                float probability = spectral_power[k] / total_power;
                /* 零概率项按极限 p*log(p)=0 跳过。 */
                if (probability > 0.0f) {
                    /* 累加 Shannon 熵的负 p*ln(p)。 */
                    spectral_entropy -= probability * logf(probability);
                }
            }
            /* 除以 ln(K) 把谱熵归一化到 [0,1]。 */
            spectral_entropy /= logf((float)frequency_bin_count);
        }
    }

    /* lag_start 是 0.15 秒换算并四舍五入后的最小自相关延迟。 */
    int lag_start = (int)(0.15f * (float)SAMPLE_RATE_HZ + 0.5f);
    /* 延迟至少为一个样本，禁止零延迟恒等峰。 */
    if (lag_start < 1) lag_start = 1;
    /* 极短输入时把起点夹到最后有效延迟。 */
    if (lag_start > n - 1) lag_start = n - 1;
    /* lag_end 初始取半窗，避免过少重叠样本。 */
    int lag_end = n / 2;
    /* max_lag 是 1.20 秒对应的最大采样延迟。 */
    int max_lag = (int)(1.20f * (float)SAMPLE_RATE_HZ + 0.5f);
    /* 搜索终点不超过 1.20 秒。 */
    if (lag_end > max_lag) lag_end = max_lag;
    /* autocorr_peak 默认零，覆盖近常量序列。 */
    float autocorr_peak = 0.0f;
    /* autocorr_peak_lag 默认零，对应零秒输出。 */
    int autocorr_peak_lag = 0;
    /* 标准差有效且延迟区间非空时搜索最强归一化自相关。 */
    if (std > 1e-6f && lag_end >= lag_start) {
        /* best_correlation 从 -1 开始，保证首个合法相关可更新。 */
        float best_correlation = -1.0f;
        /* best_lag 默认最小延迟。 */
        int best_lag = lag_start;
        /* 逐延迟遍历 0.15..1.20 秒候选。 */
        for (int lag = lag_start; lag <= lag_end; lag++) {
            /* dot 累计两个去均值重叠片段点积。 */
            float dot = 0.0f;
            /* left_energy 累计左片段平方和。 */
            float left_energy = 0.0f;
            /* right_energy 累计右片段平方和。 */
            float right_energy = 0.0f;
            /* 遍历当前延迟下 n-lag 个重叠样本。 */
            for (int i = 0; i < n - lag; i++) {
                /* left 是当前左样本去均值值。 */
                float left = x[i] - mean;
                /* right 是向后 lag 样本的去均值值。 */
                float right = x[i + lag] - mean;
                /* 累加点积。 */
                dot += left * right;
                /* 累加左片段能量。 */
                left_energy += left * left;
                /* 累加右片段能量。 */
                right_energy += right * right;
            }
            /* denominator 是两个重叠片段 L2 范数乘积。 */
            float denominator = sqrtf(left_energy * right_energy);
            /* 分母近零时相关定义为零，避免除零。 */
            float correlation = denominator > 1e-12f ? dot / denominator : 0.0f;
            /* 严格更大时更新，平局保留较早延迟。 */
            if (correlation > best_correlation) {
                /* 保存当前最大相关。 */
                best_correlation = correlation;
                /* 保存对应采样延迟。 */
                best_lag = lag;
            }
        }
        /* 输出最强归一化相关，理论范围接近 [-1,1]。 */
        autocorr_peak = best_correlation;
        /* 输出最强相关的采样延迟。 */
        autocorr_peak_lag = best_lag;
    }

    /* 追加高活动点比例；近常量序列定义为零。 */
    feature[(*idx)++] = std > 1e-6f ? (float)high_count / (float)n : 0.0f;
    /* 追加显著峰数量除以序列长度得到的峰密度。 */
    feature[(*idx)++] = (float)peak_count / (float)n;
    /* 追加主频，单位 Hz。 */
    feature[(*idx)++] = dominant_frequency_hz;
    /* 追加归一化谱熵。 */
    feature[(*idx)++] = spectral_entropy;
    /* 追加最强归一化自相关。 */
    feature[(*idx)++] = autocorr_peak;
    /* 把采样延迟除以采样率换算为秒并追加。 */
    feature[(*idx)++] = (float)autocorr_peak_lag / (float)SAMPLE_RATE_HZ;
}

/*
 * 对单通道窗口执行去均值、Hann 加窗和单边直接 DFT。
 * x 长度为 n，单位可为 g、deg/s 或 g/采样点；三个频带比和峰比无量纲，质心单位为 Hz。
 * 频带使用 [0.35,1.20)、[1.20,2.40)、[2.40,5.00) Hz；总功率过小时五个输出均为 0。
 * 时间复杂度 O(n^2)，额外空间 O(1)；公式见 docs/算法原理、训练与实时计数.md。
 */
static inline void selected_spectral_features(
    const float* x, /* 指向 n 个连续单通道采样值，生命周期覆盖本函数调用。 */
    int n, /* 输入采样点数，正式推理时等于 WINDOW_LEN=62。 */
    float* low_ratio, /* 输出 0.35-1.20 Hz 功率占比，不能为空。 */
    float* mid_ratio, /* 输出 1.20-2.40 Hz 功率占比，不能为空。 */
    float* high_ratio, /* 输出 2.40-5.00 Hz 功率占比，不能为空。 */
    float* centroid_hz, /* 输出非直流谱质心，单位 Hz，不能为空。 */
    float* peak_power_ratio /* 输出最大单频功率占比，不能为空。 */
) {
    /* mean 累加并保存输入窗口均值，用于消除直流偏置。 */
    float mean = 0.0f;
    /* 遍历 n 个采样点并累计原始输入。 */
    for (int i = 0; i < n; i++) mean += x[i];
    /* 除以采样点数得到与输入同单位的窗口均值。 */
    mean /= (float)n;
    /* total_power 累加 k=1..floor(n/2) 的全部非直流功率。 */
    float total_power = 0.0f;
    /* low_power 累加 0.35<=f<1.20 Hz 的功率。 */
    float low_power = 0.0f;
    /* mid_power 累加 1.20<=f<2.40 Hz 的功率。 */
    float mid_power = 0.0f;
    /* high_power 累加 2.40<=f<5.00 Hz 的功率。 */
    float high_power = 0.0f;
    /* weighted_frequency 累加 f_k*P[k]，用于计算谱质心。 */
    float weighted_frequency = 0.0f;
    /* peak_power 保存最大非直流单频功率。 */
    float peak_power = 0.0f;
    /* two_pi 是 DFT 和 Hann 窗共同使用的 2*pi 单精度常量。 */
    const float two_pi = 6.2831853071795864769f;
    /* 遍历单边频点 k=1..floor(n/2)，显式跳过直流频点 k=0。 */
    for (int k = 1; k <= n / 2; k++) {
        /* real 累加当前频点 DFT 的实部。 */
        float real = 0.0f;
        /* imaginary 累加当前频点 DFT 的虚部。 */
        float imaginary = 0.0f;
        /* 遍历全部时域采样，计算当前频点的直接 DFT。 */
        for (int sample = 0; sample < n; sample++) {
            /* Hann 系数降低有限窗口边界频谱泄漏；n<=1 时回退为 1。 */
            float hann = n > 1
                ? 0.5f - 0.5f * cosf(two_pi * (float)sample / (float)(n - 1))
                : 1.0f;
            /* value 是去均值并加窗后的当前采样，单位继承输入。 */
            float value = (x[sample] - mean) * hann;
            /* angle 是当前频点和采样位置对应的 DFT 相位，单位 rad。 */
            float angle = two_pi * (float)k * (float)sample / (float)n;
            /* 按 cos 分量累计 DFT 实部。 */
            real += value * cosf(angle);
            /* 按 -sin 分量累计 DFT 虚部，与 numpy.fft.rfft 符号一致。 */
            imaginary -= value * sinf(angle);
        }
        /* power=real^2+imaginary^2，单位为输入单位平方。 */
        float power = real * real + imaginary * imaginary;
        /* frequency=k*fs/n，单位 Hz，范围为 (0,fs/2]。 */
        float frequency = (float)k * (float)SAMPLE_RATE_HZ / (float)n;
        /* 累加全部非直流功率，作为比例和质心的共同分母。 */
        total_power += power;
        /* 当前功率更大时更新主谱峰功率。 */
        if (power > peak_power) peak_power = power;
        /* 累加频率加权功率，最终除以总功率得到 Hz 质心。 */
        weighted_frequency += frequency * power;
        /* 半开低频带内的功率累加到 low_power。 */
        if (frequency >= 0.35f && frequency < 1.20f) low_power += power;
        /* 半开中频带内的功率累加到 mid_power。 */
        if (frequency >= 1.20f && frequency < 2.40f) mid_power += power;
        /* 半开高频带内的功率累加到 high_power。 */
        if (frequency >= 2.40f && frequency < 5.00f) high_power += power;
    }
    /* 总功率有效时计算五个有限输出。 */
    if (total_power > 1e-12f) {
        /* 低频功率除以全部非直流功率，理论范围 [0,1]。 */
        *low_ratio = low_power / total_power;
        /* 中频功率除以全部非直流功率，理论范围 [0,1]。 */
        *mid_ratio = mid_power / total_power;
        /* 高频功率除以全部非直流功率，理论范围 [0,1]。 */
        *high_ratio = high_power / total_power;
        /* 频率加权功率除以总功率，输出范围 [0,fs/2] Hz。 */
        *centroid_hz = weighted_frequency / total_power;
        /* 最大单频功率除以总功率，理论范围 [0,1]。 */
        *peak_power_ratio = peak_power / total_power;
    } else {
        /* 近静止或常量窗口没有可靠低频比例，定义为 0。 */
        *low_ratio = 0.0f;
        /* 近静止或常量窗口没有可靠中频比例，定义为 0。 */
        *mid_ratio = 0.0f;
        /* 近静止或常量窗口没有可靠高频比例，定义为 0。 */
        *high_ratio = 0.0f;
        /* 近静止或常量窗口没有可靠谱质心，定义为 0 Hz。 */
        *centroid_hz = 0.0f;
        /* 近静止或常量窗口没有可靠主谱峰比例，定义为 0。 */
        *peak_power_ratio = 0.0f;
    }
}

/*
 * 计算单通道 Hann 加窗频谱的二次谐波功率/主谱峰功率。
 * 输入 x 长度为 n，输出无量纲；主峰功率不大于 1e-12 时返回 0。
 * 时间复杂度 O(n^2)，额外空间为 WINDOW_LEN/2+1 个 float。
 */
static inline float spectral_second_harmonic_ratio(const float* x, int n) {
    /* mean 累加并保存输入窗口均值，用于去除直流偏置。 */
    float mean = 0.0f;
    /* 遍历 n 个采样点并累计原始输入。 */
    for (int i = 0; i < n; i++) mean += x[i];
    /* 除以 n 得到与输入同单位的全窗均值。 */
    mean /= (float)n;
    /* powers 按频点索引保存 k=1..floor(n/2) 的单边功率。 */
    float powers[WINDOW_LEN / 2 + 1];
    /* peak_power 保存当前最大非直流单频功率。 */
    float peak_power = 0.0f;
    /* peak_index 保存主谱峰索引；平局时因严格大于判断保留最早索引。 */
    int peak_index = 1;
    /* two_pi 是 Hann 窗和 DFT 使用的 2*pi 常量。 */
    const float two_pi = 6.2831853071795864769f;
    /* 遍历全部单边非直流频点。 */
    for (int k = 1; k <= n / 2; k++) {
        /* real 累加当前频点的 DFT 实部。 */
        float real = 0.0f;
        /* imaginary 累加当前频点的 DFT 虚部。 */
        float imaginary = 0.0f;
        /* 遍历全部时域采样并执行直接 DFT。 */
        for (int sample = 0; sample < n; sample++) {
            /* Hann 系数与 Python np.hanning 完全一致。 */
            float hann = n > 1
                ? 0.5f - 0.5f * cosf(two_pi * (float)sample / (float)(n - 1))
                : 1.0f;
            /* value 是去均值并加窗后的当前采样。 */
            float value = (x[sample] - mean) * hann;
            /* angle 是频点 k 和采样 sample 对应的 DFT 相位，单位 rad。 */
            float angle = two_pi * (float)k * (float)sample / (float)n;
            /* 按余弦分量累计实部。 */
            real += value * cosf(angle);
            /* 按负正弦分量累计虚部，与 numpy.fft.rfft 符号一致。 */
            imaginary -= value * sinf(angle);
        }
        /* 当前功率为实部平方与虚部平方之和。 */
        float power = real * real + imaginary * imaginary;
        /* 保存当前频点功率，后续按二次谐波索引读取。 */
        powers[k] = power;
        /* 严格更大时更新主峰，保证平局时选择较低频率。 */
        if (power > peak_power) {
            /* 保存新的最大功率。 */
            peak_power = power;
            /* 保存新的主谱峰频点索引。 */
            peak_index = k;
        }
    }
    /* 主峰功率过小表示常量或近静止窗口，返回 0 防止除零。 */
    if (peak_power <= 1e-12f) return 0.0f;
    /* 等间隔 DFT 上二次谐波索引等于 2*主峰索引。 */
    int harmonic_index = 2 * peak_index;
    /* 超出 Nyquist 时取最近的最高频点，与 Python argmin 行为一致。 */
    if (harmonic_index > n / 2) harmonic_index = n / 2;
    /* 返回二次谐波功率除以主谱峰功率，输出无量纲。 */
    return powers[harmonic_index] / peak_power;
}

/*
 * 从六轴窗口计算加速度模长低于 0.70g 的总比例和最长连续比例。
 * window 通道顺序为 gx,gy,gz,ax,ay,az；仅使用后三轴 g 值，两个输出范围均为 [0,1]。
 * 时间复杂度 O(WINDOW_LEN)，额外空间 O(1)。
 */
static inline void free_flight_features_from_window(
    const float window[WINDOW_LEN][AXIS_NUM],
    float* ratio,
    float* longest_ratio
) {
    /* free_count 统计全窗加速度模长低于 0.70g 的采样点数。 */
    int free_count = 0;
    /* current_run 记录当前连续低支持力区间长度。 */
    int current_run = 0;
    /* longest_run 记录全窗最长连续低支持力区间长度。 */
    int longest_run = 0;
    /* 按时间顺序遍历 WINDOW_LEN 个六轴采样点。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* ax 是传感器 x 轴加速度，单位 g。 */
        float ax = window[i][3];
        /* ay 是传感器 y 轴加速度，单位 g。 */
        float ay = window[i][4];
        /* az 是传感器 z 轴加速度，单位 g。 */
        float az = window[i][5];
        /* acc_mag 是三轴加速度欧氏模长，单位 g 且非负。 */
        float acc_mag = sqrtf(ax * ax + ay * ay + az * az);
        /* 严格低于 0.70g 时判为低支持力/腾空候选点，与 Python 一致。 */
        if (acc_mag < 0.70f) {
            /* 累加低支持力总点数。 */
            free_count++;
            /* 当前连续区间增加一个采样点。 */
            current_run++;
            /* 当前连续长度超过历史最大值时更新。 */
            if (current_run > longest_run) longest_run = current_run;
        } else {
            /* 不满足阈值时终止当前连续区间并重置为 0。 */
            current_run = 0;
        }
    }
    /* 总点数除以窗口长度得到无量纲低支持力比例。 */
    *ratio = (float)free_count / (float)WINDOW_LEN;
    /* 最长连续点数除以窗口长度得到无量纲连续区间比例。 */
    *longest_ratio = (float)longest_run / (float)WINDOW_LEN;
}

/*
 * 返回去均值自相关第一次不大于零的延迟秒数，用于描述动作由同相转为反相的时间尺度。
 * x 长度为 n，输入单位在点积符号判断中抵消；最多搜索 min(n/2,3 秒)，时间 O(n^2)、空间 O(1)。
 */
static inline float autocorr_first_zero_seconds(const float* x, int n) {
    /* mean 累加全窗均值，单位继承 x。 */
    float mean = 0.0f;
    /* 遍历 n 个样本累计输入值。 */
    for (int i = 0; i < n; i++) mean += x[i];
    /* 除以 n 得到总体均值。 */
    mean /= (float)n;
    /* energy 保存零延迟去均值能量 C(0)。 */
    float energy = 0.0f;
    /* 遍历全窗累计中心差平方。 */
    for (int i = 0; i < n; i++) {
        /* centered 是当前样本到均值的偏差。 */
        float centered = x[i] - mean;
        /* 累加去均值能量。 */
        energy += centered * centered;
    }
    /* 近常量输入没有可信零交叉，返回确定性零秒。 */
    if (energy <= 1e-12f) return 0.0f;
    /* max_lag 初始为半窗，保证每个相关至少有半数样本重叠。 */
    int max_lag = n / 2;
    /* three_seconds 是三秒对应的采样点数。 */
    int three_seconds = 3 * SAMPLE_RATE_HZ;
    /* 搜索不超过三秒。 */
    if (max_lag > three_seconds) max_lag = three_seconds;
    /* 从一个采样延迟开始，依次寻找首个非正自相关。 */
    for (int lag = 1; lag <= max_lag; lag++) {
        /* dot 累计当前延迟下去均值重叠片段点积。 */
        float dot = 0.0f;
        /* 当前延迟有 n-lag 对重叠样本。 */
        for (int i = 0; i < n - lag; i++) {
            /* 累加 C(lag)，这里只需符号而无需归一化。 */
            dot += (x[i] - mean) * (x[i + lag] - mean);
        }
        /* 首次到达非正相关时把采样延迟换算为秒返回。 */
        if (dot <= 0.0f) return (float)lag / (float)SAMPLE_RATE_HZ;
    }
    /* 搜索范围内始终为正时返回最大已检查延迟秒数。 */
    return (float)max_lag / (float)SAMPLE_RATE_HZ;
}

/*
 * 返回去均值归一化自相关中阈值不低于 0.20 的最强次峰。
 * 输出无量纲，近常量或无显著峰时为零；搜索至 min(n/2,3 秒)，时间 O(n^2)、空间 O(n)。
 */
static inline float autocorr_secondary_peak(const float* x, int n) {
    /* mean 累加序列均值。 */
    float mean = 0.0f;
    /* 遍历全部输入累计一阶矩。 */
    for (int i = 0; i < n; i++) mean += x[i];
    /* 除以 n 得到总体均值。 */
    mean /= (float)n;
    /* energy 保存 C(0) 去均值能量。 */
    float energy = 0.0f;
    /* 遍历全窗累计去均值平方和。 */
    for (int i = 0; i < n; i++) {
        /* centered 是当前中心化样本。 */
        float centered = x[i] - mean;
        /* 累加零延迟能量。 */
        energy += centered * centered;
    }
    /* 近常量序列没有可信相关峰，返回零。 */
    if (energy <= 1e-12f) return 0.0f;
    /* max_lag 初始取半窗。 */
    int max_lag = n / 2;
    /* three_seconds 限制最大物理延迟。 */
    int three_seconds = 3 * SAMPLE_RATE_HZ;
    /* 搜索范围不超过三秒。 */
    if (max_lag > three_seconds) max_lag = three_seconds;
    /* autocorr 保存延迟 1..max_lag 的归一化相关，容量覆盖半窗。 */
    float autocorr[WINDOW_LEN / 2];
    /* 逐延迟计算 C(lag)/C(0)。 */
    for (int lag = 1; lag <= max_lag; lag++) {
        /* dot 累计当前延迟重叠片段点积。 */
        float dot = 0.0f;
        /* 遍历 n-lag 对重叠中心化样本。 */
        for (int i = 0; i < n - lag; i++) {
            /* 累加未归一化自相关。 */
            dot += (x[i] - mean) * (x[i + lag] - mean);
        }
        /* 除以零延迟能量，保存无量纲相关。 */
        autocorr[lag - 1] = dot / energy;
    }
    /* secondary_peak 默认零，表示没有满足阈值的局部峰。 */
    float secondary_peak = 0.0f;
    /* 跳过首尾相关项，只检查同时具有左右邻点的候选。 */
    for (int i = 1; i < max_lag - 1; i++) {
        /* value 是当前候选归一化相关。 */
        float value = autocorr[i];
        /* 当前项需严格高于左侧、不低于右侧、达到 0.20 且超过历史最优。 */
        if (
            value > autocorr[i - 1] &&
            value >= autocorr[i + 1] &&
            value >= 0.20f &&
            value > secondary_peak
        ) {
            /* 保存当前最强显著次峰。 */
            secondary_peak = value;
        }
    }
    /* 返回无量纲最强次峰。 */
    return secondary_peak;
}

/*
 * 统计归一化自相关中不小于 0.20 的显著局部峰数量。
 * 输入 x 长度为 n，物理单位在 C[lag]/C[0] 中抵消；输出为无量纲非负整数的 float 表示。
 * 搜索最多 min(n/2,3 秒) 个延迟，时间复杂度 O(n^2)，额外空间 O(n)。
 */
static inline float autocorr_prominent_peak_count(const float* x, int n) {
    /* 累加全窗均值，用于去除静态偏置。 */
    float mean = 0.0f;
    /* 遍历 n 个采样点并累计输入值。 */
    for (int i = 0; i < n; i++) mean += x[i];
    /* 将总和除以 n 得到与输入同单位的均值。 */
    mean /= (float)n;
    /* energy 保存零延迟去均值能量 C[0]，单位为输入单位平方。 */
    float energy = 0.0f;
    /* 遍历窗口累计去均值平方和。 */
    for (int i = 0; i < n; i++) {
        /* centered 是当前采样相对全窗均值的动态分量。 */
        float centered = x[i] - mean;
        /* 累加动态能量，后续作为自相关归一化分母。 */
        energy += centered * centered;
    }
    /* 近常量序列没有可靠周期结构，返回 0 防止除零。 */
    if (energy <= 1e-12f) return 0.0f;
    /* 延迟上限先取半窗，保证每个点积仍有足够重叠样本。 */
    int max_lag = n / 2;
    /* 三秒延迟对应 3*SAMPLE_RATE_HZ 个采样点。 */
    int three_seconds = 3 * SAMPLE_RATE_HZ;
    /* 超过三秒时截断搜索，限制噪声和 ESP32 运算量。 */
    if (max_lag > three_seconds) max_lag = three_seconds;
    /* autocorr 保存 lag=1..max_lag 的归一化自相关，最大占用 WINDOW_LEN/2 个 float。 */
    float autocorr[WINDOW_LEN / 2];
    /* 逐一计算每个正延迟的去均值点积。 */
    for (int lag = 1; lag <= max_lag; lag++) {
        /* dot 保存当前延迟下两个重叠区间的点积。 */
        float dot = 0.0f;
        /* 重叠长度为 n-lag，循环终止于最后一个有效配对。 */
        for (int i = 0; i < n - lag; i++) {
            /* 累加相隔 lag 点的两个动态分量乘积。 */
            dot += (x[i] - mean) * (x[i + lag] - mean);
        }
        /* 除以零延迟能量得到无量纲自相关值，与 Python 公式一致。 */
        autocorr[lag - 1] = dot / energy;
    }
    /* peak_count 记录满足左右邻点和 0.20 门槛的显著局部峰数。 */
    int peak_count = 0;
    /* 从第二个延迟点遍历到倒数第二个，保证左右邻点存在。 */
    for (int i = 1; i < max_lag - 1; i++) {
        /* value 是当前延迟的归一化自相关值。 */
        float value = autocorr[i];
        /* 严格高于左点、不低于右点且达到 0.20 时计为显著周期峰。 */
        if (value > autocorr[i - 1] && value >= autocorr[i + 1] && value >= 0.20f) {
            /* 每发现一个显著峰，计数增加一。 */
            peak_count++;
        }
    }
    /* 返回峰数量的 float 表示，便于写入统一特征数组。 */
    return (float)peak_count;
}

/*
 * 计算显著正峰的幅值变异系数和峰间隔变异系数。
 * 输入 x 指向长度为 n 的单通道窗口；陀螺输入单位为 deg/s，加速度输入单位为 g。
 * 输出 amplitude_cv 和 interval_cv 均无量纲；无至少两个有效峰时写入 0，避免除零。
 * 数学公式、边界条件和 Python/C 一致性要求见 docs/算法原理、训练与实时计数.md。
 */
static inline void positive_peak_shape_features(
    const float* x,
    int n,
    float* amplitude_cv,
    float* interval_cv
) {
    /* 累加输入序列的一阶矩和二阶矩，用于总体均值与总体标准差。 */
    float sum = 0.0f;
    /* 二阶矩累加值的单位是输入物理量的平方。 */
    float sum2 = 0.0f;
    /* sorted 保存输入副本，最大长度 WINDOW_LEN，用于计算抗离群中位数。 */
    float sorted[WINDOW_LEN];
    /* 遍历 n 个采样点，同时累计矩并复制到排序缓冲区。 */
    for (int i = 0; i < n; i++) {
        /* 当前采样值继承 x 的物理单位。 */
        float value = x[i];
        /* 一阶矩用于计算均值。 */
        sum += value;
        /* 二阶矩用于计算总体方差。 */
        sum2 += value * value;
        /* 复制输入，后续插入排序不会修改原始窗口。 */
        sorted[i] = value;
    }
    /* 输入均值用于自适应峰值门槛。 */
    float mean = sum / (float)n;
    /* 由 E[x^2]-E[x]^2 计算总体方差。 */
    float variance = sum2 / (float)n - mean * mean;
    /* 浮点舍入可能产生极小负方差，截断到 0 后再开方。 */
    if (variance < 0.0f) variance = 0.0f;
    /* 总体标准差与输入单位相同。 */
    float std = sqrtf(variance);
    /* 显著正峰必须不低于均值加 0.5 倍标准差。 */
    float threshold = mean + 0.5f * std;

    /* 使用插入排序得到升序副本；n 最大为 62，O(n^2) 开销可控且无需动态内存。 */
    for (int i = 1; i < n; i++) {
        /* key 是本轮待插入的采样值。 */
        float key = sorted[i];
        /* j 从已排序区间末端向前移动。 */
        int j = i - 1;
        /* 将所有大于 key 的元素右移一位，直到找到插入位置。 */
        while (j >= 0 && sorted[j] > key) {
            /* 右移元素，保持 sorted[0..i] 有序。 */
            sorted[j + 1] = sorted[j];
            /* 继续检查前一个已排序元素。 */
            j--;
        }
        /* 将 key 放入最终位置。 */
        sorted[j + 1] = key;
    }
    /* 奇数长度取中间值，偶数长度取两个中间值平均，结果单位与输入相同。 */
    float median = n % 2 == 1
        ? sorted[n / 2]
        : 0.5f * (sorted[n / 2 - 1] + sorted[n / 2]);

    /* peak_indices 保存显著正峰的采样位置，元素范围为 [1,n-2]。 */
    int peak_indices[WINDOW_LEN];
    /* peak_count 记录已检测的显著正峰数量。 */
    int peak_count = 0;
    /* 遍历所有拥有左右邻点的采样位置，检测局部最大值。 */
    for (int i = 1; i < n - 1; i++) {
        /* 当前值严格高于左点、不低于右点且越过门槛时判为显著正峰。 */
        if (x[i] > x[i - 1] && x[i] >= x[i + 1] && x[i] >= threshold) {
            /* 保存峰位置，用于后续计算幅值和相邻峰间隔。 */
            peak_indices[peak_count++] = i;
        }
    }
    /* 少于两个峰无法估计重复周期离散程度，两个输出都设为 0。 */
    if (peak_count < 2) {
        /* 0 表示没有可解析的峰幅变异。 */
        *amplitude_cv = 0.0f;
        /* 0 表示没有可解析的峰间隔变异。 */
        *interval_cv = 0.0f;
        /* 提前返回，避免访问不存在的峰间隔。 */
        return;
    }

    /* amp_sum 累加峰值相对中位数的有符号幅值。 */
    float amp_sum = 0.0f;
    /* amp_abs_sum 累加绝对幅值，作为 CV 的稳定正分母。 */
    float amp_abs_sum = 0.0f;
    /* amp_sum2 累加峰幅平方，用于总体方差。 */
    float amp_sum2 = 0.0f;
    /* 遍历所有显著正峰，累计相对中位数的幅值统计量。 */
    for (int i = 0; i < peak_count; i++) {
        /* 峰幅等于峰值减去全窗中位数，单位与输入一致。 */
        float amplitude = x[peak_indices[i]] - median;
        /* 累加有符号峰幅。 */
        amp_sum += amplitude;
        /* 累加绝对峰幅，防止正负抵消导致分母过小。 */
        amp_abs_sum += fabsf(amplitude);
        /* 累加峰幅平方。 */
        amp_sum2 += amplitude * amplitude;
    }
    /* 峰幅总体均值用于方差计算。 */
    float amp_mean = amp_sum / (float)peak_count;
    /* 峰幅总体方差按 E[a^2]-E[a]^2 计算。 */
    float amp_variance = amp_sum2 / (float)peak_count - amp_mean * amp_mean;
    /* 舍入保护：负的微小方差截断为 0。 */
    if (amp_variance < 0.0f) amp_variance = 0.0f;
    /* 绝对峰幅均值是无量纲 CV 的分母。 */
    float amp_denominator = amp_abs_sum / (float)peak_count;
    /* 分母有效时计算总体标准差/绝对均值，否则输出 0。 */
    *amplitude_cv = amp_denominator > 1e-12f
        ? sqrtf(amp_variance) / amp_denominator
        : 0.0f;

    /* interval_count 等于相邻峰对数量。 */
    int interval_count = peak_count - 1;
    /* interval_sum 累加相邻峰间隔，单位为采样点。 */
    float interval_sum = 0.0f;
    /* interval_sum2 累加峰间隔平方。 */
    float interval_sum2 = 0.0f;
    /* 遍历相邻峰对，计算位置索引差。 */
    for (int i = 0; i < interval_count; i++) {
        /* 峰间隔始终为正，范围为 [1,n-2] 个采样点。 */
        float interval = (float)(peak_indices[i + 1] - peak_indices[i]);
        /* 累加峰间隔。 */
        interval_sum += interval;
        /* 累加峰间隔平方。 */
        interval_sum2 += interval * interval;
    }
    /* 平均峰间隔作为 CV 的正分母。 */
    float interval_mean = interval_sum / (float)interval_count;
    /* 峰间隔总体方差按 E[d^2]-E[d]^2 计算。 */
    float interval_variance =
        interval_sum2 / (float)interval_count - interval_mean * interval_mean;
    /* 舍入保护：负的微小方差截断为 0。 */
    if (interval_variance < 0.0f) interval_variance = 0.0f;
    /* 平均间隔有效时计算总体标准差/平均间隔，否则输出 0。 */
    *interval_cv = interval_mean > 1e-12f
        ? sqrtf(interval_variance) / interval_mean
        : 0.0f;
}

/*
 * 计算两条等长序列的零延迟 Pearson 相关系数。
 * left/right 长度均为 n，物理单位在归一化中抵消；输出近似 [-1,1]，常量输入返回零。
 * 时间复杂度 O(n)，额外空间 O(1)。
 */
static inline float series_correlation(const float* left, const float* right, int n) {
    /* left_mean 累加左序列均值。 */
    float left_mean = 0.0f;
    /* right_mean 累加右序列均值。 */
    float right_mean = 0.0f;
    /* 一次遍历同时累计两条序列。 */
    for (int i = 0; i < n; i++) {
        /* 累加左序列当前样本。 */
        left_mean += left[i];
        /* 累加右序列当前样本。 */
        right_mean += right[i];
    }
    /* 除以 n 得到左序列总体均值。 */
    left_mean /= (float)n;
    /* 除以 n 得到右序列总体均值。 */
    right_mean /= (float)n;
    /* dot 累计两条中心化序列点积。 */
    float dot = 0.0f;
    /* left_energy 累计左中心化能量。 */
    float left_energy = 0.0f;
    /* right_energy 累计右中心化能量。 */
    float right_energy = 0.0f;
    /* 第二遍处理全部配对样本。 */
    for (int i = 0; i < n; i++) {
        /* a 是左序列当前中心化样本。 */
        float a = left[i] - left_mean;
        /* b 是右序列当前中心化样本。 */
        float b = right[i] - right_mean;
        /* 累加中心化点积。 */
        dot += a * b;
        /* 累加左平方和。 */
        left_energy += a * a;
        /* 累加右平方和。 */
        right_energy += b * b;
    }
    /* denominator 是两个 L2 范数乘积。 */
    float denominator = sqrtf(left_energy * right_energy);
    /* 分母有效时返回归一化相关，否则常量序列定义为零。 */
    return denominator > 1e-12f ? dot / denominator : 0.0f;
}

/*
 * 返回 ±1 秒内绝对值最大的有符号归一化互相关。
 * left/right 长度均为 n；输入可分别为 g 与 deg/s，归一化后输出无量纲且接近 [-1,1]。
 * 最大延迟为 min(n/4,SAMPLE_RATE_HZ)，时间复杂度 O(n*SAMPLE_RATE_HZ)，额外空间 O(n)。
 */
static inline float max_cross_correlation(const float* left, const float* right, int n) {
    /* left_mean 累加并保存左序列全窗均值。 */
    float left_mean = 0.0f;
    /* right_mean 累加并保存右序列全窗均值。 */
    float right_mean = 0.0f;
    /* 遍历等长输入，同时累计两个序列的一阶矩。 */
    for (int i = 0; i < n; i++) {
        /* 累加左序列采样值。 */
        left_mean += left[i];
        /* 累加右序列采样值。 */
        right_mean += right[i];
    }
    /* 除以 n 得到左序列均值，单位继承 left。 */
    left_mean /= (float)n;
    /* 除以 n 得到右序列均值，单位继承 right。 */
    right_mean /= (float)n;
    /* 最大延迟先取四分之一窗口，62 点窗口对应 15 点。 */
    int max_lag = n / 4;
    /* 超过一秒时截断为采样率对应的点数。 */
    if (max_lag > SAMPLE_RATE_HZ) max_lag = SAMPLE_RATE_HZ;
    /* best_correlation 保存绝对值最强的相关系数并保留正负号。 */
    float best_correlation = 0.0f;
    /* 从负最大延迟遍历到正最大延迟，顺序与 Python 完全一致。 */
    for (int lag = -max_lag; lag <= max_lag; lag++) {
        /* overlap 是当前延迟下两个序列的重叠样本数。 */
        int overlap = n - (lag < 0 ? -lag : lag);
        /* left_start 对应 Python 在负延迟时截去左序列前 -lag 点。 */
        int left_start = lag < 0 ? -lag : 0;
        /* right_start 对应 Python 在正延迟时截去右序列前 lag 点。 */
        int right_start = lag > 0 ? lag : 0;
        /* dot 保存当前重叠区间的去均值点积。 */
        float dot = 0.0f;
        /* left_energy 保存左重叠区间的平方和。 */
        float left_energy = 0.0f;
        /* right_energy 保存右重叠区间的平方和。 */
        float right_energy = 0.0f;
        /* 遍历 overlap 个成对采样点并累计归一化所需统计量。 */
        for (int i = 0; i < overlap; i++) {
            /* a 是左序列当前重叠采样减去左全窗均值。 */
            float a = left[left_start + i] - left_mean;
            /* b 是右序列当前重叠采样减去右全窗均值。 */
            float b = right[right_start + i] - right_mean;
            /* 累加跨序列点积。 */
            dot += a * b;
            /* 累加左序列重叠能量。 */
            left_energy += a * a;
            /* 累加右序列重叠能量。 */
            right_energy += b * b;
        }
        /* 两个 L2 范数乘积是相关系数分母。 */
        float denominator = sqrtf(left_energy * right_energy);
        /* 分母有效时计算有符号相关，近常量重叠区间定义为 0。 */
        float correlation = denominator > 1e-12f ? dot / denominator : 0.0f;
        /* 仅当绝对值严格更大时更新，平局保留更早遍历到的延迟。 */
        if (fabsf(correlation) > fabsf(best_correlation)) {
            /* 保存当前绝对值最强的有符号相关系数。 */
            best_correlation = correlation;
        }
    }
    /* 返回无量纲最大互相关，供标准化和单 BP 输入。 */
    return best_correlation;
}

/*
 * 计算窗口运动分数 S=std(|a|)+std(|g|)/200，用于静止与动态窗口筛选。
 * window 形状 [WINDOW_LEN,6]，前三轴 deg/s、后三轴 g；输出无量纲，时间 O(N)、空间 O(1)。
 */
static inline float bp_window_motion_score(const float window[WINDOW_LEN][AXIS_NUM]) {
    /* gyro_sum 累计角速度模长，单位 deg/s。 */
    float gyro_sum = 0.0f;
    /* gyro_sum2 累计角速度模长平方。 */
    float gyro_sum2 = 0.0f;
    /* acc_sum 累计加速度模长，单位 g。 */
    float acc_sum = 0.0f;
    /* acc_sum2 累计加速度模长平方。 */
    float acc_sum2 = 0.0f;
    /* 遍历完整 62 点窗口，通道顺序固定 gx、gy、gz、ax、ay、az。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* gyro_mag 是当前三轴角速度欧氏模长，单位 deg/s。 */
        float gyro_mag = sqrtf(
            window[i][0] * window[i][0] +
            window[i][1] * window[i][1] +
            window[i][2] * window[i][2]
        );
        /* acc_mag 是当前三轴加速度欧氏模长，单位 g。 */
        float acc_mag = sqrtf(
            window[i][3] * window[i][3] +
            window[i][4] * window[i][4] +
            window[i][5] * window[i][5]
        );
        /* 累加角速度模长。 */
        gyro_sum += gyro_mag;
        /* 累加角速度模长平方。 */
        gyro_sum2 += gyro_mag * gyro_mag;
        /* 累加加速度模长。 */
        acc_sum += acc_mag;
        /* 累加加速度模长平方。 */
        acc_sum2 += acc_mag * acc_mag;
    }
    /* gyro_mean 是角速度模长总体均值。 */
    float gyro_mean = gyro_sum / (float)WINDOW_LEN;
    /* acc_mean 是加速度模长总体均值。 */
    float acc_mean = acc_sum / (float)WINDOW_LEN;
    /* gyro_var 使用总体二阶矩计算角速度模长方差。 */
    float gyro_var = gyro_sum2 / (float)WINDOW_LEN - gyro_mean * gyro_mean;
    /* acc_var 使用总体二阶矩计算加速度模长方差。 */
    float acc_var = acc_sum2 / (float)WINDOW_LEN - acc_mean * acc_mean;
    /* 夹紧角速度方差的浮点负零误差。 */
    if (gyro_var < 0.0f) gyro_var = 0.0f;
    /* 夹紧加速度方差的浮点负零误差。 */
    if (acc_var < 0.0f) acc_var = 0.0f;
    /* 角速度标准差除以 200 后与加速度标准差相加，返回训练端同公式分数。 */
    return sqrtf(acc_var) + sqrtf(gyro_var) / 200.0f;
}

/*
 * 计算逐点活动分数超过 ACTIVE_POINT_THRESHOLD 的采样比例。
 * 单点分数为相邻加速度差模长加角速度模长/200；输出范围 [0,1]，时间 O(N)、空间 O(1)。
 */
static inline float bp_window_active_ratio(const float window[WINDOW_LEN][AXIS_NUM]) {
    /* active_count 统计判为活动的采样点数量。 */
    int active_count = 0;
    /* 遍历窗口全部采样点；首点没有加速度前驱，差分取零。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* gyro_mag 是当前三轴角速度模长，单位 deg/s。 */
        float gyro_mag = sqrtf(
            window[i][0] * window[i][0] +
            window[i][1] * window[i][1] +
            window[i][2] * window[i][2]
        );
        /* acc_delta 默认零，对应首点没有上一采样。 */
        float acc_delta = 0.0f;
        /* 第二点起计算相邻三轴加速度差。 */
        if (i > 0) {
            /* dx 是 ax 当前值减前值，单位 g。 */
            float dx = window[i][3] - window[i - 1][3];
            /* dy 是 ay 当前值减前值，单位 g。 */
            float dy = window[i][4] - window[i - 1][4];
            /* dz 是 az 当前值减前值，单位 g。 */
            float dz = window[i][5] - window[i - 1][5];
            /* acc_delta 是三轴加速度差欧氏模长。 */
            acc_delta = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        /* 组合分数超过训练端活动阈值时累计一个活动点。 */
        if (acc_delta + gyro_mag / 200.0f > ACTIVE_POINT_THRESHOLD) active_count++;
    }
    /* 除以固定窗口长度得到 [0,1] 活动比例。 */
    return (float)active_count / (float)WINDOW_LEN;
}

/*
 * 判断窗口是否满足高动态跳跃类候选条件：运动分数至少静止阈值 1.25 倍且活动比例不少于 20%。
 * 返回一表示保留、零表示过滤；时间 O(N)，不修改输入。
 */
static inline int bp_window_is_dynamic_candidate(const float window[WINDOW_LEN][AXIS_NUM]) {
    /* 两个条件必须同时成立，短路求值可在低运动分数时省去活动比例遍历。 */
    return (
        /* 第一条件拒绝接近静止的窗口。 */
        bp_window_motion_score(window) >= REST_MOTION_THRESHOLD * 1.25f &&
        /* 第二条件拒绝只有单点毛刺而没有持续活动的窗口。 */
        bp_window_active_ratio(window) >= HIGH_DYNAMIC_MIN_RATIO
    );
}

/* 对事件数组执行插入排序并返回中位数；count=0 时返回 0。 */
static inline float event_median_c(const float* values, int count) {
    /* 空事件没有可聚合物理量，返回确定性零。 */
    if (count <= 0) return 0.0f;
    /* 62 点窗口最多包含 31 个长度不少于两点的事件。 */
    float sorted[WINDOW_LEN / 2];
    /* 复制有效事件值，避免修改调用者数组。 */
    for (int i = 0; i < count; i++) sorted[i] = values[i];
    /* 使用稳定插入排序；固定小数组最坏时间复杂度 O(count^2)。 */
    for (int i = 1; i < count; i++) {
        /* key 保存当前待插入事件值。 */
        float key = sorted[i];
        /* j 从有序前缀末尾向前搜索。 */
        int j = i - 1;
        /* 将所有大于 key 的值右移一位。 */
        while (j >= 0 && sorted[j] > key) {
            /* 右移当前较大元素。 */
            sorted[j + 1] = sorted[j];
            /* 继续比较前一个元素。 */
            j--;
        }
        /* 将 key 写入最终位置。 */
        sorted[j + 1] = key;
    }
    /* 奇数长度直接返回中央值。 */
    if ((count & 1) != 0) return sorted[count / 2];
    /* 偶数长度返回两个中央值均值，与 numpy.median 一致。 */
    return 0.5f * (sorted[count / 2 - 1] + sorted[count / 2]);
}

/*
 * 计算水平加速度或角速度协方差各向异性。
 * gyro_source=0 时输入单位为 g；gyro_source=1 时输入单位为 deg/s；输出无量纲 [0,1]。
 */
static inline float horizontal_anisotropy_from_window(
    const float window[WINDOW_LEN][AXIS_NUM],
    int gyro_source
) {
    /* gravity 保存窗口三轴加速度均值，单位 g。 */
    float gravity[3] = {0.0f, 0.0f, 0.0f};
    /* 累计 ax、ay、az 以估计重力方向。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* 三个加速度分量位于列 3..5。 */
        for (int axis = 0; axis < 3; axis++) gravity[axis] += window[i][axis + 3];
    }
    /* 除以窗口长度得到重力均值。 */
    for (int axis = 0; axis < 3; axis++) gravity[axis] /= (float)WINDOW_LEN;
    /* 计算重力模长，过小时退化为 z 轴。 */
    float gravity_norm = sqrtf(
        gravity[0] * gravity[0] + gravity[1] * gravity[1] + gravity[2] * gravity[2]
    );
    /* gravity_unit 是单位重力方向。 */
    float gravity_unit[3];
    /* 正常输入按模长归一化。 */
    if (gravity_norm > 1e-6f) {
        /* 三个分量分别除以重力模长。 */
        for (int axis = 0; axis < 3; axis++) gravity_unit[axis] = gravity[axis] / gravity_norm;
    } else {
        /* 异常近零重力时使用传感器 z 轴，防止除零。 */
        gravity_unit[0] = 0.0f; gravity_unit[1] = 0.0f; gravity_unit[2] = 1.0f;
    }
    /* horizontal 保存每点投影到重力正交平面的方向向量。 */
    float horizontal[WINDOW_LEN][3];
    /* mean 保存三个水平分量的窗口均值。 */
    float mean[3] = {0.0f, 0.0f, 0.0f};
    /* 构建水平向量并累计分量均值。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* source 保存当前三轴加速度动态量或三轴角速度。 */
        float source[3];
        /* 根据 gyro_source 选择物理来源；加速度分支先减去重力均值。 */
        for (int axis = 0; axis < 3; axis++) {
            /* 陀螺取列 0..2；加速度取列 3..5 并移除静态重力。 */
            source[axis] = gyro_source ? window[i][axis] : window[i][axis + 3] - gravity[axis];
        }
        /* vertical 是 source 在重力方向的标量投影。 */
        float vertical =
            source[0] * gravity_unit[0] +
            source[1] * gravity_unit[1] +
            source[2] * gravity_unit[2];
        /* 逐轴移除垂直分量并累计水平向量均值。 */
        for (int axis = 0; axis < 3; axis++) {
            /* 水平向量仍使用原来源单位。 */
            horizontal[i][axis] = source[axis] - vertical * gravity_unit[axis];
            /* 累计该水平分量。 */
            mean[axis] += horizontal[i][axis];
        }
    }
    /* 除以窗口长度得到水平向量均值。 */
    for (int axis = 0; axis < 3; axis++) mean[axis] /= (float)WINDOW_LEN;
    /* 六个变量保存对称总体协方差的独立元素。 */
    float c00 = 0.0f, c11 = 0.0f, c22 = 0.0f;
    /* 三个变量保存协方差上三角交叉项。 */
    float c01 = 0.0f, c02 = 0.0f, c12 = 0.0f;
    /* 累计去均值水平向量外积。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* x、y、z 是当前向量相对窗口均值的分量。 */
        float x = horizontal[i][0] - mean[0];
        float y = horizontal[i][1] - mean[1];
        float z = horizontal[i][2] - mean[2];
        /* 累计三个平方项。 */
        c00 += x * x; c11 += y * y; c22 += z * z;
        /* 累计三个交叉项。 */
        c01 += x * y; c02 += x * z; c12 += y * z;
    }
    /* 总体协方差使用分母 N，与 Python 实现一致。 */
    float inverse_n = 1.0f / (float)WINDOW_LEN;
    /* 六个独立元素统一除以 N。 */
    c00 *= inverse_n; c11 *= inverse_n; c22 *= inverse_n;
    c01 *= inverse_n; c02 *= inverse_n; c12 *= inverse_n;
    /* trace 等于水平面两个非零特征值之和。 */
    float trace = c00 + c11 + c22;
    /* 总动态能量过小时返回 0，避免噪声方向放大。 */
    if (trace <= 1e-12f) return 0.0f;
    /* trace_square=trace(C^2)，对称非对角项计两次。 */
    float trace_square =
        c00 * c00 + c11 * c11 + c22 * c22 +
        2.0f * (c01 * c01 + c02 * c02 + c12 * c12);
    /* 两非零特征值差平方为 2*trace(C^2)-trace(C)^2。 */
    float difference_squared = 2.0f * trace_square - trace * trace;
    /* 单精度舍入产生负小数时截断为 0。 */
    if (difference_squared < 0.0f) difference_squared = 0.0f;
    /* 特征值差除以特征值和，得到无量纲各向异性。 */
    float anisotropy = sqrtf(difference_squared) / trace;
    /* 理论上限为 1，显式截断单精度超限。 */
    if (anisotropy > 1.0f) anisotropy = 1.0f;
    /* 返回 0 表示各向同性，1 表示单一主方向。 */
    return anisotropy;
}

/* 提取并追加四项完整腾空事件中位数；公式及边界见 docs/算法原理、训练与实时计数.md。 */
static inline void append_aligned_event_medians(
    const float window[WINDOW_LEN][AXIS_NUM],
    float* feature,
    int* index
) {
    /* gravity 保存加速度窗口均值，用于重力方向估计。 */
    float gravity[3] = {0.0f, 0.0f, 0.0f};
    /* 累计三轴加速度。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* 加速度通道位于列 3..5。 */
        for (int axis = 0; axis < 3; axis++) gravity[axis] += window[i][axis + 3];
    }
    /* 除以 N 得到重力均值。 */
    for (int axis = 0; axis < 3; axis++) gravity[axis] /= (float)WINDOW_LEN;
    /* 计算重力模长并构造单位方向。 */
    float norm = sqrtf(gravity[0] * gravity[0] + gravity[1] * gravity[1] + gravity[2] * gravity[2]);
    /* unit 保存单位重力向量。 */
    float unit[3];
    /* 正常模长时归一化，异常时退化为 z 轴。 */
    if (norm > 1e-6f) {
        /* 三轴分别除以模长。 */
        for (int axis = 0; axis < 3; axis++) unit[axis] = gravity[axis] / norm;
    } else {
        /* 确定性退化方向防止除零。 */
        unit[0] = 0.0f; unit[1] = 0.0f; unit[2] = 1.0f;
    }
    /* vertical 为含重力垂直加速度；gyro_vertical 为绕重力轴角速度。 */
    float vertical[WINDOW_LEN], gyro_vertical[WINDOW_LEN];
    /* horizontal_gyro_mag 为水平角速度模长。 */
    float horizontal_gyro_mag[WINDOW_LEN];
    /* acc_mag 为三轴加速度模长，用于 0.70g 腾空判定。 */
    float acc_mag[WINDOW_LEN];
    /* 同时构造四个事件检测序列。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* 垂直加速度投影单位为 g。 */
        vertical[i] = window[i][3] * unit[0] + window[i][4] * unit[1] + window[i][5] * unit[2];
        /* 垂直角速度投影单位为 deg/s。 */
        gyro_vertical[i] = window[i][0] * unit[0] + window[i][1] * unit[1] + window[i][2] * unit[2];
        /* 陀螺总能量减去垂直分量平方得到水平模长平方。 */
        float gyro_horizontal_squared =
            window[i][0] * window[i][0] + window[i][1] * window[i][1] + window[i][2] * window[i][2] -
            gyro_vertical[i] * gyro_vertical[i];
        /* 舍入可能产生极小负数，开方前截断。 */
        if (gyro_horizontal_squared < 0.0f) gyro_horizontal_squared = 0.0f;
        /* 水平角速度模长单位为 deg/s。 */
        horizontal_gyro_mag[i] = sqrtf(gyro_horizontal_squared);
        /* 加速度模长单位为 g。 */
        acc_mag[i] = sqrtf(
            window[i][3] * window[i][3] + window[i][4] * window[i][4] + window[i][5] * window[i][5]
        );
    }
    /* 计算垂直加速度总体均值和二阶矩。 */
    float mean = 0.0f, second = 0.0f;
    /* 累计全部垂直样本。 */
    for (int i = 0; i < WINDOW_LEN; i++) { mean += vertical[i]; second += vertical[i] * vertical[i]; }
    /* 总体均值使用分母 N。 */
    mean /= (float)WINDOW_LEN;
    /* 总体方差由 E[x^2]-E[x]^2 得到。 */
    float variance = second / (float)WINDOW_LEN - mean * mean;
    /* 截断负舍入误差。 */
    if (variance < 0.0f) variance = 0.0f;
    /* 落地冲击门槛取 1.20g 与均值加半标准差的较大值。 */
    float impact_threshold = mean + 0.5f * sqrtf(variance);
    /* 施加最低物理门槛。 */
    if (impact_threshold < 1.20f) impact_threshold = 1.20f;
    /* 四个数组保存完整事件的秒或度值。 */
    float intervals[WINDOW_LEN / 2], widths[WINDOW_LEN / 2];
    /* 两个角度数组分别保存水平和垂直积分。 */
    float horizontal_integrals[WINDOW_LEN / 2], vertical_integrals[WINDOW_LEN / 2];
    /* count 是四个数组的共同有效事件数。 */
    int count = 0;
    /* scan 是连续腾空段扫描索引。 */
    int scan = 0;
    /* 顺序扫描所有采样点。 */
    while (scan < WINDOW_LEN) {
        /* 非腾空点直接前进。 */
        if (acc_mag[scan] >= 0.70f) { scan++; continue; }
        /* start 记录当前连续低支持力段起点。 */
        int start = scan;
        /* 前进到首个非腾空点或窗口末尾。 */
        while (scan < WINDOW_LEN && acc_mag[scan] < 0.70f) scan++;
        /* end 是半开区间终点。 */
        int end = scan;
        /* 丢弃单点噪声和被窗口边界截断的事件。 */
        if (end - start < 2 || start <= 0 || end >= WINDOW_LEN) continue;
        /* 起跳搜索覆盖腾空前最多 5 点。 */
        int pre_start = start - 5;
        /* 左边界截断到 0。 */
        if (pre_start < 0) pre_start = 0;
        /* takeoff 初始为局部首点。 */
        int takeoff = pre_start;
        /* 严格更大才更新，平局保持最早索引。 */
        for (int j = pre_start + 1; j < start; j++) if (vertical[j] > vertical[takeoff]) takeoff = j;
        /* 落地搜索覆盖腾空结束后最多 6 点。 */
        int post_end = end + 6;
        /* 右边界截断到窗口长度。 */
        if (post_end > WINDOW_LEN) post_end = WINDOW_LEN;
        /* landing 初始为腾空结束后的首点。 */
        int landing = end;
        /* 严格更大才更新，匹配 numpy.argmax 最早平局规则。 */
        for (int j = end + 1; j < post_end; j++) if (vertical[j] > vertical[landing]) landing = j;
        /* 保存起跳峰到落地峰的秒数。 */
        intervals[count] = (float)(landing - takeoff) / (float)SAMPLE_RATE_HZ;
        /* points 统计落地峰后连续高冲击点数。 */
        int points = 0;
        /* 首个低于门槛的点结束连续宽度。 */
        for (int j = landing; j < post_end; j++) { if (vertical[j] >= impact_threshold) points++; else break; }
        /* 点数除以采样率得到冲击宽度秒数。 */
        widths[count] = (float)points / (float)SAMPLE_RATE_HZ;
        /* 两项角速度积分从零开始累计。 */
        float horizontal_sum = 0.0f, vertical_sum = 0.0f;
        /* 遍历半开腾空区间的全部采样点。 */
        for (int j = start; j < end; j++) {
            /* 累加水平角速度模长。 */
            horizontal_sum += horizontal_gyro_mag[j];
            /* 累加垂直角速度绝对值，避免方向抵消。 */
            vertical_sum += fabsf(gyro_vertical[j]);
        }
        /* 除以采样率得到水平总转角，单位度。 */
        horizontal_integrals[count] = horizontal_sum / (float)SAMPLE_RATE_HZ;
        /* 除以采样率得到垂直总转角，单位度。 */
        vertical_integrals[count] = vertical_sum / (float)SAMPLE_RATE_HZ;
        /* 完整事件数增加一。 */
        count++;
    }
    /* 按 Python 固定顺序追加四项事件中位数。 */
    feature[(*index)++] = event_median_c(intervals, count);
    /* 追加落地冲击宽度中位数。 */
    feature[(*index)++] = event_median_c(widths, count);
    /* 追加腾空水平角速度积分中位数。 */
    feature[(*index)++] = event_median_c(horizontal_integrals, count);
    /* 追加腾空垂直角速度绝对积分中位数。 */
    feature[(*index)++] = event_median_c(vertical_integrals, count);
}

/*
 * 计算手腕角速度 PCA 主轴上的有效换向率，单位为 Hz。
 * 输入 window 为 [WINDOW_LEN,6]，前三列单位为 deg/s；只使用手腕陀螺数据。
 * 主轴用固定 8 次幂迭代求解，90 分位门槛与 Python 一致；时间复杂度 O(N^2)，额外 RAM 约 5N 个 float。
 */
static inline float wrist_reversal_rate_hz(const float window[WINDOW_LEN][AXIS_NUM]) {
    /* mean 保存 gx、gy、gz 的窗口均值，用于去除陀螺零偏。 */
    float mean[3] = { 0.0f, 0.0f, 0.0f };
    /* 遍历全部手腕采样点并累计三轴角速度。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* 逐轴累加，通道 0..2 固定对应 gx、gy、gz。 */
        for (int axis = 0; axis < 3; axis++) mean[axis] += window[i][axis];
    }
    /* 除以窗口长度得到三轴均值，单位仍为 deg/s。 */
    for (int axis = 0; axis < 3; axis++) mean[axis] /= (float)WINDOW_LEN;
    /* moment 是去均值角速度的 3×3 二阶矩，单位为 (deg/s)^2。 */
    float moment[3][3] = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
    /* 遍历窗口累计外积，未除以 N 不影响主特征向量。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* centered 保存当前点三轴动态角速度。 */
        float centered[3];
        /* 从原始值减去各轴均值。 */
        for (int axis = 0; axis < 3; axis++) centered[axis] = window[i][axis] - mean[axis];
        /* 累计 3×3 对称二阶矩。 */
        for (int row = 0; row < 3; row++) {
            /* 每个 row 与三个 column 形成外积元素。 */
            for (int column = 0; column < 3; column++) moment[row][column] += centered[row] * centered[column];
        }
    }
    /* initial_axis 选择对角能量最大的传感器轴，避免初值与主轴正交。 */
    int initial_axis = 0;
    /* 比较 y、z 轴对角能量并更新索引；严格大于保证平局取更早轴。 */
    for (int axis_index = 1; axis_index < 3; axis_index++) if (moment[axis_index][axis_index] > moment[initial_axis][initial_axis]) initial_axis = axis_index;
    /* axis 是幂迭代单位向量，初始为最大能量坐标轴。 */
    float axis[3] = { 0.0f, 0.0f, 0.0f };
    /* 写入唯一非零初始分量。 */
    axis[initial_axis] = 1.0f;
    /* 固定执行 8 次，与 Python 保持一致且避免收敛条件分支差异。 */
    for (int iteration = 0; iteration < 8; iteration++) {
        /* next_axis 接收 moment×axis。 */
        float next_axis[3] = { 0.0f, 0.0f, 0.0f };
        /* 三行矩阵分别与当前轴做点积。 */
        for (int row = 0; row < 3; row++) {
            /* 遍历三列完成当前输出分量。 */
            for (int column = 0; column < 3; column++) next_axis[row] += moment[row][column] * axis[column];
        }
        /* norm 是下一轴的二范数。 */
        float norm = sqrtf(next_axis[0] * next_axis[0] + next_axis[1] * next_axis[1] + next_axis[2] * next_axis[2]);
        /* 近静止窗口没有可靠主轴，返回 0 次/秒。 */
        if (norm <= 1e-12f) return 0.0f;
        /* 归一化三轴分量进入下一次迭代。 */
        for (int component = 0; component < 3; component++) axis[component] = next_axis[component] / norm;
    }
    /* anchor 选择绝对值最大的主轴分量，用于固定 PCA 任意符号。 */
    int anchor = 0;
    /* 比较剩余两个分量绝对值，严格大于保证平局确定。 */
    for (int component = 1; component < 3; component++) if (fabsf(axis[component]) > fabsf(axis[anchor])) anchor = component;
    /* 锚点为负时翻转整条轴，使 Python/C 主轴符号一致。 */
    if (axis[anchor] < 0.0f) for (int component = 0; component < 3; component++) axis[component] = -axis[component];
    /* projection 保存每个采样点沿手腕主转动方向的角速度，单位 deg/s。 */
    float projection[WINDOW_LEN];
    /* 逐点计算去均值三轴与主轴的点积。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* 点积三项显式展开，减少 ESP32 循环开销。 */
        projection[i] =
            (window[i][0] - mean[0]) * axis[0] +
            (window[i][1] - mean[1]) * axis[1] +
            (window[i][2] - mean[2]) * axis[2];
    }
    /* smoothed 保存三点对称平均，边缘使用自身复制。 */
    float smoothed[WINDOW_LEN];
    /* 每个位置读取左、中、右三个投影。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* 左边缘索引在 i=0 时固定为 0。 */
        int left = i > 0 ? i - 1 : 0;
        /* 右边缘索引在末点时固定为 WINDOW_LEN-1。 */
        int right = i + 1 < WINDOW_LEN ? i + 1 : WINDOW_LEN - 1;
        /* 三点和除以 3 得到平滑角速度。 */
        smoothed[i] = (projection[left] + projection[i] + projection[right]) / 3.0f;
    }
    /* ordered_abs 用于计算与 NumPy 线性插值一致的 90 分位幅值。 */
    float ordered_abs[WINDOW_LEN];
    /* 复制全部平滑投影绝对值。 */
    for (int i = 0; i < WINDOW_LEN; i++) ordered_abs[i] = fabsf(smoothed[i]);
    /* 插入排序适合 N=62 的固定小数组，额外 RAM 为 O(N)。 */
    for (int i = 1; i < WINDOW_LEN; i++) {
        /* value 保存当前待插入绝对值。 */
        float value = ordered_abs[i];
        /* j 从已排序区末尾向前移动。 */
        int j = i - 1;
        /* 将所有大于 value 的元素右移一位。 */
        while (j >= 0 && ordered_abs[j] > value) { ordered_abs[j + 1] = ordered_abs[j]; j--; }
        /* 把 value 写入空出的有序位置。 */
        ordered_abs[j + 1] = value;
    }
    /* percentile_position 对应 NumPy percentile 的 0.9*(N-1) 线性位置。 */
    float percentile_position = 0.90f * (float)(WINDOW_LEN - 1);
    /* lower_index 是线性插值左端索引。 */
    int lower_index = (int)floorf(percentile_position);
    /* upper_index 是右端索引，限制不超过数组末尾。 */
    int upper_index = lower_index + 1 < WINDOW_LEN ? lower_index + 1 : lower_index;
    /* fraction 是左右两个顺序统计量之间的插值比例。 */
    float fraction = percentile_position - (float)lower_index;
    /* percentile90 得到 90 分位绝对角速度，单位 deg/s。 */
    float percentile90 = ordered_abs[lower_index] * (1.0f - fraction) + ordered_abs[upper_index] * fraction;
    /* 有效换向门槛取 10 deg/s 与 15% q90 中较大者。 */
    float threshold = 0.15f * percentile90;
    /* 物理下限过滤静止手腕零点抖动。 */
    if (threshold < 10.0f) threshold = 10.0f;
    /* reversal_count 记录相邻有效点符号翻转次数。 */
    int reversal_count = 0;
    /* has_previous 表示已遇到第一个达到门槛的点。 */
    int has_previous = 0;
    /* previous 保存上一个有效投影。 */
    float previous = 0.0f;
    /* 顺序遍历平滑投影，低于门槛的点不参与符号比较。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* 当前绝对值不足门槛时继续下一点。 */
        if (fabsf(smoothed[i]) < threshold) continue;
        /* 已有前一有效点且乘积为负时累计一次换向。 */
        if (has_previous && previous * smoothed[i] < 0.0f) reversal_count++;
        /* 更新上一有效点值。 */
        previous = smoothed[i];
        /* 标记有效历史存在。 */
        has_previous = 1;
    }
    /* 次数除以 N/采样率得到每秒换向次数。 */
    return (float)reversal_count / ((float)WINDOW_LEN / (float)SAMPLE_RATE_HZ);
}

/*
 * 计算手腕角速度模长自相关第二时间峰与第一时间峰之比。
 * 搜索延迟为 0.30～3.00 秒且不超过半窗；输出无量纲并限制在 [0,5]。
 * 时间复杂度 O(N^2)，额外 RAM 不超过 WINDOW_LEN 个 float。
 */
/*
 * 返回角速度模长在 0.3～3.0 秒延迟范围内的第一正自相关峰。
 * 输入 gyro_magnitude 长度为 n，单位 deg/s；输出无量纲，典型范围 [0,1]。
 * 时间复杂度 O(N^2)，额外 RAM 不超过 WINDOW_LEN 个 float。
 */
static inline float wrist_acf_first_peak(const float* gyro_magnitude, int n) {
    /* 少于 10 点时无法覆盖 0.3 秒最小延迟。 */
    if (n < 10) return 0.0f;
    /* mean 是角速度模长窗口均值，单位 deg/s。 */
    float mean = 0.0f;
    /* 累加全部 n 个点用于计算均值。 */
    for (int i = 0; i < n; i++) mean += gyro_magnitude[i];
    /* 除以点数得到窗口均值。 */
    mean /= (float)n;
    /* energy 是中心化序列的零延迟能量，单位 (deg/s)^2。 */
    float energy = 0.0f;
    /* 逐点累计中心化平方和。 */
    for (int i = 0; i < n; i++) {
        /* centered 去除角速度模长直流偏置。 */
        float centered = gyro_magnitude[i] - mean;
        /* 累加当前点能量。 */
        energy += centered * centered;
    }
    /* 近静止窗口不能稳定归一化，返回零周期证据。 */
    if (energy <= 1e-12f) return 0.0f;
    /* minimum_lag 在 25 Hz 下为 round(7.5)=8 点。 */
    int minimum_lag = (int)(0.30f * (float)SAMPLE_RATE_HZ + 0.5f);
    /* 至少保留两个点延迟，保证后续局部峰左右邻点存在。 */
    if (minimum_lag < 2) minimum_lag = 2;
    /* maximum_lag 初始取半窗，保证每个延迟有足够重叠样本。 */
    int maximum_lag = n / 2;
    /* 三秒上限换算为采样点。 */
    int three_seconds = 3 * SAMPLE_RATE_HZ;
    /* 半窗超过三秒时截断到部署合同上限。 */
    if (maximum_lag > three_seconds) maximum_lag = three_seconds;
    /* 空延迟范围返回零。 */
    if (maximum_lag <= minimum_lag) return 0.0f;
    /* correlation 按延迟升序保存归一化自相关。 */
    float correlation[WINDOW_LEN];
    /* correlation_count 是实际延迟点数。 */
    int correlation_count = 0;
    /* 从最小延迟遍历到最大延迟并计算重叠点积。 */
    for (int lag = minimum_lag; lag <= maximum_lag; lag++) {
        /* dot 累加当前延迟的中心化交叉乘积。 */
        float dot = 0.0f;
        /* 重叠长度为 n-lag，索引不会越过输入末尾。 */
        for (int i = 0; i < n - lag; i++) {
            /* 两个延迟对应点均减去同一窗口均值。 */
            dot += (gyro_magnitude[i] - mean) * (gyro_magnitude[i + lag] - mean);
        }
        /* 统一除以零延迟能量，保持与 Python 候选公式一致。 */
        correlation[correlation_count++] = dot / energy;
    }
    /* 少于三个相关点时无法定义内部峰，返回区间最大正值。 */
    if (correlation_count < 3) {
        /* best 从第一个相关值开始，避免读取未初始化内存。 */
        float best = correlation[0];
        /* 扫描剩余相关值寻找最大值。 */
        for (int i = 1; i < correlation_count; i++) if (correlation[i] > best) best = correlation[i];
        /* 负最大值表示没有正周期证据，夹紧为零。 */
        return best > 0.0f ? best : 0.0f;
    }
    /* 按时间顺序寻找第一个正内部局部峰。 */
    for (int i = 1; i < correlation_count - 1; i++) {
        /* 当前点严格高于左点、不低于右点且为正时立即返回。 */
        if (correlation[i] > correlation[i - 1] && correlation[i] >= correlation[i + 1] && correlation[i] > 0.0f) return correlation[i];
    }
    /* 没有内部峰时退化为搜索范围内最大正相关，覆盖边界周期。 */
    float best = correlation[0];
    /* 遍历全部相关值寻找最大项。 */
    for (int i = 1; i < correlation_count; i++) if (correlation[i] > best) best = correlation[i];
    /* 返回非负第一峰强度。 */
    return best > 0.0f ? best : 0.0f;
}

static inline float wrist_acf_second_first_ratio(const float* gyro_magnitude, int n) {
    /* 少于 10 点时没有合法 0.3 秒延迟范围。 */
    if (n < 10) return 0.0f;
    /* mean 是角速度模长窗口均值，单位 deg/s。 */
    float mean = 0.0f;
    /* 遍历输入累计均值分子。 */
    for (int i = 0; i < n; i++) mean += gyro_magnitude[i];
    /* 除以点数得到均值。 */
    mean /= (float)n;
    /* energy 是去均值零延迟能量。 */
    float energy = 0.0f;
    /* 累计中心化平方和。 */
    for (int i = 0; i < n; i++) { float centered = gyro_magnitude[i] - mean; energy += centered * centered; }
    /* 近常量窗口没有周期证据。 */
    if (energy <= 1e-12f) return 0.0f;
    /* 最小延迟 round(0.30*25)=8 点。 */
    int minimum_lag = (int)(0.30f * (float)SAMPLE_RATE_HZ + 0.5f);
    /* 数值下限为 2，确保局部峰左右点存在。 */
    if (minimum_lag < 2) minimum_lag = 2;
    /* 最大延迟先取半窗。 */
    int maximum_lag = n / 2;
    /* 三秒对应 3*SAMPLE_RATE_HZ 点。 */
    int three_seconds = 3 * SAMPLE_RATE_HZ;
    /* 超过三秒时截断。 */
    if (maximum_lag > three_seconds) maximum_lag = three_seconds;
    /* 无合法范围时返回零。 */
    if (maximum_lag <= minimum_lag) return 0.0f;
    /* correlation 保存 minimum_lag..maximum_lag 的归一化自相关。 */
    float correlation[WINDOW_LEN];
    /* correlation_count 是有效延迟数量。 */
    int correlation_count = 0;
    /* 按延迟时间升序计算中心化点积。 */
    for (int lag = minimum_lag; lag <= maximum_lag; lag++) {
        /* dot 累加当前延迟的重叠点积。 */
        float dot = 0.0f;
        /* 重叠区长度为 n-lag。 */
        for (int i = 0; i < n - lag; i++) dot += (gyro_magnitude[i] - mean) * (gyro_magnitude[i + lag] - mean);
        /* 与 Python 一致统一除以零延迟能量。 */
        correlation[correlation_count++] = dot / energy;
    }
    /* 少于三个相关点不能定义两个内部峰。 */
    if (correlation_count < 3) return 0.0f;
    /* first_peak 和 second_peak 按时间先后保存前两个正局部峰。 */
    float first_peak = 0.0f, second_peak = 0.0f;
    /* peak_count 记录已找到的时间峰数量。 */
    int peak_count = 0;
    /* 遍历内部相关点，保证左右邻点存在。 */
    for (int i = 1; i < correlation_count - 1; i++) {
        /* 当前值须严格高于左点、不低于右点且为正。 */
        if (correlation[i] > correlation[i - 1] && correlation[i] >= correlation[i + 1] && correlation[i] > 0.0f) {
            /* 第一个时间峰写入 first_peak。 */
            if (peak_count == 0) first_peak = correlation[i];
            /* 第二个时间峰写入后即可停止。 */
            else { second_peak = correlation[i]; break; }
            /* 峰计数增加一。 */
            peak_count++;
        }
    }
    /* 未找到两个峰或第一峰过小时返回零。 */
    if (peak_count < 1 || second_peak <= 0.0f || first_peak <= 1e-12f) return 0.0f;
    /* 计算第二/第一时间峰比。 */
    float ratio = second_peak / first_peak;
    /* 理论下限为零。 */
    if (ratio < 0.0f) ratio = 0.0f;
    /* 限制异常上限为 5，与 Python np.clip 一致。 */
    if (ratio > 5.0f) ratio = 5.0f;
    /* 返回无量纲比值。 */
    return ratio;
}

/*
 * 对最多 WINDOW_LEN 个手腕标量执行插入排序并返回中位数。
 * 输入数组只读；额外 RAM 为 WINDOW_LEN 个 float，时间复杂度 O(N^2)。
 */
static inline float wrist_window_median(const float* values, int count) {
    /* 空输入没有稳健中心，返回确定性零。 */
    if (count <= 0) return 0.0f;
    /* sorted 保存不超过一个推理窗口的副本，避免修改调用者数据。 */
    float sorted[WINDOW_LEN];
    /* 复制 count 个有效值。 */
    for (int i = 0; i < count; i++) sorted[i] = values[i];
    /* 对固定小数组执行稳定插入排序。 */
    for (int i = 1; i < count; i++) {
        /* key 是本轮待插入值。 */
        float key = sorted[i];
        /* j 从有序前缀末尾向前移动。 */
        int j = i - 1;
        /* 大于 key 的元素逐项右移。 */
        while (j >= 0 && sorted[j] > key) {
            /* 为 key 腾出插入位置。 */
            sorted[j + 1] = sorted[j];
            /* 继续检查前一个值。 */
            j--;
        }
        /* 写入 key 的最终有序位置。 */
        sorted[j + 1] = key;
    }
    /* 奇数点返回中央顺序统计量。 */
    if ((count & 1) != 0) return sorted[count / 2];
    /* 偶数点返回两个中央值均值，与 numpy.median 一致。 */
    return 0.5f * (sorted[count / 2 - 1] + sorted[count / 2]);
}

/*
 * 追加六项经文件分组三折验证的手腕形态、冲击恢复和周期特征。
 * window 通道顺序固定为 gx、gy、gz、ax、ay、az；角速度单位 deg/s，加速度单位 g。
 * 输出顺序必须与 WEAK_CLASS_FEATURE_NAMES 末六项一致；时间复杂度由直接 DFT 主导为 O(N^2)。
 */
static inline void append_additional_wrist_features(
    const float window[WINDOW_LEN][AXIS_NUM],
    const float* gyro_magnitude,
    float* feature,
    int* index
) {
    /* half_length 是前后摆动形状比较的共同长度。 */
    int half_length = WINDOW_LEN / 2;
    /* reversed_second 保存时间反转后的后半段角速度模长。 */
    float reversed_second[WINDOW_LEN / 2];
    /* 复制后半段并反转时间方向，使其与前半段外摆轨迹对齐。 */
    for (int i = 0; i < half_length; i++) reversed_second[i] = gyro_magnitude[WINDOW_LEN - 1 - i];
    /* 皮尔逊相关比较前半段和反转后半段的归一化波形。 */
    feature[(*index)++] = series_correlation(gyro_magnitude, reversed_second, half_length);

    /* acc_mean 保存 ax、ay、az 的全窗均值，单位 g。 */
    float acc_mean[3] = {0.0f, 0.0f, 0.0f};
    /* 累加三轴加速度以估计窗口内静态分量。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* ax 对应 window 第 3 列。 */
        acc_mean[0] += window[i][3];
        /* ay 对应 window 第 4 列。 */
        acc_mean[1] += window[i][4];
        /* az 对应 window 第 5 列。 */
        acc_mean[2] += window[i][5];
    }
    /* 三轴累加和除以窗口长度得到均值。 */
    for (int axis = 0; axis < 3; axis++) acc_mean[axis] /= (float)WINDOW_LEN;
    /* acc_magnitude 保存原始 specific-force 模长，单位 g。 */
    float acc_magnitude[WINDOW_LEN];
    /* dynamic_magnitude 保存去三轴均值后的动态加速度模长，单位 g。 */
    float dynamic_magnitude[WINDOW_LEN];
    /* 遍历窗口并计算两个加速度模长。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* ax、ay、az 是当前手腕加速度三轴值。 */
        float ax = window[i][3], ay = window[i][4], az = window[i][5];
        /* 原始模长供 jerk 计算。 */
        acc_magnitude[i] = sqrtf(ax * ax + ay * ay + az * az);
        /* dx、dy、dz 是相对窗口均值的动态分量。 */
        float dx = ax - acc_mean[0], dy = ay - acc_mean[1], dz = az - acc_mean[2];
        /* 动态模长供能量和恢复时间计算。 */
        dynamic_magnitude[i] = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    /* jerk 首点无前驱，固定为零；其余点单位 g/s。 */
    float jerk[WINDOW_LEN];
    /* smoothed_jerk 是边缘复制的三点均值结果。 */
    float smoothed_jerk[WINDOW_LEN];
    /* 第一 jerk 采样没有前一时刻。 */
    jerk[0] = 0.0f;
    /* 相邻模长绝对差乘采样率得到离散 jerk。 */
    for (int i = 1; i < WINDOW_LEN; i++) jerk[i] = fabsf(acc_magnitude[i] - acc_magnitude[i - 1]) * (float)SAMPLE_RATE_HZ;
    /* 三点平滑遍历全部时间点，首尾使用自身复制值。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* left 在首点复制 jerk[0]。 */
        float left = jerk[i > 0 ? i - 1 : 0];
        /* right 在末点复制 jerk[WINDOW_LEN-1]。 */
        float right = jerk[i + 1 < WINDOW_LEN ? i + 1 : WINDOW_LEN - 1];
        /* 固定核 [1,1,1]/3 与 Python np.convolve 一致。 */
        smoothed_jerk[i] = (left + jerk[i] + right) / 3.0f;
    }
    /* event_index 保存最早的最大平滑 jerk 位置。 */
    int event_index = 0;
    /* strictly greater 保证平局不覆盖早期事件。 */
    for (int i = 1; i < WINDOW_LEN; i++) if (smoothed_jerk[i] > smoothed_jerk[event_index]) event_index = i;
    /* context_points 对应 round(0.4*25)=10 点。 */
    int context_points = (int)(0.40f * (float)SAMPLE_RATE_HZ + 0.5f);
    /* 至少保留两个上下文点。 */
    if (context_points < 2) context_points = 2;
    /* pre_start 限制在窗口左边界。 */
    int pre_start = event_index - context_points;
    /* 负起点截断为零。 */
    if (pre_start < 0) pre_start = 0;
    /* post_end 为包含事件点的右侧半开终点。 */
    int post_end = event_index + context_points + 1;
    /* 超过窗口时截断。 */
    if (post_end > WINDOW_LEN) post_end = WINDOW_LEN;
    /* post_peak 保存事件后 0.4 秒内最大平滑 jerk。 */
    float post_peak = smoothed_jerk[event_index];
    /* 扫描局部后区间更新峰值。 */
    for (int i = event_index + 1; i < post_end; i++) if (smoothed_jerk[i] > post_peak) post_peak = smoothed_jerk[i];
    /* half_height 是冲击局部峰值一半。 */
    float half_height = 0.5f * post_peak;
    /* half_width_points 统计事件起始后的连续半高点。 */
    int half_width_points = 0;
    /* 从事件点顺序扫描，首次低于半高即终止。 */
    for (int i = event_index; i < post_end; i++) {
        /* 正峰值且当前点达到半高时累计。 */
        if (post_peak > 1e-12f && smoothed_jerk[i] >= half_height) half_width_points++;
        /* 零峰或跌破半高均结束连续宽度。 */
        else break;
    }
    /* 点数除以采样率得到秒。 */
    feature[(*index)++] = (float)half_width_points / (float)SAMPLE_RATE_HZ;
    /* pre_energy 和 post_energy 是动态加速度模长平方和，单位 g^2。 */
    float pre_energy = 0.0f, post_energy = 0.0f;
    /* 累计事件前半开区间能量。 */
    for (int i = pre_start; i < event_index; i++) pre_energy += dynamic_magnitude[i] * dynamic_magnitude[i];
    /* 累计包含事件点的事件后区间能量。 */
    for (int i = event_index; i < post_end; i++) post_energy += dynamic_magnitude[i] * dynamic_magnitude[i];
    /* 1e-9 保护零能量窗口，输出无量纲自然对数比。 */
    feature[(*index)++] = logf((post_energy + 1e-9f) / (pre_energy + 1e-9f));
    /* dynamic_median 是动态加速度稳健中心。 */
    float dynamic_median = wrist_window_median(dynamic_magnitude, WINDOW_LEN);
    /* deviations 保存每点到中位数的绝对距离，单位 g。 */
    float deviations[WINDOW_LEN];
    /* 生成 MAD 所需绝对偏差。 */
    for (int i = 0; i < WINDOW_LEN; i++) deviations[i] = fabsf(dynamic_magnitude[i] - dynamic_median);
    /* MAD 对孤立冲击不敏感。 */
    float dynamic_mad = wrist_window_median(deviations, WINDOW_LEN);
    /* 稳健恢复门槛为 median+0.5*MAD。 */
    float recovery_threshold = dynamic_median + 0.5f * dynamic_mad;
    /* 实际下限 0.05g 避免静止量化噪声产生长恢复。 */
    if (recovery_threshold < 0.05f) recovery_threshold = 0.05f;
    /* 默认到窗口末点仍未恢复。 */
    int recovery_index = WINDOW_LEN - 1;
    /* 寻找事件后连续两个不超过门槛的采样。 */
    for (int i = event_index + 1; i < WINDOW_LEN - 1; i++) {
        /* 首个连续双点满足条件的位置定义为恢复点。 */
        if (dynamic_magnitude[i] <= recovery_threshold && dynamic_magnitude[i + 1] <= recovery_threshold) {
            /* 保存最早恢复位置。 */
            recovery_index = i;
            /* 只保留第一次恢复。 */
            break;
        }
    }
    /* denominator 至少为一，避免事件位于末点时除零。 */
    int recovery_denominator = WINDOW_LEN - 1 - event_index;
    /* 末点事件使用一作为分母。 */
    if (recovery_denominator < 1) recovery_denominator = 1;
    /* 输出事件后归一化恢复时长。 */
    feature[(*index)++] = (float)(recovery_index - event_index) / (float)recovery_denominator;

    /* gyro_smoothed 是用于周期峰检测的三点平滑角速度模长。 */
    float gyro_smoothed[WINDOW_LEN];
    /* 平滑全部角速度点，边界复制。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* left 取前点，首点使用自身。 */
        float left = gyro_magnitude[i > 0 ? i - 1 : 0];
        /* right 取后点，末点使用自身。 */
        float right = gyro_magnitude[i + 1 < WINDOW_LEN ? i + 1 : WINDOW_LEN - 1];
        /* 三点均值与 Python 一致。 */
        gyro_smoothed[i] = (left + gyro_magnitude[i] + right) / 3.0f;
    }
    /* gyro_mean 和 gyro_variance 由未平滑模长计算峰门槛。 */
    float gyro_mean = 0.0f, gyro_variance = 0.0f;
    /* 累加均值分子。 */
    for (int i = 0; i < WINDOW_LEN; i++) gyro_mean += gyro_magnitude[i];
    /* 除以点数得到 deg/s 均值。 */
    gyro_mean /= (float)WINDOW_LEN;
    /* 累加总体方差分子。 */
    for (int i = 0; i < WINDOW_LEN; i++) { float delta = gyro_magnitude[i] - gyro_mean; gyro_variance += delta * delta; }
    /* 除以 N 与 numpy.std 默认 ddof=0 一致。 */
    gyro_variance /= (float)WINDOW_LEN;
    /* 中位数加 0.5 倍标准差作为周期活动峰门槛。 */
    float cycle_threshold = wrist_window_median(gyro_magnitude, WINDOW_LEN) + 0.5f * sqrtf(gyro_variance);
    /* candidate 标记满足局部峰和幅值门槛的内部点。 */
    int candidate[WINDOW_LEN] = {0};
    /* 扫描所有具有左右邻点的采样。 */
    for (int i = 1; i < WINDOW_LEN - 1; i++) {
        /* 严格高于左点、不低于右点且达到门槛时成为候选。 */
        if (gyro_smoothed[i] > gyro_smoothed[i - 1] && gyro_smoothed[i] >= gyro_smoothed[i + 1] && gyro_smoothed[i] >= cycle_threshold) candidate[i] = 1;
    }
    /* selected 保存按强度筛选后的峰索引。 */
    int selected[WINDOW_LEN];
    /* selected_count 是当前保留峰数。 */
    int selected_count = 0;
    /* minimum_distance 对应 round(0.3*25)=8 点。 */
    int minimum_distance = (int)(0.30f * (float)SAMPLE_RATE_HZ + 0.5f);
    /* 数值下限为两个采样点。 */
    if (minimum_distance < 2) minimum_distance = 2;
    /* 每轮取剩余候选中幅值最大、索引最小者。 */
    while (1) {
        /* best_index=-1 表示已无候选峰。 */
        int best_index = -1;
        /* 线性扫描实现 Python 的 (-幅值,索引) 排序顺序。 */
        for (int i = 1; i < WINDOW_LEN - 1; i++) if (candidate[i] && (best_index < 0 || gyro_smoothed[i] > gyro_smoothed[best_index])) best_index = i;
        /* 无候选时结束强峰选择。 */
        if (best_index < 0) break;
        /* 当前候选处理后清除标记。 */
        candidate[best_index] = 0;
        /* keep 默认保留，若靠近任何已选强峰则取消。 */
        int keep = 1;
        /* 检查与全部已选峰的距离。 */
        for (int j = 0; j < selected_count; j++) if (abs(best_index - selected[j]) < minimum_distance) keep = 0;
        /* 通过最小距离约束时追加峰索引。 */
        if (keep) selected[selected_count++] = best_index;
    }
    /* 对已选索引升序排序，便于计算时间间隔。 */
    for (int i = 1; i < selected_count; i++) {
        /* key 是当前待插入峰索引。 */
        int key = selected[i];
        /* j 从有序前缀末尾开始。 */
        int j = i - 1;
        /* 大于 key 的峰索引右移。 */
        while (j >= 0 && selected[j] > key) { selected[j + 1] = selected[j]; j--; }
        /* 将 key 写入时间顺序位置。 */
        selected[j + 1] = key;
    }
    /* cycle_cv 默认无周期证据为零。 */
    float cycle_cv = 0.0f;
    /* 至少三个峰形成两个间隔时才计算 CV。 */
    if (selected_count >= 3) {
        /* interval_count 比峰数少一。 */
        int interval_count = selected_count - 1;
        /* interval_mean 累加峰间隔采样点数。 */
        float interval_mean = 0.0f;
        /* 累加全部相邻峰间隔。 */
        for (int i = 0; i < interval_count; i++) interval_mean += (float)(selected[i + 1] - selected[i]);
        /* 除以间隔数得到均值。 */
        interval_mean /= (float)interval_count;
        /* interval_variance 累加总体方差。 */
        float interval_variance = 0.0f;
        /* 遍历间隔并累计离均差平方。 */
        for (int i = 0; i < interval_count; i++) { float delta = (float)(selected[i + 1] - selected[i]) - interval_mean; interval_variance += delta * delta; }
        /* 有效均值时按总体标准差除以均值得到无量纲 CV。 */
        if (interval_mean > 1e-12f) cycle_cv = sqrtf(interval_variance / (float)interval_count) / interval_mean;
    }
    /* 追加周期峰间隔变异系数。 */
    feature[(*index)++] = cycle_cv;
    /* two_pi 是 Hann 窗和直接 DFT 的 2*pi 单精度常量。 */
    const float two_pi = 6.2831853071795864769f;
    /* total_power 用于识别近静止窗口。 */
    float total_power = 0.0f;
    /* dominant_power 和 dominant_index 保存最早最大非直流频点。 */
    float dominant_power = 0.0f;
    int dominant_index = 0;
    /* 遍历 rfft 的非直流单边频点。 */
    for (int k = 1; k <= WINDOW_LEN / 2; k++) {
        /* real 和 imaginary 累加当前频点 DFT。 */
        float real = 0.0f, imaginary = 0.0f;
        /* 遍历窗口计算去均值、Hann 加窗后的 DFT。 */
        for (int sample = 0; sample < WINDOW_LEN; sample++) {
            /* Hann 系数与 numpy.hanning(WINDOW_LEN) 一致。 */
            float hann = 0.5f - 0.5f * cosf(two_pi * (float)sample / (float)(WINDOW_LEN - 1));
            /* value 是中心化角速度模长乘 Hann 系数。 */
            float value = (gyro_magnitude[sample] - gyro_mean) * hann;
            /* angle 是当前频点相位，单位 rad。 */
            float angle = two_pi * (float)k * (float)sample / (float)WINDOW_LEN;
            /* 累加实部。 */
            real += value * cosf(angle);
            /* 累加负正弦虚部。 */
            imaginary -= value * sinf(angle);
        }
        /* 当前功率是实部和虚部平方和。 */
        float power = real * real + imaginary * imaginary;
        /* 累加非直流总功率。 */
        total_power += power;
        /* 严格更大才更新，平局保留较低频率。 */
        if (power > dominant_power) { dominant_power = power; dominant_index = k; }
    }
    /* harmonic_ratio 默认近静止或频谱不足时为零。 */
    float harmonic_ratio = 0.0f;
    /* 有效主频功率和总功率时计算二次谐波。 */
    if (total_power > 1e-12f && dominant_power > 1e-12f && WINDOW_LEN / 2 >= 2) {
        /* 二次谐波超出单边频谱时截断到最后频点。 */
        int harmonic_index = 2 * dominant_index;
        /* 限制到 rfft 末端。 */
        if (harmonic_index > WINDOW_LEN / 2) harmonic_index = WINDOW_LEN / 2;
        /* 重新计算目标谐波频点 DFT。 */
        float real = 0.0f, imaginary = 0.0f;
        /* 遍历全部时域点。 */
        for (int sample = 0; sample < WINDOW_LEN; sample++) {
            /* Hann 窗系数。 */
            float hann = 0.5f - 0.5f * cosf(two_pi * (float)sample / (float)(WINDOW_LEN - 1));
            /* 去均值加窗角速度模长。 */
            float value = (gyro_magnitude[sample] - gyro_mean) * hann;
            /* 二次谐波 DFT 相位。 */
            float angle = two_pi * (float)harmonic_index * (float)sample / (float)WINDOW_LEN;
            /* 累加实部。 */
            real += value * cosf(angle);
            /* 累加虚部。 */
            imaginary -= value * sinf(angle);
        }
        /* 谐波功率除以基频功率得到无量纲比值。 */
        harmonic_ratio = (real * real + imaginary * imaginary) / dominant_power;
    }
    /* 追加二次谐波/基频功率比，完成六项输出。 */
    feature[(*index)++] = harmonic_ratio;
}

/*
 * 把一个 [62,6] 手腕 IMU 窗口变换为固定顺序的 297 维 float 特征。
 * raw_window 通道依次为 gx、gy、gz、ax、ay、az，前三轴 deg/s、后三轴 g；feature 形状 [FEATURE_DIM]。
 * 函数先修复单轴孤立毛刺，再生成 112+48+24+48+32+33 六组特征；顺序必须与 Python build_feature_names 完全一致。
 * DFT、自相关和小数组排序使最坏时间复杂度 O(WINDOW_LEN^2)，静态局部数组占用约 4.8 KiB。
 */
static inline void extract_features_from_window(const float raw_window[WINDOW_LEN][AXIS_NUM], float feature[FEATURE_DIM]) {
    /* cleaned_window 保存尖峰修复后的 [WINDOW_LEN,6] 六轴数据，约占 WINDOW_LEN*24 字节 RAM。 */
    float cleaned_window[WINDOW_LEN][AXIS_NUM];
    /* 按 Python 相同阈值和单轴判据清洗输入，保证部署端与训练端特征数值一致。 */
    preprocess_imu_window(raw_window, cleaned_window);
    /* C99 对二维数组添加 const 限定不会隐式转换，因此显式转为只读数组指针供全部特征函数使用。 */
    const float (*window)[AXIS_NUM] = (const float (*)[AXIS_NUM])cleaned_window;
    /* idx 记录下一个待写入特征位置，最终必须等于 FEATURE_DIM。 */
    int idx = 0;
    /* temp 复用为当前一维序列缓冲区，长度上限为 WINDOW_LEN。 */
    float temp[WINDOW_LEN];
    /* phase_sources 保存四条相位特征源序列，形状为 [4,WINDOW_LEN]。 */
    float phase_sources[4][WINDOW_LEN];
    /* phase_lengths 标记前三条长度为 WINDOW_LEN，差分加速度长度为 WINDOW_LEN-1。 */
    int phase_lengths[4] = { WINDOW_LEN, WINDOW_LEN, WINDOW_LEN, WINDOW_LEN - 1 };

    /* 第一组先按 gx、gy、gz、ax、ay、az 顺序为六条原轴序列各追加八项基础统计。 */
    for (int axis = 0; axis < AXIS_NUM; axis++) {
        /* 当前 axis 的 WINDOW_LEN 个采样依时间顺序复制到复用缓冲。 */
        for (int i = 0; i < WINDOW_LEN; i++) temp[i] = window[i][axis];
        /* 追加当前轴八项统计，前三轴单位 deg/s、后三轴单位 g。 */
        append_series_features(temp, WINDOW_LEN, feature, &idx);
    }

    /* 构造角速度模长 gyro_mag，并保存为第三条相位源序列。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* gx 是当前点 x 轴角速度，单位 deg/s。 */
        float gx = window[i][0];
        /* gy 是当前点 y 轴角速度，单位 deg/s。 */
        float gy = window[i][1];
        /* gz 是当前点 z 轴角速度，单位 deg/s。 */
        float gz = window[i][2];
        /* 三轴欧氏模长描述与佩戴轴旋转较稳健的总体转动强度。 */
        temp[i] = sqrtf(gx * gx + gy * gy + gz * gz);
        /* 相位源索引二固定对应 gyro_mag。 */
        phase_sources[2][i] = temp[i];
    }
    /* 为角速度模长追加八项基础统计。 */
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    /* 构造加速度模长 acc_mag，单位 g。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* ax 是当前 x 轴加速度。 */
        float ax = window[i][3];
        /* ay 是当前 y 轴加速度。 */
        float ay = window[i][4];
        /* az 是当前 z 轴加速度。 */
        float az = window[i][5];
        /* 三轴欧氏模长保留重力和动态冲击总量。 */
        temp[i] = sqrtf(ax * ax + ay * ay + az * az);
    }
    /* 为加速度模长追加八项基础统计。 */
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    /* 构造相邻角速度差模长 gyro_delta_mag，共 WINDOW_LEN-1 点。 */
    for (int i = 0; i < WINDOW_LEN - 1; i++) {
        /* dx 是相邻 gx 差，单位 deg/s 每采样。 */
        float dx = window[i + 1][0] - window[i][0];
        /* dy 是相邻 gy 差。 */
        float dy = window[i + 1][1] - window[i][1];
        /* dz 是相邻 gz 差。 */
        float dz = window[i + 1][2] - window[i][2];
        /* 欧氏模长突出手腕快速换向。 */
        temp[i] = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    /* 为角速度差模长追加八项统计。 */
    append_series_features(temp, WINDOW_LEN - 1, feature, &idx);

    /* 构造相邻加速度差模长 acc_delta_mag，并保存为第四条相位源。 */
    for (int i = 0; i < WINDOW_LEN - 1; i++) {
        /* dx 是相邻 ax 差，单位 g 每采样。 */
        float dx = window[i + 1][3] - window[i][3];
        /* dy 是相邻 ay 差。 */
        float dy = window[i + 1][4] - window[i][4];
        /* dz 是相邻 az 差。 */
        float dz = window[i + 1][5] - window[i][5];
        /* 欧氏模长突出起跳和落地冲击变化。 */
        temp[i] = sqrtf(dx * dx + dy * dy + dz * dz);
        /* 相位源索引三固定对应 acc_delta_mag。 */
        phase_sources[3][i] = temp[i];
    }
    /* 为加速度差模长追加八项统计。 */
    append_series_features(temp, WINDOW_LEN - 1, feature, &idx);

    /* 计算全窗平均加速度方向，用作手腕坐标系中的近似重力轴。 */
    float gravity_x = 0.0f;
    /* gravity_y 累计 y 轴加速度均值。 */
    float gravity_y = 0.0f;
    /* gravity_z 累计 z 轴加速度均值。 */
    float gravity_z = 0.0f;
    /* 遍历 62 点累计三轴加速度。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* 累加 ax。 */
        gravity_x += window[i][3];
        /* 累加 ay。 */
        gravity_y += window[i][4];
        /* 累加 az。 */
        gravity_z += window[i][5];
    }
    /* 除以窗口长度得到平均 ax。 */
    gravity_x /= (float)WINDOW_LEN;
    /* 除以窗口长度得到平均 ay。 */
    gravity_y /= (float)WINDOW_LEN;
    /* 除以窗口长度得到平均 az。 */
    gravity_z /= (float)WINDOW_LEN;
    /* gravity_norm 是平均加速度向量模长，单位 g。 */
    float gravity_norm = sqrtf(
        gravity_x * gravity_x + gravity_y * gravity_y + gravity_z * gravity_z
    );
    /* 近零均值属于异常或极端动态窗，回退 z 轴避免除零。 */
    if (gravity_norm < 1e-6f) {
        /* 回退单位方向 x 分量。 */
        gravity_x = 0.0f;
        /* 回退单位方向 y 分量。 */
        gravity_y = 0.0f;
        /* 回退单位方向 z 分量。 */
        gravity_z = 1.0f;
    } else {
        /* 正常输入把 x 分量归一化。 */
        gravity_x /= gravity_norm;
        /* 正常输入把 y 分量归一化。 */
        gravity_y /= gravity_norm;
        /* 正常输入把 z 分量归一化。 */
        gravity_z /= gravity_norm;
    }

    /* 把三轴加速度投影到重力单位方向，得到垂直加速度序列。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* vertical 是当前加速度与重力方向点积，单位 g。 */
        float vertical =
            window[i][3] * gravity_x +
            window[i][4] * gravity_y +
            window[i][5] * gravity_z;
        /* 保存到复用缓冲供基础统计。 */
        temp[i] = vertical;
        /* 相位源索引零固定对应垂直加速度。 */
        phase_sources[0][i] = vertical;
    }
    /* 为垂直加速度追加八项基础统计。 */
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    /* 从总加速度平方减去垂直分量平方，得到水平加速度模长。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* vertical 读取已经计算的垂直加速度。 */
        float vertical = phase_sources[0][i];
        /* total_squared 是三轴加速度模长平方。 */
        float total_squared =
            window[i][3] * window[i][3] +
            window[i][4] * window[i][4] +
            window[i][5] * window[i][5];
        /* horizontal_squared 按勾股分解得到水平分量平方。 */
        float horizontal_squared = total_squared - vertical * vertical;
        /* 浮点误差可能产生微小负数，夹紧后再开方。 */
        if (horizontal_squared < 0.0f) horizontal_squared = 0.0f;
        /* 水平加速度模长单位 g。 */
        temp[i] = sqrtf(horizontal_squared);
        /* 相位源索引一固定对应水平加速度模长。 */
        phase_sources[1][i] = temp[i];
    }
    /* 为水平加速度模长追加八项基础统计。 */
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    /* 把三轴角速度投影到重力方向，得到垂直轴角速度。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* 点积结果单位 deg/s。 */
        temp[i] =
            window[i][0] * gravity_x +
            window[i][1] * gravity_y +
            window[i][2] * gravity_z;
    }
    /* 为垂直角速度追加八项基础统计。 */
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    /* 从角速度总模长中移除垂直投影，得到水平角速度模长。 */
    for (int i = 0; i < WINDOW_LEN; i++) {
        /* vertical 是角速度在重力方向投影，单位 deg/s。 */
        float vertical =
            window[i][0] * gravity_x +
            window[i][1] * gravity_y +
            window[i][2] * gravity_z;
        /* total_squared 是三轴角速度模长平方。 */
        float total_squared =
            window[i][0] * window[i][0] +
            window[i][1] * window[i][1] +
            window[i][2] * window[i][2];
        /* horizontal_squared 是正交平面角速度模长平方。 */
        float horizontal_squared = total_squared - vertical * vertical;
        /* 夹紧微小负值保护平方根。 */
        if (horizontal_squared < 0.0f) horizontal_squared = 0.0f;
        /* 水平角速度模长单位 deg/s。 */
        temp[i] = sqrtf(horizontal_squared);
    }
    /* 为水平角速度模长追加八项基础统计，至此完成前 112 项。 */
    append_series_features(temp, WINDOW_LEN, feature, &idx);

    /* 依次为四条关键源追加每源 12 项原始四阶段统计，共 48 项。 */
    for (int source = 0; source < 4; source++) {
        /* 当前源长度由 phase_lengths 指定，差分源少一个采样点。 */
        append_phase_features(
            phase_sources[source], phase_lengths[source], feature, &idx
        );
    }
    /* 依次为四条关键源追加每源六项时序/频谱统计，共 24 项。 */
    for (int source = 0; source < 4; source++) {
        /* 函数内部使用 25 Hz 采样率把频点和延迟换算为物理时间。 */
        append_temporal_features(
            phase_sources[source], phase_lengths[source], feature, &idx
        );
    }
    /* 依次为四条关键源追加每源 12 项标准化四阶段统计，共 48 项。 */
    for (int source = 0; source < 4; source++) {
        /* 标准差近零的源将追加全零形态特征。 */
        append_normalized_phase_features(
            phase_sources[source], phase_lengths[source], feature, &idx
        );
    }
    /* 依次为四条关键源追加每源八项冲击分布统计，共 32 项。 */
    for (int source = 0; source < 4; source++) {
        /* 输出包含五个百分位、偏度、峰度和最大相邻跳变。 */
        append_impact_distribution_features(
            phase_sources[source], phase_lengths[source], feature, &idx
        );
    }

    /* 以下五个变量复用为频带比例、谱质心和主峰占比输出。 */
    float low_ratio = 0.0f;
    /* mid_ratio 保存 1.20-2.40 Hz 中频功率比例。 */
    float mid_ratio = 0.0f;
    /* high_ratio 保存 2.40-5.00 Hz 高频功率比例。 */
    float high_ratio = 0.0f;
    /* centroid_hz 保存谱质心，单位 Hz。 */
    float centroid_hz = 0.0f;
    /* peak_power_ratio 保存最大非直流频点占总功率比例。 */
    float peak_power_ratio = 0.0f;
    /* 对加速度差模长计算频谱并追加中频功率比例。 */
    selected_spectral_features(
        phase_sources[3], phase_lengths[3], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    /* 弱类机制组第 1 项：加速度差中频比例。 */
    feature[idx++] = mid_ratio;
    /* 对垂直加速度计算频谱并追加高频比例。 */
    selected_spectral_features(
        phase_sources[0], phase_lengths[0], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    /* 弱类机制组第 2 项：垂直加速度高频比例。 */
    feature[idx++] = high_ratio;
    /* 对角速度模长计算频谱并追加谱质心。 */
    selected_spectral_features(
        phase_sources[2], phase_lengths[2], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    /* 弱类机制组第 3 项：角速度模长谱质心 Hz。 */
    feature[idx++] = centroid_hz;
    /* 对水平加速度模长计算频谱并追加谱质心。 */
    selected_spectral_features(
        phase_sources[1], phase_lengths[1], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    /* 弱类机制组第 4 项：水平加速度谱质心 Hz。 */
    feature[idx++] = centroid_hz;
    /* 第 5 项记录垂直加速度自相关首次非正延迟秒数。 */
    feature[idx++] = autocorr_first_zero_seconds(
        phase_sources[0], phase_lengths[0]
    );
    /* 第 6 项记录角速度模长与垂直加速度零延迟 Pearson 相关。 */
    feature[idx++] = series_correlation(
        phase_sources[2], phase_sources[0], WINDOW_LEN
    );
    /* 第 7 项记录水平加速度模长最强自相关次峰。 */
    feature[idx++] = autocorr_secondary_peak(
        phase_sources[1], phase_lengths[1]
    );
    /* 重新计算角速度模长频谱并追加高频比例。 */
    selected_spectral_features(
        phase_sources[2], phase_lengths[2], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    /* 弱类机制组第 8 项：角速度模长高频比例。 */
    feature[idx++] = high_ratio;
    /* 重新计算垂直加速度频谱并追加主峰占比。 */
    selected_spectral_features(
        phase_sources[0], phase_lengths[0], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    /* 弱类机制组第 9 项：垂直加速度主峰功率比例。 */
    feature[idx++] = peak_power_ratio;
    /* 重新计算加速度差模长频谱并追加高频比例。 */
    selected_spectral_features(
        phase_sources[3], phase_lengths[3], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    /* 弱类机制组第 10 项：加速度差高频比例。 */
    feature[idx++] = high_ratio;
    /* 重新计算水平加速度频谱并追加中频比例。 */
    selected_spectral_features(
        phase_sources[1], phase_lengths[1], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    /* 弱类机制组第 11 项：水平加速度中频比例。 */
    feature[idx++] = mid_ratio;
    /* 写入陀螺模长主谱峰功率占比，输出无量纲且范围为 [0,1]。 */
    selected_spectral_features(
        phase_sources[2], phase_lengths[2], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = peak_power_ratio;
    /* 写入垂直加速度 1.20-2.40 Hz 中频功率占比，输出无量纲。 */
    selected_spectral_features(
        phase_sources[0], phase_lengths[0], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = mid_ratio;
    /* 写入水平加速度模长 2.40-5.00 Hz 高频功率占比，输出无量纲。 */
    selected_spectral_features(
        phase_sources[1], phase_lengths[1], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = high_ratio;
    /* peak_amplitude_cv 接收陀螺正峰幅值 CV，peak_interval_cv 接收峰间隔 CV。 */
    float peak_amplitude_cv = 0.0f;
    /* 初始化峰间隔 CV，调用函数后覆盖为对应通道的无量纲结果。 */
    float peak_interval_cv = 0.0f;
    /* 计算陀螺模长峰形，只写入本轮选用的峰幅变异系数。 */
    positive_peak_shape_features(
        phase_sources[2], phase_lengths[2], &peak_amplitude_cv, &peak_interval_cv
    );
    feature[idx++] = peak_amplitude_cv;
    /* 计算垂直加速度峰形，只写入本轮选用的峰间隔变异系数。 */
    positive_peak_shape_features(
        phase_sources[0], phase_lengths[0], &peak_amplitude_cv, &peak_interval_cv
    );
    feature[idx++] = peak_interval_cv;
    /* 写入垂直加速度与陀螺模长在 ±1 秒内的最强有符号互相关。 */
    feature[idx++] = max_cross_correlation(
        phase_sources[0], phase_sources[2], WINDOW_LEN
    );
    /* 写入垂直与水平加速度在 ±1 秒内的最强有符号互相关。 */
    feature[idx++] = max_cross_correlation(
        phase_sources[0], phase_sources[1], WINDOW_LEN
    );
    /* 重新计算垂直加速度频谱并写入 0.35-1.20 Hz 低频功率占比。 */
    selected_spectral_features(
        phase_sources[0], phase_lengths[0], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = low_ratio;
    /* 重新计算水平加速度频谱并写入低频功率占比。 */
    selected_spectral_features(
        phase_sources[1], phase_lengths[1], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = low_ratio;
    /* 重新计算陀螺模长频谱并写入低频功率占比。 */
    selected_spectral_features(
        phase_sources[2], phase_lengths[2], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = low_ratio;
    /* 写入水平加速度归一化自相关显著局部峰数量。 */
    feature[idx++] = autocorr_prominent_peak_count(
        phase_sources[1], phase_lengths[1]
    );
    /* 计算水平加速度正峰形并写入峰间隔总体变异系数。 */
    positive_peak_shape_features(
        phase_sources[1], phase_lengths[1], &peak_amplitude_cv, &peak_interval_cv
    );
    feature[idx++] = peak_interval_cv;
    /* 水平频谱结果仍保存在 peak_power_ratio，写入主谱峰功率占比。 */
    selected_spectral_features(
        phase_sources[1], phase_lengths[1], &low_ratio, &mid_ratio, &high_ratio,
        &centroid_hz, &peak_power_ratio
    );
    feature[idx++] = peak_power_ratio;
    /* 追加水平动态加速度协方差各向异性，输出无量纲 [0,1]。 */
    feature[idx++] = horizontal_anisotropy_from_window(window, 0);
    /* 追加水平角速度协方差各向异性，输出无量纲 [0,1]。 */
    feature[idx++] = horizontal_anisotropy_from_window(window, 1);
    /* 追加起跳-落地时间、冲击宽度及两项腾空角度积分中位数。 */
    append_aligned_event_medians(window, feature, &idx);
    /* 追加手腕 PCA 主轴每秒有效换向次数，单位 Hz。 */
    feature[idx++] = wrist_reversal_rate_hz(window);
    /* 追加手腕角速度模长自相关第二时间峰与第一时间峰之比。 */
    feature[idx++] = wrist_acf_second_first_ratio(
        phase_sources[2], phase_lengths[2]
    );
    /* 追加清洗后晋级的角速度模长第一正自相关峰，形成 297 维最终候选。 */
    feature[idx++] = wrist_acf_first_peak(
        phase_sources[2], phase_lengths[2]
    );
}


#endif
