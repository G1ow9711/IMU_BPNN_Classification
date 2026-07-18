#ifndef BOARD_SENSORS_TEST_STUB_ESP_ERR_H
#define BOARD_SENSORS_TEST_STUB_ESP_ERR_H

/* 主机语法测试把 ESP-IDF 错误码简化为有符号整数。 */
typedef int esp_err_t;
/* ESP-IDF 成功码固定为零。 */
#define ESP_OK (0)
/* ESP-IDF 参数错误仅需与成功码不同。 */
#define ESP_ERR_INVALID_ARG (-1)

#endif
