/* 引入 VeriSilicon 原始 StepCounter 的公开输入和处理合同。 */
#include "alg_step_counter.h"

/* 引入 INT16 边界和定长整数，保证数据换算在 Windows/GCC 下宽度确定。 */
#include <stdint.h>
/* 引入文件读写、sscanf 和结果日志。 */
#include <stdio.h>
/* 引入 EXIT_SUCCESS/EXIT_FAILURE。 */
#include <stdlib.h>

/* 单文件最多保存 20000 个 25 Hz 点，即 800 秒；超出时明确报错而不截断。 */
#define VALIDATION_MAX_SAMPLE_COUNT (20000U)
/* 原算法每次固定输入一秒，即 25 个三轴加速度点。 */
#define VALIDATION_CHUNK_SAMPLE_COUNT (25U)

/* 保存第 4～6 列加速度原始值，单位 raw，4096 raw 等于 1 g。 */
static int16_t validation_acc_x[VALIDATION_MAX_SAMPLE_COUNT];
/* 保存传感器 y 轴加速度原始值，单位 raw。 */
static int16_t validation_acc_y[VALIDATION_MAX_SAMPLE_COUNT];
/* 保存传感器 z 轴加速度原始值，单位 raw。 */
static int16_t validation_acc_z[VALIDATION_MAX_SAMPLE_COUNT];

/* 把带符号 32 位原始值安全转换为 StepCounter 的 int16 输入。 */
static int validation_to_int16(const int32_t value, int16_t *converted)
{
    /* 输出地址必须有效。 */
    if (converted == NULL) {
        /* 空输出地址属于参数错误。 */
        return 0;
    }
    /* 超出传感器 int16 原始量程时拒绝静默回绕。 */
    if ((value < INT16_MIN) || (value > INT16_MAX)) {
        /* 当前数据不能送入原算法。 */
        return 0;
    }
    /* 在已验证范围内执行窄化转换。 */
    *converted = (int16_t)value;
    /* 返回转换成功。 */
    return 1;
}

/* 读取七列参考数据；只提取 ax、ay、az 原始值，忽略前三列陀螺仪和最后时间戳。 */
static int validation_read_acceleration(
    const char *file_path,
    uint32_t *sample_count)
{
    /* 文件路径和样本数输出必须存在。 */
    if ((file_path == NULL) || (sample_count == NULL)) {
        /* 参数无效。 */
        return 0;
    }
    /* 以文本只读模式打开用户指定数据。 */
    FILE *input = fopen(file_path, "r");
    /* 文件不存在或无权限时拒绝继续。 */
    if (input == NULL) {
        /* 输出精确文件路径便于定位。 */
        fprintf(stderr, "无法打开数据文件：%s\n", file_path);
        /* 返回读取失败。 */
        return 0;
    }
    /* 保存一行七列文本；每列 int32 和分隔符远小于 256 字节。 */
    char line[256];
    /* 从零开始累计有效样本。 */
    *sample_count = 0U;
    /* 逐行读取到 EOF；每轮只接受恰好七个整数。 */
    while (fgets(line, sizeof(line), input) != NULL) {
        /* 前三列为 gx、gy、gz 原始值，本计数算法不读取。 */
        int32_t gyro_x = 0;
        /* 保存 gy 原始值，仅用于验证列数。 */
        int32_t gyro_y = 0;
        /* 保存 gz 原始值，仅用于验证列数。 */
        int32_t gyro_z = 0;
        /* 保存 ax 原始值，单位 raw。 */
        int32_t acc_x = 0;
        /* 保存 ay 原始值，单位 raw。 */
        int32_t acc_y = 0;
        /* 保存 az 原始值，单位 raw。 */
        int32_t acc_z = 0;
        /* 保存毫秒时间戳；原算法按固定 25 Hz 推进，不读取该列。 */
        int32_t timestamp_ms = 0;
        /* 按逗号分隔解析七列，末尾逗号允许被格式串后的空白忽略。 */
        const int field_count = sscanf(
            line,
            " %d , %d , %d , %d , %d , %d , %d",
            &gyro_x,
            &gyro_y,
            &gyro_z,
            &acc_x,
            &acc_y,
            &acc_z,
            &timestamp_ms);
        /* 非空数据行必须包含完整七列。 */
        if (field_count != 7) {
            /* 输出从 1 开始的失败行号。 */
            fprintf(stderr, "第 %u 行不是七列整数。\n", (unsigned)(*sample_count + 1U));
            /* 关闭已打开文件。 */
            (void)fclose(input);
            /* 返回格式失败。 */
            return 0;
        }
        /* 防止静态数组越界。 */
        if (*sample_count >= VALIDATION_MAX_SAMPLE_COUNT) {
            /* 输出固定容量。 */
            fprintf(stderr, "样本数超过固定容量 %u。\n", VALIDATION_MAX_SAMPLE_COUNT);
            /* 关闭文件。 */
            (void)fclose(input);
            /* 返回容量失败。 */
            return 0;
        }
        /* 把 ax 转为原算法 int16 输入。 */
        if (!validation_to_int16(acc_x, &validation_acc_x[*sample_count]) ||
            /* 把 ay 转为原算法 int16 输入。 */
            !validation_to_int16(acc_y, &validation_acc_y[*sample_count]) ||
            /* 把 az 转为原算法 int16 输入。 */
            !validation_to_int16(acc_z, &validation_acc_z[*sample_count])) {
            /* 报告发生量程错误的样本。 */
            fprintf(stderr, "第 %u 行加速度超出 int16 范围。\n", (unsigned)(*sample_count + 1U));
            /* 关闭文件。 */
            (void)fclose(input);
            /* 返回量程失败。 */
            return 0;
        }
        /* 显式消费本算法未使用的四列，避免编译器误判读取遗漏。 */
        (void)gyro_x;
        /* 标记 gy 已完成格式验证。 */
        (void)gyro_y;
        /* 标记 gz 已完成格式验证。 */
        (void)gyro_z;
        /* 标记时间戳已完成格式验证。 */
        (void)timestamp_ms;
        /* 当前行成功保存，累计样本数加一。 */
        *sample_count += 1U;
    }
    /* 文件读取完成后关闭句柄。 */
    if (fclose(input) != 0) {
        /* 关闭失败时不宣称数据完整。 */
        fprintf(stderr, "关闭数据文件失败。\n");
        /* 返回 I/O 失败。 */
        return 0;
    }
    /* 至少一个完整 25 点块才可运行原算法。 */
    if (*sample_count < VALIDATION_CHUNK_SAMPLE_COUNT) {
        /* 数据太短。 */
        fprintf(stderr, "样本不足 25 点。\n");
        /* 返回长度失败。 */
        return 0;
    }
    /* 数据读取成功。 */
    return 1;
}

