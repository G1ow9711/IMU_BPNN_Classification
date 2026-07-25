// 引入协议接口，测试直接调用与 ESP32 相同的 C 编解码实现。
#include "imu_ble_protocol.h"

// 引入标准输入输出，用于打印失败原因和最终通过标志。
#include <stdio.h>
// 引入 memcmp 和 memcpy，用于逐字节比较黄金向量和构造损坏帧。
#include <string.h>

// 断言条件失败时打印源码行号并返回非零退出码，便于 CI 定位。
#define CHECK(condition) do { if (!(condition)) { (void)fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

// 主函数依次验证 CRC、逻辑帧、MTU23 分片、重组和损坏检测。
int main(void)
{
    // 标准字符串“123456789”用于核对 CCITT-FALSE 公认结果 0x29B1。
    static const uint8_t crc_input[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    // 调用协议 CRC 实现并要求得到标准黄金值。
    CHECK(imu_ble_crc16_ccitt_false(crc_input, sizeof(crc_input)) == UINT16_C(0x29B1));

    // payload 固定为 5 字节，便于人工核对长度和 CRC 覆盖范围。
    static const uint8_t payload[] = { UINT8_C(0x10), UINT8_C(0x20), UINT8_C(0x30), UINT8_C(0x40), UINT8_C(0x50) };
    // 完整逻辑帧黄金字节来自 shared/protocol/golden_vectors.json。
    static const uint8_t expected_frame[] = {
        UINT8_C(0x7E), UINT8_C(0xB1), UINT8_C(0x01), UINT8_C(0x00), UINT8_C(0x03), UINT8_C(0x01),
        UINT8_C(0x34), UINT8_C(0x12), UINT8_C(0x04), UINT8_C(0x03), UINT8_C(0x02), UINT8_C(0x01),
        UINT8_C(0x05), UINT8_C(0x00), UINT8_C(0x10), UINT8_C(0x20), UINT8_C(0x30), UINT8_C(0x40),
        UINT8_C(0x50), UINT8_C(0x92), UINT8_C(0xF9)
    };
    // 第一片黄金字节对应 ATT MTU 23，包络后可承载 12 字节逻辑帧。
    static const uint8_t expected_fragment0[] = {
        UINT8_C(0x34), UINT8_C(0x12), UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x02), UINT8_C(0x00),
        UINT8_C(0x0C), UINT8_C(0x00), UINT8_C(0x7E), UINT8_C(0xB1), UINT8_C(0x01), UINT8_C(0x00),
        UINT8_C(0x03), UINT8_C(0x01), UINT8_C(0x34), UINT8_C(0x12), UINT8_C(0x04), UINT8_C(0x03),
        UINT8_C(0x02), UINT8_C(0x01)
    };
    // 第二片黄金字节携带剩余 9 字节，末尾包括小端 CRC 0xF992。
    static const uint8_t expected_fragment1[] = {
        UINT8_C(0x34), UINT8_C(0x12), UINT8_C(0x01), UINT8_C(0x00), UINT8_C(0x02), UINT8_C(0x00),
        UINT8_C(0x09), UINT8_C(0x00), UINT8_C(0x05), UINT8_C(0x00), UINT8_C(0x10), UINT8_C(0x20),
        UINT8_C(0x30), UINT8_C(0x40), UINT8_C(0x50), UINT8_C(0x92), UINT8_C(0xF9)
    };

    // 构造字段视图，所有数值与 JSON 黄金向量保持一致。
    const imu_ble_frame_view_t source_frame = {
        // 协议主版本固定为 1。
        IMU_BLE_PROTOCOL_MAJOR,
        // 协议次版本固定为 0。
        IMU_BLE_PROTOCOL_MINOR,
        // 消息类型 3 代表实时状态示例。
        UINT8_C(3),
        // 标志 1 作为黄金向量的非零覆盖值。
        UINT8_C(1),
        // sequence 0x1234 用于验证小端编码。
        UINT16_C(0x1234),
        // 单调时间 0x01020304 用于验证四字节顺序。
        UINT32_C(0x01020304),
        // payload 指向上方 5 字节只读数组。
        payload,
        // payload 长度等于数组元素数。
        (uint16_t)sizeof(payload)
    };
    // 编码输出缓冲区按协议最大帧长度分配，避免动态内存。
    uint8_t encoded[IMU_BLE_MAX_FRAME_SIZE];
    // encoded_length 接收编码器写入的实际字节数。
    size_t encoded_length = 0U;
    // 编码字段视图并要求成功。
    CHECK(imu_ble_encode_frame(&source_frame, encoded, sizeof(encoded), &encoded_length) == IMU_BLE_STATUS_OK);
    // 完整帧长度必须与黄金数组相同。
    CHECK(encoded_length == sizeof(expected_frame));
    // 完整帧每个字节必须与黄金向量一致。
    CHECK(memcmp(encoded, expected_frame, sizeof(expected_frame)) == 0);

    // decoded 接收零拷贝解析结果。
    imu_ble_frame_view_t decoded;
    // 解析刚编码的完整帧并要求通过长度和 CRC 校验。
    CHECK(imu_ble_decode_frame(encoded, encoded_length, &decoded) == IMU_BLE_STATUS_OK);
    // 核对解析后的消息类型。
    CHECK(decoded.message_type == UINT8_C(3));
    // 核对解析后的 sequence。
    CHECK(decoded.sequence == UINT16_C(0x1234));
    // 核对解析后的单调毫秒时间。
    CHECK(decoded.monotonic_ms == UINT32_C(0x01020304));
    // 核对 payload 长度。
    CHECK(decoded.payload_length == (uint16_t)sizeof(payload));
    // 核对 payload 内容。
    CHECK(memcmp(decoded.payload, payload, sizeof(payload)) == 0);

    // MTU23 下黄金逻辑帧应拆成两片。
    uint16_t fragment_count = 0U;
    // 调用分片计数函数并要求返回 2。
    CHECK(imu_ble_get_fragment_count(encoded_length, UINT16_C(23), &fragment_count) == IMU_BLE_STATUS_OK);
    // 核对分片数量。
    CHECK(fragment_count == UINT16_C(2));

    // 单片缓冲区按 MTU23 的最大 ATT Value 20 字节分配。
    uint8_t fragment0[20];
    // fragment0_length 接收第一片实际长度。
    size_t fragment0_length = 0U;
    // 编码索引 0 的第一片。
    CHECK(imu_ble_encode_fragment(encoded, encoded_length, UINT16_C(23), UINT16_C(0x1234), UINT16_C(0), fragment0, sizeof(fragment0), &fragment0_length) == IMU_BLE_STATUS_OK);
    // 第一片应填满 20 字节 ATT Value。
    CHECK(fragment0_length == sizeof(expected_fragment0));
    // 第一片必须逐字节匹配黄金向量。
    CHECK(memcmp(fragment0, expected_fragment0, sizeof(expected_fragment0)) == 0);

    // 第二片只有 17 字节，因此缓冲区仍使用上限 20 字节。
    uint8_t fragment1[20];
    // fragment1_length 接收第二片实际长度。
    size_t fragment1_length = 0U;
    // 编码索引 1 的最后一片。
    CHECK(imu_ble_encode_fragment(encoded, encoded_length, UINT16_C(23), UINT16_C(0x1234), UINT16_C(1), fragment1, sizeof(fragment1), &fragment1_length) == IMU_BLE_STATUS_OK);
    // 第二片实际长度必须与黄金数组相同。
    CHECK(fragment1_length == sizeof(expected_fragment1));
    // 第二片必须逐字节匹配黄金向量。
    CHECK(memcmp(fragment1, expected_fragment1, sizeof(expected_fragment1)) == 0);

    // 重组状态使用固定内存，先显式清零。
    imu_ble_reassembler_t reassembler;
    // 断连路径和首次使用都调用同一重置函数。
    imu_ble_reassembler_reset(&reassembler);
    // reassembled_data 在完成时指向 reassembler 内部缓冲区。
    const uint8_t *reassembled_data = NULL;
    // reassembled_length 接收完整逻辑帧长度。
    size_t reassembled_length = 0U;
    // complete 标记当前 push 是否完成一帧。
    uint8_t complete = UINT8_C(0);
    // 推入第一片后应成功但尚未完成。
    CHECK(imu_ble_reassembler_push(&reassembler, fragment0, fragment0_length, &reassembled_data, &reassembled_length, &complete) == IMU_BLE_STATUS_OK);
    // 第一片后完成标志必须为零。
    CHECK(complete == UINT8_C(0));
    // 推入第二片后应完成并通过完整帧 CRC 校验。
    CHECK(imu_ble_reassembler_push(&reassembler, fragment1, fragment1_length, &reassembled_data, &reassembled_length, &complete) == IMU_BLE_STATUS_OK);
    // 第二片后完成标志必须为一。
    CHECK(complete == UINT8_C(1));
    // 重组长度必须等于原始编码长度。
    CHECK(reassembled_length == encoded_length);
    // 重组结果必须与原始完整帧逐字节相同。
    CHECK(memcmp(reassembled_data, encoded, encoded_length) == 0);

    // 创建一份损坏帧副本，用于验证应用层 CRC 能发现单比特错误。
    uint8_t corrupted[sizeof(expected_frame)];
    // 复制正确帧作为损坏测试起点。
    (void)memcpy(corrupted, expected_frame, sizeof(corrupted));
    // 翻转 payload 第一个字节的最低位，但保留原 CRC 不变。
    corrupted[IMU_BLE_LOGICAL_HEADER_SIZE] ^= UINT8_C(0x01);
    // 解码器必须返回 BAD_CRC，不能把损坏 payload 交给业务层。
    CHECK(imu_ble_decode_frame(corrupted, sizeof(corrupted), &decoded) == IMU_BLE_STATUS_BAD_CRC);

    // 打印唯一成功标志，便于 RTK 或 CI 过滤测试输出。
    (void)puts("C_PROTOCOL_TESTS_OK");
    // 返回零表示全部 C 黄金向量和边界测试通过。
    return 0;
}
