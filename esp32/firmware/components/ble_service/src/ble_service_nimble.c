// 引入 ESP-NimBLE 设备服务公开接口。
#include "ble_service_nimble.h"
// 引入可主机测试的忘记电脑编排器，NimBLE 适配层只提供断开和存储回调。
#include "ble_service_bond_manager.h"

// 引入 ESP-IDF 日志接口，记录连接、订阅、协议错误和 indication 状态。
#include "esp_log.h"
// 引入单调微秒时钟，逻辑帧时间戳转换为 32 位毫秒并允许自然回绕。
#include "esp_timer.h"

// 引入 NimBLE ATT MTU 查询与 ATT 错误码。
#include "host/ble_att.h"
// 引入 NimBLE GAP、GATT、安全和主机配置接口。
#include "host/ble_hs.h"
// 引入 NimBLE 官方 ble_store_clear，删除全部安全密钥、CCCD 和对端地址。
#include "host/ble_store.h"
// 引入 UUID16/UUID128 声明宏。
#include "host/ble_uuid.h"
// 引入地址确保和绑定存储轮转回调。
#include "host/util/util.h"
// 引入 NimBLE 端口初始化、运行、停止和反初始化接口。
#include "nimble/nimble_port.h"
// 引入 ESP-IDF FreeRTOS NimBLE 主机任务适配。
#include "nimble/nimble_port_freertos.h"
// 引入标准 GAP 服务与设备名接口。
#include "services/gap/ble_svc_gap.h"
// 引入 Service Changed 标准 GATT 服务。
#include "services/gatt/ble_svc_gatt.h"

// 引入 OS_MBUF_PKTLEN、os_mbuf_append 和 mbuf 数据结构。
#include "os/os_mbuf.h"

// 引入 FreeRTOS 基础类型和毫秒换算，BLE 业务工作队列不得阻塞 NimBLE 主机任务。
#include "freertos/FreeRTOS.h"
// 引入固定容量工作队列，GATT 回调只复制分片后立即返回。
#include "freertos/queue.h"
// 引入独立 BLE 业务任务、短延时和任务退出接口。
#include "freertos/task.h"

// 引入 snprintf 和字符串长度函数。
#include <stdio.h>
// 引入 memcpy、memset、strlen 和逐字节字符串复制。
#include <string.h>

// 绑定存储初始化由 ESP-NimBLE 提供，官方 bleprph 示例同样使用外部声明。
void ble_store_config_init(void);

// 日志标签固定为 ble_service，便于设备诊断页过滤模块日志。
static const char *const BLE_SERVICE_LOG_TAG = "ble_service";

// 默认 ATT MTU 为 23；协商更新前单个 Value 最多 20 字节。
#define BLE_SERVICE_DEFAULT_ATT_MTU UINT16_C(23)
// 单个分片最大按完整协议帧加 8 字节包络预留，防止异常大 MTU 越界。
#define BLE_SERVICE_MAX_GATT_VALUE_SIZE (IMU_BLE_MAX_FRAME_SIZE + IMU_BLE_FRAGMENT_HEADER_SIZE)
// 逻辑帧序号位于固定头偏移 6，NimBLE 分片包络复用该数值。
#define BLE_SERVICE_FRAME_SEQUENCE_OFFSET ((size_t)6U)
// 最小有效 GATT 分片必须至少含 8 字节包络。
#define BLE_SERVICE_MIN_FRAGMENT_SIZE IMU_BLE_FRAGMENT_HEADER_SIZE
// 控制和传输响应最多等待一个 indication，新的响应由 PC 使用同 request_id 重试恢复。
#define BLE_SERVICE_SINGLE_INDICATION_QUEUE UINT8_C(1)
// BLE 业务队列最多缓存 6 个 ATT 分片；协议要求客户端等待响应后再发下一事务。
#define BLE_SERVICE_WORK_QUEUE_LENGTH UINT32_C(6)
// ESP-IDF 的 xTaskCreate 栈深单位为字节；16 KiB 覆盖首次绑定后的能力清单同步突发和最大控制帧调用链。
#define BLE_SERVICE_WORKER_STACK_BYTES (16U * 1024U)
// 禁止编译器把控制与传输路径的最大帧缓冲内联到常驻 worker 栈帧；真板曾因 6 KiB 合并帧触发栈溢出。
#define BLE_SERVICE_STACK_BOUNDARY __attribute__((noinline))
// BLE 业务任务优先级低于 NimBLE 主机和 IMU 采样，高于后台存储。
#define BLE_SERVICE_WORKER_PRIORITY ((UBaseType_t)5U)
// 工作任务每 20 ms 检查停止和连接代次，停止流程不会无限等待。
#define BLE_SERVICE_WORKER_POLL_MS UINT32_C(20)
// BLE 广播间隔单位为 0.625 ms；160 对应 100 ms。
#define BLE_SERVICE_FAST_ADV_INTERVAL_MIN UINT16_C(160)
// 快广播最大 240 单位对应 150 ms，避免持续占用原厂 400 mAh 电池。
#define BLE_SERVICE_FAST_ADV_INTERVAL_MAX UINT16_C(240)
// 慢广播最小 1600 单位对应 1.0 s。
#define BLE_SERVICE_SLOW_ADV_INTERVAL_MIN UINT16_C(1600)
// 慢广播最大 1920 单位对应 1.2 s。
#define BLE_SERVICE_SLOW_ADV_INTERVAL_MAX UINT16_C(1920)
// 活动连接最小 12 单位对应 15 ms，单位由 BLE 规范固定为 1.25 ms。
#define BLE_SERVICE_ACTIVE_CONN_INTERVAL_MIN UINT16_C(12)
// 活动连接最大 24 单位对应 30 ms。
#define BLE_SERVICE_ACTIVE_CONN_INTERVAL_MAX UINT16_C(24)
// 待机连接最小 40 单位对应 50 ms。
#define BLE_SERVICE_STANDBY_CONN_INTERVAL_MIN UINT16_C(40)
// 待机连接最大 80 单位对应 100 ms。
#define BLE_SERVICE_STANDBY_CONN_INTERVAL_MAX UINT16_C(80)
// 活动连接不跳过任何连接事件。
#define BLE_SERVICE_ACTIVE_CONN_LATENCY UINT16_C(0)
// 待机连接最多跳过四个事件，PC 控制最坏交互延迟仍低于约 0.5 s。
#define BLE_SERVICE_STANDBY_CONN_LATENCY UINT16_C(4)
// 活动连接监督超时 4 s，单位为 10 ms。
#define BLE_SERVICE_ACTIVE_SUPERVISION_TIMEOUT UINT16_C(400)
// 待机连接监督超时 6 s，覆盖长间隔和从机延迟后仍满足 BLE 约束。
#define BLE_SERVICE_STANDBY_SUPERVISION_TIMEOUT UINT16_C(600)
// 现场联调固定 PIN 为 123456；仍保留 MITM、LE Secure Connections 和绑定，便于 Windows 手工输入并复用绑定。
#define BLE_SERVICE_PAIRING_PASSKEY UINT32_C(123456)

// 固定自定义 UUID 的小端字节；suffix_low 对应字符串 7B2E00xx 中的最低字节。
#define BLE_SERVICE_UUID128_INIT(suffix_low) \
    BLE_UUID128_INIT(0x4e, 0x50, 0x42, 0x55, 0x4d, 0x49, 0x43, 0x9e, \
                     0x51, 0x4a, 0x57, 0x6d, (suffix_low), 0x00, 0x2e, 0x7b)

// 自定义服务 UUID 为 7B2E0000-6D57-4A51-9E43-494D5542504E。
static const ble_uuid128_t g_fitness_service_uuid = BLE_SERVICE_UUID128_INIT(0x00);
// Control Point UUID 尾号为 0001。
static const ble_uuid128_t g_control_point_uuid = BLE_SERVICE_UUID128_INIT(0x01);
// Manifest UUID 尾号为 0002。
static const ble_uuid128_t g_manifest_uuid = BLE_SERVICE_UUID128_INIT(0x02);
// Live State UUID 尾号为 0003。
static const ble_uuid128_t g_live_state_uuid = BLE_SERVICE_UUID128_INIT(0x03);
// Event UUID 尾号为 0004。
static const ble_uuid128_t g_event_uuid = BLE_SERVICE_UUID128_INIT(0x04);
// Transfer Control UUID 尾号为 0005。
static const ble_uuid128_t g_transfer_control_uuid = BLE_SERVICE_UUID128_INIT(0x05);
// Transfer Data UUID 尾号为 0006。
static const ble_uuid128_t g_transfer_data_uuid = BLE_SERVICE_UUID128_INIT(0x06);
// Raw Stream UUID 尾号为 0007。
static const ble_uuid128_t g_raw_stream_uuid = BLE_SERVICE_UUID128_INIT(0x07);

// GATT access 回调使用稳定标签区分七个自定义特征，不进行整数到指针强转。
typedef enum ble_service_characteristic_tag {
    // 控制点写入 ControlRequest，响应走 indication。
    BLE_SERVICE_TAG_CONTROL_POINT = 1,
    // Manifest 只读完整逻辑帧。
    BLE_SERVICE_TAG_MANIFEST = 2,
    // Live State 可读并可订阅 notification。
    BLE_SERVICE_TAG_LIVE_STATE = 3,
    // Event 只支持 notification。
    BLE_SERVICE_TAG_EVENT = 4,
    // Transfer Control 写入请求，响应走 indication。
    BLE_SERVICE_TAG_TRANSFER_CONTROL = 5,
    // Transfer Data 只支持 notification。
    BLE_SERVICE_TAG_TRANSFER_DATA = 6,
    // Raw Stream 只支持 notification。
    BLE_SERVICE_TAG_RAW_STREAM = 7,
    // Battery Level 属于标准 0x180F 服务。
    BLE_SERVICE_TAG_BATTERY = 8,
    // Device Information 厂商字符串。
    BLE_SERVICE_TAG_MANUFACTURER = 9,
    // Device Information 型号字符串。
    BLE_SERVICE_TAG_MODEL = 10,
    // Device Information 序列号字符串。
    BLE_SERVICE_TAG_SERIAL = 11,
    // Device Information 硬件版本字符串。
    BLE_SERVICE_TAG_HARDWARE_REVISION = 12,
    // Device Information 固件版本字符串。
    BLE_SERVICE_TAG_FIRMWARE_REVISION = 13
} ble_service_characteristic_tag_t;

// 区分进入独立业务任务的控制点与会话传输分片。
typedef enum ble_service_work_kind {
    // Control Point 0001 分片，完整后调用命令处理器并返回 ControlResponse indication。
    BLE_SERVICE_WORK_CONTROL = 0,
    // Transfer Control 0005 分片，完整后冻结会话页并返回 TransferResponse indication。
    BLE_SERVICE_WORK_TRANSFER
} ble_service_work_kind_t;

// 保存一条从 NimBLE GATT 回调复制出的稳定工作项，回调返回后不再借用 mbuf。
typedef struct ble_service_work_item {
    // kind 指明 fragment 应进入控制重组器还是传输重组器。
    ble_service_work_kind_t kind;
    // connection_handle 是接收分片时的物理链路句柄，用于丢弃断线后的陈旧工作。
    uint16_t connection_handle;
    // connection_epoch 是每次连接或断连递增的代次，防止句柄回绕后误收旧分片。
    uint32_t connection_epoch;
    // fragment_length 表示 fragment 中有效字节数，范围 8～BLE_SERVICE_MAX_GATT_VALUE_SIZE。
    uint16_t fragment_length;
    // fragment 深拷贝一个完整 ATT Value，包含 8 字节协议分片包络和连续帧数据。
    uint8_t fragment[BLE_SERVICE_MAX_GATT_VALUE_SIZE];
} ble_service_work_item_t;

// 每个 characteristic arg 指向独立静态标签，生命周期覆盖整个 GATT 数据库。
static const ble_service_characteristic_tag_t g_tag_control_point = BLE_SERVICE_TAG_CONTROL_POINT;
// Manifest 标签对象。
static const ble_service_characteristic_tag_t g_tag_manifest = BLE_SERVICE_TAG_MANIFEST;
// Live State 标签对象。
static const ble_service_characteristic_tag_t g_tag_live_state = BLE_SERVICE_TAG_LIVE_STATE;
// Event 标签对象。
static const ble_service_characteristic_tag_t g_tag_event = BLE_SERVICE_TAG_EVENT;
// Transfer Control 标签对象。
static const ble_service_characteristic_tag_t g_tag_transfer_control = BLE_SERVICE_TAG_TRANSFER_CONTROL;
// Transfer Data 标签对象。
static const ble_service_characteristic_tag_t g_tag_transfer_data = BLE_SERVICE_TAG_TRANSFER_DATA;
// Raw Stream 标签对象。
static const ble_service_characteristic_tag_t g_tag_raw_stream = BLE_SERVICE_TAG_RAW_STREAM;
// Battery Level 标签对象。
static const ble_service_characteristic_tag_t g_tag_battery = BLE_SERVICE_TAG_BATTERY;
// 厂商字符串标签对象。
static const ble_service_characteristic_tag_t g_tag_manufacturer = BLE_SERVICE_TAG_MANUFACTURER;
// 型号字符串标签对象。
static const ble_service_characteristic_tag_t g_tag_model = BLE_SERVICE_TAG_MODEL;
// 序列号字符串标签对象。
static const ble_service_characteristic_tag_t g_tag_serial = BLE_SERVICE_TAG_SERIAL;
// 硬件版本字符串标签对象。
static const ble_service_characteristic_tag_t g_tag_hardware_revision = BLE_SERVICE_TAG_HARDWARE_REVISION;
// 固件版本字符串标签对象。
static const ble_service_characteristic_tag_t g_tag_firmware_revision = BLE_SERVICE_TAG_FIRMWARE_REVISION;

// indication_pending 保存一帧可靠响应及下一待发分片；最多一个 PC，因此只需一个队列槽。
typedef struct ble_service_pending_indication {
    // active 为 1 表示正在等待上一分片确认，为 0 表示可以开始新 indication。
    uint8_t active;
    // value_handle 是 Control Point 或 Transfer Control 的特征值句柄。
    uint16_t value_handle;
    // att_mtu 是开始发送时的协商 MTU，同一逻辑帧中途不改变切片方式。
    uint16_t att_mtu;
    // logical_sequence 同时写在每片 8 字节包络中。
    uint16_t logical_sequence;
    // fragment_count 是完整帧在 att_mtu 下的总片数。
    uint16_t fragment_count;
    // next_fragment_index 是下一次发送函数要编码的分片索引。
    uint16_t next_fragment_index;
    // frame_length 表示 frame 中有效的完整逻辑帧字节数。
    size_t frame_length;
    // frame 固定保存最大 1040 字节逻辑帧，生命周期覆盖全部 indication 确认。
    uint8_t frame[IMU_BLE_MAX_FRAME_SIZE];
} ble_service_pending_indication_t;

