#ifndef BLE_SERVICE_NIMBLE_H
#define BLE_SERVICE_NIMBLE_H

// 引入纯 C BLE 业务核心，NimBLE 写回调直接复用幂等、LiveState 和分片合同。
#include "ble_service_core.h"

// 引入 ESP-IDF 统一错误码，公开启动、停止和发布函数均返回 esp_err_t。
#include "esp_err.h"

// 引入 bool，连接回调使用明确的连接/断连二值状态。
#include <stdbool.h>
// 引入 size_t 和固定宽度整数，明确所有字节缓冲区与计时字段宽度。
#include <stddef.h>
// 引入 uint8_t、uint16_t 和 uint32_t。
#include <stdint.h>

#ifdef __cplusplus
// C++ 组件使用 C 链接名调用设备端 BLE 服务。
extern "C" {
#endif

// Manifest TLV 最大 512 字节；更大静态资料应通过 Transfer Data 下载。
#define BLE_SERVICE_NIMBLE_MAX_MANIFEST_PAYLOAD ((size_t)512U)
// Device Information 字符串最大 31 字节并额外保留结尾 NUL。
#define BLE_SERVICE_NIMBLE_MAX_INFO_STRING ((size_t)31U)

// 描述生产 BLE 射频策略；数值与 power_manager 解耦，main 负责显式映射。
typedef enum ble_service_nimble_power_mode {
    // 关闭广播；若仍有连接则主动断开，适用于 Deep-sleep 或安全关机前。
    BLE_SERVICE_NIMBLE_POWER_OFF = 0,
    // 未连接时使用 100～150 ms 快速广播，缩短用户主动连接等待时间。
    BLE_SERVICE_NIMBLE_POWER_FAST_ADVERTISING,
    // 未连接时使用 1.0～1.2 s 慢广播，降低长时间待机射频占空比。
    BLE_SERVICE_NIMBLE_POWER_SLOW_ADVERTISING,
    // 已连接训练时请求 15～30 ms 连接间隔和零从机延迟，保证状态及时上传。
    BLE_SERVICE_NIMBLE_POWER_CONNECTED_ACTIVE,
    // 已连接待机时请求 50～100 ms 连接间隔及四个事件延迟，允许控制器 modem-sleep。
    BLE_SERVICE_NIMBLE_POWER_CONNECTED_MODEM_SLEEP
} ble_service_nimble_power_mode_t;

// 保存非事务性 BLE 运行快照；仅用于串口和上位机联调诊断，不参与安全授权判断。
typedef struct ble_service_nimble_runtime_status {
    // started 表示 NimBLE 端口、GATT 数据库和主机任务已创建。
    bool started;
    // host_synced 表示 identity 地址已准备且 GAP 允许开始广播。
    bool host_synced;
    // advertising 表示 NimBLE 当前报告存在活动广播过程。
    bool advertising;
    // physical_connection 表示存在物理 GAP 连接；它不代表安全握手已通过。
    bool physical_connection;
    // secure_connection 表示当前连接满足加密、MITM 和绑定三项业务安全门。
    bool secure_connection;
    // power_mode 是应用最近提交的射频模式，OFF 时保活不得越权启动广播。
    ble_service_nimble_power_mode_t power_mode;
} ble_service_nimble_runtime_status_t;

// 传输请求回调只处理已经通过逻辑帧 CRC 的消息类型 5 payload。
typedef ble_service_status_t (*ble_service_transfer_handler_fn)(
    const uint8_t *request_payload,
    uint16_t request_length,
    uint8_t *response_payload,
    size_t response_capacity,
    uint16_t *response_length,
    void *context);

// 配对码显示回调把随机六位码交给 LVGL；回调必须快速返回且不得阻塞 NimBLE 主机任务。
typedef void (*ble_service_passkey_display_fn)(uint32_t passkey, void *context);

// 描述 NimBLE 主机要求 UI 清除六位配对码的稳定原因；应用层可映射为中文日志。
typedef enum ble_service_pairing_clear_reason {
    // 加密、MITM 和绑定全部成功，配对码已失效。
    BLE_SERVICE_PAIRING_CLEAR_SUCCESS = 0,
    // 安全发起、交互注入或加密握手失败。
    BLE_SERVICE_PAIRING_CLEAR_FAILED,
    // 物理链路断开或 NimBLE 主机复位，本次配对终止。
    BLE_SERVICE_PAIRING_CLEAR_DISCONNECTED,
    // 用户在设备设置页执行“忘记电脑”。
    BLE_SERVICE_PAIRING_CLEAR_FORGOTTEN,
    // BLE 服务正在停止，屏幕不得保留敏感配对码。
    BLE_SERVICE_PAIRING_CLEAR_SERVICE_STOPPED
} ble_service_pairing_clear_reason_t;

// 配对码清除回调把成功、失败、断线、忘记或停服原因交给 UI；不得阻塞 NimBLE 任务。
typedef void (*ble_service_passkey_clear_fn)(
    ble_service_pairing_clear_reason_t reason,
    void *context);

// 连接状态回调把 PC 建连/断连事实送到应用任务；回调不得阻塞 NimBLE 主机任务。
typedef void (*ble_service_connection_changed_fn)(bool connected, uint16_t att_mtu, void *context);

// NimBLE 服务配置在 start 时复制字符串和 Manifest，回调与上下文指针需保持到 stop 返回。
typedef struct ble_service_nimble_config {
    // device_name 是广播和 GAP 名称，例如 BPNN-FIT-A1B2，最长 31 字节 UTF-8。
    const char *device_name;
    // manufacturer_name 写入标准 Device Information 0x2A29。
    const char *manufacturer_name;
    // model_number 写入标准 Device Information 0x2A24。
    const char *model_number;
    // serial_number 写入标准 Device Information 0x2A25。
    const char *serial_number;
    // hardware_revision 写入标准 Device Information 0x2A27。
    const char *hardware_revision;
    // firmware_revision 写入标准 Device Information 0x2A26。
    const char *firmware_revision;
    // manifest_payload 指向协议、模型、类别和能力 TLV；start 返回后可以释放原缓冲区。
    const uint8_t *manifest_payload;
    // manifest_length 取值 0～512 字节。
    uint16_t manifest_length;
    // initial_battery_percent 取值 0～100，或 255 表示 AXP2101 尚无有效电量。
    uint8_t initial_battery_percent;
    // command_handler 执行控制命令；重复请求由核心缓存拦截，不会二次调用。
    ble_service_command_handler_fn command_handler;
    // command_context 原样传给 command_handler，生命周期覆盖服务运行期。
    void *command_context;
    // transfer_handler 处理特征 0005 的 TransferRequest；允许为空以关闭会话下载。
    ble_service_transfer_handler_fn transfer_handler;
    // transfer_context 原样传给 transfer_handler。
    void *transfer_context;
    // passkey_display 把 LE Secure Connections 六位码显示到设备屏幕；允许为空并仅记录日志。
    ble_service_passkey_display_fn passkey_display;
    // passkey_clear 在成功、失败、断线、忘记或停服时清除屏幕六位码；允许为空。
    ble_service_passkey_clear_fn passkey_clear;
    // passkey_context 原样传给 passkey_display 和 passkey_clear，生命周期覆盖服务运行期。
    void *passkey_context;
    // connection_changed 只在加密、MITM 认证及绑定全部成功后报告 true，断连或停止时成对报告 false；允许为空。
    ble_service_connection_changed_fn connection_changed;
    // connection_context 原样传给 connection_changed，生命周期覆盖服务运行期。
    void *connection_context;
} ble_service_nimble_config_t;

// 初始化 ESP-NimBLE、注册自定义 0001～0007 与标准 0x180F/0x180A 服务并开始广播。
esp_err_t ble_service_nimble_start(const ble_service_nimble_config_t *config);

// 停止广播、断开当前 PC、停止 NimBLE 主机并释放控制器资源。
esp_err_t ble_service_nimble_stop(void);

// 断开当前 PC 并调用 NimBLE 官方 ble_store_clear 删除全部持久绑定；成功后可配对新电脑。
esp_err_t ble_service_nimble_forget_all_bonds(void);

// 返回当前是否存在加密、MITM 认证且已绑定的唯一 PC 业务连接；物理链路握手中返回 false。
bool ble_service_nimble_is_connected(void);

// 返回控制或会话响应是否仍在等待 indication 确认；TransferData 必须等本函数为 false 后再发布。
bool ble_service_nimble_reliable_response_pending(void);

// 读取启动、同步、广播、连接和射频模式快照；status 不能为空，返回值不表示设备已经连接。
esp_err_t ble_service_nimble_get_runtime_status(ble_service_nimble_runtime_status_t *status);

// 应用广播或连接省电参数；调用线程不得持有 BLE 业务 broker 锁，失败返回 ESP_FAIL。
esp_err_t ble_service_nimble_set_power_mode(ble_service_nimble_power_mode_t mode);

// 联调期确认未连接设备仍在广播；仅在服务已启动、主机已同步且功耗模式非 OFF 时补启广播。
esp_err_t ble_service_nimble_ensure_advertising(void);

// 更新标准 Battery Level 和 LiveState 使用的电量源；取值仅允许 0～100 或 255 未知。
esp_err_t ble_service_nimble_set_battery_percent(uint8_t battery_percent);

// 编码并发布 LiveStateV1；Read 返回最新完整逻辑帧，Notify 使用 MTU 自适应分片包络。
esp_err_t ble_service_nimble_publish_live_state(const ble_service_live_state_v1_t *state);

// 发布 Event payload；事件只触发动画或提示，PC 不用它自行增加权威计数。
esp_err_t ble_service_nimble_publish_event(const uint8_t *payload, uint16_t payload_length);

// 发布会话摘要或日志数据块；payload 最大 1024 字节并由逻辑帧 CRC 保护。
esp_err_t ble_service_nimble_publish_transfer_data(const uint8_t *payload, uint16_t payload_length);

// 发布开发者原始六轴流；默认不订阅且不重传丢包。
esp_err_t ble_service_nimble_publish_raw_stream(const uint8_t *payload, uint16_t payload_length);

#ifdef __cplusplus
// 结束 C 链接约定。
}
#endif

#endif
