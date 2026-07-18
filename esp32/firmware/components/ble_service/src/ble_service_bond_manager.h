#ifndef BLE_SERVICE_BOND_MANAGER_H
#define BLE_SERVICE_BOND_MANAGER_H

/* 引入布尔值和固定宽度连接句柄，纯 C 编排器不依赖 NimBLE 头文件。 */
#include <stdbool.h>
#include <stdint.h>

/* 描述绑定删除编排结果；真实 NimBLE 适配器把非 OK 映射为 ESP_FAIL。 */
typedef enum ble_service_bond_forget_status {
    /* 当前连接已请求断开，全部持久化绑定记录也已删除。 */
    BLE_SERVICE_BOND_FORGET_OK = 0,
    /* clear_store 或已连接状态下的 terminate 回调为空。 */
    BLE_SERVICE_BOND_FORGET_ERR_ARGUMENT = -1,
    /* 主动断连失败，但仍已继续尝试清除持久化绑定。 */
    BLE_SERVICE_BOND_FORGET_ERR_TERMINATE = -2,
    /* NimBLE 官方绑定存储清除失败。 */
    BLE_SERVICE_BOND_FORGET_ERR_STORE = -3
} ble_service_bond_forget_status_t;

/* 主动断连回调；connection_handle 是当前唯一 PC 句柄，context 生命周期覆盖本次调用。 */
typedef int (*ble_service_bond_terminate_fn)(uint16_t connection_handle, void *context);
/* 清空绑定存储回调；真实实现调用 NimBLE 官方 ble_store_clear。 */
typedef int (*ble_service_bond_clear_store_fn)(void *context);

/* 保存可替换操作，使主机测试验证失败顺序而不链接 NimBLE/FreeRTOS。 */
typedef struct ble_service_bond_ops {
    /* terminate 请求断开当前 PC；无活动连接时不会调用。 */
    ble_service_bond_terminate_fn terminate;
    /* clear_store 删除 OUR_SEC、PEER_SEC、CCCD 等全部官方存储对象。 */
    ble_service_bond_clear_store_fn clear_store;
    /* context 原样传给两个回调；允许为空。 */
    void *context;
    /* success_code 表示底层成功，NimBLE 固定为 0。 */
    int success_code;
    /* not_connected_code 表示连接刚好已断开，可继续视为成功。 */
    int not_connected_code;
} ble_service_bond_ops_t;

/* 先请求活动连接断开，再无条件清空绑定；即使断连失败也尝试删除持久化密钥。 */
ble_service_bond_forget_status_t ble_service_bond_forget_all(
    const ble_service_bond_ops_t *ops,
    bool connection_active,
    uint16_t connection_handle);

#endif