// 单例运行状态符合 sdkconfig 的最大一连接约束，避免为不存在的第二连接分配约 10 KiB 缓存。
typedef struct ble_service_nimble_state {
    // started 为 1 表示 NimBLE 已初始化且 GATT 数据库已注册。
    uint8_t started;
    // host_synced 为 1 表示 NimBLE 已完成 identity 地址准备，允许安全调用广播 API。
    volatile uint8_t host_synced;
    // own_address_type 是广播使用的 public 或 random identity 类型。
    uint8_t own_address_type;
    // power_mode 保存主应用最近要求的广播或连接参数，断连后据此选择快慢广播。
    ble_service_nimble_power_mode_t power_mode;
    // connection_handle 是当前 PC 连接句柄，无连接时为 BLE_HS_CONN_HANDLE_NONE。
    uint16_t connection_handle;
    // secure_connection 为 1 表示当前链路同时满足加密、MITM 认证和绑定，敏感业务才可读写或通知。
    uint8_t secure_connection;
    // connection_reported 为 1 表示应用层已收到一次安全连接 true，断连时必须成对发送 false。
    uint8_t connection_reported;
    // pairing_code_active 为 1 表示已向 UI 发布六位码，所有终止路径都必须发布成对清除。
    uint8_t pairing_code_active;
    // connection_epoch 在连接、断连和主机复位时递增；工作任务只处理当前代次。
    volatile uint32_t connection_epoch;
    // att_mtu 保存当前连接协商值，建连时从 23 开始。
    uint16_t att_mtu;
    // connection_core 保存两个重组器和最近 16 项控制请求缓存。
    ble_service_connection_t connection_core;
    // configuration 保存回调与上下文指针；字符串和 Manifest 复制到本状态其它字段。
    ble_service_nimble_config_t configuration;
    // device_name 是广播和标准 GAP 名称，最多 31 字节加 NUL。
    char device_name[BLE_SERVICE_NIMBLE_MAX_INFO_STRING + 1U];
    // manufacturer_name 是标准 0x2A29 字符串。
    char manufacturer_name[BLE_SERVICE_NIMBLE_MAX_INFO_STRING + 1U];
    // model_number 是标准 0x2A24 字符串。
    char model_number[BLE_SERVICE_NIMBLE_MAX_INFO_STRING + 1U];
    // serial_number 是标准 0x2A25 字符串。
    char serial_number[BLE_SERVICE_NIMBLE_MAX_INFO_STRING + 1U];
    // hardware_revision 是标准 0x2A27 字符串。
    char hardware_revision[BLE_SERVICE_NIMBLE_MAX_INFO_STRING + 1U];
    // firmware_revision 是标准 0x2A26 字符串。
    char firmware_revision[BLE_SERVICE_NIMBLE_MAX_INFO_STRING + 1U];
    // manifest_frame 保存可通过 0002 Read 获取的原始 Manifest TLV；Manifest 不占用消息类型 1～8。
    uint8_t manifest_frame[IMU_BLE_MAX_FRAME_SIZE];
    // manifest_frame_length 表示 Manifest TLV 有效长度。
    size_t manifest_frame_length;
    // live_state_frame 保存最近一次发布的完整 LiveState 逻辑帧，供 Read 快照恢复。
    uint8_t live_state_frame[IMU_BLE_MAX_FRAME_SIZE];
    // live_state_frame_length 为零表示尚未发布状态。
    size_t live_state_frame_length;
    // battery_percent 保存标准 Battery Level，255 时 Read 返回 255 表示未知。
    uint8_t battery_percent;
    // sequence_by_message 为消息 1～8 分别保存下一个发送序号，索引 0 不使用。
    uint16_t sequence_by_message[9];
    // subscribed_live 为 1 表示 PC 已订阅 Live State notification。
    uint8_t subscribed_live;
    // subscribed_event 为 1 表示 PC 已订阅 Event notification。
    uint8_t subscribed_event;
    // subscribed_transfer_data 为 1 表示 PC 已订阅 Transfer Data notification。
    uint8_t subscribed_transfer_data;
    // subscribed_raw 为 1 表示开发者明确订阅 Raw Stream notification。
    uint8_t subscribed_raw;
    // subscribed_battery 为 1 表示 PC 订阅标准 Battery Level notification。
    uint8_t subscribed_battery;
    // pending_indication 串行发送 Control/Transfer 的可靠响应分片。
    ble_service_pending_indication_t pending_indication;
    // work_queue 保存 GATT 回调复制的控制/传输分片，队列元素不借用 NimBLE mbuf。
    QueueHandle_t work_queue;
    // worker_task 保存独立业务任务句柄；停止完成后由任务自身清空。
    volatile TaskHandle_t worker_task;
    // worker_stop_requested 为 1 时业务任务不再执行新请求并安全退出。
    volatile uint8_t worker_stop_requested;
} ble_service_nimble_state_t;

// 全局单例放在 BSS；约 14 KiB 主体来自精确幂等缓存和两个 1040 字节重组器。
static ble_service_nimble_state_t g_ble_state;

// 七个自定义特征的 value handle 由 ble_gatts_add_svcs 注册时写入。
static uint16_t g_control_point_handle;
// Manifest value handle。
static uint16_t g_manifest_handle;
// Live State value handle。
static uint16_t g_live_state_handle;
// Event value handle。
static uint16_t g_event_handle;
// Transfer Control value handle。
static uint16_t g_transfer_control_handle;
// Transfer Data value handle。
static uint16_t g_transfer_data_handle;
// Raw Stream value handle。
static uint16_t g_raw_stream_handle;
// 标准 Battery Level value handle。
static uint16_t g_battery_handle;

// 前向声明 GATT 读写回调。
static int ble_service_gatt_access(
    uint16_t connection_handle,
    uint16_t attribute_handle,
    struct ble_gatt_access_ctxt *context,
    void *argument);
// 前向声明 GAP 连接、订阅、安全和 indication 确认回调。
static int ble_service_gap_event(struct ble_gap_event *event, void *argument);
// 前向声明广播函数，主机同步和断连后复用。
static void ble_service_start_advertising(void);
// 前向声明独立 BLE 业务任务；它可以等待应用 broker，但 NimBLE 主机回调不等待。
static void ble_service_worker_task(void *argument);

// GATT 数据库同时注册自定义服务、Battery 0x180F 和 Device Information 0x180A。
static const struct ble_gatt_svc_def g_ble_services[] = {
    {
        // 第一项为健身手柄自定义主服务。
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        // 使用 128 位服务 UUID 尾号 0000。
        .uuid = &g_fitness_service_uuid.u,
        // 七个特征按协议尾号顺序注册，末尾全零项终止数组。
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // Control Point 使用尾号 0001。
                .uuid = &g_control_point_uuid.u,
                // 所有读写通过统一回调并由静态标签区分。
                .access_cb = ble_service_gatt_access,
                // 参数指向控制点标签。
                .arg = (void *)&g_tag_control_point,
                // 写入必须加密且经过 MITM 认证；响应使用 indication，防止命令结果静默丢失。
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                    BLE_GATT_CHR_F_WRITE_AUTHEN | BLE_GATT_CHR_F_INDICATE,
                // 注册完成后写入 value handle。
                .val_handle = &g_control_point_handle
            },
            {
                // Manifest 使用尾号 0002。
                .uuid = &g_manifest_uuid.u,
                // Read 回调返回完整逻辑帧。
                .access_cb = ble_service_gatt_access,
                // 参数指向 Manifest 标签。
                .arg = (void *)&g_tag_manifest,
                // 未绑定客户端允许读取版本和兼容性清单。
                .flags = BLE_GATT_CHR_F_READ,
                // 保存值句柄供诊断使用。
                .val_handle = &g_manifest_handle
            },
            {
                // Live State 使用尾号 0003。
                .uuid = &g_live_state_uuid.u,
                // Read 返回最新完整逻辑帧，Notify 发布分片包络。
                .access_cb = ble_service_gatt_access,
                // 参数指向实时状态标签。
                .arg = (void *)&g_tag_live_state,
                // 快照读取必须加密且经过 MITM 认证；通知路径另在发布函数检查安全状态。
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                    BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_NOTIFY,
                // 保存值句柄供定向通知。
                .val_handle = &g_live_state_handle
            },
            {
                // Event 使用尾号 0004。
                .uuid = &g_event_uuid.u,
                // 该特征不支持主动 Read/Write，但保留回调满足 NimBLE 定义结构。
                .access_cb = ble_service_gatt_access,
                // 参数指向事件标签。
                .arg = (void *)&g_tag_event,
                // 事件只使用 notification。
                .flags = BLE_GATT_CHR_F_NOTIFY,
                // 保存值句柄。
                .val_handle = &g_event_handle
            },
            {
                // Transfer Control 使用尾号 0005。
                .uuid = &g_transfer_control_uuid.u,
                // 写入分片请求，响应使用 indication。
                .access_cb = ble_service_gatt_access,
                // 参数指向传输控制标签。
                .arg = (void *)&g_tag_transfer_control,
                // 会话数据请求必须加密且经过 MITM 认证；绑定状态在访问回调再次核验。
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                    BLE_GATT_CHR_F_WRITE_AUTHEN | BLE_GATT_CHR_F_INDICATE,
                // 保存值句柄。
                .val_handle = &g_transfer_control_handle
            },
            {
                // Transfer Data 使用尾号 0006。
                .uuid = &g_transfer_data_uuid.u,
                // 只用于 notification。
                .access_cb = ble_service_gatt_access,
                // 参数指向传输数据标签。
                .arg = (void *)&g_tag_transfer_data,
                // 数据块采用 notification，文件层使用偏移和 CRC32 续传。
                .flags = BLE_GATT_CHR_F_NOTIFY,
                // 保存值句柄。
                .val_handle = &g_transfer_data_handle
            },
            {
                // Raw Stream 使用尾号 0007。
                .uuid = &g_raw_stream_uuid.u,
                // 只用于 notification。
                .access_cb = ble_service_gatt_access,
                // 参数指向原始流标签。
                .arg = (void *)&g_tag_raw_stream,
                // 原始流尽力而为，不使用可靠 indication。
                .flags = BLE_GATT_CHR_F_NOTIFY,
                // 保存值句柄。
                .val_handle = &g_raw_stream_handle
            },
            {
                // 全零项终止自定义 characteristic 数组。
                0
            }
        }
    },
    {
        // 第二项注册标准 Battery Service 0x180F。
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        // 使用标准 16 位 UUID。
        .uuid = BLE_UUID16_DECLARE(0x180F),
        // Battery Service 只包含 Battery Level 0x2A19。
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // Battery Level 标准特征 UUID。
                .uuid = BLE_UUID16_DECLARE(0x2A19),
                // Read 返回单字节百分比，Notify 在值变化时发布。
                .access_cb = ble_service_gatt_access,
                // 参数指向 Battery 标签。
                .arg = (void *)&g_tag_battery,
                // 允许读取和订阅通知。
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                // 保存值句柄。
                .val_handle = &g_battery_handle
            },
            {
                // 全零项终止 Battery characteristic 数组。
                0
            }
        }
    },
    {
        // 第三项注册标准 Device Information Service 0x180A。
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        // 使用标准 16 位 UUID。
        .uuid = BLE_UUID16_DECLARE(0x180A),
        // 五个只读字符串帮助 PC 诊断固件与板型。
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // Manufacturer Name String 0x2A29。
                .uuid = BLE_UUID16_DECLARE(0x2A29),
                // 统一字符串 Read 回调。
                .access_cb = ble_service_gatt_access,
                // 参数指向厂商标签。
                .arg = (void *)&g_tag_manufacturer,
                // 允许公开读取设备厂商。
                .flags = BLE_GATT_CHR_F_READ
            },
            {
                // Model Number String 0x2A24。
                .uuid = BLE_UUID16_DECLARE(0x2A24),
                // 统一字符串 Read 回调。
                .access_cb = ble_service_gatt_access,
                // 参数指向型号标签。
                .arg = (void *)&g_tag_model,
                // 允许公开读取板卡型号。
                .flags = BLE_GATT_CHR_F_READ
            },
            {
                // Serial Number String 0x2A25。
                .uuid = BLE_UUID16_DECLARE(0x2A25),
                // 统一字符串 Read 回调。
                .access_cb = ble_service_gatt_access,
                // 参数指向序列号标签。
                .arg = (void *)&g_tag_serial,
                // 允许公开读取设备序列号。
                .flags = BLE_GATT_CHR_F_READ
            },
            {
                // Hardware Revision String 0x2A27。
                .uuid = BLE_UUID16_DECLARE(0x2A27),
                // 统一字符串 Read 回调。
                .access_cb = ble_service_gatt_access,
                // 参数指向硬件版本标签。
                .arg = (void *)&g_tag_hardware_revision,
                // 允许公开读取板卡修订号。
                .flags = BLE_GATT_CHR_F_READ
            },
            {
                // Firmware Revision String 0x2A26。
                .uuid = BLE_UUID16_DECLARE(0x2A26),
                // 统一字符串 Read 回调。
                .access_cb = ble_service_gatt_access,
                // 参数指向固件版本标签。
                .arg = (void *)&g_tag_firmware_revision,
                // 允许公开读取固件版本。
                .flags = BLE_GATT_CHR_F_READ
            },
            {
                // 全零项终止 Device Information characteristic 数组。
                0
            }
        }
    },
    {
        // 全零项终止 GATT service 数组。
        0
    }
};

// 复制可选配置字符串；空值使用给定默认文本，超长值安全截断并写入 NUL。
static void ble_service_copy_string(
    char *destination,
    size_t destination_capacity,
    const char *source,
    const char *fallback)
{
    // source 为空时选择稳定默认字符串。
    const char *const selected = (source == NULL) ? fallback : source;
    // destination_capacity 至少由固定 32 字节数组提供。
    const size_t selected_length = strlen(selected);
    // 最多复制容量减一，保留结尾 NUL。
    const size_t copy_length = (selected_length < (destination_capacity - 1U))
        ? selected_length
        : (destination_capacity - 1U);
    // 复制有效 UTF-8 字节；截断可能切到多字节字符中间，因此产品字符串建议使用 ASCII。
    (void)memcpy(destination, selected, copy_length);
    // 显式写入 NUL，后续 strlen 不会越界。
    destination[copy_length] = '\0';
}

// 获取设备单调毫秒时间；低 32 位约 49.7 天自然回绕，符合协议定义。
static uint32_t ble_service_monotonic_ms(void)
{
    // esp_timer_get_time 返回自启动后的有符号微秒数。
    const uint64_t microseconds = (uint64_t)esp_timer_get_time();
    // 转换为毫秒后截取低 32 位。
    return (uint32_t)(microseconds / UINT64_C(1000));
}

// 在配对成功、失败、断线、忘记或停服时清除 UI 六位码；回调必须仅入队且快速返回。
static void ble_service_clear_pairing_code(ble_service_pairing_clear_reason_t reason)
{
    // 用户显式忘记和服务停止必须强制通知，即使内部活动位已在竞态中清零。
    const bool force_notify =
        (reason == BLE_SERVICE_PAIRING_CLEAR_FORGOTTEN) ||
        (reason == BLE_SERVICE_PAIRING_CLEAR_SERVICE_STOPPED);
    // 保存是否需要回调，然后立即清除敏感运行态。
    const bool should_notify = (g_ble_state.pairing_code_active != 0U) || force_notify;
    // 不论回调是否存在，内部都不再保留配对码活动事实。
    g_ble_state.pairing_code_active = UINT8_C(0);
    // 只在有效显示或强制清理路径上调用应用层。
    if (should_notify && (g_ble_state.configuration.passkey_clear != NULL)) {
        // 传递稳定原因和与显示回调共享的上下文。
        g_ble_state.configuration.passkey_clear(
            reason,
            g_ble_state.configuration.passkey_context);
    }
}

// 把纯 C 绑定编排器的断开操作适配为 NimBLE GAP 请求。
static int ble_service_bond_terminate_adapter(uint16_t connection_handle, void *context)
{
    // 当前适配不需要额外上下文。
    (void)context;
    // 使用远端用户终止原因异步断开当前 PC。
    return ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
}

// 把纯 C 绑定编排器的存储操作适配为 NimBLE 官方全量清理。
static int ble_service_bond_clear_store_adapter(void *context)
{
    // 当前适配不需要额外上下文。
    (void)context;
    // 删除 OUR_SEC、PEER_SEC、CCCD、对端地址和本机 IRK 等全部存储对象。
    return ble_store_clear();
}

