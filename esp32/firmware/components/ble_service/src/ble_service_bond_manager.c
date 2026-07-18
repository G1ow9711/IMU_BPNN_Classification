// 引入可注入的绑定删除编排合同。
#include "ble_service_bond_manager.h"

// 引入 NULL，参数校验不依赖 ESP-IDF 或 NimBLE 头文件间接定义。
#include <stddef.h>

// 断开当前连接并删除全部绑定；本函数不依赖 ESP-IDF，可在主机测试中穷尽错误路径。
ble_service_bond_forget_status_t ble_service_bond_forget_all(
    const ble_service_bond_ops_t *ops,
    bool connection_active,
    uint16_t connection_handle)
{
    // 清空存储回调始终必填；有连接时还必须提供断开回调。
    if ((ops == NULL) ||
        (ops->clear_store == NULL) ||
        (connection_active && (ops->terminate == NULL))) {
        // 返回参数错误，未执行任何部分操作。
        return BLE_SERVICE_BOND_FORGET_ERR_ARGUMENT;
    }

    // 初始终止状态为成功；无连接时不调用底层 GAP。
    int terminate_status = ops->success_code;
    // 当前物理链路存在时先请求终止，防止已连接 PC 继续使用将被删除的密钥。
    if (connection_active) {
        // 把当前唯一连接句柄传给 NimBLE 适配回调。
        terminate_status = ops->terminate(connection_handle, ops->context);
    }

    // 不论断开是否成功，都尝试删除全部持久绑定，避免只断线却没有“忘记”。
    const int clear_status = ops->clear_store(ops->context);
    // 存储清空失败优先上报，因为旧 PC 密钥仍可能残留在 NVS。
    if (clear_status != ops->success_code) {
        // 返回存储错误。
        return BLE_SERVICE_BOND_FORGET_ERR_STORE;
    }

    // 断开成功或连接已在竞态中消失都是可接受结果。
    if ((terminate_status != ops->success_code) &&
        (terminate_status != ops->not_connected_code)) {
        // 绑定已清空，但断开请求失败，返回独立状态供设备日志告警。
        return BLE_SERVICE_BOND_FORGET_ERR_TERMINATE;
    }

    // 断开与存储清空全部完成。
    return BLE_SERVICE_BOND_FORGET_OK;
}
