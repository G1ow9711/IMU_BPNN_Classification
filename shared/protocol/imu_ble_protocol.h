#ifndef IMU_BLE_PROTOCOL_H
#define IMU_BLE_PROTOCOL_H

// 完整帧、CRC、分片公式和边界条件见 docs/BLE通信、设备配置与会话存储.md 第 4～6 节。
// 引入 size_t 和固定宽度整数类型，保证 PC 与 ESP32 使用相同字段宽度。
#include <stddef.h>
// 引入 uint8_t、uint16_t 和 uint32_t，避免平台相关的 int 宽度差异。
#include <stdint.h>

#ifdef __cplusplus
// C++ 编译器进入 C 链接约定，保证 ESP-IDF C++ 调用时符号名称稳定。
extern "C" {
#endif

// 逻辑帧固定魔数采用 0xB17E，小端线上字节顺序为 0x7E、0xB1。
#define IMU_BLE_MAGIC UINT16_C(0xB17E)
// 当前协议主版本为 1；主版本不同表示二进制结构不兼容。
#define IMU_BLE_PROTOCOL_MAJOR UINT8_C(1)
// 当前协议次版本为 0；次版本升级只允许增加可忽略字段或消息。
#define IMU_BLE_PROTOCOL_MINOR UINT8_C(0)
// 逻辑帧固定头长度为 14 字节，不包含末尾 2 字节 CRC16。
#define IMU_BLE_LOGICAL_HEADER_SIZE ((size_t)14U)
// 每个完整逻辑帧末尾使用 2 字节 CRC-16/CCITT-FALSE。
#define IMU_BLE_CRC_SIZE ((size_t)2U)
// 每个 GATT 分片包络为 8 字节，字段均采用小端序。
#define IMU_BLE_FRAGMENT_HEADER_SIZE ((size_t)8U)
// 单个逻辑帧 payload 最多 1024 字节，限制异常输入造成的内存占用。
#define IMU_BLE_MAX_PAYLOAD_SIZE ((size_t)1024U)
// 完整帧最大长度等于固定头、最大 payload 和 CRC 长度之和。
#define IMU_BLE_MAX_FRAME_SIZE (IMU_BLE_LOGICAL_HEADER_SIZE + IMU_BLE_MAX_PAYLOAD_SIZE + IMU_BLE_CRC_SIZE)

// 协议函数统一返回该错误码，调用者可把错误映射为 BLE NACK 或诊断日志。
typedef enum imu_ble_status {
    // 操作成功，输出参数已经写入有效结果。
    IMU_BLE_STATUS_OK = 0,
    // 必填指针为空，调用者没有提供有效输入或输出对象。
    IMU_BLE_STATUS_NULL_ARGUMENT = 1,
    // 输入长度、字段取值或分片顺序不满足协议约束。
    IMU_BLE_STATUS_INVALID_ARGUMENT = 2,
    // 输出缓冲区容量不足，调用者必须扩大缓冲区后重试。
    IMU_BLE_STATUS_BUFFER_TOO_SMALL = 3,
    // 输入魔数不是 0xB17E，说明数据不是本协议逻辑帧。
    IMU_BLE_STATUS_BAD_MAGIC = 4,
    // payload 长度超过 1024 字节或与实际输入长度不一致。
    IMU_BLE_STATUS_BAD_LENGTH = 5,
    // 收到的 CRC16 与重新计算值不同，逻辑帧必须丢弃。
    IMU_BLE_STATUS_BAD_CRC = 6,
    // 分片序号、总数或逻辑 sequence 与当前重组状态不一致。
    IMU_BLE_STATUS_BAD_FRAGMENT = 7
} imu_ble_status_t;

// 逻辑帧视图保存已解析字段；payload 指针只借用调用者缓冲区，不拥有内存。
typedef struct imu_ble_frame_view {
    // 协议主版本，当前发送值固定为 1。
    uint8_t protocol_major;
    // 协议次版本，当前发送值固定为 0。
    uint8_t protocol_minor;
    // 消息类型由上层 GATT 协议表定义，例如实时状态或控制请求。
    uint8_t message_type;
    // 标志位由上层定义，例如响应、错误、快照或结束块。
    uint8_t flags;
    // 16 位逻辑帧序号用于发现丢帧；回绕后按模 65536 比较。
    uint16_t sequence;
    // 设备开机后的单调毫秒时间，允许约 49.7 天自然回绕。
    uint32_t monotonic_ms;
    // payload 指向连续字节；payload_length 为零时允许为空。
    const uint8_t *payload;
    // payload 实际字节数，取值范围为 0 到 1024。
    uint16_t payload_length;
} imu_ble_frame_view_t;

// 重组状态只接受同一 GATT 特征上的顺序分片，并在缺片或乱序时明确失败。
typedef struct imu_ble_reassembler {
    // buffer 保存完整逻辑帧，最大占用 1040 字节，生命周期与状态对象一致。
    uint8_t buffer[IMU_BLE_MAX_FRAME_SIZE];
    // length 表示当前已经写入 buffer 的有效字节数。
    size_t length;
    // logical_sequence 表示当前正在重组的逻辑帧序号。
    uint16_t logical_sequence;
    // fragment_count 表示发送端声明的总分片数。
    uint16_t fragment_count;
    // next_fragment_index 表示下一包必须携带的分片索引。
    uint16_t next_fragment_index;
    // active 为 1 表示正在重组，为 0 表示等待索引 0 的新帧。
    uint8_t active;
} imu_ble_reassembler_t;

// 计算 CRC-16/CCITT-FALSE；空输入允许 data 为空且 length 为零。
uint16_t imu_ble_crc16_ccitt_false(const uint8_t *data, size_t length);

// 把字段视图编码为完整逻辑帧；output_length 返回实际写入字节数。
imu_ble_status_t imu_ble_encode_frame(
    const imu_ble_frame_view_t *frame,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

// 校验并解析完整逻辑帧；解析后的 payload 指针引用 input 内部区域。
imu_ble_status_t imu_ble_decode_frame(
    const uint8_t *input,
    size_t input_length,
    imu_ble_frame_view_t *frame);

// 按协商 ATT MTU 计算分片总数；ATT Value 容量固定为 mtu-3 字节。
imu_ble_status_t imu_ble_get_fragment_count(
    size_t frame_length,
    uint16_t att_mtu,
    uint16_t *fragment_count);

// 编码指定索引的 GATT 分片；逻辑帧序号同时写入 8 字节分片包络。
imu_ble_status_t imu_ble_encode_fragment(
    // 非空只读完整逻辑帧；生命周期覆盖本次编码，长度由 frame_length 指定。
    const uint8_t *frame,
    // 完整逻辑帧有效长度，单位字节；不得超过 IMU_BLE_MAX_FRAME_SIZE。
    size_t frame_length,
    // 当前协商 ATT MTU，单位字节；最小值为 23。
    uint16_t att_mtu,
    // 逻辑帧 16 位序号；全部分片必须使用同一值。
    uint16_t logical_sequence,
    // 当前零基分片索引；必须小于按 MTU 计算的 fragment_count。
    uint16_t fragment_index,
    // 非空可写输出；形状为 [output_capacity] 字节，保存一个 ATT Value。
    uint8_t *output,
    // 输出缓冲区容量，单位字节；必须容纳 8 字节包络和本片数据。
    size_t output_capacity,
    // 非空输出指针；成功时写入当前分片实际字节数。
    size_t *output_length);

// 清空重组状态；设备断连、超时或协议错误后必须调用该函数。
void imu_ble_reassembler_reset(imu_ble_reassembler_t *state);

// 顺序接收一个分片；complete 为 1 时 frame_data 指向 state 内的完整帧。
// frame_data 和其它输出必须非空；返回的二级指针只借用 state.buffer，生命周期持续到下一次
// push/reset 或 state 销毁，调用者不得释放、修改或在该生命周期后继续访问。
imu_ble_status_t imu_ble_reassembler_push(
    // 非空重组状态；生命周期覆盖同一逻辑帧的全部分片。
    imu_ble_reassembler_t *state,
    // 非空只读当前分片；生命周期仅需覆盖本次同步调用。
    const uint8_t *fragment,
    // 当前分片有效长度，单位字节；至少包含 8 字节分片包络。
    size_t fragment_length,
    // 非空二级输出；完成时指向 state.buffer，未完成时写空指针。
    const uint8_t **frame_data,
    // 非空输出；完成时写入逻辑帧长度，单位字节。
    size_t *frame_length,
    // 非空输出；写 1 表示帧完整且 frame_data 有效，写 0 表示继续等待。
    uint8_t *complete);

#ifdef __cplusplus
// 结束 C 链接约定，后续 C++ 代码恢复默认名称修饰规则。
}
#endif

#endif
