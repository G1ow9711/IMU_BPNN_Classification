#ifndef BLE_SERVICE_MANIFEST_H
#define BLE_SERVICE_MANIFEST_H

/* 引入 BLE 服务统一状态码和协议主次版本类型。 */
#include "ble_service_core.h"

/* 引入 size_t，所有输入和输出缓冲区长度均使用平台无符号尺寸类型。 */
#include <stddef.h>
/* 引入 uint8_t、uint16_t、uint32_t 和 uint64_t，固定线上整数宽度。 */
#include <stdint.h>

#ifdef __cplusplus
/* C++ 调用者使用 C ABI，避免 ESP-IDF C++ 组件发生符号改名。 */
extern "C" {
#endif

/* 当前 Manifest 特征合同版本为 1，对应固定 297 维顺序；维度或公式变化时必须递增。 */
#define BLE_SERVICE_MANIFEST_FEATURE_VERSION UINT16_C(1)
/* 当前卡路里表版本为 1，对应 docs/算法原理、训练与实时计数.md 的固定 milliMET 表。 */
#define BLE_SERVICE_MANIFEST_CALORIE_TABLE_VERSION UINT16_C(1)
/* SHA-256 原始摘要固定为 32 字节；线上不发送 64 字节十六进制文本。 */
#define BLE_SERVICE_MANIFEST_SHA256_BYTES ((size_t)32U)
/* 十六进制 SHA-256 输入固定为 64 个 ASCII 字符，末尾另有 NUL。 */
#define BLE_SERVICE_MANIFEST_SHA256_HEX_CHARS ((size_t)64U)

/* Manifest v1 TLV 标签；未知标签必须由接收端按 length 跳过。 */
typedef enum ble_service_manifest_tag {
    /* UTF-8 设备 ID，当前使用完整 48 位蓝牙 MAC，不包含结尾 NUL。 */
    BLE_SERVICE_MANIFEST_TAG_DEVICE_ID = 0x01,
    /* UTF-8 板卡修订号，不包含结尾 NUL。 */
    BLE_SERVICE_MANIFEST_TAG_BOARD_REVISION = 0x02,
    /* 两字节协议版本，依次为 major、minor。 */
    BLE_SERVICE_MANIFEST_TAG_PROTOCOL_VERSION = 0x03,
    /* UTF-8 固件语义版本，不包含结尾 NUL。 */
    BLE_SERVICE_MANIFEST_TAG_FIRMWARE_VERSION = 0x04,
    /* 四字节特征描述：feature_dim:u16LE、feature_version:u16LE。 */
    BLE_SERVICE_MANIFEST_TAG_FEATURE_DESCRIPTOR = 0x05,
    /* 基础 M0 模型 SHA-256 原始 32 字节。 */
    BLE_SERVICE_MANIFEST_TAG_BASE_MODEL_SHA256 = 0x06,
    /* 掩码 M0 模型 SHA-256 原始 32 字节。 */
    BLE_SERVICE_MANIFEST_TAG_MASKED_MODEL_SHA256 = 0x07,
    /* 五字节类别描述：class_count:u8、class_table_crc32:u32LE。 */
    BLE_SERVICE_MANIFEST_TAG_CLASS_DESCRIPTOR = 0x08,
    /* 两字节卡路里表版本，采用 u16LE。 */
    BLE_SERVICE_MANIFEST_TAG_CALORIE_TABLE_VERSION = 0x09,
    /* 四字节能力位，采用 u32LE。 */
    BLE_SERVICE_MANIFEST_TAG_CAPABILITIES = 0x0A,
    /* 八字节 LittleFS 可用容量，单位 byte，采用 u64LE。 */
    BLE_SERVICE_MANIFEST_TAG_LITTLEFS_AVAILABLE_BYTES = 0x0B
} ble_service_manifest_tag_t;

/* 能力位只允许声明已接入主应用并通过软件测试的功能。 */
typedef enum ble_service_manifest_capability {
    /* 历史马达能力保留位；当前硬件没有马达，正式 Manifest 不得置位。 */
    BLE_SERVICE_MANIFEST_CAPABILITY_LEGACY_HAPTIC_RESERVED = 1U << 0,
    /* 最近会话摘要可通过 BLE LIST/GET 分页补传。 */
    BLE_SERVICE_MANIFEST_CAPABILITY_SESSION_HISTORY = 1U << 1,
    /* 会话摘要已使用内部 Flash LittleFS 双槽持久化。 */
    BLE_SERVICE_MANIFEST_CAPABILITY_LITTLEFS_STORAGE = 1U << 2,
    /* 预留：开发者原始六轴流真正接入主应用后才允许置位。 */
    BLE_SERVICE_MANIFEST_CAPABILITY_RAW_STREAM = 1U << 3,
    /* 预留：TF 离线原始日志真正接入主应用后才允许置位。 */
    BLE_SERVICE_MANIFEST_CAPABILITY_SD_LOGGING = 1U << 4,
    /* 预留：音频提示真正接入主应用后才允许置位。 */
    BLE_SERVICE_MANIFEST_CAPABILITY_AUDIO_FEEDBACK = 1U << 5
} ble_service_manifest_capability_t;

/* 构建参数只借用调用方数据；构建函数不会保存任何指针。 */
typedef struct ble_service_manifest_config {
    /* device_id 指向非空 UTF-8 设备 ID，线上不带 NUL，单字段最长 255 字节。 */
    const char *device_id;
    /* board_revision 指向非空 UTF-8 板卡修订号，线上不带 NUL。 */
    const char *board_revision;
    /* protocol_major 是逻辑帧主版本，主版本不同表示结构不兼容。 */
    uint8_t protocol_major;
    /* protocol_minor 是向后兼容的逻辑帧次版本。 */
    uint8_t protocol_minor;
    /* firmware_version 指向非空 UTF-8 语义版本，线上不带 NUL。 */
    const char *firmware_version;
    /* feature_dimension 是模型输入特征数，当前必须由生成头 FEATURE_DIM 提供。 */
    uint16_t feature_dimension;
    /* feature_version 是特征公式和顺序版本，当前为 1。 */
    uint16_t feature_version;
    /* base_model_sha256_hex 指向 64 字符小写或大写十六进制 SHA-256。 */
    const char *base_model_sha256_hex;
    /* masked_model_sha256_hex 指向 64 字符小写或大写十六进制 SHA-256。 */
    const char *masked_model_sha256_hex;
    /* class_count 是 logits 类别数量，范围 1～255。 */
    uint8_t class_count;
    /* class_names 指向 class_count 个非空 UTF-8 名称，用于生成稳定类别表 CRC32。 */
    const char *const *class_names;
    /* calorie_table_version 标识固定 milliMET 表版本，零值无效。 */
    uint16_t calorie_table_version;
    /* capabilities 按 ble_service_manifest_capability_t 组合，只声明已实现能力。 */
    uint32_t capabilities;
    /* littlefs_available_bytes 是启动时查询的内部 Flash 剩余字节数。 */
    uint64_t littlefs_available_bytes;
} ble_service_manifest_config_t;

/*
 * 对类别表计算 CRC-32/ISO-HDLC。
 * 规范输入为按 logits 顺序连接的 UTF-8 类名，相邻名称之间插入单个 NUL，末尾不加 NUL；
 * 时间复杂度 O(类别名总字节数)，额外空间 O(1)。
 */
ble_service_status_t ble_service_manifest_class_table_crc32(
    const char *const *class_names,
    uint8_t class_count,
    uint32_t *crc32);

/*
 * 按 [tag:u8][length:u8][value] 顺序构建正式 Manifest v1。
 * 整数 value 均为小端，字符串为无 NUL UTF-8，SHA-256 为原始 32 字节；
 * 函数先完整校验和计算所需容量，失败时 output 不写入且 output_length 置零。
 */
ble_service_status_t ble_service_manifest_build(
    const ble_service_manifest_config_t *config,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

#ifdef __cplusplus
/* 结束 C ABI 区域。 */
}
#endif

#endif
