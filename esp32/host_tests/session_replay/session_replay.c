/* 引入生产训练引擎；本程序不复制分类、活动门或计数算法。 */
#include "workout_engine.h"

/* 引入 errno，校验命令行整数转换。 */
#include <errno.h>
/* 引入 bool 表达锁类、事件和活动门状态。 */
#include <stdbool.h>
/* 引入定长整数，保持设备毫秒和质量位宽度。 */
#include <stdint.h>
/* 引入 stdio，从标准输入流式读取任意真板会话。 */
#include <stdio.h>
/* 引入 stdlib，解析命令行会话序号和动作枚举。 */
#include <stdlib.h>
/* 引入 string.h，清零六轴样本和 logits。 */
#include <string.h>

/* 位 0～3、5、6 与生产训练引擎一致，表示必须切断半周期的时间线质量边界。 */
#define REPLAY_TIMELINE_BREAK_MASK (UINT16_C(0x006F))
/* 首个模型窗包含 62 个 25 Hz 点，与冻结双 M0 输入长度一致。 */
#define REPLAY_FIRST_INFERENCE_SAMPLE_COUNT (62U)
/* 第二窗相对首窗增加 12 点；两窗高置信真值只用于隔离计数器能力。 */
#define REPLAY_SECOND_INFERENCE_SAMPLE_COUNT (74U)

/* 把十进制命令行文本解析为无符号长整数，失败返回 false。 */
static bool replay_parse_unsigned(const char *text, unsigned long *value)
{
    /* 输入和输出地址必须有效。 */
    if ((text == NULL) || (value == NULL)) {
        /* 无法解析空地址。 */
        return false;
    }
    /* 清空 errno，区分合法零和转换错误。 */
    errno = 0;
    /* end 指向首个未消费字符。 */
    char *end = NULL;
    /* 只接受十进制，避免会话序号被前导零误当八进制。 */
    const unsigned long parsed = strtoul(text, &end, 10);
    /* 溢出、空输入或尾随字符都表示参数无效。 */
    if ((errno != 0) || (end == text) || (*end != '\0')) {
        /* 保持输出不变。 */
        return false;
    }
    /* 全部检查通过后提交结果。 */
    *value = parsed;
    /* 返回成功。 */
    return true;
}

/* 构造一个目标类 +4、其它类 -4 的 11 维高置信 logits。 */
static void replay_make_logits(
    float logits[WORKOUT_CLASS_COUNT],
    const uint8_t action)
{
    /* 遍历模型固定类别顺序 0..10。 */
    for (uint8_t class_index = 0U; class_index < WORKOUT_CLASS_COUNT; ++class_index) {
        /* 目标动作获得高分，其它动作获得低分。 */
        logits[class_index] = class_index == action ? 4.0F : -4.0F;
    }
}

/* 输出一条生产 MetricEvent，供外层统计逐次实时位置。 */
static void replay_print_event(
    const uint32_t session_sequence,
    const fitness_metric_event_t *event,
    const bool replayed)
{
    /* 事件地址由生产引擎保证非空。 */
    (void)printf(
        "REPLAY_EVENT session=%lu source=%s event_seq=%lu device_ms=%llu total=%llu kind=%u quality=0x%04X\n",
        (unsigned long)session_sequence,
        replayed ? "prelock" : "live",
        (unsigned long)event->event_seq,
        (unsigned long long)event->monotonic_ms,
        (unsigned long long)event->total_value,
        (unsigned int)event->metric_kind,
        (unsigned int)event->quality_flags);
}

