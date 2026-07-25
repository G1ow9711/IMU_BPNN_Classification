#ifndef BOARD_SENSORS_TEST_STUB_FREERTOS_TASK_H
#define BOARD_SENSORS_TEST_STUB_FREERTOS_TASK_H

/* 引入 TickType_t。 */
#include "freertos/FreeRTOS.h"
/* 声明任务延时；语法测试不链接实现。 */
void vTaskDelay(TickType_t ticks);

#endif
