// 引入小端整数读取工具，保证 PC 与 ESP32 的多字节字段解释一致。
using System.Buffers.Binary;
// 引入严格 UTF-8 解码器，禁止把损坏字节静默替换为问号。
using System.Text;

// Manifest 编解码类型位于纯蓝牙协议层，不依赖 WinRT 或 WPF。
namespace FitnessCoach.Bluetooth;

/// <summary>
/// 保存通过严格兼容性检查的 Manifest v1；32 字节模型摘要转换为小写十六进制文本。
/// </summary>
public sealed class ManifestV1
{
    /// <summary>创建完整且已验证的 Manifest v1 值对象。</summary>
    public ManifestV1(
        // 设备持久 ID；必须是非空严格 UTF-8 文本且不得含 NUL。
        string deviceId,
        // 板卡硬件修订文本；用于确认固件烧录目标与厂家板型一致。
        string boardRevision,
        // BLE 应用协议主版本；当前兼容合同固定为 1。
        byte protocolMajor,
        // BLE 应用协议次版本；当前兼容合同固定为 0。
        byte protocolMinor,
        // 固件语义版本文本；用于诊断升级和协议问题。
        string firmwareVersion,
        // 手工特征数量；当前双 M0 合同固定为 297，无量纲计数。
        ushort featureDimension,
        // 特征名称与索引顺序版本；当前固定为 1。
        ushort featureVersion,
        // 基础 M0 SHA-256；必须是 64 个小写十六进制字符。
        string baseModelSha256,
        // 掩码 M0 SHA-256；必须是 64 个小写十六进制字符。
        string maskedModelSha256,
        // 网络输出类别数量；当前固定为 11。
        byte classCount,
        // 类别名称与 logits 顺序表的 CRC-32/ISO-HDLC。
        uint classTableCrc32,
        // 卡路里 MET 参数表版本；当前设备与 PC 均要求版本 1。
        ushort calorieTableVersion,
        // 设备能力位；位 0 为旧马达保留，位 1、2 表示会话历史和 LittleFS。
        uint capabilities,
        // 设备实时 LittleFS 可用容量，单位字节。
        ulong littleFsAvailableBytes)
    {
        // 保存设备稳定 ID；解析器已保证非空且不含 NUL。
        DeviceId = deviceId;
        // 保存板卡修订文本，用于诊断固件是否烧到目标硬件。
        BoardRevision = boardRevision;
        // 保存协议主版本；当前兼容值固定为 1。
        ProtocolMajor = protocolMajor;
        // 保存协议次版本；当前严格合同固定为 0。
        ProtocolMinor = protocolMinor;
        // 保存固件语义版本文本。
        FirmwareVersion = firmwareVersion;
        // 保存手工特征维度；当前模型合同固定为 297。
        FeatureDimension = featureDimension;
        // 保存特征顺序版本；当前固定为 1。
        FeatureVersion = featureVersion;
        // 保存基础 M0 的 64 字符小写 SHA-256。
        BaseModelSha256 = baseModelSha256;
        // 保存掩码 M0 的 64 字符小写 SHA-256。
        MaskedModelSha256 = maskedModelSha256;
        // 保存 logits 类别数量；当前固定为 11。
        ClassCount = classCount;
        // 保存类别顺序表 CRC-32/ISO-HDLC；线上按小端编码。
        ClassTableCrc32 = classTableCrc32;
        // 保存热量参数表版本；当前固定为 1。
        CalorieTableVersion = calorieTableVersion;
        // 保存能力位；未知高位允许保留供未来固件扩展。
        Capabilities = capabilities;
        // 保存 LittleFS 当前可用字节数；该值来自设备每次连接时的实时查询。
        LittleFsAvailableBytes = littleFsAvailableBytes;
    }

    /// <summary>设备持久 ID，例如 12 位芯片标识。</summary>
    public string DeviceId { get; }

    /// <summary>目标板卡硬件修订文本。</summary>
    public string BoardRevision { get; }

    /// <summary>BLE 应用协议主版本。</summary>
    public byte ProtocolMajor { get; }

    /// <summary>BLE 应用协议次版本。</summary>
    public byte ProtocolMinor { get; }

    /// <summary>设备固件版本文本。</summary>
    public string FirmwareVersion { get; }