/* 程序入口：参数依次为会话序号和人工真值动作枚举，六轴样本从标准输入读取。 */
int main(int argc, char **argv)
{
    /* 必须提供程序名、会话序号和动作枚举三个参数。 */
    if (argc != 3) {
        /* 输出稳定用法并拒绝猜测。 */
        (void)fprintf(stderr, "usage: session_replay <session-sequence> <action-id>\n");
        /* 返回参数错误。 */
        return 2;
    }
    /* 解析会话序号。 */
    unsigned long session_value = 0UL;
    /* 解析动作枚举。 */
    unsigned long action_value = 0UL;
    /* 两个参数都必须是合法范围整数。 */
    if (!replay_parse_unsigned(argv[1], &session_value) ||
        !replay_parse_unsigned(argv[2], &action_value) ||
        (session_value == 0UL) ||
        (session_value > UINT32_MAX) ||
        (action_value >= FITNESS_ACTION_COUNT)) {
        /* 输出实际错误参数。 */
        (void)fprintf(stderr, "invalid replay arguments session=%s action=%s\n", argv[1], argv[2]);
        /* 返回参数错误。 */
        return 2;
    }
    /* 收窄为产品会话字段宽度。 */
    const uint32_t session_sequence = (uint32_t)session_value;
    /* 收窄为产品动作枚举。 */
    const uint8_t action = (uint8_t)action_value;
    /* 创建与固件完全相同的静态训练引擎。 */
    workout_engine_t engine;
    /* 初始化为空闲。 */
    workout_engine_init(&engine);
    /* logits 保存两次高置信人工真值，只用于把分类错误与计数错误隔离。 */
    float logits[WORKOUT_CLASS_COUNT];
    /* 按目标动作构造固定分数。 */
    replay_make_logits(logits, action);
    /* line 保存一行 ASCII 数值：ms,gx,gy,gz,ax,ay,az,quality。 */
    char line[256];
    /* sample_count 统计已处理 25 Hz 点。 */
    uint32_t sample_count = 0U;
    /* event_count 统计生产引擎公开的权威事件数。 */
    uint32_t event_count = 0U;
    /* gate_transition_count 统计活动/休息门变化次数。 */
    uint32_t gate_transition_count = 0U;
    /* started 表示已经用首点时间建立 PREPARING 会话。 */
    bool started = false;
    /* locked 表示两个真值窗已经选择计数器。 */
    bool locked = false;
    /* last_gate 保存上一个运行点的计数许可。 */
    bool last_gate = false;
    /* first_ms 保存输入首点时间。 */
    uint64_t first_ms = 0ULL;
    /* last_ms 保存输入末点时间。 */
    uint64_t last_ms = 0ULL;

    /* 逐行读取到 EOF；外层可直接把任意 CSV 会话转换后管道输入。 */
    while (fgets(line, sizeof(line), stdin) != NULL) {
        /* 创建当前六轴样本，未解析字段先固定为零。 */
        motion_phase_sample_t sample;
        /* 清空单调时间和六轴数组。 */
        (void)memset(&sample, 0, sizeof(sample));
        /* quality_value 使用 unsigned int 接收 %x，再检查可收窄范围。 */
        unsigned int quality_value = 0U;
        /* monotonic_value 使用与 %llu 完全匹配的临时类型，避免把 uint64_t 地址强制别名为另一整数类型。 */
        unsigned long long monotonic_value = 0ULL;
        /* 按固定八列解析；前三轴 deg/s，后三轴 g，质量位为十六进制。 */
        const int parsed = sscanf(
            line,
            "%llu,%f,%f,%f,%f,%f,%f,%x",
            &monotonic_value,
            &sample.axis[0],
            &sample.axis[1],
            &sample.axis[2],
            &sample.axis[3],
            &sample.axis[4],
            &sample.axis[5],
            &quality_value);
        /* 缺列或质量位超 16 位会破坏生产输入语义，立即失败。 */
        if ((parsed != 8) || (quality_value > UINT16_MAX)) {
            /* 报告下一样本序号，便于定位转换层错误。 */
            (void)fprintf(stderr, "invalid numeric sample at index=%lu\n", (unsigned long)sample_count);
            /* 返回数据错误。 */
            return 3;
        }
        /* 在完整八列均合法后再收窄到生产 uint64_t 单调毫秒字段，避免解析失败时提交半成品时间。 */
        sample.monotonic_ms = (uint64_t)monotonic_value;
        /* 收窄质量位，位定义与生产 IMU pipeline 一致。 */
        const uint16_t quality_flags = (uint16_t)quality_value;
        /* 首点到来时以其设备单调毫秒建立准备会话。 */
        if (!started) {
            /* 保存输入起点。 */
            first_ms = sample.monotonic_ms;
            /* 启动失败表示产品会话合同破坏。 */
            if (workout_engine_start(
                    &engine,
                    session_sequence,
                    70000U,
                    first_ms) != WORKOUT_STATUS_OK) {
                /* 报告启动失败。 */
                (void)fprintf(stderr, "workout start failed\n");
                /* 返回领域错误。 */
                return 4;
            }
            /* 后续点不再重复启动。 */
            started = true;
        }
        /* 时间线质量边界按生产主入口语义先切断未完成周期。 */
        if ((quality_flags & REPLAY_TIMELINE_BREAK_MASK) != 0U) {
            /* 保留主动作和累计，只重建活动窗与相位。 */
            workout_engine_reset_bout_evidence(&engine);
        }
        /* 当前质量为零才允许幅值进入相位；任一质量位仍推进时间和热量。 */
        const bool count_input_valid = quality_flags == 0U;
        /* event 接收当前点可能产生的唯一权威事实。 */
        fitness_metric_event_t event;
        /* emitted 保存是否产生事件。 */
        bool emitted = false;
        /* 提交真实样本；PREPARING 的 IGNORED 表示已经缓存，不是失败。 */
        const workout_status_t sample_status = workout_engine_push_sample(
            &engine,
            &sample,
            count_input_valid,
            quality_flags,
            &event,
            &emitted);
        /* 只接受正常处理或准备缓存。 */
        if ((sample_status != WORKOUT_STATUS_OK) &&
            (sample_status != WORKOUT_STATUS_IGNORED)) {
            /* 报告精确点和错误码。 */
            (void)fprintf(
                stderr,
                "sample failed index=%lu status=%d\n",
                (unsigned long)sample_count,
                (int)sample_status);
            /* 返回领域错误。 */
            return 4;
        }
        /* 当前点公开事件时输出其原始时间。 */
        if (emitted) {
            /* 累加实时事件数。 */
            event_count += 1U;
            /* 输出实时事件。 */
            replay_print_event(session_sequence, &event, false);
        }
        /* 记录本点已消费。 */
        sample_count += 1U;
        /* 保存最新时间。 */
        last_ms = sample.monotonic_ms;
        /* 第 62 点和第 74 点提交相同人工真值 logits，模拟通用两窗确认。 */
        if ((sample_count == REPLAY_FIRST_INFERENCE_SAMPLE_COUNT) ||
            (sample_count == REPLAY_SECOND_INFERENCE_SAMPLE_COUNT)) {
            /* action_locked 只在第二窗成功锁定时为 true。 */
            bool action_locked = false;
            /* 人工真值窗固定使用零质量，只隔离生产计数器能力而不复现分类失败。 */
            const workout_status_t inference_status = workout_engine_push_inference(
                &engine,
                logits,
                sample.monotonic_ms,
                0U,
                &action_locked);
            /* 第一窗应忽略，第二窗应成功；其它状态均为回放器错误。 */
            if ((inference_status != WORKOUT_STATUS_OK) &&
                (inference_status != WORKOUT_STATUS_IGNORED)) {
                /* 报告推理点和状态。 */
                (void)fprintf(
                    stderr,
                    "inference failed samples=%lu status=%d\n",
                    (unsigned long)sample_count,
                    (int)inference_status);
                /* 返回领域错误。 */
                return 4;
            }
            /* 第二窗锁定时记录活动门初值。 */
            if (action_locked) {
                /* 保存锁定事实。 */
                locked = true;
                /* 初始化门变化基准。 */
                last_gate = engine.classification_consistent;
            }
            /* 锁类补算可能形成多条事件，全部按原时间顺序输出。 */
            fitness_metric_event_t replay_event;
            /* 固定 FIFO 取空时结束。 */
            while (workout_engine_pop_replay_metric_event(&engine, &replay_event)) {
                /* 累加补算事件数。 */
                event_count += 1U;
                /* 输出补算事件。 */
                replay_print_event(session_sequence, &replay_event, true);
            }
        }
        /* RUNNING 中门变化用于诊断休息迟滞，不修改计数。 */
        if (locked && (engine.classification_consistent != last_gate)) {
            /* 累加一次开门或关门边界。 */
            gate_transition_count += 1U;
            /* 保存新门状态。 */
            last_gate = engine.classification_consistent;
            /* 输出边界时间和状态。 */
            (void)printf(
                "REPLAY_GATE session=%lu device_ms=%llu enabled=%u\n",
                (unsigned long)session_sequence,
                (unsigned long long)sample.monotonic_ms,
                last_gate ? 1U : 0U);
        }
    }
    /* 空输入无法验证任何会话。 */
    if (!started) {
        /* 报告输入为空。 */
        (void)fprintf(stderr, "no samples\n");
        /* 返回数据错误。 */
        return 3;
    }
    /* 少于 74 点不能完成通用两窗锁定。 */
    if (!locked) {
        /* 报告实际点数。 */
        (void)fprintf(stderr, "action not locked samples=%lu\n", (unsigned long)sample_count);
        /* 返回数据错误。 */
        return 3;
    }
    /* 读取生产权威快照。 */
    workout_snapshot_t snapshot;
    /* 快照失败表示内部状态损坏。 */
    if (workout_engine_snapshot(&engine, &snapshot) != WORKOUT_STATUS_OK) {
        /* 报告快照失败。 */
        (void)fprintf(stderr, "snapshot failed\n");
        /* 返回领域错误。 */
        return 4;
    }
    /* 输出机器可读总计；metric_value 必须与最后事件累计一致。 */
    (void)printf(
        "REPLAY_SUMMARY session=%lu action=%u samples=%lu first_ms=%llu last_ms=%llu events=%lu total=%llu gate=%u gate_transitions=%lu\n",
        (unsigned long)session_sequence,
        (unsigned int)action,
        (unsigned long)sample_count,
        (unsigned long long)first_ms,
        (unsigned long long)last_ms,
        (unsigned long)event_count,
        (unsigned long long)snapshot.metric_value,
        snapshot.classification_consistent ? 1U : 0U,
        (unsigned long)gate_transition_count);
    /* 回放成功。 */
    return 0;
}
