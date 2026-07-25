/*
 * ESP32-S3 健身识别手柄生产入口。
 *
 * 线程所有权：QMI 任务只读取芯片并投递原始帧；应用任务独占 imu_pipeline 和
 * device_coordinator；UI、BLE、存储和电源任务只消费按值效果。该边界保证
 * 次数、步数、热量只由 workout_engine 产生一次，公式与任务关系见
 * docs/手柄与上位机软件详细设计.md。
 */

/* 引入 Waveshare 板级运行时，提供 BSP 显示、I2C、TF 与 LVGL 锁。 */
#include "board_runtime.h"
/* 引入 QMI8658、AXP2101、PCF85063 独立驱动。 */
#include "board_sensors.h"
/* 引入 ESP-NimBLE 服务、LiveState、Event 和控制回调合同。 */
#include "ble_service_nimble.h"
/* 引入正式 Manifest TLV 构建器、标签、能力位和版本合同。 */
#include "ble_service_manifest.h"
/* 引入训练控制、UI/BLE/存储/电源统一效果。 */
#include "device_coordinator.h"
/* 引入 Cmd6/7/8/9/11 TLV、配置事务和 CRC32 稳定 blob。 */
#include "device_config.h"
/* 引入严格 25 Hz 重采样、62 点窗口和质量位。 */
#include "imu_pipeline.h"
/* 引入自动生成 297 维双六分支 M0 推理适配器。 */
#include "imu_pipeline_dual_m0.h"
/* 直接读取自动生成的特征维度、类别顺序和两个 M0 SHA-256，禁止手工复制哈希。 */
#include "../../include/esp32_dual_m0_model.h"
/* 引入双槽会话摘要和固定容量文件后端。 */
#include "session_store.h"
/* 引入 BLE LIST/GET 会话补传服务。 */
#include "session_transfer.h"
/* 引入 LVGL 9 页面渲染器和触摸按钮命令。 */
#include "ui_lvgl_renderer.h"
/* 引入 NimBLE 到应用任务的无阻塞配对码原子邮箱。 */
#include "ui_app_pairing.h"

/* 引入 GPIO 唤醒接口；GPIO21 用于 Deep-sleep，GPIO38/39 仅用于 Light-sleep。 */
#include "driver/gpio.h"
/* 引入 ESP-IDF 统一错误码。 */
#include "esp_err.h"
#include "esp_check.h"
/* 引入设备唯一蓝牙 MAC 读取接口。 */
#include "esp_mac.h"
/* 引入串口日志。 */
#include "esp_log.h"
/* 引入 40~240 MHz 动态频率和自动 Light-sleep 配置。 */
#include "esp_pm.h"
/* 引入 Deep-sleep 入口。 */
#include "esp_sleep.h"
/* 引入片内堆统计；BLE 控制器启动前记录总空闲量和最大连续块。 */
#include "esp_heap_caps.h"
/* 引入芯片复位原因接口；现场隔离版在 ready 日志保留上一次复位来源。 */
#include "esp_system.h"
/* 引入单调微秒时钟。 */
#include "esp_timer.h"
/* 引入 LittleFS VFS 挂载。 */
#include "esp_littlefs.h"
/* 引入 FreeRTOS 基础类型和毫秒换算。 */
#include "freertos/FreeRTOS.h"
/* 引入按值队列。 */
#include "freertos/queue.h"
/* 引入互斥锁与二值信号量。 */
#include "freertos/semphr.h"
/* 引入多任务创建、延时和删除。 */
#include "freertos/task.h"
/* 引入可指定 PSRAM/片内能力的 ESP-IDF 任务创建接口。 */
#include "freertos/idf_additions.h"
/* 引入 NVS 初始化；NimBLE 绑定密钥依赖 NVS。 */
#include "nvs.h"
/* 引入 NVS 分区初始化；NimBLE 绑定密钥依赖 NVS。 */
#include "nvs_flash.h"

/* 引入 bool。 */
#include <stdbool.h>
/* 引入定长整数。 */
#include <stdint.h>
/* 引入 snprintf。 */
#include <stdio.h>
/* 引入 memset 和 memcpy。 */
#include <string.h>

/* 固定应用日志标签。 */
static const char *const APP_TAG = "imu_handheld";
/*
 * 阻止编译器把事件边界函数重新内联到 app_event_task。
 * app_event_task 的 FreeRTOS 栈固定为 16 KiB；协调器事务本身需要约 8.4 KiB 候选副本。
 * 若 QMI、UI、BLE 三条高层处理链再次内联，Xtensa 会为所有互斥分支一次性预留约 9.5 KiB，
 * START 或首个采样再嵌套协调器时会耗尽余量，可能触发栈溢出并整机复位。
 * 该属性只改变调用边界，不改变任务、时序、业务状态、模型或显示链。
 */
#define APP_STACK_BOUNDARY __attribute__((noinline))
/* 应用任务最深静态链约 10.9 KiB；16 KiB 保留约 5 KiB 中断与库调用余量。 */
#define APP_OWNER_TASK_STACK_BYTES (16U * 1024U)
/* UI 渲染任务使用 8 KiB 栈；该任务不执行 Flash/NVS 写入，可安全放入板载 PSRAM。 */
#define APP_UI_TASK_STACK_BYTES (8U * 1024U)
/* LittleFS VFS 根目录固定为 /littlefs。 */
#define APP_LITTLEFS_BASE_PATH "/littlefs"
/* 双槽摘要文件固定放在内部 Flash LittleFS。 */
#define APP_SESSION_FILE_PATH APP_LITTLEFS_BASE_PATH "/sessions.bin"
/* 设备偏好使用独立 NVS 命名空间，避免与 NimBLE bond 键冲突。 */
#define APP_CONFIG_NVS_NAMESPACE "fitness"
/* CRC32 配置 blob 固定存放在 device_cfg 键。 */
#define APP_CONFIG_NVS_KEY "device_cfg"
/* QMI 活动时每 4 ms 查询一次 DATA_READY，覆盖 125 Hz 与 112.1 Hz 两路 ODR。 */
#define APP_QMI_POLL_MS UINT32_C(4)
/* 电池每 30 秒刷新一次；启动阶段另读一次。 */
#define APP_BATTERY_POLL_MS UINT32_C(30000)
/* 空闲任务每 1 秒查询一次单调计时器，误差上限约 1 秒且不会高频唤醒 CPU。 */
#define APP_IDLE_POLL_MS UINT32_C(1000)
/* 默认 30 秒无操作熄屏；设备偏好命令可在 10~300 秒内覆盖。 */
#define APP_DEFAULT_SCREEN_TIMEOUT_MS UINT32_C(30000)
/* 无活动会话时连续 10 分钟无操作进入 Deep-sleep。 */
#define APP_LONG_IDLE_MS UINT32_C(600000)
/* 当前现场联调固件固定常亮：true 时禁止自动熄屏、自动 Light-sleep 和长空闲 Deep-sleep；用户主动关机仍有效。 */
#define APP_BENCH_ALWAYS_ON (true)
/* Cmd11 成功应答后延迟 750 ms 开始通知，避免高频 RawStream 抢占 Control Point indication。 */
#define APP_RAW_STREAM_ACTIVATION_DELAY_MS UINT32_C(750)
/* 当前功能联调版恢复 BLE 射频，用于真机配对、RawStream、LiveState 和分类结果验证。 */
#define APP_BENCH_DISABLE_BLE (false)
/* 当前功能联调版恢复首个 HOME 电源策略，使 QMI 在开始训练后可切换到 ACTIVE 采样。 */
#define APP_BENCH_SKIP_INITIAL_POWER_POLICY (false)
/* 显示重影已单独登记；当前退出显示-only，恢复 NVS、传感器、模型、任务和 BLE 产品链。 */
#define APP_BENCH_DISPLAY_ONLY (false)
/* 传感器-only 仍保留为回退工具，但本轮执行完整功能链。 */
#define APP_BENCH_SENSOR_ONLY (false)
/* 自检页至少保留 350 ms，使用户能看到启动阶段而不会明显拖慢开机。 */
#define APP_SELF_TEST_HOLD_MS UINT32_C(350)
/* 1001 表示板载 QMI8658、AXP2101 或 PCF85063 初始化未达到继续运行条件。 */
#define APP_STARTUP_FAULT_SENSOR INT32_C(1001)
/* 1002 表示 LittleFS 或双槽会话仓储不能安全挂载和恢复。 */
#define APP_STARTUP_FAULT_STORAGE INT32_C(1002)
/* 1003 表示协调器、IMU 流水线或模型运行域初始化失败。 */
#define APP_STARTUP_FAULT_DOMAIN INT32_C(1003)
/* 1004 表示 BLE 启动后无法为全部业务任务分配完整栈。 */
#define APP_STARTUP_FAULT_TASK_MEMORY INT32_C(1004)
/* NimBLE 控制回调等待应用任务提交事务的上限。 */
/* 设置命令包含一次 NVS commit；异步 BLE worker 最多等待 1.5 秒，不阻塞 NimBLE GAP 回调。 */
#define APP_BLE_COMMAND_TIMEOUT_MS UINT32_C(1500)
/* NimBLE 控制器异步启动后最多等待 2 秒完成主机同步和可连接广播。 */
#define APP_BLE_READY_TIMEOUT_MS UINT32_C(2000)
/* 每 20 ms 查询一次 BLE 快照；共最多 100 次，不忙等且不阻塞 NimBLE 主机任务。 */
#define APP_BLE_READY_POLL_MS UINT32_C(20)
/* 控制请求固定头之外最多 250 字节 TLV，和 ble_service_core 合同一致。 */
#define APP_BLE_COMMAND_MAX_TLV UINT16_C(250)
/* RawStream v1 固定为 sample index、单调毫秒、六轴 int16 和质量位，共 22 字节。 */
#define APP_RAW_STREAM_V1_SIZE UINT16_C(22)
/* 0x0210 表示设置 TLV 畸形、缺项或越界。 */
#define APP_BLE_ERROR_CONFIG_TLV UINT16_C(0x0210)
/* 0x0211 表示 NVS 配置未成功 commit。 */
#define APP_BLE_ERROR_CONFIG_PERSIST UINT16_C(0x0211)
/* 0x0212 表示 PCF85063 校时写入失败。 */
#define APP_BLE_ERROR_RTC_WRITE UINT16_C(0x0212)
/* 0x0213 表示未开启开发者模式却请求 RawStream。 */
#define APP_BLE_ERROR_DEVELOPER_REQUIRED UINT16_C(0x0213)
/* 0x0214 表示资料或偏好 revision 旧于设备当前事实。 */
#define APP_BLE_ERROR_STALE_REVISION UINT16_C(0x0214)
/* 会话传输响应 indication 后至少等待 100 ms 再发第一条 notification。 */
#define APP_TRANSFER_PUMP_MS UINT32_C(100)
/* 主应用事件队列容量，覆盖短时 QMI 帧突发及 UI/BLE 事件。 */
#define APP_EVENT_QUEUE_LENGTH UINT32_C(48)
/* UI 控制邮箱只保留最新一次触摸命令；长度一允许 xQueueOverwrite 永不因数据队列满而丢失。 */
#define APP_UI_COMMAND_QUEUE_LENGTH UINT32_C(1)
/* UI 只需要最新完整快照，长度固定为 1 并使用 overwrite。 */
#define APP_UI_QUEUE_LENGTH UINT32_C(1)
/* 电源只需要最新策略，长度固定为 1。 */
#define APP_POWER_QUEUE_LENGTH UINT32_C(1)
/* BLE 输出允许缓存 LiveState 与 Event 短突发。 */
#define APP_BLE_QUEUE_LENGTH UINT32_C(16)
/* 摘要队列覆盖动作事件和停止事务。 */
#define APP_STORAGE_QUEUE_LENGTH UINT32_C(12)

/* 应用事件类型；所有可能修改 coordinator 的输入都在同一队列串行执行。 */
typedef enum app_event_kind {
    /* 一帧 QMI 异步加速度/角速度原始数据。 */
    APP_EVENT_QMI_FRAME = 0,
    /* QMI 读取或事件队列溢出，流水线必须重置连续窗。 */
    APP_EVENT_QMI_DROP,
    /* LVGL 按钮产生的用户命令。 */
    APP_EVENT_UI_COMMAND,
    /* NimBLE 控制命令 broker 请求。 */
    APP_EVENT_BLE_COMMAND,
    /* PC BLE 连接或断开。 */
    APP_EVENT_BLE_CONNECTION,
    /* NimBLE 配对码邮箱有新快照；应用任务负责安全分派到 LVGL。 */
    APP_EVENT_PAIRING_UPDATE,
    /* AXP2101 电量与充电状态更新。 */
    APP_EVENT_BATTERY,
    /* 生产空闲时钟每秒请求一次熄屏或长空闲门槛检查。 */
    APP_EVENT_IDLE_POLL,
    /* 关键摘要刷盘结果，用于授权或拒绝 PMIC 关机。 */
    APP_EVENT_STORAGE_RESULT
} app_event_kind_t;

/* 存储完成负载。 */
typedef struct app_storage_result {
    /* true 表示 session_store_upsert 已完成双槽同步。 */
    bool success;
    /* true 表示本次成功后必须执行 PMIC 关机。 */
    bool authorize_shutdown;
} app_storage_result_t;

/* 电池事件负载。 */
typedef struct app_battery_event {
    /* AXP2101 SOC，范围 0~100。 */
    uint8_t percent;
    /* true 表示 VBUS/PMIC 当前处于充电方向。 */
    bool charging;
} app_battery_event_t;

/* 跨任务应用事件；最大成员为一帧 QMI，按值复制后不借用驱动缓冲。 */
typedef struct app_event {
    /* 指明联合体有效成员。 */
    app_event_kind_t kind;
    /* 保存事件源捕获的单调毫秒。 */
    uint64_t monotonic_ms;
    /* 保存互斥事件负载。 */
    union {
        /* QMI 原始帧。 */
        board_qmi8658_frame_t qmi_frame;
        /* LVGL 命令。 */
        ui_command_t ui_command;
        /* BLE 连接状态。 */
        bool ble_connected;
        /* 电池状态。 */
        app_battery_event_t battery;
        /* 关键存储结果。 */
        app_storage_result_t storage_result;
    } data;
} app_event_t;

/* UI 控制邮箱负载；不复制 QMI 联合体，降低片内队列内存并保留触摸原始时刻。 */
typedef struct app_ui_command_event {
    /* 保存 LVGL 点击发生的设备单调毫秒，用于协调器事件排序。 */
    uint64_t monotonic_ms;
    /* 保存当前页面已经校验过的用户命令枚举。 */
    ui_command_t command;
} app_ui_command_event_t;

/* BLE 输出类型。 */
typedef enum app_ble_output_kind {
    /* 发布完整权威 LiveStateV1。 */
    APP_BLE_OUTPUT_LIVE_STATE = 0,
    /* 发布已经编码的 36 字节 EventV1 payload。 */
    APP_BLE_OUTPUT_EVENT,
    /* 发布开发者 25 Hz 六轴原始码诊断流；不重传、不落盘。 */
    APP_BLE_OUTPUT_RAW_STREAM,
    /* 发布阶段一双 M0 与融合分类诊断；复用 Raw Stream 安全通道。 */
    APP_BLE_OUTPUT_INFERENCE_DIAGNOSTIC
} app_ble_output_kind_t;

/* BLE 输出队列项。 */
typedef struct app_ble_output {
    /* 指明 live_state 或 event_payload 有效。 */
    app_ble_output_kind_t kind;
    /* 保存权威快照。 */
    ble_service_live_state_v1_t live_state;
    /* 保存固定 36 字节事件 payload。 */
    uint8_t event_payload[BLE_SERVICE_EVENT_V1_SIZE];
    /* 保存事件有效长度，当前固定为 36。 */
    uint16_t event_length;
    /* 保存原始 MetricEvent 单调毫秒低 32 位，禁止用 BLE 任务实际发送时刻替代。 */
    uint32_t event_monotonic_ms;
    /* 保存 RawStream v1 固定 22 字节 payload。 */
    uint8_t raw_payload[APP_RAW_STREAM_V1_SIZE];
    /* 保存 RawStream 有效长度，当前固定为 22。 */
    uint16_t raw_length;
    /* 保存 InferenceDiagnosticV1 固定 28 字节 payload。 */
    uint8_t inference_payload[BLE_SERVICE_INFERENCE_DIAGNOSTIC_V1_SIZE];
    /* 保存分类诊断有效长度，当前固定为 28。 */
    uint16_t inference_length;
} app_ble_output_t;

/* 摘要存储队列项。 */
typedef struct app_storage_request {
    /* 保存需幂等写入的 64 字节内存摘要。 */
    session_summary_t summary;
    /* true 表示成功刷盘后授权 PMIC 断电。 */
    bool shutdown_after_persist;
} app_storage_request_t;

/* 电源任务队列项。 */
typedef struct app_power_request {
    /* 保存协调器生成的完整电源策略。 */
    power_policy_t policy;
    /* true 表示摘要已刷盘，允许执行 policy 中的 PMIC 关机。 */
    bool persist_authorized;
    /* true 表示本请求只覆盖 QMI 等本地外设，禁止重新提交当前 BLE 连接参数。 */
    bool preserve_ble_link;
} app_power_request_t;

/* BLE 同步 broker 的单槽状态；NimBLE v1 只有一个 PC 且 GATT 写串行。 */
typedef struct app_ble_command_slot {
    /* 保存请求 ID。 */
    uint32_t request_id;
    /* 保存命令 ID 1~11。 */
    uint8_t command_id;
    /* 保存命令版本，当前必须为 1。 */
    uint8_t command_version;
    /* 深拷贝控制请求 TLV；NimBLE worker 返回前后均不借用 mbuf 内存。 */
    uint8_t tlv[APP_BLE_COMMAND_MAX_TLV];
    /* 保存深拷贝 TLV 有效长度，范围 0~250。 */
    uint16_t tlv_length;
    /* 保存应用任务生成的稳定响应。 */
    ble_service_command_result_t result;
} app_ble_command_slot_t;

/* 传感器集合；external_ops 在 board_runtime 初始化前即可指向本静态对象。 */
typedef struct app_sensor_hub {
    /* 保存 ESP-IDF 共用 I2C 设备适配器。 */
    board_sensors_esp_idf_i2c_t i2c;
    /* 保存 QMI8658 驱动状态。 */
    board_qmi8658_t qmi;
    /* 保存 AXP2101 驱动状态。 */
    board_axp2101_t axp;
    /* 保存 PCF85063 驱动状态。 */
    board_pcf85063_t rtc;
    /* 标记 QMI 轮询任务是否允许访问芯片。 */
    volatile bool qmi_sampling_enabled;
    /* 串行化 QMI 数据读取与 ACTIVE/WOM/OFF 寄存器切换；句柄由启动阶段创建。 */
    SemaphoreHandle_t qmi_mutex;
    /* 标记全部独立传感器驱动已完成初始化。 */
    bool initialized;
} app_sensor_hub_t;

/* 板级运行时生命周期覆盖全部任务。 */
static board_runtime_t s_board_runtime;
/* 保存显示初始化后的板级探测事实，后续传感器自检只读该快照。 */
static board_runtime_diagnostics_t s_board_diagnostics;
/* 独立传感器驱动生命周期覆盖全部任务。 */
static app_sensor_hub_t s_sensors;
/* IMU 流水线约 5.5 KiB，必须位于静态区而不是任务栈。 */
static imu_pipeline_t s_imu_pipeline;
/* 保存最近一次同步双 M0 中间诊断；流水线应用任务是唯一写者和发布者。 */
static imu_pipeline_dual_m0_diagnostics_t s_dual_m0_diagnostics;
/* 应用协调器含训练引擎和状态机，只有应用任务允许修改。 */
static device_coordinator_t s_coordinator;
/* 生产空闲计时器只由应用任务修改，空闲任务只投递查询事件。 */
static power_idle_timer_t s_power_idle_timer;
/* 当前熄屏门槛单位毫秒；启动使用 30 秒，加载偏好后覆盖。 */
static uint32_t s_screen_timeout_ms = APP_DEFAULT_SCREEN_TIMEOUT_MS;
/* 保存从 NVS 恢复或默认生成的完整设备配置；应用任务是运行期唯一写者。 */
static device_config_t s_device_config;
/* RawStream 是断线即关闭的易失诊断开关；volatile 允许 BLE 发布任务快速丢弃关闭后的排队帧。 */
static volatile bool s_raw_stream_enabled;
/* true 表示 Cmd11 已通过但仍在等待控制应答离开 GATT 队列；仅应用任务读写。 */
static bool s_raw_stream_activation_pending;
/* 保存允许开始发布的单调微秒门槛；与 QMI 帧 timestamp_us 使用同一 esp_timer 时间基准。 */
static uint64_t s_raw_stream_activation_due_us;
/* 每个 25 Hz 重采样点递增一次，uint32 溢出按协议自然回绕。 */
static uint32_t s_raw_sample_index;
/* 会话仓储含最近 200 条摘要，静态分配避免堆碎片。 */
static session_store_t s_session_store;
/* LittleFS 固定容量 stdio 后端。 */
static session_file_backend_t s_session_file_backend;
/* 会话 LIST/GET 补传服务。 */
static session_transfer_service_t s_session_transfer;
/* LVGL 页面渲染器。 */
static ui_lvgl_renderer_t s_ui_renderer;
/* NimBLE 主机任务到应用任务的最新配对码/清除事件邮箱；不保存动态指针。 */
static ui_app_pairing_mailbox_t s_pairing_mailbox;
/* 保存 BLE 命令同步 broker 单槽。 */
static app_ble_command_slot_t s_ble_command_slot;
/* 保存长期有效的 BLE 配置字符串。 */
static char s_ble_device_name[32];
/* 保存设备序列号。 */
static char s_ble_serial_number[32];
/* 板卡型号同时用于标准 Device Information，生命周期覆盖 BLE 服务。 */
static const char s_ble_model_number[] = "ESP32-S3-AMOLED-2.06";
/* 板卡修订同时进入标准 Device Information 和 Manifest。 */
static const char s_ble_hardware_revision[] = "2.06";
/* 固件语义版本同时进入标准 Device Information 和 Manifest。 */
static const char s_ble_firmware_revision[] = "0.1.0";
/* 正式 Manifest 最多 512 字节；NimBLE start 会复制有效部分。 */
static uint8_t s_ble_manifest[BLE_SERVICE_NIMBLE_MAX_MANIFEST_PAYLOAD];
/* 生成特征维度必须能无损写入 Manifest 的 u16LE 字段。 */
_Static_assert(FEATURE_DIM <= UINT16_MAX, "FEATURE_DIM exceeds Manifest u16");
/* 生成类别数量必须能无损写入 Manifest 的 u8 字段。 */
_Static_assert(CLASS_NUM <= UINT8_MAX, "CLASS_NUM exceeds Manifest u8");
/* 主应用事件队列。 */
static QueueHandle_t s_app_event_queue;
/* UI 控制单槽邮箱；与 125 Hz QMI 数据队列隔离。 */
static QueueHandle_t s_ui_command_queue;
/* 汇合主数据队列和 UI 控制邮箱，使应用任务无轮询地等待任一输入。 */
static QueueSetHandle_t s_app_input_queue_set;
/* UI 最新快照队列。 */
static QueueHandle_t s_ui_queue;
/* BLE 输出队列。 */
static QueueHandle_t s_ble_queue;
/* 摘要写入队列。 */
static QueueHandle_t s_storage_queue;
/* 电源策略队列。 */
static QueueHandle_t s_power_queue;
/* BLE 命令单槽互斥锁。 */
static SemaphoreHandle_t s_ble_command_mutex;
/* BLE 命令应用完成信号。 */
static SemaphoreHandle_t s_ble_command_done;
/* 会话仓储互斥锁，保护存储任务写和 BLE 传输读。 */
static SemaphoreHandle_t s_session_store_mutex;
/* 会话传输服务互斥锁，保护 NimBLE handler 与 BLE pump。 */
static SemaphoreHandle_t s_session_transfer_mutex;
/* 双 M0 推理 PM 锁；仅在 297 特征与两个网络前向期间请求 CPU 最高频率。 */
static esp_pm_lock_handle_t s_inference_pm_lock;
/* 电源任务独占保存最近成功应用的 BLE 模式；-1 表示尚未向 NimBLE 提交。 */
static int s_applied_ble_power_mode = -1;

/* 声明 ESP-IDF 应用入口。 */
void app_main(void);

/*
 * 把完整设备配置编码成固定 44 字节、带 CRC32 的稳定 blob，并同步写入 NVS。
 * config 指向应用任务拥有的候选配置；调用期间必须非空且保持有效，函数不保存该指针。
 * 返回 ESP_OK 表示配置已进入非易失存储；其它错误表示编码、打开 NVS、写入或提交失败。
 */
static esp_err_t app_persist_device_config(const device_config_t *config);

