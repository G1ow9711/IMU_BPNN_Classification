#ifndef BOARD_SENSORS_TEST_STUB_FREERTOS_H
#define BOARD_SENSORS_TEST_STUB_FREERTOS_H

/* 引入定长 tick 类型。 */
#include <stdint.h>
/* 主机语法测试使用 32 位 tick。 */
typedef uint32_t TickType_t;
/* 语法测试假设 1 ms 对应 1 tick，不验证真实调度精度。 */
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))

#endif
