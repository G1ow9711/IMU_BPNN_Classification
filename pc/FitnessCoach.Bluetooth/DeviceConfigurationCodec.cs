// 引入显式小端整数写入工具，线上字节序不能依赖 Windows CPU 端序。
using System.Buffers.Binary;

// 设备配置协议位于纯蓝牙层，不依赖 WPF 或 WinRT，便于主机测试复用。
namespace FitnessCoach.Bluetooth;

/// <summary>定义设备端训练目标种类；数值必须与固件 TLV v1 保持一致。</summary>
public enum DeviceGoalKind : byte
{
    /// <summary>关闭训练目标；对应目标值必须为零。</summary>
    None = 0,
    /// <summary>按有效动作次数结束目标。</summary>
    Repetitions = 1,
    /// <summary>按训练时长秒数结束目标。</summary>
    DurationSeconds = 2,
    /// <summary>按毫千卡累计值结束目标；1000 milli-kcal 等于 1 kcal。</summary>
    CaloriesMilliKcal = 3,
}

/// <summary>保存命令 9 的可配置设备偏好；旧马达字段由编码器固定写零。</summary>
public sealed record DevicePreferencesV1(
    // AMOLED 亮度百分比，合法范围为 5～100。
    byte BrightnessPercent,
    // true 表示允许设备提示音。
    bool SoundEnabled,
    // 无操作熄屏时间，单位秒，合法范围为 10～300。
    ushort ScreenTimeoutSeconds,
    // 偏好修订号用于设备拒绝旧配置覆盖新配置。
    uint Revision,
    // true 表示允许开发者 RawStream；普通用户默认关闭。
    bool DeveloperModeEnabled);

/// <summary>定义可选设备配置能力；Mock 与真 BLE 会话都实现同一业务边界。</summary>
public interface IDeviceConfigurationSession
{
    /// <summary>接收 UTC Unix 毫秒和本地时区分钟；编码器把毫秒截断为固件要求的 Unix 秒。</summary>
    Task SyncTimeAsync(long utcUnixMilliseconds, short timezoneOffsetMinutes, CancellationToken cancellationToken = default);

    /// <summary>设置下次训练使用的体重克数及资料修订号。</summary>
    Task SetProfileAsync(uint weightGrams, uint revision, CancellationToken cancellationToken = default);

    /// <summary>设置次数、时长、卡路里目标或关闭目标。</summary>
    Task SetGoalAsync(DeviceGoalKind kind, uint value, CancellationToken cancellationToken = default);

    /// <summary>设置亮度、反馈、熄屏和开发者偏好。</summary>
    Task SetPreferencesAsync(DevicePreferencesV1 preferences, CancellationToken cancellationToken = default);
}

/// <summary>按冻结的 type:u8、len:u8、value 小端格式编码命令 6、7、8、9、11。</summary>
public static class DeviceConfigurationCodec
{
    // 命令 6 线上值使用 Unix 秒，固件 RTC 最小支持 2000-01-01T00:00:00Z。
    private const long MinimumUtcUnixSeconds = 946_684_800L;
    // 命令 6 线上值使用 Unix 秒，固件 RTC 最大支持 2099-12-31T23:59:59Z。
    private const long MaximumUtcUnixSeconds = 4_102_444_799L;
    // 公共接口接收 Unix 毫秒；每秒固定包含 1000 毫秒。
    private const long MillisecondsPerSecond = 1_000L;
    // 允许输入的最小 Unix 毫秒与 2000 年起点完全一致。
    private const long MinimumUtcUnixMilliseconds = 946_684_800_000L;
    // 允许输入包含 2099 年最后一秒内的 0～999 毫秒。
    private const long MaximumUtcUnixMilliseconds = 4_102_444_799_999L;
    // 时区只允许国际民用时区 UTC-14:00～UTC+14:00，单位为分钟。
    private const short MaximumTimezoneOffsetMinutes = 14 * 60;
    // 设备卡路里公式允许的最小体重为 30 kg，协议使用克。
    private const uint MinimumWeightGrams = 30_000U;
    // 设备卡路里公式允许的最大体重为 250 kg，协议使用克。
    private const uint MaximumWeightGrams = 250_000U;
    // AMOLED 过低亮度影响可读性，因此产品下限固定为 5%。
    private const byte MinimumBrightnessPercent = 5;
    // 百分比上限固定为 100%。
    private const byte MaximumBrightnessPercent = 100;
    // 熄屏下限防止用户刚操作就立即黑屏，单位秒。
    private const ushort MinimumScreenTimeoutSeconds = 10;
    // 熄屏上限限制 OLED 常亮和电池消耗，单位秒。
    private const ushort MaximumScreenTimeoutSeconds = 300;