/* 返回当前单调毫秒。 */
static uint64_t app_now_ms(void)
{
    /* ESP 定时器返回单调微秒，除以 1000 得毫秒。 */
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

/* 返回当前单调微秒。 */
static uint64_t app_now_us(void)
{
    /* esp_timer 在正常设备寿命内不会达到有符号 64 位上限。 */
    return (uint64_t)esp_timer_get_time();
}

/* 使用 ESP-IDF CPU 最高频率锁包装完整 297 特征提取和双 M0 前向。 */
static int app_pm_locked_dual_m0_infer(
    void *context,
    const float window[IMU_PIPELINE_WINDOW_SAMPLES][IMU_PIPELINE_AXIS_COUNT],
    float logits[IMU_PIPELINE_CLASS_COUNT])
{
    /* 捕获完整特征提取和双模型前向开始时间，单位为设备单调微秒。 */
    const uint64_t started_us = app_now_us();
    /* 启动阶段必须成功创建锁；空锁表示软件初始化合同被破坏。 */
    if (s_inference_pm_lock == NULL) {
        /* 返回负值，流水线会给窗口增加 INFERENCE_FAILED 质量位。 */
        return -2;
    }
    /* 获取 ESP_PM_CPU_FREQ_MAX 锁，保证 DFT、自相关和双 M0 在稳定高频下完成。 */
    const esp_err_t acquire_status = esp_pm_lock_acquire(s_inference_pm_lock);
    /* 锁获取失败时不运行可能超时的推理。 */
    if (acquire_status != ESP_OK) {
        /* 记录错误供诊断，避免输出未计算 logits。 */
        ESP_LOGE(APP_TAG, "inference PM lock acquire failed=%s", esp_err_to_name(acquire_status));
        /* 返回负值。 */
        return -3;
    }
    /* 在锁持有期同步执行特征提取、基础 M0、掩码 M0 和固定 logits 融合。 */
    const int inference_status = imu_pipeline_dual_m0_infer(context, window, logits);
    /* 无论推理成功或失败都释放锁，避免设备永久停留 240 MHz。 */
    const esp_err_t release_status = esp_pm_lock_release(s_inference_pm_lock);
    /* 释放失败表示 PM 锁计数异常，记录但保留真实推理返回码供流水线诊断。 */
    if (release_status != ESP_OK) {
        /* 输出 ESP-IDF 错误。 */
        ESP_LOGE(APP_TAG, "inference PM lock release failed=%s", esp_err_to_name(release_status));
    }
    /* 非空上下文按适配器合同指向双 M0 诊断对象，应用包装器负责补充端到端耗时。 */
    if (context != NULL) {
        /* 恢复长期持有的诊断对象；本次同步调用结束后仍由应用任务独占。 */
        imu_pipeline_dual_m0_diagnostics_t *const diagnostics =
            (imu_pipeline_dual_m0_diagnostics_t *)context;
        /* 计算无符号单调耗时；正常设备运行不会发生 64 位回绕。 */
        const uint64_t elapsed_us = app_now_us() - started_us;
        /* 线上字段为 u32 微秒，超过约 71 分钟时饱和而不回绕成小值。 */
        diagnostics->inference_time_us = elapsed_us > UINT32_MAX
            ? UINT32_MAX
            : (uint32_t)elapsed_us;
    }
    /* 返回双 M0 原始状态；零表示本窗口 11 类 logits 有效。 */
    return inference_status;
}

/* 将跨任务捕获时间夹到协调器已提交时间，消除多生产者排队造成的微小乱序。 */
static uint64_t app_coordinator_time_ms(const uint64_t captured_ms)
{
    /* 相同毫秒被协调器允许；较旧事件使用最近提交时刻。 */
    return captured_ms < s_coordinator.last_monotonic_ms
               ? s_coordinator.last_monotonic_ms
               : captured_ms;
}

/* 判断当前状态是否接受完整 62 点训练推理；窗口自身已经保证约 2.48 秒连续样本。 */
static bool app_training_accepts_inference(void)
{
    /* 只有准备或运行状态属于用户主动开始的训练，Idle 诊断不发布动作判断。 */
    if ((s_coordinator.workout.state != WORKOUT_STATE_PREPARING) &&
        (s_coordinator.workout.state != WORKOUT_STATE_RUNNING)) {
        /* 空闲、暂停或总结状态不允许训练分类。 */
        return false;
    }
    /* 首个完整窗口立即进入有界累计确认，不叠加固定倒计时；最多再等待三个重叠窗。 */
    return true;
}

/* 饱和 uint64 到 uint32。 */
static uint32_t app_u64_to_u32(const uint64_t value)
{
    /* 超出线上字段范围时固定为 UINT32_MAX。 */
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

/* 把 microkcal 转为 0.001 kcal，使用整数截断且饱和。 */
static uint32_t app_microkcal_to_mcal(const uint64_t microkcal)
{
    /* 1000 microkcal 等于 1 mcal。 */
    return app_u64_to_u32(microkcal / UINT64_C(1000));
}

/* 把 u16 按协议小端写入至少两个字节。 */
static void app_write_u16_le(uint8_t *output, const uint16_t value)
{
    /* 写低八位。 */
    output[0] = (uint8_t)(value & UINT16_C(0x00FF));
    /* 写高八位。 */
    output[1] = (uint8_t)((value >> 8U) & UINT16_C(0x00FF));
}

/* 把 int16 的二补码位模式按小端写入。 */
static void app_write_i16_le(uint8_t *output, const int16_t value)
{
    /* 使用 memcpy 获取标准 int16 位模式，避免有符号右移。 */
    uint16_t raw = UINT16_C(0);
    /* 复制两个字节。 */
    (void)memcpy(&raw, &value, sizeof(raw));
    /* 复用无符号小端写入。 */
    app_write_u16_le(output, raw);
}

/* 把 u32 按协议小端写入至少四个字节。 */
static void app_write_u32_le(uint8_t *output, const uint32_t value)
{
    /* 写位 0~7。 */
    output[0] = (uint8_t)(value & UINT32_C(0x000000FF));
    /* 写位 8~15。 */
    output[1] = (uint8_t)((value >> 8U) & UINT32_C(0x000000FF));
    /* 写位 16~23。 */
    output[2] = (uint8_t)((value >> 16U) & UINT32_C(0x000000FF));
    /* 写位 24~31。 */
    output[3] = (uint8_t)((value >> 24U) & UINT32_C(0x000000FF));
}

/* 把滤波/重采样后的物理量反量化为 QMI int16 码，供开发诊断流显示。 */
static int16_t app_quantize_raw_axis(const float physical_value, const float units_per_lsb)
{
    /* 比例由流水线固定为正数；防御式拒绝零或负比例。 */
    if (!(units_per_lsb > 0.0F)) {
        /* 返回零，避免除零。 */
        return INT16_C(0);
    }
    /* 转换为传感器原始码浮点值。 */
    const float scaled = physical_value / units_per_lsb;
    /* 正方向超量程饱和到 int16 最大值。 */
    if (scaled >= 32767.0F) {
        /* 返回最大码。 */
        return INT16_MAX;
    }
    /* 负方向超量程饱和到 int16 最小值。 */
    if (scaled <= -32768.0F) {
        /* 返回最小码。 */
        return INT16_MIN;
    }
    /* 正数加 0.5、负数减 0.5 后向零截断，实现对称四舍五入。 */
    const float rounded = scaled >= 0.0F ? scaled + 0.5F : scaled - 0.5F;
    /* 范围已钳制，转换不会溢出。 */
    return (int16_t)rounded;
}

/* AXP 外部回调：读取 0~100 SOC 与 VBUS 外部供电状态。 */
static int app_sensor_read_battery(void *context, uint8_t *percent, bool *charging)
{
    /* 检查静态 hub 和两个输出指针。 */
    app_sensor_hub_t *hub = (app_sensor_hub_t *)context;
    /* 未初始化或空输出不能读。 */
    if ((hub == NULL) || !hub->initialized || (percent == NULL) || (charging == NULL)) {
        /* 返回非零供 board_runtime 映射为 I/O 错误。 */
        return -1;
    }
    /* 读取同一时刻 PMIC 快照。 */
    board_axp2101_status_t status;
    /* I2C 或范围错误时拒绝输出旧值。 */
    if (board_axp2101_read_status(&hub->axp, &status) != BOARD_SENSORS_OK) {
        /* 返回失败。 */
        return -1;
    }
    /* 输出燃料计百分比。 */
    *percent = status.soc_percent;
    /* 输出 VBUS good；即使电池充满停止充电，USB 接入时仍阻止长空闲 Deep-sleep。 */
    *charging = status.vbus_good;
    /* 返回成功。 */
    return 0;
}

/* QMI 外部回调：串行切换活动采样、21 Hz WOM 或完全关闭寄存器模式。 */
static int app_sensor_set_qmi_mode(void *context, const board_runtime_qmi_mode_t mode)
{
    /* 恢复长期 sensor hub。 */
    app_sensor_hub_t *hub = (app_sensor_hub_t *)context;
    /* 未初始化、无互斥锁或模式越界时拒绝。 */
    if ((hub == NULL) || !hub->initialized || (hub->qmi_mutex == NULL) ||
        ((mode != BOARD_RUNTIME_QMI_OFF) && (mode != BOARD_RUNTIME_QMI_ACTIVE) &&
         (mode != BOARD_RUNTIME_QMI_WAKE_ON_MOTION))) {
        /* 返回失败。 */
        return -1;
    }
    /* 先关闭轮询门控，使尚未取得互斥锁的 QMI 任务在锁内二次检查后退出。 */
    hub->qmi_sampling_enabled = false;
    /* 等待当前 I2C 数据读取结束；模式切换必须独占同一 QMI 寄存器集合。 */
    if (xSemaphoreTake(hub->qmi_mutex, portMAX_DELAY) != pdTRUE) {
        /* FreeRTOS 异常时拒绝切换。 */
        return -1;
    }
    /* 默认结果为参数错误；显式分支保证每个 runtime 枚举都映射到一个驱动 API。 */
    board_sensors_result_t result = BOARD_SENSORS_ERR_ARGUMENT;
    /* 按产品策略切换真实芯片寄存器。 */
    switch (mode) {
        /* 活动模式恢复训练部署的双传感器量程和 ODR。 */
        case BOARD_RUNTIME_QMI_ACTIVE:
            /* 调用 ACTIVE 恢复函数。 */
            result = board_qmi8658_set_active(&hub->qmi);
            /* 结束该分支。 */
            break;
        /* WOM 使用固定 40 mg 阈值和 4 个 21 Hz 消隐样本。 */
        case BOARD_RUNTIME_QMI_WAKE_ON_MOTION:
            /* 调用 WOM 配置并完成 CTRL9 握手。 */
            result = board_qmi8658_set_wake_on_motion(
                &hub->qmi,
                BOARD_QMI8658_WOM_THRESHOLD_MG,
                BOARD_QMI8658_WOM_BLANKING_SAMPLES);
            /* 结束该分支。 */
            break;
        /* OFF 清零 WOM 阈值并关闭加速度与陀螺。 */
        case BOARD_RUNTIME_QMI_OFF:
            /* 调用完全关闭函数。 */
            result = board_qmi8658_power_down(&hub->qmi);
            /* 结束该分支。 */
            break;
        /* 上方参数检查已经拒绝越界值；保留 default 作为防御式安全关闭。 */
        default:
            /* 保持参数错误。 */
            result = BOARD_SENSORS_ERR_ARGUMENT;
            /* 结束该分支。 */
            break;
    }
    /* 模式寄存器事务结束，释放 QMI 独占锁。 */
    (void)xSemaphoreGive(hub->qmi_mutex);
    /* 只有完整 ACTIVE 配置成功后才重新允许 4 ms 数据轮询。 */
    hub->qmi_sampling_enabled = (result == BOARD_SENSORS_OK) &&
                                (mode == BOARD_RUNTIME_QMI_ACTIVE);
    /* 返回零表示模式与驱动状态一致；非零会阻止 Deep-sleep。 */
    return result == BOARD_SENSORS_OK ? 0 : -1;
}

/* RTC 外部回调：读取 UTC Unix 秒。 */
static int app_sensor_read_rtc(void *context, uint64_t *unix_seconds)
{
    /* 恢复 hub 并检查输出。 */
    app_sensor_hub_t *hub = (app_sensor_hub_t *)context;
    /* 参数或状态无效时拒绝。 */
    if ((hub == NULL) || !hub->initialized || (unix_seconds == NULL)) {
        /* 返回失败。 */
        return -1;
    }
    /* 核心 RTC 使用有符号 Unix 秒。 */
    int64_t signed_seconds = 0;
    /* RTC VL 位或 I2C 错误时拒绝。 */
    if (board_pcf85063_read_unix(&hub->rtc, &signed_seconds) != BOARD_SENSORS_OK) {
        /* 返回失败。 */
        return -1;
    }
    /* 负 Unix 时间不属于产品支持范围。 */
    if (signed_seconds < 0) {
        /* 返回失败。 */
        return -1;
    }
    /* 安全转换为无符号输出。 */
    *unix_seconds = (uint64_t)signed_seconds;
    /* 返回成功。 */
    return 0;
}

/* PMIC 外部回调：执行 AXP2101 软关机。 */
static int app_sensor_request_shutdown(void *context)
{
    /* 恢复 hub。 */
    app_sensor_hub_t *hub = (app_sensor_hub_t *)context;
    /* 未初始化不能关机。 */
    if ((hub == NULL) || !hub->initialized) {
        /* 返回失败。 */
        return -1;
    }
    /* 写 AXP COMMON_CONFIG.bit0；调用方已保证摘要刷盘。 */
    return board_axp2101_request_shutdown(&hub->axp) == BOARD_SENSORS_OK ? 0 : -1;
}

/* 将 UI 完整快照投递给 LVGL 任务；队列满时覆盖旧帧。 */
static void app_queue_ui(const ui_context_t *ui)
{
    /* 空指针或队列未创建时不访问。 */
    if ((ui == NULL) || (s_ui_queue == NULL)) {
        /* 安全返回。 */
        return;
    }
    /* 长度 1 队列只保留最新状态。 */
    (void)xQueueOverwrite(s_ui_queue, ui);
}

/* 将权威 LiveState 投递给 BLE 任务。 */
static void app_queue_live_state(const ble_service_live_state_v1_t *state)
{
    /* 检查输入和队列。 */
    if ((state == NULL) || (s_ble_queue == NULL)) {
        /* 安全返回。 */
        return;
    }
    /* 构造按值消息。 */
    app_ble_output_t output;
    /* 清零事件缓冲等无效字段。 */
    (void)memset(&output, 0, sizeof(output));
    /* 标记 LiveState。 */
    output.kind = APP_BLE_OUTPUT_LIVE_STATE;
    /* 复制权威状态。 */
    output.live_state = *state;
    /* 队列短时满时丢旧中间态，后续修订会重新发布。 */
    if (xQueueSend(s_ble_queue, &output, 0U) != pdPASS) {
        /* 记录诊断；不阻塞应用任务。 */
        ESP_LOGW(APP_TAG, "BLE LiveState queue full revision=%lu", (unsigned long)state->state_revision);
    }
}

/* 编码并排队一个 25 Hz RawStream v1 样本；普通产品模式完全不产生该流。 */
static void app_queue_raw_sample(
    const imu_resampled_sample_t *sample,
    const uint32_t sample_index)
{
    /* 只有开发者模式、显式流开关、安全连接和有效队列同时成立才发布。 */
    if ((sample == NULL) || !s_device_config.developer_mode ||
        !s_raw_stream_enabled || !ble_service_nimble_is_connected() ||
        (s_ble_queue == NULL)) {
        /* 不产生缓存或持久化。 */
        return;
    }
    /* 构造按值队列项。 */
    app_ble_output_t output;
    /* 清零其它消息字段。 */
    (void)memset(&output, 0, sizeof(output));
    /* 标记 RawStream。 */
    output.kind = APP_BLE_OUTPUT_RAW_STREAM;
    /* 偏移 0 写自启动递增序号。 */
    app_write_u32_le(&output.raw_payload[0], sample_index);
    /* 偏移 4 写单调毫秒低 32 位，约 49.7 天自然回绕。 */
    app_write_u32_le(&output.raw_payload[4], (uint32_t)(sample->timestamp_us / UINT64_C(1000)));
    /* 遍历固定 gx、gy、gz、ax、ay、az 六轴顺序。 */
    for (uint8_t axis = 0U; axis < IMU_PIPELINE_AXIS_COUNT; ++axis) {
        /* 前三轴使用 deg/s/LSB，后三轴使用 g/LSB。 */
        const float scale = axis < 3U
            ? BOARD_QMI8658_GYRO_DPS_PER_LSB
            : BOARD_QMI8658_ACCEL_G_PER_LSB;
        /* 反量化滤波后的物理值，诊断流不冒充未滤波芯片 FIFO。 */
        const int16_t raw_value = app_quantize_raw_axis(sample->axes[axis], scale);
        /* 每轴占 2 字节，从偏移 8 开始连续写入。 */
        app_write_i16_le(&output.raw_payload[8U + (size_t)axis * 2U], raw_value);
    }
    /* 偏移 20 写质量位低 16 位；完整内部位图仍保留在设备诊断统计。 */
    app_write_u16_le(&output.raw_payload[20], (uint16_t)(sample->quality_flags & UINT16_MAX));
    /* 固定线上长度。 */
    output.raw_length = APP_RAW_STREAM_V1_SIZE;
    /* RawStream 不可靠且不重传；队列满时直接丢当前诊断点，避免阻塞识别。 */
    (void)xQueueSend(s_ble_queue, &output, 0U);
}

/* 编码并排队一个双 M0 分类诊断；只有开发者 RawStream 会话可见。 */
static void app_queue_inference_diagnostic(const imu_pipeline_inference_result_t *result)
{
    /* 分类诊断沿用开发者、显式 RawStream、安全连接和有效队列四重门槛。 */
    if ((result == NULL) || !s_device_config.developer_mode ||
        !s_raw_stream_enabled || !ble_service_nimble_is_connected() ||
        (s_ble_queue == NULL)) {
        /* 普通产品运行不计算额外协议帧，也不落盘。 */
        return;
    }
    /* 取得流水线只读统计；对象生命周期覆盖整个应用。 */
    const imu_pipeline_stats_t *const stats = imu_pipeline_get_stats(&s_imu_pipeline);
    /* 本窗口成功且适配器诊断有效时发布真实三路结果，否则动作使用未知值。 */
    const bool model_valid = (result->inference_status == 0) &&
        (s_dual_m0_diagnostics.valid != UINT8_C(0));
    /* 组装固定宽度线上对象；所有累计与时间字段使用设备事实。 */
    const ble_service_inference_diagnostic_v1_t diagnostic = {
        /* 当前分类诊断布局固定为版本一。 */
        .diagnostic_version = UINT8_C(1),
        /* 融合 Top-1 仅在本窗口成功时有效。 */
        .fused_action_id = model_valid
            ? s_dual_m0_diagnostics.fused_action_id
            : BLE_SERVICE_ACTION_UNKNOWN,
        /* 基础 M0 Top-1 仅在本窗口成功时有效。 */
        .base_action_id = model_valid
            ? s_dual_m0_diagnostics.base_action_id
            : BLE_SERVICE_ACTION_UNKNOWN,
        /* 掩码 M0 Top-1 仅在本窗口成功时有效。 */
        .masked_action_id = model_valid
            ? s_dual_m0_diagnostics.masked_action_id
            : BLE_SERVICE_ACTION_UNKNOWN,
        /* 失败窗口不沿用上次融合置信度。 */
        .fused_confidence_q15 = model_valid
            ? s_dual_m0_diagnostics.fused_confidence_q15
            : UINT16_C(0),
        /* 失败窗口不沿用上次基础模型置信度。 */
        .base_confidence_q15 = model_valid
            ? s_dual_m0_diagnostics.base_confidence_q15
            : UINT16_C(0),
        /* 失败窗口不沿用上次掩码模型置信度。 */
        .masked_confidence_q15 = model_valid
            ? s_dual_m0_diagnostics.masked_confidence_q15
            : UINT16_C(0),
    /* 低 16 位保留窗口预热、插值、旧执行器质量位和推理失败等事实。 */
        .quality_flags = (uint16_t)(result->quality_flags & UINT16_MAX),
        /* 窗口序号允许 u32 自然回绕。 */
        .window_sequence = result->sequence,
        /* 单调微秒换算毫秒后取低 32 位，与 RawStream 时间合同一致。 */
        .window_end_ms = (uint32_t)(result->end_timestamp_us / UINT64_C(1000)),
        /* 耗时由持有 CPU 最高频率锁的包装器现场测量。 */
        .inference_time_us = s_dual_m0_diagnostics.inference_time_us,
        /* 统计指针理论上非空；防御分支在异常初始化时返回零而不解引用。 */
        .failure_count = stats != NULL ? stats->inference_failures : UINT32_C(0),
    };
    /* 创建按值 BLE 队列项，避免借用栈上诊断对象。 */
    app_ble_output_t output;
    /* 清除 LiveState、Event 和 RawStream 无效字段。 */
    (void)memset(&output, 0, sizeof(output));
    /* 标记类型九分类诊断。 */
    output.kind = APP_BLE_OUTPUT_INFERENCE_DIAGNOSTIC;
    /* 编码器使用 size_t 返回固定 payload 长度。 */
    size_t encoded_length = 0U;
    /* 按固定小端偏移编码三路模型事实。 */
    const ble_service_status_t encode_status = ble_service_encode_inference_diagnostic_v1(
        &diagnostic,
        output.inference_payload,
        sizeof(output.inference_payload),
        &encoded_length);
    /* 编码失败或长度漂移时拒绝发布，避免 PC 错位解析。 */
    if ((encode_status != BLE_SERVICE_STATUS_OK) ||
        (encoded_length != BLE_SERVICE_INFERENCE_DIAGNOSTIC_V1_SIZE)) {
        /* 输出稳定错误码和实际长度供串口定位协议回归。 */
        ESP_LOGE(APP_TAG, "encode inference diagnostic failed status=%d length=%u",
                 (int)encode_status,
                 (unsigned int)encoded_length);
        /* 不排入坏 payload。 */
        return;
    }
    /* 固定线上长度安全收窄到队列 u16 字段。 */
    output.inference_length = (uint16_t)encoded_length;
    /* 分类通知不重传；队列满时丢当前窗口，下一窗口会给出新事实。 */
    (void)xQueueSend(s_ble_queue, &output, 0U);
}

/* 把一条领域 MetricEvent 和同修订 LiveState 转成固定 EventV1 并投递。 */
static void app_queue_one_metric_event(
    const fitness_metric_event_t *metric_event,
    const ble_service_live_state_v1_t *live_state)
{
    /* 事件、权威状态和 BLE 输出队列都必须有效。 */
    if ((metric_event == NULL) || (live_state == NULL) || (s_ble_queue == NULL)) {
        /* 安全返回。 */
        return;
    }
    /* 组装协议事件；PC 只用它驱动即时动画，不二次累计。 */
    const ble_service_event_v1_t event = {
        /* 当前事件结构版本固定为 1。 */
        .event_version = 1U,
        /* 指标事件统一使用 REPETITION_COUNTED，metric_kind 区分次/步/秒。 */
        .event_type = BLE_SERVICE_EVENT_REPETITION_COUNTED,
        /* 使用效果同修订的设备状态。 */
        .device_state = live_state->device_state,
        /* 使用领域事件动作。 */
        .action_id = (uint8_t)metric_event->action,
        /* fitness 0~2 映射 BLE 1~3。 */
        .metric_kind = (uint8_t)metric_event->metric_kind + 1U,
        /* 使用同修订电量。 */
        .battery_percent = live_state->battery_percent,
        /* 传播质量位。 */
        .quality_flags = metric_event->quality_flags,
        /* 使用会话序号。 */
        .session_sequence = metric_event->session_seq,
        /* 使用会话内幂等事件号。 */
        .event_sequence = metric_event->event_seq,
        /* 使用协调器权威修订号。 */
        .state_revision = live_state->state_revision,
        /* 写入本次增量。 */
        .metric_delta = metric_event->delta_value,
        /* 饱和写入当前累计。 */
        .metric_total = app_u64_to_u32(metric_event->total_value),
        /* microkcal 转 mcal。 */
        .calories_mcal = app_microkcal_to_mcal(metric_event->gross_microkcal),
        /* 直接携带领域 Q15 稳定度。 */
        .confidence_q15 = metric_event->stability_q15,
        /* 普通指标事件没有额外原因。 */
        .detail_code = 0U,
    };
    /* 构造队列消息。 */
    app_ble_output_t output;
    /* 清零全部字段。 */
    (void)memset(&output, 0, sizeof(output));
    /* 标记 Event。 */
    output.kind = APP_BLE_OUTPUT_EVENT;
    /* 编码函数使用 size_t 输出长度。 */
    size_t encoded_length = 0U;
    /* 编码固定小端 36 字节。 */
    const ble_service_status_t status = ble_service_encode_event_v1(
        &event,
        output.event_payload,
        sizeof(output.event_payload),
        &encoded_length);
    /* 编码失败表示内部合同错误，只记录而不发送半帧。 */
    if ((status != BLE_SERVICE_STATUS_OK) || (encoded_length != BLE_SERVICE_EVENT_V1_SIZE)) {
        /* 输出精确错误。 */
        ESP_LOGE(APP_TAG, "EventV1 encode failed status=%d len=%u", (int)status, (unsigned int)encoded_length);
        /* 返回。 */
        return;
    }
    /* 保存线上长度。 */
    output.event_length = (uint16_t)encoded_length;
    /* 保存领域事件原始时刻；预锁定样本回放产生的计数仍必须指向历史 IMU 点。 */
    output.event_monotonic_ms = (uint32_t)metric_event->monotonic_ms;
    /* Event 队列满时不重算指标；LiveState 与摘要仍是权威恢复源。 */
    if (xQueueSend(s_ble_queue, &output, 0U) != pdPASS) {
        /* 记录丢失。 */
        ESP_LOGW(APP_TAG, "BLE Event queue full event=%lu", (unsigned long)event.event_sequence);
    }
}

/* 把普通单事件或锁类补算事件序列按 event_seq 顺序投递到 BLE。 */
static void app_queue_metric_events(const device_effects_t *effects)
{
    /* effect 和 BLE 队列必须有效；调用方已检查 BLE_EVENT 标志。 */
    if ((effects == NULL) || (s_ble_queue == NULL)) {
        /* 安全返回。 */
        return;
    }
    /* 补算数组非空表示本次锁类回放形成了多条历史计数事件。 */
    if (effects->replay_metric_event_count > 0U) {
        /* 按协调器给出的严格递增 event_seq 遍历全部有效事件。 */
        for (uint8_t index = 0U; index < effects->replay_metric_event_count; ++index) {
            /* 每条事件使用同一最终锁类修订号，但保留各自原始 IMU 时间和累计值。 */
            app_queue_one_metric_event(
                &effects->replay_metric_events[index],
                &effects->live_state);
        }
        /* 补算序列已完整投递，禁止再发送未置位的普通单事件槽。 */
        return;
    }
    /* 普通运行期 effect 只含一个实时 MetricEvent。 */
    app_queue_one_metric_event(&effects->metric_event, &effects->live_state);
}

/* 把电源策略投递给电源任务，并显式声明是否保持当前 BLE 链路参数。 */
static void app_queue_power_internal(
    const power_policy_t *policy,
    const bool persist_authorized,
    const bool preserve_ble_link)
{
    /* 检查输入与队列。 */
    if ((policy == NULL) || (s_power_queue == NULL)) {
        /* 安全返回。 */
        return;
    }
    /* 构造完整请求。 */
    app_power_request_t request = {
        /* 先复制领域策略。 */
        .policy = *policy,
        /* 写入刷盘授权。 */
        .persist_authorized = persist_authorized,
        /* 写入 BLE 链路保护位；诊断覆盖不得打断正在等待 ACK 的 GATT 会话。 */
        .preserve_ble_link = preserve_ble_link,
    };
    /* 阶段一常亮联调要求每个后续电池、连接和页面事件都保持连续六轴采样。 */
    if (APP_BENCH_ALWAYS_ON) {
        /* 强制 QMI 使用 ACTIVE，防止启动电池事件把初始 25 Hz 策略覆盖回 WOM。 */
        request.policy.imu_mode = POWER_IMU_ACTIVE_25HZ;
        /* 联调期间关闭自动 Light-sleep，保证传感器时间戳连续。 */
        request.policy.automatic_light_sleep = false;
        /* 联调期间拒绝任何领域事件携带 Deep-sleep 请求。 */
        request.policy.request_deep_sleep = false;
        /* ACTIVE 采样不安装 QMI Deep-sleep 运动唤醒路径。 */
        request.policy.enable_imu_deep_wake = false;
    }
    /* 默认 35% 档由用户偏好完整替换；暂停 15% 档只允许用户再调暗。 */
    if (request.policy.display_on &&
        ((request.policy.display_brightness_percent == UINT8_C(35)) ||
         (s_device_config.brightness_percent < request.policy.display_brightness_percent))) {
        /* 使用 Cmd9/NVS 的偏好值或更低亮度。 */
        request.policy.display_brightness_percent = s_device_config.brightness_percent;
    }
    /* 长度 1 队列只保留最新策略；关机状态不会再被普通状态覆盖。 */
    (void)xQueueOverwrite(s_power_queue, &request);
}

/* 把普通完整电源策略投递给电源任务；领域状态变化允许同步更新 BLE 射频模式。 */
static void app_queue_power(const power_policy_t *policy, const bool persist_authorized)
{
    /* 普通策略使用 false，确保页面、训练和待机状态仍可按原合同切换 BLE 模式。 */
    app_queue_power_internal(policy, persist_authorized, false);
}

/* RawStream 诊断临时覆盖 QMI 模式；不修改 power_manager 领域状态，关闭后恢复当前页面策略。 */
static void app_queue_raw_stream_power_policy(const bool enabled)
{
    /* 常亮阶段一固件从开机起已固定 QMI ACTIVE；Cmd11 不再切寄存器或投递电源请求。 */
    if (APP_BENCH_ALWAYS_ON) {
        /* 保持正在运行的 QMI 和 BLE 会话，RawStream 命令只修改发布门控与流水线状态。 */
        return;
    }
    /* 从协调器取得当前 HOME、RUNNING 或 PAUSED 的完整权威功耗策略副本。 */
    power_policy_t policy = power_manager_policy(&s_coordinator.power);
    /* 开启诊断时必须产生严格 25 Hz 六轴点，HOME 的 WOM 模式不能满足 RawStream 和 62 点窗口。 */
    if (enabled) {
        /* 把 IMU 临时覆盖为活动采样；屏幕、触摸和亮度继续服从当前页面策略。 */
        policy.imu_mode = POWER_IMU_ACTIVE_25HZ;
        /* 诊断采样期间不得携带 Deep-sleep 请求，避免尚未完成的窗口被睡眠截断。 */
        policy.request_deep_sleep = false;
        /* ACTIVE 模式不使用 QMI GPIO21 深睡唤醒，防止电源任务误按 WOM 检查 EXT1。 */
        policy.enable_imu_deep_wake = false;
    }
    /* 串行切换 QMI，但保持既有 BLE 连接参数，避免 Cmd11 ACK 期间更新 GAP 参数导致取消。 */
    app_queue_power_internal(&policy, false, true);
}

/* 消费一次协调器 effect，禁止在此重算业务指标。 */
static void app_dispatch_effects(const device_effects_t *effects)
{
    /* 空 effect 无副作用。 */
    if (effects == NULL) {
        /* 安全返回。 */
        return;
    }
    /* UI 只收完整快照。 */
    if ((effects->flags & DEVICE_EFFECT_UI_RENDER) != 0U) {
        /* 投递最新页面。 */
        app_queue_ui(&effects->ui);
    }
    /* BLE 状态只收权威完整修订。 */
    if ((effects->flags & DEVICE_EFFECT_BLE_LIVE_STATE) != 0U) {
        /* 投递状态。 */
        app_queue_live_state(&effects->live_state);
    }
    /* Event 从普通单事件或锁类补算事件序列编码。 */
    if ((effects->flags & DEVICE_EFFECT_BLE_EVENT) != 0U) {
        /* 按 event_seq 顺序投递全部有效事件。 */
        app_queue_metric_events(effects);
    }
    /* 电源策略默认没有刷盘关机授权。 */
    if ((effects->flags & DEVICE_EFFECT_POWER_POLICY) != 0U) {
        /* 投递策略。 */
        app_queue_power(&effects->power_policy, false);
    }
    /* 保存摘要；shutdown 标志随同摘要交给存储任务。 */
    if ((effects->flags & DEVICE_EFFECT_SUMMARY_WRITE) != 0U) {
        /* 构造存储事务。 */
        const app_storage_request_t request = {
            /* 复制摘要。 */
            .summary = effects->summary,
            /* 保存成功后是否关机。 */
            .shutdown_after_persist =
                (effects->flags & DEVICE_EFFECT_SHUTDOWN_AFTER_PERSIST) != 0U,
        };
        /* 关机摘要最多等待 500 ms 入队，普通中间摘要不阻塞。 */
        const TickType_t wait_ticks = request.shutdown_after_persist
                                          ? pdMS_TO_TICKS(500U)
                                          : 0U;
        /* 入队失败时绝不授权断电。 */
        if (xQueueSend(s_storage_queue, &request, wait_ticks) != pdPASS) {
            /* 输出错误。 */
            ESP_LOGE(APP_TAG, "session summary queue full seq=%lu shutdown=%d",
                     (unsigned long)request.summary.session_seq,
                     request.shutdown_after_persist ? 1 : 0);
        }
    } else if ((effects->flags & DEVICE_EFFECT_SHUTDOWN_AFTER_PERSIST) != 0U) {
        /* 空会话没有摘要可写，当前持久化状态已稳定，可直接授权关机。 */
        app_queue_power(&effects->power_policy, true);
    }
}

/* 通过协调器原子提交熄屏、唤醒或长空闲事件，使 UI 与功耗状态不会分裂。 */
static device_coordinator_status_t app_handle_idle_power_event(
    const power_event_type_t event_type,
    const uint64_t captured_ms)
{
    /* 保存本次完整按值效果。 */
    device_effects_t effects;
    /* 由协调器在同一候选副本更新 UI、Power、修订号和 LiveState。 */
    const device_coordinator_status_t status = device_coordinator_handle_idle_power_event(
        &s_coordinator,
        event_type,
        app_coordinator_time_ms(captured_ms),
        &effects);
    /* 成功后才扇出页面、BLE 和硬件策略。 */
    if (status == DEVICE_COORDINATOR_OK) {
        /* 投递同一修订的全部效果。 */
        app_dispatch_effects(&effects);
    }
    /* 返回状态供 UI/BLE 调用方记录或决定是否继续控制。 */
    return status;
}

/* 25 Hz 点回调；同步运行在应用任务，直接进入唯一协调器。 */
static void app_pipeline_on_sample(void *context, const imu_resampled_sample_t *sample)
{
    /* 当前回调不使用 context。 */
    (void)context;
    /* 空样本不处理。 */
    if (sample == NULL) {
        /* 安全返回。 */
        return;
    }
    /* 为所有 25 Hz 点分配连续序号，即使 RawStream 关闭也保持时间线语义。 */
    const uint32_t raw_sample_index = s_raw_sample_index;
    /* uint32 达到最大值后按协议自然回绕。 */
    s_raw_sample_index += UINT32_C(1);
    /* 开发模式开启时把当前滤波/重采样六轴点排入 BLE 诊断流。 */
    app_queue_raw_sample(sample, raw_sample_index);
    /* 构造 motion_phase 固定六轴输入。 */
    motion_phase_sample_t motion_sample;
    /* 时间转毫秒并夹到协调器单调边界。 */
    motion_sample.monotonic_ms = app_coordinator_time_ms(sample->timestamp_us / UINT64_C(1000));
    /* 复制 gx、gy、gz、ax、ay、az。 */
    (void)memcpy(motion_sample.axis, sample->axes, sizeof(motion_sample.axis));
    /* 连续性破坏位表示当前动作段分类证据存在时间缺口，必须清空历史 logits。 */
    const uint32_t bout_reset_mask =
        (uint32_t)IMU_QUALITY_ACCEL_GAP |
        (uint32_t)IMU_QUALITY_GYRO_GAP |
        (uint32_t)IMU_QUALITY_OUT_OF_ORDER |
        (uint32_t)IMU_QUALITY_QUEUE_OVERFLOW |
        (uint32_t)IMU_QUALITY_DRIVER_DROP |
        (uint32_t)IMU_QUALITY_RESAMPLER_RESET;
    /* 只在真实连续性破坏时重置动作段分类证据；历史保留质量位不能丢弃整段历史。 */
    if ((sample->quality_flags & bout_reset_mask) != 0U) {
        /* 清空累计 logits，但保留当前会话的动作、次数、时间和热量。 */
        workout_engine_reset_bout_evidence(&s_coordinator.workout);
    }
    /* 历史保留污染位、间断或队列溢出时仍累计热量，但冻结相位和步峰。 */
    const uint32_t invalid_count_mask =
        (uint32_t)IMU_QUALITY_ACCEL_GAP |
        (uint32_t)IMU_QUALITY_GYRO_GAP |
        (uint32_t)IMU_QUALITY_OUT_OF_ORDER |
        (uint32_t)IMU_QUALITY_QUEUE_OVERFLOW |
        (uint32_t)IMU_QUALITY_LEGACY_ACTUATOR_CONTAMINATED |
        (uint32_t)IMU_QUALITY_DRIVER_DROP |
        (uint32_t)IMU_QUALITY_RESAMPLER_RESET;
    /* 只有零关键质量位时允许推进计数器。 */
    const bool count_input_valid = (sample->quality_flags & invalid_count_mask) == 0U;
    /* 保存按值效果。 */
    device_effects_t effects;
    /* 进入唯一业务事实链。 */
    const device_coordinator_status_t status = device_coordinator_push_sample(
        &s_coordinator,
        &motion_sample,
        count_input_valid,
        (uint16_t)(sample->quality_flags & UINT16_MAX),
        &effects);
    /* 成功 effect 可能为空或包含 MetricEvent。 */
    if (status == DEVICE_COORDINATOR_OK) {
        /* 扇出效果。 */
        app_dispatch_effects(&effects);
    } else if ((status != DEVICE_COORDINATOR_IGNORED) &&
               (status != DEVICE_COORDINATOR_ERR_STATE)) {
        /* 记录真正异常；准备/暂停状态拒绝样本不刷屏。 */
        ESP_LOGW(APP_TAG, "coordinator sample status=%d quality=0x%08lX",
                 (int)status,
                 (unsigned long)sample->quality_flags);
    }
}

/* 双 M0 推理回调；失败窗拒绝，质量位交给训练引擎按准备态/运行态分别判定。 */
static void app_pipeline_on_inference(
    void *context,
    const imu_pipeline_inference_result_t *result)
{
    /* 当前回调不使用 context。 */
    (void)context;
    /* 检查结果。 */
    if (result == NULL) {
        /* 安全返回。 */
        return;
    }
    /* 把窗口结束时刻换算到协调器唯一单调毫秒时基。 */
    const uint64_t inference_ms = app_coordinator_time_ms(
        result->end_timestamp_us / UINT64_C(1000));
    /* 非训练状态只保留底层采集；训练首个完整窗口必须立即进入锁类。 */
    if (!app_training_accepts_inference()) {
        /* 返回后下一窗口继续采集，START/RESUME 已清除旧窗口。 */
        return;
    }
    /* 分类门打开后才发布融合动作、双模型结果和推理耗时到上位机。 */
    app_queue_inference_diagnostic(result);
    /* 只有模型推理失败时拒绝；质量告警仍需进入诊断，RUNNING 计数由逐点活动门控制。 */
    if (result->inference_status != 0) {
        /* 输出错误，下一完整窗口继续推理。 */
        ESP_LOGE(APP_TAG, "dual M0 inference failed=%d seq=%lu",
                 result->inference_status,
                 (unsigned long)result->sequence);
        /* 失败 logits 不得进入训练引擎。 */
        return;
    }
    /* 保存效果。 */
    device_effects_t effects;
    /* 提交 11 维融合 logits。 */
    const device_coordinator_status_t status = device_coordinator_push_inference(
        &s_coordinator,
        result->logits,
        inference_ms,
        (uint16_t)(result->quality_flags & UINT16_MAX),
        &effects);
    /* 成功时可能触发动作锁定和页面切换。 */
    if (status == DEVICE_COORDINATOR_OK) {
        /* 扇出效果。 */
        app_dispatch_effects(&effects);
    } else if ((status != DEVICE_COORDINATOR_IGNORED) &&
               (status != DEVICE_COORDINATOR_ERR_STATE)) {
        /* 记录异常。 */
        ESP_LOGW(APP_TAG, "coordinator inference status=%d", (int)status);
    }
}

/* 将一帧 QMI 原始数据提交给应用任务独占的流水线。 */
static APP_STACK_BOUNDARY void app_process_qmi_frame(const board_qmi8658_frame_t *frame)
{
    /* 空帧不处理。 */
    if (frame == NULL) {
        /* 安全返回。 */
        return;
    }
    /* Cmd11 应答后的首个到期 QMI 帧原子打开发布门，BLE worker 此前不会收到六轴或分类帧。 */
    if (s_raw_stream_activation_pending &&
        (frame->timestamp_us >= s_raw_stream_activation_due_us)) {
        /* 先清除 pending，保证后续每帧不重复执行激活事务。 */
        s_raw_stream_activation_pending = false;
        /* 清零已消费的绝对门槛，避免诊断快照误读旧时间。 */
        s_raw_stream_activation_due_us = UINT64_C(0);
        /* 最后打开 volatile 发布门；BLE 任务从下一批队列项开始允许通知。 */
        s_raw_stream_enabled = true;
    }
    /* 复用一个三轴原始点结构。 */
    imu_qmi_raw_sample_t raw;
    /* 清零质量位和轴。 */
    (void)memset(&raw, 0, sizeof(raw));
    /* 两路使用驱动捕获的同一单调微秒。 */
    raw.timestamp_us = frame->timestamp_us;
    /* 新加速度存在时提交 ax、ay、az。 */
    if (frame->accel_available) {
        /* 复制三个 int16 原始轴。 */
        (void)memcpy(raw.raw_xyz, frame->accel_raw, sizeof(raw.raw_xyz));
        /* 提交 125 Hz 流。 */
        (void)imu_pipeline_push_accel_raw(&s_imu_pipeline, &raw);
    }
    /* 新角速度存在时提交 gx、gy、gz。 */
    if (frame->gyro_available) {
        /* 复制三个 int16 原始轴。 */
        (void)memcpy(raw.raw_xyz, frame->gyro_raw, sizeof(raw.raw_xyz));
        /* 提交 112.1 Hz 流。 */
        (void)imu_pipeline_push_gyro_raw(&s_imu_pipeline, &raw);
    }
}

/* 把运行配置投影到 UI 只读设置快照；不修改训练、BLE 或 NVS。 */
static void app_project_device_config_to_ui(ui_context_t *ui)
{
    /* 输出页面上下文必须存在。 */
    if (ui == NULL) {
        /* 无可写对象时安全返回。 */
        return;
    }
    /* 复制 AMOLED 用户亮度百分比。 */
    ui->view.brightness_percent = s_device_config.brightness_percent;
    /* 复制自动熄屏秒数。 */
    ui->view.screen_timeout_seconds = s_device_config.screen_timeout_seconds;
    /* 复制偏好修订号，供设备与 PC 诊断对照。 */
    ui->view.preferences_revision = s_device_config.preferences_revision;
}

/* 返回下一个设备端亮度预设；任意 PC 自定义值会向上取最近档。 */
static uint8_t app_next_brightness_percent(const uint8_t current)
{
    /* 小于 15% 先进入最低可用预设。 */
    if (current < UINT8_C(15)) {
        /* 返回 15%。 */
        return UINT8_C(15);
    }
    /* 15~34% 进入默认 35%。 */
    if (current < UINT8_C(35)) {
        /* 返回 35%。 */
        return UINT8_C(35);
    }
    /* 35~59% 进入 60%。 */
    if (current < UINT8_C(60)) {
        /* 返回 60%。 */
        return UINT8_C(60);
    }
    /* 60~99% 进入最高 100%。 */
    if (current < UINT8_C(100)) {
        /* 返回 100%。 */
        return UINT8_C(100);
    }
    /* 100% 后回到省电 15%。 */
    return UINT8_C(15);
}

/* 返回下一个设备端熄屏预设，单位秒。 */
static uint16_t app_next_screen_timeout_seconds(const uint16_t current)
{
    /* 小于 15 秒时进入 15 秒预设。 */
    if (current < UINT16_C(15)) {
        /* 返回 15 秒。 */
        return UINT16_C(15);
    }
    /* 15~29 秒进入 30 秒。 */
    if (current < UINT16_C(30)) {
        /* 返回 30 秒。 */
        return UINT16_C(30);
    }
    /* 30~59 秒进入 60 秒。 */
    if (current < UINT16_C(60)) {
        /* 返回 60 秒。 */
        return UINT16_C(60);
    }
    /* 60~119 秒进入 120 秒。 */
    if (current < UINT16_C(120)) {
        /* 返回 120 秒。 */
        return UINT16_C(120);
    }
    /* 120 秒及更长 PC 自定义值后回到 15 秒。 */
    return UINT16_C(15);
}

/* 处理不经过领域协调器的设备端设置与诊断按钮；返回 true 表示命令已消费。 */
static bool app_process_local_ui_command(const ui_command_t command)
{
    /* “忘记电脑”只删除 NimBLE 绑定，不修改设备偏好、训练记录或算法状态。 */
    if (command == UI_COMMAND_FORGET_COMPUTER) {
        /* 页面已切走时安全消费旧触摸事件，防止后台误删合法绑定。 */
        if (s_coordinator.ui.state != UI_STATE_SETTINGS) {
            /* 命令属于本地设置集合，返回已消费。 */
            return true;
        }
        /* 断开当前 PC 并调用 NimBLE 官方存储清除全部绑定。 */
        const esp_err_t forget_status = ble_service_nimble_forget_all_bonds();
        /* 服务未启动或底层清密钥失败时保留明确串口诊断；不得伪装成功。 */
        if (forget_status != ESP_OK) {
            /* 输出 ESP-IDF 稳定错误名，不记录电脑地址或密钥。 */
            ESP_LOGE(APP_TAG, "forget BLE bonds failed=%s", esp_err_to_name(forget_status));
        }
        /* API 内部会通过 passkey_clear 回调清除屏幕敏感码。 */
        return true;
    }
    /* 判断命令是否属于可持久化设备偏好。 */
    const bool brightness_command = command == UI_COMMAND_CYCLE_BRIGHTNESS;
    /* 判断熄屏时长切换。 */
    const bool timeout_command = command == UI_COMMAND_CYCLE_TIMEOUT;
    /* 其它命令交回普通导航或会话控制。 */
    if (!brightness_command && !timeout_command) {
        /* 返回未消费。 */
        return false;
    }
    /* 亮度只允许 SETTINGS；熄屏时长只允许 DIAGNOSTICS。 */
    const bool valid_page = brightness_command
        ? (s_coordinator.ui.state == UI_STATE_SETTINGS)
        : (s_coordinator.ui.state == UI_STATE_DIAGNOSTICS);
    /* 页面切换后到达的旧按钮事件不得修改配置。 */
    if (!valid_page) {
        /* 命令已安全消费。 */
        return true;
    }
    /* 在局部副本修改，NVS commit 成功前运行事实保持不变。 */
    device_config_t candidate = s_device_config;
    /* 按命令修改唯一字段。 */
    if (brightness_command) {
        /* 循环 15/35/60/100% 预设。 */
        candidate.brightness_percent = app_next_brightness_percent(candidate.brightness_percent);
    } else {
        /* 循环 15/30/60/120 秒预设。 */
        candidate.screen_timeout_seconds = app_next_screen_timeout_seconds(
            candidate.screen_timeout_seconds);
    }
    /* 修订号饱和于 UINT32_MAX；达到上限后仍允许同修订幂等覆盖。 */
    if (candidate.preferences_revision < UINT32_MAX) {
        /* 每次成功本地修改增加一次。 */
        candidate.preferences_revision += UINT32_C(1);
    }
    /* 先持久化完整 CRC32 blob，失败时不改变屏幕或运行开关。 */
    const esp_err_t persist_error = app_persist_device_config(&candidate);
    /* NVS 失败只记录并保留旧页面事实。 */
    if (persist_error != ESP_OK) {
        /* 输出错误供诊断页和串口定位。 */
        ESP_LOGE(APP_TAG, "local preference persist failed error=%s", esp_err_to_name(persist_error));
        /* 命令已消费。 */
        return true;
    }
    /* NVS 成功后提交运行配置。 */
    s_device_config = candidate;
    /* 秒转毫秒；最大 600 秒不会溢出 uint32。 */
    s_screen_timeout_ms = (uint32_t)s_device_config.screen_timeout_seconds * UINT32_C(1000);
    /* 把真实设置投影到当前页面快照。 */
    app_project_device_config_to_ui(&s_coordinator.ui);
    /* 立即刷新设置或诊断文字。 */
    app_queue_ui(&s_coordinator.ui);
    /* 重新投递当前电源策略，使亮度修改立即作用于 AMOLED。 */
    const power_policy_t policy = power_manager_policy(&s_coordinator.power);
    /* 应用亮度但不授权 PMIC 关机。 */
    app_queue_power(&policy, false);
    /* 设置命令完成。 */
    return true;
}

/* 处理 UI 命令；设置/诊断直接改纯 UI，其余走协调器统一控制。 */
static APP_STACK_BOUNDARY void app_process_ui_command(
    const ui_command_t command,
    const uint64_t captured_ms)
{
    /* 任意真实触摸都是用户活动，重新开始熄屏和长空闲计时。 */
    power_idle_timer_note_activity(&s_power_idle_timer, captured_ms);
    /* 熄屏透明按钮只负责原子恢复 UI 和功耗，不进入普通页面导航。 */
    if (command == UI_COMMAND_WAKE) {
        /* 当前状态若已被其它来源唤醒，状态错误只代表命令已过期。 */
        const device_coordinator_status_t wake_status = app_handle_idle_power_event(
            POWER_EVENT_USER_WAKE,
            captured_ms);
        /* 非幂等状态错误之外的异常需输出诊断。 */
        if ((wake_status != DEVICE_COORDINATOR_OK) &&
            (wake_status != DEVICE_COORDINATOR_ERR_STATE)) {
            /* 记录唤醒失败。 */
            ESP_LOGW(APP_TAG, "UI wake rejected status=%d", (int)wake_status);
        }
        /* 唤醒命令已消费。 */
        return;
    }
    /* 亮度与熄屏设置由应用任务本地事务处理。 */
    if (app_process_local_ui_command(command)) {
        /* 本地命令已处理或因页面过期安全忽略。 */
        return;
    }
    /* 先映射纯 UI 事件。 */
    ui_event_t ui_event;
    /* 无映射命令安全忽略。 */
    if (!ui_command_to_event(command, (uint32_t)captured_ms, &ui_event)) {
        /* 返回。 */
        return;
    }
    /* 尝试映射为训练控制。 */
    device_control_t control;
    /* 训练控制必须进入协调器。 */
    if (device_coordinator_control_from_ui(ui_event.type, &control)) {
        /* 保存效果。 */
        device_effects_t effects;
        /* 使用单调夹取时间。 */
        const device_coordinator_status_t status = device_coordinator_handle_control(
            &s_coordinator,
            control,
            app_coordinator_time_ms(captured_ms),
            &effects);
        /* 成功时扇出。 */
        if (status == DEVICE_COORDINATOR_OK) {
            /* START/RESUME 成功后丢弃旧 62 点窗口，禁止上一个会话或暂停前数据进入新判断。 */
            if ((command == UI_COMMAND_START) || (command == UI_COMMAND_RESUME)) {
                /* 同时清空重采样网格、环形窗口和待传播质量位。 */
                imu_pipeline_reset_session(&s_imu_pipeline);
            }
            /* 扇出效果。 */
            app_dispatch_effects(&effects);
        } else {
            /* 输出被拒绝原因，例如 8% 低电量。 */
            ESP_LOGW(APP_TAG, "UI command=%d rejected status=%d", (int)command, (int)status);
        }
        /* 返回。 */
        return;
    }
    /* 设置、诊断和返回只更新协调器内部纯 UI。 */
    const ui_dispatch_result_t ui_status = ui_dispatch_event(&s_coordinator.ui, &ui_event);
    /* 成功时投递新页面。 */
    if (ui_status == UI_DISPATCH_OK) {
        /* 渲染完整快照。 */
        app_queue_ui(&s_coordinator.ui);
    }
}

/* 完成一个显式 BLE 业务结果并唤醒等待中的异步 NimBLE worker。 */
static void app_finish_ble_explicit(const uint8_t status, const uint16_t error_code)
{
    /* 设置命令 v1 当前不返回可选 TLV。 */
    s_ble_command_slot.result.tlv = NULL;
    /* 可选 TLV 长度固定为零。 */
    s_ble_command_slot.result.tlv_length = 0U;
    /* 返回命令提交后的权威协调器修订号。 */
    s_ble_command_slot.result.state_revision = s_coordinator.state_revision;
    /* 写入线上业务状态。 */
    s_ble_command_slot.result.status = status;
    /* 写入稳定错误码；成功时调用者传 BLE_SERVICE_ERROR_NONE。 */
    s_ble_command_slot.result.error_code = error_code;
    /* 唤醒等待中的 NimBLE worker；GATT 回调线程本身从不等待该事务。 */
    (void)xSemaphoreGive(s_ble_command_done);
}

/* 把协调器返回码映射为 BLE 稳定响应。 */
static void app_finish_ble_command(const device_coordinator_status_t status)
{
    /* 默认不返回 TLV。 */
    s_ble_command_slot.result.tlv = NULL;
    /* 默认 TLV 长度为零。 */
    s_ble_command_slot.result.tlv_length = 0U;
    /* 返回事务后的权威修订号。 */
    s_ble_command_slot.result.state_revision = s_coordinator.state_revision;
    /* 成功或安全忽略都视为幂等成功。 */
    if ((status == DEVICE_COORDINATOR_OK) || (status == DEVICE_COORDINATOR_IGNORED)) {
        /* 写成功状态。 */
        s_ble_command_slot.result.status = BLE_SERVICE_CONTROL_OK;
        /* 无错误。 */
        s_ble_command_slot.result.error_code = BLE_SERVICE_ERROR_NONE;
    } else if ((status == DEVICE_COORDINATOR_ERR_STATE) ||
               (status == DEVICE_COORDINATOR_ERR_LOW_BATTERY)) {
        /* 安全策略拒绝。 */
        s_ble_command_slot.result.status = BLE_SERVICE_CONTROL_REJECTED;
        /* 0x0201 表示设备状态不允许，0x0202 表示低电量。 */
        s_ble_command_slot.result.error_code =
            status == DEVICE_COORDINATOR_ERR_LOW_BATTERY ? UINT16_C(0x0202) : UINT16_C(0x0201);
    } else {
        /* 其它错误映射内部错误。 */
        s_ble_command_slot.result.status = BLE_SERVICE_CONTROL_INTERNAL_ERROR;
        /* 使用通用业务处理失败码。 */
        s_ble_command_slot.result.error_code = BLE_SERVICE_ERROR_HANDLER_FAILED;
    }
    /* 唤醒等待中的 NimBLE 回调。 */
    (void)xSemaphoreGive(s_ble_command_done);
}

/* 判断命令是否属于配置/校时/RawStream TLV 组件。 */
static bool app_is_device_config_command(const uint8_t command_id)
{
    /* 五个固定命令以外仍交给会话协调器。 */
    return (command_id == (uint8_t)DEVICE_COMMAND_SYNC_TIME) ||
           (command_id == (uint8_t)DEVICE_COMMAND_SET_USER_PROFILE) ||
           (command_id == (uint8_t)DEVICE_COMMAND_SET_GOAL) ||
           (command_id == (uint8_t)DEVICE_COMMAND_SET_PREFERENCES) ||
           (command_id == (uint8_t)DEVICE_COMMAND_SET_RAW_STREAM);
}

/* 检查 Cmd9 内容是否与当前持久配置完全相同，用于 revision 相等时幂等成功。 */
static bool app_preferences_match_current(const device_preferences_command_t *preferences)
{
    /* 空输入不匹配。 */
    if (preferences == NULL) {
        /* 返回 false。 */
        return false;
    }
    /* 比较五个有效持久字段；旧振动保留位和 RawStream 都不参与 Cmd9。 */
    return (preferences->brightness_percent == s_device_config.brightness_percent) &&
           (preferences->sound_enabled == s_device_config.sound_enabled) &&
           (preferences->screen_timeout_seconds == s_device_config.screen_timeout_seconds) &&
           (preferences->preferences_revision == s_device_config.preferences_revision) &&
           (preferences->developer_mode == s_device_config.developer_mode);
}

/* 处理 Cmd6/7/8/9/11；返回 true 表示命令已完成并已释放完成信号。 */
static APP_STACK_BOUNDARY bool app_process_device_config_command(const uint64_t captured_ms)
{
    /* 非配置命令交给原会话控制路径。 */
    if (!app_is_device_config_command(s_ble_command_slot.command_id)) {
        /* 返回未处理。 */
        return false;
    }
    /* 解码为类型安全联合体；输入 TLV 已在 broker 深拷贝。 */
    device_command_v1_t command;
    /* 严格检查版本、长度、重复项、缺项和范围。 */
    const device_config_status_t decode_status = device_command_v1_decode(
        s_ble_command_slot.command_id,
        s_ble_command_slot.command_version,
        s_ble_command_slot.tlv,
        s_ble_command_slot.tlv_length,
        &command);
    /* 任一解析失败返回固定设置错误，不修改 RTC、NVS 或运行状态。 */
    if (decode_status != DEVICE_CONFIG_OK) {
        /* 输出精确内部状态供串口诊断。 */
        ESP_LOGW(APP_TAG, "device command decode failed cmd=%u status=%d",
                 (unsigned int)s_ble_command_slot.command_id,
                 (int)decode_status);
        /* 返回业务拒绝。 */
        app_finish_ble_explicit(BLE_SERVICE_CONTROL_REJECTED, APP_BLE_ERROR_CONFIG_TLV);
        /* 命令已完成。 */
        return true;
    }
    /* Cmd7 revision 小于当前值时拒绝旧配置覆盖；默认 revision=1 允许 PC 首次同号认领。 */
    if (command.command_id == (uint8_t)DEVICE_COMMAND_SET_USER_PROFILE) {
        /* 读取线上 revision。 */
        const uint32_t revision = command.value.user_profile.profile_revision;
        /* 判断严格旧 revision。 */
        if (revision < s_device_config.profile_revision) {
            /* 返回稳定陈旧修订错误。 */
            app_finish_ble_explicit(BLE_SERVICE_CONTROL_REJECTED, APP_BLE_ERROR_STALE_REVISION);
            /* 命令已完成。 */
            return true;
        }
        /* 同 revision 且同体重属于幂等重试，不重复写 Flash。 */
        if ((revision == s_device_config.profile_revision) &&
            (command.value.user_profile.weight_g == s_device_config.weight_g)) {
            /* 返回成功。 */
            app_finish_ble_explicit(BLE_SERVICE_CONTROL_OK, BLE_SERVICE_ERROR_NONE);
            /* 命令已完成。 */
            return true;
        }
    }
    /* Cmd9 同样拒绝严格旧 revision；相同 revision 可覆盖冻结默认值或形成幂等重试。 */
    if (command.command_id == (uint8_t)DEVICE_COMMAND_SET_PREFERENCES) {
        /* 读取偏好 revision。 */
        const uint32_t revision = command.value.preferences.preferences_revision;
        /* 更旧 revision 拒绝。 */
        if (revision < s_device_config.preferences_revision) {
            /* 返回陈旧修订。 */
            app_finish_ble_explicit(BLE_SERVICE_CONTROL_REJECTED, APP_BLE_ERROR_STALE_REVISION);
            /* 命令已完成。 */
            return true;
        }
        /* 完全相同属于幂等成功。 */
        if ((revision == s_device_config.preferences_revision) &&
            app_preferences_match_current(&command.value.preferences)) {
            /* 返回成功且不磨损 Flash。 */
            app_finish_ble_explicit(BLE_SERVICE_CONTROL_OK, BLE_SERVICE_ERROR_NONE);
            /* 命令已完成。 */
            return true;
        }
    }
    /* Cmd11 开启时必须先由 Cmd9 显式打开开发者模式。 */
    if ((command.command_id == (uint8_t)DEVICE_COMMAND_SET_RAW_STREAM) &&
        command.value.raw_stream.raw_stream_enabled &&
        !s_device_config.developer_mode) {
        /* 返回开发者权限错误。 */
        app_finish_ble_explicit(BLE_SERVICE_CONTROL_REJECTED, APP_BLE_ERROR_DEVELOPER_REQUIRED);
        /* 命令已完成。 */
        return true;
    }
    /* 在局部副本应用命令，失败不会污染运行配置。 */
    device_config_t candidate = s_device_config;
    /* Cmd11 使用同一配置规则，但后续不会持久化原始流位。 */
    const device_config_status_t apply_status = device_config_apply_command(&candidate, &command);
    /* 理论上解码后的命令必能应用；防御式拒绝内部组合漂移。 */
    if (apply_status != DEVICE_CONFIG_OK) {
        /* 返回设置语义错误。 */
        app_finish_ble_explicit(BLE_SERVICE_CONTROL_REJECTED, APP_BLE_ERROR_CONFIG_TLV);
        /* 命令已完成。 */
        return true;
    }
    /* Cmd11 只更新易失开关，断线、重启或关闭开发者模式立即清零。 */
    if (command.command_id == (uint8_t)DEVICE_COMMAND_SET_RAW_STREAM) {
        /* 保存运行配置视图。 */
        s_device_config.raw_stream_enabled = candidate.raw_stream_enabled;
        /* 开关命令处理期间保持发布关闭，禁止通知抢在 Control Point 应答前进入 GATT。 */
        s_raw_stream_enabled = false;
        /* 开启前清空 WOM 或旧诊断留下的时间网格，首个 62 点窗口必须全部来自连续 ACTIVE 数据。 */
        if (candidate.raw_stream_enabled) {
            /* 应用任务是流水线唯一写者，此处重置不会和 QMI 读取任务并发修改窗口。 */
            imu_pipeline_reset_session(&s_imu_pipeline);
            /* 记录延迟激活状态；后续 QMI 帧到期时才允许六轴与分类通知。 */
            s_raw_stream_activation_pending = true;
            /* 使用单调微秒构造 750 ms 门槛，uint64 在设备寿命内不会溢出。 */
            s_raw_stream_activation_due_us =
                (app_now_ms() + (uint64_t)APP_RAW_STREAM_ACTIVATION_DELAY_MS) * UINT64_C(1000);
        } else {
            /* 关闭命令取消尚未到期的激活，防止旧定时点在关闭后重新开流。 */
            s_raw_stream_activation_pending = false;
            /* 清除旧门槛。 */
            s_raw_stream_activation_due_us = UINT64_C(0);
        }
        /* 开启时临时恢复 QMI ACTIVE，关闭时恢复协调器当前页面的 HOME/WOM 或训练策略。 */
        app_queue_raw_stream_power_policy(candidate.raw_stream_enabled);
        /* 返回成功，不执行 NVS commit。 */
        app_finish_ble_explicit(BLE_SERVICE_CONTROL_OK, BLE_SERVICE_ERROR_NONE);
        /* 命令已完成。 */
        return true;
    }
    /* 保存协调器旧副本；体重/目标若 NVS 失败可原子回滚且尚未扇出效果。 */
    const device_coordinator_t coordinator_before = s_coordinator;
    /* 保存可能由体重或目标更新生成的效果。 */
    device_effects_t effects;
    /* 默认无协调器副作用。 */
    bool coordinator_changed = false;
    /* 默认协调器状态成功。 */
    device_coordinator_status_t coordinator_status = DEVICE_COORDINATOR_OK;
    /* Cmd6 先写 RTC；写失败时不保存“已同步”配置。 */
    if (command.command_id == (uint8_t)DEVICE_COMMAND_SYNC_TIME) {
        /* PCF85063 接收 UTC Unix 秒，时区仅保存在配置供显示使用。 */
        if (board_pcf85063_write_unix(
                &s_sensors.rtc,
                command.value.sync_time.utc_unix_seconds) != BOARD_SENSORS_OK) {
            /* 返回硬件校时错误。 */
            app_finish_ble_explicit(BLE_SERVICE_CONTROL_INTERNAL_ERROR, APP_BLE_ERROR_RTC_WRITE);
            /* 命令已完成。 */
            return true;
        }
    } else if (command.command_id == (uint8_t)DEVICE_COMMAND_SET_USER_PROFILE) {
        /* 更新下次会话体重；正在运行的会话保持启动时体重。 */
        coordinator_status = device_coordinator_set_next_session_weight(
            &s_coordinator,
            candidate.weight_g,
            app_coordinator_time_ms(captured_ms),
            &effects);
        /* 标记成功后需在持久化成功时扇出。 */
        coordinator_changed = coordinator_status == DEVICE_COORDINATOR_OK;
    } else if (command.command_id == (uint8_t)DEVICE_COMMAND_SET_GOAL) {
        /* 更新目标并重算权威进度。 */
        coordinator_status = device_coordinator_set_goal(
            &s_coordinator,
            candidate.goal_kind,
            candidate.goal_value,
            app_coordinator_time_ms(captured_ms),
            &effects);
        /* 标记成功后需扇出 UI 与 LiveState。 */
        coordinator_changed = coordinator_status == DEVICE_COORDINATOR_OK;
    }
    /* 协调器拒绝时保持原配置并使用统一映射响应。 */
    if (coordinator_status != DEVICE_COORDINATOR_OK) {
        /* 恢复防御性旧副本，尽管协调器事务本身也保证失败不提交。 */
        s_coordinator = coordinator_before;
        /* 返回统一业务错误。 */
        app_finish_ble_command(coordinator_status);
        /* 命令已完成。 */
        return true;
    }
    /* Cmd9 关闭开发者模式时同时关闭易失 RawStream。 */
    if ((command.command_id == (uint8_t)DEVICE_COMMAND_SET_PREFERENCES) &&
        !candidate.developer_mode) {
        /* 清除候选中的易失位。 */
        candidate.raw_stream_enabled = false;
    }
    /* 先把候选完整写入 NVS，成功前不发布协调器效果或设备偏好。 */
    const esp_err_t persist_error = app_persist_device_config(&candidate);
    /* NVS 失败时恢复协调器，RTC 可能已校准但不会伪装为跨重启成功。 */
    if (persist_error != ESP_OK) {
        /* 回滚尚未向外发布的协调器副本。 */
        s_coordinator = coordinator_before;
        /* 记录 NVS 错误。 */
        ESP_LOGE(APP_TAG, "device config persist failed cmd=%u error=%s",
                 (unsigned int)command.command_id,
                 esp_err_to_name(persist_error));
        /* 返回内部持久化错误。 */
        app_finish_ble_explicit(BLE_SERVICE_CONTROL_INTERNAL_ERROR, APP_BLE_ERROR_CONFIG_PERSIST);
        /* 命令已完成。 */
        return true;
    }
    /* 持久化成功后提交运行配置。 */
    s_device_config = candidate;
    /* 同步易失 RawStream；Cmd9 仅可能在关闭开发者模式时把它清零。 */
    s_raw_stream_enabled = s_device_config.raw_stream_enabled && s_device_config.developer_mode;
    /* Cmd9 立即更新熄屏门槛和当前显示亮度。 */
    if (command.command_id == (uint8_t)DEVICE_COMMAND_SET_PREFERENCES) {
        /* 秒转毫秒；最大 600 秒安全落入 uint32。 */
        s_screen_timeout_ms = (uint32_t)s_device_config.screen_timeout_seconds * UINT32_C(1000);
        /* 重新投递当前功耗策略，app_queue_power 会套用新亮度。 */
        const power_policy_t policy = power_manager_policy(&s_coordinator.power);
        /* 应用亮度与外设状态。 */
        app_queue_power(&policy, false);
        /* 把 PC 已确认持久化的真实偏好同步到设备设置/诊断页。 */
        app_project_device_config_to_ui(&s_coordinator.ui);
        /* 即使没有训练领域 effect，也立即刷新当前设备页面。 */
        app_queue_ui(&s_coordinator.ui);
    }
    /* 体重或目标的 effect 此时才允许对 UI/BLE 可见。 */
    if (coordinator_changed) {
        /* 扇出同一修订效果。 */
        app_dispatch_effects(&effects);
    }
    /* 返回成功。 */
    app_finish_ble_explicit(BLE_SERVICE_CONTROL_OK, BLE_SERVICE_ERROR_NONE);
    /* 命令已完成。 */
    return true;
}

/* 应用任务处理 broker 中已复制的 BLE 控制命令。 */
static APP_STACK_BOUNDARY void app_process_ble_command(const uint64_t captured_ms)
{
    /* 每个已经通过 BLE 安全层和协议解析的控制命令都属于用户活动。 */
    power_idle_timer_note_activity(&s_power_idle_timer, captured_ms);
    /* START/RESUME 可能从黑屏 Home/Paused 发起，先原子恢复页面与功耗。 */
    if (((s_ble_command_slot.command_id == BLE_SERVICE_COMMAND_START_SESSION) ||
         (s_ble_command_slot.command_id == BLE_SERVICE_COMMAND_RESUME_SESSION)) &&
        (s_coordinator.ui.state == UI_STATE_SCREEN_OFF)) {
        /* 恢复熄屏前页面，保证随后统一控制状态合法。 */
        const device_coordinator_status_t wake_status = app_handle_idle_power_event(
            POWER_EVENT_USER_WAKE,
            captured_ms);
        /* 不能恢复时直接返回对应稳定业务状态，避免半执行训练控制。 */
        if (wake_status != DEVICE_COORDINATOR_OK) {
            /* 完成命令并唤醒等待中的 NimBLE handler。 */
            app_finish_ble_command(wake_status);
            /* 返回。 */
            return;
        }
    }
    /* 配置/校时/RawStream 命令由专用 TLV 事务处理，成功或失败都已发完成信号。 */
    if (app_process_device_config_command(captured_ms)) {
        /* 命令已完成。 */
        return;
    }
    /* 专用配置命令已在上方结束；这里只把会话命令 1~5/10 映射为协调器控制。 */
    device_control_t control;
    /* 不能映射的剩余编号属于未知或当前协议版本不支持的命令，必须明确拒绝。 */
    if (!device_coordinator_control_from_ble(s_ble_command_slot.command_id, &control)) {
        /* 返回无 TLV。 */
        s_ble_command_slot.result.tlv = NULL;
        /* 返回零长度。 */
        s_ble_command_slot.result.tlv_length = 0U;
        /* 当前状态修订不变。 */
        s_ble_command_slot.result.state_revision = s_coordinator.state_revision;
        /* 使用拒绝而不是伪装成功。 */
        s_ble_command_slot.result.status = BLE_SERVICE_CONTROL_REJECTED;
        /* 0x0203 表示该命令编号在当前固件协议版本中不受支持。 */
        s_ble_command_slot.result.error_code = UINT16_C(0x0203);
        /* 通知完成。 */
        (void)xSemaphoreGive(s_ble_command_done);
        /* 返回。 */
        return;
    }
    /* 保存效果。 */
    device_effects_t effects;
    /* 执行统一控制。 */
    const device_coordinator_status_t status = device_coordinator_handle_control(
        &s_coordinator,
        control,
        app_coordinator_time_ms(captured_ms),
        &effects);
    /* 成功时先提交所有副作用。 */
    if (status == DEVICE_COORDINATOR_OK) {
        /* BLE START/RESUME 与本机触摸遵守同一窗口隔离合同。 */
        if ((control == DEVICE_CONTROL_START) || (control == DEVICE_CONTROL_RESUME)) {
            /* 清空暂停前或上一会话留下的重采样点及 62 点推理窗口。 */
            imu_pipeline_reset_session(&s_imu_pipeline);
        }
        /* 扇出效果。 */
        app_dispatch_effects(&effects);
    }
    /* 填响应并通知 NimBLE。 */
    app_finish_ble_command(status);
}

/* NimBLE command handler：把命令同步 broker 到应用任务，自己不改业务状态。 */
static ble_service_status_t app_ble_command_handler(
    const ble_service_control_request_t *request,
    ble_service_command_result_t *result,
    void *context)
{
    /* 当前不使用 context。 */
    (void)context;
    /* 检查输入和同步对象。 */
    if ((request == NULL) || (result == NULL) ||
        (s_ble_command_mutex == NULL) || (s_ble_command_done == NULL)) {
        /* 返回 handler 错误。 */
        return BLE_SERVICE_STATUS_HANDLER_ERROR;
    }
    /* v1、TLV 长度和非空地址应由核心提前校验，这里再次保护跨组件边界。 */
    if ((request->command_version != 1U) ||
        (request->tlv_length > APP_BLE_COMMAND_MAX_TLV) ||
        ((request->tlv_length > 0U) && (request->tlv == NULL))) {
        /* 返回非法参数。 */
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    /* 单连接串行取得 broker。 */
    if (xSemaphoreTake(s_ble_command_mutex, pdMS_TO_TICKS(APP_BLE_COMMAND_TIMEOUT_MS)) != pdTRUE) {
        /* 返回忙错误。 */
        return BLE_SERVICE_STATUS_HANDLER_ERROR;
    }
    /* 清理异常超时遗留的完成信号。 */
    (void)xSemaphoreTake(s_ble_command_done, 0U);
    /* 复制不借用请求视图的稳定字段。 */
    s_ble_command_slot.request_id = request->request_id;
    /* 复制命令。 */
    s_ble_command_slot.command_id = request->command_id;
    /* 复制版本。 */
    s_ble_command_slot.command_version = request->command_version;
    /* 保存 TLV 长度。 */
    s_ble_command_slot.tlv_length = request->tlv_length;
    /* 非空 TLV 深拷贝到静态 broker；应用队列延迟期间不再依赖 NimBLE mbuf。 */
    if (request->tlv_length > 0U) {
        /* 复制精确有效字节。 */
        (void)memcpy(
            s_ble_command_slot.tlv,
            request->tlv,
            request->tlv_length);
    }
    /* 清零旧响应。 */
    (void)memset(&s_ble_command_slot.result, 0, sizeof(s_ble_command_slot.result));
    /* 构造应用事件。 */
    const app_event_t event = {
        /* 标记 BLE 命令。 */
        .kind = APP_EVENT_BLE_COMMAND,
        /* 捕获当前单调时间。 */
        .monotonic_ms = app_now_ms(),
    };
    /* 应用队列满时释放 broker 并失败。 */
    if (xQueueSend(s_app_event_queue, &event, pdMS_TO_TICKS(50U)) != pdPASS) {
        /* 释放互斥锁。 */
        (void)xSemaphoreGive(s_ble_command_mutex);
        /* 返回失败。 */
        return BLE_SERVICE_STATUS_HANDLER_ERROR;
    }
    /* 等待应用任务完成事务。 */
    if (xSemaphoreTake(s_ble_command_done, pdMS_TO_TICKS(APP_BLE_COMMAND_TIMEOUT_MS)) != pdTRUE) {
        /* 释放互斥锁。 */
        (void)xSemaphoreGive(s_ble_command_mutex);
        /* 返回超时。 */
        return BLE_SERVICE_STATUS_HANDLER_ERROR;
    }
    /* 复制稳定结果；设置命令当前不返回可选 TLV。 */
    *result = s_ble_command_slot.result;
    /* 释放 broker。 */
    (void)xSemaphoreGive(s_ble_command_mutex);
    /* 返回服务处理成功，业务拒绝由 result.status 表达。 */
    return BLE_SERVICE_STATUS_OK;
}

/* 包装 session transfer handler，用双互斥保护存储快照和待发送页。 */
static ble_service_status_t app_transfer_handler(
    const uint8_t *request_payload,
    const uint16_t request_length,
    uint8_t *response_payload,
    const size_t response_capacity,
    uint16_t *response_length,
    void *context)
{
    /* 当前 context 应指向全局 transfer service。 */
    session_transfer_service_t *service = (session_transfer_service_t *)context;
    /* 取得存储锁，防止同时 upsert 改环形索引。 */
    if (xSemaphoreTake(s_session_store_mutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        /* 返回 handler 失败。 */
        return BLE_SERVICE_STATUS_HANDLER_ERROR;
    }
    /* 取得传输状态锁。 */
    if (xSemaphoreTake(s_session_transfer_mutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        /* 释放存储锁。 */
        (void)xSemaphoreGive(s_session_store_mutex);
        /* 返回失败。 */
        return BLE_SERVICE_STATUS_HANDLER_ERROR;
    }
    /* 在锁内冻结一页并生成响应。 */
    const ble_service_status_t status = session_transfer_service_handle_request(
        request_payload,
        request_length,
        response_payload,
        response_capacity,
        response_length,
        service);
    /* 释放传输锁。 */
    (void)xSemaphoreGive(s_session_transfer_mutex);
    /* 释放存储锁。 */
    (void)xSemaphoreGive(s_session_store_mutex);
    /* 返回原状态。 */
    return status;
}

/* LVGL 按钮回调：只投递命令，不阻塞 UI 任务。 */
static void app_ui_command_callback(void *context, const ui_command_t command)
{
    /* 当前不使用 context。 */
    (void)context;
    /* 两个分级隔离阶段都没有创建 UI 控制邮箱；即使用户触摸按钮也不能访问空句柄。 */
    if (APP_BENCH_DISPLAY_ONLY || APP_BENCH_SENSOR_ONLY || (s_ui_command_queue == NULL)) {
        /* 丢弃隔离阶段交互命令；恢复产品启动链后由同一回调正常投递。 */
        return;
    }
    /* 构造仅含触摸时刻和命令的单槽邮箱负载。 */
    const app_ui_command_event_t command_event = {
        /* 捕获触摸单调时间，应用任务稍后提交时仍保留原始交互顺序。 */
        .monotonic_ms = app_now_ms(),
        /* 保存 presenter 已绑定的有效命令。 */
        .command = command,
    };
    /* 单槽邮箱使用 overwrite；即使 QMI 数据队列已满，用户最后一次点击也不会丢失或阻塞 LVGL。 */
    (void)xQueueOverwrite(s_ui_command_queue, &command_event);
}

/* BLE 连接回调：只投递连接事实。 */
static void app_ble_connection_changed(const bool connected, const uint16_t att_mtu, void *context)
{
    /* 当前不使用 context。 */
    (void)context;
    /* 构造事件。 */
    app_event_t event;
    /* 清零联合体。 */
    (void)memset(&event, 0, sizeof(event));
    /* 标记连接事件。 */
    event.kind = APP_EVENT_BLE_CONNECTION;
    /* 捕获时间。 */
    event.monotonic_ms = app_now_ms();
    /* 保存连接状态。 */
    event.data.ble_connected = connected;
    /* 投递失败只记录，GAP 回调不得阻塞。 */
    if (xQueueSend(s_app_event_queue, &event, 0U) != pdPASS) {
        /* 输出错误。 */
        ESP_LOGW(APP_TAG, "BLE connection queue full");
    }
    /* 输出协商 MTU 供诊断。 */
    ESP_LOGI(APP_TAG, "BLE connected=%d mtu=%u", connected ? 1 : 0, (unsigned int)att_mtu);
}

/* 把配对邮箱更新提示投递到应用任务；队列满时每秒空闲事件仍会补取最新快照。 */
static void app_notify_pairing_mailbox(void)
{
    /* 应用队列尚未创建时不能投递；正常 BLE 启动严格晚于队列创建。 */
    if (s_app_event_queue == NULL) {
        /* 安全返回。 */
        return;
    }
    /* 构造无负载提示事件；真正敏感码只保存在原子邮箱，不复制进通用队列。 */
    const app_event_t event = {
        /* 标记配对邮箱有新快照。 */
        .kind = APP_EVENT_PAIRING_UPDATE,
        /* 捕获当前单调时间供日志排序。 */
        .monotonic_ms = app_now_ms(),
    };
    /* NimBLE 回调不得阻塞；队列满时丢提示但不丢邮箱中的最新状态。 */
    (void)xQueueSend(s_app_event_queue, &event, 0U);
}

/* 配对码回调：无阻塞发布六位码并唤醒应用任务显示中文覆盖层。 */
static void app_ble_passkey_display(const uint32_t passkey, void *context)
{
    /* context 必须指向启动前初始化且覆盖 BLE 生命周期的静态邮箱。 */
    ui_app_pairing_mailbox_t *const mailbox = (ui_app_pairing_mailbox_t *)context;
    /* 发布 0～999999 六位码；单调毫秒截取低 32 位并使用回绕安全超时比较。 */
    const ui_app_pairing_result_t status = ui_app_pairing_publish_code(
        mailbox,
        passkey,
        (uint32_t)app_now_ms());
    /* 六位补零只写串口诊断；LVGL 从邮箱数值独立格式化，不解析日志。 */
    ESP_LOGI(APP_TAG, "BLE passkey=%06lu", (unsigned long)passkey);
    /* 成功发布后提示应用任务立即消费。 */
    if (status == UI_APP_PAIRING_OK) {
        /* 非阻塞投递提示。 */
        app_notify_pairing_mailbox();
    } else {
        /* 邮箱忙或参数损坏时记录稳定状态，后续安全失败/断线仍会发布清除。 */
        ESP_LOGW(APP_TAG, "pairing code mailbox status=%d", (int)status);
    }
}

/* 把 NimBLE 清除原因映射为 UI 稳定原因并发布到同一原子邮箱。 */
static void app_ble_passkey_clear(
    const ble_service_pairing_clear_reason_t reason,
    void *context)
{
    /* context 与显示回调共享同一静态邮箱。 */
    ui_app_pairing_mailbox_t *const mailbox = (ui_app_pairing_mailbox_t *)context;
    /* 保存映射后的 UI 原因。 */
    ui_pairing_clear_reason_t ui_reason = UI_PAIRING_CLEAR_FAILED;
    /* 显式映射全部公开 BLE 枚举，禁止依赖两个枚举偶然具有相同数值。 */
    switch (reason) {
        /* 安全绑定完成。 */
        case BLE_SERVICE_PAIRING_CLEAR_SUCCESS:
            ui_reason = UI_PAIRING_CLEAR_SUCCESS;
            break;
        /* 安全握手或 IO 注入失败。 */
        case BLE_SERVICE_PAIRING_CLEAR_FAILED:
            ui_reason = UI_PAIRING_CLEAR_FAILED;
            break;
        /* 物理断线或主机复位。 */
        case BLE_SERVICE_PAIRING_CLEAR_DISCONNECTED:
            ui_reason = UI_PAIRING_CLEAR_DISCONNECTED;
            break;
        /* 用户显式忘记电脑。 */
        case BLE_SERVICE_PAIRING_CLEAR_FORGOTTEN:
            ui_reason = UI_PAIRING_CLEAR_FORGOTTEN;
            break;
        /* BLE 服务停止。 */
        case BLE_SERVICE_PAIRING_CLEAR_SERVICE_STOPPED:
            ui_reason = UI_PAIRING_CLEAR_SERVICE_STOPPED;
            break;
        /* 未知值按失败处理，并保留日志供版本漂移诊断。 */
        default:
            ESP_LOGW(APP_TAG, "unknown pairing clear reason=%d", (int)reason);
            break;
    }
    /* 发布清除事件并覆盖邮箱旧码，避免敏感值在成功或失败后残留。 */
    const ui_app_pairing_result_t status = ui_app_pairing_publish_clear(
        mailbox,
        ui_reason,
        (uint32_t)app_now_ms());
    /* 成功时提示应用任务尽快恢复底层业务页。 */
    if (status == UI_APP_PAIRING_OK) {
        /* 非阻塞投递提示。 */
        app_notify_pairing_mailbox();
    } else {
        /* 并发覆盖失败需要诊断；连接状态事件仍会在 UI 状态机中强制清码。 */
        ESP_LOGW(APP_TAG, "pairing clear mailbox status=%d", (int)status);
    }
}

/* 应用任务消费最新配对邮箱并更新唯一 UI 状态机。 */
static void app_drain_pairing_mailbox(void)
{
    /* 保存从原子邮箱复制出的无指针事件。 */
    ui_event_t pairing_event;
    /* 无新事件或发布正在进行时安全返回，后续空闲轮询会重试。 */
    if (!ui_app_pairing_try_take(&s_pairing_mailbox, &pairing_event)) {
        /* 当前没有稳定快照。 */
        return;
    }
    /* 显示配对码时确保黑屏设备恢复显示，否则用户无法输入六位码。 */
    if ((pairing_event.type == UI_EVENT_PAIRING_CODE_SHOWN) &&
        (s_coordinator.ui.state == UI_STATE_SCREEN_OFF)) {
        /* 复用统一唤醒事务；返回状态错误只表示其它来源已先唤醒。 */
        (void)app_handle_idle_power_event(
            POWER_EVENT_USER_WAKE,
            (uint64_t)pairing_event.monotonic_ms);
    }
    /* 配对属于用户连接活动，重新开始熄屏和长空闲计时。 */
    power_idle_timer_note_activity(
        &s_power_idle_timer,
        (uint64_t)pairing_event.monotonic_ms);
    /* 由应用任务独占更新 UI，避免 NimBLE 任务直接访问 LVGL 或协调器。 */
    const ui_dispatch_result_t status = ui_dispatch_event(
        &s_coordinator.ui,
        &pairing_event);
    /* 成功时投递完整页面快照。 */
    if (status == UI_DISPATCH_OK) {
        /* 渲染配对覆盖层或恢复底层页面。 */
        app_queue_ui(&s_coordinator.ui);
    } else {
        /* 范围错误表示跨组件合同损坏，需要串口诊断。 */
        ESP_LOGW(APP_TAG, "pairing UI event rejected status=%d", (int)status);
    }
}

/* 应用任务：唯一修改 pipeline 和 coordinator。 */
static void app_event_task(void *argument)
{
    /* 当前不使用参数。 */
    (void)argument;
    /* 保存循环事件。 */
    app_event_t event;
    /* 永久消费队列。 */
    while (true) {
        /* 阻塞等待数据队列或 UI 控制邮箱任一就绪；Queue Set 不产生周期唤醒。 */
        const QueueSetMemberHandle_t ready_member =
            xQueueSelectFromSet(s_app_input_queue_set, portMAX_DELAY);
        /* Queue Set 理论上只返回两个已注册成员；空句柄防御性忽略。 */
        if (ready_member == NULL) {
            /* 继续阻塞等待，不修改领域状态。 */
            continue;
        }
        /* UI 邮箱就绪时把轻量命令还原为统一应用事件，后续仍走同一串行 switch。 */
        if (ready_member == s_ui_command_queue) {
            /* 保存从单槽邮箱取出的触摸事实。 */
            app_ui_command_event_t command_event;
            /* Queue Set 已报告可读；异常空读时放弃本轮并保留任务存活。 */
            if (xQueueReceive(s_ui_command_queue, &command_event, 0U) != pdPASS) {
                /* 继续等待下一项。 */
                continue;
            }
            /* 清零完整事件联合体，避免未使用成员携带栈垃圾。 */
            (void)memset(&event, 0, sizeof(event));
            /* 标记统一 UI 命令类型。 */
            event.kind = APP_EVENT_UI_COMMAND;
            /* 恢复点击发生时的单调毫秒。 */
            event.monotonic_ms = command_event.monotonic_ms;
            /* 写入用户命令。 */
            event.data.ui_command = command_event.command;
        } else if (ready_member == s_app_event_queue) {
            /* 主数据队列就绪时读取一条 QMI、BLE、电池、空闲或存储事实。 */
            if (xQueueReceive(s_app_event_queue, &event, 0U) != pdPASS) {
                /* Queue Set 与成员状态不一致时失败关闭，不提交未初始化事件。 */
                continue;
            }
        } else {
            /* 未注册句柄表示启动合同损坏；忽略它，避免把任意内存解释成事件。 */
            continue;
        }
        /* 按事件类型串行处理。 */
        switch (event.kind) {
            /* 提交 QMI 帧。 */
            case APP_EVENT_QMI_FRAME:
                app_process_qmi_frame(&event.data.qmi_frame);
                break;
            /* 上报两路驱动丢样并清空连续窗。 */
            case APP_EVENT_QMI_DROP:
                (void)imu_pipeline_report_source_drop(&s_imu_pipeline, IMU_SOURCE_ACCEL, 1U);
                (void)imu_pipeline_report_source_drop(&s_imu_pipeline, IMU_SOURCE_GYRO, 1U);
                /* 驱动丢帧造成连续窗失效，同时清空动作段分类历史，避免跨缺口累计 logits。 */
                workout_engine_reset_bout_evidence(&s_coordinator.workout);
                break;
            /* 处理触摸命令。 */
            case APP_EVENT_UI_COMMAND:
                app_process_ui_command(event.data.ui_command, event.monotonic_ms);
                break;
            /* 处理同步 BLE 命令。 */
            case APP_EVENT_BLE_COMMAND:
                app_process_ble_command(event.monotonic_ms);
                break;
            /* 更新 BLE 连接、电源和 UI。 */
            case APP_EVENT_BLE_CONNECTION: {
                /* 断线立即关闭易失 RawStream，重连后必须由开发者再次显式授权。 */
                if (!event.data.ble_connected) {
                    /* 关闭 BLE 发布任务门控。 */
                    s_raw_stream_enabled = false;
                    /* 断线同时取消尚未到期的延迟激活，重连后必须重新发送 Cmd11。 */
                    s_raw_stream_activation_pending = false;
                    /* 清除旧连接的激活门槛。 */
                    s_raw_stream_activation_due_us = UINT64_C(0);
                    /* 同步运行配置视图，但不写入 NVS。 */
                    s_device_config.raw_stream_enabled = false;
                    /* 断线后恢复当前页面策略，HOME 不得继续为已失效的 PC 诊断保持 ACTIVE 采样。 */
                    app_queue_raw_stream_power_policy(false);
                }
                /* 保存效果。 */
                device_effects_t effects;
                /* 更新连接。 */
                const device_coordinator_status_t status = device_coordinator_set_ble_connected(
                    &s_coordinator,
                    event.data.ble_connected,
                    app_coordinator_time_ms(event.monotonic_ms),
                    &effects);
                /* 成功时扇出。 */
                if (status == DEVICE_COORDINATOR_OK) {
                    /* 扇出效果。 */
                    app_dispatch_effects(&effects);
                }
                break;
            }
            /* 消费 NimBLE 最新配对码或清除快照。 */
            case APP_EVENT_PAIRING_UPDATE:
                /* 原子邮箱保证多字段一致；应用任务是 UI 唯一写者。 */
                app_drain_pairing_mailbox();
                break;
            /* 更新电池并执行 15/8/5% 规则。 */
            case APP_EVENT_BATTERY: {
                /* 保存效果。 */
                device_effects_t effects;
                /* 更新电量。 */
                const device_coordinator_status_t status = device_coordinator_update_battery(
                    &s_coordinator,
                    event.data.battery.percent,
                    event.data.battery.charging,
                    app_coordinator_time_ms(event.monotonic_ms),
                    &effects);
                /* 同步标准 Battery 特征。 */
                (void)ble_service_nimble_set_battery_percent(event.data.battery.percent);
                /* 成功时扇出，5% 关机仍由存储确认授权。 */
                if (status == DEVICE_COORDINATOR_OK) {
                    /* 扇出效果。 */
                    app_dispatch_effects(&effects);
                }
                break;
            }
            /* 每秒检查一次尚未发出的熄屏或长空闲门槛。 */
            case APP_EVENT_IDLE_POLL: {
                /* 即使即时提示事件曾因队列满丢失，每秒也补取一次最新配对邮箱。 */
                app_drain_pairing_mailbox();
                /* 保存可能到期的六位码清除事件。 */
                ui_event_t pairing_timeout_event;
                /* 60 秒到期时在应用任务清除敏感码，不依赖 NimBLE 再产生事件。 */
                if (ui_app_pairing_build_timeout_event(
                        &s_coordinator.ui,
                        (uint32_t)event.monotonic_ms,
                        &pairing_timeout_event)) {
                    /* 分派超时清除事件。 */
                    const ui_dispatch_result_t pairing_status = ui_dispatch_event(
                        &s_coordinator.ui,
                        &pairing_timeout_event);
                    /* 成功后恢复底层业务页面。 */
                    if (pairing_status == UI_DISPATCH_OK) {
                        /* 配对超时本身算一次活动边界，避免同一轮立即触发熄屏。 */
                        power_idle_timer_note_activity(&s_power_idle_timer, event.monotonic_ms);
                        /* 投递恢复页。 */
                        app_queue_ui(&s_coordinator.ui);
                    }
                }
                /* 常亮功能联调版每秒只读检查 GAP 活动位；无连接且广播意外停止时按当前策略补启。 */
                if (APP_BENCH_ALWAYS_ON && !APP_BENCH_DISABLE_BLE) {
                    /* 调用不覆盖 OFF 策略、不打断现有连接，也不会在健康广播时重启射频。 */
                    const esp_err_t advertise_status = ble_service_nimble_ensure_advertising();
                    /* ESP_FAIL 表示主机已同步但补启仍失败；未同步或明确 OFF 的 INVALID_STATE 留待下一轮。 */
                    if (advertise_status == ESP_FAIL) {
                        /* 输出稳定 ESP-IDF 错误名，便于串口区分上位机扫描问题与控制器启动失败。 */
                        ESP_LOGE(APP_TAG, "BLE advertising keepalive failed=%s", esp_err_to_name(advertise_status));
                    }
                }
                /* 配对码显示期间禁止普通 15～120 秒熄屏，保证完整 60 秒输入机会。 */
                if (s_coordinator.ui.view.pairing_active) {
                    /* 结束本次空闲检查。 */
                    break;
                }
                /* 现场联调阶段禁用全部自动低功耗入口，保证用户能持续查看并操作测试页面。 */
                if (APP_BENCH_ALWAYS_ON) {
                    /* 不产生 SCREEN_TIMEOUT 或 LONG_IDLE，电源状态持续保持 HOME/RUNNING/PAUSED 的亮屏策略。 */
                    break;
                }
                /* 保存空闲时钟输出事件类型。 */
                power_event_type_t idle_event_type = POWER_EVENT_SCREEN_TIMEOUT;
                /* 有准备、运行或暂停会话时只允许熄屏，不允许长空闲 Deep-sleep。 */
                const bool session_active = s_coordinator.power.session_active;
                /* 纯计时器只在首次跨过对应门槛时返回 true。 */
                if (power_idle_timer_poll(
                        &s_power_idle_timer,
                        event.monotonic_ms,
                        s_screen_timeout_ms,
                        APP_LONG_IDLE_MS,
                        session_active,
                        &idle_event_type)) {
                    /* 通过协调器原子提交 UI 与电源变化。 */
                    const device_coordinator_status_t idle_status = app_handle_idle_power_event(
                        idle_event_type,
                        event.monotonic_ms);
                    /* 页面或功耗已不接受该事件时只记录调试信息，计时器不会每秒重发。 */
                    if ((idle_status != DEVICE_COORDINATOR_OK) &&
                        (idle_status != DEVICE_COORDINATOR_ERR_STATE)) {
                        /* 输出异常状态。 */
                        ESP_LOGW(APP_TAG, "idle event=%d rejected status=%d",
                                 (int)idle_event_type,
                                 (int)idle_status);
                    }
                }
                /* 结束分支。 */
                break;
            }
            /* 关键摘要刷盘成功后授权 PMIC。 */
            case APP_EVENT_STORAGE_RESULT:
                if (event.data.storage_result.authorize_shutdown) {
                    /* 成功才授权，失败保持安全关机页但继续供电。 */
                    if (event.data.storage_result.success) {
                        /* 使用当前安全关机策略。 */
                        const power_policy_t policy = power_manager_policy(&s_coordinator.power);
                        /* 授权 PMIC。 */
                        app_queue_power(&policy, true);
                    } else {
                        /* 明确拒绝断电。 */
                        ESP_LOGE(APP_TAG, "summary persist failed; PMIC shutdown blocked");
                    }
                }
                break;
            /* 未知枚举不修改状态。 */
            default:
                ESP_LOGE(APP_TAG, "unknown app event=%d", (int)event.kind);
                break;
        }
    }
}

/* QMI 任务：读取芯片并按值投递，不调用 pipeline。 */
static void app_qmi_task(void *argument)
{
    /* 当前不使用参数。 */
    (void)argument;
    /* 标记是否需在下一帧前上报丢样。 */
    bool drop_pending = false;
    /* 永久轮询。 */
    while (true) {
        /* 非训练状态不访问 QMI 数据寄存器。 */
        if (!s_sensors.qmi_sampling_enabled) {
            /* 20 ms 检查一次状态，Home 功耗由后续实机测量。 */
            vTaskDelay(pdMS_TO_TICKS(20U));
            /* 继续。 */
            continue;
        }
        /* 先补交上次队列溢出事实。 */
        if (drop_pending) {
            /* 构造丢样事件。 */
            const app_event_t drop_event = {
                /* 标记丢样。 */
                .kind = APP_EVENT_QMI_DROP,
                /* 捕获时间。 */
                .monotonic_ms = app_now_ms(),
            };
            /* 成功后解除待上报。 */
            if (xQueueSend(s_app_event_queue, &drop_event, 0U) == pdPASS) {
                /* 清除待上报。 */
                drop_pending = false;
            }
        }
        /* 等待 QMI 专用锁，防止数据读取与 ACTIVE/WOM/OFF 寄存器事务交叉。 */
        if (xSemaphoreTake(s_sensors.qmi_mutex, portMAX_DELAY) != pdTRUE) {
            /* FreeRTOS 异常时记录一次数据不连续。 */
            drop_pending = true;
            /* 短暂退避，避免不可用互斥锁导致忙等。 */
            vTaskDelay(pdMS_TO_TICKS(10U));
            /* 继续下一轮。 */
            continue;
        }
        /* 锁内再次检查采样门控，关闭方可能已在等锁期间切换模式。 */
        if (!s_sensors.qmi_sampling_enabled) {
            /* 释放 QMI 独占锁，允许电源任务继续完成模式切换。 */
            (void)xSemaphoreGive(s_sensors.qmi_mutex);
            /* 20 ms 后再检查 ACTIVE 状态。 */
            vTaskDelay(pdMS_TO_TICKS(20U));
            /* 不访问 WOM/OFF 模式的数据寄存器。 */
            continue;
        }
        /* 保存一帧六轴定点数据和同一总线时戳。 */
        board_qmi8658_frame_t frame;
        /* 使用读取前单调时刻作为本次总线帧时间。 */
        const board_sensors_result_t status = board_qmi8658_read_available(
            &s_sensors.qmi,
            app_now_us(),
            &frame);
        /* 一次 I2C 读取已结束，在任何结果分支前释放 QMI 专用锁。 */
        (void)xSemaphoreGive(s_sensors.qmi_mutex);
        /* I2C 错误或驱动状态错误上报丢样并退避。 */
        if (status != BOARD_SENSORS_OK) {
            /* 标记丢样待应用任务重置。 */
            drop_pending = true;
            /* 10 ms 退避避免坏设备占满 I2C。 */
            vTaskDelay(pdMS_TO_TICKS(10U));
            /* 继续。 */
            continue;
        }
        /* 没有新轴时只等待下一轮。 */
        if (!frame.accel_available && !frame.gyro_available) {
            /* 按 4 ms 轮询。 */
            vTaskDelay(pdMS_TO_TICKS(APP_QMI_POLL_MS));
            /* 继续。 */
            continue;
        }
        /* 构造按值事件。 */
        app_event_t event;
        /* 清零。 */
        (void)memset(&event, 0, sizeof(event));
        /* 标记 QMI 帧。 */
        event.kind = APP_EVENT_QMI_FRAME;
        /* 保存同一帧毫秒时间。 */
        event.monotonic_ms = frame.timestamp_us / UINT64_C(1000);
        /* 复制原始帧。 */
        event.data.qmi_frame = frame;
        /* 队列满则丢弃本帧，并在恢复后上报连续性断裂。 */
        if (xQueueSend(s_app_event_queue, &event, 0U) != pdPASS) {
            /* 标记待上报。 */
            drop_pending = true;
        }
        /* 等待下一 DATA_READY。 */
        vTaskDelay(pdMS_TO_TICKS(APP_QMI_POLL_MS));
    }
}

/* 电池任务：30 秒读取一次 PMIC。 */
static void app_battery_task(void *argument)
{
    /* 当前不使用参数。 */
    (void)argument;
    /* 永久轮询。 */
    while (true) {
        /* 读取同一 PMIC 快照。 */
        uint8_t percent = UINT8_MAX;
        /* 保存充电状态。 */
        bool charging = false;
        /* 通过 runtime 回调读取。 */
        if (board_adapter_read_battery(
                board_runtime_adapter(&s_board_runtime),
                &percent,
                &charging) == BOARD_ADAPTER_OK) {
            /* 构造电池事件。 */
            app_event_t event;
            /* 清零。 */
            (void)memset(&event, 0, sizeof(event));
            /* 标记电池。 */
            event.kind = APP_EVENT_BATTERY;
            /* 捕获时间。 */
            event.monotonic_ms = app_now_ms();
            /* 写百分比。 */
            event.data.battery.percent = percent;
            /* 写充电状态。 */
            event.data.battery.charging = charging;
            /* 最多等待 100 ms，避免静默跳过 5% 门槛。 */
            if (xQueueSend(s_app_event_queue, &event, pdMS_TO_TICKS(100U)) != pdPASS) {
                /* 输出错误。 */
                ESP_LOGE(APP_TAG, "battery event queue full");
            }
        } else {
            /* PMIC 读取失败保留上一可信值。 */
            ESP_LOGW(APP_TAG, "AXP2101 battery read failed");
        }
        /* 等待 30 秒。 */
        vTaskDelay(pdMS_TO_TICKS(APP_BATTERY_POLL_MS));
    }
}

/* 空闲任务：只投递单调时钟采样，所有状态变化仍由 app_event_task 串行完成。 */
static void app_idle_task(void *argument)
{
    /* 当前不使用任务参数。 */
    (void)argument;
    /* 永久每秒采样一次。 */
    while (true) {
        /* 先等待完整周期，启动渲染不会被立即误判为空闲。 */
        vTaskDelay(pdMS_TO_TICKS(APP_IDLE_POLL_MS));
        /* 构造无联合体负载的空闲查询事件。 */
        const app_event_t event = {
            /* 标记空闲轮询。 */
            .kind = APP_EVENT_IDLE_POLL,
            /* 捕获当前单调毫秒。 */
            .monotonic_ms = app_now_ms(),
        };
        /* 队列短时满时丢弃本轮；下一秒仍会重试且计时基准不丢失。 */
        if (xQueueSend(s_app_event_queue, &event, 0U) != pdPASS) {
            /* 使用调试日志避免 QMI 突发期间每秒刷警告。 */
            ESP_LOGD(APP_TAG, "idle poll queue full");
        }
    }
}

/* UI 任务：只调用 renderer，所有 LVGL API 在 BSP 锁内。 */
static void app_ui_task(void *argument)
{
    /* 当前不使用参数。 */
    (void)argument;
    /* 保存页面快照。 */
    ui_context_t ui;
    /* 永久等待最新页面。 */
    while (true) {
        /* 所有页面都只等待领域层新快照；准备页不再用本地倒计时制造额外唤醒和重绘。 */
        const TickType_t wait_ticks = portMAX_DELAY;
        /* 标记本轮是否收到领域层新快照。 */
        const bool received = xQueueReceive(s_ui_queue, &ui, wait_ticks) == pdPASS;
        /* 只有收到新的领域事实才渲染，减少 QSPI 局部刷新并保持官方异步显示路径。 */
        if (received) {
            /* 渲染失败只记录，业务状态仍保持。 */
            const ui_lvgl_result_t status = ui_lvgl_renderer_render(&s_ui_renderer, &ui);
            /* 输出错误。 */
            if (status != UI_LVGL_OK) {
                /* 记录错误。 */
                ESP_LOGE(APP_TAG, "LVGL render failed=%d", (int)status);
            }
        }
    }
}

/* BLE 任务：发布状态/事件，并在响应后泵送冻结会话页。 */
static void app_ble_task(void *argument)
{
    /* 当前不使用参数。 */
    (void)argument;
    /* 保存输出消息。 */
    app_ble_output_t output;
    /* 保存已从传输服务取出、但尚未成功排入 NimBLE notification 的稳定数据块。 */
    uint8_t pending_transfer_payload[SESSION_TRANSFER_DATA_SIZE];
    /* 保存 pending_transfer_payload 的有效长度。 */
    size_t pending_transfer_length = 0U;
    /* 标记是否存在需重试的同一数据块，发送失败时禁止继续 pop 下一项。 */
    bool transfer_payload_pending = false;
    /* 首次 BLE 健康检查延后 1 秒，允许 app_main 完成 NimBLE 异步主机启动。 */
    uint64_t next_ble_health_ms = app_now_ms() + UINT64_C(1000);
    /* 每五次一秒检查输出一次状态，兼顾可观测性与串口噪声。 */
    uint8_t ble_health_log_divider = 0U;
    /* 永久运行。 */
    while (true) {
        /* 常亮功能联调版由独立 BLE 任务保活，避免 125 Hz IMU 队列挤掉 IDLE_POLL 后失去检查机会。 */
        const uint64_t now_ms = app_now_ms();
        /* 单调时钟达到下一秒门槛且 BLE 未被编译期隔离时执行一次检查。 */
        if (APP_BENCH_ALWAYS_ON && !APP_BENCH_DISABLE_BLE && (now_ms >= next_ble_health_ms)) {
            /* 下一检查从当前时刻顺延 1 秒；任务短暂阻塞不会连续补跑旧周期。 */
            next_ble_health_ms = now_ms + UINT64_C(1000);
            /* 保存启动、同步、广播、连接和射频模式快照。 */
            ble_service_nimble_runtime_status_t runtime_status;
            /* 先尝试恢复异常停止的广播；健康广播和现有连接不会被重启。 */
            const esp_err_t ensure_status = ble_service_nimble_ensure_advertising();
            /* 恢复调用后读取最终状态，日志反映本轮结束事实。 */
            const esp_err_t snapshot_status = ble_service_nimble_get_runtime_status(&runtime_status);
            /* 每五秒输出一次；uint8 只在 0～4 循环，不存在溢出风险。 */
            ble_health_log_divider = (uint8_t)((ble_health_log_divider + UINT8_C(1)) % UINT8_C(5));
            /* 快照读取失败属于编程错误；正常路径按固定间隔输出全部生命周期位。 */
            if (snapshot_status != ESP_OK) {
                /* 记录空指针或未来 API 错误，当前静态对象不应进入该路径。 */
                ESP_LOGE(APP_TAG, "BLE runtime snapshot failed=%s", esp_err_to_name(snapshot_status));
            } else if (ble_health_log_divider == 0U) {
                /* 输出可由串口脚本解析的稳定键值，不包含密钥、地址或用户数据。 */
                ESP_LOGI(
                    APP_TAG,
                    "BLE_STATUS started=%d synced=%d advertising=%d physical=%d secure=%d mode=%d ensure=%s",
                    runtime_status.started ? 1 : 0,
                    runtime_status.host_synced ? 1 : 0,
                    runtime_status.advertising ? 1 : 0,
                    runtime_status.physical_connection ? 1 : 0,
                    runtime_status.secure_connection ? 1 : 0,
                    (int)runtime_status.power_mode,
                    esp_err_to_name(ensure_status));
            }
        }
        /* 最多等待 100 ms，使会话传输有固定泵送机会。 */
        if (xQueueReceive(s_ble_queue, &output, pdMS_TO_TICKS(APP_TRANSFER_PUMP_MS)) == pdPASS) {
            /* 发布 LiveState。 */
            if (output.kind == APP_BLE_OUTPUT_LIVE_STATE) {
                /* 忽略未连接的无订阅错误。 */
                (void)ble_service_nimble_publish_live_state(&output.live_state);
            } else if (output.kind == APP_BLE_OUTPUT_EVENT) {
                /* 发布 EventV1 payload。 */
                (void)ble_service_nimble_publish_event(
                    output.event_payload,
                    output.event_length,
                    output.event_monotonic_ms);
            } else if ((output.kind == APP_BLE_OUTPUT_RAW_STREAM) && s_raw_stream_enabled) {
                /* 仅在易失开关仍开启时发布；关闭后排队的旧帧会被静默丢弃。 */
                (void)ble_service_nimble_publish_raw_stream(output.raw_payload, output.raw_length);
            } else if ((output.kind == APP_BLE_OUTPUT_INFERENCE_DIAGNOSTIC) &&
                       s_raw_stream_enabled) {
                /* 复用 Raw Stream 的已加密订阅发布分类窗口，不阻塞应用推理任务。 */
                (void)ble_service_nimble_publish_inference_diagnostic(
                    output.inference_payload,
                    output.inference_length);
            }
        }
        /* 未连接时不消费 transfer 页，保留给 PC 重试。 */
        if (!ble_service_nimble_is_connected()) {
            /* 断线后丢弃本地已 pop 副本；PC 重连会用游标重新发 TransferRequest 冻结同一页。 */
            transfer_payload_pending = false;
            /* 清零长度，避免诊断误读旧缓存。 */
            pending_transfer_length = 0U;
            /* 继续等待。 */
            continue;
        }
        /* TransferResponse 的全部 indication 分片必须先由 PC 确认，随后才能发送对应 TransferData。 */
        if (ble_service_nimble_reliable_response_pending()) {
            /* 等下一次 100 ms 泵送机会，不提前消费会话页。 */
            continue;
        }
        /* 没有待重试数据时，才从冻结传输页取出下一条。 */
        if (!transfer_payload_pending) {
            /* 取得 transfer 锁，避免与独立 BLE 业务任务冻结新页并发修改游标。 */
            if (xSemaphoreTake(s_session_transfer_mutex, pdMS_TO_TICKS(20U)) == pdTRUE) {
                /* 取出一条冻结数据并保存到跨循环稳定缓存。 */
                transfer_payload_pending = session_transfer_service_pop_data(
                    &s_session_transfer,
                    pending_transfer_payload,
                    sizeof(pending_transfer_payload),
                    &pending_transfer_length);
                /* 释放锁。 */
                (void)xSemaphoreGive(s_session_transfer_mutex);
            }
        }
        /* 存在数据时尝试发布；本地排队失败保留完全相同 payload 下轮重试。 */
        if (transfer_payload_pending) {
            /* 检查长度后发送。 */
            const esp_err_t error = ble_service_nimble_publish_transfer_data(
                pending_transfer_payload,
                (uint16_t)pending_transfer_length);
            /* 成功排入 NimBLE 后才允许读取下一条；无线 notification 丢失仍由 PC 游标重试恢复。 */
            if (error == ESP_OK) {
                /* 清除待重试标志。 */
                transfer_payload_pending = false;
                /* 清零长度。 */
                pending_transfer_length = 0U;
            }
            /* 本地发送失败保留同一缓存，不前移传输服务第二次。 */
            if (error != ESP_OK) {
                /* 记录错误。 */
                ESP_LOGW(APP_TAG, "transfer data publish failed=%s", esp_err_to_name(error));
            }
        }
    }
}

/* 存储任务：唯一写 session_store；每次 upsert 内含双槽 sync。 */
static void app_storage_task(void *argument)
{
    /* 当前不使用参数。 */
    (void)argument;
    /* 保存事务。 */
    app_storage_request_t request;
    /* 永久处理。 */
    while (true) {
        /* 阻塞等待摘要。 */
        if (xQueueReceive(s_storage_queue, &request, portMAX_DELAY) != pdPASS) {
            /* 继续等待。 */
            continue;
        }
        /* 取得仓储锁。 */
        bool success = false;
        /* 最多等待 2 秒，关机事务不能无限卡死。 */
        if (xSemaphoreTake(s_session_store_mutex, pdMS_TO_TICKS(2000U)) == pdTRUE) {
            /* 保存是否实际改变；重复 event_seq 不写 Flash。 */
            bool changed = false;
            /* 执行幂等 upsert 和双槽同步。 */
            success = session_store_upsert(&s_session_store, &request.summary, &changed) ==
                      SESSION_STORE_STATUS_OK;
            /* 释放锁。 */
            (void)xSemaphoreGive(s_session_store_mutex);
        }
        /* 输出写入结果。 */
        if (!success) {
            /* 记录失败。 */
            ESP_LOGE(APP_TAG, "session persist failed seq=%lu", (unsigned long)request.summary.session_seq);
        }
        /* 只有关机事务需要回应用任务授权。 */
        if (request.shutdown_after_persist) {
            /* 构造确认。 */
            app_event_t event;
            /* 清零。 */
            (void)memset(&event, 0, sizeof(event));
            /* 标记存储结果。 */
            event.kind = APP_EVENT_STORAGE_RESULT;
            /* 捕获完成时间。 */
            event.monotonic_ms = app_now_ms();
            /* 保存成功。 */
            event.data.storage_result.success = success;
            /* 保存关机授权意图。 */
            event.data.storage_result.authorize_shutdown = true;
            /* 等待送达，避免关键确认丢失。 */
            (void)xQueueSend(s_app_event_queue, &event, portMAX_DELAY);
        }
    }
}

/* 为自动 Light-sleep 安装 GPIO38 触摸和 GPIO39 RTC 的低电平唤醒源。 */
static bool app_configure_light_sleep_wake(const power_policy_t *policy)
{
    /* 策略指针为空时无法决定应启用哪个唤醒源。 */
    if (policy == NULL) {
        /* 返回失败，上层将禁用本次自动 Light-sleep。 */
        return false;
    }
    /* 取得已探测板型的真实引脚表，避免在主程序重复硬编码。 */
    board_adapter_t *const adapter = board_runtime_adapter(&s_board_runtime);
    /* 板级运行时未初始化时不允许进入不可验证的睡眠状态。 */
    if (adapter == NULL) {
        /* 返回失败。 */
        return false;
    }
    /* 先移除上一份策略的触摸 GPIO 唤醒，防止状态切换后残留旧源。 */
    const esp_err_t touch_disable_error = gpio_wakeup_disable(
        (gpio_num_t)adapter->profile.touch_interrupt_gpio);
    /* 除了“本来未启用”，其它 GPIO 关闭错误需记录供真机追踪。 */
    if ((touch_disable_error != ESP_OK) && (touch_disable_error != ESP_ERR_INVALID_STATE)) {
        /* 输出触摸引脚和 ESP-IDF 错误名称。 */
        ESP_LOGW(
            APP_TAG,
            "touch GPIO wake disable failed gpio=%u error=%s",
            (unsigned int)adapter->profile.touch_interrupt_gpio,
            esp_err_to_name(touch_disable_error));
    }
    /* 移除上一份策略的 RTC GPIO 唤醒。 */
    const esp_err_t rtc_disable_error = gpio_wakeup_disable(
        (gpio_num_t)adapter->profile.rtc_interrupt_gpio);
    /* 仅忽略源未启用的正常状态。 */
    if ((rtc_disable_error != ESP_OK) && (rtc_disable_error != ESP_ERR_INVALID_STATE)) {
        /* 记录本地 RTC 唤醒源关闭失败。 */
        ESP_LOGW(
            APP_TAG,
            "RTC GPIO wake disable failed gpio=%u error=%s",
            (unsigned int)adapter->profile.rtc_interrupt_gpio,
            esp_err_to_name(rtc_disable_error));
    }
    /* 禁用 ESP 睡眠层的 GPIO 唤醒聚合源，后面按新策略重建。 */
    const esp_err_t source_disable_error = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    /* 未启用该源会返回 INVALID_STATE，属于首次配置正常情况。 */
    if ((source_disable_error != ESP_OK) && (source_disable_error != ESP_ERR_INVALID_STATE)) {
        /* 无法清理旧源时禁用自动 Light-sleep，避免未知唤醒路径。 */
        ESP_LOGE(
            APP_TAG,
            "Light-sleep GPIO source reset failed=%s",
            esp_err_to_name(source_disable_error));
        /* 返回失败。 */
        return false;
    }
    /* 策略不需要自动 Light-sleep 时，保持 GPIO 唤醒源关闭即可。 */
    if (!policy->automatic_light_sleep) {
        /* 返回配置成功。 */
        return true;
    }
    /* 记录是否至少启用了一个板级 GPIO 唤醒源。 */
    bool gpio_source_enabled = false;
    /* 触摸 INT 在本板为 GPIO38 低有效，只能用数字 GPIO Light-sleep 唤醒。 */
    if (policy->enable_touch_light_wake) {
        /* 配置触摸 INT 低电平唤醒。 */
        const esp_err_t touch_enable_error = gpio_wakeup_enable(
            (gpio_num_t)adapter->profile.touch_interrupt_gpio,
            GPIO_INTR_LOW_LEVEL);
        /* 引脚不支持或驱动配置失败时不启用自动 Light-sleep。 */
        if (touch_enable_error != ESP_OK) {
            /* 记录实际引脚和错误。 */
            ESP_LOGE(
                APP_TAG,
                "touch Light-sleep wake enable failed gpio=%u error=%s",
                (unsigned int)adapter->profile.touch_interrupt_gpio,
                esp_err_to_name(touch_enable_error));
            /* 返回失败。 */
            return false;
        }
        /* 标记已有一个有效 GPIO 源。 */
        gpio_source_enabled = true;
    }
    /* PCF85063 INT 在本板为 GPIO39 低有效，只用于 Light-sleep。 */
    if (policy->enable_rtc_light_wake) {
        /* 配置 RTC 低电平唤醒。 */
        const esp_err_t rtc_enable_error = gpio_wakeup_enable(
            (gpio_num_t)adapter->profile.rtc_interrupt_gpio,
            GPIO_INTR_LOW_LEVEL);
        /* 任一要求的唤醒源失败都禁用本次自动 Light-sleep。 */
        if (rtc_enable_error != ESP_OK) {
            /* 记录 RTC 引脚和错误。 */
            ESP_LOGE(
                APP_TAG,
                "RTC Light-sleep wake enable failed gpio=%u error=%s",
                (unsigned int)adapter->profile.rtc_interrupt_gpio,
                esp_err_to_name(rtc_enable_error));
            /* 返回失败。 */
            return false;
        }
        /* 标记已有有效 GPIO 源。 */
        gpio_source_enabled = true;
    }
    /* 策略可能只依赖 FreeRTOS 定时器唤醒；没有 GPIO 源时不需启用 GPIO 聚合源。 */
    if (!gpio_source_enabled) {
        /* 自动 Light-sleep 仍可由最近的 RTOS 定时器唤醒。 */
        return true;
    }
    /* 通知 ESP 睡眠层把上述数字 GPIO 中断纳入 Light-sleep 唤醒条件。 */
    const esp_err_t gpio_source_error = esp_sleep_enable_gpio_wakeup();
    /* 聚合源启用失败时拒绝自动 Light-sleep。 */
    if (gpio_source_error != ESP_OK) {
        /* 记录 ESP-IDF 错误。 */
        ESP_LOGE(
            APP_TAG,
            "Light-sleep GPIO source enable failed=%s",
            esp_err_to_name(gpio_source_error));
        /* 返回失败。 */
        return false;
    }
    /* 返回配置成功。 */
    return true;
}

/* 检查 QMI WOM 全链路并为 GPIO21 安装 Deep-sleep EXT1 高电平唤醒。 */
static bool app_prepare_deep_sleep_wake(
    const power_policy_t *policy,
    const bool wom_configured)
{
    /* 策略指针为空时绝不允许进入 Deep-sleep。 */
    if (policy == NULL) {
        /* 返回失败。 */
        return false;
    }
    /* 取得已初始化的板级配置。 */
    board_adapter_t *const adapter = board_runtime_adapter(&s_board_runtime);
    /* 板型不可用时无法验证 GPIO21 是否为 RTC IO。 */
    if (adapter == NULL) {
        /* 返回失败。 */
        return false;
    }
    /* 同时检查产品策略、编译开关、QMI 探测、RTC IO 范围和 WOM 寄存器结果。 */
    if (!board_runtime_qmi_deep_wake_ready(
            &s_board_runtime,
            policy->enable_imu_deep_wake,
            wom_configured)) {
        /* 任一先决条件缺失时拒绝无唤醒源 Deep-sleep。 */
        return false;
    }
    /* 清除 Light-sleep GPIO 等旧唤醒源，Deep-sleep 只保留经验证的 QMI INT1。 */
    const esp_err_t disable_error = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    /* ALL 清理失败表示唤醒状态不可预测，必须拒绝睡眠。 */
    if (disable_error != ESP_OK) {
        /* 记录错误名称。 */
        ESP_LOGE(APP_TAG, "Deep-sleep wake reset failed=%s", esp_err_to_name(disable_error));
        /* 返回失败。 */
        return false;
    }
    /* 把 GPIO21 编码为 64 位 EXT1 位图；运行时门控已保证引脚不超过 21。 */
    const uint64_t imu_wake_mask = UINT64_C(1) << adapter->profile.imu_interrupt_gpio;
    /* QMI WOM 输出动作高电平，ESP32-S3 EXT1 使用 ANY_HIGH 唤醒。 */
    const esp_err_t ext1_error = esp_sleep_enable_ext1_wakeup_io(
        imu_wake_mask,
        ESP_EXT1_WAKEUP_ANY_HIGH);
    /* 引脚不是 RTC IO 或 EXT1 配置冲突时不能进入 Deep-sleep。 */
    if (ext1_error != ESP_OK) {
        /* 记录引脚、位图和 ESP-IDF 错误。 */
        ESP_LOGE(
            APP_TAG,
            "QMI EXT1 wake enable failed gpio=%u mask=0x%llX error=%s",
            (unsigned int)adapter->profile.imu_interrupt_gpio,
            (unsigned long long)imu_wake_mask,
            esp_err_to_name(ext1_error));
        /* 返回失败。 */
        return false;
    }
    /* 返回唤醒源已完整安装。 */
    return true;
}

/* 把纯电源领域 BLE 策略逐项映射到 NimBLE 适配层，避免两个组件共享枚举数值假设。 */
static ble_service_nimble_power_mode_t app_map_ble_power_mode(const power_ble_mode_t mode)
{
    /* 按领域枚举显式选择射频模式；未知值使用关闭策略。 */
    switch (mode) {
        /* Deep-sleep 与安全关机前停止广播或连接。 */
        case POWER_BLE_OFF:
            /* 返回 NimBLE 关闭枚举。 */
            return BLE_SERVICE_NIMBLE_POWER_OFF;
        /* 主页和亮屏训练使用快速广播。 */
        case POWER_BLE_FAST_ADVERTISING:
            /* 返回 100～150 ms 广播模式。 */
            return BLE_SERVICE_NIMBLE_POWER_FAST_ADVERTISING;
        /* 熄屏无连接待机使用慢广播。 */
        case POWER_BLE_SLOW_ADVERTISING:
            /* 返回 1.0～1.2 s 广播模式。 */
            return BLE_SERVICE_NIMBLE_POWER_SLOW_ADVERTISING;
        /* 训练连接要求低延迟上传。 */
        case POWER_BLE_CONNECTED_ACTIVE:
            /* 返回活动连接参数。 */
            return BLE_SERVICE_NIMBLE_POWER_CONNECTED_ACTIVE;
        /* 无训练连接待机允许从机延迟和 modem-sleep。 */
        case POWER_BLE_CONNECTED_MODEM_SLEEP:
            /* 返回连接省电参数。 */
            return BLE_SERVICE_NIMBLE_POWER_CONNECTED_MODEM_SLEEP;
        /* 内存损坏或未来未知枚举按关闭处理。 */
        default:
            /* 返回最保守关闭模式。 */
            return BLE_SERVICE_NIMBLE_POWER_OFF;
    }
}

/* 差分应用 BLE 射频模式；离线模式不阻断本地训练。 */
static void app_apply_ble_power_mode(const power_ble_mode_t mode)
{
    /* 转换为 NimBLE 公开枚举。 */
    const ble_service_nimble_power_mode_t mapped_mode = app_map_ble_power_mode(mode);
    /* 已成功应用同一模式时不重复提交连接参数更新。 */
    if (s_applied_ble_power_mode == (int)mapped_mode) {
        /* 无需访问主机栈。 */
        return;
    }
    /* 提交广播启停、快慢间隔或连接省电参数。 */
    const esp_err_t status = ble_service_nimble_set_power_mode(mapped_mode);
    /* 成功时更新差分缓存。 */
    if (status == ESP_OK) {
        /* 只有成功事实进入缓存，失败策略会在下一次电源效果重试。 */
        s_applied_ble_power_mode = (int)mapped_mode;
        /* 结束成功路径。 */
        return;
    }
    /* BLE 服务未启动表示设备处于离线模式，不影响本地识别和低功耗外设。 */
    if (status == ESP_ERR_INVALID_STATE) {
        /* 使用调试级日志，避免离线设备每次策略变化刷错误。 */
        ESP_LOGD(APP_TAG, "BLE power mode skipped: service offline mode=%d", (int)mapped_mode);
        /* 结束离线路径。 */
        return;
    }
    /* 其它错误表示 GAP 参数或主机状态异常，需要现场诊断。 */
    ESP_LOGW(
        APP_TAG,
        "BLE power mode apply failed mode=%d error=%s",
        (int)mapped_mode,
        esp_err_to_name(status));
}

/* 电源任务：差分应用显示、触摸、IMU、CPU 和最终睡眠/关机。 */
static void app_power_task(void *argument)
{
    /* 当前不使用参数。 */
    (void)argument;
    /* 保存最新策略。 */
    app_power_request_t request;
    /* 永久处理。 */
    while (true) {
        /* 等待策略。 */
        if (xQueueReceive(s_power_queue, &request, portMAX_DELAY) != pdPASS) {
            /* 继续。 */
            continue;
        }
        /* 先安装本策略要求的 Light-sleep GPIO 唤醒源。 */
        const bool light_wake_ready = app_configure_light_sleep_wake(&request.policy);
        /* 设置动态频率和自动 Light-sleep。 */
        const esp_pm_config_t pm = {
            /* 使用策略最高 MHz。 */
            .max_freq_mhz = (int)request.policy.max_cpu_mhz,
            /* 使用策略最低 MHz。 */
            .min_freq_mhz = (int)request.policy.min_cpu_mhz,
            /* 只在策略允许且唤醒源配置成功时启用自动 Light-sleep。 */
            .light_sleep_enable = request.policy.automatic_light_sleep && light_wake_ready,
        };
        /* 配置失败只记录，继续应用外设安全状态。 */
        const esp_err_t pm_error = esp_pm_configure(&pm);
        /* 输出失败。 */
        if (pm_error != ESP_OK) {
            /* 记录错误。 */
            ESP_LOGW(APP_TAG, "esp_pm_configure failed=%s", esp_err_to_name(pm_error));
        }
        /* 默认把无效或明确 OFF 策略映射为完全关闭传感器。 */
        board_runtime_qmi_mode_t qmi_mode = BOARD_RUNTIME_QMI_OFF;
        /* 按电源领域枚举选择驱动层寄存器模式。 */
        switch (request.policy.imu_mode) {
            /* 25 Hz 识别策略需要恢复 ACTIVE 量程和 ODR。 */
            case POWER_IMU_ACTIVE_25HZ:
                /* 选择活动采样。 */
                qmi_mode = BOARD_RUNTIME_QMI_ACTIVE;
                /* 结束分支。 */
                break;
            /* 待机与 Deep-sleep 策略需要 21 Hz 低功耗加速度 WOM。 */
            case POWER_IMU_WAKE_ON_MOTION:
                /* 选择运动唤醒。 */
                qmi_mode = BOARD_RUNTIME_QMI_WAKE_ON_MOTION;
                /* 结束分支。 */
                break;
            /* 显式 OFF 与未知枚举都保留安全关闭。 */
            case POWER_IMU_OFF:
            default:
                /* 保持 OFF。 */
                qmi_mode = BOARD_RUNTIME_QMI_OFF;
                /* 结束分支。 */
                break;
        }
        /* 串行切换 QMI 的 ACTIVE/WOM/OFF 真实寄存器状态。 */
        const board_runtime_result_t qmi_status = board_runtime_set_qmi_mode(
            &s_board_runtime,
            qmi_mode);
        /* 记录模式切换失败；Deep-sleep 门控会因 wom_configured=false 拒绝睡眠。 */
        if (qmi_status != BOARD_RUNTIME_OK) {
            /* 输出目标模式和 runtime 错误码。 */
            ESP_LOGE(
                APP_TAG,
                "QMI power mode change failed mode=%d status=%d",
                (int)qmi_mode,
                (int)qmi_status);
        }
        /* 只有 WOM 模式真实写寄存器成功才允许后续 EXT1 门控。 */
        const bool wom_configured = (qmi_status == BOARD_RUNTIME_OK) &&
                                    (qmi_mode == BOARD_RUNTIME_QMI_WAKE_ON_MOTION);
        /* Deep-sleep 前必须先验证 WOM 和 EXT1；失败时不得提前关闭 BLE、屏幕或触摸。 */
        if (request.policy.request_deep_sleep) {
            /* 安装 GPIO21 EXT1 ANY_HIGH 并检查全链路先决条件。 */
            if (!app_prepare_deep_sleep_wake(&request.policy, wom_configured)) {
                /* 拒绝没有可验证唤醒源的 Deep-sleep，避免设备变成无法唤醒。 */
                ESP_LOGE(
                    APP_TAG,
                    "Deep-sleep refused: QMI WOM or GPIO21 EXT1 wake source unavailable");
                /* 保留此前成功应用的 BLE、显示和触摸状态；下一次用户/电源事件可重试。 */
                continue;
            }
        }
        /* RawStream 临时 QMI 覆盖必须保持当前 BLE 会话；其它策略继续差分切换射频模式。 */
        if (!request.preserve_ble_link) {
            /* 仅普通页面、训练、待机和关机请求允许提交 GAP 连接或广播参数。 */
            app_apply_ble_power_mode(request.policy.ble_mode);
        }
        /* 应用 AMOLED 物理 Display On/Off 与亮度。 */
        (void)board_adapter_set_display(
            board_runtime_adapter(&s_board_runtime),
            request.policy.display_on,
            request.policy.display_brightness_percent);
        /* 应用 FT3168 硬件活动状态；待机触摸唤醒策略会保持 true。 */
        (void)board_runtime_set_touch_active(&s_board_runtime, request.policy.touch_active);
        /* 扬声器 v1 默认关闭。 */
        (void)board_runtime_set_speaker_enabled(&s_board_runtime, request.policy.speaker_active);
        /* TF 常规卸载，仅显式日志事务挂载。 */
        (void)board_runtime_set_storage_active(&s_board_runtime, request.policy.storage_active);
        /* 只有唤醒链验证通过且外设已经安全降功耗后才真正进入 Deep-sleep。 */
        if (request.policy.request_deep_sleep) {
            /* 确保 NimBLE 主机和控制器已停止；重复 stop 由服务幂等处理。 */
            (void)ble_service_nimble_stop();
            /* 给日志和 VFS 50 ms 调度机会。 */
            vTaskDelay(pdMS_TO_TICKS(50U));
            /* 进入 Deep-sleep；本函数成功后不会返回。 */
            esp_deep_sleep_start();
        }
        /* PMIC 请求必须同时具有刷盘授权。 */
        if (request.policy.request_pmic_shutdown && request.persist_authorized) {
            /* 停止 BLE 广播/连接。 */
            (void)ble_service_nimble_stop();
            /* 留 600 ms 播放轻量关机动画。 */
            vTaskDelay(pdMS_TO_TICKS(600U));
            /* 请求 AXP2101 真关机。 */
            const board_runtime_result_t status =
                board_runtime_request_pmic_shutdown(&s_board_runtime);
            /* PMIC 写失败时保留供电并输出错误。 */
            if (status != BOARD_RUNTIME_OK) {
                /* 记录错误。 */
                ESP_LOGE(APP_TAG, "AXP2101 shutdown failed=%d", (int)status);
            }
        }
    }
}

/* 创建所有队列和同步对象。 */
static bool app_create_os_objects(void)
{
    /* 创建主事件队列。 */
    s_app_event_queue = xQueueCreate(APP_EVENT_QUEUE_LENGTH, sizeof(app_event_t));
    /* 创建 UI 单槽控制邮箱；只保存最新点击，不与高频 QMI 帧共享容量。 */
    s_ui_command_queue = xQueueCreate(
        APP_UI_COMMAND_QUEUE_LENGTH,
        sizeof(app_ui_command_event_t));
    /* 创建 Queue Set；容量等于两个成员队列长度之和，保证每个就绪事实都有槽位。 */
    s_app_input_queue_set = xQueueCreateSet(
        APP_EVENT_QUEUE_LENGTH + APP_UI_COMMAND_QUEUE_LENGTH);
    /* 创建 UI 最新帧队列。 */
    s_ui_queue = xQueueCreate(APP_UI_QUEUE_LENGTH, sizeof(ui_context_t));
    /* 创建 BLE 输出队列。 */
    s_ble_queue = xQueueCreate(APP_BLE_QUEUE_LENGTH, sizeof(app_ble_output_t));
    /* 创建存储队列。 */
    s_storage_queue = xQueueCreate(APP_STORAGE_QUEUE_LENGTH, sizeof(app_storage_request_t));
    /* 创建电源最新策略队列。 */
    s_power_queue = xQueueCreate(APP_POWER_QUEUE_LENGTH, sizeof(app_power_request_t));
    /* 创建 BLE broker 互斥锁。 */
    s_ble_command_mutex = xSemaphoreCreateMutex();
    /* 创建 BLE broker 完成信号。 */
    s_ble_command_done = xSemaphoreCreateBinary();
    /* 创建仓储互斥锁。 */
    s_session_store_mutex = xSemaphoreCreateMutex();
    /* 创建传输服务互斥锁。 */
    s_session_transfer_mutex = xSemaphoreCreateMutex();
    /* 创建 QMI 专用互斥锁，串行化数据读取与低功耗寄存器切换。 */
    s_sensors.qmi_mutex = xSemaphoreCreateMutex();
    /* 默认标记两个成员尚未加入 Queue Set；任一对象为空时保持失败。 */
    BaseType_t app_queue_added = pdFAIL;
    /* 默认标记 UI 邮箱尚未加入 Queue Set。 */
    BaseType_t ui_queue_added = pdFAIL;
    /* 三个句柄均有效后才允许注册成员，避免向 FreeRTOS 传入空队列。 */
    if ((s_app_input_queue_set != NULL) &&
        (s_app_event_queue != NULL) &&
        (s_ui_command_queue != NULL)) {
        /* 把主数据队列加入等待集合。 */
        app_queue_added = xQueueAddToSet(s_app_event_queue, s_app_input_queue_set);
        /* 把 UI 控制邮箱加入同一等待集合。 */
        ui_queue_added = xQueueAddToSet(s_ui_command_queue, s_app_input_queue_set);
    }
    /* 全部对象必须成功。 */
    return (s_app_event_queue != NULL) && (s_ui_command_queue != NULL) &&
           (s_app_input_queue_set != NULL) &&
           (app_queue_added == pdPASS) && (ui_queue_added == pdPASS) &&
           (s_ui_queue != NULL) &&
           (s_ble_queue != NULL) && (s_storage_queue != NULL) &&
           (s_power_queue != NULL) &&
           (s_ble_command_mutex != NULL) && (s_ble_command_done != NULL) &&
           (s_session_store_mutex != NULL) && (s_session_transfer_mutex != NULL) &&
           (s_sensors.qmi_mutex != NULL);
}

/* 初始化 NVS，处理分区页耗尽和版本变化。 */
static esp_err_t app_init_nvs(void)
{
    /* 首次初始化。 */
    esp_err_t error = nvs_flash_init();
    /* 仅在官方可恢复错误下擦除后重建。 */
    if ((error == ESP_ERR_NVS_NO_FREE_PAGES) || (error == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        /* 擦除旧 NVS。 */
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), APP_TAG, "NVS erase failed");
        /* 重新初始化。 */
        error = nvs_flash_init();
    }
    /* 返回结果。 */
    return error;
}

/* 把有效配置同步写入 NVS；RawStream 属于断线即关闭的易失状态，不写入持久化 blob。 */
static esp_err_t app_persist_device_config(const device_config_t *config)
{
    /* 必填配置不能为空。 */
    if (config == NULL) {
        /* 返回 ESP-IDF 非法参数。 */
        return ESP_ERR_INVALID_ARG;
    }
    /* 创建持久化副本，避免修改应用任务正在使用的运行配置。 */
    device_config_t persisted = *config;
    /* 原始六轴流重启、断线或换用户后必须重新显式授权。 */
    persisted.raw_stream_enabled = false;
    /* 保存固定 44 字节 CRC32 blob。 */
    uint8_t blob[DEVICE_CONFIG_BLOB_SIZE];
    /* 保存编码输出长度。 */
    size_t blob_length = 0U;
    /* 使用纯 C codec 验证全部范围并编码。 */
    const device_config_status_t encode_status = device_config_blob_encode(
        &persisted,
        blob,
        sizeof(blob),
        &blob_length);
    /* 内部配置非法或尺寸漂移时拒绝写 NVS。 */
    if ((encode_status != DEVICE_CONFIG_OK) || (blob_length != sizeof(blob))) {
        /* 记录 codec 状态。 */
        ESP_LOGE(APP_TAG, "device config encode failed=%d len=%u",
                 (int)encode_status,
                 (unsigned int)blob_length);
        /* 返回参数错误。 */
        return ESP_ERR_INVALID_ARG;
    }
    /* 打开独立读写命名空间。 */
    nvs_handle_t handle = 0U;
    /* NVS 打开失败时不改变运行配置。 */
    esp_err_t error = nvs_open(APP_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    /* 传播打开错误。 */
    if (error != ESP_OK) {
        /* 返回原错误。 */
        return error;
    }
    /* 写入完整 blob；NVS 自身还提供条目级 CRC 和断电恢复。 */
    error = nvs_set_blob(handle, APP_CONFIG_NVS_KEY, blob, blob_length);
    /* set 成功后 commit 才形成跨重启事实。 */
    if (error == ESP_OK) {
        /* 提交当前命名空间事务。 */
        error = nvs_commit(handle);
    }
    /* 无论成功或失败都释放句柄。 */
    nvs_close(handle);
    /* 返回最终 NVS 状态。 */
    return error;
}

/* 从 NVS 恢复设备配置；缺失、损坏或旧格式均安全回退冻结默认值。 */
static void app_load_device_config(void)
{
    /* 先写完整默认值，任何读取失败都保留可启动配置。 */
    device_config_set_defaults(&s_device_config);
    /* 启动时强制关闭原始流。 */
    s_raw_stream_enabled = false;
    /* 尝试只读打开产品命名空间。 */
    nvs_handle_t handle = 0U;
    /* 命名空间从未创建时属于首次启动。 */
    const esp_err_t open_error = nvs_open(APP_CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
    /* 首次启动直接使用默认值。 */
    if (open_error == ESP_ERR_NVS_NOT_FOUND) {
        /* 写入默认熄屏毫秒。 */
        s_screen_timeout_ms = (uint32_t)s_device_config.screen_timeout_seconds * UINT32_C(1000);
        /* 返回。 */
        return;
    }
    /* 其它打开错误记录后继续默认值。 */
    if (open_error != ESP_OK) {
        /* 记录 NVS 错误。 */
        ESP_LOGW(APP_TAG, "device config NVS open failed=%s; using defaults",
                 esp_err_to_name(open_error));
        /* 写入默认熄屏毫秒。 */
        s_screen_timeout_ms = (uint32_t)s_device_config.screen_timeout_seconds * UINT32_C(1000);
        /* 返回。 */
        return;
    }
    /* 固定容量缓冲只接受当前 44 字节格式。 */
    uint8_t blob[DEVICE_CONFIG_BLOB_SIZE];
    /* 输入长度初始化为完整容量，NVS 会返回实际所需长度。 */
    size_t blob_length = sizeof(blob);
    /* 读取完整配置条目。 */
    const esp_err_t read_error = nvs_get_blob(
        handle,
        APP_CONFIG_NVS_KEY,
        blob,
        &blob_length);
    /* 读取结束立即关闭句柄。 */
    nvs_close(handle);
    /* 缺键表示升级前设备，继续默认值。 */
    if (read_error == ESP_ERR_NVS_NOT_FOUND) {
        /* 写入默认熄屏毫秒。 */
        s_screen_timeout_ms = (uint32_t)s_device_config.screen_timeout_seconds * UINT32_C(1000);
        /* 返回。 */
        return;
    }
    /* 长度、介质或读取错误均不使用部分字节。 */
    if ((read_error != ESP_OK) || (blob_length != sizeof(blob))) {
        /* 输出实际错误和长度。 */
        ESP_LOGW(APP_TAG, "device config NVS read failed=%s len=%u; using defaults",
                 esp_err_to_name(read_error),
                 (unsigned int)blob_length);
        /* 写入默认熄屏毫秒。 */
        s_screen_timeout_ms = (uint32_t)s_device_config.screen_timeout_seconds * UINT32_C(1000);
        /* 返回。 */
        return;
    }
    /* 在局部对象执行魔数、版本、保留位、范围和 CRC32 校验。 */
    device_config_t decoded;
    /* 解码失败不污染默认配置。 */
    const device_config_status_t decode_status = device_config_blob_decode(
        blob,
        blob_length,
        &decoded);
    /* 损坏或未来格式回退默认值。 */
    if (decode_status != DEVICE_CONFIG_OK) {
        /* 记录稳定 codec 状态。 */
        ESP_LOGW(APP_TAG, "device config blob rejected=%d; using defaults", (int)decode_status);
        /* 写入默认熄屏毫秒。 */
        s_screen_timeout_ms = (uint32_t)s_device_config.screen_timeout_seconds * UINT32_C(1000);
        /* 返回。 */
        return;
    }
    /* RawStream 即使旧 blob 曾保存 true，冷启动仍强制关闭。 */
    decoded.raw_stream_enabled = false;
    /* 全部验证通过后一次性提交。 */
    s_device_config = decoded;
    /* 秒转毫秒；最大 600 秒不会溢出 uint32。 */
    s_screen_timeout_ms = (uint32_t)s_device_config.screen_timeout_seconds * UINT32_C(1000);
}

    /* 先初始化显示、触摸、I2C 与功放门控，使后续自检失败仍可显示中文 ERROR 页。 */
static bool app_init_board_runtime(void)
{
    /* 构造外部驱动回调；hub 生命周期为静态。 */
    const board_runtime_config_t config = {
        /* 注入真实传感器回调。 */
        .external_ops = {
            /* 指向静态 hub。 */
            .context = &s_sensors,
            /* 注入电池读取。 */
            .read_battery = app_sensor_read_battery,
            /* 注入 QMI ACTIVE/WOM/OFF 真实寄存器切换。 */
            .set_qmi_mode = app_sensor_set_qmi_mode,
            /* 注入 RTC。 */
            .read_rtc_unix_seconds = app_sensor_read_rtc,
            /* 注入 PMIC 关机。 */
            .request_pmic_shutdown = app_sensor_request_shutdown,
        },
        /* Mock 后端默认也使用厂家 400 mAh 的 80% 电量。 */
        .mock = {
            /* 设 80%。 */
            .battery_percent = 80U,
            /* 默认未充电。 */
            .charging = false,
            /* 模拟有 TF。 */
            .storage_present = true,
            /* 模拟三芯片存在。 */
            .sensor_devices_present = true,
        },
        /* 启动亮度直接使用 NVS/Cmd9 偏好，首次启动默认为 35%。 */
        .initial_brightness_percent = s_device_config.brightness_percent,
    };
    /* 初始化厂家 BSP、显示、触摸、I2C 和功放门控。 */
    if (board_runtime_init(&s_board_runtime, &config) != BOARD_RUNTIME_OK) {
        /* 输出失败。 */
        ESP_LOGE(APP_TAG, "board_runtime_init failed");
        /* 返回失败。 */
        return false;
    }
    /* 清空静态诊断快照，避免复位前残留值被后续传感器初始化误用。 */
    (void)memset(&s_board_diagnostics, 0, sizeof(s_board_diagnostics));
    /* 读取启动阶段固定的显示、I2C、QMI8658、PMIC 与 RTC 探测结果。 */
    if (board_runtime_get_diagnostics(&s_board_runtime, &s_board_diagnostics) != BOARD_RUNTIME_OK) {
        /* 输出诊断读取失败，禁止后续使用未初始化的设备地址。 */
        ESP_LOGE(APP_TAG, "board diagnostics unavailable");
        /* 返回失败，避免向错误 I2C 地址发送配置命令。 */
        return false;
    }
    /* 返回成功；外设芯片是否可用于产品功能由显示 SELF_TEST 页后的独立阶段判断。 */
    return true;
}

/* 在 SELF_TEST 页可见期间初始化 QMI8658、AXP2101 与 PCF85063。 */
static bool app_init_external_sensors(void)
{
    /* QMI8658 必须在 0x6A 或 0x6B 应答；零地址表示板级探测没有发现 IMU。 */
    if (!s_board_diagnostics.qmi_present) {
        /* 输出明确故障，便于实机自检区分总线故障与寄存器配置故障。 */
        ESP_LOGE(APP_TAG, "QMI8658 not found at 0x6A or 0x6B");
        /* 没有 IMU 无法执行动作识别，停止启动。 */
        return false;
    }
    /* 取得 BSP 共用 I2C 总线。 */
    i2c_master_bus_handle_t bus =
        (i2c_master_bus_handle_t)board_runtime_i2c_handle(&s_board_runtime);
    /* 总线必须有效。 */
    if (bus == NULL) {
        /* 输出错误。 */
        ESP_LOGE(APP_TAG, "BSP I2C handle unavailable");
        /* 返回失败。 */
        return false;
    }
    /* 使用板级探测得到的 0x6A/0x6B 地址注册 QMI，同时注册固定地址的 AXP2101 与 PCF85063。 */
    if (board_sensors_esp_idf_i2c_init_with_qmi_address(
            &s_sensors.i2c,
            bus,
            s_board_diagnostics.qmi_i2c_address) != BOARD_SENSORS_OK) {
        /* 输出错误。 */
        ESP_LOGE(APP_TAG, "board sensor I2C adapter init failed");
        /* 返回失败。 */
        return false;
    }
    /* 按真实 SA0 地址初始化 QMI 到 ±8g@125 Hz 和 ±1024dps@112.1 Hz。 */
    if (board_qmi8658_init_with_address(
            &s_sensors.qmi,
            &s_sensors.i2c.port,
            s_board_diagnostics.qmi_i2c_address) != BOARD_SENSORS_OK) {
        /* 输出错误。 */
        ESP_LOGE(APP_TAG, "QMI8658 init failed; detected address=0x%02X",
                 (unsigned int)s_board_diagnostics.qmi_i2c_address);
        /* 返回失败。 */
        return false;
    }
    /* 初始化 AXP2101；安全电量和关机依赖它。 */
    if (board_axp2101_init(&s_sensors.axp, &s_sensors.i2c.port) != BOARD_SENSORS_OK) {
        /* 输出错误。 */
        ESP_LOGE(APP_TAG, "AXP2101 init failed");
        /* 返回失败。 */
        return false;
    }
    /* 初始化 RTC；失败不阻断单调会话，但 UTC 会保持未知。 */
    if (board_pcf85063_init(&s_sensors.rtc, &s_sensors.i2c.port) != BOARD_SENSORS_OK) {
        /* 输出警告。 */
        ESP_LOGW(APP_TAG, "PCF85063 init failed; session UTC remains unknown");
    }
    /* 默认关闭 QMI 数据轮询，开始会话后由电源策略开启。 */
    s_sensors.qmi_sampling_enabled = false;
    /* 标记外部回调可用。 */
    s_sensors.initialized = true;
    /* 返回成功。 */
    return true;
}

/* 挂载 LittleFS 并恢复最近 200 条会话。 */
static bool app_init_session_store(void)
{
    /* 配置内部 Flash LittleFS；首次空白分区允许格式化。 */
    const esp_vfs_littlefs_conf_t config = {
        /* 挂载路径。 */
        .base_path = APP_LITTLEFS_BASE_PATH,
        /* 对应 partitions.csv 的 littlefs 标签。 */
        .partition_label = "littlefs",
        /* 首次空白或损坏时格式化；恢复前仍由双槽 CRC 拒绝半写记录。 */
        .format_if_mount_failed = true,
        /* 挂载 VFS。 */
        .dont_mount = false,
    };
    /* 注册 VFS。 */
    const esp_err_t mount_error = esp_vfs_littlefs_register(&config);
    /* 挂载失败不能提供安全摘要。 */
    if (mount_error != ESP_OK) {
        /* 输出错误。 */
        ESP_LOGE(APP_TAG, "LittleFS mount failed=%s", esp_err_to_name(mount_error));
        /* 返回失败。 */
        return false;
    }
    /* 打开固定双槽文件并扩容为 0xFF。 */
    if (session_file_backend_open(
            &s_session_file_backend,
            APP_SESSION_FILE_PATH,
            session_store_required_backend_size(),
            true) != SESSION_STORE_STATUS_OK) {
        /* 输出错误。 */
        ESP_LOGE(APP_TAG, "session file open failed path=%s", APP_SESSION_FILE_PATH);
        /* 返回失败。 */
        return false;
    }
    /* 生成后端接口副本。 */
    const session_store_backend_t backend =
        session_file_backend_interface(&s_session_file_backend);
    /* 恢复两个槽中最新有效快照。 */
    if (session_store_init(&s_session_store, &backend) != SESSION_STORE_STATUS_OK) {
        /* 输出错误。 */
        ESP_LOGE(APP_TAG, "session store recovery failed");
        /* 返回失败。 */
        return false;
    }
    /* 绑定会话补传。 */
    if (!session_transfer_service_init(&s_session_transfer, &s_session_store)) {
        /* 输出错误。 */
        ESP_LOGE(APP_TAG, "session transfer init failed");
        /* 返回失败。 */
        return false;
    }
    /* 返回成功。 */
    return true;
}

/* 从最近摘要生成重启后下一会话序号。 */
static uint32_t app_next_session_sequence(void)
{
    /* 空仓储从 1 开始。 */
    if (session_store_count(&s_session_store) == 0U) {
        /* 返回 1。 */
        return 1U;
    }
    /* 读取最新摘要。 */
    session_summary_t latest;
    /* 读取失败回退 1，并由日志提示。 */
    if (session_store_get_recent(&s_session_store, 0U, &latest) != SESSION_STORE_STATUS_OK) {
        /* 输出警告。 */
        ESP_LOGW(APP_TAG, "latest session read failed; sequence resets to 1");
        /* 返回 1。 */
        return 1U;
    }
    /* uint32 最大值后跳过零。 */
    const uint32_t next = latest.session_seq + 1U;
    /* 零哨兵改为 1。 */
    return next == 0U ? 1U : next;
}

/*
 * 在长期 UI 任务启动前同步显示开机页与自检页。
 * 该函数只操作局部纯 UI 上下文，不修改已经初始化到 HOME 的业务协调器；
 * renderer 在每次调用内获取 LVGL 锁，因此启动任务无需直接接触任何 lv_ 对象。
 */
static bool app_show_startup_pages(ui_context_t *startup_ui)
{
    /* 调用方必须提供持续到自检结束的上下文，失败路径还要用它渲染 ERROR 页。 */
    if (startup_ui == NULL) {
        /* 空上下文无法维护 BOOT 到 SELF_TEST 的合法状态转移。 */
        return false;
    }
    /* 使用当前单调毫秒初始化到 BOOT，避免 RTC 校时影响动画。 */
    ui_context_init(startup_ui, (uint32_t)app_now_ms());
    /* 首帧立即显示产品 Logo 与本地 AI 提示。 */
    if (ui_lvgl_renderer_render(&s_ui_renderer, startup_ui) != UI_LVGL_OK) {
        /* 渲染失败表示屏幕对象树不可用，禁止伪装启动成功。 */
        ESP_LOGE(APP_TAG, "boot page render failed");
        /* 返回失败。 */
        return false;
    }
    /* 读取集中动画配置；均衡模式的开机显示时长固定为 800 ms。 */
    const ui_animation_profile_t animation = ui_default_animation_profile();
    /* app_main 本身是 FreeRTOS 任务，阻塞期间会让出 CPU，不是忙等待。 */
    vTaskDelay(pdMS_TO_TICKS(animation.boot_ms));
    /* 构造 BOOT 到 SELF_TEST 的纯状态事件。 */
    const ui_event_t boot_ready = {
        /* 标记板级初始化已完成，可以展示自检结果。 */
        .type = UI_EVENT_BOOT_READY,
        /* 记录切页时单调毫秒。 */
        .monotonic_ms = (uint32_t)app_now_ms(),
    };
    /* 状态机必须从 BOOT 准确进入 SELF_TEST。 */
    if (ui_dispatch_event(startup_ui, &boot_ready) != UI_DISPATCH_OK) {
        /* 理论错误需阻止进入无法解释的主页。 */
        ESP_LOGE(APP_TAG, "boot page transition failed");
        /* 返回失败。 */
        return false;
    }
    /* 显示硬件、触摸、IMU 与存储均已完成检查的自检页。 */
    if (ui_lvgl_renderer_render(&s_ui_renderer, startup_ui) != UI_LVGL_OK) {
        /* 自检页渲染失败同样视为 UI 启动失败。 */
        ESP_LOGE(APP_TAG, "self-test page render failed");
        /* 返回失败。 */
        return false;
    }
    /* 返回成功；真实外设和存储检查接下来在 SELF_TEST 页可见期间执行。 */
    return true;
}

/*
 * 显示质量隔离模式只绘制一次 HOME 页面。
 * 状态机仍依次接收 BOOT_READY 和 SELF_TEST_OK，避免直接改写 state 破坏 presenter 合同；
 * 两个中间状态不调用渲染器，因此面板上不存在可与 HOME 同坐标叠加的旧文字帧。
 */
static bool app_show_static_home_once(ui_context_t *static_ui)
{
    /* 调用方必须提供在永久显示期间持续有效的局部 UI 上下文。 */
    if (static_ui == NULL) {
        /* 空指针无法承载合法状态转移，返回失败且不接触 LVGL。 */
        return false;
    }
    /* 从 BOOT 初始化纯状态对象；此处不渲染 BOOT，避免向 AMOLED 写入第一组文字。 */
    ui_context_init(static_ui, (uint32_t)app_now_ms());
    /* 构造 BOOT 到 SELF_TEST 的合法状态事件。 */
    const ui_event_t boot_ready = {
        /* 标记板级显示初始化已完成。 */
        .type = UI_EVENT_BOOT_READY,
        /* 记录单调毫秒；该值只服务状态机，不参与任何页面动画。 */
        .monotonic_ms = (uint32_t)app_now_ms(),
    };
    /* 只推进状态，不调用 ui_lvgl_renderer_render，因此 SELF_TEST 文字不会写入面板。 */
    if (ui_dispatch_event(static_ui, &boot_ready) != UI_DISPATCH_OK) {
        /* 状态机拒绝表示启动合同损坏，不能用直接写 state 的方式绕过。 */
        ESP_LOGE(APP_TAG, "static HOME BOOT_READY transition failed");
        /* 返回失败。 */
        return false;
    }
    /* 构造 SELF_TEST 到 HOME 的合法状态事件。 */
    const ui_event_t self_test_ok = {
        /* 表示本显示隔离固件允许进入 HOME；传感器与存储未在此模式初始化。 */
        .type = UI_EVENT_SELF_TEST_OK,
        /* 记录第二次状态转移的单调毫秒。 */
        .monotonic_ms = (uint32_t)app_now_ms(),
    };
    /* 再次只推进状态，使 presenter 最终读取标准 HOME 视图。 */
    if (ui_dispatch_event(static_ui, &self_test_ok) != UI_DISPATCH_OK) {
        /* 拒绝转移时记录唯一故障点。 */
        ESP_LOGE(APP_TAG, "static HOME SELF_TEST_OK transition failed");
        /* 返回失败。 */
        return false;
    }
    /* 全流程唯一一次渲染直接绘制 HOME；静态渲染器会立即加载、整页失效并同步刷新。 */
    if (ui_lvgl_renderer_render(&s_ui_renderer, static_ui) != UI_LVGL_OK) {
        /* HOME 仍无法绘制时把故障限定到页面树、锁或显示刷新链。 */
        ESP_LOGE(APP_TAG, "static HOME single render failed");
        /* 返回失败。 */
        return false;
    }
    /* 返回成功；调用方随后永久等待，页面不再切换或重绘。 */
    return true;
}

/* 把已可见的 SELF_TEST 页切到中文 ERROR 页并保留稳定故障码。 */
static void app_show_startup_error(ui_context_t *startup_ui, const int32_t fault_code)
{
    /* 空上下文表示连最小 UI 状态都不可用，只能依靠串口日志。 */
    if (startup_ui == NULL) {
        /* 结束安全无操作路径。 */
        return;
    }
    /* 在派发失败事件前写入稳定码；状态机合同明确从 context 读取该字段。 */
    startup_ui->fault_code = fault_code;
    /* 构造只含单调时间的自检失败事件。 */
    const ui_event_t failed = {
        /* 指明关键自检失败。 */
        .type = UI_EVENT_SELF_TEST_FAILED,
        /* 使用单调毫秒，RTC 故障不会影响页面状态。 */
        .monotonic_ms = (uint32_t)app_now_ms(),
    };
    /* 只有 SELF_TEST 到 ERROR 的合法转移才允许渲染故障页。 */
    if (ui_dispatch_event(startup_ui, &failed) != UI_DISPATCH_OK) {
        /* 输出状态机错误；此时保留当前 SELF_TEST 页面。 */
        ESP_LOGE(APP_TAG, "startup ERROR transition failed code=%ld", (long)fault_code);
        /* 结束。 */
        return;
    }
    /* 渲染器内部获取 BSP LVGL 锁；失败时串口仍保留同一稳定故障码。 */
    if (ui_lvgl_renderer_render(&s_ui_renderer, startup_ui) != UI_LVGL_OK) {
        /* 记录渲染失败。 */
        ESP_LOGE(APP_TAG, "startup ERROR render failed code=%ld", (long)fault_code);
    }
}

/* 完成 SELF_TEST 到 HOME 的合法转移；真实协调器 HOME 随后覆盖局部启动上下文。 */
static bool app_finish_startup_success(ui_context_t *startup_ui)
{
    /* 上下文必须仍停留在本次启动的 SELF_TEST 页面。 */
    if (startup_ui == NULL) {
        /* 返回失败。 */
        return false;
    }
    /* 自检页至少再保留 350 ms，避免快速主机或真机上一闪而过。 */
    vTaskDelay(pdMS_TO_TICKS(APP_SELF_TEST_HOLD_MS));
    /* 构造成功事件。 */
    const ui_event_t passed = {
        /* 指明全部关键软件与板级合同已建立。 */
        .type = UI_EVENT_SELF_TEST_OK,
        /* 使用单调时间。 */
        .monotonic_ms = (uint32_t)app_now_ms(),
    };
    /* 必须从 SELF_TEST 精确进入 HOME。 */
    if (ui_dispatch_event(startup_ui, &passed) != UI_DISPATCH_OK) {
        /* 记录错误。 */
        ESP_LOGE(APP_TAG, "startup HOME transition failed");
        /* 返回失败。 */
        return false;
    }
    /* 返回成功；长期 UI 首帧使用协调器的权威 HOME 快照。 */
    return true;
}

/* 在板级显示可用后尽早建立 LVGL 页面树，保证后续外设失败可见。 */
static bool app_init_ui_renderer(void)
{
    /* 构造 LVGL 锁端口。 */
    const ui_lvgl_port_t ui_port = {
        /* 锁上下文为 board_runtime。 */
        .context = &s_board_runtime,
        /* 使用 BSP 锁。 */
        .lock = board_runtime_lvgl_lock,
        /* 使用 BSP 解锁。 */
        .unlock = board_runtime_lvgl_unlock,
    };
    /* 创建全部中文页面和按钮。 */
    if (ui_lvgl_renderer_init(
            &s_ui_renderer,
            &ui_port,
            app_ui_command_callback,
            NULL) != UI_LVGL_OK) {
        /* 页面树不可用时无法提供触屏产品。 */
        ESP_LOGE(APP_TAG, "LVGL renderer init failed");
        /* 返回失败。 */
        return false;
    }
    /* 返回成功。 */
    return true;
}

/* 初始化协调器和 IMU 流水线；LVGL renderer 已在外设自检前建立。 */
static bool app_init_domains(void)
{
    /* 取得启动探测后的板级配置，协调器不应再硬编码 Deep-sleep 能力。 */
    board_adapter_t *const adapter = board_runtime_adapter(&s_board_runtime);
    /* 板级运行时未就绪时拒绝初始化业务域。 */
    if (adapter == NULL) {
        /* 记录运行时状态错误。 */
        ESP_LOGE(APP_TAG, "board adapter unavailable before domain initialization");
        /* 返回失败。 */
        return false;
    }
    /* 构造协调器配置。 */
    const device_coordinator_config_t coordinator_config = {
        /* 使用持久化后下一序号。 */
        .next_session_seq = app_next_session_sequence(),
        /* 使用 NVS 恢复的下次会话体重，单位 g。 */
        .weight_g = s_device_config.weight_g,
        /* 使用 NVS 恢复的目标种类。 */
        .goal_kind = s_device_config.goal_kind,
        /* 使用 NVS 恢复的次数、秒或 mcal 目标值。 */
        .goal_value = s_device_config.goal_value,
        /* 使用编译配置生成的板型能力；真机仍须校准 40 mg/4 样本 WOM 参数。 */
        .allow_imu_deep_wake = adapter->profile.enable_imu_deep_wake,
    };
    /* 初始化到 Home。 */
    if (device_coordinator_init(&s_coordinator, &coordinator_config, app_now_ms()) !=
        DEVICE_COORDINATOR_OK) {
        /* 输出错误。 */
        ESP_LOGE(APP_TAG, "device coordinator init failed");
        /* 返回失败。 */
        return false;
    }
    /* 用 NVS 恢复值覆盖纯 UI 冻结默认值，设置页首次打开即显示真实配置。 */
    app_project_device_config_to_ui(&s_coordinator.ui);
    /* 构造严格重采样与双 M0 配置。 */
    const imu_pipeline_config_t pipeline_config = {
        /* ±8 g 每 LSB。 */
        .accel_g_per_lsb = BOARD_QMI8658_ACCEL_G_PER_LSB,
        /* ±1024 deg/s 每 LSB。 */
        .gyro_dps_per_lsb = BOARD_QMI8658_GYRO_DPS_PER_LSB,
        /* 生产双 M0 使用短时最高频率锁，推理结束立即允许动态降频。 */
        .infer = app_pm_locked_dual_m0_infer,
        /* 长期诊断对象接收基础、掩码和融合 Top-1；由同一应用任务串行读写。 */
        .inference_context = &s_dual_m0_diagnostics,
        /* 25 Hz 点进入计数/热量。 */
        .on_sample = app_pipeline_on_sample,
        /* 不保存原始窗口。 */
        .on_window = NULL,
        /* 推理结果进入动作锁定。 */
        .on_inference = app_pipeline_on_inference,
        /* 回调无需上下文。 */
        .callback_context = NULL,
    };
    /* 初始化流水线。 */
    if (imu_pipeline_init(&s_imu_pipeline, &pipeline_config) != IMU_PIPELINE_OK) {
        /* 输出错误。 */
        ESP_LOGE(APP_TAG, "IMU pipeline init failed");
        /* 返回失败。 */
        return false;
    }
    /* 返回成功。 */
    return true;
}

/* 创建唯一任务集合。 */
static bool app_create_tasks(void)
{
    /* 应用任务 16 KiB 栈，覆盖约 10.9 KiB 最深静态链并保留约 5 KiB 运行余量。 */
    const BaseType_t app_created = xTaskCreatePinnedToCore(
        app_event_task,
        "app_owner",
        APP_OWNER_TASK_STACK_BYTES,
        NULL,
        9U,
        NULL,
        1);
    /* QMI 读取任务固定 core0。 */
    const BaseType_t qmi_created = xTaskCreatePinnedToCore(
        app_qmi_task,
        "qmi_reader",
        4U * 1024U,
        NULL,
        10U,
        NULL,
        0);
    /*
     * UI 任务只消费按值快照并在 BSP LVGL 锁内更新对象，不执行 Flash、NVS 或 ISR 路径。
     * 其 8 KiB 栈使用板载 8 MB PSRAM，避免产品级页面对象与 NimBLE 争抢片内任务栈。
     */
    const BaseType_t ui_created = xTaskCreateWithCaps(
        app_ui_task,
        "ui_render",
        APP_UI_TASK_STACK_BYTES,
        NULL,
        5U,
        NULL,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* BLE 发布任务。 */
    const BaseType_t ble_created = xTaskCreate(
        app_ble_task,
        "ble_publish",
        6U * 1024U,
        NULL,
        6U,
        NULL);
    /* 存储任务。 */
    const BaseType_t storage_created = xTaskCreate(
        app_storage_task,
        "session_store",
        6U * 1024U,
        NULL,
        4U,
        NULL);
    /* 电池任务。 */
    const BaseType_t battery_created = xTaskCreate(
        app_battery_task,
        "battery",
        3U * 1024U,
        NULL,
        3U,
        NULL);
    /* 电源任务。 */
    const BaseType_t power_created = xTaskCreate(
        app_power_task,
        "power",
        4U * 1024U,
        NULL,
        4U,
        NULL);
    /* 空闲计时任务只投递每秒事件，2 KiB 栈足够 FreeRTOS 队列操作。 */
    const BaseType_t idle_created = xTaskCreate(
        app_idle_task,
        "idle_timer",
        2U * 1024U,
        NULL,
        2U,
        NULL);
    /* 读取任务创建后的片内总余量，验证科技 UI、BLE 与八条业务链可同时常驻。 */
    const size_t internal_free = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    /* 读取最大连续片内块，后续协议和驱动临时对象不能只依赖离散总量。 */
    const size_t internal_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    /* 输出每个任务结果和剩余片内堆，真板启动日志可直接定位具体失败项。 */
    ESP_LOGI(
        APP_TAG,
        "TASK_HEAP free=%u largest=%u result=%d%d%d%d%d%d%d%d",
        (unsigned int)internal_free,
        (unsigned int)internal_largest,
        app_created == pdPASS ? 1 : 0,
        qmi_created == pdPASS ? 1 : 0,
        ui_created == pdPASS ? 1 : 0,
        ble_created == pdPASS ? 1 : 0,
        storage_created == pdPASS ? 1 : 0,
        battery_created == pdPASS ? 1 : 0,
        power_created == pdPASS ? 1 : 0,
        idle_created == pdPASS ? 1 : 0);
    /* 全部八个真实硬件/业务任务必须创建成功，任一失败都由启动错误页阻断产品运行。 */
    return (app_created == pdPASS) && (qmi_created == pdPASS) &&
           (ui_created == pdPASS) && (ble_created == pdPASS) &&
           (storage_created == pdPASS) &&
           (battery_created == pdPASS) && (power_created == pdPASS) &&
           (idle_created == pdPASS);
}

/* 启动 NimBLE 服务。 */
static bool app_start_ble(const uint8_t initial_battery_percent)
{
    /* 读取蓝牙 MAC。 */
    uint8_t mac[6] = {0U};
    /* 读取失败时仍使用固定名称。 */
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        /* 清零保留默认后缀。 */
        (void)memset(mac, 0, sizeof(mac));
    }
    /* 设备名使用 MAC 后两字节避免多设备混淆。 */
    (void)snprintf(
        s_ble_device_name,
        sizeof(s_ble_device_name),
        "BPNN-FIT-%02X%02X",
        (unsigned int)mac[4],
        (unsigned int)mac[5]);
    /* 序列号使用完整 48 位 MAC。 */
    (void)snprintf(
        s_ble_serial_number,
        sizeof(s_ble_serial_number),
        "%02X%02X%02X%02X%02X%02X",
        (unsigned int)mac[0],
        (unsigned int)mac[1],
        (unsigned int)mac[2],
        (unsigned int)mac[3],
        (unsigned int)mac[4],
        (unsigned int)mac[5]);
    /* 查询 LittleFS 总容量；单位 byte，值仅表示 Manifest 构建时快照。 */
    size_t littlefs_total_bytes = 0U;
    /* 查询 LittleFS 已使用容量；单位 byte。 */
    size_t littlefs_used_bytes = 0U;
    /* 读取已挂载 littlefs 分区容量，失败时不能发布字段不完整的正式 Manifest。 */
    const esp_err_t littlefs_info_error = esp_littlefs_info(
        "littlefs",
        &littlefs_total_bytes,
        &littlefs_used_bytes);
    /* 容量查询失败说明 Manifest 的必填存储字段不可用。 */
    if (littlefs_info_error != ESP_OK) {
        /* 记录精确 ESP-IDF 错误并让设备保持 BLE 离线模式。 */
        ESP_LOGE(APP_TAG, "LittleFS info for BLE Manifest failed=%s",
                 esp_err_to_name(littlefs_info_error));
        /* 返回失败，禁止重新退回空 Manifest。 */
        return false;
    }
    /* 已用容量异常大于总容量时钳制为零并记录，避免 size_t 无符号下溢。 */
    const uint64_t littlefs_available_bytes =
        littlefs_used_bytes <= littlefs_total_bytes
            ? (uint64_t)(littlefs_total_bytes - littlefs_used_bytes)
            : UINT64_C(0);
    /* 异常容量关系只影响提示值，不破坏已经成功恢复的会话仓储。 */
    if (littlefs_used_bytes > littlefs_total_bytes) {
        /* 输出异常原始值，供现场诊断 LittleFS 实现或分区表。 */
        ESP_LOGW(APP_TAG, "LittleFS used exceeds total used=%u total=%u",
                 (unsigned int)littlefs_used_bytes,
                 (unsigned int)littlefs_total_bytes);
    }
    /* 能力位只声明主应用已接入的历史补传、LittleFS 与显式开发者原始流。 */
    const uint32_t manifest_capabilities =
        (uint32_t)BLE_SERVICE_MANIFEST_CAPABILITY_SESSION_HISTORY |
        (uint32_t)BLE_SERVICE_MANIFEST_CAPABILITY_LITTLEFS_STORAGE |
        (uint32_t)BLE_SERVICE_MANIFEST_CAPABILITY_RAW_STREAM;
    /* 构造全部正式 Manifest 字段；SHA 和类别顺序直接来自自动生成模型头。 */
    const ble_service_manifest_config_t manifest_config = {
        /* 完整 48 位蓝牙 MAC 作为动态设备 ID，和标准序列号保持一致。 */
        .device_id = s_ble_serial_number,
        /* 板卡修订使用当前厂家 2.06 板型。 */
        .board_revision = s_ble_hardware_revision,
        /* 协议主版本直接复用共享逻辑帧常量。 */
        .protocol_major = IMU_BLE_PROTOCOL_MAJOR,
        /* 协议次版本直接复用共享逻辑帧常量。 */
        .protocol_minor = IMU_BLE_PROTOCOL_MINOR,
        /* 固件版本与标准 0x2A26 特征一致。 */
        .firmware_version = s_ble_firmware_revision,
        /* 特征维度直接使用生成头 FEATURE_DIM，当前为 297。 */
        .feature_dimension = (uint16_t)FEATURE_DIM,
        /* 特征公式与顺序版本使用 Manifest v1 固定常量。 */
        .feature_version = BLE_SERVICE_MANIFEST_FEATURE_VERSION,
        /* 基础模型摘要直接引用生成头，禁止人工抄写造成漂移。 */
        .base_model_sha256_hex = BP_BASE_MODEL_SHA256,
        /* 掩码模型摘要直接引用生成头，禁止人工抄写造成漂移。 */
        .masked_model_sha256_hex = BP_MASKED_MODEL_SHA256,
        /* 类别数量直接使用生成头 CLASS_NUM，当前为 11。 */
        .class_count = (uint8_t)CLASS_NUM,
        /* 类别名按 logits 索引直接使用生成头 BP_CLASS_NAMES。 */
        .class_names = BP_CLASS_NAMES,
        /* 卡路里固定 milliMET 表当前为版本 1。 */
        .calorie_table_version = BLE_SERVICE_MANIFEST_CALORIE_TABLE_VERSION,
        /* 只设置已经完整接入的四项能力。 */
        .capabilities = manifest_capabilities,
        /* 写入启动时 LittleFS 可用容量快照，单位 byte。 */
        .littlefs_available_bytes = littlefs_available_bytes,
    };
    /* 保存 Manifest 构建后的实际字节数。 */
    size_t manifest_length = 0U;
    /* 先构建并校验全部 TLV，再交给 NimBLE 深拷贝。 */
    const ble_service_status_t manifest_status = ble_service_manifest_build(
        &manifest_config,
        s_ble_manifest,
        sizeof(s_ble_manifest),
        &manifest_length);
    /* 任何字段、SHA、类表或容量错误都禁止 BLE 退回空 Manifest。 */
    if ((manifest_status != BLE_SERVICE_STATUS_OK) ||
        (manifest_length == 0U) ||
        (manifest_length > UINT16_MAX)) {
        /* 输出 BLE 统一状态和长度，便于定位生成头或字段错误。 */
        ESP_LOGE(APP_TAG, "BLE Manifest build failed status=%d length=%u",
                 (int)manifest_status,
                 (unsigned int)manifest_length);
        /* 返回失败，设备仍可离线训练。 */
        return false;
    }
    /* 配置 GATT。 */
    const ble_service_nimble_config_t config = {
        /* 广播名。 */
        .device_name = s_ble_device_name,
        /* 厂商名。 */
        .manufacturer_name = "Waveshare+BPNN",
        /* 型号。 */
        .model_number = s_ble_model_number,
        /* 唯一序列号。 */
        .serial_number = s_ble_serial_number,
        /* 硬件修订来自当前板型，实物丝印后再细化。 */
        .hardware_revision = s_ble_hardware_revision,
        /* 固件语义版本。 */
        .firmware_revision = s_ble_firmware_revision,
        /* 发布已完整校验的正式 Manifest TLV；start 返回前复制到 NimBLE 静态状态。 */
        .manifest_payload = s_ble_manifest,
        /* builder 已保证长度不超过 512 和 uint16_t 上限。 */
        .manifest_length = (uint16_t)manifest_length,
        /* 使用启动 PMIC 电量。 */
        .initial_battery_percent = initial_battery_percent,
        /* 命令经同步 broker 进入应用任务。 */
        .command_handler = app_ble_command_handler,
        /* 无额外上下文。 */
        .command_context = NULL,
        /* 会话补传包装器。 */
        .transfer_handler = app_transfer_handler,
        /* 指向静态服务。 */
        .transfer_context = &s_session_transfer,
        /* 配对码显示。 */
        .passkey_display = app_ble_passkey_display,
        /* 配对成功、失败、断线、忘记或停服时清除屏幕敏感码。 */
        .passkey_clear = app_ble_passkey_clear,
        /* 两个回调共享启动前初始化的静态无锁邮箱。 */
        .passkey_context = &s_pairing_mailbox,
        /* 连接状态回调。 */
        .connection_changed = app_ble_connection_changed,
        /* 无额外上下文。 */
        .connection_context = NULL,
    };
    /* 启动服务与广播。 */
    const esp_err_t error = ble_service_nimble_start(&config);
    /* 失败时输出。 */
    if (error != ESP_OK) {
        /* 记录错误。 */
        ESP_LOGE(APP_TAG, "NimBLE start failed=%s", esp_err_to_name(error));
        /* 返回失败。 */
        return false;
    }
    /* 返回成功。 */
    return true;
}

/* 等待异步 NimBLE 主机完成同步并进入可连接广播，防止业务任务栈与主机内存池并发抢占片内 RAM。 */
static bool app_wait_ble_ready(void)
{
    /* 使用单调毫秒时钟计算固定截止点，系统校时不会改变等待长度。 */
    const uint64_t deadline_ms = app_now_ms() + APP_BLE_READY_TIMEOUT_MS;
    /* 保存最后一次生命周期快照，超时时输出精确启动阶段。 */
    ble_service_nimble_runtime_status_t runtime_status;
    /* 清零全部布尔位和射频枚举，避免首次快照读取失败时输出未初始化值。 */
    (void)memset(&runtime_status, 0, sizeof(runtime_status));
    /* 截止前周期让出 CPU，NimBLE 主机任务可完成控制器事件、GATT 注册和广播配置。 */
    while (app_now_ms() < deadline_ms) {
        /* 读取只含按值字段的生命周期快照；函数不返回或保存内部指针。 */
        const esp_err_t snapshot_error = ble_service_nimble_get_runtime_status(&runtime_status);
        /* 服务、主机同步和广播活动三项同时成立才允许九个业务任务开始分配栈。 */
        if ((snapshot_error == ESP_OK) &&
            runtime_status.started &&
            runtime_status.host_synced &&
            runtime_status.advertising) {
            /* 输出可由真机脚本解析的成功状态，不包含地址、密钥或配对信息。 */
            ESP_LOGI(
                APP_TAG,
                "BLE_READY started=1 synced=1 advertising=1");
            /* 返回成功，调用方现在可以创建业务任务。 */
            return true;
        }
        /* 休眠 20 ms 让出调度器；不会占用 NimBLE 主机所在 core0。 */
        vTaskDelay(pdMS_TO_TICKS(APP_BLE_READY_POLL_MS));
    }
    /* 超时输出最后事实，区分控制器失败、主机未同步和广播配置失败。 */
    ESP_LOGE(
        APP_TAG,
        "BLE_READY timeout started=%d synced=%d advertising=%d",
        runtime_status.started ? 1 : 0,
        runtime_status.host_synced ? 1 : 0,
        runtime_status.advertising ? 1 : 0);
    /* 返回失败；调用方必须停止半启动服务，释放控制器资源后稳定离线运行。 */
    return false;
}

/* ESP-IDF 应用入口。 */
void app_main(void)
{
    /* 读取本次启动原因；枚举由 ESP-IDF 定义，后续日志可区分掉电、软件复位、看门狗和 Deep-sleep。 */
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    /* 明确记录当前固件为现场常亮版本，串口可据此区分正式低功耗版本。 */
    ESP_LOGW(
        APP_TAG,
        "bench functional mode: reset_reason=%d screen_off=0 sleep=0 ble=1 initial_power_policy=1",
        (int)reset_reason);
    /* 分级隔离分支严格早于 NVS：第一阶段仅显示，第二阶段只增加三个 I2C 外设驱动。 */
    if (APP_BENCH_DISPLAY_ONLY || APP_BENCH_SENSOR_ONLY) {
        /* 不读取 NVS；先恢复冻结默认值，显示-only 会在 BSP 初始化前覆盖为厂家同档 100%。 */
        device_config_set_defaults(&s_device_config);
        /* 仅显示复测使用 100% 亮度，排除 35% AMOLED 调光把低灰阶文字边缘表现为抖动。 */
        if (APP_BENCH_DISPLAY_ONLY) {
            /* 亮度单位为百分比；100 是厂家 BSP 冷启动默认值，不改变产品 NVS 默认配置。 */
            s_device_config.brightness_percent = UINT8_C(100);
        }
        /* 初始化厂家 BSP 2.0.0 的显示、触摸和共用 I2C；第二阶段在页面可见后才配置独立外设。 */
        if (!app_init_board_runtime()) {
            /* 最早显示初始化失败只能写串口，不能继续创建 LVGL 对象。 */
            ESP_LOGE(APP_TAG, "bench staged board runtime failed");
            /* 结束 app_main；BSP 失败事实由复位日志定位。 */
            return;
        }
        /* 创建真实中文页面树和 Noto Sans SC 字体对象，直接覆盖字体、布局和 SH8601 刷新链。 */
        if (!app_init_ui_renderer()) {
            /* 页面树失败表示问题位于自定义 LVGL/UI 层。 */
            ESP_LOGE(APP_TAG, "bench staged UI renderer failed");
            /* 结束当前入口，禁止进入任何非显示初始化。 */
            return;
        }
        /* 显示-only 在首个 BOOT 帧前关闭全部 LVGL 动画，避免旧页面与新页面中文字形交叉淡入。 */
        if (APP_BENCH_DISPLAY_ONLY &&
            (ui_lvgl_renderer_set_animations_enabled(&s_ui_renderer, false) != UI_LVGL_OK)) {
            /* 动画开关失败表示 LVGL 锁或渲染器状态异常，不能继续产生不可解释的动态刷新。 */
            ESP_LOGE(APP_TAG, "display-only failed to disable LVGL animations");
            /* 保留串口错误并结束入口，不进入传感器或产品链。 */
            return;
        }
        /* 保存隔离阶段局部页面状态；该对象在永久等待期间始终有效。 */
        ui_context_t bench_ui;
        /* 显示-only 只绘制一次 HOME；传感器-only 保留三页启动链供下一阶段观察自检结果。 */
        if (APP_BENCH_DISPLAY_ONLY) {
            /* 单页 helper 通过合法事件推进状态，但只向面板提交一张 HOME。 */
            if (!app_show_static_home_once(&bench_ui)) {
                /* 单次 HOME 失败时记录显示链问题。 */
                ESP_LOGE(APP_TAG, "display-only static HOME failed");
                /* 不进入普通产品启动链。 */
                return;
            }
        } else if (!app_show_startup_pages(&bench_ui)) {
            /* 传感器-only 的 BOOT 或 SELF_TEST 渲染失败时记录阶段错误。 */
            ESP_LOGE(APP_TAG, "sensor-only startup pages failed");
            /* 不进入外设初始化。 */
            return;
        }
        /* 传感器阶段默认尚未失败；显示-only 不进入下面的 I2C 独立驱动路径。 */
        bool bench_sensor_ready = true;
        /* 第二阶段只增加 QMI8658、AXP2101 与 PCF85063，不创建其它产品对象。 */
        if (APP_BENCH_SENSOR_ONLY) {
            /* 创建 QMI 专用互斥量；维持驱动访问合同，但不创建完整产品队列或其它锁。 */
            s_sensors.qmi_mutex = xSemaphoreCreateMutex();
            /* 互斥量创建失败表示内部 RAM 不足，不能安全进入独立 QMI 驱动。 */
            if (s_sensors.qmi_mutex == NULL) {
                /* 记录明确阶段错误，避免把内存失败误判为 I2C 或 PMIC 故障。 */
                ESP_LOGE(APP_TAG, "sensor-only QMI mutex allocation failed");
                /* 标记失败；下方显示固定传感器错误页并永久停留。 */
                bench_sensor_ready = false;
            } else if (!app_init_external_sensors()) {
                /* 任一 QMI/AXP 关键初始化失败都保持自检错误页；RTC 失败只由该函数记录警告。 */
                ESP_LOGE(APP_TAG, "sensor-only external sensor initialization failed");
                /* 标记失败，禁止切到 HOME 或进入后续产品链。 */
                bench_sensor_ready = false;
            }
            /* 失败时显示稳定中文故障码 1001，方便肉眼确认故障已发生在传感器阶段。 */
            if (!bench_sensor_ready) {
                /* SELF_TEST 合法切到 ERROR；该函数内部持有 LVGL 锁并同步刷新。 */
                app_show_startup_error(&bench_ui, APP_STARTUP_FAULT_SENSOR);
                /* 输出唯一失败标记，区分硬掉电、初始化返回失败和后续软件路径。 */
                ESP_LOGE(APP_TAG, "SENSOR_ONLY_FAULT_POINT ui=ERROR sensors=0 storage=0 model=0 tasks=0");
            }
        }
        /* 只有传感器-only 仍停在 SELF_TEST；显示-only 已由单页 helper 进入 HOME。 */
        if (APP_BENCH_SENSOR_ONLY && bench_sensor_ready && !app_finish_startup_success(&bench_ui)) {
            /* 状态机拒绝意味着页面合同本身不一致。 */
            ESP_LOGE(APP_TAG, "bench staged HOME transition failed");
            /* 不继续。 */
            return;
        }
        /* 传感器-only 成功时渲染 HOME；显示-only 禁止第二次渲染，失败时 ERROR 已单独绘制。 */
        if (APP_BENCH_SENSOR_ONLY && bench_sensor_ready &&
            (ui_lvgl_renderer_render(&s_ui_renderer, &bench_ui) != UI_LVGL_OK)) {
            /* HOME 刷新失败表明自定义页面或显示缓冲仍有问题。 */
            ESP_LOGE(APP_TAG, "bench staged HOME render failed");
            /* 结束入口。 */
            return;
        }
        /* 第二阶段成功时输出唯一标记，证明三个外设已初始化且未进入存储、模型和产品任务。 */
        if (APP_BENCH_SENSOR_ONLY && bench_sensor_ready) {
            /* 串口监视器以该文本判断 QMI、AXP 与 RTC 初始化调用均已返回。 */
            ESP_LOGW(APP_TAG, "SENSOR_ONLY_STABLE_POINT ui=HOME sensors=1 storage=0 model=0 tasks=0");
        } else if (APP_BENCH_DISPLAY_ONLY) {
            /* 保留第一阶段标记，未来切回显示-only 时无需改动日志解析工具。 */
            ESP_LOGW(APP_TAG, "DISPLAY_ONLY_STABLE_POINT ui=HOME sensors=0 storage=0 model=0 tasks=0");
        }
        /* 永久让出 CPU；LVGL 使用 BSP 自建任务运行，本循环不调用休眠或电源管理。 */
        while (true) {
            /* 每秒阻塞等待，既保持 app_main 局部 UI 上下文生命周期，又不产生忙循环。 */
            vTaskDelay(pdMS_TO_TICKS(1000U));
        }
    }
    /* 初始化 NVS；失败阻断绑定和安全配置。 */
    ESP_ERROR_CHECK(app_init_nvs());
    /* 恢复 CRC32 设备配置；缺失或损坏时使用冻结默认值继续安全启动。 */
    app_load_device_config();
    /* 常亮功能联调版只在 RAM 打开开发者门，允许上位机命令 11 实测 RawStream；不写回 NVS。 */
    if (APP_BENCH_ALWAYS_ON) {
        /* true 仅解除 RawStream 安全门，诊断流仍需 PC 显式发送开启命令。 */
        s_device_config.developer_mode = true;
    }
    /* 创建所有队列与锁。 */
    if (!app_create_os_objects()) {
        /* 触发可诊断内存错误复位。 */
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    /* 初始化 NimBLE 到应用任务的配对事件邮箱；此时尚未启动任何生产任务或 BLE。 */
    ui_app_pairing_mailbox_init(&s_pairing_mailbox);
    /* 创建完整特征与双 M0 共用的最高频率锁；参数 0 表示不限定最低持续时间。 */
    ESP_ERROR_CHECK(esp_pm_lock_create(
        ESP_PM_CPU_FREQ_MAX,
        0,
        "dual_m0",
        &s_inference_pm_lock));
    /* 先初始化厂家 BSP 与显示；该步骤失败时物理屏幕本身不可用于故障页。 */
    if (!app_init_board_runtime()) {
        /* 记录不可显示的最早板级错误。 */
        ESP_LOGE(APP_TAG, "board runtime blocked startup before UI");
        /* 返回并删除 app_main 任务。 */
        return;
    }
    /* 显示可用后立即创建中文页面树。 */
    if (!app_init_ui_renderer()) {
        /* 页面树失败时只能依赖串口诊断。 */
        ESP_LOGE(APP_TAG, "UI renderer blocked startup");
        /* 返回。 */
        return;
    }
    /* 保存贯穿 BOOT、SELF_TEST 和可能 ERROR 页的局部启动上下文。 */
    ui_context_t startup_ui;
    /* 显示 BOOT 动画后进入可见 SELF_TEST 页。 */
    if (!app_show_startup_pages(&startup_ui)) {
        /* 启动页面链失败，避免继续进入不可见产品状态。 */
        ESP_LOGE(APP_TAG, "startup pages blocked startup");
        /* 返回。 */
        return;
    }
    /* 在 SELF_TEST 可见期间初始化关键板载传感器。 */
    if (!app_init_external_sensors()) {
        /* 显示稳定传感器故障码。 */
        app_show_startup_error(&startup_ui, APP_STARTUP_FAULT_SENSOR);
        /* 保留 LVGL 任务与 ERROR 页，暂停 app_main 而不是静默返回。 */
        vTaskSuspend(NULL);
        /* 静态分析保护；暂停成功后不会到达。 */
        return;
    }
    /* 在 SELF_TEST 可见期间挂载内部存储并恢复会话。 */
    if (!app_init_session_store()) {
        /* 无安全持久化时显示中文错误页。 */
        ESP_LOGE(APP_TAG, "session storage blocked startup");
        /* 显示稳定存储故障码。 */
        app_show_startup_error(&startup_ui, APP_STARTUP_FAULT_STORAGE);
        /* 保留页面并暂停当前任务。 */
        vTaskSuspend(NULL);
        /* 静态分析保护。 */
        return;
    }
    /* 初始化 coordinator 与双 M0；renderer 已经用于启动页面。 */
    if (!app_init_domains()) {
        /* 领域失败时停止启动并显示稳定错误。 */
        ESP_LOGE(APP_TAG, "domain self-test blocked startup");
        /* 显示领域故障码。 */
        app_show_startup_error(&startup_ui, APP_STARTUP_FAULT_DOMAIN);
        /* 保留页面并暂停当前任务。 */
        vTaskSuspend(NULL);
        /* 静态分析保护。 */
        return;
    }
    /* 领域对象就绪后建立空闲基准，避免把后续 BLE 与任务启动耗时计入熄屏门槛。 */
    power_idle_timer_init(&s_power_idle_timer, app_now_ms());
    /* 读取启动电量；失败使用未知哨兵。 */
    uint8_t battery_percent = UINT8_MAX;
    /* 保存充电状态。 */
    bool charging = false;
    /* 读取 PMIC。 */
    const board_adapter_result_t battery_status = board_adapter_read_battery(
        board_runtime_adapter(&s_board_runtime),
        &battery_percent,
        &charging);
    /* 读取失败保持 255。 */
    if (battery_status != BOARD_ADAPTER_OK) {
        /* 输出警告。 */
        ESP_LOGW(APP_TAG, "initial battery unknown");
        /* 标记未充电。 */
        charging = false;
    }
    /* BLE 控制器必须在九个业务任务栈之前预留连续片内 RAM，避免大栈先分配造成碎片。 */
    if (!APP_BENCH_DISABLE_BLE) {
        /* 读取当前可用于控制器、任务栈和普通内部对象的片内总空闲字节数。 */
        const size_t ble_internal_free = heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        /* 读取最大连续片内块；控制器单次分配不能跨越多个离散空闲区。 */
        const size_t ble_internal_largest = heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        /* 串口同时输出总量与连续量，便于区分容量不足和内存碎片。 */
        ESP_LOGI(
            APP_TAG,
            "BLE_HEAP before_start free=%u largest=%u",
            (unsigned int)ble_internal_free,
            (unsigned int)ble_internal_largest);
        /* 启动 BLE 控制器和 NimBLE 主机任务；该调用在主机同步前异步返回。 */
        if (!app_start_ble(battery_percent)) {
            /* 记录离线模式。 */
            ESP_LOGW(APP_TAG, "device continues in offline mode");
        /* 服务启动后等待主机同步和广告活动，禁止九个任务栈抢占主机内存池。 */
        } else if (!app_wait_ble_ready()) {
            /* 停止半启动或不可广播的服务，释放控制器与主机资源。 */
            (void)ble_service_nimble_stop();
            /* 超时后明确进入稳定离线模式，后续任务不会因 BLE 失败反复复位。 */
            ESP_LOGW(APP_TAG, "BLE not ready; device continues in stable offline mode");
        }
    } else {
        /* 明确记录 BLE 未启动是诊断设计，不是 NimBLE 初始化失败。 */
        ESP_LOGW(APP_TAG, "bench isolation: BLE startup skipped");
    }
    /* BLE 控制器完成片内保留后再创建八个业务任务，防止任务栈切碎控制器连续块。 */
    if (!app_create_tasks()) {
        /* 输出稳定内存故障；app_create_tasks 已记录八个任务结果和剩余片内堆。 */
        ESP_LOGE(APP_TAG, "business task allocation blocked startup");
        /* startup_ui 仍处于 SELF_TEST，可合法切到中文错误码 1004。 */
        app_show_startup_error(&startup_ui, APP_STARTUP_FAULT_TASK_MEMORY);
        /* 禁止 ESP_ERROR_CHECK 形成设备检查重启循环；保留错误页供现场诊断。 */
        vTaskSuspend(NULL);
        /* 静态分析保护；暂停成功后不会到达。 */
        return;
    }
    /* 传感器、存储、领域、BLE 和八个业务任务均通过后才允许进入 HOME。 */
    if (!app_finish_startup_success(&startup_ui)) {
        /* 把状态机异常归入领域故障并保留可见页。 */
        app_show_startup_error(&startup_ui, APP_STARTUP_FAULT_DOMAIN);
        /* 暂停当前任务。 */
        vTaskSuspend(NULL);
        /* 静态分析保护。 */
        return;
    }
    /* 渲染初始 Home。 */
    app_queue_ui(&s_coordinator.ui);
    /* 隔离版保持 BSP 已建立的 35% 常亮状态；普通版本才允许初始策略切换全部外设。 */
    if (!APP_BENCH_SKIP_INITIAL_POWER_POLICY) {
        /* 复制初始 Home 功耗策略；联调常亮版需要在 BLE 命令到来前预先保持 QMI 活动。 */
        power_policy_t initial_policy = power_manager_policy(&s_coordinator.power);
        /* 阶段一禁用低功耗并持续采集，避免 Cmd11 控制事务中途切换 QMI 寄存器。 */
        if (APP_BENCH_ALWAYS_ON) {
            /* 六轴采样从开机即使用 ACTIVE 量程和 ODR，RawStream 与分类共享同一连续时间轴。 */
            initial_policy.imu_mode = POWER_IMU_ACTIVE_25HZ;
            /* 常亮联调不得携带自动 Light-sleep 请求。 */
            initial_policy.automatic_light_sleep = false;
            /* 常亮联调不得携带 Deep-sleep 请求。 */
            initial_policy.request_deep_sleep = false;
            /* ACTIVE 采样不使用 QMI 运动唤醒中断。 */
            initial_policy.enable_imu_deep_wake = false;
        }
        /* 投递策略。 */
        app_queue_power(&initial_policy, false);
        /* PMIC 可信时立即送入 15/8/5% 规则。 */
        if (battery_percent <= 100U) {
            /* 构造电池事件。 */
            app_event_t battery_event;
            /* 清零。 */
            (void)memset(&battery_event, 0, sizeof(battery_event));
            /* 标记电池。 */
            battery_event.kind = APP_EVENT_BATTERY;
            /* 捕获时间。 */
            battery_event.monotonic_ms = app_now_ms();
            /* 写百分比。 */
            battery_event.data.battery.percent = battery_percent;
            /* 写充电状态。 */
            battery_event.data.battery.charging = charging;
            /* 确保启动门槛送达。 */
            (void)xQueueSend(s_app_event_queue, &battery_event, portMAX_DELAY);
        }
    } else {
        /* 诊断期间不让后台电源任务重新发送 Display/QMI/BLE 命令。 */
        ESP_LOGW(APP_TAG, "bench isolation: initial power and battery policies skipped");
    }
    /* 输出软件预算；不是实机功耗结果。 */
    ESP_LOGI(
        APP_TAG,
        "ready reset_reason=%d battery=400mAh active_budget=%.2fmA standby_budget=%.3fmA features=297",
        (int)reset_reason,
        (double)power_budget_max_average_current_ma(POWER_ACTIVE_RUNTIME_TARGET_HOURS),
        (double)power_budget_max_average_current_ma(POWER_DEEP_STANDBY_TARGET_HOURS));
    /* app_main 初始化完成，删除自身；长期工作由九个任务承担。 */
    vTaskDelete(NULL);
}