    /// <summary>送入 M0 的特征数量。</summary>
    public ushort FeatureDimension { get; }

    /// <summary>297 项特征顺序版本。</summary>
    public ushort FeatureVersion { get; }

    /// <summary>基础 M0 模型 SHA-256，小写十六进制 64 字符。</summary>
    public string BaseModelSha256 { get; }

    /// <summary>掩码 M0 模型 SHA-256，小写十六进制 64 字符。</summary>
    public string MaskedModelSha256 { get; }

    /// <summary>模型输出类别数量。</summary>
    public byte ClassCount { get; }

    /// <summary>类别名称和顺序表 CRC-32/ISO-HDLC。</summary>
    public uint ClassTableCrc32 { get; }

    /// <summary>热量参数表版本。</summary>
    public ushort CalorieTableVersion { get; }

    /// <summary>设备能力位；位 0 为旧马达保留，位 1、2 分别表示会话历史和 LittleFS。</summary>
    public uint Capabilities { get; }

    /// <summary>LittleFS 当前可用容量，单位字节。</summary>
    public ulong LittleFsAvailableBytes { get; }

    /// <summary>基础模型摘要前 12 个十六进制字符，便于 UI 快速核对。</summary>
    public string BaseModelSha256Short => BaseModelSha256[..12];

    /// <summary>掩码模型摘要前 12 个十六进制字符，便于 UI 快速核对。</summary>
    public string MaskedModelSha256Short => MaskedModelSha256[..12];
}

/// <summary>
/// 解析固件发出的 TLV Manifest v1；未知 tag 可跳过，已知 tag 的缺失、重复或错误长度必须拒绝。
/// </summary>
public static class ManifestV1Codec
{
    // 设备 ID tag；value 为非空严格 UTF-8。
    private const byte DeviceIdTag = 0x01;
    // 板卡修订 tag；value 为非空严格 UTF-8。
    private const byte BoardRevisionTag = 0x02;
    // 协议版本 tag；value 固定两个字节 major、minor。
    private const byte ProtocolVersionTag = 0x03;
    // 固件版本 tag；value 为非空严格 UTF-8。
    private const byte FirmwareVersionTag = 0x04;
    // 特征合同 tag；value 为两个小端 uint16：维度和版本。
    private const byte FeatureContractTag = 0x05;
    // 基础模型 SHA-256 tag；value 固定 32 个原始摘要字节。
    private const byte BaseModelSha256Tag = 0x06;
    // 掩码模型 SHA-256 tag；value 固定 32 个原始摘要字节。
    private const byte MaskedModelSha256Tag = 0x07;
    // 类别表 tag；value 为 class_count 加小端 CRC32。
    private const byte ClassDescriptorTag = 0x08;
    // 热量表版本 tag；value 为小端 uint16。
    private const byte CalorieTableVersionTag = 0x09;
    // 能力位 tag；value 为小端 uint32。
    private const byte CapabilitiesTag = 0x0A;
    // LittleFS 可用量 tag；value 为小端 uint64 字节数。
    private const byte LittleFsAvailableTag = 0x0B;
    // Manifest 最大允许长度限制异常设备造成的无界内存和解析时间。
    private const int MaximumManifestLength = 512;
    // 当前特征维度与 Python/C 297 项顺序绑定。
    public const ushort RequiredFeatureDimension = 297;
    // 当前特征顺序版本固定为 1。
    public const ushort RequiredFeatureVersion = 1;
    // 当前 logits 固定为 11 类。
    public const byte RequiredClassCount = 11;
    // 类别名称按 logits 顺序并以单个 NUL 分隔计算得到的 CRC-32/ISO-HDLC。
    public const uint RequiredClassTableCrc32 = 0xD8193927U;
    // 当前热量计算表版本固定为 1。
    public const ushort RequiredCalorieTableVersion = 1;
    // 正式 PC 只要求会话历史和 LittleFS；位 0 的旧马达能力不再要求。
    public const uint RequiredCapabilities = 0x00000006U;
    // 严格 UTF-8 解码器在遇到非法字节时抛异常，不生成替换字符。
    private static readonly UTF8Encoding StrictUtf8 = new(
        encoderShouldEmitUTF8Identifier: false,
        throwOnInvalidBytes: true);
    // 11 个必填 tag 用于解析结束后的完整性检查。
    private static readonly byte[] RequiredTags =
    [
        DeviceIdTag,
        BoardRevisionTag,
        ProtocolVersionTag,
        FirmwareVersionTag,
        FeatureContractTag,
        BaseModelSha256Tag,
        MaskedModelSha256Tag,
        ClassDescriptorTag,
        CalorieTableVersionTag,
        CapabilitiesTag,
        LittleFsAvailableTag,
    ];