    /// <summary>编码命令 6：把接口 Unix 毫秒截断为线上 Unix 秒 int64LE，并附带时区 int16LE。</summary>
    public static byte[] EncodeTimeSync(long utcUnixMilliseconds, short timezoneOffsetMinutes)
    {
        // 固件 RTC 合同只接受 2000-01-01 到 2099-12-31，先按毫秒边界拒绝越界输入。
        if ((utcUnixMilliseconds < MinimumUtcUnixMilliseconds) || (utcUnixMilliseconds > MaximumUtcUnixMilliseconds))
        {
            // 抛出明确年份边界，防止真机返回 DEVICE_CONFIG_ERR_RANGE。
            throw new ArgumentOutOfRangeException(nameof(utcUnixMilliseconds), "UTC 时间必须位于 2000-01-01T00:00:00Z～2099-12-31T23:59:59.999Z。" );
        }

        // 时区必须位于 UTC-14:00～UTC+14:00。
        if ((timezoneOffsetMinutes < -MaximumTimezoneOffsetMinutes) || (timezoneOffsetMinutes > MaximumTimezoneOffsetMinutes))
        {
            // 拒绝固件无法表达的民用时区。
            throw new ArgumentOutOfRangeException(nameof(timezoneOffsetMinutes), "时区偏移必须位于 -840～840 分钟。");
        }

        // 使用整数除法把接口毫秒转换为线上秒；正数整数除法明确截断 0～999 毫秒尾数。
        long utcUnixSeconds = utcUnixMilliseconds / MillisecondsPerSecond;
        // 内部防御检查确保常量或转换未来修改后仍符合固件秒级范围。
        if ((utcUnixSeconds < MinimumUtcUnixSeconds) || (utcUnixSeconds > MaximumUtcUnixSeconds))
        {
            // 该分支表示编码器内部合同漂移，仍按调用参数越界报告并禁止发送。
            throw new ArgumentOutOfRangeException(nameof(utcUnixMilliseconds), "UTC Unix 秒转换结果超出固件 2000～2099 年范围。" );
        }

        // 两个 TLV 共占 8+2 字节 value 和 4 字节头，精确分配 14 字节。
        byte[] payload = new byte[14];
        // 写入 type=1、len=8 的 UTC 项。
        payload[0] = 0x01;
        // UTC value 固定为 8 字节。
        payload[1] = 0x08;
        // 从偏移 2 写入小端 int64 Unix 秒；禁止把接口毫秒原样发给固件。
        BinaryPrimitives.WriteInt64LittleEndian(payload.AsSpan(2, 8), utcUnixSeconds);
        // 写入 type=2 的时区项。
        payload[10] = 0x02;
        // 时区 value 固定为 2 字节。
        payload[11] = 0x02;
        // 从偏移 12 写入小端 int16 分钟数。
        BinaryPrimitives.WriteInt16LittleEndian(payload.AsSpan(12, 2), timezoneOffsetMinutes);
        // 返回命令 6 的 TLV 区域，不含控制请求固定头。
        return payload;
    }