// 查询当前连接是否同时满足加密、MITM 认证和绑定；三个条件缺一不可。
static bool ble_service_connection_security_is_valid(uint16_t connection_handle)
{
    // 只有当前唯一连接句柄才允许进入安全检查，陈旧句柄直接判为不安全。
    if ((connection_handle == BLE_HS_CONN_HANDLE_NONE) ||
        (connection_handle != g_ble_state.connection_handle)) {
        // 返回 false，禁止陈旧连接访问敏感业务。
        return false;
    }
    // descriptor 由 NimBLE 填充当前链路地址、参数和安全位，函数返回前一直有效。
    struct ble_gap_conn_desc descriptor;
    // 清零结构，避免查询失败时读取未初始化安全位。
    (void)memset(&descriptor, 0, sizeof(descriptor));
    // 根据连接句柄读取 NimBLE 权威连接描述。
    const int find_status = ble_gap_conn_find(connection_handle, &descriptor);
    // 查询失败表示连接已断开或主机状态异常，必须按不安全处理。
    if (find_status != 0) {
        // 返回 false，调用方可丢弃通知或终止连接。
        return false;
    }
    // encrypted 防窃听，authenticated 表示 MITM 验证成功，bonded 表示身份密钥已持久保存。
    return (descriptor.sec_state.encrypted != 0U) &&
        (descriptor.sec_state.authenticated != 0U) &&
        (descriptor.sec_state.bonded != 0U);
}

// 从完整逻辑帧固定头偏移 6 读取小端 sequence。
static uint16_t ble_service_frame_sequence(const uint8_t *frame)
{
    // 合并低字节和高字节。
    return (uint16_t)((uint16_t)frame[BLE_SERVICE_FRAME_SEQUENCE_OFFSET] |
        (uint16_t)((uint16_t)frame[BLE_SERVICE_FRAME_SEQUENCE_OFFSET + 1U] << 8U));
}