    /// <summary>
    /// 解析并验证 TLV v1；输入格式为重复的 tag:u8、length:u8、value:length，整数全部为小端。
    /// 时间复杂度 O(B)，空间复杂度 O(T)，B 不超过 512 字节，T 为 tag 数。
    /// </summary>
    public static ManifestV1 Parse(ReadOnlySpan<byte> input)
    {
        // 空值不包含任何必填 tag，直接拒绝。
        if (input.IsEmpty)
        {
            // 抛出数据格式异常，连接状态机将释放半初始化 GATT 链路。
            throw new InvalidDataException("设备 Manifest 为空。");
        }

        // 限制总长度，避免异常固件利用未知 tag 造成过量处理。
        if (input.Length > MaximumManifestLength)
        {
            // 报告实际长度和协议上限，便于固件诊断。
            throw new InvalidDataException($"设备 Manifest 长度 {input.Length} 超过 {MaximumManifestLength} 字节上限。");
        }

        // 记录全部 tag，包括未知 tag；同一 tag 重复会产生歧义，统一拒绝。
        HashSet<byte> seenTags = [];
        // 保存必填设备 ID，null 表示尚未遇到 tag 0x01。
        string? deviceId = null;
        // 保存必填板卡修订。
        string? boardRevision = null;
        // 保存协议主版本。
        byte? protocolMajor = null;
        // 保存协议次版本。
        byte? protocolMinor = null;
        // 保存固件版本。
        string? firmwareVersion = null;
        // 保存特征维度。
        ushort? featureDimension = null;
        // 保存特征顺序版本。
        ushort? featureVersion = null;
        // 保存基础模型摘要。
        string? baseModelSha256 = null;
        // 保存掩码模型摘要。
        string? maskedModelSha256 = null;
        // 保存类别数量。
        byte? classCount = null;
        // 保存类别表 CRC32。
        uint? classTableCrc32 = null;
        // 保存热量表版本。
        ushort? calorieTableVersion = null;
        // 保存能力位。
        uint? capabilities = null;
        // 保存 LittleFS 可用字节数。
        ulong? littleFsAvailableBytes = null;
        // offset 指向下一个 TLV 的 tag；循环终止条件为恰好消费全部输入。
        int offset = 0;

        // 逐个解析 TLV；未知 tag 只跳过 value，保持协议向前扩展能力。
        while (offset < input.Length)
        {
            // TLV 至少还需要 tag 和 length 两字节。
            if ((input.Length - offset) < 2)
            {
                // 只有 tag 或残余一字节属于截断帧。
                throw new InvalidDataException($"Manifest 在偏移 {offset} 处缺少完整 TLV 头。");
            }

            // 读取一字节 tag 后移动游标。
            byte tag = input[offset++];
            // 读取一字节 value 长度后移动游标。
            int length = input[offset++];
            // 声明长度不能超过剩余输入。
            if ((input.Length - offset) < length)
            {
                // 报告 tag 和缺口，定位固件构造错误。
                throw new InvalidDataException($"Manifest tag 0x{tag:X2} 声明 {length} 字节但数据已截断。");
            }

            // 重复 tag 会导致接收端无法判断哪个值权威。
            if (!seenTags.Add(tag))
            {
                // 已知和未知 tag 均采用同一严格重复规则。
                throw new InvalidDataException($"Manifest tag 0x{tag:X2} 重复出现。");
            }

            // value 仅在当前循环有效，不产生多余数组分配。
            ReadOnlySpan<byte> value = input.Slice(offset, length);
            // 将游标移动到下一个 TLV 头。
            offset += length;

            // 按 tag 解码已知合同；default 明确保留未知扩展。
            switch (tag)
            {
                case DeviceIdTag:
                    // 设备 ID 必须是非空严格 UTF-8 且不含 NUL。
                    deviceId = DecodeRequiredText(value, tag, "设备 ID");
                    // 完成当前 tag。
                    break;
                case BoardRevisionTag:
                    // 板卡修订必须是非空严格 UTF-8。
                    boardRevision = DecodeRequiredText(value, tag, "板卡修订");
                    // 完成当前 tag。
                    break;
                case ProtocolVersionTag:
                    // 协议版本固定两个字节，避免隐式字段扩展改变已有解析。
                    RequireLength(value, tag, 2);
                    // 第一个字节为主版本。
                    protocolMajor = value[0];
                    // 第二个字节为次版本。
                    protocolMinor = value[1];
                    // 完成当前 tag。
                    break;
                case FirmwareVersionTag:
                    // 固件版本必须是非空严格 UTF-8。
                    firmwareVersion = DecodeRequiredText(value, tag, "固件版本");
                    // 完成当前 tag。
                    break;
                case FeatureContractTag:
                    // 两个 uint16 共四字节。
                    RequireLength(value, tag, 4);
                    // 前两字节按小端读取特征数量。
                    featureDimension = BinaryPrimitives.ReadUInt16LittleEndian(value[..2]);
                    // 后两字节按小端读取特征顺序版本。
                    featureVersion = BinaryPrimitives.ReadUInt16LittleEndian(value[2..4]);
                    // 完成当前 tag。
                    break;
                case BaseModelSha256Tag:
                    // SHA-256 必须是 32 个原始字节，不接受 64 字节 ASCII hex。
                    RequireLength(value, tag, 32);
                    // 转成稳定小写十六进制供 UI 和日志显示。
                    baseModelSha256 = Convert.ToHexString(value).ToLowerInvariant();
                    // 完成当前 tag。
                    break;
                case MaskedModelSha256Tag:
                    // 掩码模型摘要同样固定 32 个原始字节。
                    RequireLength(value, tag, 32);
                    // 转成稳定小写十六进制。
                    maskedModelSha256 = Convert.ToHexString(value).ToLowerInvariant();
                    // 完成当前 tag。
                    break;
                case ClassDescriptorTag:
                    // 类别数量一字节加 CRC32 四字节，共五字节。
                    RequireLength(value, tag, 5);
                    // 首字节保存 logits 类别数量。
                    classCount = value[0];
                    // 后四字节按小端读取 CRC32。
                    classTableCrc32 = BinaryPrimitives.ReadUInt32LittleEndian(value[1..5]);
                    // 完成当前 tag。
                    break;
                case CalorieTableVersionTag:
                    // 热量表版本固定 uint16。
                    RequireLength(value, tag, 2);
                    // 按小端读取版本。
                    calorieTableVersion = BinaryPrimitives.ReadUInt16LittleEndian(value);
                    // 完成当前 tag。
                    break;
                case CapabilitiesTag:
                    // 能力位固定 uint32。
                    RequireLength(value, tag, 4);
                    // 按小端读取能力掩码。
                    capabilities = BinaryPrimitives.ReadUInt32LittleEndian(value);
                    // 完成当前 tag。
                    break;
                case LittleFsAvailableTag:
                    // LittleFS 可用量固定 uint64，单位字节。
                    RequireLength(value, tag, 8);
                    // 按小端读取容量，避免大于 4 GiB 时截断。
                    littleFsAvailableBytes = BinaryPrimitives.ReadUInt64LittleEndian(value);
                    // 完成当前 tag。
                    break;
                default:
                    // 未知 tag 已按 length 完整跳过，允许未来固件追加字段。
                    break;
            }
        }

        // 检查 11 个必填 tag；未知 tag 不影响完整性判断。
        foreach (byte requiredTag in RequiredTags)
        {
            // 任一必填 tag 缺失都无法证明固件、模型和 PC 兼容。
            if (!seenTags.Contains(requiredTag))
            {
                // 指出缺失 tag，便于抓包定位。
                throw new InvalidDataException($"Manifest 缺少必填 tag 0x{requiredTag:X2}。");
            }
        }

        // 当前上位机严格支持协议 1.0；未知 tag 的向前兼容不放宽核心版本。
        if ((protocolMajor != ProtocolConstants.ProtocolMajor) || (protocolMinor != ProtocolConstants.ProtocolMinor))
        {
            // 拒绝核心语义可能不同的设备。
            throw new InvalidDataException($"Manifest 协议 {protocolMajor}.{protocolMinor} 与 PC 要求的 {ProtocolConstants.ProtocolMajor}.{ProtocolConstants.ProtocolMinor} 不兼容。");
        }

        // 特征数量和顺序版本必须与两个 M0 权重完全一致。
        if ((featureDimension != RequiredFeatureDimension) || (featureVersion != RequiredFeatureVersion))
        {
            // 防止把不同顺序或维度特征送入当前模型。
            throw new InvalidDataException($"Manifest 特征合同 {featureDimension}/{featureVersion} 与要求的 {RequiredFeatureDimension}/{RequiredFeatureVersion} 不兼容。");
        }

        // 类别数量和顺序 CRC 必须同时匹配。
        if ((classCount != RequiredClassCount) || (classTableCrc32 != RequiredClassTableCrc32))
        {
            // 防止 PC 把动作索引显示成错误名称。
            throw new InvalidDataException($"Manifest 类别合同 count={classCount}、CRC=0x{classTableCrc32:X8} 不兼容。");
        }

        // 热量表版本不同会让设备与 PC 对 kcal 含义产生分歧。
        if (calorieTableVersion != RequiredCalorieTableVersion)
        {
            // 当前 PC 不对未知热量表做隐式换算。
            throw new InvalidDataException($"Manifest 热量表版本 {calorieTableVersion} 不兼容，要求 {RequiredCalorieTableVersion}。");
        }

        // 正式 UI 依赖历史同步和 LittleFS；旧马达保留位缺失不能拒绝无马达真表。
        if ((capabilities!.Value & RequiredCapabilities) != RequiredCapabilities)
        {
            // 未知高位允许存在，只校验必需低位。
            throw new InvalidDataException($"Manifest 能力 0x{capabilities.Value:X8} 缺少必需位 0x{RequiredCapabilities:X8}。");
        }

        // 所有 nullable 值均由必填 tag 检查保证非空；构造不可变兼容结果。
        return new ManifestV1(
            deviceId!,
            boardRevision!,
            protocolMajor.Value,
            protocolMinor.Value,
            firmwareVersion!,
            featureDimension.Value,
            featureVersion.Value,
            baseModelSha256!,
            maskedModelSha256!,
            classCount.Value,
            classTableCrc32.Value,
            calorieTableVersion.Value,
            capabilities.Value,
            littleFsAvailableBytes!.Value);
    }

