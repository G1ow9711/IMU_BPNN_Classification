/* 实现 BLE Manifest v1；线上布局与标签定义见 docs/BLE通信协议.md 第 12 节。 */
#include "ble_service_manifest.h"

/* 引入 strlen 和 memcpy；输入字符串只读且线上不复制结尾 NUL。 */
#include <string.h>

/* TLV 固定头由一字节标签和一字节值长度组成。 */
#define BLE_SERVICE_MANIFEST_TLV_HEADER_SIZE ((size_t)2U)
/* CRC-32/ISO-HDLC 的反射多项式为 0xEDB88320。 */
#define BLE_SERVICE_MANIFEST_CRC32_POLYNOMIAL UINT32_C(0xEDB88320)
/* CRC-32/ISO-HDLC 初值和最终异或值均为 0xFFFFFFFF。 */
#define BLE_SERVICE_MANIFEST_CRC32_XOR UINT32_C(0xFFFFFFFF)

/* 验证字符串存在、非空且可由单字节 TLV length 表示。 */
static ble_service_status_t ble_service_manifest_string_length(
    const char *text,
    size_t *length)
{
    /* 两个指针都是必填项，空指针不能进入 strlen。 */
    if ((text == NULL) || (length == NULL)) {
        /* 返回统一空指针错误。 */
        return BLE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    /* 计算不含结尾 NUL 的 UTF-8 字节数；本层不解释 Unicode 码点。 */
    const size_t measured_length = strlen(text);
    /* 空字符串没有设备或版本语义，超过 255 字节也无法写入 u8 length。 */
    if ((measured_length == 0U) || (measured_length > UINT8_MAX)) {
        /* 返回字段范围错误。 */
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    /* 输出已验证长度，调用方可安全转换为 uint8_t。 */
    *length = measured_length;
    /* 字符串合同满足。 */
    return BLE_SERVICE_STATUS_OK;
}

/* 把单个十六进制 ASCII 字符转成 0～15；非法字符返回 false。 */
static int ble_service_manifest_hex_nibble(const char character, uint8_t *nibble)
{
    /* 输出指针必须存在。 */
    if (nibble == NULL) {
        /* 返回假，调用方把它映射为 INVALID_ARGUMENT。 */
        return 0;
    }
    /* 数字字符映射到 0～9。 */
    if ((character >= '0') && (character <= '9')) {
        /* 减去字符零得到数值。 */
        *nibble = (uint8_t)(character - '0');
        /* 返回真表示转换成功。 */
        return 1;
    }
    /* 小写十六进制字符映射到 10～15。 */
    if ((character >= 'a') && (character <= 'f')) {
        /* 在字符差值上加 10。 */
        *nibble = (uint8_t)(UINT8_C(10) + (uint8_t)(character - 'a'));
        /* 返回真表示转换成功。 */
        return 1;
    }
    /* 大写十六进制字符也允许，保证工具链大小写变化不破坏启动。 */
    if ((character >= 'A') && (character <= 'F')) {
        /* 在字符差值上加 10。 */
        *nibble = (uint8_t)(UINT8_C(10) + (uint8_t)(character - 'A'));
        /* 返回真表示转换成功。 */
        return 1;
    }
    /* 其它字符不是合法 SHA-256 十六进制。 */
    return 0;
}

/* 校验 64 字符 SHA-256 文本并解码为 32 字节原始摘要。 */
static ble_service_status_t ble_service_manifest_decode_sha256(
    const char *hex,
    uint8_t digest[BLE_SERVICE_MANIFEST_SHA256_BYTES])
{
    /* 输入文本和输出数组均为必填。 */
    if ((hex == NULL) || (digest == NULL)) {
        /* 返回统一空指针错误。 */
        return BLE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    /* 长度必须严格为 64，禁止截断摘要或尾随字符。 */
    if (strlen(hex) != BLE_SERVICE_MANIFEST_SHA256_HEX_CHARS) {
        /* 返回字段范围错误。 */
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    /* 每两个十六进制字符生成一个摘要字节。 */
    for (size_t index = 0U; index < BLE_SERVICE_MANIFEST_SHA256_BYTES; ++index) {
        /* 保存高四位。 */
        uint8_t high = 0U;
        /* 保存低四位。 */
        uint8_t low = 0U;
        /* 任一字符非法时立即拒绝整个摘要。 */
        if ((ble_service_manifest_hex_nibble(hex[index * 2U], &high) == 0) ||
            (ble_service_manifest_hex_nibble(hex[index * 2U + 1U], &low) == 0)) {
            /* 返回字段内容错误。 */
            return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
        }
        /* 合并高低四位，字节顺序与常见 SHA-256 十六进制显示顺序一致。 */
        digest[index] = (uint8_t)((uint8_t)(high << 4U) | low);
    }
    /* 摘要完整有效。 */
    return BLE_SERVICE_STATUS_OK;
}

/* 用 CRC-32/ISO-HDLC 逐字节更新反射寄存器。 */
static uint32_t ble_service_manifest_crc32_update(
    uint32_t crc,
    const uint8_t byte)
{
    /* 把新字节异或到寄存器低八位。 */
    crc ^= (uint32_t)byte;
    /* 每个输入字节固定执行八轮反射多项式更新。 */
    for (uint8_t bit = 0U; bit < UINT8_C(8); ++bit) {
        /* 保存最低位，避免分支外重复计算。 */
        const uint32_t least_significant_bit = crc & UINT32_C(1);
        /* 先右移一位；若原最低位为一则异或反射多项式。 */
        crc = (crc >> 1U) ^
              (least_significant_bit != 0U ? BLE_SERVICE_MANIFEST_CRC32_POLYNOMIAL : UINT32_C(0));
    }
    /* 返回尚未执行最终异或的寄存器。 */
    return crc;
}

ble_service_status_t ble_service_manifest_class_table_crc32(
    const char *const *class_names,
    const uint8_t class_count,
    uint32_t *crc32)
{
    /* 类别数组和输出指针均为必填，类别数零没有可识别语义。 */
    if ((class_names == NULL) || (crc32 == NULL)) {
        /* 返回统一空指针错误。 */
        return BLE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    /* 零类别不能构造与模型 logits 对应的表。 */
    if (class_count == 0U) {
        /* 返回字段范围错误。 */
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    /* CRC 初值固定为全一。 */
    uint32_t crc = BLE_SERVICE_MANIFEST_CRC32_XOR;
    /* 按 logits 索引顺序处理每个名称，顺序变化必须改变 CRC。 */
    for (uint8_t class_index = 0U; class_index < class_count; ++class_index) {
        /* 保存当前 UTF-8 名称字节数。 */
        size_t name_length = 0U;
        /* 验证名称非空且能写入一字节长度。 */
        const ble_service_status_t name_status =
            ble_service_manifest_string_length(class_names[class_index], &name_length);
        /* 任一类别名无效时拒绝整张表。 */
        if (name_status != BLE_SERVICE_STATUS_OK) {
            /* 保留空指针与范围错误的精确状态。 */
            return name_status;
        }
        /* 按 UTF-8 原始字节顺序纳入名称，不包括 C 字符串结尾 NUL。 */
        for (size_t byte_index = 0U; byte_index < name_length; ++byte_index) {
            /* char 转为无符号字节，避免实现相关符号扩展。 */
            crc = ble_service_manifest_crc32_update(
                crc,
                (uint8_t)(unsigned char)class_names[class_index][byte_index]);
        }
        /* 除最后一个名称外插入单个零字节，锁定相邻类名边界且末尾不附加 NUL。 */
        if ((uint8_t)(class_index + UINT8_C(1)) < class_count) {
            /* 分隔符属于 CRC 规范输入，不属于任一 UTF-8 类名。 */
            crc = ble_service_manifest_crc32_update(crc, UINT8_C(0));
        }
    }
    /* 执行 CRC-32/ISO-HDLC 最终异或并输出。 */
    *crc32 = crc ^ BLE_SERVICE_MANIFEST_CRC32_XOR;
    /* 类别表 CRC 构建成功。 */
    return BLE_SERVICE_STATUS_OK;
}

/* 把 16 位整数按小端序写入至少两字节输出。 */
static void ble_service_manifest_write_u16_le(uint8_t *output, const uint16_t value)
{
    /* 写最低八位。 */
    output[0] = (uint8_t)(value & UINT16_C(0x00FF));
    /* 写最高八位。 */
    output[1] = (uint8_t)((value >> 8U) & UINT16_C(0x00FF));
}

/* 把 32 位整数按小端序写入至少四字节输出。 */
static void ble_service_manifest_write_u32_le(uint8_t *output, const uint32_t value)
{
    /* 逐偏移写入从低到高四个字节。 */
    for (uint8_t byte_index = 0U; byte_index < UINT8_C(4); ++byte_index) {
        /* 每轮右移 8 位的倍数并截取低字节。 */
        output[byte_index] = (uint8_t)((value >> (byte_index * UINT8_C(8))) & UINT32_C(0xFF));
    }
}

/* 把 64 位整数按小端序写入至少八字节输出。 */
static void ble_service_manifest_write_u64_le(uint8_t *output, const uint64_t value)
{
    /* 逐偏移写入从低到高八个字节。 */
    for (uint8_t byte_index = 0U; byte_index < UINT8_C(8); ++byte_index) {
        /* 每轮右移 8 位的倍数并截取低字节。 */
        output[byte_index] = (uint8_t)((value >> (byte_index * UINT8_C(8))) & UINT64_C(0xFF));
    }
}

/* 在已验证容量的输出末尾写入一个 TLV，并推进偏移。 */
static void ble_service_manifest_append_tlv(
    uint8_t *output,
    size_t *offset,
    const uint8_t tag,
    const uint8_t *value,
    const size_t value_length)
{
    /* 写入固定一字节标签。 */
    output[*offset] = tag;
    /* 写入已验证不超过 255 的 value 长度。 */
    output[*offset + 1U] = (uint8_t)value_length;
    /* 非空值复制到两字节 TLV 头之后。 */
    if (value_length > 0U) {
        /* value 已在构建前验证，复制精确字节数且不包含字符串 NUL。 */
        (void)memcpy(&output[*offset + BLE_SERVICE_MANIFEST_TLV_HEADER_SIZE], value, value_length);
    }
    /* 偏移推进到下一个 TLV 起点。 */
    *offset += BLE_SERVICE_MANIFEST_TLV_HEADER_SIZE + value_length;
}

ble_service_status_t ble_service_manifest_build(
    const ble_service_manifest_config_t *config,
    uint8_t *output,
    const size_t output_capacity,
    size_t *output_length)
{
    /* 输出长度指针必须存在，才能在任何失败路径明确返回零。 */
    if (output_length == NULL) {
        /* 无法安全报告长度。 */
        return BLE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    /* 先清零长度，保证后续失败不会让调用方误用旧值。 */
    *output_length = 0U;
    /* 配置和输出缓冲区都是必填项。 */
    if ((config == NULL) || (output == NULL)) {
        /* 返回统一空指针错误。 */
        return BLE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    /* 保存三个字符串的不含 NUL 长度。 */
    size_t device_id_length = 0U;
    /* 保存板卡修订字节数。 */
    size_t board_revision_length = 0U;
    /* 保存固件版本字节数。 */
    size_t firmware_version_length = 0U;
    /* 依次验证三个必填字符串。 */
    ble_service_status_t status =
        ble_service_manifest_string_length(config->device_id, &device_id_length);
    /* 第一个字符串失败时不继续读取其它配置。 */
    if (status != BLE_SERVICE_STATUS_OK) {
        /* 返回精确错误。 */
        return status;
    }
    /* 验证板卡修订号。 */
    status = ble_service_manifest_string_length(config->board_revision, &board_revision_length);
    /* 无效板卡修订号阻断构建。 */
    if (status != BLE_SERVICE_STATUS_OK) {
        /* 返回精确错误。 */
        return status;
    }
    /* 验证固件版本。 */
    status = ble_service_manifest_string_length(config->firmware_version, &firmware_version_length);
    /* 无效固件版本阻断构建。 */
    if (status != BLE_SERVICE_STATUS_OK) {
        /* 返回精确错误。 */
        return status;
    }
    /* 特征维度、特征版本、类别数和卡路里版本均不得为零。 */
    if ((config->feature_dimension == 0U) ||
        (config->feature_version == 0U) ||
        (config->class_count == 0U) ||
        (config->calorie_table_version == 0U)) {
        /* 返回字段范围错误。 */
        return BLE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    /* 保存基础模型 32 字节原始摘要。 */
    uint8_t base_digest[BLE_SERVICE_MANIFEST_SHA256_BYTES];
    /* 保存掩码模型 32 字节原始摘要。 */
    uint8_t masked_digest[BLE_SERVICE_MANIFEST_SHA256_BYTES];
    /* 解码生成头提供的基础模型 SHA-256。 */
    status = ble_service_manifest_decode_sha256(config->base_model_sha256_hex, base_digest);
    /* 摘要长度或字符非法时拒绝。 */
    if (status != BLE_SERVICE_STATUS_OK) {
        /* 返回精确错误。 */
        return status;
    }
    /* 解码生成头提供的掩码模型 SHA-256。 */
    status = ble_service_manifest_decode_sha256(config->masked_model_sha256_hex, masked_digest);
    /* 摘要长度或字符非法时拒绝。 */
    if (status != BLE_SERVICE_STATUS_OK) {
        /* 返回精确错误。 */
        return status;
    }
    /* 保存按固定类名顺序计算的 CRC32。 */
    uint32_t class_table_crc32 = 0U;
    /* 计算类别表 CRC 并同时验证全部名称。 */
    status = ble_service_manifest_class_table_crc32(
        config->class_names,
        config->class_count,
        &class_table_crc32);
    /* 无效类别表阻断构建。 */
    if (status != BLE_SERVICE_STATUS_OK) {
        /* 返回精确错误。 */
        return status;
    }
    /* 11 个 TLV 的固定 value 长度之和不含三个可变字符串。 */
    const size_t fixed_value_length =
        2U + 4U + BLE_SERVICE_MANIFEST_SHA256_BYTES +
        BLE_SERVICE_MANIFEST_SHA256_BYTES + 5U + 2U + 4U + 8U;
    /* 总长度包含 11 个两字节 TLV 头和全部 value。 */
    const size_t required_length =
        11U * BLE_SERVICE_MANIFEST_TLV_HEADER_SIZE +
        device_id_length + board_revision_length + firmware_version_length +
        fixed_value_length;
    /* 输出容量不足时不写任何字节，调用方可扩大到 required_length 后重试。 */
    if (output_capacity < required_length) {
        /* 返回缓冲区不足。 */
        return BLE_SERVICE_STATUS_BUFFER_TOO_SMALL;
    }
    /* 协议版本固定两字节。 */
    const uint8_t protocol_value[2] = {config->protocol_major, config->protocol_minor};
    /* 特征描述固定四字节。 */
    uint8_t feature_value[4];
    /* 写入 feature_dim:u16LE。 */
    ble_service_manifest_write_u16_le(&feature_value[0], config->feature_dimension);
    /* 写入 feature_version:u16LE。 */
    ble_service_manifest_write_u16_le(&feature_value[2], config->feature_version);
    /* 类别描述固定五字节。 */
    uint8_t class_value[5];
    /* 首字节写类别数量。 */
    class_value[0] = config->class_count;
    /* 后四字节写类别表 CRC32 小端值。 */
    ble_service_manifest_write_u32_le(&class_value[1], class_table_crc32);
    /* 卡路里表版本固定两字节。 */
    uint8_t calorie_value[2];
    /* 写 u16LE 卡路里表版本。 */
    ble_service_manifest_write_u16_le(calorie_value, config->calorie_table_version);
    /* 能力位固定四字节。 */
    uint8_t capability_value[4];
    /* 写 u32LE 能力位。 */
    ble_service_manifest_write_u32_le(capability_value, config->capabilities);
    /* LittleFS 可用空间固定八字节。 */
    uint8_t storage_value[8];
    /* 写 u64LE 可用字节数。 */
    ble_service_manifest_write_u64_le(storage_value, config->littlefs_available_bytes);
    /* 从输出起点开始追加固定顺序 TLV。 */
    size_t offset = 0U;
    /* 写动态设备 ID。 */
    ble_service_manifest_append_tlv(output, &offset, BLE_SERVICE_MANIFEST_TAG_DEVICE_ID,
                                    (const uint8_t *)config->device_id, device_id_length);
    /* 写板卡修订。 */
    ble_service_manifest_append_tlv(output, &offset, BLE_SERVICE_MANIFEST_TAG_BOARD_REVISION,
                                    (const uint8_t *)config->board_revision, board_revision_length);
    /* 写协议主次版本。 */
    ble_service_manifest_append_tlv(output, &offset, BLE_SERVICE_MANIFEST_TAG_PROTOCOL_VERSION,
                                    protocol_value, sizeof(protocol_value));
    /* 写固件语义版本。 */
    ble_service_manifest_append_tlv(output, &offset, BLE_SERVICE_MANIFEST_TAG_FIRMWARE_VERSION,
                                    (const uint8_t *)config->firmware_version, firmware_version_length);
    /* 写特征维度和版本。 */
    ble_service_manifest_append_tlv(output, &offset, BLE_SERVICE_MANIFEST_TAG_FEATURE_DESCRIPTOR,
                                    feature_value, sizeof(feature_value));
    /* 写基础模型 SHA-256。 */
    ble_service_manifest_append_tlv(output, &offset, BLE_SERVICE_MANIFEST_TAG_BASE_MODEL_SHA256,
                                    base_digest, sizeof(base_digest));
    /* 写掩码模型 SHA-256。 */
    ble_service_manifest_append_tlv(output, &offset, BLE_SERVICE_MANIFEST_TAG_MASKED_MODEL_SHA256,
                                    masked_digest, sizeof(masked_digest));
    /* 写类别数量和类别表 CRC32。 */
    ble_service_manifest_append_tlv(output, &offset, BLE_SERVICE_MANIFEST_TAG_CLASS_DESCRIPTOR,
                                    class_value, sizeof(class_value));
    /* 写卡路里表版本。 */
    ble_service_manifest_append_tlv(output, &offset, BLE_SERVICE_MANIFEST_TAG_CALORIE_TABLE_VERSION,
                                    calorie_value, sizeof(calorie_value));
    /* 写能力位。 */
    ble_service_manifest_append_tlv(output, &offset, BLE_SERVICE_MANIFEST_TAG_CAPABILITIES,
                                    capability_value, sizeof(capability_value));
    /* 写启动时 LittleFS 可用字节。 */
    ble_service_manifest_append_tlv(output, &offset, BLE_SERVICE_MANIFEST_TAG_LITTLEFS_AVAILABLE_BYTES,
                                    storage_value, sizeof(storage_value));
    /* 输出最终有效长度；offset 必须等于预先计算的 required_length。 */
    *output_length = offset;
    /* 正式 Manifest 构建完成。 */
    return BLE_SERVICE_STATUS_OK;
}