    /// <summary>编码命令 7：weight_g u32LE 与 revision u32LE。</summary>
    public static byte[] EncodeProfile(uint weightGrams, uint revision)
    {
        // 体重必须位于设备卡路里算法的 30～250 kg 有效区间。
        if ((weightGrams < MinimumWeightGrams) || (weightGrams > MaximumWeightGrams))
        {
            // 拒绝会让热量估算失真的体重。
            throw new ArgumentOutOfRangeException(nameof(weightGrams), "体重必须位于 30000～250000 克。");
        }

        // 两个四字节 TLV 共占 12 字节。
        byte[] payload = new byte[12];
        // 写入 type=1 的体重项。
        payload[0] = 0x01;
        // 体重 value 为 4 字节。
        payload[1] = 0x04;
        // 写入小端克数。
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(2, 4), weightGrams);
        // 写入 type=2 的资料修订号项。
        payload[6] = 0x02;
        // 修订号 value 为 4 字节。
        payload[7] = 0x04;
        // 写入小端无符号修订号。
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(8, 4), revision);
        // 返回命令 7 TLV。
        return payload;
    }

    /// <summary>编码命令 8：kind u8 与 value u32LE。</summary>
    public static byte[] EncodeGoal(DeviceGoalKind kind, uint value)
    {
        // v1 只接受四种已冻结目标类型。
        if (!Enum.IsDefined(kind))
        {
            // 拒绝未来或损坏枚举被旧固件误解释。
            throw new ArgumentOutOfRangeException(nameof(kind), "未知训练目标类型。");
        }

        // 关闭目标时 value 必须为零，避免保留语义不明的旧值。
        if ((kind == DeviceGoalKind.None) && (value != 0U))
        {
            // 报告调用方字段组合错误。
            throw new ArgumentOutOfRangeException(nameof(value), "关闭目标时目标值必须为零。");
        }

        // 启用目标时 value 必须大于零。
        if ((kind != DeviceGoalKind.None) && (value == 0U))
        {
            // 零目标会立即完成且没有业务意义，因此拒绝。
            throw new ArgumentOutOfRangeException(nameof(value), "启用目标时目标值必须大于零。");
        }

        // kind TLV 占 3 字节，value TLV 占 6 字节，总计 9 字节。
        byte[] payload = new byte[9];
        // 写入 type=1 的目标类型项。
        payload[0] = 0x01;
        // kind value 固定一字节。
        payload[1] = 0x01;
        // 写入冻结枚举值。
        payload[2] = (byte)kind;
        // 写入 type=2 的目标数值项。
        payload[3] = 0x02;
        // value 固定为 4 字节。
        payload[4] = 0x04;
        // 写入小端目标值。
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(5, 4), value);
        // 返回命令 8 TLV。
        return payload;
    }

    /// <summary>编码命令 9：亮度、旧马达保留位、声音、熄屏、revision 和开发者模式。</summary>
    public static byte[] EncodePreferences(DevicePreferencesV1 preferences)
    {
        // 记录对象不能为空。
        ArgumentNullException.ThrowIfNull(preferences);

        // 亮度必须位于 5～100%。
        if ((preferences.BrightnessPercent < MinimumBrightnessPercent) || (preferences.BrightnessPercent > MaximumBrightnessPercent))
        {
            // 拒绝不可见或超百分比亮度。
            throw new ArgumentOutOfRangeException(nameof(preferences), "亮度必须位于 5～100%。");
        }

        // 熄屏时间必须位于 10～300 秒。
        if ((preferences.ScreenTimeoutSeconds < MinimumScreenTimeoutSeconds) || (preferences.ScreenTimeoutSeconds > MaximumScreenTimeoutSeconds))
        {
            // 拒绝不符合低功耗产品边界的熄屏值。
            throw new ArgumentOutOfRangeException(nameof(preferences), "熄屏时间必须位于 10～300 秒。");
        }

        // 六项 TLV 的固定总长为 22 字节：四个 u8 项 12 字节、u16 项 4 字节、u32 项 6 字节。
        byte[] payload = new byte[22];
        // 从偏移零依次写入字段；返回偏移用于防止手算错位。
        int offset = 0;
        // 写入亮度 u8。
        offset = WriteByteItem(payload, offset, 0x01, preferences.BrightnessPercent);
        // 旧协议 type=2 是马达开关；当前手表没有马达，固定写零并从公开设置中移除。
        offset = WriteByteItem(payload, offset, 0x02, 0);
        // 写入声音 bool。
        offset = WriteByteItem(payload, offset, 0x03, preferences.SoundEnabled ? (byte)1 : (byte)0);
        // 写入熄屏 u16LE。
        offset = WriteUInt16Item(payload, offset, 0x04, preferences.ScreenTimeoutSeconds);
        // 写入偏好 revision u32LE。
        offset = WriteUInt32Item(payload, offset, 0x05, preferences.Revision);
        // 写入开发者模式 bool。
        offset = WriteByteItem(payload, offset, 0x06, preferences.DeveloperModeEnabled ? (byte)1 : (byte)0);
        // 固定长度异常表示编码器自身字段表漂移，不能发送部分配置。
        if (offset != payload.Length)
        {
            // 使用不可恢复异常提示开发错误，而不是用户输入错误。
            throw new InvalidOperationException($"偏好 TLV 编码长度错误：{offset}。" );
        }

        // 返回命令 9 TLV。
        return payload;
    }

    /// <summary>编码命令 11：RawStream 开关 bool。</summary>
    public static byte[] EncodeRawStreamEnabled(bool enabled)
    {
        // 固定返回 type=1、len=1、value=0/1 的三字节 TLV。
        return [0x01, 0x01, enabled ? (byte)1 : (byte)0];
    }

    // 写入一字节 TLV 并返回下一项偏移。
    private static int WriteByteItem(Span<byte> payload, int offset, byte type, byte value)
    {
        // 写入字段类型。
        payload[offset] = type;
        // 写入固定 value 长度 1。
        payload[offset + 1] = 1;
        // 写入实际一字节值。
        payload[offset + 2] = value;
        // 返回下一空闲偏移。
        return offset + 3;
    }

    // 写入小端 u16 TLV 并返回下一项偏移。
    private static int WriteUInt16Item(Span<byte> payload, int offset, byte type, ushort value)
    {
        // 写入字段类型。
        payload[offset] = type;
        // 写入固定 value 长度 2。
        payload[offset + 1] = 2;
        // 写入小端数值。
        BinaryPrimitives.WriteUInt16LittleEndian(payload.Slice(offset + 2, 2), value);
        // 返回下一空闲偏移。
        return offset + 4;
    }

    // 写入小端 u32 TLV 并返回下一项偏移。
    private static int WriteUInt32Item(Span<byte> payload, int offset, byte type, uint value)
    {
        // 写入字段类型。
        payload[offset] = type;
        // 写入固定 value 长度 4。
        payload[offset + 1] = 4;
        // 写入小端数值。
        BinaryPrimitives.WriteUInt32LittleEndian(payload.Slice(offset + 2, 4), value);
        // 返回下一空闲偏移。
        return offset + 6;
    }
}