    // 解码非空 UTF-8 字段；禁止 NUL 防止日志或原生字符串被截断。
    private static string DecodeRequiredText(ReadOnlySpan<byte> value, byte tag, string fieldName)
    {
        // 空文本不能表达稳定设备身份或版本。
        if (value.IsEmpty)
        {
            // 报告具体字段和 tag。
            throw new InvalidDataException($"Manifest {fieldName} tag 0x{tag:X2} 不能为空。");
        }

        try
        {
            // 使用严格 UTF-8 解码，非法序列抛 DecoderFallbackException。
            string text = StrictUtf8.GetString(value);
            // 空白或 NUL 会造成日志歧义和原生端截断。
            if (string.IsNullOrWhiteSpace(text) || text.Contains('\0'))
            {
                // 拒绝不可见或含终止符的关键文本。
                throw new InvalidDataException($"Manifest {fieldName} tag 0x{tag:X2} 包含空白或 NUL。");
            }

            // 去除文本首尾空白后返回；协议字段不保留格式空格。
            return text.Trim();
        }
        catch (DecoderFallbackException exception)
        {
            // 把底层 UTF-8 错误转换为统一协议数据异常。
            throw new InvalidDataException($"Manifest {fieldName} tag 0x{tag:X2} 不是合法 UTF-8。", exception);
        }
    }

    // 检查已知 tag 的固定 value 长度，拒绝短值和尾随字节。
    private static void RequireLength(ReadOnlySpan<byte> value, byte tag, int expectedLength)
    {
        // 固定字段长度必须完全相等，不能只检查最小长度。
        if (value.Length != expectedLength)
        {
            // 报告实际和期望长度，定位固件编码错误。
            throw new InvalidDataException($"Manifest tag 0x{tag:X2} 长度 {value.Length} 错误，要求 {expectedLength}。");
        }
    }
}