/* 命令行：stepcounter_dataset_harness.exe <七列开合跳文件>。 */
int main(const int argument_count, char **argument_values)
{
    /* 必须且只能提供一个输入文件。 */
    if ((argument_count != 2) || (argument_values == NULL)) {
        /* 输出稳定用法。 */
        fprintf(stderr, "用法：stepcounter_dataset_harness.exe <数据文件>\n");
        /* 返回命令行错误。 */
        return EXIT_FAILURE;
    }
    /* 保存有效样本数。 */
    uint32_t sample_count = 0U;
    /* 读取第一个用户文件。 */
    if (!validation_read_acceleration(argument_values[1], &sample_count)) {
        /* 读取失败已输出原因。 */
        return EXIT_FAILURE;
    }
    /* 重置原算法全部滤波、缓冲和峰谷状态，保证每个文件相互独立。 */
    if (step_counter_init() != ALGO_NORMAL) {
        /* 初始化失败。 */
        fprintf(stderr, "StepCounter 初始化失败。\n");
        /* 返回算法错误。 */
        return EXIT_FAILURE;
    }
    /* 保存当前 1 秒块的原始峰计数。 */
    uint16_t chunk_period_count = 0U;
    /* 保存全文件原始峰周期累计；乘 2 前使用 32 位避免溢出。 */
    uint32_t raw_period_total = 0U;
    /* 保存原算法三轴输入视图；每轮指向静态数组连续 25 点。 */
    AccInput chunk = {0U, NULL, NULL, NULL};
    /* 每次严格推进 25 个点，尾部不足一秒的数据按原 main.c 合同忽略。 */
    for (uint32_t offset = 0U;
         (offset + VALIDATION_CHUNK_SAMPLE_COUNT) <= sample_count;
         offset += VALIDATION_CHUNK_SAMPLE_COUNT) {
        /* 当前块长度固定 25 点。 */
        chunk.len = VALIDATION_CHUNK_SAMPLE_COUNT;
        /* x 指针指向当前块 ax 起点，生命周期覆盖函数调用。 */
        chunk.x = &validation_acc_x[offset];
        /* y 指针指向当前块 ay 起点。 */
        chunk.y = &validation_acc_y[offset];
        /* z 指针指向当前块 az 起点。 */
        chunk.z = &validation_acc_z[offset];
        /* 原样运行 StepCounter，一次返回当前处理窗的三轴中位峰数。 */
        if (step_counter_process(&chunk, &chunk_period_count) != ALGO_NORMAL) {
            /* 输出失败点对应秒数。 */
            fprintf(stderr, "StepCounter 在第 %u 秒处理失败。\n", (unsigned)(offset / 25U));
            /* 返回算法错误。 */
            return EXIT_FAILURE;
        }
        /* 累加原算法峰周期。 */
        raw_period_total += chunk_period_count;
        /* 输出稳定逐秒累计，便于与图中窗口对应。 */
        printf(
            "HARNESS second=%u chunk_periods=%u raw_total=%u doubled_total=%u\n",
            (unsigned)(offset / 25U),
            (unsigned)chunk_period_count,
            (unsigned)raw_period_total,
            (unsigned)(raw_period_total * 2U));
    }
    /* 输出机器可解析的最终结果。 */
    printf(
        "C_RESULT file=%s samples=%u duration_s=%.2f raw_periods=%u doubled_repetitions=%u\n",
        argument_values[1],
        (unsigned)sample_count,
        (double)sample_count / 25.0,
        (unsigned)raw_period_total,
        (unsigned)(raw_period_total * 2U));
    /* 原始算法运行成功。 */
    return EXIT_SUCCESS;
}