// 把字节追加到 GATT Read 响应 mbuf，并转换为 ATT 错误码。
static int ble_service_append_read_value(
    struct os_mbuf *output,
    const void *data,
    size_t length)
{
    // 输出 mbuf 和非空数据是 NimBLE Read 路径必填对象。
    if ((output == NULL) || ((length > 0U) && (data == NULL))) {
        // 返回不太可能错误，避免空指针访问。
        return BLE_ATT_ERR_UNLIKELY;
    }
    // NimBLE os_mbuf_append 长度参数可安全接收当前不超过 1040 字节的值。
    const int result = os_mbuf_append(output, data, (uint16_t)length);
    // 分配成功返回 0，否则返回 ATT 资源不足。
    return (result == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// 编码并立即发送一个 notification 逻辑帧的全部 MTU 分片。
static esp_err_t ble_service_send_notification(
    uint16_t value_handle,
    const uint8_t *frame,
    size_t frame_length)
{
    // 未完成加密、MITM 认证和绑定时禁止发送自定义敏感 notification。
    if ((g_ble_state.connection_handle == BLE_HS_CONN_HANDLE_NONE) ||
        (g_ble_state.secure_connection == 0U) ||
        !ble_service_connection_security_is_valid(g_ble_state.connection_handle)) {
        // 返回无效状态，调用者仍保留本地权威状态且不会泄露健身数据。
        return ESP_ERR_INVALID_STATE;
    }
    // 计算当前协商 MTU 下的分片总数。
    uint16_t fragment_count = UINT16_C(0);
    // 调用共享分片计数器。
    const imu_ble_status_t count_status = imu_ble_get_fragment_count(
        frame_length,
        g_ble_state.att_mtu,
        &fragment_count);
    // MTU 小于包络要求或帧长度非法时拒绝发送。
    if (count_status != IMU_BLE_STATUS_OK) {
        // 返回参数错误并记录诊断。
        ESP_LOGE(BLE_SERVICE_LOG_TAG, "通知分片计数失败 status=%d mtu=%u", (int)count_status, (unsigned int)g_ble_state.att_mtu);
        // 映射为 ESP-IDF 参数错误。
        return ESP_ERR_INVALID_ARG;
    }
    // 从完整帧读取逻辑序号，所有片使用同一序号。
    const uint16_t logical_sequence = ble_service_frame_sequence(frame);
    // 分片缓冲区按协议最大值预留，实际发送不超过 MTU-3。
    uint8_t fragment[BLE_SERVICE_MAX_GATT_VALUE_SIZE];
    // 遍历全部分片并按升序调用 notify_custom。
    for (uint16_t index = UINT16_C(0); index < fragment_count; ++index) {
        // 保存当前分片实际字节数。
        size_t fragment_length = 0U;
        // 编码 8 字节包络和连续帧数据。
        const imu_ble_status_t encode_status = imu_ble_encode_fragment(
            frame,
            frame_length,
            g_ble_state.att_mtu,
            logical_sequence,
            index,
            fragment,
            sizeof(fragment),
            &fragment_length);
        // 本地分片编码失败时停止，PC 会通过下一 LiveState 或续传游标恢复。
        if (encode_status != IMU_BLE_STATUS_OK) {
            // 记录精确分片索引和状态。
            ESP_LOGE(BLE_SERVICE_LOG_TAG, "通知分片编码失败 index=%u status=%d", (unsigned int)index, (int)encode_status);
            // 返回本地参数错误。
            return ESP_ERR_INVALID_ARG;
        }
        // 从当前分片复制到 NimBLE mbuf；所有权在发送调用后转交 NimBLE。
        struct os_mbuf *const packet = ble_hs_mbuf_from_flat(fragment, (uint16_t)fragment_length);
        // mbuf 池耗尽时停止发送，不能使用空对象。
        if (packet == NULL) {
            // 记录内存不足。
            ESP_LOGE(BLE_SERVICE_LOG_TAG, "通知 mbuf 分配失败 index=%u", (unsigned int)index);
            // 返回内存不足。
            return ESP_ERR_NO_MEM;
        }
        // 向当前唯一连接的指定特征发送 notification。
        const int notify_result = ble_gatts_notify_custom(
            g_ble_state.connection_handle,
            value_handle,
            packet);
        // NimBLE 非零返回表示该片未排入发送队列。
        if (notify_result != 0) {
            // NimBLE 在失败路径负责释放传入 mbuf；记录错误供诊断页显示。
            ESP_LOGW(BLE_SERVICE_LOG_TAG, "通知发送失败 index=%u rc=%d", (unsigned int)index, notify_result);
            // 转换为通用失败。
            return ESP_FAIL;
        }
    }
    // 所有分片均已排入 NimBLE 发送队列。
    return ESP_OK;
}

// 发送 pending_indication 的下一片；上一片必须已确认或这是首片。
static esp_err_t ble_service_send_next_indication_fragment(void)
{
    // 没有活动响应时无需发送。
    if (g_ble_state.pending_indication.active == 0U) {
        // 返回无效状态提示调用者事件顺序错误。
        return ESP_ERR_INVALID_STATE;
    }
    // 全部分片已经确认时清空活动状态。
    if (g_ble_state.pending_indication.next_fragment_index >= g_ble_state.pending_indication.fragment_count) {
        // 关闭 indication 队列槽。
        g_ble_state.pending_indication.active = UINT8_C(0);
        // 响应已经完整确认。
        return ESP_OK;
    }
    // 分片缓冲区最多容纳完整帧加包络，实际长度由 ATT MTU 限制。
    uint8_t fragment[BLE_SERVICE_MAX_GATT_VALUE_SIZE];
    // 保存当前分片长度。
    size_t fragment_length = 0U;
    // 编码下一严格顺序分片。
    const imu_ble_status_t encode_status = imu_ble_encode_fragment(
        g_ble_state.pending_indication.frame,
        g_ble_state.pending_indication.frame_length,
        g_ble_state.pending_indication.att_mtu,
        g_ble_state.pending_indication.logical_sequence,
        g_ble_state.pending_indication.next_fragment_index,
        fragment,
        sizeof(fragment),
        &fragment_length);
    // 本地编码失败时终止整个可靠响应，PC 将使用同 request_id 重试。
    if (encode_status != IMU_BLE_STATUS_OK) {
        // 清除活动槽，避免永远阻塞后续响应。
        g_ble_state.pending_indication.active = UINT8_C(0);
        // 记录精确错误。
        ESP_LOGE(BLE_SERVICE_LOG_TAG, "indication 分片编码失败 status=%d", (int)encode_status);
        // 返回参数错误。
        return ESP_ERR_INVALID_ARG;
    }
    // 分配承载当前分片的 NimBLE mbuf。
    struct os_mbuf *const packet = ble_hs_mbuf_from_flat(fragment, (uint16_t)fragment_length);
    // 内存池不足时终止本响应，等待 PC 重试。
    if (packet == NULL) {
        // 清除活动槽。
        g_ble_state.pending_indication.active = UINT8_C(0);
        // 返回内存不足。
        return ESP_ERR_NO_MEM;
    }
    // 保存本次实际发送索引用于日志。
    const uint16_t sending_index = g_ble_state.pending_indication.next_fragment_index;
    // 先前移下一个索引；确认事件成功后继续发送该新索引。
    g_ble_state.pending_indication.next_fragment_index = (uint16_t)(sending_index + UINT16_C(1));
    // 发起可靠 indication；每次只能有一个未确认 indication。
    const int indicate_result = ble_gatts_indicate_custom(
        g_ble_state.connection_handle,
        g_ble_state.pending_indication.value_handle,
        packet);
    // 发送未排队时回退并释放队列槽，PC 超时后重试。
    if (indicate_result != 0) {
        // 清除活动槽，禁止错误地等待不存在的确认。
        g_ble_state.pending_indication.active = UINT8_C(0);
        // 记录 NimBLE 返回值。
        ESP_LOGW(BLE_SERVICE_LOG_TAG, "indication 发送失败 index=%u rc=%d", (unsigned int)sending_index, indicate_result);
        // 返回通用失败。
        return ESP_FAIL;
    }
    // 当前分片正在等待 PC 确认。
    return ESP_OK;
}

// 把完整控制或传输响应复制到唯一 indication 槽并发送首片。
static esp_err_t ble_service_begin_indication(
    uint16_t value_handle,
    const uint8_t *frame,
    size_t frame_length)
{
    // 无连接时无法发送可靠响应。
    if (g_ble_state.connection_handle == BLE_HS_CONN_HANDLE_NONE) {
        // 返回无效状态。
        return ESP_ERR_INVALID_STATE;
    }
    // 已有 indication 未确认时拒绝覆盖；控制幂等缓存允许 PC 稍后重试。
    if (g_ble_state.pending_indication.active != 0U) {
        // 返回无效状态并保留原响应。
        return ESP_ERR_INVALID_STATE;
    }
    // 完整逻辑帧必须位于协议固定上限内。
    if ((frame == NULL) || (frame_length == 0U) || (frame_length > IMU_BLE_MAX_FRAME_SIZE)) {
        // 拒绝空帧或过大帧。
        return ESP_ERR_INVALID_ARG;
    }
    // 查询固定 MTU 下总片数。
    uint16_t fragment_count = UINT16_C(0);
    // 使用共享计数实现。
    const imu_ble_status_t count_status = imu_ble_get_fragment_count(
        frame_length,
        g_ble_state.att_mtu,
        &fragment_count);
    // 分片计数失败时不占用队列槽。
    if (count_status != IMU_BLE_STATUS_OK) {
        // 返回参数错误。
        return ESP_ERR_INVALID_ARG;
    }
    // 清零旧槽，避免诊断转储显示上次帧尾部。
    (void)memset(&g_ble_state.pending_indication, 0, sizeof(g_ble_state.pending_indication));
    // 保存目标特征句柄。
    g_ble_state.pending_indication.value_handle = value_handle;
    // 锁定当前 MTU。
    g_ble_state.pending_indication.att_mtu = g_ble_state.att_mtu;
    // 从完整逻辑帧读取 sequence。
    g_ble_state.pending_indication.logical_sequence = ble_service_frame_sequence(frame);
    // 保存总分片数。
    g_ble_state.pending_indication.fragment_count = fragment_count;
    // 首片索引从 0 开始。
    g_ble_state.pending_indication.next_fragment_index = UINT16_C(0);
    // 保存完整帧长度。
    g_ble_state.pending_indication.frame_length = frame_length;
    // 复制完整帧，调用者返回后原缓冲区可以复用。
    (void)memcpy(g_ble_state.pending_indication.frame, frame, frame_length);
    // 最后设置活动位。
    g_ble_state.pending_indication.active = BLE_SERVICE_SINGLE_INDICATION_QUEUE;
    // 立即发送首片并等待 GAP NOTIFY_TX 确认事件。
    return ble_service_send_next_indication_fragment();
}

// 把任意消息 payload 编码成逻辑帧并发布 notification。
static esp_err_t ble_service_publish_notification_message(
    uint8_t message_type,
    uint16_t value_handle,
    uint8_t subscribed,
    const uint8_t *payload,
    uint16_t payload_length,
    uint32_t monotonic_ms,
    uint8_t *latest_frame,
    size_t latest_capacity,
    size_t *latest_length)
{
    // 非零 payload 必须有有效指针。
    if ((payload_length > 0U) && (payload == NULL)) {
        // 返回参数错误。
        return ESP_ERR_INVALID_ARG;
    }
    // 使用调用者最新帧缓冲或本地临时缓冲。
    uint8_t temporary_frame[IMU_BLE_MAX_FRAME_SIZE];
    // target_frame 指向要编码的连续缓冲区。
    uint8_t *const target_frame = (latest_frame == NULL) ? temporary_frame : latest_frame;
    // target_capacity 与所选缓冲区一致。
    const size_t target_capacity = (latest_frame == NULL) ? sizeof(temporary_frame) : latest_capacity;
    // 保存完整逻辑帧长度。
    size_t frame_length = 0U;
    // 当前消息使用自己独立的 16 位 sequence。
    const uint16_t sequence = g_ble_state.sequence_by_message[message_type];
    // 编码协议头、payload 和 CRC16。
    const ble_service_status_t encode_status = ble_service_encode_message(
        message_type,
        UINT8_C(0),
        sequence,
        monotonic_ms,
        payload,
        payload_length,
        target_frame,
        target_capacity,
        &frame_length);
    // 编码失败时不消耗序号。
    if (encode_status != BLE_SERVICE_STATUS_OK) {
        // 返回参数或容量错误。
        return (encode_status == BLE_SERVICE_STATUS_BUFFER_TOO_SMALL) ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }
    // uint16 序号自然回绕。
    g_ble_state.sequence_by_message[message_type] = (uint16_t)(sequence + UINT16_C(1));
    // 调用者要求保存最新帧时写回长度。
    if (latest_length != NULL) {
        // 保存完整逻辑帧长度供 GATT Read。
        *latest_length = frame_length;
    }
    // 未建立安全绑定连接或未订阅时只更新最新状态，不发送无线数据。
    if ((g_ble_state.connection_handle == BLE_HS_CONN_HANDLE_NONE) ||
        (g_ble_state.secure_connection == 0U) ||
        (subscribed == 0U)) {
        // 本地状态更新成功。
        return ESP_OK;
    }
    // 使用当前 MTU 分片并发送 notification。
    return ble_service_send_notification(value_handle, target_frame, frame_length);
}

// 把 GATT Write mbuf 展平到固定分片缓冲区。
static int ble_service_flatten_write(
    const struct os_mbuf *input,
    uint8_t *output,
    size_t output_capacity,
    uint16_t *output_length)
{
    // 必填对象为空时返回 ATT 不太可能错误。
    if ((input == NULL) || (output == NULL) || (output_length == NULL)) {
        // 不尝试读取无效 mbuf。
        return BLE_ATT_ERR_UNLIKELY;
    }
    // 获取整个 mbuf 链字节数。
    const uint16_t packet_length = OS_MBUF_PKTLEN(input);
    // 每个写入必须至少包含 8 字节分片包络，并不得超过固定缓冲区。
    if (((size_t)packet_length < BLE_SERVICE_MIN_FRAGMENT_SIZE) || ((size_t)packet_length > output_capacity)) {
        // 返回标准属性值长度错误。
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    // 把可能分链的 mbuf 展平为连续字节供共享重组器处理。
    const int flatten_result = ble_hs_mbuf_to_flat(
        input,
        output,
        (uint16_t)output_capacity,
        output_length);
    // 展平失败通常表示 mbuf 内部异常或容量不一致。
    return (flatten_result == 0) ? 0 : BLE_ATT_ERR_UNLIKELY;
}

// 处理 Transfer Control 严格分片、消息类型 5 和业务回调，并开始类型 6 indication。
static BLE_SERVICE_STACK_BOUNDARY int ble_service_handle_transfer_write(
    const uint8_t *fragment,
    uint16_t fragment_length,
    uint32_t connection_epoch)
{
    // completed_frame 借用连接内 transfer_reassembler 缓冲区。
    const uint8_t *completed_frame = NULL;
    // 保存完整逻辑帧长度。
    size_t completed_length = 0U;
    // complete 表示最后一片和 CRC 均已通过。
    uint8_t complete = UINT8_C(0);
    // 严格顺序重组 0005 特征分片。
    const imu_ble_status_t reassembly_status = imu_ble_reassembler_push(
        &g_ble_state.connection_core.transfer_reassembler,
        fragment,
        fragment_length,
        &completed_frame,
        &completed_length,
        &complete);
    // 坏分片后清空状态并返回 ATT 错误。
    if (reassembly_status != IMU_BLE_STATUS_OK) {
        // 清空半帧。
        imu_ble_reassembler_reset(&g_ble_state.connection_core.transfer_reassembler);
        // 记录协议错误。
        ESP_LOGW(BLE_SERVICE_LOG_TAG, "TransferRequest 重组失败 status=%d", (int)reassembly_status);
        // 告知客户端写入值不可接受。
        return BLE_ATT_ERR_UNLIKELY;
    }
    // 尚未收齐时成功接收当前片。
    if (complete == 0U) {
        // 等待下一片。
        return 0;
    }
    // 解码完整逻辑帧，重组器已经校验过一次，二次解析用于读取字段。
    imu_ble_frame_view_t frame_view;
    // 解析固定头和 payload 视图。
    const imu_ble_status_t decode_status = imu_ble_decode_frame(completed_frame, completed_length, &frame_view);
    // 理论不会失败；仍防止内存损坏进入传输业务。
    if ((decode_status != IMU_BLE_STATUS_OK) ||
        (frame_view.protocol_major != IMU_BLE_PROTOCOL_MAJOR) ||
        (frame_view.message_type != (uint8_t)BLE_SERVICE_MESSAGE_TRANSFER_REQUEST)) {
        // 记录错误类型或版本。
        ESP_LOGW(BLE_SERVICE_LOG_TAG, "Transfer Control 收到非 TransferRequest 逻辑帧");
        // 返回 ATT 不太可能错误。
        return BLE_ATT_ERR_UNLIKELY;
    }
    // 未注册传输处理器时明确拒绝，避免发送无定义响应格式。
    if (g_ble_state.configuration.transfer_handler == NULL) {
        // 返回应用错误。
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    // 传输响应 payload 最大使用协议 1024 字节。
    uint8_t response_payload[IMU_BLE_MAX_PAYLOAD_SIZE];
    // 保存业务返回长度。
    uint16_t response_payload_length = UINT16_C(0);
    // 调用会话/文件传输业务处理器。
    const ble_service_status_t handler_status = g_ble_state.configuration.transfer_handler(
        frame_view.payload,
        frame_view.payload_length,
        response_payload,
        sizeof(response_payload),
        &response_payload_length,
        g_ble_state.configuration.transfer_context);
    // 业务失败时不发送含义不明的响应。
    if ((handler_status != BLE_SERVICE_STATUS_OK) || ((size_t)response_payload_length > IMU_BLE_MAX_PAYLOAD_SIZE)) {
        // 记录传输处理失败。
        ESP_LOGW(BLE_SERVICE_LOG_TAG, "TransferRequest 业务处理失败 status=%d", (int)handler_status);
        // 返回 ATT 不太可能错误。
        return BLE_ATT_ERR_UNLIKELY;
    }
    // 保存完整 TransferResponse 逻辑帧。
    uint8_t response_frame[IMU_BLE_MAX_FRAME_SIZE];
    // 保存完整帧长度。
    size_t response_frame_length = 0U;
    // 读取并使用类型 6 独立发送序号。
    const uint16_t sequence = g_ble_state.sequence_by_message[BLE_SERVICE_MESSAGE_TRANSFER_RESPONSE];
    // 编码逻辑响应和 CRC。
    const ble_service_status_t encode_status = ble_service_encode_message(
        (uint8_t)BLE_SERVICE_MESSAGE_TRANSFER_RESPONSE,
        UINT8_C(0),
        sequence,
        ble_service_monotonic_ms(),
        response_payload,
        response_payload_length,
        response_frame,
        sizeof(response_frame),
        &response_frame_length);
    // 编码失败时返回 ATT 错误。
    if (encode_status != BLE_SERVICE_STATUS_OK) {
        // 不消耗序号。
        return BLE_ATT_ERR_UNLIKELY;
    }
    // 成功编码后消耗序号。
    g_ble_state.sequence_by_message[BLE_SERVICE_MESSAGE_TRANSFER_RESPONSE] = (uint16_t)(sequence + UINT16_C(1));
    // 处理期间若发生断连或新连接，丢弃旧响应且不触碰新连接 indication 槽。
    if ((connection_epoch != g_ble_state.connection_epoch) ||
        (g_ble_state.secure_connection == 0U)) {
        // 返回成功表示旧业务已经安全结束，但无线响应应由客户端重连后重试。
        return 0;
    }
    // 开始可靠 indication，多片会在确认事件中继续发送。
    const esp_err_t indication_status = ble_service_begin_indication(
        g_transfer_control_handle,
        response_frame,
        response_frame_length);
    // indication 队列忙时让写操作失败，PC 可按传输游标重试。
    return (indication_status == ESP_OK) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// 在独立 BLE 业务任务中处理一片 Control Point，允许等待应用 broker 而不阻塞 NimBLE 主机。
static BLE_SERVICE_STACK_BOUNDARY void ble_service_process_control_work(const ble_service_work_item_t *item)
{
    // 工作项必须存在，且调用者已经核对连接代次和安全状态。
    if (item == NULL) {
        // 空工作项属于内部错误，直接返回。
        return;
    }
    // response_frame 保存完整 ControlResponse 逻辑帧，最大 1040 字节。
    uint8_t response_frame[IMU_BLE_MAX_FRAME_SIZE];
    // response_length 保存 response_frame 有效字节数。
    size_t response_length = 0U;
    // from_cache 为 1 表示 request_id 命中最近 16 项幂等缓存。
    uint8_t from_cache = UINT8_C(0);
    // 推入当前控制分片；完整后才调用应用 command_handler，处理器可等待应用任务最多 500 ms。
    const ble_service_status_t service_status = ble_service_process_control_fragment(
        &g_ble_state.connection_core,
        item->fragment,
        item->fragment_length,
        ble_service_monotonic_ms(),
        g_ble_state.configuration.command_handler,
        g_ble_state.configuration.command_context,
        response_frame,
        sizeof(response_frame),
        &response_length,
        &from_cache);
    // 中间分片只更新重组器，不产生响应。
    if (service_status == BLE_SERVICE_STATUS_INCOMPLETE) {
        // 等待同连接代次的下一分片。
        return;
    }
    // CRC、类型或长度错误没有响应帧，记录后由 PC 超时并重试完整事务。
    if (response_length == 0U) {
        // 记录纯协议核心返回码，不输出请求载荷。
        ESP_LOGW(BLE_SERVICE_LOG_TAG, "ControlRequest 异步处理拒绝 status=%d", (int)service_status);
        // 返回，不占用 indication 槽。
        return;
    }
    // 应用处理期间若断连或连接代次变化，丢弃旧响应，避免发送给后来连接的 PC。
    if ((item->connection_epoch != g_ble_state.connection_epoch) ||
        (item->connection_handle != g_ble_state.connection_handle) ||
        (g_ble_state.secure_connection == 0U)) {
        // 记录陈旧响应被安全丢弃；同 request_id 可在重连后幂等重试。
        ESP_LOGI(BLE_SERVICE_LOG_TAG, "ControlResponse 因连接代次变化丢弃 request cache=%u", (unsigned int)from_cache);
        // 返回。
        return;
    }
    // 在可靠响应槽空闲时开始 ControlResponse indication，后续片由 GAP 确认事件推进。
    const esp_err_t indication_status = ble_service_begin_indication(
        g_control_point_handle,
        response_frame,
        response_length);
    // 资源或连接状态错误只记录，PC 使用相同 request_id 重试不会二次执行业务。
    if (indication_status != ESP_OK) {
        // 输出缓存命中与 ESP-IDF 错误，不输出敏感 TLV。
        ESP_LOGW(
            BLE_SERVICE_LOG_TAG,
            "控制响应 indication 启动失败 cache=%u err=%s",
            (unsigned int)from_cache,
            esp_err_to_name(indication_status));
    }
}

// 独立 BLE 业务任务串行重组控制/传输帧；只有本任务修改两个重组器和幂等缓存。
static void ble_service_worker_task(void *argument)
{
    // 当前任务不使用启动参数，全部状态位于 g_ble_state 单例。
    (void)argument;
    // observed_epoch 保存本任务已经重置到的连接代次，初值强制首轮清理核心。
    uint32_t observed_epoch = UINT32_MAX;
    // item 按值接收稳定分片，任务处理期间不借用队列内部存储。
    ble_service_work_item_t item;
    // minimum_free_bytes 保存任务启动后的历史最小剩余栈字节，真板可据此验证首次绑定余量。
    UBaseType_t minimum_free_bytes = (UBaseType_t)BLE_SERVICE_WORKER_STACK_BYTES;
    // 直到 stop 明确置位前持续处理业务分片。
    while (g_ble_state.worker_stop_requested == 0U) {
        // GAP 连接代次变化时清空半帧和最近请求缓存，旧连接内容绝不跨到新连接。
        if (observed_epoch != g_ble_state.connection_epoch) {
            // 重置两个重组器和 16 项幂等缓存；本任务是唯一修改者。
            ble_service_connection_reset(&g_ble_state.connection_core);
            // 记录已经完成清理的代次。
            observed_epoch = g_ble_state.connection_epoch;
        }
        // 上一可靠响应尚未全部确认时不执行下一事务，确保 response indication 先于 TransferData。
        if (g_ble_state.pending_indication.active != 0U) {
            // 短暂让出 CPU，GAP NOTIFY_TX 事件会推进或清空可靠响应。
            vTaskDelay(pdMS_TO_TICKS(BLE_SERVICE_WORKER_POLL_MS));
            // 返回循环顶部检查停止和连接代次。
            continue;
        }
        // 最多等待 20 ms 接收一个 GATT 分片，使 stop 不依赖向满队列插入哨兵。
        if (xQueueReceive(
                g_ble_state.work_queue,
                &item,
                pdMS_TO_TICKS(BLE_SERVICE_WORKER_POLL_MS)) != pdPASS) {
            // 超时后检查停止位和连接代次。
            continue;
        }
        // stop 可能在队列接收返回前置位；此时不得再执行刚取得的业务分片。
        if (g_ble_state.worker_stop_requested != 0U) {
            // 跳出循环并释放任务。
            break;
        }
        // 丢弃断连、句柄变化或安全状态撤销后的陈旧分片。
        if ((item.connection_epoch != g_ble_state.connection_epoch) ||
            (item.connection_handle != g_ble_state.connection_handle) ||
            (g_ble_state.secure_connection == 0U)) {
            // 不调用业务处理器，避免旧 PC 改变当前状态。
            continue;
        }
        // Control Point 使用幂等控制核心和应用 broker。
        if (item.kind == BLE_SERVICE_WORK_CONTROL) {
            // 在本任务处理控制分片；函数可等待应用任务但不占用 NimBLE 主机任务。
            ble_service_process_control_work(&item);
        } else if (item.kind == BLE_SERVICE_WORK_TRANSFER) {
            // 传输分片完整后可冻结会话页并开始 TransferResponse indication。
            const int transfer_status = ble_service_handle_transfer_write(
                item.fragment,
                item.fragment_length,
                item.connection_epoch);
            // 异步路径无法再返回 ATT 错误，只记录并由 PC 超时重试。
            if (transfer_status != 0) {
                // 输出 ATT 风格状态码，不输出会话数据。
                ESP_LOGW(BLE_SERVICE_LOG_TAG, "TransferRequest 异步处理失败 att=%d", transfer_status);
            }
        } else {
            // 未知枚举表示内存损坏或版本不一致，记录并丢弃。
            ESP_LOGE(BLE_SERVICE_LOG_TAG, "未知 BLE 工作类型=%d", (int)item.kind);
        }
        // ESP-IDF 返回历史最小空闲栈字节；在大缓冲函数退出后读取仍能覆盖刚完成的最深调用链。
        const UBaseType_t current_free_bytes = uxTaskGetStackHighWaterMark(NULL);
        // 只在刷新历史低点时输出，避免正常 20 ms 队列轮询产生重复日志。
        if (current_free_bytes < minimum_free_bytes) {
            // 保存新低点；数值越大表示与栈金丝雀的安全距离越充足。
            minimum_free_bytes = current_free_bytes;
            // 输出字节单位事实，不输出控制请求内容或配对密钥。
            ESP_LOGI(
                BLE_SERVICE_LOG_TAG,
                "BLE_WORKER_STACK minimum_free_bytes=%u",
                (unsigned int)minimum_free_bytes);
        }
    }
    // 清空任务句柄，stop 轮询该值确认任务已经不再访问 g_ble_state。
    g_ble_state.worker_task = NULL;
    // 删除当前 FreeRTOS 任务并释放动态任务资源。
    vTaskDelete(NULL);
}

// GATT Read/Write 统一入口。
static int ble_service_gatt_access(
    uint16_t connection_handle,
    uint16_t attribute_handle,
    struct ble_gatt_access_ctxt *context,
    void *argument)
{
    // attribute_handle 仅用于日志，标签才是稳定业务路由依据。
    (void)attribute_handle;
    // 必填上下文和静态标签不可为空。
    if ((context == NULL) || (argument == NULL)) {
        // 返回通用 ATT 错误。
        return BLE_ATT_ERR_UNLIKELY;
    }
    // 读取静态标签值。
    const ble_service_characteristic_tag_t tag = *(const ble_service_characteristic_tag_t *)argument;
    // 处理 characteristic Read。
    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        // Manifest 返回 start 时固定的原始 TLV；ATT 长读由 NimBLE 自动拆包。
        if (tag == BLE_SERVICE_TAG_MANIFEST) {
            // 追加 Manifest TLV。
            return ble_service_append_read_value(
                context->om,
                g_ble_state.manifest_frame,
                g_ble_state.manifest_frame_length);
        }
        // Live State 返回最近权威快照；尚未发布时返回空值。
        if (tag == BLE_SERVICE_TAG_LIVE_STATE) {
            // LiveState 含动作、次数、卡路里和训练状态，必须只对已绑定安全连接开放。
            if ((g_ble_state.secure_connection == 0U) ||
                !ble_service_connection_security_is_valid(connection_handle)) {
                // 返回认证不足，提示 Windows 完成配对而不是泄露快照。
                return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
            }
            // 追加最新逻辑帧，长度可以为 0。
            return ble_service_append_read_value(
                context->om,
                g_ble_state.live_state_frame,
                g_ble_state.live_state_frame_length);
        }
        // Battery Level 返回单字节 0～100 或 255 未知。
        if (tag == BLE_SERVICE_TAG_BATTERY) {
            // 追加当前电量字节。
            return ble_service_append_read_value(context->om, &g_ble_state.battery_percent, sizeof(g_ble_state.battery_percent));
        }
        // Device Information 厂商字符串。
        if (tag == BLE_SERVICE_TAG_MANUFACTURER) {
            // 不发送结尾 NUL，长度按 UTF-8 字节数计算。
            return ble_service_append_read_value(context->om, g_ble_state.manufacturer_name, strlen(g_ble_state.manufacturer_name));
        }
        // Device Information 型号字符串。
        if (tag == BLE_SERVICE_TAG_MODEL) {
            // 追加型号有效字节。
            return ble_service_append_read_value(context->om, g_ble_state.model_number, strlen(g_ble_state.model_number));
        }
        // Device Information 序列号字符串。
        if (tag == BLE_SERVICE_TAG_SERIAL) {
            // 追加序列号有效字节。
            return ble_service_append_read_value(context->om, g_ble_state.serial_number, strlen(g_ble_state.serial_number));
        }
        // Device Information 硬件版本字符串。
        if (tag == BLE_SERVICE_TAG_HARDWARE_REVISION) {
            // 追加硬件版本有效字节。
            return ble_service_append_read_value(context->om, g_ble_state.hardware_revision, strlen(g_ble_state.hardware_revision));
        }
        // Device Information 固件版本字符串。
        if (tag == BLE_SERVICE_TAG_FIRMWARE_REVISION) {
            // 追加固件版本有效字节。
            return ble_service_append_read_value(context->om, g_ble_state.firmware_revision, strlen(g_ble_state.firmware_revision));
        }
        // 其它特征未声明 Read，若栈仍调用则返回不允许。
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    // 处理 characteristic Write。
    if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // 只允许当前已连接 PC 写入自己的特征。
        if (connection_handle != g_ble_state.connection_handle) {
            // 拒绝无连接或陈旧连接句柄。
            return BLE_ATT_ERR_UNLIKELY;
        }
        // 自定义可写特征均会改变训练状态或读取历史，必须同时满足加密、MITM 认证和绑定。
        if ((g_ble_state.secure_connection == 0U) ||
            !ble_service_connection_security_is_valid(connection_handle)) {
            // 返回认证不足；客户端完成安全配对后可使用同 request_id 重试。
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }
        // 固定缓冲区保存一个完整 ATT Value 分片。
        uint8_t fragment[BLE_SERVICE_MAX_GATT_VALUE_SIZE];
        // 保存展平后的实际分片长度。
        uint16_t fragment_length = UINT16_C(0);
        // 展平可能分链的 mbuf。
        const int flatten_status = ble_service_flatten_write(
            context->om,
            fragment,
            sizeof(fragment),
            &fragment_length);
        // 长度或 mbuf 异常时立即返回 ATT 错误。
        if (flatten_status != 0) {
            // 不进入任何重组状态。
            return flatten_status;
        }
        // Control Point 与 Transfer Control 都只复制到有界工作队列，禁止在 NimBLE 回调等待应用或 Flash 锁。
        if ((tag == BLE_SERVICE_TAG_CONTROL_POINT) ||
            (tag == BLE_SERVICE_TAG_TRANSFER_CONTROL)) {
            // 工作队列必须已创建且停止流程尚未开始。
            if ((g_ble_state.work_queue == NULL) ||
                (g_ble_state.worker_stop_requested != 0U)) {
                // 返回资源不足，PC 可稍后使用同 request_id 重试。
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            // work_item 深拷贝当前分片和连接身份，回调返回后 mbuf 可立即释放。
            ble_service_work_item_t work_item;
            // 清零未使用尾部，避免诊断转储包含旧栈数据。
            (void)memset(&work_item, 0, sizeof(work_item));
            // 根据特征标签选择纯控制或会话传输重组器。
            work_item.kind = (tag == BLE_SERVICE_TAG_CONTROL_POINT)
                ? BLE_SERVICE_WORK_CONTROL
                : BLE_SERVICE_WORK_TRANSFER;
            // 保存当前安全连接句柄。
            work_item.connection_handle = connection_handle;
            // 保存当前连接代次，工作任务可识别断线后的陈旧分片。
            work_item.connection_epoch = g_ble_state.connection_epoch;
            // 保存有效字节数。
            work_item.fragment_length = fragment_length;
            // 复制完整 ATT Value；长度已由 flatten 函数验证不超过数组容量。
            (void)memcpy(work_item.fragment, fragment, fragment_length);
            // 零等待投递，队列满时回调立即返回资源不足而不是阻塞 NimBLE 主机。
            if (xQueueSend(g_ble_state.work_queue, &work_item, 0U) != pdPASS) {
                // 记录背压事实，不输出分片内容。
                ESP_LOGW(BLE_SERVICE_LOG_TAG, "BLE 业务队列已满 tag=%d", (int)tag);
                // 客户端等待后重试同一逻辑事务。
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            // 当前分片已稳定入队，GATT 回调立即成功返回。
            return 0;
        }
        // 其它特征没有 Write 权限。
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    // 描述符由 NimBLE 自动管理 CCCD，不应进入本回调。
    return BLE_ATT_ERR_UNLIKELY;
}

// 注册 GATT 数据库并初始化标准 GAP/GATT 服务。
static int ble_service_register_gatt_database(void)
{
    // 初始化标准 GAP 服务，提供 Device Name 和 Appearance。
    ble_svc_gap_init();
    // 初始化标准 GATT 服务，支持 Service Changed。
    ble_svc_gatt_init();
    // 预计算全部服务、特征和 CCCD 所需属性数量。
    int result = ble_gatts_count_cfg(g_ble_services);
    // 属性计数失败时返回 NimBLE 错误。
    if (result != 0) {
        // 不继续注册半个数据库。
        return result;
    }
    // 把自定义、Battery 和 Device Information 服务加入数据库。
    result = ble_gatts_add_svcs(g_ble_services);
    // 返回注册结果。
    return result;
}

// 开始可连接、通用发现广播；128 位服务 UUID 放广播包，完整名称放扫描响应。
static void ble_service_start_advertising(void)
{
    // 服务未启动或已经连接时不发起新广播。
    if ((g_ble_state.started == 0U) || (g_ble_state.connection_handle != BLE_HS_CONN_HANDLE_NONE)) {
        // 直接返回，避免重复 ble_gap_adv_start。
        return;
    }
    // OFF 策略禁止同步回调或断连回调自行恢复广播。
    if (g_ble_state.power_mode == BLE_SERVICE_NIMBLE_POWER_OFF) {
        // 保持控制器空闲，等待主应用显式切换到快/慢广播。
        return;
    }
    // advertising fields 保存 flags、TX 功率和完整 128 位服务 UUID。
    struct ble_hs_adv_fields advertising_fields;
    // 先清零所有可选字段。
    (void)memset(&advertising_fields, 0, sizeof(advertising_fields));
    // 声明通用可发现且不支持经典蓝牙 BR/EDR。
    advertising_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    // 请求控制器自动填写 TX 功率。
    advertising_fields.tx_pwr_lvl_is_present = UINT8_C(1);
    // 使用 NimBLE 自动功率哨兵。
    advertising_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    // 广播完整自定义服务 UUID，使 Windows 可在连接前过滤设备。
    advertising_fields.uuids128 = (ble_uuid128_t *)&g_fitness_service_uuid;
    // 广播一个 128 位 UUID。
    advertising_fields.num_uuids128 = UINT8_C(1);
    // 标记 UUID 列表完整。
    advertising_fields.uuids128_is_complete = UINT8_C(1);
    // 设置广播字段。
    int result = ble_gap_adv_set_fields(&advertising_fields);
    // 广播包超过 31 字节或字段非法时记录并返回。
    if (result != 0) {
        // 记录 NimBLE 错误码。
        ESP_LOGE(BLE_SERVICE_LOG_TAG, "设置广播字段失败 rc=%d", result);
        // 禁止用残留字段开始广播。
        return;
    }
    // scan_response_fields 只保存完整设备名，避免 128 位 UUID 与名称共同超过 31 字节。
    struct ble_hs_adv_fields scan_response_fields;
    // 清零扫描响应字段。
    (void)memset(&scan_response_fields, 0, sizeof(scan_response_fields));
    // 指向 start 时复制的稳定设备名。
    scan_response_fields.name = (uint8_t *)g_ble_state.device_name;
    // 设备名长度不包含 NUL。
    scan_response_fields.name_len = (uint8_t)strlen(g_ble_state.device_name);
    // 标记完整名称。
    scan_response_fields.name_is_complete = UINT8_C(1);
    // 设置扫描响应。
    result = ble_gap_adv_rsp_set_fields(&scan_response_fields);
    // 名称过长等错误时记录并返回。
    if (result != 0) {
        // 记录 NimBLE 错误码。
        ESP_LOGE(BLE_SERVICE_LOG_TAG, "设置扫描响应失败 rc=%d", result);
        // 禁止开始不完整产品广播。
        return;
    }
    // adv_params 定义不定向可连接和通用发现模式。
    struct ble_gap_adv_params advertising_parameters;
    // 清零所有参数；下方按功耗策略填写可审计的固定间隔。
    (void)memset(&advertising_parameters, 0, sizeof(advertising_parameters));
    // 允许一个 Windows PC 发起连接。
    advertising_parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    // 通用发现模式持续到连接或显式停止。
    advertising_parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // 慢广播策略以及待机连接断开后的回退均使用 1.0～1.2 s 间隔。
    if ((g_ble_state.power_mode == BLE_SERVICE_NIMBLE_POWER_SLOW_ADVERTISING) ||
        (g_ble_state.power_mode == BLE_SERVICE_NIMBLE_POWER_CONNECTED_MODEM_SLEEP)) {
        // 写入慢广播最小间隔，单位 0.625 ms。
        advertising_parameters.itvl_min = BLE_SERVICE_SLOW_ADV_INTERVAL_MIN;
        // 写入慢广播最大间隔，单位 0.625 ms。
        advertising_parameters.itvl_max = BLE_SERVICE_SLOW_ADV_INTERVAL_MAX;
    } else {
        // 主页、训练和活动连接断开后的回退使用快速广播。
        advertising_parameters.itvl_min = BLE_SERVICE_FAST_ADV_INTERVAL_MIN;
        // 写入快速广播最大间隔，单位 0.625 ms。
        advertising_parameters.itvl_max = BLE_SERVICE_FAST_ADV_INTERVAL_MAX;
    }
    // 无限期开始广播。
    result = ble_gap_adv_start(
        g_ble_state.own_address_type,
        NULL,
        BLE_HS_FOREVER,
        &advertising_parameters,
        ble_service_gap_event,
        NULL);
    // 记录启动结果。
    if (result != 0) {
        // 失败时保留服务启动状态，主机重置后可再次同步。
        ESP_LOGE(BLE_SERVICE_LOG_TAG, "开始广播失败 rc=%d", result);
    }
}

// NimBLE 主机同步回调在地址和 GATT 数据库可用后开始广播。
static void ble_service_on_sync(void)
{
    // 确保存在 public 或 random identity 地址。
    int result = ble_hs_util_ensure_addr(0);
    // 地址不可用时记录并等待 NimBLE 重置。
    if (result != 0) {
        // 记录错误码。
        ESP_LOGE(BLE_SERVICE_LOG_TAG, "确保 BLE identity 地址失败 rc=%d", result);
        // 不尝试广播。
        return;
    }
    // 自动选择 public 优先的地址类型。
    result = ble_hs_id_infer_auto(0, &g_ble_state.own_address_type);
    // 推断失败时不能广播。
    if (result != 0) {
        // 记录错误码。
        ESP_LOGE(BLE_SERVICE_LOG_TAG, "推断广播地址类型失败 rc=%d", result);
        // 返回等待故障处理。
        return;
    }
    // identity 地址和 GATT 数据库均已可用；应用任务此后才允许补启丢失的广播。
    g_ble_state.host_synced = UINT8_C(1);
    // 开始广播自定义 128 位服务。
    ble_service_start_advertising();
}

// NimBLE 主机复位回调清空当前连接，防止旧缓存跨主机重启保留。
static void ble_service_on_reset(int reason)
{
    // 记录主机复位原因。
    ESP_LOGE(BLE_SERVICE_LOG_TAG, "NimBLE 主机复位 reason=%d", reason);
    // 主机复位后 identity 地址状态失效，必须等待下一次 sync 回调再允许广播保活。
    g_ble_state.host_synced = UINT8_C(0);
    // 主机复位终止未完成配对，先清除屏幕六位码再重置连接态。
    ble_service_clear_pairing_code(BLE_SERVICE_PAIRING_CLEAR_DISCONNECTED);
    // 保存应用层是否已看到安全连接，主机复位等价于一次断连。
    const uint8_t was_reported = g_ble_state.connection_reported;
    // 标记无连接。
    g_ble_state.connection_handle = BLE_HS_CONN_HANDLE_NONE;
    // 清除安全状态，禁止复位期间继续发布敏感通知。
    g_ble_state.secure_connection = UINT8_C(0);
    // 清除已上报标志，保证下一连接可重新发送 true。
    g_ble_state.connection_reported = UINT8_C(0);
    // 递增连接代次，使业务任务丢弃复位前已经复制的工作项。
    g_ble_state.connection_epoch += UINT32_C(1);
    // 工作队列存在时立即丢弃尚未处理的旧链路分片；业务任务不会在 GAP 回调被等待。
    if (g_ble_state.work_queue != NULL) {
        // xQueueReset 只重置队列索引，不释放队列内存。
        (void)xQueueReset(g_ble_state.work_queue);
    }
    // 恢复默认 MTU。
    g_ble_state.att_mtu = BLE_SERVICE_DEFAULT_ATT_MTU;
    // 清除分片和幂等缓存。
    ble_service_connection_reset(&g_ble_state.connection_core);
    // 清除未完成 indication。
    (void)memset(&g_ble_state.pending_indication, 0, sizeof(g_ble_state.pending_indication));
    // 已上报过安全连接时发送成对 false，使 UI 与电源状态不会停留在已连接。
    if ((was_reported != 0U) && (g_ble_state.configuration.connection_changed != NULL)) {
        // 回调只允许入队；默认 MTU 表示连接已不存在。
        g_ble_state.configuration.connection_changed(
            false,
            BLE_SERVICE_DEFAULT_ATT_MTU,
            g_ble_state.configuration.connection_context);
    }
}

// NimBLE FreeRTOS 主机任务运行到 ble_service_nimble_stop 调用 nimble_port_stop。
static void ble_service_host_task(void *argument)
{
    // 当前实现不使用任务参数。
    (void)argument;
    // 进入 NimBLE 主机事件循环。
    nimble_port_run();
    // 主机循环停止后删除 FreeRTOS 适配任务资源。
    nimble_port_freertos_deinit();
}

// GAP 回调处理单连接、MTU、订阅、安全配对和 indication 分片确认。
static int ble_service_gap_event(struct ble_gap_event *event, void *argument)
{
    // 当前实现不使用回调参数。
    (void)argument;
    // 空事件属于 NimBLE 内部异常。
    if (event == NULL) {
        // 返回 0 避免主机任务崩溃，并依赖日志诊断。
        return 0;
    }
    // 新连接建立或连接尝试失败。
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        // status 0 表示连接建立成功。
        if (event->connect.status == 0) {
            // 保存当前唯一连接句柄。
            g_ble_state.connection_handle = event->connect.conn_handle;
            // 物理连接建立不等于安全会话，等待 ENC_CHANGE 权威事件后再置位。
            g_ble_state.secure_connection = UINT8_C(0);
            // 应用层尚未收到连接 true。
            g_ble_state.connection_reported = UINT8_C(0);
            // 新物理连接使用新代次，旧队列分片即使句柄回绕也会被拒绝。
            g_ble_state.connection_epoch += UINT32_C(1);
            // 协商前使用最小 MTU 23。
            g_ble_state.att_mtu = BLE_SERVICE_DEFAULT_ATT_MTU;
            // 清空尚未由业务任务取得的旧连接分片；重组器由业务任务按 connection_epoch 独占重置。
            if (g_ble_state.work_queue != NULL) {
                // 队列重置不等待业务任务，保持 GAP 回调快速返回。
                (void)xQueueReset(g_ble_state.work_queue);
            }
            // 清除所有订阅状态，等待本连接重新写 CCCD。
            g_ble_state.subscribed_live = UINT8_C(0);
            // 清除 Event 订阅。
            g_ble_state.subscribed_event = UINT8_C(0);
            // 清除 Transfer Data 订阅。
            g_ble_state.subscribed_transfer_data = UINT8_C(0);
            // 清除 Raw Stream 订阅。
            g_ble_state.subscribed_raw = UINT8_C(0);
            // 清除 Battery 订阅。
            g_ble_state.subscribed_battery = UINT8_C(0);
            // 清除未完成 indication。
            (void)memset(&g_ble_state.pending_indication, 0, sizeof(g_ble_state.pending_indication));
            // 主动发起加密、MITM 和绑定；Control/Transfer 写本身也要求 WRITE_ENC。
            const int security_result = ble_gap_security_initiate(event->connect.conn_handle);
            // Windows 可能已用旧绑定先启动加密；EALREADY 表示同一安全流程正在进行，必须等待 ENC_CHANGE。
            if ((security_result != 0) &&
                (security_result != BLE_HS_EALREADY)) {
                // 记录安全错误。
                ESP_LOGE(BLE_SERVICE_LOG_TAG, "发起连接安全失败 rc=%d", security_result);
                // 若上一次配对显示尚未清理，安全发起失败必须立即清码。
                ble_service_clear_pairing_code(BLE_SERVICE_PAIRING_CLEAR_FAILED);
                // 使用本地终止原因断开；断连事件负责清理句柄和恢复广播。
                (void)ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            } else if (security_result == BLE_HS_EALREADY) {
                // 记录正常竞态；不得断开，否则 Windows 服务发现会得到 Unreachable。
                ESP_LOGI(BLE_SERVICE_LOG_TAG, "PC 安全握手已在进行，等待加密状态事件");
            }
            // 这里只记录物理链路；应用层连接状态等安全握手完成后才上报。
            ESP_LOGI(BLE_SERVICE_LOG_TAG, "PC 物理链路已建立，等待安全绑定 handle=%u", (unsigned int)event->connect.conn_handle);
        } else {
            // 连接尝试失败后继续广播。
            ESP_LOGW(BLE_SERVICE_LOG_TAG, "连接失败 status=%d", event->connect.status);
            // 恢复广播。
            ble_service_start_advertising();
        }
        // 事件已处理。
        return 0;
    }
    // 连接断开时清除所有每连接状态并重新广播。
    if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        // 保存断开前是否已向应用报告安全连接，只有 true/false 成对时才发送 false。
        const uint8_t was_reported = g_ble_state.connection_reported;
        // 记录断连原因。
        ESP_LOGI(BLE_SERVICE_LOG_TAG, "PC 已断开 reason=%d", event->disconnect.reason);
        // 物理断线会终止未完成的配对，即使安全连接 true 尚未上报也要清码。
        ble_service_clear_pairing_code(BLE_SERVICE_PAIRING_CLEAR_DISCONNECTED);
        // 标记无连接。
        g_ble_state.connection_handle = BLE_HS_CONN_HANDLE_NONE;
        // 清除加密、认证和绑定运行态。
        g_ble_state.secure_connection = UINT8_C(0);
        // 清除应用上报状态。
        g_ble_state.connection_reported = UINT8_C(0);
        // 递增连接代次，使正在等待应用结果的旧工作项回到任务后自动丢弃响应。
        g_ble_state.connection_epoch += UINT32_C(1);
        // 恢复默认 MTU。
        g_ble_state.att_mtu = BLE_SERVICE_DEFAULT_ATT_MTU;
        // 清空尚未处理的旧链路分片；重组器和幂等缓存由业务任务在新代次独占清理。
        if (g_ble_state.work_queue != NULL) {
            // 不等待任何业务锁，保证 NimBLE 断连回调及时完成。
            (void)xQueueReset(g_ble_state.work_queue);
        }
        // 清除未完成 indication。
        (void)memset(&g_ble_state.pending_indication, 0, sizeof(g_ble_state.pending_indication));
        // 清除所有订阅标志。
        g_ble_state.subscribed_live = UINT8_C(0);
        // 清除 Event 订阅。
        g_ble_state.subscribed_event = UINT8_C(0);
        // 清除 Transfer Data 订阅。
        g_ble_state.subscribed_transfer_data = UINT8_C(0);
        // 清除 Raw Stream 订阅。
        g_ble_state.subscribed_raw = UINT8_C(0);
        // 清除 Battery 订阅。
        g_ble_state.subscribed_battery = UINT8_C(0);
        // 仅服务正常运行时上报意外/远端断连；主动 stop 会在停止前同步上报一次。
        if ((g_ble_state.started != 0U) &&
            (was_reported != 0U) &&
            (g_ble_state.configuration.connection_changed != NULL)) {
            // 断连后 MTU 恢复 23，connected=false 是电源/UI 的权威输入。
            g_ble_state.configuration.connection_changed(
                false,
                BLE_SERVICE_DEFAULT_ATT_MTU,
                g_ble_state.configuration.connection_context);
        }
        // 恢复广播等待 PC 重连。
        ble_service_start_advertising();
        // 事件已处理。
        return 0;
    }
    // 加密状态变化是应用层可用连接的唯一入口；物理连接本身不开放健身数据。
    if (event->type == BLE_GAP_EVENT_ENC_CHANGE) {
        // 忽略陈旧连接的安全事件，避免旧事件覆盖新连接状态。
        if (event->enc_change.conn_handle != g_ble_state.connection_handle) {
            // 事件已安全忽略。
            return 0;
        }
        // 非零状态表示配对、加密或密钥恢复失败，必须立即断开。
        if (event->enc_change.status != 0) {
            // 记录安全失败原因，不输出密钥或用户数据。
            ESP_LOGW(BLE_SERVICE_LOG_TAG, "BLE 安全握手失败 status=%d", event->enc_change.status);
            // 配对失败后六位码立即失效，不等待异步 DISCONNECT 才清理。
            ble_service_clear_pairing_code(BLE_SERVICE_PAIRING_CLEAR_FAILED);
            // 终止当前连接，随后由 DISCONNECT 清理并重新广播。
            (void)ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            // 事件已处理。
            return 0;
        }
        // 查询 NimBLE 权威安全位，拒绝只有加密但没有 MITM 或绑定的降级连接。
        if (!ble_service_connection_security_is_valid(event->enc_change.conn_handle)) {
            // 记录降级拒绝，不泄露具体密钥材料。
            ESP_LOGW(BLE_SERVICE_LOG_TAG, "BLE 链路未同时满足加密、MITM 和绑定，拒绝业务连接");
            // 安全位不完整按失败处理，防止降级连接仍显示旧配对码。
            ble_service_clear_pairing_code(BLE_SERVICE_PAIRING_CLEAR_FAILED);
            // 主动断开不满足产品安全合同的连接。
            (void)ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            // 事件已处理。
            return 0;
        }
        // 标记敏感 GATT 读写和通知可以启用。
        g_ble_state.secure_connection = UINT8_C(1);
        // 加密、MITM 和绑定全部成功，先清除配对码再向应用报告已连接。
        ble_service_clear_pairing_code(BLE_SERVICE_PAIRING_CLEAR_SUCCESS);
        // 首次完成安全握手时向应用层报告连接；重复 ENC_CHANGE 不重复入队。
        if ((g_ble_state.connection_reported == 0U) &&
            (g_ble_state.configuration.connection_changed != NULL)) {
            // 先置位，防止回调间接触发重入时重复报告。
            g_ble_state.connection_reported = UINT8_C(1);
            // 传递当前协商 MTU；应用回调必须快速入队后返回。
            g_ble_state.configuration.connection_changed(
                true,
                g_ble_state.att_mtu,
                g_ble_state.configuration.connection_context);
        }
        // 记录已完成安全绑定，不记录配对密钥。
        ESP_LOGI(BLE_SERVICE_LOG_TAG, "PC 安全绑定连接可用 handle=%u", (unsigned int)event->enc_change.conn_handle);
        // 事件已处理。
        return 0;
    }
    // MTU 更新决定后续逻辑帧的分片容量。
    if (event->type == BLE_GAP_EVENT_MTU) {
        // 只接受当前连接的有效 MTU，最小值必须为 23。
        if ((event->mtu.conn_handle == g_ble_state.connection_handle) &&
            (event->mtu.value >= BLE_SERVICE_DEFAULT_ATT_MTU)) {
            // 保存协商值；正在发送的 indication 已锁定旧 MTU。
            g_ble_state.att_mtu = event->mtu.value;
            // 记录新 MTU。
            ESP_LOGI(BLE_SERVICE_LOG_TAG, "ATT MTU=%u", (unsigned int)event->mtu.value);
        }
        // 事件已处理。
        return 0;
    }
    // CCCD 订阅变化更新定向 notification 开关。
    if (event->type == BLE_GAP_EVENT_SUBSCRIBE) {
        // Live State 只读取 cur_notify。
        if (event->subscribe.attr_handle == g_live_state_handle) {
            // 保存当前通知订阅值。
            g_ble_state.subscribed_live = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == g_event_handle) {
            // 保存 Event 订阅值。
            g_ble_state.subscribed_event = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == g_transfer_data_handle) {
            // 保存 Transfer Data 订阅值。
            g_ble_state.subscribed_transfer_data = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == g_raw_stream_handle) {
            // 保存 Raw Stream 订阅值。
            g_ble_state.subscribed_raw = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == g_battery_handle) {
            // 保存 Battery Level 订阅值。
            g_ble_state.subscribed_battery = event->subscribe.cur_notify;
        }
        // indication 是否订阅由 NimBLE 自身在发送时验证，服务只维护单槽队列。
        return 0;
    }
    // indication 确认或 notification 完成事件。
    if (event->type == BLE_GAP_EVENT_NOTIFY_TX) {
        // 只处理当前活动 indication 对应特征的确认。
        if ((event->notify_tx.indication != 0U) &&
            (g_ble_state.pending_indication.active != 0U) &&
            (event->notify_tx.attr_handle == g_ble_state.pending_indication.value_handle)) {
            // status 0 表示对端已确认当前分片。
            if (event->notify_tx.status == 0) {
                // 若尚有后续片，立即发送下一片；全部完成时函数会清空 active。
                const esp_err_t next_status = ble_service_send_next_indication_fragment();
                // 无后续片时 send_next 返回 ESP_OK；其它错误记录等待 PC 重试。
                if ((next_status != ESP_OK) && (next_status != ESP_ERR_INVALID_STATE)) {
                    // 记录后续片失败。
                    ESP_LOGW(BLE_SERVICE_LOG_TAG, "发送下一 indication 分片失败 err=%s", esp_err_to_name(next_status));
                }
            } else {
                // 确认失败或超时后清空队列，防止阻塞后续响应。
                g_ble_state.pending_indication.active = UINT8_C(0);
                // 记录状态，PC 将用同 request_id 重试。
                ESP_LOGW(BLE_SERVICE_LOG_TAG, "indication 未确认 status=%d", event->notify_tx.status);
            }
        }
        // 事件已处理。
        return 0;
    }
    // 配对过程中显示或确认六位码。
    if (event->type == BLE_GAP_EVENT_PASSKEY_ACTION) {
        // io 保存要注入 NimBLE 安全管理器的用户交互结果。
        struct ble_sm_io io;
        // 清零全部字段。
        (void)memset(&io, 0, sizeof(io));
        // Display Only 模式由设备提供固定六位联调码供 PC 输入。
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            // 读取固定联调 PIN；量产前应恢复随机码或改为设备唯一密钥。
            const uint32_t passkey = BLE_SERVICE_PAIRING_PASSKEY;
            // 设置交互类型。
            io.action = BLE_SM_IOACT_DISP;
            // 写入六位码。
            io.passkey = passkey;
            // 先标记配对码已活动，后续任一错误路径都能发布成对清除。
            g_ble_state.pairing_code_active = UINT8_C(1);
            // 有 LVGL 回调时立即显示六位码。
            if (g_ble_state.configuration.passkey_display != NULL) {
                // 回调必须快速返回，不能阻塞 NimBLE 主机任务。
                g_ble_state.configuration.passkey_display(passkey, g_ble_state.configuration.passkey_context);
            }
            // 把显示码注入安全管理器。
            const int inject_status = ble_sm_inject_io(event->passkey.conn_handle, &io);
            // 注入失败表示主机不会继续使用该码，必须立即清除 UI。
            if (inject_status != 0) {
                // 发布失败原因，应用任务会清零敏感快照。
                ble_service_clear_pairing_code(BLE_SERVICE_PAIRING_CLEAR_FAILED);
            }
            // 原样返回 NimBLE 注入结果。
            return inject_status;
        }
        // Display Only 产品合同不允许 Numeric Comparison；意外进入该模式时必须拒绝，禁止自动同意。
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            // 设置比较交互类型。
            io.action = BLE_SM_IOACT_NUMCMP;
            // 明确拒绝未经实体界面确认的数值比较，阻断中间人降级路径。
            io.numcmp_accept = UINT8_C(0);
            // 产品不支持数值比较，拒绝时同步清理可能残留的显示码。
            ble_service_clear_pairing_code(BLE_SERVICE_PAIRING_CLEAR_FAILED);
            // 把拒绝结果注入安全管理器，随后 ENC_CHANGE 失败路径会断开连接。
            return ble_sm_inject_io(event->passkey.conn_handle, &io);
        }
        // 其它 IO 动作不在 Display Only 产品合同内。
        return 0;
    }
    // Windows 显示未配对而设备仍保留旧密钥时会触发重复配对；按 NimBLE 官方 bleprph 流程仅更新当前电脑绑定。
    if (event->type == BLE_GAP_EVENT_REPEAT_PAIRING) {
        // connection 保存当前物理链路的对端身份地址，删除范围不得扩大到其它绑定设备。
        struct ble_gap_conn_desc connection;
        // 清零结构，避免查询失败后误用未初始化地址。
        (void)memset(&connection, 0, sizeof(connection));
        // 按重复配对事件携带的连接句柄查询当前电脑身份。
        const int find_status = ble_gap_conn_find(event->repeat_pairing.conn_handle, &connection);
        // 无法解析当前电脑时不得删除任意绑定，也不能假装允许重试。
        if (find_status != 0) {
            // 记录稳定错误码，便于区分句柄失效和存储故障。
            ESP_LOGE(BLE_SERVICE_LOG_TAG, "重复配对查询连接失败 rc=%d", find_status);
            // 拒绝本次重复配对；连接断开后广播恢复，用户可重新尝试。
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        // 删除当前电脑的旧 LTK/IRK，使 Windows 与设备端从同一空白安全状态重新绑定。
        const int delete_status = ble_store_util_delete_peer(&connection.peer_id_addr);
        // 存储删除失败时保留旧密钥，禁止进入必然失败的新握手。
        if (delete_status != 0) {
            // 输出删除错误，不记录对端地址或任何密钥材料。
            ESP_LOGE(BLE_SERVICE_LOG_TAG, "重复配对删除旧绑定失败 rc=%d", delete_status);
            // 忽略重试，避免在旧绑定仍存在时形成循环配对。
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        // 旧配对覆盖层若仍活动，先清除后再由新 PASSKEY_ACTION 显示固定 PIN。
        ble_service_clear_pairing_code(BLE_SERVICE_PAIRING_CLEAR_FAILED);
        // 记录恢复路径；固定 PIN 本身只在配对动作回调和设备界面显示。
        ESP_LOGI(BLE_SERVICE_LOG_TAG, "重复配对已删除当前电脑旧绑定，重新开始安全握手");
        // 告诉 NimBLE 立即重试；随后 Display Only 流程注入固定 PIN 123456。
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    // 其它 GAP 事件不需要业务处理。
    return 0;
}

// 初始化 ESP-NimBLE 和完整 GATT 数据库。
esp_err_t ble_service_nimble_start(const ble_service_nimble_config_t *config)
{
    // 配置对象为必填；Manifest 非零长度时也必须提供指针。
    if ((config == NULL) || ((config->manifest_length > 0U) && (config->manifest_payload == NULL))) {
        // 返回参数错误。
        return ESP_ERR_INVALID_ARG;
    }
    // Manifest 最大 512 字节，超出时应改走 Transfer Data。
    if ((size_t)config->manifest_length > BLE_SERVICE_NIMBLE_MAX_MANIFEST_PAYLOAD) {
        // 返回参数错误。
        return ESP_ERR_INVALID_ARG;
    }
    // 初始电量只允许 0～100 或 255 未知。
    if ((config->initial_battery_percent > UINT8_C(100)) &&
        (config->initial_battery_percent != BLE_SERVICE_BATTERY_UNKNOWN)) {
        // 返回参数错误。
        return ESP_ERR_INVALID_ARG;
    }
    // 已启动时禁止重复初始化 NimBLE 端口。
    if (g_ble_state.started != 0U) {
        // 返回无效状态。
        return ESP_ERR_INVALID_STATE;
    }
    // 清空整个单例状态。
    (void)memset(&g_ble_state, 0, sizeof(g_ble_state));
    // 复制回调与上下文指针；字符串和 Manifest 在后续单独深拷贝。
    g_ble_state.configuration = *config;
    // 深拷贝设备名并提供默认值。
    ble_service_copy_string(g_ble_state.device_name, sizeof(g_ble_state.device_name), config->device_name, "BPNN-FIT-0000");
    // 深拷贝厂商名。
    ble_service_copy_string(g_ble_state.manufacturer_name, sizeof(g_ble_state.manufacturer_name), config->manufacturer_name, "IMU Fitness");
    // 深拷贝型号。
    ble_service_copy_string(g_ble_state.model_number, sizeof(g_ble_state.model_number), config->model_number, "ESP32-S3-Touch-AMOLED-2.06");
    // 深拷贝序列号。
    ble_service_copy_string(g_ble_state.serial_number, sizeof(g_ble_state.serial_number), config->serial_number, "UNKNOWN");
    // 深拷贝硬件版本。
    ble_service_copy_string(g_ble_state.hardware_revision, sizeof(g_ble_state.hardware_revision), config->hardware_revision, "WAVESHARE");
    // 深拷贝固件版本。
    ble_service_copy_string(g_ble_state.firmware_revision, sizeof(g_ble_state.firmware_revision), config->firmware_revision, "0.1.0");
    // 保存初始电量。
    g_ble_state.battery_percent = config->initial_battery_percent;
    // 初始无连接。
    g_ble_state.connection_handle = BLE_HS_CONN_HANDLE_NONE;
    // 初始 MTU 为 23。
    g_ble_state.att_mtu = BLE_SERVICE_DEFAULT_ATT_MTU;
    // 冷启动默认快速广播；电源任务随后按 Home/Standby 策略覆盖。
    g_ble_state.power_mode = BLE_SERVICE_NIMBLE_POWER_FAST_ADVERTISING;
    // 清空每连接重组和缓存。
    ble_service_connection_reset(&g_ble_state.connection_core);
    // Manifest 不占用消息类型 1～8；特征 0002 Read 直接返回 TLV，避免误标成 TransferData。
    if (config->manifest_length > 0U) {
        // 深拷贝 Manifest TLV，调用者随后可以释放原缓冲区。
        (void)memcpy(g_ble_state.manifest_frame, config->manifest_payload, config->manifest_length);
    }
    // 保存 Manifest TLV 长度。
    g_ble_state.manifest_frame_length = config->manifest_length;
    // 初始化 NimBLE 控制器和主机端口；NVS 必须由 app_main 先初始化。
    const esp_err_t port_status = nimble_port_init();
    // 端口初始化失败时清空配置并返回。
    if (port_status != ESP_OK) {
        // 清空状态防止错误地认为已启动。
        (void)memset(&g_ble_state, 0, sizeof(g_ble_state));
        // 返回底层错误。
        return port_status;
    }
    // 配置主机复位回调。
    ble_hs_cfg.reset_cb = ble_service_on_reset;
    // 配置主机同步回调。
    ble_hs_cfg.sync_cb = ble_service_on_sync;
    // 配置绑定存储状态轮转回调。
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // Display Only 支持设备显示六位码、PC 输入。
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    // 开启绑定，密钥由 NimBLE 保存到 NVS。
    ble_hs_cfg.sm_bonding = UINT8_C(1);
    // 强制 MITM 保护。
    ble_hs_cfg.sm_mitm = UINT8_C(1);
    // 强制 LE Secure Connections。
    ble_hs_cfg.sm_sc = UINT8_C(1);
    // 本机分发加密密钥和身份解析密钥。
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    // 要求对端分发加密密钥和身份解析密钥。
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    // 注册标准和自定义 GATT 数据库。
    const int gatt_status = ble_service_register_gatt_database();
    // 注册失败时反初始化端口。
    if (gatt_status != 0) {
        // 释放 NimBLE 端口资源。
        (void)nimble_port_deinit();
        // 清空状态。
        (void)memset(&g_ble_state, 0, sizeof(g_ble_state));
        // 返回通用失败。
        return ESP_FAIL;
    }
    // 设置标准 GAP Device Name。
    const int name_status = ble_svc_gap_device_name_set(g_ble_state.device_name);
    // 名称设置失败时反初始化端口。
    if (name_status != 0) {
        // 释放 NimBLE 端口资源。
        (void)nimble_port_deinit();
        // 清空状态。
        (void)memset(&g_ble_state, 0, sizeof(g_ble_state));
        // 返回通用失败。
        return ESP_FAIL;
    }
    // 初始化 NVS 绑定存储模板。
    ble_store_config_init();
    // 创建固定长度业务队列；每项深拷贝一个 ATT 分片，不借用 NimBLE mbuf。
    g_ble_state.work_queue = xQueueCreate(
        BLE_SERVICE_WORK_QUEUE_LENGTH,
        sizeof(ble_service_work_item_t));
    // 队列分配失败时反初始化 NimBLE，禁止退回会阻塞 GATT 回调的同步路径。
    if (g_ble_state.work_queue == NULL) {
        // 释放 NimBLE 端口资源。
        (void)nimble_port_deinit();
        // 清空状态。
        (void)memset(&g_ble_state, 0, sizeof(g_ble_state));
        // 返回内存不足。
        return ESP_ERR_NO_MEM;
    }
    // 启动前清除停止请求。
    g_ble_state.worker_stop_requested = UINT8_C(0);
    // 使用局部句柄接收任务创建结果，避免把 volatile 字段地址传给 FreeRTOS。
    TaskHandle_t worker_handle = NULL;
    // 创建独立 BLE 业务任务；它可以等待应用 broker，但 NimBLE 主机任务始终不等待。
    const BaseType_t worker_created = xTaskCreate(
        ble_service_worker_task,
        "ble_business",
        BLE_SERVICE_WORKER_STACK_BYTES,
        NULL,
        BLE_SERVICE_WORKER_PRIORITY,
        &worker_handle);
    // 任务创建失败时删除队列并反初始化 NimBLE。
    if (worker_created != pdPASS) {
        // 删除启动阶段唯一动态队列。
        vQueueDelete(g_ble_state.work_queue);
        // 防止清理代码再次删除同一队列。
        g_ble_state.work_queue = NULL;
        // 释放 NimBLE 端口资源。
        (void)nimble_port_deinit();
        // 清空状态。
        (void)memset(&g_ble_state, 0, sizeof(g_ble_state));
        // 返回内存不足。
        return ESP_ERR_NO_MEM;
    }
    // 保存已经运行的业务任务句柄，stop 将等待它退出。
    g_ble_state.worker_task = worker_handle;
    // 在启动主机任务前设置 started，使同步回调允许开始广播。
    g_ble_state.started = UINT8_C(1);
    // 创建 NimBLE FreeRTOS 主机任务。
    nimble_port_freertos_init(ble_service_host_task);
    // 初始化和异步广播启动成功。
    return ESP_OK;
}

// 停止并反初始化 NimBLE。
esp_err_t ble_service_nimble_stop(void)
{
    // 未启动时无需停止。
    if (g_ble_state.started == 0U) {
        // 返回无效状态，防止重复 deinit。
        return ESP_ERR_INVALID_STATE;
    }
    // 保存停止前是否已向应用层报告安全连接，用于严格成对发送 false。
    const bool was_connected = g_ble_state.connection_reported != 0U;
    // 停服前强制通知 UI 清除六位码，避免服务状态 memset 后屏幕仍保留敏感信息。
    ble_service_clear_pairing_code(BLE_SERVICE_PAIRING_CLEAR_SERVICE_STOPPED);
    // 先阻止断连回调重新开始广播。
    g_ble_state.started = UINT8_C(0);
    // 主动停止时 GAP 断连事件可能在主机退出后不再分派，因此这里保证一次 false 通知。
    if (was_connected && (g_ble_state.configuration.connection_changed != NULL)) {
        // 回调只允许入队，不能等待 NimBLE 自身停止。
        g_ble_state.configuration.connection_changed(
            false,
            BLE_SERVICE_DEFAULT_ATT_MTU,
            g_ble_state.configuration.connection_context);
    }
    // 请求业务任务停止，不再执行队列中尚未开始的命令或会话请求。
    g_ble_state.worker_stop_requested = UINT8_C(1);
    // 最多等待 1.5 秒，覆盖 500 ms 命令 broker 与两个 500 ms 会话锁的最坏串行等待。
    for (uint32_t waited_ms = UINT32_C(0);
         (g_ble_state.worker_task != NULL) && (waited_ms < UINT32_C(1500));
         waited_ms += BLE_SERVICE_WORKER_POLL_MS) {
        // 短延时让业务任务从队列等待或应用回调返回后自行退出。
        vTaskDelay(pdMS_TO_TICKS(BLE_SERVICE_WORKER_POLL_MS));
    }
    // 超时仍未退出时强制删除，防止关机或 BLE 重启永久卡住。
    if (g_ble_state.worker_task != NULL) {
        // 记录超时，表明应用 broker 或存储锁违反了既定上限。
        ESP_LOGE(BLE_SERVICE_LOG_TAG, "BLE 业务任务停止超时，执行强制删除");
        // 删除仍在运行的任务。
        vTaskDelete((TaskHandle_t)g_ble_state.worker_task);
        // 清空句柄。
        g_ble_state.worker_task = NULL;
    }
    // 有连接时请求远端用户终止原因断开。
    if (g_ble_state.connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        // 异步发起断连；后续主机停止会清理剩余事件。
        (void)ble_gap_terminate(g_ble_state.connection_handle, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        // 无连接时停止可能仍在运行的广播。
        (void)ble_gap_adv_stop();
    }
    // 停止 NimBLE 主机循环。
    const int stop_status = nimble_port_stop();
    // 停止失败时保留底层资源供诊断，不执行危险反初始化。
    if (stop_status != 0) {
        // 返回通用失败。
        return ESP_FAIL;
    }
    // 反初始化 NimBLE 端口；官方 IDF 流程要求先 stop 再 deinit。
    const int deinit_status = nimble_port_deinit();
    // 业务任务已退出，可以删除其固定工作队列。
    if (g_ble_state.work_queue != NULL) {
        // 释放队列控制块和元素存储。
        vQueueDelete(g_ble_state.work_queue);
        // 清空指针，防止诊断误用。
        g_ble_state.work_queue = NULL;
    }
    // 清空业务单例，无论 deinit 结果如何都禁止继续发布。
    (void)memset(&g_ble_state, 0, sizeof(g_ble_state));
    // 返回底层反初始化结果。
    return (deinit_status == 0) ? ESP_OK : ESP_FAIL;
}

// 断开当前 PC 并删除 NimBLE 全部持久绑定。
esp_err_t ble_service_nimble_forget_all_bonds(void)
{
    // 服务未启动时 NimBLE store 和 GAP 不在有效生命周期内。
    if (g_ble_state.started == 0U) {
        // 返回无效状态，不盲目访问已反初始化的存储。
        return ESP_ERR_INVALID_STATE;
    }

    // 配置纯 C 编排器使用 NimBLE 成功码 0 和连接已消失竞态码 BLE_HS_ENOTCONN。
    const ble_service_bond_ops_t operations = {
        // 活动连接先请求 GAP 断开。
        .terminate = ble_service_bond_terminate_adapter,
        // 无条件调用官方 ble_store_clear 删除全部绑定。
        .clear_store = ble_service_bond_clear_store_adapter,
        // 两个适配回调都不需要上下文。
        .context = NULL,
        // NimBLE 所有本路径返回 0 表示成功。
        .success_code = 0,
        // 用户点击同时 PC 断线属正常竞态，仍可继续清除绑定。
        .not_connected_code = BLE_HS_ENOTCONN,
    };
    // 句柄非 NONE 表示当前有物理连接，不要求已完成安全握手。
    const bool connection_active =
        g_ble_state.connection_handle != BLE_HS_CONN_HANDLE_NONE;
    // 保存当前句柄，异步断线回调可在函数返回后清零全局值。
    const uint16_t connection_handle = g_ble_state.connection_handle;
    // 执行“断开+无条件清密钥”编排。
    const ble_service_bond_forget_status_t status = ble_service_bond_forget_all(
        &operations,
        connection_active,
        connection_handle);
    // 用户明确忘记后强制清理配对码，即使底层存储报错也不在屏幕保留旧码。
    ble_service_clear_pairing_code(BLE_SERVICE_PAIRING_CLEAR_FORGOTTEN);
    // 编排成功时记录不包含地址或密钥的结果。
    if (status == BLE_SERVICE_BOND_FORGET_OK) {
        // 日志说明新 PC 可以重新发起六位码配对。
        ESP_LOGI(BLE_SERVICE_LOG_TAG, "已断开当前 PC 并删除全部 BLE 绑定");
        // 返回成功。
        return ESP_OK;
    }
    // 记录稳定编排错误码，不输出任何密钥或 PC 身份。
    ESP_LOGE(BLE_SERVICE_LOG_TAG, "忘记电脑失败 status=%d", (int)status);
    // 返回通用失败，设置页应给出中文错误提示并允许重试。
    return ESP_FAIL;
}

// 查询当前是否存在已加密、MITM 认证并绑定的唯一业务连接。
bool ble_service_nimble_is_connected(void)
{
    // started 和 secure_connection 共同防止物理链路或清空后的默认句柄被误判为可用业务连接。
    return (g_ble_state.started != 0U) &&
        (g_ble_state.connection_handle != BLE_HS_CONN_HANDLE_NONE) &&
        (g_ble_state.secure_connection != 0U) &&
        ble_service_connection_security_is_valid(g_ble_state.connection_handle);
}

// 查询 ControlResponse 或 TransferResponse 是否仍有 indication 分片等待 PC 确认。
bool ble_service_nimble_reliable_response_pending(void)
{
    // 未启动时没有可靠响应；active 非零表示至少一片正在等待确认或尚待发送。
    return (g_ble_state.started != 0U) &&
        (g_ble_state.pending_indication.active != 0U);
}

// 读取 BLE 生命周期快照；各字段为独立原子宽度读，允许主机回调在读取期间推进下一状态。
esp_err_t ble_service_nimble_get_runtime_status(ble_service_nimble_runtime_status_t *status)
{
    // 调用方必须提供可写对象，函数不保存该指针。
    if (status == NULL) {
        // 空地址不能返回任何诊断事实。
        return ESP_ERR_INVALID_ARG;
    }
    // started 只表示服务对象已创建，不代表主机完成同步。
    status->started = g_ble_state.started != 0U;
    // host_synced 来自 NimBLE sync/reset 回调，是广播 API 可用的前置条件。
    status->host_synced = g_ble_state.host_synced != 0U;
    // 只有服务已启动且主机完成同步后 GAP 上下文才存在；启动失败时必须短路为 false。
    status->advertising = status->started && status->host_synced &&
        (ble_gap_adv_active() != 0);
    // 物理连接只检查句柄；安全握手尚未完成时仍为 true。
    status->physical_connection = g_ble_state.connection_handle != BLE_HS_CONN_HANDLE_NONE;
    // 安全连接位只在加密、MITM 和绑定全部确认后置位。
    status->secure_connection = g_ble_state.secure_connection != 0U;
    // 复制当前射频策略，调用方可区分主动 OFF 和异常无广播。
    status->power_mode = g_ble_state.power_mode;
    // 快照已完整写入调用方对象。
    return ESP_OK;
}

// 应用生产 BLE 射频策略；广播重启和连接参数更新均使用 NimBLE 公共 GAP API。
esp_err_t ble_service_nimble_set_power_mode(const ble_service_nimble_power_mode_t mode)
{
    // 服务未启动时没有可配置控制器或 GAP 状态。
    if (g_ble_state.started == 0U) {
        // 返回无效状态，调用方可以保留离线模式而不崩溃。
        return ESP_ERR_INVALID_STATE;
    }
    // 枚举必须位于公开合同范围，防止未初始化整数写入 GAP 参数。
    if ((mode < BLE_SERVICE_NIMBLE_POWER_OFF) ||
        (mode > BLE_SERVICE_NIMBLE_POWER_CONNECTED_MODEM_SLEEP)) {
        // 返回参数错误。
        return ESP_ERR_INVALID_ARG;
    }
    // 保存新策略，使异步断连或主机同步回调使用同一目标模式。
    g_ble_state.power_mode = mode;
    // OFF 策略先停止广播，或在有连接时请求断开。
    if (mode == BLE_SERVICE_NIMBLE_POWER_OFF) {
        // 当前存在物理连接时主动终止；DISCONNECT 因 power_mode=OFF 不会恢复广播。
        if (g_ble_state.connection_handle != BLE_HS_CONN_HANDLE_NONE) {
            // 使用本地用户终止原因，PC 可区分正常省电断连。
            const int terminate_status = ble_gap_terminate(
                g_ble_state.connection_handle,
                BLE_ERR_REM_USER_CONN_TERM);
            // 零表示断连请求已进入主机队列。
            return terminate_status == 0 ? ESP_OK : ESP_FAIL;
        }
        // 无连接且正在广播时停止射频；未广播时已经满足目标。
        if (ble_gap_adv_active() != 0) {
            // 停止现有快/慢广播。
            const int stop_status = ble_gap_adv_stop();
            // 映射 NimBLE 整数错误为 ESP-IDF 稳定结果。
            return stop_status == 0 ? ESP_OK : ESP_FAIL;
        }
        // 当前已经关闭。
        return ESP_OK;
    }
    // 有连接时只有两种连接策略需要提交参数更新；广播策略仅保存为断连后回退值。
    if (g_ble_state.connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        // 广播枚举在竞态连接期间到达时不打断安全业务连接。
        if ((mode != BLE_SERVICE_NIMBLE_POWER_CONNECTED_ACTIVE) &&
            (mode != BLE_SERVICE_NIMBLE_POWER_CONNECTED_MODEM_SLEEP)) {
            // 已保存目标，等断连回调选择对应快慢广播。
            return ESP_OK;
        }
        // 构造 BLE 规范连接参数；全部单位由结构字段固定。
        struct ble_gap_upd_params parameters;
        // 清零连接事件长度等可选字段，让控制器自行选择。
        (void)memset(&parameters, 0, sizeof(parameters));
        // 活动模式使用 15 ms，待机模式使用 50 ms 下界。
        parameters.itvl_min = mode == BLE_SERVICE_NIMBLE_POWER_CONNECTED_ACTIVE
            ? BLE_SERVICE_ACTIVE_CONN_INTERVAL_MIN
            : BLE_SERVICE_STANDBY_CONN_INTERVAL_MIN;
        // 活动模式使用 30 ms，待机模式使用 100 ms 上界。
        parameters.itvl_max = mode == BLE_SERVICE_NIMBLE_POWER_CONNECTED_ACTIVE
            ? BLE_SERVICE_ACTIVE_CONN_INTERVAL_MAX
            : BLE_SERVICE_STANDBY_CONN_INTERVAL_MAX;
        // 活动模式零延迟，待机模式允许跳过四个连接事件。
        parameters.latency = mode == BLE_SERVICE_NIMBLE_POWER_CONNECTED_ACTIVE
            ? BLE_SERVICE_ACTIVE_CONN_LATENCY
            : BLE_SERVICE_STANDBY_CONN_LATENCY;
        // 监督超时分别为 4 s 与 6 s，均满足间隔和延迟约束。
        parameters.supervision_timeout = mode == BLE_SERVICE_NIMBLE_POWER_CONNECTED_ACTIVE
            ? BLE_SERVICE_ACTIVE_SUPERVISION_TIMEOUT
            : BLE_SERVICE_STANDBY_SUPERVISION_TIMEOUT;
        // 请求连接参数更新；Windows 可在协议允许范围内协商其它值。
        const int update_status = ble_gap_update_params(
            g_ble_state.connection_handle,
            &parameters);
        // 零表示参数请求已提交；最终采用值由 CONN_UPDATE 事件和诊断页观察。
        return update_status == 0 ? ESP_OK : ESP_FAIL;
    }
    // 无连接时需要重启现有广播，才能立即应用新的快慢间隔。
    if (ble_gap_adv_active() != 0) {
        // 停止旧参数广播；失败时不启动第二份广播。
        if (ble_gap_adv_stop() != 0) {
            // 返回失败供电源任务记录。
            return ESP_FAIL;
        }
    }
    // 使用刚保存的 power_mode 重新构建广播参数并启动。
    ble_service_start_advertising();
    // NimBLE 广播 API 由 helper 记录详细错误；active 位作为最终事实检查。
    return ble_gap_adv_active() != 0 ? ESP_OK : ESP_FAIL;
}

// 确认无连接设备仍保持可连接广播；用于现场联调发现异常停止，不改变正式功耗状态机目标。
esp_err_t ble_service_nimble_ensure_advertising(void)
{
    // 服务未启动或主机尚未同步时没有合法 GAP 广播上下文。
    if ((g_ble_state.started == 0U) || (g_ble_state.host_synced == 0U)) {
        // 返回状态错误；调用方下一轮可以在 sync 完成后重试。
        return ESP_ERR_INVALID_STATE;
    }
    // 已有物理连接时不应同时发起可连接广播；安全连接状态由 GAP 事件继续推进。
    if (g_ble_state.connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        // 已连接即满足“可被上位机使用”的目标。
        return ESP_OK;
    }
    // OFF 是电源状态机的明确目标，保活函数不得越权重新打开射频。
    if (g_ble_state.power_mode == BLE_SERVICE_NIMBLE_POWER_OFF) {
        // 返回状态错误，要求调用方先恢复快/慢广播策略。
        return ESP_ERR_INVALID_STATE;
    }
    // GAP 已报告广播活动时不重启，避免每秒产生广播间隙或额外射频操作。
    if (ble_gap_adv_active() != 0) {
        // 当前广播健康，无需访问控制器。
        return ESP_OK;
    }
    // 使用当前快/慢策略重新构建广告包和扫描响应；不会修改设备名、服务 UUID 或连接参数。
    ble_service_start_advertising();
    // 以 NimBLE 活动位作为恢复事实，不能只凭 void helper 调用判成功。
    if (ble_gap_adv_active() != 0) {
        // 输出一次恢复日志，串口可区分正常持续广播和联调保活介入。
        ESP_LOGW(
            BLE_SERVICE_LOG_TAG,
            "广播保活已恢复 name=%s mode=%d",
            g_ble_state.device_name,
            (int)g_ble_state.power_mode);
        // 返回恢复成功。
        return ESP_OK;
    }
    // 广告字段或控制器调用失败时 helper 已输出具体 NimBLE 错误码。
    return ESP_FAIL;
}

// 更新 Battery Level 并在已订阅时发送标准单字节 notification。
esp_err_t ble_service_nimble_set_battery_percent(uint8_t battery_percent)
{
    // 服务必须已启动。
    if (g_ble_state.started == 0U) {
        // 返回无效状态。
        return ESP_ERR_INVALID_STATE;
    }
    // 电量范围只允许 0～100 或 255 未知。
    if ((battery_percent > UINT8_C(100)) && (battery_percent != BLE_SERVICE_BATTERY_UNKNOWN)) {
        // 返回参数错误。
        return ESP_ERR_INVALID_ARG;
    }
    // 保存最新电量供标准 Read 和后续 LiveState 构造者读取。
    g_ble_state.battery_percent = battery_percent;
    // 无连接或未订阅时只更新本地值。
    if ((g_ble_state.connection_handle == BLE_HS_CONN_HANDLE_NONE) || (g_ble_state.subscribed_battery == 0U)) {
        // 本地更新成功。
        return ESP_OK;
    }
    // 分配一个字节 Battery Level mbuf。
    struct os_mbuf *const packet = ble_hs_mbuf_from_flat(&battery_percent, sizeof(battery_percent));
    // 内存池耗尽时返回内存不足。
    if (packet == NULL) {
        // 返回内存不足。
        return ESP_ERR_NO_MEM;
    }
    // 发送标准 Battery Level notification。
    const int notify_status = ble_gatts_notify_custom(g_ble_state.connection_handle, g_battery_handle, packet);
    // 转换 NimBLE 返回值。
    return (notify_status == 0) ? ESP_OK : ESP_FAIL;
}

// 发布 LiveStateV1 权威状态。
esp_err_t ble_service_nimble_publish_live_state(const ble_service_live_state_v1_t *state)
{
    // 服务和状态对象均必须有效。
    if ((g_ble_state.started == 0U) || (state == NULL)) {
        // 区分服务未启动和空参数。
        return (state == NULL) ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    // 固定 30 字节 payload。
    uint8_t payload[BLE_SERVICE_LIVE_STATE_V1_SIZE];
    // 保存编码长度。
    size_t payload_length = 0U;
    // 编码字段和范围。
    const ble_service_status_t encode_status = ble_service_encode_live_state_v1(
        state,
        payload,
        sizeof(payload),
        &payload_length);
    // 非法动作、电量或目标值不得发送。
    if (encode_status != BLE_SERVICE_STATUS_OK) {
        // 返回参数错误。
        return ESP_ERR_INVALID_ARG;
    }
    // 编码类型 3 完整逻辑帧，保存最新 Read 快照，并按订阅状态通知。
    return ble_service_publish_notification_message(
        (uint8_t)BLE_SERVICE_MESSAGE_LIVE_STATE,
        g_live_state_handle,
        g_ble_state.subscribed_live,
        payload,
        (uint16_t)payload_length,
        ble_service_monotonic_ms(),
        g_ble_state.live_state_frame,
        sizeof(g_ble_state.live_state_frame),
        &g_ble_state.live_state_frame_length);
}

// 发布类型 4 Event。
esp_err_t ble_service_nimble_publish_event(
    const uint8_t *payload,
    uint16_t payload_length,
    const uint32_t monotonic_ms)
{
    // 服务必须已启动。
    if (g_ble_state.started == 0U) {
        // 返回无效状态。
        return ESP_ERR_INVALID_STATE;
    }
    // 编码并通知事件，不保存为权威状态。
    return ble_service_publish_notification_message(
        (uint8_t)BLE_SERVICE_MESSAGE_EVENT,
        g_event_handle,
        g_ble_state.subscribed_event,
        payload,
        payload_length,
        monotonic_ms,
        NULL,
        0U,
        NULL);
}

// 发布类型 7 TransferData。
esp_err_t ble_service_nimble_publish_transfer_data(const uint8_t *payload, uint16_t payload_length)
{
    // 服务必须已启动。
    if (g_ble_state.started == 0U) {
        // 返回无效状态。
        return ESP_ERR_INVALID_STATE;
    }
    // 编码并通知会话摘要或日志数据块。
    return ble_service_publish_notification_message(
        (uint8_t)BLE_SERVICE_MESSAGE_TRANSFER_DATA,
        g_transfer_data_handle,
        g_ble_state.subscribed_transfer_data,
        payload,
        payload_length,
        ble_service_monotonic_ms(),
        NULL,
        0U,
        NULL);
}

// 发布类型 8 RawStream。
esp_err_t ble_service_nimble_publish_raw_stream(const uint8_t *payload, uint16_t payload_length)
{
    // 服务必须已启动。
    if (g_ble_state.started == 0U) {
        // 返回无效状态。
        return ESP_ERR_INVALID_STATE;
    }
    // 编码并通知开发者原始六轴流。
    return ble_service_publish_notification_message(
        (uint8_t)BLE_SERVICE_MESSAGE_RAW_STREAM,
        g_raw_stream_handle,
        g_ble_state.subscribed_raw,
        payload,
        payload_length,
        ble_service_monotonic_ms(),
        NULL,
        0U,
        NULL);
}

// 发布类型 9 InferenceDiagnosticV1；分类诊断与六轴样本共享开发者 Raw Stream 特征。
esp_err_t ble_service_nimble_publish_inference_diagnostic(
    const uint8_t *payload,
    uint16_t payload_length)
{
    // 服务必须已启动，否则没有有效连接、句柄或序号状态。
    if (g_ble_state.started == 0U) {
        // 返回无效状态，调用方不得把未发布数据当作 PC 已收到。
        return ESP_ERR_INVALID_STATE;
    }
    // 编码类型 9 并走与 RawStream 相同的安全订阅；通知丢失不会阻塞下一推理窗口。
    return ble_service_publish_notification_message(
        (uint8_t)BLE_SERVICE_MESSAGE_INFERENCE_DIAGNOSTIC,
        g_raw_stream_handle,
        g_ble_state.subscribed_raw,
        payload,
        payload_length,
        ble_service_monotonic_ms(),
        NULL,
        0U,
        NULL);
}
